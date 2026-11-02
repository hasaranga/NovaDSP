/*
 * i2s_output_module - the PCM5102A, and an honest account of what this module
 * does and does not do.
 *
 * ---- the pin, which is the part that is really ours ----------------------
 *
 * **This module owns GP16.** attach() function-selects the pad to PIO and
 * drives it, handing the DAC data line to the I2S state machine. Nothing in
 * the core does that: audio.c points the state machine's shift register at the
 * pin, because the PIO program does an `out pins, 1` and has to shift
 * somewhere, and stops there. Whether a pin is driven at all - whether there
 * is a DAC on the end of it - is this module's business, and an output module
 * that is not driving the I2S DAC simply leaves attach() NULL and leaves GP16
 * a plain input.
 *
 * attach() runs from inside audio_init(), after the state machine is
 * configured and before it is started. That is the only moment the claim is
 * both possible and safe, which is why it is a separate hook from init().
 *
 * ---- write(), which is not ----------------------------------------------
 *
 * The ADC and the DAC share one PIO state machine and one triple-buffered DMA
 * ring: while the RX DMA fills buffer N, the effect works on N-1 in place, and
 * the TX DMA drains N-2. **The block handed to write() is already the buffer
 * the TX DMA will drain.** Getting it to the DAC is not something that happens
 * after processing; it is what processing in place means.
 *
 * So write() is empty, and that is the correct implementation rather than a
 * stub. If a second output ever wants a genuine copy - a USB audio endpoint, a
 * second I2S bus, a file - write() is where it goes, and it will find the
 * block still in the format the effect left it in: AUDIO_BLOCK_SAMPLES
 * interleaved 32-bit samples, 24-bit left-justified, left first.
 */

#include "module.h"
#include "audio.h"

// Core 0, from inside audio_init(). The one thing here that touches hardware.
static void i2s_attach(void)
{
    audio_claim_dout();
}

static void i2s_write(int32_t *buf)
{
    // Deliberately empty. See the header comment: the buffer *is* the TX
    // buffer. Copying it anywhere would be copying it onto itself.
    (void)buf;
}

static void i2s_report(void)
{
    NOVA_LOG("  output: I2S DAC on GP%u (DOUT, claimed here), "
             "sharing BCK GP%u / LRCK GP%u\n",
             AUDIO_PIN_DOUT, AUDIO_PIN_BCK, AUDIO_PIN_LRCK);
}

static const output_module_t kModule = {
    .name   = "i2s",
    .init   = NULL,
    .attach = i2s_attach,
    .write  = i2s_write,
    .report = i2s_report,
};

const output_module_t *i2s_output_module(void) { return &kModule; }
