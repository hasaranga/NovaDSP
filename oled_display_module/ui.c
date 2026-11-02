#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "module.h"
#include "sh1106.h"
#include "settings.h"

// ---- what the idle screen is -------------------------------------------
//
// **At 0 the idle screen is the spectrum analyser and nothing else.** At 1 the
// stored animations join it and the two take turns every VIS_ROTATE_MS with a
// slide between them, which is what this module shipped with and what
// oled_vis.md documents.
//
// It is off because the spectrum now carries the output meters underneath it
// (see *the spectrum* below), which makes it the page worth leaving up rather
// than one of several taking turns.
//
// **Measured, dev, this tree: 92,416 bytes of text and 1,040 of bss.** The
// frames themselves were always __in_flash and cost no SRAM - what off
// reclaims is the 91,136 bytes of them that --gc-sections can now drop,
// because nothing references oled_anim6.c any more, plus the transition's own
// 1 KB snapshot of the outgoing frame, which was never in flash and is the
// only part of this that was ever SRAM. The file stays in the module's
// SOURCES because this is a switch and not a deletion.
#ifndef ENABLE_VISUALIZATION
#define ENABLE_VISUALIZATION  0
#endif

#if ENABLE_VISUALIZATION
#include "oled_anim6.h"
#endif

// ---- geometry ----------------------------------------------------------
// 64 rows: a 10 px title bar, then six 9 px list rows starting at 12. The last
// row runs to 64, which clips one blank pixel off the bottom of a 7 px glyph
// and nothing else.

#define TITLE_H     10
#define BODY_Y      12
#define ROW_H       9

// ---- the panel's own frame rate ----------------------------------------
//
// **A measurement build, not a feature.** Next steps item 14 suspects this
// module of both faults the ST7789 produced - a clear-then-redraw that dirties
// every column holding ink, so a static page re-sends its whole 1 KB, and a
// redraw timer scheduled from the end of the frame rather than from the
// deadline. Both were invisible to every build over there until the module was
// asked to report its own rate, and both are still arguments here.
//
// All six rows of DSP STATUS are spent, so this **replaces** the dsp/ram row
// rather than joining it. That is why it is a switch: the page is worth more
// than the counter once the question is settled. `dsp` is on the console in a
// dev build meanwhile, and `ram` is a constant per build.
//
// Reads `gfx <frames>f <worst>ms <worst>b` over a one-second window:
//
//   frames  what the pacing achieved. 25 is what module.c asks for; 16 is the
//           timer scheduled from the wrong end, which is what the ST7789 read
//   worst   the longest single flush. ~23 ms is a full-panel refresh
//   b       **mean payload per frame.** 0 is a repaint that changed nothing
//           and sent nothing, which is what sh1106_clear()'s comment claims
//           happens. 1024 is every column of every page - the clear-then-
//           redraw fault stated exactly. This is the number the ST7789 did not
//           have, and the reason its two bugs were able to cover for each
//           other there
// **Off, because the question it was added for is answered.** It read
// `16f 23ms 835b` on a static page, both faults in one line; with the timer
// re-based from the deadline and the dirty span replaced by a shadow it reads
// 25 fps. Turn it back on - here, or with -DUI_SHOW_GFX=1 - before believing
// anything about this panel's frame cost again, because the counter is the
// only thing that has ever been right about it.
#ifndef UI_SHOW_GFX
#define UI_SHOW_GFX  0
#endif

#if UI_SHOW_GFX
#define GFX_WINDOW_MS   1000u
static uint32_t gfx_window_ms, gfx_frames, gfx_worst_us, gfx_bytes;
#endif
#define ROWS        6
#define LIST_W      123          // leaves the right edge for the scrollbar

// Meter bars: thin, and sitting on the middle of the row so they line up with
// the x-height of the label beside them rather than towering over it.
#define BAR_H       3
#define BAR_INSET   2            // from the row's text baseline
#define IDLE_MS     10000u

// The idle screen is several screens taking turns - the spectrum, then each
// animation in kAnims, then round again. Twenty seconds each: long enough that
// the spectrum is still a usable meter if you glance at the box, short enough
// that none of them outstays its welcome on something left running. The ST7789
// rotates its six animations on the same principle at 15 s.
//
// The rotation is *not* the idle timeout and does not restart it. Any input
// leaves the idle screen entirely, whichever part of it was showing.
#define VIS_ROTATE_MS   20000u

typedef enum { PAGE_EFFECTS, PAGE_PARAMS, PAGE_CHAIN, PAGE_STATUS, PAGE_ABOUT,
               PAGE_SPECTRUM, PAGE_ANIM } page_t;

#define PAGE_IS_IDLE(p) ((p) == PAGE_SPECTRUM || (p) == PAGE_ANIM)

static page_t   page;
static page_t   resume_page;     // what to return to when the idle screen wakes
#if ENABLE_VISUALIZATION
static uint32_t vis_since_ms;    // when the current idle screen came up
static int      vis_anim;        // which animation, while page is PAGE_ANIM
#endif
static int      cur_effect;      // cursor in the effect list
static int      cur_item;        // 0 = Back, 1.. = parameter index + 1
static int      scroll;
static bool     editing;
static int      edit_value;      // authoritative while editing; see below
static uint32_t last_input_ms;

// Reset leaves nothing on screen to acknowledge it: the values it changed are
// above the row that was pressed, and on a long list they are off the top of
// it. So the row says "done" for a moment. Zero means not showing.
#define RESET_FLASH_MS   900u
static uint32_t reset_done_ms;
static uint32_t saved_done_ms;   // Save row's "saved" acknowledgement

// A press that cannot be honoured because the chain is already full needs to
// say so, or it reads as a dropped count on a knob that is known for dropping
// them. Same flash, on the row that was pressed.
static uint32_t chain_full_ms;
static int      chain_full_item;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// ---- drawing helpers ---------------------------------------------------

static void draw_title(const char *s)
{
    sh1106_fill_rect(0, 0, SH1106_WIDTH, TITLE_H, true);

    int x = (SH1106_WIDTH - (int)strlen(s) * SH1106_CHAR_W) / 2;
    if (x < 0) x = 0;
    sh1106_text_inv(x, 1, s);     // inverted glyphs: black on the white bar
}

// One list row. row_inv highlights the whole row (the navigation cursor);
// value_inv highlights only the value cell (the edit cursor). Using two
// different highlights is what makes "this row is selected" and "this value is
// live" tell each other apart without a mode indicator.
static void draw_row(int slot, const char *left, const char *right,
                     bool row_inv, bool value_inv)
{
    const int y = BODY_Y + slot * ROW_H;

    if (row_inv) sh1106_fill_rect(0, y - 1, LIST_W, ROW_H, true);

    if (row_inv) sh1106_text_inv(2, y, left);
    else         sh1106_text(2, y, left);

    if (!right || !*right) return;

    const int w = (int)strlen(right) * SH1106_CHAR_W;
    const int x = LIST_W - 2 - w;

    if (value_inv) {
        sh1106_fill_rect(x - 1, y - 1, w + 2, ROW_H, true);
        sh1106_text_inv(x, y, right);
    } else if (row_inv) {
        sh1106_text_inv(x, y, right);
    } else {
        sh1106_text(x, y, right);
    }
}

