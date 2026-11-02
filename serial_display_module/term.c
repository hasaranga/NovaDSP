#include "term.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

// ---- the buffer ---------------------------------------------------------
//
// Two of them: what the next flush should show, and what the terminal was last
// told. 80 * 25 * 2 bytes each, so 4 KB in .bss for the pair - four times the
// OLED framebuffer, and the whole of what this display module costs in RAM.

typedef struct {
    uint8_t glyph;
    uint8_t attr;
} cell_t;

static cell_t fb[TERM_H][TERM_W];
static cell_t shadow[TERM_H][TERM_W];

static bool     full_repaint = true;
static bool     was_connected;
static uint32_t bytes_out;

// ---- the non-ASCII glyphs -----------------------------------------------
//
// Indexed by glyph - TG_HLINE. UTF-8 by default; the ASCII set is there for a
// terminal that is not set to UTF-8, and loses only the frame's looks.
#if NOVA_SERIAL_ASCII
static const char *const kGlyphs[] = {
    "-", "|", "+", "+", "+", "+", "+", "+", "#", ":", "^", "v", "*",
};
#else
static const char *const kGlyphs[] = {
    "\xe2\x94\x80",   // TG_HLINE   U+2500
    "\xe2\x94\x82",   // TG_VLINE   U+2502
    "\xe2\x94\x8c",   // TG_TL      U+250C
    "\xe2\x94\x90",   // TG_TR      U+2510
    "\xe2\x94\x94",   // TG_BL      U+2514
    "\xe2\x94\x98",   // TG_BR      U+2518
    "\xe2\x94\x9c",   // TG_TEE_L   U+251C
    "\xe2\x94\xa4",   // TG_TEE_R   U+2524
    "\xe2\x96\x88",   // TG_BLOCK   U+2588
    "\xe2\x96\x91",   // TG_HALF    U+2591
    "\xe2\x96\xb2",   // TG_UP      U+25B2
    "\xe2\x96\xbc",   // TG_DOWN    U+25BC
    "\xe2\x96\xa0",   // TG_MARK    U+25A0
};
#endif

#define GLYPH_COUNT ((int)(sizeof kGlyphs / sizeof kGlyphs[0]))

// ---- drawing ------------------------------------------------------------

void term_clear(uint8_t attr)
{
    for (int y = 0; y < TERM_H; ++y)
        for (int x = 0; x < TERM_W; ++x) {
            fb[y][x].glyph = ' ';
            fb[y][x].attr  = attr;
        }
}

void term_putc(int x, int y, uint8_t glyph, uint8_t attr)
{
    if (x < 0 || x >= TERM_W || y < 0 || y >= TERM_H) return;
    fb[y][x].glyph = glyph;
    fb[y][x].attr  = attr;
}

void term_puts(int x, int y, const char *s, uint8_t attr)
{
    if (!s) return;
    for (; *s && x < TERM_W; ++s, ++x) term_putc(x, y, (uint8_t)*s, attr);
}

void term_fill(int x, int y, int w, int h, uint8_t glyph, uint8_t attr)
{
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) term_putc(x + c, y + r, glyph, attr);
}

void term_box(int x, int y, int w, int h, uint8_t attr)
{
    if (w < 2 || h < 2) return;

    term_fill(x + 1, y,         w - 2, 1, TG_HLINE, attr);
    term_fill(x + 1, y + h - 1, w - 2, 1, TG_HLINE, attr);
    term_fill(x,         y + 1, 1, h - 2, TG_VLINE, attr);
    term_fill(x + w - 1, y + 1, 1, h - 2, TG_VLINE, attr);

    term_putc(x,         y,         TG_TL, attr);
    term_putc(x + w - 1, y,         TG_TR, attr);
    term_putc(x,         y + h - 1, TG_BL, attr);
    term_putc(x + w - 1, y + h - 1, TG_BR, attr);
}

