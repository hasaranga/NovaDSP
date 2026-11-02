#include "sh1106.h"
#include "display.h"   // display_core1_mark, for the stall report

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define OLED_I2C_PORT   i2c1        // GP26/GP27 are an I2C1 pair; the I2C
                                    // function is fixed per pin on RP2350

// Every transfer is bounded. A display that browns out or is unplugged must
// not be able to wedge the core that owns it.
#define I2C_TIMEOUT_US  20000

// Co = 0, D/C# = 0 -> the rest of the transfer is commands.
#define CTRL_CMD        0x00u
// Co = 0, D/C# = 1 -> the rest of the transfer is display data.
#define CTRL_DATA       0x40u

static bool     present;
static uint8_t  addr;
static uint32_t err_count;
static sh1106_status_t status = SH1106_NO_DEVICE;

// fb[page][x]: one byte is eight vertical pixels, bit 0 topmost. That is the
// panel's own RAM layout, so a flush is a straight copy with no repacking.
static uint8_t  fb[SH1106_PAGES][SH1106_WIDTH];

// **What the panel was last told**, not what has been drawn. The two are the
// same only at the moment a flush finishes, and the difference is the whole
// point: the UI clears and redraws every pass, so an ink byte changes twice in
// a frame - content -> 0 -> the same content - and per-write tracking marks
// both transitions. It is right to: both were real changes to `fb`. But the
// frame that results is identical to the one the panel already holds, so the
// span covers the whole ink extent and a static page re-sends it.
//
// Measured on the board before this existed: **835 bytes and 23 ms a frame on
// a silent, untouched DSP STATUS**, where sh1106_clear()'s comment claimed an
// unchanged repaint sent nothing at all.
//
// Diffing against what was sent makes that property true by construction
// rather than by every draw call remembering to compare. It also made the
// drawing path cheaper - every write below is now a plain store with no
// read-compare, and sh1106_clear() is one memset.
//
// 1 KB. The ST7789 pays 57.6 KB for the same buffer and judged it worth it.
static uint8_t  shadow[SH1106_PAGES][SH1106_WIDTH];

// ---- what a frame actually costs ---------------------------------------
//
// Instrumentation, ported from st7789.c. Asking that module for its own frame
// rate found two bugs in an hour that no build could see, and the same two are
// suspected here - a clear-then-redraw that dirties every column holding ink,
// and a redraw timer scheduled from the end of the frame. Both are arguments
// until this is read.
//
// Bytes are counted as well as time, and that is the addition. On the ST7789
// the two bugs covered for each other because time alone could not separate
// "the panel is slow" from "we are sending everything every frame". Here the
// prediction is exact: a static page that re-sends its whole ink extent is
// SH1106_PAGES * SH1106_WIDTH = 1024 bytes, ~23 ms at 400 kHz. Bytes say which
// fault it is; time says whether it matters.
static uint32_t stat_frames;
static uint32_t stat_longest_us;
static uint32_t stat_bytes;        // payload bytes, summed over the window

// ---- 5x7 font, ASCII 32..126 -------------------------------------------
// Column-major, five columns per glyph, bit 0 = top row. The sixth column of
// the cell is the inter-character gap and is not stored. This is the classic
// 5x7 table that has been copied through a hundred display libraries; if the
// provenance matters for your build, swap in one whose licence you know.
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
    {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x01,0x01}, {0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x04,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63}, {0x03,0x04,0x78,0x04,0x03},
    {0x61,0x51,0x49,0x45,0x43}, {0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20}, {0x41,0x41,0x7F,0x00,0x00},
    {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02}, {0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00}, {0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C},
    {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00},
    {0x08,0x08,0x2A,0x1C,0x08},
};

// ---- transport ---------------------------------------------------------

static bool cmd(uint8_t c)
{
    uint8_t b[2] = { CTRL_CMD, c };
    bool ok = i2c_write_timeout_us(OLED_I2C_PORT, addr, b, 2, false,
                                   I2C_TIMEOUT_US) == 2;
    if (!ok) err_count++;
    return ok;
}

