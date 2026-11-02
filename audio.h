/*
 * NovaDSP audio I/O: I2S in and out over one PIO state machine, with a
 * triple-buffered DMA ring.
 *
 * Pipeline: while the RX DMA fills buffer N, the application processes
 * buffer N-1 in place, and the TX DMA drains buffer N-2. Because RX and TX
 * share a state machine and a buffer pool, input and output stay locked to
 * the same frame with no resynchronisation.
 */

#ifndef NOVADSP_AUDIO_H
#define NOVADSP_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Clocking ----------------------------------------------------------
// 48 kHz divides exactly out of a 307.2 MHz clk_sys:
//   PIO   = 256 * Fs = 12.288 MHz -> clk_sys / 25  (integer)
//   MCLK  = 256 * Fs = 12.288 MHz -> clk_sys / 25  (integer, via CLK_GPOUT)
//   BCK   =  64 * Fs =  3.072 MHz
// Both dividers are integers off the same clk_sys, so MCLK and BCK stay
// locked at exactly 4:1 - which is what the PCM1808 requires of SCKI.
#define AUDIO_SAMPLE_RATE   48000u
#define AUDIO_MCLK_MULT     256u        // PCM1808 accepts 256/384/512 fs

// ---- Pins --------------------------------------------------------------
// MCLK must be a CLK_GPOUT-capable pin. On RP2350 those are GPIO 13, 15, 21,
// 23, 24, 25 - and 23/24/25 are taken by the Pico 2 board (SMPS, VBUS sense,
// LED), so GPIO 13 or 15 is the real choice. BCK and LRCK must be consecutive
// because they are driven by PIO side-set.
#define AUDIO_PIN_MCLK      13u         // -> PCM1808 SCKI
#define AUDIO_PIN_BCK       14u         // -> PCM1808 BCK, PCM5102A BCK
#define AUDIO_PIN_LRCK      15u         // -> PCM1808 LRC, PCM5102A LRCK  (BCK+1)
#define AUDIO_PIN_DOUT      16u         // -> PCM5102A DIN. Claimed by the output
                                        //    module; see audio_claim_dout() below
#define AUDIO_PIN_DIN       17u         // <- PCM1808 DOUT

// ---- Buffering ---------------------------------------------------------
// Stereo frames per buffer. The processing deadline is one buffer period:
// 64 frames / 48000 Hz = 1.333 ms.
#define AUDIO_BLOCK_FRAMES  64u
#define AUDIO_BLOCK_SAMPLES (2u * AUDIO_BLOCK_FRAMES)   // interleaved L,R
#define AUDIO_NUM_BUFFERS   3u

typedef struct {
    uint32_t rx_blocks;   // RX DMA completions
    uint32_t tx_blocks;   // TX DMA completions
    uint32_t overruns;    // blocks handed over before the previous was taken
} audio_stats_t;

// Configures MCLK, the PIO state machine and the DMA ring, then starts them.
// Call after the system clock is final - the dividers are computed from it.
//
// Part-way through, once the state machine is configured and before it is
// started, it calls the selected output module's attach() hook. That is the
// one moment at which claiming DOUT is possible and safe, and it is why the
// hook exists - see module.h.
void audio_init(void);

// Function-select AUDIO_PIN_DOUT to PIO and drive it, handing the DAC data
// line to the I2S state machine.
//
// **This is the output module's call to make, not the core's.** The core
// points the shift register at the pin because the PIO program does an
// `out pins, 1` and has to shift somewhere; it does not decide that a DAC is
// fitted, or that the processed block should reach one. Both output modules in
// the tree do call it - no_output_module included, because a PCM5102A with a
// floating DIN is worse than one being fed zeroes - but a module for a box
// with no DAC at all would simply not, and GP16 would stay a plain input.
//
// Only valid from attach(). Before audio_init() there is no state machine to
// claim it for, and after the state machine is running the pin is already
// whatever it is going to be.
void audio_claim_dout(void);

// Returns the next buffer ready for in-place processing, or NULL if none is
// ready yet. AUDIO_BLOCK_SAMPLES interleaved 32-bit samples, left first.
// Samples are 24-bit left-justified: value = buf[i] >> 8.
int32_t *audio_take_block(void);

// Snapshot of the running counters.
void audio_get_stats(audio_stats_t *out);

// Actual configured frequencies, for reporting.
uint32_t audio_get_pio_clk_hz(void);
uint32_t audio_get_mclk_hz(void);
bool audio_dividers_are_integer(void);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_AUDIO_H
