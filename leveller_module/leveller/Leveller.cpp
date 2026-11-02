// Leveller.cpp - see Leveller.h for the design rules.
//
// Derived from Audacity's EffectLeveller (ref/Leveller.cpp, ref/Leveller.h).
// The transfer curve and its factor table are unchanged; what is new is the
// real-time packaging: interleaved float processing, an explicit sample-rate
// entry point, continuous (rather than enumerated) parameters, dry/wet, a
// makeup trim, and peak meters.
//
// Four deliberate deviations from the original:
//   * by default the curve drives a gain from a peak envelope rather than
//     shaping each sample (see Leveller.h) - the original's slope changes at
//     |x| = 0.1/0.3/0.5 measure ~10% distortion on loud material, which on
//     bass is plainly audible.  Turn "Smooth" off for the original behaviour.
//   * samples above the top limit came back as +0.99 regardless of sign,
//     which turns a loud negative peak into a positive DC step.  Here the
//     sign is carried through: -0.99 for a negative sample.
//   * the boost is dry/wet-mixed and trimmed with smoothed gains, so moving a
//     control while audio is running does not click.
//   * the audio path is float throughout.  See below.
//
// ---- why the audio path is float, and what that cost to find out ---------
//
// The first version of this file kept the original's doubles for the envelope,
// the two parameter ramps, the curve division and the meters.  On a desktop
// host that is free.  On a Cortex-M33 - single-precision FPU, no double unit -
// it measured **22% of a 1.33 ms block at 48 kHz**, dearer than a three-band
// loudness effect doing far more work.
//
// The reason was not the algorithm.  objdump found 56 double call sites in
// process(), and roughly 56 of them ran per frame: 14 in the two ramps, 10 in
// the envelope, ~20 in the segment tests (a float sample compared against a
// double limit is a conversion *and* a comparison, six times a pass), 3 for
// the divide, 6 converting back down to float for the audio loop, 6 in the
// meters.  Measured against the cycle count that is ~25 cycles each, which is
// what a soft-float call costs.  Every one of those was in the *control* path,
// computed at double width and then immediately narrowed to float to be used.
//
// So the rule this file now follows: **double stops at the parameter
// boundary.**  mParams[], dbToLinear()/linearToDb(), and the coefficient
// derivation stay double, because they run when a control moves.  Everything
// the per-frame path reads is float.
//
// Two things to keep in mind if you edit it:
//   * a `static_cast<double>` anywhere in process(), curveGain() or
//     levelOneFrame() is a bug, not a tidy-up.  The build pins
//     -Wdouble-promotion, so an accidental one is a warning rather than a
//     silent 25 cycles.
//   * the envelope coefficients are stored as alpha (1 - exp) rather than as
//     the coefficient, because at these time constants exp() lands within 1e-4
//     of 1.0 and subtracting it from 1.0f per frame would throw away most of
//     the precision the double derivation just produced.

#include "Leveller.h"

#include <math.h>

namespace {

// Segment gains of the original effect.  Index 0 is "below the very bottom",
// index 1 the noise-threshold segment, then the boosted middle, then the
// limited top.  The offsets are derived from these plus the limits, which is
// what keeps the curve continuous.
//
// These two stay double: they are read only by calcLevellerFactors(), which
// runs when the threshold moves, and the recurrence that builds the offsets
// out of them is worth doing at full width before it is stored.
const double kAdjFactor[Leveller::kNumFactors] =
    { 0.80, 1.00, 1.20, 1.20, 1.00, 0.80 };

// Fixed segment boundaries; slot 1 is replaced by the noise threshold.
const double kBaseLimit[Leveller::kNumFactors] =
    { 0.0001, 0.0, 0.1, 0.3, 0.5, 1.0 };

// Anything past the last limit is clamped here (the original's 0.99).
const float kCeiling = 0.99f;

const double kSmoothSeconds = 0.020;   // parameter ramp
const double kMeterSeconds  = 0.300;   // peak-meter release
const double kMinDb         = -120.0;

const Leveller::ParamInfo kParams[Leveller::kNumParams] =
{
    //  name         unit     min      max    default  dec  log    switch
    { "Bypass",      "",       0.0,    1.0,     0.0,   -1, false, true  },
    { "Leveling",    "",       1.0,    5.0,     2.0,    0, false, false },
    { "Threshold",   "dB",   -80.0,  -20.0,   -70.0,    0, false, false },
    { "Smooth",      "",       0.0,    1.0,     1.0,   -1, false, true  },
    { "Attack",      "ms",     0.5,  100.0,    10.0,    1, true,  false },
    { "Release",     "ms",    20.0, 2000.0,   200.0,    0, true,  false },
    { "Output",      "dB",   -12.0,   12.0,     0.0,    1, false, false },
    { "Mix",         "%",      0.0,  100.0,   100.0,    0, false, false },
};

// Below this the envelope is treated as silence: dividing f(env) by it would
// only amplify the noise floor towards the boost ceiling.
const float kEnvFloor = 1.0e-6f;

// A one-pole ramp only approaches its destination, so without this a bypassed
// plug-in would sit forever a hair off the dry signal and the "skip the wet
// path" shortcut below could never fire.  1e-3 of the way out is 60 dB down -
// the step it lands with is inaudible, and after it the dry path is exact.
const float kRampSnap = 1.0e-3f;

float rampTo(float current, float dest, float coef)
{
    float v = dest + (current - dest) * coef;
    const float d = v - dest;
    if (d < kRampSnap && d > -kRampSnap) v = dest;
    return v;
}

double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

double dbToLinear(double db)
{
    return pow(10.0, db / 20.0);
}

double linearToDb(double linear)
{
    if (linear <= 0.0) return kMinDb;
    double db = 20.0 * log10(linear);
    return db < kMinDb ? kMinDb : db;
}

// One-pole coefficient for a time constant, guarding against a host that
// hands over a nonsense rate.  Double, and called only from the two places a
// coefficient can change.
double onePoleCoef(double seconds, double sampleRate)
{
    if (sampleRate < 1.0 || seconds <= 0.0) return 0.0;
    return exp(-1.0 / (seconds * sampleRate));
}

} // namespace

