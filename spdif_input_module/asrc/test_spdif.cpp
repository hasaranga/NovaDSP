// test_spdif.cpp - host test for the S/PDIF decoder.
//
// **Not in the build**, same as the other host suites here.
//
//   "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
//   cl /nologo /O2 /EHsc /W4 /std:c++17 test_spdif.cpp SpdifRx.cpp
//   test_spdif.exe
//
// or:
//
//   g++ -O2 -Wall -Wextra -o test_spdif test_spdif.cpp SpdifRx.cpp
//
// ---- what is and is not covered -----------------------------------------
//
// The first half of this file is an **IEC 60958 encoder**: it takes stereo
// samples and produces the oversampled pin levels a real transmitter would
// put on the wire, at an arbitrary rate and with arbitrary jitter. The
// decoder is then driven with that and the audio has to come back bit exact.
//
// That covers everything in SpdifRx.cpp - threshold learning, run extraction,
// biphase decode, preamble classification, framing, parity, block position,
// carrier detection and resynchronisation.
//
// **What it cannot cover is the PIO program.** ../spdif_rx.pio is one
// instruction (`in pins, 1`) precisely so that the untestable part is as
// small as it can be, but it is still untested until the board is in front of
// you. What needs checking there is the sampling and the divider, not the
// decode.

#include "SpdifRx.h"
#include "spdif_test_source.h"

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

const double kPi = 3.14159265358979323846;

// Edge jitter the decoder is required to survive with no resyncs at all, as a
// fraction of a UI. Measured rather than assumed - see the jitter group.
#if SPDIF_OVERSAMPLE >= 4
const double kJitterOk = 0.10;
#else
const double kJitterOk = 0.05;
#endif

// ---- shared buffers (too big for the stack) ------------------------------
const int kMaxSamples = 1 << 22;

unsigned char g_levels[kMaxSamples];
uint32_t      g_words[kMaxSamples / 32];
int32_t       g_out[1 << 17];
int32_t       g_srcL[8192], g_srcR[8192];

// A deterministic test signal that exercises the full 24-bit range including
// both sign extremes.
void makeSource(int frames)
{
    for (int i = 0; i < frames; ++i) {
        g_srcL[i] = (int32_t)(sin(2.0 * kPi * 997.0 * i / 48000.0) * 8388607.0);
        g_srcR[i] = (int32_t)(sin(2.0 * kPi * 311.0 * i / 48000.0) * 8388607.0);
    }
    if (frames > 4) {
        g_srcL[1] = 0x7FFFFF; g_srcR[1] = -8388608;   // both rails
        g_srcL[2] = -8388608; g_srcR[2] = 0x7FFFFF;
        g_srcL[3] = 0;        g_srcR[3] = 0;
    }
}

// Encode `frames` frames at `spu` samples per UI, decode, and report how many
// came back and how many matched.
struct RoundTrip { int decoded; int mismatches; int firstBad; };

RoundTrip roundTrip(int frames, double spu, double jitterUi, SpdifRx &rx)
{
    SpdifSource e;
    e.begin(g_levels, kMaxSamples, spu);
    e.jitterUi = jitterUi;

    // A lead-in of plain frames so the histogram has something to learn from
    // before the frames being checked arrive.
    for (int i = 0; i < frames; ++i)
        e.frame(g_srcL[i % 8192], g_srcR[i % 8192], i % 192);

    const int nw = spdif_pack(g_levels, e.n, g_words, kMaxSamples / 32);
    const int got = rx.process(g_words, nw, g_out, 1 << 16);

    // The decoder needs a few hundred runs before its thresholds settle, so
    // the leading frames are legitimately missing.
    //
    // **The offset cannot be inferred from the frame count**, which is what
    // this originally did: `skipped = frames - got` assumes every lost frame
    // is at the start, and frames are lost at the *end* too because spdif_pack()
    // truncates the level stream to whole 32-sample words. That mis-assumption
    // reported every frame as a mismatch while parity was perfect - a
    // combination that can only mean the comparison is wrong, since a real
    // decode error would have to break parity to change the audio.
    //
    // So find the alignment instead of predicting it.
    int skipped = -1;
    for (int off = 0; off <= 64 && off < 8192; ++off) {
        if (got > 0 &&
            g_out[0] == (g_srcL[off] << 8) && g_out[1] == (g_srcR[off] << 8)) {
            skipped = off;
            break;
        }
    }

    RoundTrip r; r.decoded = got; r.mismatches = 0; r.firstBad = -1;
    if (skipped < 0) { r.mismatches = got; r.firstBad = 0; return r; }

    for (int i = 0; i < got; ++i) {
        const int si = (skipped + i) % 8192;
        const int32_t wantL = g_srcL[si] << 8;
        const int32_t wantR = g_srcR[si] << 8;
        if (g_out[i * 2] != wantL || g_out[i * 2 + 1] != wantR) {
            if (r.firstBad < 0) r.firstBad = i;
            ++r.mismatches;
        }
    }
    return r;
}

} // namespace

