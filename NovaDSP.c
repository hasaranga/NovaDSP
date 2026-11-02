/*
 * NovaDSP - audio DSP firmware for the Raspberry Pi Pico 2 (RP2350).
 *
 * PCM1808 ADC in, PCM5102A DAC out, both slaved to clocks generated here:
 * 48 kHz, 24-bit, one PIO state machine carrying both directions, triple
 * buffered through DMA.
 *
 * The core runs at 307.2 MHz, double the 150 MHz the RP2350 is specified for,
 * because that clock divides exactly into the 48 kHz audio family.
 *
 * ---- what this file knows, and what it deliberately does not ------------
 *
 * The loop below is the whole of the firmware's fixed behaviour: take a block
 * from the ADC, meter it, hand it to whichever effect is active, meter it
 * again, transform it for the spectrum, hand it to the output.
 *
 * **It names no effect, no display and no output.** There is no
 * effect_register() call here any more, no dsp_init(), no sh1106 anything.
 * Those are modules, discovered by CMake from the module.cmake files in the
 * tree and reached through the three descriptors in module.h. Adding an effect
 * or a second kind of panel does not touch this file, which is the point:
 * everything below is true of every build, and everything that is not true of
 * every build lives in a module directory.
 *
 * The order of the three calls inside the `if (buf)` is a contract rather than
 * a convenience, and the one that matters is that the spectrum sees the block
 * **before** the output module does. That is what lets no_output_module blank
 * the DAC without blanking the analyser, which is the only reason it exists.
 */

#include <stdio.h>
#include <math.h>

#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "audio.h"
#include "effect.h"
#include "module.h"
#include "display.h"
#include "fault.h"
#include "settings.h"
#include "spectrum.h"
#include "nova_build.h"

#ifdef PICO_DEFAULT_LED_PIN
#define LED_PIN PICO_DEFAULT_LED_PIN
#else
#define LED_PIN 25
#endif

// The deadline is one block period: 1.333 ms at 64 frames / 48 kHz. Watch the
// overrun counter in the status line - it increments whenever a block arrives
// before the previous one was collected, which is the deadline being missed.

// ---- instrumentation ---------------------------------------------------

// Two sets of peaks. The slow pair backs the once-a-second console line; the
// fast pair drives the display, which needs to move like a meter rather than
// like a report. meter_block() fills the fast pair and the slow pair takes the
// maximum of it whenever the fast one is drained.
static int32_t peak_in_l, peak_in_r, peak_out_l, peak_out_r;
static int32_t fast_in_l, fast_in_r, fast_out_l, fast_out_r;

// 40 Hz. Faster than the eye resolves as steps, slow enough that the seqlock
// and the publish are lost in the noise against a 1333 us block.
#define METER_INTERVAL_MS   25

static void meter_block(const int32_t *buf, int32_t *pl, int32_t *pr) {
    for (uint32_t i = 0; i < AUDIO_BLOCK_SAMPLES; i += 2) {
        int32_t l = buf[i] >> 8;
        int32_t r = buf[i + 1] >> 8;
        if (l < 0) l = -l;
        if (r < 0) r = -r;
        if (l > *pl) *pl = l;
        if (r > *pr) *pr = r;
    }
}

// 24-bit full scale is 2^23. Floors at -120 dB for silence.
static float dbfs(int32_t peak) {
    if (peak <= 0) return -120.0f;
    return 20.0f * log10f((float)peak / 8388608.0f);
}

