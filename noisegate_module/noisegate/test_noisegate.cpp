// test_noisegate.cpp - host verification for NoiseGate.
//
// **Deliberately not in CMakeLists.txt**, the same way agc/test_autogain.cpp
// is not: it is a host program, it has no place in the firmware image, and
// this is not a git repo so deleting it would mean gone.
//
// NoiseGate needs no stubbing to build on a host - it includes only math.h,
// stdint.h and string.h, and its one piece of ARM assembly is behind an
// __ARM_FP guard a host compiler does not satisfy. So:
//
//     call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
//     cl /nologo /EHsc /O2 /std:c++17 /Inoisegate_module\noisegate noisegate_module\noisegate\NoiseGate.cpp noisegate_module\noisegate\test_noisegate.cpp /Fe:tn.exe
//     tn.exe
//
// What it is checking is the things that cannot be checked by listening:
//
//   - that it actually opens on loud material and closes on quiet material,
//   - that Hold does what it says - a dip shorter than it does not close the
//     gate, one longer than it does,
//   - that Attack and Release are the ramp times they claim to be, measured
//     directly rather than inferred,
//   - that the ramp is a bounded slew and so cannot click, at any block size,
//   - and that a stereo signal is gated as one signal, not two.
//
// Re-run it after touching the detector, the state machine or the slew rates.
//
// ---- harness notes ---------------------------------------------------------
//
//   process() is in place, so a reused buffer is not silence. Re-zero it every
//   block, or read it back before it is overwritten - runFine() below reads it
//   back immediately, so this does not bite there.
//
//   The block-wise checks use a 750 Hz stereo sine, the same tone
//   agc/test_autogain.cpp standardises on: 750 Hz is exactly one cycle per 64
//   frames at 48 kHz, so every block presents the same mean square and the
//   detector is not wobbling for reasons that have nothing to do with what is
//   under test.
//
//   The timing checks (Attack, Release) need sample resolution, not block
//   resolution, so they call process() one frame at a time - runFine() below.
//   NoiseGate re-derives its block-rate-dependent coefficients (Hold, the
//   detector's own smoothing) whenever the caller's block size changes, so
//   frames == 1 is a legitimate way to run it, not a hack that happens to
//   compile: Hold then counts in actual samples instead of in blocks of 64,
//   which is exactly the resolution these checks want. They use a constant
//   (DC) level rather than a tone for the same reason AutoGain's own tests
//   pick block-synchronous frequencies: a non-varying input gives the
//   detector one number to converge on instead of one that is also moving,
//   which is what makes a rise or fall time measurable to a fraction of a
//   millisecond instead of averaged over a cycle.

#include "NoiseGate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_fail = 0;

void checkNear(const char *what, double got, double want, double tol)
{
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("%-64s got %10.5f  want %10.5f  %s\n",
                what, got, want, ok ? "ok" : "** FAIL **");
    if (!ok) ++g_fail;
}

void checkLE(const char *what, double got, double limit)
{
    const bool ok = (got <= limit);
    std::printf("%-64s got %10.5f  <=  %10.5f  %s\n",
                what, got, limit, ok ? "ok" : "** FAIL **");
    if (!ok) ++g_fail;
}

void checkTrue(const char *what, bool ok)
{
    std::printf("%-64s %s\n", what, ok ? "ok" : "** FAIL **");
    if (!ok) ++g_fail;
}

const float kFs    = 48000.0f;
const int   kBlock = 64;
const int   kCh    = 2;

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
double linToDb(double a)  { return (a > 0.0) ? 20.0 * std::log10(a) : -200.0; }

// The gate's fixed floor, -96 dBFS - see NoiseGate.h. Computed here rather
// than copied from the .cpp, the same way AutoGain's test recomputes its own
// reference numbers instead of trusting the engine's.
const double kFloorGain = std::pow(10.0, -96.0 / 20.0);

NoiseGate make(float thresholdDb, float attackMs, float holdMs, float releaseMs)
{
    NoiseGate ng;
    ng.setup(kFs, kCh);
    ng.setParam(NoiseGate::kThresholdDb, thresholdDb);
    ng.setParam(NoiseGate::kAttackMs,    attackMs);
    ng.setParam(NoiseGate::kHoldMs,      holdMs);
    ng.setParam(NoiseGate::kReleaseMs,   releaseMs);
    ng.reset();
    return ng;
}

