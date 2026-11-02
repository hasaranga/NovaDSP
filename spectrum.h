/*
 * spectrum.h - a 512-point real FFT of the active effect's *output*, folded
 * into sixteen log-spaced bands for the idle screen.
 *
 * ---- which core does what ----------------------------------------------
 *
 * This runs on core 0, because only core 0 may look at an audio buffer. The
 * block handed to effect_process_block() is a DMA target that gets refilled
 * two blocks later, so there is no window in which core 1 could read it
 * safely - and copying a whole sample window across for core 1 to transform
 * would be a far larger thing to hand over than the sixteen numbers that come
 * out of the transform. Core 0 therefore does the FFT and publishes the bands;
 * core 1 maps them to pixels and applies the ballistics.
 *
 * ---- what it costs -----------------------------------------------------
 *
 * The per-block feed is a mono sum over 64 frames - a few hundred cycles, lost
 * in the noise. The transform itself runs on one block in eight, when the
 * window fills. That block pays for all of it at once, so it is the peak and
 * not the average that matters against the 1333 us deadline: watch the "fft"
 * figure in the status line, and the overrun counter beside it.
 *
 * A 512-point *real* transform is done as a 256-point complex one plus an
 * untangle pass, which is roughly half the work of transforming 512 complex
 * points with the imaginary halves set to zero. That is what makes it fit.
 */

#ifndef NOVADSP_SPECTRUM_H
#define NOVADSP_SPECTRUM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 32 bars across 128 px is a 4 px pitch: a 3 px bar and a 1 px gap. Thinner
// than the 16-band version, which at 7 px read as a row of blocks rather than
// as a spectrum. Must divide SH1106_WIDTH, so 16, 32 or 64.
//
// Note what 32 bands costs at this transform size: the bottom nineteen bands
// come out one bin wide, so below about 1.8 kHz the display is effectively
// linear at 93.75 Hz per bar rather than log. That is not a bug in the band
// table, it is all the resolution a 512-point transform has down there -
// SPECTRUM_FFT_N is the knob that changes it, at roughly 2x the cost per
// doubling.
#define SPECTRUM_BANDS      32

// 512 samples at 48 kHz is a 10.7 ms window and 93.75 Hz bins. Larger would
// resolve the bass better and cost proportionally more on the block that runs
// it; smaller stops distinguishing the bottom two octaves at all. Must be a
// multiple of AUDIO_BLOCK_FRAMES, and a power of two.
#define SPECTRUM_FFT_N      512

// What core 0 hands to core 1. dBFS per band, band 0 lowest. Silence floors
// at about -200 dB rather than -inf, so the display arithmetic stays finite.
typedef struct {
    float band_db[SPECTRUM_BANDS];
} spectrum_frame_t;

// Builds the window, twiddle and band tables. Call once, on core 0, before
// audio starts - it runs about nine hundred sinf/cosf calls.
void spectrum_init(void);

// Core 0, immediately after the effect's block hook: accumulates the block
// into the window and transforms when the window is full.
void spectrum_feed(const int32_t *buf);

// Core 0. The most recent completed frame; unchanged between transforms.
void spectrum_get(spectrum_frame_t *out);

// Longest single transform since the last call, in clk_sys cycles, and reset.
// This is deliberately separate from the effect timing: the FFT is not part of
// any effect, but it lands on the same deadline.
uint32_t spectrum_timing_take(void);

// Geometric centre frequency of a band, in Hz, or 0 for an out-of-range index.
//
// For the display's tilt table. This module publishes *true* dBFS and has no
// opinion about how it should be presented; the UI needs the frequencies to
// build its slope, and they live here.
float spectrum_band_centre_hz(int band);

// One line for the boot banner, while printing is still free.
void spectrum_report(void);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_SPECTRUM_H
