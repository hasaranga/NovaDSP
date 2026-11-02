# spdif_input_module

S/PDIF input: `-DNOVA_INPUT=spdif`, or `input:spdif` to `make_firmware.bat`.

**Builds clean in all four combinations** (i2s/spdif input × dev/dist), zero
warnings at the pinned `-Wall -Wextra -Wdouble-promotion`. **182 host checks**,
and — unlike before — that now includes the PIO program itself; see *The
decoder*.

Freshly measured on this tree, OLED and the full effect set:

| | text | bss |
|---|---|---|
| i2s input, dev | 124,296 | 194,968 |
| **spdif input, dev** | **134,576** | **237,704** |
| i2s input, dist | 116,400 | 194,964 |
| spdif input, dist | 124,744 | 237,660 |

So S/PDIF costs **+10,280 text and +42,736 bss**: the 16 KB DMA ring, the
16.5 KB ASRC kernel, the 8 KB input ring and the fade table. That is a third of
the SRAM this board had spare. If it is ever not enough, dropping effect
modules from the build is the lever - each one leaves with its own bss.

**Both decoders are compiled and the unused one is dropped.** Building with the
per-UI decoder came out **864 bytes of text and 104 of bss smaller** than the
reference one in the same configuration, which is `--gc-sections` doing what it
was relied on to do rather than a saving worth having.

**Both front ends have now been on hardware.** The reference decoder measured
73 % of a core; the per-UI one that replaced it measures **9 %**, locks a real
44.1 kHz optical source at −42 ppm with zero resyncs and zero parity errors,
and survives the OLED idle screen that used to take the box down. See
*Measured on hardware* below, and *What still needs the board* at the end for
what is left.

**The transmitter is verified too**, including an optical loopback through
this receiver — which is what found the ASRC bug below. See
[../spdif_output_module/README.md](../spdif_output_module/README.md).

## Why the ASRC exists

An S/PDIF stream carries its own clock. The source's 48 kHz comes from the
source's crystal and ours from ours, so the two drift permanently — about
2.4 frames a second at 50 ppm, which is a 64-frame block gained or lost every
27 seconds. **A bigger buffer does not fix that**, it only postpones the first
click: drift is a slope, not a transient.

The RP2350 cannot fix it in the clock domain. Every clock it generates is a
divided-down `clk_sys`, and the finest trim available is the PIO divider's
16.8 fixed point — at the divisor of 25 that 48 kHz needs, **one LSB is
~156 ppm**, coarser than the ±100 ppm being corrected. (An ESP32 does this in
the clock domain because its APLL trims to ~1.5 ppm via `sdm0`. We do not have
that part, and this is the one place where that difference decides the
architecture.)

So the correction happens in the **data** domain, where resolution is free.

## The contract, and what it buys

> **The input module emits exactly `AUDIO_BLOCK_FRAMES` frames per block on the
> 48 kHz grid, always** — whatever the source is doing, including doing
> nothing at all.

Everything behind that boundary is this module's private business. Because of
it:

- `AUDIO_SAMPLE_RATE` stays a compile-time `48000u` and all 22 sites that read
  it are untouched
- `effect_t` needs no `set_rate()` hook and no effect is retuned
- the spectrum band table, the settings layout hash and every published `dsp`
  figure stay valid
- no core file learns that a rate can vary

**That placement is the whole design.** The tempting shortcut is to let the
real rate leak upstream "just for the spectrum"; doing that gives back every
line above.

## Scope: 48 kHz output, input at or below it

Supported input rates are **32000, 44100 and 48000**. The ratio is therefore
always ≤ 1, which means the kernel only ever has to reconstruct the
band-limited signal the input samples already represent — its cutoff is
normalised to the *input* rate, so **one prototype filter serves every
supported rate with no redesign**.

96 kHz is deliberately out. That is decimation: the kernel would have to
narrow to the *output* Nyquist to stop aliasing, making the filter
rate-dependent. Different design, not a bigger table.

## Measured

`asrc/test_asrc.cpp`, 26 checks, all passing, clean at `/W4`. Build command is
in that file's header and nowhere else. 32 taps, 128 phases, kernel table
**16,512 bytes of SRAM**.