Leveller::Leveller()
{
    for (int i = 0; i < kNumParams; ++i)
        mParams[i] = kParams[i].defaultValue;

    for (int f = 0; f < kNumFactors; ++f) {
        mLimit[f]      = static_cast<float>(kBaseLimit[f]);
        mAdjFactor[f]  = static_cast<float>(kAdjFactor[f]);
        mAddOnValue[f] = 0.0f;
        mAdjLimit[f]   = 0.0f;
    }

    mNumPasses          = static_cast<int>(kParams[kDegree].defaultValue);
    mDbSilenceThreshold = static_cast<float>(dbToLinear(kParams[kThreshold].defaultValue));
    mOutputGain         = static_cast<float>(dbToLinear(kParams[kOutput].defaultValue));
    mMix                = static_cast<float>(kParams[kMix].defaultValue / 100.0);
    mSmoothMode         = kParams[kSmooth].defaultValue >= 0.5;
    mBypass             = kParams[kBypass].defaultValue >= 0.5;
    mAttackMs           = kParams[kAttack].defaultValue;
    mReleaseMs          = kParams[kRelease].defaultValue;

    mSampleRate = 44100.0;
    mChannels   = 2;

    mGainSmooth = mOutputGain;
    mMixSmooth  = mMix;
    mEnvelope   = 0.0f;
    mCurveGain  = 1.0f;
    mInputPeak  = 0.0f;
    mOutputPeak = 0.0f;

    calcLevellerFactors();
    setSampleRate(mSampleRate);
}

const Leveller::ParamInfo &Leveller::paramInfo(int id)
{
    if (id < 0 || id >= kNumParams) id = 0;
    return kParams[id];
}

// --------------------------------------------------------------------------
// Format
// --------------------------------------------------------------------------

void Leveller::setSampleRate(double sampleRate)
{
    if (sampleRate < 1.0) sampleRate = 1.0;
    mSampleRate = sampleRate;

    // The curve itself is memoryless, so a rate change costs only these
    // coefficients - no state is thrown away and audio keeps flowing.
    mSmoothCoef = static_cast<float>(onePoleCoef(kSmoothSeconds, mSampleRate));
    mMeterCoef  = static_cast<float>(onePoleCoef(kMeterSeconds,  mSampleRate));
    calcEnvelopeCoefs();
}

// Attack and release are given in ms, so they have to be re-derived both when
// the user moves them and when the host changes the sample rate.
//
// Stored as alpha - 1 - exp(...) - and not as the coefficient: see Leveller.h.
// The subtraction happens here, in double, once per control move.
void Leveller::calcEnvelopeCoefs()
{
    mAttackAlpha  = static_cast<float>(1.0 - onePoleCoef(mAttackMs  * 0.001, mSampleRate));
    mReleaseAlpha = static_cast<float>(1.0 - onePoleCoef(mReleaseMs * 0.001, mSampleRate));
}

void Leveller::setChannels(int channels)
{
    if (channels < 1) channels = 1;
    if (channels > kMaxChannels) channels = kMaxChannels;
    mChannels = channels;

    mInputPeak  = 0.0f;
    mOutputPeak = 0.0f;
}

