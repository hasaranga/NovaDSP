// test_spdif_ui.cpp - host test for the per-UI S/PDIF front end and decoder.
//
// **Not in the build**, same as the other host suites here.
//
//   "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
//   cl /nologo /O2 /EHsc /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS test_spdif_ui.cpp SpdifRxUi.cpp
//   test_spdif_ui.exe
//
// or:
//
//   g++ -O2 -Wall -Wextra -o test_spdif_ui test_spdif_ui.cpp SpdifRxUi.cpp
//
// ---- what this covers that the other suites cannot ----------------------
//
// **It runs the PIO program.** test_spdif.cpp drives SpdifRx from packed pin
// levels and says plainly that the one thing it cannot reach is
// ../spdif_rx.pio. This one assembles ../spdif_rx_ui.pio with the SDK's own
// pioasm and executes the instruction words that come out, one cycle at a
// time, against a two-cycle input synchroniser - so the delay slots, the poll
// loop length and the branch targets are all under test, and an edit to the
// .pio reaches this file.
//
// It finds pioasm at $PIOASM, or at %USERPROFILE%\.pico-sdk\tools\2.3.0\
// pioasm\pioasm.exe. **If it cannot assemble the program it fails rather than
// skipping**: a suite that quietly stops testing the interesting half is worse
// than one that does not run.
//
// ---- what it still cannot cover -----------------------------------------
//
// The clock divider (arithmetic in the .pio's c-sdk block, never executed
// here), the pad's real synchroniser timing, the DMA ring, and whether GP1 is
// connected to anything. Those need the board.
//
// ---- the one property everything else rests on --------------------------
//
// The front end must emit **exactly one sample per UI**. Every check below
// that decodes audio is downstream of that, and the first group tests it
// directly by counting.

#include "SpdifRxUi.h"
#include "PioSim.h"
#include "spdif_test_source.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

void checkf(bool ok, const char *fmt, double a, double b)
{
    ++g_checks;
    if (!ok) { ++g_failed; printf("  FAIL  "); } else printf("  ok    ");
    printf(fmt, a, b);
    printf("\n");
}

const double kPi = 3.14159265358979323846;

// State machine cycles per UI. Must match SPDIF_UI_CYCLES in the .pio; the
// first group checks the program's free-run period against it rather than
// trusting the two to agree.
const int kUiCycles = 32;

// The divider positions the module scans - one per supported rate. A shared
// position for 44100 and 48000 is inside the window and is deliberately not
// used; the rate sweep below is what says so.
const double kScanAB = 46010.0;      // the shared position, kept as a check


// Samples per UI the encoder must produce for a source at `fsReal` when the
// front end is tuned for `fsNom`. This is the whole rate-error model: a UI
// that is longer or shorter than the state machine expects.
double spuFor(double fsReal, double fsNom) { return kUiCycles * fsNom / fsReal; }

// ---- shared buffers (far too big for the stack) -------------------------
//
// One UI is 32 state machine cycles here, so a stereo frame is 4096 of them.
// Everything is generated in chunks and rebased, which is what
// SpdifSource::rebase() is for.
const int kChunkSamples = 1 << 21;          // 512 frames' worth
const int kMaxWords     = kChunkSamples / kUiCycles + 64;
const int kMaxFrames    = kChunkSamples / (128 * kUiCycles) + 64;

unsigned char g_levels[kChunkSamples];
uint32_t      g_words[kMaxWords];
int32_t       g_out[kMaxFrames * 2];

uint16_t      g_prog[32];
int           g_progN = 0;

// ---- assembling the shipped program -------------------------------------

bool assemble_program()
{
    char cmd[1024];
    const char *pioasm = getenv("PIOASM");
    char dflt[512];

    if (!pioasm) {
        const char *home = getenv("USERPROFILE");
        if (!home) home = getenv("HOME");
        if (!home) home = ".";
        snprintf(dflt, sizeof dflt,
                 "%s/.pico-sdk/tools/2.3.0/pioasm/pioasm.exe", home);
        pioasm = dflt;
    }

    snprintf(cmd, sizeof cmd, "\"%s\" -o hex ../spdif_rx_ui.pio spdif_rx_ui.hex",
             pioasm);
    const int rc = system(cmd);
    if (rc != 0) {
        printf("  pioasm failed (rc %d): %s\n", rc, cmd);
        printf("  set PIOASM to the assembler's path and re-run.\n");
        return false;
    }

    g_progN = pio_load_hex("spdif_rx_ui.hex", g_prog, 32);
    return g_progN > 0;
}

