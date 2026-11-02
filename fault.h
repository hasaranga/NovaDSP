/*
 * fault.h - see fault.c. One call, from main(), before anything can crash.
 */

#ifndef NOVADSP_FAULT_H
#define NOVADSP_FAULT_H

#ifdef __cplusplus
extern "C" {
#endif

// Tell the fault handlers which pin to blink. Until this is called they do
// nothing, which is correct: a fault before the LED is configured would
// otherwise drive a pad that main() has not claimed yet.
void nova_fault_led_init(unsigned pin);

#ifdef __cplusplus
}
#endif

#endif // NOVADSP_FAULT_H