void Leveller::reset()
{
    // Adopt the bypass state rather than ramping into it from silence.
    mGainSmooth = mBypass ? 1.0f : mOutputGain;
    mMixSmooth  = mBypass ? 0.0f : mMix;
    mEnvelope   = 0.0f;
    mCurveGain  = curveGain(0.0f);
    mInputPeak  = 0.0f;
    mOutputPeak = 0.0f;
}

// --------------------------------------------------------------------------
// Parameters
// --------------------------------------------------------------------------

void Leveller::setParam(int id, double value)
{
    if (id < 0 || id >= kNumParams) return;

    const ParamInfo &p = kParams[id];
    value = clampd(value, p.minValue, p.maxValue);
    if (p.decimals == 0) value = floor(value + 0.5);
    mParams[id] = value;

    switch (id) {
    case kDegree:
        mNumPasses = static_cast<int>(value);
        if (mNumPasses < 1) mNumPasses = 1;
        break;

    case kThreshold:
        // The threshold has to stay inside the segment it replaces, or the
        // limits stop being monotonic and the curve folds over itself.
        mDbSilenceThreshold =
            static_cast<float>(clampd(dbToLinear(value), kBaseLimit[0], kBaseLimit[2]));
        calcLevellerFactors();
        break;

    case kBypass:
        // Only the target moves; process() ramps to it, so the switch is
        // click-free even in the middle of a loud passage.
        mBypass = value >= 0.5;
        break;

    case kSmooth:
        mSmoothMode = value >= 0.5;
        break;

    case kAttack:
        mAttackMs = value;
        calcEnvelopeCoefs();
        break;

    case kRelease:
        mReleaseMs = value;
        calcEnvelopeCoefs();
        break;

    case kOutput:
        mOutputGain = static_cast<float>(dbToLinear(value));
        break;

    case kMix:
        mMix = static_cast<float>(value / 100.0);
        break;

    default:
        break;
    }
}

double Leveller::getParam(int id) const
{
    if (id < 0 || id >= kNumParams) return 0.0;
    return mParams[id];
}

// --------------------------------------------------------------------------
// Metering
// --------------------------------------------------------------------------
//
// The meters are read by a UI, not by the audio path, so widening here costs
// nothing and keeps the dB conversion where all the others are.

double Leveller::inputPeakDb()  const { return linearToDb(static_cast<double>(mInputPeak)); }
double Leveller::outputPeakDb() const { return linearToDb(static_cast<double>(mOutputPeak)); }

double Leveller::gainDb() const
{
    if (mInputPeak <= 0.0f || mOutputPeak <= 0.0f) return 0.0;
    return linearToDb(static_cast<double>(mOutputPeak) / static_cast<double>(mInputPeak));
}

// --------------------------------------------------------------------------
// Audio
// --------------------------------------------------------------------------
//
// Nothing below this line is double, and nothing below it should become
// double. See the note at the top of the file.

