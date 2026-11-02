# AGENTS.md

Guidance for AI coding agents working on NovaDSP. Human contributors will find
it useful too, but it is written for the failure modes an agent actually hits.

Read [README.md](README.md) first for what the firmware is. This file is about
how to change it without breaking it.

---

## The one-paragraph orientation

This is **hard real-time embedded audio** on a Raspberry Pi Pico 2 (RP2350,
Cortex-M33). Every 1.333 ms a block of 64 stereo frames must be processed and
handed back. Code that is merely *correct* can still be wrong here, because it
was too slow — and nothing in the type system will tell you. The firmware is
**modular**: effects, the display, the input and the output are separate
directories discovered by a glob, and the core names none of them.

---

## Non-negotiables

These are the rules that cause silent, hard-to-diagnose breakage when ignored.

### 1. `float` only. Never `double`.

The M33's FPU is **single precision**. A `double` is emulated in software and
costs hundreds of cycles, which in the audio path is a missed deadline that
presents as clicking, not as an error.

```c
float x = 2.0f;        // yes
float x = 2.0;         // no - promotes
y = sinf(x);           // yes
y = sin(x);            // no - takes and returns double
powf, expf, logf, sqrtf, tanf, fabsf, fmaxf   // the f-suffixed forms, always
```

The build pins **`-Wdouble-promotion`** so this is a warning rather than
something found by ear. **Do not silence it.** If you see it fire in code you
touched, you introduced a real defect.

A literal without `f` is the usual culprit, and it hides well inside an
otherwise-float expression: `k * 0.5` promotes the whole thing.

### 2. Anything reached from `process()` is real-time code

No `malloc`, no `free`, no `printf`, no file or flash access, no locks, no
unbounded loops. State is preallocated in the object. If you need a scratch
buffer, it is a static array sized at compile time — see `eq_module/module.cpp`.

The deadline is **1.333 ms for 64 frames**. `AUDIO_BLOCK_FRAMES` is in
`audio.h`. The `ovr` counter on the DSP STATUS page counts missed deadlines and
is the number that matters.

### 3. Do not touch the pin assignments or the clock tree

They look arbitrary and are not. MCLK must be on a CLK_GPOUT-capable pin; BCK
and LRCK must be consecutive because PIO side-set drives them as one field;
encoder A and B must be consecutive because the quadrature decoder requires it.
Every audio clock is an **integer** divisor of `clk_sys`, which is why there is
no fractional-divider jitter. README.md has the derivations.

Likewise **the 307.2 MHz overclock is load-bearing**, not a tuning knob: the
integer divisors fall out of that specific number.

### 4. Renaming a parameter wipes everyone's saved settings

The settings store is keyed by a **hash of the whole registered layout** —
every effect's name in registration order, its parameter count, and each
parameter's name, kind and range (`modules_layout_id()` in `modules.c`).

So changing a parameter's label, reordering effects, or changing a range
invalidates **every stored record, for every effect**, not just the one you
touched. That is the mechanism working as designed — a stored curve that no
longer means what it meant should not come back — but it is a user-visible
consequence and it belongs in your change description.

The reverse case is the trap: **a parameter whose *meaning* changed while its
name, kind and range did not** is invisible to the hash. That is what
`-DNOVA_SETTINGS_SALT=` exists for, and it is the only correct use of it.

### 5. Modules do not include each other

A module's own headers are reachable only from inside that module. This is
enforced by the include paths in `nova_modules.cmake`, deliberately. If you find
yourself wanting `#include "../other_module/thing.h"`, the design answer is
that the thing belongs in the core or is duplicated — ask before doing either.

### 6. `-Wno-missing-field-initializers` is off on purpose

Every effect builds its `effect_t` with designated initialisers and omits
`flags` and `slot_effect`. C++ value-initialises what an aggregate initialiser
omits, so those are zero, which is correct for both. **Do not "fix" this** by
adding `= 0` in every module — the comment in `CMakeLists.txt` explains why the
warning is disabled rather than satisfied.

---

## Building and verifying

### Build

Use the scripts, not raw `cmake`:

```bat
make_firmware.bat dist oled output:i2s all
```

**Why it matters: `NOVA_EFFECT_<NAME>` is a cached CMake option.** An effect you
simply stop naming on a `cmake` command line stays `ON` in a build directory
that already had it, and you get a firmware containing an effect you did not ask
for with no line of output saying so. `make_firmware.bat` always passes an
explicit `ON` or `OFF` for every effect in the tree.

```bat
make_firmware.bat list          :: what this tree has
make_firmware.bat help          :: every option
build.bat dist                  :: the everyday configuration
build-variants.bat              :: nine combinations, proves the plug points unplug
```

> **Adding a whole new module directory needs one manual reconfigure**, because
> a glob cannot see a file that did not exist when it ran. Delete the build
> directory or re-run `cmake -B build`. Editing an existing `module.cmake`
> reconfigures by itself.

### A fast syntax check without a full build

For a DSP engine that has no SDK dependencies, this catches the warnings that
matter in about a second:

```bash
arm-none-eabi-g++ -fsyntax-only -Wall -Wextra -Wdouble-promotion \
  -mcpu=cortex-m33 -mfloat-abi=softfp -mfpu=fpv5-sp-d16 \
  -Ieq_module/equalizer eq_module/equalizer/Equalizer.cpp
```

It is not a substitute for building — it will not catch a link error or a
module wiring mistake — but it turns the `-Wdouble-promotion` loop from minutes
into seconds.

### Host tests — use these, they are the real safety net

Several engines have **host test programs that run on your PC**, no board
required. They are deliberately *not* in `module.cmake` SOURCES.

**Each file carries its own build command in its header comment, and that is
the authority** — note that they are not uniform: the S/PDIF and Leveller ones
are run from inside their own directory, while the AGC and Noise Gate ones give
paths relative to the repo root. Read the header, do not pattern-match from
another test.

| Test | Covers |
|---|---|
| `leveller_module/leveller/test_leveller.cpp` | 43 checks — curve, ballistics, bypass, denormals |
| `agc_module/agc/test_autogain.cpp` | levelling and the brick-wall limiter |
| `noisegate_module/noisegate/test_noisegate.cpp` | gate thresholds and timing |
| `spdif_input_module/asrc/test_asrc.cpp` | 26 checks — resampler images and passband |
| `spdif_input_module/asrc/test_rate.cpp` | rate estimator under drift |
| `spdif_input_module/asrc/test_spdif.cpp` | the biphase-mark decoder |
| `spdif_input_module/asrc/test_spdif_ui.cpp` | assembles the real `.pio` with pioasm and runs it in an emulator |
| `spdif_output_module/tx/test_tx.cpp` | transmitter, decoded back by the receiver |

For example, from `leveller_module/leveller/` with GCC or Clang:

```bash
g++ -O2 -Wall -Wextra -o test_leveller test_leveller.cpp Leveller.cpp && ./test_leveller
```

or with MSVC, from a developer prompt (`vcvarsall.bat x64`):

```bat
cl /nologo /O2 /EHsc /W4 /std:c++17 test_leveller.cpp Leveller.cpp /Fe:tl.exe && tl.exe
```

Each prints a list of named checks and a `N checks, 0 failed` line. **If you
change a DSP engine that has a test, run it and paste the tail of the output.**

Do not commit the built `.exe` or `.obj` — `.gitignore` covers them.

### What you can and cannot verify

| Can | Cannot, without hardware |
|---|---|
| It compiles, warning-clean | It sounds right |
| Host tests pass | It meets the deadline on the board |
| Sizes: `arm-none-eabi-size` on the `.elf` | The panel draws correctly |
| Module combinations still link | S/PDIF locks to a real source |

**Say which of these you actually did.** "Builds clean and the 43 leveller
checks pass; not tested on hardware" is a useful report. "Fixed" is not.

---

## Adding an effect

A directory with a `module.c` (or `.cpp`) and a `module.cmake`. **No core file
is touched** — there is no `EFFECT_MAX` to raise and no registry to edit.