static void draw_scrollbar(int first, int total)
{
    if (total <= ROWS) return;

    const int track_y = BODY_Y - 1;
    const int track_h = ROWS * ROW_H;
    const int x       = SH1106_WIDTH - 3;

    sh1106_fill_rect(x + 1, track_y, 1, track_h, true);          // track

    int h = track_h * ROWS / total;
    if (h < 4) h = 4;
    const int y = track_y + (track_h - h) * first / (total - ROWS);
    sh1106_fill_rect(x, y, 3, h, true);                          // thumb
}

// Keep the cursor inside the visible window, and never scroll past the end.
static void ensure_visible(int cur, int total)
{
    if (cur < scroll)            scroll = cur;
    if (cur >= scroll + ROWS)    scroll = cur - ROWS + 1;
    if (scroll > total - ROWS)   scroll = total - ROWS;
    if (scroll < 0)              scroll = 0;
}

// ---- pages -------------------------------------------------------------

// The effect list, plus two rows on the end that are not effects. Putting the
// status page here rather than leaving it reachable only by waiting ten
// seconds means it can be asked for, which is what you want when you are
// checking whether something is overrunning.
//
// With four effects that is six rows exactly, which is what fits without
// scrolling - a fifth effect starts the scrollbar.
// Save sits directly under the effects, ahead of the two informational rows.
// It is the one row here that *does* something to the box rather than showing
// you something, and after editing a parameter the cursor is already in the
// effects block, so it is the nearest non-effect row to reach.
#define LIST_ROWS()       (effect_count() + 3)
#define ROW_IS_SAVE(i)    ((i) == effect_count())
#define ROW_IS_STATUS(i)  ((i) == effect_count() + 1)
#define ROW_IS_ABOUT(i)   ((i) == effect_count() + 2)
#define ROW_IS_EFFECT(i)  ((i) <  effect_count())



// Room for the marker box in front of every row label.
#define MARK_X      2
#define MARK_SIZE   5
#define LIST_INDENT "  "

static void draw_effects(void)
{
    draw_title("Nova DSP");

    const int n = LIST_ROWS();
    for (int i = 0; i < ROWS && scroll + i < n; ++i) {
        const int idx = scroll + i;

        const bool sel = (idx == cur_effect);

        if (ROW_IS_SAVE(idx)) {
            // Three states, and the row is the only feedback there is: what a
            // press did, or - the useful one - whether pressing would do
            // anything at all. Without the unsaved marker the user has no way
            // to know the box is holding changes that a power cycle would eat,
            // which was the whole complaint about saving silently on Back.
            const bool just = saved_done_ms != 0u
                           && (now_ms() - saved_done_ms) < RESET_FLASH_MS;
            const char *val = just ? "saved"
                            : (settings_is_dirty() ? "unsaved" : "ok");
            draw_row(i, LIST_INDENT "Save", val, sel, false);
            continue;
        }

        if (ROW_IS_STATUS(idx)) {
            draw_row(i, LIST_INDENT "DSP Status", NULL, sel, false);
            continue;
        }

        if (ROW_IS_ABOUT(idx)) {
            draw_row(i, LIST_INDENT "About", NULL, sel, false);
            continue;
        }

        char label[24];
        snprintf(label, sizeof label, LIST_INDENT "%s", effect_at(idx)->name);
        draw_row(i, label, NULL, sel, false);

        // A filled square marks the effect actually processing. Drawn rather
        // than spelled with a character - the 5x7 font has nothing that reads
        // as "this one is live". Its polarity follows the row: a selected row
        // is filled white, so its marker has to be black to be visible at all.
        if (idx == effect_active())
            sh1106_fill_rect(MARK_X, BODY_Y + i * ROW_H + 1,
                             MARK_SIZE, MARK_SIZE, !sel);
    }
    draw_scrollbar(scroll, n);
}

// Every parameter page is "< Back", the parameters, then "Reset" - so the two
// things that are not a parameter sit at the two ends and the list between them
// is exactly what the effect declared.
//
// Reset is last rather than first for the same reason Back is first: Back is
// what you reach for constantly and Reset is what you reach for once, after
// turning something too far. Putting it at the top would mean passing over it
// on the way to every parameter, and it is one press with no confirmation.
#define PARAM_ROWS(e)        ((e)->param_count + 2)
#define ITEM_IS_RESET(e, i)  ((i) == (e)->param_count + 1)

// True while the "done" acknowledgement is still up.
static bool reset_flashing(void)
{
    return reset_done_ms != 0u && (now_ms() - reset_done_ms) < RESET_FLASH_MS;
}

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
            draw_row(i, "Reset", reset_flashing() ? "done" : NULL,
                     cur_item == item, false);
            continue;
        }

        const int p   = item - 1;
        const bool on = (cur_item == item);

        // While editing, the pending value is the truth for display: the
        // shadow only catches up a block later, and reading it back would make
        // a fast turn look like it was dropping counts.
        const int v = (on && editing) ? edit_value
                                      : effect_value(cur_effect, p);

        char val[16];
        effect_format_value(cur_effect, p, v, val, sizeof val);
        draw_row(i, e->params[p].name, val, on && !editing, on && editing);
    }
    draw_scrollbar(scroll, n);
}

// ---- the chain editor --------------------------------------------------
//
// FX Chain's parameter page. It edits the same four EP_ENUM slots any other
// page would, and it goes through effect_request_set() like any other page -
// what it does differently is *draw* them, because four rows reading
// "Slot 2  Equalizer" is a poor way to answer the only two questions you have:
// which effects are in, and in what order.
//
// So it lists the effects instead and puts the order number beside the ones
// that are in. A press adds the effect at the end of the chain; a press on one
// already in takes it out and closes the gap, so the numbers stay 1..n with
// nothing missing in the middle.
//
// The mirror below is the same trick draw_params() uses for edit_value: the
// shadow lags a change by a block, and one press moves up to four slots, so
// reading the shadow back between two quick presses would compute the second
// one against stale state. The mirror is authoritative while the page is open
// and is re-read from the shadow when it opens. Nothing else can move the
// chain in the meantime - the console does not touch it.

// ---- what this page knows about the effect it is editing ----------------
//
// **Nothing, by name.** It used to call fx_chain_effect(), FX_CHAIN_SLOTS and
// fx_chain_candidate(), which meant this display module could only be built
// alongside that one effect module. It now works off the descriptor alone:
//
//   the effect         whichever one is under the cursor - it got here because
//                      it declared EFFECT_F_COMPOSITE
//   how many slots     param_count
//   how many           params[0].max: the slots are EP_ENUM over
//   candidates         0..n, where 0 is empty
//   what each is       params[0].labels[c + 1], which the composite filled in
//   called             with the effects own name strings
//   what a slot        slot_effect(), because the encoding belongs to the
//   resolves to        composite and an int16 cannot hold a name
//
// A slot holds a candidate position plus one, not a registry index. This page
// therefore works in candidate positions throughout and asks for a registry
// index only when it needs one.
static int chain_sel[EFFECT_MAX_PARAMS];   // 0 = empty, else candidate + 1

// The composite being edited is the one the cursor is on: PAGE_CHAIN is only
// ever reached from open_params(), which is only ever reached from the effect
// list. There is nothing to search for.
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

// The candidate's name, straight out of the enum labels the composite built.
// labels[0] is the empty row; labels[c + 1] names candidate c.
static const char *chain_candidate_name(int c)
{
    const effect_t *e = chain_effect();
    if (!e || e->param_count <= 0 || !e->params[0].labels) return NULL;
    if (c < 0 || c >= chain_candidates()) return NULL;
    return e->params[0].labels[c + 1];
}

// Where this candidate sits in the chain, or -1 if it is not in it.
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

