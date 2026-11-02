// test_leveller.cpp - host test for Leveller, and the A/B that the float
// conversion needed.
//
// **Not in the build.** Like agc/test_autogain.cpp and
// noisegate/test_noisegate.cpp this is a host program with no place in a
// firmware image, so it is deliberately absent from leveller_module's
// module.cmake SOURCES. Its build command is here and nowhere else:
//
//   "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
//   cl /nologo /O2 /EHsc /W4 /std:c++17 test_leveller.cpp Leveller.cpp
//   test_leveller.exe
//
// or, with a GCC to hand:
//
//   g++ -O2 -Wall -Wextra -o test_leveller test_leveller.cpp Leveller.cpp
//
// ---- what it is for -----------------------------------------------------
//
// The engine's audio path was converted from double to float because the
// double version measured 22% of the block deadline on the RP2350, which has
// no double-precision unit (see ../README.md). A conversion like that is
// exactly the kind of change that compiles clean, links clean, and is wrong
// in a way you only hear - so the first half of this file is a **reference
// implementation of the engine as it was before the conversion**, doubles and
// all, and the checks below run the two side by side on the same input and
// compare sample for sample.
//
// RefLeveller is therefore not "a simpler model of the effect". It is the old
// code, kept verbatim apart from the renaming, and it should be left that way:
// the moment it is tidied to match the new version it stops being able to
// catch anything.
//
// The second half checks properties that hold regardless of width - curve
// continuity, bypass convergence, the denormal guards, parameter clamping -
// because a conversion can also be arithmetically faithful and still have
// broken an invariant that the double version happened to satisfy.

#include "Leveller.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// ==========================================================================
//  RefLeveller - the pre-conversion engine, doubles and all.
// ==========================================================================

namespace ref {

const double kAdjFactor[6] = { 0.80, 1.00, 1.20, 1.20, 1.00, 0.80 };
const double kBaseLimit[6] = { 0.0001, 0.0, 0.1, 0.3, 0.5, 1.0 };
const double kCeiling      = 0.99;
const double kSmoothSeconds = 0.020;
const double kMeterSeconds  = 0.300;
const double kEnvFloor      = 1.0e-6;
const double kRampSnap      = 1.0e-3;

double rampTo(double current, double dest, double coef)
{
    double v = dest + (current - dest) * coef;
    const double d = v - dest;
    if (d < kRampSnap && d > -kRampSnap) v = dest;
    return v;
}

double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

double dbToLinear(double db) { return pow(10.0, db / 20.0); }

double onePoleCoef(double seconds, double sampleRate)
{
    if (sampleRate < 1.0 || seconds <= 0.0) return 0.0;
    return exp(-1.0 / (seconds * sampleRate));
}

class RefLeveller
{
public:
    enum { kNumFactors = 6 };

    RefLeveller()
    {
        for (int i = 0; i < Leveller::kNumParams; ++i)
            mParams[i] = Leveller::paramInfo(i).defaultValue;

        for (int f = 0; f < kNumFactors; ++f) {
            mLimit[f]      = kBaseLimit[f];
            mAdjFactor[f]  = kAdjFactor[f];
            mAddOnValue[f] = 0.0;
            mAdjLimit[f]   = 0.0;
        }

        mNumPasses          = (int)mParams[Leveller::kDegree];
        mDbSilenceThreshold = dbToLinear(mParams[Leveller::kThreshold]);
        mOutputGain         = dbToLinear(mParams[Leveller::kOutput]);
        mMix                = mParams[Leveller::kMix] / 100.0;
        mSmoothMode         = mParams[Leveller::kSmooth] >= 0.5;
        mBypass             = mParams[Leveller::kBypass] >= 0.5;
        mAttackMs           = mParams[Leveller::kAttack];
        mReleaseMs          = mParams[Leveller::kRelease];

        mSampleRate = 44100.0;
        mChannels   = 2;

        mGainSmooth = mOutputGain;
        mMixSmooth  = mMix;
        mEnvelope   = 0.0;
        mCurveGain  = 1.0;
        mInputPeak  = 0.0;
        mOutputPeak = 0.0;

        calcLevellerFactors();
        setSampleRate(mSampleRate);
    }