int main(void) {
    bi_decl(bi_program_description("NovaDSP - 48kHz I2S DSP on RP2350"));
    bi_decl(bi_1pin_with_name(LED_PIN, "Status LED"));
    bi_decl(bi_1pin_with_name(AUDIO_PIN_MCLK, "I2S MCLK -> PCM1808 SCKI"));
    bi_decl(bi_1pin_with_name(AUDIO_PIN_BCK, "I2S BCK"));
    bi_decl(bi_1pin_with_name(AUDIO_PIN_LRCK, "I2S LRCK"));
    bi_decl(bi_1pin_with_name(AUDIO_PIN_DOUT, "I2S DOUT -> PCM5102A DIN"));
    bi_decl(bi_1pin_with_name(AUDIO_PIN_DIN, "I2S DIN <- PCM1808 DOUT"));

    // Overclock before stdio, so USB enumerates at the final clock.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(10);  // let voltage settle

    set_sys_clock_khz(307200, true);  // 307.2MHz - exact 48kHz audio clocks

    stdio_init_all();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    // ---- a channel that does not depend on the one that breaks -----------
    //
    // **The console cannot report a crash, because a crash is what stops the
    // console.** An S/PDIF build was producing no output at all - not even
    // this file's banner, which prints well before any audio runs - while USB
    // stayed enumerated and picotool could still reboot the board. That
    // pattern is a hard fault on core 0: the default handler spins, the main
    // loop never runs, and TinyUSB keeps answering the host from its timer
    // interrupt, so from outside the board looks alive and mute.
    //
    // Livelock and hard fault look identical over a dead serial port and need
    // completely different fixes, so the LED is made to tell them apart: a
    // fault blinks it fast and forever, where the running firmware blinks it
    // once a second from the status tick. Nothing here needs USB, a terminal,
    // or the main loop to survive.
    nova_fault_led_init(LED_PIN);

#if NOVA_TELEMETRY
    // Give the host a moment to enumerate the CDC device so the first few
    // lines are not lost when a terminal is already attached. Nothing is
    // printed in dist, so there is nothing to wait for and the board comes up
    // two seconds sooner.
    //
    // **That difference is load-bearing and it once cost a debugging round.**
    // A display module's init() sits below this, so on a dist build it runs
    // 2.4 s earlier - under an OLED's power-on time, where the panel ACKs
    // every byte and ignores the display commands. A display module must wait
    // on time-since-boot rather than inherit whatever delay preceded it.
    sleep_ms(2000);
#endif

    // Every effect module's init(), then its effect_register(), in the order
    // CMake emitted - which is each module.cmake's ORDER field, which is menu
    // order, and index 0 is what the box comes up running. Then every
    // late_init(), for the effects that compose other effects and cannot build
    // their candidate list until the registry is finished.
    modules_init_effects();

#if NOVA_TELEMETRY
    // Before audio_init(): a benchmark needs the CPU and the console to
    // itself, and it would otherwise count as one very long missed deadline.
    // Only the modules that have one run anything here.
    modules_benchmark();
#endif

    // Builds the window, twiddle and band tables - about nine hundred
    // sinf/cosf calls, so it belongs here rather than anywhere near the audio.
    spectrum_init();

    // After registration, before audio: applies whatever was stored last run
    // through the same path the console and the display use. Also where the
    // store is erased if the layout has changed since it was written, which is
    // safe exactly here and nowhere later - an erase is 36 ms, twenty-seven
    // block deadlines, and long enough for the USB host to drop the CDC device.
    settings_load();

    // Before audio_init() so the result can still be printed freely - the
    // display module owns core 1 from display_start() onwards, and from then on
    // it must not touch stdio.
    bool have_display = display_init();

    if (modules_output()->init) modules_output()->init();

    audio_init();
    display_start();

    // ---- the banner ----------------------------------------------------
    // Measured against the crystal, not just what the SDK thinks it configured.
    NOVA_LOG("NovaDSP (" NOVA_BUILD_NAME ") starting - clk_sys %lu kHz\n",
           (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS));
    NOVA_LOG("  Fs %u Hz, %u frames/block, dividers %s\n",
           AUDIO_SAMPLE_RATE, AUDIO_BLOCK_FRAMES,
           audio_dividers_are_integer() ? "integer" : "FRACTIONAL");
    NOVA_LOG("  MCLK %lu Hz on GP%u, PIO %lu Hz, BCK GP%u, LRCK GP%u\n",
           (unsigned long)audio_get_mclk_hz(), AUDIO_PIN_MCLK,
           (unsigned long)audio_get_pio_clk_hz(), AUDIO_PIN_BCK, AUDIO_PIN_LRCK);

    // The module summary, then the display and the output. The core prints
    // none of it and knows none of it.
    modules_report();
    if (!have_display)
        NOVA_LOG("  display: init reported no hardware - running headless\n");

    spectrum_report();

    // After settings_load(), so this is what was actually restored rather than
    // the effect that happened to register first.
    //
    // **This line and the effect-module reports below it are one thing, in this
    // order.** An effect module's report may dump its live parameter set -
    // DeeFloat's does - and that set is DeeFloat's whatever effect is actually
    // running. On a board running a chain it therefore describes something you
    // are not listening to, and this is the only line that says so. Anything
    // printed between the two breaks the pairing.
    effect_report_active();
    modules_report_effects();

#if NOVA_TELEMETRY
    NOVA_LOG("console: 's' settings, '?' per-module key help\n");
#endif

    // Cycles available per block. The DSP figure below is measured against
    // this, not against wall-clock time.
    const uint32_t budget_cyc =
        (uint32_t)(((uint64_t)clock_get_hz(clk_sys) * AUDIO_BLOCK_FRAMES)
                   / AUDIO_SAMPLE_RATE);
    const float cyc_per_us = (float)clock_get_hz(clk_sys) / 1e6f;

    // Resolved once. Both are function calls into modules.c that return a
    // pointer to a const descriptor, and the write hook is on the block path.
    const output_module_t *const out = modules_output();

    // Resolved once for the same reason. fill() may be NULL - that is the I2S
    // case, where the buffer the RX DMA just wrote is already the samples -
    // so the branch below tests the hook rather than the module.
    const input_module_t *const in = modules_input();

    audio_stats_t stats;
    effect_timing_t timing;
    uint32_t last_rx = 0;
    uint32_t last_c1 = 0, last_ie = 0;
    absolute_time_t next_report = make_timeout_time_ms(1000);
    absolute_time_t next_meter  = make_timeout_time_ms(METER_INTERVAL_MS);

    // Held between meter updates so a 40 Hz publish can carry the once-a-second
    // figures alongside the levels, rather than blanking them 39 times out of 40.
    display_status_t ui = {0};

    // Free-running, for the heartbeat below. Wraps harmlessly.
    uint32_t heartbeat = 0;

    // ---- "init finished" ------------------------------------------------
    //
    // Three deliberate slow blinks, then the heartbeat takes over. Without
    // this, a board that never reaches this line and a board whose loop stops
    // one block later look identical - both are just a lit LED - and they are
    // completely different faults. The three blinks say init completed:
    // modules registered, settings loaded, audio and core 1 started.
    //
    // It costs 600 ms of a build that has already spent two seconds waiting
    // for USB, and it is the only progress report that survives a dead
    // console.
    for (int i = 0; i < 3; ++i) {
        gpio_put(LED_PIN, 1); sleep_ms(120);
        gpio_put(LED_PIN, 0); sleep_ms(80);
    }

    while (true) {
        int32_t *buf = audio_take_block();
        if (buf) {
            // First, and before the input meters: an input module that is not
            // the ADC overwrites the block here, so metering ahead of it would
            // be metering whatever the RX DMA happened to leave behind.
            //
            // The block still arrives on the I2S state machine's schedule
            // whatever the input is. That is what keeps the DAC clocked at
            // exactly 48 kHz through a source appearing, disappearing or
            // changing rate - see module.h.
            if (in->fill) in->fill(buf);

            meter_block(buf, &fast_in_l, &fast_in_r);
            effect_process_block(buf);
            meter_block(buf, &fast_out_l, &fast_out_r);

            // The effect's output, for the idle screen. Cheap every block; on
            // one block in eight it also runs the transform, which is the
            // "fft" figure in the status line below.
            spectrum_feed(buf);

            // Last, and after the spectrum on purpose. An output module that
            // silences the block must not be able to silence the display.
            out->write(buf);

            // ---- heartbeat ----------------------------------------------
            //
            // **Inside `if (buf)`, so it counts blocks and not loop spins.**
            // The loop polls audio_take_block() and gets NULL most times
            // round, at hundreds of kHz, so a divisor applied out there
            // produces a multi-kilohertz square wave - which to the eye is
            // simply a lit LED, indistinguishable from a board that has
            // stopped. Measured the hard way: it read "always on" for a
            // perfectly healthy loop.
            //
            // Blocks arrive at exactly 750 a second, so 128 of them is 5.9 Hz
            // and unmistakably alive. It also sits after write(), so a stall
            // anywhere in the block path stops it.
            if ((++heartbeat & 127u) == 0u) gpio_xor_mask(1u << LED_PIN);
        }

        // Carry out whatever the display module asked for, between blocks
        // rather than during one. Usually an empty ring and a handful of loads.
        effect_service();

        // Writes only when a save was actually requested.
        settings_tick();

#if NOVA_TELEMETRY
        // Non-blocking. Anything that prints here costs deadline, so a key
        // press may show up as an overrun - that is the console, not the DSP.
        //
        // 's' is the core's, because the settings store is the core's. Every
        // other key is offered to each effect module in turn until one takes
        // it, so the console works with whatever module set this build has and
        // needs no updating when that changes.
        int key = getchar_timeout_us(0);
        if (key == 's') {
            settings_debug();
        } else if (key != PICO_ERROR_TIMEOUT) {
            modules_handle_key(key);
        }
#endif

        // Drain the fast peaks into the display at 40 Hz, folding them into
        // the slow pair on the way past so the console line still reports the
        // peak over its whole second rather than over the last 25 ms.
        if (time_reached(next_meter)) {
            next_meter = make_timeout_time_ms(METER_INTERVAL_MS);

            if (fast_in_l  > peak_in_l)  peak_in_l  = fast_in_l;
            if (fast_in_r  > peak_in_r)  peak_in_r  = fast_in_r;
            if (fast_out_l > peak_out_l) peak_out_l = fast_out_l;
            if (fast_out_r > peak_out_r) peak_out_r = fast_out_r;

            ui.in_l  = dbfs(fast_in_l);
            ui.in_r  = dbfs(fast_in_r);
            ui.out_l = dbfs(fast_out_l);
            ui.out_r = dbfs(fast_out_r);
            display_publish(&ui);

            // Same 40 Hz tick. The transform completes at 94 Hz, faster than
            // either the publish or a display's redraw, so nothing here has to
            // chase it - it just takes whatever the latest frame is.
            spectrum_frame_t spec;
            spectrum_get(&spec);
            display_publish_spectrum(&spec);

            fast_in_l = fast_in_r = fast_out_l = fast_out_r = 0;
        }

        if (time_reached(next_report)) {
            audio_get_stats(&stats);
            effect_timing_take(&timing);
            uint32_t blocks = stats.rx_blocks - last_rx;
            last_rx = stats.rx_blocks;

            float avg_cyc = timing.blocks
                          ? (float)((double)timing.sum_cyc / timing.blocks) : 0.0f;
            float max_cyc = timing.blocks ? (float)timing.max_cyc : 0.0f;

            // Reported separately from the effect: the FFT is not part of any
            // effect, but it lands on the same deadline, and on the block that
            // runs it the two add up. That sum is what ovr is watching.
            float fft_us = (float)spectrum_timing_take() / cyc_per_us;

            // blocks/s * frames/block should land on the nominal sample rate.
            NOVA_LOG("%lu Hz ovr=%lu | in L %6.1f R %6.1f | out L %6.1f R %6.1f dBFS"
                   " | dsp %5.1f/%5.1f us %4.1f%% | fft %5.1f us\n",
                   (unsigned long)(blocks * AUDIO_BLOCK_FRAMES),
                   (unsigned long)stats.overruns,
                   (double)dbfs(peak_in_l), (double)dbfs(peak_in_r),
                   (double)dbfs(peak_out_l), (double)dbfs(peak_out_r),
                   (double)(avg_cyc / cyc_per_us),
                   (double)(max_cyc / cyc_per_us),
                   (double)(100.0f * max_cyc / (float)budget_cyc),
                   (double)fft_us);

            // Only worth a line when something is wrong. A stalled tick count
            // means core 1 died and the panel is frozen showing stale numbers;
            // a display error count means the panel stopped acknowledging.
            // Neither shows up in the audio figures, so nothing else would say
            // so - and the error count is whatever the display module counts,
            // which for a module with no hardware is nothing at all.
            uint32_t c1 = display_core1_ticks();
            uint32_t ie = display_errors();
            if (c1 == last_c1 || ie != last_ie) {
                // **The tick count itself, not just "stalled".** Zero means
                // core 1 never got going at all - a launch that did not take,
                // or a module start() that faulted. A large frozen number
                // means it ran and then stopped, which is a different fault
                // with a different cause, and the two are indistinguishable
                // from the word alone.
                static const char *const kStage[] = {
                    "before the loop", "status_read", "spectrum_read",
                    "the module tick", "sleep"
                };
                const uint32_t sg = display_core1_stage();

                NOVA_LOG("  WARNING core1 %s at %lu ticks, in %s (mark %lu),"
                       " display errors %lu\n",
                       c1 == last_c1 ? "STALLED" : "running",
                       (unsigned long)c1,
                       sg <= 4u ? kStage[sg]
                     : sg == 101u ? "tick: encoder_poll"
                     : sg == 102u ? "tick: ui_tick"
                     : sg == 103u ? "tick: sh1106_flush"
                     : sg == 104u ? "tick: done"
                     : "?",
                       (unsigned long)sg,
                       (unsigned long)ie);
            }
            last_c1 = c1;
            last_ie = ie;

            // The slow half of what the display shows. The levels are
            // refreshed forty times a second above; these are not, so they
            // live in a held struct rather than being republished from scratch.
            ui.rate_hz    = blocks * AUDIO_BLOCK_FRAMES;
            ui.overruns   = stats.overruns;
            ui.dsp_avg_us = avg_cyc / cyc_per_us;
            ui.dsp_max_us = max_cyc / cyc_per_us;
            ui.dsp_pct    = 100.0f * max_cyc / (float)budget_cyc;

            // Generic input properties. An ADC has no lock state and no rate
            // of its own, so a NULL hook is read as "locked, at the rate this
            // board is generating" rather than as "unknown".
            ui.input_locked  = in->locked  ? in->locked()  : true;
            ui.input_rate_hz = in->rate_hz ? in->rate_hz() : AUDIO_SAMPLE_RATE;

            peak_in_l = peak_in_r = 0;
            peak_out_l = peak_out_r = 0;
            next_report = make_timeout_time_ms(1000);
        }
    }
}