// All four every time, not just the ones that moved. Removing from the middle
// shifts everything below it, and four requests against a 32-slot ring drained
// every 1.33 ms is nothing.
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

    chain_full_ms = 0;      // any press that lands clears a stale "full"

    const int slots = chain_slots();
    const int pos   = chain_pos(c);
    if (pos >= 0) {
        // Out, and close the gap. Renumbering the rest is the whole reason the
        // removal is a shift rather than a hole: an order that reads 1, 3, 4
        // would be asking the user to work out that 2 left.
        for (int s = pos; s < slots - 1; ++s)
            chain_sel[s] = chain_sel[s + 1];
        chain_sel[slots - 1] = 0;
    } else {
        int s = 0;
        while (s < slots && chain_sel[s] != 0) ++s;

        // Full. Nothing to do but say so - the row itself carries the word,
        // which costs no space and lands where the user is already looking.
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
    draw_title(e ? e->name : "Chain");

    const int n = CHAIN_ROWS();
    for (int i = 0; i < ROWS && scroll + i < n; ++i) {
        const int item = scroll + i;
        const bool on  = (cur_item == item);

        if (item == 0) {
            draw_row(i, "< Back", NULL, on, false);
            continue;
        }

        if (CHAIN_IS_RESET(item)) {
            draw_row(i, "Reset", reset_flashing() ? "done" : NULL, on, false);
            continue;
        }

        const int c = item - 1;
        const char *cname = chain_candidate_name(c);
        if (!cname) continue;

        // Indented like the effect list, so the two read as the same kind of
        // list - and the order number lands in the value column, where the
        // parameter pages put the thing a press changes.
        char label[24];
        snprintf(label, sizeof label, LIST_INDENT "%s", cname);

        const int pos = chain_pos(c);
        char num[6] = { 0 };
        if (pos >= 0) {
            snprintf(num, sizeof num, "%d", pos + 1);
        } else if (item == chain_full_item && chain_full_ms != 0u
                   && (now_ms() - chain_full_ms) < RESET_FLASH_MS) {
            snprintf(num, sizeof num, "full");
        }

        draw_row(i, label, num[0] ? num : NULL, on, false);
    }
    draw_scrollbar(scroll, n);
}

// A solid horizontal bar - no frame, so the whole width is signal and an empty
// bar reads as empty rather than as an outline waiting to be filled. frac is
// clamped, so callers can hand it anything.
static void draw_bar(int x, int y, int w, int h, float frac)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    const int fill = (int)((float)w * frac + 0.5f);
    if (fill > 0) sh1106_fill_rect(x, y, fill, h, true);
}

// -60 dBFS to 0 dBFS across the bar. Below -60 the meter is not telling you
// anything useful anyway, and the top of the scale is where the headroom is.
static float db_frac(float db) { return (db + 60.0f) / 60.0f; }

// Meter ballistics: instant attack, timed decay. A bar that simply tracked the
// last published peak would flicker; one that decays reads as a level. 20 dB
// per second is the usual sort of fallback rate and looks right here.
#define METER_DECAY_DB_PER_S   20.0f

static float    meter_db[4] = { -120.0f, -120.0f, -120.0f, -120.0f };
static uint32_t meter_last_ms;

// ---- the number under each bar -----------------------------------------
//
// **The bar's value is the wrong thing to print**, and that is the whole
// reason this state exists. The bar attacks instantly and falls at 20 dB/s,
// which at the 25 fps redraw is 0.8 dB a frame - exactly what makes it read as
// a level, and exactly what makes a tenths digit derived from it unreadable.
// The last digit would never sit still long enough to be read.
//
// So the text carries its own reading: the peak over a window, refreshed at
// the end of it. Four times a second is slow enough for the eye to hold the
// value and fast enough to feel attached to the audio - and because it is a
// peak rather than a sample, a transient between refreshes still shows up
// instead of being missed.
#define METER_TEXT_MS   250

static float    meter_txt[4]  = { -60.0f, -60.0f, -60.0f, -60.0f };
static float    meter_peak[4] = { -120.0f, -120.0f, -120.0f, -120.0f };
static uint32_t meter_txt_ms;

// The ballistics, in one place because two pages now show these bars: DSP
// STATUS shows all four, and the spectrum shows the output pair underneath
// itself. **Exactly one of them draws in any pass**, so the shared decay clock
// is coherent - and running it from both is what makes switching between the
// two pages arrive with the meters already where the audio is, instead of
// attacking up from the floor on the first frame.
//
// **The fall rate is the caller's**, because it is the one thing the two pages
// legitimately disagree about. On DSP STATUS the bar sits next to a number and
// is read as a level; on the idle screen it sits under bands falling at
// 90 dB/s, and at the status page's rate it reads as the one dead thing on a
// moving screen. Everything else here - the attack, the floor, the peak window
// the printed reading comes from - stays common, so the two pages still show
// the same audio and only its ballistics differ.
//
// Indices are in L, in R, out L, out R throughout, which is the order the
// bars are drawn in on both pages.
static void meters_update(const display_status_t *s, float decay_db_per_s)
{
    const float db[4] = { s->in_l, s->in_r, s->out_l, s->out_r };

    // Decay against elapsed time rather than per call: this is also reached
    // from encoder events, which arrive at no fixed rate.
    const uint32_t now = now_ms();
    uint32_t dt = now - meter_last_ms;
    if (dt > 500u) dt = 500u;          // first draw, or back from a menu
    meter_last_ms = now;
    const float fall = decay_db_per_s * (float)dt / 1000.0f;

    for (int i = 0; i < 4; ++i) {
        if (db[i] >= meter_db[i]) meter_db[i] = db[i];
        else                      meter_db[i] -= fall;
        if (meter_db[i] < -60.0f) meter_db[i] = -60.0f;
    }

    // The printed reading is a peak over its own window, not the bar's value -
    // see METER_TEXT_MS. Accumulate every pass, publish on the tick.
    for (int i = 0; i < 4; ++i)
        if (db[i] > meter_peak[i]) meter_peak[i] = db[i];

    if ((uint32_t)(now - meter_txt_ms) >= METER_TEXT_MS) {
        for (int i = 0; i < 4; ++i) {
            meter_txt[i]  = (meter_peak[i] < -60.0f) ? -60.0f : meter_peak[i];
            meter_peak[i] = -120.0f;
        }
        meter_txt_ms = now;
    }
}

// ---- SRAM, as the linker left it ---------------------------------------
//
// Under copy_to_ram the whole image is resident, so what is worth showing is
// how much of main SRAM the build has already spent: everything from the base
// of SRAM up to the end of .bss. Both ends come from the link rather than from
// a constant here, so the figure cannot go stale.
//
//   __bss_end__  end of the image, and where the heap would start
//   __HeapLimit  the top of main SRAM - the two 4 KB core stacks live above it
//                in the scratch banks and are not counted
//
// It is a constant for any given build, which is exactly why it belongs on a
// page rather than in a log: the number you want is "how much room is left for
// the next effect", and the answer only changes when you rebuild.
extern char __bss_end__[];
extern char __HeapLimit[];

// Unreachable in a UI_SHOW_GFX build - that row is the counter instead - and
// -Wunused-function is right about it. Guarded rather than deleted: the switch
// is meant to go back off once item 14 is settled.
#if !UI_SHOW_GFX
static uint32_t ram_used_kb(void)
{
    return (uint32_t)(((uintptr_t)__bss_end__ - SRAM_BASE) / 1024u);
}

