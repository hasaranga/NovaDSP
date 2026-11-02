#include "AutoGain.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace {

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// dB to linear amplitude. Setup only - never in the audio path, which is the
// point of computing everything it feeds in advance.
inline float dbToLin(float db)
{
    return powf(10.0f, db * (1.0f / 20.0f));
}

// ---- the rate law ------------------------------------------------------
//
// This is the file's one real idea and the reason it has no `Speed` control.
//
// A leveller has to be fast between tracks and invisible inside one, and a
// single time constant cannot be both: fast enough to catch a new track is
// fast enough to chase a chorus, which is pumping. So the time constant is
// chosen per block from **how far the gain currently is from where it wants to
// be**, expressed as a plain ratio so no logarithm is needed.
//
//   error <= 1 dB   ->  3 s     the level wandering inside a track. Far too
//                               slow to hear, which is the point.
//   error >= 4 dB   ->  0.08 s  a different track. Gone in a few hundred ms.
//   between         ->  interpolated
//
// 4 dB rather than 8 for the top of the ramp: the measured settle after an
// 18 dB step nearly halved, and the gain still moves less than 0.3 dB against
// 4 dB of dynamics inside a track, so the no-pump end was nowhere near being
// spent. Both numbers are checked by the host test rather than reasoned about.
//
// The useful side effect is the shape of a settle: a big step covers most of
// its distance at the fast rate, then slows as the error shrinks and creeps
// the last dB over seconds. Nobody hears the last dB arrive, and declining to
// chase it is what keeps the thing from breathing.
const float kRatioSlow   = 1.122f;   // 1 dB
const float kRatioFast   = 1.585f;   // 4 dB
const float kInvTauSlow  = 1.0f / 3.00f;
const float kInvTauFast  = 1.0f / 0.08f;

// ---- and why cutting is faster than boosting ---------------------------
//
// The rate above is symmetric, and symmetric is wrong. **A leveller allowed to
// boost as fast as it cuts will wind the gain up into every quiet passage**,
// because a quiet passage and a quiet track look identical to any detector -
// the only thing that distinguishes them is how long they last. When the music
// returns, its first moments arrive at that wound-up gain. The wall stops it
// clipping, so it is not heard as distortion: it is heard as a sudden burst of
// very loud, very squashed audio, on material that has no pattern to it.
//
// Measured on the host before this factor existed: **14 dB of spike** when
// music returns after two seconds 25 dB down, and the same again on the track
// following a fade-out. Both are checks now.
//
// So boosting waits and cutting does not, which is the right asymmetry for the
// same reason it always was: arriving too loud is the fault you have to fix
// immediately, and arriving too quiet is merely quiet.
//
// **What it waits for is duration, not size.** The rate law above knows how big
// the error is but not how long it has been there - and duration is the *only*
// thing that distinguishes a quiet passage from a quiet track. So a rise has to
// persist for this long before it is believed, and until then the gain *holds*.
// A passage shorter than this produces no wind-up at all; a genuinely quieter
// track costs this much delay and is then let out gradually, per the slew below.
//
// Two other things were tried first and are worth not repeating. Scaling the
// boost rate down by a constant slows *everything* indiscriminately, including
// the legitimate quiet track: at a quarter rate a quiet track took 15 s to come
// up, which is worse than the engine this replaced. And leaning on a long
// detector release is worse still - a one-pole on *mean square* falling 18 dB
// takes about four time constants of the linear value, so lengthening it pushed
// the quiet-track case out to 19-24 s while barely improving the spike.
// Duration is the thing to measure, so measure it directly.
//
// **The wait holds the gain rather than creeping it.** An earlier version let
// it move at the slow rate meanwhile, which sounds harmless and is not:
// kInvTauSlow is a 3 s constant, so across a 2 s passage the gain still
// travelled half way to the wound-up value and left several dB of spike behind.
// Holding makes a passage shorter than the delay produce exactly none.
// This wait is the `Boost` parameter - see AutoGain.h. It is exposed rather
// than fixed because the right value depends on the material: it has to be
// longer than the quiet sections inside a track and shorter than a listener's
// patience with a quiet one. Everything below it stays fixed.

