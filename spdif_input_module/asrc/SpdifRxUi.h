// SpdifRxUi.h - IEC 60958 (S/PDIF) decoder for the per-UI front end.
//
// The PIO side of this is ../spdif_rx_ui.pio, which samples the pin **once
// per UI**, phase-locked to the incoming edges. Read that file first: it is
// where the sampling, the divider and the rate window are argued, and this
// file assumes all of it.
//
// ---- what changed, and why there are two decoders ----------------------
//
// `SpdifRx` is the same job done from a 4x oversampled pin: it extracts runs,
// classifies them against learnt thresholds and walks a biphase state machine
// one run at a time. That decoder is correct, it is the reference this one is
// tested against, and it is **73 % of a 307.2 MHz core** - measured on the
// board, 292 cycles against each of 768000 words a second. The average fits
// inside the block deadline and the peak does not.
//
// The cost is not in any one line of it. It is structural: the work is per
// *run*, and biphase produces about 0.75 runs per UI whatever you sample it
// at, so 4.6 million of them a second arrive whatever the oversampling is.
// No constant in that file can change that.
//
// This decoder is the other side of the split. The front end hands over one
// bit per UI, so:
//
//   * a 32-bit word is **32 UI** rather than 8, and the word rate is 4 x Fs -
//     192000/s at 48 kHz against 768000;
//   * a subframe is 64 UI, so it is **exactly two words**, and decoding one is
//     word-parallel bit arithmetic with no loop in it at all;
//   * there are **no thresholds to learn**. The front end does not measure run
//     lengths, so there is no histogram, no acquisition transient, and no
//     bistable "too slow to ever get fast" basin for the module to work
//     around.
//
// ---- the format, in the order this file deals with it -------------------
//
// **Biphase mark.** Each bit cell is 2 UI with a transition at every cell
// boundary; a `1` adds a transition in the middle. Sampled once per UI, a
// cell is two levels: equal is a `0`, different is a `1`. That is the whole
// data decode - one XOR of the stream against itself shifted by one UI.
//
// **Preambles** violate biphase and are 8 UI long. Written as the UI levels
// they produce, with `a` for the level the preamble starts on:
//
//     B (Z)  3,1,1,3   a a a . a . . .      block start, and channel A
//     M (X)  3,3,1,1   a a a . . . a .      channel A (left)
//     W (Y)  3,2,1,2   a a a . . a . .      channel B (right)
//
// Three distinct bytes, and six with both polarities. **That is the sync
// mechanism**: nothing else in the stream produces three like UI in a row,
// because data runs are never longer than 2 UI. A preamble is found by
// comparing one byte against six constants, and framing is confirmed by the
// rest of the subframe decoding rather than by the byte alone.
//
// **Subframe**: 32 time slots of 2 UI, so 64 UI. Slots 0-3 are the preamble.
// Then, LSB first: 4 aux bits, 20 audio bits, then V, U, C and P. Parity is
// even over slots 4-31, so the 28 bits collected must XOR to zero.
//
// A 24-bit sample is the aux nibble as the four LSBs plus the 20 main bits
// above it, shifted up by 8 to give 24-bit left-justified in int32 - what the
// rest of the firmware means by a sample.
//
// ---- the two checks, and why the second one is the load-bearing one -----
//
// Parity catches one bad bit in a subframe. It does **not** catch a framing
// error, because a subframe read at the wrong offset is 28 bits of something
// and has an even chance of passing.
//
// So the structural check is separate and much stronger: **every bit cell
// begins with a transition**, and there are 28 of them in a subframe. Read at
// the wrong offset, or with a UI inserted or dropped, that pattern breaks
// almost immediately. Both are one mask and one compare here, because the
// transition map is computed for the data decode anyway.
//
// That is what makes hunting safe. A candidate preamble byte is one in forty
// by chance; a candidate that also carries 28 cell boundaries and correct
// parity is not.

#ifndef NOVADSP_SPDIFRXUI_H
#define NOVADSP_SPDIFRXUI_H

#include <stdint.h>

