<!-- Moved out of the root README.md: this module is compiled in only when its
     build option selects it. File paths below are relative to the repo root.
     See ../README.md for the core firmware. -->

# FX Chain

`fxchain_module/module.c`. An effect whose `process()` is a loop over other effects'
`process()`, in the order you put them in. Up to **four** slots. It appears in
the list like any other effect; a press selects it, a second press opens its
editor.

## The editor

The page lists the effects a chain may contain. Pressing one adds it to the end
of the chain and puts its **order number** in the value column; pressing one
already in takes it out and closes the gap, so the numbers stay 1..n with
nothing missing in the middle. A press that cannot be honoured because all four
slots are full says `full` on the row rather than doing nothing silently.

`< Back` is the first row and `Reset` the last; `Reset` empties the chain.

**Two effects are not on that list**, and neither is excluded by name. Anything
declaring `EFFECT_F_COMPOSITE` is out, because a chain containing a chain
recurses until the stack gives out — which covers FX Chain itself and any other
composite a future module might add. And `Passthru` sets `EFFECT_F_NOT_IN_CHAIN`
for itself, because it is a gain trim whose whole purpose is to be the thing you
A/B an effect *against*.

Parameters of an effect inside the chain are edited the usual way — select that
effect on its own, tune it, then select `FX Chain` again.

## The one thing to remember

**A slot stores a position in the candidate list** — not a name, and not a
registry index either. Everything else in the settings store is keyed by name
precisely so registration order can change safely, and this is the one field
that cannot be, because a parameter value is an `int16`.

So **anything that changes the candidate list changes what a stored chain
means** — enabling an effect module, disabling one, or changing an `ORDER`
field. That used to need a `VERSION` bump somebody had to remember. It is now
automatic: the settings store is keyed by a hash of every effect's name *in
registration order*, and all three of those change it.

## What it costs

The chain is timed as one hook, so `dsp` shows the total. **Nothing throttles it
and nothing warns before the fact — `ovr` is the answer**, and the spectrum
transform still lands on the same deadline on one block in eight.

Two things that add up faster than the arithmetic suggests:

- **Each effect converts the block int→float and back again.** Four in series is
  eight conversions rather than two, about 28 µs each way at 64 frames, so
  roughly 170 µs of the block is spent on format alone.
- **Auto Gain adds 1.33 ms of latency each time it appears**, and it belongs
  last.

Duplicates are legal — the editor will not create one, but a settings record
could. An empty chain is a bit-exact passthrough.

## Reading a chain off the console

**dev build only.** The boot banner names the effect the board came up on, and
expands a chain into its running order:

```
  active effect: FX Chain - Auto Gain > Equalizer > Leveller
```

It sits directly above the parameter line for a reason: **that line is
DeeFloat's whatever is active.** On a board running a chain it describes
something you are not listening to, and this is the line that says so. An empty
chain reads `- empty, bit-exact passthrough`.

