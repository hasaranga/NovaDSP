#include "audio.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "i2s.pio.h"

// For the output module's attach() hook, called below. This names no module -
// modules_output() returns whichever one the build selected.
#include "module.h"

// Triple buffer pool. int32_t matches the 32-bit slots the PIO shifts.
static int32_t audio_buffers[AUDIO_NUM_BUFFERS][AUDIO_BLOCK_SAMPLES];

static PIO  i2s_pio;
static uint i2s_sm;

static int dma_rx_a, dma_rx_b, dma_tx_a, dma_tx_b;

// Ring position. Written by the ISR, read by audio_take_block().
static volatile int      rx_fill_idx   = 1;
static volatile int      tx_drain_idx  = 2;
static volatile int      ready_idx     = -1;
static volatile bool     ready_pending = false;
static volatile uint32_t stat_rx, stat_tx, stat_overruns;

static uint32_t pio_clk_hz, mclk_hz;
static bool     dividers_integer;

// Hand the just-filled buffer to the application and advance the ring.
static inline void rx_completed(int dma_ch) {
    int completed = rx_fill_idx;

    // If the previous block was never taken, the DSP missed its deadline.
    if (ready_pending) {
        stat_overruns++;
    }
    ready_idx = completed;
    ready_pending = true;

    rx_fill_idx = (completed + 1) % (int)AUDIO_NUM_BUFFERS;
    stat_rx++;

    // The chained partner is already filling the next buffer; point this one
    // at the buffer after that.
    dma_channel_set_write_addr(dma_ch,
        audio_buffers[(completed + 2) % (int)AUDIO_NUM_BUFFERS], false);
}

static inline void tx_completed(int dma_ch) {
    int completed = tx_drain_idx;
    tx_drain_idx = (completed + 1) % (int)AUDIO_NUM_BUFFERS;
    stat_tx++;

    dma_channel_set_read_addr(dma_ch,
        audio_buffers[(completed + 2) % (int)AUDIO_NUM_BUFFERS], false);
}

static void __isr audio_dma_isr(void) {
    uint32_t ints = dma_hw->ints0;

    if (ints & (1u << dma_rx_a)) { dma_hw->ints0 = 1u << dma_rx_a; rx_completed(dma_rx_a); }
    if (ints & (1u << dma_rx_b)) { dma_hw->ints0 = 1u << dma_rx_b; rx_completed(dma_rx_b); }
    if (ints & (1u << dma_tx_a)) { dma_hw->ints0 = 1u << dma_tx_a; tx_completed(dma_tx_a); }
    if (ints & (1u << dma_tx_b)) { dma_hw->ints0 = 1u << dma_tx_b; tx_completed(dma_tx_b); }
}

static void configure_rx(int ch, int chain_to, int32_t *dest) {
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(i2s_pio, i2s_sm, false));
    channel_config_set_chain_to(&c, chain_to);

    dma_channel_configure(ch, &c, dest, &i2s_pio->rxf[i2s_sm],
                          AUDIO_BLOCK_SAMPLES, false);
}

static void configure_tx(int ch, int chain_to, int32_t *src) {
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(i2s_pio, i2s_sm, true));
    channel_config_set_chain_to(&c, chain_to);

    dma_channel_configure(ch, &c, &i2s_pio->txf[i2s_sm], src,
                          AUDIO_BLOCK_SAMPLES, false);
}

