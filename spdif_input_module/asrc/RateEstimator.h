// RateEstimator.h - works out what rate the incoming stream is running at, and
// keeps the ASRC's ratio trimmed so the input buffer neither fills nor empties.
//
// ---- the two problems, which are not the same one ------------------------
//
// **Coarse: which rate is this?** 32000, 44100 or 48000 - or something we do
// not handle, in which case the module must say so and mute rather than run
// 8 % detuned with every counter reading healthy. These are 9 % apart, so this
// is nearly trivial and a short window settles it.
//
// **Fine: what is the residual, in ppm?** This is the one that matters. Two
// crystals differ by tens of ppm and that difference drifts with temperature.
// Getting it wrong by even 1 ppm means the buffer walks off at 0.048 frames a
// second, hits a rail, and clicks. Eventually. Which is the worst kind of
// fault to ship.
//
// ---- why frequency measurement alone cannot work -------------------------
//
// The obvious design measures the input frequency accurately and hands it to
// the ASRC. It does not work, and the reason is structural rather than a
// question of measurement quality: **buffer level is the integral of rate
// error**. Any residual error in the measurement, however small, integrates
// without bound. A measurement is a rate; what has to be controlled is its
// integral.
//
// So the loop closes on **buffer level**, not on measured frequency. Level
// error drives the correction, which makes zero steady-state drift a property
// of the loop rather than of how good the measurement is.
//
// ---- the output rate is the timebase, so there are no timestamps ---------
//
// A tempting design timestamps frame arrivals against clk_sys and divides.
// Unnecessary here: **our output rate is exactly 48000 by construction** -
// locally mastered, integer divider, the one clock in this system that is
// beyond question. So input frames counted against output frames produced is
// already the ratio, with no timebase to measure and nothing to calibrate.
//
// That also means this class needs no access to a timer, which is what lets
// the whole thing be driven from a plain loop in the host test.
//
// ---- the controller ------------------------------------------------------
//
// PI on buffer level, with the nominal rate as feedforward.
//
//   * **Feedforward** (32000/44100/48000 exactly) puts the ratio within a few
//     ppm the instant the rate is classified, so acquisition does not have to
//     wait for an integrator to wind up from zero.
//   * **P** gives the loop its response - about a 2 second time constant.
//     Note that P alone would already be stable and drift-free, because the
//     plant is an integrator; it would just settle with a constant level
//     offset rather than at the target.
//   * **I** removes that offset, which matters only because a centred buffer
//     is what gives the coast policy its headroom in both directions.
//
// **Deliberately slow.** The loop modulates the resampling ratio, and ratio
// modulation is pitch modulation. There is no reason to be fast: crystal
// drift moves over minutes. Acquisition speed comes from the feedforward, not
// from loop gain.
//
// ---- units ---------------------------------------------------------------
//
// Everything fine-grained is in **ppm relative to the nominal rate**. It keeps
// the numbers readable, makes every threshold in here mean something you can
// reason about, and keeps the integrator in a range where float32 has plenty
// of resolution left (bounded to +-1000, increments around 0.007).

#ifndef NOVADSP_RATEESTIMATOR_H
#define NOVADSP_RATEESTIMATOR_H

#include <stdint.h>

class RateEstimator
{
public:
    enum State {
        kNoCarrier,     // decoder reports nothing to decode
        kUnsupported,   // carrier present, rate is not one we handle - mute
        kAcquiring,     // rate classified, loop still converging
        kLocked         // converged; buffer is being held at target
    };

    RateEstimator();

    // targetLevel is where the input buffer should sit, in input frames. The
    // module sizes its ring around this - see inputFramesNeeded() on Asrc and
    // the coast policy in the module README.
    void init(float outputRateHz, int targetLevel);

    // Full re-acquire: forget the rate, the trim and the loop state. For a
    // carrier that went away, or one whose rate changed. The module should
    // fade around this.
    void reset();

    // The decoder's verdict on whether there is a stream at all. Losing the
    // carrier drops out of lock immediately rather than waiting for a window
    // to expire - a dead input is knowable at once and there is no reason to
    // keep resampling against a stale ratio while finding out.
    void setCarrier(bool present);

    // Drive once per output block, after the ASRC has run.
    //
    //   framesArrived - input frames the decoder delivered since the last call
    //   outFrames     - output frames produced this block (AUDIO_BLOCK_FRAMES)
    //   bufferLevel   - input frames currently queued, after this block
    //
    // Returns the rate to hand to Asrc::setInputRate(), or 0.0f when not
    // locked or acquiring - in which case the caller should leave the ASRC's
    // ratio alone and apply its mute/fade policy instead.
    float update(int framesArrived, int outFrames, int bufferLevel);

    State state()     const { return m_state; }
    bool  usable()    const { return m_state == kAcquiring || m_state == kLocked; }
    float rateHz()    const { return m_rateHz; }     // nominal plus trim
    int   nominalHz() const { return m_nominalHz; }  // 0 until classified
    float trimPpm()   const { return m_trimPpm; }    // signed, vs nominal
    float levelAvg()  const { return m_levelAvg; }   // smoothed, for reporting

private:
    void classify(float ratio);

    float m_outRateHz;
    int   m_targetLevel;

    State m_state;
    int   m_nominalHz;
    float m_rateHz;
    float m_trimPpm;      // the integrator, in ppm
    float m_levelAvg;     // one-pole smoothed buffer level

    // Classification window: input and output frames accumulated since the
    // last verdict.
    int32_t m_winIn;
    int32_t m_winOut;

    int32_t m_lockCount;  // consecutive blocks inside the lock band

    // Whether the caller last reported a carrier. The classification window
    // is cleared on the *transition* into carrier, never while it is already
    // up - see setCarrier().
    bool    m_carrierPresent;
};

#endif // NOVADSP_RATEESTIMATOR_H
