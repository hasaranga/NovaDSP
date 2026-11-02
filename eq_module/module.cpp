/*
 * eq.cpp - Equalizer (equalizer/) packaged as an effect.
 *
 * The glue, in the same shape as dsp.cpp and bbe.cpp: convert the block to
 * interleaved float at +-1.0, call the engine, convert back with a clamp.
 *
 * This one is nearly a straight pass-through of the interface: every parameter
 * is a gain in whole dB over the same range, so unlike the BBE glue there is
 * no unit translation to own - the descriptor table is the engine's own band
 * labels and one range repeated. Which is the point of the exercise: a third
 * effect that fits the descriptor model without bending it.
 *
 * The preamp sits first in the list rather than last. Ten bands that each boost
 * 12 dB add to far more than 12 dB, so it is the control you reach for as soon
 * as you start pushing bands up, and it should not be eleven detents past them.
 */

#include "module.h"
#include "nova_build.h"
#include "audio.h"

#include <stdio.h>

#include "equalizer/Equalizer.h"
namespace {

Equalizer g_eq;
float     g_scratch[AUDIO_BLOCK_SAMPLES];

const float kToFloat = 1.0f / 8388608.0f;
const float kToFixed = 8388607.0f;

inline float clampf(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }

// Preamp first, then the ten bands in frequency order.
enum { P_PREAMP = 0, P_BAND0, P_COUNT = P_BAND0 + Equalizer::kNumBands };

static_assert(P_COUNT <= EFFECT_MAX_PARAMS,
              "the EQ has outgrown the registry's per-effect parameter row");

// Built at init rather than written out, so the band labels and the gain range
// come from the engine and cannot drift from it.
effect_param_t kEqParams[P_COUNT];

void build_params(void)
{
    kEqParams[P_PREAMP] = (effect_param_t){
        "Preamp", EP_INT,
        (int)Equalizer::kMinGainDb, (int)Equalizer::kMaxGainDb, nullptr, "dB"
    };

    for (int b = 0; b < Equalizer::kNumBands; ++b) {
        kEqParams[P_BAND0 + b] = (effect_param_t){
            Equalizer::kBandLabel[b], EP_INT,
            (int)Equalizer::kMinGainDb, (int)Equalizer::kMaxGainDb,
            nullptr, "dB"
        };
    }
}

// ---- the registry's view of it -----------------------------------------

void eq_set(int p, int value)
{
    if (p < 0 || p >= P_COUNT) return;

    // The engine clamps too; this keeps the shadow honest for the UI, which
    // reads the descriptor's range to bound the knob.
    if (value < kEqParams[p].min) value = kEqParams[p].min;
    if (value > kEqParams[p].max) value = kEqParams[p].max;

    if (p == P_PREAMP) g_eq.setPreampDb((float)value);
    else               g_eq.setBandGainDb(p - P_BAND0, (float)value);
}

int eq_get(int p)
{
    if (p < 0 || p >= P_COUNT) return 0;

    const float db = (p == P_PREAMP) ? g_eq.preampDb()
                                     : g_eq.bandGainDb(p - P_BAND0);

    // Every value that can be set here is a whole dB, so this is exact rather
    // than a rounding of something continuous.
    return (int)(db < 0.0f ? db - 0.5f : db + 0.5f);
}

void eq_process(int32_t *buf)
{
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
        g_scratch[i] = (float)(buf[i] >> 8) * kToFloat;

    g_eq.process(g_scratch, (int)AUDIO_BLOCK_FRAMES);

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t v = (int32_t)(clampf(g_scratch[i]) * kToFixed);
        buf[i] = (int32_t)((uint32_t)v << 8);
    }
}

effect_t kEqEffect = {
    /* name        */ "Equalizer",
    /* param_count */ P_COUNT,
    /* params      */ kEqParams,
    /* enter       */ nullptr,   // gains and filter state both persist, so
                                 // coming back to it sounds as you left it
    /* process     */ eq_process,
    /* get         */ eq_get,
    /* set         */ eq_set,
};

} // namespace

// ---- entry points ------------------------------------------------------

void eq_init(void)
{
    build_params();
    g_eq.setup((float)AUDIO_SAMPLE_RATE, 2);

    // FPSCR is per core and setup() ran on this one, but so will process().
    // Called again for the same reason as in bbe.cpp: it costs nothing and
    // removes the dependency on those staying the same core.
    Equalizer::enableFlushToZero();

    NOVA_LOG("Equalizer: %u bytes per instance, %d bands %.0f..%.0f Hz, "
           "%+.0f..%+.0f dB\n",
           (unsigned)sizeof(Equalizer), Equalizer::kNumBands,
           (double)Equalizer::kBandHz[0],
           (double)Equalizer::kBandHz[Equalizer::kNumBands - 1],
           (double)Equalizer::kMinGainDb, (double)Equalizer::kMaxGainDb);
}

static const effect_t *eq_effect(void) { return &kEqEffect; }

// ---- the module ---------------------------------------------------------
//
// One accessor, and it is the whole of this file's public surface. The core
// links against it by the name CMake found in module.cmake; nothing else in
// the firmware declares it, and no header exports anything above.

static const effect_module_t kModule = {
    .name      = "eq",
    .init      = eq_init,
    .effect    = eq_effect,
    .late_init = NULL,
    .report    = NULL,
    .on_key    = NULL,
    .benchmark = NULL,
};

extern "C" const effect_module_t *eq_module(void) { return &kModule; }