// ...and then it opens up over this long rather than all at once, to a boost
// no faster than this many dB per second.
//
// **A binary hold-then-release is a cliff, and the cliff is audible.** Caught
// on hardware after the persistence rule was already in: a loop whose quiet
// section was almost exactly the `Boost` wait long produced a clean +8 dB
// jump, once per repeat, on the one second the counter happened to expire in.
// Gaps shorter than the delay were silent and gaps longer were fine - only
// material sitting on the boundary tripped it, which is exactly what made it
// look random.
//
// So the delay no longer releases to the *fast* rate. What it releases to is a
// slew in dB per second, itself ramped in over kBoostRampSeconds, which makes
// the wind-up from a gap of length T bounded and roughly proportional to how
// far past the delay it ran - instead of a step the moment a counter crosses.
const float kBoostRampSeconds  = 6.000f;
const float kBoostSlewDbPerSec = 8.000f;

// The mean square of a single 1.33 ms block of music is far too noisy to steer
// a fast gain with, so it is smoothed before the rate law ever sees it.
//
// **Asymmetric, and for the same reason the rate law is.** Rising fast means a
// track that arrives louder is recognised within a tenth of a second. Falling
// slowly means a quiet passage, a decrescendo or a fade-out does not convince
// the detector that the programme itself got quieter - so what it reports is
// closer to the level of the loud parts than to a running average, which is
// what "how loud is this programme" actually means.
//
// The release does *not* carry the anti-spike job - the `Boost` wait does.
// That was tried and it fails, for a reason worth knowing: a one-pole on mean
// *square* falling 18 dB takes about four time constants of the linear value,
// so a release long enough to blunt a quiet passage also pushed a genuinely
// quieter track out to 19-24 s. Kept short here, at the value that minimises
// how long a quiet track waits without letting block-to-block noise through.
const float kDetectorAttackSeconds  = 0.100f;
const float kDetectorReleaseSeconds = 0.500f;

// ...and none of that applies to the first arrival after a reset, which waits
// this long instead. See the header for why the persistence rule has nothing
// to do at power-on.
//
// **It is not zero, and this is the whole subtlety of the thing.** The
// detector primes off a single block, and one 1.33 ms window of music is not
// the level of the music - it can sit a long way either side of it depending
// on whether it caught a transient or the gap after one. Acting on that
// immediately would set the gain from a reading that means nothing: too high
// and the first thing heard is a loud moment, too low and it has to boost back
// up, which is the slow direction and the exact complaint this exists to fix.
//
// So the first arrival waits one detector attack constant before it moves.
// That is the shortest wait with a reason behind it rather than a number
// chosen to feel small - by definition it is how long the detector takes to
// mean something - and against the four seconds it replaces it is nothing.
const float kPrimeSeconds = kDetectorAttackSeconds;

// Below this the gain holds where it is. Not a parameter: silence is the
// quietest thing the detector will ever see, so without a gate the gap between
// two tracks is spent winding the gain to MaxGain and then dumping it the
// instant the next one starts. -60 dBFS RMS is below any programme material
// and above any converter's noise floor.
//
// Written squared and in linear, because that is how the detector compares it
// and because -60 dB is exactly 1e-3: no powf, at setup or anywhere else.
//
// It is compared against the *weighted* mean square, since that is now what
// the detector holds and the gate is part of the detector. The threshold did
// not need moving: -60 dBFS is far enough below programme material that the
// few dB the weighting moves it by changes nothing, and content quiet enough
// to be weighted under the gate is content with no loudness in it - which is
// the thing the gate is for.
const float kGateMs = 1.0e-6f;   // (10^(-60/20))^2

// How long the wall takes to let go after the peak that pulled it down has
// passed. Not a parameter either: it is the one number here with a right
// answer for programme material rather than a preference.
const float kReleaseSeconds = 0.150f;