| | worst spur | THD+N |
|---|---|---|
| 48000 → 48000 (unity) | -141.4 dB | -140.8 dB |
| 44100 → 48000 | -101.9 dB | -97.9 dB |
| 32000 → 48000 | -99.0 dB | -98.5 dB |

Level within 0.01 dB and frequency within 0.0001 Hz at all three. Passband at
44.1 kHz in: flat to -0.09 dB at 15 kHz, -0.55 dB at 19 kHz, -2.42 dB at
20 kHz.

**`kCutoff` 0.95 was measured, not chosen** — the three-row comparison and the
reasoning are in `Asrc.cpp` beside the constant.

Two things worth knowing about the suite:

- **The drift check runs at two lengths and compares.** The obvious version —
  run once, assert the frame error is small — passes just as happily on a
  constant offset as on real drift, and there *is* an offset (the 32 frames of
  kernel priming). At 2M frames it would have let 17 ppm of genuine drift
  through, the same order as the error being corrected. An offset is identical
  at both lengths; drift is proportional.
- **Level and THD+N are measured in the time domain**, by coherently fitting
  the fundamental and looking at the residual. Summing FFT power across a
  window mainlobe over-reads a pure tone by exactly √2, which presented as
  every rate being 3.02 dB hot and looked like an engine gain bug. The FFT is
  still used, but only for ratios between bins.

## Cost

Not measured on target. Per output frame the inner loop is 32 taps of one
lerp and two multiply-accumulates — roughly 112 MAC-equivalents. Against this
project's own measured ~4 cycles/MAC (15 biquads = 306 cycles) that is
~500–700 cycles against a 6,400-cycle frame budget, so **8–11 %**, or about
half a DeeFloat. **That is an estimate and this tree has form on estimates**;
`dsp` on `DSP STATUS` is the reading.

One thing that makes measuring it easier than usual: the warning that *"any
benchmark against silence reads ~40 % of the real cost"* does **not** apply
here. The ASRC has no detector, no gate and no early-out — it does identical
work on silence and on full scale, so a silent benchmark is a valid one.

## The ASRC could not run faster than 1:1

**Found on hardware by the optical loopback, and nothing else could have found
it.** The loopback is the only configuration in which the source rate *is* the
output rate, which is the one place this fires.

The symptom: `acquiring` for ever at 48000 Hz, ring frozen at 324 frames, trim
climbing 10 ppm a second until it pinned at the +1000 ppm rail and stayed there
for 75 seconds. Parity 0, resync 0 — the decode was perfect. The loop was asking
to consume faster and could not.

`Asrc::setInputRate()` had **two** ceilings:

```c
if (ratio < kMinRatio || ratio > kMaxRatio) return;          // kMaxRatio = 1.02
...
if (m_step > ((uint64_t)1 << 32)) m_step = (uint64_t)1 << 32;   // ratio 1.0
```

The accept band had already been widened from 1.00 to 1.02 **to fix exactly
this fault** — its comment in `Asrc.cpp` describes the symptom precisely, down
to "the click arrives seconds after the cause". That fix never worked, because
the step clamp three lines below threw the rate away again. There is one bound
now.

Two things worth more than the fix:

- **A 44.1 kHz source can never trigger it.** Ratio 0.919 is nowhere near the
  ceiling, and every source this module had ever seen was slower than the
  output.
- **The host check for it passed the whole time.** It asserted `inputRate()` —
  the value handed in, stored *before* the step is computed — so it confirmed
  the rate was *accepted* and never that it *took effect*. It now runs two
  identical passes at 1.000 and 1.001 and asserts the second consumes more
  input, so the per-call priming overhead cancels. **The replacement was
  verified to fail against the old code before being kept**, which the original
  never was — and a first attempt at it would itself have passed with the bug
  present, because priming is ~1.5 % over those call sizes and swamps the 0.1 %
  being measured.

## The phase accumulator is fixed point, deliberately

