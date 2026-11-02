/*
 * spdif_output_module - the processed block, out as S/PDIF.
 *
 * The counterpart to spdif_input_module and by far the simpler half. Every
 * piece of machinery over there - the ASRC, the rate estimator, the lock
 * states, the fade policy - exists because a receiver has to follow somebody
 * else's clock. **Transmitting, this box is the master**, the divider is
 * exactly 50, and there is nothing to track.
 *
 * ---- what write() actually does -----------------------------------------
 *
 * It encodes and hands over. It does *not* claim DOUT, and this is the case
 * module.h describes as an output module that is not driving the I2S DAC:
 * attach() claims GP22 and leaves GP16 a plain input.
 *
 * **That means the PCM5102A's DIN floats in this build**, which is why
 * `output:spdif` is a digital-only configuration. A board wanting both
 * analogue and digital out wants two output modules running at once, and the
 * interface has exactly one - see the note at the end of this file.
 *
 * ---- the ring, and why it cannot slip -----------------------------------
 *
 * A frame is exactly four words (see tx/SpdifTx.h), so a block is exactly
 * AUDIO_BLOCK_FRAMES * 4 = 256 words, and the DMA drains one block's worth in
 * exactly one block period. Both sides are divided from the same clk_sys, so
 * the producer and the consumer are locked together by construction rather
 * than by a control loop: there is no drift here to correct.
 *
 * The ring holds four blocks so that a late write() has somewhere to land.
 */

#include "module.h"
#include "audio.h"
#include "cycles.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "spdif_tx.pio.h"
#include "tx/SpdifTx.h"

// ---- pins and sizing ----------------------------------------------------

// GP22. Free on this board, and away from GP13's MCLK for the same reason the
// receiver's GP1 is - a 6.144 MHz edge-rich line next to an ADC's system
// clock is how jitter gets into a conversion.
//
// Deliberately *not* adjacent to the receiver's GP1: an `input:spdif
// output:spdif` build has a transmitter running at 6.144 MHz beside a
// receiver trying to resolve 4-sample runs, and putting them at opposite ends
// of the header costs nothing.
#define SPDIF_TX_PIN 22u

#define SPDIF_TX_BLOCK_WORDS (AUDIO_BLOCK_FRAMES * SPDIF_TX_WORDS_PER_FRAME)
#define SPDIF_TX_SLOTS       4u
#define SPDIF_TX_RING_WORDS  (SPDIF_TX_BLOCK_WORDS * SPDIF_TX_SLOTS)

static uint32_t s_ring[SPDIF_TX_RING_WORDS]
    __attribute__((aligned(SPDIF_TX_RING_WORDS * sizeof(uint32_t))));

static SpdifTx s_tx;

static PIO   s_pio;
static uint  s_sm;
static int   s_dma = -1;
static uint  s_slot;
static bool  s_running;

// ---- what write() costs, which this module never had --------------------
//
// **`dsp` on the status line times effect_process_block() and nothing else.**
// That is how the receiver's decoder - heavy enough to livelock core 0 - read
// as "dsp 12.6 %" for a whole session while `ovr` climbed 440 a second, and
// `SESSION.md` says in as many words not to let `write()` be the second
// untimed thing in the block path. It was, until a TOSLINK went on GP22 and
// there was finally a reason to look.
//
// Costs a few adds and one cycle read per block. Off in dist either way,
// because NOVA_TELEMETRY is what actually gates it.
#ifndef SPDIF_TX_DIAG
#define SPDIF_TX_DIAG 1
#endif

#if NOVA_TELEMETRY && SPDIF_TX_DIAG

static uint32_t s_diag_cyc_sum;
static uint32_t s_diag_cyc_max;
static uint32_t s_diag_calls;
static uint32_t s_diag_blocks;

// The producer/consumer separation, in slots. **This module claims it cannot
// slip** - both sides are divided from the same clk_sys and each move one slot
// per block period, so write() should land two slots ahead of the DMA for
// ever. That is an argument, and this tree has a file full of what happens to
// arguments nobody measured. If `gap` is ever anything but 2, the claim is
// wrong and the ring is not the safe structure it is described as.
static uint32_t s_diag_gap_min;
static uint32_t s_diag_gap_max;

