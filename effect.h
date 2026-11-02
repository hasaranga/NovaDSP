/*
 * effect.h - the effect interface and registry.
 *
 * An effect is a block processor that can describe its own parameters well
 * enough for a UI to render and edit them without knowing anything about the
 * effect. Register several; exactly one processes audio at a time.
 *
 * ---- which core does what ----------------------------------------------
 *
 * Core 0 owns every effect's state. It is the only core that calls process(),
 * set() or get(). This is not a style choice: DeeFloat retunes filters inside
 * setParam, and doing that concurrently with process() on the other core would
 * corrupt coefficients mid-block.
 *
 * Core 1 (the UI) therefore never touches an effect directly. It *requests*
 * changes, which land in a lock-free ring that core 0 drains between blocks,
 * and it *reads* values from a shadow copy core 0 keeps up to date. The cost
 * on the audio side is draining a usually-empty ring once per block.
 *
 * The split shows up in the naming: effect_request_* is the core 1 side and is
 * asynchronous; effect_apply_* is the core 0 side and takes effect at once.
 */

#ifndef NOVADSP_EFFECT_H
#define NOVADSP_EFFECT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registry size.
//
// This used to be a hand-maintained 8 that the effect set had to be counted
// against, and a ninth effect registered past the end silently did not appear.
// It is now sized from the build: an effect module contributes one effect, so
// the number of enabled modules is exactly how many rows the registry needs.
// nova_modules_generated.h is written by CMake at configure time.
//
// The one real ceiling left is the settings record, which has to fit a
// 512-byte flash slot: one block_t per effect is NAME_LEN + 2 *
// EFFECT_MAX_PARAMS bytes, so ten effects fit and eleven do not. settings.c
// carries the static assert, and it names the way out (a larger SLOT_SIZE,
// which halves the number of slots in the ring).
#include "nova_modules_generated.h"

#if NOVA_EFFECT_MODULE_COUNT > 0
#define EFFECT_MAX          NOVA_EFFECT_MODULE_COUNT
#else
#define EFFECT_MAX          1      // a build with no effects still needs a row
#endif

#define EFFECT_MAX_PARAMS   16     // per effect; also the shadow row width

// ---- what an effect is, beyond a block hook ----------------------------
//
// Two facts a *composing* effect - one whose process() runs other registered
// effects - has to know about its candidates, and which it must not learn by
// naming them. FX Chain used to call thru_effect() directly to exclude
// Passthru, and comparing pointers against its own descriptor to exclude
// itself; both worked and both meant one module knew another existed.
//
// Declaring the property on the effect instead means the rule survives any
// module set, including one where FX Chain is the only composite and one where
// there are two.
enum {
    // process() runs other registered effects. A composite must not contain
    // another composite: the consequence is unbounded recursion rather than a
    // wrong sound. It is also the flag a display reads to decide whether to
    // offer a chain editor instead of a plain parameter list.
    EFFECT_F_COMPOSITE    = 1u << 0,

    // Keep this effect out of every composite's candidate list. Passthru sets
    // it because its whole purpose is to be the thing you A/B an effect
    // *against*; inside a chain it is a multiply by a constant that every
    // other effect there already has its own control for.
    EFFECT_F_NOT_IN_CHAIN = 1u << 1,
};

// How a value should be rendered and edited.
typedef enum {
    EP_INT,     // plain number, optionally with a unit
    EP_ENUM,    // labels[value - min]
    EP_BOOL,    // off / on
} effect_param_kind_t;

typedef struct {
    const char         *name;     // <= 8 characters reads best on 128 px
    effect_param_kind_t kind;
    int                 min;
    int                 max;
    const char *const  *labels;   // EP_ENUM: exactly (max - min + 1) entries
    const char         *unit;     // EP_INT: suffix such as "dB", or NULL
} effect_param_t;

