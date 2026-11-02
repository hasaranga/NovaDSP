/*
 * gate.cpp - NoiseGate (noisegate/) packaged as an effect.
 *
 * The glue, in the same shape as dsp.cpp, bbe.cpp, eq.cpp, crys.cpp and
 * agc.cpp: convert the block to interleaved float at +-1.0, call the engine,
 * convert back with a clamp.
 *
 * ---- mapping its parameters onto the menu ------------------------------
 *
 * The engine's own ParamInfo table is not used to build the descriptor the way
 * agc.cpp uses AutoGain's, and it is worth saying why rather than leaving it
 * looking like an oversight. Three of the four ranges are in milliseconds and
 * are either fractional at the bottom or enormous at the top:
 *
 *   Attack   0.1..50 ms.  As EP_INT the minimum rounds to 0, so the panel
 *            would read "0ms" while the engine ran 0.1 - a number that is not
 *            what the engine holds is worse than a coarser one that is.
 *   Hold     0..500 ms.   Five hundred detents is twenty-five turns of the
 *            knob for one parameter, and the UI has no acceleration.
 *   Release  5..2000 ms.  Worse again: a hundred turns end to end.
 *
 * So all three are EP_ENUM over log-spaced stops, the same move bbe.cpp makes
 * for its two crossovers and DeeFloat makes for BASS_FRQ and TREB_FRQ. **The
 * stops include the engine's own defaults** (2 ms, 50 ms, 150 ms) so power-on
 * lands exactly on a row rather than near one - the same rule bbe.cpp's
 * crossover stops follow, and the opposite trade from crys.cpp, where the
 * defaults are not multiples of ten and landing on a row won over the turn.
 *
 * Threshold is the exception and stays EP_INT at one dB per detent. Eighty
 * detents is four turns, which is more than anything else here - but it is the
 * one control that has to be set *against* a particular noise floor, so
 * resolution is worth more than turn count. It is also not a control anyone
 * sweeps end to end: you put it near the floor you can hear and nudge.
 *
 * get() maps back by nearest stop rather than trusting a stored index, so a
 * value arriving from the settings store or a future preset still lands on the
 * right row.
 *
 * ---- where it belongs in a chain ----------------------------------------
 *
 * First. A gate decides whether to pass the signal at all, so it wants to see
 * the signal as it arrived - before anything has lifted the noise floor it is
 * trying to shut out. That is the exact opposite end from Auto Gain, which
 * belongs last because it is the only thing that bounds the output, and the
 * two are worth thinking of as a pair: gate the hiss out, then level what is
 * left. A gate placed *after* a leveller is gating a signal whose quiet parts
 * have just been boosted towards the threshold it is judging them by.
 *
 * It adds no latency. There is no lookahead, because a gate has nothing to
 * protect against crossing a ceiling - only a decision to make about whether
 * to pass what it already has.
 */

#include "module.h"
#include "audio.h"
#include "nova_build.h"

#include <math.h>

#include "noisegate/NoiseGate.h"