static uint32_t ram_total_kb(void)
{
    return (uint32_t)(((uintptr_t)__HeapLimit - SRAM_BASE) / 1024u);
}
#endif

// The page is six 9 px rows and it was already using all six, so making room
// for SRAM meant giving something up. What went was four *stacked* meters:
// in L, in R, out L and out R now sit two to a row, which halves each bar to
// 50 px and frees two rows.
//
// That turned out to be the better layout anyway. The comparison the meters
// exist for is left against right and in against out, and both of those are
// now side by side instead of one above the other with a label in between.
//
// **Each meter then gained a dBFS number, and the page had to pay for it.**
// A number wants five characters - "-22.7", or "-60.0" at the bottom of the
// scale - which is 30 px of a 128 px row. Beside a bar that leaves 31 px of
// bar, and a 31 px meter is a row of blocks rather than a level. Under the bar
// it costs a whole row instead, and the bar keeps its 50 px.
//
// Two rows were needed and only one was spare, so what went was the two budget
// bars: `dsp` and `ram` are now text on one shared row at the bottom. That is
// the right thing to give up. **The `ram` figure is a constant for any given
// build** - a bar that never moves between reboots was never telling anyone
// anything a number does not - and `dsp` keeps its percentage, which is the
// part you actually read when you are chasing an overrun. The blank separator
// row went with them; the two rows of numbers now do that job, because the
// page reads as two labelled blocks rather than as six bars.
#define ST_LABEL_W   22          // widest label is "out"
#define ST_METER_W   50          // two of these plus a gap is the panel
#define ST_METER_X2  76

static void draw_status(const display_status_t *s)
{
    draw_title("DSP STATUS");

    const effect_t *e = effect_at(effect_active());

    char buf[24];
    snprintf(buf, sizeof buf, "ovr %lu", (unsigned long)s->overruns);
    draw_row(0, e ? e->name : "-", buf, false, false);

    // in L, in R, out L, out R - the order draw_bar sees them in below.
    meters_update(s, METER_DECAY_DB_PER_S);

    // Two rows a side: the bars, then their numbers directly underneath. The
    // number is left-aligned on its own bar rather than centred under it, so
    // the two columns line up with each other and with the bars above them.
    static const char *const kSide[2] = { "in", "out" };
    for (int side = 0; side < 2; ++side) {
        const int yb = BODY_Y + (1 + side * 2) * ROW_H;   // bars
        const int yn = yb + ROW_H;                        // numbers

        sh1106_text(0, yb, kSide[side]);

        draw_bar(ST_LABEL_W, yb + BAR_INSET, ST_METER_W, BAR_H,
                 db_frac(meter_db[side * 2]));
        draw_bar(ST_METER_X2, yb + BAR_INSET, ST_METER_W, BAR_H,
                 db_frac(meter_db[side * 2 + 1]));

        // "%5.1f" rather than "%.1f": the field is fixed, so the decimal
        // points of a two-digit and a one-digit reading sit in the same
        // column and the pair can be compared at a glance. That comparison -
        // left against right, in against out - is what the page is for.
        snprintf(buf, sizeof buf, "%5.1f", (double)meter_txt[side * 2]);
        sh1106_text(ST_LABEL_W, yn, buf);

        snprintf(buf, sizeof buf, "%5.1f", (double)meter_txt[side * 2 + 1]);
        sh1106_text(ST_METER_X2, yn, buf);
    }

    // The two things this box can run out of, on one row and as text. See the
    // note by ST_LABEL_W for what bought the rows above.
    //
    // **It deliberately does not line up with the columns above it.** The
    // blank separator row is gone, so this row carries the separation itself:
    // signal is two aligned columns, resources are not, and the eye reads the
    // break without a rule or a spare row being spent on it.
    const int yb = BODY_Y + 5 * ROW_H;

#if UI_SHOW_GFX
    // See UI_SHOW_GFX. The whole row, because the reading needs three numbers
    // and any two of them can cover for the third.
    snprintf(buf, sizeof buf, "gfx %luf %lums %lub",
             (unsigned long)gfx_frames,
             (unsigned long)((gfx_worst_us + 500u) / 1000u),
             (unsigned long)gfx_bytes);
    sh1106_text(0, yb, buf);
    (void)s;
#else
    snprintf(buf, sizeof buf, "dsp %.0f%%", (double)s->dsp_pct);
    sh1106_text(0, yb, buf);

    // Used *and* total, where the bar only ever implied the total. Both ends
    // still come from the link rather than from a constant here, so neither
    // can go stale: __bss_end__ is the end of the image under copy_to_ram and
    // __HeapLimit is the top of main SRAM.
    snprintf(buf, sizeof buf, "ram %lu/%luk",
             (unsigned long)ram_used_kb(), (unsigned long)ram_total_kb());
    sh1106_text(54, yb, buf);
#endif
}

// ---- about -------------------------------------------------------------

// Centred on the panel rather than on the list's left margin: there is no
// column of values here to line anything up with.
static void draw_centred(int y, const char *s)
{
    int x = (SH1106_WIDTH - (int)strlen(s) * SH1106_CHAR_W) / 2;
    if (x < 0) x = 0;
    sh1106_text(x, y, s);
}

static void draw_about(void)
{
    draw_title("ABOUT");

    // Three lines, not the two the credit is written as: "Designed by
    // R.Hasaranga" is 23 characters and the panel fits 21, so it wraps at the
    // only place it can without hyphenating a name.
    draw_centred(20, "Designed by");
    draw_centred(30, "R.Hasaranga - 2026");
    draw_centred(46, "www.hasaranga.com");
}

// ---- the spectrum ------------------------------------------------------
//
// The idle screen. Log-spaced bands of the *output* of whichever effect is
// running, so it shows what the box is putting out rather than what went into
// it. Core 0 does the transform (spectrum.c); everything here is dB to pixels
// plus the ballistics.
//
// No title bar and no header: the top of the page is spectrum straight to the
// edge, and the effect name is one knob-nudge away underneath.
//
// **The bottom sixteen rows are the output level, left over right.** The
// spectrum says how the output is distributed and says nothing about how loud
// it is - every bar is level against a tilted reference, and the tilt is there
// precisely so the shape reads rather than the level. So the one thing the
// idle screen could not previously tell you is the one thing you look at a box
// for across a room, and it is now underneath: the same bars, the same
// ballistics and the same -60..0 scale as DSP STATUS, at full width because
// there is a whole row for each instead of two to a row.

// Not the meters' -60..0. A single FFT bin of real material sits far below the
// programme peak - a mix peaking at -10 dBFS spreads its energy over hundreds
// of bins - so a meter scale would leave every bar stubbed along the bottom.
// This window is where music actually lives.
#define SPEC_DB_FLOOR   (-78.0f)
#define SPEC_DB_CEIL    (-6.0f)

