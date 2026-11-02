// SonicMaximizer.cpp - single precision engine.  See SonicMaximizer.h for what
// the effect is and why the topology differs from the desktop build.
//
// Everything here is float.  There are no double literals and no double-taking
// math calls, so nothing drags in software double emulation on a target whose
// FPU is single precision only.  Building with -Wdouble-promotion should be
// silent.

#include "SonicMaximizer.h"

#include <math.h>
#include <string.h>

namespace {

const float kPi = 3.14159265f;

// Butterworth Q.  Two of these cascaded make one Linkwitz-Riley 4th order arm.
const float kLrQ = 0.70710678f;

// Full-scale boost available at Process = 10, and the masking range that the
// dynamic half of that boost is spread over.
const float kMaxProcessDb = 12.0f;
const float kMaskRangeDb  = 24.0f;

// Lo Contour at 10: shelf gain, and the size of the complementary mid dip.
const float kMaxContourDb   = 10.0f;
const float kContourDipFrac = 0.25f;

const float kContourFreq = 60.0f;
const float kDipFreq     = 250.0f;
const float kDipQ        = 0.9f;

const float kLimitThresholdDb = -0.2f;   // safety limiter ceiling

// HF gain is recomputed once per this many frames; the per-sample smoother
// covers the gap.  Keeps log10f/powf off the hot path.
const int kControlInterval = 32;

// Frames converted per pass in processInt16.  At stereo this is a 512 byte
// stack buffer.
const int kChunkFrames = 64;

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// One-pole coefficient for a time constant in seconds.
inline float onePole(float seconds, float sampleRate)
{
    if (seconds <= 0.0f || sampleRate <= 0.0f) return 1.0f;
    return 1.0f - expf(-1.0f / (seconds * sampleRate));
}

const SonicMaximizer::ParamInfo kParams[SonicMaximizer::kNumParams] = {
    // name            unit    min      max     default  dec  log    switch
    { "Bypass",        "",      0.0f,    1.0f,     0.0f,  -1, false, true  },
    { "Process",       "",      0.0f,   10.0f,     5.0f,   1, false, false },
    { "Lo Contour",    "",      0.0f,   10.0f,     3.0f,   1, false, false },
    { "Dynamics",      "%",     0.0f,  100.0f,    60.0f,   0, false, false },
    { "Lo/Mid X-over", "Hz",   60.0f,  400.0f,   150.0f,   0, true,  false },
    { "Mid/Hi X-over", "Hz",  600.0f, 4000.0f,  1200.0f,   0, true,  false },
    { "Output",        "dB",  -12.0f,   12.0f,     0.0f,   1, false, false },
    { "Safety Limit",  "",      0.0f,    1.0f,     1.0f,  -1, false, true  },
};

} // namespace

// ---------------------------------------------------------------------------
// Filter design
//
// g is the prewarped cutoff, tan(pi*fc/fs), and k is 1/Q.  Storing the cutoff
// as g is the whole reason this form survives a 60 Hz corner in float32: g is
// about 0.004 there and every bit of it is significant, where a biquad would
// be trying to express the same corner as a difference between coefficients
// that are all within a whisker of 1, -2 and 1.
// ---------------------------------------------------------------------------

void SonicMaximizer::setCore(Coeffs &c, float g, float k)
{
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
}

void SonicMaximizer::makeLowpass(Coeffs &c, float fc, float sr, float q)
{
    fc = clampf(fc, 10.0f, sr * 0.45f);
    setCore(c, tanf(kPi * fc / sr), 1.0f / q);
    c.m0 = 0.0f;
    c.m1 = 0.0f;
    c.m2 = 1.0f;
}

void SonicMaximizer::makeHighpass(Coeffs &c, float fc, float sr, float q)
{
    fc = clampf(fc, 10.0f, sr * 0.45f);
    const float k = 1.0f / q;
    setCore(c, tanf(kPi * fc / sr), k);
    c.m0 =  1.0f;
    c.m1 = -k;
    c.m2 = -1.0f;
}

