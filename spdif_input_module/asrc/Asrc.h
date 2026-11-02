// Asrc.h - asynchronous sample rate conversion, arbitrary input rate to a
// fixed 48 kHz output grid.
//
// ---- what this is for ---------------------------------------------------
//
// An S/PDIF stream carries its own clock. The source's 48 kHz is defined by
// the source's crystal and ours by ours, so the two drift permanently - about
// 2.4 frames a second at 50 ppm, which is a 64-frame block gained or lost
// every 27 seconds. A bigger buffer does not fix that; it only postpones the
// first click, because drift is a slope and not a transient.
//
// The RP2350 cannot fix it in the clock domain either. Every clock it
// generates is a divided-down clk_sys, and the finest trim available is the
// PIO clock divider's 16.8 fixed point: at the divisor of 25 that 48 kHz
// needs, one LSB is ~156 ppm - coarser than the +-100 ppm being corrected.
// (An ESP32 does this in the clock domain because its APLL trims to ~1.5 ppm.
// We do not have that part.)
//
// So the correction happens in the **data** domain instead, where resolution
// is free: resample whatever arrives onto our own exactly-48000 grid. The
// entire firmware downstream keeps believing AUDIO_SAMPLE_RATE, every effect
// keeps running in the configuration it was measured in, and the layout hash,
// the spectrum band table and every published figure stay valid.
//
// **That is the contract this class exists to make true**: an input module
// emits exactly AUDIO_BLOCK_FRAMES frames per block on the 48 kHz grid,
// always, whatever the source is doing. Nothing upstream of that boundary is
// any of the core's business.
//
// ---- what it deliberately does not do -----------------------------------
//
// **It does not estimate the ratio.** setInputRate() is told what the rate is;
// working that out from frame arrival times is the decoder's job and it is a
// control loop with its own convergence behaviour. Splitting them keeps this
// class testable against exact ratios on the host, which is the whole reason
// it was written before the decoder existed.
//
// **It does not decide what to do when input runs out.** produce() reports how
// many frames it managed and how many it consumed, and stops. Coasting,
// fading and muting are policy, and policy belongs to the module that knows
// whether the carrier is gone or merely late. See spdif_input_module/README.md.
//
// ---- upsampling only, and why that is the whole simplification ----------
//
// Supported input rates are 32000, 44100 and 48000 - everything at or below
// the 48 kHz output. That makes the ratio fs_in/fs_out always <= 1, which
// means the interpolation kernel only ever has to reconstruct the band-limited
// signal the input samples already represent. Its cutoff is normalised to the
// **input** rate, so one prototype filter serves every supported rate with no
// redesign.
//
// Accepting 96 kHz would break that: it is decimation, the kernel would have
// to narrow to the *output* Nyquist to stop aliasing, and the filter becomes
// rate-dependent. That is a different design, not a bigger table.
//
// ---- the phase accumulator is fixed point, deliberately ------------------
//
// This tree's rule is float and never double - the M33 has no double-precision
// unit. But a float32 phase accumulated 48000 times a second loses precision
// in a way that presents as slow drift, which is exactly the fault this class
// exists to remove. So the phase is a **64-bit fixed-point accumulator in
// Q32**, which is exact: no rounding, no random walk, no drift however long it
// runs. The audio path either side of it is float throughout.

#ifndef NOVADSP_ASRC_H
#define NOVADSP_ASRC_H

#include <stdint.h>

// ---- design constants ---------------------------------------------------
//
// Both are compile-time so the host suite can sweep them and report what the
// quality actually is rather than what a comment claims it is.
//
// kTaps must be a power of two - the input history is a circular buffer
// masked with kTaps-1, and shifting 32 frames per input sample instead would
// cost more than the filter.
#define ASRC_TAPS    32     // kernel length, power of two
#define ASRC_PHASES  128    // polyphase branches; linear interpolation between

class Asrc
{
public:
    Asrc();

    // Build the kernel and clear all state. Call once before audio starts -
    // it runs a Kaiser window and a sinc per table entry, which is not
    // something to do on the audio path.
    void init(float outputRateHz);

    // Same as init() minus the table build: restore the rates and clear all
    // state, keep the kernel.
    //
    // **This is the one a re-acquire calls, and init() is not.** The kernel is
    // a function of ASRC_TAPS, ASRC_PHASES and the window constants in
    // Asrc.cpp and of nothing else - not of either rate - so there is never
    // anything to rebuild once it exists. Calling init() instead costs tens of
    // milliseconds of software `double` sin/sqrt/Bessel on the M33, which on
    // the block path is not a slow call but a missed deadline and a string of
    // overruns. See SpdifInput::reset().
    void restart(float outputRateHz);

    // Drop history and phase, keep the kernel and the rate. For a carrier
    // that went away and came back: the module should fade around this,
    // because the first outputs after it are a filter refilling.
    void reset();

    // Set the measured input rate. Safe to call every block - the phase
    // accumulator is continuous across a change, so a slowly moving rate
    // produces no discontinuity. This is the control input the decoder's
    // rate estimator drives.
    //
    // Clamped to a sane band around the output rate; rates outside it mean
    // the estimator has lost its way and holding the last good ratio is
    // better than following it.
    void setInputRate(float hz);

    float inputRate()  const { return m_inRateHz; }
    float outputRate() const { return m_outRateHz; }

    // Frames of input needed to produce `outFrames` of output at the current
    // ratio, plus a little slack. What a caller sizes its ring against.
    int inputFramesNeeded(int outFrames) const;

    // Resample interleaved stereo.
    //
    //   in       - `inFrames` interleaved L,R input frames
    //   out      - room for `outFrames` interleaved L,R output frames
    //   inUsed   - out: how many input frames were consumed
    //
    // Returns the number of output frames actually produced. **A short return
    // means the input ran out**, which is information the caller needs and
    // not an error this class can act on.
    int produce(float *out, int outFrames,
                const float *in, int inFrames, int *inUsed);

    // Total input frames consumed and output frames produced since init().
    // The ratio of these two is the achieved conversion rate, which is what a
    // long-run drift check measures.
    uint64_t framesIn()  const { return m_totalIn; }
    uint64_t framesOut() const { return m_totalOut; }

private:
    void buildKernel();
    void pushFrame(float l, float r);

    // kernel[q][k], q = 0..ASRC_PHASES inclusive. The extra row is real: the
    // linear interpolation between branch q and q+1 reaches q+1 when q is the
    // last branch, and an off-by-one there is a wrap to the opposite end of
    // the kernel - which sounds like a periodic tick, not like a filter.
    float m_kernel[ASRC_PHASES + 1][ASRC_TAPS];

    float m_hist[ASRC_TAPS * 2];   // circular, interleaved L,R
    int   m_head;                  // next write slot, masked with ASRC_TAPS-1

    uint64_t m_phase;              // Q32, in [0, 1<<32)
    uint64_t m_step;               // Q32, fs_in/fs_out, in (0, 1<<32]

    float m_inRateHz;
    float m_outRateHz;

    uint64_t m_totalIn;
    uint64_t m_totalOut;

    bool m_primed;                 // history has seen at least kTaps frames
};

#endif // NOVADSP_ASRC_H
