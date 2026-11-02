// AutoGain.h - automatic volume levelling with a lookahead brick-wall limiter,
// single precision, for RP2350 / Pico 2 and other small FPU targets.
//
// The problem it solves is reaching for the volume control between one song
// and the next. It measures how loud the programme actually is, moves the gain
// until it sits at a level you specify, and puts an exact brick wall after that
// so the boost can never clip the output conversion.
//
//     Target    the level it aims the programme at, RMS
//     MaxGain   the most it may move it, in either direction
//     Boost     how long a quiet stretch must last before it is lifted
//     Output    the wall. Nothing leaves above it, ever
//
// Four controls, and each one is a decision only the listener can make: how
// loud, how far it may reach to get there, how patient it should be about
// quiet stretches, and how close to full scale the result may sit. Everything
// else - the rate law, the detector, how the wall is enforced - is fixed,
// because those have right answers rather than preferences. See the rate law
// in AutoGain.cpp for the one that used to be a `Speed` knob and should not be
// one.
//
// `Boost` is not `Speed` coming back. Speed set a single time constant for
// everything and so had to be wrong in one direction or the other. This sets
// only how long a *rise* must persist before it is believed - the thing that
// separates a quiet passage from a quiet track - and leaves cutting, the rate
// law and the slew alone.
//
// Fast between tracks, invisible inside one
// -----------------------------------------
// This is the whole design problem, and a single time constant cannot solve
// it. A leveller fast enough to catch a new track in a fraction of a second is
// also fast enough to chase the dynamics inside one, which is pumping; slow
// enough not to pump is slow enough to spend the first several seconds of
// every track arriving.
//
// Two mechanisms replace it, answering two different questions.
//
// **How big is the error?** The time constant is chosen per block from how far
// the gain is from where it wants to be. A track arriving 4 dB or more off gets
// 0.08 s and is gone in a few hundred milliseconds; the level wandering a dB
// either side of target inside a track gets 3 s, far too slow to hear. Between
// the two it interpolates.
//
// **How long has it been there?** Size alone is not enough, because a quiet
// passage and a quiet track look identical to any detector - duration is the
// only thing separating them. So a rise must persist for 4 s before it is
// believed, and until then the gain *holds*. It is then released gradually
// rather than all at once - a slew in dB/s, ramped in over several seconds -
// because a binary hold-then-release is a cliff, and material whose gaps sit on
// the boundary hears it as a jump. Cutting is never delayed or limited: it is
// the direction that protects the signal.
//
// **Except on the first arrival after a reset, where that rule has nothing to
// protect against.** It distinguishes a quiet passage from a quiet track by
// measuring how long the quiet lasts - which presumes a previous level to have
// dropped away from. At power-on there is no previous level, only an unknown
// one, so the wait buys nothing and costs the opening of the first track: the
// gain sits at unity for four seconds while the music plays through it
// unlevelled. So the first arrival skips the hold and the slew, and the box
// comes up levelled in about a third of a second instead.
//
// Two things keep that from being a way back into the spikes. It is **the rate
// law that does the moving, not a jump** - the gain is not assigned, it is
// driven at the same speed a mid-programme correction of that size would get,
// so nothing steps. And it is **bounded twice over**: it ends the moment the
// gain arrives, and in any case after no longer than the wait it replaces, so
// the worst it can ever do is behave exactly as it did before.
//
// Measured, and all of it checked by agc/test_autogain.cpp: a track arriving
// 18 dB louder is mostly fixed in 0.22 s, one 18 dB quieter in 8.7 s, 4 dB of
// in-track dynamics moves the gain 0.68 dB, a quiet passage or fade of up to
// 3 s produces 0.00 dB of spike, and no gap length changes that by more than
// 1.8 dB per extra half-second.
//
// The size rule also gives a settle the right shape without being told: most of
// the distance quickly, then a creep over seconds for the last dB. Nobody hears
// the last dB arrive, and not chasing it is what keeps it from breathing.
//
// The detector is smoothed - 0.1 s attack, 0.5 s release - because the mean
// square of one 1.33 ms block of music is far too noisy to steer a fast gain
// with. It is deliberately short: it does not carry the anti-spike job, and
// AutoGain.cpp records what happened when it was asked to.
//
// What it measures, and why that is K-weighted
// --------------------------------------------
// The detector measures an **ITU-R BS.1770 K-weighted** mean square, not a
// broadband one, and the weighting is on the measurement path only - never on
// the audio. Not one sample the listener hears goes through these filters.
//
// The reason is a fault that was heard on the board: when the bass drops out
// of a track, the voice climbs over the next few seconds. The output was never
// exceeding Target, so this was the engine working exactly as designed and the
// design being wrong. Bass carries most of the energy in music - often 10 to
// 20 dB more than the midrange - so a broadband mean square is very largely a
// measurement of the bass. A bass drop-out therefore reads as "the programme
// got quieter" even though the voice never moved, and the leveller lifts the
// gain to put total energy back on target. **It was holding energy constant
// where what wants holding constant is loudness.**
//
// K-weighting is two biquads: a +4 dB high shelf above ~1.5 kHz, and a second
// order high-pass at ~38 Hz which is mostly subsonic rather than bass anyone
// hears. Gentle, and that gentleness is the whole point -
//
// **do not replace it with a hard high-pass at 100-150 Hz.** That was
// considered and rejected, and the reasoning is the part worth keeping:
//
//     The detector and the wall are separate paths, so weighting the detector
//     does not weaken the wall at all - the wall still measures the true
//     broadband peak one block ahead and a kick is still caught exactly. But
//     if bass stops counting toward the measurement, bass-heavy material
//     measures quieter, the leveller pushes it up further, and that extra gain
//     lands on a signal whose peaks *are* the kick. The wall then works harder
//     and more often on kicks, not less. A hard high-pass buys the voice
//     problem at the cost of a kick problem; a gentle weighting does not.
//
// Two consequences to know about:
//
//   **Target is now a K-weighted level.** Bass-heavy material measures several
//   dB lower than it used to, so the same Target setting produces a louder
//   output on it. A number that sounded right before this change will not be
//   the same number afterwards.
//
//   **Each channel is filtered separately and the squares summed**, which is
//   what BS.1770 specifies and what the broadband detector was already doing
//   across channels. Filtering a mono sum instead would be half the work and
//   is the obvious shortcut - but anti-correlated stereo sums to nothing, so
//   material that is merely wide would measure as silence and shut the gate.
//   The cost of not taking that shortcut is one extra biquad pair per block.
//
// Why the wall is what makes the speed safe
// -----------------------------------------
// A leveller that moves fast will sometimes be caught 10 dB too high when a
// track kicks in. What stops that being a clipped output is the limiter behind
// it, and that limiter's ceiling is exact rather than approximate - see
// process(). The two are a pair: the wall is what buys the leveller permission
// to be aggressive.
//
// No transcendentals in the audio path
// ------------------------------------
// Not one log, exp or pow per sample or per block. Levels are kept squared so
// the detector compares without a root; the rate law works on a plain ratio
// rather than on dB, so it needs no log; and the one-pole coefficient uses
// 1 - exp(-x) ~= x, which at these time constants is accurate to a part in
// ten thousand. The single square root per block is VSQRT.F32 inline given
// -fno-math-errno. Both dB parameters are converted exactly once, in
// updateCoefficients(), at control rate.
//
// The K-weighting filters do not change that: they are two biquads, which is
// multiplies and adds. Their coefficients need a tangent and a power, and both
// are taken once in designWeighting(), which runs from setup() and depends on
// nothing but the sample rate.
//
// Signal flow
// -----------
//     in --+-> [gain, ramped across the block, at a rate set by the size of
//          |    the error; a rise is held 4 s then slew-limited]
//          |       -> [one block of delay] -> [brick wall at Output] -> out
//          |
//          +-> [K-weighting: shelf + 38 Hz high-pass]        the side chain.
//              -> [detector: mean square, 0.1 s up / 0.5 s down]   Measures
//              -> [wanted gain = target/measured, +-MaxGain, gated]  only;
//              -> sets the gain above                        no audio here.
//
// Latency
// -------
// One block, whatever the caller's block is - 1.33 ms at 64 frames and 48 kHz.
// Always in circuit. That is on top of whatever the host's own buffering costs.
//
// Usage
// -----
//     static AutoGain agc;
//     agc.setup(48000.0f, 2);
//     agc.setParam(AutoGain::kTargetDb, -30.0f);
//     ...
//     agc.process(interleaved, frames);
//
// process() and the setters are not synchronised. Set parameters from the
// audio core between blocks, as the effect registry already does.

