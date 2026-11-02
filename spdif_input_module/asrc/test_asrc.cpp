// test_asrc.cpp - host test for the ASRC.
//
// **Not in the build.** Like agc/test_autogain.cpp, noisegate/test_noisegate.cpp
// and leveller/test_leveller.cpp this is a host program with no place in a
// firmware image, so it is deliberately absent from spdif_input_module's
// module.cmake SOURCES. Its build command is here and nowhere else:
//
//   "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
//   cl /nologo /O2 /EHsc /W4 /std:c++17 test_asrc.cpp Asrc.cpp
//   test_asrc.exe
//
// or, with a GCC to hand:
//
//   g++ -O2 -Wall -Wextra -o test_asrc test_asrc.cpp Asrc.cpp
//
// ---- what it is for -----------------------------------------------------
//
// This engine was written **before the S/PDIF decoder exists**, deliberately.
// It carries the quality risk of the whole feature and it has no hardware
// dependency, so all of that risk can be retired on the host: if the images
// are not far enough down or the passband is not flat, that is knowable now,
// while this is still two files and not something entangled with a PIO
// program and a rate estimator.
//
// The suite is in three parts:
//
//   1. **Correctness that has an exact answer** - DC gain, silence, the
//      fixed-point phase not drifting, starvation reporting. These are
//      pass/fail and there is nothing to tune.
//   2. **Measurements that are reported, not asserted** - passband edge,
//      image rejection, THD+N. These are printed with generous thresholds,
//      because the useful output is the number, not the verdict. Read them.
//   3. **Properties the module will rely on** - block-boundary invariance,
//      continuity across a rate change.
//
// The thresholds in part 2 are set where a *fault* would trip them, not where
// good performance sits. A tighter number belongs here only once it has been
// read off a real run.

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
    if (!ok) {
        ++g_failed;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

const double kPi   = 3.14159265358979323846;
const double kOut  = 48000.0;
const int    kBlock = 64;          // AUDIO_BLOCK_FRAMES on the target

double dbfs(double v) { return v > 1.0e-30 ? 20.0 * log10(v) : -600.0; }

// ==========================================================================
//  A small radix-2 FFT, host-only, doubles. Only used to look at the output
//  spectrum - nothing here ships.
// ==========================================================================

void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / (double)len;
        const double wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const double xr = re[b] * cr - im[b] * ci;
                const double xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

// Blackman-Harris 4-term, -92 dB sidelobes. Deep enough that leakage from a
// bin-centred tone does not masquerade as an image.
double bh4(int i, int n)
{
    const double t = 2.0 * kPi * (double)i / (double)(n - 1);
    return 0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2.0 * t)
         - 0.01168 * cos(3.0 * t);
}

// ==========================================================================
//  Driving the engine the way the module will: fixed 64-frame output blocks,
//  input supplied a block at a time.
// ==========================================================================

// Generates a stereo sine at `rateHz`, keeping phase across calls.
struct SineSource {
    double freq, amp, rate, phase;
    SineSource(double f, double a, double r)
        : freq(f), amp(a), rate(r), phase(0.0) {}

    void fill(float *buf, int frames)
    {
        for (int i = 0; i < frames; ++i) {
            const double v = amp * sin(phase);
            buf[i * 2 + 0] = (float)v;
            buf[i * 2 + 1] = (float)v;
            phase += 2.0 * kPi * freq / rate;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }
    }
};

// Runs the engine until `outFrames` output frames exist, feeding input in
// small chunks exactly as a ring buffer would. Returns the left channel.
void run(Asrc &a, SineSource &src, double inRate, double *outL, int outFrames)
{
    float inBuf[512 * 2];
    float outBuf[kBlock * 2];

    int   inHave = 0, inPos = 0;
    int   produced = 0;

    while (produced < outFrames) {
        const int want = (produced + kBlock <= outFrames)
                       ? kBlock : (outFrames - produced);

        int made = 0;
        while (made < want) {
            if (inPos >= inHave) {
                // Refill. 128 frames is arbitrary and deliberately not a
                // multiple of anything the engine cares about.
                inHave = 128;
                inPos  = 0;
                src.fill(inBuf, inHave);
            }
            int usedHere = 0;
            made += a.produce(outBuf + made * 2, want - made,
                              inBuf + inPos * 2, inHave - inPos, &usedHere);
            inPos += usedHere;
            if (usedHere == 0 && made < want) {
                // Would spin forever if the engine could not make progress.
                printf("  (engine made no progress - aborting run)\n");
                return;
            }
        }

        for (int i = 0; i < want; ++i)
            outL[produced + i] = outBuf[i * 2 + 0];
        produced += want;
    }
    (void)inRate;
}

