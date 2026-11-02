#include "NoiseGate.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace {

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// dB to linear amplitude. Parameter time only - never in the audio path.
inline float dbToLin(float db)
{
    return powf(10.0f, db * (1.0f / 20.0f));
}

// The gate never reaches true zero; it stops at a fixed -96 dBFS. Not a
// parameter - see NoiseGate.h. Written out by hand rather than through
// dbToLin() at run time, the same way AutoGain's gate threshold is: it is a
// constant, so there is no reason to spend a powf reaching it more than once.
const float kFloorGain = 1.5849e-5f;   // 10^(-96/20)

// Detector smoothing across blocks - fast enough that a real onset is not
// missed inside the block it arrives in, slow enough that one noisy block
// cannot flicker the gate by itself. Not the Attack/Release the user sets:
// those drive the *gain* once the detector has decided; this only steadies
// what the detector sees, the same split AutoGain draws between its internal
// detector constants and the Boost a user can turn.
const float kEnvAttackSeconds = 0.0010f;
const float kEnvReleaseSeconds = 0.0200f;

} // namespace

NoiseGate::NoiseGate()
{
    for (int i = 0; i < kNumParams; ++i)
        m_params[i] = paramInfo(i).defaultValue;

    m_sampleRate = 48000.0f;
    m_channels   = 2;
    m_look       = 0;   // adopted from the first block handed over

    m_envMs      = 0.0f;
    m_holdBlocks = 0;
    m_target     = kFloorGain;
    m_gain       = kFloorGain;

    updateCoefficients();
}

void NoiseGate::setup(float sampleRate, int channels)
{
    if (sampleRate > 0.0f) m_sampleRate = sampleRate;
    m_channels = (channels < 1) ? 1
               : (channels > kMaxChannels ? kMaxChannels : channels);

    updateCoefficients();
    reset();
    enableFlushToZero();
}

void NoiseGate::reset()
{
    m_envMs      = 0.0f;
    m_holdBlocks = 0;
    m_target     = kFloorGain;
    m_gain       = kFloorGain;
}

// Everything the audio path needs, derived once here rather than per sample
// or per block.
void NoiseGate::updateCoefficients()
{
    const float thresh = dbToLin(m_params[kThresholdDb]);
    m_threshMs = thresh * thresh;

    // Slew rates: gain units per sample. 0.1 ms floor keeps a division by
    // zero out of reach without needing a branch in process().
    m_attackRate  = 1.0f / (m_params[kAttackMs]  * 0.001f * m_sampleRate);
    m_releaseRate = 1.0f / (m_params[kReleaseMs] * 0.001f * m_sampleRate);

    // Per block, so it needs the block rate. m_look is zero until the first
    // block; process() calls this again the moment it knows the real one.
    const float blockRate = (m_look > 0) ? (m_sampleRate / (float)m_look)
                                         : m_sampleRate;
    m_holdBlocksMax = (int)(m_params[kHoldMs] * 0.001f * blockRate);

    // 1 - exp(-x) ~= x holds to a part in ten thousand at these constants,
    // the same approximation AutoGain's own detector coefficients use, so
    // there is no expf here either.
    m_envAtk = clampf(1.0f / (kEnvAttackSeconds  * blockRate), 0.0f, 1.0f);
    m_envRel = clampf(1.0f / (kEnvReleaseSeconds * blockRate), 0.0f, 1.0f);
}

void NoiseGate::setParam(int id, float value)
{
    if (id < 0 || id >= kNumParams) return;

    const ParamInfo &pi = paramInfo(id);
    m_params[id] = clampf(value, pi.minValue, pi.maxValue);

    updateCoefficients();
}

float NoiseGate::getParam(int id) const
{
    if (id < 0 || id >= kNumParams) return 0.0f;
    return m_params[id];
}

void NoiseGate::process(float *samples, int frames)
{
    if (frames <= 0) return;

    const int ch = m_channels;

    // Block rate feeds the hold counter and the detector's own smoothing, so
    // both are recomputed the moment the caller's block size is known or
    // changes - the same adoption AutoGain's lookahead makes on its first
    // call.
    if (frames != m_look) {
        m_look = frames;
        updateCoefficients();
    }

    const int n = frames * ch;

    // ---- detector: broadband mean square -----------------------------------
    //
    // Full range, on purpose - the gate's whole job here is level, not tone,
    // so it reads the signal as a whole rather than any one band of it. Reads
    // samples and does not write it.
    float sumsq = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float x = samples[i];
        sumsq += x * x;
    }
    const float blockMs = sumsq / (float)n;

    // ---- smooth across blocks, then decide --------------------------------
    const float eCoef = (blockMs > m_envMs) ? m_envAtk : m_envRel;
    m_envMs += (blockMs - m_envMs) * eCoef;

    const bool above = (m_envMs > m_threshMs);

    // ---- the gate's own state: Hold, then Attack/Release drive the gain ---
    //
    // Above threshold always (re)arms the hold counter and asks to be open.
    // Below it, Hold counts down before the gate is allowed to ask to close -
    // this is what keeps a gap inside a phrase from being read as silence.
    // Attack and Release do not appear here at all: they are slew rates
    // applied below, not part of this decision.
    if (above) {
        m_holdBlocks = m_holdBlocksMax;
        m_target = 1.0f;
    } else if (m_holdBlocks > 0) {
        --m_holdBlocks;
        m_target = 1.0f;
    } else {
        m_target = kFloorGain;
    }

    // ---- apply the gain, slewed every sample so nothing here can click ----
    for (int f = 0; f < frames; ++f) {
        if (m_gain < m_target) {
            m_gain += m_attackRate;
            if (m_gain > m_target) m_gain = m_target;
        } else if (m_gain > m_target) {
            m_gain -= m_releaseRate;
            if (m_gain < m_target) m_gain = m_target;
        }

        float *fr = samples + f * ch;
        for (int c = 0; c < ch; ++c)
            fr[c] *= m_gain;
    }
}

void NoiseGate::enableFlushToZero()
{
#if defined(__ARM_FP) && (__ARM_FP > 0)
    // FPSCR bit 24 is the flush-to-zero flag. Denormals on a Cortex-M33 are
    // handled in software and cost hundreds of cycles apiece, which the
    // detector's smoothed envelope can walk into while it settles toward
    // silence.
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
}