// ---- the K-weighting curve ---------------------------------------------
//
// ITU-R BS.1770 defines its weighting as two biquads and publishes their
// coefficients at 48 kHz. It does not publish them at any other rate - what it
// publishes instead is the analogue prototype each stage came from, and the
// numbers below are that prototype. Running them through the same bilinear
// transform the standard used reproduces the published 48 kHz table exactly,
// which the host test asserts rather than assumes, and gives a correct filter
// at any other rate for free.
//
// Stage 1 is the "pre-filter": a +4 dB high shelf standing for the acoustic
// effect of a head in a sound field. Stage 2 is the RLB high-pass, which is
// the part doing the work here - it is what stops sub-audio content counting
// as loudness. Note where it sits: 38 Hz is below the bottom note of a bass
// guitar. This curve does not remove bass, it de-emphasises it, and the
// difference between those two is the whole reason a hard high-pass was
// rejected. See the header.
const float kShelfHz = 1681.974450955533f;
const float kShelfDb =    3.999843853973347f;
const float kShelfQ  =    0.7071752369554196f;

// The exponent relating the shelf's mid-band gain to its high-band gain. It is
// not 0.5 and the difference is not a rounding: this exact value is what makes
// the derivation land on the standard's published table rather than near it.
const float kShelfVbExp = 0.4996667741545416f;

const float kHpHz = 38.13547087602444f;
const float kHpQ  =  0.5003270373238773f;

} // namespace

AutoGain::AutoGain()
{
    for (int i = 0; i < kNumParams; ++i)
        m_params[i] = paramInfo(i).defaultValue;

    m_sampleRate = 48000.0f;
    m_channels   = 2;

    m_detMs = 0.0f;
    m_gain  = 1.0f;
    m_want  = 1.0f;
    m_wall  = 1.0f;
    m_req   = 1.0f;
    m_boostBlocks = 0;
    m_arriveBlocks = 0;
    m_look  = 0;            // adopted from the first block handed over
    memset(m_delay, 0, sizeof m_delay);
    memset(m_kz,    0, sizeof m_kz);

    designWeighting();      // so an instance is usable without setup()
    updateCoefficients();
}

void AutoGain::setup(float sampleRate, int channels)
{
    if (sampleRate > 0.0f) m_sampleRate = sampleRate;
    m_channels = (channels < 1) ? 1
               : (channels > kMaxChannels ? kMaxChannels : channels);

    designWeighting();      // depends on the rate and on nothing else
    updateCoefficients();
    reset();
    enableFlushToZero();
}

// The two K-weighting stages, by the bilinear transform of the prototypes at
// the top of this file. Called from setup() only: nothing a user can turn
// changes this filter, so it is not in updateCoefficients() where it would be
// recomputed on every knob movement.
//
// The tangent and the two powers here are the only transcendentals in the
// class outside dbToLin, and like those they run at setup and never again.
void AutoGain::designWeighting()
{
    const float pi = 3.14159265358979f;

    // ---- stage 1: the high shelf ----
    {
        const float K  = tanf(pi * kShelfHz / m_sampleRate);
        const float Vh = powf(10.0f, kShelfDb * (1.0f / 20.0f));
        const float Vb = powf(Vh, kShelfVbExp);
        const float KK = K * K;
        const float KQ = K / kShelfQ;

        const float a0 = 1.0f + KQ + KK;
        m_kShelfB[0] = (Vh + Vb * KQ + KK) / a0;
        m_kShelfB[1] =      2.0f * (KK - Vh) / a0;
        m_kShelfB[2] = (Vh - Vb * KQ + KK) / a0;
        m_kShelfA[0] =     2.0f * (KK - 1.0f) / a0;
        m_kShelfA[1] =      (1.0f - KQ + KK) / a0;
    }

    // ---- stage 2: the RLB high-pass ----
    //
    // Numerator {1, -2, 1} over the same a0, so b2 == b0 and only two
    // coefficients are stored.
    {
        const float K  = tanf(pi * kHpHz / m_sampleRate);
        const float KK = K * K;
        const float KQ = K / kHpQ;

        const float a0 = 1.0f + KQ + KK;
        m_kHpB[0] =               1.0f / a0;
        m_kHpB[1] =              -2.0f / a0;
        m_kHpA[0] = 2.0f * (KK - 1.0f) / a0;
        m_kHpA[1] =  (1.0f - KQ + KK)  / a0;
    }
}

