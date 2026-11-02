<!-- Moved out of the root README.md: this module is compiled in only when its
     build option selects it. File paths below are relative to the repo root.
     See ../README.md for the core firmware. -->

# The Noise Gate

`noisegate_module/noisegate/NoiseGate.{h,cpp}`, glued in by
`noisegate_module/module.cpp`. A broadband gate: below
the threshold it pulls the signal down to silence, above it the signal passes
untouched. **Four controls, 68 bytes**, no allocation, no lookahead and no
transcendental in the audio path.

The problem it solves is a source that is clean while something is playing and
audibly noisy in the gaps — hiss, hum, mic bleed.

| Menu | Range | Power-on | What it does |
|---|---|---|---|
| `Thresh` | −80…0 dBFS | −40 | the level below which the gate closes |
| `Attack` | 0.1…50 ms | 2 | how fast it opens once the level crosses back above it |
| `Hold` | off…500 ms | 50 | how long it stays open after the level drops back below |
| `Release` | 5…2000 ms | 150 | how fast it closes once `Hold` has run out |

`Attack`, `Hold` and `Release` are enums over nine or ten log-spaced stops
rather than plain numbers — 0.1 ms cannot be written as an integer and 2000 of
them is a hundred turns of the knob. **The stops include the engine's own
defaults**, so power-on lands exactly on a row. `Thresh` stays a plain number
at 1 dB a detent: it is the one control that has to be set against a particular
noise floor, so resolution is worth the four turns end to end.

## Setting it up

**`Thresh` is the whole job and the other three are how it sounds doing it.**
It is an absolute level, like Auto Gain's `Target` and unlike anything on a
tone control, so **it moves with the input level** — the value below is right
for this board and is not a number to copy.

1. **Read `in` on `DSP STATUS` with the source paused.** That is the noise
   floor, and it is what you are trying to shut out.
2. **Read it again with the quietest material you care about playing.** That is
   what must get through.
3. **Put `Thresh` between the two**, nearer the floor than the music. Too high
   and the quiet ends of notes are chopped; too low and the gate never closes.
4. **Then `Hold`**, if the gate is closing inside phrases — raise it. If it
   hangs open too long after they end, lower it.
5. **Then `Release`**, if the tail is heard as a cut rather than a decay.
6. **`Attack` last, and usually not at all.** 2 ms is fast enough that a
   transient is not audibly clipped on the way in and slow enough that a single
   cycle of a low note cannot chatter the gate.
7. **Press `Save` on the effect list.** Nothing reaches flash until you do.

**If it does nothing, `Thresh` is below your noise floor.** If it swallows the
music, it is above it. There is no third possibility, and no other control can
fix either one.

## What is fixed, and why

The gate pulls down to **−96 dBFS rather than to a true zero**. A genuine zero
costs nothing to reach and buys nothing — −96 dB is inaudible against any
programme material — while stopping just short of it keeps the tail of the ramp
out of denormals.

The detector is a **broadband mean square, smoothed across blocks** with a fast
fixed attack and release. That smoothing is *not* the `Attack` and `Release` on
the menu: those set how the **gain** moves once the detector has decided; this
only steadies what the detector sees, the same split Auto Gain draws between
its internal constants and the `Boost` you can turn.

The gain is **slewed every sample**, so nothing here can click at any setting.

## Where it belongs in a chain

**First** — the opposite end from Auto Gain, and the two are worth thinking of
as a pair. A gate decides whether to pass the signal at all, so it wants to see
the signal as it arrived, before anything has lifted the noise floor it is
shutting out. A gate placed *after* a leveller is judging quiet parts that the
leveller has just boosted towards the threshold.

**It adds no latency.** There is no lookahead, because a gate has nothing to
protect against crossing a ceiling.

## The host test

`noisegate_module/noisegate/test_noisegate.cpp`, **deliberately absent from that
module's `SOURCES`**, the same way `agc_module/agc/test_autogain.cpp` is. Twenty-eight checks, all passing.
NoiseGate needs no stubbing to build on a host.

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /nologo /EHsc /O2 /std:c++17 /Inoisegate_module\noisegate noisegate_module\noisegate\NoiseGate.cpp noisegate_module\noisegate\test_noisegate.cpp /Fe:tn.exe
tn.exe
```

It measures the things listening cannot: that `Hold` bridges a dip shorter than
itself and not one longer, that `Attack` and `Release` are the ramp times they
claim, that no sample-to-sample gain step exceeds the slew rate at any block
size, and that a stereo signal is gated as one signal rather than two.
**Re-run it after touching the detector, the state machine or the slew rates.**

## What it costs

**Not measured** — see the note at the top of this file. Two passes over the
block, one summing squares and one multiplying, with no transcendental in
either, so it should be among the cheapest things here; `dsp` on `DSP STATUS`
is how to find out.

**Measure it against signal, not silence.** Every detector in this firmware
gates and returns early, so an idle input reads far below the real figure — the
same trap the DeeFloat benchmark records, where silence measures 40 % of the
cost on programme material.

