#include "encoder.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "encoder.pio.h"   // generated from encoder.pio; the program inside is "quadrature"

// pio0 carries the I2S. pio1 is otherwise untouched, which matters more than
// usual here: encoder.pio declares .origin 0, so it needs the bottom of a PIO
// block's instruction memory to itself.
#define ENC_PIO         pio1

// Target state machine clock. The decode loop is 12 instructions at worst, so
// ~9.6 MHz samples the phases every ~1.25 us - three orders of magnitude
// faster than the 1 ms edges the RC filter produces, and slow enough that the
// SM is not thrashing the FIFO at clk_sys. Nothing here is critical; it just
// wants to be far below the edge rate and far above the step rate.
#define ENC_SM_CLK_HZ   9600000u

static uint     enc_sm;
static int32_t  enc_base;        // raw count at init / zero, subtracted out
static int32_t  enc_last_count;  // for the per-poll delta
static int32_t  enc_acc;         // sub-detent remainder
static int32_t  enc_detents;

static bool     btn_state;       // debounced level, true = pressed
static bool     btn_prev_raw;    // for the two-in-a-row confirm
static uint32_t btn_down_ms;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

void encoder_init(void)
{
    enc_sm = (uint)pio_claim_unused_sm(ENC_PIO, true);
    uint offset = pio_add_program(ENC_PIO, &quadrature_program);

    float div = (float)clock_get_hz(clk_sys) / (float)ENC_SM_CLK_HZ;
    quadrature_program_init(ENC_PIO, enc_sm, offset, ENC_PIN_A, div);

    // The switch has its own pull-up and debounce cap on the module, so this
    // is a plain input. gpio_init leaves the Schmitt trigger enabled, which is
    // what keeps the ~1 ms rising ramp from being seen as several edges.
    gpio_init(ENC_PIN_PUSH);
    gpio_set_dir(ENC_PIN_PUSH, GPIO_IN);

    // Let the state machine run one pass so its first sample is the real pin
    // state rather than the reset value, then take that as zero. This is what
    // absorbs the one spurious count the program can produce on entry.
    sleep_us(50);
    enc_base = quadrature_get_count(ENC_PIO, enc_sm);
    enc_last_count = 0;
    enc_acc = 0;
    enc_detents = 0;

    btn_state = btn_prev_raw = !gpio_get(ENC_PIN_PUSH);
    btn_down_ms = 0;
}

int32_t encoder_count(void)
{
    return quadrature_get_count(ENC_PIO, enc_sm) - enc_base;
}

void encoder_zero(void)
{
    enc_base += encoder_count();
    enc_last_count = 0;
    enc_acc = 0;
    enc_detents = 0;
}

void encoder_poll(encoder_state_t *out)
{
    int32_t count = encoder_count();

    // Fold raw counts into detents. Dividing the running total instead would
    // be asymmetric about zero - C truncates towards zero, so -3/4 and 3/4
    // both give 0, but a shift would floor and make one direction sticky.
    // Carrying the remainder keeps both directions identical.
    enc_acc += count - enc_last_count;
    enc_last_count = count;

    int32_t steps = enc_acc / ENC_COUNTS_PER_DETENT;
    enc_acc -= steps * ENC_COUNTS_PER_DETENT;
    enc_detents += steps;

    // The cap debounces in hardware; requiring two agreeing reads costs
    // nothing and covers a module that turns out not to populate it.
    bool raw = !gpio_get(ENC_PIN_PUSH);
    bool was = btn_state;
    if (raw == btn_prev_raw && raw != btn_state) {
        btn_state = raw;
        if (raw) btn_down_ms = now_ms();
    }
    btn_prev_raw = raw;

    out->count        = count;
    out->detents      = enc_detents;
    out->delta        = steps;
    out->pressed      = btn_state;
    out->press_edge   = btn_state && !was;
    out->release_edge = !btn_state && was;
    out->held_ms      = btn_state ? now_ms() - btn_down_ms : 0;
}
