#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "module.h"
#include "settings.h"
#include "term.h"

// ---- geometry ----------------------------------------------------------
//
// 80x25, laid out as a title bar, a framed panel and two lines of chrome at
// the bottom. The panel is 60 wide rather than the full 80 because a list row
// whose label is at column 1 and whose value is at column 78 is two separate
// things to look at; at 60 the eye takes the pair in at once, which is the
// same reason the OLED's list stops short of its scrollbar.

#define TITLE_Y     0
#define BOX_X       10
#define BOX_Y       2
#define BOX_W       60
#define BOX_H       19                      // rows 2..20 inclusive
#define ROW0_Y      (BOX_Y + 1)             // 3
#define ROWS        (BOX_H - 2)             // 17 list rows
#define LABEL_X     (BOX_X + 3)
#define VALUE_R     (BOX_X + BOX_W - 4)     // right-most column of a value
#define SCROLL_X    (BOX_X + BOX_W - 2)
#define HINT_Y      22
#define STATUS_Y    (TERM_H - 1)            // 24

// ---- the palette --------------------------------------------------------
//
// Blue desktop, light grey chrome, and the cursor as a light grey bar with
// blue text on it. That is the IBM/AMI setup screen, and it is not nostalgia:
// on a link where a cursor cannot be a shape or an animation, an inverted row
// is the only thing that reads as one at a glance, and blue on light grey has
// the contrast to survive whatever palette the terminal is actually using.

#define ATTR_DESK      TERM_ATTR(TC_LIGHTGRAY,   TC_BLUE)
#define ATTR_TITLE     TERM_ATTR(TC_BLACK,       TC_LIGHTGRAY)
#define ATTR_TITLE_HI  TERM_ATTR(TC_BLUE,        TC_LIGHTGRAY)
#define ATTR_FRAME     TERM_ATTR(TC_WHITE,       TC_BLUE)
#define ATTR_ROW       TERM_ATTR(TC_LIGHTGRAY,   TC_BLUE)
#define ATTR_ROW_SEL   TERM_ATTR(TC_BLUE,        TC_LIGHTGRAY)
#define ATTR_VALUE     TERM_ATTR(TC_LIGHTCYAN,   TC_BLUE)
#define ATTR_EDIT      TERM_ATTR(TC_BLACK,       TC_CYAN)
#define ATTR_MARK      TERM_ATTR(TC_YELLOW,      TC_BLUE)
#define ATTR_MARK_SEL  TERM_ATTR(TC_RED,         TC_LIGHTGRAY)
#define ATTR_HINT      TERM_ATTR(TC_LIGHTCYAN,   TC_BLUE)
#define ATTR_STATUS    TERM_ATTR(TC_BLACK,       TC_LIGHTGRAY)
#define ATTR_DIM       TERM_ATTR(TC_DARKGRAY,    TC_BLUE)
#define ATTR_HEAD      TERM_ATTR(TC_WHITE,       TC_BLUE)
#define ATTR_OK        TERM_ATTR(TC_LIGHTGREEN,  TC_BLUE)
#define ATTR_WARN      TERM_ATTR(TC_YELLOW,      TC_BLUE)
#define ATTR_BAD       TERM_ATTR(TC_LIGHTRED,    TC_BLUE)

typedef enum { PAGE_EFFECTS, PAGE_PARAMS, PAGE_CHAIN, PAGE_STATUS,
               PAGE_ABOUT } page_t;

static page_t page;
static int    cur_effect;      // cursor in the effect list
static int    cur_item;        // 0 = Back, 1.. = parameter index + 1
static int    scroll;
static bool   editing;
static int    edit_value;      // authoritative while editing; see draw_params

// The same acknowledgement flashes the panel has, and for the same reason:
// Reset changes values that are above the row pressed, and Save changes
// nothing you can see at all.
#define FLASH_MS      900u
static uint32_t reset_done_ms;
static uint32_t saved_done_ms;
static uint32_t chain_full_ms;
static int      chain_full_item;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static bool flashing(uint32_t at)
{
    return at != 0u && (now_ms() - at) < FLASH_MS;
}

// ---- chrome -------------------------------------------------------------

static void draw_title(const char *s)
{
    term_fill(0, TITLE_Y, TERM_W, 1, ' ', ATTR_TITLE);
    term_puts(1, TITLE_Y, "NovaDSP", ATTR_TITLE_HI);

    const int x = (TERM_W - (int)strlen(s)) / 2;
    term_puts(x < 0 ? 0 : x, TITLE_Y, s, ATTR_TITLE);
}