void AutoGain::reset()
{
    memset(m_delay, 0, sizeof m_delay);
    memset(m_kz,    0, sizeof m_kz);
    m_detMs = 0.0f;
    m_gain  = 1.0f;
    m_want  = 1.0f;
    m_wall  = 1.0f;
    m_req   = 1.0f;
    m_boostBlocks = 0;
    m_arriveBlocks = 0;
}

// Everything the audio path needs, derived once here. Two divisions of labour
// worth noticing: levels are kept *squared*, so the detector never takes a root
// to compare against them, and the gain limits are kept linear, so clamping
// what the detector wants is two compares rather than a logarithm.
void AutoGain::updateCoefficients()
{
    const float target = dbToLin(m_params[kTargetDb]);
    m_targetMs = target * target;

    m_maxGain    = dbToLin(m_params[kMaxGainDb]);
    m_invMaxGain = 1.0f / m_maxGain;

    m_ceiling = dbToLin(m_params[kOutputDb]);

    // Per block, so it needs the block rate. m_look is zero until the first
    // block, and process() calls this again the moment it knows the real one.
    // 1 - exp(-x) ~= x holds to a part in ten thousand at these constants, so
    // there is no expf here either.
    const float blockRate = (m_look > 0) ? (m_sampleRate / (float)m_look)
                                         : m_sampleRate;
    m_detAtk  = clampf(1.0f / (kDetectorAttackSeconds  * blockRate), 0.0f, 1.0f);
    m_detRel  = clampf(1.0f / (kDetectorReleaseSeconds * blockRate), 0.0f, 1.0f);

    m_boostDelay = (int)(m_params[kBoostDelay] * blockRate);
    m_boostRamp  = (int)(kBoostRampSeconds  * blockRate);
    if (m_boostRamp < 1) m_boostRamp = 1;

    m_primeBlocks = (int)(kPrimeSeconds * blockRate);

    // Fractional rise per block for kBoostSlewDbPerSec. 20/ln(10) = 8.6859,
    // and (10^(x/20) - 1) ~= x/8.6859 for the small x a single block covers -
    // so the slew costs no powf here either.
    m_boostStep = kBoostSlewDbPerSec / (8.6859f * blockRate);
    m_wallRel = clampf(1.0f / (kReleaseSeconds * blockRate), 0.0f, 1.0f);
}

void AutoGain::setParam(int id, float value)
{
    if (id < 0 || id >= kNumParams) return;

    const ParamInfo &pi = paramInfo(id);
    m_params[id] = clampf(value, pi.minValue, pi.maxValue);

    updateCoefficients();
}

float AutoGain::getParam(int id) const
{
    if (id < 0 || id >= kNumParams) return 0.0f;
    return m_params[id];
}

