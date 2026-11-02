/*
 * fault.c - say that the firmware crashed, on a channel a crash cannot take.
 *
 * ---- why this exists ----------------------------------------------------
 *
 * The console is the first thing a crash removes, so it cannot be the thing
 * that reports one. An S/PDIF build produced no output whatsoever - not the
 * status line, not even the boot banner, which prints long before any audio
 * is decoded - while the board stayed enumerated on USB and picotool could
 * still reboot it. From the host that is indistinguishable from a board that
 * never booted, from a livelocked main loop, and from a broken serial reader,
 * and this project spent a long session mistaking each of those for another.
 *
 * The distinguishing fact is that **USB survives a hard fault**. pico_stdio_usb
 * pumps TinyUSB from a timer interrupt, so the device keeps answering the host
 * while the faulting core sits in the default handler's infinite loop. Hence:
 * enumerated, resettable, and completely silent.
 *
 * ---- what it does -------------------------------------------------------
 *
 * Overrides the SDK's weak fault handlers and blinks the LED fast and forever.
 * The running firmware blinks the same LED once a second from the status tick,
 * so the two are unmistakable across a room:
 *
 *     ~1 Hz, even          running normally
 *     ~10 Hz, frantic      hard fault - core 0 is dead, nothing else is coming
 *     dark, USB present    hung or livelocked, but not faulted
 *     dark, no USB         not booting at all, or no power
 *
 * **It deliberately does not print.** Anything that touches stdio from a fault
 * handler can take the stdio mutex, which the faulting code may already hold,
 * and a handler that deadlocks reports nothing at all. The LED needs one SIO
 * register write and cannot fail.
 *
 * The handlers are plain busy loops with no stack use beyond a couple of
 * registers, because the fault may well be a stack overflow.
 */

#include "hardware/gpio.h"
#include "hardware/structs/sio.h"

#include "fault.h"

static uint32_t s_led_mask;

void nova_fault_led_init(unsigned pin)
{
    s_led_mask = 1u << pin;
}

// Busy-wait without the timer, which may be part of whatever went wrong.
// Cycle-counted loosely; the exact rate does not matter, only that it is
// obviously faster than the one-a-second heartbeat.
static void spin(volatile uint32_t n)
{
    while (n--) __asm__ volatile ("nop");
}

static void blink_forever(void)
{
    if (!s_led_mask) return;          // never initialised; nothing to blink

    for (;;) {
        sio_hw->gpio_set = s_led_mask;
        spin(1500000);
        sio_hw->gpio_clr = s_led_mask;
        spin(1500000);
    }
}

// The SDK declares these weak, so defining them here replaces the defaults
// without touching any SDK file.
void isr_hardfault(void)  { blink_forever(); }
void isr_nmi(void)        { blink_forever(); }
void isr_svcall(void)     { blink_forever(); }
