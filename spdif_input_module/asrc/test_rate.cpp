// test_rate.cpp - host test for the rate estimator, and the closed loop it
// forms with the ASRC.
//
// **Not in the build**, same as test_asrc.cpp and the other three host suites.
//
//   "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
//   cl /nologo /O2 /EHsc /W4 /std:c++17 test_rate.cpp RateEstimator.cpp Asrc.cpp
//   test_rate.exe
//
// or:
//
//   g++ -O2 -Wall -Wextra -o test_rate test_rate.cpp RateEstimator.cpp Asrc.cpp
//
// ---- how this is structured, and why ------------------------------------
//
// Most of the suite drives the estimator against an **analytic model of the
// consumer**: level += arrived - outFrames*ratio. That is not a simplification
// of what the ASRC does, it is exactly what it does - it consumes `step` input
// frames per output frame - so the model is faithful and runs hundreds of
// times faster, which is what makes ten-minute and one-hour simulations
// practical.
//
// The last group then closes the loop around the **real** Asrc with a real
// ring buffer, because a faithful model of a component is still not the
// component, and the thing actually being claimed is that these two hold each
// other steady.

#include "RateEstimator.h"
#include "Asrc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok) { ++g_failed; printf("  FAIL  %s\n", what); }
    else     {             printf("  ok    %s\n", what); }
}

const double kOut    = 48000.0;
const int    kBlock  = 64;
const int    kTarget = 256;        // input frames the ring is held at
const double kPi     = 3.14159265358979323846;

// Blocks per second of simulated time, at the 64-frame block rate.
const int kBlocksPerSec = (int)(kOut / kBlock);   // 750

// The correction the loop is actually applying, in ppm against nominal.
//
// **Not the same thing as trimPpm()**, and confusing the two cost a round of
// wrong conclusions here: trimPpm() is only the integrator, while the ratio
// handed to the ASRC is trim + Kp*error. Early in a run the P term can be
// supplying a good fraction of the total, so asserting on the integrator
// alone reports a converged loop as broken.
double totalPpm(const RateEstimator &e)
{
    if (e.nominalHz() <= 0) return 0.0;
    return ((double)e.rateHz() / (double)e.nominalHz() - 1.0) * 1.0e6;
}

const char *stateName(RateEstimator::State s)
{
    switch (s) {
        case RateEstimator::kNoCarrier:   return "NoCarrier";
        case RateEstimator::kUnsupported: return "Unsupported";
        case RateEstimator::kAcquiring:   return "Acquiring";
        case RateEstimator::kLocked:      return "Locked";
    }
    return "?";
}

// ==========================================================================
//  Analytic loop: a source at `srcHz`, a consumer that takes exactly the
//  ratio the estimator asks for.
// ==========================================================================

struct Loop {
    RateEstimator est;
    double level;          // input frames queued
    double arrivalCredit;  // fractional input frames not yet delivered
    double consumeCredit;  // fractional input frames not yet consumed
    double ratio;          // what the ASRC would be using

    double minLevel, maxLevel;
    double minPpm,   maxPpm;

    void begin()
    {
        est.init((float)kOut, kTarget);
        level         = (double)kTarget;
        arrivalCredit = 0.0;
        consumeCredit = 0.0;
        ratio         = 1.0;
        minLevel = maxLevel = level;
        minPpm = 1e9; maxPpm = -1e9;
        est.setCarrier(true);
    }

    // One block. Returns input frames that arrived.
    int step(double srcHz)
    {
        // Arrivals: the source delivers srcHz frames a second while we make
        // kBlock frames at kOut.
        arrivalCredit += (double)kBlock * srcHz / kOut;
        const int arrived = (int)arrivalCredit;
        arrivalCredit -= (double)arrived;
        level += (double)arrived;

        // Consumption: whatever ratio the estimator last asked for.
        consumeCredit += (double)kBlock * ratio;
        const int consumed = (int)consumeCredit;
        consumeCredit -= (double)consumed;
        level -= (double)consumed;

        const float hz = est.update(arrived, kBlock, (int)(level + 0.5));
        if (hz > 0.0f) ratio = (double)hz / kOut;

        if (level < minLevel) minLevel = level;
        if (level > maxLevel) maxLevel = level;
        if (est.state() == RateEstimator::kLocked) {
            const double p = est.trimPpm();
            if (p < minPpm) minPpm = p;
            if (p > maxPpm) maxPpm = p;
        }
        return arrived;
    }

