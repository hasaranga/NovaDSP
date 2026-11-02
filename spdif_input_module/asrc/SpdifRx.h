// SpdifRx.h - IEC 60958 (S/PDIF) decoder, from oversampled pin levels to
// stereo frames.
//
// **This is no longer the shipped decoder.** SpdifRxUi is, and this one is the
// reference implementation it was built against - see SpdifRxUi.h for why, and
// SPDIF_USE_PIO_DECODE in SpdifInput.h for how to select either. Everything
// below is still true of this decoder and this decoder still passes its 31
// host checks; what changed is which front end the module runs.
//
// The short version of why: this decoder's cost is per *run*, and biphase
// produces about 0.75 runs per UI whatever the oversampling is - 4.6 million
// a second - so no constant in this file could ever have changed the 73 % of a
// core it measured at. The replacement does not extract runs at all.
//
// The PIO side of this is one instruction: it oversamples the input pin and
// pushes raw levels (see ../spdif_rx.pio for why the split is there and not
// where a practical receiver would normally put it). Everything below is the
// decode, and all of it is host-testable.
//
// ---- the format, in the order this file deals with it -------------------
//
// **Biphase mark.** Each bit cell is 2 UI with a transition at every cell
// boundary; a `1` adds a transition in the middle. So a `0` is one run of
// 2 UI and a `1` is two runs of 1 UI. Data therefore never produces a run
// longer than 2 UI - which is what makes the preambles findable.
//
// **Preambles** deliberately violate biphase and are 8 UI long. As run
// lengths, and independent of polarity:
//
//     B (Z)  3,1,1,3   start of a 192-frame block, and of channel A
//     M (X)  3,3,1,1   channel A (left)
//     W (Y)  3,2,1,2   channel B (right)
//
// Two things fall out of that table and both are load-bearing here:
//
//   * **A run of 3 UI can only be a preamble.** That is the whole sync
//     mechanism; nothing else in the stream produces one.
//   * **The run immediately after it identifies which preamble it is** - 1
//     for B, 3 for M, 2 for W. No pattern matching over the full 8 UI is
//     needed.
//
// **Subframe**: 32 time slots. Slots 0-3 are the preamble and carry no data.
// Then, LSB first: 4 aux bits, 20 audio bits, then V, U, C and P. Parity is
// even over slots 4-31 inclusive, so the 28 bits this decoder collects must
// XOR to zero.
//
// A 24-bit sample is the aux nibble as the four LSBs plus the 20 main bits
// above it. Output is 24-bit left-justified in int32, matching what the rest
// of the firmware means by a sample.
//
// ---- thresholds are learnt, not declared --------------------------------
//
// The PIO divider is fixed - one sample clock for every input rate - so a
// 48 kHz UI is 2.00 samples, a 44.1 kHz UI is 2.18 and a 32 kHz UI is 3.00.
// Rather than switching dividers per rate and needing to know the rate before
// decoding anything, the decoder **builds a histogram of run lengths and puts
// its thresholds at 1.5x and 2.5x the shortest cluster it finds**. One
// configuration then covers every supported rate, and there is no
// chicken-and-egg with the rate estimator.
//
// The technique is netham45/esp32-spdif's, which is AGPL and was read for the
// approach only.

#ifndef NOVADSP_SPDIFRX_H
#define NOVADSP_SPDIFRX_H

#include <stdint.h>

