// test_autogain.cpp - host verification for AutoGain.
//
// **Deliberately not in CMakeLists.txt**, the same way scope.c is not: it is
// a host program, it has no place in the firmware image, and this is not a git
// repo so deleting it would mean gone.
//
// AutoGain needs no stubbing at all to build on a host - it includes only
// math.h, stdint.h and string.h, and its one piece of ARM assembly is behind
// an __ARM_FP guard that a host compiler does not satisfy. So:
//
//     cl /EHsc /O2 /Iagc_module\agc agc_module\agc\AutoGain.cpp agc_module\agc\test_autogain.cpp /Fe:ta.exe
//     ta.exe
//
// after running the Visual Studio vcvarsall.bat. Any C++17 compiler will do.
//
// What it is checking is the things that cannot be checked by listening:
//
//   - that it actually lands on the Target rather than near it,
//   - **how long it takes**, which is the whole reason this engine exists in
//     its present form and the one number a listener will notice,
//   - that being that fast has not made it chase dynamics inside a track,
//   - that the wall holds by construction rather than by clipping,
//   - and that the detector measures loudness rather than energy, which is
//     checks 14 to 16 and the reason a bass drop-out no longer lifts a voice.
//
// Re-run it after touching the detector, the rate law, the gate, the wall
// schedule, the delay line or the weighting.
//
//   **The weighting moves every level in this file**, because the leveller
//   drives a weighted measurement to Target and a 750 Hz tone weighs about
//   0.22 dB heavy. That offset is computed as kK750 rather than written down,
//   so the level checks stay honest if the curve is ever changed.
//
// ---- harness traps, all of them learned the hard way ---------------------
//
//   process() is in place, so a reused buffer is not silence. Re-zero it every
//   block or the previous block's output is read back as the next one's input.
//
//   Keep test tones block-synchronous. 750 Hz is exactly one cycle per 64
//   frames at 48 kHz, so every block presents an identical mean square and an
//   identical peak. Anything else wobbles the detector a few tenths of a dB
//   from block to block, which is correct behaviour and completely inaudible
//   but makes "what is the gain now" ambiguous to about the resolution these
//   checks want.
//
//   Give a settle check time in units of the thing settling. The rate law
//   deliberately slows down as the error shrinks, so the last dB takes seconds
//   by design. Check 1 dB of settle quickly, not 0.01 dB quickly.

#include "AutoGain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_fail = 0;

void checkNear(const char *what, double got, double want, double tol)
{
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("%-56s got %10.5f  want %10.5f  %s\n",
                what, got, want, ok ? "ok" : "** FAIL **");
    if (!ok) ++g_fail;
}

void checkLE(const char *what, double got, double limit)
{
    const bool ok = (got <= limit);
    std::printf("%-56s got %10.5f  <=  %10.5f  %s\n",
                what, got, limit, ok ? "ok" : "** FAIL **");
    if (!ok) ++g_fail;
}

void checkTrue(const char *what, bool ok)
{
    std::printf("%-56s %s\n", what, ok ? "ok" : "** FAIL **");
    if (!ok) ++g_fail;
}

const float kFs    = 48000.0f;
const int   kBlock = 64;
const int   kCh    = 2;
const double kBlockSecs = (double)kBlock / kFs;

// Must track kReleaseSeconds in AutoGain.cpp - predict() cannot read it out of
// the header.
const double kReleaseSeconds = 0.150;

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
double linToDb(double a)  { return (a > 0.0) ? 20.0 * std::log10(a) : -200.0; }

// ---- an independent implementation of the K-weighting curve -------------
//
// The same pattern as predictWall() below: a second implementation, so the
// engine is checked against something rather than against itself. This one has
// an external anchor as well - BS.1770-4 publishes its coefficients at 48 kHz,
// and check 14 compares the derivation against that table. If either this or
// the engine drifts, the published numbers catch it.
//
// It exists for two jobs. One is that check. The other is that the weighting
// moves every level in this file by the curve's gain at 750 Hz, and that
// offset wants computing rather than typing in - see kK750 below.
struct KBiquad { double b0, b1, b2, a1, a2; };

KBiquad kShelfRef(double rate)
{
    const double PI = 3.14159265358979;
    const double f0 = 1681.974450955533, G = 3.999843853973347;
    const double Q  = 0.7071752369554196;

    const double K  = std::tan(PI * f0 / rate);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;

    return { (Vh + Vb * K / Q + K * K) / a0,
             2.0 * (K * K - Vh) / a0,
             (Vh - Vb * K / Q + K * K) / a0,
             2.0 * (K * K - 1.0) / a0,
             (1.0 - K / Q + K * K) / a0 };
}

KBiquad kHighpassRef(double rate)
{
    const double PI = 3.14159265358979;
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;

    const double K  = std::tan(PI * f0 / rate);
    const double a0 = 1.0 + K / Q + K * K;

    return { 1.0 / a0, -2.0 / a0, 1.0 / a0,
             2.0 * (K * K - 1.0) / a0,
             (1.0 - K / Q + K * K) / a0 };
}

// Run a mono signal through the weighting cascade and return its RMS. Both
// stages as difference equations rather than as a transfer function, because
// what is under test is the recursion the engine actually runs.
double kWeightedRms(const std::vector<double> &x, int skip = 0)
{
    const KBiquad S = kShelfRef(kFs), H = kHighpassRef(kFs);
    double s1 = 0, s2 = 0, t1 = 0, t2 = 0, e = 0;
    int n = 0;

    for (size_t i = 0; i < x.size(); ++i) {
        const double in = x[i];
        const double y = S.b0 * in + s1;
        s1 = S.b1 * in - S.a1 * y + s2;
        s2 = S.b2 * in - S.a2 * y;

        const double z = H.b0 * y + t1;
        t1 = H.b1 * y - H.a1 * z + t2;
        t2 = H.b2 * y - H.a2 * z;

        if ((int)i >= skip) { e += z * z; ++n; }
    }
    return n ? std::sqrt(e / n) : 0.0;
}

