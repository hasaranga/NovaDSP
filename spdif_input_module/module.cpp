/*
 * spdif_input_module - glue between the core's block loop and the receiver.
 *
 * Everything with any judgement in it lives in asrc/: SpdifRx decodes,
 * RateEstimator works out what rate the source is running at and trims the
 * ratio, Asrc resamples onto the local grid, and SpdifInput ties the three
 * together behind one call that always fills the block. This file is the PIO
 * and DMA plumbing and nothing else.
 *
 * ---- the pin, and why it is where it is ---------------------------------
 *
 * GP1. The free pins on this board are GP0, GP1, GP7-GP12, GP21, GP22 and
 * GP28, and the one thing that matters is distance from **GP13**, which
 * carries the PCM1808's 12.288 MHz MCLK. A 24.576 MHz edge-rich signal
 * running alongside an ADC's system clock is how jitter gets into a
 * conversion, and it would be looked for in the DSP for a week. That is the
 * same argument that put the ST7789 on GP2/GP3 rather than GP10/GP11.
 *
 * GP0/GP1 are also the furthest from the ST7789's SPI block. Not a measured
 * problem; a placement that costs nothing and removes a question.
 *
 * **Coax needs a front end** - a transformer and a comparator - because
 * 0.5 Vpp into a GPIO is not reliable even with the pad's Schmitt trigger. A
 * TOSLINK receiver module outputs logic level and needs none of that, which
 * is why optical is the easy way to bring this up first.
 *
 * ---- PIO2, and the DMA ring ---------------------------------------------
 *
 * PIO0 is the I2S and PIO1 the encoder, whose quadrature program declares
 * `.origin 0` and takes 29 of 32 instructions. RP2350 has a third block and
 * nothing else wants it.
 *
 * The state machine pushes one word per 32 samples and a DMA channel drains
 * the FIFO into a wrapping ring. fill() then works out how far the write
 * pointer has moved and hands the new words to the decoder. **Nothing here
 * can block**, which is the property that matters on core 0: an absent source
 * costs exactly what a present one does.
 */

#include "module.h"
#include "audio.h"
#include "cycles.h"

#include "hardware/clocks.h"

#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"

// SPDIF_USE_PIO_DECODE lives in asrc/SpdifInput.h and selects both halves -
// the front end here and the decoder behind it - so the two cannot be
// mismatched. See that header for what the switch is for.
#include "asrc/SpdifInput.h"

#if SPDIF_USE_PIO_DECODE
#include "spdif_rx_ui.pio.h"
#else
#include "spdif_rx.pio.h"
#endif

// ---- pins and sizing ----------------------------------------------------

#define SPDIF_PIN_RX  1u

// Words of DMA ring. One block period is
//
//     per-UI        128 * AUDIO_BLOCK_FRAMES / 32              =  256 words
//     oversampling  OVERSAMPLE * 128 * AUDIO_BLOCK_FRAMES / 32 = 1024 words
//
// so 4096 is four block periods of slack for the oversampling front end and
// sixteen for the per-UI one. It must be a power of two and 4096 words is
// 16 KB, which the DMA ring wrap needs aligned to its own size.
//
// **Not shrunk with the word rate**, although it could be: the slack is worth
// most during the divider scan below, which is the one time fill() is looking
// at words it cannot frame, and 8 KB of SRAM is not the constraint here.
#define SPDIF_DMA_WORDS 4096u

static uint32_t s_ring[SPDIF_DMA_WORDS]
    __attribute__((aligned(SPDIF_DMA_WORDS * sizeof(uint32_t))));

// One block period of words, and one second of them. Derived from the front
// end in use rather than written down, because both change by a factor of four
// with SPDIF_USE_PIO_DECODE and a stale constant here would misreport the one
// figure this module's future turns on.
//
// **These belong out here rather than with the diagnostic**, because the clamp
// in fill() that uses them is a fix and not telemetry - putting them behind
// NOVA_TELEMETRY compiled in dev and failed only in dist, which is the exact
// asymmetry nova_build.h describes.
#if SPDIF_USE_PIO_DECODE
#define SPDIF_SAMPLES_PER_UI 1u
#else
#define SPDIF_SAMPLES_PER_UI SPDIF_OVERSAMPLE
#endif

#define SPDIF_WORDS_PER_BLOCK \
    (SPDIF_SAMPLES_PER_UI * 128u * AUDIO_BLOCK_FRAMES / 32u)
