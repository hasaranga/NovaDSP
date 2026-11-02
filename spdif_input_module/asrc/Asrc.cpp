// Asrc.cpp - see Asrc.h for why this exists and what it deliberately leaves
// to its caller.

#include "Asrc.h"

#include <math.h>
#include <string.h>

namespace {

// ---- kernel shape -------------------------------------------------------
//
// Cutoff as a fraction of the *input* Nyquist. Not 1.0, because a brickwall
// at Nyquist has no transition band to put the stopband in; the kernel would
// have to be far longer to get the same rejection.
//
// **0.95 was measured, not chosen.** Three values were run through the host
// suite at 32 taps, and what it costs is one dB of stopband:
//
//   kCutoff   19 kHz   20 kHz   worst spur 44.1->48   worst spur 32->48
//   0.92      -1.51    -4.79         -102.9 dB            -95.6 dB
//   0.95      -0.55    -2.42         -101.9 dB            -99.0 dB
//   0.97      -0.23    -1.34          -99.2 dB            -93.6 dB
//
// 0.92 gives away real top end on a 44.1 kHz source for rejection nothing
// needs. 0.97 costs 3 dB of stopband for another 1 dB of passband, and it is
// the 32 kHz row that pays - a wider kernel relative to that Nyquist puts the
// images closer in. 0.95 is where the trade stops being worth taking.
//
// If the top end ever has to come up further, raise kCutoff and ASRC_TAPS
// **together**. Raising it alone buys passband and pays for it in the
// stopband, where the images are - which is what the 0.97 row is showing.
const double kCutoff = 0.95;

// Kaiser beta. 8.6 is the classic ~-90 dB sidelobe point; with the transition
// band kCutoff leaves and 32 taps, the suite measures what actually comes out.
const double kKaiserBeta = 8.6;

// Rates outside this band around the output are treated as an estimator that
// has lost the signal rather than as a real rate. 0.6 admits 32 kHz against a
// 48 kHz output with margin.
//
// **The upper bound is 1.02, not 1.00, and that matters more than it looks.**
// 1.00 is the obvious value - anything above the output rate is decimation,
// which this kernel cannot do - and it is wrong, because the *nominal* ratio
// is not the ratio the loop asks for. A 48 kHz source that is 100 ppm fast
// needs 1.0001, and clamping that to 1.0 does not merely round it: this
// function **silently holds the previous rate**, so the correction never
// lands at all. The buffer then fills without bound, overruns, drops frames,
// and the symptom is a click several seconds after the event that caused it.
// It only shows up when the source is *fast*, so an exactly-48000 test source
// never produces it.
//
// 2 % is far more headroom than any crystal needs and is still nowhere near
// decimation: the kernel cuts at 0.95 of the input Nyquist, and at a ratio of
// 1.02 the output Nyquist is still 0.98 of it.
const double kMinRatio = 0.60;
const double kMaxRatio = 1.02;

// Modified Bessel function of the first kind, order zero. Series form - this
// runs ASRC_TAPS*(ASRC_PHASES+1) times at init and never again.
//
// **"And never again" is a requirement, not an observation.** It is `double`,
// which is software on the M33, and 4128 entries of it - plus a `sin` and a
// `sqrt` each - is tens of milliseconds. It was reachable from the block path
// once, through Asrc::init() in SpdifInput::reset(), and the symptom was the
// overrun counter climbing steadily with *no source connected* while a playing
// source was clean: the divider scan only steps when nothing has framed.
double bessel_i0(double x)
{
    double sum  = 1.0;
    double term = 1.0;
    const double half_x_sq = (x * 0.5) * (x * 0.5);
    for (int k = 1; k < 40; ++k) {
        term *= half_x_sq / ((double)k * (double)k);
        sum  += term;
        if (term < sum * 1.0e-17) break;
    }
    return sum;
}

double sinc_pi(double x)
{
    if (x > -1.0e-9 && x < 1.0e-9) return 1.0;
    const double px = 3.14159265358979323846 * x;
    return sin(px) / px;
}

} // namespace