// Samples per UI at 48 kHz. ../spdif_rx.pio derives its divider from this:
// 4 gives 24.576 MHz, exactly clk_sys/12.5.
//
// **4 rather than 2, and the reason is not the one you would expect.** The
// obvious trade is jitter margin against CPU, and the measured margin is
// clear enough - test_spdif.cpp drives an edge-jitter sweep:
//
//     oversample   clean to      breaks at
//     2            +-0.05 UI     +-0.10 UI  (31 resyncs / 1500 frames)
//     4            +-0.10 UI     +-0.20 UI  (466 resyncs / 1500 frames)
//
// But the CPU half of that trade barely exists, because **the cost is
// dominated by runs per second, and that is a property of the signal**:
// biphase produces about 0.75 runs per UI whatever rate you sample it at, so
// ~4.6 M runs a second either way. Doubling the oversampling doubles only the
// per-word overhead, which is a small fraction of the total.
//
// So 4 buys twice the jitter margin for a few percent, and the only real
// costs are DMA bandwidth (3 MB/s) and the ring buffer being twice the size.
// **The measured cost at 4 is 492 cycles per word**, which at the 768000
// words/s this produces is 123 % of a 307.2 MHz core - more than the whole
// chip, before the effects and the FFT get any. That is four times the ~30 %
// that was estimated here, and it is the figure this module's future turns on.
//
// **2 was tried on the board and is worse, not cheaper.** Halving the word
// rate to 384000/s is the obvious saving and it is not what happens: the board
// went silent - no console, no LED, core 1 stalled - and unlike at 4 it could
// no longer even service picotool's reset request, so recovering it needed the
// BOOTSEL button. Core 0 was saturated past the point of servicing the USB
// interrupt.
//
// No cyc/word figure was captured at 2, so *how much* worse is not known. The
// likely reason is the table above rather than anything new: real optical
// jitter exceeds the +-0.05 UI that 2 is clean to, and a decoder in permanent
// resync costs far more per run than the words it saves. That is consistent
// with this header's own claim that the cost is dominated by runs per second -
// a property of the signal, which no amount of oversampling changes.
//
// The way out is therefore not a constant in this file. It is the split
// ../spdif_rx.pio names as the fallback: decode biphase-mark in PIO and push
// one word per subframe, 88200 words/s at 44.1 kHz instead of 768000.
#define SPDIF_OVERSAMPLE 4

class SpdifRx
{
public:
    enum Preamble { kNone = 0, kB, kM, kW };

    SpdifRx();

    void init();
    void reset();

    // Feed oversampled pin levels. `words` are as PIO pushed them: the first
    // sample in bit 0, the last in bit 31.
    //
    // Decoded stereo frames are written to `out` as interleaved L,R, 24-bit
    // left-justified in int32. Returns frames produced, never more than
    // maxFrames - **a caller that supplies too small a buffer loses audio**,
    // so size it from the word count: one frame per 128*OVERSAMPLE samples.
    int process(const uint32_t *words, int count, int32_t *out, int maxFrames);

    // True while subframes are arriving and passing parity. Goes false after
    // kCarrierTimeout samples with nothing decodable, which is what the
    // module's mute policy hangs off.
    bool carrier() const { return m_carrier; }

    // Interchangeable with SpdifRxUi's, so that SpdifInput can hold either.
    // There is no separate framing state in this decoder - thresholds plus a
    // carrier is the same claim - so it is derived rather than tracked.
    bool framed() const { return m_haveThresholds && m_carrier; }

    // 0..191, the position in the channel-status block. Only meaningful once
    // a B preamble has been seen.
    int  blockPos() const { return m_blockPos; }

    uint32_t framesDecoded() const { return m_frames; }
    uint32_t parityErrors()  const { return m_parityErr; }
    uint32_t resyncs()       const { return m_resyncs; }

    // The shortest run the histogram is currently settled on, in samples.
    // 2.0 means a 48 kHz source at OVERSAMPLE 2. Reported for the banner -
    // it is the one number that says the front end is looking at a real
    // signal rather than at noise.
    float unitSamples() const { return m_unit; }

private:
    // Hot, read-mostly decoder state, hoisted into locals for the duration of
    // one process() call.
    //
    // **The compiler cannot do this itself.** The classifier reads
    // m_haveThresholds, m_learnPhase and m_th1i on every single run - five
    // memory accesses before it has even dispatched on the stage - and GCC has
    // to reload them each time because the cold paths out of the same switch
    // call learnRun() and finishSubframe(), either of which could write the
    // object. Confirmed in the disassembly at 325 cycles a word: ldrb [r0,#92],
    // ldr/str [r0,#108], ldr [r0,#96], ldrb [r0,#113], every run.
    //
    // The thresholds only move inside learnRun(), which runs ~2600 times a
    // second against 4.66 million runs. So they are loaded once per call, kept
    // in registers (this struct never escapes an inlined callee), and refreshed
    // only where they can actually have changed.
    struct Hot {
        uint32_t th1, th2, th3;
        uint32_t have;              // m_haveThresholds, as a word
        uint32_t learnPhase;
        uint32_t stage;             // m_stage, read and written every run
        uint32_t halfBit;           // m_halfBit, likewise
    };