#define SPDIF_WORDS_REQUIRED_PER_S \
    (SPDIF_SAMPLES_PER_UI * 128u * 48000u / 32u)

// Outstanding words past which the backlog is dropped rather than decoded.
//
// **This has to be tight enough to escape, not merely tight enough to bound.**
// Measured on the board with the oversampling front end: the decode cost ~229
// cycles a word, so one block period of words (1024) was ~763 us of the 1333 us
// deadline. A threshold of three block periods bounded the work and still never
// recovered - clamping to 1024 words left 2.8 ms of decoding, during which 2189
// more words arrived, which is *under* 3072, so the next call did not clamp and
// the spiral resumed. It oscillated around the threshold at 387 % of deadline.
//
// The general condition is the one that matters, and it is easy to state.
// Clamping to B words costs B.c cycles, during which B.c.(W/F) more arrive,
// where W is the word rate and F is clk_sys. The backlog drains iff
//
//     c < F / W
//
// which is 400 cycles a word for the oversampling front end at 768000 words a
// second, and **1600 for the per-UI one at 192000**. The old decoder measured
// 292 - inside the bound, but only just, and with nothing left for the effects
// or the FFT. The new one is straight-line arithmetic on a quarter as many
// words, so the escape condition stops being the thing that decides the design.
//
// Two block periods, rather than the one and a half the old front end needed:
// the worst call is then 512 words of a kind that are much cheaper, and the
// extra slack is for the divider scan below.
#if SPDIF_USE_PIO_DECODE
#define SPDIF_LAP_THRESHOLD (SPDIF_WORDS_PER_BLOCK * 2u)
#else
#define SPDIF_LAP_THRESHOLD (SPDIF_WORDS_PER_BLOCK + SPDIF_WORDS_PER_BLOCK / 2u)
#endif

// Hard ceiling on words decoded in one call, for bisecting. Normally the same
// as the threshold above, so it changes nothing; set it small to make fill()
// nearly free while the PIO and DMA keep running exactly as they were. That
// separates "the decode load is starving something" from "the module's mere
// presence breaks it", which no amount of reading the code distinguishes.
//
// **What it has already found: core 0 overrunning this deadline stops core 1
// dead.** With the decode at 130 % of deadline the panel froze inside its very
// first I2C flush - core1_ticks stuck at 1, wedged in the page-0 data write,
// with no I2C error recorded - and the board read as "display never came up".
// Throttled to 64 words a call, fill() drops to 5.8 %, ovr to 0, and core 1
// runs normally. Same module, same PIO, same DMA, same panel.
//
// So a frozen display in an S/PDIF build is **not a display fault** and there
// is nothing to look for in the OLED module. It is this number being too big.
//
// **And it does not stop at a frozen panel - it takes the whole box down.**
// Bisected on hardware, one variable at a time:
//
//   oled + i2s in/out                 runs indefinitely
//   oled + spdif, no idle timeout     runs indefinitely
//   no display + spdif                runs indefinitely
//   oled + spdif, idle reached        dies after exactly 10 s - both cores
//                                     stop, the panel goes dark, the LED
//                                     freezes wherever it was, and picotool
//                                     can no longer reset the board
//   the same, decode throttled to 64  spectrum page draws, nothing dies
//
// Ten seconds is IDLE_MS in oled_display_module/ui.c, when the panel switches
// to the spectrum. A menu page is nearly static, so the shadow diff sends a
// handful of bytes; the spectrum changes almost every pixel every frame and
// flushes the whole kilobyte - about 23 ms of I2C at 400 kHz, the heaviest
// I2C load this firmware can produce. Core 1 is already starved by core 0
// overrunning the block deadline, and that is where it stops for good.
//
// The lesson for anything added here later: **the display is the thing that
// fails first when core 0 runs over, and it fails in a way that looks like a
// display bug.** Read fill() against the deadline before believing the panel.
//
// Note it must be set here rather than passed on the command line:
// make_firmware.bat hands -D straight to *CMake*, where it becomes a cache
// variable and not a preprocessor define, so -DSPDIF_MAX_WORDS_PER_CALL=64
// compiles cleanly and changes nothing at all. That cost a build to notice.
#ifndef SPDIF_MAX_WORDS_PER_CALL
#define SPDIF_MAX_WORDS_PER_CALL SPDIF_LAP_THRESHOLD
#endif

