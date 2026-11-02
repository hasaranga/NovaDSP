/*
 * display.h - core 1, and the two seqlocks that feed it.
 *
 * This is the core half of the display plug point: it launches core 1, hands
 * the numbers across, and calls the compiled-in display module's hooks. It has
 * no opinion about what a display is - see display_module_t in module.h for
 * the half a module implements.
 *
 * ---- why the transport is core-side rather than the module's ------------
 *
 * Because every display module would otherwise write the same seqlock, and one
 * of them would write it slightly wrong. The requirement is one-sided: **core 0
 * must never wait for core 1.** A full OLED frame is ~23 ms of blocking I2C,
 * and an audio deadline is 1333 us; a mutex shared between the two would put
 * the audio path behind the panel. A seqlock costs core 0 two stores and two
 * barriers and cannot block it at all, and a reader that loses the race just
 * reads again.
 *
 * There are two of them rather than one struct, because the levels move at
 * 40 Hz and the spectrum at 25 Hz for different reasons, and the meters have
 * no business queueing behind a bandful of floats.
 */

#ifndef NOVADSP_DISPLAY_H
#define NOVADSP_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

#include "module.h"

#ifdef __cplusplus
extern "C" {
#endif

// Core 0, before audio_init(), while printing is still free. Seeds the
// spectrum channel at silence and calls the module's init(). Returns what the
// module returned - false means the hardware did not answer, and everything
// here is still safe to call afterwards.
bool display_init(void);

// Hands ownership to core 1 and starts the poll/redraw loop. Call after
// audio_init().
void display_start(void);

// Core 0 -> core 1. Wait-free on this side; call it as often as you like.
void display_publish(const display_status_t *s);

// The same handover again, on its own counter, for the spectrum bands.
void display_publish_spectrum(const spectrum_frame_t *s);

// Core 1 loop iterations. If this is not climbing, core 1 is not running and
// nothing on the panel will ever update again - which shows up in no audio
// counter, so nothing else would say so.
uint32_t display_core1_ticks(void);

// Which call core 1 is currently inside: 1 status_read, 2 spectrum_read,
// 3 the display module's tick, 4 sleep. Only meaningful when the tick count
// has stopped advancing.
uint32_t display_core1_stage(void);

// A breadcrumb for core 1, for finding where inside a display module's tick()
// it stopped. The core writes 1..4 around its own calls; a module may write
// anything above 100 for its own sub-steps. Read back by the once-a-second
// stall report, and meaningful only once the tick count has stopped moving.
void display_core1_mark(uint32_t where);

// Whatever the module counts as a failure, or 0 if it counts nothing.
uint32_t display_errors(void);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_DISPLAY_H