typedef struct {
    const char           *name;         // menu label
    int                   param_count;  // <= EFFECT_MAX_PARAMS
    const effect_param_t *params;

    // Optional. Called on core 0 when this effect becomes the active one.
    void (*enter)(void);

    // The block hook: AUDIO_BLOCK_SAMPLES interleaved 32-bit samples, 24-bit
    // left-justified, processed in place. Core 0 only.
    void (*process)(int32_t *buf);

    int  (*get)(int param);
    void (*set)(int param, int value);   // must clamp

    // EFFECT_F_* above. Zero for an ordinary effect, which is what a
    // designated initialiser that omits it gets.
    uint32_t flags;

    // Optional, and only meaningful with EFFECT_F_COMPOSITE. The registry
    // index this parameter's *current value* resolves to, or -1 for a slot
    // that is empty or holds something it must not.
    //
    // This is what lets a display draw a chain editor, and the boot banner
    // expand a chain into its running order, without either of them knowing
    // how a slot is encoded. A slot holds a position in a candidate list, an
    // int16 cannot hold a name, and that mapping belongs to the effect that
    // built the list - so it is asked rather than re-derived. The composite's
    // guards must match its process() exactly, so what a caller reports here
    // is what actually runs.
    int (*slot_effect)(int param);
} effect_t;

// ---- registry (core 0, at startup) -------------------------------------
// Registering reads every parameter once to seed the shadow, so an effect must
// be initialised before it is registered. The first one registered is active.
bool effect_register(const effect_t *e);
int  effect_count(void);
const effect_t *effect_at(int index);

// ---- audio path (core 0) -----------------------------------------------

// Dispatch to the active effect, timing the call.
void effect_process_block(int32_t *buf);

// Drain the UI's command ring and refresh the active effect's shadow. Call
// once per main-loop pass, between blocks - never from an interrupt.
void effect_service(void);

// Immediate, core 0 only. The console uses this; effect_service() uses it to
// carry out what core 1 asked for.
void effect_apply_select(int index);
void effect_apply_set(int index, int param, int value);

// Restore every parameter of an effect to the value it had when it was
// registered. Registration happens before settings_load(), so that snapshot is
// the compiled-in power-on set rather than whatever was last stored - which is
// what "default" has to mean for a Reset the user reaches for after turning
// something too far.
void effect_apply_reset(int index);

// ---- timing ------------------------------------------------------------
// Cycles spent in the active effect's process(), per block. This times the
// whole hook including any format conversion the effect does, which is the
// number that matters against the deadline.

typedef struct {
    uint32_t blocks;
    uint32_t min_cyc;
    uint32_t max_cyc;
    uint64_t sum_cyc;
} effect_timing_t;

void effect_timing_take(effect_timing_t *out);   // snapshot and reset

// ---- UI side (core 1) --------------------------------------------------

int  effect_active(void);      // index of the effect currently processing

// One boot-banner line naming the effect the box came up on, and - when that
// is FX Chain - what is in it, in running order. Core 0, at startup: it prints
// through NOVA_LOG and so says nothing at all in dist.
void effect_report_active(void);

void effect_request_select(int index);
void effect_request_set(int index, int param, int value);
void effect_request_reset(int index);

// Last value core 0 saw for this parameter. Lags a real change by at most one
// block, which is invisible at UI rates.
int  effect_value(int index, int param);

// Render a value the way its descriptor says it should read - enum label,
// on/off, or number plus unit. Safe from either core. Returns buf.
char *effect_format_value(int index, int param, int value, char *buf, int n);

// ---- the effects themselves --------------------------------------------
//
// **Nothing.** This header used to end with one accessor declaration per
// effect - dsp_effect(), bbe_effect(), crys_effect() and the rest - and a
// matching effect_register() call in main(). That is what made "add an effect"
// a change to two shared files, and what made EFFECT_MAX a number somebody had
// to remember to raise.
//
// An effect now lives entirely inside its own module directory and reaches the
// core through effect_module_t in module.h. The core does not know which
// effects exist; it asks the registry above. See module.h.

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_EFFECT_H
