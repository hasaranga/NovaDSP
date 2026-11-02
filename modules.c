/*
 * modules.c - walks the module list CMake generated, and names nothing.
 *
 * Every module in this build appears in `nova_modules_generated.h` - the effects
 * as an X-macro list, and the single-choice types as one macro each. That header is written at configure time from
 * the `module.cmake` files CMake found, so this file changes when the module
 * *interface* changes and at no other time - adding, removing or reordering a
 * module does not touch it.
 *
 * The declarations below are generated from the same lists, so a module whose
 * accessor is missing or misspelled is a link error naming the accessor, not a
 * silent absence.
 */

#include "module.h"
#include "nova_build.h"
#include "nova_modules_generated.h"

#include <string.h>

// ---- declarations -------------------------------------------------------
// One extern per accessor named in the generated lists. Each module defines
// exactly one of these, and it is the whole of its public surface.

#define X(fn) extern const effect_module_t *fn(void);
NOVA_EFFECT_MODULES(X)
#undef X

extern const display_module_t *NOVA_DISPLAY_MODULE(void);
extern const output_module_t  *NOVA_OUTPUT_MODULE(void);
extern const input_module_t   *NOVA_INPUT_MODULE(void);

// ---- the tables ---------------------------------------------------------
// Built at startup rather than as a static initialiser, because an accessor is
// a function call and this has to work whether a module returns a pointer to a
// const struct in .rodata or to something it constructed in init().
//
// The order here is the order CMake emitted, which is the ORDER field in each
// module.cmake. **That is the menu order**, and index 0 is what the box comes
// up running before settings are applied.

// NOVA_EFFECT_MODULE_COUNT is used directly, and it is deliberately not
// wrapped in an enum constant.
//
// **The first version of this file did wrap it, and every effect module
// vanished.** The guards below are `#if`, and the preprocessor does not know
// what an enum is - it substitutes 0 for any identifier it has no macro for.
// So `#if NOVA_EFFECT_MODULE_COUNT > 0` read as `#if 0 > 0`, the whole of collect()
// and modules_init_effects() compiled to nothing, --gc-sections dropped every
// module accessor that nothing then referenced, and the firmware linked clean
// and built an empty registry. Nothing warned, at any stage.
//
// The static assert below is what would have caught it: it re-counts the list
// and requires the count CMake wrote to agree.
#define X(fn) +1
_Static_assert((0 NOVA_EFFECT_MODULES(X)) == NOVA_EFFECT_MODULE_COUNT,
               "generated module list and count disagree");
#undef X

// A build with no effect modules at all is legal - the core passes the block
// through untouched. no_effect_module exists so that such a build still has
// something for a display to show and something for settings to key on, and
// CMake enables it automatically when the effect set would otherwise be empty.
#if NOVA_EFFECT_MODULE_COUNT > 0
static const effect_module_t *effect_mods[NOVA_EFFECT_MODULE_COUNT];
#endif

static void collect(void)
{
#if NOVA_EFFECT_MODULE_COUNT > 0
    int n = 0;
#define X(fn) effect_mods[n++] = fn();
    NOVA_EFFECT_MODULES(X)
#undef X
    (void)n;
#endif
}

// ---- startup ------------------------------------------------------------

void modules_init_effects(void)
{
    collect();

#if NOVA_EFFECT_MODULE_COUNT > 0
    // init() before effect(), always: the registry reads every parameter as it
    // registers, in order to snapshot the set that Reset restores. An engine
    // that does not exist yet reports zeroes, and those zeroes become the
    // defaults - silently, and only visible the first time someone presses
    // Reset and the effect goes flat.
    for (int i = 0; i < NOVA_EFFECT_MODULE_COUNT; ++i) {
        effect_mods[i]->init();
        if (!effect_register(effect_mods[i]->effect()))
            NOVA_LOG("  module %s: registry full, effect NOT registered\n",
                     effect_mods[i]->name);
    }

    // After every registration including their own. An effect that composes
    // other effects builds its list here, and it is the reason this is a
    // second pass rather than a line in init().
    for (int i = 0; i < NOVA_EFFECT_MODULE_COUNT; ++i)
        if (effect_mods[i]->late_init) effect_mods[i]->late_init();
#endif
}

void modules_report(void)
{
    NOVA_LOG("  modules: %d effect%s, display '%s', output '%s'\n",
             NOVA_EFFECT_MODULE_COUNT, NOVA_EFFECT_MODULE_COUNT == 1 ? "" : "s",
             modules_display()->name, modules_output()->name);

    if (modules_display()->report) modules_display()->report();
    if (modules_output()->report)  modules_output()->report();
}

