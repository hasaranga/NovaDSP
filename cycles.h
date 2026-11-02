/*
 * cycles.h - a free-running clk_sys cycle counter, for timing the DSP.
 *
 * SysTick, not DWT: the RP2350's Cortex-M33 is built without the DWT cycle
 * counter (M33_DWT_CTRL.NOCYCCNT reads 1), so CYCCNT is not available. SysTick
 * is, it is clocked from the processor clock, and nothing in the SDK uses it -
 * pico_stdlib's timing all comes off the hardware timer.
 *
 * It counts DOWN and is 24 bits, so:
 *   - elapsed is (start - now) & 0xFFFFFF, not (now - start)
 *   - the range is 16777215 cycles = 54.6 ms at 307.2 MHz, which is 41 block
 *     periods. Fine for timing one block; do not span a report interval with it.
 *
 * Per core. Only core 0 is instrumented here.
 */

#ifndef NOVADSP_CYCLES_H
#define NOVADSP_CYCLES_H

#include <stdint.h>
#include "hardware/structs/m33.h"

#define CYC_MASK 0x00FFFFFFu

// ENABLE | CLKSOURCE=processor clock. TICKINT left off: no exception.
#define CYC_CSR_RUN 0x00000005u

static inline void cyc_init(void) {
    m33_hw->syst_csr = 0;
    m33_hw->syst_rvr = CYC_MASK;
    m33_hw->syst_cvr = 0;          // any write clears it and reloads from RVR
    m33_hw->syst_csr = CYC_CSR_RUN;
}

static inline uint32_t cyc_now(void) {
    return m33_hw->syst_cvr;
}

static inline uint32_t cyc_since(uint32_t start) {
    return (start - m33_hw->syst_cvr) & CYC_MASK;
}

#endif // NOVADSP_CYCLES_H
