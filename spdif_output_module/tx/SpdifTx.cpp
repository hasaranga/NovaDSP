// SpdifTx.cpp - see SpdifTx.h for the geometry that makes this a table lookup.

#include "SpdifTx.h"

namespace {

// Preambles as run lengths in UI. Identical to the receiver's table, and to
// spdif_input_module/asrc/spdif_test_source.h's - three copies of the same
// four rows, which is three too many, but the alternative is a module
// including another module's header. The loopback test is what holds them
// together: if this table and the receiver's ever disagree, nothing decodes.
const int kRunsB[4] = { 3, 1, 1, 3 };   // block start, channel A
const int kRunsM[4] = { 3, 3, 1, 1 };   // channel A
const int kRunsW[4] = { 3, 2, 1, 2 };   // channel B

// ---- channel status, consumer format (IEC 60958-3) ----------------------
//
// 192 bits, one per frame, sent LSB first from byte 0. Most receivers ignore
// most of it; the sample rate and word length are the two fields that change
// what a DAC does, so they are the two set deliberately here.
//
//   byte 0  bit 0 = 0   consumer
//           bit 1 = 0   linear PCM
//           bit 2 = 1   no copyright asserted
//           bits 3-5    pre-emphasis, 000 = none
//   byte 3  bits 0-3    sampling frequency: 0100 = 48 kHz
//   byte 4  bit 0 = 1   maximum word length 24 bits
//           bits 1-3    word length, 101 = 24 bits
//
// **This is write-only and nothing in this tree reads it back.** SpdifRx does
// not parse channel status - it only needs the preambles - so the loopback
// test proves the framing and the audio, and these five bytes are checked by
// reading them rather than by a round trip. Getting the rate field wrong
// presents as a DAC that locks and plays at the wrong speed, or refuses.
const unsigned char kChannelStatus[24] = {
    0x04,   // consumer, PCM, no copyright
    0x00,   // category: general
    0x00,   // source and channel number: do not care
    0x02,   // 48 kHz
    0x0B,   // 24-bit word length
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

} // namespace

SpdifTx::SpdifTx() { init(); }

void SpdifTx::init()
{
    buildTables();
    reset();
}

void SpdifTx::reset()
{
    m_level    = 0;
    m_blockPos = 0;
}

void SpdifTx::buildTables()
{
    // ---- biphase mark ---------------------------------------------------
    //
    // Every cell boundary transitions; a 1 adds a transition in the middle.
    // So starting from level L, a bit b emits (!L) then (b ? L : !L), and the
    // level going into the next cell is whatever the second UI was.
    for (int lvl = 0; lvl < 2; ++lvl) {
        for (int n = 0; n < 16; ++n) {
            unsigned pattern = 0;
            int L = lvl;
            for (int b = 0; b < 4; ++b) {
                const int bit = (n >> b) & 1;
                L ^= 1;                                  // cell boundary
                pattern |= (unsigned)L << (b * 2);       // first UI
                if (bit) L ^= 1;                         // mid-cell
                pattern |= (unsigned)L << (b * 2 + 1);   // second UI
            }
            m_bmc[lvl][n] = (uint8_t)pattern;
        }
    }

    // ---- preambles ------------------------------------------------------
    //
    // Not biphase - that is the point of them. Each is four runs whose
    // lengths sum to 8 UI, and every run starts with a transition.
    const int *runs[3] = { kRunsB, kRunsM, kRunsW };
    for (int t = 0; t < 3; ++t) {
        for (int lvl = 0; lvl < 2; ++lvl) {
            unsigned pattern = 0;
            int L = lvl;
            int ui = 0;
            for (int r = 0; r < 4; ++r) {
                L ^= 1;                                  // every run transitions
                for (int i = 0; i < runs[t][r]; ++i)
                    pattern |= (unsigned)L << (ui++);
            }
            m_pre[t][lvl] = (uint8_t)pattern;
        }
    }
}

int SpdifTx::encode(const int32_t *in, int frames, uint32_t *words, int maxWords)
{
    if (!in || !words || frames <= 0) return 0;
    if (maxWords < frames * SPDIF_TX_WORDS_PER_FRAME) return 0;

    int w = 0;

    for (int f = 0; f < frames; ++f) {
        // One channel status bit per frame, LSB first from byte 0.
        const int cbit = (kChannelStatus[m_blockPos >> 3] >> (m_blockPos & 7)) & 1;

        for (int ch = 0; ch < 2; ++ch) {
            // Channel A carries B at the top of a block and M otherwise;
            // channel B always carries W.
            const int pre = (ch == 1) ? 2 : (m_blockPos == 0 ? 0 : 1);

            // 24-bit sample, from 24-bit left-justified in int32.
            const uint32_t s24 = ((uint32_t)in[(f * 2) + ch] >> 8) & 0x00FFFFFFu;

            // Slots 4..31, LSB first: 4 aux + 20 audio (together the 24-bit
            // sample), then V, U, C, P. V=0 means the sample is valid.
            uint32_t bits = s24
                          | (0u          << 24)   // V - valid
                          | (0u          << 25)   // U - user data
                          | ((uint32_t)cbit << 26);

            // Even parity over slots 4..31 inclusive, so the 28 bits below
            // must XOR to zero. P is bit 27.
            uint32_t p = bits;
            p ^= p >> 16; p ^= p >> 8; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
            bits |= (p & 1u) << 27;

            const uint8_t preamble = m_pre[pre][m_level];
            m_level = (uint8_t)((preamble >> 7) & 1u);

            uint32_t w0 = preamble;
            for (int n = 0; n < 3; ++n) {
                const uint8_t b = m_bmc[m_level][(bits >> (n * 4)) & 0xFu];
                w0 |= (uint32_t)b << (8 * (n + 1));
                m_level = (uint8_t)((b >> 7) & 1u);
            }

            uint32_t w1 = 0;
            for (int n = 3; n < 7; ++n) {
                const uint8_t b = m_bmc[m_level][(bits >> (n * 4)) & 0xFu];
                w1 |= (uint32_t)b << (8 * (n - 3));
                m_level = (uint8_t)((b >> 7) & 1u);
            }

            words[w++] = w0;
            words[w++] = w1;
        }

        m_blockPos = (m_blockPos + 1) % 192;
    }

    return w;
}