// Deliberately a second function, called *after* effect_report_active(), and
// this is not tidiness.
//
// An effect module's report() is the last thing on the banner because of what
// DeeFloat's is: the console key list and **a dump of its live parameter set**.
// That set is DeeFloat's whatever effect is actually running, so on a board
// running a chain it describes something you are not listening to - and
// `active effect:` is the only line that says so. The two have to be adjacent,
// in that order, or the banner reads as one thing.
//
// The first version of this file called both from modules_report() and put
// four lines between them. It was wrong on the board before it was wrong in
// principle: the parameter line came out above the display and output lines
// and the active-effect line came out below all three.
void modules_report_effects(void)
{
#if NOVA_EFFECT_MODULE_COUNT > 0
    for (int i = 0; i < NOVA_EFFECT_MODULE_COUNT; ++i)
        if (effect_mods[i]->report) effect_mods[i]->report();
#endif
}

void modules_benchmark(void)
{
#if NOVA_TELEMETRY && NOVA_EFFECT_MODULE_COUNT > 0
    for (int i = 0; i < NOVA_EFFECT_MODULE_COUNT; ++i)
        if (effect_mods[i]->benchmark) effect_mods[i]->benchmark();
#endif
}

bool modules_handle_key(int key)
{
#if NOVA_TELEMETRY && NOVA_EFFECT_MODULE_COUNT > 0
    for (int i = 0; i < NOVA_EFFECT_MODULE_COUNT; ++i)
        if (effect_mods[i]->on_key && effect_mods[i]->on_key(key)) return true;
#else
    (void)key;
#endif
    return false;
}

// ---- the settings store's identity --------------------------------------
//
// This replaces the hand-maintained VERSION that used to sit in settings.c,
// and it replaces the rule that went with it: "bump it whenever a parameter is
// added, removed or reordered, or whenever the chain candidate list changes".
// That rule was correct and it was forgotten twice, both times presenting as
// an effect that came up silently wrong rather than as anything a counter
// could show.
//
// What is hashed is exactly what makes a stored record readable:
//
//   - every effect's name, **in registration order**. Catches an effect added,
//     removed or reordered - which is also what makes a stored FX Chain slot
//     mean something different, because a slot holds a position in a list
//     derived from that order and an int16 cannot hold a name.
//   - each effect's parameter count, and each parameter's name, kind and
//     range. Catches the positional-value trap: values inside a block are laid
//     out by index, so removing a parameter shifts every one after it.
//
// A *narrowing* of a range is caught for the same reason a reorder is - stored
// values outside the new range would be silently pulled in at load, which
// reads as an effect quietly changing its own settings. A widening is caught
// too, which is stricter than it needs to be and costs one erase.
//
// What a hash cannot see is a parameter whose *meaning* changed while its
// name, kind and range did not. That is what NOVA_SETTINGS_SALT is for: bump
// it from the build (-DNOVA_SETTINGS_SALT=n) and every stored record becomes
// unreadable, exactly as a VERSION bump used to.
#ifndef NOVA_SETTINGS_SALT
#define NOVA_SETTINGS_SALT 0
#endif

static uint32_t mix(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t mix_str(uint32_t h, const char *s)
{
    return mix(h, s ? s : "", s ? strlen(s) : 0);
}

uint32_t modules_layout_id(void)
{
    uint32_t salt = (uint32_t)(NOVA_SETTINGS_SALT);
    uint32_t h = mix(2166136261u, &salt, sizeof salt);

    const int n = effect_count();
    h = mix(h, &n, sizeof n);

    for (int i = 0; i < n; ++i) {
        const effect_t *e = effect_at(i);
        h = mix_str(h, e->name);
        h = mix(h, &e->param_count, sizeof e->param_count);

        for (int p = 0; p < e->param_count; ++p) {
            const effect_param_t *d = &e->params[p];
            const int kind = (int)d->kind;
            h = mix_str(h, d->name);
            h = mix(h, &kind,   sizeof kind);
            h = mix(h, &d->min, sizeof d->min);
            h = mix(h, &d->max, sizeof d->max);
        }
    }

    // Never zero: a blank flash slot reads as 0xFFFFFFFF and a memset one as 0,
    // and neither should be able to look like a legitimate layout.
    return h ? h : 1u;
}

// ---- the single-choice modules ------------------------------------------
// Exactly one of each, chosen with -DNOVA_DISPLAY=, -DNOVA_INPUT= and
// -DNOVA_OUTPUT=. CMake fails the configure if a name matches no module, so
// none of these can be NULL.

const display_module_t *modules_display(void) { return NOVA_DISPLAY_MODULE(); }
const output_module_t  *modules_output(void)  { return NOVA_OUTPUT_MODULE(); }
const input_module_t   *modules_input(void)   { return NOVA_INPUT_MODULE(); }
