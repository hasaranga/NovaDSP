// SpdifRx.cpp - see SpdifRx.h for the format and for why the decode is here
// rather than in the PIO program.

#include "SpdifRx.h"

#include <math.h>   // ceilf, for the integer thresholds

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

// Count trailing zeros. On the target this is arm-none-eabi-gcc's builtin,
// which becomes RBIT + CLZ - two instructions. The MSVC branch exists only
// because the host suite builds with cl; nothing in a firmware image takes
// that path.
inline uint32_t ctz32(uint32_t v)
{
#if defined(_MSC_VER)
    unsigned long idx;
    _BitScanForward(&idx, v);
    return (uint32_t)idx;
#else
    return (uint32_t)__builtin_ctz(v);
#endif
}

// Samples with nothing decodable before the carrier is declared gone. At
// 12.288 MHz this is about 2.7 ms - long enough that a burst of interference
// does not unlock a working input, short enough that a source switching off
// is noticed well inside the module's fade.
const uint32_t kCarrierTimeout = 32768;

// Runs longer than this are not a signal at all - a stuck pin, or a source
// that stopped mid-cell. 32 samples is 16 UI at OVERSAMPLE 2.
const uint32_t kMaxRun = 32;

// Runs to gather before the thresholds are trusted. Preambles are ~1 in 32
// runs, so this is a few hundred microseconds and comfortably more than one
// subframe.
const uint32_t kLearnRuns = 512;

// How often a run is fed to the histogram once the thresholds are known.
//
// **Prime, and a power of two is actively wrong here.** A subframe on digital
// silence is a highly regular ~32-run pattern - 28 identical 2 UI data runs
// plus the preamble - and the only 1 UI runs in the whole stream are inside
// those preambles, at fixed positions. Sampling every 8th run therefore lands
// on the same positions in every subframe and never sees a 1 UI run at all:
// the histogram's shortest populated cluster becomes the 2 UI one, the unit
// doubles, every threshold doubles with it and nothing decodes. Measured with
// 8: unit 8.706 samples where 4.35 was right, and three host checks failed.
//
// 7 is coprime with the subframe period, so the sampled positions rotate
// through every residue instead of standing still. The test is a compare and
// a reset rather than a mask, which costs nothing measurable next to the
// memory traffic it removes.
const uint32_t kLearnEvery = 7;

} // namespace

SpdifRx::SpdifRx() { init(); }

void SpdifRx::init()
{
    reset();
}

void SpdifRx::reset()
{
    m_lastLevel   = 0;
    m_runLen      = 0;
    m_idleSamples = 0;

    for (int i = 0; i < kHistBins; ++i) m_hist[i] = 0;
    m_histCount      = 0;
    m_unit           = 0.0f;
    m_th1            = 0.0f;
    m_th2            = 0.0f;
    m_th1i = m_th2i = m_th3i = 0;
    m_learnPhase     = 0;
    m_haveThresholds = false;

    m_halfBit  = false;
    m_stage    = kHunting;
    m_preTail  = 0;
    m_pending  = kNone;
    m_bits     = 0;
    m_nbits    = 0;

    m_haveLeft = false;
    m_left     = 0;
    m_blockPos = -1;

    m_carrier   = false;
    m_frames    = 0;
    m_parityErr = 0;
    m_resyncs   = 0;

    m_out = 0; m_outMax = 0; m_outCount = 0;
}

void SpdifRx::loseCarrier()
{
    // Drop the framing but **keep the learnt thresholds**. A carrier that
    // flickers is usually the same source coming back, and re-learning costs
    // several hundred microseconds of audio for nothing. They are discarded
    // only by reset(), which the module calls when it decides the input has
    // genuinely changed.
    m_carrier  = false;
    m_stage    = kHunting;
    m_halfBit  = false;
    m_nbits    = 0;
    m_bits     = 0;
    m_haveLeft = false;
    m_blockPos = -1;
}