void Leveller::process(float *samples, int frames)
{
    if (!samples || frames <= 0) return;

    const int nch    = mChannels;
    const int passes = mNumPasses;

    // Bypass is not a branch around the loop but a destination for the two
    // ramps that are already there: mix falls to fully dry and the output trim
    // to unity over ~20 ms, so switching it mid-track cannot click.  Once the
    // ramp arrives the wet multiply is skipped.
    //
    // It is not a way to make the effect free, and on a target where that
    // matters the host should stop calling process() instead: the envelope
    // follower below runs either way, which is what stops the gain swelling
    // when bypass comes back off.
    const float gainDest = mBypass ? 1.0f : mOutputGain;
    const float mixDest  = mBypass ? 0.0f : mMix;
    const float sc       = mSmoothCoef;
    const float mc       = mMeterCoef;

    const bool  smooth = mSmoothMode;
    const float aa     = mAttackAlpha;
    const float ra     = mReleaseAlpha;

    float inPeak  = mInputPeak;
    float outPeak = mOutputPeak;
    float gain    = mGainSmooth;
    float mix     = mMixSmooth;
    float env     = mEnvelope;
    float curve   = mCurveGain;

    for (int f = 0; f < frames; ++f) {
        // One-pole ramp towards the target, so slider moves are inaudible.
        gain = rampTo(gain, gainDest, sc);
        mix  = rampTo(mix,  mixDest,  sc);

        float *frameStart = samples + static_cast<size_t>(f) * nch;

        // Smooth mode: the curve reads the envelope, not the sample, and what
        // it produces is a gain shared by every channel - so the waveform
        // keeps its shape (no added harmonics) and the stereo image holds
        // still.  The envelope is the peak across channels for the same
        // reason: per-channel gains would wander apart on wide material.
        // This runs even while bypassed, so the follower is already tracking
        // the music when bypass comes off - no swell on the way back in.
        if (smooth) {
            float frameMax = 0.0f;
            for (int c = 0; c < nch; ++c) {
                const float v = frameStart[c];
                const float a = v < 0.0f ? -v : v;
                if (a > frameMax) frameMax = a;
            }

            env += (frameMax - env) * (frameMax > env ? aa : ra);
            if (env < 1.0e-25f) env = 0.0f;         // denormal guard

            const float target = curveGain(env);
            curve = target + (curve - target) * sc;
        }

        float frameInMax  = 0.0f;
        float frameOutMax = 0.0f;

        for (int c = 0; c < nch; ++c) {
            const float dry = frameStart[c];

            float wet;
            if (mix == 0.0f) {
                wet = dry;              // fully dry: nothing to compute
            } else if (smooth) {
                wet = dry * curve;
            } else {
                wet = dry;
                for (int p = 0; p < passes; ++p)
                    wet = levelOneFrame(wet);
            }

            float out = dry + (wet - dry) * mix;
            out *= gain;

            // Never hand the host a value that overflows its own conversion,
            // and kill denormals while we are here.
            if (out >  1.0f) out =  1.0f;
            if (out < -1.0f) out = -1.0f;
            if (out > -1.0e-25f && out < 1.0e-25f) out = 0.0f;

            frameStart[c] = out;

            const float ain  = dry < 0.0f ? -dry : dry;
            const float aout = out < 0.0f ? -out : out;
            if (ain  > frameInMax)  frameInMax  = ain;
            if (aout > frameOutMax) frameOutMax = aout;
        }

        // Instant attack, exponential release - normal peak-meter ballistics.
        inPeak  *= mc;
        outPeak *= mc;
        if (frameInMax  > inPeak)  inPeak  = frameInMax;
        if (frameOutMax > outPeak) outPeak = frameOutMax;
    }

    if (inPeak  < 1.0e-30f) inPeak  = 0.0f;
    if (outPeak < 1.0e-30f) outPeak = 0.0f;

    mInputPeak  = inPeak;
    mOutputPeak = outPeak;
    mGainSmooth = gain;
    mMixSmooth  = mix;
    mEnvelope   = env;
    mCurveGain  = curve;
}

// What the original curve would do to a signal sitting at `envelope`,
// expressed as a gain.  Multiplying by this reproduces the curve's level
// riding without reproducing its corners.
float Leveller::curveGain(float envelope) const
{
    float e = envelope;
    if (e < kEnvFloor) e = kEnvFloor;

    float y = e;
    for (int p = 0; p < mNumPasses; ++p)
        y = levelOneFrame(y);

    return y / e;
}

// --------------------------------------------------------------------------
// The curve (unchanged from the original effect)
// --------------------------------------------------------------------------

// Built in double and stored float: this runs when the threshold moves, and
// the offsets are a recurrence - each one is derived from the one before it,
// so the width is worth having while it is being built even though nothing
// reads it at that width afterwards.
void Leveller::calcLevellerFactors()
{
    double limit[kNumFactors];
    double adjFactor[kNumFactors];
    double addOnValue[kNumFactors];
    double adjLimit[kNumFactors];

    for (int f = 0; f < kNumFactors; ++f) {
        limit[f]     = kBaseLimit[f];
        adjFactor[f] = kAdjFactor[f];
    }
    limit[1] = static_cast<double>(mDbSilenceThreshold);

    addOnValue[0] = 0.0;
    adjLimit[0]   = limit[0] * adjFactor[0];

    for (int f = 1; f < kNumFactors; ++f) {
        const int    prev         = f - 1;
        const double adj          = adjFactor[f];
        const double prevAdjLimit = adjLimit[prev];

        addOnValue[f] = prevAdjLimit - (adj * limit[prev]);
        adjLimit[f]   = (adj * limit[f]) + addOnValue[f];
    }

    for (int f = 0; f < kNumFactors; ++f) {
        mLimit[f]      = static_cast<float>(limit[f]);
        mAdjFactor[f]  = static_cast<float>(adjFactor[f]);
        mAddOnValue[f] = static_cast<float>(addOnValue[f]);
        mAdjLimit[f]   = static_cast<float>(adjLimit[f]);
    }
}

float Leveller::levelOneFrame(float frameInBuffer) const
{
    const float curSign      = frameInBuffer < 0.0f ? -1.0f : 1.0f;
    const float fabsCurFrame = frameInBuffer < 0.0f ? -frameInBuffer : frameInBuffer;

    for (int f = 0; f < kNumFactors; ++f) {
        if (fabsCurFrame <= mLimit[f]) {
            float out = frameInBuffer * mAdjFactor[f];
            out += mAddOnValue[f] * curSign;
            return out;
        }
    }
    // Past the top limit: the original returned +0.99 even for a negative
    // sample; carry the sign so hot material limits instead of flipping.
    return kCeiling * curSign;
}
