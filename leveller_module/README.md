<!-- Moved out of the root README.md: this module is compiled in only when its
     build option selects it. File paths below are relative to the repo root.
     See ../README.md for the core firmware. -->

# Leveller

`leveller_module/leveller/Leveller.{h,cpp}`, glued in by
`leveller_module/module.cpp`. Audacity's *Leveller* — a static soft-compression
curve — repackaged for real time: **two controls and a bypass**, no allocation,
no lookahead, one float of envelope state.

The problem it solves is a performance whose quiet parts are too quiet against
its loud ones. It is not a volume control between tracks — that is Auto Gain —
and it is not a noise gate. It works *inside* a passage.

| Menu | Range | Power-on | What it does |
|---|---|---|---|
| `Degree` | 1…5 | 2 | how many times the curve is applied — **the only control that moves the cost** |
| `Thresh` | −80…−20 dB | −70 | the bottom of the boosted region |
| `Bypass` | off / on | off | hand the input straight back |

**Five more parameters exist in the engine and are not on the panel.** That is
deliberate; *Which five, and why they are not on the knob* below says which and
what they are pinned at.

## What each control actually does

**`Degree` — how hard the curve works.**

The curve compares the level against six limits and applies that segment's own
gain: the middle segments have gain 1.2, the outer ones 0.8, so quiet material
is lifted and loud material held back. `Degree` is how many times that is run,
so it is a strength control and it compounds — 5 is not two and a half times 2,
it is the curve applied five times. **2 is the power-on value and is where to
start**; 4 and 5 are audible as levelling rather than as a fixed balance.

**`Thresh` — where lifting stops.**

The bottom edge of the boosted region, and **not a gate threshold**: below it
the curve backs off (segment gain 0.8) instead of lifting. So it is the line
between "quiet part of the music" and "the noise floor I do not want brought up
with it".

Set it *below* the quietest passage worth lifting. Too high and that passage is
pushed down instead of up; too low and the hiss between phrases comes up with
everything else. −70 dB is the compiled-in default and is deliberately low,
which suits a clean source. A hissy one wants it raised until the gaps stay
quiet — and if the gaps are the whole problem, the Noise Gate is the right
effect, not this one.

**`Bypass` — the A/B.**

Not a branch around the effect: it is a destination for the two gain ramps that
are already running, so the wet path fades out over ~20 ms and cannot click,
even mid-passage. Once the ramp arrives the wet multiply is skipped.
**It does not make the effect free.** The envelope follower keeps tracking
while bypassed - which is what stops it swelling on the way back in - so only
the wet multiply goes. A bypassed Leveller measures about what a running one
does. Use `Passthru` to A/B against if you need the CPU back, not `Bypass`.

## Which five, and why they are not on the knob

`module.cpp` publishes three of the engine's eight parameters. The other five
sit at the engine's own defaults, set explicitly at init and never touched
again:

| Hidden | Pinned at | Why it is not a control |
|---|---|---|
| `Smooth` | on | the envelope-gain mode. Off is the original per-sample waveshaper, which measures ~10 % distortion on loud bass — a sound, not a setting, and not one to leave a knob pointing at |
| `Attack` | 10 ms | with `Smooth` on, these two are the ballistics of the gain, and they are the pair that turn a leveller into a pumping compressor when set wrong |
| `Release` | 200 ms | as above. The defaults are what the engine was voiced at |
| `Output` | 0 dB | unity. Makeup belongs to Passthru's trim or to Auto Gain, both already in the menu; a second output trim here is only a way to clip twice |
| `Mix` | 100 % | fully wet. `Degree` already sets how hard the curve works, so a dry blend is a second control for one axis |

**There is no hidden flag on `effect_param_t`, and there does not need to be.**
The UI walks `0..param_count-1`, the settings record stores `param_count` values
per effect, and `modules_layout_id()` hashes the same range — so a parameter
left out of the descriptor is invisible on the panel, absent from flash and
outside the layout hash, from one omission. A "hidden" bit would have had to be
honoured separately in all three places.