// The same ceiling while the decoder has not framed yet.
//
// **The oversampling decoder needed this and the per-UI one does not**, which
// is worth saying rather than just deleting. That decoder learnt thresholds on
// every run until it had them, so acquisition was several times the cost of
// steady state - and because it could only leave acquisition by being fast and
// could only be fast once it had left, the whole input was bistable: boot into
// the bad basin and it never came out, with core 0 pinned and the panel dead.
// A quarter of a block period was how that transient was kept inside the
// deadline.
//
// The per-UI decoder learns nothing. Hunting costs one early-reject per UI,
// which is a few times a framed word and not a few hundred, so it is bounded
// by the same clamp as everything else and there is no second basin to fall
// into.
#ifndef SPDIF_ACQUIRE_WORDS_PER_CALL
#if SPDIF_USE_PIO_DECODE
#define SPDIF_ACQUIRE_WORDS_PER_CALL SPDIF_MAX_WORDS_PER_CALL
#else
#define SPDIF_ACQUIRE_WORDS_PER_CALL (SPDIF_WORDS_PER_BLOCK / 4u)
#endif
#endif

#if SPDIF_USE_PIO_DECODE
// ---- the divider scan ---------------------------------------------------
//
// The per-UI front end has to be told roughly what rate to expect: its sample
// clock is cycles per UI, and a UI is a property of the source. The window
// either side of a position is wide - 0.881x to 1.170x, derived and swept in
// spdif_rx_ui.pio - but 32 kHz is half an octave from the other two and does
// not fit in one with them.
//
// So the module scans. There is one position per supported rate and it steps
// to the next whenever the decoder has not framed for a while, which costs
// nothing when a source is present and does nothing visible when one is not.
//
// **`framed()` is the right signal and `carrier()` is not.** A front end on
// the wrong position still produces words at a steady rate off a live pin; it
// is the 28 cell boundaries in a row that cannot happen, which is exactly what
// framing means here.
static const uint32_t kScanHz[] = SPDIF_UI_SCAN_HZ;

// Blocks without framing before the next position is tried. 150 blocks is
// 200 ms, comfortably longer than the decoder needs to frame on a position
// that is right - one subframe - and short enough that the worst case, three
// positions with the last one correct, is under two thirds of a second.
#define SPDIF_SCAN_BLOCKS 150u

// Resyncs a position may accumulate over one window and still be kept. A
// healthy lock has none at all; the board measured 360 a window on a position
// that was decoding 96 % of its frames and classifying nothing.
#define SPDIF_SCAN_MAX_RESYNC 8u

// **`framed()` on its own is not the condition, and the board is what said
// so.** On the first hardware run of this front end the scan sat on 48000
// against a 44100 source and never moved. The source was only 4 % from the
// edge of that position's window, so the decoder re-framed constantly rather
// than failing outright: it reported `framed`, decoded 42286 frames a second
// of wreckage, and climbed 1800 resyncs a second while the rate estimator
// never classified anything. **Every counter except the resync rate said it
// was working.**
//
// The first fix was per-block - framed, and no resync in this block - and it
// did not work either, because the resyncs are *bursty*. Plenty of individual
// blocks are quiet, each one reset the timer, and 150 consecutive bad blocks
// never happened.
//
// So the verdict is taken over the whole window instead: at the end of
// SPDIF_SCAN_BLOCKS, step unless the decoder is framed **and** collected no
// more than SPDIF_SCAN_MAX_RESYNC over those 200 ms. A fixed window with one
// decision at the end cannot be defeated by burstiness, which is the property
// the per-block version was missing.
static uint32_t s_scanIdx;
static uint32_t s_scanBlocks;
static uint32_t s_scanResync;
#endif

static SpdifInput s_in;

static PIO      s_pio;
static uint     s_sm;
static int      s_dma = -1;
static uint32_t s_read;          // words already handed to the decoder
static bool     s_running;

// Whether s_read has been aligned to the DMA yet. See the note in fill().
static bool     s_primed;

