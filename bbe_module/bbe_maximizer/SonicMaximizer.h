// SonicMaximizer.h - BBE Sonic Maximizer style processor, single precision,
// for RP2350 / Pico 2 and other small FPU targets.
//
// This is a port of the desktop engine in ../Maximizer.cpp.  Same effect, same
// parameter meanings, same voicing - but every value is float, there is no
// allocation, and the filter topology was changed for the reasons below.
//
// What the effect does
// --------------------
// The high band is boosted by an amount driven by how much low-band energy is
// masking it, so bass-heavy passages get more lift than sparse ones.  That
// program dependence is what separates the effect from a fixed treble shelf;
// the Dynamics parameter sets how much of the boost is program-driven.  A Lo
// Contour bass shelf and a small complementary mid dip complete the familiar
// two-knob front panel.
//
// Signal flow: LR4 three-way split (150 Hz / 1.2 kHz by default) -> per-band
// shaping -> sum -> output trim -> safety limiter.
//
// Three things differ from the desktop build
// ------------------------------------------
// 1. State-variable filters instead of biquads.  A direct-form biquad keeps
//    its coefficients close to (1, -2, 1) as the cutoff falls, so in float32 a
//    60 Hz corner has almost no significant bits left to describe it - the
//    response drifts and the low band stops summing flat.  The topology-
//    preserving state-variable form used here stores tan(pi*fc/fs) directly
//    and stays well conditioned all the way down.  It also produces lowpass,
//    highpass, allpass, shelf and bell from one core, so the whole chain is a
//    single 20-line routine.
//
// 2. Coefficients live once, not per channel.  Every channel runs the same
//    filters, so only the two integrator states are per channel.  The whole
//    object is well under a kilobyte.
//
// 3. No Lo Delay / Mid Delay.  The desktop build applies BBE's arrival-time
//    correction with an allpass network - about 2.5 ms at 25 Hz tapering to
//    nothing above 2 kHz.  Measured against the Blauert & Laws thresholds for
//    audibility of group delay distortion, that is 6.5x below threshold at
//    500 Hz and 10x below at 1 kHz, i.e. inaudible on its own.  It was costing
//    two filter sections per channel per sample to do nothing a listener can
//    hear, so it is gone.  What is audible - the dynamic HF restoration and
//    the bass shelf - is all still here.  See README.md.
//
// Usage
// -----
//     static SonicMaximizer dsp;
//     dsp.setup(48000.0f, 2);
//     dsp.setParam(SonicMaximizer::kProcess, 6.0f);
//     ...
//     dsp.processInt16(i2s_buffer, frames);   // or process() for float
//
// process() and setParam() are not synchronised.  If a UI task changes
// parameters while an audio task is running, guard the calls (a mutex, or set
// parameters from the audio core between blocks).

#ifndef SONIC_MAXIMIZER_H_INCLUDED
#define SONIC_MAXIMIZER_H_INCLUDED

#include <stdint.h>

// Raise if you feed more than stereo; it costs 88 bytes of state per channel.
#ifndef SONIC_MAXIMIZER_MAX_CHANNELS
#define SONIC_MAXIMIZER_MAX_CHANNELS 2
#endif

class SonicMaximizer
{
public:
    enum { kMaxChannels = SONIC_MAXIMIZER_MAX_CHANNELS };

    // Ranges, defaults, and what each one does.  paramInfo(id) hands back the
    // same numbers at runtime, from the table at the top of
    // SonicMaximizer.cpp; setParam() clamps to them.
    //
    //   name         range            default
    //   kBypass      on / off         off    pass audio through untouched
    //   kProcess     0 .. 10          5.0    HF restoration amount
    //   kLoContour   0 .. 10          3.0    bass shelf amount
    //   kDynamics    0 .. 100 %       60     program-driven share of Process
    //   kLoFreq      60 .. 400 Hz     150    low/mid crossover, logarithmic
    //   kHiFreq      600 .. 4000 Hz   1200   mid/high crossover, logarithmic
    //   kOutput      -12 .. +12 dB    0      output trim
    //   kSafety      on / off         on     output limiter at -0.2 dBFS
    //
    // Process at 10 is +12 dB of high band boost; Lo Contour at 10 is +10 dB of
    // shelf.  Dynamics at 0 makes the boost a fixed shelf, at 100 it is driven
    // entirely by how much the lows are masking the highs.
    //
    // The desktop build's kLoDelay and kMidDelay are not here - see the note at
    // the top of this file.
    enum ParamId {
        kBypass = 0,
        kProcess,
        kLoContour,
        kDynamics,
        kLoFreq,
        kHiFreq,
        kOutput,
        kSafety,
        kNumParams
    };