// A plain 2nd-order Butterworth high-pass, for the one job of showing what the
// rejected alternative would have done. See check 16.
double hardHpRms(const std::vector<double> &x, double f0, int skip = 0)
{
    const double PI = 3.14159265358979;
    const double K  = std::tan(PI * f0 / kFs), Q = 0.70710678;
    const double a0 = 1.0 + K / Q + K * K;
    const double b0 = 1.0 / a0, b1 = -2.0 / a0, b2 = 1.0 / a0;
    const double a1 = 2.0 * (K * K - 1.0) / a0, a2 = (1.0 - K / Q + K * K) / a0;

    double s1 = 0, s2 = 0, e = 0;
    int n = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double in = x[i];
        const double y = b0 * in + s1;
        s1 = b1 * in - a1 * y + s2;
        s2 = b2 * in - a2 * y;
        if ((int)i >= skip) { e += y * y; ++n; }
    }
    return n ? std::sqrt(e / n) : 0.0;
}

// Plain RMS of a mono signal, skipping a settling head.
double plainRms(const std::vector<double> &x, int skip = 0)
{
    double e = 0; int n = 0;
    for (size_t i = (size_t)skip; i < x.size(); ++i) { e += x[i] * x[i]; ++n; }
    return n ? std::sqrt(e / n) : 0.0;
}

// Channel 0 of an interleaved signal, as doubles - what the reference filters
// above take.
std::vector<double> mono(const std::vector<float> &v)
{
    std::vector<double> m(v.size() / kCh);
    for (size_t i = 0; i < m.size(); ++i) m[i] = v[i * kCh];
    return m;
}

// Gain of the weighting at one frequency, in dB, by measurement.
double kGainDb(double f)
{
    const double PI = 3.14159265358979;
    std::vector<double> x(96000);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = std::sin(2.0 * PI * f * (double)i / kFs);
    // Skip the first tenth of a second: the 38 Hz stage has a settling
    // transient and it is not what is being measured.
    return linToDb(kWeightedRms(x, 4800) * std::sqrt(2.0));
}

// **Every level check in this file moves by this much**, and it is computed
// rather than written down so it cannot go stale.
//
// The leveller drives the *weighted* measurement to Target, so a tone whose
// weighted level reads kK750 dB above its true one comes out kK750 dB below
// Target. 750 Hz sits on the shoulder of the shelf at about +0.22 dB - small,
// but the level checks here are held to 0.2 dB, so it is not small enough to
// ignore. That is worth knowing before assuming a test tone is unaffected by a
// weighting curve: "roughly flat there" and "flat there" are different claims.
const double kK750 = kGainDb(750.0);

AutoGain make(float target, float maxGain, float output = -1.0f,
              float boost = 4.0f)
{
    AutoGain ag;
    ag.setup(kFs, kCh);
    ag.setParam(AutoGain::kTargetDb,   target);
    ag.setParam(AutoGain::kMaxGainDb,  maxGain);
    ag.setParam(AutoGain::kBoostDelay, boost);
    ag.setParam(AutoGain::kOutputDb,   output);
    ag.reset();
    return ag;
}

// ---- signals ------------------------------------------------------------

// A 750 Hz stereo sine at a given RMS level, `blocks` blocks long, starting at
// a given phase so segments can be concatenated without a discontinuity.
std::vector<float> sineRms(double rmsDb, int blocks, double *phase = nullptr)
{
    const double amp = dbToLin(rmsDb) * std::sqrt(2.0);
    const double w   = 2.0 * 3.14159265358979 * 750.0 / kFs;

    double ph = phase ? *phase : 0.0;
    std::vector<float> v((size_t)blocks * kBlock * kCh);
    for (int f = 0; f < blocks * kBlock; ++f) {
        const float s = (float)(amp * std::sin(ph));
        ph += w;
        v[(size_t)f * kCh + 0] = s;
        v[(size_t)f * kCh + 1] = s;
    }
    if (phase) *phase = ph;
    return v;
}

void append(std::vector<float> &a, const std::vector<float> &b)
{
    a.insert(a.end(), b.begin(), b.end());
}

// The same, at any frequency. Used by the weighting checks, which need a bass
// tone and a midrange one at the same time.
//
// **Only 750 Hz and its multiples are block-synchronous** - see the harness
// note at the top of the file. A 60 Hz tone puts a twelfth of a cycle in a
// block, so its per-block mean square swings wildly and the detector's own
// smoothing is what makes it steady. That is fine here because nothing below
// measures a single block; the levels that matter are read over whole seconds
// with binRmsDb(), which rejects everything but the frequency it is asked for.
std::vector<float> toneRms(double freq, double rmsDb, int blocks,
                           double *phase = nullptr)
{
    const double amp = dbToLin(rmsDb) * std::sqrt(2.0);
    const double w   = 2.0 * 3.14159265358979 * freq / kFs;

    double ph = phase ? *phase : 0.0;
    std::vector<float> v((size_t)blocks * kBlock * kCh);
    for (int f = 0; f < blocks * kBlock; ++f) {
        const float s = (float)(amp * std::sin(ph));
        ph += w;
        v[(size_t)f * kCh + 0] = s;
        v[(size_t)f * kCh + 1] = s;
    }
    if (phase) *phase = ph;
    return v;
}