// ==========================================================================

int main()
{
    printf("SpdifRx: oversample %d, sample clock %u Hz (clk_sys/%.1f)\n\n",
           SPDIF_OVERSAMPLE, SPDIF_OVERSAMPLE * 128u * 48000u,
           307200000.0 / (SPDIF_OVERSAMPLE * 128.0 * 48000.0));

    makeSource(8192);

    // ---- 1. round trip at each supported rate ---------------------------
    //
    // One fixed sample clock, three input rates. The UI is a different number
    // of samples in each - which is the whole reason the thresholds are
    // learnt rather than declared.

    printf("round trip, one sample clock, three input rates\n");
    {
        // Samples per UI follows from the one fixed sample clock: a slower
        // source has a longer UI and therefore more samples in it.
        const int rateHz[3] = { 48000, 44100, 32000 };

        for (int i = 0; i < 3; ++i) {
            const double spu = SPDIF_OVERSAMPLE * (48000.0 / (double)rateHz[i]);
            char label[64];
            snprintf(label, sizeof label, "%d Hz (%.2f samples/UI)", rateHz[i], spu);

            SpdifRx rx;
            const RoundTrip r = roundTrip(1500, spu, 0.0, rx);

            printf("  %s\n", label);
            printf("        %d of 1500 frames, %d mismatches, unit %.3f samples,"
                   " parity errors %u, resyncs %u\n",
                   r.decoded, r.mismatches, (double)rx.unitSamples(),
                   rx.parityErrors(), rx.resyncs());

            char line[128];
            snprintf(line, sizeof line, "%s: decodes almost every frame", label);
            check(r.decoded > 1400, line);

            snprintf(line, sizeof line, "%s: audio is bit exact", label);
            check(r.mismatches == 0, line);

            snprintf(line, sizeof line, "%s: learns the right unit length", label);
            check(fabs((double)rx.unitSamples() - spu) < 0.15 * SPDIF_OVERSAMPLE, line);

            snprintf(line, sizeof line, "%s: no parity errors", label);
            check(rx.parityErrors() == 0, line);

            snprintf(line, sizeof line, "%s: carrier is up", label);
            check(rx.carrier(), line);
        }
    }

    // ---- 2. jitter -------------------------------------------------------

    printf("\njitter tolerance at %d samples per UI (independent random phase\n"
           "error on every edge - the worst case, and pessimistic against a\n"
           "real link, whose jitter is mostly low-frequency wander)\n",
           SPDIF_OVERSAMPLE);
    {
        // Jitter here is **independent random phase error on every edge**,
        // which is the worst case and pessimistic against real S/PDIF: most
        // of a real link's jitter is low-frequency wander, which moves
        // adjacent edges together and so barely changes a run length.
        //
        // The number that matters is resyncs, not mismatches. Once a run is
        // misclassified the framing shifts, and everything after it compares
        // as wrong against a single alignment offset - so the mismatch count
        // measures "did anything go wrong at all", while the resync count
        // measures how often.
        const double jits[] = { 0.10, 0.20, 0.30, 0.40 };
        const double spu = SPDIF_OVERSAMPLE * (48000.0 / 44100.0);

        for (int i = 0; i < 4; ++i) {
            SpdifRx rx;
            const RoundTrip r = roundTrip(1500, spu, jits[i], rx);
            printf("        +-%.2f UI: %4d frames, %4d mismatches, %u parity, %u resync\n",
                   jits[i], r.decoded, r.mismatches, rx.parityErrors(), rx.resyncs());
        }

        // The threshold below is set from what this actually measures, not
        // from what the eye-opening figure suggests it ought to manage. See
        // the note in the module README.
        SpdifRx rx;
        const RoundTrip r = roundTrip(1500, spu, kJitterOk, rx);
        char line[128];
        snprintf(line, sizeof line, "+-%.2f UI of edge jitter decodes clean", kJitterOk);
        check(r.decoded > 1400 && r.mismatches == 0 && rx.resyncs() == 0, line);
    }

    // ---- 2b. digital silence --------------------------------------------

    printf("\ndigital silence\n");
    {
        // **Every source in this suite was a sine wave, and that hid a real
        // bug for a while.** All-zero samples are all zero bits, and a zero
        // bit in biphase is a single 2 UI run - so a silent stream is ~90 %
        // 2 UI runs and the 1 UI cluster collapses to just what the preambles
        // contain. The threshold rule that assumed 1 UI runs were common then
        // picked the 2 UI cluster as the unit, doubled every threshold, and
        // decoded nothing whatever.
        //
        // It is not a corner case - it is what a source sends between tracks.
        // The transmitter's loopback test found it; this is where it belongs.
        SpdifRx rx;
        SpdifSource e;
        e.begin(g_levels, kMaxSamples, SPDIF_OVERSAMPLE * (48000.0 / 44100.0));
        for (int i = 0; i < 1500; ++i) e.frame(0, 0, i % 192);

        const int nw  = spdif_pack(g_levels, e.n, g_words, kMaxSamples / 32);
        const int got = rx.process(g_words, nw, g_out, 1 << 16);

        bool allZero = true;
        for (int i = 0; i < got * 2; ++i) if (g_out[i] != 0) allZero = false;

        printf("        %d of 1500 frames, unit %.3f samples, parity %u, resyncs %u\n",
               got, (double)rx.unitSamples(), rx.parityErrors(), rx.resyncs());
        check(got > 1400, "a silent stream still decodes as frames");
        check(allZero, "and the frames are silent");
        check(rx.carrier(), "a silent stream is still a carrier");
        check(rx.parityErrors() == 0, "no parity errors on silence");
    }

    // ---- 3. block structure ---------------------------------------------

    printf("\nchannel status block\n");
    {
        SpdifRx rx;
        SpdifSource e;
        e.begin(g_levels, kMaxSamples, 2.0);
        for (int i = 0; i < 600; ++i)
            e.frame(g_srcL[i % 8192], g_srcR[i % 8192], i % 192);

        const int nw  = spdif_pack(g_levels, e.n, g_words, kMaxSamples / 32);
        const int got = rx.process(g_words, nw, g_out, 1 << 16);

        // 600 frames spans three block starts, and the position must be
        // walking one per frame rather than one per subframe - a mistake that
        // stays invisible until something reads the channel status.
        printf("        %d frames decoded, block position %d\n", got, rx.blockPos());
        check(got > 550, "frames decode across block boundaries");
        check(rx.blockPos() >= 0 && rx.blockPos() < 192,
              "block position is inside the block");

        const int expected = (600 - 1) % 192;
        const int slack    = 600 - got;      // frames lost while learning
        const bool near = (rx.blockPos() >= expected - slack - 1) &&
                          (rx.blockPos() <= expected + 1);
        printf("        expected about %d (allowing %d lost while learning)\n",
               expected, slack);
        check(near, "block position advances once per frame, not once per subframe");
    }

    // ---- 4. corruption and recovery -------------------------------------

    printf("\ncorruption\n");
    {
        // Flip a stretch of samples in the middle. The decoder must notice
        // (parity or resync), must not emit wrong audio, and must recover.
        SpdifRx rx;
        SpdifSource e;
        e.begin(g_levels, kMaxSamples, 2.0);
        for (int i = 0; i < 1500; ++i)
            e.frame(g_srcL[i % 8192], g_srcR[i % 8192], i % 192);

        const int mid = e.n / 2;
        for (int i = mid; i < mid + 400 && i < e.n; ++i) g_levels[i] ^= 1;

        const int nw  = spdif_pack(g_levels, e.n, g_words, kMaxSamples / 32);
        const int got = rx.process(g_words, nw, g_out, 1 << 16);

        printf("        %d frames, %u parity errors, %u resyncs, carrier %d\n",
               got, rx.parityErrors(), rx.resyncs(), (int)rx.carrier());
        check(rx.parityErrors() > 0 || rx.resyncs() > 0,
              "corruption is noticed rather than silently decoded");
        check(got > 1300, "the decoder recovers and keeps going");
        check(rx.carrier(), "a burst of corruption does not drop the carrier");
    }

    // ---- 5. carrier ------------------------------------------------------

    printf("\ncarrier detection\n");
    {
        SpdifRx rx;
        SpdifSource e;
        e.begin(g_levels, kMaxSamples, 2.0);
        for (int i = 0; i < 400; ++i)
            e.frame(g_srcL[i % 8192], g_srcR[i % 8192], i % 192);
        const int upTo = e.n;
        e.idle(200000);            // transmitter stops, line sits at a level

        int nw = spdif_pack(g_levels, upTo, g_words, kMaxSamples / 32);
        rx.process(g_words, nw, g_out, 1 << 16);
        check(rx.carrier(), "carrier is up while frames are arriving");

        nw = spdif_pack(g_levels + upTo, e.n - upTo, g_words, kMaxSamples / 32);
        rx.process(g_words, nw, g_out, 1 << 16);
        check(!rx.carrier(), "carrier drops after the transmitter stops");

        // And it comes back without a reset - the same source returning
        // should not cost a re-learn.
        SpdifSource e2;
        e2.begin(g_levels, kMaxSamples, 2.0);
        e2.level = 0;
        for (int i = 0; i < 400; ++i)
            e2.frame(g_srcL[i % 8192], g_srcR[i % 8192], i % 192);
        nw = spdif_pack(g_levels, e2.n, g_words, kMaxSamples / 32);
        const int got = rx.process(g_words, nw, g_out, 1 << 16);
        printf("        %d frames after the carrier returned\n", got);
        check(rx.carrier() && got > 380,
              "carrier returns and decodes immediately, thresholds retained");
    }

    // ---- 6. noise --------------------------------------------------------

    printf("\nnoise on a dead input\n");
    {
        // Random levels are not a signal. What must not happen is that this
        // produces audio - a decoder that emits garbage on noise is worse
        // than one that stays quiet, because the module would unmute into it.
        SpdifRx rx;
        uint32_t s = 99991u;
        for (int i = 0; i < 400000; ++i) {
            s = s * 1664525u + 1013904223u;
            g_levels[i] = (unsigned char)((s >> 24) & 1u);
        }
        const int nw  = spdif_pack(g_levels, 400000, g_words, kMaxSamples / 32);
        const int got = rx.process(g_words, nw, g_out, 1 << 16);
        printf("        %d frames from pure noise, carrier %d\n", got, (int)rx.carrier());
        check(got < 40, "random noise does not decode as a stream");
    }

    // ---- 7. the caller's buffer -----------------------------------------

    printf("\ncaller plumbing\n");
    {
        SpdifRx rx;
        SpdifSource e;
        e.begin(g_levels, kMaxSamples, 2.0);
        for (int i = 0; i < 1500; ++i)
            e.frame(g_srcL[i % 8192], g_srcR[i % 8192], i % 192);
        const int nw = spdif_pack(g_levels, e.n, g_words, kMaxSamples / 32);

        // Feeding the same stream in small irregular chunks must give the
        // same audio - the module will call this with whatever the DMA ring
        // happens to hold.
        SpdifRx rx2;
        static int32_t whole[1 << 17];
        const int gotWhole = rx.process(g_words, nw, whole, 1 << 16);

        static int32_t piece[1 << 17];
        int gotPieces = 0, pos = 0;
        while (pos < nw) {
            int chunk = 7 + (pos % 53);
            if (pos + chunk > nw) chunk = nw - pos;
            gotPieces += rx2.process(g_words + pos, chunk,
                                     piece + gotPieces * 2, (1 << 16) - gotPieces);
            pos += chunk;
        }

        bool same = (gotWhole == gotPieces);
        if (same)
            for (int i = 0; i < gotWhole * 2; ++i)
                if (whole[i] != piece[i]) { same = false; break; }

        printf("        whole %d frames, chunked %d frames\n", gotWhole, gotPieces);
        check(same, "output does not depend on how the input is chunked");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