// ---- the tilt ----------------------------------------------------------
//
// A constant-bandwidth FFT measures energy per bin, and every bin is the same
// 93.75 Hz wide, so what the bars show is spectral *density*. Music is roughly
// flat per octave rather than per hertz - and an octave at the top of the range
// is eighty times wider than one at the bottom, so the same energy lands in
// eighty times the bandwidth. Measured per bin, pink noise therefore falls at
// 3 dB an octave, and produced material usually falls faster than that above a
// couple of kHz. An untilted display of music is a downhill ramp, correctly.
//
// Adding a fixed slope back is what every RTA and every media-player visualiser
// does. 3 dB/octave flattens pink noise exactly; music wants a little more, and
// this is set so that a passthrough of ordinary material sits roughly level -
// which then leaves an effect's own bass lift visible as a lift, rather than as
// a slightly steeper version of a slope that was always there.
//
// The band centres span 6.8 octaves, 133 Hz to 14.8 kHz, so each dB/octave here
// is 6.8 dB of top-against-bottom. 4.5 was tried first and put the treble too
// high; 3 is the floor worth trying, below which the bass slope reappears.
//
// The pivot sits near the geometric centre of the range, so the tilt pushes the
// bass down as much as it lifts the treble and the overall height of the
// display stays put. Anchoring it at the bottom instead would lift the top by
// the full 33 dB and pin the treble against the ceiling.
//
// The cost: a bar is no longer the true dBFS of its band, it is level against a
// tilted reference. That is the right trade for something you look at and the
// wrong one for something you measure with - spectrum.c still publishes the
// untilted truth, and this is applied on the way to the screen.
#define SPEC_TILT_DB_PER_OCT   3.5f
#define SPEC_TILT_REF_HZ       1000.0f

// Much faster than the level meters' 20 dB/s, and deliberately so: a spectrum
// wants to snap back between transients. At meter speed the bands smear into
// one slow-moving hillside and stop telling the bass from the treble. This
// falls the full height in about 0.8 s.
#define SPEC_DECAY_DB_PER_S   90.0f

// The cap holds where it landed and then falls, rather than drifting down from
// the moment it lands. A cap that falls continuously reads as a second, laggy
// meter chasing the first; hold and release reads as a mark being left and then
// let go, which is the only thing the cap is there to say. 300 ms is long
// enough to register and short enough that the caps never accumulate into a
// frame around the display.
//
// The fall is given in *rows per second*, not dB per second, because what is
// being specified here is a visual rate: at the 25 fps redraw, 25 rows/s is
// exactly one row per frame, so the cap steps down line by line instead of
// sliding. In dB/s the same intent would be a number that silently stopped
// meaning it the moment SPEC_DB_FLOOR or SPEC_DB_CEIL moved.
#define SPEC_PEAK_HOLD_MS          300u
#define SPEC_PEAK_FALL_PX_PER_S    25.0f

// ---- the output meters under the spectrum ------------------------------
//
// Two rows of eight, which is the font's own cell height, so "L" and "R" sit
// in front of their bars with no arithmetic: the glyph is 5x7 in a 6x8 cell,
// and a 3 px bar inset two rows is centred on it.
//
// The label is a whole character cell wide plus a pixel of air. That is 7 px
// of the 128, so the bar keeps 121 - against DSP STATUS's 50 - which is what
// "full width" buys: a dB is two pixels here where it was less than one there.
//
// **The two rows are two pixels further apart than the font asks for, and that
// air comes off the top of the block rather than the bottom.** R cannot simply
// move down: its row already ends at 62 of the panel's 63, so another two would
// put the last row of a 7 px glyph at y=64. blit_col() drops it rather than
// writing out of bounds, so the fault would be a quietly footless R rather than
// a crash - which is the kind of thing that ships. So the block grows upward
// instead, L stays where the font put it relative to R, and the two pixels are
// spent out of SPEC_H like everything else on this page.
#define SPEC_MET_ROW_H     8                                // the font's own cell
#define SPEC_MET_R_DROP    2                                // extra air under L
#define SPEC_MET_ROW_PITCH (SPEC_MET_ROW_H + SPEC_MET_R_DROP)
#define SPEC_MET_H         (SPEC_MET_ROW_H + SPEC_MET_ROW_PITCH)  // 18, both rows
#define SPEC_MET_Y         (SH1106_HEIGHT - SPEC_MET_H)     // 46
#define SPEC_MET_LABEL_W   (SH1106_CHAR_W + 1)              // "L" / "R", then air
#define SPEC_MET_BAR_X     SPEC_MET_LABEL_W
#define SPEC_MET_BAR_H     3
#define SPEC_MET_BAR_INSET 2                                // centres it on the glyph

// ---- the segments ------------------------------------------------------
//
// **Appearance only.** The bar is drawn solid and then cut into segments by
// punching one blank column every SPEC_MET_SEG_PITCH, so the level itself is
// still the continuous fill draw_bar() computed - nothing is quantised to a
// segment and no reading changes. A gap over the unlit part of the bar is
// black on black and invisible, which is why the punch can run the whole width
// without knowing where the fill ended.
//
// **The bar width is a multiple of the pitch, and that is why it is 120 rather
// than the 121 that was there.** 121 leaves a one-pixel stub at the right end -
// a segment visibly narrower than the fifteen beside it, at exactly the end of
// the bar where a level is read. Giving up the last column costs nothing: it is
// 0.5 dB of a 60 dB scale, and the bar no longer runs into the panel edge.
//
// 8 px is seven lit and one dark, which is two of the four-pixel segments this
// started at. Three lit pixels read as a dash rather than as a lamp; seven is
// wide enough to have a shape, and fifteen segments across 60 dB puts each at
// 4 dB - the spacing a real LED meter uses, rather than a hatched bar.
#define SPEC_MET_SEG_PITCH 8
#define SPEC_MET_SEG_GAP   1
#define SPEC_MET_SEG_COUNT ((SH1106_WIDTH - SPEC_MET_BAR_X) / SPEC_MET_SEG_PITCH)
#define SPEC_MET_BAR_W     (SPEC_MET_SEG_COUNT * SPEC_MET_SEG_PITCH)   // 120

_Static_assert(SPEC_MET_SEG_GAP < SPEC_MET_SEG_PITCH,
               "a segment gap at least as wide as the pitch erases the bar");

// **The bands' own rate**, four and a half times DSP STATUS's 20 dB/s: the
// full 60 dB scale in two thirds of a second against three. The attack is
// instant either way, so what this changes is only how long a transient's mark
// is left standing - and on a page whose bands are snapping back at
// SPEC_DECAY_DB_PER_S, a meter taking three seconds to come down did not read
// as a slower meter, it read as a stuck one. Matching them is also why this is
// a defensible place to stop: everything on the page now falls together.
//
// **Past here is where it stops being a level.** The segments are what set the
// limit rather than the eye: at 8 px a lamp is 4 dB, so above about 100 dB/s a
// single 40 ms redraw can drop more than one whole lamp and the fall reads as
// flicker rather than as motion. Raise SPEC_MET_SEG_PITCH's granularity before
// raising this, or accept that the bar is showing transients rather than
// level. DSP STATUS's peak-over-250 ms is where an exact number lives; this is
// the one you glance at.
#define SPEC_MET_DECAY_DB_PER_S   90.0f

// The gap between the bands and the L meter, and it is **ten rows, not a
// hairline**. One blank row is enough to keep the two from touching and is not
// enough to make them two things: a band sitting at full scale still read as
// something growing out of the meter under it. The separation has to be larger
// than the meters themselves before the eye takes the page as bands above and
// levels below rather than as one stack.
//
// **It is a number of its own and deliberately not a multiple of
// SPEC_MET_BAR_H**, which it started as. Tying the two means thinning the bars
// also closes the gap, and those are two separate judgements about two
// separate things - the bar height is how heavy a level reads, this is how far
// apart the two halves of the page sit. It was settled by eye at 5 px bars and
// held when they went to 3.
//
// **It is spent out of the spectrum, and that is the trade.** The bands are
// 36 px here against 64 before the meters existed, so the analyser has given up
// nearly half its height to the meters and the air around them. Worth it: what
// the bands are read for is their shape against each other, which a shorter
// display keeps, and what was actually unreadable was where the spectrum ended
// and the meters began.
#define SPEC_MET_GAP       10