// The 2nd-order allpass an LR4 pair sums to.  Running the high band through it
// is what keeps low + mid + high reconstructing the input.
void SonicMaximizer::makeAllpass(Coeffs &c, float fc, float sr, float q)
{
    fc = clampf(fc, 10.0f, sr * 0.45f);
    const float k = 1.0f / q;
    setCore(c, tanf(kPi * fc / sr), k);
    c.m0 =  1.0f;
    c.m1 = -2.0f * k;
    c.m2 =  0.0f;
}

void SonicMaximizer::makeLowShelf(Coeffs &c, float fc, float sr, float q, float gainDb)
{
    fc = clampf(fc, 10.0f, sr * 0.45f);
    const float A = powf(10.0f, gainDb / 40.0f);
    const float k = 1.0f / q;
    setCore(c, tanf(kPi * fc / sr) / sqrtf(A), k);
    c.m0 = 1.0f;
    c.m1 = k * (A - 1.0f);
    c.m2 = A * A - 1.0f;
}

void SonicMaximizer::makeBell(Coeffs &c, float fc, float sr, float q, float gainDb)
{
    fc = clampf(fc, 10.0f, sr * 0.45f);
    const float A = powf(10.0f, gainDb / 40.0f);
    const float k = 1.0f / (q * A);
    setCore(c, tanf(kPi * fc / sr), k);
    c.m0 = 1.0f;
    c.m1 = k * (A * A - 1.0f);
    c.m2 = 0.0f;
}

// ---------------------------------------------------------------------------
// Flush to zero
// ---------------------------------------------------------------------------

void SonicMaximizer::enableFlushToZero()
{
#if defined(__ARM_FP) && (__ARM_FP > 0)
    uint32_t fpscr;
    __asm__ volatile ("vmrs %0, fpscr" : "=r" (fpscr));
    fpscr |= (1u << 24);            // FZ
    __asm__ volatile ("vmsr fpscr, %0" : : "r" (fpscr));
#endif
}

// ---------------------------------------------------------------------------
// Construction and configuration
// ---------------------------------------------------------------------------

const SonicMaximizer::ParamInfo &SonicMaximizer::paramInfo(int id)
{
    if (id < 0 || id >= kNumParams) id = 0;
    return kParams[id];
}

SonicMaximizer::SonicMaximizer()
    : m_sampleRate(0.0f)
    , m_channels(0)
    , m_bypass(false)
    , m_safety(true)
    , m_outputGain(1.0f)
    , m_processDb(0.0f)
    , m_dynFrac(0.0f)
    , m_envLow(0.0f)
    , m_envHigh(0.0f)
    , m_envAttack(1.0f)
    , m_envRelease(1.0f)
    , m_hfGain(1.0f)
    , m_hfTarget(1.0f)
    , m_gainSmooth(1.0f)
    , m_limGain(1.0f)
    , m_limRelease(1.0f)
    , m_threshold(powf(10.0f, kLimitThresholdDb / 20.0f))
    , m_controlCount(0)
{
    for (int i = 0; i < kNumParams; ++i)
        m_params[i] = kParams[i].defaultValue;

    memset(&m_lpHi, 0, sizeof m_lpHi);
    memset(&m_hpHi, 0, sizeof m_hpHi);
    memset(&m_lpLo, 0, sizeof m_lpLo);
    memset(&m_hpLo, 0, sizeof m_hpLo);
    memset(&m_apLo, 0, sizeof m_apLo);
    memset(&m_loShelf, 0, sizeof m_loShelf);
    memset(&m_midDip, 0, sizeof m_midDip);
    memset(m_ch, 0, sizeof m_ch);

    updateGains();
}

void SonicMaximizer::setup(float sampleRate, int channels)
{
    if (sampleRate <= 0.0f) return;
    if (channels < 1) channels = 1;
    if (channels > kMaxChannels) channels = kMaxChannels;

    enableFlushToZero();

    m_sampleRate = sampleRate;
    m_channels   = channels;

    m_envAttack  = onePole(0.010f, sampleRate);   // masking detector tracks the
    m_envRelease = onePole(0.150f, sampleRate);   // program, not each transient
    m_gainSmooth = onePole(0.005f, sampleRate);
    m_limRelease = onePole(0.100f, sampleRate);

    updateCoefficients();
    updateGains();
    reset();
}

