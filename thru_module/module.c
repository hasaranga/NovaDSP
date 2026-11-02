/*
 * thru.c - passthrough with a gain trim.
 *
 * **The effect the box comes up running.** ORDER 5 in module.cmake puts it
 * first in registration order, and the first effect registered is the active
 * one until settings_load() says otherwise - so a freshly flashed board, or
 * one whose stored record was invalidated by a layout change, passes audio
 * through unchanged rather than starting inside somebody's power-on parameter
 * table.
 *
 * It is also useful in its own right: switching between this and DeeFloat at a
 * matched level is the only honest way to judge what the effect is doing.
 *
 * Gain is computed on the parameter change, never per sample - one powf at
 * control rate against 128 multiplies per block.
 */

#include "module.h"
#include "audio.h"

#include <math.h>

enum { P_GAIN = 0 };

static int   gain_db;            // -12..+12
static float gain_lin = 1.0f;

static const effect_param_t kParams[] = {
    { "Gain", EP_INT, -12, 12, NULL, "dB" },
};

static void thru_set(int p, int value)
{
    if (p != P_GAIN) return;
    if (value < kParams[P_GAIN].min) value = kParams[P_GAIN].min;
    if (value > kParams[P_GAIN].max) value = kParams[P_GAIN].max;
    gain_db  = value;
    gain_lin = powf(10.0f, (float)value / 20.0f);
}

static int thru_get(int p)
{
    return (p == P_GAIN) ? gain_db : 0;
}

static void thru_process(int32_t *buf)
{
    if (gain_db == 0) return;    // bit-exact passthrough, and the common case

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        float s = (float)(buf[i] >> 8) * gain_lin;
        if (s >  8388607.0f) s =  8388607.0f;
        if (s < -8388608.0f) s = -8388608.0f;
        buf[i] = (int32_t)((uint32_t)(int32_t)s << 8);
    }
}

static const effect_t kThru = {
    .name        = "Passthru",
    .param_count = 1,
    .params      = kParams,
    .enter       = NULL,
    .process     = thru_process,
    .get         = thru_get,
    .set         = thru_set,

    // Kept out of every composite's candidate list. FX Chain used to know this
    // by calling thru_effect() and comparing pointers, which meant one module
    // knew another existed; the effect declares it instead, and whether a
    // chain is even in this build stops mattering.
    //
    // The reason is unchanged: this is the thing you switch *to* in order to
    // hear what an effect was doing. Inside a chain it is a multiply by a
    // constant that every other effect there already has its own control for.
    .flags       = EFFECT_F_NOT_IN_CHAIN,
};

static const effect_t *thru_effect(void) { return &kThru; }

// Nothing to construct: the gain is an int and a float, both statically
// initialised. init() exists because the interface requires it, and because an
// effect that grows a real engine later should not have to change shape.
static void thru_module_init(void) { }

static const effect_module_t kModule = {
    .name      = "thru",
    .init      = thru_module_init,
    .effect    = thru_effect,
    .late_init = NULL,
    .report    = NULL,
    .on_key    = NULL,
    .benchmark = NULL,
};

const effect_module_t *thru_module(void) { return &kModule; }
