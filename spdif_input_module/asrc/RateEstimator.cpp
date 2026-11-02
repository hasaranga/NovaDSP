// RateEstimator.cpp - see RateEstimator.h for why the loop closes on buffer
// level rather than on measured frequency.

#include "RateEstimator.h"

#include <math.h>

namespace {

// ---- the rates we handle -------------------------------------------------
//
// At or below the 48 kHz output, because the ASRC's kernel only reconstructs
// and cannot decimate. 96 kHz is not "unsupported yet" - it is a different
// filter design. See spdif_input_module/README.md.
const int   kRates[]  = { 32000, 44100, 48000 };
const int   kNumRates = 3;

// How close a measured ratio has to be to a nominal one to be called that
// rate. The three are 9 % apart at their closest (44.1 against 48), so 1 % is
// enormous headroom for classification and still tight enough that an
// unsupported rate - 88.2, 96, or a source in trouble - is rejected rather
// than snapped to the nearest thing we know.
const float kClassifyTol = 0.01f;

// Classification window, in output frames. 12000 is a quarter of a second:
// long enough that +-1 frame of quantisation is 80 ppm (irrelevant at 1 %
// tolerance), short enough that a source changing rate between tracks is
// noticed promptly.
const int32_t kWindowOut = 12000;

// ---- loop gains ----------------------------------------------------------
//
// The plant is an integrator: a ppm of uncorrected rate error fills the buffer
// at k frames a second, where
//
//     k = fs_out * 1e-6 = 0.048 frames/sec per ppm
//
// so with u = Kp*e + I and dI/dt = Ki*e the closed loop is
//
//     s^2 + k*Kp*s + k*Ki = 0
//     wn = sqrt(k*Ki)              zeta = (Kp/2) * sqrt(k/Ki)
//
// **These were derived, then corrected against a measurement.** The first
// pair - Kp 10, Ki 0.5 - gave zeta 1.55 and a slow pole of 17.6 s, which
// looked reasonable and was not: the integrator was still winding up two
// minutes in, so the buffer sat several frames off target and the P term was
// quietly supplying a fifth of the correction. Nothing was unstable and
// nothing drifted. It just never actually arrived, and an hour-long run
// showed the trim still climbing from 67 to 100 ppm.
//
// Kp 15, Ki 2.5 gives wn 0.346 rad/s and zeta 1.04 - poles at 3.8 s and
// 2.2 s, so settled in about fifteen seconds with no overshoot worth the name.
//
//     Kp = 15, Ki = 2.5  ->  wn 0.346, zeta 1.04, slow pole tau 3.8 s
//
// **Sizing note for whoever picks the buffer target**: before the integrator
// winds up, the P term alone has to supply the whole correction, so the
// transient level excursion during acquisition is about delta/Kp frames - 33
// frames for a source 500 ppm out. The target has to clear that in both
// directions.
const float kKp = 15.0f;

// ppm per (frame of error * second).
const float kKi = 2.5f;

// The integrator is clamped here. A crystal 1000 ppm out is not a crystal, so
// hitting this means the input is not what we think it is, and winding up
// further would only make recovery slower.
const float kTrimLimit = 1000.0f;

// Smoothing on the buffer level before it reaches the P term. The level moves
// in whole frames, so it is inherently quantised; this takes the step edges
// off without eating loop phase margin (50 ms against a 2 s loop).
const float kLevelTau = 0.050f;

// Lock declaration: the smoothed level has to sit inside this many frames of
// target for this many consecutive blocks. 750 blocks is one second at the
// 64-frame block rate.
const float   kLockBand   = 4.0f;
const int32_t kLockBlocks = 750;

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

RateEstimator::RateEstimator()
    : m_outRateHz(48000.0f), m_targetLevel(0),
      m_state(kNoCarrier), m_nominalHz(0), m_rateHz(0.0f), m_trimPpm(0.0f),
      m_levelAvg(0.0f), m_winIn(0), m_winOut(0), m_lockCount(0),
      m_carrierPresent(false)
{
}

void RateEstimator::init(float outputRateHz, int targetLevel)
{
    m_outRateHz   = (outputRateHz > 1.0f) ? outputRateHz : 48000.0f;
    m_targetLevel = targetLevel > 0 ? targetLevel : 1;
    reset();
}

void RateEstimator::reset()
{
    m_state     = kNoCarrier;
    m_nominalHz = 0;
    m_rateHz    = 0.0f;
    m_trimPpm   = 0.0f;
    m_levelAvg  = (float)m_targetLevel;
    m_winIn     = 0;
    m_winOut    = 0;
    m_lockCount = 0;
    m_carrierPresent = false;
}

void RateEstimator::setCarrier(bool present)
{
    if (!present) {
        // Drop everything. Coming back is a full re-acquire, because a
        // carrier that returns may well be a different source at a different
        // rate, and carrying the old trim into it would start the loop off
        // pointing the wrong way.
        if (m_carrierPresent) reset();
        m_carrierPresent = false;
        return;
    }

    // **On the transition only.** Clearing the window whenever this is called
    // with the carrier already up is the obvious-looking version and it is a
    // deadlock: the state stays kNoCarrier until the first window *closes*,
    // so a caller that reports the carrier every block - which is the natural
    // thing to do - resets the window every block and it never closes. It
    // sits in Acquiring for ever with the rate unclassified.
    //
    // test_rate.cpp did not catch that because it calls setCarrier() once at
    // the start. SpdifInput calls it per block, which is what found it.
    if (!m_carrierPresent) {
        m_carrierPresent = true;
        // Carrier just appeared. Leave the state at kNoCarrier until the
        // first classification window closes - there is genuinely nothing
        // known yet, and reporting kAcquiring would invite the module to
        // start unmuting against a ratio nobody has measured.
        m_winIn  = 0;
        m_winOut = 0;
    }
}

void RateEstimator::classify(float ratio)
{
    int   best     = 0;
    float bestErr  = 1.0e9f;

    for (int i = 0; i < kNumRates; ++i) {
        const float nom = (float)kRates[i] / m_outRateHz;
        const float err = fabsf(ratio - nom) / nom;
        if (err < bestErr) { bestErr = err; best = kRates[i]; }
    }

    if (bestErr > kClassifyTol) {
        // Carrier present, rate is not one we handle. **This is a reported
        // state and not a silent fallback**: running a 96 kHz source through
        // a kernel that cannot decimate would alias, and snapping it to 48
        // would play it at half speed. Either is worse than muting and saying
        // so on the panel.
        m_state     = kUnsupported;
        m_nominalHz = 0;
        m_rateHz    = 0.0f;
        m_lockCount = 0;
        return;
    }

    if (best != m_nominalHz) {
        // A new rate, including the first one. The trim belonged to the old
        // nominal and means nothing against this one.
        m_nominalHz = best;
        m_trimPpm   = 0.0f;
        m_lockCount = 0;
        m_state     = kAcquiring;
    }
}

float RateEstimator::update(int framesArrived, int outFrames, int bufferLevel)
{
    if (outFrames <= 0) return usable() ? m_rateHz : 0.0f;

    if (m_state == kNoCarrier && framesArrived == 0) {
        // Nothing arriving and nothing known. Keep the level average parked
        // at target so that when a carrier does appear the P term does not
        // open with a large bogus error.
        m_levelAvg = (float)m_targetLevel;
        return 0.0f;
    }

    // ---- classification window ------------------------------------------
    m_winIn  += framesArrived;
    m_winOut += outFrames;
    if (m_winOut >= kWindowOut) {
        classify((float)m_winIn / (float)m_winOut);
        m_winIn  = 0;
        m_winOut = 0;
    }

    if (m_state == kNoCarrier || m_state == kUnsupported) return 0.0f;

    // ---- level smoothing -------------------------------------------------
    //
    // dt is the block period expressed from the output rate, which is exact
    // here for the same reason the ratio measurement needs no timestamps.
    const float dt    = (float)outFrames / m_outRateHz;
    const float alpha = dt / (kLevelTau + dt);
    m_levelAvg += ((float)bufferLevel - m_levelAvg) * alpha;

    // ---- PI on level error ----------------------------------------------
    //
    // Sign: a filling buffer means input is arriving faster than we are
    // consuming it, so we must consume faster - a *higher* ratio, therefore a
    // higher reported input rate. Getting this backwards produces a loop that
    // drives the buffer into whichever rail it started nearest, which reads
    // as "the estimator does not work at all" rather than as a sign error.
    const float err = m_levelAvg - (float)m_targetLevel;

    m_trimPpm += kKi * err * dt;
    m_trimPpm  = clampf(m_trimPpm, -kTrimLimit, kTrimLimit);

    const float ppm = clampf(m_trimPpm + kKp * err, -kTrimLimit, kTrimLimit);

    m_rateHz = (float)m_nominalHz * (1.0f + ppm * 1.0e-6f);

    // ---- lock declaration ------------------------------------------------
    if (fabsf(err) <= kLockBand) {
        if (m_lockCount < kLockBlocks) ++m_lockCount;
        if (m_lockCount >= kLockBlocks) m_state = kLocked;
    } else {
        // Falling out of the band drops lock but does **not** reset the trim.
        // The loop is still tracking; what changed is that the module should
        // stop calling this settled. Throwing the trim away here would turn
        // every transient into a full re-acquire.
        m_lockCount = 0;
        if (m_state == kLocked) m_state = kAcquiring;
    }

    return m_rateHz;
}
