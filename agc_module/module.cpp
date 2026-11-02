/*
 * agc.cpp - AutoGain (agc/) packaged as an effect.
 *
 * The glue, in the same shape as dsp.cpp, bbe.cpp, eq.cpp and crys.cpp:
 * convert the block to interleaved float at +-1.0, call the engine, convert
 * back with a clamp.
 *
 * Like the EQ glue this is nearly a straight pass-through of the
 * interface - all four parameters are whole-numbered over their own range,
 * three of them dB and Boost in seconds, so there is no unit translation to
 * own. The descriptor is built from the engine's
 * ParamInfo table so the ranges and defaults cannot drift from it, and the
 * engine's names are already eight characters or fewer because it was written
 * for this panel.
 *
 * ---- it is the one effect that changes the latency of the path -----------
 *
 * The lookahead is one block, always in circuit. That is 1.33 ms on top of the
 * ~2.7 ms the buffering already costs, and it is what buys a wall that holds
 * the ceiling instead of clipping at it. Worth knowing before putting it in an
 * FX Chain twice.
 *
 * ---- where it belongs in a chain ----------------------------------------
 *
 * Last. It is the only thing here that bounds the output, and every effect
 * after it can push the signal back over the ceiling it just enforced.
 */

#include "module.h"
#include "audio.h"
#include "nova_build.h"

#include "agc/AutoGain.h"

namespace {

AutoGain g_agc;
float    g_scratch[AUDIO_BLOCK_SAMPLES];

const float kToFloat = 1.0f / 8388608.0f;
const float kToFixed = 8388607.0f;

inline float clampf(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }

static_assert((int)AutoGain::kNumParams <= EFFECT_MAX_PARAMS,
              "AutoGain has outgrown the registry's per-effect parameter row");

// The engine's gain schedule is exact only while its delay line can hold a
// whole block, because the lookahead *is* the block. Raising AUDIO_BLOCK_FRAMES
// past the line's capacity would silently demote the wall to a clamp - i.e.
// back to clipping - so it is caught here instead.
static_assert((int)AUDIO_BLOCK_FRAMES <= (int)AutoGain::kLookahead,
              "AUDIO_BLOCK_FRAMES exceeds AUTOGAIN_LOOKAHEAD: raise the latter");

effect_param_t kAgcParams[AutoGain::kNumParams];

void build_params(void)
{
    for (int i = 0; i < (int)AutoGain::kNumParams; ++i) {
        const AutoGain::ParamInfo &pi = AutoGain::paramInfo(i);

        kAgcParams[i] = (effect_param_t){
            pi.name,
            pi.isSwitch ? EP_BOOL : EP_INT,
            (int)pi.minValue,
            (int)pi.maxValue,
            nullptr,
            (pi.unit && pi.unit[0]) ? pi.unit : nullptr,
        };
    }
}

// ---- the registry's view of it -----------------------------------------

void agc_set(int p, int value)
{
    if (p < 0 || p >= (int)AutoGain::kNumParams) return;

    // The engine clamps too; this keeps the shadow honest for the UI, which
    // reads the descriptor's range to bound the knob.
    if (value < kAgcParams[p].min) value = kAgcParams[p].min;
    if (value > kAgcParams[p].max) value = kAgcParams[p].max;

    g_agc.setParam(p, (float)value);
}

int agc_get(int p)
{
    if (p < 0 || p >= (int)AutoGain::kNumParams) return 0;

    const float v = g_agc.getParam(p);
    return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

void agc_process(int32_t *buf)
{
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
        g_scratch[i] = (float)(buf[i] >> 8) * kToFloat;

    g_agc.process(g_scratch, (int)AUDIO_BLOCK_FRAMES);

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t v = (int32_t)(clampf(g_scratch[i]) * kToFixed);
        buf[i] = (int32_t)((uint32_t)v << 8);
    }
}

// No enter(). The gain the leveller has arrived at is state worth keeping -
// clearing it on every selection would mean an effect that starts levelling
// from scratch every time you look at another page. Coming back to it sounds
// as you left it, the same as everything else here.
effect_t kAgcEffect = {
    /* name        */ "Auto Gain",
    /* param_count */ (int)AutoGain::kNumParams,
    /* params      */ kAgcParams,
    /* enter       */ nullptr,
    /* process     */ agc_process,
    /* get         */ agc_get,
    /* set         */ agc_set,
};

} // namespace

// ---- entry points ------------------------------------------------------

void agc_init(void)
{
    build_params();
    g_agc.setup((float)AUDIO_SAMPLE_RATE, 2);

    // FPSCR is per core and setup() ran on this one, but so will process().
    // Called again for the same reason as in bbe.cpp and eq.cpp: it costs
    // nothing and removes the dependency on those staying the same core.
    AutoGain::enableFlushToZero();

    // The lookahead is printed from AUDIO_BLOCK_FRAMES rather than from the
    // engine: the engine adopts it from the first block it is handed, and none
    // has gone through yet.
    NOVA_LOG("Auto Gain: %u bytes per instance, target %.0f dBFS, "
             "+-%.0f dB, boost after %.0f s, wall %.0f dBFS, "
             "lookahead %u frames (%.2f ms)\n",
             (unsigned)sizeof(AutoGain),
             (double)g_agc.getParam(AutoGain::kTargetDb),
             (double)g_agc.getParam(AutoGain::kMaxGainDb),
             (double)g_agc.getParam(AutoGain::kBoostDelay),
             (double)g_agc.getParam(AutoGain::kOutputDb),
             (unsigned)AUDIO_BLOCK_FRAMES,
             (double)(1000.0f * (float)AUDIO_BLOCK_FRAMES
                      / (float)AUDIO_SAMPLE_RATE));
}

static const effect_t *agc_effect(void) { return &kAgcEffect; }

// ---- the module ---------------------------------------------------------
//
// One accessor, and it is the whole of this file's public surface. The core
// links against it by the name CMake found in module.cmake; nothing else in
// the firmware declares it, and no header exports anything above.

static const effect_module_t kModule = {
    .name      = "agc",
    .init      = agc_init,
    .effect    = agc_effect,
    .late_init = NULL,
    .report    = NULL,
    .on_key    = NULL,
    .benchmark = NULL,
};

extern "C" const effect_module_t *agc_module(void) { return &kModule; }

