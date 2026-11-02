/*
 * module.h - the four plug points, and the only thing a module and the core
 * agree on.
 *
 * NovaDSP's core does four things: get a block of samples, hand it to whichever
 * effect is active, transform it for the spectrum, and hand it to the output. It
 * knows nothing about *which* effects exist, what kind of display is fitted,
 * where the samples came from, or where the processed block ends up. Those are
 * modules, chosen at build time, and each one reaches the core through exactly
 * one of the four descriptors below.
 *
 * **Input and output are the two ends of the same asymmetry.** On this board the
 * ADC and the DAC share one PIO state machine and one DMA ring, so the buffer
 * handed to an input module is already the buffer RX wrote and the buffer handed
 * to an output module is already the one TX will drain - which is why both I2S
 * modules are almost entirely empty, and why the hooks are worth having anyway:
 * an end that is *not* on that state machine is where they do the work.
 *
 * ---- what a module may and may not know --------------------------------
 *
 * A module may use the core: effect.h's registry, spectrum.h, settings.h,
 * audio.h's block geometry, nova_build.h's NOVA_LOG. **A module may not
 * include another module's headers.** Two effects that need to know about each
 * other are one effect; a display that needs to know which effects are
 * compiled in reads the registry, which is the interface for exactly that.
 *
 * FX Chain is the case worth reading, because it looks like a violation and is
 * not: it runs other effects without naming any of them, by walking the
 * registry through late_init() below.
 *
 * ---- how a module is discovered ----------------------------------------
 *
 * It is not, by any code in this file. Each module directory carries a
 * `module.cmake` declaring its name, type, sources and one accessor function;
 * the top-level CMakeLists globs those, drops the ones switched off, and
 * generates `nova_modules_generated.h` naming the survivors. modules.c walks
 * that list. **Adding a module means adding a directory and nothing else** -
 * no core file is edited, and no core file names any module.
 *
 * See nova_modules.cmake for the declaration syntax and the build options.
 */

#ifndef NOVADSP_MODULE_H
#define NOVADSP_MODULE_H

#include <stdint.h>
#include <stdbool.h>

// NULL. Every module descriptor below is a designated initialiser with unused
// hooks spelled NULL, and this is the header that has to make that work.
//
// It is here rather than left to each module because of an asymmetry that has
// caught this project twice: nova_build.h pulls in <stdio.h> on a **dev** build
// and nothing at all on **dist**, so anything relying on that transitive
// include compiles cleanly in dev and fails only in dist. That is how the first
// dist build of fxchain.c failed, and it is how the first dist build after this
// refactor failed too - four NULLs in agc_module's descriptor.
#include <stddef.h>

#include "effect.h"
#include "spectrum.h"
#include "nova_build.h"   // NOVA_LOG / NOVA_TELEMETRY: every module reports