    void setSampleRate(double sampleRate)
    {
        if (sampleRate < 1.0) sampleRate = 1.0;
        mSampleRate = sampleRate;
        mSmoothCoef = onePoleCoef(kSmoothSeconds, mSampleRate);
        mMeterCoef  = onePoleCoef(kMeterSeconds,  mSampleRate);
        calcEnvelopeCoefs();
    }

    void calcEnvelopeCoefs()
    {
        mAttackCoef  = onePoleCoef(mAttackMs  * 0.001, mSampleRate);
        mReleaseCoef = onePoleCoef(mReleaseMs * 0.001, mSampleRate);
    }

    void setChannels(int channels)
    {
        if (channels < 1) channels = 1;
        if (channels > 8) channels = 8;
        mChannels   = channels;
        mInputPeak  = 0.0;
        mOutputPeak = 0.0;
    }

    void reset()
    {
        mGainSmooth = mBypass ? 1.0 : mOutputGain;
        mMixSmooth  = mBypass ? 0.0 : mMix;
        mEnvelope   = 0.0;
        mCurveGain  = curveGain(0.0);
        mInputPeak  = 0.0;
        mOutputPeak = 0.0;
    }

    void setParam(int id, double value)
    {
        if (id < 0 || id >= Leveller::kNumParams) return;

        const Leveller::ParamInfo &p = Leveller::paramInfo(id);
        value = clampd(value, p.minValue, p.maxValue);
        if (p.decimals == 0) value = floor(value + 0.5);
        mParams[id] = value;

        switch (id) {
        case Leveller::kDegree:
            mNumPasses = (int)value;
            if (mNumPasses < 1) mNumPasses = 1;
            break;
        case Leveller::kThreshold:
            mDbSilenceThreshold = clampd(dbToLinear(value), kBaseLimit[0], kBaseLimit[2]);
            calcLevellerFactors();
            break;
        case Leveller::kBypass: mBypass     = value >= 0.5; break;
        case Leveller::kSmooth: mSmoothMode = value >= 0.5; break;
        case Leveller::kAttack:  mAttackMs  = value; calcEnvelopeCoefs(); break;
        case Leveller::kRelease: mReleaseMs = value; calcEnvelopeCoefs(); break;
        case Leveller::kOutput:  mOutputGain = dbToLinear(value); break;
        case Leveller::kMix:     mMix = value / 100.0; break;
        default: break;
        }
    }

    double getParam(int id) const
    {
        if (id < 0 || id >= Leveller::kNumParams) return 0.0;
        return mParams[id];
    }

