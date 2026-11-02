/*
 * ui.h - the menu on the terminal. Runs on core 1, driven by the keyboard.
 *
 * The same state machine as the OLED module's, drawn 80x25 instead of 128x64
 * and with two differences that are the terminal's rather than a redesign:
 *
 *   **No idle screen.** On the panel, ten seconds without input switches to
 *   the spectrum analyser. There is no spectrum here - thirty-two bars
 *   redrawn at 25 fps is a poor use of a serial link, and the transform is
 *   still running on core 0 for whatever else wants it - and an idle screen
 *   with nothing on it is worse than the page the user left open. So the page
 *   stays where it was put, and DSP STATUS is the thing you leave up.
 *
 *   **A back key.** Esc and Left leave a page without walking the cursor up to
 *   "< Back" first. That row is still there and still works; the key is an
 *   addition, not a replacement. See keys.h.
 *
 * Everything else is deliberately identical, including the parts that look
 * like quirks and are not: rotating has no side effect, pressing a row that is
 * not the active effect selects it and pressing the one already running opens
 * its parameters, "< Back" is the first item of every list and "Reset" the
 * last, and nothing at all reaches flash until the Save row is pressed.
 *
 * Nothing here talks to an effect directly - see the core split in effect.h -
 * and nothing here names one either. The chain editor draws whichever effect
 * declares EFFECT_F_COMPOSITE, exactly as the OLED's does.
 */

#ifndef NOVADSP_SERIAL_UI_H
#define NOVADSP_SERIAL_UI_H

#include "module.h"
#include "keys.h"

#ifdef __cplusplus
extern "C" {
#endif

void serial_ui_init(void);

// Advance the state machine and draw into the term buffer. The caller flushes.
void serial_ui_tick(const keys_state_t *keys, const display_status_t *status);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_SERIAL_UI_H