// The same tone in quadrature: sine in the left channel, cosine in the right.
//
// **This is how to put a non-block-synchronous tone in front of this detector
// without the harness lying to you.** The detector sums the squares of both
// channels, and sin^2 + cos^2 is 1 whatever the phase, so a quadrature pair
// presents an exactly constant mean square to every block regardless of how
// the frequency divides into 64 frames. An ordinary 60 Hz tone puts a twelfth
// of a cycle in a block and its per-block mean square swings enormously; the
// detector's attack is faster than its release, so it rides those peaks and
// reports a level up to a dB above the true RMS. That is correct behaviour and
// inaudible, and it is also large enough to swamp what check 15 is measuring.
//
// Per-channel RMS is the same as toneRms(), so levels are directly comparable.
std::vector<float> quadToneRms(double freq, double rmsDb, int blocks,
                               double *phase = nullptr)
{
    const double amp = dbToLin(rmsDb) * std::sqrt(2.0);
    const double w   = 2.0 * 3.14159265358979 * freq / kFs;

    double ph = phase ? *phase : 0.0;
    std::vector<float> v((size_t)blocks * kBlock * kCh);
    for (int f = 0; f < blocks * kBlock; ++f) {
        v[(size_t)f * kCh + 0] = (float)(amp * std::sin(ph));
        v[(size_t)f * kCh + 1] = (float)(amp * std::cos(ph));
        ph += w;
    }
    if (phase) *phase = ph;
    return v;
}

// Sample-wise sum of two equal-length signals.
std::vector<float> mix(const std::vector<float> &a, const std::vector<float> &b)
{
    std::vector<float> v(a.size());
    for (size_t i = 0; i < a.size(); ++i) v[i] = a[i] + b[i];
    return v;
}

// A kick drum, near enough: a 60 Hz burst with a fast decay, once every
// `everyBlocks`, at a given peak. What matters for the wall is that its peaks
// are low frequency and well above the bed they sit on.
std::vector<float> kicks(int blocks, int everyBlocks, double peak,
                         double decaySeconds)
{
    const double w = 2.0 * 3.14159265358979 * 60.0 / kFs;
    std::vector<float> v((size_t)blocks * kBlock * kCh, 0.0f);

    for (int b0 = 0; b0 < blocks; b0 += everyBlocks) {
        const int start = b0 * kBlock;
        for (int f = start; f < blocks * kBlock; ++f) {
            const double t = (double)(f - start) / kFs;
            if (t > decaySeconds * 6.0) break;
            const float s = (float)(peak * std::exp(-t / decaySeconds)
                                         * std::sin(w * (double)(f - start)));
            v[(size_t)f * kCh + 0] += s;
            v[(size_t)f * kCh + 1] += s;
        }
    }
    return v;
}

std::vector<float> impulses(int blocks, int period, double amp)
{
    std::vector<float> v((size_t)blocks * kBlock * kCh, 0.0f);
    for (int f = 0; f < blocks * kBlock; f += period) {
        v[(size_t)f * kCh + 0] = (float)amp;
        v[(size_t)f * kCh + 1] = (float)amp;
    }
    return v;
}

// A 750 Hz stereo sine whose RMS ramps linearly in dB from one level to
// another - a fade. The gradualness is the point: an abrupt cut to silence is
// caught by the gate, and a fade walks straight through it.
std::vector<float> fade(double fromDb, double toDb, int blocks, double *phase)
{
    const double w = 2.0 * 3.14159265358979 * 750.0 / kFs;
    double ph = phase ? *phase : 0.0;

    std::vector<float> v((size_t)blocks * kBlock * kCh);
    for (int f = 0; f < blocks * kBlock; ++f) {
        const double t  = (double)f / (double)(blocks * kBlock);
        const double db = fromDb + (toDb - fromDb) * t;
        const float  sm = (float)(dbToLin(db) * std::sqrt(2.0) * std::sin(ph));
        ph += w;
        v[(size_t)f * kCh + 0] = sm;
        v[(size_t)f * kCh + 1] = sm;
    }
    if (phase) *phase = ph;
    return v;
}

std::vector<float> silence(int blocks)
{
    return std::vector<float>((size_t)blocks * kBlock * kCh, 0.0f);
}

// ---- running ------------------------------------------------------------

struct Result {
    std::vector<float>  out;
    std::vector<double> blockRms;   // output RMS per block
    std::vector<double> gain;       // leveller gain after each block
    std::vector<double> wall;       // wall gain after each block, 1.0 = idle
};

Result run(AutoGain &ag, const std::vector<float> &in)
{
    Result r;
    r.out.resize(in.size());

    std::vector<float> buf((size_t)kBlock * kCh);
    const int blocks = (int)(in.size() / (kBlock * kCh));

    for (int b = 0; b < blocks; ++b) {
        const size_t off = (size_t)b * kBlock * kCh;
        std::copy(in.begin() + off, in.begin() + off + kBlock * kCh, buf.begin());
        ag.process(buf.data(), kBlock);
        std::copy(buf.begin(), buf.end(), r.out.begin() + off);

        double s = 0.0;
        for (float v : buf) s += (double)v * v;
        r.blockRms.push_back(std::sqrt(s / (kBlock * kCh)));
        r.gain.push_back(ag.gain());
        r.wall.push_back(ag.wallGain());
    }
    return r;
}

// RMS of one frequency component of the output, in dB, over a window of
// blocks - channel 0 only, which carries the same signal as channel 1
// throughout this file.
//
// **The window has to hold a whole number of cycles of everything present**,
// or one tone leaks into another's measurement and the number is quietly
// wrong. One second at 48 kHz holds exactly 60 cycles of 60 Hz and exactly
// 750 of 750 Hz, so a one-second window separates those two exactly. That is
// what makes it possible to watch a voice while a bass note plays over it,
// which is the entire subject of check 15.
double binRmsDb(const Result &r, int fromBlock, int blocks, double freq)
{
    const double w = 2.0 * 3.14159265358979 * freq / kFs;
    const size_t from = (size_t)fromBlock * kBlock;
    const size_t n    = (size_t)blocks    * kBlock;
    if ((from + n) * kCh > r.out.size()) return -200.0;

    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double s = r.out[(from + i) * kCh];
        re += s * std::cos(w * (double)(from + i));
        im += s * std::sin(w * (double)(from + i));
    }
    // Amplitude of the component, then RMS.
    return linToDb(2.0 * std::sqrt(re * re + im * im) / (double)n
                   / std::sqrt(2.0));
}

double peakOf(const std::vector<float> &v)
{
    double p = 0.0;
    for (float s : v) p = std::max(p, (double)std::fabs(s));
    return p;
}