Asrc::Asrc()
    : m_head(0), m_phase(0), m_step((uint64_t)1 << 32),
      m_inRateHz(48000.0f), m_outRateHz(48000.0f),
      m_totalIn(0), m_totalOut(0), m_primed(false)
{
    memset(m_kernel, 0, sizeof m_kernel);
    memset(m_hist,   0, sizeof m_hist);
}

void Asrc::init(float outputRateHz)
{
    buildKernel();
    restart(outputRateHz);
}

// Everything init() does except the table. Split out because the only caller
// that used to exist was on the block path: see the note against restart() in
// the header.
void Asrc::restart(float outputRateHz)
{
    m_outRateHz = (outputRateHz > 1.0f) ? outputRateHz : 48000.0f;
    m_inRateHz  = m_outRateHz;
    reset();
}

void Asrc::reset()
{
    memset(m_hist, 0, sizeof m_hist);
    m_head     = 0;
    m_phase    = 0;
    m_totalIn  = 0;
    m_totalOut = 0;
    m_primed   = false;
    setInputRate(m_inRateHz);
}

// The interpolation point sits between history slots (kTaps/2 - 1) and
// (kTaps/2), so the output lags the newest input frame by about half the
// kernel. That latency is the price of a symmetric (linear phase) kernel and
// it is small: 16 input frames is 0.36 ms at 44.1 kHz.
void Asrc::buildKernel()
{
    const double centre = (double)(ASRC_TAPS / 2 - 1);
    const double halfLen = (double)ASRC_TAPS * 0.5;
    const double i0beta = bessel_i0(kKaiserBeta);

    for (int q = 0; q <= ASRC_PHASES; ++q) {
        const double frac = (double)q / (double)ASRC_PHASES;

        double sum = 0.0;
        for (int k = 0; k < ASRC_TAPS; ++k) {
            // Distance from the interpolation point to tap k, in input
            // sample periods.
            const double x = (double)k - centre - frac;

            // Kaiser window argument, normalised so the window reaches zero
            // at the two ends of the kernel.
            const double r = x / halfLen;
            double w = 0.0;
            if (r > -1.0 && r < 1.0)
                w = bessel_i0(kKaiserBeta * sqrt(1.0 - r * r)) / i0beta;

            const double h = sinc_pi(x * kCutoff) * w;
            m_kernel[q][k] = (float)h;
            sum += h;
        }

        // Normalise every branch to unit DC gain. Without this the gain
        // wobbles with the fractional phase, which is a fs_out-rate amplitude
        // modulation of the signal - it measures as a noise floor that rises
        // with level and it is not obvious from looking at the kernel.
        if (sum > 1.0e-12 || sum < -1.0e-12) {
            const float g = (float)(1.0 / sum);
            for (int k = 0; k < ASRC_TAPS; ++k)
                m_kernel[q][k] *= g;
        }
    }
}

void Asrc::setInputRate(float hz)
{
    if (!(hz > 0.0f)) return;               // also rejects NaN

    double ratio = (double)hz / (double)m_outRateHz;
    if (ratio < kMinRatio || ratio > kMaxRatio) return;   // hold the last good one

    m_inRateHz = hz;
    m_step = (uint64_t)(ratio * 4294967296.0 + 0.5);
    if (m_step == 0) m_step = 1;

    // **The ceiling is kMaxRatio, not 1.0, and it used to be 1.0.**
    //
    // The accept band above was widened from 1.00 to 1.02 to fix exactly the
    // fault its comment describes - a source slightly fast, every upward trim
    // silently discarded, the buffer filling without bound. **That fix did not
    // work, because this line clamped the step back to 1.0 three lines later.**
    // The band let the rate in and this threw it away, so the symptom was
    // identical and the comment above read as though it had been dealt with.
    //
    // Found on hardware, by an optical loopback: GP22 out to GP1 in, so the
    // source rate *is* the output rate and the ring started above target. The
    // loop asked for 1.0005, got 1.0, asked harder, and wound the integrator
    // into its +1000 ppm rail with the ring frozen at 324 frames for 75
    // seconds. **A 44.1 kHz source never produces it** - ratio 0.919 is nowhere
    // near this ceiling - which is why a year of 44.1 testing never saw it.
    //
    // A ratio above 1 is decimation and the kernel does not narrow for it, but
    // 1.02 is not decimation in any meaningful sense: the cutoff is 0.95 of the
    // input Nyquist, so at 1.02 the output Nyquist is still 0.98 of the cutoff.
    // That is the same argument kMaxRatio is chosen by, which is the point -
    // **there is one bound here now, not two that can disagree.**
    const uint64_t kStepMax = (uint64_t)(kMaxRatio * 4294967296.0);
    if (m_step > kStepMax) m_step = kStepMax;
}