// ---- bring-up diagnostic ------------------------------------------------
//
// **On while this module has never been on hardware.** Set to 0 once the
// input is trusted; it costs nothing in a dist build either way, because
// NOVA_TELEMETRY is what actually gates it.
//
// The one question it exists to answer is the one no other counter in the
// firmware can: **is the pin moving at all.** A flat `in` meter is produced
// identically by no light in the fibre, a dead receiver module, a PIO claim
// that failed, a decoder that never found a threshold, and a rate the ASRC
// declined - and the fix is different for every one of them.
//
// So the first half of the line is measured off the raw DMA words, *before*
// the decoder sees them, and does not depend on any of the decode being
// right. Transitions per second is the number to read:
//
//     ~3.1-6.1 M/s, avg ~4.6 M     a real 48 kHz S/PDIF carrier. Biphase puts
//                                  0.5-1.0 transitions in every UI and there
//                                  are 128 UI per frame, so 6.144 M UI/s
//     0, levels "stuck hi/lo"      nothing is arriving. Fibre, receiver power,
//                                  the source, or the wrong pin - not the
//                                  decoder, which is not being given anything
//     low but non-zero             noise on a floating pin, or a front end
//                                  that is not resolving edges
//
// `words/s` should read SPDIF_WORDS_REQUIRED_PER_S - **192,000 with the per-UI
// front end, 768,000 with the oversampling one**, and the report prints the
// figure it is expecting rather than leaving it to be remembered. Well under
// that means fill() is not keeping up with the ring rather than anything about
// the signal.
//
// With the per-UI front end the word rate follows the *source*, so 176,400 at
// 44.1 kHz and 128,000 at 32 kHz are correct rather than a shortfall. The
// scan position on the same line is what says which to expect.
//
// Cost is ~3 % of one core, almost all of it the popcount, which ARMv8-M has
// no instruction for. That is affordable against an 18 % DSP load and it is
// why this is a switch rather than something left in.
#ifndef SPDIF_RX_DIAG
#define SPDIF_RX_DIAG 1
#endif

#if NOVA_TELEMETRY && SPDIF_RX_DIAG

// The raw-pin half of the diagnostic - transitions per second and stuck-level
// detection - costs a popcount per word, and ARMv8-M has no instruction for
// one. **It was worth ~3 % of a core to answer "is anything arriving on GP1",
// and that question is answered.** Left here for the next time a front end is
// suspected; off because the decoder needs those cycles more than the counter
// does. The rest of the report - words, frames, cycles, laps - is a few adds
// and stays on unconditionally.
#ifndef SPDIF_RX_PIN_SCAN
#define SPDIF_RX_PIN_SCAN 0
#endif

static uint32_t s_diag_words;
static uint32_t s_diag_edges;
static uint32_t s_diag_frames;   // input frames the decoder produced
static uint32_t s_diag_or;       // every bit that was ever 1
static uint32_t s_diag_and;      // every bit that was ever 0, inverted
static uint32_t s_diag_blocks;

#if SPDIF_RX_PIN_SCAN
// Only the pin scan carries state from word to word; the other two counters
// are read by diag_report() whether the scan is on or not, which is why they
// are not under this guard.
static uint32_t s_diag_last;     // last sample of the previous word
#endif

// ---- what fill() costs, which is the number this module never had ----------
//
// `dsp` on the status line times effect_process_block() and nothing else, so
// **the most expensive code in an S/PDIF build is the only code with no figure
// against it.** That is how a decoder heavy enough to livelock core 0 read as
// "dsp 12.6 %" while `ovr` climbed 440 a second.
static uint32_t s_diag_cyc_sum;
static uint32_t s_diag_cyc_max;
static uint32_t s_diag_calls;

static uint32_t s_diag_laps;
static uint32_t s_diag_worst_outstanding;
static uint32_t s_diag_resync0;      // resyncs as of the last report

// One block period of fill() calls. 750 * 64 / 48000 is exactly one second,
// so every rate below is already per-second and needs no division.
#define SPDIF_DIAG_PERIOD 750u

static inline void diag_scan(const uint32_t *w, int n)
{
    s_diag_words += (uint32_t)n;

#if !SPDIF_RX_PIN_SCAN
    (void)w;
}
#else
    uint32_t edges = s_diag_edges, orv = s_diag_or, andv = s_diag_and;
    uint32_t last = s_diag_last;

    for (int i = 0; i < n; ++i) {
        const uint32_t v = w[i];
        orv  |= v;
        andv &= v;

        // Bit k of x is set when sample k and sample k+1 differ. Bit 31 has no
        // partner inside the word, so it is masked off and the transition
        // across the word boundary is added from the previous word's top bit.
        const uint32_t x = (v ^ (v >> 1)) & 0x7FFFFFFFu;
        edges += (uint32_t)__builtin_popcount(x) + ((v & 1u) ^ last);
        last = v >> 31;
    }

    s_diag_edges = edges;
    s_diag_or    = orv;
    s_diag_and   = andv;
    s_diag_last  = last;
}
#endif