Un-hiding one is adding a row to `kEngineParam[]` and `kNames[]` in
`module.cpp` and raising `P_COUNT`; the descriptor is built from the engine's
own `ParamInfo` table, so range, unit and switch-ness come across on their own.
**It invalidates the settings store** — `param_count` is hashed, so `Target`,
`Thresh` and the active effect need putting back and `Save` pressing, the same
as any other layout change.

## Where it belongs in a chain

After the Noise Gate, before Auto Gain — which is what `ORDER 65` says, sitting
between the gate at 50 / Auto Gain at 60 and FX Chain at 70.

A gate wants to see the signal as it arrived, and a leveller lifts the quiet
parts *towards* the threshold the gate is judging by, so gating after levelling
means gating a signal whose quiet parts have just been boosted. Auto Gain still
belongs last, because it is the only thing that bounds the output.

It adds **no latency**: no lookahead, and the only state is a peak envelope.

## What it costs, and what it used to cost

**The engine is single-precision throughout, and that was not free to discover.**

The first version kept the original's doubles for the envelope, the two
parameter ramps, the curve division and the meters. It measured **22 % of the
block deadline** on the board — ~293 µs of the 1333 µs a 64-frame block gets at
48 kHz, dearer than DeeFloat for an effect doing a fraction of the work.

The algorithm was not the problem. `objdump` found **56 double call sites** in
`process()`, and roughly 56 of them ran per frame:

| Where | Double ops / frame |
|---|---|
| `rampTo()` ×2 (gain, mix) | 14 |
| Envelope: per-channel `f2d` + compare, then the one-pole update | 10 |
| `curveGain()` — 2 passes × ~4 segment tests, each `f2d` + `dcmple` | ~20 |
| The `y / e` divide and its conversions | 3 |
| Curve ramp, three `d2f` for the float inner loop | 6 |
| Peak meters | 6 |
| **Total** | **~56** |

56 operations into the measured ~1400 cycles per frame is ~25 cycles each, which
is what a call into the SDK's `__wrap___aeabi_d*` routines costs. Those are
bootrom-backed and already the fast path — the M33 has a single-precision FPU
only, so there is no flag that makes a double cheaper. Every one of those calls
was in the *control* path: an envelope, two ramps and one divide, computed at
double width and then immediately narrowed to float to be used.

**So the audio path was converted to float.** The curve tables, the envelope,
both ramps, the peaks and the derived coefficients are `float`; `mParams[]`,
`dbToLinear()`/`linearToDb()` and the coefficient derivation stay `double`,
because they run when a control moves rather than per frame. The public
interface did not change, and neither did `module.cpp`.

`process()` now contains **zero double operations and no calls at all** — it is
a leaf function of single-precision VFP instructions, 354 of them against the
583 it compiled to before.

**Measured after the conversion: `dsp` 8 % at `Degree 5`** - the worst case the
knob can select, and down from 22 % on a double engine at `Degree 2`. That is
~107 µs of the 1333 µs block, ~513 cycles a frame. `Degree 2` runs three fewer
passes through `curveGain()` and reads lower; it has not been written down.

**An instruction count is not a cycle count, and this is how far out that can
be.** 583 instructions became 354 and the estimate from it was "low single
digits" - the answer is nearer three times that. What the estimate ignored is
that the per-frame path is not the function: `curveGain()` walks up to six
segment tests per pass, so at `Degree 5` that is up to thirty compare-and-branch
sequences a frame, plus a `VDIV.F32` that does not pipeline and a two-channel
inner loop. **The conversion was still worth 14 points of deadline** - what was
wrong was the prediction, not the change.

Two things that did not change with the conversion:

- **`Bypass` still does not make it cheap.** The envelope follower runs whether
  bypassed or not — that is what stops the gain swelling when bypass comes back
  off — so only the wet multiply goes. `Passthru` is the A/B that gives the CPU
  back.