void SonicMaximizer::reset()
{
    memset(m_ch, 0, sizeof m_ch);

    m_envLow       = 0.0f;
    m_envHigh      = 0.0f;
    m_limGain      = 1.0f;
    m_controlCount = 0;
    m_hfTarget     = computeHfGain();
    m_hfGain       = m_hfTarget;
}

void SonicMaximizer::updateCoefficients()
{
    if (m_sampleRate <= 0.0f) return;

    const float fLo = m_params[kLoFreq];
    const float fHi = m_params[kHiFreq];
    const float contourDb = kMaxContourDb * (m_params[kLoContour] * 0.1f);

    makeLowpass (m_lpHi, fHi, m_sampleRate, kLrQ);
    makeHighpass(m_hpHi, fHi, m_sampleRate, kLrQ);
    makeLowpass (m_lpLo, fLo, m_sampleRate, kLrQ);
    makeHighpass(m_hpLo, fLo, m_sampleRate, kLrQ);
    makeAllpass (m_apLo, fLo, m_sampleRate, kLrQ);
    makeLowShelf(m_loShelf, kContourFreq, m_sampleRate, kLrQ, contourDb);
    makeBell    (m_midDip,  kDipFreq,     m_sampleRate, kDipQ,
                 -contourDb * kContourDipFrac);
}

void SonicMaximizer::updateGains()
{
    m_bypass     = m_params[kBypass] >= 0.5f;
    m_safety     = m_params[kSafety] >= 0.5f;
    m_outputGain = powf(10.0f, m_params[kOutput] / 20.0f);
    m_processDb  = kMaxProcessDb * (m_params[kProcess] * 0.1f);
    m_dynFrac    = clampf(m_params[kDynamics] * 0.01f, 0.0f, 1.0f);
}

void SonicMaximizer::setParam(int id, float value)
{
    if (id < 0 || id >= kNumParams) return;

    const ParamInfo &p = kParams[id];
    value = clampf(value, p.minValue, p.maxValue);
    if (value == m_params[id]) return;
    m_params[id] = value;

    if (id == kLoFreq || id == kHiFreq || id == kLoContour)
        updateCoefficients();
    updateGains();
}

float SonicMaximizer::getParam(int id) const
{
    if (id < 0 || id >= kNumParams) return 0.0f;
    return m_params[id];
}

float SonicMaximizer::gainReductionDb() const
{
    if (m_limGain >= 1.0f)  return 0.0f;
    if (m_limGain <= 1e-6f) return -120.0f;
    return 20.0f * log10f(m_limGain);
}

float SonicMaximizer::processBoostDb() const
{
    if (m_hfGain <= 1e-6f) return 0.0f;
    return 20.0f * log10f(m_hfGain);
}

// ---------------------------------------------------------------------------
// The masking estimate
//
// envLow / envHigh are the band envelopes.  Their ratio says how much low-band
// energy is currently piled on top of the high band; the more there is, the
// more of the available boost gets handed to the highs.  With Dynamics at 0
// this collapses to a fixed shelf.
// ---------------------------------------------------------------------------