static bool cmd2(uint8_t c, uint8_t arg)
{
    uint8_t b[3] = { CTRL_CMD, c, arg };
    bool ok = i2c_write_timeout_us(OLED_I2C_PORT, addr, b, 3, false,
                                   I2C_TIMEOUT_US) == 3;
    if (!ok) err_count++;
    return ok;
}

// ---- init --------------------------------------------------------------

static const uint8_t init_seq[] = {
    0xAE,               // display off - the pump settings below want it off
    0xD5, 0x80,         // clock divide 1, oscillator default
    0xA8, 0x3F,         // multiplex ratio 1/64
    0xD3, 0x00,         // display offset 0
    0x40,               // start line 0
    // Charge pump, both spellings. This is not belt-and-braces for its own
    // sake: the panel on this board ACKs every byte of the SH1106 sequence and
    // stays dark until it gets 0x8D 0x14, so whatever the module was sold as,
    // its controller is an SSD1306. Each part ignores the other's command -
    // 0x8D/0x8B are undefined opcodes on the part that does not own them, and
    // 0x14 lands on the SH1106 as "set higher column address", which is
    // harmless because flush_page() sets the column before every transfer.
    // Sending both makes the driver work on either without identifying it.
    0xAD, 0x8B,         // SH1106: DC-DC control mode, converter on
    0x8D, 0x14,         // SSD1306: charge pump enable
    0x32,               // SH1106 pump output 8.0 V (0x30..0x33); undefined and
                        // ignored on an SSD1306
    0xA1,               // segment remap: SEG127 -> column 0
    0xC8,               // COM scan direction reversed. A1 and C8 together are
                        // the usual orientation for these modules; drop both
                        // for a 180-degree rotation.
    0xDA, 0x12,         // COM pins: alternative, no left/right remap
    0x81, 0x80,         // contrast
    0xD9, 0x22,         // pre-charge
    0xDB, 0x35,         // VCOM deselect
    0xA4,               // display follows RAM (not all-on)
    0xA6,               // not inverted
    0xAF,               // display on
};

// Free a bus left locked by a reset that landed mid-transfer.
//
// This is not theoretical here. Core 1 writes the panel continuously, so a
// reset - picotool, BOOTSEL, the debugger - almost always interrupts a byte.
// The panel is then still clocking out an acknowledge and holding SDA low,
// and an I2C master brought up onto a bus with SDA already low sees no start
// condition it can win: every probe NAKs and the display looks absent.
//
// The cure is to finish the transfer on the slave's behalf: clock SCL until it
// lets go of SDA, then issue a STOP. Pins are driven open-drain the usual way
// on RP2350 - the output register stays at 0 and the direction is toggled, so
// "high" is the external pull-up and never a driven level that could fight it.
static void i2c_bus_recover(void)
{
    gpio_init(OLED_PIN_SCL);
    gpio_init(OLED_PIN_SDA);
    gpio_put(OLED_PIN_SCL, 0);
    gpio_put(OLED_PIN_SDA, 0);
    gpio_set_dir(OLED_PIN_SCL, GPIO_IN);   // released, pulled high
    gpio_set_dir(OLED_PIN_SDA, GPIO_IN);
    gpio_pull_up(OLED_PIN_SCL);
    gpio_pull_up(OLED_PIN_SDA);
    sleep_us(10);

    // Nine at most: a slave interrupted mid-byte can owe eight data bits plus
    // the acknowledge, and nothing legal holds SDA longer than that.
    for (int i = 0; i < 9 && !gpio_get(OLED_PIN_SDA); i++) {
        gpio_set_dir(OLED_PIN_SCL, GPIO_OUT);   // drive low
        sleep_us(5);
        gpio_set_dir(OLED_PIN_SCL, GPIO_IN);    // release
        sleep_us(5);
    }

    // STOP: SDA released while SCL is already high.
    gpio_set_dir(OLED_PIN_SDA, GPIO_OUT);
    sleep_us(5);
    gpio_set_dir(OLED_PIN_SCL, GPIO_IN);
    sleep_us(5);
    gpio_set_dir(OLED_PIN_SDA, GPIO_IN);
    sleep_us(5);
}

static bool probe(uint8_t a)
{
    uint8_t b[2] = { CTRL_CMD, 0xE3 };   // NOP - harmless, just wants an ACK
    return i2c_write_timeout_us(OLED_I2C_PORT, a, b, 2, false,
                                I2C_TIMEOUT_US) == 2;
}

