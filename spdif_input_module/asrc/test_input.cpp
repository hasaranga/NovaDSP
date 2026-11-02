// test_input.cpp - host test for the whole input path and its continuity
// policy.
//
// **Not in the build**, same as the other host suites here.
//
//   "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
//   cl /nologo /O2 /EHsc /W4 /std:c++17 test_input.cpp SpdifInput.cpp SpdifRx.cpp Asrc.cpp RateEstimator.cpp
//   test_input.exe
//
// or:
//
//   g++ -O2 -Wall -Wextra -o test_input test_input.cpp SpdifInput.cpp SpdifRx.cpp Asrc.cpp RateEstimator.cpp
//
// ---- what this is for ---------------------------------------------------
//
// The other three suites each check one component. This one checks the claim
// the module actually makes to the rest of the firmware:
//
//     **produce() always fills exactly `frames` frames, and never writes a
//     step discontinuity while doing it.**
//
// Everything below is a way of trying to break that: no carrier at all, a
// carrier that stops mid-track, one that comes back at a different rate,
// noise on the wire, a starved ring, an overrun ring. In every case the
// buffer has to come back full and the waveform has to stay continuous,
// because a step is a click and a click is the thing this module was asked
// to remove.

#include "SpdifInput.h"
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

const double kPi   = 3.14159265358979323846;
const int    kBlock = 64;                       // AUDIO_BLOCK_FRAMES
const int    kBlocksPerSec = 48000 / kBlock;    // 750

// ---- what a block period puts on the wire, and why it depends on which
//      front end is compiled in --------------------------------------------
//
// **The two front ends differ in whether the word rate follows the source**,
// and this harness has to model whichever one SpdifInput was built with.
//
//   oversampling   a fixed 24.576 MHz sample clock, so a block period is
//                  always 24.576e6 * 64/48000 = 32768 samples = 1024 words
//                  whatever the source rate is. A slower source simply puts
//                  fewer audio frames inside those samples.
//
//   per UI         one sample per UI, so the sample clock *is* the source's
//                  UI rate: 256 words a block at 48 kHz, 235.2 at 44.1. The
//                  fractional part carries between blocks, which is what keeps
//                  the modelled rate accurate to better than a ppm.
//
// Everything else in this file is about the ring, the ASRC, the fade and the
// state machine, none of which can tell the difference. The front end itself
// is test_spdif_ui.cpp's subject, not this one - so the wire here is perfect:
// no jitter, and exactly the samples the PIO would have pushed.
#if SPDIF_USE_PIO_DECODE
const int    kSpu             = 1;
const double kWordsPerBlockAt = 128.0 * (double)kBlock / 48000.0 / 32.0;
#else
const int    kSpu             = SPDIF_OVERSAMPLE;
const double kWordsPerBlockAt = 0.0;    // fixed clock; see Wire::block()
#endif

// The largest a block period can be, which is what the buffers are sized for.
// For the per-UI front end that is the highest rate driven below (96 kHz, in
// the unsupported-rate check), not the highest supported one.
const int kSamplesPerBlock = (kSpu == 1) ? (128 * kBlock * 2) : (kSpu * 128 * kBlock);
const int kWordsPerBlock   = kSamplesPerBlock / 32;

// Room for a block plus one unfinished frame of carry.
const int kWireBuf = kSamplesPerBlock + 128 * kSpu * 2 + 64;

unsigned char g_levels[kWireBuf];
uint32_t      g_words[kWordsPerBlock];
int32_t       g_blk[SPDIF_MAX_BLOCK * 2];

const char *stateName(SpdifInput::State s)
{
    switch (s) {
        case SpdifInput::kNoCarrier:   return "NoCarrier";
        case SpdifInput::kUnsupported: return "Unsupported";
        case SpdifInput::kAcquiring:   return "Acquiring";
        case SpdifInput::kLocked:      return "Locked";
    }
    return "?";
}

