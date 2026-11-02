/*
 * encoder.h - rotary encoder: quadrature in PIO, push button in software.
 *
 * The quadrature decoder is a dedicated PIO state machine (encoder.pio) that
 * samples continuously and keeps its own count, so nothing has to be polled
 * fast enough to catch an edge and nothing interrupts core 0.
 *
 * All three lines idle high: the module has 10k pull-ups and 100 nF debounce
 * caps on A, B and the switch. That makes the falling edge fast and the rising
 * edge a ~1 ms ramp, which in turn sets a ceiling of roughly 6 rev/s on a
 * 20-detent encoder before the ramp eats the quarter-phase spacing. See the
 * Control I/O section of README.md.
 */

#ifndef NOVADSP_ENCODER_H
#define NOVADSP_ENCODER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// A and B must be consecutive: the PIO program reads them as one field.
#define ENC_PIN_A       18u
#define ENC_PIN_B       19u        // must be ENC_PIN_A + 1
#define ENC_PIN_PUSH    20u        // active low

// Quadrature counts per mechanical detent. Four is right for the usual
// 20-detent part, which completes one full A/B cycle between clicks. Set it to
// 1 if you want every quadrature edge to register.
#define ENC_COUNTS_PER_DETENT   4

// Milliseconds a press must be held before held_ms is worth acting on as a
// long press, for callers that want the distinction.
#define ENC_LONG_PRESS_MS       600u

typedef struct {
    int32_t  count;        // raw quadrature counts since encoder_init()
    int32_t  detents;      // count / ENC_COUNTS_PER_DETENT, accumulated
    int32_t  delta;        // detents moved since the previous poll, signed
    bool     pressed;      // button is down now
    bool     press_edge;   // went down since the previous poll
    bool     release_edge; // came up since the previous poll
    uint32_t held_ms;      // how long it has been down; 0 when up
} encoder_state_t;

// Claims a state machine on pio1 (pio0 belongs to the I2S) and starts it.
// Safe to call from either core, but the poller should own it afterwards.
void encoder_init(void);

// Reads the PIO count, folds it into detents and samples the button. Call it
// regularly - a few hundred Hz is plenty, because the state machine is doing
// the catching, not this. delta/press_edge/release_edge describe the interval
// since the previous call and are cleared by it.
void encoder_poll(encoder_state_t *out);

// Raw count straight from the state machine, baseline-corrected.
int32_t encoder_count(void);

// Rebases the count and detent accumulator to zero without disturbing the
// state machine.
void encoder_zero(void);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_ENCODER_H