// ---- signals --------------------------------------------------------------

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

// The same, with each channel at its own level - for the stereo-linking
// check, where what matters is that one shared gain is applied to both.
std::vector<float> sineRmsStereo(double dbL, double dbR, int blocks)
{
    const double ampL = dbToLin(dbL) * std::sqrt(2.0);
    const double ampR = dbToLin(dbR) * std::sqrt(2.0);
    const double w    = 2.0 * 3.14159265358979 * 750.0 / kFs;

    std::vector<float> v((size_t)blocks * kBlock * kCh);
    double ph = 0.0;
    for (int f = 0; f < blocks * kBlock; ++f) {
        v[(size_t)f * kCh + 0] = (float)(ampL * std::sin(ph));
        v[(size_t)f * kCh + 1] = (float)(ampR * std::sin(ph));
        ph += w;
    }
    return v;
}

std::vector<float> silence(int blocks)
{
    return std::vector<float>((size_t)blocks * kBlock * kCh, 0.0f);
}

// A constant (DC) level, both channels alike. Used only by the sample-rate
// timing checks - see the harness note at the top of the file for why a
// non-varying input is what makes a ramp time measurable precisely.
std::vector<float> constLevel(double db, int frames)
{
    const float a = (float)dbToLin(db);
    std::vector<float> v((size_t)frames * kCh);
    for (int f = 0; f < frames; ++f) {
        v[(size_t)f * kCh + 0] = a;
        v[(size_t)f * kCh + 1] = a;
    }
    return v;
}

void append(std::vector<float> &a, const std::vector<float> &b)
{
    a.insert(a.end(), b.begin(), b.end());
}

// ---- running, at the block size the firmware actually uses ----------------

struct Result {
    std::vector<float>  out;
    std::vector<double> blockRms;   // output RMS per block
    std::vector<double> gain;       // gate gain after each block
};

Result run(NoiseGate &ng, const std::vector<float> &in)
{
    Result r;
    r.out.resize(in.size());

    std::vector<float> buf((size_t)kBlock * kCh);
    const int blocks = (int)(in.size() / (kBlock * kCh));

    for (int b = 0; b < blocks; ++b) {
        const size_t off = (size_t)b * kBlock * kCh;
        std::copy(in.begin() + off, in.begin() + off + kBlock * kCh, buf.begin());
        ng.process(buf.data(), kBlock);
        std::copy(buf.begin(), buf.end(), r.out.begin() + off);

        double s = 0.0;
        for (float v : buf) s += (double)v * v;
        r.blockRms.push_back(std::sqrt(s / (kBlock * kCh)));
        r.gain.push_back(ng.gain());
    }
    return r;
}

// ---- running one frame at a time, for the timing checks --------------------
//
// See the harness note at the top of the file: this is what gives Hold,
// Attack and Release their real, per-sample resolution instead of the block's
// 1.33 ms.
struct FineResult {
    std::vector<float>  out;
    std::vector<double> gain;   // one entry per sample
};

FineResult runFine(NoiseGate &ng, const std::vector<float> &in)
{
    FineResult r;
    const int frames = (int)(in.size() / kCh);
    r.out.resize(in.size());
    r.gain.reserve(frames);

    float buf[kCh];
    for (int f = 0; f < frames; ++f) {
        buf[0] = in[(size_t)f * kCh + 0];
        buf[1] = in[(size_t)f * kCh + 1];
        ng.process(buf, 1);
        r.out[(size_t)f * kCh + 0] = buf[0];
        r.out[(size_t)f * kCh + 1] = buf[1];
        r.gain.push_back(ng.gain());
    }
    return r;
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

} // namespace