    void process(float *samples, int frames)
    {
        if (!samples || frames <= 0) return;

        const int    nch    = mChannels;
        const int    passes = mNumPasses;

        const double gainDest = mBypass ? 1.0 : mOutputGain;
        const double mixDest  = mBypass ? 0.0 : mMix;
        const double sc       = mSmoothCoef;
        const double mc       = mMeterCoef;

        const bool   smooth = mSmoothMode;
        const double ac     = mAttackCoef;
        const double rc     = mReleaseCoef;

        double inPeak  = mInputPeak;
        double outPeak = mOutputPeak;
        double gain    = mGainSmooth;
        double mix     = mMixSmooth;
        double env     = mEnvelope;
        double curve   = mCurveGain;

        for (int f = 0; f < frames; ++f) {
            gain = rampTo(gain, gainDest, sc);
            mix  = rampTo(mix,  mixDest,  sc);

            float *frameStart = samples + (size_t)f * nch;

            if (smooth) {
                double frameMax = 0.0;
                for (int c = 0; c < nch; ++c) {
                    const float v = frameStart[c];
                    const double a = (double)(v < 0.0f ? -v : v);
                    if (a > frameMax) frameMax = a;
                }

                env += (frameMax - env) * (1.0 - (frameMax > env ? ac : rc));
                if (env < 1.0e-25) env = 0.0;

                const double target = curveGain(env);
                curve = target + (curve - target) * sc;
            }

            const float gainF  = (float)gain;
            const float mixF   = (float)mix;
            const float curveF = (float)curve;

            float frameInMax  = 0.0f;
            float frameOutMax = 0.0f;

            for (int c = 0; c < nch; ++c) {
                const float dry = frameStart[c];

                float wet;
                if (mixF == 0.0f) {
                    wet = dry;
                } else if (smooth) {
                    wet = dry * curveF;
                } else {
                    wet = dry;
                    for (int p = 0; p < passes; ++p)
                        wet = levelOneFrame(wet);
                }

                float out = dry + (wet - dry) * mixF;
                out *= gainF;

                if (out >  1.0f) out =  1.0f;
                if (out < -1.0f) out = -1.0f;
                if (out > -1.0e-25f && out < 1.0e-25f) out = 0.0f;

                frameStart[c] = out;

                const float ain  = dry < 0.0f ? -dry : dry;
                const float aout = out < 0.0f ? -out : out;
                if (ain  > frameInMax)  frameInMax  = ain;
                if (aout > frameOutMax) frameOutMax = aout;
            }

            inPeak  *= mc;
            outPeak *= mc;
            if ((double)frameInMax  > inPeak)  inPeak  = frameInMax;
            if ((double)frameOutMax > outPeak) outPeak = frameOutMax;
        }

        if (inPeak  < 1.0e-30) inPeak  = 0.0;
        if (outPeak < 1.0e-30) outPeak = 0.0;

        mInputPeak  = inPeak;
        mOutputPeak = outPeak;
        mGainSmooth = gain;
        mMixSmooth  = mix;
        mEnvelope   = env;
        mCurveGain  = curve;
    }

private:
    double curveGain(double envelope) const
    {
        double e = envelope;
        if (e < kEnvFloor) e = kEnvFloor;

        float y = (float)e;
        for (int p = 0; p < mNumPasses; ++p)
            y = levelOneFrame(y);

        return (double)y / e;
    }

    void calcLevellerFactors()
    {
        for (int f = 0; f < kNumFactors; ++f) {
            mLimit[f]     = kBaseLimit[f];
            mAdjFactor[f] = kAdjFactor[f];
        }
        mLimit[1] = mDbSilenceThreshold;

        mAddOnValue[0] = 0.0;
        mAdjLimit[0]   = mLimit[0] * mAdjFactor[0];

        for (int f = 1; f < kNumFactors; ++f) {
            const int    prev         = f - 1;
            const double adjFactor    = mAdjFactor[f];
            const double prevAdjLimit = mAdjLimit[prev];
            const double addOnValue   = prevAdjLimit - (adjFactor * mLimit[prev]);

            mAddOnValue[f] = addOnValue;
            mAdjLimit[f]   = (adjFactor * mLimit[f]) + addOnValue;
        }
    }

    float levelOneFrame(float frameInBuffer) const
    {
        const float curSign      = frameInBuffer < 0.0f ? -1.0f : 1.0f;
        const float fabsCurFrame = frameInBuffer < 0.0f ? -frameInBuffer : frameInBuffer;

        for (int f = 0; f < kNumFactors; ++f) {
            if ((double)fabsCurFrame <= mLimit[f]) {
                float out = frameInBuffer * (float)mAdjFactor[f];
                out += (float)mAddOnValue[f] * curSign;
                return out;
            }
        }
        return (float)kCeiling * curSign;
    }

    double mLimit[kNumFactors];
    double mAdjFactor[kNumFactors];
    double mAddOnValue[kNumFactors];
    double mAdjLimit[kNumFactors];
    double mParams[Leveller::kNumParams];
    int    mNumPasses;
    double mDbSilenceThreshold;
    double mOutputGain;
    double mMix;
    bool   mSmoothMode;
    bool   mBypass;
    double mAttackMs;
    double mReleaseMs;
    double mSampleRate;
    int    mChannels;
    double mSmoothCoef;
    double mMeterCoef;
    double mAttackCoef;
    double mReleaseCoef;
    double mGainSmooth;
    double mMixSmooth;
    double mEnvelope;
    double mCurveGain;
    double mInputPeak;
    double mOutputPeak;
};

} // namespace ref