    void run(double srcHz, int blocks) { for (int i = 0; i < blocks; ++i) step(srcHz); }

    void resetStats()
    {
        minLevel = maxLevel = level;
        minPpm = 1e9; maxPpm = -1e9;
    }
};

// ==========================================================================
//  A ring of stereo input frames, for the closed-loop test.
// ==========================================================================

struct Ring {
    static const int kCap = 4096;
    float buf[kCap * 2];
    int   head, tail, count;

    void clear() { head = tail = count = 0; }

    bool push(float l, float r)
    {
        if (count >= kCap) return false;         // overrun
        buf[head * 2 + 0] = l;
        buf[head * 2 + 1] = r;
        head = (head + 1) % kCap;
        ++count;
        return true;
    }

    // Longest contiguous readable span from the tail.
    int contig() const
    {
        const int toEnd = kCap - tail;
        return (count < toEnd) ? count : toEnd;
    }
    const float *peek() const { return buf + tail * 2; }
    void consume(int n) { tail = (tail + n) % kCap; count -= n; }
};

} // namespace

// ==========================================================================

int main()
{
    printf("RateEstimator: output %.0f Hz, %d-frame blocks, target level %d frames\n\n",
           kOut, kBlock, kTarget);

    // ---- 1. classification ----------------------------------------------

    printf("classification\n");
    {
        const int rates[] = { 32000, 44100, 48000 };
        for (int i = 0; i < 3; ++i) {
            Loop lp; lp.begin();
            lp.run((double)rates[i], kBlocksPerSec);   // one second
            char line[96];
            snprintf(line, sizeof line, "%d Hz is classified as %d Hz",
                     rates[i], lp.est.nominalHz());
            check(lp.est.nominalHz() == rates[i], line);
        }
    }

    {
        // A rate we cannot handle must be reported, not snapped to the
        // nearest thing we know. 96 kHz is the realistic case: it is
        // decimation, and the ASRC's kernel cannot do it.
        Loop lp; lp.begin();
        lp.run(96000.0, kBlocksPerSec);
        printf("        96000 Hz -> state %s, nominal %d\n",
               stateName(lp.est.state()), lp.est.nominalHz());
        check(lp.est.state() == RateEstimator::kUnsupported,
              "96000 Hz reports Unsupported rather than picking a rate");
        check(lp.est.nominalHz() == 0, "an unsupported rate publishes no nominal");
    }

    {
        Loop lp; lp.begin();
        lp.run(22050.0, kBlocksPerSec);
        check(lp.est.state() == RateEstimator::kUnsupported,
              "22050 Hz reports Unsupported");
    }

    {
        RateEstimator e;
        e.init((float)kOut, kTarget);
        const float hz = e.update(0, kBlock, kTarget);
        check(e.state() == RateEstimator::kNoCarrier, "no carrier means NoCarrier");
        check(hz == 0.0f, "no carrier returns no rate, rather than a stale one");
    }

    {
        // **setCarrier(true) every block must behave like calling it once.**
        //
        // This suite originally called it a single time at the start, and that
        // hid a deadlock: clearing the classification window on every call
        // meant a caller reporting the carrier per block - the natural way to
        // drive this - reset the window before it could ever close, so the
        // rate was never classified and the state sat in Acquiring for ever.
        // SpdifInput calls it per block, which is what found it.
        Loop lp;
        lp.est.init((float)kOut, kTarget);
        lp.level = (double)kTarget;
        lp.arrivalCredit = lp.consumeCredit = 0.0;
        lp.ratio = 1.0;
        lp.minLevel = lp.maxLevel = lp.level;
        lp.minPpm = 1e9; lp.maxPpm = -1e9;

        for (int i = 0; i < kBlocksPerSec * 2; ++i) {
            lp.est.setCarrier(true);          // every block, deliberately
            lp.step(44100.0);
        }
        printf("        setCarrier() per block: nominal %d, state %s\n",
               lp.est.nominalHz(), stateName(lp.est.state()));
        check(lp.est.nominalHz() == 44100,
              "reporting the carrier every block still classifies the rate");
    }

    // ---- 2. convergence and steady state --------------------------------

    printf("\nconvergence\n");
    {
        struct { double ppm; const char *name; } cases[] = {
            {  +100.0, "source 100 ppm fast" },
            {  -100.0, "source 100 ppm slow" },
            {   +25.0, "source 25 ppm fast"  },
            {  +500.0, "source 500 ppm fast" },
        };

        for (int c = 0; c < 4; ++c) {
            const double src = 44100.0 * (1.0 + cases[c].ppm * 1.0e-6);
            Loop lp; lp.begin();
            lp.run(src, kBlocksPerSec * 120);          // two minutes

            const double corrErr = fabs(totalPpm(lp.est) - cases[c].ppm);
            const double lvlErr  = fabs(lp.level - (double)kTarget);

            printf("  %s\n", cases[c].name);
            printf("        state %s, correction %+.3f ppm (want %+.1f), "
                   "of which trim %+.2f, level %.1f\n",
                   stateName(lp.est.state()), totalPpm(lp.est),
                   cases[c].ppm, lp.est.trimPpm(), lp.level);

            char line[96];
            snprintf(line, sizeof line, "%s: reaches Locked", cases[c].name);
            check(lp.est.state() == RateEstimator::kLocked, line);

            snprintf(line, sizeof line, "%s: correction within 1 ppm of truth", cases[c].name);
            check(corrErr < 1.0, line);

            snprintf(line, sizeof line, "%s: buffer settles at target", cases[c].name);
            check(lvlErr < 4.0, line);
        }
    }

    // ---- 3. the property the whole thing exists for ---------------------

    printf("\nno drift over a long run\n");
    {
        // One simulated hour. Without a working loop the buffer walks off at
        // 4.8 frames a second here and is 17000 frames away by the end - it
        // would have railed inside a minute. This is the check that says the
        // approach works at all.
        const double src = 44100.0 * (1.0 + 100.0e-6);
        Loop lp; lp.begin();
        lp.run(src, kBlocksPerSec * 120);     // settle for two minutes
        lp.resetStats();
        lp.run(src, kBlocksPerSec * 3600);    // then an hour

        printf("        level over the hour: min %.1f, max %.1f (target %d)\n",
               lp.minLevel, lp.maxLevel, kTarget);
        printf("        trim ripple: %+.4f to %+.4f ppm (%.4f ppm p-p)\n",
               lp.minPpm, lp.maxPpm, lp.maxPpm - lp.minPpm);

        check(fabs(lp.minLevel - kTarget) < 8.0 &&
              fabs(lp.maxLevel - kTarget) < 8.0,
              "buffer level stays within 8 frames of target for an hour");

        // Ratio modulation is pitch modulation, so the steady-state ripple is
        // a real audio number and not just a control-loop nicety. A few tenths
        // of a ppm at this rate is inaudible by a wide margin.
        check((lp.maxPpm - lp.minPpm) < 2.0,
              "steady-state trim ripple under 2 ppm");
    }

    {
        // A crystal drifting with temperature: 40 ppm over ten minutes. The
        // loop should follow it without ever falling out of lock.
        Loop lp; lp.begin();
        lp.run(48000.0, kBlocksPerSec * 60);
        lp.resetStats();

        bool heldLock = true;
        const int blocks = kBlocksPerSec * 600;
        for (int i = 0; i < blocks; ++i) {
            const double ppm = 40.0 * (double)i / (double)blocks;
            lp.step(48000.0 * (1.0 + ppm * 1.0e-6));
            if (lp.est.state() != RateEstimator::kLocked) heldLock = false;
        }
        printf("        after a 40 ppm ramp: trim %+.2f ppm, level %.1f\n",
               lp.est.trimPpm(), lp.level);
        check(heldLock, "lock is held through a 40 ppm drift over ten minutes");
        check(fabs(lp.est.trimPpm() - 40.0) < 2.0, "the drift is tracked to within 2 ppm");
    }

    // ---- 4. the transitions the module's fade policy hangs on ------------

    printf("\ntransitions\n");
    {
        // A source changing rate between tracks - the case that probably
        // causes the plop this feature exists to remove.
        Loop lp; lp.begin();
        lp.run(44100.0, kBlocksPerSec * 120);
        check(lp.est.nominalHz() == 44100 && lp.est.state() == RateEstimator::kLocked,
              "locked at 44100 before the change");

        lp.run(48000.0, kBlocksPerSec * 120);
        printf("        after 44100 -> 48000: nominal %d, state %s, correction %+.3f ppm\n",
               lp.est.nominalHz(), stateName(lp.est.state()), totalPpm(lp.est));
        check(lp.est.nominalHz() == 48000, "re-classifies to 48000");
        check(lp.est.state() == RateEstimator::kLocked, "re-locks at the new rate");
        check(fabs(totalPpm(lp.est)) < 1.0,
              "the new rate is tracked on its own terms, not the old one's");
    }

    {
        // Carrier loss and return. The module fades around this; what is
        // being checked here is that the estimator does not carry stale state
        // into a source that may be something else entirely.
        Loop lp; lp.begin();
        lp.run(44100.0, kBlocksPerSec * 120);

        lp.est.setCarrier(false);
        check(lp.est.state() == RateEstimator::kNoCarrier, "carrier loss drops to NoCarrier");
        check(lp.est.rateHz() == 0.0f, "no rate is published without a carrier");
        check(lp.est.trimPpm() == 0.0f, "the trim is discarded with the carrier");

        lp.est.setCarrier(true);
        lp.level = kTarget;
        lp.run(48000.0, kBlocksPerSec * 120);
        check(lp.est.nominalHz() == 48000 && lp.est.state() == RateEstimator::kLocked,
              "a different source after the gap acquires cleanly");
    }

    {
        // Lock must not be claimed early: the module unmutes on it.
        Loop lp; lp.begin();
        bool earlyLock = false;
        for (int i = 0; i < kBlocksPerSec / 2; ++i) {      // first half second
            lp.step(44100.0);
            if (lp.est.state() == RateEstimator::kLocked) earlyLock = true;
        }
        check(!earlyLock, "Locked is not declared within the first half second");
    }

    // ---- 5. closed loop against the real ASRC ---------------------------

    printf("\nclosed loop with the real Asrc\n");
    {
        Asrc          asrc;
        RateEstimator est;
        Ring          ring;

        asrc.init((float)kOut);
        est.init((float)kOut, kTarget);
        est.setCarrier(true);
        ring.clear();

        const double srcHz = 44100.0 * (1.0 + 120.0e-6);

        // Prime the ring to target before starting, the way the module will
        // after a fade-in.
        double phase = 0.0;
        const double toneHz = 997.0;
        auto genFrame = [&](float &l, float &r) {
            const double v = 0.5 * sin(phase);
            phase += 2.0 * kPi * toneHz / srcHz;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
            l = (float)v; r = (float)v;
        };
        for (int i = 0; i < kTarget; ++i) { float l, r; genFrame(l, r); ring.push(l, r); }

        double arrivalCredit = 0.0;
        int    minCount = ring.count, maxCount = ring.count;
        bool   overran = false, starved = false;
        double worstStep = 0.0, prev = 0.0;

        float out[kBlock * 2];
        const int blocks = kBlocksPerSec * 300;      // five minutes

        for (int b = 0; b < blocks; ++b) {
            // Deliver this block's arrivals.
            arrivalCredit += (double)kBlock * srcHz / kOut;
            const int arrived = (int)arrivalCredit;
            arrivalCredit -= (double)arrived;
            for (int i = 0; i < arrived; ++i) {
                float l, r; genFrame(l, r);
                if (!ring.push(l, r)) overran = true;
            }

            // ---- the mute policy, and why it is not optional -------------
            //
            // **The ASRC must not be pulled before the rate is classified.**
            // The first version of this test drove it unconditionally and it
            // starved inside a second, which is a real constraint on the
            // module and not an artefact of the harness: Asrc::init() leaves
            // the ratio at 1.0, so against a 44.1 kHz source it consumes 64
            // frames a block while 59 arrive. The buffer drains at 5 frames a
            // block and the classification window is 187 blocks long, so it
            // is ~900 frames short before the estimator has said anything.
            //
            // The module is muted over exactly that interval anyway - there
            // is no lock, so the policy is "hold silence". What this models
            // is the other half of being muted: the ring is **trimmed to
            // target rather than allowed to fill**, because 0.25 s of audio
            // nobody played is stale by the time it would be played, and
            // letting it accumulate would overrun the ring instead.
            if (!est.usable()) {
                while (ring.count > kTarget) ring.consume(1);
                asrc.reset();       // prime from fresh audio when we resume
                est.update(arrived, kBlock, ring.count);
                continue;
            }

            // Produce exactly one block, taking from the ring in whatever
            // contiguous spans it happens to offer.
            int made = 0;
            while (made < kBlock) {
                const int avail = ring.contig();
                if (avail <= 0) { starved = true; break; }
                int used = 0;
                made += asrc.produce(out + made * 2, kBlock - made,
                                     ring.peek(), avail, &used);
                ring.consume(used);
                if (used == 0 && made < kBlock) { starved = true; break; }
            }

            const float hz = est.update(arrived, kBlock, ring.count);
            if (hz > 0.0f) asrc.setInputRate(hz);

            if (b > kBlocksPerSec * 60) {           // after settling
                if (ring.count < minCount) minCount = ring.count;
                if (ring.count > maxCount) maxCount = ring.count;
                for (int i = 0; i < made; ++i) {
                    const double v = out[i * 2];
                    const double d = fabs(v - prev);
                    if (d > worstStep) worstStep = d;
                    prev = v;
                }
            } else {
                if (made > 0) prev = out[(made - 1) * 2];
            }
        }

        printf("        state %s, nominal %d, correction %+.3f ppm (source is +120)\n",
               stateName(est.state()), est.nominalHz(), totalPpm(est));
        printf("        ring over the last four minutes: %d..%d (target %d, cap %d)\n",
               minCount, maxCount, kTarget, Ring::kCap);

        check(!overran, "the ring never overran");
        check(!starved, "the ASRC was never starved");
        check(est.state() == RateEstimator::kLocked, "the closed loop reaches Locked");
        check(fabs(totalPpm(est) - 120.0) < 2.0, "the true offset is recovered to 2 ppm");
        check(minCount > kTarget - 16 && maxCount < kTarget + 16,
              "ring stays within 16 frames of target for four minutes");

        // The audio has to survive the loop doing its work. A 997 Hz sine at
        // 0.5 slews at most 2*pi*997/48000*0.5 = 0.0652 a sample; anything
        // well above that would be the ratio being yanked rather than trimmed.
        const double natural = 2.0 * kPi * toneHz / kOut * 0.5;
        printf("        natural max slew %.5f, worst observed %.5f\n",
               natural, worstStep);
        check(worstStep < natural * 1.2,
              "no discontinuity in the audio while the loop is tracking");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