This tree's rule is float and never double. But a float32 phase accumulated
48000 times a second loses precision in a way that presents as slow drift —
exactly the fault this engine exists to remove. The phase is therefore a
**64-bit Q32 accumulator**, which is exact: no rounding, no random walk, no
drift however long it runs. The audio path either side of it is float
throughout. The two-length drift check is what holds that claim up.

## The rate estimator

`asrc/RateEstimator.{h,cpp}`, tested by `asrc/test_rate.cpp` — 40 checks, all
passing, clean at `/W4`. The last group closes the loop around the **real**
`Asrc` with a real ring buffer.

**The loop closes on buffer level, not on measured frequency**, and that is
structural rather than a tuning preference: buffer level is the *integral* of
rate error, so any residual error in a frequency measurement — however good —
integrates without bound. Controlling the integral is the only way to make
zero drift a property of the loop instead of a property of the measurement.

**There are no timestamps anywhere.** Our output rate is exactly 48000 by
construction, so input frames counted against output frames produced is
already the ratio. Nothing needs a timer, which is also what lets the whole
thing run in a plain host loop.

PI on level error, with the nominal rate (32000/44100/48000 exactly) as
feedforward so acquisition doesn't wait for an integrator to wind up from
zero. Gains are derived in `RateEstimator.cpp` beside the constants —
`Kp 15, Ki 2.5`, giving ζ 1.04 and a 3.8 s slow pole.

Measured:

| | |
|---|---|
| correction accuracy at ±25/±100/+500 ppm | within **0.07 ppm** |
| buffer level over one simulated hour | **255–257** frames (target 256) |
| steady-state trim ripple | **0.029 ppm** p-p |
| 40 ppm thermal drift over ten minutes | tracked, lock never lost |
| closed loop with the real ASRC, 5 min | ring 255–257, no starve, no overrun |

Ratio modulation is pitch modulation, so that ripple figure is an audio number
and not just a control-loop nicety. 0.029 ppm is inaudible by a wide margin.

**The gains were derived and then corrected against a measurement.** The first
pair (Kp 10, Ki 0.5) gave ζ 1.55 and a 17.6 s slow pole. Nothing was unstable
and nothing drifted — it simply never arrived: two minutes in, the integrator
was still winding up, the buffer sat several frames off target and the P term
was quietly supplying a fifth of the correction. An hour-long run showed the
trim still climbing from 67 to 100 ppm. **A loop that is converging is not a
loop that has converged**, and only the long run showed the difference.

Two things about the suite worth keeping:

- **`trimPpm()` is not the correction.** It is only the integrator; the ratio
  handed to the ASRC is `trim + Kp*error`. Asserting on the integrator alone
  reported a correctly converged loop as being 4.3 ppm wrong, which sent the
  first round of diagnosis in the wrong direction entirely.
- **Most of the suite models the consumer analytically** (`level += arrived -
  outFrames*ratio`). That is not a simplification — it is exactly what the
  ASRC does — and it runs fast enough to make hour-long simulations practical.
  The real `Asrc` is then used to close the loop for real, because a faithful
  model of a component is still not the component.

### The ASRC must not be pulled before the rate is classified

Found by the closed-loop test, and it is a **constraint on the module**, not
an artefact of the harness. `Asrc::init()` leaves the ratio at 1.0, so against
a 44.1 kHz source it consumes 64 frames a block while 59 arrive — the buffer
drains at 5 frames a block, and the classification window is 187 blocks long.
It starves ~900 frames short before the estimator has said anything at all.

The module is muted over exactly that interval anyway (no lock → hold
silence), so the rule costs nothing. But being muted has a second half that is
easy to miss: **the ring must be trimmed to target rather than allowed to
fill.** A quarter-second of audio nobody played is stale by the time it would
be played, and letting it accumulate overruns the ring instead of starving it.

So while `est.usable()` is false: trim the ring to target, hold the ASRC in
reset, and emit silence.

### Buffer sizing

Before the integrator winds up, the P term alone supplies the whole
correction, so the transient level excursion during acquisition is about
`delta/Kp` frames — 33 frames for a source 500 ppm out. **The target level has
to clear that in both directions**, on top of whatever the coast policy wants.
The test uses a target of 256 with a 4096-frame ring.