// ---------------------------------------------------------------------------
// The wall, and why it holds exactly
//
// The delay line carries exactly one block, so when block D is due to leave,
// its own contents are already known and so are the contents of the block
// behind it. Write r(B) for the gain block B needs if it is not to cross the
// ceiling - ceiling/peak(B), or 1 where the block never reaches it.
//
// D leaves on a linear ramp from g_start to g_end, where
//
//     g_start = the g_end the previous block finished on
//     g_end   = min(r(D), r(next))                       (release-limited up)
//
// g_end <= r(D) by construction. And g_start was itself min(r(D-1), r(D)),
// which is also <= r(D). A linear ramp between two values that are both at or
// below r(D) never rises above r(D), so no sample of D leaves above the
// ceiling - not approximately, and not because something caught it afterwards.
//
// This is what makes it safe for the leveller in front to be aggressive. A
// fast leveller will sometimes be 10 dB too high when a track kicks in; the
// wall is what stops that being a clipped output rather than a loud second.
// ---------------------------------------------------------------------------
void AutoGain::process(float *samples, int frames)
{
    if (frames <= 0) return;

    const int ch = m_channels;

    // The lookahead *is* the block. Adopted on the first call and re-adopted
    // if the caller ever changes it - AUDIO_BLOCK_FRAMES is a build-time knob
    // in the firmware this was written for. A caller exceeding the line's
    // capacity is the one case the schedule cannot make exact; the clamp at
    // the bottom of the loop is what covers it.
    if (frames != m_look) {
        m_look = (frames < kLookahead) ? frames : kLookahead;
        reset();
        updateCoefficients();   // the detector coefficient is per block
    }

    const int n = frames * ch;

    // ---- detector --------------------------------------------------------
    //
    // Mean square rather than peak. Peak follows a single transient and would
    // have the leveller chasing snare hits; what it is being asked to match
    // between one track and the next is loudness.
    //
    // And K-weighted rather than broadband, which is what makes that last word
    // true rather than approximate - see the header. **This reads `samples`
    // and does not write it.** The weighting exists so the leveller measures
    // what a listener hears; putting it in the signal would be an equalizer,
    // and there is already one of those in the menu.
    //
    // A channel at a time, so both biquad states stay in registers for the
    // length of the block. The states are carried across blocks and are not
    // touched by the gate: a filter that stopped running through a quiet
    // passage would hand the detector a settling transient at the moment the
    // music came back, which is precisely when it is being asked to hold
    // still.
    float sumsq = 0.0f;
    for (int c = 0; c < ch; ++c) {
        float s1 = m_kz[c][0], s2 = m_kz[c][1];   // shelf
        float t1 = m_kz[c][2], t2 = m_kz[c][3];   // high-pass
        float acc = 0.0f;

        for (int f = 0; f < frames; ++f) {
            const float x = samples[f * ch + c];

            const float y = m_kShelfB[0] * x + s1;
            s1 = m_kShelfB[1] * x - m_kShelfA[0] * y + s2;
            s2 = m_kShelfB[2] * x - m_kShelfA[1] * y;

            // b2 == b0 for this stage, hence m_kHpB[0] twice.
            const float z = m_kHpB[0] * y + t1;
            t1 = m_kHpB[1] * y - m_kHpA[0] * z + t2;
            t2 = m_kHpB[0] * y - m_kHpA[1] * z;

            acc += z * z;
        }

        m_kz[c][0] = s1; m_kz[c][1] = s2;
        m_kz[c][2] = t1; m_kz[c][3] = t2;
        sumsq += acc;
    }

    // Per channel and then summed, which is BS.1770's own definition and is
    // also what the broadband detector did. Not a mono sum - see the header
    // for why that shortcut costs more than it saves.
    const float blockMs = sumsq / (float)n;

    // ---- what the programme wants ----------------------------------------
    //
    // Feed-forward, not a loop: the detector measures the *input* and the gain
    // is target/measured, so the output lands on the target by construction
    // rather than by converging on it. There is nothing here that can hunt.
    //
    // **The gate is decided on this block's own mean square, and when it is
    // shut the smoother is not fed either.** That second half is the part that
    // matters, and leaving it out is a real bug rather than an inefficiency:
    // a smoother fed through a gap spends the fall from programme level down
    // to the gate reporting ever quieter levels, and a leveller this fast
    // follows them all the way to MaxGain on the way down - arriving at the
    // next track at full boost, which is the exact thing a gate exists to
    // prevent. Freezing the detector as well as the wanted gain is what
    // actually holds it. Measured: 23 dB of wind-up across ten seconds of
    // silence before this, none after.
    const bool programme = (blockMs > kGateMs);

    if (programme) {
        // Primed rather than smoothed on the first block of programme, so a
        // session does not begin by slamming to MaxGain and walking back.
        const float dCoef = (blockMs > m_detMs) ? m_detAtk : m_detRel;
        m_detMs = (m_detMs > 0.0f)
                ? (m_detMs + (blockMs - m_detMs) * dCoef)
                : blockMs;

        // One divide and one root for the whole block; sqrtf is VSQRT.F32
        // inline here. MaxGain is symmetric - it bounds the cut as much as the
        // boost, which is what makes "every track at one volume" work rather
        // than only rescuing the quiet ones.
        m_want = clampf(sqrtf(m_targetMs / m_detMs), m_invMaxGain, m_maxGain);
    }

    // ---- how fast to get there -------------------------------------------
    //
    // See the rate law at the top of the file. The ratio is taken the way up
    // that makes it >= 1 so one comparison covers both directions; the law is
    // symmetric because a track 8 dB too loud is exactly as wrong as one 8 dB
    // too quiet, and the wall behind is what handles the danger in the loud
    // direction.
    const float ratio = (m_want > m_gain) ? (m_want / m_gain)
                                          : (m_gain / m_want);

    float invTau;
    if (ratio <= kRatioSlow)      invTau = kInvTauSlow;
    else if (ratio >= kRatioFast) invTau = kInvTauFast;
    else {
        const float t = (ratio - kRatioSlow) / (kRatioFast - kRatioSlow);
        invTau = kInvTauSlow + t * (kInvTauFast - kInvTauSlow);
    }
    // How far the gain may travel this block.
    //
    // The one-pole is evaluated **once per block** and the result ramped across
    // the block linearly, exactly as the wall's gain is. Over 64 frames that is
    // indistinguishable from filtering per sample, and it is what makes the
    // boost cap below a true slew in dB per second: capping a *per-sample*
    // coefficient compounds with the filter, and the gain then rises orders of
    // magnitude slower than the constant says. That cost a debugging round.
    const float blockCoef =
        clampf(invTau * (float)frames / m_sampleRate, 0.0f, 1.0f);

    float gainEnd = m_gain + (m_want - m_gain) * blockCoef;

    // A rise has to persist before it is believed, and even then it is let out
    // gradually rather than all at once - see the `Boost` wait and the note
    // on the cliff below it. Cutting is never delayed or limited: it is the
    // direction that protects the signal.
    // ---- the first arrival after a reset ---------------------------------
    //
    // The persistence rule tells a quiet passage from a quiet track by how long
    // the quiet lasts, which presumes a level to have dropped away from. At
    // power-on there is none, so it has nothing to do and the only thing it
    // achieves is four seconds of the first track played at unity. This is the
    // window where it stands aside. See the header.
    //
    // **Programme only.** A box powered up an hour before anyone presses play
    // must not spend this waiting on silence, so the count advances on gated
    // blocks and the window is closed by arriving rather than by the clock.
    const bool firstArrival = (m_arriveBlocks >= m_primeBlocks);

    if (m_arriveBlocks >= 0 && programme) {
        // Two ways out, and the second is what bounds this. Either the gain
        // has arrived - within the rate law's own "close enough" ratio, the
        // same 1 dB that puts it on the slow constant - or it has used the
        // whole of the wait it replaced, after which the ordinary rules apply
        // whatever happened. So the worst case is the behaviour it replaced.
        if (ratio <= kRatioSlow || m_arriveBlocks >= m_boostDelay) {
            // **Hand over with the persistence rule already satisfied**, but
            // only if this window actually did something. The rule exists to
            // decide whether a rise is real; the window has just spent itself
            // deciding exactly that, so re-imposing the wait on the way out
            // would freeze the last dB for four seconds having already
            // answered the question. Without this the gain stalls one dB short
            // and the box is not levelled until the counter runs down anyway,
            // which is the whole complaint.
            //
            // The guard matters. A source that is already at Target leaves
            // this window on the first block without the fast path ever
            // engaging, and crediting it there would hand a free pass to the
            // first quiet passage that came along - which is the spike bug,
            // bought back for nothing. Nothing was corrected, so nothing needs
            // believing.
            if (firstArrival) m_boostBlocks = m_boostDelay + m_boostRamp;
            m_arriveBlocks = -1;
        } else {
            ++m_arriveBlocks;
        }
    }

    if (m_want > m_gain) {
        ++m_boostBlocks;

        // Inside the first arrival, gainEnd keeps what the rate law gave it:
        // no hold, no slew cap. It is driven, not assigned - a rise of this
        // size gets exactly the speed the same rise would get mid-programme,
        // so there is no step here for anything to click on.
        if (!firstArrival) {
            const int past = m_boostBlocks - m_boostDelay;
            if (past <= 0) {
                gainEnd = m_gain;               // not believed yet: hold
            } else {
                const float open = (past >= m_boostRamp)
                                 ? 1.0f
                                 : (float)past / (float)m_boostRamp;
                const float cap = m_gain * (1.0f + m_boostStep * open);
                if (gainEnd > cap) gainEnd = cap;
            }
        }
    } else {
        m_boostBlocks = 0;
    }

    const float gStep = (gainEnd - m_gain) / (float)frames;

    // ---- apply the leveller, and find the peak it produces ----------------
    //
    // The gain moves every frame rather than every block. That is two flops,
    // and it is what keeps a change of gain from arriving as a step at a block
    // boundary - a step is a click, however small the step is.
    //
    // The peak is taken across channels rather than per channel, so a
    // transient on one side pulls both down together and the stereo image does
    // not shift sideways every time the wall works.
    float peak = 0.0f;
    for (int f = 0; f < frames; ++f) {
        m_gain += gStep;

        float *fr = samples + f * ch;
        for (int c = 0; c < ch; ++c) {
            const float v = fr[c] * m_gain;
            fr[c] = v;

            const float a = (v < 0.0f) ? -v : v;
            if (a > peak) peak = a;
        }
    }

    // What this block will need when its turn comes, one block from now.
    // The divide happens only on blocks that actually reach the ceiling.
    const float reqNext = (peak > m_ceiling) ? (m_ceiling / peak) : 1.0f;

    // ---- where this block's wall ramp ends -------------------------------
    float target = (m_req < reqNext) ? m_req : reqNext;

    // Coming back up is smoothed; going down is not, because going down is
    // what the lookahead exists to make instant. Smoothing only ever moves
    // target *below* the minimum above, so the invariant survives it.
    if (target > m_wall)
        target = m_wall + (target - m_wall) * m_wallRel;

    const float step = (target - m_wall) / (float)frames;
    float       g    = m_wall;

    // ---- hand over the delayed block, and take this one in ---------------
    //
    // No circular index: the line holds exactly one block and is consumed in
    // order, so frame f of the output is frame f of the line, every time.
    for (int f = 0; f < frames; ++f) {
        float *fr   = samples + f * ch;
        float *slot = m_delay + f * ch;

        for (int c = 0; c < ch; ++c) {
            const float out = slot[c] * g;
            slot[c] = fr[c];          // this block, waiting its turn

            // Unreachable given the schedule above, and kept for the two cases
            // that sit outside it: a caller handing over more frames than the
            // line holds, and an input that arrives beyond full scale. If this
            // line ever fires the schedule is wrong, not slow.
            fr[c] = clampf(out, -m_ceiling, m_ceiling);
        }
        g += step;
    }

    m_wall = target;
    m_req  = reqNext;
}

void AutoGain::enableFlushToZero()
{
#if defined(__ARM_FP) && (__ARM_FP > 0)
    // FPSCR bit 24 is the flush-to-zero flag. Denormals on a Cortex-M33 are
    // handled in software and cost hundreds of cycles apiece, which the tail
    // of the release can walk into on a fade to silence.
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
}
