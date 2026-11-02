// PioSim.h - a small RP2350 PIO state machine emulator. **Host tests only.**
//
// Nothing in a firmware image includes this.
//
// ---- why this exists ----------------------------------------------------
//
// `spdif_rx.pio` was one instruction on the argument that **PIO cannot be
// tested on the host**, and that argument was right about the consequence: a
// decoder written in PIO is unverifiable until the board is in front of you,
// and this project has a file full of what happens to claims that were never
// measured.
//
// It was wrong about the premise. A PIO state machine is a 16-bit instruction
// word, five registers and a shift register, and the subset a decoder needs -
// JMP with its eight conditions, IN, SET, MOV - is about a hundred lines to
// execute. So `spdif_rx_ui.pio` is assembled by **the SDK's own pioasm**, at
// test time, and the instruction words that come out are the ones run here.
//
// That is a real check on the shipped program and not a model of it: a delay
// slot edited in the .pio changes what this emulator executes. What it cannot
// check is anything outside the state machine - the clock divider, the pad
// synchroniser's real timing, DMA, and whether GP1 is connected to anything.
// Those still need the board, and spdif_input_module/README.md says so.
//
// ---- what is modelled ---------------------------------------------------
//
//   * one instruction per cycle, plus its delay field;
//   * the input pin, through a **two-cycle synchroniser**, which is what the
//     pad does and what the program's timing arithmetic assumes;
//   * ISR shifting (either direction), the shift counter and autopush;
//   * X, Y, the PC, and wrapping.
//
// What is deliberately *not* modelled: the TX path, IRQs, side-set, DMA, and
// FIFO depth - pushes go to an unbounded sink. A real FIFO overflow stalls the
// state machine and loses UI phase; the decoder recovers by re-hunting, and
// the test that matters for that is the dropped-UI one in test_spdif_ui.cpp,
// which injects the fault directly rather than through a queue model.

#ifndef NOVADSP_PIOSIM_H
#define NOVADSP_PIOSIM_H

#include <stdint.h>
#include <stdio.h>

class PioSim
{
public:
    // `prog` is the assembled program, `n` its length. The program is loaded
    // at offset 0, which is what the module does and what makes the absolute
    // JMP targets pioasm emitted correct.
    void load(const uint16_t *prog, int n)
    {
        m_n = n;
        for (int i = 0; i < n && i < 32; ++i) m_prog[i] = prog[i];
        m_wrapTarget = 0;
        m_wrap       = n - 1;
        reset();
    }

    // shiftRight: ISR shifts right, data entering at the MSB - which is what
    // makes the first UI of a word land in bit 0. push_threshold 32.
    void configure(bool shiftRight, int pushThreshold, bool autopush)
    {
        m_shiftRight = shiftRight;
        m_thresh     = pushThreshold;
        m_autopush   = autopush;
    }

    void reset()
    {
        m_pc = 0; m_x = 0; m_y = 0;
        m_isr = 0; m_isrCount = 0; m_osr = 0;
        m_delay = 0;
        m_sync0 = m_sync1 = 0;
        m_bad = 0;
    }