// ---- the rig ------------------------------------------------------------
//
// Encoder -> state machine emulator -> decoder, in chunks, so that a run of
// any length costs a fixed amount of memory.

struct Rig {
    SpdifSource src;
    PioSim      sm;
    SpdifRxUi   rx;

    // Everything the decoder produced over the whole run.
    int32_t *audio;
    int      audioCap;
    int      audioFrames;

    long long uiGenerated;      // UI the encoder put on the wire
    long long wordsPushed;      // words the state machine produced
    int       framePos;

    void begin(double spu, int32_t *audioBuf, int cap)
    {
        src.begin(g_levels, kChunkSamples, spu);
        sm.load(g_prog, g_progN);
        sm.configure(true, 32, true);
        rx.reset();

        audio = audioBuf; audioCap = cap; audioFrames = 0;
        uiGenerated = 0; wordsPushed = 0; framePos = 0;
    }

    void jitter(double ui) { src.jitterUi = ui; }

    // Push whatever the encoder has buffered through the state machine and
    // the decoder, then rebase the encoder.
    void flush()
    {
        int nw = 0;
        for (int i = 0; i < src.n; ++i) {
            uint32_t w;
            if (sm.cycle(g_levels[i], &w) && nw < kMaxWords) g_words[nw++] = w;
        }
        wordsPushed += nw;

        // Fed in the sizes fill() uses, not as one large call, so that the
        // decoder's own word-to-word state carries across calls the way it
        // has to on the board.
        const int kCall = 256;
        for (int i = 0; i < nw; i += kCall) {
            const int n = (nw - i < kCall) ? (nw - i) : kCall;
            const int room = audioCap - audioFrames;
            const int made = rx.process(&g_words[i], n, &audio[audioFrames * 2],
                                        room > 0 ? room : 0);
            audioFrames += made;
        }
        src.rebase();
    }

    // Generate `n` frames of whatever `gen` produces, flushing as the buffer
    // fills.
    template <class Gen>
    void frames(int n, Gen gen)
    {
        for (int i = 0; i < n; ++i) {
            if (src.n > kChunkSamples - 8 * 128 * kUiCycles) flush();
            int32_t l, r;
            gen(framePos, l, r);
            src.frame(l, r, framePos % 192);
            uiGenerated += 128;
            ++framePos;
        }
        flush();
    }

    // A stuck line, in UI worth of samples.
    void idleUi(int ui)
    {
        for (int i = 0; i < ui; ++i) {
            if (src.n > kChunkSamples - 64) flush();
            src.idle(kUiCycles);
        }
        flush();
    }
};

// A 1 kHz sine at 24 bits, deterministic and different in the two channels so
// a swapped pair cannot pass.
void sine(int pos, int32_t &l, int32_t &r)
{
    const double t = (double)pos;
    l = (int32_t)(7000000.0 * sin(2.0 * kPi * t / 48.0));
    r = (int32_t)(5000000.0 * sin(2.0 * kPi * t / 64.0 + 1.0));
}

void silence(int, int32_t &l, int32_t &r) { l = 0; r = 0; }