// 750 * 64 / 48000 is exactly one second, so the rates below need no division.
#define SPDIF_TX_DIAG_PERIOD 750u

static void tx_diag_report(void)
{
    if (!s_running) {
        NOVA_LOG("spdif-tx: NOT RUNNING - PIO or DMA claim failed at attach()\n");
        return;
    }

    const float cyc_us = (float)clock_get_hz(clk_sys) / 1e6f;
    const float budget = 1e6f * (float)AUDIO_BLOCK_FRAMES / (float)AUDIO_SAMPLE_RATE;
    const float avg_us = s_diag_calls
                       ? (float)s_diag_cyc_sum / (float)s_diag_calls / cyc_us : 0.0f;
    const float max_us = (float)s_diag_cyc_max / cyc_us;

    // Percentages are against the same 1333 us deadline `dsp` and the
    // receiver's `fill` are measured against, so the three add up by eye.
    NOVA_LOG("spdif-tx: write %.1f/%.1f us %.1f%% of deadline | %lu calls/s, "
             "%.0f cyc/frame\n",
             (double)avg_us, (double)max_us, (double)(100.0f * max_us / budget),
             (unsigned long)s_diag_calls,
             (double)(s_diag_calls
                      ? (float)s_diag_cyc_sum
                        / (float)(s_diag_calls * AUDIO_BLOCK_FRAMES) : 0.0f));

    NOVA_LOG("          slot gap %lu..%lu (must be 2; anything else means the\n"
             "          ring slipped and the 'cannot drift' claim is wrong)\n",
             (unsigned long)s_diag_gap_min, (unsigned long)s_diag_gap_max);

    s_diag_cyc_sum = s_diag_cyc_max = s_diag_calls = 0;
    s_diag_gap_min = 0xFFFFFFFFu;
    s_diag_gap_max = 0;
}

#endif // NOVA_TELEMETRY && SPDIF_TX_DIAG

// ---- lifecycle ----------------------------------------------------------

static void spdif_out_init(void)
{
    s_tx.init();
    s_slot    = 0;
    s_running = false;

#if NOVA_TELEMETRY && SPDIF_TX_DIAG
    // Starts at the top so the first minimum is the first reading rather than
    // a zero nobody measured.
    s_diag_gap_min = 0xFFFFFFFFu;
    s_diag_gap_max = 0;
#endif
}

static void spdif_out_attach(void)
{
    // **audio_claim_dout() is deliberately not called.** GP16 stays a plain
    // input, because this build's audio is leaving on GP22 and a PCM5102A
    // being fed a floating line is not this module's business to arrange.
    s_pio = pio2;

    if (!pio_can_add_program(s_pio, &spdif_tx_program)) return;

    const uint off = pio_add_program(s_pio, &spdif_tx_program);
    const int  sm  = pio_claim_unused_sm(s_pio, false);
    if (sm < 0) return;
    s_sm = (uint)sm;

    spdif_tx_program_init(s_pio, s_sm, off, SPDIF_TX_PIN, spdif_tx_ui_rate_hz());

    // Prime every slot with encoded digital silence before the DMA starts.
    //
    // **Not zeroed** - a zeroed ring is a stuck line, not a stream, and a
    // receiver would see no carrier until the first block arrived. Encoded
    // silence is a legal S/PDIF stream carrying zero samples, so a downstream
    // DAC locks once at power-on and never has to again. That is the same
    // property spdif_input_module works to give the DAC on this board, seen
    // from the transmitting end.
    {
        int32_t quiet[AUDIO_BLOCK_SAMPLES] = {0};
        for (uint i = 0; i < SPDIF_TX_SLOTS; ++i)
            s_tx.encode(quiet, (int)AUDIO_BLOCK_FRAMES,
                        &s_ring[i * SPDIF_TX_BLOCK_WORDS],
                        (int)SPDIF_TX_BLOCK_WORDS);
    }

    s_dma = dma_claim_unused_channel(false);
    if (s_dma < 0) return;

    dma_channel_config c = dma_channel_get_default_config((uint)s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true));

    // Wrap the read address inside the ring: 1024 words = 4096 bytes = 2^12.
    channel_config_set_ring(&c, false, 12);

    dma_channel_configure((uint)s_dma, &c, &s_pio->txf[s_sm], s_ring,
                          0xFFFFFFFFu, true);

    pio_sm_set_enabled(s_pio, s_sm, true);
    s_running = true;
}

