// SpdifRxUi.cpp - see SpdifRxUi.h for the format and for why this decoder
// exists alongside SpdifRx.

#include "SpdifRxUi.h"

namespace {

// The six preamble bytes: three types, two polarities. UI 0 is bit 0.
//
//     B  a a a . a . . .   0x17     ~B  0xE8
//     M  a a a . . . a .   0x47     ~M  0xB8
//     W  a a a . . a . .   0x27     ~W  0xD8
//
// **Derived in the header from the run lengths.** A transcription error here
// cannot pass the suite: test_spdif_ui.cpp builds its stream from the run
// tables in spdif_test_source.h, so a wrong B never starts a block, a wrong M
// stops blockPos advancing, and a wrong W stops frames being completed at all.
const uint8_t kPreB = 0x17, kPreM = 0x47, kPreW = 0x27;

// **This runs once per UI while hunting** - 32 times a word, against a word
// rate of 192000 a second - so it is written to reject early and to compare
// three constants rather than six.
//
// The early reject is the one thing all three preambles share and nothing else
// in the stream has: three like UI followed by one that differs. That is the
// 3 UI run, and data runs never reach it. It throws out seven bytes in eight
// for a shift, an and and a compare.
//
// The polarity is then folded out rather than doubled up. UI 0 is the level
// the preamble starts on, so a byte whose bit 0 is clear is the inverse of one
// of the three canonical patterns.
inline SpdifRxUi::Preamble classify(uint8_t b)
{
    if ((uint8_t)((b ^ (b >> 1)) & 0x07u) != 0x04u) return SpdifRxUi::kNone;

    const uint8_t p = (b & 1u) ? b : (uint8_t)~b;
    if (p == kPreB) return SpdifRxUi::kB;
    if (p == kPreM) return SpdifRxUi::kM;
    if (p == kPreW) return SpdifRxUi::kW;
    return SpdifRxUi::kNone;
}

inline bool isPreambleByte(uint8_t b)
{
    return classify(b) != SpdifRxUi::kNone;
}

// Gather the even-numbered bits of a word: bit 0 stays, bit 2 becomes bit 1,
// bit 4 becomes bit 2, and so on, giving 16 bits from 32.
//
// Five shift-or-mask stages, no loop and no table. **A table would be the
// obvious speed-up and is not one here**: eight byte lookups plus their shifts
// is about the same instruction count, and it puts a 256-byte array in a
// decoder whose whole point is that it touches almost no memory.
inline uint32_t gather_even(uint32_t x)
{
    x &= 0x55555555u;
    x = (x | (x >> 1))  & 0x33333333u;
    x = (x | (x >> 2))  & 0x0F0F0F0Fu;
    x = (x | (x >> 4))  & 0x00FF00FFu;
    x = (x | (x >> 8))  & 0x0000FFFFu;
    return x;
}

} // namespace

SpdifRxUi::SpdifRxUi() { init(); }

void SpdifRxUi::init() { reset(); }

void SpdifRxUi::reset()
{
    m_acc      = 0;
    m_have     = 0;
    m_framed   = false;
    m_badRun   = 0;
    m_idleUi   = 0;

    m_haveLeft = false;
    m_left     = 0;
    m_blockPos = -1;

    m_carrier   = false;
    m_frames    = 0;
    m_parityErr = 0;
    m_resyncs   = 0;

    m_out = 0; m_outMax = 0; m_outCount = 0;
}

float SpdifRxUi::unitSamples() const
{
    // The front end's cycles per UI, reported only while framed. See the
    // header: a lock lamp rather than a measurement.
    return m_framed ? 32.0f : 0.0f;
}

void SpdifRxUi::loseFraming()
{
    m_framed   = false;
    m_badRun   = 0;
    m_haveLeft = false;
    ++m_resyncs;
}

void SpdifRxUi::loseCarrier()
{
    m_carrier  = false;
    m_framed   = false;
    m_badRun   = 0;
    m_haveLeft = false;
    m_blockPos = -1;
}

// ---------------------------------------------------------------------------
//  One subframe, 64 UI, no loop
// ---------------------------------------------------------------------------