// ---------------------------------------------------------------------------
//  Threshold learning
// ---------------------------------------------------------------------------

void SpdifRx::learnRun(uint32_t lengthSamples)
{
    // The histogram increment and the kLearnRuns test now live in onRun(),
    // which is the only caller: this function is the *slow* half, reached on
    // roughly one run in 256, and hoisting the fast half out of it removed a
    // call from the hot path.
    (void)lengthSamples;

    // ---- finding the unit length ----------------------------------------
    //
    // **The obvious rule is "the shortest bin holding a real share of the
    // runs", and it fails on digital silence.** All-zero samples are all zero
    // bits, and a zero bit in biphase is one 2 UI run - so silence is ~90 %
    // 2 UI runs, and the 1 UI runs left are only the ones the preambles
    // themselves contain. With a floor of an eighth of the total, the 1 UI
    // cluster falls below it, the 2 UI cluster gets called the unit, every
    // threshold doubles and **nothing decodes at all**.
    //
    // That is not a corner case: it is what a source sends between tracks,
    // which is exactly when this has to keep working. The loopback in
    // spdif_output_module/tx/test_tx.cpp is what found it - neither suite
    // alone had a silent source in it.
    //
    // So use the structure instead of a threshold. Runs cluster at u, 2u and
    // 3u; whichever bin is *most* populated is either the 1 UI or the 2 UI
    // cluster, and the two are told apart by whether anything sits at half
    // its length. Preambles guarantee one or two 1 UI runs per subframe
    // whatever the audio is - B and M contain two each, W one - so that bin
    // is never empty, only sometimes sparse.
    int      best  = -1;
    uint32_t bestN = 0;
    for (int i = 1; i < kHistBins; ++i) {
        if (m_hist[i] > bestN) { bestN = m_hist[i]; best = i; }
    }
    if (best < 0) return;

    // 1/64 is well under the ~5 % the 1 UI cluster falls to on silence, and
    // well over what a stray glitch on a clean line contributes.
    const uint32_t floorCount = m_histCount / 64;

    int first = best;
    const int half = best / 2;
    if (half >= 1 && m_hist[half] >= floorCount) first = half;

    if (first > 0) {
        // Centroid of that bin and its neighbours, so the unit is not
        // quantised to whole samples. At 44.1 kHz and OVERSAMPLE 2 the true
        // figure is 2.18, and rounding it to 2 would put the 3 UI threshold
        // at 5.0 where the real 3 UI run is 6.53 - still right, but with half
        // the margin for no reason.
        // **float, not double.** The M33's FPU is single precision and
        // double is software-emulated - the project's first invariant. This
        // ran every ~256 runs (m_histCount >>= 1 halves 512, not clears it),
        // so at 4.6 M runs/s it was ~18000 emulated-double passes a second.
        // -Wdouble-promotion never caught it: it warns on implicit promotion,
        // not on an explicit double declaration.
        float num = 0.0f, den = 0.0f;
        for (int i = first > 1 ? first - 1 : 1;
             i <= first + 1 && i < kHistBins; ++i) {
            num += (float)i * (float)m_hist[i];
            den += (float)m_hist[i];
        }
        if (den > 0.0f) {
            m_unit = num / den;
            m_th1  = m_unit * 1.5f;
            m_th2  = m_unit * 2.5f;

            // Integer forms for the hot path. ceil, because for integer len
            // (len < m_th1) is exactly (len < ceil(m_th1)).
            m_th1i = (uint32_t)ceilf(m_th1);
            m_th2i = (uint32_t)ceilf(m_th2);
            m_th3i = (uint32_t)ceilf(m_unit * 3.5f);

            m_haveThresholds = true;
        }
    }

    // Decay rather than clear, so the histogram keeps tracking a source that
    // changes rate without starting from nothing.
    for (int i = 0; i < kHistBins; ++i) m_hist[i] >>= 1;
    m_histCount >>= 1;
}