// How long after its supply comes up the panel needs before it will accept an
// init sequence. The datasheet asks for a wait after VCC is stable; the module
// adds an RC on its own reset, so this is the generous end of it. It is spent
// once, at boot, before audio_init() - it cannot touch a deadline.
#define PANEL_READY_US 250000u

bool sh1106_init(void)
{
    present = false;
    addr = 0;
    status = SH1106_NO_DEVICE;

    // ---- wait for the panel's own power-on, not for the build -------------
    //
    // **This is what made the dist build come up with a black screen while dev
    // was fine, and it is worth understanding before deleting it.** Below its
    // power-on time the panel answers on I2C perfectly - the interface block
    // wakes long before the display side does - and quietly ignores the charge
    // pump and display-on commands. Every byte is acknowledged, the probe
    // succeeds, `init_seq` returns success, the error count stays at zero, and
    // the screen stays black. Nothing anywhere reports a fault.
    //
    // dev never hit it by luck rather than by design: it spends two seconds
    // waiting for a USB host and another four hundred milliseconds in the boot
    // benchmark before reaching this function, and dist has neither, so it
    // arrived here about 2.4 s earlier and too soon. A panel initialised this
    // late by accident is a panel that breaks the day someone deletes a print.
    //
    // time_us_64() is time since the RP2350 came up, which is the same supply
    // the panel is on, so this waits for the remainder of the panel's settling
    // time and no more. In dev it is already long past and this costs nothing;
    // that is the point of writing it as a deadline rather than a sleep.
    //
    // Only a cold start needs it. A warm reboot leaves the panel powered and
    // already initialised, which is exactly why this hid for so long - the
    // usual way to reflash is picotool, and picotool does not power-cycle it.
    const uint64_t now = time_us_64();
    if (now < PANEL_READY_US)
        sleep_us(PANEL_READY_US - now);

    i2c_bus_recover();

    i2c_init(OLED_I2C_PORT, OLED_I2C_HZ);
    gpio_set_function(OLED_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OLED_PIN_SCL, GPIO_FUNC_I2C);
    // The module carries its own pull-ups; these are a weak backstop only and
    // would not be enough on their own at 400 kHz.
    gpio_pull_up(OLED_PIN_SDA);
    gpio_pull_up(OLED_PIN_SCL);

    if      (probe(0x3Cu)) addr = 0x3Cu;
    else if (probe(0x3Du)) addr = 0x3Du;
    else return false;

    status = SH1106_INIT_FAILED;   // downgraded to OK once the sequence lands

    for (size_t i = 0; i < sizeof(init_seq); ) {
        uint8_t c = init_seq[i++];
        bool ok;
        // The two-byte commands are the ones whose value follows in the table.
        switch (c) {
        case 0xD5: case 0xA8: case 0xD3: case 0xAD: case 0x8D:
        case 0xDA: case 0x81: case 0xD9: case 0xDB:
            ok = cmd2(c, init_seq[i++]);
            break;
        default:
            ok = cmd(c);
            break;
        }
        if (!ok) return false;
    }

    present = true;
    status = SH1106_OK;
    sh1106_clear();
    sh1106_flush_all();
    return true;
}

bool sh1106_present(void)      { return present; }
// Reports the probed address even when the init sequence later failed - that
// is exactly the case where knowing it is useful.
uint8_t sh1106_address(void)   { return addr; }
sh1106_status_t sh1106_status(void) { return status; }

// ---- drawing -----------------------------------------------------------

// **An unchanged redraw sends nothing at all** - but that is sh1106_flush()'s
// property now, not this function's, and it used to be claimed here and was
// false. Comparing here cannot work: the UI clears and redraws every pass, so
// this write and the redraw that follows it are both genuine changes to `fb`
// and there is nothing at this level that knows the pair cancels. Only the
// shadow knows, because only the shadow holds what the panel was told.
void sh1106_clear(void)
{
    memset(fb, 0, sizeof fb);
}