// ==========================================================================
//  Harness
// ==========================================================================

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) {
        ++g_failed;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

const double kFs      = 48000.0;
const int    kBlock   = 64;      // AUDIO_BLOCK_FRAMES on the target
const int    kChans   = 2;

// The tolerance the A/B is judged against, as an absolute error on a signal
// that lives in -1..+1.
//
// **It is derived, and the derivation was wrong twice before it was right** -
// which is why the two "locating the divergence" groups below exist. They are
// the experiment, kept in the suite so the next person does not repeat it.
//
// What the divergence actually is: the smooth-mode gain is smoothed by a 20 ms
// one-pole, and a one-pole in float has a **stall band**. Once `curve` is close
// enough to `target` that the correction rounds away, it stops moving - and
// that happens while it is still
//
//     0.5 * ulp(curve) / (1 - coef)
//
// short of where it should be. For a gain around 1.44, ulp is 1.19e-7 and
// 1 - coef is 1.0416e-3, so the band is **5.72e-5** of gain. Measured: the
// float engine sits 5.67e-5 (relative) from the double one, right across the
// levels where the ramp is tracking. The prediction and the measurement agree
// to 1%, which is the whole reason to believe the explanation.
//
// As an error that is 5.7e-5 of gain, or **0.0005 dB**. It is a gain offset
// rather than added noise, it does not depend on level, and it is four orders
// of magnitude below audible. Not worth a cycle to chase.
//
// So the bound is 5.72e-5 of full scale, and 1e-4 is that rounded up. Observed
// worst case anywhere in the suite is 1.425e-5, on a -12 dBFS tone.
//
// Two explanations that were tried and are wrong, both refuted by the groups
// below rather than by argument:
//
//   * **"it is the release follower."** The follower is a leaky integrator too,
//     and its 200 ms release amplifies rounding by 9600, so it looked like the
//     obvious culprit. It is not: 20 ms and 2000 ms release give the same
//     answer to within 5%.
//   * **"the ramp is in the wrong algebraic form."** Rewriting
//     `dest + (current - dest) * coef` as `current + (dest - current) * alpha`
//     produced a result identical to the last digit. Both forms have the same
//     stall band, because the band is a property of the recurrence and not of
//     how it is spelled. That experiment is not in the tree; this is what is
//     left of it.
const double kTolerance = 1.0e-4;

double dbfs(double x) { return x > 0.0 ? 20.0 * log10(x) : -999.0; }

// A deterministic noise source, so a failure is reproducible.
struct Lcg {
    unsigned s;
    explicit Lcg(unsigned seed) : s(seed) {}
    float next() {                       // -1..+1
        s = s * 1664525u + 1013904223u;
        return (float)((double)(s >> 8) / 8388608.0 - 1.0);
    }
};

struct Result {
    double maxAbs;
    double rms;
    int    nonFinite;
};

// Run both engines over the same signal, block by block at the target's block
// size, and compare. `fill` writes one block of interleaved stereo.
template <typename Fill>
Result compare(int blocks, Fill fill,
               void (*setup)(Leveller &, ref::RefLeveller &))
{
    Leveller         a;
    ref::RefLeveller b;

    a.setSampleRate(kFs); a.setChannels(kChans);
    b.setSampleRate(kFs); b.setChannels(kChans);
    if (setup) setup(a, b);
    a.reset();
    b.reset();

    Result r = { 0.0, 0.0, 0 };
    double sum = 0.0;
    long   n   = 0;

    for (int blk = 0; blk < blocks; ++blk) {
        float src[kBlock * kChans];
        float ba[kBlock * kChans];
        float bb[kBlock * kChans];

        fill(blk, src);
        memcpy(ba, src, sizeof src);
        memcpy(bb, src, sizeof src);

        a.process(ba, kBlock);
        b.process(bb, kBlock);

        for (int i = 0; i < kBlock * kChans; ++i) {
            if (!(ba[i] == ba[i]) || ba[i] > 2.0f || ba[i] < -2.0f) ++r.nonFinite;

            const double d = fabs((double)ba[i] - (double)bb[i]);
            if (d > r.maxAbs) r.maxAbs = d;
            sum += d * d;
            ++n;
        }
    }

    r.rms = n ? sqrt(sum / (double)n) : 0.0;
    return r;
}

void report(const char *name, const Result &r)
{
    char line[256];
    snprintf(line, sizeof line,
             "%-34s max %.3e (%7.1f dBFS)  rms %.3e", name, r.maxAbs,
             dbfs(r.maxAbs), r.rms);
    check(r.maxAbs <= kTolerance && r.nonFinite == 0, line);
}

// ---- signals -------------------------------------------------------------

struct Sine {
    double amp, hz, phase;
    Sine(double a, double f) : amp(a), hz(f), phase(0.0) {}
    void operator()(int, float *out) {
        for (int f = 0; f < kBlock; ++f) {
            const float v = (float)(amp * sin(phase));
            out[f * 2 + 0] = v;
            out[f * 2 + 1] = v;
            phase += 2.0 * 3.14159265358979323846 * hz / kFs;
        }
    }
};

// Stereo with different content per channel, which is what makes the
// across-channel peak in the envelope do something.
struct WideNoise {
    Lcg l, r;
    double amp;
    explicit WideNoise(double a) : l(1u), r(99991u), amp(a) {}
    void operator()(int, float *out) {
        for (int f = 0; f < kBlock; ++f) {
            out[f * 2 + 0] = (float)(amp * l.next());
            out[f * 2 + 1] = (float)(amp * r.next() * 0.4);
        }
    }
};

// Quiet, then loud, then quiet: the shape that exercises attack, release and
// the curve ramp rather than a steady state they all settle into.
struct Steps {
    Sine s;
    Steps() : s(1.0, 1000.0) {}
    void operator()(int blk, float *out) {
        const double amp = (blk < 100) ? 0.002 : (blk < 300 ? 0.7 : 0.01);
        s.amp = amp;
        s(blk, out);
    }
};

// A slow sweep from silence to full scale, so every sample sits somewhere
// different on the curve and the segment boundaries (0.1, 0.3, 0.5) are all
// crossed, in both directions, with the envelope tracking.
struct Ramp {
    void operator()(int blk, float *out) {
        for (int f = 0; f < kBlock; ++f) {
            const double t = (double)(blk * kBlock + f) / (400.0 * kBlock);
            const double a = t < 1.0 ? t : 2.0 - t;
            const float  v = (float)(a * sin((double)(blk * kBlock + f) * 0.05));
            out[f * 2 + 0] = v;
            out[f * 2 + 1] = -v;
        }
    }
};

} // namespace