// UI per subframe. 32 slots of 2 UI; the preamble is four of those slots.
#define SPDIF_UI_PER_SUBFRAME 64

// UI with nothing decodable before the carrier is declared gone.
//
// **Counted in UI, not in samples**, which is the same thing said in units
// that do not change when the front end does - the figure in SpdifRx.cpp is
// in oversampled samples and its own comment went stale when OVERSAMPLE moved
// from 2 to 4. 8192 UI is 64 frames: 1.33 ms at 48 kHz, 1.45 at 44.1, 2.0 at
// 32. Long enough that a burst of interference does not unlock a working
// input, short enough to be noticed well inside the module's fade.
#define SPDIF_UI_CARRIER_TIMEOUT 8192u

// Consecutive subframes that may fail the structural check before framing is
// abandoned and the hunt restarts.
//
// **One is too few.** A single dropped UI - a glitch on the fibre, or the RX
// FIFO overflowing because fill() ran late - breaks one subframe and leaves
// the alignment shifted, so the next one fails too. Two in a row is the
// signal; a lone corrupted subframe is dropped and framing is kept, exactly
// as SpdifRx does with a parity failure.
#define SPDIF_UI_FRAMING_LOSS 2

class SpdifRxUi
{
public:
    enum Preamble { kNone = 0, kB, kM, kW };

    SpdifRxUi();

    void init();
    void reset();

    // Feed UI levels as PIO pushed them: the first UI of a word in bit 0, the
    // last in bit 31.
    //
    // Decoded stereo frames are written to `out` as interleaved L,R, 24-bit
    // left-justified in int32. Returns frames produced, never more than
    // maxFrames - **a caller that supplies too small a buffer loses audio**,
    // so size it from the word count: one frame per 128 UI, which is four
    // words.
    int process(const uint32_t *words, int count, int32_t *out, int maxFrames);

    // True while subframes are arriving and passing both checks. Goes false
    // after SPDIF_UI_CARRIER_TIMEOUT UI with nothing decodable, which is what
    // the module's mute policy hangs off.
    bool carrier() const { return m_carrier; }

    // True while the subframe alignment is established. **This is the figure
    // the module scans the clock divider against**: a front end tuned for the
    // wrong rate produces UI that are not UI, and the one thing that can
    // never happen then is 28 cell boundaries in a row.
    bool framed() const { return m_framed; }

    // 0..191, the position in the channel-status block. Only meaningful once
    // a B preamble has been seen.
    int  blockPos() const { return m_blockPos; }

    uint32_t framesDecoded() const { return m_frames; }
    uint32_t parityErrors()  const { return m_parityErr; }
    uint32_t resyncs()       const { return m_resyncs; }

    // Kept for the diagnostic line, which prints it for both decoders. There
    // is no learnt unit here - the front end is told the rate rather than
    // measuring it - so this reports the *nominal* UI in state machine cycles
    // while framed, and 0 while not. It is a lock lamp, not a measurement,
    // and the header of the report says so.
    float unitSamples() const;

private:
    enum Result { kOk, kBadParity, kBadFraming };

    Result decodeSubframe(uint64_t v);
    void   emit(Preamble p, int32_t sample);
    void   loseFraming();
    void   loseCarrier();

    // ---- bit plumbing ----
    //
    // A subframe is 64 UI and a word is 32, so the alignment between the two
    // is a bit offset that has to be found once and then held. `m_acc` holds
    // up to 64 UI with the oldest in bit 0; the hunt shifts it one UI at a
    // time and a decode consumes all 64.
    uint64_t m_acc;
    int      m_have;

    bool     m_framed;
    int      m_badRun;          // consecutive structural failures

    uint32_t m_idleUi;          // UI since anything decodable happened

    // ---- frame assembly ----
    bool     m_haveLeft;
    int32_t  m_left;
    int      m_blockPos;

    bool     m_carrier;
    uint32_t m_frames;
    uint32_t m_parityErr;
    uint32_t m_resyncs;

    // Output plumbing for the current process() call.
    int32_t *m_out;
    int      m_outMax;
    int      m_outCount;
};

#endif // NOVADSP_SPDIFRXUI_H