float SonicMaximizer::computeHfGain() const
{
    if (m_processDb <= 0.0f) return 1.0f;

    const float lo = m_envLow  + 1e-9f;
    const float hi = m_envHigh + 1e-9f;
    const float maskDb = 20.0f * log10f(lo / hi);
    const float t = clampf(maskDb / kMaskRangeDb, 0.0f, 1.0f);

    const float db = m_processDb * ((1.0f - m_dynFrac) + m_dynFrac * t);
    return powf(10.0f, db / 20.0f);
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void SonicMaximizer::process(float *interleaved, int frames)
{
    if (!interleaved || frames <= 0) return;
    if (m_sampleRate <= 0.0f || m_channels <= 0) return;
    if (m_bypass) return;

    const int ch = m_channels;

    for (int n = 0; n < frames; ++n) {
        float *frame = interleaved + (size_t)n * ch;

        float low[kMaxChannels], mid[kMaxChannels], high[kMaxChannels];
        float lowPeak = 0.0f, highPeak = 0.0f;

        // --- split -----------------------------------------------------
        for (int c = 0; c < ch; ++c) {
            ChannelState &s = m_ch[c];
            const float x = frame[c];

            const float lowMid = run(m_lpHi, s.lpHi[1], run(m_lpHi, s.lpHi[0], x));
            float hi = run(m_hpHi, s.hpHi[1], run(m_hpHi, s.hpHi[0], x));
            hi = run(m_apLo, s.apLo, hi);   // keeps the three bands summing back

            float lo = run(m_lpLo, s.lpLo[1], run(m_lpLo, s.lpLo[0], lowMid));
            float md = run(m_hpLo, s.hpLo[1], run(m_hpLo, s.hpLo[0], lowMid));

            lo = run(m_loShelf, s.loShelf, lo);
            md = run(m_midDip,  s.midDip,  md);

            low[c]  = lo;
            mid[c]  = md;
            high[c] = hi;

            // The detectors are linked across channels - a per-channel boost
            // would pull the stereo image around on off-centre material.
            const float al = fabsf(lo), ah = fabsf(hi);
            if (al > lowPeak)  lowPeak  = al;
            if (ah > highPeak) highPeak = ah;
        }

        // --- masking estimate ------------------------------------------
        m_envLow  += (lowPeak  - m_envLow)  * (lowPeak  > m_envLow  ? m_envAttack : m_envRelease);
        m_envHigh += (highPeak - m_envHigh) * (highPeak > m_envHigh ? m_envAttack : m_envRelease);

        if (--m_controlCount <= 0) {
            m_hfTarget     = computeHfGain();
            m_controlCount = kControlInterval;
        }
        m_hfGain += (m_hfTarget - m_hfGain) * m_gainSmooth;

        // --- recombine -------------------------------------------------
        float out[kMaxChannels];
        float peak = 0.0f;
        for (int c = 0; c < ch; ++c) {
            const float y = (low[c] + mid[c] + high[c] * m_hfGain) * m_outputGain;
            out[c] = y;
            const float a = fabsf(y);
            if (a > peak) peak = a;
        }

        // --- safety limiter --------------------------------------------
        // Instant attack, no lookahead: the gain is computed from the very
        // sample it is applied to, so the ceiling is never overshot.
        if (m_safety) {
            float g = 1.0f;
            if (peak > m_threshold) g = m_threshold / peak;
            if (g < m_limGain) m_limGain = g;
            else               m_limGain += (g - m_limGain) * m_limRelease;

            for (int c = 0; c < ch; ++c) {
                float y = out[c] * m_limGain;
                if (y >  1.0f) y =  1.0f;
                if (y < -1.0f) y = -1.0f;
                frame[c] = y;
            }
        } else {
            m_limGain = 1.0f;
            for (int c = 0; c < ch; ++c)
                frame[c] = out[c];
        }
    }
}

void SonicMaximizer::processInt16(int16_t *interleaved, int frames)
{
    if (!interleaved || frames <= 0) return;
    if (m_sampleRate <= 0.0f || m_channels <= 0) return;
    if (m_bypass) return;

    const int ch = m_channels;
    float scratch[kChunkFrames * kMaxChannels];

    for (int done = 0; done < frames; ) {
        int block = frames - done;
        if (block > kChunkFrames) block = kChunkFrames;

        int16_t  *src = interleaved + (size_t)done * ch;
        const int n   = block * ch;

        for (int i = 0; i < n; ++i)
            scratch[i] = (float)src[i] * (1.0f / 32768.0f);

        process(scratch, block);

        for (int i = 0; i < n; ++i) {
            const float f = scratch[i] * 32768.0f;
            int v = (int)(f >= 0.0f ? f + 0.5f : f - 0.5f);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            src[i] = (int16_t)v;
        }
        done += block;
    }
}