- **`Degree` still scales part of it** — each extra pass is another trip through
  the six segment tests, so `Degree 5` is dearer than `Degree 2`.

### What to watch if you edit the engine

`-Wdouble-promotion` is pinned in this build, so an accidental `float` meeting a
`double` in the audio path is a warning rather than a silent 25 cycles. **A
`static_cast<double>` inside `process()`, `curveGain()` or `levelOneFrame()` is
a bug, not a tidy-up.**

The one place precision needed thought is the envelope coefficient. At 10 ms and
200 ms, `exp(-1/(t·Fs))` lands within 1e-4 of 1.0, so computing `1.0f - coef`
per frame in float would have kept about three significant figures of a value
the derivation had produced to fifteen. It is stored as **alpha** — `1 - exp(...)`,
subtracted once in double when the control moves — which removes a per-frame
operation and keeps the precision. That is a precision-of-derivation argument
and nothing else: the same rewrite applied to the 20 ms gain ramp changes the
result by nothing, for the reason the next section works out.

## The host test, and what stands behind the conversion

`leveller/test_leveller.cpp` — **43 checks, all passing**. Not in the build; it
is a host program, like the gate's and Auto Gain's, and its build command is in
its own header.

What makes it worth having is its first half: **a copy of the engine as it was
before the float conversion**, doubles and all. The checks run the two side by
side on the same input and compare sample for sample, which is the only way to
say anything honest about a change that compiles clean either way.

| Group | What it covers |
|---|---|
| Smooth mode, Degree 2 | tones at −1/−12/−30/−60 dBFS, 40 Hz, stereo noise, a quiet/loud/quiet step, a full-scale sweep |
| Degree 1…5 | every setting the knob can reach |
| The five hidden parameters | classic waveshaper, threshold −24 dB, fast ballistics, mix and trim — so un-hiding one is not a trap |
| Locating the divergence | the two experiments below |
| Invariants | bypass, curve monotonicity and continuity, denormal guards, silence, full-scale bounds, the sign fix, parameter clamping, a rate change mid-stream, and that the effect actually levels |

**Worst divergence anywhere in the suite: 1.4e-5, at −12 dBFS** — −97 dBFS, or
5.7e-5 of gain, which is **0.0005 dB**. It is a gain offset rather than added
noise, and it does not depend on level.

### Where that 5.7e-5 comes from, since two obvious answers are wrong

The smooth-mode gain is smoothed by a 20 ms one-pole, and **a one-pole in float
has a stall band**: once `curve` is close enough to `target` that the correction
rounds away, it stops moving, while still

```
0.5 × ulp(curve) / (1 − coef)
```

short of where it should be. For a gain near 1.44 that is `0.5 × 1.19e-7 /
1.0416e-3` = **5.72e-5**. Measured: **5.67e-5**. Prediction and measurement agree
to 1 %, which is the reason to believe it rather than the next story.

Two explanations that came first and are both wrong — refuted by the two
"locating the divergence" groups rather than by argument:

- **"It is the release follower."** It is a leaky integrator too, and its 200 ms
  release amplifies rounding by 9600, so it looked obvious. But 20 ms and
  2000 ms release give the same answer to within 5 %.
- **"The ramp is in the wrong algebraic form."** Rewriting `dest + (current −
  dest) × coef` as `current + (dest − current) × alpha` changed the result by
  nothing at all, to the last digit. The stall band is a property of the
  recurrence, not of how it is spelled.

Neither is worth a cycle to chase at 0.0005 dB. What the groups are for is the
next person, who will otherwise re-derive both wrong answers.

## Build option

```
-DNOVA_EFFECT_LEVELLER=OFF
```

to leave it out. It is on by default. **A new module directory needs one manual
CMake reconfigure** before the glob sees it; editing an existing `module.cmake`
does not.