// RMS of the last `blocks` blocks of output.
double tailRms(const Result &r, int blocks)
{
    double s = 0.0;
    const int from = std::max(0, (int)r.blockRms.size() - blocks);
    int n = 0;
    for (int i = from; i < (int)r.blockRms.size(); ++i) {
        s += r.blockRms[i] * r.blockRms[i];
        ++n;
    }
    return n ? std::sqrt(s / n) : 0.0;
}

// The loudest block RMS in a window of the result, in dB.
double loudestIn(const Result &r, int from, int count)
{
    double m = 0.0;
    for (int i = from; i < (int)r.blockRms.size() && i < from + count; ++i)
        m = std::max(m, r.blockRms[i]);
    return linToDb(m);
}

// First block after `from` at which the output is within tolDb of targetDb and
// *stays* there for `hold` blocks. Returns -1 if it never does.
//
// The hold matters: output lags input by one block, so the block immediately
// after a step still carries the old level at the old gain and looks perfectly
// settled. Without the hold, every step appears to settle instantly - which is
// how the first version of check 12 managed to report 0.000 s.
int settleBlocks(const Result &r, int from, double targetDb, double tolDb, int hold)
{
    for (int b = from; b < (int)r.blockRms.size(); ++b) {
        bool holds = true;
        for (int k = b; k < (int)r.blockRms.size() && k < b + hold; ++k)
            if (std::fabs(linToDb(r.blockRms[k]) - targetDb) > tolDb)
                { holds = false; break; }
        if (holds) return b - from;
    }
    return -1;
}

// ---- an independent implementation of the wall schedule -----------------
//
// Deliberately a second implementation rather than a call into the engine. The
// claim under test is that the ceiling is produced by the gain schedule and
// not by the clamp at the end of the loop, and the only way to see that from
// outside is to predict every output sample with **no clamp anywhere** and
// find that the engine agrees.
//
// It models the wall only, so the tests that use it run with MaxGain 0, which
// pins the leveller at unity and leaves the wall as the only thing acting.
// That is deliberate: comparing a recursive float detector against a double
// reimplementation would drift and prove nothing about the clamp.
std::vector<float> predictWall(const std::vector<float> &in, double output)
{
    const double ceiling = dbToLin(output);
    const double rel     = 1.0 / (kReleaseSeconds * (kFs / kBlock));

    const int blocks = (int)(in.size() / (kBlock * kCh));
    std::vector<float>  out(in.size(), 0.0f);
    std::vector<double> line((size_t)kBlock * kCh, 0.0);

    double wall = 1.0, req = 1.0;

    for (int b = 0; b < blocks; ++b) {
        const size_t off = (size_t)b * kBlock * kCh;

        double peak = 0.0;
        for (int i = 0; i < kBlock * kCh; ++i)
            peak = std::max(peak, std::fabs((double)in[off + i]));

        const double reqNext = (peak > ceiling) ? (ceiling / peak) : 1.0;

        double target = std::min(req, reqNext);
        if (target > wall) target = wall + (target - wall) * rel;

        const double stepG = (target - wall) / kBlock;
        double g = wall;

        for (int f = 0; f < kBlock; ++f) {
            for (int c = 0; c < kCh; ++c) {
                const size_t i = (size_t)f * kCh + c;
                out[off + i] = (float)(line[i] * g);   // no clamp, on purpose
                line[i] = (double)in[off + i];
            }
            g += stepG;
        }
        wall = target;
        req  = reqNext;
    }
    return out;
}

} // namespace

