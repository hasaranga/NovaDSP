/*
 * nova_build.h - which of the two builds this is.
 *
 * There are two, and the difference is entirely telemetry:
 *
 *   dev   (default)        everything. The USB console, the once-a-second
 *                          status line, the boot banner, the boot benchmark
 *                          and the single-key parameter control.
 *
 *   dist  (-DNOVA_DIST)    the audio path, the panel and the knob. No console
 *                          output at all, no benchmark, no key handling.
 *
 * ---- what dist does *not* remove, and why -------------------------------
 *
 * **USB stays enumerated.** It is tempting to turn `pico_enable_stdio_usb`
 * off in dist and reclaim the CDC stack, but that is also what provides the
 * reset interface `picotool load -f` uses - drop it and every future flash
 * needs the BOOTSEL button held while the board is plugged in. A device that
 * is awkward to update is a worse outcome than a few KB, so dist keeps the
 * interface and only stops writing to it. Turn it off in CMakeLists.txt if a
 * particular build genuinely wants the port gone.
 *
 * **The measurements themselves stay.** `NOVA_LOG` removes the *printing* of
 * the block rate, the overrun count, the levels and the DSP time - it does
 * not remove the counters. They still feed DSP STATUS on the panel, which is
 * the whole reason that page exists, and the numbers there are as live in
 * dist as they are in dev.
 *
 * ---- using it -----------------------------------------------------------
 *
 * Print through NOVA_LOG rather than printf anywhere that runs on core 0
 * after start-up. In dist it compiles to nothing at all, so the format string
 * does not reach .rodata either - which is most of what dist actually saves.
 *
 * `#if NOVA_TELEMETRY` guards whole blocks that exist only to talk to the
 * console: the benchmark, the key handler, the enumeration wait.
 *
 * Core 1 must not use either of these. It never could use printf - stdio's
 * mutex is shared with core 0's status line, and blocking on it would put the
 * audio deadline at the mercy of the USB host. sh1106_printf() is the one it
 * has, and that formats into a stack buffer and never touches stdout.
 */

#ifndef NOVADSP_BUILD_H
#define NOVADSP_BUILD_H

#if defined(NOVA_DIST)
#define NOVA_TELEMETRY 0
#else
#define NOVA_TELEMETRY 1
#endif

#if NOVA_TELEMETRY

#include <stdio.h>
#define NOVA_LOG(...)   printf(__VA_ARGS__)
#define NOVA_BUILD_NAME "dev"

#else

// The arguments are **not evaluated** and **not type-checked**, and that is
// the point - it is what keeps the format strings out of .rodata, which is
// most of what dist actually saves. It does mean a NOVA_LOG with a broken
// argument list compiles clean here and fails in dev, so build dev first;
// build.bat does both in that order for this reason.
//
// It also means NOVA_LOG must never be handed an argument with a side effect.
//
// **`sizeof` is why the arguments can be mentioned without being evaluated.**
// A plain `((void)0)` discarded them so completely that a variable computed
// only in order to be printed came out unused in dist and not in dev - twelve
// of them, which pinning -Wall -Wextra finally made visible. `sizeof` puts its
// operand in an unevaluated context: the call below is type-checked and its
// arguments count as used, and **nothing is emitted** - no call, no format
// string, no evaluation of anything.
//
// **Verified by size, not by argument**: dist text and bss are byte-identical
// to the `((void)0)` this replaced. If that ever stops being true, the format
// strings are back in .rodata and most of what dist saves has gone with them.
//
// It is a variadic sink rather than the obvious `(void)sizeof((__VA_ARGS__, 0))`
// because the comma form makes every argument the discarded left operand of a
// comma expression, and -Wunused-value then fires on all of them - about ninety
// warnings traded for twelve. It is not `if (0) printf(...)` either: that needs
// <stdio.h> in dist, and dist not having it is what catches code relying on
// that transitive include, which is otherwise invisible in dev forever.
static inline int nova_log_sink(const char *fmt, ...) { (void)fmt; return 0; }
#define NOVA_LOG(...)   ((void)sizeof(nova_log_sink(__VA_ARGS__)))
#define NOVA_BUILD_NAME "dist"

#endif

#endif // NOVADSP_BUILD_H