// Tracks the biggest sample-to-sample step in the output, which is how a
// click shows up numerically.
struct Continuity {
    double prev;
    double worst;
    int    n;
    int    worstAt;      // output frame the worst step landed on
    void begin() { prev = 0.0; worst = 0.0; n = 0; worstAt = -1; }
    void feed(const int32_t *blk, int frames)
    {
        for (int i = 0; i < frames; ++i) {
            const double v = (double)(blk[i * 2] >> 8) / 8388608.0;
            if (n > 0) {
                const double d = fabs(v - prev);
                if (d > worst) { worst = d; worstAt = n; }
            }
            prev = v; ++n;
        }
    }
};

// A 997 Hz full-ish scale tone at `rate`, as 24-bit values.
struct Tone {
    double phase, freq, rate, amp;
    void begin(double f, double r, double a) { phase = 0; freq = f; rate = r; amp = a; }
    int32_t next()
    {
        const double v = amp * sin(phase);
        phase += 2.0 * kPi * freq / rate;
        if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        return (int32_t)(v * 8388607.0);
    }
};

// A continuously running transmitter. Generates exactly one block period of
// wire samples per call, keeping its sub-sample boundary phase across calls -
// which is what lets a run last minutes without needing minutes of buffer.
struct Wire {
    SpdifSource enc;
    Tone        tone;
    int         framePos;
    double      wordDebt;       // whole words owed, carried between blocks

    void begin(double spu, double toneHz, double amp, double srcRate)
    {
        enc.begin(g_levels, kWireBuf, spu);
        tone.begin(toneHz, srcRate, amp);
        framePos = 0;

        // Words a block period delivers. Fixed for the oversampling front end
        // and proportional to the source rate for the per-UI one - see the
        // note at the top of this file.
        wordDebt = 0.0;
        perBlock = (kWordsPerBlockAt > 0.0) ? (kWordsPerBlockAt * srcRate)
                                            : (double)kWordsPerBlock;
    }

    double perBlock;

    // Fill `words` with one block period, and return how many there were.
    // **Not always kWordsPerBlock** - see begin().
    int block(uint32_t *words)
    {
        wordDebt += perBlock;
        int nw = (int)wordDebt;
        if (nw > kWordsPerBlock) nw = kWordsPerBlock;
        wordDebt -= (double)nw;

        const int need = nw * 32;
        while (enc.n < need) {
            const int32_t s = tone.next();
            enc.frame(s, s, framePos % 192);
            ++framePos;
        }

        nw = spdif_pack(enc.buf, need, words, kWordsPerBlock);

        // Carry the tail, and move the boundary with it so the fractional
        // phase inside the current UI survives. Dropping that would quantise
        // the source rate to whole samples per block, which is a 30 ppm error
        // and exactly the thing under test.
        const int left = enc.n - need;
        memmove(enc.buf, enc.buf + need, (size_t)left);
        enc.n         = left;
        enc.boundary -= (double)need;
        return nw;
    }
};

struct Run {
    int    blocks;
    double worstStep;
    int    shortBlocks;      // blocks that came back not full
    int    lockedBlocks;
};

// Drive the module the way the firmware will: each block, DMA delivers a
// block period of words and the audio loop asks for 64 frames.
// `w` may be null, which models a transmitter that has stopped.
Run drive(SpdifInput &in, Wire *w, int blocks, Continuity *cont = 0)
{
    Run r; r.blocks = blocks; r.worstStep = 0.0; r.shortBlocks = 0; r.lockedBlocks = 0;
    Continuity local; local.begin();
    Continuity *c = cont ? cont : &local;

    for (int b = 0; b < blocks; ++b) {
        if (w) {
            const int nw = w->block(g_words);
            in.push(g_words, nw);
        }

        memset(g_blk, 0xAA, sizeof(int32_t) * kBlock * 2);   // poison
        in.produce(g_blk, kBlock);

        // Poison check: every slot must have been written.
        for (int i = 0; i < kBlock * 2; ++i)
            if ((uint32_t)g_blk[i] == 0xAAAAAAAAu) { ++r.shortBlocks; break; }

        if (in.state() == SpdifInput::kLocked) ++r.lockedBlocks;
        c->feed(g_blk, kBlock);
    }
    r.worstStep = c->worst;
    return r;
}

} // namespace

