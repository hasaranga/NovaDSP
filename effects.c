#include "effect.h"
#include "audio.h"
#include "cycles.h"
#include "nova_build.h"

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/sync.h"

static const effect_t *reg[EFFECT_MAX];
static int             reg_count;

// Written only by core 0; read by core 1 for the "*" marker in the menu.
static volatile int    active;

// Last value core 0 saw for each parameter. Single 32-bit aligned words, so a
// reader on the other core sees whole values without a lock - there is nothing
// here that has to be consistent across two fields at once.
static volatile int    shadow[EFFECT_MAX][EFFECT_MAX_PARAMS];

// What each parameter read as at registration time. main() registers every
// effect before settings_load() runs, so this is the compiled-in power-on set
// and not whatever was last written to flash - which is the only thing "reset
// to default" can usefully mean from the knob. Core 0 writes it once and never
// again; core 1 never reads it.
static int             defaults[EFFECT_MAX][EFFECT_MAX_PARAMS];

// ---- timing ------------------------------------------------------------

static uint32_t t_blocks;
static uint32_t t_min = 0xFFFFFFFFu;
static uint32_t t_max;
static uint64_t t_sum;

// ---- core 1 -> core 0 command ring -------------------------------------
//
// Single producer (core 1), single consumer (core 0), so the indices need no
// atomics: each is written by exactly one core and read by the other, and a
// 32-bit aligned store is indivisible on this part. The barriers are what
// matter - they stop the compiler or the core publishing an index before the
// slot it refers to.
//
// 32 slots against a knob that can generate one command every 2 ms and a
// consumer that runs every 1.33 ms is enormous headroom; the overflow branch
// exists to be correct, not because it is expected to fire.

enum { CMD_SELECT, CMD_SET, CMD_RESET };

typedef struct {
    uint8_t op;
    uint8_t index;
    uint8_t param;
    int32_t value;
} cmd_t;

#define RING_SIZE 32

static cmd_t             ring[RING_SIZE];
static volatile uint32_t ring_head;   // producer
static volatile uint32_t ring_tail;   // consumer

static void ring_push(const cmd_t *c)
{
    uint32_t head = ring_head;
    uint32_t next = (head + 1u) % RING_SIZE;
    if (next == ring_tail) return;      // full: drop rather than block core 1
    ring[head] = *c;
    __dmb();
    ring_head = next;
}

// ---- registry ----------------------------------------------------------

bool effect_register(const effect_t *e)
{
    if (!e || !e->process || !e->get || !e->set) return false;
    if (reg_count >= EFFECT_MAX) return false;
    if (e->param_count < 0 || e->param_count > EFFECT_MAX_PARAMS) return false;

    reg[reg_count] = e;
    for (int p = 0; p < e->param_count; ++p) {
        const int v = e->get(p);
        shadow[reg_count][p]   = v;
        defaults[reg_count][p] = v;
    }
    reg_count++;
    return true;
}

int effect_count(void) { return reg_count; }

const effect_t *effect_at(int index)
{
    if (index < 0 || index >= reg_count) return NULL;
    return reg[index];
}

int effect_active(void) { return active; }

// Called from the boot banner, after settings_load() has applied whatever was
// stored - so this reports what the box actually came up running, not index 0.
//
// It earns a line because nothing else in the banner says it, and the parameter
// dump printed just below is DeeFloat's whatever is active: without this, a
// board running a chain prints a DeeFloat parameter set that has nothing to do
// with what you are hearing, and the only other place the answer exists is the
// panel. A chain is expanded because "FX Chain" on its own is the one name in
// the list that does not tell you what is running - and the dsp figure in the
// status line is the sum of everything in it.
//
// The slot -> effect mapping is asked of the effect rather than re-derived
// here: a slot holds a position in a candidate list, and that encoding belongs
// to whichever effect built the list. This used to call fx_chain_slot_effect()
// by name, which meant the core knew one particular module existed and would
// not link without it. It now tests EFFECT_F_COMPOSITE and calls the optional
// slot_effect() hook, so a build with no chain in it prints one line and a
// build with a different composite prints the right one.
void effect_report_active(void)
{
    const effect_t *e = reg[active];
    if (!e) {
        NOVA_LOG("  active effect: none\n");
        return;
    }

    NOVA_LOG("  active effect: %s", e->name);

    if ((e->flags & EFFECT_F_COMPOSITE) && e->slot_effect) {
        int n = 0;
        for (int s = 0; s < e->param_count; ++s) {
            const int idx = e->slot_effect(s);
            if (idx < 0) continue;   // empty slot: the chain closes up over it

            // No side effect inside a NOVA_LOG argument - dist does not
            // evaluate them. Hence the separate ++n.
            NOVA_LOG("%s%s", n ? " > " : " - ", effect_at(idx)->name);
            ++n;
        }
        if (n == 0) NOVA_LOG(" - empty, bit-exact passthrough");
    }

    NOVA_LOG("\n");
}

