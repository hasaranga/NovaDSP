/*
 * i2s_input_module - the PCM1808, on the state machine the DAC already uses.
 *
 * **Every hook here is NULL, and the descriptor is still not empty.** What it
 * says is: the samples are already in the buffer, this input has no clock of
 * its own to lock to, and it runs at exactly AUDIO_SAMPLE_RATE because the
 * board is the master.
 *
 * That is the same shape as i2s_output_module's empty write(), and for the
 * same hardware reason - one PIO state machine and one DMA ring carry both
 * directions, so the block handed to fill() is the block RX just wrote and
 * the block TX is about to drain. Copying it anywhere would be copying it
 * onto itself.
 *
 * The pins are claimed in i2s_program_init(): BCK, LRCK and DIN are the state
 * machine's own, because BCK/LRCK are what the ADC is slaved to and DIN is
 * where its samples arrive. There is no attach() here for the same reason
 * there is one in the output module - DOUT is a pin somebody chooses to
 * drive, DIN is a pin this state machine has to read.
 */

#include "module.h"

static const input_module_t k = {
    .name    = "i2s",

    // No init, no attach: audio.c's i2s_program_init() has already claimed
    // everything this input needs by the time either would run.
    .init    = NULL,
    .attach  = NULL,

    // NULL, not a no-op function. The core reads this as "the buffer already
    // holds the samples" and skips the call entirely - which is different
    // from an input that writes silence, and the distinction matters for a
    // build with no source at all.
    .fill    = NULL,

    .report  = NULL,

    // NULL locked() means permanently locked, NULL rate_hz() means
    // AUDIO_SAMPLE_RATE. Both are true of an ADC the board is clocking, and
    // saying so with a NULL rather than a function that returns a constant
    // keeps the status line's "is this input healthy" question meaningful
    // only where it has an answer.
    .locked  = NULL,
    .rate_hz = NULL,
};

const input_module_t *i2s_input_module(void) { return &k; }
