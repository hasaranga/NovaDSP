// NoiseGate.h - broadband noise gate, single precision, for RP2350 / Pico 2
// and other small FPU targets.
//
// The problem it solves is a source that is clean while something is playing
// and audibly noisy in the gaps - hiss, hum, mic bleed. Below the threshold
// the gate pulls the signal down to silence; above it, the signal passes
// untouched. Broadband, full range: the decision is made on the signal as a
// whole, not on any one band of it.
//
//     Threshold  the level below which the gate closes, dBFS
//     Attack     how fast it opens once the level crosses back above it
//     Hold       how long it stays open after the level drops back below it
//     Release    how fast it closes once Hold has run out
//
// Each of these is a decision only the listener can make: where the noise
// floor sits, how quickly a transient must be let through, how much of a gap
// between notes is worth preserving, and how gradually the tail is cut.
// Everything else - the detector's own smoothing, how far down the gate
// pulls, whether the ramp can click - is fixed, because those have right
// answers rather than preferences.
//
// Why there is no exposed floor
// ------------------------------
// The gate pulls all the way down, to a fixed -96 dBFS rather than to zero.
// Not a parameter: a genuine zero costs nothing extra to reach and buys
// nothing either, since -96 dB is already inaudible against any programme
// material, and stopping just short of it is what keeps the flush-to-zero
// mode doing its job instead of the tail of the ramp landing on denormals.
//
// Detection
// ---------
// Mean square of the (optionally filtered) input, smoothed across blocks with
// a fast, fixed attack and release - fast enough that a real onset is not
// missed inside the block it arrives in, slow enough that one noisy block
// cannot flicker the gate on its own. This is not the Attack/Release the
// knobs control - those set how the *gain* moves once the detector has
// decided; this only smooths what the detector sees, the same split AutoGain
// makes between its own internal detector constants and the `Boost` a user
// can turn.
//
// Signal flow
// -----------
//     in --+-> [gain, slewed toward 1 (open) or the floor (closed) at the
//          |    Attack or Release rate]                              -> out
//          |
//          +-> [mean square, smoothed across blocks]        the side chain.
//              -> [compare to Threshold]                    Measures only;
//              -> [Hold counts down while below; Attack/Release drive gain]
//
// No lookahead, unlike AutoGain - a gate has nothing to protect against
// crossing a ceiling, only a decision to make about whether to pass the
// signal, so there is no reason to delay it to make that decision early.
//
// Usage
// -----
//     static NoiseGate gate;
//     gate.setup(48000.0f, 2);
//     gate.setParam(NoiseGate::kThresholdDb, -40.0f);
//     ...
//     gate.process(interleaved, frames);
//
// process() and the setters are not synchronised. Set parameters from the
// audio core between blocks, as the effect registry already does.

#ifndef NOISEGATE_H_INCLUDED
#define NOISEGATE_H_INCLUDED

#ifndef NOISEGATE_MAX_CHANNELS
#define NOISEGATE_MAX_CHANNELS 2
#endif

class NoiseGate
{
public:
    enum { kMaxChannels = NOISEGATE_MAX_CHANNELS };

    // The order the UI lists them in.
    enum ParamId {
        kThresholdDb = 0,   // dBFS below which the gate closes
        kAttackMs,          // ms to open once the level crosses back above it
        kHoldMs,            // ms to stay open after the level drops below it
        kReleaseMs,         // ms to close once Hold has run out
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

    NoiseGate();

    // Call before processing, and again if the stream format changes. Clears
    // all state. Allocates nothing.
    void setup(float sampleRate, int channels);
    void reset();

    void  setParam(int id, float value);
    float getParam(int id) const;

    // frames interleaved samples per channel, processed in place. 1.0f is
    // full scale.
    void process(float *samples, int frames);

    // What the gate is applying right now, linear: 1.0 open, the fixed floor
    // closed, anything between while it is moving. For a display: a gate
    // doing its job looks exactly like one that has nothing to do.
    float gain() const { return m_gain; }

    float sampleRate() const { return m_sampleRate; }
    int   channels()   const { return m_channels; }

    // Puts the FPU in flush-to-zero mode. setup() calls it, but FPSCR is per
    // core: if process() runs on a different core than setup() did, call this
    // from the audio core too. No-op off ARM.
    static void enableFlushToZero();

private:
    void updateCoefficients();

    // ---- parameters, as given -------------------------------------------
    float m_params[kNumParams];

    // ---- derived at parameter time, never per sample --------------------
    float m_threshMs;      // threshold, squared, so the detector needs no root
    float m_attackRate;    // gain units per sample, opening
    float m_releaseRate;   // gain units per sample, closing
    int   m_holdBlocksMax; // blocks the gate stays open after level drops
    float m_envAtk;        // detector one-pole up,   per block
    float m_envRel;        // detector one-pole down, per block

    // ---- detector / gate state ---------------------------------------------
    float m_envMs;         // smoothed mean square of the (filtered) input
    int   m_holdBlocks;    // blocks remaining before the gate may close
    float m_target;        // 1.0f (open) or the floor (closed)
    float m_gain;          // what is being applied right now

    int   m_look;          // frames in the last block seen; the live block size

    float m_sampleRate;
    int   m_channels;
};

// ---------------------------------------------------------------------------
// The parameter table. Ranges and defaults live here and nowhere else.
//
// -40 dBFS is a middling default threshold: low enough to sit under ordinary
// programme material, high enough to actually catch hiss and hum sitting a
// little above the noise floor. It has to be set against the source, the same
// way AutoGain's Target does - there is no setting here that is right for
// every input level.
//
// 2 ms attack is fast enough that a transient is not audibly clipped on the
// way in, without being fast enough to react to a single cycle of a low note
// and chatter. 50 ms hold is enough to bridge the gaps inside ordinary speech
// and playing without holding through a real pause between phrases; raise it
// if the gate is closing inside words, lower it if it is holding open too
// long after they end. 150 ms release closes gradually enough not to be heard
// as a cut.
//
// A static local rather than a namespace-scope array: every initialiser is a
// constant expression, so it is built at compile time into read-only data and
// costs no start-up code.
// ---------------------------------------------------------------------------

inline const NoiseGate::ParamInfo &NoiseGate::paramInfo(int id)
{
    static const ParamInfo table[kNumParams] = {
        //  name        min      max     default  switch  unit
        { "Thresh",   -80.0f,    0.0f,  -40.0f,   false,  "dB" },
        { "Attack",     0.1f,   50.0f,    2.0f,   false,  "ms" },
        { "Hold",       0.0f,  500.0f,   50.0f,   false,  "ms" },
        { "Release",    5.0f, 2000.0f,  150.0f,   false,  "ms" },
    };

    if (id < 0 || id >= kNumParams) id = 0;
    return table[id];
}

#endif // NOISEGATE_H_INCLUDED