// ---- the block path -----------------------------------------------------

static void spdif_out_write(int32_t *buf)
{
    if (!s_running) return;

#if NOVA_TELEMETRY && SPDIF_TX_DIAG
    const uint32_t t0 = cyc_now();
#endif

    // Write into the slot furthest from the one the DMA is reading. Producer
    // and consumer are both divided from clk_sys and each move one slot per
    // block period, so this stays two slots ahead for ever - there is no
    // drift here to manage, which is the whole difference between this
    // module and the input one.
    const uint32_t r = (uint32_t)(((uintptr_t)dma_hw->ch[s_dma].read_addr
                                   - (uintptr_t)s_ring) / sizeof(uint32_t));
    const uint reading = (uint)(r / SPDIF_TX_BLOCK_WORDS) % SPDIF_TX_SLOTS;
    s_slot = (reading + 2u) % SPDIF_TX_SLOTS;

    s_tx.encode(buf, (int)AUDIO_BLOCK_FRAMES,
                &s_ring[s_slot * SPDIF_TX_BLOCK_WORDS],
                (int)SPDIF_TX_BLOCK_WORDS);

#if NOVA_TELEMETRY && SPDIF_TX_DIAG
    const uint32_t cyc = cyc_since(t0);
    s_diag_cyc_sum += cyc;
    if (cyc > s_diag_cyc_max) s_diag_cyc_max = cyc;
    ++s_diag_calls;

    // How far the slot we are writing sits ahead of the one being read.
    const uint32_t gap = (s_slot - reading) & (SPDIF_TX_SLOTS - 1u);
    if (gap < s_diag_gap_min) s_diag_gap_min = gap;
    if (gap > s_diag_gap_max) s_diag_gap_max = gap;

    if (++s_diag_blocks >= SPDIF_TX_DIAG_PERIOD) {
        s_diag_blocks = 0;
        tx_diag_report();
    }
#endif
}

// ---- reporting ----------------------------------------------------------

static void spdif_out_report(void)
{
    NOVA_LOG("  output:  S/PDIF on GP%u, PIO2 at %u Hz UI (%u Hz frames)\n",
             SPDIF_TX_PIN, (unsigned)spdif_tx_ui_rate_hz(),
             (unsigned)AUDIO_SAMPLE_RATE);

    if (!s_running) {
        NOVA_LOG("           NOT RUNNING - PIO or DMA claim failed\n");
        return;
    }

    // No probing, and no claim that anything is listening. A transmitter
    // cannot tell whether a receiver is attached, so saying "ok" here would
    // be the same empty reassurance the SSD1306 banner used to give.
    NOVA_LOG("           consumer format, 24-bit, %u-word ring; GP16 left as an\n"
             "           input - this build does not drive the I2S DAC\n",
             (unsigned)SPDIF_TX_RING_WORDS);
    NOVA_LOG("           coax needs a divider to 0.5 Vpp; TOSLINK takes 3V3 direct\n");
}

// ---- descriptor ---------------------------------------------------------
//
// **There is one output module, so this build has no analogue out.** Wanting
// both would mean either a module that drives DOUT *and* encodes S/PDIF - a
// perfectly good thing to write, and the honest place for it, since it is one
// decision about where the block goes - or an interface change to a list of
// outputs. The list is the wrong answer for the same reason there is one
// display: the interface says "where does the block go", and two answers to
// that is two modules pretending to be one.

static const output_module_t k = {
    .name   = "spdif",
    .init   = spdif_out_init,
    .attach = spdif_out_attach,
    .write  = spdif_out_write,
    .report = spdif_out_report,
};

extern "C" const output_module_t *spdif_output_module(void) { return &k; }
