// SpdifTx.h - IEC 60958 transmitter: stereo frames to biphase-mark UI bits.
//
// ---- this is the easy half of S/PDIF, and the reason is the clock --------
//
// The receiver in spdif_input_module exists to solve a hard problem: a source
// brings its own clock, the RP2350 has no steerable audio PLL, and the two
// drift permanently. **None of that applies here.** Transmitting, the box is
// the master. The rate is our own 48 kHz, the divider is exact, and there is
// nothing to track:
//
//     UI rate = 128 * 48000 = 6.144 MHz
//     clkdiv  = 307200000 / 6144000 = 50 exactly
//
// So this module has no ASRC, no rate estimator, no lock state and no
// continuity policy. It encodes what it is given and shifts it out.
//
// ---- the split is the same one the receiver makes ------------------------
//
// The PIO program is one instruction (`out pins, 1`) and everything else is
// here, in C that a host test can drive. See ../spdif_tx.pio.
//
// ---- geometry, which is what makes the assembly cheap --------------------
//
// A subframe is 4 preamble time slots plus 28 data bits; biphase turns each
// data bit into 2 UI, so a subframe is 8 + 56 = **64 UI, exactly two 32-bit
// words**, and a frame is exactly four. Nothing carries between frames and no
// bit accumulator is needed:
//
//     word 0 = preamble(8) | nibble0(8) | nibble1(8) | nibble2(8)
//     word 1 = nibble3(8)  | nibble4(8) | nibble5(8) | nibble6(8)
//
// Seven nibbles of 4 data bits each, 8 UI apiece. Encoding is therefore a
// table lookup per nibble - 14 per frame - rather than a loop over 56 bits.
//
// **First UI is bit 0 of the word**, matching the PIO shift direction.

#ifndef NOVADSP_SPDIFTX_H
#define NOVADSP_SPDIFTX_H

#include <stdint.h>

// UI per frame: 2 subframes * 32 time slots * 2 UI per slot.
#define SPDIF_TX_UI_PER_FRAME  128
#define SPDIF_TX_WORDS_PER_FRAME (SPDIF_TX_UI_PER_FRAME / 32)

class SpdifTx
{
public:
    SpdifTx();

    // Builds the biphase and preamble tables and starts a channel status
    // block. Call once before audio starts.
    void init();

    // Restart the 192-frame channel status block and the line polarity.
    // Encoding is stateless apart from those two.
    void reset();

    // Encode `frames` interleaved stereo frames - 24-bit left-justified in
    // int32, the format the rest of the firmware uses - into `words`.
    //
    // Writes exactly frames * SPDIF_TX_WORDS_PER_FRAME words and returns that
    // count, or 0 if the buffer is too small. There is no partial encode:
    // a frame is four words or it is nothing.
    int encode(const int32_t *in, int frames, uint32_t *words, int maxWords);

    // Position in the 192-frame channel status block, 0..191.
    int blockPos() const { return m_blockPos; }

private:
    void buildTables();

    // kBmc[level][nibble] - 8 UI for four data bits, given the line level
    // going in. The level coming out is bit 7 of the pattern, so it does not
    // need storing.
    uint8_t m_bmc[2][16];

    // kPre[type][level] - 8 UI for each preamble, given the level going in.
    // Types are 0 = B (block start), 1 = M (channel A), 2 = W (channel B).
    uint8_t m_pre[3][2];

    uint8_t m_level;      // line level going into the next UI
    int     m_blockPos;
};

#endif // NOVADSP_SPDIFTX_H
