# NovaDSP

Real-time audio DSP firmware for the **Raspberry Pi Pico 2 / RP2350**.

48 kHz, 24-bit stereo in and out, with a graphic EQ, a BBE-style maximizer, a
noise gate, auto gain and a leveller — driven from a rotary encoder and a
128x64 OLED, or from a serial terminal, or from nothing at all.

**Everything is a module.** Effects, the display, the input and the output are
all chosen at build time, and the core knows none of them by name. A build can
be a full box with a panel and every effect, or a headless S/PDIF-in to
S/PDIF-out processor with one.

- **[UserManual.md](UserManual.md)** — how to *use* the box: the knob, the menu,
  every effect and what its controls do. Start there if you have one built.
- This file is the engineering side: pins, building, and how the modular
  firmware fits together.
- Each module has its own `README.md` with the detail for that module.
- **[AGENTS.md](AGENTS.md)** — conventions and constraints for contributors,
  written for AI coding agents but useful to anyone: the real-time rules, the
  host tests, and what a change here can and cannot be verified to have done.

---

## Contents

- [What it does](#what-it-does)
- [Hardware](#hardware) — [pins](#pin-assignments), [codec strapping](#codec-strapping)
- [Building](#building)
- [Modules](#modules)
- [Effects](#effects)
- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [License](#license)

---

## What it does

- **48 kHz / 24-bit stereo**, in and out.
- **Core at 307.2 MHz**, Release / `-O3`, **executing from SRAM** rather than
  XIP flash.
- **Triple-buffered DMA ring.** One block is 64 stereo frames, so the deadline
  is **1.333 ms**. Measured round-trip latency is about **2.7 ms**.
- **Seven effect modules**, each describing its own parameters so the knob can
  render and edit them without the UI knowing what the effect is.
- **The box comes up on Passthru** — a freshly flashed board passes audio
  through unchanged rather than starting inside somebody's power-on parameter
  table.
- **Input and output are independent build options.** I2S in, S/PDIF in; I2S
  out, S/PDIF out, or no output at all.
- **A 512-point real FFT** drives a spectrum display on the idle screen.
- **Settings persist to flash**, keyed by a hash of the parameter layout rather
  than a version number you have to remember to bump.

A dist build of everything — all seven effects, the OLED and the I2S output —
measures **86 KB of text and 27 KB of bss**. Both are resident in SRAM, because
the image runs from there, so leaving a module out buys real bytes.

> ### The 307.2 MHz overclock
>
> The RP2350 is specified for **150 MHz**. This runs it at **307.2 MHz** — a 2x
> overclock, outside Raspberry Pi's guarantees and not certified across
> temperature, silicon variation or supply quality. It has been stable on the
> author's hardware; **verify on yours before relying on it.**
>
> The number is not arbitrary. Every audio clock divides down from it by an
> *integer*, so there is no fractional-divider jitter anywhere in the tree.

---

## Hardware

A Pico 2 (RP2350), a PCM1808 ADC module, a PCM5102A DAC module, an SH1106 or
SSD1306 128x64 I2C OLED, and a rotary encoder with a push switch. The S/PDIF
ends need only a TOSLINK transmitter and receiver.

### Pin assignments

**I2S audio** — one PIO state machine carries both directions, so input and
output can never drift apart.

| Signal | GPIO | Goes to |
|---|---|---|
| MCLK (256 fs, 12.288 MHz) | **13** | PCM1808 `SCKI` |
| BCK (64 fs, 3.072 MHz) | **14** | PCM1808 `BCK`, PCM5102A `BCK` |
| LRCK (48 kHz) | **15** | PCM1808 `LRC`, PCM5102A `LRCK` |
| DOUT | **16** | PCM5102A `DIN` |
| DIN | **17** | PCM1808 `DOUT` |

These are mostly forced rather than chosen. MCLK must sit on a CLK_GPOUT-capable
pin — on RP2350 that means 13, 15, 21, 23, 24 or 25, and 23/24/25 are taken by
the Pico 2 board itself. BCK and LRCK must be **consecutive**, because PIO
side-set drives them as one field.

**OLED and encoder** — both on core 1.

| Signal | GPIO | Header pin | Goes to |
|---|---|---|---|
| `OLED_SDA` | **26** | 31 | SH1106 `SDA` |
| `OLED_SCL` | **27** | 32 | SH1106 `SCL` |
| OLED `VCC` | — | 36 | 3V3(OUT) |
| OLED `GND` | — | 38 | GND |
| `ENC_TRA` | **18** | 24 | encoder A |
| `ENC_TRB` | **19** | 25 | encoder B (**must** be A+1) |
| `ENC_PUSH` | **20** | 26 | encoder switch, active low |
| ENC `VCC` | — | 36 | **3V3(OUT) — not 5 V** |
| ENC `GND` | — | 23 | GND |

A and B are consecutive because the PIO quadrature decoder requires it.

> **Both modules run from 3V3, not 5 V.** The PCM1808 needs a 5 V rail, so 5 V
> is present on this build and it is easy to grab the wrong one. **RP2350 GPIOs
> are not 5 V tolerant**, and the encoder's pull-ups tie the inputs to whatever
> rail the module is given.

**S/PDIF**, if either end is selected.

| Signal | GPIO | Build option |
|---|---|---|
| S/PDIF in | **1** | `-DNOVA_INPUT=spdif` |
| S/PDIF out | **22** | `-DNOVA_OUTPUT=spdif` |

Both are on **PIO2**, because PIO0 is the I2S and PIO1 is the encoder — whose
quadrature program declares `.origin 0` and takes 29 of its 32 instructions. The
receiver is 13 instructions and the transmitter is 1, so both ends fit in one
block together with room left over.

Both are placed **away from GP13**, which carries the PCM1808's MCLK: an
edge-rich multi-megahertz line beside an ADC's system clock is how jitter gets
into a conversion, and it would be hunted for in the DSP for a week.

> **Neither S/PDIF pin is an interface on its own.** A 3V3 logic pin needs a
> front end. **Coax** wants 0.5 Vpp into 75 Ω — a divider and a blocking
> capacitor at least, a transformer if isolation matters — and *receiving* coax
> wants a comparator, because 0.5 Vpp into a GPIO is not reliable even with the
> pad's Schmitt trigger. **Optical needs none of that**: a TOSLINK transmitter
> takes 3V3 logic directly and a TOSLINK receiver hands back logic level, which
> is why optical is the easy way to bring either end up first.

### Codec strapping

**PCM1808** — slave mode, standard I2S:

| Pin | Tie to |
|---|---|
| `FMT`, `MD0`, `MD1` | GND |
| Power | most modules need both 3.3 V **and** 5 V |

`SCKI` is required even in slave mode — the PCM1808 has no internal PLL. Slave
mode accepts 64 or 48 BCK per frame, never 32.

**PCM5102A** — 3-wire slave, internal PLL:

| Pin | Tie to |
|---|---|
| `SCK` | **GND** (enables the internal PLL) |
| `FMT` | GND (I2S) |
| `FLT` | GND (normal filter) |
| `DEMP` | GND (no de-emphasis) |
| `XSMT` | **3.3 V** (un-mute — low means muted) |

`SCK` must be **actively held low**, not left floating: the PLL only starts if
SCK stays at ground for 16 LRCK periods after BCK/LRCK begin. The common purple
GY-PCM5102 boards have a solder pad pair for exactly this.

> ### If a PCM1808 module sounds muffled, check the board before the firmware
>
> Some PCM1808 modules ship with the wrong anti-aliasing RC on the analog
> inputs: **10 kΩ with 100 nF**, which puts the corner at
>
>     1 / (2*pi * 10k * 100n) = 159 Hz
>
> That is a 6 dB/octave rolloff from 160 Hz up — roughly −16 dB at 1 kHz and
> −30 dB at 5 kHz. It sounds muddy with no treble at all while measuring
> perfectly fine digitally.
>
> The fix is to **remove all four parts** (2 R and 2 C, one pair per channel)
> and bridge the resistor pads. That RC is only there for RF rejection, so
> removing it is safe; the alternative is to keep the resistors and swap the
> caps for ~1 nF, putting the corner near 160 kHz. Not all modules are
> affected.

---

## Building

### What you need

Either works, and the build scripts find whichever you have:

**A standard Pico SDK install.** Clone the SDK **with submodules**, then set one
environment variable and put the tools on `PATH`:

```bash
git clone -b master --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
```

```bat
setx PICO_SDK_PATH C:\pico\pico-sdk
```

`arm-none-eabi-gcc`, `cmake`, `ninja` and `picotool` must be on `PATH`. Nothing
in this tree needs editing.

**Or the VS Code "Raspberry Pi Pico" extension**, which installs all of the
above privately under `%USERPROFILE%\.pico-sdk` and puts none of it on `PATH`.
`pico-env.bat` finds it automatically and picks the newest version of each
component — there are no version numbers written down anywhere to go stale.

To pin anything for one machine without touching a tracked file, create
`pico-env.local.bat` next to `pico-env.bat` and `set` what you want in it. It is
read first and is in `.gitignore`.

> `pico-env.bat` prints the SDK and toolchain it settled on at the top of every
> build. A build that quietly picked up a different SDK than you meant is
> otherwise indistinguishable from one that did not.

### Three commands

**1. See what this tree has.**

```bat
make_firmware.bat list
```

It reads the effects, displays, inputs and outputs out of the module
directories, so it cannot go out of date.

**2. Build, naming what you want.**

```bat
make_firmware.bat dist oled output:i2s agc
```

A **dist** image with the **OLED** panel, the **I2S** output and **Auto Gain**
as the only effect, into `build\`. The words may come in any order and none are
positional.

**3. Flash it.**

```bat
flash.bat nobuild
```

`nobuild` means "flash what is already in `build\`". Leave the board plugged in
normally — `picotool -f` reboots it into BOOTSEL for you. If no firmware with
USB stdio is on the board yet, hold BOOTSEL while plugging it in.

### The four rules

| | |
|---|---|
| **Name no effect** | you get every effect whose manifest says `DEFAULT ON` |
| **Name any effect** | you get **exactly** those and no others |
| **Display, input, output** | one each; defaults are `oled`, `i2s`, `i2s` |
| **`dev` or `dist`** | `dev` is the default and has the USB console; `dist` is the quiet one |

There is no way to say "the usual set, plus one". That is deliberate: it is the
only way the command line reads back as a complete description of the firmware.

The second rule is also why this is a script rather than a `cmake` line you
could copy. `NOVA_EFFECT_<NAME>` is a **cached** option, so an effect you simply
stop naming stays ON in a build directory that already had it — you would get a
firmware containing an effect you did not ask for, and no line of output would
say so. The script always passes an explicit `ON` or `OFF` for every effect in
the tree.

### More examples

```bat
:: everything, dev build, straight onto the board
make_firmware.bat flash

:: an S/PDIF receiver feeding the DAC - digital in, analogue out
make_firmware.bat dist input:spdif output:i2s

:: no panel and no DAC - the analyser and one effect
make_firmware.bat display:none output:none eq

:: the menu on the USB serial port, for a box with no panel fitted
make_firmware.bat dist serial

:: keep it out of build\ so the everyday image survives
make_firmware.bat dist oled bbe out:build-bbe
flash.bat dir:build-bbe
```

Two things that will otherwise catch you out:

- **`none` is a display, an output *and* an effect**, and **`i2s` is both an
  input and an output**, so a bare name that matches more than one kind is
  refused rather than guessed at. Write `display:none`, `output:none`,
  `effect:none`, `input:i2s`, `output:i2s`.
- **`serial` needs `dist`.** The serial display draws on the port a dev build's
  console is already printing to. The script says so before compiling anything.

`make_firmware.bat help` prints the full option list.

### The other scripts

| | |
|---|---|
| `build.bat` | the two or three configurations built every day — `build.bat`, `build.bat dist`, `build.bat serial`, `+ clean` |
| `build-variants.bat` | configures and builds nine module combinations into `build-var\`, to prove the plug points actually unplug. A build test, not a firmware test |
| `comcap.py` | reads a Pico's USB CDC port without being able to wedge on it — `python comcap.py COM17 15` |

### dev vs dist

They differ **only in telemetry**. `dev` has the USB console, the boot banner,
the once-a-second status line and the boot benchmark. `dist` has none of them
and comes up about two seconds sooner. The audio path, the panel and the knob
are identical, and the `DSP STATUS` page is live in both.

**`dist` does not remove USB** — it stops writing to the port but keeps it
enumerated, because that is what `picotool load -f` needs to reboot the board
without anyone reaching for BOOTSEL.

---

## Modules

Modules are discovered by globbing `*/module.cmake`. **Nothing in `CMakeLists.txt`
names a module**, so adding one is adding a directory.

A `module.cmake` is a single declaration:

```cmake
nova_module(
    TYPE     EFFECT
    NAME     eq
    ACCESSOR eq_module
    ORDER    30
    DESC     "ten band graphic equalizer"
    SOURCES  module.cpp equalizer/Equalizer.cpp
)
```

`LIBS` names an SDK library only this module needs — which is what keeps
`hardware_i2c` out of a build with no OLED in it. `PIO` names a `.pio` program.
`VENDORED` marks sources that are somebody else's code and excuses them from the
house warning set.

### What is in this tree

**Effects** — `ORDER` is menu order, and the first registered is what the box
boots on.

| | Module | |
|---|---|---|
| 5 | `thru_module` | Passthru — a gain trim, and what the box comes up running |
| 20 | `bbe_module` | BBE Sonic Maximizer style dynamic HF restoration plus a bass shelf |
| 30 | `eq_module` | Ten band graphic EQ |
| 50 | `noisegate_module` | Broadband noise gate |
| 60 | `agc_module` | Auto Gain — levelling plus a brick-wall limiter |
| 65 | `leveller_module` | Soft compression curve, no lookahead |
| 70 | `fxchain_module` | Runs up to four of the above in series |
| 900 | `no_effect_module` | Bit-exact passthrough. `DEFAULT OFF` |

**Displays** — `-DNOVA_DISPLAY=`

| Name | Module | |
|---|---|---|
| `oled` | `oled_display_module` | SH1106/SSD1306 + rotary encoder. The default |
| `serial` | `serial_display_module` | The whole menu on the USB port. **dist builds only** |
| `none` | `no_display_module` | No display, no keys |

**Inputs** — `-DNOVA_INPUT=`

| Name | Module | |
|---|---|---|
| `i2s` | `i2s_input_module` | PCM1808 on the shared state machine. The default |
| `spdif` | `spdif_input_module` | S/PDIF receiver with ASRC onto the local 48 kHz grid |

**Outputs** — `-DNOVA_OUTPUT=`

| Name | Module | |
|---|---|---|
| `i2s` | `i2s_output_module` | PCM5102A. The default |
| `spdif` | `spdif_output_module` | S/PDIF transmitter, consumer, 24-bit |
| `none` | `no_output_module` | Silent — analyser only |

### The four interfaces

A module implements one of `effect_module_t`, `display_module_t`,
`input_module_t` or `output_module_t` from `module.h`, and exports exactly one
accessor. That accessor is its whole public surface; no header exports anything
else, and **a module's own headers are not on the include path of any other
module**, so one cannot pick up another's header by accident.

Adding an effect is a directory with a `module.c` and a `module.cmake` in it.
**No core file is touched** — there is no `EFFECT_MAX` to raise and no registry
to edit. Read `module.h` and copy `thru_module/`.

> **Adding a whole new module directory needs one manual reconfigure**, because
> a glob cannot notice a file that did not exist when it ran. Delete the build
> directory, or run `cmake -B build` again. Editing an existing `module.cmake`
> reconfigures automatically.

### S/PDIF: receiving and transmitting are very different jobs

**`input:spdif` follows somebody else's clock.** The RP2350 has no steerable
audio PLL, so the correction is made in the *data* domain: the stream is
resampled onto the local 48 kHz grid, and nothing downstream ever learns that a
rate can vary. The block still arrives on the I2S state machine's schedule at
exactly 48 kHz, so the DAC's clocks never stop — including across a source
changing sample rate mid-programme, which is the plop this module was built to
remove.

The front end **samples the pin once per UI**, phase-locked to the incoming
edges, and the whole decode is then word-parallel bit arithmetic — 192,000 words
a second at 48 kHz rather than 768,000, with no run extraction and no learnt
thresholds anywhere. Measured on hardware against a real optical source: **9 %
of the block deadline**, locked, zero parity errors and zero resyncs. It
replaced a 4x oversampling front end that measured **73 % of a core**.

**`output:spdif` is the master**, so there is nothing to track: the divider is
exactly `clk_sys/50`. `write()` costs **2.4 %** of the deadline, and it is a
tenth of the code and a tenth of the SRAM of the receiver.

Both ends are verified on hardware, including an **optical loopback** — GP22 out
straight back into the GP1 receiver — which locks at 48000 Hz with zero errors.
Note that a box wired that way locks to *itself* and plays **silence**. That is
correct and is not a fault, and it is worth knowing before concluding the
receiver is dead.

See [spdif_input_module/README.md](spdif_input_module/README.md) and
[spdif_output_module/README.md](spdif_output_module/README.md).

---

## Effects

Full descriptions of every control are in **[UserManual.md](UserManual.md)**;
the engineering detail for each is in that module's own `README.md`.

```
Passthru    a gain trim, -12 to +12 dB, and what the box comes up running.
            Switching between this and an effect at a matched level is the
            only honest way to hear what the effect is doing
BBE Maximizer
            dynamic HF restoration driven by how much the lows are masking
            the highs, plus a bass shelf
Equalizer   ten band graphic EQ, 60 Hz to 16 kHz, +-12 dB a band, with a
            preamp because ten bands at +12 dB add to far more than +12 dB
Noise Gate  a broadband gate - shuts the signal off below a threshold, so a
            source that is clean while something plays and hissy in the gaps
            is quiet in the gaps too
Auto Gain   automatic volume levelling, so one track does not arrive louder
            than the last, with an exact brick wall behind it
Leveller    a soft compression curve applied one to five times - evens out
            the quiet and loud parts of one performance, rather than one
            track against the next
FX Chain    runs up to four of the above in series, in an order you set
```

**An effect describes its own parameters**, which is how the UI renders and
edits them without knowing anything about the effect:

```c
typedef struct {
    const char         *name;     // <= 8 characters reads best on 128 px
    effect_param_kind_t kind;     // EP_INT, EP_ENUM or EP_BOOL
    int                 min, max;
    const char *const  *labels;   // EP_ENUM: (max - min + 1) entries
    const char         *unit;     // EP_INT: "dB", or NULL
} effect_param_t;
```

So `BASS_DYN` shows as `normal` rather than `2`, and `AGC` as `on`. The
descriptor is the **only** place those ranges and labels are written down — the
console's parameter line reads the same table, so the two cannot drift.

Every parameter page ends with a **Reset** row that puts that effect back to the
values it was registered with.

---

## How it works

### Clocking

Everything derives from `clk_sys` by **integer** division, so there is no
fractional-divider jitter anywhere:

| Clock | Divider | Frequency |
|---|---|---|
| MCLK (`CLK_GPOUT0`) | clk_sys / 25 | 12.288 MHz = 256 fs |
| PIO state machine | clk_sys / 25 | 12.288 MHz = 256 fs |
| BCK | PIO / 4 | 3.072 MHz = 64 fs |
| LRCK | PIO / 256 | 48 kHz |

MCLK comes from a **hardware clock output**, not a PIO pin toggle: a PIO toggle
would need a fractional divider and would smear ~3.3 ns of jitter onto the ADC's
system clock. Both dividers act on the same `clk_sys`, so MCLK and BCK stay
locked at exactly 4:1 — which is what the PCM1808 requires of `SCKI`.

### Buffering and the DSP hook

Triple buffered: the RX DMA fills buffer N while `process_block()` works on N−1
**in place** and the TX DMA drains N−2. RX and TX share one state machine, so
input and output can never drift apart.

The hook is the active effect's `process()`, reached through
`effect_process_block()`: `AUDIO_BLOCK_FRAMES` stereo frames, interleaved L,R,
24-bit left-justified in 32 bits.

The `overruns` counter increments whenever a block is handed over before the
previous one was collected — that is the deadline being missed, and it is the
number to watch as DSP load grows. Halve `AUDIO_BLOCK_FRAMES` to trade CPU
headroom for latency.

> **Use `float` for DSP code.** The M33 has a single-precision FPU, but `double`
> is **software-emulated** and will blow the deadline. `sinf()`, not `sin()`.
> The build pins `-Wdouble-promotion` for exactly this reason, so a stray `2.0`
> in the audio path is a warning rather than a missed deadline found by ear.

### Running from RAM

```cmake
pico_set_binary_type(NovaDSP copy_to_ram)
```

The image still lives in flash and still survives power cycles — the startup
code copies it into SRAM and runs it there, so **no code fetch touches flash at
runtime**. That removes XIP cache misses from the DSP path: execution timing
stops depending on whether a routine happened to be in cache.

Verify it with the ELF program headers — `main` should link at `0x2000xxxx`.
`picotool info` still reports `binary start: 0x10000000`, which is the flash
*load* address, not where the code executes.

**The cost is SRAM**, since the whole image is resident. That is why leaving a
heavy module out is worth real bytes.

### Settings storage

Parameters and the selected effect are persisted to flash. A write happens only
when you leave a parameter page and only if something changed — a single sweep
through a parameter would otherwise be an erase/program cycle per detent.

**The store is keyed by a hash of the registered layout**, not a version number
you have to remember to bump: every effect's name in registration order, its
parameter count, and each parameter's name, kind and range. It changes exactly
when a stored record would stop meaning what it meant, and never otherwise — so
an unrelated rebuild keeps your tuning, and reordering effects or renaming a
parameter invalidates the store by itself.

What a hash of the layout **cannot** see is a parameter whose *meaning* changed
while its name, kind and range did not. There is one escape hatch for that case
and no other:

```bat
make_firmware.bat -DNOVA_SETTINGS_SALT=1
```

Records are also keyed **by effect name, not by index**, so inserting an effect
cannot silently apply the wrong stored values to every effect after it.

---

## Repository layout

### Core

| File | |
|---|---|
| `NovaDSP.c` | main loop, metering, the banner |
| `audio.c` | PIO I2S in and out, the DMA ring |
| `effects.c` | the registry, the core 0 / core 1 command ring, the timing |
| `spectrum.c` | the 512-point real FFT and the band folding |
| `settings.c` | parameters and effect selection, persisted to flash |
| `display.c` | core 1, and the two seqlocks that feed it |
| `modules.c` | walks the generated module list, and names nothing |
| `fault.c` | hard fault handler |
| `module.h` | the four module interfaces — read this first to add one |
| `effect.h` | the effect interface and the parameter descriptor |
| `i2s.pio` | the shared I2S state machine |

### Build

| File | |
|---|---|
| `CMakeLists.txt` | the core target. Names no module |
| `nova_modules.cmake` | `nova_module()`, discovery, selection, manifest generation |
| `pico-env.bat` | finds the SDK and toolchain. Sourced by every other script |
| `make_firmware.bat` | build any combination of modules |
| `build.bat` | the everyday configurations |
| `flash.bat` | build and flash over USB via picotool |
| `build-variants.bat` | build nine combinations as a plug-point test |

### Tools

`oled_vis/` is a Go tool that converts a video into the 1 KB-per-frame C arrays
the OLED idle screen plays back. `comcap.py` reads a Pico's USB CDC port.

---

## License

**MIT** — see [LICENSE](LICENSE). Fill your name into the copyright line before
publishing.

### Third-party code and names — read before you rely on the MIT grant

The MIT licence covers the work in this repository. It cannot cover code that
came from somewhere else, so two things are worth stating outright:

- **"BBE" and "Sonic Maximizer" are trademarks of BBE Sound, Inc.** The engine
  in `bbe_module/` is an independent implementation of the *style* of
  processing, not BBE's code — but the name is theirs. Describing an effect as
  "BBE-style" is ordinary nominative use; naming a product after it is not.
- **`spdif_input_module/` reads an AGPL-3.0 project for its approach and copies
  none of its code.** That is stated in `SpdifRx.h` and in the module's README,
  and an algorithm is not copyrightable. Noted here so nobody has to rediscover
  the question.

The Pico SDK is BSD-3-Clause and is not vendored here — you supply it.