// ---------------------------------------------------------------------------
//  Run -> biphase -> subframe
// ---------------------------------------------------------------------------

void SpdifRx::onPreamble(Preamble p)
{
    // A preamble is the one unambiguous position in the stream, so it is
    // where framing is (re)established rather than merely checked.
    if (m_stage == kData && m_nbits != 28) ++m_resyncs;

    m_pending = p;
    m_bits    = 0;
    m_nbits   = 0;
    m_halfBit = false;
    m_stage   = kPreTail;
    m_preTail = 3;      // three runs left in the 8 UI preamble

    // Block position advances once per *frame*, so it follows the channel A
    // preambles only. Advancing on W as well would run it round twice as
    // fast, which would not show up until something read the channel status.
    if (p == kB)                            m_blockPos = 0;
    else if (p == kM && m_blockPos >= 0)    m_blockPos = (m_blockPos + 1) % 192;
}

void SpdifRx::finishSubframe()
{
    // Even parity over slots 4..31, which is exactly the 28 bits collected.
    uint32_t p = m_bits;
    p ^= p >> 16; p ^= p >> 8; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    const bool parityOk = ((p & 1u) == 0u);

    if (!parityOk) {
        ++m_parityErr;
        // Do not emit, but stay framed: a single corrupted subframe is not a
        // reason to go hunting for preambles again, and dropping framing here
        // would turn one bad bit into a lost frame.
        m_stage = kHunting;
        return;
    }

    // aux nibble is the four LSBs of a 24-bit sample, then 20 main bits.
    // Shifted up by 8 to give 24-bit left-justified in int32, which is what
    // the rest of the firmware means by a sample. The shift is done unsigned
    // and then reinterpreted - bit 23 landing on bit 31 is the sign extension,
    // and shifting a signed value into its own sign bit is undefined.
    const int32_t sample = (int32_t)((m_bits & 0x00FFFFFFu) << 8);

    m_idleSamples = 0;

    if (m_pending == kW) {
        // Channel B completes a frame - but only if we saw its A.
        if (m_haveLeft && m_out && m_outCount < m_outMax) {
            m_out[m_outCount * 2 + 0] = m_left;
            m_out[m_outCount * 2 + 1] = sample;
            ++m_outCount;
            ++m_frames;
        }
        m_haveLeft = false;
    } else {
        m_left     = sample;
        m_haveLeft = true;
    }

    m_carrier = true;
    m_stage   = kHunting;   // the next thing must be a preamble
}