// Measures the output spectrum of a resampled tone and returns signal level,
// worst spur (dB relative to signal) and THD+N.
struct SpectrumResult {
    double signalDb;
    double worstSpurDb;
    double thdnDb;
    double freqErrHz;
};

// Level and THD+N are measured in the **time domain**, by coherently fitting
// the fundamental and looking at what is left. That avoids having to account
// for how much of a window's energy lands in the mainlobe - an easy thing to
// get wrong, and it was wrong here first: summing power across the mainlobe
// over-reads a pure tone by exactly sqrt(2), which presented as every rate
// being 3.02 dB hot and looked like an engine gain bug.
//
// The tone is an exact number of cycles in `n` samples (it is placed on an
// output bin centre), so the correlation is clean with no window at all.
//
// The FFT is still used, but only for two things that are ratios between
// bins and therefore immune to that scaling: where the worst spur is, and the
// frequency of the peak.
SpectrumResult analyse(const double *x, int n, double toneHz)
{
    const double w = 2.0 * kPi * toneHz / kOut;

    double sr = 0.0, si = 0.0;
    for (int i = 0; i < n; ++i) {
        sr += x[i] * cos(w * (double)i);
        si += x[i] * sin(w * (double)i);
    }
    sr *= 2.0 / (double)n;
    si *= 2.0 / (double)n;
    const double amp = sqrt(sr * sr + si * si);

    double resPow = 0.0;
    for (int i = 0; i < n; ++i) {
        const double fit = sr * cos(w * (double)i) + si * sin(w * (double)i);
        const double r   = x[i] - fit;
        resPow += r * r;
    }
    const double resRms = sqrt(resPow / (double)n);
    const double sigRms = amp / sqrt(2.0);

    // ---- spectrum, for the spur location and the frequency --------------
    static double re[16384], im[16384];
    double winSum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double win = bh4(i, n);
        re[i] = x[i] * win;
        im[i] = 0.0;
        winSum += win;
    }
    fft(re, im, n);

    const int    half   = n / 2;
    const double binHz  = kOut / (double)n;
    const int    sigBin = (int)(toneHz / binHz + 0.5);

    static double mag[8192];
    for (int i = 0; i < half; ++i)
        mag[i] = sqrt(re[i] * re[i] + im[i] * im[i]) * 2.0 / winSum;

    double worst = 0.0;
    for (int i = 5; i < half; ++i) {
        if (i >= sigBin - 4 && i <= sigBin + 4) continue;   // BH4 mainlobe
        if (mag[i] > worst) worst = mag[i];
    }

    double fpeak = (double)sigBin;
    if (sigBin > 0 && sigBin + 1 < half) {
        const double a = mag[sigBin - 1], b = mag[sigBin], c = mag[sigBin + 1];
        const double d = a - 2.0 * b + c;
        if (fabs(d) > 1.0e-20) fpeak = (double)sigBin + 0.5 * (a - c) / d;
    }

    SpectrumResult r;
    r.signalDb    = dbfs(amp);
    r.worstSpurDb = dbfs(worst) - dbfs(mag[sigBin]);
    r.thdnDb      = dbfs(resRms / sigRms);
    r.freqErrHz   = fpeak * binHz - toneHz;
    return r;
}

} // namespace

// ==========================================================================