## The decoder

**There are two, and the tree builds both.** `SPDIF_USE_PIO_DECODE` in
`asrc/SpdifInput.h` picks which one the module uses; the other is dropped whole
by `--gc-sections` and costs nothing.

| | front end | decoder | words/s at 48 kHz | measured on the board |
|---|---|---|---|---|
| **1** (shipped) | `spdif_rx_ui.pio`, one sample per UI | `asrc/SpdifRxUi.*` | 192,000 | 147 cyc/word = **9 % of a core** |
| 0 (reference) | `spdif_rx.pio`, 4× oversampled | `asrc/SpdifRx.*` | 768,000 | 292 cyc/word = **73 % of a core** |

The reference decoder is correct, is exhaustively tested, has been on the board
and decodes a real optical source. What it cannot do is fit: the average fits
inside the 1333 µs block deadline and the peak does not, and no constant in it
changes that, because **the cost is per *run*** and biphase produces ~0.75 runs
per UI whatever the oversampling is — 4.6 M a second either way.

### Measured on hardware, 44.1 kHz optical source

```
spdif: 176393 words/s | scan 44100 Hz, framed, 44098 frames/s
       fill 112.3/119.3 us 8.9% of deadline | 750 calls/s, 235 words/call, 147 cyc/word
       laps 0, worst 236 words outstanding (ring 4096)
       LOCKED, 44100 Hz, ring 315, trim -42.0 ppm, gain 1.00 | parity 0 resync 0 (0/s)
```

`fill()` went from **73 % of a core to 9 %** — an eight-fold reduction, and the
audio path now has the headroom it did not have. 60 s soak with the OLED
fitted: `ovr` static, zero resyncs in every report, and it rode through the ten
second idle screen that previously took both cores down.

**The per-word estimate was wrong again, and by about three times.** This file
predicted "the low tens" and set a tripwire at 120; the answer is 147. What
carried the redesign was the half that *was* solid arithmetic — a quarter as
many words — not the per-word figure. Worth remembering the next time a
decision here rests on an estimate: the countable part was right and the
cycle-counting part was not, and that has now happened twice in the same
module in the same direction.

The remaining 147 is not obviously irreducible — a 64-bit shift accumulator on
a 32-bit core is the first suspect — but at 9 % there is no reason to look.

### The per-UI front end

`spdif_rx_ui.pio`, 13 instructions on PIO2. It samples the pin **once per UI,
phase-locked to the incoming edges**:

- on every transition, **snap** — a UI boundary just happened, so wait half a
  UI and sample, which is the centre of the UI that just started;
- then poll for the next transition for one UI. If it comes, snap again. If a
  whole UI passes without one, sample again.

A 1 UI run yields one sample, a 2 UI run two, a 3 UI run three: **exactly one
sample per UI, always**, with the phase re-established at every edge and
free-running for at most three UI between them.

**One sample per UI is not a lossy shortcut.** Every transition lands on a UI
boundary, so the level at the centre of each UI describes the waveform
completely — biphase data, all three preambles and the violations that identify
them. Written as UI levels, with `a` for the level the preamble starts on:

| | runs | UI levels | byte |
|---|---|---|---|
| B (Z) | 3,1,1,3 | `a a a . a . . .` | `0x17` / `0xE8` |
| M (X) | 3,3,1,1 | `a a a . . . a .` | `0x47` / `0xB8` |
| W (Y) | 3,2,1,2 | `a a a . . a . .` | `0x27` / `0xD8` |

Three distinct bytes, six with both polarities, and a comparison of one byte
replaces run-length classification entirely. There are **no thresholds to
learn**, so there is no histogram, no acquisition transient and no bistable
"too slow to ever get fast" basin for the module to work around.

### Decoding a subframe

A subframe is 64 UI — exactly two words — and decoding one is straight-line bit
arithmetic with no loop in it:

```
X = v ^ (v >> 1)      transition between UI i and i+1
Y = X >> 7            even bits are cell boundaries, odd bits are the data
```

- **the data** is the odd bits of `Y`, gathered 16 at a time by a five-stage
  shift-or-mask;
