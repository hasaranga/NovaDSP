/*
 * fxchain.c - "FX Chain": run several of the other effects in series.
 *
 * It is an effect like any other. It registers, it appears in the list, it is
 * selected with a press, and exactly one effect processes audio - which is
 * this one. What makes it different is that its process() is a loop over other
 * effects' process(), in the order the user put them in.
 *
 * ---- why the chain is stored as ordinary parameters ---------------------
 *
 * Four EP_ENUM parameters, "Slot 1".."Slot 4", each holding 0 for empty or a
 * candidate position plus one. Nothing else in the firmware needs to know
 * the chain exists: it persists through settings.c because settings.c stores
 * parameters, it resets through effect_apply_reset() because that restores
 * parameters, and it reaches core 0 through the ordinary command ring because
 * the UI edits it with effect_request_set(). The chain editor page in ui.c is
 * a different way of *drawing* four enum parameters, not a second mechanism.
 *
 * ---- the candidate list --------------------------------------------------
 *
 * Not every registered effect can go in a chain. Two are excluded, for
 * different reasons:
 *
 *   FX Chain itself, because a chain containing itself recurses until the
 *   stack gives out. That is checked in three places rather than one, because
 *   the cost of missing it is not a wrong sound but a dead board.
 *
 *   Passthru, because it is a gain trim whose whole purpose is to be the thing
 *   you A/B an effect *against*. Inside a chain it is a multiply by a constant
 *   in the middle of the signal path, which every other effect there already
 *   has its own control for.
 *
 * Neither exclusion names an effect. Both are read off effect_t::flags, so the
 * rule survives any module set: EFFECT_F_COMPOSITE keeps a chain out of a
 * chain, and EFFECT_F_NOT_IN_CHAIN is how Passthru declines. late_init() walks
 * the registry once and builds `cand`, so the rule is applied exactly here and
 * a display's editor asks rather than re-deriving it.
 *
 * ---- what a slot stores, and the one thing to remember -----------------
 *
 * A slot holds a **position in that candidate list** - not a name, and not a
 * registry index either. Everything else in the settings store is keyed by
 * name precisely so registration order can change without applying the wrong
 * values, and this is the one record field that cannot be: a parameter value
 * is an int16.
 *
 * So **anything that changes the candidate list changes what a stored chain
 * means** - enabling an effect module, disabling one, changing an ORDER field.
 * That used to need a VERSION bump in settings.c that somebody had to remember;
 * it is now automatic, because the settings store is keyed by a hash of every
 * effect's name in registration order and any of those three changes it. See
 * modules_layout_id() in modules.c.
 *
 * ---- registration order --------------------------------------------------
 *
 * The candidate list is built in the module's late_init(), which the core
 * calls after every module has registered - including this one. That is what
 * frees the chain from having to register last, and lets its ORDER put it
 * wherever it reads best in the menu.
 *
 * ---- cost ---------------------------------------------------------------
 *
 * The chain is timed as one hook, so the status line and DSP STATUS show the
 * total against the 1333 us deadline the same as any single effect. That is
 * the number to watch here: four effects in series can add up to a real
 * fraction of the block, and the FFT still lands on the same deadline on one
 * block in eight. Nothing here throttles it - `ovr` is the honest answer.
 */

#include "module.h"
#include "audio.h"

#include <stddef.h>   // NULL - nova_build.h only pulls in stdio.h in the dev build

// How many effects a chain may hold. Private to this module: the registry sees
// it as param_count, and a display that draws a chain editor reads it from
// there rather than from a shared constant. It used to live in effect.h, which
// meant the core carried a number that was only ever this file's business.
#define FX_CHAIN_SLOTS 4

// 0 = empty, otherwise (candidate position + 1).
static int slot[FX_CHAIN_SLOTS];

// The registry indices a slot is allowed to name, in menu order, and how many
// there are. Built once by fx_chain_late_init().
static uint8_t cand[EFFECT_MAX];
static int     n_candidates;

// labels[0] is "empty"; labels[i + 1] names cand[i]. Pointers into the
// effects' own name strings, so a name cannot drift from its row.
static const char *labels[EFFECT_MAX + 1];

static effect_param_t kParams[FX_CHAIN_SLOTS];

// Fixed strings rather than snprintf into a buffer: the descriptor holds a
// pointer and the UI reads it whenever it draws.
static const char *const kSlotName[FX_CHAIN_SLOTS] = {
    "Slot 1", "Slot 2", "Slot 3", "Slot 4",
};

// The descriptor is defined ahead of the functions it points at, because two
// of them need its address to check that the chain can never contain itself.
static void fx_process(int32_t *buf);
static void fx_enter(void);
static int  fx_get(int p);
static void fx_set(int p, int value);
static int  fx_slot_effect(int p);

static const effect_t kFxChain = {
    .name        = "FX Chain",
    .param_count = FX_CHAIN_SLOTS,
    .params      = kParams,
    .enter       = fx_enter,
    .process     = fx_process,
    .get         = fx_get,
    .set         = fx_set,

    // What makes this effect composable-with rather than composable-into, and
    // what a display reads to decide it should draw a chain editor instead of
    // four enum rows. Nothing here names FX Chain; the flag does.
    .flags       = EFFECT_F_COMPOSITE,
    .slot_effect = fx_slot_effect,
};

