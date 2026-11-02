// Leveller.h - Audacity's "Leveller" effect (a static, memoryless soft
// compressor/limiter curve) repackaged as a host-agnostic real-time class.
//
// Design rules, so it can drop into an embedded target as-is:
//   * no malloc / new / free / delete anywhere - every buffer is a fixed
//     member array, so one Leveller instance is a plain lump of .bss,
//   * process() takes interleaved float, in place, any frame count,
//   * no C++ exceptions, no RTTI, no static initialisation order tricks,
//   * setSampleRate() is separate from setChannels() and from the parameter
//     setters, because a host (Winamp does exactly this) can hand you a new
//     sample rate on any callback and only the rate-derived state should be
//     re-derived when it does,
//   * **every value the audio path touches is float.**  The curve table, the
//     envelope, the two parameter ramps and the peak meters are all single
//     precision; double survives only where a control moves - mParams[], the
//     dB conversions and the coefficient derivation - which happens when a
//     slider moves rather than per frame.  On a desktop host that choice is
//     free either way.  On a Cortex-M33, which has no double unit, it is the
//     difference between an effect that fits the budget and one that does not:
//     see the note at the top of Leveller.cpp.
//
// The effect itself: |x| is compared against six limits, and the segment it
// lands in applies its own gain plus an offset that keeps the curve
// continuous.  The middle segments have gain > 1, the outer ones < 1, so
// quiet material is lifted and loud material is held back.
//
// Applied the way Audacity applies it - per sample, straight to the waveform -
// that curve is a waveshaper, and its slope changes at |x| = 0.1, 0.3 and 0.5
// put a kink in every waveform that crosses them.  Loud bass crosses all three
// twice per cycle with a lot of energy behind it, so the kinks come back as
// audible harmonics: measured at ~10% of the output at -1 dBFS.
//
// So there are two modes (kSmooth):
//   * smooth (default) runs the curve on a peak envelope instead and applies
//     the resulting *gain* to the samples.  Same level-riding behaviour, but
//     the gain now moves over milliseconds rather than sample to sample, which
//     is what keeps the harmonics out.  Distortion drops to ~0.1%.
//   * classic is the original per-sample waveshaper, kept because its grit is
//     the sound some people are after.
//
// Either way there is no lookahead and no delay line - one float of envelope
// state per instance - which is what keeps it cheap enough for a
// microcontroller.

#ifndef LEVELLER_H
#define LEVELLER_H

class Leveller
{
public:
    enum { kMaxChannels = 8 };

    // Number of segments in the transfer curve (from the original effect).
    enum { kNumFactors = 6 };

    // The table in Leveller.cpp is the authority on range and default; the
    // values repeated here are a reading aid, so keep the two in step.
    //
    //   id           range            default   meaning
    //   kBypass      off / on         off       hand the input straight back
    //   kDegree      1 .. 5           2         times the curve is applied
    //   kThreshold   -80 .. -20 dB    -70 dB    bottom of the boosted region
    //   kSmooth      off / on         on        envelope gain vs. waveshaper
    //   kAttack      0.5 .. 100 ms    10 ms     envelope attack, smooth only
    //   kRelease     20 .. 2000 ms    200 ms    envelope release, smooth only
    //   kOutput      -12 .. +12 dB    0 dB      makeup / trim
    //   kMix         0 .. 100 %       100 %     dry/wet
    //
    // Defaults are what a fresh instance starts with, so a host that never
    // calls setParam() gets 2-pass smooth leveling at unity with no bypass.
    enum ParamId
    {
        kBypass = 0,   // 1 = hand the input straight back        (default off)
        kDegree,       // how many times the curve is applied     (default 2)
        kThreshold,    // "noise threshold", dB                   (default -70)
        kSmooth,       // 1 = envelope gain, 0 = waveshaper       (default on)
        kAttack,       // envelope attack, ms, smooth mode only   (default 10)
        kRelease,      // envelope release, ms, smooth mode only  (default 200)
        kOutput,       // makeup / trim, dB                       (default 0)
        kMix,          // dry/wet, percent                        (default 100)
        kNumParams
    };

    struct ParamInfo
    {
        const char *name;
        const char *unit;
        double minValue;
        double maxValue;
        double defaultValue;
        int    decimals;    // digits shown; -1 means render as on/off
        bool   isLog;       // slider maps logarithmically
        bool   isSwitch;    // render as a check box
    };

