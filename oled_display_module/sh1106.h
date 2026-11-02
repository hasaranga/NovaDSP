/*
 * sh1106.h - SH1106 128x64 I2C OLED.
 *
 * Framebuffer in RAM, page-at-a-time flush over I2C, dirty-column tracking so
 * a small update costs a short transfer rather than a whole frame.
 *
 * The SH1106 is not an SSD1306, whatever the module is sold as. Two
 * differences bite:
 *
 *   - Its RAM is 132 columns wide and the panel shows columns 2..129, so an
 *     SSD1306 driver renders shifted two pixels. SH1106_COL_OFFSET is that
 *     two.
 *   - It has no horizontal addressing mode. The column pointer does not roll
 *     into the next page, so every page needs its own address setup - hence
 *     the eight transfers per full frame.
 *
 * A full frame is 1024 bytes plus overhead: about 23 ms at 400 kHz. That is
 * seventeen audio block deadlines, so none of this may run on core 0.
 */

#ifndef NOVADSP_SH1106_H
#define NOVADSP_SH1106_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_PIN_SDA        26u        // I2C1 SDA
#define OLED_PIN_SCL        27u        // I2C1 SCL
#define OLED_I2C_HZ         400000u

#define SH1106_WIDTH        128
#define SH1106_HEIGHT       64
#define SH1106_PAGES        (SH1106_HEIGHT / 8)
// 0 for the panel on this board, 2 for a true SH1106.
//
// An SH1106 has 132 columns of RAM and shows columns 2..129, so a driver has
// to offset by 2 or the image sits two pixels left of where it should. This
// panel is an SSD1306 (see the charge-pump note in sh1106.c), whose RAM is
// exactly 128 wide - and the offset proved it: writing 128 bytes from column 2
// ran off the end and the last two columns *wrapped round to the left edge*.
// Anything reaching the right edge showed it. Text did not, which is why it
// only appeared once the status bars and the scrollbar were drawn.
#define SH1106_COL_OFFSET   0

#define SH1106_CHAR_W       6          // 5x7 glyph in a 6x8 cell
#define SH1106_CHAR_H       8
#define SH1106_COLS         (SH1106_WIDTH / SH1106_CHAR_W)    // 21
#define SH1106_ROWS         (SH1106_HEIGHT / SH1106_CHAR_H)   // 8

// Brings up I2C on the pins above, probes 0x3C then 0x3D, and runs the init
// sequence. Returns false if neither address answers, in which case every
// other call in this file is a no-op and the caller can carry on without a
// display. Never blocks indefinitely: all transfers are timed out.
bool sh1106_init(void);

// Why init failed, for a banner that can tell "nothing on the bus" apart from
// "something answered but would not take the init sequence" - two different
// faults that otherwise look identical.
typedef enum {
    SH1106_OK = 0,
    SH1106_NO_DEVICE,     // nothing acknowledged at 0x3C or 0x3D
    SH1106_INIT_FAILED,   // a device answered but rejected a command
} sh1106_status_t;

sh1106_status_t sh1106_status(void);

bool sh1106_present(void);
uint8_t sh1106_address(void);

// ---- drawing (framebuffer only; nothing reaches the panel until flush) --

void sh1106_clear(void);

// A whole frame of stored artwork, page-packed in the panel's own layout -
// data[page * SH1106_WIDTH + x], bit (y & 7), bit 0 the topmost row of the
// page. That is fb's layout exactly, so this is one 1 KB memcpy and not a
// pixel loop. The source must be SH1106_PAGES * SH1106_WIDTH bytes.
void sh1106_blit(const uint8_t *page_packed);

// The framebuffer back out again, in the same layout. For capturing the frame
// on screen before drawing over it - see sh1106_slide(). dst must be
// SH1106_PAGES * SH1106_WIDTH bytes.
void sh1106_snapshot(uint8_t *dst);

// Which way the outgoing frame leaves. The incoming one follows it in from the
// opposite edge.
typedef enum {
    SH1106_SLIDE_LEFT = 0,
    SH1106_SLIDE_RIGHT,
    SH1106_SLIDE_UP,
    SH1106_SLIDE_DOWN,
    SH1106_SLIDE_DIRS          // how many there are, for picking one at random
} sh1106_slide_dir_t;

// How far a slide in this direction has to travel: the width for the
// horizontal pair, the height for the vertical. Ask rather than assume - it is
// what lets a caller pick a direction at random without knowing which axis it
// landed on.
int sh1106_slide_travel(sh1106_slide_dir_t dir);

// A slide between two whole frames: `prev` leaving and whatever is already in
// the framebuffer arriving, `shift` pixels through sh1106_slide_travel(dir).
//
// Draw the incoming frame normally, then call this. shift <= 0 gives the
// outgoing frame alone and shift >= the travel leaves the incoming one
// untouched, so both ends of the travel need no special case.
//
// Horizontal is memcpy work; vertical packs each column into a uint64_t and
// shifts it, because a byte here is eight stacked pixels. Both are cheap, but
// they are not the same cost - see sh1106.c.
void sh1106_slide(const uint8_t *prev, int shift, sh1106_slide_dir_t dir);

void sh1106_pixel(int x, int y, bool on);
void sh1106_fill_rect(int x, int y, int w, int h, bool on);

// Draw at pixel coordinates - y need not be a multiple of 8. Returns the x
// just past the last glyph, so calls can be chained. Text is clipped at the
// right edge rather than wrapped.
int sh1106_text(int x, int y, const char *s);
int sh1106_text_inv(int x, int y, const char *s);   // dark on light, for a
                                                    // selected row

// printf into a 48-byte stack buffer and draw the result. Formats only - it
// does not touch stdout, so unlike printf it is safe to call from core 1
// while core 0 is real-time.
int sh1106_printf(int x, int y, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// ---- panel -------------------------------------------------------------

// Sends only the columns that changed since the last flush. Cheap when little
// moved, ~23 ms when everything did.
void sh1106_flush(void);
void sh1106_flush_all(void);

// Frames flushed, the longest single flush, and the payload of the largest,
// all since the last call - which resets them, so one call covers exactly the
// interval between two. **One flush is one frame**, so counting flushes over a
// known window gives the rate the panel achieved rather than the one asked
// for. `bytes` is the payload summed over the window, so bytes/frames is what
// a frame costs - and that is what separates a slow bus from sending
// everything every frame. A full refresh is SH1106_PAGES * SH1106_WIDTH = 1024
// and an unchanged repaint should be 0.
void sh1106_stats_take(uint32_t *frames, uint32_t *longest_us, uint32_t *bytes);

// ---- bring-up ----------------------------------------------------------

// Whole framebuffer to one colour. sh1106_fill(true) + sh1106_flush_all() is
// the unambiguous "is this panel alive" test: it does not depend on the font,
// the column offset, or anything having been drawn.
void sh1106_fill(bool on);

// Raw command access, for trying controller variants during bring-up.
bool sh1106_command(uint8_t c);
bool sh1106_command2(uint8_t c, uint8_t arg);

// Transfers that failed or timed out since init. Should stay at 0.
uint32_t sh1106_error_count(void);

void sh1106_contrast(uint8_t level);   // 0..255
void sh1106_display_on(bool on);
void sh1106_invert(bool inverted);     // whole-panel inversion, no redraw

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_SH1106_H