int Asrc::inputFramesNeeded(int outFrames) const
{
    if (outFrames <= 0) return 0;
    // step is Q32 input frames per output frame; add the kernel's own
    // priming plus one for the partial phase already banked.
    const uint64_t need = ((uint64_t)outFrames * m_step) >> 32;
    return (int)need + 2;
}

inline void Asrc::pushFrame(float l, float r)
{
    m_hist[m_head * 2 + 0] = l;
    m_hist[m_head * 2 + 1] = r;
    m_head = (m_head + 1) & (ASRC_TAPS - 1);
    ++m_totalIn;
    if (!m_primed && m_totalIn >= (uint64_t)ASRC_TAPS) m_primed = true;
}

int Asrc::produce(float *out, int outFrames,
                  const float *in, int inFrames, int *inUsed)
{
    int used = 0;
    int made = 0;

    if (inUsed) *inUsed = 0;
    if (!out || outFrames <= 0) return 0;

    const uint64_t kOne = (uint64_t)1 << 32;

    // Phase index and the bits left over for interpolating between branches.
    // ASRC_PHASES is a power of two, so both are shifts.
    const int qShift = 32;
    int phaseBits = 0;
    for (int p = ASRC_PHASES; p > 1; p >>= 1) ++phaseBits;
    const int fracShift = qShift - phaseBits;

    while (made < outFrames) {

        // Advance the input until the interpolation point is inside the
        // current window. step <= 1<<32 so this runs at most once, but it is
        // written as a loop because that is what makes it correct rather than
        // merely true for the rates we accept today.
        while (m_phase >= kOne) {
            if (used >= inFrames) goto done;      // starved - caller's problem
            pushFrame(in[used * 2 + 0], in[used * 2 + 1]);
            ++used;
            m_phase -= kOne;
        }

        // Prime the history before emitting anything, or the first kTaps
        // outputs are a filter convolving with the zeros it was cleared to.
        if (!m_primed) {
            if (used >= inFrames) goto done;
            pushFrame(in[used * 2 + 0], in[used * 2 + 1]);
            ++used;
            continue;
        }

        {
            const uint32_t ph = (uint32_t)m_phase;
            const int   q     = (int)(ph >> fracShift);
            const float mu    = (float)(ph & ((1u << fracShift) - 1))
                              * (1.0f / (float)(1u << fracShift));

            const float *k0 = m_kernel[q];
            const float *k1 = m_kernel[q + 1];

            const int base = m_head;    // oldest of the kTaps stored frames

            float accL = 0.0f;
            float accR = 0.0f;
            for (int k = 0; k < ASRC_TAPS; ++k) {
                const int   idx = ((base + k) & (ASRC_TAPS - 1)) * 2;
                const float c   = k0[k] + (k1[k] - k0[k]) * mu;
                accL += m_hist[idx + 0] * c;
                accR += m_hist[idx + 1] * c;
            }

            out[made * 2 + 0] = accL;
            out[made * 2 + 1] = accR;
            ++made;
            ++m_totalOut;
        }

        m_phase += m_step;
    }

done:
    if (inUsed) *inUsed = used;
    return made;
}
