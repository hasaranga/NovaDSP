/*
 * serial_display_module - the USB serial port as the display and the keyboard
 * as the knob.
 *
 * A full menu on a terminal: the effect list, every parameter page, the chain
 * editor, DSP STATUS with live meters, and the Save row - the same state
 * machine the OLED module runs, drawn 80x25 in colour instead of 128x64 in
 * black and white. Open the board's COM port in PuTTY and it is there.
 *
 * It exists for a box with no panel fitted. Until now that build was
 * -DNOVA_DISPLAY=none, which on a dist image leaves no interface at all: the
 * board runs whatever was last saved to flash for ever, and the only way to
 * change it is to reflash. This is the other answer.
 *
 * ---- it is a dist-only module, and that is structural -------------------
 *
 * There is exactly one USB CDC device, and a display module that draws on it
 * cannot share it with a console that prints on it. On a dev build core 0
 * writes a status line to stdout once a second and reads single keys from
 * stdin in the main loop; both would land in the middle of this module's
 * escape sequences, and its keystrokes would be eaten by whichever core read
 * them first.
 *
 * dist is what makes the port free. NOVA_LOG compiles to nothing, the boot
 * banner and the benchmark are gone, and the getchar in NovaDSP.c's main loop
 * is inside `#if NOVA_TELEMETRY` - so **in a dist build core 0 never touches
 * stdio after stdio_init_all(), and core 1 owns the port outright.**
 *
 * That also disposes of the rule that core 1 must never call printf. The rule
 * was never about formatting; it was that stdio's mutex is shared with core
 * 0's status line, so blocking on it would put the audio deadline at the mercy
 * of the USB host. With nothing on core 0 contending for that mutex there is
 * no one to block, and the worst case is that this module's own frame is slow.
 *
 * The `#error` below is not a warning that got promoted. Built into a dev
 * image this module would produce a garbled screen and a menu that dropped
 * every other keystroke, and both would look like bugs in this file.
 *
 * ---- pacing --------------------------------------------------------------
 *
 * **poll at 20 ms.** There is no edge to catch - the CDC stack has already
 * buffered the keystrokes - so the only thing the rate has to beat is the
 * user's typing, and it also sets how long a lone Esc waits before it is taken
 * to be a real Esc rather than the start of an arrow (see keys.h).
 *
 * **repaint at 10 fps, or immediately when a key arrived.** The timer path
 * keeps the meters and the status bar moving; the key path is what makes the
 * menu feel attached to the keyboard. The flush is a diff, so a repaint where
 * nothing changed costs no bytes at all - which is what makes the timer path
 * nearly free, exactly as the dirty-span flush does on the OLED.
 *
 * **a full repaint every four seconds, and on reconnect.** A terminal that
 * attaches part-way through has been told none of what the shadow buffer
 * thinks it knows. term_flush() spots the reconnect through DTR and repaints.
 *
 * The periodic one is the belt to that pair of braces, and it covers the case
 * DTR cannot: a host that is attached but not draining, where the CDC write
 * times out and part of a frame is dropped after the shadow has already been
 * updated to say it was sent. Anything wrong on screen therefore corrects
 * itself within four seconds without anyone pressing anything, and 'r' is
 * there for the impatient. It costs about 6 KB every four seconds, which on a
 * link that is idle the rest of the time is nothing.
 */

#include "module.h"
#include "display.h"

#include "pico/stdlib.h"

#include "keys.h"
#include "term.h"
#include "ui.h"

#if NOVA_TELEMETRY
#error "serial_display_module needs a dist build: it and the console cannot \
share the one USB CDC port. Build it with -DNOVA_DIST=ON, or pick another \
display - -DNOVA_DISPLAY=oled or -DNOVA_DISPLAY=none."
#endif

#define POLL_INTERVAL_US      20000u
#define REDRAW_INTERVAL_MS      100u
#define FULL_REPAINT_MS        4000u

static keys_state_t    keys;
static absolute_time_t next_redraw;
static absolute_time_t next_full;

// ---- core 0 -------------------------------------------------------------

// Nothing to bring up: stdio_init_all() has already run in main(), and the
// port is there whether or not anything is attached to it. Returning true is
// honest rather than optimistic - a display module returns false to say its
// *hardware* did not answer, and there is no hardware here to fail. Whether a
// terminal is listening is a separate question, asked every frame by
// term_flush() and answered by DTR.
//
// Everything else waits for start(), on core 1, because the terminal is core
// 1's from then on and there is no reason for core 0 to have touched it first.
static bool serial_init(void)
{
    return true;
}

// ---- core 1 -------------------------------------------------------------

static void serial_start(void)
{
    term_init();
    keys_init();
    serial_ui_init();

    next_redraw = make_timeout_time_ms(0);
    next_full   = make_timeout_time_ms(FULL_REPAINT_MS);
}

static void serial_tick(const display_status_t *status,
                        const spectrum_frame_t *spec)
{
    // The spectrum is computed and published whatever display is fitted, and
    // this module deliberately does not draw it. Thirty-two bands at 25 fps is
    // a screenful of escape sequences several times a second for a picture
    // that is mostly moving noise at terminal resolution - and the idle screen
    // it would replace is a page the user left open on purpose. See ui.h.
    (void)spec;

    keys_poll(&keys);

    const bool pressed = keys.delta || keys.press_edge || keys.back_edge
                      || keys.save_edge || keys.redraw_edge;

    if (time_reached(next_full)) {
        term_invalidate();
        next_full = make_timeout_time_ms(FULL_REPAINT_MS);
    } else if (!pressed && !time_reached(next_redraw)) {
        return;
    }

    serial_ui_tick(&keys, status);
    term_flush();               // diff only, so an unchanged frame costs nothing

    next_redraw = make_timeout_time_ms(REDRAW_INTERVAL_MS);
}

// ---- the module ---------------------------------------------------------

static const display_module_t kModule = {
    .name    = "serial",
    .init    = serial_init,
    .start   = serial_start,
    .tick    = serial_tick,

    // No report() and no errors(), and neither is an omission.
    //
    // report() prints through NOVA_LOG, which in dist - the only build this
    // module compiles in - expands to nothing. A hook that can only ever be
    // called in a build where it cannot say anything is better left NULL than
    // written and never run.
    //
    // errors() is for what a panel can fail at: the OLED counts I2C timeouts,
    // because a display that stops acknowledging shows up in no audio counter.
    // There is nothing here that can fail that way. A host that is not
    // attached is not a fault, it is Tuesday, and term_flush() already handles
    // it by dropping the frame and repainting when it comes back.
    .report  = NULL,
    .errors  = NULL,

    .poll_us = POLL_INTERVAL_US,
};

const display_module_t *serial_display_module(void) { return &kModule; }