- **parity** is an XOR fold of those same bits, no popcount needed;
- **the structural check** is `(ylo & 0x55555555) == 0x55555555` and one more
  like it: *every bit cell begins with a transition*, and there are 28 of them.

That second check is the load-bearing one. Parity catches one bad bit; it does
**not** catch a framing error, because a subframe read at the wrong offset is
28 bits of something and has an even chance of passing. Twenty-eight cell
boundaries do not. It is what makes the hunt safe — a candidate preamble byte
is one in forty by chance, and a candidate that also carries 28 cell boundaries
and correct parity is not.

### The divider scan

The front end has to be told roughly what rate to expect, because its sample
clock is cycles per UI. The window either side of a position is wide, and
derived rather than guessed:

> After a sample the program spends three cycles reaching its first poll and
> two leaving the loop, so the pin is watched over `[s+3, s+29]` of every 32.
> An edge in the six cycles after that is not seen until the next period has
> already emitted a sample. For a run of L UI that gives
> `16 + 32(L-1) < L·R <= 16 + 32(L-1) + 29`, and L = 3 — the run inside every
> preamble — binds it at **26.67 < R <= 36.33 cycles**.

In rates: **a source between 0.881× and 1.170× the position decodes.** Swept in
1 % steps by the host suite, and the cliffs land at 0.88 and 1.18 — the
arithmetic to the step size.

There is **one position per supported rate** — 48000, 44100, 32000, the same
three `RateEstimator` classifies against. 44.1 and 48 are only 8.8 % apart and
would share a position comfortably; they are given one each anyway, so that the
whole window is jitter margin rather than half of it being spent on being on
the wrong divider. The module steps to the next position after 200 ms without
framing, so the worst case is under two thirds of a second.

### Measured

Both decoders, on the host. The per-UI figures come from
`asrc/test_spdif_ui.cpp`, which **runs the actual PIO program** — see below.

| | per-UI | reference |
|---|---|---|
| 48000 / 44100 / 32000 round trip | **bit exact**, 0 parity, 0 resyncs | same |
| edge jitter, clean to | **±0.175 UI** | ±0.10 UI |
| edge jitter, breaking up at | ±0.20 UI | ±0.20 UI |
| rate window per divider position | **0.881× to 1.170×** | every rate, one divider |
| digital silence | decodes, and decodes to zero | decodes (and was the bug that found the histogram fix) |
| pure random levels | **0 frames** | 0 frames |
| a dropped word (FIFO overflow) | re-frames, 398 of ~400 frames, 1 resync | n/a |
| carrier loss / return | drops, then re-acquires in one subframe | drops, returns without re-learning |
| block position | advances once per frame across block boundaries | same |

The jitter model is independent random phase error on **every** edge — the
worst case, and pessimistic against a real link whose jitter is mostly
low-frequency wander that moves adjacent edges together. It is also why the
figure is not the ±0.39 UI a static margin calculation gives: the sample is
half a UI after a *jittered* edge and the run ends on another *independently
jittered* one, so at ±j peak the two can differ by 2j.

### What the suite covers that it could not before

**It runs the PIO program.** `asrc/PioSim.h` is a small RP2350 state machine
emulator — JMP with its eight conditions, IN, SET, MOV, the delay field, the
ISR and autopush, and the pad's two-cycle input synchroniser. The test shells
out to the **SDK's own pioasm** (`$PIOASM`, or
`%USERPROFILE%\.pico-sdk\tools\2.3.0\pioasm\pioasm.exe`), assembles
`spdif_rx_ui.pio`, and executes the instruction words that come out.

So the delay slots, the poll loop length and the branch targets are all under
test, and an edit to the `.pio` reaches the suite. The emulator also counts
instructions it did not understand, and the suite checks that count is zero
rather than trusting it.

The original argument for keeping the PIO program to one instruction was
"**PIO cannot be tested on the host**". That was wrong, and the way it was
wrong is worth keeping: a state machine is a 16-bit instruction word and five
registers, and the subset a decoder needs is about a hundred lines to execute.
What was *right* was the fear of the specific program it imagined — a
run-length classifier with staged bounded waits and a polarity-symmetric copy
of all of it really would not have fitted in 32 instructions. The way out was
to stop classifying runs.