void SpdifRx::onRun(uint32_t lengthSamples, Hot &h)
{
    if (lengthSamples == 0 || lengthSamples > kMaxRun) {
        hotSync(h); loseCarrier(); hotResume(h);
        return;
    }

    // The learn fast path, inlined - it was costing a call on every one of
    // the 4.6 million runs a second this sees.
    //
    // **And it is no longer run on every one of them.** Until the thresholds
    // exist every run is used, so acquisition is unchanged; after that one in
    // kLearnEvery is enough to track a source whose rate is not moving, and
    // it removes six sevenths of 9.2 M memory read-modify-writes a second.
    bool learn = !h.have;
    if (!learn && ++h.learnPhase >= kLearnEvery) { h.learnPhase = 0; learn = true; }

    if (learn && lengthSamples < (uint32_t)kHistBins) {
        ++m_hist[lengthSamples];
        ++m_histCount;
        if (m_histCount >= kLearnRuns) {
            learnRun(lengthSamples);
            // The one place the thresholds can have moved. Refreshed here and
            // nowhere else, which is what makes keeping them in registers for
            // the rest of the call correct rather than merely fast.
            const uint32_t phase = h.learnPhase;
            const uint32_t stage = h.stage, half = h.halfBit;
            hotLoad(h);
            h.learnPhase = phase; h.stage = stage; h.halfBit = half;
        }
    }
    if (!h.have) return;

    // Classify, in integers. Anything past the 3 UI band is not biphase.
    int ui;
    if      (lengthSamples < h.th1) ui = 1;
    else if (lengthSamples < h.th2) ui = 2;
    else if (lengthSamples < h.th3) ui = 3;
    else { hotSync(h); loseCarrier(); hotResume(h); return; }

    switch ((Stage)h.stage) {

    case kHunting:
        // Only a 3 UI run means anything here. Data runs are 1 or 2 UI, so
        // everything else is either the tail of a subframe we are no longer
        // framed to, or noise.
        if (ui == 3) {
            h.stage   = (uint32_t)kPreTail;
            m_preTail = 3;
            m_pending = kNone;
        }
        break;

    case kPreTail:
        // The run straight after the leading 3 names the preamble: 1 -> B,
        // 3 -> M, 2 -> W. The remaining two are swallowed without
        // interpretation - they carry no information the type has not
        // already given us.
        if (m_preTail == 3) {
            Preamble p = kNone;
            if      (ui == 1) p = kB;
            else if (ui == 3) p = kM;
            else              p = kW;
            hotSync(h); onPreamble(p); hotResume(h);
            m_preTail = 2;      // onPreamble reset it; two runs still to go
        } else if (--m_preTail == 0) {
            h.stage = (uint32_t)kData;
        }
        break;

    case kData:
        if (ui == 3) {
            // A preamble arriving mid-subframe means framing was wrong.
            // Take it as truth rather than finishing a subframe we know is
            // broken.
            ++m_resyncs;
            h.stage   = (uint32_t)kPreTail;
            m_preTail = 3;
            h.halfBit = 0u;
        } else if (ui == 2) {
            if (h.halfBit) {
                // A 2 UI run cannot follow a lone 1 UI run in valid biphase.
                h.halfBit = 0u;
                ++m_resyncs;
                h.stage = (uint32_t)kHunting;
            } else {
                onDataBit(0, h);
            }
        } else {                     // 1 UI
            if (h.halfBit) { h.halfBit = 0u; onDataBit(1, h); }
            else             h.halfBit = 1u;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
//  Word-at-a-time run extraction
// ---------------------------------------------------------------------------

int SpdifRx::process(const uint32_t *words, int count,
                     int32_t *out, int maxFrames)
{
    m_out      = out;
    m_outMax   = maxFrames;
    m_outCount = 0;

    Hot h;
    hotLoad(h);

    for (int w = 0; w < count; ++w) {
        const uint32_t word = words[w];

        // Edge map: bit i is set where sample i differs from sample i-1,
        // with sample -1 carried in from the previous word. Doing it a word
        // at a time is what keeps this affordable - the per-sample loop this
        // replaces would be 12.3 M iterations a second.
        const uint32_t prev  = (word << 1) | (m_lastLevel & 1u);
        uint32_t       edges = word ^ prev;

        uint32_t pos = 0;
        while (edges) {
            // Index of the next edge. __builtin_ctz maps to RBIT+CLZ on the
            // M33; the loop runs about three times per word on real audio.
            const uint32_t e = ctz32(edges);
            m_runLen += (e - pos);
            onRun(m_runLen, h);
            m_runLen = 0;
            pos      = e;
            edges   &= edges - 1;
        }

        m_runLen += (32u - pos);
        m_lastLevel = (word >> 31) & 1u;

        // Carrier timeout is counted in samples rather than in runs, or a
        // stuck line would never advance it at all. It is cleared by a
        // subframe passing parity - not by this call having produced a frame,
        // which would make the timeout depend on how the caller chose to
        // chop up its buffer.
        m_idleSamples += 32u;
        if (m_idleSamples >= kCarrierTimeout && m_carrier) loseCarrier();
    }

    hotStore(h);

    m_out = 0;
    return m_outCount;
}