// A whole frame at once, for stored artwork already held in the panel's own
// layout. The generator emits frames page-packed - data[page * W + x], bit
// (y & 7), bit 0 the topmost row - which is exactly fb's layout, so this is a
// memcpy rather than a pixel loop. Getting that wrong would cost 8192
// sh1106_pixel() calls a frame instead of one 1 KB copy.
//
// It does not touch the shadow: what reaches the panel is still whatever the
// next flush finds different, so a frame identical to the last one costs
// nothing on the bus.
void sh1106_blit(const uint8_t *page_packed)
{
    if (!page_packed) return;
    memcpy(fb, page_packed, sizeof fb);
}

void sh1106_snapshot(uint8_t *dst)
{
    if (!dst) return;
    memcpy(dst, fb, sizeof fb);
}

// ---- sliding one frame over another ------------------------------------
//
// The two axes cost very different things here, and the layout is why.
//
// **Sideways moves no bits at all.** x is the byte index within a page, so a
// horizontal slide is three memcpys per page.
//
// **Vertically, every byte is eight stacked pixels**, so the same idea has to
// shift across page boundaries. Done per pixel that would be 8192 bit tests a
// frame. Done a column at a time it is one shift: a column's 64 rows are eight
// bytes strided by WIDTH, and packing them into a uint64_t with bit y as row y
// makes the whole slide `>>` or `<<`. That is what slide_vertical does, and it
// is why the vertical case is a few hundred operations rather than a few
// thousand.
_Static_assert(SH1106_PAGES * 8 == 64,
               "a column no longer fits a uint64_t; slide_vertical assumes it");

int sh1106_slide_travel(sh1106_slide_dir_t dir)
{
    return (dir == SH1106_SLIDE_UP || dir == SH1106_SLIDE_DOWN)
         ? SH1106_HEIGHT : SH1106_WIDTH;
}

static void slide_horizontal(const uint8_t *prev, int shift, bool leftward)
{
    for (int p = 0; p < SH1106_PAGES; ++p) {
        // One page at a time through a 128-byte scratch, so this needs no
        // second framebuffer: the outgoing frame is the caller's, and the
        // incoming one is read out of fb before it is overwritten.
        uint8_t        row[SH1106_WIDTH];
        const uint8_t *in  = fb[p];                  // the incoming frame
        const uint8_t *out = prev + p * SH1106_WIDTH;

        if (leftward) {
            const int keep = SH1106_WIDTH - shift;
            memcpy(row,        out + shift, (size_t)keep);
            memcpy(row + keep, in,          (size_t)shift);
        } else {
            memcpy(row,         in + SH1106_WIDTH - shift, (size_t)shift);
            memcpy(row + shift, out,        (size_t)(SH1106_WIDTH - shift));
        }

        memcpy(fb[p], row, SH1106_WIDTH);
    }
}

static void slide_vertical(const uint8_t *prev, int shift, bool upward)
{
    // shift is 1..63 here - sh1106_slide() has already returned for both ends -
    // so neither shift below is by 64, which would be undefined.
    const int back = SH1106_HEIGHT - shift;

    for (int x = 0; x < SH1106_WIDTH; ++x) {
        uint64_t in = 0, out = 0;

        for (int p = 0; p < SH1106_PAGES; ++p) {
            in  |= (uint64_t)fb[p][x]                   << (p * 8);
            out |= (uint64_t)prev[p * SH1106_WIDTH + x] << (p * 8);
        }

        // Bit y is row y, so the outgoing frame moving up by `shift` is one
        // right shift, and the incoming frame follows it in at the bottom.
        const uint64_t composed = upward ? (out >> shift) | (in  << back)
                                         : (out << shift) | (in  >> back);

        for (int p = 0; p < SH1106_PAGES; ++p)
            fb[p][x] = (uint8_t)(composed >> (p * 8));
    }
}