### What the test still cannot cover

The clock divider arithmetic (in the `.pio`'s `c-sdk` block, never executed on
the host), the pad's real synchroniser timing, the DMA ring, and whether GP1 is
connected to anything. Those need the board.

### One capability lost

**A 96 kHz source reads as `NoCarrier` rather than `Unsupported`** on the
per-UI front end, because 96 kHz is twice the fastest scan position and a UI at
half the expected length is outside the window by a factor of two — nothing
frames, so it looks the same as an unplugged fibre. The reference front end's
fixed clock digitises 96 kHz perfectly well and lets the estimator name it.

Neither could ever *play* it — `RateEstimator` supports 32/44.1/48 — so what is
lost is the diagnosis, not a rate. Recovering it would need a second PIO
program at 16 cycles per UI; the window cannot stretch to it.

### The histogram technique (reference decoder only)

The reference decoder's learnt thresholds are
[netham45/esp32-spdif](https://github.com/netham45/esp32-spdif)'s technique.
**That project is AGPL-3.0 and this one is not** — it was read for the approach
only. The per-UI decoder learns nothing and borrows nothing.

## The continuity policy

`asrc/SpdifInput.{h,cpp}`, tested by `asrc/test_input.cpp` — 32 checks, all
passing against **either** decoder. This is the whole path behind one call, and
the claim it makes to the rest of the firmware:

> **`produce()` always fills exactly `frames` frames, and never writes a step
> discontinuity while doing it.**

Not "when locked", not "when a carrier is present" — always, from the moment
`init()` returns, with nothing plugged in at all.

**Zero is not the answer; ramps are.** Writing zeros on carrier loss steps from
whatever the last sample was, which is a broadband click — the plop relocated,
not removed. Every transition is a 144-frame raised cosine, and a dropout is
filled with the last sample *through that ramp* rather than held. A hold would
be DC into a speaker; the ramp lands on true zero and stays there.

| | measured |
|---|---|
| no input at all, 1 s | every block full, output **exactly zero** |
| 44.1 kHz source, 30 s | Locked, trim **-0.28 ppm**, ring 256, gain 1.000 |
| carrier stops and returns | gain 1.000 → **0.000** → 1.000, **no step** |
| rate change 44.1 → 48 | re-classifies, **no step** |
| random noise on the wire | never reaches Locked, no audio out |
| 96 kHz source | reports Unsupported, mutes |
| bursty delivery (dry ring) | every block full, no step |
| burst of 30 block periods | 1055 frames dropped, ring bounded, blocks full |
| block sizes 1…256 | all completely written |

"No step" means the worst sample-to-sample delta stayed at 0.065, which is the
test tone's own natural slew — i.e. nothing above the signal itself.

### Three bugs, and how each was found

**1. `setCarrier(true)` per block deadlocked the estimator.** It cleared the
classification window on every call while still unclassified, and the state
only leaves `kNoCarrier` when a window *closes* — so a caller reporting the
carrier every block, which is the natural way to drive it, reset the window
before it could ever close. `test_rate.cpp` missed it by calling `setCarrier()`
once at the start. Now clears on the transition only, with a regression check
in that suite.

**2. The ASRC silently refused every upward trim at 48 kHz.** `kMaxRatio` was
1.00 — the obvious value, since anything above the output rate is decimation.
But the nominal ratio is not the ratio the loop asks for: a 48 kHz source
100 ppm fast needs 1.0001, and `setInputRate()` **holds the previous rate**
rather than clamping, so the correction never landed at all. The ring filled
without bound, overran, dropped frames, and the click arrived *seconds* after
the cause. Only reachable with a source that is both at the top supported rate
and fast, which no test source was. Band widened to 1.02 (still nowhere near
decimation — the kernel cuts at 0.95 of input Nyquist), and `test_asrc.cpp`
gained the missing case.

**3. A hypothesis that a trace disproved.** The rate-change step looked like a
seam artefact, so a resync-burst trigger was written to fade on decoder
distress. A trace showed the seam is completely clean — zero resyncs, zero
parity errors, because the histogram simply re-learns the new unit length —
and the worst step was **3369 blocks later**, where the ring overflowed from
bug 2. The trigger was removed rather than kept as defensive code no test
exercises. **Two rounds of guessing cost more than the trace did.**

### Sizing

Ring target 256 input frames of 1024 (8 KB as stereo float). The target has to
clear the acquisition transient — before the estimator's integrator winds up
its P term alone supplies the correction, so the excursion is about `delta/Kp`
frames, 33 for a source 500 ppm out.

**`push()`'s decode scratch used to be `int32_t decoded[512*2]` — 4 KB — on the
stack, inside the block loop on core 0**, with no explicit stack size set
anywhere in the build. It was sized by guessing generously rather than from the
word count. It is now `SPDIF_PUSH_FRAMES`, derived from the chunk size and the
UI a word carries, and it is a member rather than a local, so the block loop's
stack depth does not depend on this module at all.

That was the first suspect in the unexplained hard fault at
`SPDIF_OVERSAMPLE 2` (`SESSION.md`). **It is not a fix for that fault**, which
is still unexplained and now unreachable — the constant it depended on belongs
to the front end that is no longer shipped. It was worth doing on its own
terms.

## The plumbing

`module.cmake` and `module.cpp` here; `input_module_t` in the core's
`module.h`. Adding a fourth module type touched the core the way the other
three are touched and no more — the discovery glob, the selection, the
generated manifest, one accessor in `modules.c`, one call in the block loop.
**No core file names a module.**

**`fill()` is the whole interface, and it is the mirror of `write()`.** The
core hands the input module the block *before* the effect and the meters; an
input on the shared I2S state machine leaves `fill` NULL, because the buffer
already holds the samples, and an input with its own front end overwrites it.
The two empty hooks at the two ends are the same hardware fact, not stubs.

**The block cadence stays the core's.** The I2S state machine keeps running
whatever the input is, so blocks arrive at exactly 48 kHz and the DAC's clocks
never stop through a source appearing, disappearing or changing rate. Letting
the input module drive the cadence instead would put the output rate at the
mercy of whatever is plugged in, and on a chip with no steerable audio PLL
there is no way back from that.

`display_status_t` gained `input_locked` and `input_rate_hz` as **generic input
properties** — an ADC reports locked at `AUDIO_SAMPLE_RATE` through NULL hooks,
so no core file learns what S/PDIF is.

**One documentation bug fell out of it.** `i2s` now names both an input and an
output module, so a bare `i2s` token is ambiguous and `make_firmware.bat`
rejects it — which broke the README's own quick-start example. Fixed to
`output:i2s`, and the script's `list` and ambiguity messages now name the
`input:` prefix. `input:` and not `in:`/`out:`, because **`out:` is already the
build-directory switch** and the tempting pair would have silently redefined it.
## What still needs the board

**Settled on hardware** (44.1 kHz optical into GP1, dev + OLED, S/PDIF both
ways):

- **`cyc/word` and the cost.** 147 cyc/word, `fill` 9 % of the deadline. The
  ceiling is `clk_sys / SPDIF_WORDS_REQUIRED_PER_S` = 1600 cycles a word
  against the old front end's 400, so there is a factor of ten in hand.
- **That it frames, locks and classifies.** `LOCKED, 44100 Hz`, trim −42 ppm,
  zero parity errors, zero resyncs, 44,098 frames/s.
- **Real jitter.** A real optical link at 0 resyncs a second is comfortably
  inside the ±0.175 UI the host sweep measured. If a source ever is not, the
  knob is not `SPDIF_UI_CYCLES` — raising that buys phase resolution, not
  margin — it is switching `SPDIF_USE_PIO_DECODE` to 0 and comparing.
- **Core 1 survives it.** The reference decoder at 73 % of a core took the whole
  box down when the OLED reached its idle screen. That went unexplained for a
  long time and was only avoided; **it is now understood and fixed.** The
  remaining half was `SpdifInput::reset()` calling `Asrc::init()` from inside
  `fill()` — a 129×32 Kaiser-windowed sinc rebuilt in software `double`, tens of
  ms — which the divider scan triggered every 200 ms for as long as nothing
  framed, i.e. continuously with no source connected. Core 0 overran, core 1
  starved behind it. `Asrc::restart()` is now the re-acquire path and the kernel
  is built once. See `SESSION.md`.

**Still open:** nothing that blocks listening. Items 1 and 2 are settled on
hardware; 3 is untested by choice; 4 and 5 are only relevant if someone wants
coax or touches the i2s input.

**1. ~~The actual complaint.~~ Settled — listened to on hardware.** Source into
GP1, GP22 into an amp's optical input. Audio clean, no glitches, and **changing
tracks across a sample-rate change produces no plop**, which is the thing this
whole module was built for. The one-input-frame hole at the seam is not audible.

**2. ~~The transmitter.~~ Listened to.** GP22 drives an amp's optical input and
the box has been played through it. `write()` is timed at **2.4 % of the
deadline** (`SESSION.md`).

**3. 32 kHz — untested on purpose, not a pass.** Decoded and resampled, with its
own scan position, but no 32 kHz source has ever been played through this board
and it is not a rate it gets listened to at. The host suite covers the rate; the
hardware path does not. Start here if a 32 kHz source ever fails to lock.

**4. Coax, if it is ever wanted.** GP1 is fine for optical — the DLR2180 on
this board outputs logic level straight into the pad. Coax needs a transformer
and a comparator, because 0.5 Vpp into a GPIO is not reliable even with the
pad's Schmitt trigger.

**5. `-DNOVA_INPUT=i2s` must be unchanged.** The regression check is that `ovr`
and `dsp` do not move against the current build — the i2s input module's
`fill()` is NULL, so the block path should be byte-for-byte what it was.

### Two bugs the board found that no host test could

**The divider scan never stepped.** It sat on 48000 against a 44100 source
indefinitely. The source was only 4 % from the edge of that position's window,
so the decoder did not fail outright — it re-framed constantly, reported
`framed`, and produced 42,286 frames a second of wreckage while climbing 1800
resyncs a second. **Every counter except the resync rate said it was working.**
The first fix (framed, and no resync in this block) failed too, because the
resyncs are bursty and the quiet blocks kept resetting the timer. The verdict
is now taken once at the end of a fixed 200 ms window, which burstiness cannot
defeat.

That is also the clearest justification the design has for giving every rate
its own scan position rather than sharing one: this source sits dead centre of
44100 and 4 % from the edge of 48000.

**`levels` read a confident `stuck LO` over a live pin.** `s_diag_or` and
`s_diag_and` are only filled when `SPDIF_RX_PIN_SCAN` is on, and it is off by
default — so the field read its reset value and named it. It prints `?` now.
The word rate on the same line was tracking the source to 300 ppm at the time,
which is what gave it away.

## Still to write

**The transmitter has never been on hardware or timed.** `output:spdif` is
written and host-verified through a loopback against `SpdifRx`, and `write()`
runs on the same block deadline that `fill()` does. **`fill()` being the only
untimed thing in the block path is exactly what hid the receiver's cost for a
whole session** — do not let `write()` be the second.

**32 kHz is decoded and resampled but untried on hardware**, and it is the rate
whose UI is furthest from the others. On the per-UI front end it also needs the
scan to step twice to reach it.

## One thing to check that is not this module

Sustained *true digital silence* is a new operating condition for Auto Gain
and the Leveller. Until now "quiet" meant an open ADC input picking up noise —
never exact zero, and never for thirty seconds. The detectors gate below
threshold and return early, so gain should simply hold, but that is an
argument and the notes in this tree are full of gain-law behaviours that
looked like tuning and were bugs.

Cheap to settle: run 30 s of digital zero through both engines on the host and
assert the gain at the end equals the gain at the start. If it ramps up during
the gap, the first track after a silence arrives amplified and gets yanked
back — which would present as a *new* plop, and would naturally be blamed on
the S/PDIF work.
