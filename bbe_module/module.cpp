/*
 * bbe.cpp - SonicMaximizer (bbe_maximizer/) packaged as an effect.
 *
 * The glue, in the same shape as dsp.cpp: convert the block to interleaved
 * float at +-1.0, call the engine, convert back with a clamp. The engine is
 * C++ and everything the registry sees is C, which is why this file is .cpp
 * with an extern "C" accessor.
 *
 * ---- mapping its parameters onto the menu ------------------------------
 *
 * SonicMaximizer takes floats over wide, sometimes logarithmic ranges; the
 * effect interface takes integers, one detent per unit. So this file owns the
 * translation, and the descriptor table below is the only place the two
 * meanings are written down together.
 *
 * Two of them are not one-to-one, and the reason is the knob rather than the
 * engine:
 *
 *   Dynamics is 0..100 % in the engine and 0..10 here. A hundred detents is
 *   five turns of the encoder for one parameter, and the UI has no
 *   acceleration - a 0..10 scale matches Process and Lo Contour beside it and
 *   is what the value is actually worth.
 *
 *   The two crossovers are logarithmic ranges 340 Hz and 3400 Hz wide. As
 *   plain integers they would be hundreds of detents; as EP_ENUM they become
 *   nine sensible stops that read as frequencies on the panel, which is what
 *   DeeFloat already does with BASS_FRQ and TREB_FRQ.
 *
 * get() maps back by nearest stop rather than trusting a stored index, so a
 * value set from anywhere - the console, the settings store, a future preset -
 * still lands on the right row.
 */

#include "module.h"
#include "nova_build.h"
#include "audio.h"

#include <math.h>
#include <stdio.h>

#include "bbe_maximizer/SonicMaximizer.h"
namespace {

SonicMaximizer g_bbe;
float          g_scratch[AUDIO_BLOCK_SAMPLES];

const float kToFloat = 1.0f / 8388608.0f;
const float kToFixed = 8388607.0f;

inline float clampf(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }

// ---- the menu's view of it ---------------------------------------------

enum {
    P_PROCESS = 0,
    P_CONTOUR,
    P_DYNAMIC,
    P_LO_FRQ,
    P_HI_FRQ,
    P_OUTPUT,
    P_BYPASS,
    P_COUNT
};

// Safety is deliberately not in that list. It is the engine's output limiter
// at -0.2 dBFS, it is pinned on in bbe_init(), and there is no good reason to
// let anyone turn it off: with Process at 10 the high band is boosted 12 dB,
// and the only thing between that and a clipped conversion on the way out is
// the limiter. Leaving it out of the descriptor keeps it out of the menu, out
// of the settings record, and out of the shadow - the registry only knows
// about parameters the effect declares.

// Nine stops each, chosen to include the engine's own defaults (150 Hz and
// 1200 Hz) so the power-on setting lands exactly on a row rather than near one.
const float kLoStops[] = { 60.0f, 80.0f, 100.0f, 120.0f, 150.0f,
                           200.0f, 250.0f, 300.0f, 400.0f };
const float kHiStops[] = { 600.0f, 800.0f, 1000.0f, 1200.0f, 1600.0f,
                           2000.0f, 2500.0f, 3000.0f, 4000.0f };

const char *const kLoLabels[] = { "60Hz", "80Hz", "100Hz", "120Hz", "150Hz",
                                  "200Hz", "250Hz", "300Hz", "400Hz" };
const char *const kHiLabels[] = { "600Hz", "800Hz", "1kHz", "1.2kHz", "1.6kHz",
                                  "2kHz", "2.5kHz", "3kHz", "4kHz" };

const int kLoCount = (int)(sizeof kLoStops / sizeof kLoStops[0]);
const int kHiCount = (int)(sizeof kHiStops / sizeof kHiStops[0]);

// Names are <= 8 characters, which is what reads cleanly on 128 px beside a
// value cell. The engine's own longer names ("Lo/Mid X-over") do not fit.
const effect_param_t kBbeParams[P_COUNT] = {
    { "Process", EP_INT,   0,  10, nullptr,   nullptr },  // HF restoration
    { "Contour", EP_INT,   0,  10, nullptr,   nullptr },  // bass shelf
    { "Dynamic", EP_INT,   0,  10, nullptr,   nullptr },  // x10 = engine %
    { "LoFreq",  EP_ENUM,  0,   8, kLoLabels, nullptr },  // lo/mid crossover
    { "HiFreq",  EP_ENUM,  0,   8, kHiLabels, nullptr },  // mid/hi crossover
    { "Output",  EP_INT, -12,  12, nullptr,   "dB"    },
    { "Bypass",  EP_BOOL,  0,   1, nullptr,   nullptr },
};

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Nearest stop to a frequency the engine is actually holding.
int nearest_stop(float hz, const float *stops, int n)
{
    int   best = 0;
    float d    = fabsf(hz - stops[0]);

    for (int i = 1; i < n; ++i) {
        const float di = fabsf(hz - stops[i]);
        if (di < d) { d = di; best = i; }
    }
    return best;
}

// ---- the registry's view of it -----------------------------------------

void bbe_set(int p, int value)
{
    if (p < 0 || p >= P_COUNT) return;
    value = clampi(value, kBbeParams[p].min, kBbeParams[p].max);

    switch (p) {
    case P_PROCESS: g_bbe.setParam(SonicMaximizer::kProcess,   (float)value); break;
    case P_CONTOUR: g_bbe.setParam(SonicMaximizer::kLoContour, (float)value); break;
    case P_DYNAMIC: g_bbe.setParam(SonicMaximizer::kDynamics,  (float)value * 10.0f); break;
    case P_LO_FRQ:  g_bbe.setParam(SonicMaximizer::kLoFreq,    kLoStops[value]); break;
    case P_HI_FRQ:  g_bbe.setParam(SonicMaximizer::kHiFreq,    kHiStops[value]); break;
    case P_OUTPUT:  g_bbe.setParam(SonicMaximizer::kOutput,    (float)value); break;
    case P_BYPASS:  g_bbe.setParam(SonicMaximizer::kBypass,    value ? 1.0f : 0.0f); break;
    default: break;
    }
}

int bbe_get(int p)
{
    switch (p) {
    case P_PROCESS: return (int)lrintf(g_bbe.getParam(SonicMaximizer::kProcess));
    case P_CONTOUR: return (int)lrintf(g_bbe.getParam(SonicMaximizer::kLoContour));
    case P_DYNAMIC: return (int)lrintf(g_bbe.getParam(SonicMaximizer::kDynamics) * 0.1f);
    case P_LO_FRQ:  return nearest_stop(g_bbe.getParam(SonicMaximizer::kLoFreq),
                                        kLoStops, kLoCount);
    case P_HI_FRQ:  return nearest_stop(g_bbe.getParam(SonicMaximizer::kHiFreq),
                                        kHiStops, kHiCount);
    case P_OUTPUT:  return (int)lrintf(g_bbe.getParam(SonicMaximizer::kOutput));
    case P_BYPASS:  return g_bbe.getParam(SonicMaximizer::kBypass) >= 0.5f ? 1 : 0;
    default:        return 0;
    }
}

// The block hook. Timing lives in effect_process_block(), so it covers the
// conversion either side as well as the engine call - the honest figure
// against the deadline.
void bbe_process(int32_t *buf)
{
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
        g_scratch[i] = (float)(buf[i] >> 8) * kToFloat;

    g_bbe.process(g_scratch, (int)AUDIO_BLOCK_FRAMES);

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t v = (int32_t)(clampf(g_scratch[i]) * kToFixed);
        buf[i] = (int32_t)((uint32_t)v << 8);
    }
}

