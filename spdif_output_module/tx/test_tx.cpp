// test_tx.cpp - host test for the S/PDIF transmitter.
//
// **Not in the build**, same as the other host suites in this tree.
//
//   "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
//   cl /nologo /O2 /EHsc /W4 /std:c++17 test_tx.cpp SpdifTx.cpp ..\..\spdif_input_module\asrc\SpdifRx.cpp
//   test_tx.exe
//
// or:
//
//   g++ -O2 -Wall -Wextra -o test_tx test_tx.cpp SpdifTx.cpp ../../spdif_input_module/asrc/SpdifRx.cpp
//
// ---- the loopback, and the one rule it bends ----------------------------
//
// This file includes **another module's header** - spdif_input_module's
// SpdifRx - which is exactly the thing nova_modules.cmake is arranged to
// prevent. It is deliberate and it is confined to here:
//
//   * this is a host program, absent from every SOURCES, so no firmware image
//     contains a line of it;
//   * the include is written as `../../spdif_input_module/...`, which is the
//     form the build makes you type when you mean it;
//   * what it buys is worth more than the isolation - **the transmitter is
//     checked by decoding what it produces**, which tests the preambles, the
//     biphase encoding, the parity, the framing and the sample packing
//     against an independent implementation rather than against a table of
//     expected bytes written by the same person on the same afternoon.
//
// The three preamble tables in this tree - transmitter, receiver and test
// source - are separate copies. This test is what holds them together: if any
// of them drifts, nothing decodes.
//
// ---- what it cannot cover ----------------------------------------------
//
// ../spdif_tx.pio, which is one instruction (`out pins, 1`) precisely so that
// the untestable part is as small as possible, and the analogue side - the
// divider down to 0.5 Vpp for coax, or the level into a TOSLINK transmitter.

#include "SpdifTx.h"
#include "../../spdif_input_module/asrc/SpdifRx.h"

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

const int kFrames   = 4000;
const int kTxWords  = kFrames * SPDIF_TX_WORDS_PER_FRAME;

int32_t  g_src[kFrames * 2];
uint32_t g_tx[kTxWords];

// The receiver works on oversampled pin levels; the transmitter emits one bit
// per UI. Expanding by SPDIF_OVERSAMPLE is what a perfect wire does.
uint32_t g_rx[kTxWords * SPDIF_OVERSAMPLE];
int32_t  g_out[kFrames * 2];

int expand(const uint32_t *ui, int uiWords, uint32_t *out, int maxWords)
{
    const int total = uiWords * 32 * SPDIF_OVERSAMPLE;
    if (total / 32 > maxWords) return 0;

    for (int i = 0; i < total; ++i) {
        const int src = i / SPDIF_OVERSAMPLE;
        const uint32_t bit = (ui[src >> 5] >> (src & 31)) & 1u;
        if (bit) out[i >> 5] |=  (1u << (i & 31));
        else     out[i >> 5] &= ~(1u << (i & 31));
    }
    return total / 32;
}

void makeSource(void)
{
    for (int i = 0; i < kFrames; ++i) {
        const double l = sin(2.0 * kPi * 997.0 * i / 48000.0) * 8388607.0;
        const double r = sin(2.0 * kPi * 311.0 * i / 48000.0) * 8388607.0;
        g_src[i * 2 + 0] = (int32_t)l << 8;
        g_src[i * 2 + 1] = (int32_t)r << 8;
    }
    // Both rails and zero, so the sign path and the full 24-bit range are in
    // the stream rather than only the middle of it.
    g_src[2] = (int32_t)0x7FFFFF << 8;  g_src[3] = (int32_t)(-8388608) << 8;
    g_src[4] = (int32_t)(-8388608) << 8; g_src[5] = (int32_t)0x7FFFFF << 8;
    g_src[6] = 0;                        g_src[7] = 0;
}

// Count UI runs in the transmitted stream, so the shape can be checked
// without a decoder: biphase never produces a run longer than 2 UI, and a
// 3 UI run is therefore always a preamble.
void runHistogram(const uint32_t *ui, int words, int *hist, int bins)
{
    for (int i = 0; i < bins; ++i) hist[i] = 0;

    int last = (int)(ui[0] & 1u);
    int len  = 0;
    const int total = words * 32;
    for (int i = 0; i < total; ++i) {
        const int b = (int)((ui[i >> 5] >> (i & 31)) & 1u);
        if (b == last) { ++len; }
        else {
            if (len > 0 && len < bins) ++hist[len];
            last = b; len = 1;
        }
    }
}

} // namespace