    Leveller();

    // Parameter table - static, so a host can build its UI without an instance.
    static const ParamInfo &paramInfo(int id);

    // --- format ------------------------------------------------------------
    // Both are safe to call from the audio thread between blocks and are
    // deliberately independent: a track change to a new rate must not disturb
    // the channel setup, and vice versa.
    //
    // The rate stays double because that is what hosts hand over, and because
    // it is only ever divided into a time constant here - never multiplied by
    // a sample.
    void setSampleRate(double sampleRate);
    void setChannels(int channels);
    void reset();                       // clears meters and smoothing state

    double sampleRate() const { return mSampleRate; }
    int    channels()   const { return mChannels; }

    // --- parameters --------------------------------------------------------
    // The parameter boundary is double on purpose: it is the one place a host
    // hands values in, nothing here runs per frame, and keeping it wide means
    // getParam() gives back exactly what was set rather than a rounded copy.
    void   setParam(int id, double value);
    double getParam(int id) const;

    // --- audio -------------------------------------------------------------
    // `samples` holds `frames * channels()` interleaved floats, nominally
    // -1..+1, and is overwritten in place.
    void process(float *samples, int frames);

    // --- metering (audio thread writes, UI thread reads; approximate) ------
    double inputPeakDb()  const;
    double outputPeakDb() const;

    // Effective boost the curve is currently applying, in dB.
    double gainDb() const;

private:
    void   calcLevellerFactors();
    void   calcEnvelopeCoefs();
    float  levelOneFrame(float frame) const;
    float  curveGain(float envelope) const;    // f(env) / env, for smooth mode

    // Transfer curve, rebuilt whenever the threshold moves. Float because
    // levelOneFrame() compares a float sample against mLimit[] six times per
    // pass - that comparison is the single hottest thing in the class, and at
    // mixed width every one of them is a conversion as well.
    float mLimit[kNumFactors];
    float mAdjFactor[kNumFactors];
    float mAddOnValue[kNumFactors];
    float mAdjLimit[kNumFactors];

    // Parameters, as handed in by the host.
    double mParams[kNumParams];

    // Derived from parameters. The two that reach the audio path are float;
    // the millisecond times are not, because only calcEnvelopeCoefs() reads
    // them and it runs when a control moves.
    int    mNumPasses;
    float  mDbSilenceThreshold;
    float  mOutputGain;     // linear
    float  mMix;            // 0..1
    bool   mSmoothMode;
    bool   mBypass;
    double mAttackMs;
    double mReleaseMs;

    // Format.
    double mSampleRate;
    int    mChannels;

    // Coefficients derived from the sample rate (and, for the envelope pair,
    // from the attack/release times). Derived in double by calcEnvelopeCoefs()
    // and stored float, so the width is paid once per control move.
    // **The envelope pair is stored as alpha, not as the coefficient** -
    // 1 - exp(...) rather than exp(...) - because at these time constants exp()
    // lands within 1e-4 of 1.0, so computing 1.0f - coef per frame would cost
    // an operation and throw away most of the precision the double derivation
    // just produced.
    //
    // **That is a precision-of-derivation argument and nothing more.** It is
    // worth saying because the obvious next thought - that the alpha form also
    // makes the one-pole itself better conditioned - was tried and measured and
    // is false. A one-pole in float stalls once the correction rounds away, at
    // 0.5 * ulp(value) / (1 - coef) short of its destination, and that band is a
    // property of the recurrence rather than of how it is written: rewriting
    // mSmoothCoef in alpha form changed the host A/B by nothing at all. See the
    // derivation at kTolerance in test_leveller.cpp, which is also where the
    // 0.0005 dB that band is worth on the shipped settings is worked out.
    //
    // mMeterCoef stays a coefficient because it is a decay towards zero rather
    // than a track towards a destination, and nothing reads it but the meters.
    float mSmoothCoef;
    float mMeterCoef;
    float mAttackAlpha;
    float mReleaseAlpha;

    // Running state.
    float mGainSmooth;
    float mMixSmooth;
    float mEnvelope;        // peak follower, smooth mode
    float mCurveGain;       // ramped output of the curve, smooth mode
    float mInputPeak;
    float mOutputPeak;
};

#endif // LEVELLER_H