SpdifRxUi::Result SpdifRxUi::decodeSubframe(uint64_t v)
{
    const Preamble p = classify((uint8_t)v);
    if (p == kNone) return kBadFraming;

    // X[i] is the transition between UI i and UI i+1. Bits 0..62 are real;
    // bit 63 has no partner inside the word and is masked off below by never
    // being read.
    const uint64_t X = v ^ (v >> 1);

    // Shift so that the boundary transition *into* cell 0 lands in bit 0.
    // After that, Y alternates: even bits are cell boundaries, odd bits are
    // the cells' own data.
    //
    //     Y[0]  boundary into cell 0      Y[1]  cell 0's bit
    //     Y[2]  boundary into cell 1      Y[3]  cell 1's bit
    //     ...
    //     Y[54] boundary into cell 27     Y[55] cell 27's bit (P)
    const uint64_t Y = X >> 7;

    const uint32_t ylo = (uint32_t)Y;             // cells 0..15
    const uint32_t yhi = (uint32_t)(Y >> 32);     // cells 16..27, 24 bits used

    // **The structural check.** Every one of the 28 cell boundaries must carry
    // a transition. This is what tells a framing error from a bit error, and
    // it is what makes the hunt below safe.
    if ((ylo & 0x55555555u) != 0x55555555u) return kBadFraming;
    if ((yhi & 0x00555555u) != 0x00555555u) return kBadFraming;

    // Even parity over slots 4..31 - the 28 data bits, which are the odd bits
    // of Y. Folded rather than counted; ARMv8-M has no popcount.
    uint32_t par = (ylo & 0xAAAAAAAAu) ^ (yhi & 0x00AAAAAAu);
    par ^= par >> 16; par ^= par >> 8; par ^= par >> 4; par ^= par >> 2;
    par ^= par >> 1;
    if (par & 1u) return kBadParity;

    // The sample is cells 0..23: the aux nibble as the four LSBs, then 20
    // audio bits. Shifted up by 8 for 24-bit left-justified in int32. The
    // shift is done unsigned and then reinterpreted - bit 23 landing on bit 31
    // is the sign extension, and shifting a signed value into its own sign bit
    // is undefined.
    const uint32_t lo16 = gather_even(ylo >> 1);             // cells 0..15
    const uint32_t hi8  = gather_even(yhi >> 1) & 0xFFu;     // cells 16..23
    const int32_t  sample = (int32_t)(((lo16 | (hi8 << 16)) & 0x00FFFFFFu) << 8);

    emit(p, sample);
    return kOk;
}

void SpdifRxUi::emit(Preamble p, int32_t sample)
{
    // Block position advances once per *frame*, so it follows the channel A
    // preambles only. Advancing on W as well would run it round twice as
    // fast, which would not show up until something read the channel status.
    if      (p == kB)                       m_blockPos = 0;
    else if (p == kM && m_blockPos >= 0)    m_blockPos = (m_blockPos + 1) % 192;

    if (p == kW) {
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
    m_idleUi  = 0;
}

// ---------------------------------------------------------------------------
//  Words in, frames out
// ---------------------------------------------------------------------------

int SpdifRxUi::process(const uint32_t *words, int count,
                       int32_t *out, int maxFrames)
{
    m_out      = out;
    m_outMax   = maxFrames;
    m_outCount = 0;

    for (int w = 0; w < count; ++w) {
        uint32_t src  = words[w];
        int      left = 32;         // UI of this word not yet in m_acc

        // Counted per word rather than per decode, or a stuck line - which
        // produces words perfectly steadily and decodes none of them - would
        // never advance it at all.
        m_idleUi += 32u;
        if (m_idleUi >= SPDIF_UI_CARRIER_TIMEOUT && m_carrier) loseCarrier();

        for (;;) {
            // ---- top up ------------------------------------------------
            //
            // m_acc never holds more than 64 UI, so this moves the word
            // across in at most two passes.
            if (left > 0 && m_have < 64) {
                int n = 64 - m_have;
                if (n > left) n = left;

                const uint32_t chunk = (n >= 32) ? src : (src & ((1u << n) - 1u));
                m_acc  |= (uint64_t)chunk << m_have;
                m_have += n;
                src     = (n >= 32) ? 0u : (src >> n);
                left   -= n;
            }

            // ---- hunt --------------------------------------------------
            //
            // Only reached when framing is not established. A preamble byte
            // is one candidate in about forty by chance, so a match is not
            // taken as a lock: the whole subframe has to decode, all 28 cell
            // boundaries and parity included, before the alignment is kept.
            if (!m_framed) {
                while (m_have >= 8 && !isPreambleByte((uint8_t)m_acc)) {
                    m_acc >>= 1;
                    --m_have;
                }
                if (m_have < 64) {
                    if (left == 0) break;
                    continue;               // need more UI to confirm
                }
                if (decodeSubframe(m_acc) != kOk) {
                    m_acc >>= 1;            // false alarm: keep hunting
                    --m_have;
                    continue;
                }
                m_framed = true;
                m_badRun = 0;
                m_acc    = 0;
                m_have   = 0;
                continue;
            }

            // ---- framed ------------------------------------------------
            if (m_have < 64) {
                if (left == 0) break;
                continue;
            }

            switch (decodeSubframe(m_acc)) {
            case kOk:
                m_badRun = 0;
                break;

            case kBadParity:
                // One bad bit. Do not emit, but **stay framed** - dropping
                // the alignment here would turn one corrupted bit into a lost
                // hunt, and the structural check already passed, which is the
                // evidence that the alignment is still right.
                ++m_parityErr;
                m_badRun = 0;
                break;

            case kBadFraming:
                if (++m_badRun >= SPDIF_UI_FRAMING_LOSS) {
                    loseFraming();
                    m_acc >>= 1;            // resume hunting one UI along
                    --m_have;
                    continue;
                }
                break;
            }

            m_acc  = 0;
            m_have = 0;
        }
    }

    m_out = 0;
    return m_outCount;
}
