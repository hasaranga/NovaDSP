/*
 * settings.h - parameter and effect selection persisted to flash.
 *
 * Everything the knob can change survives a power cycle: every registered
 * effect's parameters, and which effect was active.
 *
 * ---- why this is not simply "write on change" --------------------------
 *
 * Two costs make that wrong. Flash endurance is finite, and one detent of a
 * knob would be one erase/program cycle - a single sweep through a parameter
 * would spend twenty. And a sector erase takes tens of milliseconds with the
 * core stalled, which at a 1333 us deadline is an audible dropout.
 *
 * So a write happens only when the user leaves a parameter page, and only if
 * something actually differs from what is stored. The store is divided into
 * slots that are filled one at a time, and - this is the important part - the
 * erase never happens while audio is running. A full store is compacted by
 * settings_load(), before audio_init(). An erase stalls the core for tens of
 * milliseconds, which is dozens of missed deadlines and long enough for the
 * USB host to drop the CDC device; a 512-byte program is about a millisecond
 * and costs nothing measurable.
 *
 * All of this is core 0 work. Core 1 must have called
 * multicore_lockout_victim_init() so the write can pause it.
 */

#ifndef NOVADSP_SETTINGS_H
#define NOVADSP_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call after every effect is registered and before audio starts. Applies the
// stored values through effect_apply_set()/effect_apply_select(), so it goes
// through exactly the same path the console and the UI do. Silently does
// nothing if the store is blank or fails its checksum - a first boot, or a
// corrupted record, leaves the compiled-in defaults in place.
void settings_load(void);

// Core 1 asks for a save; core 0 performs it on its next settings_tick().
// The UI calls this from the Save row on the effect list - saving is an
// explicit action the user takes, not a side effect of navigating.
void settings_request_save(void);

// Call once per main-loop pass. Performs a requested save, and refreshes the
// dirty flag below on its own slow timer.
void settings_tick(void);

// True when the live settings differ from what is in flash - i.e. there is
// something to save. Safe from either core: core 0 computes it in
// settings_tick() and core 1 only reads the result, because building a
// snapshot calls effect get() and that is core 0's alone.
//
// Refreshed a few times a second, not per block, so it lags a knob turn
// slightly. That is invisible at UI rates and keeps the cost off the audio
// deadline.
bool settings_is_dirty(void);

// Force a write now if anything differs from the stored copy. Blocks for the
// duration, so not while audio matters. Returns true if it wrote.
bool settings_save_now(void);

// Console 's': report the state and request a save, through the same path the
// UI's Save row uses.
void settings_debug(void);

// Counters for the console: how many records have been written this session,
// and how many sector erases that cost.
uint32_t settings_writes(void);
uint32_t settings_erases(void);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_SETTINGS_H