const effect_t kBbeEffect = {
    /* name        */ "BBE Maximizer",
    /* param_count */ P_COUNT,
    /* params      */ kBbeParams,
    /* enter       */ nullptr,   // parameters and filter state both persist,
                                 // so coming back to it sounds as you left it
    /* process     */ bbe_process,
    /* get         */ bbe_get,
    /* set         */ bbe_set,
};

} // namespace

// ---- entry points ------------------------------------------------------

void bbe_init(void)
{
    g_bbe.setup((float)AUDIO_SAMPLE_RATE, 2);

    // Pinned on, and not exposed - see the note beside the descriptor table.
    g_bbe.setParam(SonicMaximizer::kSafety, 1.0f);

    // setup() already did this, and both it and process() run on core 0 - but
    // FPSCR is per core and the header is explicit that the audio core needs
    // it. Calling it again from the core that will actually run the filters
    // costs nothing and removes the dependency on those staying the same core.
    SonicMaximizer::enableFlushToZero();

    NOVA_LOG("BBE Maximizer: %u bytes per instance, %d params\n",
           (unsigned)sizeof(SonicMaximizer), P_COUNT);
    NOVA_LOG("  process %.0f, contour %.0f, dynamics %.0f%%, "
           "xover %.0f/%.0f Hz, out %.0f dB, safety %s (pinned)\n",
           (double)g_bbe.getParam(SonicMaximizer::kProcess),
           (double)g_bbe.getParam(SonicMaximizer::kLoContour),
           (double)g_bbe.getParam(SonicMaximizer::kDynamics),
           (double)g_bbe.getParam(SonicMaximizer::kLoFreq),
           (double)g_bbe.getParam(SonicMaximizer::kHiFreq),
           (double)g_bbe.getParam(SonicMaximizer::kOutput),
           g_bbe.getParam(SonicMaximizer::kSafety) >= 0.5f ? "on" : "off");
}

static const effect_t *bbe_effect(void) { return &kBbeEffect; }

// ---- the module ---------------------------------------------------------
//
// One accessor, and it is the whole of this file's public surface. The core
// links against it by the name CMake found in module.cmake; nothing else in
// the firmware declares it, and no header exports anything above.

static const effect_module_t kModule = {
    .name      = "bbe",
    .init      = bbe_init,
    .effect    = bbe_effect,
    .late_init = NULL,
    .report    = NULL,
    .on_key    = NULL,
    .benchmark = NULL,
};

extern "C" const effect_module_t *bbe_module(void) { return &kModule; }