#ifndef AUTOGAIN_H_INCLUDED
#define AUTOGAIN_H_INCLUDED

#ifndef AUTOGAIN_MAX_CHANNELS
#define AUTOGAIN_MAX_CHANNELS 2
#endif

// Capacity of the delay line, in frames. The *live* lookahead is the caller's
// block size, which process() adopts on its first call and re-adopts if it
// ever changes; this is only the ceiling on it. A caller handing over more
// frames than this at a time is the one case the wall cannot make exact, so
// keep it at or above the host's block.
#ifndef AUTOGAIN_LOOKAHEAD
#define AUTOGAIN_LOOKAHEAD 64
#endif

class AutoGain
{
public:
    enum {
        kMaxChannels = AUTOGAIN_MAX_CHANNELS,
        kLookahead   = AUTOGAIN_LOOKAHEAD,
    };

    // The order the UI lists them in.
    enum ParamId {
        kTargetDb = 0,   // dBFS RMS the leveller aims the programme at
        kMaxGainDb,      // the most it may move it, either way
        kBoostDelay,     // seconds a rise must persist before it is believed
        kOutputDb,       // dBFS the wall holds every sample under
        kNumParams
    };

    struct ParamInfo {
        const char *name;         // <= 8 characters, for a 128 px panel
        float       minValue;
        float       maxValue;
        float       defaultValue;
        bool        isSwitch;     // on/off rather than continuous
        const char *unit;
    };