int main()
{
    std::printf("NoiseGate: %u bytes, block %d\n\n",
                (unsigned)sizeof(NoiseGate), kBlock);

    // ---- 1. a loud signal opens the gate fully -----------------------------
    {
        NoiseGate ng = make(-40.0f, 2.0f, 50.0f, 150.0f);
        const Result r = run(ng, sineRms(-10.0, 2000));
        checkNear("loud signal settles the gate fully open",
                  r.gain.back(), 1.0, 1e-4);
    }

    // ---- 2. from open, silence closes it to the floor ----------------------
    {
        NoiseGate ng = make(-40.0f, 2.0f, 50.0f, 150.0f);
        const Result open = run(ng, sineRms(-10.0, 500));
        checkNear("  (opened first)", open.gain.back(), 1.0, 1e-4);

        // Long enough to clear Hold (50 ms) and Release (150 ms) many times
        // over, plus the detector's own release smoothing.
        const Result r = run(ng, silence(3000));
        checkNear("silence closes the gate to the floor",
                  r.gain.back(), kFloorGain, kFloorGain * 0.1);
        checkNear("  and the output actually reaches silence",
                  peakOf(r.out), 0.0, 1e-6);
    }

    // ---- 3. a dip shorter than Hold does not close the gate ----------------
    //
    // The whole point of Hold: the gap between two notes must not be read as
    // the gap between two phrases. 15 ms is comfortably inside a 50 ms Hold
    // even after the detector's own ~20 ms release smoothing is spent
    // noticing the dip at all.
    {
        NoiseGate ng = make(-40.0f, 2.0f, 50.0f, 150.0f);
        double ph = 0.0;
        std::vector<float> in = sineRms(-10.0, 1000, &ph);   // settle open
        append(in, silence(11));                             // ~15 ms dip
        append(in, sineRms(-10.0, 200, &ph));                 // resumes

        const Result r = run(ng, in);
        double worst = 1.0;
        for (size_t b = 1000; b < 1013 && b < r.gain.size(); ++b)
            worst = std::min(worst, r.gain[b]);

        checkNear("a dip shorter than Hold leaves the gate open",
                  worst, 1.0, 1e-3);
    }

    // ---- 4. a dip longer than Hold does close it, and it reopens -----------
    {
        NoiseGate ng = make(-40.0f, 2.0f, 50.0f, 150.0f);
        double ph = 0.0;
        std::vector<float> in = sineRms(-10.0, 1000, &ph);   // settle open
        append(in, silence(600));                             // 800 ms, well
                                                               // past Hold+Release
        append(in, sineRms(-10.0, 500, &ph));                 // resumes

        const Result r = run(ng, in);

        double closedTo = 1.0;
        for (int b = 1000; b < 1600; ++b) closedTo = std::min(closedTo, r.gain[b]);
        checkNear("a dip longer than Hold closes the gate",
                  closedTo, kFloorGain, kFloorGain * 0.1);
        checkNear("and it reopens once the signal resumes",
                  r.gain.back(), 1.0, 1e-4);
    }

    // ---- 5. Attack is the rise time it claims to be -------------------------
    //
    // Sample-accurate: see runFine() and the harness note at the top of the
    // file. A constant level well above threshold makes "above" true on the
    // very first sample of the loud segment, so the measured rise starts at
    // the boundary and its length is Attack alone - no detector lag folded
    // into the number under test.
    {
        NoiseGate ng = make(-40.0f, 5.0f, 50.0f, 150.0f);
        std::vector<float> in = constLevel(-80.0, 2400);      // 50 ms, closed
        append(in, constLevel(-6.0, 2400));                    // then loud

        const FineResult r = runFine(ng, in);

        const size_t triggerAt = 2400;
        size_t start = 0, reach99 = 0;
        bool foundStart = false, found99 = false;
        for (size_t i = triggerAt; i < r.gain.size(); ++i) {
            if (!foundStart && r.gain[i] > r.gain[triggerAt - 1] + 1e-6) {
                start = i; foundStart = true;
            }
            if (foundStart && !found99 && r.gain[i] >= 0.99) {
                reach99 = i; found99 = true; break;
            }
        }
        checkTrue("attack: the gate starts opening at the loud boundary",
                  foundStart && start == triggerAt);
        checkTrue("attack: the gate reaches fully open", found99);
        if (found99) {
            const double ms = (double)(reach99 - start) / kFs * 1000.0;
            checkNear("attack: 0%..99% rise time matches the Attack param",
                      ms, 5.0 * 0.99, 0.3);
        }
    }

    // ---- 6. Release is the fall time it claims to be ------------------------
    {
        NoiseGate ng = make(-40.0f, 5.0f, 50.0f, 100.0f);
        std::vector<float> in = constLevel(-6.0, 4800);        // 100 ms, open
        append(in, constLevel(-80.0, 15000));                   // then quiet,
                                                                // well past
                                                                // Hold + Release
        const FineResult r = runFine(ng, in);

        const size_t triggerAt = 4800;
        size_t start = 0, reachFloor = 0;
        bool foundStart = false, foundFloor = false;
        for (size_t i = triggerAt; i < r.gain.size(); ++i) {
            if (!foundStart && r.gain[i] < 1.0 - 1e-6) {
                start = i; foundStart = true;
            }
            if (foundStart && !foundFloor && r.gain[i] <= kFloorGain * 1.001) {
                reachFloor = i; foundFloor = true; break;
            }
        }
        checkTrue("release: the gain starts falling after Hold expires",
                  foundStart);
        checkTrue("release: the gate reaches the floor", foundFloor);
        if (foundFloor) {
            const double ms = (double)(reachFloor - start) / kFs * 1000.0;
            checkNear("release: fall time matches the Release param",
                      ms, 100.0, 0.5);
        }
        if (foundStart) {
            const double delayMs = (double)(start - triggerAt) / kFs * 1000.0;
            // Not pinned to Hold alone: the detector's own smoothed envelope
            // has to decay from the loud level down through Threshold before
            // "below" is even true, which for a large drop costs several
            // times its own 20 ms time constant - here, ln(0.2512/1e-4) of
            // them, about 156 ms, on top of the 50 ms Hold itself. What is
            // actually being checked is the one-directional claim: the delay
            // can only be Hold or more, never less, because Hold cannot start
            // counting down before the detector agrees the level has dropped.
            checkTrue("  and the fall never starts before Hold could allow it",
                      delayMs >= 50.0 - 0.1);
        }
    }

    // ---- 7. the ramp is a bounded slew - nothing here can click ------------
    //
    // Run something with a great deal to react to - on, off, on, at a block
    // size the firmware never uses - and check that no sample-to-sample
    // change in gain ever exceeds the configured rate by more than rounding.
    // A step here would be an audible click; this is what rules it out by
    // construction rather than by ear.
    {
        NoiseGate ng = make(-30.0f, 3.0f, 20.0f, 80.0f);
        double ph = 0.0;
        std::vector<float> in = sineRms(-6.0, 300, &ph);
        append(in, silence(300));
        append(in, sineRms(-6.0, 300, &ph));
        append(in, silence(300));

        const FineResult r = runFine(ng, in);

        const double attackRate  = 1.0 / (3.0 * 0.001 * kFs);
        const double releaseRate = 1.0 / (80.0 * 0.001 * kFs);
        // The engine computes and stores this rate in float32, so the
        // reference double above agrees with it to single-precision rounding
        // only - not to 1e-9. The margin still catches anything that matters:
        // a real step (a clamp firing early, say) is orders of magnitude
        // bigger than a rounding error at the sixth decimal place.
        const double limit = std::max(attackRate, releaseRate) * 1.001;

        double worst = 0.0;
        for (size_t i = 1; i < r.gain.size(); ++i)
            worst = std::max(worst, std::fabs(r.gain[i] - r.gain[i - 1]));

        checkLE("no sample-to-sample gain step exceeds the slew rate",
                worst, limit);
    }

    // ---- 8. parameters clamp to the published range -------------------------
    {
        NoiseGate ng;
        ng.setup(kFs, kCh);

        ng.setParam(NoiseGate::kThresholdDb, -999.0f);
        checkNear("Threshold clamps at its floor",
                  ng.getParam(NoiseGate::kThresholdDb), -80.0, 1e-6);
        ng.setParam(NoiseGate::kThresholdDb, 999.0f);
        checkNear("Threshold clamps at its ceiling",
                  ng.getParam(NoiseGate::kThresholdDb), 0.0, 1e-6);

        ng.setParam(NoiseGate::kAttackMs, 0.0f);
        checkNear("Attack clamps at its floor",
                  ng.getParam(NoiseGate::kAttackMs), 0.1, 1e-6);
        ng.setParam(NoiseGate::kHoldMs, -5.0f);
        checkNear("Hold clamps at its floor",
                  ng.getParam(NoiseGate::kHoldMs), 0.0, 1e-6);
        ng.setParam(NoiseGate::kReleaseMs, 99999.0f);
        checkNear("Release clamps at its ceiling",
                  ng.getParam(NoiseGate::kReleaseMs), 2000.0, 1e-6);
    }

    // ---- 9. a minute of alternating material stays finite --------------------
    {
        NoiseGate ng = make(-30.0f, 2.0f, 50.0f, 150.0f);
        double worst = 0.0, ph = 0.0;
        bool   finite = true;

        for (int i = 0; i < 60; ++i) {
            const Result r = run(ng, sineRms((i & 1) ? -6.0 : -60.0, 750, &ph));
            for (float s : r.out)
                if (!std::isfinite(s)) finite = false;
            worst = std::max(worst, peakOf(r.out));
        }
        checkTrue("a minute of alternating material stays finite", finite);
        // sineRms() sets *RMS* level, so the loudest segment's actual peak is
        // 3 dB above its -6 dBFS RMS - the sqrt(2) crest factor of a sine.
        checkLE("and never exceeds the input peak (no gain over unity)",
                worst, dbToLin(-6.0) * std::sqrt(2.0) + 1e-6);
    }

    // ---- 10. it actually gates, at more than one threshold --------------------
    //
    // The basic claim, checked directly rather than assumed from the pieces
    // above: material below Threshold ends up at the floor, material above it
    // passes at unity, and that holds whether the line is drawn at -50 or
    // -25.
    {
        struct { double thresh, below, above; } cases[] = {
            { -50.0, -60.0, -40.0 },
            { -25.0, -35.0, -15.0 },
        };
        for (auto &c : cases) {
            NoiseGate ng = make((float)c.thresh, 2.0f, 50.0f, 150.0f);
            const Result rb = run(ng, sineRms(c.below, 3000));
            char label[96];
            std::snprintf(label, sizeof label,
                          "at Threshold %.0f: %.0f dBFS closes the gate",
                          c.thresh, c.below);
            checkNear(label, rb.gain.back(), kFloorGain, kFloorGain * 0.1);

            NoiseGate ng2 = make((float)c.thresh, 2.0f, 50.0f, 150.0f);
            const Result ra = run(ng2, sineRms(c.above, 3000));
            std::snprintf(label, sizeof label,
                          "at Threshold %.0f: %.0f dBFS opens the gate",
                          c.thresh, c.above);
            checkNear(label, ra.gain.back(), 1.0, 1e-4);
        }
    }

    // ---- 11. a stereo signal is gated as one signal, not two ------------------
    //
    // The detector sums both channels, and one shared gain is applied to
    // both - so a channel imbalance in the input must survive unchanged in
    // the output rather than being narrowed or widened by independent gates.
    {
        NoiseGate ng = make(-40.0f, 2.0f, 50.0f, 150.0f);
        const Result r = run(ng, [] {
            return sineRmsStereo(-10.0, -20.0, 2000);
        }());

        double sl = 0.0, sr = 0.0;
        const size_t from = (r.out.size() / kCh - 500) * kCh;
        for (size_t f = from / kCh; f < r.out.size() / kCh; ++f) {
            sl += (double)r.out[f * kCh + 0] * r.out[f * kCh + 0];
            sr += (double)r.out[f * kCh + 1] * r.out[f * kCh + 1];
        }
        const double gotDiffDb = linToDb(std::sqrt(sl)) - linToDb(std::sqrt(sr));
        checkNear("channel balance survives a shared gain (10 dB in, 10 dB out)",
                  gotDiffDb, 10.0, 0.05);
    }

    // ---- 12. and it never boosts ------------------------------------------
    //
    // A gate only ever holds the signal at unity or pulls it down; there is
    // no path in the state machine that can push gain above 1.0.
    {
        NoiseGate ng = make(-60.0f, 0.1f, 0.0f, 5.0f);   // as snappy as it gets
        const Result r = run(ng, sineRms(0.0, 500));
        checkLE("gain never exceeds unity, even at the fastest settings",
                *std::max_element(r.gain.begin(), r.gain.end()), 1.0 + 1e-6);
    }

    std::printf("\n%s\n", g_fail ? "FAILURES" : "all checks passed");
    return g_fail ? 1 : 0;
}