// Compose a slide between two whole frames: `prev` leaving and whatever is
// currently in the framebuffer arriving, `shift` pixels through a travel of
// sh1106_slide_travel(dir).
//
// The caller draws the incoming frame normally first; this rewrites the
// framebuffer as the two images side by side, seen through a window sliding
// along them. shift <= 0 is the outgoing frame alone and shift >= travel
// leaves the incoming frame untouched, so the ends need no special case at the
// call site - and neither does the axis, because the travel comes from the
// direction rather than from the caller knowing which one it picked.
void sh1106_slide(const uint8_t *prev, int shift, sh1106_slide_dir_t dir)
{
    if (!prev) return;
    if (shift >= sh1106_slide_travel(dir)) return;   // already the new frame
    if (shift <= 0) { memcpy(fb, prev, sizeof fb); return; }

    switch (dir) {
    case SH1106_SLIDE_LEFT:  slide_horizontal(prev, shift, true);  break;
    case SH1106_SLIDE_RIGHT: slide_horizontal(prev, shift, false); break;
    case SH1106_SLIDE_UP:    slide_vertical(prev, shift, true);    break;
    default:                 slide_vertical(prev, shift, false);   break;
    }
}

void sh1106_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SH1106_WIDTH || y < 0 || y >= SH1106_HEIGHT) return;
    int page = y >> 3;
    uint8_t bit = (uint8_t)(1u << (y & 7));
    fb[page][x] = on ? (uint8_t)(fb[page][x] | bit)
                     : (uint8_t)(fb[page][x] & ~bit);
}

void sh1106_fill_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SH1106_WIDTH)  w = SH1106_WIDTH - x;
    if (y + h > SH1106_HEIGHT) h = SH1106_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    // Walk page by page rather than pixel by pixel: within a page the whole
    // run is one masked read-modify-write per column.
    for (int yy = y; yy < y + h; ) {
        int page = yy >> 3;
        int top  = yy & 7;
        int run  = 8 - top;
        if (run > y + h - yy) run = y + h - yy;

        uint8_t m = (uint8_t)(((1u << run) - 1u) << top);
        for (int xx = x; xx < x + w; xx++)
            fb[page][xx] = on ? (uint8_t)(fb[page][xx] | m)
                              : (uint8_t)(fb[page][xx] & ~m);
        yy += run;
    }
}

// One 8-pixel-tall column of glyph bits at an arbitrary y, so text does not
// have to sit on a page boundary. It straddles two pages whenever y is not a
// multiple of 8, which is why this writes through a mask rather than storing.
static void blit_col(int x, int y, uint8_t bits)
{
    if (x < 0 || x >= SH1106_WIDTH || y <= -8 || y >= SH1106_HEIGHT) return;

    int page  = y >> 3;                // arithmetic shift: floors when y < 0
    int shift = y & 7;
    uint32_t v = (uint32_t)bits << shift;
    uint32_t m = 0xFFu << shift;

    if (page >= 0)
        fb[page][x] = (uint8_t)((fb[page][x] & ~(uint8_t)m) | (uint8_t)v);
    if (shift && page + 1 >= 0 && page + 1 < SH1106_PAGES)
        fb[page + 1][x] = (uint8_t)((fb[page + 1][x] & ~(uint8_t)(m >> 8))
                                    | (uint8_t)(v >> 8));
}

static int draw_text(int x, int y, const char *s, bool inv)
{
    if (!s) return x;
    for (; *s; s++) {
        if (x >= SH1106_WIDTH) break;      // clip, do not wrap
        unsigned char ch = (unsigned char)*s;
        const uint8_t *g = font5x7[(ch < 32 || ch > 126) ? 0 : ch - 32];
        for (int c = 0; c < SH1106_CHAR_W; c++) {
            uint8_t bits = (c < 5) ? g[c] : 0x00u;   // 6th column is the gap
            blit_col(x + c, y, inv ? (uint8_t)~bits : bits);
        }
        x += SH1106_CHAR_W;
    }
    return x;
}

int sh1106_text(int x, int y, const char *s)     { return draw_text(x, y, s, false); }
int sh1106_text_inv(int x, int y, const char *s) { return draw_text(x, y, s, true); }

int sh1106_printf(int x, int y, const char *fmt, ...)
{
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return draw_text(x, y, buf, false);
}

// ---- flush -------------------------------------------------------------

// The column pointer does not wrap into the next page on this controller, so
// each page carries its own address setup. That is the eight-transfers-per-
// frame cost, and the reason a partial update is worth tracking.
static uint32_t frame_bytes;      // payload of the flush in progress