// What is left is the spectrum's, and the bars stand on SPEC_H rather than on
// the bottom of the panel. SPEC_DB_PER_PX is derived from this, so the cap's
// fall stays one row per frame without anything else being touched.
#define SPEC_H          (SPEC_MET_Y - SPEC_MET_GAP)       // 36
#define SPEC_PITCH      (SH1106_WIDTH / SPECTRUM_BANDS)   // 4 px at 32 bands
#define SPEC_BAR_GAP    1
#define SPEC_BAR_W      (SPEC_PITCH - SPEC_BAR_GAP)       // 3 px

// What one row of the panel is worth, for the cap's fall.
#define SPEC_DB_PER_PX  ((SPEC_DB_CEIL - SPEC_DB_FLOOR) / (float)SPEC_H)

_Static_assert(SPEC_BAR_W >= 1, "SPECTRUM_BANDS leaves no room for a bar");

static float    spec_db[SPECTRUM_BANDS];
static float    spec_peak_db[SPECTRUM_BANDS];
static float    spec_tilt[SPECTRUM_BANDS];    // per-band dB, built at init
static uint16_t spec_peak_hold[SPECTRUM_BANDS];   // ms left before it drops
static uint32_t spec_last_ms;

// dB to bar height, clamped. Both the bar and its cap come through here, so
// they cannot disagree about where a given level sits.
static int spec_height(float db)
{
    float frac = (db - SPEC_DB_FLOOR) / (SPEC_DB_CEIL - SPEC_DB_FLOOR);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return (int)((float)SPEC_H * frac + 0.5f);
}

// The output pair, full width, left over right. meters_update() has already
// run the ballistics for the frame - this only maps them to pixels, through
// the same db_frac() and the same -60..0 scale draw_status() uses, so the two
// pages differ in how fast a bar falls and in nothing else.
static void draw_spec_meters(void)
{
    static const char *const kSide[2] = { "L", "R" };

    for (int i = 0; i < 2; ++i) {
        const int y = SPEC_MET_Y + i * SPEC_MET_ROW_PITCH;

        const int by = y + SPEC_MET_BAR_INSET;

        sh1106_text(0, y, kSide[i]);
        draw_bar(SPEC_MET_BAR_X, by, SPEC_MET_BAR_W, SPEC_MET_BAR_H,
                 db_frac(meter_db[2 + i]));      // out L, out R

        // Cut it into segments: the trailing SPEC_MET_SEG_GAP columns of every
        // pitch, blanked after the fill rather than skipped during it. Over the
        // unlit part of the bar this writes black onto black and shows nothing,
        // so the loop needs no idea where the level reached.
        for (int s = 1; s <= SPEC_MET_SEG_COUNT; ++s)
            sh1106_fill_rect(SPEC_MET_BAR_X + s * SPEC_MET_SEG_PITCH
                                             - SPEC_MET_SEG_GAP,
                             by, SPEC_MET_SEG_GAP, SPEC_MET_BAR_H, false);
    }
}

static void draw_spectrum(const spectrum_frame_t *sp,
                          const display_status_t *st)
{
    // Decay against elapsed time, not per call: this is reached from encoder
    // events too, which arrive at no fixed rate.
    const uint32_t now = now_ms();
    uint32_t dt = now - spec_last_ms;
    if (dt > 500u) dt = 500u;          // first draw, or back from a menu
    spec_last_ms = now;

    const float fall = SPEC_DECAY_DB_PER_S * (float)dt / 1000.0f;

    for (int b = 0; b < SPECTRUM_BANDS; ++b) {
        const float v = sp->band_db[b] + spec_tilt[b];

        if (v >= spec_db[b]) spec_db[b] = v;      // instant attack
        else                 spec_db[b] -= fall;
        if (spec_db[b] < SPEC_DB_FLOOR) spec_db[b] = SPEC_DB_FLOOR;

        // The cap chases the bar rather than the raw band, so it can never sit
        // below the thing it is capping - which it could if both tracked the
        // published value and the bar happened to attack past it first.
        if (spec_db[b] >= spec_peak_db[b]) {
            spec_peak_db[b]   = spec_db[b];
            spec_peak_hold[b] = SPEC_PEAK_HOLD_MS;
        } else if (spec_peak_hold[b] > dt) {
            spec_peak_hold[b] -= (uint16_t)dt;
        } else {
            // The hold ran out somewhere inside this frame. Fall for the part
            // of it that was left over rather than for the whole frame, or the
            // release would quantise to the 40 ms redraw and land a frame late.
            const uint32_t after = dt - spec_peak_hold[b];
            spec_peak_hold[b] = 0;
            spec_peak_db[b] -= SPEC_PEAK_FALL_PX_PER_S * SPEC_DB_PER_PX
                             * (float)after / 1000.0f;
        }
        if (spec_peak_db[b] < SPEC_DB_FLOOR) spec_peak_db[b] = SPEC_DB_FLOOR;

        const int x = b * SPEC_PITCH;
        const int h = spec_height(spec_db[b]);
        const int p = spec_height(spec_peak_db[b]);

        if (h > 0) sh1106_fill_rect(x, SPEC_H - h, SPEC_BAR_W, h, true);

        // The cap draws once it is clear of the bar, with a blank row between
        // the two: while they coincide the bar top *is* the peak, so a cap
        // drawn there is simply the top row of a filled bar - it adds nothing
        // and costs the eye a moving edge to track. The bar falls about 3 px
        // per frame at 90 dB/s, so it separates within a frame or two of a
        // peak and never looks missing.
        //
        // When there is no bar at all the clearance rule has nothing to be
        // clear *of*, so the cap draws regardless and comes to rest on the
        // bottom row. A band that went quiet would otherwise take its peak off
        // the screen entirely, and an idle panel would be blank rather than
        // showing a floor to read the bars against.
        if (h == 0 || p >= h + 2) {
            const int row = SPEC_H - (p > 0 ? p : 1);
            sh1106_fill_rect(x, row, SPEC_BAR_W, 1, true);
        }
    }

    // Drawn from the same pass, and after the bands, so the two halves of the
    // page are one frame of the same audio rather than two of different ones.
    meters_update(st, SPEC_MET_DECAY_DB_PER_S);
    draw_spec_meters();
}

// ---- the animations ----------------------------------------------------
//
// **All of this is behind ENABLE_VISUALIZATION**, at the top of this file. It
// is the only thing that reads oled_anim6.c, and it is the only thing that
// uses the slide, the rotation and the xorshift - so at 0 the idle screen is
// the spectrum, one page, and none of the below is compiled or linked.
#if ENABLE_VISUALIZATION

// The rest of the idle screen. Stored frames in the panel's own layout, so
// drawing one is a memcpy - see sh1106_blit(). All of it is __in_flash and
// costs no SRAM; the whole feature is the handful of bytes of state below.
//
// **The frame is chosen from the clock, not counted per call.** This is
// reached from encoder events as well as the 40 ms redraw, so a counter would
// run at whatever rate the knob happened to be turned. Deriving it from
// elapsed time also means each animation plays at its own rate against a panel
// redrawing at 25 fps: some redraws land on the frame already showing, and
// those cost nothing on the bus because the flush diffs against the shadow.
//
// FRAME_US rather than FRAME_MS because the generators say so - anim1's
// 66.667 ms rounded to 67 is 1.5 frames of drift across one 20 s turn.
//
// **Adding one is adding a row.** The rate and the length come off the table
// rather than being compiled into the drawing, so two animations recorded at
// different frame rates need no special case - and nothing below counts them
// but ANIM_COUNT. oled_vis.md in this directory is the whole procedure:
// generate, include, add the row, add the source. No page enum, no count.
#define ANIM_BYTES  (SH1106_PAGES * SH1106_WIDTH)