int main()
{
    printf("ASRC: %d taps, %d phases, output %.0f Hz, %d-frame blocks\n",
           ASRC_TAPS, ASRC_PHASES, kOut, kBlock);
    printf("kernel table %zu bytes of SRAM\n\n",
           sizeof(float) * (ASRC_PHASES + 1) * ASRC_TAPS);

    // ---- 1. things with an exact answer ---------------------------------

    printf("exact properties\n");
    {
        // DC in, the same DC out. This is the single best correctness check
        // in the file: it fails on a normalisation error, on a history
        // indexing error, and on an off-by-one in the phase table, and it
        // fails visibly rather than as a raised noise floor.
        Asrc a;
        a.init((float)kOut);
        a.setInputRate(44100.0f);

        float in[256 * 2], out[kBlock * 2];
        for (int i = 0; i < 256 * 2; ++i) in[i] = 0.5f;

        int inPos = 0, inHave = 256;
        double worst = 0.0;
        int seen = 0;
        for (int blk = 0; blk < 40; ++blk) {
            int made = 0;
            while (made < kBlock) {
                if (inPos >= inHave) inPos = 0;
                int used = 0;
                made += a.produce(out + made * 2, kBlock - made,
                                  in + inPos * 2, inHave - inPos, &used);
                inPos += used;
            }
            if (blk >= 4)   // let the kernel fill
                for (int i = 0; i < kBlock * 2; ++i) {
                    const double e = fabs(out[i] - 0.5);
                    if (e > worst) worst = e;
                    ++seen;
                }
        }
        printf("        DC error %.3e over %d samples\n", worst, seen);
        check(worst < 1.0e-5, "DC passes through at unit gain");
    }

    {
        Asrc a;
        a.init((float)kOut);
        a.setInputRate(44100.0f);

        float in[256 * 2], out[kBlock * 2];
        memset(in, 0, sizeof in);

        bool clean = true;
        int inPos = 0;
        for (int blk = 0; blk < 100; ++blk) {
            int made = 0;
            while (made < kBlock) {
                if (inPos >= 256) inPos = 0;
                int used = 0;
                made += a.produce(out + made * 2, kBlock - made,
                                  in + inPos * 2, 256 - inPos, &used);
                inPos += used;
            }
            for (int i = 0; i < kBlock * 2; ++i)
                if (out[i] != 0.0f) clean = false;
        }
        check(clean, "silence in, exactly silence out for 100 blocks");
    }

    {
        // The fixed-point phase claim: after millions of frames the achieved
        // ratio must still be the requested one. A float accumulator drifts
        // here, and drift is precisely the fault this engine exists to remove
        // - so if this check ever fails, the engine is not merely inaccurate,
        // it is pointless.
        //
        // **The obvious version of this test does not test that.** Running
        // once and asserting the frame error is small passes just as happily
        // on a constant offset as on real drift - and there *is* a constant
        // offset, the ASRC_TAPS frames the kernel is primed with before it
        // emits anything. At 2M frames a bound of ASRC_TAPS+2 would have let
        // 17 ppm of genuine drift through unnoticed, which is the same order
        // as the error being corrected.
        //
        // So it is run at two lengths and the two errors are compared. An
        // offset is identical at both; drift is proportional. That is the
        // distinction the check has to make.
        auto runFrames = [](int outFrames) -> double {
            Asrc a;
            a.init((float)kOut);
            a.setInputRate(44100.0f);

            float in[256 * 2], out[kBlock * 2];
            memset(in, 0, sizeof in);

            int produced = 0, inPos = 0;
            while (produced < outFrames) {
                int made = 0;
                while (made < kBlock) {
                    if (inPos >= 256) inPos = 0;
                    int used = 0;
                    made += a.produce(out + made * 2, kBlock - made,
                                      in + inPos * 2, 256 - inPos, &used);
                    inPos += used;
                }
                produced += kBlock;
            }
            return (double)a.framesIn()
                 - (double)produced * (44100.0 / kOut);
        };

        const double e1 = runFrames(1000000);
        const double e2 = runFrames(4000000);
        const double slip = fabs(e2 - e1);
        const double ppm  = slip / (3000000.0 * (44100.0 / kOut)) * 1.0e6;

        printf("        offset at 1M frames %+.2f, at 4M frames %+.2f\n", e1, e2);
        printf("        slip across the extra 3M frames %.3f frames (%.4f ppm)\n",
               slip, ppm);
        check(slip < 1.0,
              "the frame error is a fixed priming offset, not drift");
        check(ppm < 0.01,
              "44.1->48 holds its ratio to better than 0.01 ppm");
    }

    {
        // Starvation has to be reported honestly, because the module's
        // coast/fade policy is built entirely on this return value.
        Asrc a;
        a.init((float)kOut);
        a.setInputRate(48000.0f);

        float in[8 * 2], out[kBlock * 2];
        memset(in, 0, sizeof in);

        int used = 0;
        const int made = a.produce(out, kBlock, in, 8, &used);
        printf("        asked %d, got %d, consumed %d of 8\n", kBlock, made, used);
        check(made < kBlock, "a short input produces a short output");
        check(used == 8,     "a short input is fully consumed");
    }

    {
        Asrc a;
        a.init((float)kOut);
        a.setInputRate(44100.0f);
        const int need = a.inputFramesNeeded(kBlock);
        printf("        %d output frames need %d input frames at 44.1 kHz\n",
               kBlock, need);
        check(need >= 58 && need <= 64, "inputFramesNeeded is in the right region");
    }

    // ---- 2. measurements - read the numbers ------------------------------

    printf("\nsignal quality (thresholds are fault detectors, not targets)\n");

    const int kN = 8192;
    static double buf[16384];
    const double binHz = kOut / (double)kN;

    struct { double rate; const char *name; } kRates[] = {
        { 48000.0, "48000 -> 48000 (unity)" },
        { 44100.0, "44100 -> 48000"          },
        { 32000.0, "32000 -> 48000"          },
    };

    for (int r = 0; r < 3; ++r) {
        // A tone on an output bin centre, so the analysis is not fighting
        // scalloping loss while trying to measure a -100 dB image.
        const double tone = 85.0 * binHz;      // ~996 Hz

        Asrc a;
        a.init((float)kOut);
        a.setInputRate((float)kRates[r].rate);

        SineSource src(tone, 0.5, kRates[r].rate);
        run(a, src, kRates[r].rate, buf, kN + 2048);

        // Skip the first 2048 output frames: the kernel is filling and the
        // module will be fading in over this region anyway.
        const SpectrumResult sr = analyse(buf + 2048, kN, tone);

        printf("  %s\n", kRates[r].name);
        printf("        signal %.2f dBFS (in -6.02), freq error %+.4f Hz\n",
               sr.signalDb, sr.freqErrHz);
        printf("        worst spur %.1f dB, THD+N %.1f dB\n",
               sr.worstSpurDb, sr.thdnDb);

        char line[96];
        snprintf(line, sizeof line, "%s: level within 0.1 dB", kRates[r].name);
        check(fabs(sr.signalDb - (-6.0206)) < 0.1, line);

        snprintf(line, sizeof line, "%s: frequency within 0.01 Hz", kRates[r].name);
        check(fabs(sr.freqErrHz) < 0.01, line);

        snprintf(line, sizeof line, "%s: worst spur below -80 dB", kRates[r].name);
        check(sr.worstSpurDb < -80.0, line);

        snprintf(line, sizeof line, "%s: THD+N below -75 dB", kRates[r].name);
        check(sr.thdnDb < -75.0, line);
    }

    {
        // Where the passband actually ends. Printed as a table because the
        // number that matters - how much top end a 44.1 kHz source loses - is
        // a judgement call, and kCutoff in Asrc.cpp is where it is made.
        printf("\n  passband, 44100 -> 48000 (response vs input frequency)\n");
        const double freqs[] = { 100, 1000, 5000, 10000, 15000,
                                 17000, 18000, 19000, 20000, 21000 };
        bool flat = true;
        for (int i = 0; i < 10; ++i) {
            const double f = freqs[i];
            Asrc a;
            a.init((float)kOut);
            a.setInputRate(44100.0f);

            SineSource src(f, 0.5, 44100.0);
            run(a, src, 44100.0, buf, 4096 + 2048);

            double peak = 0.0;
            for (int k = 2048; k < 2048 + 4096; ++k)
                if (fabs(buf[k]) > peak) peak = fabs(buf[k]);

            const double db = dbfs(peak / 0.5);
            printf("        %6.0f Hz  %+7.2f dB\n", f, db);
            if (f <= 15000.0 && fabs(db) > 0.2) flat = false;
        }
        check(flat, "passband flat within 0.2 dB to 15 kHz");
    }

    // ---- 3. properties the module depends on -----------------------------

    printf("\nproperties the input module relies on\n");
    {
        // The rate estimator will move the ratio continuously while running.
        // If that produced a discontinuity, every correction would be a tick
        // and the whole approach would be worse than the drift it fixes.
        Asrc a;
        a.init((float)kOut);
        a.setInputRate(44100.0f);

        SineSource src(1000.0, 0.5, 44100.0);
        float inBuf[128 * 2], outBuf[kBlock * 2];
        int inHave = 0, inPos = 0;

        double prev = 0.0;
        double worstStep = 0.0, worstAfterChange = 0.0;
        bool changed = false;
        int frames = 0;

        for (int blk = 0; blk < 400; ++blk) {
            // Half way through, move the rate by 100 ppm - a plausible
            // correction from a settling estimator.
            if (blk == 200) {
                a.setInputRate(44100.0f * 1.0001f);
                changed = true;
            }
            int made = 0;
            while (made < kBlock) {
                if (inPos >= inHave) { inHave = 128; inPos = 0; src.fill(inBuf, inHave); }
                int used = 0;
                made += a.produce(outBuf + made * 2, kBlock - made,
                                  inBuf + inPos * 2, inHave - inPos, &used);
                inPos += used;
            }
            for (int i = 0; i < kBlock; ++i) {
                const double v = outBuf[i * 2];
                if (frames > 64) {
                    const double d = fabs(v - prev);
                    if (d > worstStep) worstStep = d;
                    if (changed && blk >= 200 && blk < 202 && d > worstAfterChange)
                        worstAfterChange = d;
                }
                prev = v;
                ++frames;
            }
        }
        // A 1 kHz sine at 0.5 amplitude slews at most 2*pi*1000/48000*0.5
        // = 0.0654 per sample. Anything much above that is a discontinuity.
        const double natural = 2.0 * kPi * 1000.0 / kOut * 0.5;
        printf("        natural max slew %.5f, worst overall %.5f, worst at change %.5f\n",
               natural, worstStep, worstAfterChange);
        check(worstStep < natural * 1.2,
              "a 100 ppm rate change mid-stream introduces no discontinuity");
    }

    {
        // The module will call produce() with whatever the ring happens to
        // hold, which is never a tidy number. The output must not depend on
        // how the input was chopped up.
        Asrc a, b;
        a.init((float)kOut); a.setInputRate(44100.0f);
        b.init((float)kOut); b.setInputRate(44100.0f);

        SineSource sa(1000.0, 0.5, 44100.0), sb(1000.0, 0.5, 44100.0);
        static double outA[8192], outB[8192];

        // a: fed in 128-frame chunks (what run() does)
        run(a, sa, 44100.0, outA, 8192);

        // b: fed one frame at a time
        {
            float in1[2], outBuf[kBlock * 2];
            int produced = 0;
            while (produced < 8192) {
                const int want = (produced + kBlock <= 8192) ? kBlock : (8192 - produced);
                int made = 0;
                while (made < want) {
                    sb.fill(in1, 1);
                    int used = 0;
                    made += b.produce(outBuf + made * 2, want - made, in1, 1, &used);
                }
                for (int i = 0; i < want; ++i) outB[produced + i] = outBuf[i * 2];
                produced += want;
            }
        }

        double worst = 0.0;
        for (int i = 0; i < 8192; ++i) {
            const double d = fabs(outA[i] - outB[i]);
            if (d > worst) worst = d;
        }
        printf("        worst difference %.3e\n", worst);
        check(worst < 1.0e-6,
              "output is identical however the input is chopped into calls");
    }

    {
        // reset() has to leave the engine in the state init() did, because
        // the module calls it every time a carrier comes back.
        Asrc a;
        a.init((float)kOut);
        a.setInputRate(44100.0f);

        SineSource s1(1000.0, 0.5, 44100.0);
        static double first[4096], second[4096];
        run(a, s1, 44100.0, first, 4096);

        a.reset();
        a.setInputRate(44100.0f);
        SineSource s2(1000.0, 0.5, 44100.0);
        run(a, s2, 44100.0, second, 4096);

        double worst = 0.0;
        for (int i = 0; i < 4096; ++i) {
            const double d = fabs(first[i] - second[i]);
            if (d > worst) worst = d;
        }
        check(worst < 1.0e-6, "reset() returns the engine to its initial state");
    }

    {
        // A rate the estimator could never legitimately report must be
        // ignored rather than followed, or one bad measurement becomes an
        // audible pitch jump.
        Asrc a;
        a.init((float)kOut);
        a.setInputRate(44100.0f);
        const float before = a.inputRate();
        a.setInputRate(96000.0f);       // decimation - out of scope
        a.setInputRate(0.0f);
        a.setInputRate(-44100.0f);
        a.setInputRate((float)NAN);
        check(a.inputRate() == before, "out-of-range rates are ignored, not followed");
    }

    {
        // **A source at the top supported rate, running fast.** This is the
        // case the accept band has to leave room for, and getting it wrong is
        // silent: setInputRate() holds the previous value rather than
        // clamping, so a rejected trim is a correction that never lands. The
        // buffer then fills without bound and the click arrives seconds after
        // the cause.
        //
        // Nothing else in this suite reaches it, because every other source
        // here is either slower than 48 kHz or exactly 48 kHz.
        Asrc a;
        a.init((float)kOut);
        bool allTook = true;
        const float ppm[] = { 10.0f, 100.0f, 500.0f, 1000.0f };
        for (int i = 0; i < 4; ++i) {
            const float want = 48000.0f * (1.0f + ppm[i] * 1.0e-6f);
            a.setInputRate(want);
            if (fabs((double)a.inputRate() - (double)want) > 0.01) allTook = false;
        }
        printf("        48 kHz +1000 ppm accepted as %.3f Hz\n", (double)a.inputRate());
        check(allTook, "a fast 48 kHz source is tracked, not silently held");

        // **The check above passed for two years while the feature was
        // broken.** inputRate() is the value handed in, stored *before* the
        // step is computed, so it reported the rate as accepted while a second
        // clamp three lines later pinned the step back to exactly 1.0. The
        // accept band said yes and the step said no, and only the accessor was
        // being asked.
        //
        // Found on hardware by an optical loopback, where the source rate *is*
        // the output rate: the loop asked for 1.0005, got 1.0, and wound its
        // integrator into the +1000 ppm rail with the ring frozen for 75
        // seconds. See the note in Asrc.cpp beside kStepMax.
        //
        // So ask the engine what it *did*: at a ratio above 1 it must consume
        // more input than it produces output. That is the whole meaning of a
        // fast source and it is not observable from any accessor.
        //
        // **Measured as a difference against ratio 1.0, not as an absolute.**
        // The first version of this check compared the absolute consumed/made
        // ratio against 1.0005 and **would have passed with the bug present**:
        // `produce()` also draws a couple of frames per call for kernel
        // priming, which is ~1.5 % over these call sizes and swamps the 0.1 %
        // being looked for. Running it longer does not dilute it either, since
        // the overhead is per call. The two runs below use identical call
        // patterns, so the priming is identical and cancels, and what is left
        // is the step.
        {
            int usedAt[2] = { 0, 0 };
            const float rate[2] = { 48000.0f, 48000.0f * 1.001f };

            for (int r = 0; r < 2; ++r) {
                Asrc b;
                b.init((float)kOut);
                b.setInputRate(rate[r]);

                float in[512 * 2], out[256 * 2];
                for (int i = 0; i < 512 * 2; ++i) in[i] = 0.0f;

                for (int pass = 0; pass < 8; ++pass) {
                    int used = 0;
                    b.produce(out, 256, in, 512, &used);
                    usedAt[r] += used;
                }
            }

            // 8 passes x 256 output frames at +1000 ppm is 2048 * 0.001 = 2.0
            // input frames more. Small, exact, and zero when the step is
            // pinned - which is the only outcome being distinguished.
            const int extra = usedAt[1] - usedAt[0];
            printf("        1.000 used %d, 1.001 used %d -> %d extra input frames\n",
                   usedAt[0], usedAt[1], extra);
            check(extra >= 2,
                  "a ratio above 1 actually consumes faster, not just reports it");
        }
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