static void diag_report(void)
{
    static const char *const kState[] = {
        "NO CARRIER", "UNSUPPORTED RATE", "acquiring", "LOCKED"
    };

    // Said first and plainly, because it is the failure that looks exactly
    // like every other one from the panel.
    if (!s_running) {
        NOVA_LOG("spdif: NOT RUNNING - PIO or DMA claim failed at attach()\n");
        return;
    }

    // **`levels` is only meaningful when the pin scan is on**, because that is
    // what fills s_diag_or / s_diag_and. With the scan off they stay at their
    // reset values and the line reads a confident "stuck LO" over a perfectly
    // live pin - which it did on the first hardware run of this front end,
    // next to a word rate that was tracking a real 44.1 kHz source to 300 ppm.
    // Say "?" rather than something false.
#if SPDIF_RX_PIN_SCAN
    const char *levels = (s_diag_or == 0)           ? "stuck LO"
                       : (s_diag_and == 0xFFFFFFFF) ? "stuck HI"
                                                    : "both";
#else
    const char *levels = "?";
#endif

#if SPDIF_USE_PIO_DECODE
    // The scan position is the first thing to read when a live pin will not
    // frame: a source at a rate no position covers looks identical from every
    // other counter here.
    NOVA_LOG("spdif: pin %lu edges/s, %lu words/s, levels %s | "
             "scan %u Hz, %sframed, %lu frames/s%s\n",
             (unsigned long)s_diag_edges, (unsigned long)s_diag_words, levels,
             (unsigned)kScanHz[s_scanIdx], s_in.framed() ? "" : "NOT ",
             (unsigned long)s_diag_frames,
             SPDIF_RX_PIN_SCAN ? "" : "   [pin scan off]");
#else
    NOVA_LOG("spdif: pin %lu edges/s, %lu words/s, levels %s | "
             "unit %.2f smp/UI, %lu frames/s%s\n",
             (unsigned long)s_diag_edges, (unsigned long)s_diag_words, levels,
             (double)s_in.unitSamples(), (unsigned long)s_diag_frames,
             SPDIF_RX_PIN_SCAN ? "" : "   [pin scan off]");
#endif

    // The two figures the bring-up actually turned on. Percentages are against
    // the same 1333 us block deadline `dsp` is measured against, so the two
    // lines can be added up by eye.
    const float cyc_us = (float)clock_get_hz(clk_sys) / 1e6f;
    const float budget = 1e6f * (float)AUDIO_BLOCK_FRAMES / (float)AUDIO_SAMPLE_RATE;
    const float avg_us = s_diag_calls
                       ? (float)s_diag_cyc_sum / (float)s_diag_calls / cyc_us : 0.0f;
    const float max_us = (float)s_diag_cyc_max / cyc_us;

    // Words per call and cycles per word, reported rather than inferred. The
    // second is the figure that decides whether this decoder can run in real
    // time here: it has to clear SPDIF_WORDS_REQUIRED_PER_S words a second, so
    // the ceiling is clk_sys divided by that - **400 cycles a word for the
    // oversampling front end and 1600 for the per-UI one** - and that is the
    // ceiling before the effects and the FFT take any of it.
    const float w_per_call = s_diag_calls
                           ? (float)s_diag_words / (float)s_diag_calls : 0.0f;
    const float cyc_per_w  = s_diag_words
                           ? (float)s_diag_cyc_sum / (float)s_diag_words : 0.0f;

    NOVA_LOG("       fill %.1f/%.1f us %.1f%% of deadline | %lu calls/s, "
             "%.0f words/call, %.0f cyc/word\n",
             (double)avg_us, (double)max_us, (double)(100.0f * max_us / budget),
             (unsigned long)s_diag_calls, (double)w_per_call, (double)cyc_per_w);

    NOVA_LOG("       laps %lu, worst %lu words outstanding (ring %u) | "
             "needs %.0f%% of core at %lu words/s\n",
             (unsigned long)s_diag_laps,
             (unsigned long)s_diag_worst_outstanding,
             (unsigned)SPDIF_DMA_WORDS,
             (double)(100.0f * cyc_per_w * (float)SPDIF_WORDS_REQUIRED_PER_S
                      / (float)clock_get_hz(clk_sys)),
             (unsigned long)SPDIF_WORDS_REQUIRED_PER_S);

    if (s_diag_laps) {
        NOVA_LOG("       *** RING LAPPED - decoded words are stale and every\n"
                 "       *** figure above derived from them is meaningless\n");
    }

    // **`ring` reads about one block above SPDIF_RING_TARGET and that is
    // correct.** This runs inside fill(), after push() has added a block of
    // input and before produce() drains it; the estimator controls on the
    // level it sees *after* the drain. So a converged loop prints target + 64,
    // not target. Chased once already - it looks exactly like a loop settling
    // on the wrong set point.
    NOVA_LOG("       %s, %d Hz, ring %d, trim %+.1f ppm, gain %.2f | "
             "parity %lu resync %lu (%lu/s) under %lu drop %lu\n",
             kState[s_in.state()], s_in.rateHz(), s_in.ringLevel(),
             (double)s_in.trimPpm(), (double)s_in.gain(),
             (unsigned long)s_in.parityErrors(),
             (unsigned long)s_in.resyncsSeen(),
             (unsigned long)(s_in.resyncsSeen() - s_diag_resync0),
             (unsigned long)s_in.underruns(),
             (unsigned long)s_in.dropped());

    // **The rate is what matters and only the total was printed.** A decoder
    // thrashing at the edge of its window shows a cumulative resync count in
    // six figures, which reads as history rather than as the live fault it is.
    // The report period is exactly one second, so the difference needs no
    // division. It is the same signal the scan itself now runs on - see
    // s_scanResync.
    s_diag_resync0 = s_in.resyncsSeen();

    s_diag_words = s_diag_edges = s_diag_frames = 0;
    s_diag_or    = 0;
    s_diag_and   = 0xFFFFFFFFu;
    s_diag_cyc_sum = s_diag_cyc_max = s_diag_calls = 0;
    s_diag_laps  = 0;
    s_diag_worst_outstanding = 0;
}

