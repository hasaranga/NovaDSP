// SpdifInput.h - the whole input path behind one call, and the continuity
// policy that makes it safe to listen to.
//
// Ties together the four pieces in this directory:
//
//     PIO words -> SpdifRx -> ring -> Asrc -> 48 kHz blocks
//                     |                 ^
//                     +-- RateEstimator +
//
// ---- the contract ------------------------------------------------------
//
//     **produce() always fills exactly `frames` frames.**
//
// Not "when locked", not "when a carrier is present", not "when the source
// has not changed rate mid-track" - always, from the moment init() returns,
// with nothing plugged in at all. That is the requirement this class exists
// for and every decision below follows from it.
//
// Two reasons it matters, and only one of them is obvious. The obvious one is
// that the core's block loop has a deadline and cannot be told "no audio this
// time". The other is the actual complaint: **a speaker system plops when the
// stream stops and starts.** Sources drop the S/PDIF carrier between tracks,
// often returning at a different rate, and every device downstream mutes and
// unmutes around that. A box that keeps producing samples absorbs it.
//
// One large piece of that is already free from the architecture: the output
// side is locally mastered at exactly 48 kHz, so BCK/LRCK never stop and the
// DAC's PLL locks once at boot and never again.
//
// ---- zero is not the answer, ramps are ---------------------------------
//
// The tempting implementation of "no input" is to write zeros. That writes a
// **step discontinuity** from whatever the last sample was, which is a
// broadband click - precisely the plop being removed, just relocated.
//
// So every transition is a raised-cosine ramp of kFadeFrames, and the fill
// during a dropout is the last sample *through that ramp* rather than a hold.
// A hold would be DC into a speaker; the ramp lands on true zero and stays
// there.
//
// ---- three tiers, because a dry buffer and a dead carrier differ --------
//
//     buffer briefly dry     coast: ramp down over the shortfall only, and
//                            ramp straight back when frames arrive
//     shortfall persists     stay down; the ramp has already finished
//     no carrier / unusable  hold true zero, and trim the ring
//     re-lock or rate change re-converge, then ramp up
//
// ---- the ring is trimmed while muted, not filled -----------------------
//
// This is the half that is easy to miss, and the closed-loop test in
// test_rate.cpp is what found it. While the rate is unclassified the ASRC
// must not be pulled at all: it starts at a ratio of 1.0, so against a
// 44.1 kHz source it would consume 64 frames a block while 59 arrive and
// starve long before the estimator has said anything.
//
// Being muted therefore also means **discarding the oldest input** rather
// than letting it accumulate - a quarter second of audio nobody played is
// stale by the time it would be played, and letting it pile up overruns the
// ring instead of starving it.

#ifndef NOVADSP_SPDIFINPUT_H
#define NOVADSP_SPDIFINPUT_H

#include <stdint.h>

#include "Asrc.h"
#include "RateEstimator.h"

// ---- which decoder, and why there are two -------------------------------
//
// **1 selects the per-UI front end** (../spdif_rx_ui.pio + SpdifRxUi), which
// is what this module ships with. 0 selects the 4x oversampling front end
// (../spdif_rx.pio + SpdifRx), which is the reference implementation the other
// one is tested against and is kept buildable for exactly that reason.
//
// They are not a preference. The oversampling decoder was measured on the
// board at **73 % of a 307.2 MHz core** and does not fit the block deadline;
// the per-UI one hands a quarter as many words to arithmetic with no per-run
// loop in it. See SpdifRxUi.h, and SESSION.md for the bring-up that produced
// the figure.
//
// Switching this switches the PIO program too - module.cpp keys off the same
// define - so the two halves cannot be mismatched.
#ifndef SPDIF_USE_PIO_DECODE
#define SPDIF_USE_PIO_DECODE 1
#endif

#if SPDIF_USE_PIO_DECODE
#include "SpdifRxUi.h"
typedef SpdifRxUi SpdifDecoder;
#else
#include "SpdifRx.h"
typedef SpdifRx   SpdifDecoder;
#endif

// Input frames the ring is held at. Has to clear the acquisition transient -
// before the estimator's integrator winds up, its P term alone supplies the
// correction, so the level excursion is about delta/Kp frames, 33 for a
// source 500 ppm out - plus whatever the coast policy wants on top.
#define SPDIF_RING_TARGET 256

