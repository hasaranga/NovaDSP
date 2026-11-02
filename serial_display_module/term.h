/*
 * term.h - an 80x25 character cell buffer with a dirty-diff flush over the USB
 * serial port. The serial display module's framebuffer.
 *
 * This is deliberately the same shape as sh1106.{c,h}: draw into a buffer, then
 * flush only what changed. On the OLED that was to avoid spending 23 ms of I2C
 * on a frame where nothing moved; here it is to avoid spending the serial link
 * on one. A full 80x25 repaint with colour changes is around 6 KB of escape
 * sequences, and at any usable frame rate that would both saturate the link and
 * make the screen flicker in the terminal. A diff of a moving level meter is a
 * few hundred bytes.
 *
 * ---- what a cell is ------------------------------------------------------
 *
 * One byte of glyph and one byte of attribute, which is exactly the IBM PC text
 * mode this is imitating and is why the whole buffer is 2 KB.
 *
 * A glyph below 0x80 is that ASCII character. A glyph at or above 0x80 is an
 * index into kGlyphs[] in term.c, which is how the box-drawing characters get
 * in without making a cell wide enough to hold a UTF-8 sequence.
 *
 * ---- the character set, and PuTTY ---------------------------------------
 *
 * The box-drawing glyphs are emitted as UTF-8, which is what current PuTTY
 * expects by default (Window -> Translation -> Remote character set, "UTF-8").
 * If a terminal is set to something else the frame will be drawn out of
 * mojibake and the text will still be readable; build with
 * -DNOVA_SERIAL_ASCII=1 to get +, - and | instead and lose nothing but looks.
 *
 * ---- colour --------------------------------------------------------------
 *
 * Sixteen foreground colours and eight background ones, as the PC text mode
 * had. Backgrounds are held to 0..7 on purpose rather than by oversight: the
 * bright-background codes are an aixterm extension that not every terminal
 * honours, and the palette does not need them.
 *
 * **The order below is the PC's, not ANSI's**, and the two are not the same:
 * the PC counts its colours BGR and an SGR parameter counts them RGB, so blue
 * is 1 here and 4 on the wire. term.c holds the translation table. Write
 * TC_BLUE and mean blue; do not hand a raw number to either side.
 */

#ifndef NOVADSP_SERIAL_TERM_H
#define NOVADSP_SERIAL_TERM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERM_W  80
#define TERM_H  25

// The PC text-mode palette, in its usual order.
enum {
    TC_BLACK = 0, TC_BLUE,      TC_GREEN,        TC_CYAN,
    TC_RED,       TC_MAGENTA,   TC_BROWN,        TC_LIGHTGRAY,
    TC_DARKGRAY,  TC_LIGHTBLUE, TC_LIGHTGREEN,   TC_LIGHTCYAN,
    TC_LIGHTRED,  TC_LIGHTMAGENTA, TC_YELLOW,    TC_WHITE,
};

// Foreground 0..15, background 0..7. A background above 7 is masked down
// rather than rejected, so a mistake is dim rather than invisible.
#define TERM_ATTR(fg, bg)  ((uint8_t)((((bg) & 0x07u) << 4) | ((fg) & 0x0Fu)))

// Glyphs that are not ASCII. Anything >= 0x80 indexes kGlyphs[] in term.c.
enum {
    TG_HLINE = 0x80,   // horizontal frame
    TG_VLINE,          // vertical frame
    TG_TL, TG_TR, TG_BL, TG_BR,
    TG_TEE_L, TG_TEE_R,
    TG_BLOCK,          // full cell, for meter bars
    TG_HALF,           // half-intensity fill, for meter backgrounds
    TG_UP, TG_DOWN,    // scroll indicators
    TG_MARK,           // the "this effect is live" marker
};

// Core 1. Clears the shadow, hides the cursor and forces a full repaint.
void term_init(void);

// Fill the whole buffer with spaces in this attribute. Call once per frame
// before drawing, the way ui.c calls sh1106_clear().
void term_clear(uint8_t attr);

void term_putc(int x, int y, uint8_t glyph, uint8_t attr);
void term_puts(int x, int y, const char *s, uint8_t attr);
void term_fill(int x, int y, int w, int h, uint8_t glyph, uint8_t attr);

// A single-line frame. w and h include the border.
void term_box(int x, int y, int w, int h, uint8_t attr);

// Formats into a stack buffer and never touches stdout, exactly as
// sh1106_printf() does - drawing and emitting are separate here for the same
// reason they are there. Truncates at TERM_W.
void term_printf(int x, int y, uint8_t attr, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

// Emit the difference between the buffer and what the terminal was last told,
// then remember it. Costs nothing at all if nothing changed.
//
// A frame where the host is not attached is dropped rather than queued, and
// the next attached frame is a full repaint - see term_invalidate().
void term_flush(void);

// Forget what the terminal was last told, so the next flush repaints
// everything. Wanted after the host reconnects, on the redraw key, and on a
// slow timer - a host that is attached but not draining can have part of a
// frame dropped by the CDC write after the shadow has been updated to say it
// was sent, and a periodic full repaint is the only thing that notices.
void term_invalidate(void);

// Whether the USB CDC host is attached. This follows DTR, which PuTTY asserts
// and a naive script does not - see the note in SESSION.md about
// System.IO.Ports leaving DtrEnable false.
bool term_connected(void);

// Bytes written since term_init(), for anyone wanting to know what the frame
// rate is costing.
uint32_t term_bytes_out(void);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_SERIAL_TERM_H
