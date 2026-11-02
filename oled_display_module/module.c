/*
 * oled_display_module - SH1106/SSD1306 128x64 OLED and a rotary encoder, as one
 * display module.
 *
 * This file is the module's whole contract with the core: bring the hardware
 * up on core 0 while printing is still free, then, on core 1, sample the knob
 * and repaint. Everything about *what* is painted and what a press does is in
 * ui.c, and nothing in the core knows any of it.
 *
 * ---- the pacing, which is the one thing that is not obvious --------------
 *
 * The core calls tick() every poll_us and hands it the latest numbers. That is
 * a sampling rate, not a repaint rate, and the two are deliberately different:
 *
 *   **poll at 2 ms** because that is what the knob needs. The PIO state
 *   machine is doing the catching, so nothing here has to be fast enough to
 *   see an edge - but the encoder's 100 nF debounce caps put transitions about
 *   2 ms apart at the top of its usable speed, and a poll slower than that
 *   starts folding two detents into one.
 *
 *   **repaint at 25 fps, or immediately when the knob moved.** The timer path
 *   is what keeps the idle countdown and the live meters moving when nothing
 *   is being touched; the movement path is what makes the control feel
 *   attached to the display rather than sampled. Repainting on every poll
 *   instead would be 500 fps of I2C for no visible difference.
 *
 * The flush is dirty-span only, so a repaint where nothing changed costs no
 * I2C at all - which is what makes the timer path free. See sh1106.c.
 *
 * ---- what may not happen here -------------------------------------------
 *
 * **No printf, ever, after init().** stdio's mutex is shared with core 0's
 * status line, and blocking on it would put the audio deadline at the mercy of
 * the USB host. sh1106_printf() formats into a stack buffer and never touches
 * stdout, which is why it exists.
 *
 * **Nothing here may run on core 0 once audio has started.** A full panel
 * refresh is ~23 ms of blocking I2C - seventeen consecutive audio deadlines.
 * init() and report() are the exceptions, and they run before audio_init().
 */

#include "module.h"
#include "display.h"

#include "pico/stdlib.h"

#include "encoder.h"
#include "sh1106.h"
#include "ui.h"

#define POLL_INTERVAL_US    2000u
#define REDRAW_INTERVAL_MS  40u

static encoder_state_t  enc;
static absolute_time_t  next_redraw;

// ---- core 0 -------------------------------------------------------------

static bool oled_init(void)
{
    encoder_init();
    return sh1106_init();
}

static void oled_report(void)
{
    NOVA_LOG("  display: OLED 128x64 + encoder GP%u/GP%u, push GP%u | panel ",
             ENC_PIN_A, ENC_PIN_B, ENC_PIN_PUSH);

    if (sh1106_present())
        NOVA_LOG("ok at 0x%02X on GP%u/GP%u\n",
                 sh1106_address(), OLED_PIN_SDA, OLED_PIN_SCL);
    else if (sh1106_status() == SH1106_INIT_FAILED)
        NOVA_LOG("answered at 0x%02X but rejected the init sequence\n",
                 sh1106_address());
    else
        NOVA_LOG("nothing at 0x3C or 0x3D on GP%u/GP%u - check wiring and 3V3\n",
                 OLED_PIN_SDA, OLED_PIN_SCL);
}

static uint32_t oled_errors(void) { return sh1106_error_count(); }

// ---- core 1 -------------------------------------------------------------

static void oled_start(void)
{
    ui_init();
    next_redraw = make_timeout_time_ms(0);
}

static void oled_tick(const display_status_t *status,
                      const spectrum_frame_t *spec)
{
    display_core1_mark(101);
    encoder_poll(&enc);

    const bool moved = enc.delta || enc.press_edge || enc.release_edge;
    if (!moved && !time_reached(next_redraw)) return;

    display_core1_mark(102);
    ui_tick(&enc, status, spec);
    display_core1_mark(103);
    sh1106_flush();
    display_core1_mark(104);          // dirty columns only, so an unchanged repaint
                             // costs no I2C at all
    // **Advance from the deadline, not from now.** Scheduling the next frame
    // from the moment this one finished made the period the interval *plus*
    // the frame time: measured on the panel as 40 + 23 = 63 ms, which reads as
    // 16 fps where 25 was asked for. It survived for months because it looks
    // like the panel simply being slow, and the 23 ms beside it looked like an
    // explanation - the same pair of bugs, covering for each other, that the
    // ST7789 produced.
    next_redraw = delayed_by_ms(next_redraw, REDRAW_INTERVAL_MS);

    // If a frame overran badly - or the knob was turned, which repaints off the
    // movement path and leaves this deadline in the past - do not then try to
    // catch up by drawing several frames back to back. Re-base instead. The
    // rate is a target, not a debt to be repaid.
    if (absolute_time_diff_us(get_absolute_time(), next_redraw) < 0)
        next_redraw = make_timeout_time_ms(REDRAW_INTERVAL_MS);
}

// ---- bring-up -----------------------------------------------------------
//
// Flashes the panel white twice, once under each controller's charge-pump
// command, and says which is which. This is what identified the panel on this
// board as an SSD1306 sold as an SH1106 - a part given the wrong charge-pump
// spelling **acknowledges it and ignores it**, so the I2C error count stays at
// zero, the banner reports the display found, and the screen is black. Nothing
// in any counter distinguishes that from a working panel showing black pixels.
//
// The call in oled_init() is commented out rather than the function deleted.
// Uncomment it if a replacement module is dark; it needs core 0 and a console,
// so it only works before audio_init() and only on a dev build.
__attribute__((unused))
static void oled_bringup_test(void)
{
    if (!sh1106_present()) return;

    NOVA_LOG("  OLED selftest A - white, SH1106 pump 0xAD 0x8B\n");
    sh1106_fill(true);
    sh1106_flush_all();
    sleep_ms(2000);
    sh1106_fill(false);
    sh1106_flush_all();
    sleep_ms(500);

    NOVA_LOG("  OLED selftest B - white, SSD1306 pump 0x8D 0x14\n");
    sh1106_command2(0x8Du, 0x14u);
    sh1106_command(0xAFu);
    sh1106_fill(true);
    sh1106_flush_all();
    sleep_ms(2000);
    sh1106_fill(false);
    sh1106_flush_all();

    NOVA_LOG("  OLED selftest done, %lu I2C errors\n",
             (unsigned long)sh1106_error_count());
}

// ---- the module ---------------------------------------------------------

static const display_module_t kModule = {
    .name    = "oled",
    .init    = oled_init,
    .start   = oled_start,
    .tick    = oled_tick,
    .report  = oled_report,
    .errors  = oled_errors,
    .poll_us = POLL_INTERVAL_US,
};

const display_module_t *oled_display_module(void) { return &kModule; }