typedef struct {
    const uint8_t (*frames)[ANIM_BYTES];
    uint16_t       count;
    uint32_t       frame_us;
} oled_anim_t;


_Static_assert(OLED_ANIM6_W == SH1106_WIDTH && OLED_ANIM6_H == SH1106_HEIGHT,
               "oled_anim6 is not this panel's size");
_Static_assert(OLED_ANIM6_BYTES == ANIM_BYTES,
               "oled_anim6 frame is not one framebuffer");

static const oled_anim_t kAnims[] = {
	{ oled_anim6_frames, OLED_ANIM6_FRAMES, OLED_ANIM6_FRAME_US },
};

#define ANIM_COUNT  ((int)(sizeof kAnims / sizeof kAnims[0]))

_Static_assert(ANIM_COUNT >= 1, "PAGE_ANIM with nothing to play");

static void draw_anim(void)
{
    const oled_anim_t *a = &kAnims[vis_anim];

    const uint32_t elapsed_ms = now_ms() - vis_since_ms;
    const uint32_t frame = (uint32_t)(((uint64_t)elapsed_ms * 1000u)
                                      / a->frame_us)
                         % a->count;

    sh1106_blit(a->frames[frame]);
}

// ---- picking the next one ----------------------------------------------
//
// Random rather than round-robin, so a box left running does not settle into a
// recognisable loop. Uniform over everything *except* what is showing, which is
// the one outcome that would read as the rotation having failed rather than as
// chance.
//
// A xorshift is the right size of tool here: this decides which cartoon plays
// next. It is seeded from the microsecond timer the first time it is asked -
// which is genuinely variable, because the first rotation is thirty seconds
// after the last knob movement and a dev build's boot waits on a USB host - and
// stirred from the same timer on every input, so a box being used diverges from
// one that was switched on and left. No pico_rand dependency for this.
static uint32_t rnd_state;

static uint32_t vis_rand(void)
{
    if (rnd_state == 0u) rnd_state = time_us_32() | 1u;   // never seed to zero
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void vis_rand_stir(void)
{
    rnd_state ^= time_us_32();
    if (rnd_state == 0u) rnd_state = 1u;
}

// ---- the transition ----------------------------------------------------
//
// The outgoing frame slides off and the incoming one follows it in, in one of
// the four directions, chosen per transition for the same reason the next
// visual is: a fixed direction is a tell.
//
// **The duration is fixed and the pixel rate is not.** A vertical slide crosses
// 64 px where a horizontal one crosses 128, and both take TRANS_MS - so the
// screen is wiped in the same half second whichever way it went, which is what
// reads as one effect rather than two. sh1106_slide_travel() is what keeps that
// true without this code knowing which axis it picked.
//
// 500 ms is twelve frames at the 25 fps redraw. Slower reads as the panel being
// sluggish; faster is a jump cut. Every one of those frames changes the whole
// screen, so each costs a full ~1 KB flush - about 280 ms of I2C spread over
// the half second, all of it on core 1.
#define TRANS_MS    500u

static uint8_t            trans_prev[ANIM_BYTES];   // the outgoing frame, whole
static uint32_t           trans_start_ms;
static bool               trans_active;
static sh1106_slide_dir_t trans_dir;

// Which visual is showing, as one number over the spectrum and the animations
// together: 0 is the spectrum, 1.. are kAnims. Only the rotation thinks in
// these, and only so that "pick a different one" is one modulo.
#define VIS_TOTAL   (ANIM_COUNT + 1)

static void vis_go_to(int which, uint32_t now)
{
    // The framebuffer still holds the frame the panel is showing - ui_tick has
    // not cleared it yet - so this is the moment the outgoing image exists.
    sh1106_snapshot(trans_prev);
    trans_start_ms = now;
    trans_active   = true;
    trans_dir      = (sh1106_slide_dir_t)(vis_rand() % SH1106_SLIDE_DIRS);

    if (which == 0) {
        page = PAGE_SPECTRUM;
    } else {
        page     = PAGE_ANIM;
        vis_anim = which - 1;
    }
    vis_since_ms = now;
}

#endif // ENABLE_VISUALIZATION

// ---- input -------------------------------------------------------------

static void open_params(void)
{
    cur_item      = 0;
    scroll        = 0;
    editing       = false;
    reset_done_ms = 0;
    chain_full_ms = 0;

    // A composite effect's parameters are slots, and a list of slots is the
    // wrong way to look at a chain: "Slot 2  Equalizer" answers neither of the
    // two questions you have, which are what is in it and in what order. Same
    // press, same place in the flow, a different page - and underneath it is
    // still the same parameters, edited through the same request ring.
    //
    // The test is the flag, not the effect's identity, so a build with no
    // composite in it never reaches this page, and one with two gets the
    // editor for both.
    const effect_t *e = effect_at(cur_effect);
    if (e && (e->flags & EFFECT_F_COMPOSITE) && e->param_count > 0) {
        chain_sync();
        page = PAGE_CHAIN;
        return;
    }

    page = PAGE_PARAMS;
}

// Leaving either editor. This used to commit settings to flash, and no longer
// does: a write is something the user asks for on the Save row, not something
// that happens because they navigated. The Save row shows "unsaved" until they
// do, which is the part that makes it safe to have taken the automatic write
// away.
static void leave_to_list(void)
{
    page          = PAGE_EFFECTS;
    scroll        = 0;
    reset_done_ms = 0;
    chain_full_ms = 0;
    ensure_visible(cur_effect, LIST_ROWS());
}

static void handle_effects(const encoder_state_t *enc)
{
    const int n = LIST_ROWS();

    // Rotating moves the cursor and does nothing else. It used to switch the
    // active effect as it went, which meant you could not pass over a row
    // without selecting it - scrolling down to DSP Status left whatever was
    // last in the way running, and on a two-effect list that was always
    // Passthru. Selection is a press now.
    if (enc->delta) {
        cur_effect = clampi(cur_effect + enc->delta, 0, n - 1);
        ensure_visible(cur_effect, n);
    }

    if (!enc->press_edge) return;

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
        // the change is audible immediately.
        effect_request_select(cur_effect);
        return;
    }

    // Pressing the one already running opens its parameters. Two presses to
    // reach the parameters of an effect that was not live, which is right:
    // the first one is a decision about what you are listening to.
    open_params();
}

static void handle_params(const encoder_state_t *enc)
{
    const effect_t *e = effect_at(cur_effect);
    if (!e) return;

    const int n = PARAM_ROWS(e);

    if (editing) {
        const int p = cur_item - 1;
        const effect_param_t *d = &e->params[p];

        if (enc->delta) {
            edit_value = clampi(edit_value + enc->delta, d->min, d->max);
            effect_request_set(cur_effect, p, edit_value);
        }
        if (enc->press_edge) editing = false;
        return;
    }

    if (enc->delta) {
        cur_item = clampi(cur_item + enc->delta, 0, n - 1);
        ensure_visible(cur_item, n);
    }
    if (enc->press_edge) {
        if (cur_item == 0) {
            leave_to_list();
        } else if (ITEM_IS_RESET(e, cur_item)) {
            // No confirmation. It restores values the user can see and turn
            // back, on a page they had to press twice to reach - a second
            // press to confirm would cost more than the mistake does. Nothing
            // reaches flash until the Save row is pressed, either.
            effect_request_reset(cur_effect);
            reset_done_ms = now_ms();
        } else {
            editing    = true;
            edit_value = effect_value(cur_effect, cur_item - 1);
        }
    }
}

