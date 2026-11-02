/*
 * module.cpp - Leveller (leveller/) packaged as an effect.
 *
 * The glue, in the same shape as dsp.cpp, bbe.cpp, eq.cpp, crys.cpp, agc.cpp
 * and gate.cpp: convert the block to interleaved float at +-1.0, call the
 * engine, convert back with a clamp.
 *
 * ---- three parameters out of eight, and why -----------------------------
 *
 * The engine carries eight parameters. This descriptor publishes **three** -
 * Degree, Thresh and Bypass - and the other five are put at their own defaults
 * in lev_init() and never mentioned again. That is the whole of how "hidden"
 * is implemented here, and it is worth being explicit about, because the
 * temptation is to look for a hidden flag on effect_param_t and there isn't
 * one.
 *
 * There does not need to be. The UI renders exactly what the descriptor
 * declares: the menu walks 0..param_count-1, the settings record stores
 * param_count values per effect, and modules_layout_id() hashes the same
 * range. A parameter left out of the descriptor is therefore invisible on the
 * panel, absent from flash, and outside the layout hash - one omission, three
 * consistent consequences. A "hidden" bit would have had to be honoured
 * separately in all three places.
 *
 * What the five hidden ones are left at, and why those are the right values to
 * be stuck with:
 *
 *   Smooth   on      the envelope-gain mode. The classic waveshaper measures
 *                    ~10 % distortion on loud bass (see Leveller.h); it is a
 *                    sound, not a setting, and not one to leave a knob
 *                    pointing at on a box with no undo.
 *   Attack   10 ms   with Smooth on, these two are the ballistics of the gain,
 *   Release  200 ms  and they are the pair that turn a leveller into a pumping
 *                    compressor when set wrong. The defaults are what the
 *                    engine was voiced at.
 *   Output   0 dB    unity. Makeup belongs to Passthru's trim or to Auto Gain,
 *                    both of which are already in the menu; a second output
 *                    trim here would only be a way to clip twice.
 *   Mix      100 %   fully wet. Degree already sets how hard the curve works,
 *                    so a dry blend would be a second control for one axis.
 *
 * Un-hiding one is adding a row to kEngineParam[] and kNames[] and raising
 * P_COUNT - the descriptor is built from the engine's own ParamInfo table, so
 * range, unit and switch-ness come across on their own. **It invalidates the
 * settings store**, the same as any other layout change: param_count is
 * hashed.
 *
 * ---- the two ranges the knob sees ---------------------------------------
 *
 * Both are whole-numbered over a range the knob can walk, so unlike gate.cpp
 * there is no EP_ENUM translation to own. Degree is 1..5, five detents. Thresh
 * is -80..-20 dB at one dB per detent, sixty detents - three turns, which is
 * the same trade the gate's threshold makes: it is a control set *against* a
 * particular noise floor, so resolution beats turn count.
 *
 * Thresh is the bottom of the boosted region, **not** a gate threshold. Below
 * it the curve backs off (segment gain 0.8) instead of lifting, so it is the
 * line between "quiet part of the music" and "the noise floor I do not want
 * brought up with it". Set it below the quietest passage worth lifting.
 *
 * ---- where it belongs in a chain ----------------------------------------
 *
 * After the gate, before Auto Gain. It rides levels within a passage, which is
 * what a gate wants to happen *after* it has decided what to pass - a leveller
 * lifts the quiet parts towards the threshold the gate is judging by - and
 * Auto Gain still belongs last, because it is the only thing that bounds the
 * output.
 *
 * It adds no latency: no lookahead, one float of envelope state.
 *
 * ---- one thing to measure -----------------------------------------------
 *
 * The engine measured **22 % of the deadline** on its first build, when its
 * envelope, ramps and curve division were still `double` on a chip with no
 * double unit. It is single-precision throughout now and `process()` compiles
 * to no double operations at all - **but what that is worth has not been read
 * off the board.** Select `Leveller`, read `dsp` on DSP STATUS with material
 * playing; against silence the engine's own floor checks short-circuit, which
 * is not the figure that matters.
 *
 * None of that reached this file: the conversion was contained to
 * leveller/Leveller.{h,cpp}, and the interface it presents here - double in at
 * setParam(), float samples through process() - is what it always was. That is
 * the argument for the glue owning nothing but the mapping.
 */

#include "module.h"
#include "audio.h"
#include "nova_build.h"

#include <math.h>

#include "leveller/Leveller.h"

namespace {

Leveller g_lev;
float    g_scratch[AUDIO_BLOCK_SAMPLES];

const float kToFloat = 1.0f / 8388608.0f;
const float kToFixed = 8388607.0f;

inline float clampf(float x) { return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); }

// ---- the menu's view of it ---------------------------------------------
//
// Position in this list is menu order; the engine id each row maps onto is in
// kEngineParam[]. Bypass sits last for the same reason it does in bbe.cpp - it
// is the row you reach for to A/B, not one you set and leave.

enum {
    P_DEGREE = 0,
    P_THRESH,
    P_BYPASS,
    P_COUNT
};

const int kEngineParam[P_COUNT] = {
    Leveller::kDegree,
    Leveller::kThreshold,
    Leveller::kBypass,
};

static_assert((int)P_COUNT <= (int)Leveller::kNumParams,
              "the descriptor publishes more parameters than the engine has");