    // Defined at the bottom of this file. That table is the only place the
    // ranges and defaults are written down; setParam() clamps to it and a UI
    // builds itself from it.
    static const ParamInfo &paramInfo(int id);

    AutoGain();

    // Call before processing, and again if the stream format changes. Clears
    // all state. Allocates nothing.
    void setup(float sampleRate, int channels);
    void reset();

    void  setParam(int id, float value);
    float getParam(int id) const;

    // frames interleaved samples per channel, processed in place. 1.0f is full
    // scale. Output is hard-bounded by Output. frames should be constant
    // across calls and no greater than kLookahead - see the note on that
    // constant, and the static_assert in the firmware's glue.
    void process(float *samples, int frames);

    // What the leveller is applying right now, linear. For a display: this is
    // the number that says whether the thing is working, and a leveller doing
    // its job looks exactly like one that is not.
    float gain() const { return m_gain; }

    // How far the wall is pulling down right now, linear, 1.0 for not at all.
    float wallGain() const { return m_wall; }

    // The live lookahead, in frames - the block size process() last saw. Zero
    // until the first block goes through.
    int   lookahead()  const { return m_look; }

    float sampleRate() const { return m_sampleRate; }
    int   channels()   const { return m_channels; }

    // Puts the FPU in flush-to-zero mode. setup() calls it, but FPSCR is per
    // core: if process() runs on a different core than setup() did, call this
    // from the audio core too. No-op off ARM.
    static void enableFlushToZero();

private:
    void updateCoefficients();
    void designWeighting();

    // ---- parameters, as given -------------------------------------------
    float m_params[kNumParams];

    // ---- derived at parameter time, never per sample --------------------
    float m_targetMs;      // target level squared, so the detector needs no root
    float m_maxGain;       // linear, and 1/m_maxGain is the floor
    float m_invMaxGain;
    float m_ceiling;       // linear
    float m_detAtk;        // detector one-pole up,   per block
    float m_detRel;        // detector one-pole down, per block
    int   m_boostDelay;    // blocks a rise must persist before it is believed
    int   m_boostRamp;     // blocks over which the boost slew then opens up
    int   m_primeBlocks;   // blocks of programme before the first arrival runs
    float m_boostStep;     // max fractional gain rise per block, at full slew
    float m_wallRel;       // wall release one-pole, per block

    // ---- K-weighting, derived from the sample rate alone -----------------
    //
    // Two biquads in cascade, direct form II transposed, coefficients shared
    // by every channel. The high-pass numerator is {1, -2, 1} scaled, so it
    // needs two stored coefficients rather than three.
    //
    // Direct form is safe at a 38 Hz corner here, which is worth saying because
    // the Equalizer and the BBE port both use state-variable filters expressly
    // to avoid it. Two things make the difference: this filter is designed once
    // and never retuned, and BS.1770's published coefficients happen to land on
    // exact float32 grid points. Measured against a double-precision reference,
    // the float32 cascade is within 0.003 dB across the whole band. A bell that
    // gets its cutoff from a knob is a different problem.
    float m_kShelfB[3];    // +4 dB high shelf, ~1682 Hz
    float m_kShelfA[2];
    float m_kHpB[2];       // RLB high-pass, ~38 Hz. b2 == b[0]
    float m_kHpA[2];
    float m_kz[kMaxChannels][4];   // two states per stage, per channel