// ---- the registry's view of it -----------------------------------------

static int fx_get(int p)
{
    if (p < 0 || p >= FX_CHAIN_SLOTS) return 0;
    return slot[p];
}

static void fx_set(int p, int value)
{
    if (p < 0 || p >= FX_CHAIN_SLOTS) return;

    // Clamps to empty rather than rejecting, so a record written against a
    // different candidate list lands on something legal instead of on a
    // position that no longer exists.
    if (value < 0)            value = 0;
    if (value > n_candidates) value = 0;

    const int was = slot[p];
    slot[p] = value;

    // An effect joining the chain is the same moment as one being selected, so
    // it gets the same call. Nothing registered today defines enter(), but the
    // interface promises it and a chain that quietly skipped it would be a
    // latent bug for the first effect that needs one. Core 0 only, like every
    // other path into an effect.
    if (value != 0 && value != was) {
        const effect_t *e = effect_at(cand[value - 1]);
        if (e && !(e->flags & EFFECT_F_COMPOSITE) && e->enter) e->enter();
    }
}

// Duplicates are legal here and are not filtered out: the same effect twice in
// series is a meaningful thing to ask for, and rejecting it would break the
// renumbering the editor does when a slot is removed - that walks the chain
// down one place, which transiently repeats a value. The editor never creates
// one; a hand-written settings record could.
static void fx_process(int32_t *buf)
{
    for (int s = 0; s < FX_CHAIN_SLOTS; ++s) {
        const int v = slot[s];
        if (v <= 0 || v > n_candidates) continue;

        const effect_t *e = effect_at(cand[v - 1]);

        // Never a composite, which includes never itself. Nothing composite
        // is in the candidate list, so this cannot happen - but the
        // consequence if it ever did is unbounded recursion rather than a
        // wrong sound, so it is checked where the call actually is.
        if (!e || (e->flags & EFFECT_F_COMPOSITE)) continue;

        e->process(buf);
    }
    // An empty chain leaves the block untouched, which is the right answer:
    // a chain with nothing in it is a passthrough, bit-exact.
}

// Called when the chain becomes the active effect, and passed on to whatever
// is in it for the same reason fx_set() does.
static void fx_enter(void)
{
    for (int s = 0; s < FX_CHAIN_SLOTS; ++s) {
        const int v = slot[s];
        if (v <= 0 || v > n_candidates) continue;
        const effect_t *e = effect_at(cand[v - 1]);
        if (e && !(e->flags & EFFECT_F_COMPOSITE) && e->enter) e->enter();
    }
}

// ---- entry points ------------------------------------------------------

static void fx_chain_late_init(void)
{
    n_candidates = 0;
    labels[0]    = "-";

    // The exclusion rule, and the whole of it. **Nothing here names an
    // effect.** A composite is excluded because a chain containing one
    // recurses until the stack gives out - which covers this effect and any
    // other composite a future module might add. EFFECT_F_NOT_IN_CHAIN covers
    // the rest, and it is the effect itself that decides, because "would this
    // be useful inside a chain" is a property of the effect and not of the
    // chain.
    for (int i = 0; i < effect_count() && n_candidates < EFFECT_MAX; ++i) {
        const effect_t *e = effect_at(i);
        if (!e) continue;
        if (e->flags & (EFFECT_F_COMPOSITE | EFFECT_F_NOT_IN_CHAIN)) continue;

        cand[n_candidates]       = (uint8_t)i;
        labels[n_candidates + 1] = e->name;
        ++n_candidates;
    }

    for (int s = 0; s < FX_CHAIN_SLOTS; ++s) {
        kParams[s] = (effect_param_t){
            kSlotName[s], EP_ENUM, 0, n_candidates, labels, NULL
        };
    }

    NOVA_LOG("FX Chain: %d slots over %d of %d effects\n",
             FX_CHAIN_SLOTS, n_candidates, effect_count());
}

static const effect_t *fx_chain_effect(void) { return &kFxChain; }

// The registry index running in a given chain position, or -1 if that slot is
// empty or holds something it must not.
//
// This is effect_t::slot_effect, so a display's chain editor and the boot
// banner both get the mapping by asking rather than by re-deriving it - which
// they could not do anyway, since the encoding (0 for empty, otherwise a
// candidate position plus one) is private to this file. The guards mirror
// fx_process() exactly, so what a caller reports is what actually runs.
static int fx_slot_effect(int s)
{
    if (s < 0 || s >= FX_CHAIN_SLOTS) return -1;

    const int v = slot[s];
    if (v <= 0 || v > n_candidates) return -1;

    const effect_t *e = effect_at(cand[v - 1]);
    if (!e || (e->flags & EFFECT_F_COMPOSITE)) return -1;

    return (int)cand[v - 1];
}

// ---- the module ---------------------------------------------------------
//
// init() does nothing: the candidate list cannot be built until every other
// module has registered, so all of the work is in late_init(). That second
// pass is the only reason effect_module_t has one.

static void fx_chain_init(void) { }

static const effect_module_t kModule = {
    .name      = "fxchain",
    .init      = fx_chain_init,
    .effect    = fx_chain_effect,
    .late_init = fx_chain_late_init,
    .report    = NULL,
    .on_key    = NULL,
    .benchmark = NULL,
};

const effect_module_t *fxchain_module(void) { return &kModule; }