static void draw_hint(const char *s)
{
    const int x = (TERM_W - (int)strlen(s)) / 2;
    term_puts(x < 0 ? 0 : x, HINT_Y, s, ATTR_HINT);
}

// ---- SRAM, as the linker left it ---------------------------------------
//
// Both ends come from the link rather than from a constant here, so the figure
// cannot go stale: __bss_end__ is the end of the image under copy_to_ram, and
// __HeapLimit is the top of main SRAM. The two 4 KB core stacks live above it
// in the scratch banks and are not counted.
extern char __bss_end__[];
extern char __HeapLimit[];

static uint32_t ram_used_kb(void)
{
    return (uint32_t)(((uintptr_t)__bss_end__ - SRAM_BASE) / 1024u);
}

static uint32_t ram_total_kb(void)
{
    return (uint32_t)(((uintptr_t)__HeapLimit - SRAM_BASE) / 1024u);
}

// The bottom line, and it is on every page rather than only on DSP STATUS.
// The panel could not afford that - six 9 px rows is the whole screen - and a
// terminal can, so the numbers you check while doing something else are always
// in view, and DSP STATUS becomes the page you go to for the meters rather
// than for the overrun count.
static void draw_status_bar(const display_status_t *s)
{
    term_fill(0, STATUS_Y, TERM_W, 1, ' ', ATTR_STATUS);

    const effect_t *e = effect_at(effect_active());

    term_printf(1, STATUS_Y, ATTR_STATUS,
                "%-13.13s  %5lu Hz  ovr %-4lu  dsp %5.1f%%  ram %lu/%luk",
                e ? e->name : "-",
                (unsigned long)s->rate_hz,
                (unsigned long)s->overruns,
                (double)s->dsp_pct,
                (unsigned long)ram_used_kb(),
                (unsigned long)ram_total_kb());

    // Whether a power cycle would lose anything, on every page. The Save row
    // says the same thing; this is the half of it you can see without going
    // and looking.
    const char *tail = settings_is_dirty() ? "UNSAVED" : "saved";
    term_puts(TERM_W - 1 - (int)strlen(tail), STATUS_Y, tail, ATTR_STATUS);
}

// ---- list rows ----------------------------------------------------------

// One row of the framed panel. row_sel highlights the whole interior width -
// the navigation cursor - and value_edit highlights only the value cell, which
// is the edit cursor. Two different highlights is what tells "this row is
// selected" and "this value is live" apart without spending a line on a mode
// indicator, and it is the panel's rule kept rather than reinvented.
static void draw_row(int slot, const char *left, const char *right,
                     bool row_sel, bool value_edit)
{
    if (slot < 0 || slot >= ROWS) return;

    const int     y = ROW0_Y + slot;
    const uint8_t a = row_sel ? ATTR_ROW_SEL : ATTR_ROW;

    term_fill(BOX_X + 1, y, BOX_W - 2, 1, ' ', a);
    term_puts(LABEL_X, y, left, a);

    if (!right || !*right) return;

    const int w = (int)strlen(right);
    const int x = VALUE_R - w + 1;

    if (value_edit)   term_puts(x, y, right, ATTR_EDIT);
    else if (row_sel) term_puts(x, y, right, a);
    else              term_puts(x, y, right, ATTR_VALUE);
}

// Two arrows rather than a proportional thumb. A thumb needs a column of its
// own and a terminal cell is a whole character wide, so it would cost more of
// the panel than it tells anyone - and with seventeen rows visible, a list
// only scrolls in a build with an unusual number of effects in it.
static void draw_scroll_marks(int first, int total)
{
    if (total <= ROWS) return;
    if (first > 0)
        term_putc(SCROLL_X, ROW0_Y, TG_UP, ATTR_FRAME);
    if (first + ROWS < total)
        term_putc(SCROLL_X, ROW0_Y + ROWS - 1, TG_DOWN, ATTR_FRAME);
}

static void ensure_visible(int cur, int total)
{
    if (cur < scroll)          scroll = cur;
    if (cur >= scroll + ROWS)  scroll = cur - ROWS + 1;
    if (scroll > total - ROWS) scroll = total - ROWS;
    if (scroll < 0)            scroll = 0;
}

