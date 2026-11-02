#include "settings.h"
#include "nova_build.h"
#include "effect.h"
#include "module.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/clocks.h"

// Set from the build (-DNOVA_SETTINGS_SALT=n). The fallback is here so this
// file still compiles outside CMake; see the note beside layout_id().
#ifndef NOVA_SETTINGS_SALT
#define NOVA_SETTINGS_SALT 0
#endif

// ---- where -------------------------------------------------------------
// The last sector of flash. The image is loaded from the bottom and is nowhere
// near 4 MB, so the top sector is free and stays free however much the code
// grows. Reads go through the XIP window: nothing executes from flash under
// copy_to_ram, but the mapping is still there for data.

#define SLOT_SIZE     512u
#define STORE_SECTORS 2u
#define STORE_BYTES   (FLASH_SECTOR_SIZE * STORE_SECTORS)
#define SLOTS         (STORE_BYTES / SLOT_SIZE)                // 16
#define STORE_OFFSET  (PICO_FLASH_SIZE_BYTES - STORE_BYTES)
#define STORE_BASE    ((const uint8_t *)(XIP_BASE + STORE_OFFSET))

#define MAGIC         0x5344564EuL     // "NVDS"

// ---- the store's identity ----------------------------------------------
//
// There is no VERSION here any more, and no rule to remember about bumping it.
//
// What was here was a hand-maintained integer plus two pages of instructions:
// bump it whenever any effect gains, loses or reorders a parameter, and bump
// it whenever the chain candidate list changes. Both halves of that rule were
// correct and both were forgotten, and each time the symptom was an effect
// that came up silently wrong rather than anything a counter could show -
// removing BBE's `Safety` parameter applied the stored Safety=1 to `Bypass`
// and the effect booted bypassed, with `dsp` showing only the cost of format
// conversion.
//
// The store is now keyed by a hash of the layout it was written against:
// every effect's name in registration order, its parameter count, and each
// parameter's name, kind and range. modules_layout_id() computes it from the
// finished registry, so it is derived from the same descriptors the record is
// laid out against and cannot fall behind them.
//
// What that buys, beyond not having to remember:
//
//   - **module selection is covered for free.** Switching an effect module off
//     changes the registration order, which changes the hash, which is exactly
//     right - a stored FX Chain slot holds a *position in a candidate list*
//     derived from that order, and an int16 cannot hold a name.
//   - **an unrelated rebuild keeps your tuning.** A random per-build id would
//     also make stale records impossible, and would throw away Target, Thresh
//     and the active effect on every single flash.
//
// What it cannot see is a parameter whose *meaning* changed while its name,
// kind and range did not. NOVA_SETTINGS_SALT is the manual override for that
// one case, and it is set from the build:
//
//     cmake -B build -DNOVA_SETTINGS_SALT=1
//
// A widened range is caught along with a narrowed one, which is stricter than
// it strictly needs to be and costs one erase at the next boot. Not worth
// special-casing: a narrowing genuinely must invalidate, because stored values
// outside the new range are silently pulled in at load and that reads as an
// effect quietly changing its own settings.
static uint32_t layout_id(void)
{
    // Computed once. It walks every parameter of every effect, and
    // build_current() calls this on a 250 ms timer.
    static uint32_t cached;
    if (!cached) cached = modules_layout_id();
    return cached;
}

#define NAME_LEN      12

// ---- what --------------------------------------------------------------
// Effects are keyed by name, not by index. Registration order is menu order
// and is going to change as effects are added; if a record were keyed by
// position, inserting one effect would silently apply the wrong stored values
// to every effect after it.

typedef struct {
    char    name[NAME_LEN];
    int16_t value[EFFECT_MAX_PARAMS];
} block_t;

typedef struct {
    uint32_t magic;
    uint32_t layout;                 // modules_layout_id() when it was written
    uint16_t seq;                    // newest wins; wraps harmlessly
    uint8_t  count;
    uint8_t  pad;
    char     active[NAME_LEN];       // name, for the same reason
    block_t  fx[EFFECT_MAX];
    uint32_t sum;                    // must stay last: hashed up to here
} record_t;

// EFFECT_MAX is now the number of enabled effect modules, so this is the one
// thing that still puts a ceiling on how many there can be: a block_t is
// NAME_LEN + 2 * EFFECT_MAX_PARAMS = 44 bytes and the header is 24, so eleven
// effects fit a 512-byte slot exactly and twelve do not.
//
// The way out is SLOT_SIZE 1024, which halves the ring from sixteen slots to
// eight - fewer saves before a boot-time compaction, and half the endurance
// multiplier. Worth knowing before assuming the assert is the end of it.
_Static_assert(sizeof(record_t) <= SLOT_SIZE,
               "settings record does not fit a slot - too many effect modules; "
               "see the note above about SLOT_SIZE");
