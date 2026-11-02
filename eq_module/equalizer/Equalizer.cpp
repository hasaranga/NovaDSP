// Equalizer.cpp - ten band graphic equaliser, single precision engine.
// See Equalizer.h for the band layout and why the topology is what it is.
//
// Everything here is float. There are no double literals and no double-taking
// math calls, so nothing drags in software double emulation on a target whose
// FPU is single precision only. Building with -Wdouble-promotion is silent.

#include "Equalizer.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace {

const float kPi = 3.14159265f;

// One Q for every band. From 125 Hz up the bands are octave centres, and 1.4 is
// the Q of a bell exactly one octave wide - 1/(2^0.5 - 2^-0.5) = 1.4142 - so
// over that span neighbours cross near their half-power points and a row of
// them adds into a smooth curve rather than a row of bumps.
//
// The bottom two, 60 and 80 Hz, are 0.415 of an octave apart and so are much
// closer than that Q assumes: they overlap heavily and add, the same way the
// old 12k/14k/16k did. That is deliberate and not a bug - see Equalizer.h.
// Narrowing Q to separate them would make the eight octave-spaced bands above
// ring and leave gaps between them, and there is one Q for the whole set.
const float kBandQ = 1.4f;

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

const float Equalizer::kMinGainDb = -12.0f;
const float Equalizer::kMaxGainDb =  12.0f;

// Octave centres from 125 Hz up; the bottom two are 60 and 80 rather than the
// ISO 31.5 and 63. See the note in Equalizer.h - the bottom of the range is
// placed where small speakers actually reproduce, and the cost is that 60 and
// 80 sit under half an octave apart and so overlap.
const float Equalizer::kBandHz[Equalizer::kNumBands] = {
    60.0f, 80.0f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

const char *const Equalizer::kBandLabel[Equalizer::kNumBands] = {
    "60Hz", "80Hz", "125Hz", "250Hz", "500Hz",
    "1kHz", "2kHz", "4kHz", "8kHz", "16kHz"
};

Equalizer::Equalizer()
    : m_preampDb(0.0f), m_preampGain(1.0f), m_sampleRate(48000.0f), m_channels(2)
{
    memset(m_gainDb, 0, sizeof m_gainDb);
    memset(m_band,   0, sizeof m_band);
    memset(m_state,  0, sizeof m_state);
    memset(m_active, 0, sizeof m_active);
}

void Equalizer::setup(float sampleRate, int channels)
{
    m_sampleRate = (sampleRate > 0.0f) ? sampleRate : 48000.0f;
    m_channels   = (channels < 1) ? 1
                 : (channels > kMaxChannels ? kMaxChannels : channels);

    for (int b = 0; b < kNumBands; ++b) designBand(b);

    reset();
    enableFlushToZero();
}

void Equalizer::reset()
{
    memset(m_state, 0, sizeof m_state);
}

// Simper's state-variable bell. g is the prewarped cutoff, tan(pi*fc/fs), and
// k is 1/(Q*A) - storing the cutoff as g is what keeps a 60 Hz band well
// conditioned in float32, where a biquad would be expressing the same corner
// as a difference between coefficients all within a whisker of 1, -2 and 1.
void Equalizer::designBand(int band)
{
    if (band < 0 || band >= kNumBands) return;

    // Above about 0.45 fs the prewarp runs away; at 48 kHz that is 21.6 kHz, so
    // every band in the table is comfortably inside it. The clamp is here for
    // the case where someone runs this engine at 32 kHz, where 16 kHz is not.
    // The 10 Hz floor is far below the 60 Hz bottom band and never bites.
    const float fc = clampf(kBandHz[band], 10.0f, m_sampleRate * 0.45f);
    const float A  = powf(10.0f, m_gainDb[band] / 40.0f);
    const float k  = 1.0f / (kBandQ * A);
    const float g  = tanf(kPi * fc / m_sampleRate);

    Coeffs &c = m_band[band];
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    c.m0 = 1.0f;
    c.m1 = k * (A * A - 1.0f);
    c.m2 = 0.0f;

    // A band at unity has m1 == 0, so it returns its input exactly. Rather than
    // run ten flops to compute that, the audio loop skips it - most of the ten
    // are flat most of the time, which is what keeps a mostly-flat EQ cheap.
    // The state is deliberately not cleared: a band switched back on picks up
    // where it was rather than restarting, so nudging a gain through zero does
    // not click.
    m_active[band] = (m_gainDb[band] != 0.0f);
}

void Equalizer::setBandGainDb(int band, float db)
{
    if (band < 0 || band >= kNumBands) return;

    m_gainDb[band] = clampf(db, kMinGainDb, kMaxGainDb);
    designBand(band);
}

float Equalizer::bandGainDb(int band) const
{
    return (band < 0 || band >= kNumBands) ? 0.0f : m_gainDb[band];
}

void Equalizer::setPreampDb(float db)
{
    m_preampDb   = clampf(db, kMinGainDb, kMaxGainDb);
    m_preampGain = powf(10.0f, m_preampDb / 20.0f);
}

float Equalizer::preampDb() const { return m_preampDb; }

void Equalizer::process(float *interleaved, int frames)
{
    if (!interleaved || frames <= 0) return;

    const bool trim = (m_preampGain != 1.0f);

    for (int ch = 0; ch < m_channels; ++ch) {
        State *st = m_state[ch];

        for (int f = 0; f < frames; ++f) {
            float *p = &interleaved[f * m_channels + ch];
            float  s = *p;

            if (trim) s *= m_preampGain;

            // Bands in series. The branch costs less than the ten flops it
            // skips, and on a flat EQ it skips all ten.
            for (int b = 0; b < kNumBands; ++b)
                if (m_active[b]) s = run(m_band[b], st[b], s);

            *p = s;
        }
    }
}

void Equalizer::enableFlushToZero()
{
#if defined(__ARM_FP) && (__ARM_FP > 0)
    // FPSCR bit 24 is the flush-to-zero flag. Denormals on a Cortex-M33 are
    // handled in software and cost hundreds of cycles apiece, which a decaying
    // filter tail can walk straight into.
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
}