void audio_init(void) {
    const uint32_t sys_hz = clock_get_hz(clk_sys);

    // PIO runs at 4 cycles/bit * 64 bits/frame = 256 * Fs, the same rate as
    // MCLK at 256 fs. Both are therefore the same integer divide of clk_sys.
    const uint32_t target_hz = AUDIO_SAMPLE_RATE * 256u;

    // 24.8 fixed point so we can see whether the divide came out exact.
    const uint32_t div_q8   = (uint32_t)(((uint64_t)sys_hz * 256u) / target_hz);
    const uint32_t div_int  = div_q8 >> 8;
    const uint8_t  div_frac = (uint8_t)(div_q8 & 0xFFu);

    dividers_integer = (div_frac == 0) && ((sys_hz % target_hz) == 0);
    pio_clk_hz = sys_hz / div_int;

    for (uint i = 0; i < AUDIO_NUM_BUFFERS; i++) {
        for (uint j = 0; j < AUDIO_BLOCK_SAMPLES; j++) {
            audio_buffers[i][j] = 0;
        }
    }

    // ---- MCLK on a hardware clock output, not PIO -----------------------
    // A dedicated CLK_GPOUT divider is integer here (25), where a PIO pin
    // toggle would need 12.5 and jitter the ADC's system clock.
    const uint32_t mclk_target = AUDIO_SAMPLE_RATE * AUDIO_MCLK_MULT;
    const uint32_t mclk_div    = sys_hz / mclk_target;
    clock_gpio_init_int_frac8(AUDIO_PIN_MCLK,
                              CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS,
                              mclk_div, 0);
    mclk_hz = sys_hz / mclk_div;

    // ---- PIO ------------------------------------------------------------
    i2s_pio = pio0;
    i2s_sm  = (uint)pio_claim_unused_sm(i2s_pio, true);
    uint offset = pio_add_program(i2s_pio, &i2s_program);
    i2s_program_init(i2s_pio, i2s_sm, offset,
                     AUDIO_PIN_DOUT, AUDIO_PIN_DIN, AUDIO_PIN_BCK,
                     div_int, div_frac);

    // ---- hand DOUT to the output module ---------------------------------
    // i2s_program_init() claimed the clocks and DIN and deliberately did not
    // claim DOUT: the DAC data line belongs to whichever output module this
    // build selected, and this is the one moment it can be claimed - the state
    // machine exists and is configured, and it has not started clocking yet.
    //
    // A module with no attach() hook simply leaves GP16 a plain input, which
    // is a legitimate thing for an output module that is not driving a DAC.
    if (modules_output()->attach) modules_output()->attach();

    // The input module gets the same moment, for the same reason. The I2S
    // input needs nothing here - i2s_program_init() already claimed DIN,
    // because that pin is the state machine's own - but an input with its own
    // front end claims it here rather than at init(), so that a build with no
    // source fitted leaves those pads alone.
    if (modules_input()->attach) modules_input()->attach();

    // ---- DMA ring -------------------------------------------------------
    dma_rx_a = dma_claim_unused_channel(true);
    dma_rx_b = dma_claim_unused_channel(true);
    dma_tx_a = dma_claim_unused_channel(true);
    dma_tx_b = dma_claim_unused_channel(true);

    // RX fills 1 then 2 then 0...; TX trails two buffers behind it.
    configure_rx(dma_rx_a, dma_rx_b, audio_buffers[1]);
    configure_rx(dma_rx_b, dma_rx_a, audio_buffers[2]);
    configure_tx(dma_tx_a, dma_tx_b, audio_buffers[2]);
    configure_tx(dma_tx_b, dma_tx_a, audio_buffers[0]);

    dma_channel_set_irq0_enabled(dma_rx_a, true);
    dma_channel_set_irq0_enabled(dma_rx_b, true);
    dma_channel_set_irq0_enabled(dma_tx_a, true);
    dma_channel_set_irq0_enabled(dma_tx_b, true);

    irq_set_exclusive_handler(DMA_IRQ_0, audio_dma_isr);
    irq_set_enabled(DMA_IRQ_0, true);

    // Pre-load the PIO TX FIFO with silence. Without this the TX DMA empties
    // the whole FIFO the instant it starts and runs a few words ahead of RX,
    // eating into the processing deadline.
    for (int i = 0; i < 4; i++) {
        pio_sm_put(i2s_pio, i2s_sm, 0);
    }

    // Start both DMA chains on the same cycle, then let the PIO clock them.
    dma_start_channel_mask((1u << dma_rx_a) | (1u << dma_tx_a));
    pio_sm_set_enabled(i2s_pio, i2s_sm, true);
}

int32_t *audio_take_block(void) {
    if (!ready_pending) {
        return NULL;
    }
    int idx = ready_idx;
    ready_pending = false;
    return audio_buffers[idx];
}

void audio_get_stats(audio_stats_t *out) {
    out->rx_blocks = stat_rx;
    out->tx_blocks = stat_tx;
    out->overruns  = stat_overruns;
}

void audio_claim_dout(void) {
    // Called from the output module's attach(), from inside audio_init()
    // above. The pio/sm handles are this file's, which is the whole reason
    // this is a function here rather than something the module does directly.
    i2s_claim_dout(i2s_pio, i2s_sm, AUDIO_PIN_DOUT);
}

uint32_t audio_get_pio_clk_hz(void)   { return pio_clk_hz; }
uint32_t audio_get_mclk_hz(void)      { return mclk_hz; }
bool     audio_dividers_are_integer(void) { return dividers_integer; }
