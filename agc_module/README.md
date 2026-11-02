<!-- Moved out of the root README.md: this module is compiled in only when its
     build option selects it. File paths below are relative to the repo root.
     See ../README.md for the core firmware. -->

# Auto Gain

`agc_module/agc/AutoGain.{h,cpp}`, glued in by `agc_module/module.cpp`. Automatic volume levelling with
an exact brick-wall limiter behind it: **four controls**, **680 bytes**, no
allocation, and not one transcendental in the audio path.

The problem it solves is reaching for the volume control between one song and
the next.

| Menu | Range | Power-on | What it does |
|---|---|---|---|
| `Target` | −36…−6 dBFS | −30 | the level it aims the programme at |
| `MaxGain` | 0…24 dB | 18 | the most it may move it, **either way** |
| `Boost` | 0…12 s | 4 | how long a quiet stretch must last before it is lifted |
| `Output` | −36…0 dBFS | −1 | the wall. Nothing leaves above it, ever |

Each is a decision only the listener can make. Everything else — how fast it
moves, when it stops trying, how the wall is enforced — is fixed, because those
have right answers rather than preferences. `../SESSION.md` records what they are
and what happens if you retune them.

## What each control actually does

**`Target` — the level everything ends up at.**

An *absolute* level, not an amount of correction. The leveller measures how loud
the programme really is and moves the gain until it sits here. Turn it **up**
(towards −6) for a louder result, **down** (towards −36) for a quieter one.

The direction trips people up, because a bigger number on the panel is a
*smaller* one numerically. `Target −36` does not mean "correct by 36 dB" — it
means "make everything as quiet as −36 dBFS", which on an already-quiet source
does nothing at all. **If it seems to be doing nothing, this is the first thing
to look at.**

−30 dBFS is the **compiled-in** default and is deliberately conservative, set for
a quiet source. A hot source wants something nearer −18; −14 is roughly where
streaming services normalise and is about the loudest worth asking for.

**`Target` and the input level move together**, so this is not a number to copy
from anywhere — including from this file. It is an absolute destination, so what
makes a value usable is whether the source can reach it without pinning
`MaxGain`; change the source level and the old value stops being reachable. This
board currently runs −18 against an input arriving near −5 dBFS peak.

Start low and work up. A `Target` far above what the source can reach pins the
gain against `MaxGain`, and **at the clamp levelling stops happening at all** —
every track gets the same maximum boost and their differences pass through
untouched. A target the source can actually reach is worth more than a loud one
it cannot.

**`MaxGain` — how far it is allowed to reach.**

A bound in both directions: `MaxGain 18` lets it lift a quiet track by up to
18 dB and pull a loud one down by up to 18 dB.

**The thing to understand is what happens when it runs out.** Pinned against
`MaxGain`, levelling stops — a loud track and a quiet one come out exactly as
far apart as they went in. The effect looks like it is working, because
everything did get louder, and it is doing nothing you wanted.

So `MaxGain` needs to be comfortably larger than the difference between your
loudest and quietest material. If it is pinned, the fix is almost always more
input level, not more `MaxGain` — raising it further just brings the converter's
noise floor up with the music.

**`Boost` — how patient it is about quiet stretches.**

A quiet passage and a quiet track look identical to any detector; the only thing
that separates them is how long they last. This is where you draw that line.
Below it, a drop in level is treated as part of the music. Above it, it is
treated as a quieter track and the gain comes up.

**Lower it** if quiet tracks take too long to come up. **Raise it** if quiet
passages inside a track are being lifted and then dumped when the music returns.
4 s is the default and was measured against real material; 0 disables the wait
entirely and will wind up into every gap — it exists so the bottom of the range
is a real value, not because it is a good setting.

A track arriving too loud is still fixed in a quarter of a second whatever
`Boost` says. Cutting is never delayed.

**`Output` — the wall, and a safety net rather than a tone control.**

The absolute ceiling. No sample leaves above it, ever. Its job is to catch the
moments when the leveller is briefly caught too high — a track kicking in after
a quiet intro — and turn what would have been clipping into a fraction of a
second of limiting.

−1 dBFS suits nearly everything. Go to −3 if the output feeds a lossy encoder,
which can overshoot on decode.

The range reaches **−36** so the box can be held to a fixed low level for a
downstream input that needs one. The wall is exact at any setting — verified at
the floor, where it holds the ceiling to 0.00003 dB and the clamp still never
fires.

**Below about `Target` + 10 dB it stops being a safety net.** `Target` is an RMS
destination and this is a peak ceiling, so the distance you want between them is
the crest factor of the music, 10–15 dB for most material. Set closer than that
and the wall limits continuously by the difference instead of catching the
occasional transient — which flattens exactly the dynamics `Target` is placed
not to touch. **If the output is too loud, that is `Target`'s job, not this
one:** `Target` moves the output one-for-one and costs nothing, this attenuates
into a wall. `Output` also cannot make anything *louder* — it only ever pulls
down — so it can never do the half of levelling that lifts quiet material.

## Setting it up, in order