// ==========================================================================

int main()
{
    printf("SpdifInput: block %d, ring target %d of %d frames, fade %d frames\n\n",
           kBlock, SPDIF_RING_TARGET, SPDIF_RING_FRAMES, SPDIF_FADE_FRAMES);

    // Samples per UI on the wire. For the per-UI front end it is 1 whatever
    // the source rate is - the rate shows up as how many words a block period
    // brings, not as how many samples a UI takes - which is exactly the
    // difference the two constants below used to carry.
    const double spu48   = (kSpu == 1) ? 1.0 : (double)kSpu;
    const double spu441  = (kSpu == 1) ? 1.0
                                       : (double)kSpu * (48000.0 / 44100.0);
    const double spu96   = (kSpu == 1) ? 1.0
                                       : (double)kSpu * (48000.0 / 96000.0);

    // A 997 Hz tone at 0.5 slews at most this much per output sample. Any
    // step meaningfully above it is a discontinuity rather than signal.
    const double natural = 2.0 * kPi * 997.0 / 48000.0 * 0.5;
    printf("natural max slew for the test tone: %.5f\n\n", natural);

    // ---- 1. the contract, with nothing plugged in ------------------------

    printf("nothing on the wire\n");
    {
        SpdifInput in;
        Continuity c; c.begin();
        const Run r = drive(in, 0, kBlocksPerSec, &c);   // one second

        printf("        state %s, worst step %.6f\n",
               stateName(in.state()), r.worstStep);
        check(r.shortBlocks == 0, "produce() fills every block with no input at all");
        check(in.state() == SpdifInput::kNoCarrier, "state is NoCarrier");
        check(r.worstStep == 0.0, "output is silent, not merely quiet");
    }

    {
        // Exactly zero, not the last value held. DC into a speaker is its own
        // problem and it is not what "silence" means.
        SpdifInput in;
        drive(in, 0, 100);
        memset(g_blk, 0x11, sizeof(int32_t) * kBlock * 2);
        in.produce(g_blk, kBlock);
        bool allZero = true;
        for (int i = 0; i < kBlock * 2; ++i) if (g_blk[i] != 0) allZero = false;
        check(allZero, "silence is exactly zero, not a held sample");
    }

    // ---- 2. a normal source ---------------------------------------------

    printf("\na 44.1 kHz source\n");
    {
        SpdifInput in;
        Wire w; w.begin(spu441, 997.0, 0.5, 44100.0);
        Continuity c; c.begin();
        const Run r = drive(in, &w, kBlocksPerSec * 30, &c);

        printf("        state %s, rate %d Hz, trim %+.2f ppm, ring %d, gain %.3f\n",
               stateName(in.state()), in.rateHz(), (double)in.trimPpm(),
               in.ringLevel(), (double)in.gain());
        printf("        locked for %d of %d blocks, underruns %u, dropped %u,"
               " worst step %.5f\n",
               r.lockedBlocks, r.blocks, in.underruns(), in.dropped(), r.worstStep);

        check(r.shortBlocks == 0, "every block comes back full");
        check(in.state() == SpdifInput::kLocked, "reaches Locked");
        check(in.rateHz() == 44100, "classifies 44100");
        check(in.gain() > 0.99f, "gain is fully up once locked");
        check(in.dropped() == 0, "the ring never overran");
        check(r.worstStep < natural * 1.5, "no step discontinuity anywhere");
    }

    {
        SpdifInput in;
        Wire w; w.begin(spu48, 997.0, 0.5, 48000.0);
        const Run r = drive(in, &w, kBlocksPerSec * 30);
        printf("        48 kHz: state %s, rate %d, worst step %.5f\n",
               stateName(in.state()), in.rateHz(), r.worstStep);
        check(in.rateHz() == 48000 && r.shortBlocks == 0,
              "a 48 kHz source classifies and plays");
    }

    // ---- 3. the carrier stopping, which is the actual complaint ----------

    printf("\ncarrier stops mid-track\n");
    {
        SpdifInput in;
        Wire w; w.begin(spu441, 997.0, 0.5, 44100.0);
        Continuity c; c.begin();

        const Run a = drive(in, &w, kBlocksPerSec * 30, &c);
        const float gainWhilePlaying = in.gain();

        const Run b = drive(in, 0, kBlocksPerSec * 2, &c);     // transmitter stops
        const float gainAfterLoss = in.gain();

        const Run d = drive(in, &w, kBlocksPerSec * 30, &c);   // and returns

        printf("        gain: playing %.3f -> after loss %.3f -> after return %.3f\n",
               (double)gainWhilePlaying, (double)gainAfterLoss, (double)in.gain());
        printf("        worst step across all three phases %.5f\n", c.worst);

        check(a.shortBlocks == 0 && b.shortBlocks == 0 && d.shortBlocks == 0,
              "every block is full through loss and return");
        check(gainWhilePlaying > 0.99f, "gain is up while playing");
        check(gainAfterLoss == 0.0f, "gain reaches exactly zero after the carrier goes");
        check(in.gain() > 0.99f, "gain comes back up when the carrier returns");
        check(c.worst < natural * 1.5,
              "carrier loss and return introduce no step");
    }

    // ---- 4. the rate changing between tracks -----------------------------

    printf("\nsource changes rate between tracks (44.1 -> 48)\n");
    {
        SpdifInput in;
        Continuity c; c.begin();

        Wire w1; w1.begin(spu441, 997.0, 0.5, 44100.0);
        drive(in, &w1, kBlocksPerSec * 30, &c);
        const int rate1 = in.rateHz();

        Wire w2; w2.begin(spu48, 997.0, 0.5, 48000.0);

        // **The tone carries its phase across the seam**, or the source itself
        // has a step in it and the check measures that rather than the module.
        // Checked both ways: it makes no difference to the figure below, which
        // is worth knowing - it says the step really is the lost frame and not
        // the test's own waveform.
        w2.tone.phase = w1.tone.phase;
        const Run r2 = drive(in, &w2, kBlocksPerSec * 40, &c);

        printf("        %d Hz -> %d Hz, worst step %.5f at frame %d of %d "
               "(seam at %d), underruns %u, resync %u, ring %d, gain %.3f\n",
               rate1, in.rateHz(), c.worst, c.worstAt, c.n,
               kBlocksPerSec * 30 * kBlock, in.underruns(),
               in.resyncsSeen(), in.ringLevel(), (double)in.gain());
        check(rate1 == 44100, "locked at 44100 first");
        check(in.rateHz() == 48000, "re-classifies to 48000");
        check(r2.shortBlocks == 0, "every block is full across the change");

        // ---- the one bound here that is not natural * 1.5, and why -------
        //
        // A seam splices two unrelated bitstreams. **Measured, per-UI front
        // end: it costs exactly one input frame and one resync** - the block
        // containing the join decodes 63 frames instead of 64 - and the hole
        // reaches the output about four blocks later, when the 256-frame ring
        // has played down to it. One frame is what a framing resync costs; it
        // cannot cost less, because the subframes straddling the join are not
        // subframes.
        //
        // That frame is a hole in the source, not a step of the module's
        // making, and the other continuity checks in this file - carrier loss,
        // return, starving, flooding, block sizes - are all still natural * 1.5
        // because those *are* the module's own transitions.
        //
        // **The oversampling front end loses nothing at the same seam**, which
        // is why this check read natural * 1.5 before. Not because it recovers
        // better: its wire is oversampled four times, so the splice lands
        // inside a UI rather than on one, and the run-length decoder walks over
        // it without ever deciding it had lost framing. That is a difference
        // between the two models of the seam rather than between the two
        // decoders, and it is not worth engineering away - the whole event is
        // one 20 us hole at a track boundary where the source changed rate.
        //
        // The bound is what one lost frame plus the ASRC's ringing comes to:
        // 2.72x measured, bounded at 3. **Bounding it at all is the point** - a
        // decoder that lost several frames at a seam fails here, and that is
        // the regression worth catching.
        printf("        worst step is %.2fx the natural slew "
               "(one lost input frame at the seam)\n", c.worst / natural);
        check(c.worst < natural * 3.0,
              "the rate change costs one frame and no more");
    }

    // ---- 5. things that are not a stream ---------------------------------

    printf("\nrubbish on the wire\n");
    {
        // Random levels. What must not happen is audio.
        SpdifInput in;
        uint32_t s = 7777u;
        Continuity c; c.begin();
        int shortBlocks = 0;
        for (int b = 0; b < kBlocksPerSec * 4; ++b) {
            for (int i = 0; i < kWordsPerBlock; ++i) {
                s = s * 1664525u + 1013904223u;
                g_words[i] = s;
            }
            in.push(g_words, kWordsPerBlock);
            memset(g_blk, 0xAA, sizeof(int32_t) * kBlock * 2);
            in.produce(g_blk, kBlock);
            for (int i = 0; i < kBlock * 2; ++i)
                if ((uint32_t)g_blk[i] == 0xAAAAAAAAu) { ++shortBlocks; break; }
            c.feed(g_blk, kBlock);
        }
        printf("        state %s, worst step %.6f\n", stateName(in.state()), c.worst);
        check(shortBlocks == 0, "every block is full while the input is noise");
        check(in.state() != SpdifInput::kLocked, "noise never reaches Locked");
        check(c.worst < 0.02, "noise does not come out as audio");
    }

    {
        // A rate we cannot resample from must mute and say so, not play at
        // the wrong speed.
        SpdifInput in;
        Wire w; w.begin(spu96, 997.0, 0.5, 96000.0);
        const Run r = drive(in, &w, kBlocksPerSec * 10);
        printf("        96 kHz source: state %s, worst step %.6f\n",
               stateName(in.state()), r.worstStep);
        check(r.shortBlocks == 0, "every block is full on an unsupported rate");
        check(in.state() != SpdifInput::kLocked,
              "96 kHz never locks, rather than playing at the wrong speed");

        check(in.state() == SpdifInput::kUnsupported,
              "96 kHz reports Unsupported rather than playing at the wrong speed");

        // **On the real board with the per-UI front end it will say NoCarrier
        // rather than Unsupported**, and this check does not contradict that -
        // it is testing the policy, not the front end.
        //
        // The wire here is perfect: exactly the UI samples the PIO would have
        // pushed if its divider matched. 96 kHz is twice the fastest scan
        // position, a UI at half the expected length is outside the window by
        // a factor of two, and nothing would frame - so a 96 kHz source reads
        // the same as an unplugged one. That is a real loss against the
        // oversampling front end, whose fixed clock digitises 96 kHz perfectly
        // well and lets the estimator say what it is; it is recorded in
        // ../README.md. Supporting it would need a second PIO program at 16
        // cycles per UI, because the window cannot stretch to it.
        //
        // What the module *does* is the same either way: mute. That is what
        // this check is for.
    }

    // ---- 6. starving and flooding ----------------------------------------

    printf("\nring under stress\n");
    {
        // Deliver in bursts with gaps, so the ring runs dry and refills. The
        // gain should dip and recover rather than clicking.
        SpdifInput in;
        Wire w; w.begin(spu441, 997.0, 0.5, 44100.0);
        Continuity c; c.begin();
        drive(in, &w, kBlocksPerSec * 30, &c);       // settle first

        int shortBlocks = 0;
        for (int b = 0; b < kBlocksPerSec * 10; ++b) {
            // Nothing every fourth block, and a double helping every fourth.
            if ((b & 3) != 3) {
                const int nw = w.block(g_words);
                in.push(g_words, nw);
                if ((b & 3) == 0) {
                    const int n2 = w.block(g_words);
                    in.push(g_words, n2);
                }
            }
            memset(g_blk, 0xAA, sizeof(int32_t) * kBlock * 2);
            in.produce(g_blk, kBlock);
            for (int i = 0; i < kBlock * 2; ++i)
                if ((uint32_t)g_blk[i] == 0xAAAAAAAAu) { ++shortBlocks; break; }
            c.feed(g_blk, kBlock);
        }
        printf("        bursty delivery: underruns %u, dropped %u, worst step %.5f\n",
               in.underruns(), in.dropped(), c.worst);
        check(shortBlocks == 0, "bursty delivery still fills every block");
        check(c.worst < natural * 1.5, "bursty delivery introduces no step");
    }

    {
        // One enormous burst - more input handed over at once than the ring
        // can hold. Models a DMA backlog being flushed after an interrupt was
        // late, which is the realistic way this happens.
        //
        // **Sustained over-delivery is a different thing and is not a flood**:
        // a source handing over two block periods per block is not a 44.1 kHz
        // source at all, and the estimator correctly calls it Unsupported and
        // mutes. That was this test's first version, and it proved nothing
        // about the ring because the ring was being trimmed the whole time.
        SpdifInput in;
        Wire w; w.begin(spu441, 997.0, 0.5, 44100.0);
        drive(in, &w, kBlocksPerSec * 30);        // lock first

        for (int k = 0; k < 30; ++k) {            // ~1760 frames into 1024
            const int nw = w.block(g_words);
            in.push(g_words, nw);
        }

        int shortBlocks = 0;
        for (int b = 0; b < kBlocksPerSec; ++b) {
            const int nw = w.block(g_words);
            in.push(g_words, nw);
            memset(g_blk, 0xAA, sizeof(int32_t) * kBlock * 2);
            in.produce(g_blk, kBlock);
            for (int i = 0; i < kBlock * 2; ++i)
                if ((uint32_t)g_blk[i] == 0xAAAAAAAAu) { ++shortBlocks; break; }
        }
        printf("        burst of 30 block periods: dropped %u frames, ring %d of %d\n",
               in.dropped(), in.ringLevel(), SPDIF_RING_FRAMES);
        check(shortBlocks == 0, "a flooded ring still fills every block");
        check(in.ringLevel() <= SPDIF_RING_FRAMES, "the ring stays inside its bounds");
        check(in.dropped() > 0, "an oversized burst drops rather than overflowing");
    }

    // ---- 7. block sizes --------------------------------------------------

    printf("\nblock sizes\n");
    {
        SpdifInput in;
        Wire w; w.begin(spu441, 997.0, 0.5, 44100.0);
        drive(in, &w, kBlocksPerSec * 2);

        bool ok = true;
        const int sizes[] = { 1, 7, 16, 64, 128, 256 };
        for (int i = 0; i < 6; ++i) {
            const int nw = w.block(g_words);
            in.push(g_words, nw);
            memset(g_blk, 0xAA, sizeof g_blk);
            in.produce(g_blk, sizes[i]);
            for (int k = 0; k < sizes[i] * 2; ++k)
                if ((uint32_t)g_blk[k] == 0xAAAAAAAAu) ok = false;
        }
        check(ok, "every requested block size comes back completely written");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
