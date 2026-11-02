// SpdifInput.cpp - see SpdifInput.h for the contract and the policy.

#include "SpdifInput.h"

#include <math.h>
#include <string.h>

namespace {

const float kOutRate = 48000.0f;

// Decoded samples arrive 24-bit left-justified in int32. The ring holds them
// normalised, which is what the ASRC wants and what makes the ramp a plain
// multiply. Exact in both directions: a 24-bit value has 24 significant bits
// and float32 has 24 of mantissa.
inline float toFloat(int32_t s) { return (float)(s >> 8) * (1.0f / 8388608.0f); }

inline int32_t toInt(float v)
{
    // Clamp before scaling. The ASRC's kernel overshoots on transients - a
    // windowed sinc has to - so a full-scale square wave can produce values
    // slightly past +-1, and wrapping those would turn an overshoot into a
    // full-amplitude sign flip.
    if (v >  0.99999994f) v =  0.99999994f;
    if (v < -1.0f)        v = -1.0f;
    return (int32_t)(v * 8388608.0f) << 8;
}

} // namespace

SpdifInput::SpdifInput() { init(); }

void SpdifInput::init()
{
    for (int i = 0; i <= SPDIF_FADE_FRAMES; ++i) {
        // Raised cosine rather than linear: a linear ramp has a slope
        // discontinuity at each end, which is a (small) click of its own.
        const double t = (double)i / (double)SPDIF_FADE_FRAMES;
        m_fade[i] = (float)(0.5 - 0.5 * cos(3.14159265358979323846 * t));
    }

    // The one place the resampler's table is built. reset() below deliberately
    // does not, so this has to happen here rather than being left to it.
    m_asrc.init(kOutRate);
    reset();
}

void SpdifInput::reset()
{
    m_rx.reset();

    // **restart(), not init().** This is called from the block path - the
    // module's divider scan calls it every SPDIF_SCAN_BLOCKS while nothing is
    // framing, which is continuously whenever no source is connected - and
    // init() rebuilds a 129x32 Kaiser-windowed sinc in software `double`.
    // That is tens of milliseconds against a 1333 us deadline, so `ovr` rose
    // the whole time the input was idle and stopped the moment a source
    // appeared. The kernel does not depend on anything a re-acquire changes.
    m_asrc.restart(kOutRate);
    m_est.init(kOutRate, SPDIF_RING_TARGET);

    m_head = m_tail = m_count = 0;
    m_fadeIdx = 0;
    m_gain    = 0.0f;
    m_lastL = m_lastR = 0.0f;
    m_state = kNoCarrier;
    m_arrivedSinceBlock = 0;
    m_lastNominal = 0;
    m_rateChanged = false;

    m_underruns = 0;
    m_dropped   = 0;
}

// ---------------------------------------------------------------------------
//  Ring
// ---------------------------------------------------------------------------

void SpdifInput::ringPush(const int32_t *frames, int n)
{
    for (int i = 0; i < n; ++i) {
        if (m_count >= SPDIF_RING_FRAMES) {
            // Overrun. Drop the oldest rather than the newest: the newest is
            // what the loop is about to be judged on, and an old frame is the
            // one nobody is going to miss.
            m_tail = (m_tail + 1) % SPDIF_RING_FRAMES;
            --m_count;
            ++m_dropped;
        }
        m_ring[m_head * 2 + 0] = toFloat(frames[i * 2 + 0]);
        m_ring[m_head * 2 + 1] = toFloat(frames[i * 2 + 1]);
        m_head = (m_head + 1) % SPDIF_RING_FRAMES;
        ++m_count;
    }
}

void SpdifInput::ringTrim(int target)
{
    while (m_count > target) {
        m_tail = (m_tail + 1) % SPDIF_RING_FRAMES;
        --m_count;
    }
}

int SpdifInput::ringContig() const
{
    const int toEnd = SPDIF_RING_FRAMES - m_tail;
    return (m_count < toEnd) ? m_count : toEnd;
}

// ---------------------------------------------------------------------------
//  Input
// ---------------------------------------------------------------------------

int SpdifInput::push(const uint32_t *words, int count)
{
    if (!words || count <= 0) return 0;

    int total = 0;

    // The frames one chunk can produce are bounded by the UI in it, and
    // m_decoded is sized from that rather than from a generous guess - see
    // SPDIF_PUSH_FRAMES. The loop is what makes any `count` safe.
    int pos = 0;
    while (pos < count) {
        int chunk = count - pos;
        if (chunk > SPDIF_PUSH_CHUNK) chunk = SPDIF_PUSH_CHUNK;

        const int got = m_rx.process(words + pos, chunk, m_decoded,
                                     SPDIF_PUSH_FRAMES);
        if (got > 0) {
            ringPush(m_decoded, got);
            total += got;
        }
        pos += chunk;
    }

    m_arrivedSinceBlock += total;
    return total;
}

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------