// ---- the effect list ----------------------------------------------------

#define LIST_ROWS()       (effect_count() + 3)
#define ROW_IS_SAVE(i)    ((i) == effect_count())
#define ROW_IS_STATUS(i)  ((i) == effect_count() + 1)
#define ROW_IS_ABOUT(i)   ((i) == effect_count() + 2)
#define ROW_IS_EFFECT(i)  ((i) <  effect_count())

static void draw_effects(void)
{
    draw_title("EFFECTS");

    const int n = LIST_ROWS();
    for (int i = 0; i < ROWS && scroll + i < n; ++i) {
        const int  idx = scroll + i;
        const bool sel = (idx == cur_effect);

        if (ROW_IS_SAVE(idx)) {
            // Three states, and the row is the only feedback there is: what a
            // press did, or - the useful one - whether pressing would do
            // anything at all. A save you have to remember is a save you will
            // forget, which is why taking the automatic write away was only
            // safe once this row existed.
            const char *val = flashing(saved_done_ms) ? "saved"
                            : (settings_is_dirty() ? "unsaved" : "ok");
            draw_row(i, "Save settings", val, sel, false);
            continue;
        }
        if (ROW_IS_STATUS(idx)) {
            draw_row(i, "DSP Status", NULL, sel, false);
            continue;
        }
        if (ROW_IS_ABOUT(idx)) {
            draw_row(i, "About", NULL, sel, false);
            continue;
        }

        const effect_t *e = effect_at(idx);
        draw_row(i, e ? e->name : "-", NULL, sel, false);

        // A filled square marks the effect actually processing. Its colour has
        // to follow the row for the same reason the panel's polarity does: on
        // the selected row the background is light grey, and yellow on light
        // grey is not a marker.
        if (idx == effect_active())
            term_putc(LABEL_X - 2, ROW0_Y + i, TG_MARK,
                      sel ? ATTR_MARK_SEL : ATTR_MARK);
    }
    draw_scroll_marks(scroll, n);
    draw_hint("Up/Down move   Enter select, or open if live   S save   R redraw");
}

// ---- parameter pages ----------------------------------------------------

#define PARAM_ROWS(e)        ((e)->param_count + 2)
#define ITEM_IS_RESET(e, i)  ((i) == (e)->param_count + 1)

static void draw_params(void)
{
    const effect_t *e = effect_at(cur_effect);
    if (!e) return;

    draw_title(e->name);

    const int n = PARAM_ROWS(e);
    for (int i = 0; i < ROWS && scroll + i < n; ++i) {
        const int item = scroll + i;

        if (item == 0) {
            draw_row(i, "< Back", NULL, cur_item == 0, false);
            continue;
        }
        if (ITEM_IS_RESET(e, item)) {
            draw_row(i, "Reset to defaults",
                     flashing(reset_done_ms) ? "done" : NULL,
                     cur_item == item, false);
            continue;
        }

        const int  p  = item - 1;
        const bool on = (cur_item == item);

        // While editing, the pending value is the truth for display: the
        // shadow only catches up a block later, and reading it back would make
        // a fast key repeat look like it was dropping counts.
        const int v = (on && editing) ? edit_value
                                      : effect_value(cur_effect, p);

        char val[24];
        effect_format_value(cur_effect, p, v, val, sizeof val);
        draw_row(i, e->params[p].name, val, on && !editing, on && editing);
    }
    draw_scroll_marks(scroll, n);

    draw_hint(editing ? "Up/Down change value   Enter done"
                      : "Up/Down move   Enter edit   Esc back");
}

// ---- the chain editor ---------------------------------------------------
//
// What this page knows about the effect it is editing is **nothing, by name**.
// It got here because the effect under the cursor declared EFFECT_F_COMPOSITE;
// the slot count is param_count, the candidate count is params[0].max, the
// names are params[0].labels[c + 1], and what a slot resolves to is the
// optional slot_effect() hook. So it works in a build with no FX Chain in it,
// and would work for a second composite nobody has written yet.
//
// The mirror below is the same trick draw_params() uses for edit_value: the
// shadow lags by a block and one press can move every slot, so reading it back
// between two quick presses would compute the second against stale state.
static int chain_sel[EFFECT_MAX_PARAMS];   // 0 = empty, else candidate + 1

static int chain_index(void) { return cur_effect; }