// ==========================================================================
//  Checks
// ==========================================================================

static void set_degree(Leveller &a, ref::RefLeveller &b, int d)
{
    a.setParam(Leveller::kDegree, d);
    b.setParam(Leveller::kDegree, d);
}

static int g_degree = 2;
static void setup_degree(Leveller &a, ref::RefLeveller &b) { set_degree(a, b, g_degree); }

static void setup_classic(Leveller &a, ref::RefLeveller &b)
{
    a.setParam(Leveller::kSmooth, 0.0);
    b.setParam(Leveller::kSmooth, 0.0);
}

static void setup_hot_threshold(Leveller &a, ref::RefLeveller &b)
{
    a.setParam(Leveller::kThreshold, -24.0);
    b.setParam(Leveller::kThreshold, -24.0);
}

static void setup_fast(Leveller &a, ref::RefLeveller &b)
{
    a.setParam(Leveller::kAttack, 0.5);   a.setParam(Leveller::kRelease, 20.0);
    b.setParam(Leveller::kAttack, 0.5);   b.setParam(Leveller::kRelease, 20.0);
}

static double g_release = 200.0;
static void setup_release(Leveller &a, ref::RefLeveller &b)
{
    a.setParam(Leveller::kRelease, g_release);
    b.setParam(Leveller::kRelease, g_release);
}

static void setup_mix_trim(Leveller &a, ref::RefLeveller &b)
{
    a.setParam(Leveller::kMix, 40.0);  a.setParam(Leveller::kOutput, -6.0);
    b.setParam(Leveller::kMix, 40.0);  b.setParam(Leveller::kOutput, -6.0);
}