#endif // NOVA_TELEMETRY && SPDIF_RX_DIAG

// ---- lifecycle ----------------------------------------------------------

static void spdif_init(void)
{
    s_in.init();
    s_read    = 0;
    s_running = false;
    s_primed  = false;

#if SPDIF_USE_PIO_DECODE
    s_scanIdx    = 0;
    s_scanBlocks = 0;
    s_scanResync = 0;
#endif

#if NOVA_TELEMETRY && SPDIF_RX_DIAG
    // Starts all-ones so the first AND is the first word rather than zero,
    // which would read as "stuck LO" for the whole first second.
    s_diag_and = 0xFFFFFFFFu;
#endif
}

static void spdif_attach(void)
{
    // Claimed here rather than in init() for the same reason the output
    // module's DOUT is: this is the moment the core has set aside for a
    // module to take the pins it owns, and taking them earlier would mean
    // driving a pad before anything is ready to read it.
    s_pio = pio2;

#if SPDIF_USE_PIO_DECODE
    if (!pio_can_add_program(s_pio, &spdif_rx_ui_program)) return;

    const uint off = pio_add_program(s_pio, &spdif_rx_ui_program);
    const int  sm  = pio_claim_unused_sm(s_pio, false);
    if (sm < 0) return;
    s_sm = (uint)sm;

    spdif_rx_ui_program_init(s_pio, s_sm, off, SPDIF_PIN_RX, kScanHz[0]);
#else
    if (!pio_can_add_program(s_pio, &spdif_rx_program)) return;

    const uint off = pio_add_program(s_pio, &spdif_rx_program);
    const int  sm  = pio_claim_unused_sm(s_pio, false);
    if (sm < 0) return;
    s_sm = (uint)sm;

    spdif_rx_program_init(s_pio, s_sm, off, SPDIF_PIN_RX,
                          spdif_rx_sample_rate_hz(SPDIF_OVERSAMPLE));
#endif

    s_dma = dma_claim_unused_channel(false);
    if (s_dma < 0) return;

    dma_channel_config c = dma_channel_get_default_config((uint)s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));

    // Wrap the write address inside the ring. The ring is 4096 words = 16384
    // bytes = 2^14, and the buffer is aligned to that.
    channel_config_set_ring(&c, true, 14);

    // A count this large never completes in practice - the point is that the
    // channel free-runs and fill() reads the write pointer, so nothing has to
    // service an interrupt on the audio path.
    dma_channel_configure((uint)s_dma, &c, s_ring, &s_pio->rxf[s_sm],
                          0xFFFFFFFFu, true);

    pio_sm_set_enabled(s_pio, s_sm, true);
    s_running = true;
}