_Static_assert(SLOT_SIZE % FLASH_PAGE_SIZE == 0, "slot must be programmable");

static record_t  saved;              // what is in flash right now
static record_t  current;            // scratch, rebuilt on each compare
static uint8_t   page_buf[SLOT_SIZE];

static bool      have_saved;
static uint32_t  next_slot;
static uint16_t  next_seq = 1;
static uint32_t  n_writes, n_erases;

// Set by core 1 when the user leaves a parameter page; acted on by core 0.
// A single bool written by one core and read by the other - no ordering
// requirement beyond eventual visibility, so volatile is enough.
static volatile bool   save_requested;
static bool            store_full;

// Read by core 1 to mark the Save row. Written only by core 0, and a plain
// aligned bool, so it needs nothing more than volatile for eventual visibility.
static volatile bool   dirty_flag;
static absolute_time_t next_dirty;

// ---- XIP timing at 307.2 MHz -------------------------------------------
//
// This project runs from SRAM, so until settings existed nothing ever read
// flash at the overclocked speed and the XIP read timing was never exercised.
// The settings store is the first thing to do it, and the default setup does
// not survive it:
//
//   - QMI_M0_TIMING resets to CLKDIV 4, RXDELAY 0. At 307.2 MHz that is
//     76.8 MHz SCK sampled with no allowance for the round trip out to the
//     flash and back.
//   - The ROM resets M0 to that clean configuration on every flash_exit_xip,
//     and hardware_flash saves and restores only the CS1 window and the QSPI
//     pads - m[0].timing is not in flash_save_hardware_state. So a write
//     silently retunes the read path underneath us.
//
// Hence: set it explicitly, and set it again after every flash operation.
// CLKDIV 8 is 38.4 MHz, far below anything marginal. Nothing executes from
// flash and the store is 8 KB read once at boot, so the speed costs nothing
// and the margin is worth everything.
#define XIP_CLKDIV   8u
#define XIP_RXDELAY  1u

static void xip_retune(void)
{
    qmi_hw->m[0].timing =
        (qmi_hw->m[0].timing & ~(QMI_M0_TIMING_CLKDIV_BITS |
                                 QMI_M0_TIMING_RXDELAY_BITS))
        | (XIP_CLKDIV  << QMI_M0_TIMING_CLKDIV_LSB)
        | (XIP_RXDELAY << QMI_M0_TIMING_RXDELAY_LSB);

    // The datasheet requires a dummy access plus a barrier before the new
    // divisor can be relied on.
    (void)*(volatile uint32_t *)XIP_BASE;
    __dmb();
}