void term_printf(int x, int y, uint8_t attr, const char *fmt, ...)
{
    char buf[TERM_W + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    term_puts(x, y, buf, attr);
}

// ---- emitting -----------------------------------------------------------
//
// Everything goes through one small buffer rather than a putchar per cell,
// because a per-cell write to the CDC stack is a function call and a mutex
// where a 256-byte write is one of each.

static char     obuf[256];
static unsigned olen;

static void oflush(void)
{
    if (!olen) return;
    fwrite(obuf, 1, olen, stdout);
    bytes_out += olen;
    olen = 0;
}

static void oput(const char *s, unsigned n)
{
    if (olen + n > sizeof obuf) oflush();
    if (n > sizeof obuf) return;              // cannot happen; keeps it total
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void ostr(const char *s) { oput(s, (unsigned)strlen(s)); }

static void ofmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ofmt(const char *fmt, ...)
{
    char t[32];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n > 0) oput(t, (unsigned)n);
}

// ---- the palettes are not in the same order ------------------------------
//
// **The PC text mode counts its colours BGR and ANSI counts them RGB**, so the
// two agree on six of sixteen entries and disagree on the rest. Bit 0 is blue
// in the palette term.h declares and red in an SGR parameter; bit 2 is the
// other way round. Bits 1 and 3 - green and the intensity bit - line up.
//
// Emitting `40 + bg` therefore turned TC_BLUE into SGR 41 and painted the whole
// page red, which is how this was found. TC_CYAN came out yellow, TC_YELLOW
// came out cyan, and TC_LIGHTRED came out light blue; TC_BLACK, TC_GREEN,
// TC_MAGENTA, TC_LIGHTGRAY and their bright partners happened to land on
// themselves, which is what made the fault look like one wrong colour rather
// than nine.
//
// The fix is the bit swap, written as a table because a table can be read
// against a colour chart and `((c & 1) << 2) | ((c >> 2) & 1)` cannot.
static const uint8_t kAnsi[16] = {
    0,   // TC_BLACK          -> 0  black
    4,   // TC_BLUE           -> 4  blue
    2,   // TC_GREEN          -> 2  green
    6,   // TC_CYAN           -> 6  cyan
    1,   // TC_RED            -> 1  red
    5,   // TC_MAGENTA        -> 5  magenta
    3,   // TC_BROWN          -> 3  yellow, which at normal intensity is brown
    7,   // TC_LIGHTGRAY      -> 7  white
    8,   // TC_DARKGRAY       -> 8  bright black
    12,  // TC_LIGHTBLUE      -> 12 bright blue
    10,  // TC_LIGHTGREEN     -> 10 bright green
    14,  // TC_LIGHTCYAN      -> 14 bright cyan
    9,   // TC_LIGHTRED       -> 9  bright red
    13,  // TC_LIGHTMAGENTA   -> 13 bright magenta
    11,  // TC_YELLOW         -> 11 bright yellow
    15,  // TC_WHITE          -> 15 bright white
};

// SGR for one attribute. Foreground uses the bright (90..97) range above 7;
// background stays in 40..47 - see the note in term.h about aixterm.
static void emit_attr(uint8_t attr)
{
    const int fg = kAnsi[attr & 0x0Fu];
    const int bg = kAnsi[(attr >> 4) & 0x07u];

    ofmt("\x1b[%d;%dm", fg < 8 ? 30 + fg : 90 + (fg - 8), 40 + bg);
}

static void emit_glyph(uint8_t g)
{
    if (g >= 0x80u) {
        const int i = g - TG_HLINE;
        ostr((i >= 0 && i < GLYPH_COUNT) ? kGlyphs[i] : "?");
    } else {
        const char c = (char)(g < 0x20u ? ' ' : g);
        oput(&c, 1);
    }
}

void term_invalidate(void) { full_repaint = true; }

void term_flush(void)
{
    // A host that is not attached gets nothing, and the next attached frame is
    // a full repaint. Queueing would be worse than useless: the CDC stack
    // discards it anyway, and the shadow would then claim the terminal had
    // been told things it never saw.
    const bool conn = stdio_usb_connected();
    if (conn && !was_connected) full_repaint = true;
    was_connected = conn;

    if (!conn) {
        full_repaint = true;
        return;
    }

    // Hide the cursor, turn autowrap off, and go home. Repeated on every full
    // repaint rather than only at init, because a full repaint is exactly what
    // happens when a terminal has just attached and knows none of this.
    //
    // **Autowrap off is the load-bearing one.** The bottom-right cell of a
    // full-screen page is the classic way to lose a layout: with autowrap on,
    // writing there leaves the terminal in a pending-wrap state, and the next
    // character scrolls the whole page up by a line. Nothing here writes that
    // next character - a frame ends with a cursor move - so it would work by
    // luck, on this terminal, today. ESC[?7l stops it depending on that.
    if (full_repaint) ostr("\x1b[?25l\x1b[?7l\x1b[H");

    int  last_attr = -1;     // no SGR emitted yet this flush
    int  cx = -1, cy = -1;   // where the terminal's cursor is, as far as we know
    bool wrote = false;      // whether anything at all was emitted

    for (int y = 0; y < TERM_H; ++y) {
        for (int x = 0; x < TERM_W; ++x) {
            const cell_t *n = &fb[y][x];
            const cell_t *o = &shadow[y][x];

            if (!full_repaint && n->glyph == o->glyph && n->attr == o->attr)
                continue;

            // Only move the cursor when it is not already where it needs to
            // be. On a run of changed cells - which is what a moving meter bar
            // or a redrawn row actually is - that is one escape sequence for
            // the whole run rather than one per cell.
            if (cy != y || cx != x) {
                ofmt("\x1b[%d;%dH", y + 1, x + 1);
                cy = y;
                cx = x;
            }

            if ((int)n->attr != last_attr) {
                emit_attr(n->attr);
                last_attr = n->attr;
            }

            emit_glyph(n->glyph);
            cx++;
            wrote = true;

            shadow[y][x] = *n;
        }
    }

    // Leave the terminal in a sane state whatever it was mid-frame, and park
    // the cursor on the last line so a stray host echo lands somewhere
    // harmless rather than in the middle of the page.
    //
    // The test is `wrote` and not `olen`: a frame larger than obuf has already
    // been part-flushed inside the loop, and one that happens to end exactly on
    // a buffer boundary would leave olen at zero with the terminal mid-frame,
    // holding whatever SGR the last cell set.
    if (wrote) {
        ostr("\x1b[0m\x1b[25;1H");
        oflush();
        fflush(stdout);
    }

    full_repaint = false;
}

void term_init(void)
{
    memset(fb,     0, sizeof fb);
    memset(shadow, 0, sizeof shadow);
    term_clear(TERM_ATTR(TC_LIGHTGRAY, TC_BLACK));

    olen          = 0;
    bytes_out     = 0;
    was_connected = false;
    full_repaint  = true;
}

bool term_connected(void) { return stdio_usb_connected(); }

uint32_t term_bytes_out(void) { return bytes_out; }