static const effect_t *chain_effect(void) { return effect_at(chain_index()); }

static int chain_slots(void)
{
    const effect_t *e = chain_effect();
    if (!e) return 0;
    return e->param_count < EFFECT_MAX_PARAMS ? e->param_count
                                              : EFFECT_MAX_PARAMS;
}

static int chain_candidates(void)
{
    const effect_t *e = chain_effect();
    return (e && e->param_count > 0) ? e->params[0].max : 0;
}

static const char *chain_candidate_name(int c)
{
    const effect_t *e = chain_effect();
    if (!e || e->param_count <= 0 || !e->params[0].labels) return NULL;
    if (c < 0 || c >= chain_candidates()) return NULL;
    return e->params[0].labels[c + 1];
}

static int chain_pos(int c)
{
    for (int s = 0; s < chain_slots(); ++s)
        if (chain_sel[s] == c + 1) return s;
    return -1;
}

static void chain_sync(void)
{
    const int ci = chain_index();
    memset(chain_sel, 0, sizeof chain_sel);
    for (int s = 0; s < chain_slots(); ++s)
        chain_sel[s] = (ci >= 0) ? effect_value(ci, s) : 0;
}

static void chain_push(void)
{
    const int ci = chain_index();
    if (ci < 0) return;
    for (int s = 0; s < chain_slots(); ++s)
        effect_request_set(ci, s, chain_sel[s]);
}

static void chain_toggle(int c)
{
    if (c < 0) return;
    chain_full_ms = 0;              // any press that lands clears a stale "full"

    const int slots = chain_slots();
    const int pos   = chain_pos(c);

    if (pos >= 0) {
        // Out, and close the gap. An order that read 1, 3, 4 would be asking
        // the user to work out that 2 left.
        for (int s = pos; s < slots - 1; ++s)
            chain_sel[s] = chain_sel[s + 1];
        chain_sel[slots - 1] = 0;
    } else {
        int s = 0;
        while (s < slots && chain_sel[s] != 0) ++s;
        if (s == slots) {
            chain_full_ms   = now_ms();
            chain_full_item = cur_item;
            return;
        }
        chain_sel[s] = c + 1;
    }
    chain_push();
}

#define CHAIN_ROWS()        (chain_candidates() + 2)
#define CHAIN_IS_RESET(i)   ((i) == chain_candidates() + 1)

static void draw_chain(void)
{
    const effect_t *e = chain_effect();
    draw_title(e ? e->name : "CHAIN");

    const int n = CHAIN_ROWS();
    for (int i = 0; i < ROWS && scroll + i < n; ++i) {
        const int  item = scroll + i;
        const bool on   = (cur_item == item);

        if (item == 0) {
            draw_row(i, "< Back", NULL, on, false);
            continue;
        }
        if (CHAIN_IS_RESET(item)) {
            draw_row(i, "Reset - empty the chain",
                     flashing(reset_done_ms) ? "done" : NULL, on, false);
            continue;
        }

        const int   c     = item - 1;
        const char *cname = chain_candidate_name(c);
        if (!cname) continue;

        const int pos = chain_pos(c);
        char num[8] = { 0 };
        if (pos >= 0) {
            snprintf(num, sizeof num, "%d", pos + 1);
        } else if (item == chain_full_item && flashing(chain_full_ms)) {
            // A press that cannot be honoured has to say so. Doing nothing
            // silently is the wrong feedback anywhere; on a terminal it reads
            // as a dropped keystroke.
            snprintf(num, sizeof num, "full");
        }

        draw_row(i, cname, num[0] ? num : NULL, on, false);
    }
    draw_scroll_marks(scroll, n);
    draw_hint("Up/Down move   Enter add to / remove from the chain   Esc back");
}

// ---- DSP STATUS ---------------------------------------------------------
//
// The panel fits four 50 px bars two to a row and has to print their values on
// a separate line. Eighty columns fits a labelled 40-cell bar and its reading
// on one line, four times over, with room left under it for the load and the
// SRAM - so this page is the same information laid out the way it wanted to be.

#define METER_W               40
#define METER_DECAY_DB_PER_S  20.0f
#define METER_TEXT_MS         250u

