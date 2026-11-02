/*
 * ui.h - the menu on the OLED. Runs on core 1, driven by the encoder.
 *
 * Four pages. The first three carry a white title bar naming the page:
 *
 *   Nova DSP the effect list. Rotating only moves the cursor - it has no side
 *            effect at all. Pressing a row that is not the active effect
 *            selects it; pressing the one already running opens its
 *            parameters. A filled square marks which is live. The last row is
 *            not an effect but a way in to DSP STATUS.
 *   <effect> the parameter list, with "< Back" as the first item. Pressing a
 *            parameter starts editing it; rotating then changes the value and
 *            pressing again leaves it.
 *   STATUS   levels, overruns and load. Opened deliberately from the list, so
 *            unlike the idle screen it does not time out - it leaves on a
 *            press and ignores rotation.
 *   ABOUT    who built it. Same rules as STATUS.
 *   SPECTRUM the idle screen, after ten seconds without input: filled bands of
 *            whatever the active effect is putting out, with peak-hold caps,
 *            and the output level underneath as two full-width bars, L over R.
 *            The bands show how the output is distributed and deliberately say
 *            nothing about how loud it is - the tilt sees to that - so the
 *            meters are what makes this page readable across a room. Any input
 *            returns to the page that was open, and that first input is
 *            consumed by the wake so it cannot change anything by accident.
 *   ANIM     the rest of the idle screen, and **off unless
 *            ENABLE_VISUALIZATION is 1** (ui.c): stored animations, played
 *            from the clock rather than counted. With it on, every twenty
 *            seconds the idle screen slides to a different one at random - the
 *            spectrum included - for as long as the box is left alone. They
 *            wake exactly as the spectrum does, and waking never slides, so
 *            which one was showing is not something the user has to have
 *            noticed.
 *
 * Nothing here talks to an effect directly - see the core split in effect.h.
 *
 * And nothing here names an effect either. The chain editor draws whichever
 * effect declares EFFECT_F_COMPOSITE, reading its slot count from param_count
 * and its candidate names from the slots' own EP_ENUM labels, so this menu
 * works unchanged whether FX Chain is in the build or not - and would work for
 * a second composite nobody has written yet.
 */

#ifndef NOVADSP_UI_H
#define NOVADSP_UI_H

#include "module.h"
#include "encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);

// Advance the state machine and redraw into the framebuffer. The caller
// flushes. Call it on every encoder event and periodically besides, so the
// idle timer and the live figures both keep moving.
void ui_tick(const encoder_state_t *enc, const display_status_t *status,
             const spectrum_frame_t *spec);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_UI_H