1. **Get the source hot first.** No setting here can manufacture headroom that
   never arrived. A PC volume slider is a digital attenuator, so anything below
   full throws away level *and* signal-to-noise before the audio reaches the
   ADC. Set it to 100 % and control loudness from `Target` instead.
2. **Check `in` on `DSP STATUS`.** You want it peaking around −6 to −3 dBFS on
   loud passages. If it reads −30, nothing below will work properly and the
   problem is upstream.
3. **Set `Target`** for how loud you want the result. Raise it towards −18 as
   far as it goes without pinning `MaxGain`.
4. **Set `MaxGain`** to comfortably more than your material varies by. There is
   no cost to headroom you do not use.
5. **Leave `Boost` at 4 s** unless quiet tracks feel slow (lower it) or quiet
   passages are being lifted (raise it).
6. **Leave `Output` at −1** unless you have a specific reason.
7. **Press `Save` on the effect list.** Nothing is written to flash until you do.

**How to confirm it is working:** `in` and `out` sit side by side on
`DSP STATUS`. Change tracks. `in` should move and `out` should not — that is the
entire behaviour this effect exists to produce, and it is directly visible.

**And read the difference of the two, never `out` on its own.** `out − in` is the
gain being applied. A few dB off either rail is healthy; sitting exactly on
±`MaxGain` is the clamp, and at the clamp levelling has stopped — `out` then
tracks `in` one-for-one and every track keeps the level it arrived with. **`out`
alone cannot tell those apart**, because the value a correctly levelled output
shows is `Target` plus the crest factor of the music, which is a similar number.

## What it does, in one paragraph

There is no `Speed` control, because a single time constant cannot be both fast
between tracks and invisible inside one. The rate is chosen per block from how
big the error is (≤ 1 dB → 3 s, ≥ 4 dB → 0.08 s, interpolated), and a *rise* has
to persist for `Boost` seconds before it is believed, then opens up gradually
rather than all at once. The detector is a mean square, **ITU-R BS.1770
K-weighted** so it levels by loudness rather than by energy, smoothed with a
0.1 s attack and 0.5 s release, gated below −60 dBFS. It is feed-forward, so
nothing here can hunt.

Measured on the host, and all of these are checks rather than claims:

| | |
|---|---|
| a track arriving **18 dB louder** is mostly fixed in | **0.22 s** |
| a track arriving **18 dB quieter** is mostly fixed in | **8.7 s** |
| gain movement against 4 dB of in-track dynamics | **0.68 dB** |
| spike after a quiet passage or a fade of up to 3 s | **0.00 dB** |
| worst change in that spike per extra 0.5 s of gap | **1.8 dB** |

The asymmetry is deliberate: a track arriving too loud is the fault you have to
fix immediately; one arriving too quiet is merely quiet for a few seconds.
**These figures are in direct tension and any change to the rate law trades them
against each other** — read the Auto Gain section of `../SESSION.md` before
retuning any of it. Two rounds of hardware bug reports went into them.

**`Target` is a K-weighted level**, so its numbers moved when the weighting went
in: bass-heavy material measures several dB below what it used to, and the same
setting produces a louder output on it. Re-tune by ear against `in` and `out`.

## The wall, and the lookahead contract

The ceiling is **exact by construction, not caught by a clamp** — the delay line
holds exactly one block, so at the moment a block is due to leave, the gain it
needs and the gain the next block will need are both already known, and the
block leaves on a ramp bounded by both. The clamp at the end of the loop is
unreachable. `../SESSION.md` has the proof; the thing not to do is undo it by
accident.

The reduction is **stereo-linked**, so a transient on one side pulls both down
together and the image does not shift sideways.

**The lookahead is the block, and that is a contract.** `process()` adopts the
caller's block size, so halving `AUDIO_BLOCK_FRAMES` halves the latency for
free; `agc_module/module.cpp` carries a `static_assert` that it still fits inside
`AUTOGAIN_LOOKAHEAD`. Exceeding it would silently demote the wall back to a
clamp, which is to say back to clipping.

So Auto Gain is the one effect that changes the latency of the box: **1.33 ms on
top of the ~2.7 ms** the buffering already costs. Worth remembering before
putting it in a chain twice.

## Where it belongs in a chain

**Last.** It is the only thing in the firmware that bounds the output, and any
effect placed after it can push the signal straight back over the ceiling it
just enforced. An EQ band or a BBE process setting both can.

## What it costs

**47.1 µs average and 54 µs peak, 4.1 % of the 1333 µs deadline**, measured on
the board over 81 seconds of programme material at `ovr=0`. On the one block in
eight that also runs the spectrum transform the worst case is **252 µs, 19 %**.

## The host test

`agc_module/agc/test_autogain.cpp`, **deliberately absent from that module's
`SOURCES`**. Forty-five checks, all passing. AutoGain needs no stubbing to build on a host.

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /nologo /EHsc /O2 /std:c++17 /Iagc_module\agc agc_module\agc\AutoGain.cpp agc_module\agc\test_autogain.cpp /Fe:ta.exe
ta.exe
```

**Re-run it after touching the detector, the rate law, the gate, the wall
schedule, the weighting or the delay line.** `../SESSION.md` records the harness
traps — they are not obvious and every one of them was learned the hard way.