static void handle_chain(const encoder_state_t *enc)
{
    const int n = CHAIN_ROWS();

    if (enc->delta) {
        cur_item = clampi(cur_item + enc->delta, 0, n - 1);
        ensure_visible(cur_item, n);
    }
    if (!enc->press_edge) return;

    if (cur_item == 0) {
        leave_to_list();
        return;
    }

    if (CHAIN_IS_RESET(cur_item)) {
        // Clears the mirror directly rather than going through
        // effect_request_reset(). The result is identical - a composite's
        // registered defaults are empty slots - but this keeps the mirror and
        // the effect in step, which a request that lands a block later would
        // not.
        memset(chain_sel, 0, sizeof chain_sel);
        chain_push();
        reset_done_ms = now_ms();
        chain_full_ms = 0;
        return;
    }

    chain_toggle(cur_item - 1);
}

// ---- entry points ------------------------------------------------------

void ui_init(void)
{
    page          = PAGE_EFFECTS;
    resume_page   = PAGE_EFFECTS;
    cur_effect    = effect_active();
    cur_item      = 0;
    scroll        = 0;
    editing       = false;
    last_input_ms = now_ms();
    spec_last_ms  = last_input_ms;
    ensure_visible(cur_effect, LIST_ROWS());

    // Start the bands at the floor rather than at zero, or the first frame
    // arrives as an attack from 0 dBFS and the screen opens full.
    for (int b = 0; b < SPECTRUM_BANDS; ++b) {
        spec_db[b]        = SPEC_DB_FLOOR;
        spec_peak_db[b]   = SPEC_DB_FLOOR;
        spec_peak_hold[b] = 0;

        // Built once here rather than per frame: it is 32 log2f calls against
        // a table that cannot change while running.
        const float f = spectrum_band_centre_hz(b);
        spec_tilt[b] = (f > 0.0f)
                     ? SPEC_TILT_DB_PER_OCT * log2f(f / SPEC_TILT_REF_HZ)
                     : 0.0f;
    }
}

void ui_tick(const encoder_state_t *enc, const display_status_t *status,
             const spectrum_frame_t *spec)
{
    const uint32_t now = now_ms();
    const bool input = (enc->delta != 0) || enc->press_edge;

#if UI_SHOW_GFX
    // Sampled here rather than in draw_status() so the window is a real second
    // whatever page is open. Taken only while the page was visible, the first
    // reading after opening it would cover however long the page had been shut
    // and read as a wild fps - the same trap draw_anim() fell into on the
    // ST7789.
    if ((uint32_t)(now - gfx_window_ms) >= GFX_WINDOW_MS) {
        uint32_t frames = 0, worst = 0, bytes = 0;
        sh1106_stats_take(&frames, &worst, &bytes);

        const uint32_t elapsed = now - gfx_window_ms;
        gfx_frames    = elapsed ? (frames * 1000u + elapsed / 2u) / elapsed : 0u;
        gfx_worst_us  = worst;
        gfx_bytes     = frames ? (bytes + frames / 2u) / frames : 0u;
        gfx_window_ms = now;
    }
#endif

    if (input) {
        last_input_ms = now;
#if ENABLE_VISUALIZATION
        trans_active  = false;   // waking is instant; nothing slides on the way out
        vis_rand_stir();
#endif

        switch (page) {
        case PAGE_SPECTRUM:
        case PAGE_ANIM:
            // Wake only. Acting on this event would mean a knob nudged to see
            // the screen also changed a parameter. Both halves of the idle
            // screen leave the same way, so which one was showing when the
            // knob moved is not something the user has to have noticed.
            page = resume_page;
            break;

        case PAGE_STATUS:
        case PAGE_ABOUT:
            // These were asked for, so they leave on a press and ignore
            // rotation - a brush against the knob should not close a page the
            // user deliberately opened.
            if (enc->press_edge) {
                page = PAGE_EFFECTS;
                ensure_visible(cur_effect, LIST_ROWS());
            }
            break;

        case PAGE_EFFECTS:
            handle_effects(enc);
            break;

        case PAGE_CHAIN:
            handle_chain(enc);
            break;

        default:
            handle_params(enc);
            break;
        }
    } else if ((page == PAGE_EFFECTS || page == PAGE_PARAMS
                || page == PAGE_CHAIN)
               && now - last_input_ms >= IDLE_MS) {
        // Going idle does not write, for the same reason leaving a parameter
        // page does not: the user asks for a save on the Save row, and the
        // unsaved marker is what makes that safe to rely on. What the idle
        // screen must still do is not lose their *place*.
        resume_page   = page;
        editing       = false;    // do not come back mid-edit
        reset_done_ms = 0;        // nor with an acknowledgement ten seconds old
        saved_done_ms = 0;
        page          = PAGE_SPECTRUM;
#if ENABLE_VISUALIZATION
        vis_since_ms  = now;      // the spectrum gets a full turn, not the
                                  // remainder of somebody else's
    } else if (PAGE_IS_IDLE(page) && now - vis_since_ms >= VIS_ROTATE_MS) {
        // The idle screen's parts taking turns, in a random order. Nothing else
        // changes - resume_page still holds where the knob goes back to, and
        // the idle timeout is not restarted, because this is not input.
        //
        // Uniform over the others: pick out of the VIS_TOTAL - 1 that are not
        // showing, then step over the one that is. That is one modulo and no
        // retry loop, which matters because a "draw again until it differs"
        // has no bound on how long it takes.
        const int cur  = (page == PAGE_SPECTRUM) ? 0 : 1 + vis_anim;
        int       next = cur;
        if (VIS_TOTAL > 1) {
            next = (int)(vis_rand() % (uint32_t)(VIS_TOTAL - 1));
            if (next >= cur) next++;
        }
        vis_go_to(next, now);
#endif
    }

    sh1106_clear();
    switch (page) {
    case PAGE_EFFECTS: draw_effects();               break;
    case PAGE_PARAMS:  draw_params();                break;
    case PAGE_CHAIN:   draw_chain();                 break;
    case PAGE_STATUS:  draw_status(status);          break;
    case PAGE_ABOUT:   draw_about();                 break;
#if ENABLE_VISUALIZATION
    case PAGE_ANIM:    draw_anim();                  break;
#endif
    default:           draw_spectrum(spec, status);  break;
    }

#if ENABLE_VISUALIZATION
    // The incoming frame is now in the framebuffer, which is what sh1106_slide
    // wants: it composes that against the outgoing frame captured at the
    // switch. Done here rather than inside each draw_* so the transition works
    // between any two visuals without either knowing about it.
    if (trans_active) {
        const uint32_t t = now - trans_start_ms;
        if (t >= TRANS_MS) {
            trans_active = false;
        } else {
            // The travel is the direction's, not the width's: a vertical slide
            // has 64 px to cross and a horizontal one 128, in the same TRANS_MS.
            const uint32_t travel = (uint32_t)sh1106_slide_travel(trans_dir);
            sh1106_slide(trans_prev, (int)(travel * t / TRANS_MS), trans_dir);
        }
    }
#endif // ENABLE_VISUALIZATION
}