void SpdifInput::updateState()
{
    if (!m_rx.carrier()) {
        // The decoder saying there is nothing on the wire outranks whatever
        // the estimator last thought, and it knows sooner.
        if (m_state != kNoCarrier) {
            m_est.setCarrier(false);
            m_asrc.reset();
        }
        m_state = kNoCarrier;
        return;
    }

    m_est.setCarrier(true);

    // **A rate change has to be faded through, not played through.**
    //
    // The estimator notices a new rate a classification window after it
    // happens, and until then the ASRC has been resampling the new stream at
    // the old ratio - 44.1 material played at the 48 kHz ratio is 9 % sharp.
    // Carrying straight on from there also steps the waveform, because the
    // source's own content is discontinuous across a track change.
    //
    // So treat it as a re-acquire: ramp down, re-prime, ramp back up. The
    // ratio is right the instant the new rate is classified, because the
    // nominal rate is the feedforward - only the ppm trim is still
    // converging - so the mute is two ramps long and not a lock time.
    const int nom = m_est.nominalHz();
    if (nom != m_lastNominal) {
        if (m_lastNominal != 0 && nom != 0) m_rateChanged = true;
        m_lastNominal = nom;
    }

    // **What actually covers the observed 44.1 -> 48 transition is the
    // Unsupported state, not this latch.** A trace across the seam showed the
    // decoder riding straight through - zero resyncs, zero parity errors,
    // because the histogram simply re-learns the new unit length - and the
    // estimator passing through Unsupported while its window spans both
    // rates, which mutes and trims on its own.
    //
    // The latch is kept because a transition that classifies cleanly from one
    // rate straight to another would skip Unsupported, and playing on through
    // that would be 9 % sharp until the ratio caught up.
    //
    // A resync-burst trigger was written here first, on the theory that the
    // seam would produce decoder distress worth fading on. **The trace
    // disproved it** - the seam is clean - so it was removed rather than kept
    // as defensive code no test exercises.

    switch (m_est.state()) {
        case RateEstimator::kUnsupported: m_state = kUnsupported; break;
        case RateEstimator::kLocked:      m_state = kLocked;      break;
        case RateEstimator::kAcquiring:   m_state = kAcquiring;   break;
        default:                          m_state = kAcquiring;   break;
    }
}

// ---------------------------------------------------------------------------
//  Output
// ---------------------------------------------------------------------------

void SpdifInput::emitMuted(int32_t *out, int frames)
{
    // Still ramps: this is reached the moment a carrier disappears, and the
    // gain may be anywhere. Only once the ramp has run out is the output
    // genuinely zero - and it is *exactly* zero then, not the last sample
    // held, because DC into a speaker is its own problem.
    for (int i = 0; i < frames; ++i) {
        if (m_fadeIdx > 0) --m_fadeIdx;
        m_gain = m_fade[m_fadeIdx];
        out[i * 2 + 0] = toInt(m_lastL * m_gain);
        out[i * 2 + 1] = toInt(m_lastR * m_gain);
    }
    if (m_fadeIdx == 0) { m_lastL = 0.0f; m_lastR = 0.0f; }
}

void SpdifInput::applyGain(const float *src, int made, int32_t *out, int frames)
{
    int i = 0;
    for (; i < made; ++i) {
        if (m_fadeIdx < SPDIF_FADE_FRAMES) ++m_fadeIdx;
        m_gain  = m_fade[m_fadeIdx];
        m_lastL = src[i * 2 + 0];
        m_lastR = src[i * 2 + 1];
        out[i * 2 + 0] = toInt(m_lastL * m_gain);
        out[i * 2 + 1] = toInt(m_lastR * m_gain);
    }

    // Shortfall: the ASRC ran out of input part way through the block. Ramp
    // *down* across what is left rather than writing zeros - the ramp is what
    // keeps a dropout from being a click. If frames arrive again next block
    // the ramp simply turns around, so a one-block hiccup costs a dip and not
    // a gap.
    for (; i < frames; ++i) {
        if (m_fadeIdx > 0) --m_fadeIdx;
        m_gain = m_fade[m_fadeIdx];
        out[i * 2 + 0] = toInt(m_lastL * m_gain);
        out[i * 2 + 1] = toInt(m_lastR * m_gain);
    }
}

void SpdifInput::produce(int32_t *out, int frames)
{
    if (!out || frames <= 0) return;
    if (frames > SPDIF_MAX_BLOCK) frames = SPDIF_MAX_BLOCK;

    updateState();

    // A rate change holds the mute only until the ramp has actually reached
    // zero - then the pipeline is re-primed and the normal path ramps back
    // up. Resetting before the ramp finishes would put the step back.
    if (m_rateChanged && m_fadeIdx == 0) {
        m_rateChanged = false;
        m_asrc.reset();
        m_rx.reset();
        m_head = m_tail = m_count = 0;
    }

    const bool usable = (m_state == kAcquiring || m_state == kLocked)
                        && m_est.usable() && !m_rateChanged;

    if (!usable) {
        // Muted. Keep the ring fresh rather than letting it fill: the ASRC
        // must not be pulled before the rate is known, and stale audio is of
        // no use when it does start. See the note in SpdifInput.h.
        ringTrim(SPDIF_RING_TARGET);
        m_est.update(m_arrivedSinceBlock, frames, m_count);
        m_arrivedSinceBlock = 0;
        m_asrc.reset();
        emitMuted(out, frames);
        return;
    }

    float tmp[SPDIF_MAX_BLOCK * 2];
    int made = 0;

    while (made < frames) {
        const int avail = ringContig();
        if (avail <= 0) break;

        int used = 0;
        const int got = m_asrc.produce(tmp + made * 2, frames - made,
                                       m_ring + m_tail * 2, avail, &used);
        m_tail   = (m_tail + used) % SPDIF_RING_FRAMES;
        m_count -= used;
        made    += got;

        if (got == 0 && used == 0) break;
    }

    if (made < frames) ++m_underruns;

    applyGain(tmp, made, out, frames);

    const float hz = m_est.update(m_arrivedSinceBlock, frames, m_count);
    m_arrivedSinceBlock = 0;
    if (hz > 0.0f) m_asrc.setInputRate(hz);
}