#ifdef __cplusplus
extern "C" {
#endif

// ---- effect modules -----------------------------------------------------
//
// One module contributes one effect. A module that wants to contribute two is
// two modules; nothing is saved by folding them, and the build option that
// switches one off should not take the other with it.
//
// Order of registration is menu order, and the first one registered is what
// the board comes up running before settings are applied. That order is the
// ORDER field in module.cmake, not the glob order and not this struct.

typedef struct {
    const char *name;                    // for the banner; not the effect name

    // Core 0, before the effect is registered. Construct the engine and set
    // its power-on parameters. Required, because the registry reads every
    // parameter as it registers in order to snapshot the Reset set - so the
    // engine has to exist by then.
    void (*init)(void);

    // The effect itself. Required. Called immediately after init().
    const effect_t *(*effect)(void);

    // Optional. Core 0, after *every* module has registered. This is what an
    // effect that composes other effects needs, and the only reason it exists:
    // FX Chain builds its candidate list by walking the finished registry.
    void (*late_init)(void);

    // Optional. One or more banner lines, core 0, while printing is still
    // free. Prints through NOVA_LOG, so it says nothing at all in dist.
    void (*report)(void);

    // Optional, dev build only. The console offers each key to every module in
    // turn until one returns true. Return true if the key changed something
    // worth reprinting.
    bool (*on_key)(int key);

    // Optional, dev build only. Core 0, before audio_init() - it may take the
    // CPU for as long as it likes, because there is no deadline yet.
    void (*benchmark)(void);
} effect_module_t;

// ---- display modules ----------------------------------------------------
//
// A display module owns its panel, its keys and its entire navigation model.
// The core hands it numbers and nothing else: it does not know how many pages
// there are, what a press does, or whether there is a screen at all.
//
// **All of it runs on core 1**, apart from init() and report(). A full OLED
// frame is ~23 ms of blocking I2C - seventeen consecutive audio deadlines - so
// nothing here may run on core 0 once audio has started, and nothing here may
// call printf: stdio's mutex is shared with core 0's status line, and blocking
// on it would put the audio deadline at the mercy of the USB host.
//
// Changing an effect is done by asking: effect_request_*() from effect.h lands
// in a ring core 0 drains between blocks. Reading one is effect_value(), which
// is a shadow core 0 keeps current. Neither blocks either core.

typedef struct {
    uint32_t rate_hz;      // measured blocks/s * frames, i.e. the real Fs
    uint32_t overruns;
    float    in_l, in_r;   // dBFS, before the effect
    float    out_l, out_r; // dBFS, after it
    float    dsp_avg_us;
    float    dsp_max_us;
    float    dsp_pct;      // max against the block deadline

    // Generic input properties, filled from the selected input module. An ADC
    // reports locked at AUDIO_SAMPLE_RATE and always has; an input with a
    // clock of its own - S/PDIF - reports what it actually found. Deliberately
    // not named after any module: an input having a rate and a lock state is
    // true of the category.
    bool     input_locked;
    uint32_t input_rate_hz;
} display_status_t;

typedef struct {
    const char *name;

    // Core 0, before audio_init(), while printing is still free. Bring up the
    // panel and the key hardware. Return false if the hardware did not answer
    // - and stay safe to call afterwards, degrading to no-ops. **A missing
    // panel must never be able to take the audio down with it.**
    bool (*init)(void);

    // Core 1, once, before the first tick(). Optional.
    void (*start)(void);

    // Core 1, every poll_us. Sample the keys, decide whether anything needs
    // redrawing, redraw it. The module owns its own pacing: this is the rate
    // it is *offered*, not the rate it must repaint at.
    void (*tick)(const display_status_t *status, const spectrum_frame_t *spec);

    // Optional. Banner line, core 0, after init().
    void (*report)(void);

    // Optional. A module-specific error counter for the once-a-second health
    // line - I2C failures, or whatever this panel can fail at. Read on core 0;
    // return 0 if there is nothing to count.
    uint32_t (*errors)(void);

    // How often core 1 should call tick(). The encoder wants ~2 ms; a module
    // with no hardware at all can ask for far less often.
    uint32_t poll_us;
} display_module_t;

// ---- output modules -----------------------------------------------------
//
// Where the processed block goes. Called on core 0, after the effect and after
// the spectrum has seen the block - so an output module cannot change what the
// analyser displays, which is the point of being able to switch it off.
//
// **This is a thin interface and the reason is hardware.** The ADC and the DAC
// share one PIO state machine and one DMA ring: the block handed to write() is
// already the buffer the TX DMA will drain, and the clocks the ADC needs are
// generated by the same state machine that clocks the DAC. So the default
// module's write() has nothing to do, and switching output off cannot stop the
// pins toggling - it can only stop meaningful samples reaching them. See
// i2s_output_module/ and no_output_module/.
//
// What an output module does genuinely own is **its pin**. attach() below is
// where AUDIO_PIN_DOUT is claimed, and no core file claims it: audio.c points
// the state machine's shift register at that pin because the PIO program has
// to shift somewhere, and stops there. Whether the pad is function-selected to
// PIO and driven at all is the module's decision.

typedef struct {
    const char *name;

    // Optional. Core 0, before audio_init().
    void (*init)(void);

    // Optional. Core 0, from **inside** audio_init(), after the I2S state
    // machine is configured and before it is started. Claim the pins this
    // output owns - for both modules in this tree that means one call to
    // audio_claim_dout().
    //
    // It is a separate hook from init() because it is a separate moment, and
    // neither would do for the other: at init() there is no state machine to
    // claim a pin for, and once the state machine is running the pin is
    // already whatever it is going to be.
    //
    // A module that leaves this NULL leaves GP16 a plain input. That is the
    // right answer for an output that is not driving the I2S DAC - a USB
    // endpoint, say - and the wrong one for a board with a PCM5102A on it,
    // whose DIN would then float.
    void (*attach)(void);

    // Core 0, once per block, last. AUDIO_BLOCK_SAMPLES interleaved 32-bit
    // samples, 24-bit left-justified, in place - the same buffer and the same
    // format the effect saw.
    void (*write)(int32_t *buf);

    // Optional. Banner line, core 0.
    void (*report)(void);
} output_module_t;

// ---- input modules ------------------------------------------------------
//
// Where the block's *samples* come from. Called on core 0, once per block,
// before the effect and before the meters.
//
// **This interface is thin for exactly the same reason the output one is, and
// it is the mirror image of it.** The ADC and the DAC share one PIO state
// machine and one DMA ring, so the buffer the core hands to fill() is already
// the buffer the RX DMA just filled - which means the I2S input module's
// fill() has nothing to do, in the same way the I2S output module's write()
// has nothing to do. The two empty hooks are the same fact seen from the two
// ends, and neither is a placeholder.
//
// What an input module that is *not* the ADC does is **overwrite** the block.
// The timing is not its business: the I2S state machine keeps running whatever
// the input is, so blocks keep arriving at exactly 48 kHz and the DAC's clocks
// never stop. An S/PDIF input decodes, resamples onto that grid and writes the
// result in. See spdif_input_module/.
//
// That is what makes a source with its own clock safe here. The alternative -
// letting the input module drive the block cadence - would put the output rate
// at the mercy of whatever is plugged in, and there is no way back from that
// on a chip with no steerable audio PLL.

typedef struct {
    const char *name;

    // Optional. Core 0, before audio_init().
    void (*init)(void);

    // Optional. Core 0, from **inside** audio_init(), after the I2S state
    // machine is configured and before it is started - the same moment the
    // output module's attach() runs, and for the same reason. Claim whatever
    // pins this input owns.
    void (*attach)(void);

    // Core 0, once per block, first. AUDIO_BLOCK_SAMPLES interleaved 32-bit
    // samples, 24-bit left-justified, in place.
    //
    // **Must always leave the buffer completely written.** There is no way to
    // say "no audio this block" - the deadline does not move and the DAC is
    // still being clocked - so an input with nothing to give writes silence,
    // and one whose source just disappeared ramps to silence rather than
    // stepping to it.
    //
    // NULL means "the buffer already holds the samples", which is the I2S
    // case. It is not the same as writing silence.
    void (*fill)(int32_t *buf);

    // Optional. Banner line, core 0.
    void (*report)(void);

    // Optional. For the status line. NULL locked() means permanently locked,
    // which is what an ADC is; NULL rate_hz() means AUDIO_SAMPLE_RATE.
    //
    // These are **generic input properties** and not S/PDIF ones - an input
    // having a lock state and a rate is true of the category - which is what
    // keeps display_status_t from naming a module.
    bool     (*locked)(void);
    uint32_t (*rate_hz)(void);
} input_module_t;

// ---- what the core calls (modules.c) ------------------------------------
//
// All core 0, all at startup unless marked otherwise. Every one of these walks
// the generated list; none of them names a module.

// init() then effect_register() for each effect module in ORDER order, then
// late_init() for each. Call before settings_load().
void modules_init_effects(void);

// The module summary line, then the display's report() and the output's.
// Prints through NOVA_LOG. Call after settings_load(), so what it says is what
// was actually restored.
void modules_report(void);

// Every effect module's report(), separately and **after
// effect_report_active()**. See the note in modules.c: an effect module's
// report may dump a live parameter set, and that set belongs directly below the
// line naming what is actually running.
void modules_report_effects(void);

// dev only: every effect module's benchmark(), before audio_init().
void modules_benchmark(void);

// dev only: offer a console key to each effect module until one takes it.
bool modules_handle_key(int key);

// FNV-1a over the registered layout - every effect's name in registration
// order, its parameter count, and each parameter's name, kind and range. This
// is the settings store's identity, and it changes exactly when a stored
// record would stop meaning what it meant. See settings.c.
uint32_t modules_layout_id(void);

// The one display, output and input module this build was compiled with.
// Never NULL: a build with no display gets no_display_module, one with no
// output gets no_output_module, and the input always resolves to something
// because there is always a state machine feeding the buffer.
const display_module_t *modules_display(void);
const output_module_t  *modules_output(void);
const input_module_t   *modules_input(void);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_MODULE_H