Copy `thru_module/` — it is the smallest complete example — and read
[module.h](module.h) and [effect.h](effect.h).

```cmake
nova_module(
    TYPE     EFFECT
    NAME     myfx            # the token make_firmware.bat accepts
    ACCESSOR myfx_module     # the one extern "C" function the core links
    ORDER    35              # menu order; low numbers come first
    DESC     "what it does"
    SOURCES  module.cpp engine/MyFx.cpp
)
```

The effect interface is:

```c
typedef struct {
    const char           *name;         // menu label
    int                   param_count;
    const effect_param_t *params;
    void (*enter)(void);                // optional; core 0, on becoming active
    void (*process)(int32_t *buf);      // the block hook, core 0 only
    int  (*get)(int param);
    void (*set)(int param, int value);  // must clamp
    ...
} effect_t;
```

Things to get right:

- **`set()` must clamp.** It is reachable from the console as well as the knob.
- **Parameters are integers, one detent per unit.** The *glue* owns any unit
  translation to the engine's real range — that is what `bbe_module/module.cpp`
  demonstrates and why it exists as a separate file from the engine.
- **Build the descriptor from the engine's own tables** where you can, so the
  menu cannot drift from the filter set. `eq_module/module.cpp` builds its
  labels and ranges at init from `Equalizer::kBandLabel` and the gain limits.
- **A parameter omitted from the descriptor is invisible everywhere** — off the
  panel, absent from flash, outside the layout hash. That is one omission with
  three consistent consequences, and it is how `leveller_module` keeps five
  engine parameters off the knob. There is no "hidden" flag and there does not
  need to be.
- **Registration order is `ORDER`, and the first registered is what the box
  boots on.** Passthru is `ORDER 5` for that reason. Do not displace it without
  saying so.

---

## Code style

The house style here is unusual and worth matching rather than overriding.

**Comments explain *why*, at length, and often carry measurements.** The
codebase treats a comment as the place where a decision and the evidence for it
are recorded — including decisions that were tried and abandoned. Look at
`eq_module/equalizer/Equalizer.h` or `flash.bat`'s note about picotool exiting 0
without writing anything.

So:

- **Do not strip existing comments to make a diff smaller.** If a comment is now
  wrong, fix the comment — that is part of the change, not optional cleanup.
- **When you change a measured claim, re-measure it.** The tree is full of
  them — the S/PDIF receiver at "9 % of the block deadline", the EQ at
  "468 bytes per instance", a band layout's summed response in dB — and each
  one is something the next person will rely on rather than re-derive.
- Match the surrounding density. A one-line fix does not need an essay; a new
  filter topology does.
- Prose in comments and docs uses `-` and `--` rather than Unicode dashes in
  `.c`/`.h` files, and full Unicode in `.md` files. Follow the file you are in.

**Documentation lives next to the thing.** Each module has its own `README.md`.
If you change what a module does, that file is part of the change.

---

## Repository facts that surprise people

- **`oled_vis/oled_anim*.c` are generated artwork**, ~500 KB each, produced by
  the Go tool in the same directory from video. Do not hand-edit them and do not
  reformat them.
- **`.exe` files are never committed.** Host tests and `animtool` build locally.
- **There is no CI.** Verification is whatever you ran and reported.
- **`pico-env.local.bat` is the place for machine-specific paths.** It is
  gitignored and read before anything else. Never edit `pico-env.bat` to hardcode
  a local path.
- **The serial display only builds in a `dist` image**, because it draws on the
  USB port a dev build's console prints to. The module says so with an `#error`.

---

## Before you report done

1. Did it **build**? Which configuration? Paste the `NovaDSP: effects` line.
2. Did **`-Wdouble-promotion`** stay silent?
3. If you touched an engine with a host test, did you **run it**?
4. Did you change a **parameter name, range, or effect order**? If so, say that
   stored settings will be discarded on the next boot.
5. Did you update the **module's own README** and any measured figures you
   invalidated?
6. Are there **build directories or `.exe` files** in your diff? There should
   not be.

State plainly what you did not verify. On this codebase, "compiles" and "works"
are genuinely different claims.