namespace {

NoiseGate g_gate;
float     g_scratch[AUDIO_BLOCK_SAMPLES];

const float kToFloat = 1.0f / 8388608.0f;
const float kToFixed = 8388607.0f;

inline float clampf(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }

// ---- the menu's view of it ---------------------------------------------

enum {
    P_THRESH = 0,
    P_ATTACK,
    P_HOLD,
    P_RELEASE,
    P_COUNT
};

static_assert((int)P_COUNT == (int)NoiseGate::kNumParams,
              "the descriptor has drifted from NoiseGate::ParamId");
static_assert((int)P_COUNT <= EFFECT_MAX_PARAMS,
              "NoiseGate has outgrown the registry's per-effect parameter row");

// Log-spaced stops, each list including the engine's own default so power-on
// sits exactly on a row. The values are milliseconds and are handed to the
// engine unchanged; the labels are what the panel shows.
const float kAttackStops[]  = { 0.1f, 0.2f, 0.5f, 1.0f, 2.0f,
                                5.0f, 10.0f, 20.0f, 50.0f };
const float kHoldStops[]    = { 0.0f, 10.0f, 20.0f, 50.0f, 100.0f,
                                150.0f, 200.0f, 300.0f, 500.0f };
const float kReleaseStops[] = { 5.0f, 10.0f, 20.0f, 50.0f, 100.0f, 150.0f,
                                300.0f, 500.0f, 1000.0f, 2000.0f };

// Hold's bottom stop reads "off" rather than "0ms" because that is what a zero
// hold is: the gate is free to start closing the moment the level drops back
// below the threshold, with nothing bridging the gap between two notes.
const char *const kAttackLabels[]  = { "0.1ms", "0.2ms", "0.5ms", "1ms", "2ms",
                                       "5ms", "10ms", "20ms", "50ms" };
const char *const kHoldLabels[]    = { "off", "10ms", "20ms", "50ms", "100ms",
                                       "150ms", "200ms", "300ms", "500ms" };
const char *const kReleaseLabels[] = { "5ms", "10ms", "20ms", "50ms", "100ms",
                                       "150ms", "300ms", "500ms", "1s", "2s" };

const int kAttackCount  = (int)(sizeof kAttackStops  / sizeof kAttackStops[0]);
const int kHoldCount    = (int)(sizeof kHoldStops    / sizeof kHoldStops[0]);
const int kReleaseCount = (int)(sizeof kReleaseStops / sizeof kReleaseStops[0]);

const effect_param_t kGateParams[P_COUNT] = {
    { "Thresh",  EP_INT,  -80,  0, nullptr,        "dB"    },
    { "Attack",  EP_ENUM,   0,  8, kAttackLabels,  nullptr },
    { "Hold",    EP_ENUM,   0,  8, kHoldLabels,    nullptr },
    { "Release", EP_ENUM,   0,  9, kReleaseLabels, nullptr },
};

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Nearest stop to the millisecond value the engine is actually holding.
int nearest_stop(float ms, const float *stops, int n)
{
    int   best = 0;
    float d    = fabsf(ms - stops[0]);

    for (int i = 1; i < n; ++i) {
        const float di = fabsf(ms - stops[i]);
        if (di < d) { d = di; best = i; }
    }
    return best;
}

// ---- the registry's view of it -----------------------------------------

void gate_set(int p, int value)
{
    if (p < 0 || p >= P_COUNT) return;

    // The engine clamps to its own table too; this keeps the shadow honest for
    // the UI, which reads the descriptor's range to bound the knob.
    value = clampi(value, kGateParams[p].min, kGateParams[p].max);

    switch (p) {
    case P_THRESH:  g_gate.setParam(NoiseGate::kThresholdDb, (float)value); break;
    case P_ATTACK:  g_gate.setParam(NoiseGate::kAttackMs,  kAttackStops[value]);  break;
    case P_HOLD:    g_gate.setParam(NoiseGate::kHoldMs,    kHoldStops[value]);    break;
    case P_RELEASE: g_gate.setParam(NoiseGate::kReleaseMs, kReleaseStops[value]); break;
    default: break;
    }
}

int gate_get(int p)
{
    switch (p) {
    case P_THRESH:  return (int)lrintf(g_gate.getParam(NoiseGate::kThresholdDb));
    case P_ATTACK:  return nearest_stop(g_gate.getParam(NoiseGate::kAttackMs),
                                        kAttackStops, kAttackCount);
    case P_HOLD:    return nearest_stop(g_gate.getParam(NoiseGate::kHoldMs),
                                        kHoldStops, kHoldCount);
    case P_RELEASE: return nearest_stop(g_gate.getParam(NoiseGate::kReleaseMs),
                                        kReleaseStops, kReleaseCount);
    default:        return 0;
    }
}

// The block hook. Timing lives in effect_process_block(), so it covers the
// conversion either side as well as the engine call - the honest figure
// against the deadline.
void gate_process(int32_t *buf)
{
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
        g_scratch[i] = (float)(buf[i] >> 8) * kToFloat;

    g_gate.process(g_scratch, (int)AUDIO_BLOCK_FRAMES);

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t v = (int32_t)(clampf(g_scratch[i]) * kToFixed);
        buf[i] = (int32_t)((uint32_t)v << 8);
    }
}

// No enter(). Where the gate currently sits - open, closed, or part way
// through a ramp - is state worth keeping, the same as everywhere else here.
// Resetting it on selection would mean the gate slams shut and re-opens every
// time you looked at another page.
const effect_t kGateEffect = {
    /* name        */ "Noise Gate",
    /* param_count */ P_COUNT,
    /* params      */ kGateParams,
    /* enter       */ nullptr,
    /* process     */ gate_process,
    /* get         */ gate_get,
    /* set         */ gate_set,
};

} // namespace

// ---- entry points ------------------------------------------------------

void gate_init(void)
{
    g_gate.setup((float)AUDIO_SAMPLE_RATE, 2);

    // setup() already did this, and both it and process() run on core 0 - but
    // FPSCR is per core and the header is explicit that the audio core needs
    // it. Called again for the same reason as in bbe.cpp, eq.cpp and agc.cpp:
    // it costs nothing and removes the dependency on those staying the same
    // core.
    NoiseGate::enableFlushToZero();

    NOVA_LOG("Noise Gate: %u bytes per instance, %d params\n",
             (unsigned)sizeof(NoiseGate), P_COUNT);
    NOVA_LOG("  thresh %.0f dBFS, attack %.1f ms, hold %.0f ms, "
             "release %.0f ms, no lookahead\n",
             (double)g_gate.getParam(NoiseGate::kThresholdDb),
             (double)g_gate.getParam(NoiseGate::kAttackMs),
             (double)g_gate.getParam(NoiseGate::kHoldMs),
             (double)g_gate.getParam(NoiseGate::kReleaseMs));
}

static const effect_t *gate_effect(void) { return &kGateEffect; }

// ---- the module ---------------------------------------------------------
//
// One accessor, and it is the whole of this file's public surface. The core
// links against it by the name CMake found in module.cmake; nothing else in
// the firmware declares it, and no header exports anything above.

static const effect_module_t kModule = {
    .name      = "noisegate",
    .init      = gate_init,
    .effect    = gate_effect,
    .late_init = NULL,
    .report    = NULL,
    .on_key    = NULL,
    .benchmark = NULL,
};

extern "C" const effect_module_t *noisegate_module(void) { return &kModule; }

