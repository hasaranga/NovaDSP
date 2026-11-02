/*
 * no_effect_module - the effect that is not one.
 *
 * A build with no effect modules is legal: the core takes a block from the
 * ADC, transforms it for the spectrum and hands it to the output, and an empty
 * registry simply means nothing happens in between. But an empty registry also
 * leaves the menu with no rows, the settings store with nothing to key on and
 * the boot banner with nothing to name, and all three of those read as a
 * firmware that is broken rather than one that was asked for nothing.
 *
 * So this is what an effectless build gets: one registry entry, no parameters,
 * and a process() that returns immediately. It is bit-exact - the block is not
 * copied, scaled or converted - so it costs a call and nothing else.
 *
 * **It is not Passthru.** thru_module is a gain trim you reach for in order to
 * A/B an effect at a matched level, and it earns its menu row. This earns
 * nothing and exists so that the rest of the firmware never has to special-case
 * a count of zero. CMake enables it only when it would otherwise be alone.
 */

#include "module.h"

static void none_init(void) { }

static void none_process(int32_t *buf) { (void)buf; }

static int  none_get(int p)            { (void)p; return 0; }
static void none_set(int p, int value) { (void)p; (void)value; }

static const effect_t kNone = {
    .name        = "None",
    .param_count = 0,
    .params      = NULL,
    .enter       = NULL,
    .process     = none_process,
    .get         = none_get,
    .set         = none_set,
};

static const effect_t *none_effect(void) { return &kNone; }

static const effect_module_t kModule = {
    .name      = "none",
    .init      = none_init,
    .effect    = none_effect,
    .late_init = NULL,
    .report    = NULL,
    .on_key    = NULL,
    .benchmark = NULL,
};

const effect_module_t *no_effect_module(void) { return &kModule; }