// ---- the block path -----------------------------------------------------

static void spdif_fill(int32_t *buf)
{
#if NOVA_TELEMETRY && SPDIF_RX_DIAG
    const uint32_t t0 = cyc_now();
#endif

    if (s_running) {
        // ---- how far behind are we --------------------------------------
        //
        // Back to the write pointer. **The transfer counter cannot be used
        // here**: RP2350's TRANS_COUNT carries a MODE field in bits 31:28, so
        // the 0xFFFFFFFF this channel was configured with selects ENDLESS
        // mode, in which the count never decrements. It reads constant, which
        // makes the input look like a dead carrier - measured, and it is why
        // this comment exists rather than the obvious code.
        //
        // So the difference is masked, and a masked difference genuinely
        // cannot tell 904 words behind from 5000 behind. That is survivable
        // only because the clamp below bounds the *work* rather than trusting
        // the measurement: whatever this reads, no call decodes more than
        // SPDIF_LAP_THRESHOLD words, so the spiral cannot start. Once the
        // decoder is keeping up the aliasing stops happening at all.
        const uint32_t w = (uint32_t)(((uintptr_t)dma_hw->ch[s_dma].write_addr
                                       - (uintptr_t)s_ring) / sizeof(uint32_t));

        // attach() starts the DMA free-running and the main loop does not reach
        // the first fill() for a long time afterwards - display_start(),
        // modules_report() and modules_report_effects() all print over USB CDC
        // in between. Starting from 0 would mean decoding a backlog that was
        // overwritten before the box finished introducing itself.
        if (!s_primed) {
            s_primed = true;
            s_read   = w;
        }

        // ---- acquisition, and the cap it gets ----------------------------
        //
        // See SPDIF_ACQUIRE_WORDS_PER_CALL: the oversampling decoder was far
        // more expensive before it had framed than after, and needed a tighter
        // cap to stop that transient becoming permanent. The per-UI decoder is
        // not, so the two caps are the same number there and this reads as the
        // no-op it is.
        const uint32_t budget = s_in.framed() ? SPDIF_MAX_WORDS_PER_CALL
                                              : SPDIF_ACQUIRE_WORDS_PER_CALL;

        uint32_t outstanding = (w - s_read) & (SPDIF_DMA_WORDS - 1u);

        // ---- bound the work, before doing any of it ---------------------
        //
        // Decoding however far behind it has fallen is unbounded work on a
        // deadline: run late once, decode more, run later still. Core 0
        // livelocks, core 1 stalls behind it, and the board looks unbooted.
        //
        // A receiver a ring behind has already lost that audio - the DMA
        // overwrote it - so decoding the wreckage cannot recover it and is
        // precisely what prevents catching up.
        if (outstanding > budget) {
            s_read      = (w - budget) & (SPDIF_DMA_WORDS - 1u);
            outstanding = budget;
#if NOVA_TELEMETRY && SPDIF_RX_DIAG
            ++s_diag_laps;
#endif
        }

#if NOVA_TELEMETRY && SPDIF_RX_DIAG
        if (outstanding > s_diag_worst_outstanding)
            s_diag_worst_outstanding = outstanding;
#endif

        while (outstanding) {
            const uint32_t idx  = s_read & (SPDIF_DMA_WORDS - 1u);
            const uint32_t room = SPDIF_DMA_WORDS - idx;
            const int      n    = (int)((outstanding < room) ? outstanding : room);
#if NOVA_TELEMETRY && SPDIF_RX_DIAG
            // Before push(), and off the raw words, so what it measures is the
            // pin and not the decoder's opinion of the pin.
            diag_scan(&s_ring[idx], n);
            s_diag_frames += (uint32_t)s_in.push(&s_ring[idx], n);
#else
            s_in.push(&s_ring[idx], n);
#endif
            s_read       = (s_read + (uint32_t)n) & (SPDIF_DMA_WORDS - 1u);
            outstanding -= (uint32_t)n;
        }

#if SPDIF_USE_PIO_DECODE
        // ---- step the divider if nothing is framing ----------------------
        //
        // Done after the decode rather than before it, so a position gets the
        // whole of its SPDIF_SCAN_BLOCKS to prove itself.
        //
        // **The reset is not optional.** Changing the divider makes every word
        // already in the ring - and every UI already in the decoder's
        // accumulator - a measurement of a UI that is no longer the UI. Left
        // in place they are a plausible-looking stream at the wrong cadence,
        // which is exactly what the hunt is bad at rejecting quickly. There is
        // nothing to lose by dropping them: this path only runs when nothing
        // has framed, so there is no rate estimate and no audio in flight.
        if (++s_scanBlocks >= SPDIF_SCAN_BLOCKS) {
            const uint32_t rs   = s_in.resyncsSeen();
            const uint32_t grew = rs - s_scanResync;

            s_scanBlocks = 0;
            s_scanResync = rs;

            if (!s_in.framed() || grew > SPDIF_SCAN_MAX_RESYNC) {
                s_scanIdx = (s_scanIdx + 1u) % (uint32_t)SPDIF_UI_SCAN_N;

                spdif_rx_ui_set_rate(s_pio, s_sm, kScanHz[s_scanIdx]);
                s_in.reset();
                s_primed     = false;
                s_scanResync = s_in.resyncsSeen();
            }
        }
#endif
    }

#if NOVA_TELEMETRY && SPDIF_RX_DIAG
    // Closed before produce() and before the print, so what it measures is the
    // decode rather than the resampler or the console.
    const uint32_t cyc = cyc_since(t0);
    s_diag_cyc_sum += cyc;
    if (cyc > s_diag_cyc_max) s_diag_cyc_max = cyc;
    ++s_diag_calls;

    // Printed from the block path, which costs deadline and may tick `ovr` -
    // that is the console, not the DSP, and the same trade the status line
    // already makes once a second from the main loop.
    if (++s_diag_blocks >= SPDIF_DIAG_PERIOD) {
        s_diag_blocks = 0;
        diag_report();
    }
#endif

    // **Always**, running or not. A build whose PIO or DMA claim failed still
    // has a deadline to meet and a DAC being clocked; what it does not have
    // is anything to play, and SpdifInput answers that with silence rather
    // than with an unwritten buffer.
    s_in.produce(buf, (int)AUDIO_BLOCK_FRAMES);
}