// ---- audio path --------------------------------------------------------

void effect_process_block(int32_t *buf)
{
    const effect_t *e = reg[active];
    if (!e) return;

    const uint32_t t0 = cyc_now();
    e->process(buf);
    const uint32_t dt = cyc_since(t0);

    ++t_blocks;
    t_sum += dt;
    if (dt < t_min) t_min = dt;
    if (dt > t_max) t_max = dt;
}

void effect_timing_take(effect_timing_t *out)
{
    out->blocks  = t_blocks;
    out->min_cyc = (t_blocks ? t_min : 0);
    out->max_cyc = t_max;
    out->sum_cyc = t_sum;

    t_blocks = 0;
    t_min    = 0xFFFFFFFFu;
    t_max    = 0;
    t_sum    = 0;
}

// ---- applying changes (core 0) -----------------------------------------

void effect_apply_select(int index)
{
    if (index < 0 || index >= reg_count) return;
    active = index;
    if (reg[index]->enter) reg[index]->enter();
}

void effect_apply_set(int index, int param, int value)
{
    const effect_t *e = effect_at(index);
    if (!e || param < 0 || param >= e->param_count) return;
    e->set(param, value);
    shadow[index][param] = e->get(param);   // the effect clamps; trust it, not
                                            // the value we asked for
}

// Goes through effect_apply_set() one parameter at a time rather than poking
// the effect directly, so an effect that retunes filters inside set() does it
// exactly as it would for a knob turn, and the shadow lands right without a
// second path to remember.
void effect_apply_reset(int index)
{
    const effect_t *e = effect_at(index);
    if (!e) return;
    for (int p = 0; p < e->param_count; ++p)
        effect_apply_set(index, p, defaults[index][p]);
}

void effect_service(void)
{
    while (ring_tail != ring_head) {
        cmd_t c = ring[ring_tail];
        __dmb();
        ring_tail = (ring_tail + 1u) % RING_SIZE;

        switch (c.op) {
        case CMD_SELECT: effect_apply_select(c.index);                 break;
        case CMD_RESET:  effect_apply_reset(c.index);                  break;
        default:         effect_apply_set(c.index, c.param, c.value);  break;
        }
    }

    // Re-read the active effect every pass. This is a handful of array loads,
    // and it means the shadow cannot drift no matter who moved a parameter -
    // the console keys, the 'r' reload, the benchmark restoring its saved set.
    // Without it every one of those paths would need to remember to publish.
    const effect_t *e = reg[active];
    if (e) {
        for (int p = 0; p < e->param_count; ++p)
            shadow[active][p] = e->get(p);
    }
}

// ---- UI side -----------------------------------------------------------

void effect_request_select(int index)
{
    cmd_t c = { CMD_SELECT, (uint8_t)index, 0, 0 };
    ring_push(&c);
}

void effect_request_set(int index, int param, int value)
{
    cmd_t c = { CMD_SET, (uint8_t)index, (uint8_t)param, value };
    ring_push(&c);
}

void effect_request_reset(int index)
{
    cmd_t c = { CMD_RESET, (uint8_t)index, 0, 0 };
    ring_push(&c);
}

int effect_value(int index, int param)
{
    if (index < 0 || index >= reg_count) return 0;
    if (param < 0 || param >= EFFECT_MAX_PARAMS) return 0;
    return shadow[index][param];
}

char *effect_format_value(int index, int param, int value, char *buf, int n)
{
    const effect_t *e = effect_at(index);
    if (!e || param < 0 || param >= e->param_count) {
        if (n > 0) buf[0] = '\0';
        return buf;
    }

    const effect_param_t *d = &e->params[param];
    switch (d->kind) {
    case EP_BOOL:
        snprintf(buf, (size_t)n, "%s", value ? "on" : "off");
        break;
    case EP_ENUM:
        if (d->labels && value >= d->min && value <= d->max)
            snprintf(buf, (size_t)n, "%s", d->labels[value - d->min]);
        else
            snprintf(buf, (size_t)n, "%d", value);
        break;
    default:
        if (d->unit) snprintf(buf, (size_t)n, "%d%s", value, d->unit);
        else         snprintf(buf, (size_t)n, "%d", value);
        break;
    }
    return buf;
}