static void flush_page(int page, int lo, int hi)
{
    int col = lo + SH1106_COL_OFFSET;
    if (!cmd((uint8_t)(0xB0u | (unsigned)page)))        return;  // page
    if (!cmd((uint8_t)(0x00u | (col & 0x0F))))          return;  // column low
    if (!cmd((uint8_t)(0x10u | ((col >> 4) & 0x0F))))   return;  // column high

    uint8_t tx[1 + SH1106_WIDTH];
    int n = hi - lo + 1;
    frame_bytes += (uint32_t)n;
    tx[0] = CTRL_DATA;
    memcpy(&tx[1], &fb[page][lo], (size_t)n);
    display_core1_mark(200u + (uint32_t)page);   // inside the I2C data write
    const int wrote = i2c_write_timeout_us(OLED_I2C_PORT, addr, tx,
                                           (size_t)n + 1, false, I2C_TIMEOUT_US);
    display_core1_mark(210u + (uint32_t)page);   // it returned
    if (wrote != n + 1) {
        err_count++;
        return;                 // shadow not advanced, so the next frame retries
    }

    // **Only the span actually sent.** Copying the whole page would claim the
    // panel had been shown bytes outside the window, and the next frame would
    // then skip pushing them. The three early returns above leave the shadow
    // alone for the same reason: a page whose address setup failed was told
    // nothing at all.
    memcpy(&shadow[page][lo], &fb[page][lo], (size_t)n);
}

void sh1106_flush(void)
{
    if (!present) return;

    const uint32_t t0 = time_us_32();
    frame_bytes = 0;

    // One span per page, from the first byte that differs to the last. A page
    // is 128 bytes and there are eight of them, so the whole scan is 1 KB of
    // compares - a few microseconds against a flush measured in tens of
    // milliseconds, and it is skipped entirely for a page that did not change.
    for (int p = 0; p < SH1106_PAGES; p++) {
        int lo = 0;
        while (lo < SH1106_WIDTH && fb[p][lo] == shadow[p][lo]) lo++;
        if (lo == SH1106_WIDTH) continue;             // page unchanged

        int hi = SH1106_WIDTH - 1;
        while (fb[p][hi] == shadow[p][hi]) hi--;      // lo < hi+1 guarantees this stops

        flush_page(p, lo, hi);
    }

    // Counted whether or not anything moved: a frame where nothing changed is
    // still a frame the pacing produced, and leaving it out would make an idle
    // menu read as 0 fps.
    const uint32_t dt = time_us_32() - t0;
    if (dt > stat_longest_us)     stat_longest_us = dt;
    // Summed, not worst: the mean over the window is what answers "what does a
    // frame cost", and the worst would be carried by the one frame a second in
    // which the counter's own text changes.
    stat_bytes += frame_bytes;
    ++stat_frames;
}

void sh1106_stats_take(uint32_t *frames, uint32_t *longest_us, uint32_t *bytes)
{
    if (frames)     *frames     = stat_frames;
    if (longest_us) *longest_us = stat_longest_us;
    if (bytes)      *bytes      = stat_bytes;
    stat_frames     = 0;
    stat_longest_us = 0;
    stat_bytes      = 0;
}

void sh1106_flush_all(void)
{
    if (!present) return;
    // Unconditional, and that is what it is for: at init the panel's own RAM
    // holds whatever it powered up with, so there is nothing to diff against.
    // flush_page() syncs the shadow, so an ordinary flush right afterwards
    // correctly sends nothing.
    for (int p = 0; p < SH1106_PAGES; p++)
        flush_page(p, 0, SH1106_WIDTH - 1);
}

void sh1106_fill(bool on)
{
    memset(fb, on ? 0xFF : 0x00, sizeof(fb));
}

bool sh1106_command(uint8_t c)              { return present && cmd(c); }
bool sh1106_command2(uint8_t c, uint8_t a)  { return present && cmd2(c, a); }
uint32_t sh1106_error_count(void)           { return err_count; }

void sh1106_contrast(uint8_t level) { if (present) cmd2(0x81u, level); }
void sh1106_display_on(bool on)     { if (present) cmd(on ? 0xAFu : 0xAEu); }
void sh1106_invert(bool inverted)   { if (present) cmd(inverted ? 0xA7u : 0xA6u); }