static void xip_report(const char *when)
{
    const uint32_t t  = qmi_hw->m[0].timing;
    const uint32_t d  = (t & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    const uint32_t rx = (t & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB;
    NOVA_LOG("  xip %s: timing=%08lx clkdiv=%lu rxdelay=%lu -> sck %lu kHz\n",
           when, (unsigned long)t, (unsigned long)d, (unsigned long)rx,
           (unsigned long)(d ? clock_get_hz(clk_sys) / (1000u * d) : 0));
}

// FNV-1a over everything up to the checksum field. Not a CRC and not claimed to
// be one - it only has to catch a record torn by a power cut mid-program.
static uint32_t hash_of(const record_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(record_t, sum); ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void compact_if_full(void);
static bool commit(const record_t *r);
static void erase_store(void *param);

static const record_t *slot_at(uint32_t i)
{
    return (const record_t *)(STORE_BASE + i * SLOT_SIZE);
}

static bool slot_blank(uint32_t i)
{
    const uint8_t *p = STORE_BASE + i * SLOT_SIZE;
    for (uint32_t n = 0; n < SLOT_SIZE; ++n)
        if (p[n] != 0xFFu) return false;
    return true;
}

// Snapshot the live effects. Reads through get() rather than the UI shadow:
// this runs on core 0, where the effects themselves are the truth.
static void build_current(record_t *r)
{
    memset(r, 0, sizeof *r);
    r->magic   = MAGIC;
    r->layout  = layout_id();
    r->count   = (uint8_t)effect_count();

    const effect_t *act = effect_at(effect_active());
    if (act) strncpy(r->active, act->name, NAME_LEN - 1);

    for (int i = 0; i < effect_count() && i < EFFECT_MAX; ++i) {
        const effect_t *e = effect_at(i);
        strncpy(r->fx[i].name, e->name, NAME_LEN - 1);
        for (int p = 0; p < e->param_count; ++p)
            r->fx[i].value[p] = (int16_t)e->get(p);
    }
    // seq and sum are set at write time; leave them zero so two snapshots of
    // the same settings compare equal.
}

// ---- load --------------------------------------------------------------

void settings_load(void)
{
    NOVA_LOG("  settings: layout %08lx, salt %d, %d effects, %u slots\n",
             (unsigned long)layout_id(), (int)(NOVA_SETTINGS_SALT),
             effect_count(), (unsigned)SLOTS);

    xip_report("at boot");
    xip_retune();
    xip_report("retuned");

    int best = -1;
    uint16_t best_seq = 0;

    for (uint32_t i = 0; i < SLOTS; ++i) {
        const record_t *r = slot_at(i);
        if (r->magic != MAGIC || r->layout != layout_id()) continue;
        if (hash_of(r) != r->sum) continue;

        // Serial-number arithmetic, so a wrap of seq does not make the newest
        // record look like the oldest.
        if (best < 0 || (uint16_t)(r->seq - best_seq) < 0x8000u) {
            best = (int)i;
            best_seq = r->seq;
        }
    }

    if (best < 0) {
        next_slot = 0;               // keep the compiled-in defaults

        // Nothing readable, but that is not the same as nothing there. After
        // the layout changes, every slot still holds a record written against
        // the old one, and programming a new one over it would only clear bits
        // - the result fails its checksum and settings silently never persist
        // again.
        //
        // So erase here if the store is not actually blank. This is the one
        // place it is safe: settings_load() runs before audio_init(), which is
        // exactly why the design puts every erase at boot.
        if (!slot_blank(0)) {
            NOVA_LOG("  settings: no record for layout %08lx, erasing\n",
                     (unsigned long)layout_id());

            const uint32_t t0 = time_us_32();
            const int erc = flash_safe_execute(erase_store, NULL, 1000);
            xip_retune();

            if (erc != PICO_OK)
                NOVA_LOG("  settings: erase FAILED, settings will not persist\n");
            else
                NOVA_LOG("  settings: erased in %lu us\n",
                       (unsigned long)(time_us_32() - t0));
        }
        return;
    }

    saved      = *slot_at((uint32_t)best);
    have_saved = true;
    next_seq   = (uint16_t)(saved.seq + 1u);
    next_slot  = (uint32_t)best + 1u;

    // Apply by name. An effect in the record that is no longer registered is
    // ignored; one that is registered but not in the record keeps its default.
    for (int b = 0; b < EFFECT_MAX; ++b) {
        if (!saved.fx[b].name[0]) continue;
        for (int i = 0; i < effect_count(); ++i) {
            const effect_t *e = effect_at(i);
            if (strncmp(e->name, saved.fx[b].name, NAME_LEN - 1) != 0) continue;
            for (int p = 0; p < e->param_count; ++p)
                effect_apply_set(i, p, saved.fx[b].value[p]);
            break;
        }
    }

    for (int i = 0; i < effect_count(); ++i) {
        if (strncmp(effect_at(i)->name, saved.active, NAME_LEN - 1) == 0) {
            effect_apply_select(i);
            break;
        }
    }

    // Re-snapshot: the effects clamp, so what actually landed may differ from
    // what was stored. Without this a record written by an older build with a
    // wider range would look permanently dirty and rewrite on every boot.
    build_current(&saved);

    compact_if_full();
}

// The only place an erase ever happens. Called from settings_load(), which
// runs before audio_init() - so the tens of milliseconds it costs land where
// there is no block deadline to miss and no stream to interrupt.
//
// Rewrites the newest record into slot 0 of the fresh store, so nothing is
// lost by compacting.
static void compact_if_full(void)
{
    if (next_slot < SLOTS) return;

    NOVA_LOG("  settings: store full (%u slots), compacting\n", (unsigned)SLOTS);

    const uint32_t t0 = time_us_32();
    const int erc = flash_safe_execute(erase_store, NULL, 1000);
    xip_retune();
    if (erc != PICO_OK) {
        NOVA_LOG("  settings: erase FAILED, settings will not persist\n");
        store_full = true;
        return;
    }

    next_slot  = 0;
    next_seq   = 1;
    store_full = false;

    record_t rec = saved;
    have_saved = false;              // force commit() to write it out
    commit(&rec);
    saved = rec;
    saved.seq = 0;                   // build_current() leaves these zero, so
    saved.sum = 0;                   // clear them to keep comparisons honest
    have_saved = true;

    NOVA_LOG("  settings: compacted in %lu us\n",
           (unsigned long)(time_us_32() - t0));
}

// ---- save --------------------------------------------------------------

// Both of these run with the other core locked out and interrupts off.
// Nothing in here may touch anything but flash.

static void write_slot(void *param)
{
    (void)param;
    flash_range_program(STORE_OFFSET + next_slot * SLOT_SIZE,
                        page_buf, SLOT_SIZE);
    next_slot++;
    n_writes++;
}

static void erase_store(void *param)
{
    (void)param;
    flash_range_erase(STORE_OFFSET, STORE_BYTES);
    n_erases++;
}

static bool commit(const record_t *r)
{
    // No free slot. Deliberately does NOT erase here.
    //
    // A sector erase takes tens of milliseconds with interrupts off and the
    // other core parked. That is dozens of missed audio deadlines, and it
    // stalls USB long enough for the host to drop the CDC device - measured on
    // this board, and it looked exactly like a hang. A 512-byte program is
    // around a millisecond and costs nothing measurable, so the erase is the
    // whole problem and it belongs at boot, where there is no deadline to miss.
    // settings_load() compacts a full store before audio ever starts.
    if (next_slot >= SLOTS) {
        if (!store_full)
            NOVA_LOG("  settings: store full, not saving until next boot\n");
        store_full = true;
        return false;
    }

    record_t rec = *r;
    rec.seq = next_seq;
    rec.sum = hash_of(&rec);

    memset(page_buf, 0xFF, sizeof page_buf);
    memcpy(page_buf, &rec, sizeof rec);

    const uint32_t erases_before = n_erases;
    const uint32_t t0 = time_us_32();
    const int rc = flash_safe_execute(write_slot, NULL, 500);
    const uint32_t us = time_us_32() - t0;

    // The ROM reset the read path; put it back before anything reads flash.
    xip_retune();

    if (rc != PICO_OK) {
        NOVA_LOG("  settings: flash write FAILED (%d)\n", rc);
        return false;
    }

    // Worth a line. It says the settings actually reached flash, and the
    // elapsed time is the honest cost - the erase rows are the ones that can
    // register overruns.
    NOVA_LOG("  settings saved: slot %lu, %lu us%s\n",
           (unsigned long)(next_slot - 1u), (unsigned long)us,
           n_erases != erases_before ? "  (sector erased)" : "");

    saved      = *r;
    have_saved = true;
    next_seq++;
    return true;
}

bool settings_save_now(void)
{
    build_current(&current);
    if (have_saved && memcmp(&current, &saved, sizeof current) == 0) return false;
    return commit(&current);
}

void settings_request_save(void)
{
    save_requested = true;
}

void settings_tick(void)
{
    if (save_requested) {
        save_requested = false;
        settings_save_now();
        // Whatever the write decided, the answer to "is there anything to
        // save" has just changed. Recompute now rather than leaving the row
        // stale for a quarter of a second after the user pressed it.
        next_dirty = get_absolute_time();
    }

    // Snapshotting every effect is a few hundred loads - nothing against a
    // 1333 us block, but no reason to do it 750 times a second either. Four
    // times a second is far faster than a user can notice the row change.
    if (time_reached(next_dirty)) {
        next_dirty = make_timeout_time_ms(250);
        build_current(&current);
        dirty_flag = !have_saved
                  || memcmp(&current, &saved, sizeof current) != 0;
    }
}

bool settings_is_dirty(void)
{
    return dirty_flag;
}

void settings_debug(void)
{
    build_current(&current);
    const bool differs = !have_saved
                      || memcmp(&current, &saved, sizeof current) != 0;

    settings_request_save();     // same route the Save row takes

    NOVA_LOG("  settings: have_saved=%d differs=%d full=%d next_slot=%lu "
           "seq=%u writes=%lu erases=%lu\n",
           (int)have_saved, (int)differs, (int)store_full,
           (unsigned long)next_slot, (unsigned)next_seq,
           (unsigned long)n_writes, (unsigned long)n_erases);

}

uint32_t settings_writes(void) { return n_writes; }
uint32_t settings_erases(void) { return n_erases; }