static float    meter_db[4]   = { -120.0f, -120.0f, -120.0f, -120.0f };
static float    meter_txt[4]  = { -60.0f, -60.0f, -60.0f, -60.0f };
static float    meter_peak[4] = { -120.0f, -120.0f, -120.0f, -120.0f };
static uint32_t meter_last_ms;
static uint32_t meter_txt_ms;

// -60 dBFS to 0 dBFS across the bar, the same window the panel uses.
static float db_frac(float db)
{
    const float f = (db + 60.0f) / 60.0f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// The bar is coloured by where each cell sits on the scale rather than by the
// level as a whole, so the green-to-red transition stays put and the eye reads
// position against a fixed landmark. A bar that changed colour bodily would
// make the same level look like two different things.
static void draw_meter(int x, int y, float db)
{
    const int fill = (int)((float)METER_W * db_frac(db) + 0.5f);

    term_putc(x - 1, y, '[', ATTR_DIM);
    term_putc(x + METER_W, y, ']', ATTR_DIM);

    for (int i = 0; i < METER_W; ++i) {
        const float at = (float)i / (float)METER_W;     // 0 = -60, 1 = 0 dBFS

        uint8_t a = ATTR_OK;
        if (at >= 0.95f)      a = ATTR_BAD;             // above -3 dBFS
        else if (at >= 0.80f) a = ATTR_WARN;            // above -12 dBFS

        if (i < fill) term_putc(x + i, y, TG_BLOCK, a);
        else          term_putc(x + i, y, TG_HALF,  ATTR_DIM);
    }
}

static void draw_status(const display_status_t *s)
{
    draw_title("DSP STATUS");

    const effect_t *e = effect_at(effect_active());

    int y = ROW0_Y;
    term_printf(BOX_X + 2, y, ATTR_HEAD, "Effect   %s", e ? e->name : "-");
    term_printf(VALUE_R - 12, y, s->overruns ? ATTR_BAD : ATTR_OK,
                "overruns %lu", (unsigned long)s->overruns);
    y += 2;

    // in L, in R, out L, out R - the order the meters are indexed in below.
    const float db[4] = { s->in_l, s->in_r, s->out_l, s->out_r };

    // Decay against elapsed time rather than per call: this is also reached
    // from key events, which arrive at no fixed rate.
    const uint32_t now = now_ms();
    uint32_t dt = now - meter_last_ms;
    if (dt > 500u) dt = 500u;              // first draw, or back from a menu
    meter_last_ms = now;

    const float fall = METER_DECAY_DB_PER_S * (float)dt / 1000.0f;
    for (int i = 0; i < 4; ++i) {
        if (db[i] >= meter_db[i]) meter_db[i] = db[i];   // instant attack
        else                      meter_db[i] -= fall;
        if (meter_db[i] < -60.0f) meter_db[i] = -60.0f;

        if (db[i] > meter_peak[i]) meter_peak[i] = db[i];
    }

    // **The printed reading is not the bar's value, and that is deliberate.**
    // The bar attacks instantly and falls at 20 dB/s, which is what makes it
    // read as a level and what would make a tenths digit taken from it
    // unreadable. The text carries its own reading: the peak over 250 ms,
    // published at the end of each window. Before folding the two into one:
    // the ballistics that make a good bar make a bad number.
    if ((uint32_t)(now - meter_txt_ms) >= METER_TEXT_MS) {
        for (int i = 0; i < 4; ++i) {
            meter_txt[i]  = (meter_peak[i] < -60.0f) ? -60.0f : meter_peak[i];
            meter_peak[i] = -120.0f;
        }
        meter_txt_ms = now;
    }

    static const char *const kName[4] = { "in  L", "in  R", "out L", "out R" };
    for (int i = 0; i < 4; ++i) {
        const int row = y + i + (i >= 2 ? 1 : 0);   // a gap between in and out

        term_puts(BOX_X + 2, row, kName[i], ATTR_ROW);
        draw_meter(BOX_X + 9, row, meter_db[i]);

        // "%6.1f" rather than "%.1f": the field is fixed, so the decimal
        // points of a two-digit and a one-digit reading sit in the same column
        // and the four can be compared at a glance. That comparison - left
        // against right, in against out - is what the page is for. And read
        // out minus in, never out on its own: a leveller clamped against
        // MaxGain shows the same out a correctly levelled one does.
        term_printf(BOX_X + 51, row, ATTR_VALUE, "%6.1f", (double)meter_txt[i]);
    }
    y += 6;

    term_puts(BOX_X + 2, y, "resources", ATTR_DIM);
    y += 1;
    term_printf(BOX_X + 2, y, ATTR_ROW,
                "DSP load    %5.1f / %5.1f us   %4.1f %% of 1333 us",
                (double)s->dsp_avg_us, (double)s->dsp_max_us,
                (double)s->dsp_pct);
    y += 1;
    term_printf(BOX_X + 2, y, ATTR_ROW, "SRAM        %lu / %lu kB",
                (unsigned long)ram_used_kb(), (unsigned long)ram_total_kb());
    y += 1;
    term_printf(BOX_X + 2, y, ATTR_ROW, "Sample rate %lu Hz, measured",
                (unsigned long)s->rate_hz);

    draw_hint("Enter or Esc  back to the effect list");
}

// ---- about --------------------------------------------------------------

static void draw_centred(int y, const char *s, uint8_t attr)
{
    const int x = (TERM_W - (int)strlen(s)) / 2;
    term_puts(x < 0 ? 0 : x, y, s, attr);
}

static void draw_about(void)
{
    draw_title("ABOUT");

    draw_centred(ROW0_Y + 3,  "N o v a D S P", ATTR_HEAD);
    draw_centred(ROW0_Y + 5,  "48 kHz / 24-bit audio DSP", ATTR_ROW);
    draw_centred(ROW0_Y + 6,  "Raspberry Pi Pico 2 - RP2350 at 307.2 MHz",
                 ATTR_ROW);
    draw_centred(ROW0_Y + 9,  "Designed by R.Hasaranga", ATTR_ROW);
    draw_centred(ROW0_Y + 10, "www.hasaranga.com", ATTR_VALUE);
    draw_centred(ROW0_Y + 13, "serial display module", ATTR_DIM);

    draw_hint("Enter or Esc  back to the effect list");
}

// ---- input --------------------------------------------------------------

static void open_params(void)
{
    cur_item      = 0;
    scroll        = 0;
    editing       = false;
    reset_done_ms = 0;
    chain_full_ms = 0;

    // A composite effect's parameters are slots, and a list of slots is the
    // wrong way to look at a chain: "Slot 2  Equalizer" answers neither of the
    // two questions you have, which are what is in it and in what order. The
    // test is the flag rather than the effect's identity, so a build with no
    // composite in it never reaches this page and one with two gets the editor
    // for both.
    const effect_t *e = effect_at(cur_effect);
    if (e && (e->flags & EFFECT_F_COMPOSITE) && e->param_count > 0) {
        chain_sync();
        page = PAGE_CHAIN;
        return;
    }
    page = PAGE_PARAMS;
}

// Leaving either editor writes nothing. A write is something the user asks for
// on the Save row, not something that happens because they navigated - and the
// status bar carries the unsaved marker, which is the part that makes having
// taken the automatic write away safe.
static void leave_to_list(void)
{
    page          = PAGE_EFFECTS;
    scroll        = 0;
    editing       = false;
    reset_done_ms = 0;
    chain_full_ms = 0;
    ensure_visible(cur_effect, LIST_ROWS());
}

static void handle_effects(const keys_state_t *k)
{
    const int n = LIST_ROWS();

    // Moving the cursor has no side effect at all. That is worth keeping even
    // though a keyboard has no equivalent of brushing a knob: selecting as the
    // cursor passed was a real bug on the panel - walking down to DSP Status
    // left whatever was in the way running, and the next save wrote it.
    if (k->delta) {
        cur_effect = clampi(cur_effect + k->delta, 0, n - 1);
        ensure_visible(cur_effect, n);
    }

    if (!k->press_edge) return;

    if (ROW_IS_SAVE(cur_effect)) {
        // The only thing in the firmware that writes flash on purpose. It
        // declines when nothing differs, so a second press costs nothing.
        settings_request_save();
        saved_done_ms = now_ms();
        return;
    }
    if (!ROW_IS_EFFECT(cur_effect)) {
        page = ROW_IS_STATUS(cur_effect) ? PAGE_STATUS : PAGE_ABOUT;
        return;
    }
    if (cur_effect != effect_active()) {
        // First press selects. The marker moving is the acknowledgement, and
        // the change is audible immediately. Two presses to reach the
        // parameters of an effect that was not live is right: the first is a
        // decision about what you are listening to.
        effect_request_select(cur_effect);
        return;
    }
    open_params();
}

static void handle_params(const keys_state_t *k)
{
    const effect_t *e = effect_at(cur_effect);
    if (!e) return;

    const int n = PARAM_ROWS(e);

    if (editing) {
        const int p = cur_item - 1;
        const effect_param_t *d = &e->params[p];

        if (k->delta) {
            edit_value = clampi(edit_value + k->delta, d->min, d->max);
            effect_request_set(cur_effect, p, edit_value);
        }
        // Esc leaves the edit rather than the page, which is the one place the
        // back key is not simply "< Back": the value is already applied, so
        // there is nothing to cancel and stepping out one level at a time is
        // what the key means everywhere else.
        if (k->press_edge || k->back_edge) editing = false;
        return;
    }

    if (k->back_edge) { leave_to_list(); return; }

    if (k->delta) {
        cur_item = clampi(cur_item + k->delta, 0, n - 1);
        ensure_visible(cur_item, n);
    }
    if (!k->press_edge) return;

    if (cur_item == 0) {
        leave_to_list();
    } else if (ITEM_IS_RESET(e, cur_item)) {
        // No confirmation. It restores values the user can see and turn back,
        // on a page they had to press twice to reach - and nothing reaches
        // flash until the Save row is pressed either. The row says "done"
        // instead, because everything Reset changed is above it.
        effect_request_reset(cur_effect);
        reset_done_ms = now_ms();
    } else {
        editing    = true;
        edit_value = effect_value(cur_effect, cur_item - 1);
    }
}

static void handle_chain(const keys_state_t *k)
{
    const int n = CHAIN_ROWS();

    if (k->back_edge) { leave_to_list(); return; }

    if (k->delta) {
        cur_item = clampi(cur_item + k->delta, 0, n - 1);
        ensure_visible(cur_item, n);
    }
    if (!k->press_edge) return;

    if (cur_item == 0) { leave_to_list(); return; }

    if (CHAIN_IS_RESET(cur_item)) {
        // Clears the mirror directly rather than going through
        // effect_request_reset(): the result is identical, and this keeps the
        // mirror and the effect in step where a request landing a block later
        // would not.
        memset(chain_sel, 0, sizeof chain_sel);
        chain_push();
        reset_done_ms = now_ms();
        chain_full_ms = 0;
        return;
    }

    chain_toggle(cur_item - 1);
}

// ---- entry points -------------------------------------------------------

void serial_ui_init(void)
{
    page          = PAGE_EFFECTS;
    cur_effect    = effect_active();
    cur_item      = 0;
    scroll        = 0;
    editing       = false;
    reset_done_ms = 0;
    saved_done_ms = 0;
    chain_full_ms = 0;
    meter_last_ms = now_ms();
    meter_txt_ms  = meter_last_ms;

    ensure_visible(cur_effect, LIST_ROWS());
}

void serial_ui_tick(const keys_state_t *k, const display_status_t *status)
{
    // 's' saves from anywhere. The Save row is still there and is still the
    // thing that tells you whether saving would do anything; this is the
    // shortcut for when you already know it would.
    if (k->save_edge) {
        settings_request_save();
        saved_done_ms = now_ms();
    }

    // 'r' repaints a terminal something else has scribbled on. There is no
    // equivalent on the panel because nothing else can write to it.
    if (k->redraw_edge) term_invalidate();

    switch (page) {
    case PAGE_STATUS:
    case PAGE_ABOUT:
        // These were asked for, so they leave on a press and ignore movement -
        // a page the user deliberately opened should not close because they
        // pressed an arrow.
        if (k->press_edge || k->back_edge) {
            page = PAGE_EFFECTS;
            ensure_visible(cur_effect, LIST_ROWS());
        }
        break;

    case PAGE_EFFECTS: handle_effects(k); break;
    case PAGE_CHAIN:   handle_chain(k);   break;
    default:           handle_params(k);  break;
    }

    term_clear(ATTR_DESK);
    term_box(BOX_X, BOX_Y, BOX_W, BOX_H, ATTR_FRAME);

    switch (page) {
    case PAGE_EFFECTS: draw_effects();      break;
    case PAGE_PARAMS:  draw_params();       break;
    case PAGE_CHAIN:   draw_chain();        break;
    case PAGE_STATUS:  draw_status(status); break;
    default:           draw_about();        break;
    }

    draw_status_bar(status);
}