static_assert((int)P_COUNT <= EFFECT_MAX_PARAMS,
              "Leveller has outgrown the registry's per-effect parameter row");

// Names are <= 8 characters, which is what reads cleanly on 128 px beside a
// value cell. The engine calls these "Leveling" and "Threshold" - the names
// Audacity's dialog uses, and neither fits.
const char *const kNames[P_COUNT] = { "Degree", "Thresh", "Bypass" };

// Filled from the engine's ParamInfo table in lev_init(), so range, unit and
// switch-ness cannot drift from the engine the way a hand-copied row would.
effect_param_t kLevParams[P_COUNT];

void build_params(void)
{
    for (int i = 0; i < P_COUNT; ++i) {
        const Leveller::ParamInfo &pi = Leveller::paramInfo(kEngineParam[i]);

        kLevParams[i] = (effect_param_t){
            kNames[i],
            pi.isSwitch ? EP_BOOL : EP_INT,
            (int)pi.minValue,
            (int)pi.maxValue,
            nullptr,
            (pi.unit && pi.unit[0]) ? pi.unit : nullptr,
        };
    }
}

// ---- the registry's view of it -----------------------------------------

void lev_set(int p, int value)
{
    if (p < 0 || p >= P_COUNT) return;

    // The engine clamps to its own table too; this keeps the shadow honest for
    // the UI, which reads the descriptor's range to bound the knob.
    if (value < kLevParams[p].min) value = kLevParams[p].min;
    if (value > kLevParams[p].max) value = kLevParams[p].max;

    g_lev.setParam(kEngineParam[p], (double)value);
}

int lev_get(int p)
{
    if (p < 0 || p >= P_COUNT) return 0;
    return (int)lrint(g_lev.getParam(kEngineParam[p]));
}

// The block hook. Timing lives in effect_process_block(), so it covers the
// conversion either side as well as the engine call - the honest figure
// against the deadline.
void lev_process(int32_t *buf)
{
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i)
        g_scratch[i] = (float)(buf[i] >> 8) * kToFloat;

    g_lev.process(g_scratch, (int)AUDIO_BLOCK_FRAMES);

    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        const int32_t v = (int32_t)(clampf(g_scratch[i]) * kToFixed);
        buf[i] = (int32_t)((uint32_t)v << 8);
    }
}

// No enter(). Where the envelope has got to is state worth keeping, the same
// as everywhere else here: reset() on selection would mean the gain swells
// back up from silence every time you looked at another page.
effect_t kLevEffect = {
    /* name        */ "Leveller",
    /* param_count */ P_COUNT,
    /* params      */ kLevParams,
    /* enter       */ nullptr,
    /* process     */ lev_process,
    /* get         */ lev_get,
    /* set         */ lev_set,
};

} // namespace

// ---- entry points ------------------------------------------------------

void lev_init(void)
{
    build_params();

    g_lev.setSampleRate((double)AUDIO_SAMPLE_RATE);
    g_lev.setChannels(2);

    // The five parameters this build does not publish, put explicitly at the
    // engine's own defaults. The constructor has already done exactly this, so
    // the loop changes nothing today - it is here so that what a hidden
    // parameter holds is a decision this file makes rather than one it
    // inherits, and so an engine whose constructor later comes up somewhere
    // else cannot quietly change what the box sounds like.
    for (int i = 0; i < (int)Leveller::kNumParams; ++i) {
        bool published = false;
        for (int p = 0; p < P_COUNT; ++p)
            if (kEngineParam[p] == i) { published = true; break; }

        if (!published)
            g_lev.setParam(i, Leveller::paramInfo(i).defaultValue);
    }

    // Adopt the parameters just set rather than ramping into them from the
    // constructor's state. Cheap here, and there is no audio yet to click.
    g_lev.reset();

    NOVA_LOG("Leveller: %u bytes per instance, %d of %d params published\n",
             (unsigned)sizeof(Leveller), P_COUNT, (int)Leveller::kNumParams);
    NOVA_LOG("  degree %.0f, thresh %.0f dB, bypass %s | hidden: smooth %s, "
             "attack %.1f ms, release %.0f ms, output %.1f dB, mix %.0f %%\n",
             g_lev.getParam(Leveller::kDegree),
             g_lev.getParam(Leveller::kThreshold),
             g_lev.getParam(Leveller::kBypass) >= 0.5 ? "on" : "off",
             g_lev.getParam(Leveller::kSmooth) >= 0.5 ? "on" : "off",
             g_lev.getParam(Leveller::kAttack),
             g_lev.getParam(Leveller::kRelease),
             g_lev.getParam(Leveller::kOutput),
             g_lev.getParam(Leveller::kMix));
}

static const effect_t *lev_effect(void) { return &kLevEffect; }

// ---- the module ---------------------------------------------------------
//
// One accessor, and it is the whole of this file's public surface. The core
// links against it by the name CMake found in module.cmake; nothing else in
// the firmware declares it, and no header exports anything above.

static const effect_module_t kModule = {
    .name      = "leveller",
    .init      = lev_init,
    .effect    = lev_effect,
    .late_init = NULL,
    .report    = NULL,
    .on_key    = NULL,
    .benchmark = NULL,
};

extern "C" const effect_module_t *leveller_module(void) { return &kModule; }
