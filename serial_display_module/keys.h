/*
 * keys.h - the keyboard, in the shape the encoder had.
 *
 * The OLED module's UI is driven by a struct carrying "how far the knob moved"
 * and "was it pressed". A keyboard can produce both, and expressing it that way
 * is what lets this module's ui.c be the same state machine as that one's
 * rather than a different design that happens to show the same numbers.
 *
 * It cannot include encoder.h to say so - module directories are not on the
 * include path, and reaching another module's header would mean writing
 * ../oled_display_module/encoder.h and meaning it. The shape is deliberately
 * the same; the struct is this module's own.
 *
 * ---- what a keyboard has that a knob does not ---------------------------
 *
 * A back key, and that is the one real departure. On the panel, leaving a page
 * means turning the knob up to "< Back" and pressing it, because there is
 * nothing else to press. Esc costs nothing here and not offering it would be
 * making the terminal worse to use in order to keep a hardware limitation that
 * does not apply. "< Back" is still the first row of every list, so the two
 * ways of leaving agree.
 *
 * Save and redraw are the same argument: 's' saves without walking to the Save
 * row, and 'r' repaints a terminal that something else has scribbled on.
 *
 * ---- decoding ------------------------------------------------------------
 *
 * Arrow keys arrive as ESC [ A and friends, and PuTTY sends ESC O A instead
 * when the host has put it in application cursor mode - both are accepted. The
 * awkward case is a bare Esc, which is indistinguishable from the start of an
 * arrow until either the rest arrives or it does not, so a lone ESC is held for
 * ESC_TIMEOUT_MS and then delivered as a back. 'q' is there for anyone who
 * would rather not wait 50 ms.
 */

#ifndef NOVADSP_SERIAL_KEYS_H
#define NOVADSP_SERIAL_KEYS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// How long a lone ESC is held before it is taken to be a real Esc rather than
// the start of an arrow sequence. Longer than any terminal takes to send the
// remaining two bytes, short enough not to feel like a delay.
#define KEYS_ESC_TIMEOUT_MS  50u

typedef struct {
    int32_t delta;        // net cursor movement since the previous poll
    bool    press_edge;   // Enter, Space or Right - "the knob was pressed"
    bool    back_edge;    // Esc, Left, q or Backspace
    bool    save_edge;    // s
    bool    redraw_edge;  // r or Ctrl-L
} keys_state_t;

void keys_init(void);

// Drains whatever the host has sent and folds it into *out. Everything in the
// struct describes the interval since the previous call and is cleared by it,
// exactly as encoder_poll() does.
void keys_poll(keys_state_t *out);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_SERIAL_KEYS_H