// ---- reporting ----------------------------------------------------------

static void spdif_report(void)
{
#if SPDIF_USE_PIO_DECODE
    NOVA_LOG("  input:   S/PDIF on GP%u, PIO2 one sample per UI, "
             "%u cycles/UI at %u Hz\n",
             SPDIF_PIN_RX, (unsigned)SPDIF_UI_CYCLES,
             (unsigned)kScanHz[s_scanIdx]);
#else
    NOVA_LOG("  input:   S/PDIF on GP%u, PIO2 at %u Hz (%d samples/UI at 48k)\n",
             SPDIF_PIN_RX, (unsigned)spdif_rx_sample_rate_hz(SPDIF_OVERSAMPLE),
             SPDIF_OVERSAMPLE);
#endif

    if (!s_running) {
        NOVA_LOG("           NOT RUNNING - PIO or DMA claim failed\n");
        return;
    }

    // The same discipline the ST7789 banner uses: say what was configured,
    // and do not claim a source was found. There is nothing to probe here -
    // a receiver cannot ask a transmitter whether it is there - so a silent
    // input is not an error and must not be reported as one.
    NOVA_LOG("           ring %u words, resample to %u Hz; a silent input is\n"
             "           not reported here - watch 'in' on DSP STATUS\n",
             (unsigned)SPDIF_DMA_WORDS, (unsigned)AUDIO_SAMPLE_RATE);
}

static bool spdif_locked(void)
{
    return s_in.state() == SpdifInput::kLocked;
}

static uint32_t spdif_rate_hz(void)
{
    // 0 while nothing is decodable, which is what the status line should show
    // rather than a stale rate or a hopeful 48000.
    const int hz = s_in.rateHz();
    return hz > 0 ? (uint32_t)hz : 0u;
}

// ---- descriptor ---------------------------------------------------------

static const input_module_t k = {
    .name    = "spdif",
    .init    = spdif_init,
    .attach  = spdif_attach,
    .fill    = spdif_fill,
    .report  = spdif_report,
    .locked  = spdif_locked,
    .rate_hz = spdif_rate_hz,
};

extern "C" const input_module_t *spdif_input_module(void) { return &k; }