    // Run one state machine cycle against the raw pin level. The level is put
    // through the synchroniser here, so callers pass the true waveform.
    //
    // Returns 1 and writes *word if a push happened this cycle.
    int cycle(int rawLevel, uint32_t *word)
    {
        const int pin = m_sync1;            // what the program sees this cycle
        m_sync1 = m_sync0;
        m_sync0 = rawLevel;

        if (m_delay > 0) { --m_delay; return 0; }

        const uint16_t ins = m_prog[m_pc];
        const int      del = (ins >> 8) & 0x1F;
        const int      op  = (ins >> 13) & 0x7;

        uint32_t next = (m_pc == (uint32_t)m_wrap) ? (uint32_t)m_wrapTarget
                                                   : m_pc + 1u;
        int pushed = 0;

        switch (op) {
        case 0: {                            // JMP
            const int cond = (ins >> 5) & 0x7;
            const uint32_t addr = ins & 0x1F;
            bool take = false;
            switch (cond) {
            case 0: take = true;                       break;
            case 1: take = (m_x == 0);                 break;
            case 2: take = (m_x != 0); --m_x;          break;   // X--
            case 3: take = (m_y == 0);                 break;
            case 4: take = (m_y != 0); --m_y;          break;   // Y--
            case 5: take = (m_x != m_y);               break;
            case 6: take = (pin != 0);                 break;   // PIN
            case 7: take = false; ++m_bad;             break;   // !OSRE, unused
            }
            if (take) next = addr;
            break;
        }

        case 2: {                            // IN
            const int src   = (ins >> 5) & 0x7;
            int       count = ins & 0x1F;
            if (count == 0) count = 32;

            uint32_t data;
            switch (src) {
            case 0: data = (uint32_t)pin;  break;      // PINS (one pin here)
            case 1: data = m_x;            break;
            case 2: data = m_y;            break;
            case 3: data = 0;              break;      // NULL
            case 6: data = m_isr;          break;
            case 7: data = m_osr;          break;
            default: data = 0; ++m_bad;    break;
            }

            const uint32_t mask = (count >= 32) ? 0xFFFFFFFFu
                                                : ((1u << count) - 1u);
            data &= mask;

            if (m_shiftRight)
                m_isr = (count >= 32) ? data
                                      : ((m_isr >> count) | (data << (32 - count)));
            else
                m_isr = (count >= 32) ? data : ((m_isr << count) | data);

            m_isrCount += count;
            if (m_isrCount > 32) m_isrCount = 32;

            if (m_autopush && m_isrCount >= m_thresh) {
                *word      = m_isr;
                pushed     = 1;
                m_isr      = 0;
                m_isrCount = 0;
            }
            break;
        }

        case 5: {                            // MOV
            const int dst = (ins >> 5) & 0x7;
            const int mop = (ins >> 3) & 0x3;
            const int src = ins & 0x7;

            uint32_t v;
            switch (src) {
            case 0: v = (uint32_t)pin; break;
            case 1: v = m_x;           break;
            case 2: v = m_y;           break;
            case 3: v = 0;             break;
            case 6: v = m_isr;         break;
            case 7: v = m_osr;         break;
            default: v = 0; ++m_bad;   break;
            }
            if (mop == 1) v = ~v;
            else if (mop == 2) v = bitrev(v);

            switch (dst) {
            case 1: m_x = v;   break;
            case 2: m_y = v;   break;
            case 6: m_isr = v; m_isrCount = 0; break;
            case 7: m_osr = v; break;
            case 0: break;                       // PINS - no outputs here
            default: ++m_bad; break;
            }
            break;
        }

        case 7: {                            // SET
            const int dst  = (ins >> 5) & 0x7;
            const uint32_t data = ins & 0x1F;
            switch (dst) {
            case 1: m_x = data; break;
            case 2: m_y = data; break;
            case 0: case 4: break;               // PINS / PINDIRS - unused
            default: ++m_bad; break;
            }
            break;
        }

        default:
            // WAIT, OUT, PUSH/PULL, IRQ. None are in this program; reaching
            // one means the .pio changed under the emulator, and a silent
            // no-op would show up as a decode failure fifty lines away.
            ++m_bad;
            break;
        }

        m_pc    = next;
        m_delay = del;
        return pushed;
    }

    // Instructions the emulator did not understand. **Must be 0**; the suite
    // checks it rather than trusting it.
    int unsupported() const { return m_bad; }

private:
    static uint32_t bitrev(uint32_t v)
    {
        uint32_t r = 0;
        for (int i = 0; i < 32; ++i) { r = (r << 1) | (v & 1u); v >>= 1; }
        return r;
    }

    uint16_t m_prog[32];
    int      m_n = 0;
    int      m_wrapTarget = 0, m_wrap = 0;

    uint32_t m_pc = 0, m_x = 0, m_y = 0, m_isr = 0, m_osr = 0;
    int      m_isrCount = 0;
    int      m_delay = 0;

    int      m_sync0 = 0, m_sync1 = 0;

    bool     m_shiftRight = true;
    int      m_thresh = 32;
    bool     m_autopush = true;

    int      m_bad = 0;
};

// Load a program from pioasm's `-o hex` output: one 16-bit instruction per
// line, hex, no prefix. Returns the instruction count, or -1.
//
// **The test assembles the shipped .pio rather than carrying a copy of the
// encoding**, so an edit to the program reaches the emulator and a stale
// header cannot quietly diverge from it.
static inline int pio_load_hex(const char *path, uint16_t *out, int maxN)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int n = 0;
    char line[64];
    while (n < maxN && fgets(line, sizeof line, f)) {
        unsigned v = 0;
        if (sscanf(line, "%x", &v) == 1) out[n++] = (uint16_t)v;
    }
    fclose(f);
    return n;
}

#endif // NOVADSP_PIOSIM_H