int main()
{
    printf("Leveller: float engine against the pre-conversion double engine\n");
    printf("Fs %.0f, %d frames/block, %d channels, tolerance %.1e (%.0f dBFS)\n\n",
           kFs, kBlock, kChans, kTolerance, dbfs(kTolerance));

    // ---- 1. the A/B, smooth mode (what the firmware ships) ---------------
    printf("smooth mode, Degree 2 (the shipped configuration)\n");
    {
        g_degree = 2;
        report("sine 1k -1 dBFS",  compare(400, Sine(0.891, 1000.0), setup_degree));
        report("sine 1k -12 dBFS", compare(400, Sine(0.251, 1000.0), setup_degree));
        report("sine 1k -30 dBFS", compare(400, Sine(0.0316, 1000.0), setup_degree));
        report("sine 1k -60 dBFS", compare(400, Sine(0.001, 1000.0), setup_degree));
        report("sine 40 Hz -3 dBFS", compare(400, Sine(0.708, 40.0), setup_degree));
        report("stereo noise -6 dBFS", compare(400, WideNoise(0.5), setup_degree));
        report("quiet/loud/quiet steps", compare(500, Steps(), setup_degree));
        report("full-scale sweep", compare(800, Ramp(), setup_degree));
    }

    // ---- 2. every Degree -------------------------------------------------
    printf("\nsmooth mode, Degree 1..5 on the sweep\n");
    for (int d = 1; d <= 5; ++d) {
        g_degree = d;
        char name[64];
        snprintf(name, sizeof name, "Degree %d", d);
        report(name, compare(800, Ramp(), setup_degree));
    }
    g_degree = 2;

    // ---- 3. the paths the panel cannot reach -----------------------------
    //
    // The firmware pins Smooth, Attack, Release, Output and Mix - but the
    // engine still has them, and a conversion that only held for the shipped
    // configuration would be a trap for whoever un-hides one.
    printf("\nconfigurations the firmware does not expose\n");
    report("classic waveshaper (Smooth off)", compare(400, Ramp(), setup_classic));
    report("threshold -24 dB",               compare(400, Ramp(), setup_hot_threshold));
    report("attack 0.5 ms, release 20 ms",   compare(500, Steps(), setup_fast));
    report("mix 40%, output -6 dB",          compare(400, Ramp(), setup_mix_trim));

    // ---- 3b. the two experiments that located the divergence --------------
    //
    // These are not extra coverage. They are the experiment that found out
    // what the difference between the two engines actually is, kept in the
    // suite because the answer is not obvious and two plausible wrong answers
    // came first. See the derivation at kTolerance.
    //
    // Read them like this:
    //
    //   * **the amplitude sweep** should show a roughly *constant relative*
    //     error - 5.7e-5 of whatever the signal is - because the stall band is
    //     a fixed offset on the gain, not something the level scales. The rows
    //     where it collapses (0.282 here) are levels where the ramp happens to
    //     land on its target instead of stalling short of it.
    //   * **the release sweep** should show almost nothing across a 100x
    //     change, because the peak follower is not what is diverging. If this
    //     ever starts tracking release time, the explanation at kTolerance has
    //     stopped being true and wants revisiting.
    printf("\nlocating the divergence: amplitude (1 kHz, Degree 2)\n");
    {
        static const double amps[] = { 0.16, 0.20, 0.224, 0.251, 0.282, 0.35, 0.50 };
        for (double amp : amps) {
            char name[64];
            snprintf(name, sizeof name, "sine 1k at %.3f", amp);
            report(name, compare(400, Sine(amp, 1000.0), setup_degree));
        }
    }

    printf("\nlocating the divergence: release time (the control)\n");
    {
        static const double rels[] = { 20.0, 50.0, 200.0, 800.0, 2000.0 };
        for (double ms : rels) {
            g_release = ms;
            char name[64];
            snprintf(name, sizeof name, "release %6.0f ms", ms);
            report(name, compare(400, Sine(0.251, 1000.0), setup_release));
        }
        g_release = 200.0;
    }

    // ---- 4. invariants that hold at any width ----------------------------
    printf("\ninvariants\n");

    // Bypass must converge to the dry signal exactly, not approximately -
    // that is what the ramp snap is for, and it is the one place a float
    // ramp could plausibly stall a hair off its destination.
    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(kChans);
        lev.setParam(Leveller::kBypass, 1.0);
        lev.reset();

        Sine sig(0.5, 997.0);
        float src[kBlock * kChans], buf[kBlock * kChans];
        bool exact = true;

        for (int blk = 0; blk < 200; ++blk) {
            sig(blk, src);
            memcpy(buf, src, sizeof src);
            lev.process(buf, kBlock);
            if (blk < 100) continue;                 // let the ramp arrive
            for (int i = 0; i < kBlock * kChans; ++i)
                if (buf[i] != src[i]) exact = false;
        }
        check(exact, "bypass is bit-exact once the ramp has settled");
    }

    // Coming off bypass must not swell: the follower runs while bypassed, so
    // the first block back should already be at the settled gain.
    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(kChans);
        lev.setParam(Leveller::kBypass, 1.0);
        lev.reset();

        Sine sig(0.3, 997.0);
        float buf[kBlock * kChans];
        for (int blk = 0; blk < 300; ++blk) { sig(blk, buf); lev.process(buf, kBlock); }

        lev.setParam(Leveller::kBypass, 0.0);
        double firstPeak = 0.0;
        sig(0, buf);
        lev.process(buf, kBlock);
        for (int i = 0; i < kBlock * kChans; ++i)
            if (fabs(buf[i]) > firstPeak) firstPeak = fabs(buf[i]);

        // Ramping in over 20 ms, so one 1.33 ms block gets nowhere near a
        // swell; what is being ruled out is a follower that restarted at zero
        // and is boosting a quiet signal it thinks it is seeing.
        check(firstPeak < 0.45, "no swell on the first block after bypass comes off");
    }

    // The curve has to stay monotonic across the threshold's whole range, or
    // the limits have stopped being ordered and the transfer function has
    // folded over itself.
    {
        bool monotonic = true;
        for (double th = -80.0; th <= -20.0; th += 1.0) {
            Leveller lev;
            lev.setSampleRate(kFs); lev.setChannels(1);
            lev.setParam(Leveller::kSmooth, 0.0);        // waveshaper: f(x) directly
            lev.setParam(Leveller::kThreshold, th);
            lev.reset();

            float prev = -2.0f;
            for (double x = 0.0; x <= 1.0; x += 0.0005) {
                float s = (float)x;
                lev.process(&s, 1);
                if (s < prev - 1.0e-6f) monotonic = false;
                prev = s;
            }
        }
        check(monotonic, "transfer curve is monotonic for every threshold -80..-20");
    }

    // Continuity: the offsets exist to make the segments join. A step at a
    // boundary means the recurrence that builds them is wrong - which is the
    // part that now runs in double and stores float.
    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(1);
        lev.setParam(Leveller::kSmooth, 0.0);
        lev.reset();

        double worst = 0.0;
        float  prev  = 0.0f;
        for (double x = 0.0; x <= 1.0; x += 0.0001) {
            float s = (float)x;
            lev.process(&s, 1);
            const double step = fabs((double)s - (double)prev);
            if (x > 0.0 && step > worst) worst = step;
            prev = s;
        }
        char line[128];
        snprintf(line, sizeof line, "curve is continuous (largest step %.2e)", worst);
        check(worst < 1.0e-3, line);
    }

    // Denormals and silence: the guards are the reason there is no FTZ call
    // in this engine, so they are load-bearing rather than decoration.
    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(kChans);
        lev.reset();

        float buf[kBlock * kChans];
        bool clean = true;
        for (int blk = 0; blk < 200; ++blk) {
            for (int i = 0; i < kBlock * kChans; ++i) buf[i] = 1.0e-30f;
            lev.process(buf, kBlock);
            for (int i = 0; i < kBlock * kChans; ++i)
                if (buf[i] != 0.0f || buf[i] != buf[i]) clean = false;
        }
        check(clean, "denormal input is flushed to exactly zero, no NaN");
    }

    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(kChans);
        lev.reset();

        float buf[kBlock * kChans];
        bool clean = true;
        for (int blk = 0; blk < 500; ++blk) {
            memset(buf, 0, sizeof buf);
            lev.process(buf, kBlock);
            for (int i = 0; i < kBlock * kChans; ++i)
                if (buf[i] != 0.0f) clean = false;
        }
        check(clean, "silence in, silence out for 500 blocks");
        check(lev.inputPeakDb() < -100.0, "meters decay to the floor on silence");
    }

    // Full scale must not leave the rails, in either sign.
    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(kChans);
        lev.reset();

        float buf[kBlock * kChans];
        bool bounded = true;
        for (int blk = 0; blk < 300; ++blk) {
            for (int f = 0; f < kBlock; ++f) {
                buf[f * 2 + 0] =  1.0f;
                buf[f * 2 + 1] = -1.0f;
            }
            lev.process(buf, kBlock);
            for (int i = 0; i < kBlock * kChans; ++i)
                if (buf[i] > 1.0f || buf[i] < -1.0f || buf[i] != buf[i]) bounded = false;
        }
        check(bounded, "DC at full scale stays inside +-1, both signs");
    }

    // The sign fix (deviation two in Leveller.cpp): a loud negative sample
    // must come back negative. The original returned +0.99 for it.
    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(1);
        lev.setParam(Leveller::kSmooth, 0.0);
        lev.reset();

        float s = -1.5f;                      // past the top limit
        lev.process(&s, 1);
        check(s < 0.0f, "a sample past the top limit keeps its sign");
    }

    // Parameters: clamped to the table, and read back as set.
    {
        Leveller lev;
        bool ok = true;
        for (int id = 0; id < Leveller::kNumParams; ++id) {
            const Leveller::ParamInfo &pi = Leveller::paramInfo(id);
            lev.setParam(id, pi.minValue - 1000.0);
            if (lev.getParam(id) != pi.minValue) ok = false;
            lev.setParam(id, pi.maxValue + 1000.0);
            if (lev.getParam(id) != pi.maxValue) ok = false;
            lev.setParam(id, pi.defaultValue);
            if (lev.getParam(id) != pi.defaultValue) ok = false;
        }
        check(ok, "every parameter clamps to its table and round-trips");
    }

    // The parameter boundary is still double, which is the whole reason it was
    // left double: a value set here comes back unrounded.
    {
        Leveller lev;
        lev.setParam(Leveller::kAttack, 33.3);
        check(lev.getParam(Leveller::kAttack) == 33.3,
              "a fractional parameter survives the boundary unrounded");
    }

    // A rate change must not throw state away or produce a dead engine.
    {
        Leveller lev;
        lev.setSampleRate(44100.0); lev.setChannels(kChans);
        lev.reset();

        Sine sig(0.2, 1000.0);
        float buf[kBlock * kChans];
        for (int blk = 0; blk < 100; ++blk) { sig(blk, buf); lev.process(buf, kBlock); }

        lev.setSampleRate(96000.0);
        double peak = 0.0;
        for (int blk = 0; blk < 100; ++blk) {
            sig(blk, buf);
            lev.process(buf, kBlock);
            for (int i = 0; i < kBlock * kChans; ++i)
                if (fabs(buf[i]) > peak) peak = fabs(buf[i]);
        }
        check(peak > 0.1 && peak < 1.01, "audio survives a sample-rate change mid-stream");
    }

    // What the effect is for: quiet material comes up, loud material does not
    // run away. Checked on the float engine alone, because it is a claim about
    // the effect and not about the conversion.
    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(kChans);
        lev.reset();

        Sine sig(0.02, 1000.0);              // ~-34 dBFS
        float buf[kBlock * kChans];
        double peak = 0.0;
        for (int blk = 0; blk < 600; ++blk) {
            sig(blk, buf);
            lev.process(buf, kBlock);
            if (blk > 500)
                for (int i = 0; i < kBlock * kChans; ++i)
                    if (fabs(buf[i]) > peak) peak = fabs(buf[i]);
        }
        check(peak > 0.02, "quiet material is lifted, not left alone");
    }

    {
        Leveller lev;
        lev.setSampleRate(kFs); lev.setChannels(kChans);
        lev.reset();

        Sine sig(0.9, 1000.0);
        float buf[kBlock * kChans];
        double peak = 0.0;
        for (int blk = 0; blk < 600; ++blk) {
            sig(blk, buf);
            lev.process(buf, kBlock);
            if (blk > 500)
                for (int i = 0; i < kBlock * kChans; ++i)
                    if (fabs(buf[i]) > peak) peak = fabs(buf[i]);
        }
        check(peak <= 0.9 + 1.0e-6, "loud material is held back, not boosted");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