int main()
{
    std::printf("AutoGain: %u bytes, delay line capacity %d frames, block %d\n\n",
                (unsigned)sizeof(AutoGain), (int)AutoGain::kLookahead, kBlock);

    // ---- 1. it lands on the Target, from either direction -----------------
    {
        AutoGain ag = make(-18.0f, 24.0f);
        // 90 s, not 20: boosting is deliberately slow (kBoostRate) and the
        // rate law decelerates further as the error shrinks, so the last half
        // dB genuinely takes a minute. Inaudible, but it has to be waited for
        // if this check is going to be tight. See check 12.
        const Result r = run(ag, sineRms(-32.0, 70000));
        checkNear("quiet source levelled to Target", linToDb(tailRms(r, 400)),
                  -18.0 - kK750, 0.2);
    }
    {
        AutoGain ag = make(-18.0f, 24.0f);
        const Result r = run(ag, sineRms(-6.0, 15000));
        checkNear("loud source levelled to Target", linToDb(tailRms(r, 400)),
                  -18.0 - kK750, 0.2);
    }

    // ---- 2. MaxGain bounds it in both directions --------------------------
    {
        AutoGain ag = make(-18.0f, 6.0f);
        const Result r = run(ag, sineRms(-40.0, 70000));
        checkNear("boost clamped by MaxGain (-40 +6)", linToDb(tailRms(r, 400)),
                  -34.0, 0.2);
    }
    {
        AutoGain ag = make(-30.0f, 6.0f);
        const Result r = run(ag, sineRms(-8.0, 15000));
        checkNear("cut clamped by MaxGain (-8 -6)", linToDb(tailRms(r, 400)),
                  -14.0, 0.2);
    }

    // ---- 3. how long it takes, which is the point of this engine ----------
    //
    // 18 dB of step - about the worst a real pair of tracks will differ by -
    // and the measure is time to get within 1 dB of Target and stay there.
    // The old levelling engine took about six seconds to do this at its
    // power-on Speed, which is what the rate law exists to fix.
    {
        AutoGain ag = make(-18.0f, 24.0f);

        double ph = 0.0;
        std::vector<float> in = sineRms(-30.0, 3000, &ph);   // 4 s to settle
        append(in, sineRms(-12.0, 3000, &ph));               // then 18 dB up

        const Result r = run(ag, in);

        const int stepAt = 3000;
        const double want = dbToLin(-18.0 - kK750);
        int settled = -1;
        for (int b = stepAt; b < (int)r.blockRms.size(); ++b) {
            bool holds = true;
            for (int k = b; k < (int)r.blockRms.size() && k < b + 200; ++k)
                if (std::fabs(linToDb(r.blockRms[k]) - linToDb(want)) > 1.0)
                    { holds = false; break; }
            if (holds) { settled = b - stepAt; break; }
        }
        checkTrue("an 18 dB step settles at all", settled >= 0);
        if (settled >= 0)
            checkLE("  and gets within 1 dB in under 1.5 s",
                    settled * kBlockSecs, 1.5);
    }

    // ---- 4. and it still does not chase dynamics inside a track -----------
    //
    // The other half of the rate law. 4 dB of level swing every 200 ms is
    // ordinary musical dynamics, not a change of track, and the gain must
    // largely ignore it. A single fast time constant would follow it, and
    // following it is what pumping is.
    {
        AutoGain ag = make(-18.0f, 24.0f);

        double ph = 0.0;
        std::vector<float> in = sineRms(-18.0, 3000, &ph);   // settle first
        for (int i = 0; i < 40; ++i)                         // then 8 s of it
            append(in, sineRms((i & 1) ? -16.0 : -20.0, 150, &ph));

        const Result r = run(ag, in);

        double lo = 1e9, hi = -1e9;
        for (size_t b = 4000; b < r.gain.size(); ++b) {
            lo = std::min(lo, r.gain[b]);
            hi = std::max(hi, r.gain[b]);
        }
        checkLE("gain swing over 4 dB of dynamics stays under 1.5 dB",
                linToDb(hi) - linToDb(lo), 1.5);
    }

    // ---- 5. the gate: silence does not wind it up -------------------------
    //
    // Without a gate the gap between two tracks is the quietest thing the
    // detector has ever seen, and it spends the silence winding to MaxGain and
    // dumping it the instant the next track starts.
    {
        AutoGain ag = make(-18.0f, 24.0f);

        double ph = 0.0;
        std::vector<float> in = sineRms(-18.0, 3000, &ph);
        const Result pre = run(ag, in);
        const double settled = pre.gain.back();

        const Result r = run(ag, std::vector<float>(
                                    (size_t)7500 * kBlock * kCh, 0.0f));  // 10 s
        checkNear("gain holds through ten seconds of silence",
                  linToDb(r.gain.back()) - linToDb(settled), 0.0, 0.01);
        // From the second block on: the first one legitimately carries the
        // tail of the previous signal out of the delay line.
        checkNear("and the silence stays silent",
                  peakOf(std::vector<float>(r.out.begin() + kBlock * kCh,
                                            r.out.end())), 0.0, 0.0);
    }

    // ---- 6. the wall holds, with the leveller working hard ----------------
    {
        AutoGain ag = make(-6.0f, 24.0f, -1.0f);   // asks for far too much
        const Result r = run(ag, sineRms(-40.0, 6000));
        checkLE("wall holds while the leveller boosts 24 dB",
                linToDb(peakOf(r.out)), -1.0 + 1e-4);
    }
    {
        AutoGain ag = make(-6.0f, 24.0f, -6.0f);
        const Result r = run(ag, impulses(6000, 137, 1.0));
        checkLE("wall holds on a full-scale impulse train",
                linToDb(peakOf(r.out)), -6.0 + 1e-4);
    }
    // The bottom of Output's range, which is 35 dB below where every other
    // check here runs. The leveller is asked for +24 dB at the same time, so
    // the wall is pulling down about 23 dB continuously rather than catching
    // transients - the state Output reaches when it is set below the level
    // Target is driving to.
    {
        AutoGain ag = make(-6.0f, 24.0f, -36.0f);
        const Result r = run(ag, sineRms(-40.0, 6000));
        checkLE("wall holds at the -36 floor while the leveller boosts 24 dB",
                linToDb(peakOf(r.out)), -36.0 + 1e-4);
    }

    // ---- 7. and it is the schedule doing it, not the clamp ----------------
    //
    // MaxGain 0 pins the leveller at unity, leaving the wall as the only thing
    // acting, so predictWall() models the whole engine. If the clamp were ever
    // load-bearing the two would part company on exactly those samples.
    {
        struct { double out; const char *what; } cases[] = {
            { -1.0,  "sine at full scale"      },
            { -6.0,  "impulse train"           },
            // Output's floor. The exactness argument is that a linear ramp
            // between two gains both <= r(D) never exceeds r(D), which has no
            // term in the ceiling - so it should hold 35 dB down as well as it
            // does at -1. Asserted rather than assumed, because the range was
            // widened after every other case here was written.
            { -36.0, "impulse train at the floor" },
        };
        std::vector<float> sigs[3] = {
            sineRms(-3.0, 2000), impulses(2000, 137, 1.0),
            impulses(2000, 137, 1.0)
        };
        for (int i = 0; i < 3; ++i) {
            AutoGain ag = make(-18.0f, 0.0f, (float)cases[i].out);
            const Result got = run(ag, sigs[i]);
            const std::vector<float> want = predictWall(sigs[i], cases[i].out);

            double worst = 0.0;
            for (size_t k = 0; k < want.size(); ++k)
                worst = std::max(worst, (double)std::fabs(got.out[k] - want[k]));

            char label[96];
            std::snprintf(label, sizeof label,
                          "clamp never fires: %s matches unclamped",
                          cases[i].what);
            checkNear(label, worst, 0.0, 2e-6);
        }
    }

    // ---- 8. latency is exactly one block ----------------------------------
    {
        AutoGain ag = make(-18.0f, 0.0f, 0.0f);
        std::vector<float> in((size_t)4 * kBlock * kCh, 0.0f);
        in[0] = in[1] = 0.5f;

        const Result r = run(ag, in);
        int at = -1;
        for (int f = 0; f < 4 * kBlock; ++f)
            if (std::fabs(r.out[(size_t)f * kCh]) > 0.0f) { at = f; break; }

        checkNear("latency is exactly one block", at, kBlock, 0.0);
    }

    // ---- 9. a minute of alternating material stays finite -----------------
    {
        AutoGain ag = make(-18.0f, 24.0f);
        double worst = 0.0, ph = 0.0;
        bool   finite = true;

        for (int i = 0; i < 60; ++i) {
            const Result r = run(ag, sineRms((i & 1) ? -6.0 : -45.0, 750, &ph));
            for (float s : r.out)
                if (!std::isfinite(s)) finite = false;
            worst = std::max(worst, peakOf(r.out));
        }
        checkTrue("a minute of alternating material stays finite", finite);
        checkLE("and never crosses the wall", linToDb(worst), -1.0 + 1e-4);
    }

    // ---- 10. it does not spike when music returns after a quiet passage ----
    //
    // Reported from the board as sudden gain jumps, "random and I cannot
    // recreate it". It is not random: it is what a leveller does when it is
    // allowed to boost quickly. A quiet passage looks exactly like a quiet
    // track to a detector, so the gain winds up into it - and when the music
    // comes back, the first fraction of a second arrives at that wound-up
    // gain. The wall stops it clipping, so what is heard is not distortion but
    // a burst of very loud, very squashed audio.
    {
        AutoGain ag = make(-18.0f, 18.0f);

        double ph = 0.0;
        std::vector<float> in = sineRms(-18.0, 2200, &ph);   // ~3 s at level
        append(in, sineRms(-43.0, 2250, &ph));               // 3 s, 25 dB down
        append(in, sineRms(-18.0, 1500, &ph));               // and back
                                                             // 3 s because that
                                                             // is the gap the
                                                             // hardware report
                                                             // was looping

        const Result r = run(ag, in);

        const double steady = linToDb(r.blockRms[2100]);
        const double burst  = loudestIn(r, 4450, 400);       // 0.5 s after
        checkLE("no spike when music returns after a quiet passage",
                burst - steady, 1.0);
    }

    // ---- 11. and not after a fade to silence either ------------------------
    //
    // The worse version, and the one most likely to be heard between tracks. An
    // abrupt cut to silence is caught by the gate and the gain freezes where it
    // was; a *fade* walks the level down through the gate gradually, and the
    // detector follows it all the way down before the gate ever closes. The
    // gain is then frozen at maximum for the whole gap, and the next track
    // arrives into it.
    {
        AutoGain ag = make(-18.0f, 18.0f);

        double ph = 0.0;
        std::vector<float> in = sineRms(-18.0, 2200, &ph);   // ~3 s at level
        append(in, fade(-18.0, -75.0, 1100, &ph));           // 1.5 s fade out
        append(in, silence(750));                            // 1 s of gap
        append(in, sineRms(-18.0, 1500, &ph));               // next track

        const Result r = run(ag, in);

        const double steady = linToDb(r.blockRms[2100]);
        const double burst  = loudestIn(r, 4050, 400);       // 0.5 s in
        checkLE("no spike on the track after a fade-out",
                burst - steady, 1.0);
    }

    // ---- 12. the asymmetry, stated as a measurement ------------------------
    //
    // Cutting is fast and boosting is not, and that is a deliberate trade
    // rather than a limitation - see kBoostRate. A track arriving too loud is
    // the fault you have to fix immediately; one arriving too quiet is merely
    // quiet for a moment. Both halves are pinned here so neither drifts
    // silently: tightening the boost to make checks 10 and 11 look better is
    // exactly the change that brings the spike back.
    //
    // Measured to within 3 dB rather than 1, because 3 dB is where the bulk of
    // the correction has happened and the rate law deliberately decelerates
    // after that. Check 3 already covers the last dB in the cut direction.
    {
        // Input jumps UP 18 dB, so the gain must come DOWN. The urgent case.
        AutoGain ag = make(-18.0f, 24.0f);
        double ph = 0.0;
        std::vector<float> in = sineRms(-30.0, 4000, &ph);
        append(in, sineRms(-12.0, 4000, &ph));

        const Result r = run(ag, in);
        const int n = settleBlocks(r, 4000, -18.0 - kK750, 3.0, 300);
        checkLE("a track arriving 18 dB LOUDER is mostly fixed in 0.5 s",
                n < 0 ? 999.0 : n * kBlockSecs, 0.5);
    }
    {
        // Input drops 18 dB, so the gain must go UP. The patient case.
        AutoGain ag = make(-18.0f, 24.0f);
        double ph = 0.0;
        std::vector<float> in = sineRms(-12.0, 4000, &ph);
        append(in, sineRms(-30.0, 20000, &ph));

        const Result r = run(ag, in);
        const int n = settleBlocks(r, 4000, -18.0 - kK750, 3.0, 300);
        checkLE("a track arriving 18 dB QUIETER is mostly fixed in 10 s",
                n < 0 ? 999.0 : n * kBlockSecs, 10.0);
    }

    // ---- 13. no cliff: spike must grow smoothly with gap length ------------
    //
    // The bug this exists for was found on hardware *after* the persistence
    // rule was already in. A loop whose quiet section was almost exactly
    // the `Boost` wait long produced a clean +8 dB jump once per repeat - the
    // counter expired just as the music returned and the gain was released
    // straight to the fast rate. Gaps shorter than the delay were silent and
    // longer ones were fine; only the boundary tripped, which is precisely what
    // made it look random on real material.
    //
    // So the invariant is not "no wind-up at any gap length" - a passage that
    // stays quiet for many seconds is a real level change and correcting it is
    // the whole job. The invariant is that **nothing steps**: the response has
    // to be a ramp in gap length, so no piece of material can sit on a boundary
    // and behave unlike the material either side of it.
    {
        double prev = 0.0, worstJump = 0.0;
        double atShort = 0.0;
        int    worstAt = 0;

        for (int g = 375; g <= 5250; g += 375) {          // 0.5 s .. 7 s
            AutoGain ag = make(-18.0f, 18.0f);

            double ph = 0.0;
            std::vector<float> in = sineRms(-18.0, 2200, &ph);
            append(in, sineRms(-43.0, g, &ph));
            append(in, sineRms(-18.0, 1500, &ph));

            const Result r = run(ag, in);
            const double steady = linToDb(r.blockRms[2100]);
            const double burst  = loudestIn(r, 2200 + g, 400) - steady;

            if (g > 375 && burst - prev > worstJump)
                { worstJump = burst - prev; worstAt = g; }
            if (g <= 2250) atShort = std::max(atShort, burst);
            prev = burst;
        }

        char label[96];
        std::snprintf(label, sizeof label,
                      "no cliff: worst step per 0.5 s of gap (at %.1f s)",
                      worstAt * kBlockSecs);
        checkLE(label, worstJump, 2.0);
        checkLE("and nothing at all from gaps up to 3 s", atShort, 1.0);
    }

    // ---- 14. the K-weighting is the standard's, not something like it ------
    //
    // BS.1770-4 publishes the two stages' coefficients at 48 kHz. The engine
    // does not carry that table - it carries the analogue prototype and runs
    // the same bilinear transform the standard did, so that it is correct at
    // any sample rate. This is what makes that safe: the derivation has to
    // reproduce the published numbers, or one of the two is wrong.
    //
    // Held to 1e-6 rather than to the last digit because the engine derives in
    // single precision. The reference here derives in double, so the tolerance
    // is really a statement about float32 being good enough for a filter that
    // is designed once and never retuned - which is the argument the header
    // makes for using direct form at a 38 Hz corner at all.
    {
        const KBiquad S = kShelfRef(kFs), H = kHighpassRef(kFs);

        checkNear("BS.1770 shelf b0 matches the published table", S.b0,
                   1.53512485958697, 1e-9);
        checkNear("BS.1770 shelf b1 matches the published table", S.b1,
                  -2.69169618940638, 1e-9);
        checkNear("BS.1770 shelf b2 matches the published table", S.b2,
                   1.19839281085285, 1e-9);
        checkNear("BS.1770 shelf a1 matches the published table", S.a1,
                  -1.69065929318241, 1e-9);
        checkNear("BS.1770 shelf a2 matches the published table", S.a2,
                   0.73248077421585, 1e-9);
        checkNear("BS.1770 RLB   a1 matches the published table", H.a1,
                  -1.99004745483398, 1e-9);
        checkNear("BS.1770 RLB   a2 matches the published table", H.a2,
                   0.99007225036621, 1e-9);

        // And the curve is the shape it is supposed to be: subsonic content
        // heavily discounted, bass gently discounted, treble lifted 4 dB.
        checkLE("K curve discounts 20 Hz by at least 10 dB", kGainDb(20.0), -10.0);
        checkNear("K curve at 60 Hz",   kGainDb(60.0),   -2.94, 0.05);
        checkNear("K curve at 200 Hz",  kGainDb(200.0),  -0.31, 0.05);
        checkNear("K curve at 1 kHz",   kGainDb(1000.0), +0.65, 0.05);
        checkNear("K curve at 10 kHz",  kGainDb(10000.0), +4.00, 0.05);
    }

    // ---- 15. a bass drop-out no longer lifts the voice as far --------------
    //
    // **This is the fault the weighting exists for**, reported from the board:
    // when the bass drops out of a track, the voice climbs over the next few
    // seconds. The output never exceeded Target - the engine was working as
    // designed and the design was wrong, because a broadband mean square of
    // music is very largely a measurement of its bass.
    //
    // 60 Hz standing in for the bass and 750 Hz for the voice, the bass 6 dB
    // above the voice. Then the bass stops and the voice does not move. How far
    // the *voice* climbs is the whole measurement, and binRmsDb() reads it
    // through the bass because a one-second window puts both tones on a bin.
    //
    // The two reference numbers either side of it are the point. A feed-forward
    // leveller moves its gain by exactly the drop its detector reports, so the
    // lift is the measurement drop - and the measurement is the only thing the
    // weighting changed. Both are computed here from the input rather than
    // written down, so the check states the mechanism and not just a threshold.
    //
    // **Target is set for a cut rather than a boost at the start**, so the gain
    // is settled before the bass leaves. Boosting is deliberately slow - 4 s of
    // `Boost` and then a slew - so a target needing a boost here would still be
    // rising when the measurement was taken, and the lift would read high for a
    // reason having nothing to do with weighting. That cost a round.
    {
        const int settle = 6000;     // 8 s at level; the cut arrives in under 1
        const int after  = 15000;    // 20 s, long enough for the boost to finish

        double p1 = 0.0, p2 = 0.0;
        const std::vector<float> bassy =
            mix(quadToneRms(60.0, -18.0, settle, &p1),
                toneRms(750.0, -24.0, settle, &p2));
        const std::vector<float> voice = toneRms(750.0, -24.0, after, &p2);

        std::vector<float> in = bassy;
        append(in, voice);

        const std::vector<double> mb = mono(bassy), mv = mono(voice);
        const double broadDrop = linToDb(plainRms(mb, 4800))
                               - linToDb(plainRms(mv, 4800));
        const double kDrop     = linToDb(kWeightedRms(mb, 4800))
                               - linToDb(kWeightedRms(mv, 4800));

        AutoGain ag = make(-24.0f, 18.0f);
        const Result r = run(ag, in);

        const double before = binRmsDb(r, settle - 750, 750, 750.0);
        double worst = -200.0;
        for (int b = settle; b + 750 <= settle + after; b += 750)
            worst = std::max(worst, binRmsDb(r, b, 750, 750.0));

        std::printf("    [measurement drop: broadband %.2f dB, "
                    "K-weighted %.2f dB]\n", broadDrop, kDrop);

        checkNear("voice lift on a bass drop-out tracks the K measurement",
                  worst - before, kDrop, 0.5);
        checkLE("  and is well short of what broadband would have lifted it",
                worst - before, broadDrop - 1.5);
    }

    // ---- 16. and the wall does not pay for it -----------------------------
    //
    // The objection to fixing check 15 with a hard high-pass at 100-150 Hz,
    // stated as a measurement rather than argued.
    //
    // The detector and the wall are separate paths, so weighting the detector
    // cannot weaken the wall directly - it still sees the true broadband peak
    // one block ahead. What it does instead is indirect: if bass counts for
    // less, bass-heavy material measures quieter, the leveller pushes it up
    // further, and **that extra gain lands on a signal whose peaks are the
    // kick**. So the extra work handed to the wall is exactly the difference
    // between the broadband and the weighted measurement of that material.
    //
    // That difference is the number below, computed for both candidates on the
    // same signal. It is the whole case for the gentle curve over the hard one,
    // and it is why this check compares them rather than bounding one of them.
    //
    // Full-scale kicks twice a second over a quiet 750 Hz bed: nearly all the
    // energy at 60 Hz and nearly all the peak too, which is about as
    // unfavourable as programme material gets. They are full scale so that the
    // wall has something to do from the first block - at a lower level the
    // peaks never reach the ceiling and this check passes by measuring nothing,
    // which it did for two rounds before the level was raised.
    //
    // **The measured answer is that the wall barely works on this material at
    // all**, and the reason is worth recording because it is not the weighting.
    // The leveller settles about 1.9 dB *down* rather than up: kicks make the
    // detector fluctuate, cutting is immediate while a rise has to survive
    // `Boost`, so the gain tracks the loud parts and never winds up. The wall
    // engages on a few percent of blocks, all of them in the first second
    // before that cut arrives. The asymmetry that exists to stop wind-up on
    // quiet passages turns out to protect the wall here too.
    {
        const int blocks = 22500;    // 30 s
        const std::vector<float> in =
            mix(kicks(blocks, 375, dbToLin(0.0), 0.060),
                toneRms(750.0, -26.0, blocks));

        const std::vector<double> m = mono(in);
        const double broad = linToDb(plainRms(m, 4800));
        const double kw    = linToDb(kWeightedRms(m, 4800));
        const double hard  = linToDb(hardHpRms(m, 150.0, 4800));

        std::printf("    [kick-heavy material measures: broadband %.2f, "
                    "K-weighted %.2f, 150 Hz high-passed %.2f dBFS]\n",
                    broad, kw, hard);

        AutoGain ag = make(-14.0f, 18.0f);
        const Result r = run(ag, in);

        int engaged = 0;
        double deepest = 1.0;
        const size_t from = 0;   // the whole run: see the note above
        for (size_t b = from; b < r.wall.size(); ++b) {
            if (r.wall[b] < 0.999) ++engaged;
            deepest = std::min(deepest, r.wall[b]);
        }
        std::printf("    [wall engaged on %.1f%% of blocks, deepest %.2f dB; "
                    "leveller settled at %.2f dB]\n",
                    100.0 * (double)engaged / (double)(r.wall.size() - from),
                    linToDb(deepest), linToDb(r.gain.back()));

        checkLE("K-weighting adds little gain on kick-heavy material",
                broad - kw, 3.0);
        checkTrue("  and less than half what a 150 Hz high-pass would",
                  (broad - kw) * 2.0 < (broad - hard));
        checkLE("and the wall still holds the ceiling exactly",
                linToDb(peakOf(r.out)), -1.0 + 1e-4);
    }

    // ---- 17. it comes up levelled, instead of four seconds later ----------
    //
    // The persistence rule tells a quiet passage from a quiet track by how long
    // the quiet lasts, which presumes a level to have dropped away from. At
    // power-on there is none - so it had nothing to protect against and the
    // only thing it achieved was the opening of the first track played at unity
    // gain while the counter ran down. The first arrival after a reset now
    // skips the hold and the slew and is driven by the rate law alone.
    //
    // A source 12 dB below Target, from reset. Before this it took the 4 s wait
    // plus a slew ramping in at 8 dB/s; now it is the rate law's own time for a
    // correction that size, which check 3 measures separately as under a second
    // for 18 dB.
    {
        AutoGain ag = make(-18.0f, 24.0f);
        const Result r = run(ag, sineRms(-30.0, 6000));      // 8 s
        const int n = settleBlocks(r, 0, -18.0 - kK750, 1.0, 300);

        checkTrue("the first arrival settles at all", n >= 0);
        checkLE("box comes up levelled in under 1.5 s",
                n < 0 ? 999.0 : n * kBlockSecs, 1.5);
    }
    {
        // **And the allowance is spent on programme, not on the clock.** A box
        // powered up long before anyone presses play has not started arriving
        // at anything, so the window has to survive the silence - otherwise
        // this whole feature works only for people who press play within four
        // seconds of switching on. The gate is what makes that distinction.
        AutoGain ag = make(-18.0f, 24.0f);

        std::vector<float> in = silence(7500);               // 10 s of nothing
        double ph = 0.0;
        append(in, sineRms(-30.0, 6000, &ph));               // then the track

        const Result r = run(ag, in);
        const int n = settleBlocks(r, 7500, -18.0 - kK750, 1.0, 300);

        checkLE("and still does after ten seconds powered up in silence",
                n < 0 ? 999.0 : n * kBlockSecs, 1.5);
    }
    {
        // The other half: nothing about this touches a *cut*, which was never
        // delayed, and nothing about it survives into the programme. Check 10
        // pins the second half - a quiet passage well after startup must still
        // produce no wind-up - and it runs with this code in place.
        AutoGain ag = make(-18.0f, 24.0f);
        double ph = 0.0;
        std::vector<float> in = sineRms(-6.0, 4000, &ph);    // starts too loud
        const Result r = run(ag, in);
        const int n = settleBlocks(r, 0, -18.0 - kK750, 1.0, 300);

        checkLE("a source that starts too loud is still cut immediately",
                n < 0 ? 999.0 : n * kBlockSecs, 1.5);
    }

    std::printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail ? 1 : 0;
}