// Ring capacity in input frames. Four times the target; 8 KB of SRAM as
// stereo float.
#define SPDIF_RING_FRAMES 1024

// Ramp length. ~3 ms at 48 kHz - long enough that the step is inaudible,
// short enough that the start of a track is not visibly missing.
#define SPDIF_FADE_FRAMES 144

// Largest block produce() will be asked for. AUDIO_BLOCK_FRAMES is 64.
#define SPDIF_MAX_BLOCK 256

// Words push() hands the decoder at a time, and the frames that can come back
// from one such call.
//
// **This used to be `int32_t decoded[512*2]` - 4 KB - on the stack, inside the
// block loop on core 0, with no explicit stack size set anywhere in the
// build.** It was sized by guessing generously rather than from the word
// count, and it is the first suspect in SESSION.md's unexplained hard fault at
// SPDIF_OVERSAMPLE 2. It is now derived and it is a member, so the block loop's
// stack depth does not depend on this module at all.
//
//   per-UI front end   one word is 32 UI, a frame is 128 UI -> chunk/4 frames
//   oversampling       one word is 32/OVERSAMPLE UI            -> chunk/16
//
// The larger of the two is used, so the switch above cannot undersize it.
#define SPDIF_PUSH_CHUNK  256
#define SPDIF_PUSH_FRAMES (SPDIF_PUSH_CHUNK / 4 + 2)

class SpdifInput
{
public:
    enum State {
        kNoCarrier,     // nothing on the wire
        kUnsupported,   // a carrier, at a rate we cannot resample from
        kAcquiring,     // decoding; rate known or being classified
        kLocked         // converged and playing
    };

    SpdifInput();

    void init();

    // Full re-acquire: forget the rate, the trim, the ring and the decoder's
    // framing. Thresholds inside SpdifRx are dropped too, so this is for an
    // input that has genuinely changed rather than for a carrier blink.
    void reset();

    // Feed raw oversampled words as PIO pushed them (first sample in bit 0).
    // Returns input frames decoded. Safe to call with any count, including 0.
    int push(const uint32_t *words, int count);

    // Produce exactly `frames` frames, interleaved L,R, 24-bit left-justified
    // in int32. **Always fills the buffer.** `frames` must not exceed
    // SPDIF_MAX_BLOCK.
    void produce(int32_t *out, int frames);

    State    state()      const { return m_state; }
    bool     playing()    const { return m_state == kLocked; }
    int      rateHz()     const { return m_est.nominalHz(); }
    float    trimPpm()    const { return m_est.trimPpm(); }
    int      ringLevel()  const { return m_count; }
    float    gain()       const { return m_gain; }
    float    unitSamples()const { return m_rx.unitSamples(); }
    bool     framed()     const { return m_rx.framed(); }

    uint32_t underruns()  const { return m_underruns; }
    uint32_t dropped()    const { return m_dropped; }
    uint32_t parityErrors() const { return m_rx.parityErrors(); }
    uint32_t resyncsSeen()  const { return m_rx.resyncs(); }

private:
    void ringPush(const int32_t *frames, int n);
    void ringTrim(int target);
    int  ringContig() const;
    void updateState();
    void emitMuted(int32_t *out, int frames);
    void applyGain(const float *src, int made, int32_t *out, int frames);

    SpdifDecoder  m_rx;
    Asrc          m_asrc;
    RateEstimator m_est;

    // Scratch for one push() chunk. A member rather than a local; see
    // SPDIF_PUSH_FRAMES above.
    int32_t m_decoded[SPDIF_PUSH_FRAMES * 2];

    // Ring of decoded input frames, stereo, normalised to +-1.
    float m_ring[SPDIF_RING_FRAMES * 2];
    int   m_head, m_tail, m_count;

    // Raised-cosine ramp, precomputed. Index walks toward 0 or the top.
    float m_fade[SPDIF_FADE_FRAMES + 1];
    int   m_fadeIdx;
    float m_gain;

    float   m_lastL, m_lastR;   // for the ramp-to-zero fill
    State   m_state;
    int     m_arrivedSinceBlock;

    // Rate-change handling: the nominal we last saw, and a latch that holds
    // the mute until the ramp has reached zero. See updateState().
    int      m_lastNominal;
    bool     m_rateChanged;

    uint32_t m_underruns;
    uint32_t m_dropped;
};

#endif // NOVADSP_SPDIFINPUT_H