// Compare what came back against what the encoder was asked to send. The
// decoder may start a frame or two late - it has to find a preamble first -
// so the comparison is aligned by searching a small window.
bool audio_matches(const Rig &rig, int nFrames, int &offsetOut, int &firstBad)
{
    offsetOut = -1; firstBad = -1;
    if (rig.audioFrames < nFrames / 2) return false;

    for (int off = 0; off < 8; ++off) {
        bool ok = true;
        const int n = rig.audioFrames - 0;
        for (int i = 0; i + off < n && i < nFrames - 8; ++i) {
            int32_t l, r;
            sine(i + off, l, r);
            const int32_t el = (int32_t)((uint32_t)(l & 0xFFFFFF) << 8);
            const int32_t er = (int32_t)((uint32_t)(r & 0xFFFFFF) << 8);
            if (rig.audio[i * 2] != el || rig.audio[i * 2 + 1] != er) {
                ok = false; firstBad = i; break;
            }
        }
        if (ok) { offsetOut = off; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
//  1. The program, and the one-sample-per-UI property
// ---------------------------------------------------------------------------

void test_program()
{
    printf("\n-- the PIO program --\n");

    check(g_progN == 13, "assembles to 13 instructions");

    // The free-run period. With no edges at all the program samples once per
    // SPDIF_UI_CYCLES, so a stuck line is the cleanest way to measure it -
    // and it is also what a dead carrier looks like, which is worth knowing
    // stays regular.
    PioSim sm;
    sm.load(g_prog, g_progN);
    sm.configure(true, 32, true);

    int pushes = 0;
    const int kCycles = 32 * 1000;
    for (int i = 0; i < kCycles; ++i) {
        uint32_t w;
        if (sm.cycle(0, &w)) ++pushes;
    }
    // Each push is 32 samples, and at one sample per kUiCycles cycles that is
    // kCycles / (32 * kUiCycles) pushes, give or take the first partial word.
    const int expect = kCycles / (32 * kUiCycles);
    checkf(pushes >= expect - 1 && pushes <= expect,
           "free-runs at one sample per %.0f cycles (%.0f words)",
           (double)kUiCycles, (double)pushes);

    check(sm.unsupported() == 0, "no unsupported instructions executed");

    // The property everything else rests on: one sample per UI, whatever the
    // data. 600 frames of sine is 76800 UI and every run length in the format.
    static int32_t audio[kMaxFrames * 2];
    Rig rig;
    rig.begin(spuFor(48000.0, 48000.0), audio, kMaxFrames);
    rig.frames(600, sine);

    const long long samples = rig.wordsPushed * 32;
    // The state machine is still holding up to 31 UI in its ISR, and it needs
    // one edge to find its phase at the start.
    checkf(samples <= rig.uiGenerated &&
           samples >= rig.uiGenerated - 64,
           "one sample per UI: %.0f UI in, %.0f out", (double)rig.uiGenerated,
           (double)samples);

    check(rig.sm.unsupported() == 0, "still no unsupported instructions");
}

// ---------------------------------------------------------------------------
//  2. Decoding, at every rate and on both divider positions
// ---------------------------------------------------------------------------

void decode_at(double fsReal, double fsNom, const char *what)
{
    static int32_t audio[kMaxFrames * 2];
    Rig rig;
    rig.begin(spuFor(fsReal, fsNom), audio, kMaxFrames);
    rig.frames(400, sine);

    int off = -1, bad = -1;
    const bool ok = audio_matches(rig, 400, off, bad);

    ++g_checks;
    if (!ok) {
        ++g_failed;
        printf("  FAIL  %s: %d frames, first mismatch at %d, parity %u, "
               "resync %u, framed %d\n",
               what, rig.audioFrames, bad,
               (unsigned)rig.rx.parityErrors(), (unsigned)rig.rx.resyncs(),
               (int)rig.rx.framed());
    } else {
        printf("  ok    %s: %d frames bit exact (lock cost %d), parity %u, "
               "resync %u\n",
               what, rig.audioFrames, off,
               (unsigned)rig.rx.parityErrors(), (unsigned)rig.rx.resyncs());
    }

    check(rig.rx.parityErrors() == 0, "  .. no parity errors");
    check(rig.rx.resyncs() == 0,      "  .. no resyncs");
    check(rig.rx.carrier(),           "  .. carrier held");
    check(rig.rx.framed(),            "  .. still framed at the end");
}

void test_rates()
{
    printf("\n-- decode, each rate on its own scan position --\n");
    decode_at(48000.0, 48000.0, "48000");
    decode_at(44100.0, 44100.0, "44100");
    decode_at(32000.0, 32000.0, "32000");

    printf("\n-- and both common rates on one shared position, which the\n"
           "   module does not use but the window says it could --\n");
    decode_at(48000.0, kScanAB, "48000 on the 46010 position");
    decode_at(44100.0, kScanAB, "44100 on the 46010 position");
}

// ---------------------------------------------------------------------------
//  3. Digital silence, and full scale
// ---------------------------------------------------------------------------

void test_silence()
{
    printf("\n-- digital silence and full scale --\n");

    // **The case that broke the oversampling decoder's threshold learner.**
    // All-zero samples are all zero bits, which is all 2 UI runs, and the only
    // 1 UI runs in the stream are inside the preambles. This decoder learns
    // nothing, so there is nothing to mislead - but silence is what a source
    // sends between tracks and it has to be checked rather than reasoned about.
    {
        static int32_t audio[kMaxFrames * 2];
        Rig rig;
        rig.begin(spuFor(44100.0, 44100.0), audio, kMaxFrames);
        rig.frames(300, silence);

        bool allZero = (rig.audioFrames > 250);
        for (int i = 0; i < rig.audioFrames * 2; ++i)
            if (rig.audio[i] != 0) { allZero = false; break; }

        check(allZero, "digital silence decodes, and decodes to zero");
        check(rig.rx.parityErrors() == 0 && rig.rx.resyncs() == 0,
              "  .. clean throughout");
    }

    // All ones is the other degenerate pattern: every data bit a 1, so every
    // data run is 1 UI and the whole stream is at the short end.
    {
        static int32_t audio[kMaxFrames * 2];
        Rig rig;
        rig.begin(spuFor(48000.0, 48000.0), audio, kMaxFrames);
        rig.frames(300, [](int, int32_t &l, int32_t &r) {
            l = (int32_t)0xFFFFFF; r = (int32_t)0xFFFFFF;
        });

        int nonZero = 0;
        for (int i = 0; i < rig.audioFrames * 2; ++i)
            if (rig.audio[i] == (int32_t)0xFFFFFF00) ++nonZero;

        check(rig.audioFrames > 250 && nonZero == rig.audioFrames * 2,
              "all-ones decodes to 0xFFFFFF00 in both channels");
        check(rig.rx.parityErrors() == 0 && rig.rx.resyncs() == 0,
              "  .. clean throughout");
    }
}

// ---------------------------------------------------------------------------
//  4. Block position
// ---------------------------------------------------------------------------

void test_block_position()
{
    printf("\n-- the 192-frame channel status block --\n");

    static int32_t audio[kMaxFrames * 2];
    Rig rig;
    rig.begin(spuFor(48000.0, 48000.0), audio, kMaxFrames);
    rig.frames(400, sine);

    // 400 frames is two block starts plus a bit, so the position must be a
    // sensible distance into the second one rather than at -1 or stuck.
    const int pos = rig.rx.blockPos();
    checkf(pos >= 0 && pos < 192, "block position tracked: %.0f (0..191)%.0s",
           (double)pos, 0.0);
}

// ---------------------------------------------------------------------------
//  5. Jitter
// ---------------------------------------------------------------------------

void test_jitter()
{
    printf("\n-- edge jitter --\n");
    printf("     the oversampling decoder was clean to +-0.10 UI and broke\n"
           "     at +-0.20; sampling the centre of a UI rather than resolving\n"
           "     its ends should do considerably better. Measured, not assumed:\n");

    double lastClean = 0.0;
    for (double j = 0.05; j <= 0.30001; j += 0.025) {
        static int32_t audio[kMaxFrames * 2];
        Rig rig;
        rig.begin(spuFor(48000.0, 48000.0), audio, kMaxFrames);
        rig.jitter(j);
        rig.frames(300, sine);

        int off = -1, bad = -1;
        const bool exact = audio_matches(rig, 300, off, bad);
        const unsigned err = rig.rx.parityErrors() + rig.rx.resyncs();

        printf("     +-%.2f UI : %4d frames, parity %2u, resync %2u  %s\n",
               j, rig.audioFrames, (unsigned)rig.rx.parityErrors(),
               (unsigned)rig.rx.resyncs(), (exact && err == 0) ? "clean" : "");
        if (exact && err == 0) lastClean = j;
    }

    // **The number here is the measurement, not an aspiration.** It was 0.175
    // when this was written; a change that moves it is a change worth knowing
    // about in either direction, which a loose bound would hide.
    checkf(lastClean >= 0.175, "clean to at least +-0.175 UI (reached %.3f)%.0s",
           lastClean, 0.0);
}

// ---------------------------------------------------------------------------
//  6. The rate window the divider buys
// ---------------------------------------------------------------------------

void test_rate_window()
{
    printf("\n-- the rate window (the .pio derives 0.881x to 1.170x) --\n");

    // Inside the window the decoder must be bit exact; outside it must fail
    // rather than produce plausible rubbish. The second half matters as much
    // as the first: a front end tuned for the wrong rate must not lock.
    //
    // The two `true` cases nearest the cliffs are one step inside the derived
    // bound rather than on it, because the bound is a real number and the
    // encoder's UI is not - a source exactly on the cliff is allowed to go
    // either way and a suite that asserted otherwise would be flaky.
    struct Case { double ratio; bool expect; };
    const Case cases[] = {
        { 0.90, true  }, { 0.95, true }, { 1.00, true },
        { 1.10, true  }, { 1.16, true },
        { 0.85, false }, { 1.25, false }, { 1.50, false },
    };

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        static int32_t audio[kMaxFrames * 2];
        Rig rig;
        // ratio is Freal / Fnom, so a larger ratio is a shorter real UI.
        rig.begin(kUiCycles / cases[i].ratio, audio, kMaxFrames);
        rig.frames(250, sine);

        int off = -1, bad = -1;
        const bool exact = audio_matches(rig, 250, off, bad) &&
                           rig.rx.parityErrors() == 0;
        const bool got = exact && rig.audioFrames > 200;

        ++g_checks;
        if (got != cases[i].expect) {
            ++g_failed;
            printf("  FAIL  %.2fx: %d frames, expected %s\n", cases[i].ratio,
                   rig.audioFrames, cases[i].expect ? "decode" : "no decode");
        } else {
            printf("  ok    %.2fx: %s (%d frames)\n", cases[i].ratio,
                   cases[i].expect ? "decodes" : "declines", rig.audioFrames);
        }
    }
}

// ---------------------------------------------------------------------------
//  7. Carrier loss, re-acquisition, and a broken stream
// ---------------------------------------------------------------------------

void test_carrier()
{
    printf("\n-- carrier loss and recovery --\n");

    static int32_t audio[kMaxFrames * 2];
    Rig rig;
    rig.begin(spuFor(48000.0, 48000.0), audio, kMaxFrames);
    rig.frames(120, sine);

    check(rig.rx.carrier(), "carrier up while the source is playing");

    // A stuck line for longer than the timeout.
    rig.idleUi((int)SPDIF_UI_CARRIER_TIMEOUT + 4096);
    check(!rig.rx.carrier(), "carrier dropped after a stuck line");
    check(!rig.rx.framed(),  "framing dropped with it");

    const int before = rig.audioFrames;
    rig.frames(200, sine);
    checkf(rig.audioFrames - before > 150,
           "re-acquires when the source returns: %.0f frames%.0s",
           (double)(rig.audioFrames - before), 0.0);
    check(rig.rx.carrier(), "  .. carrier back up");
}

void test_dropped_ui()
{
    printf("\n-- a dropped UI, which is what a FIFO overflow looks like --\n");

    // Generate a clean stream, then delete one word from the middle of it
    // before the decoder sees it. That shifts the subframe alignment by 32 UI
    // - an even number, so the cell structure survives and only the framing
    // is wrong, which is the case the structural check exists for.
    static int32_t audio[kMaxFrames * 2];
    Rig rig;
    rig.begin(spuFor(48000.0, 48000.0), audio, kMaxFrames);

    // Build the words by hand so one can be removed.
    rig.src.begin(g_levels, kChunkSamples, spuFor(48000.0, 48000.0));
    rig.sm.load(g_prog, g_progN);
    rig.sm.configure(true, 32, true);
    rig.rx.reset();

    int nw = 0;
    for (int f = 0; f < 400; ++f) {
        int32_t l, r; sine(f, l, r);
        rig.src.frame(l, r, f % 192);
        if (rig.src.n > kChunkSamples - 8 * 128 * kUiCycles || f == 399) {
            for (int i = 0; i < rig.src.n; ++i) {
                uint32_t w;
                if (rig.sm.cycle(g_levels[i], &w) && nw < kMaxWords)
                    g_words[nw++] = w;
            }
            rig.src.rebase();
        }
    }

    // Drop a word a third of the way in, and one bit out of another - two
    // different faults, because an odd shift breaks the cell structure and an
    // even one does not.
    const int drop = nw / 3;
    for (int i = drop; i + 1 < nw; ++i) g_words[i] = g_words[i + 1];
    --nw;

    int frames = 0;
    for (int i = 0; i < nw; i += 256) {
        const int n = (nw - i < 256) ? (nw - i) : 256;
        frames += rig.rx.process(&g_words[i], n, &audio[frames * 2],
                                 kMaxFrames - frames);
    }

    checkf(frames > 340, "recovers from a dropped word: %.0f of ~400 frames%.0s",
           (double)frames, 0.0);
    checkf(rig.rx.resyncs() >= 1 && rig.rx.resyncs() <= 4,
           "  .. and says so: %.0f resyncs (1..4)%.0s",
           (double)rig.rx.resyncs(), 0.0);
}

// ---------------------------------------------------------------------------
//  8. Noise must not look like audio
// ---------------------------------------------------------------------------

void test_noise()
{
    printf("\n-- random levels must not frame --\n");

    PioSim sm;
    sm.load(g_prog, g_progN);
    sm.configure(true, 32, true);

    SpdifRxUi rx;
    static int32_t audio[4096 * 2];

    uint32_t rng = 987654321u;
    int      nw  = 0;
    int      frames = 0;

    // Levels that change at random, at roughly UI rate - noise with the right
    // spectrum to be mistaken for a carrier, which white-per-cycle noise is
    // not.
    int level = 0, hold = 0;
    for (int c = 0; c < 4 * 1000 * 1000; ++c) {
        if (hold-- <= 0) {
            rng = rng * 1664525u + 1013904223u;
            hold  = kUiCycles * (1 + (int)((rng >> 16) % 3u));
            level ^= 1;
        }
        uint32_t w;
        if (sm.cycle(level, &w)) {
            g_words[nw++] = w;
            if (nw == 256) {
                frames += rx.process(g_words, nw, &audio[0], 4096);
                nw = 0;
            }
        }
    }

    // A false lock is possible - a preamble byte plus 28 cell boundaries plus
    // parity can happen by chance - but it must be rare and must not hold.
    checkf(frames < 40, "random levels produce almost no frames: %.0f%.0s",
           (double)frames, 0.0);
    check(sm.unsupported() == 0, "  .. still no unsupported instructions");
}

// ---------------------------------------------------------------------------
//  9. What it costs, stated in the unit the decision was made in
// ---------------------------------------------------------------------------

void report_word_rate()
{
    printf("\n-- word rate, which is the number the redesign was for --\n");

    struct { double fs; const char *name; } rates[] = {
        { 48000.0, "48000" }, { 44100.0, "44100" }, { 32000.0, "32000" },
    };

    for (unsigned i = 0; i < 3; ++i) {
        const double ui    = rates[i].fs * 128.0;
        const double words = ui / 32.0;
        printf("     %s Hz : %8.0f UI/s -> %7.0f words/s   "
               "(oversampled front end: %.0f)\n",
               rates[i].name, ui, words, rates[i].fs * 128.0 * 4.0 / 32.0);
    }

    printf("\n     The figure that decides this is cycles per word on the\n"
           "     board, and it is not knowable here. What is knowable is that\n"
           "     there are a quarter as many words and that decoding one is\n"
           "     straight-line bit arithmetic with no per-run loop in it.\n");
}

} // namespace

int main()
{
    printf("test_spdif_ui - the per-UI front end and its decoder\n");

    if (!assemble_program()) {
        printf("\nCANNOT ASSEMBLE ../spdif_rx_ui.pio - not running.\n");
        return 2;
    }
    printf("  assembled %d instructions from ../spdif_rx_ui.pio\n", g_progN);

    test_program();
    test_rates();
    test_silence();
    test_block_position();
    test_jitter();
    test_rate_window();
    test_carrier();
    test_dropped_ui();
    test_noise();
    report_word_rate();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