    // Enough for a UI or a serial console to render a control without knowing
    // what the parameter is.  decimals < 0 marks an on/off switch.  The table
    // is const, so it stays in flash.
    struct ParamInfo {
        const char *name;
        const char *unit;
        float       minValue;
        float       maxValue;
        float       defaultValue;
        int         decimals;
        bool        isLog;      // a slider should travel logarithmically
        bool        isSwitch;
    };

    SonicMaximizer();

    static const ParamInfo &paramInfo(int id);

    // Call before processing, and again if the stream format changes.  Rebuilds
    // the filters and clears all state.  Allocates nothing.
    void setup(float sampleRate, int channels);
    void reset();

    // value is clamped to the parameter's range.  Recomputes coefficients, so
    // do not call it from an interrupt that can pre-empt process().
    void  setParam(int id, float value);
    float getParam(int id) const;

    // frames = sample frames, not individual samples.  The buffer holds
    // frames * channels() interleaved floats, nominally in [-1, +1].
    void process(float *interleaved, int frames);

    // Same, for the 16-bit buffers an I2S or USB audio path deals in.
    // Converts in chunks through a small stack buffer; nothing is allocated
    // and any number of frames is fine.
    void processInt16(int16_t *interleaved, int frames);

    float sampleRate() const { return m_sampleRate; }
    int   channels()   const { return m_channels; }

    float gainReductionDb() const;   // safety limiter, <= 0
    float processBoostDb()  const;   // HF boost currently applied, >= 0

    // Puts the FPU in flush-to-zero mode so decaying filter tails cannot drift
    // into denormals.  setup() calls it, but FPSCR is per core: if process()
    // runs on a different core than setup() did - which on an RP2350 it often
    // does - call this once from the audio core too.  No-op off ARM.
    static void enableFlushToZero();

private:
    // Topology-preserving state-variable filter (Simper).  One core covers
    // every response the chain needs; only the output mix changes.
    struct Coeffs {
        float a1, a2, a3;
        float m0, m1, m2;
    };

    struct State {
        float ic1eq, ic2eq;
    };

    static inline float run(const Coeffs &c, State &s, float v0)
    {
        const float v3 = v0 - s.ic2eq;
        const float v1 = c.a1 * s.ic1eq + c.a2 * v3;
        const float v2 = s.ic2eq + c.a2 * s.ic1eq + c.a3 * v3;
        s.ic1eq = 2.0f * v1 - s.ic1eq;
        s.ic2eq = 2.0f * v2 - s.ic2eq;
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }

    // 88 bytes per channel.  Two cascaded sections make each Linkwitz-Riley
    // 4th order arm; apLo is the allpass that keeps low + mid + high summing
    // back to the input.
    struct ChannelState {
        State lpHi[2], hpHi[2];
        State lpLo[2], hpLo[2];
        State apLo;
        State loShelf, midDip;
    };

    void updateCoefficients();
    void updateGains();
    float computeHfGain() const;

    static void setCore    (Coeffs &c, float g, float k);
    static void makeLowpass (Coeffs &c, float fc, float sr, float q);
    static void makeHighpass(Coeffs &c, float fc, float sr, float q);
    static void makeAllpass (Coeffs &c, float fc, float sr, float q);
    static void makeLowShelf(Coeffs &c, float fc, float sr, float q, float gainDb);
    static void makeBell    (Coeffs &c, float fc, float sr, float q, float gainDb);

    float m_params[kNumParams];

    float m_sampleRate;
    int   m_channels;

    // Shared by every channel - only the state above is per channel.
    Coeffs m_lpHi, m_hpHi, m_lpLo, m_hpLo, m_apLo, m_loShelf, m_midDip;

    ChannelState m_ch[kMaxChannels];

    // Derived from the parameters, so the audio loop does no unit conversion.
    bool  m_bypass;
    bool  m_safety;
    float m_outputGain;
    float m_processDb;   // maximum HF boost at the current Process setting
    float m_dynFrac;     // 0..1

    // Running state.
    float m_envLow, m_envHigh;
    float m_envAttack, m_envRelease;
    float m_hfGain, m_hfTarget;
    float m_gainSmooth;
    float m_limGain, m_limRelease, m_threshold;
    int   m_controlCount;
};

#endif // SONIC_MAXIMIZER_H_INCLUDED