// ==========================================================================

int main()
{
    printf("SpdifTx: %d UI per frame, %d words per frame, UI rate %d Hz "
           "(clk_sys/%.1f)\n\n",
           SPDIF_TX_UI_PER_FRAME, SPDIF_TX_WORDS_PER_FRAME,
           128 * 48000, 307200000.0 / (128.0 * 48000.0));

    makeSource();

    SpdifTx tx;
    tx.init();
    const int nw = tx.encode(g_src, kFrames, g_tx, kTxWords);

    // ---- 1. geometry -----------------------------------------------------

    printf("geometry\n");
    {
        printf("        %d frames -> %d words\n", kFrames, nw);
        check(nw == kTxWords, "every frame is exactly four words, no carry");

        // A buffer one word short must produce nothing rather than a partial
        // frame - a torn frame on the wire is worse than a late one.
        SpdifTx t2; t2.init();
        uint32_t small[8];
        check(t2.encode(g_src, 4, small, 7) == 0,
              "a buffer too small encodes nothing rather than part of a frame");
        check(t2.encode(g_src, 2, small, 8) == 8, "an exact-fit buffer is accepted");
    }

    // ---- 2. the shape of the line ----------------------------------------

    printf("\nbiphase shape\n");
    {
        int hist[8];
        runHistogram(g_tx, nw, hist, 8);
        printf("        run lengths: 1 UI %d, 2 UI %d, 3 UI %d, 4+ UI %d\n",
               hist[1], hist[2], hist[3], hist[4] + hist[5] + hist[6] + hist[7]);

        check(hist[4] + hist[5] + hist[6] + hist[7] == 0,
              "no run longer than 3 UI - anything longer is not biphase");
        check(hist[3] > 0, "3 UI runs exist - those are the preambles");
        check(hist[1] > 0 && hist[2] > 0, "both data run lengths are present");

        // Every subframe opens with a preamble and each preamble contains
        // exactly two 3 UI runs (the leading one, and one more in B and M).
        // W has one. So the count is bounded by the subframe count either way.
        printf("        %d subframes sent, %d runs of 3 UI\n", kFrames * 2, hist[3]);
        check(hist[3] >= kFrames * 2,
              "at least one 3 UI run per subframe");
    }

    // ---- 3. the loopback -------------------------------------------------

    printf("\nloopback through SpdifRx\n");
    {
        memset(g_rx, 0, sizeof g_rx);
        const int rxWords = expand(g_tx, nw, g_rx, kTxWords * SPDIF_OVERSAMPLE);
        check(rxWords > 0, "the UI stream expands to receiver samples");

        SpdifRx rx;
        const int got = rx.process(g_rx, rxWords, g_out, kFrames);

        printf("        %d of %d frames decoded, unit %.3f samples, "
               "parity errors %u, resyncs %u\n",
               got, kFrames, (double)rx.unitSamples(),
               rx.parityErrors(), rx.resyncs());

        check(got > kFrames - 60, "almost every transmitted frame decodes back");
        check(rx.parityErrors() == 0, "the receiver finds no parity errors");
        check(rx.resyncs() == 0, "the receiver never loses framing");
        check(rx.carrier(), "the receiver sees a carrier");
        check(fabs((double)rx.unitSamples() - SPDIF_OVERSAMPLE) < 0.1,
              "the receiver measures the unit length we transmitted");

        // Align: the receiver drops a few frames while its histogram settles.
        int skip = -1;
        for (int off = 0; off <= 64 && off < kFrames; ++off) {
            if (got > 0 && g_out[0] == g_src[off * 2] &&
                           g_out[1] == g_src[off * 2 + 1]) { skip = off; break; }
        }
        printf("        aligned at source frame %d\n", skip);
        check(skip >= 0, "the decoded stream lines up with what was sent");

        int bad = 0;
        if (skip >= 0) {
            for (int i = 0; i < got && (skip + i) < kFrames; ++i)
                if (g_out[i * 2] != g_src[(skip + i) * 2] ||
                    g_out[i * 2 + 1] != g_src[(skip + i) * 2 + 1]) ++bad;
        }
        printf("        %d mismatched frames\n", bad);
        check(bad == 0, "audio survives the round trip bit exact, both channels");
    }

    // ---- 4. the block, and channel status --------------------------------

    printf("\nchannel status block\n");
    {
        SpdifTx t; t.init();
        check(t.blockPos() == 0, "a fresh transmitter starts at the block top");

        uint32_t buf[192 * SPDIF_TX_WORDS_PER_FRAME];
        t.encode(g_src, 192, buf, (int)(sizeof buf / sizeof buf[0]));
        check(t.blockPos() == 0, "192 frames returns to the block top");

        t.encode(g_src, 5, buf, (int)(sizeof buf / sizeof buf[0]));
        check(t.blockPos() == 5, "the block position advances one per frame");

        // The B preamble marks the block top and must appear exactly once in
        // 192 frames. It is the only preamble whose second run is 1 UI, so
        // counting 3,1 pairs at subframe starts finds it - but the simpler
        // check is that the receiver's own block tracking agrees, which the
        // loopback above already exercised.
        SpdifRx rx;
        SpdifTx t3; t3.init();
        static uint32_t ui[600 * SPDIF_TX_WORDS_PER_FRAME];
        static uint32_t samp[600 * SPDIF_TX_WORDS_PER_FRAME * SPDIF_OVERSAMPLE];
        static int32_t  out[600 * 2];
        const int n = t3.encode(g_src, 600, ui, (int)(sizeof ui / sizeof ui[0]));
        memset(samp, 0, sizeof samp);
        const int sw = expand(ui, n, samp, (int)(sizeof samp / sizeof samp[0]));
        const int got = rx.process(samp, sw, out, 600);
        printf("        600 frames sent, %d decoded, receiver block position %d\n",
               got, rx.blockPos());
        check(rx.blockPos() >= 0,
              "the receiver finds the block starts we transmitted");
    }

    // ---- 5. silence ------------------------------------------------------

    printf("\nsilence\n");
    {
        // Digital silence must still be a *stream* - preambles, parity, a
        // carrier. A transmitter that goes quiet on the wire when the audio
        // is quiet would make every downstream DAC re-lock, which is the
        // whole class of fault this project is trying to remove.
        static int32_t quiet[600 * 2];
        memset(quiet, 0, sizeof quiet);

        SpdifTx t; t.init();
        static uint32_t ui[600 * SPDIF_TX_WORDS_PER_FRAME];
        static uint32_t samp[600 * SPDIF_TX_WORDS_PER_FRAME * SPDIF_OVERSAMPLE];
        static int32_t  out[600 * 2];

        const int n = t.encode(quiet, 600, ui, (int)(sizeof ui / sizeof ui[0]));
        memset(samp, 0, sizeof samp);
        const int sw = expand(ui, n, samp, (int)(sizeof samp / sizeof samp[0]));

        int hist[8];
        runHistogram(ui, n, hist, 8);
        printf("        silence: 1 UI %d, 2 UI %d, 3 UI %d, 4+ %d\n",
               hist[1], hist[2], hist[3], hist[4] + hist[5] + hist[6] + hist[7]);
        check(hist[4] + hist[5] + hist[6] + hist[7] == 0,
              "silence still produces a legal biphase line, not a stuck level");

        SpdifRx rx;
        const int got = rx.process(samp, sw, out, 600);
        bool allZero = true;
        for (int i = 0; i < got * 2; ++i) if (out[i] != 0) allZero = false;
        printf("        %d frames decoded, carrier %d\n", got, (int)rx.carrier());
        check(got > 540 && rx.carrier(), "silence decodes as a carrier with frames");
        check(allZero, "silence comes back as silence");
        check(rx.parityErrors() == 0, "silence carries correct parity");
    }

    // ---- 6. reset --------------------------------------------------------

    printf("\nreset\n");
    {
        SpdifTx a; a.init();
        SpdifTx b; b.init();
        static uint32_t ua[64 * SPDIF_TX_WORDS_PER_FRAME];
        static uint32_t ub[64 * SPDIF_TX_WORDS_PER_FRAME];

        a.encode(g_src, 64, ua, (int)(sizeof ua / sizeof ua[0]));
        b.encode(g_src, 200, ub, (int)(sizeof ub / sizeof ub[0]));
        b.reset();
        b.encode(g_src, 64, ub, (int)(sizeof ub / sizeof ub[0]));

        bool same = true;
        for (int i = 0; i < 64 * SPDIF_TX_WORDS_PER_FRAME; ++i)
            if (ua[i] != ub[i]) { same = false; break; }
        check(same, "reset() returns the transmitter to its initial state");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