    // ---- leveller state -------------------------------------------------
    float m_detMs;         // smoothed K-weighted mean square of the input
    float m_gain;          // what is being applied right now
    float m_want;          // what the detector last asked for
    int   m_boostBlocks;   // how long it has wanted to rise, in blocks

    // Blocks of programme since the last reset, while the first arrival is
    // still in progress; -1 once it is done and the ordinary rules apply.
    // Counts programme only - a box powered up into silence has not started
    // arriving at anything yet, and must not spend its allowance waiting for
    // someone to press play.
    int   m_arriveBlocks;

    // ---- wall state -----------------------------------------------------
    float m_wall;          // the gain the last block's ramp finished on
    float m_req;           // the gain the block now in the delay line needs
    float m_delay[kLookahead * kMaxChannels];   // exactly the previous block
    int   m_look;          // frames in it; the live lookahead

    float m_sampleRate;
    int   m_channels;
};

// ---------------------------------------------------------------------------
// The parameter table. Ranges and defaults live here and nowhere else.
//
// -30 dBFS RMS is a deliberately conservative default, set for the input this
// board actually sees rather than for a nominal one. A hot source wants
// something nearer -18, which leaves ordinary music peaking around -6 with a
// typical crest factor - the wall then has something to do on transients and
// nothing to do the rest of the time. -14 is roughly where streaming services
// normalise and is the loudest worth asking for; past that the wall works
// continuously and the result is level but flat.
//
// The reason to start low: a Target far above what the source can reach pins
// the gain against MaxGain, and **at the clamp the levelling stops happening at
// all** - every track gets the same maximum boost and their differences pass
// through untouched. A Target the source can actually reach is worth more than
// a loud one it cannot.
//
// Boost defaults to 4 s, which is what measured clean on the material this was
// developed against: it is longer than the quiet sections inside a track, so
// they are not lifted, and short enough that a genuinely quieter track is not
// left waiting. Lower it if quiet tracks take too long to come up; raise it if
// quiet passages inside a track are being lifted and then dumped. 0 disables
// the wait entirely and will wind up into every gap - it is there because the
// bottom of a range should be a real value, not because it is a good setting.
//
// 18 dB of movement covers most of what real material differs by. It is the
// control to reach for when a source is quiet - though a source 30 dB down is
// a level problem upstream, and winding MaxGain up to rescue it amplifies the
// converter's noise floor by exactly as much.
//
// Output reaches down to -36 rather than the -12 it used to, so the box can be
// held to a fixed low level for a downstream input that needs one. The wall
// itself does not care - the ceiling is exact by construction at any value, and
// nothing below is a special case.
//
// **What it is not is a loudness control.** Set below the level Target is
// driving the programme to, it stops catching occasional transients and starts
// limiting continuously by the difference, which flattens exactly the dynamics
// Target is placed not to touch. Target moves the output one-for-one and costs
// nothing to move; this attenuates into a wall. If the two disagree, Target is
// the one to change - see the note above it, and README's "Setting it up, in
// order".
//
// The gap between them is not slack: Target is an RMS destination and Output is
// a peak ceiling, so the distance wanted between them is the crest factor of
// the material, 10-15 dB for most music.
//
// A static local rather than a namespace-scope array: every initialiser is a
// constant expression, so it is built at compile time into read-only data and
// costs no start-up code.
// ---------------------------------------------------------------------------

inline const AutoGain::ParamInfo &AutoGain::paramInfo(int id)
{
    static const ParamInfo table[kNumParams] = {
        //  name        min     max    default  switch  unit
        { "Target",   -36.0f,  -6.0f,  -30.0f,  false,  "dB" },
        { "MaxGain",    0.0f,  24.0f,   18.0f,  false,  "dB" },
        { "Boost",      0.0f,  12.0f,    4.0f,  false,  "s"  },
        { "Output",   -36.0f,   0.0f,   -1.0f,  false,  "dB" },
    };

    if (id < 0 || id >= kNumParams) id = 0;
    return table[id];
}

#endif // AUTOGAIN_H_INCLUDED
