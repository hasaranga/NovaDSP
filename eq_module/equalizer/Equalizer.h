// Equalizer.h - ten band graphic equaliser, single precision, for RP2350 /
// Pico 2 and other small FPU targets.
//
// The bands are the standard octave centres from 125 Hz up, with the bottom two
// moved off the ISO values (31.5 and 63) to 60 and 80:
//
//     60  80  125  250  500  1k  2k  4k  8k  16k  Hz
//
// From 125 Hz up each centre is exactly twice the one below it. Q is 1.4 for
// every band - the Q of a bell exactly one octave wide, 1/(2^0.5 - 2^-0.5) =
// 1.4142 - so across that span neighbours cross near their half-power points
// and a row of them adds into a smooth curve rather than a row of bumps.
//
// The bottom two are the deliberate exception and behave differently. 60 and
// 80 Hz are 0.415 of an octave apart, well inside each other's one-octave
// skirts, so they overlap and add much as the old 12k/14k/16k did. Measured at
// 48 kHz: either one alone at +12 dB is +12.00 dB at its own centre, but both
// at +12 dB is +19.0 dB at each centre and +20.0 dB between them at 69 Hz.
// With all ten at +12 dB the bass is the hottest part of the curve - +25 dB at
// 80 Hz against +18.5 dB across the octave-spaced middle. That is the trade
// that was chosen: the bottom of the range sits where small speakers actually
// reproduce rather than an octave below it, at the price of two bands that
// steer together instead of independently. Pull the preamp down accordingly.
//
// The consequence at the very bottom is that there is no control below 60 Hz.
// All ten at +12 dB reaches only +1.9 dB at 20 Hz and +9.0 dB at 40 Hz, so
// sub-bass is shaped by the skirt of the 60 Hz band or not at all.
//
// Topology
// --------
// Each band is a topology-preserving state-variable bell (Simper), the same
// form the BBE port uses, and for the same reason: a direct-form biquad keeps
// its coefficients close to (1, -2, 1) as the cutoff falls, so in float32 a
// 60 Hz bell has almost no significant bits left to describe it and the
// response drifts. The state-variable form stores tan(pi*fc/fs) directly - at
// 60 Hz and 48 kHz that is about 0.00393, and every bit of it is significant.
//
// Coefficients live once and are shared by every channel; only the two
// integrator states per band are per channel. The whole object is well under
// a kilobyte.
//
// The preamp
// ----------
// Ten bands that each boost 12 dB can add to far more than 12 dB, and with
// nothing in front of them that lands as clipping in the output conversion.
// The preamp is the usual answer and is why consumer EQs all have one: pull it
// down as you push bands up. It is applied once per sample before the bands,
// so it costs one multiply.
//
// Usage
// -----
//     static Equalizer eq;
//     eq.setup(48000.0f, 2);
//     eq.setBandGainDb(0, +6.0f);
//     eq.setPreampDb(-3.0f);
//     ...
//     eq.process(interleaved, frames);
//
// process() and the setters are not synchronised. Set parameters from the
// audio core between blocks, as the effect registry already does.

#ifndef EQUALIZER_H_INCLUDED
#define EQUALIZER_H_INCLUDED

#ifndef EQUALIZER_MAX_CHANNELS
#define EQUALIZER_MAX_CHANNELS 2
#endif

class Equalizer
{
public:
    enum { kNumBands = 10, kMaxChannels = EQUALIZER_MAX_CHANNELS };

    // Gain limits, in dB, for every band and for the preamp.
    static const float kMinGainDb;
    static const float kMaxGainDb;

    // Centre frequencies, and short labels a 128 px panel can render.
    static const float        kBandHz[kNumBands];
    static const char *const  kBandLabel[kNumBands];

    Equalizer();

    // Call before processing, and again if the stream format changes. Rebuilds
    // every band and clears all state. Allocates nothing.
    void setup(float sampleRate, int channels);
    void reset();

    // Clamped to [kMinGainDb, kMaxGainDb]. Redesigns one band, so do not call
    // from an interrupt that can pre-empt process().
    void  setBandGainDb(int band, float db);
    float bandGainDb(int band) const;

    // Clamped the same way. Costs one multiply per sample, no redesign.
    void  setPreampDb(float db);
    float preampDb() const;

    void process(float *interleaved, int frames);

    float sampleRate() const { return m_sampleRate; }
    int   channels()   const { return m_channels; }

    // Puts the FPU in flush-to-zero mode so decaying filter tails cannot drift
    // into denormals. setup() calls it, but FPSCR is per core: if process()
    // runs on a different core than setup() did, call this from the audio core
    // too. No-op off ARM.
    static void enableFlushToZero();

private:
    struct Coeffs { float a1, a2, a3, m0, m1, m2; };
    struct State  { float ic1eq, ic2eq; };

    static inline float run(const Coeffs &c, State &s, float v0)
    {
        const float v3 = v0 - s.ic2eq;
        const float v1 = c.a1 * s.ic1eq + c.a2 * v3;
        const float v2 = s.ic2eq + c.a2 * s.ic1eq + c.a3 * v3;
        s.ic1eq = 2.0f * v1 - s.ic1eq;
        s.ic2eq = 2.0f * v2 - s.ic2eq;
        return c.m0 * v0 + c.m1 * v1 + c.m2 * v2;
    }

    void designBand(int band);

    float  m_gainDb[kNumBands];
    Coeffs m_band[kNumBands];
    State  m_state[kMaxChannels][kNumBands];

    // Which bands are actually doing anything. A band at 0 dB is a no-op that
    // still costs ten flops a sample, and most of the ten are usually flat, so
    // the audio loop skips them - which is what keeps a mostly-flat EQ cheap.
    bool   m_active[kNumBands];

    float  m_preampDb;
    float  m_preampGain;
    float  m_sampleRate;
    int    m_channels;
};

#endif // EQUALIZER_H_INCLUDED