    inline void hotLoad(Hot &h) const
    {
        h.th1 = m_th1i; h.th2 = m_th2i; h.th3 = m_th3i;
        h.have = m_haveThresholds ? 1u : 0u;
        h.learnPhase = m_learnPhase;
        h.stage = (uint32_t)m_stage;
        h.halfBit = m_halfBit ? 1u : 0u;
    }
    inline void hotStore(const Hot &h)
    {
        m_learnPhase = h.learnPhase;
        m_stage      = (Stage)h.stage;
        m_halfBit    = h.halfBit != 0u;
    }

    // Round trip for the cold paths. onPreamble(), finishSubframe() and
    // loseCarrier() all read and write m_stage / m_halfBit, so the hoisted
    // copies have to be handed back before calling one and picked up after.
    inline void hotSync(Hot &h)   { m_stage = (Stage)h.stage;
                                    m_halfBit = h.halfBit != 0u; }
    inline void hotResume(Hot &h) { h.stage = (uint32_t)m_stage;
                                    h.halfBit = m_halfBit ? 1u : 0u; }

    void  onRun(uint32_t lengthSamples, Hot &h);
    void  onPreamble(Preamble p);
    void  finishSubframe();

    // Inline, and deliberately: 28 data bits a subframe at 88200 subframes a
    // second is ~2.5 M calls, and the body is three instructions plus a
    // compare. The call overhead was a measurable share of it.
    inline void onDataBit(uint32_t bit, Hot &h)
    {
        if (m_nbits < 28) {
            m_bits |= (bit & 1u) << m_nbits;
            if (++m_nbits == 28) {
                // finishSubframe() sets m_stage, so the hoisted copy has to
                // round-trip across it.
                hotSync(h);
                finishSubframe();
                hotResume(h);
            }
        }
    }

    void  learnRun(uint32_t lengthSamples);
    void  loseCarrier();

    // ---- run extraction ----
    uint32_t m_lastLevel;      // 0 or 1, level of the previous sample
    uint32_t m_runLen;         // samples in the run so far
    uint32_t m_idleSamples;    // since anything decodable happened

    // ---- threshold learning ----
    //
    // A small histogram of short run lengths. The shortest well-populated bin
    // is 1 UI; everything else follows from it.
    static const int kHistBins = 16;
    uint32_t m_hist[kHistBins];
    uint32_t m_histCount;
    float    m_unit;           // samples per UI, learnt
    float    m_th1;            // boundary between 1 UI and 2 UI runs
    float    m_th2;            // boundary between 2 UI and 3 UI runs
    bool     m_haveThresholds;

    // The same three boundaries as integers, which is what the classifier
    // actually compares against. **Run lengths are whole samples**, so
    // (len < m_th1) for integer len is exactly (len < ceil(m_th1)) - the
    // float form was converting an integer to float 4.6 million times a
    // second to answer a question integers already answer. Measured at
    // 492 cycles per word before this, of which the conversions and FPU
    // compares were a visible share.
    uint32_t m_th1i, m_th2i, m_th3i;

    // Runs seen since the last one that was fed to the histogram. **Learning
    // every run is 9.2 million memory read-modify-writes a second to track a
    // threshold that only moves when the source rate does.** Once the
    // thresholds exist, one run in kLearnEvery is plenty - that is still
    // ~575000 samples a second into a 16-bin histogram. Before they exist,
    // every run is used, so acquisition is exactly as quick as it was.
    uint32_t m_learnPhase;

    // ---- biphase state ----
    bool     m_halfBit;        // a 1 UI run is pending its partner

    // ---- subframe assembly ----
    enum Stage { kHunting, kPreTail, kData };
    Stage    m_stage;
    int      m_preTail;        // preamble runs still to swallow
    Preamble m_pending;        // preamble of the subframe being collected
    uint32_t m_bits;           // data bits so far, LSB = slot 4
    int      m_nbits;

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

#endif // NOVADSP_SPDIFRX_H
