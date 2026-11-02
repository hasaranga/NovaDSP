# spdif_output_module

S/PDIF output: `-DNOVA_OUTPUT=spdif`, or `output:spdif` to `make_firmware.bat`.

**24 host checks, all passing. Builds clean, zero warnings.**

**Running on hardware, timed, and verified by loopback.** A TOSLINK
transmitter is fitted on GP22. `write()` costs **30.5/32.3 us, 2.4 % of the
1333 us block deadline, 146 cycles a frame** — measured, not the "under 2 %"
this file used to estimate. The slot gap reads **2..2**, which is the
*measurement* of the claim below that this ring cannot slip.

**The optical loopback passes.** GP22 out into the DLR2180 receiver on GP1,
decoded by this project's own receiver:

```
spdif: 192000 words/s | scan 48000 Hz, framed, 48000 frames/s
       LOCKED, 48000 Hz, trim +0.0 ppm, gain 1.00 | parity 0 resync 0 (0/s)
```

Exactly 48000 frames a second, trim converging to zero — which is the right
answer, because in a loopback the source rate *is* the output rate. So the
biphase encoding, the preambles, the parity and the `clk_sys/50` divider are
all correct, on real hardware, at both ends. That is `tx/test_tx.cpp`'s
loopback done with the real PIO instead of a model of it.

It also found a live bug in the *receiver's* resampler, which nothing else
could have: see `SESSION.md`, *The ASRC could not run faster than 1:1*.

| build | text | bss |
|---|---|---|
| i2s in, i2s out (baseline) | 115,284 | 188,436 |
| **i2s in, spdif out** | **117,092** | **192,584** |
| spdif in, spdif out | 126,784 | 234,828 |

So S/PDIF output costs **+1,808 text and +4,148 bss** — almost all of it the
4 KB DMA ring. Compare the receiver's +9,944 / +42,244, and the difference is
the whole story of this module.

## Why this is the easy half

Everything in `spdif_input_module` — the ASRC, the rate estimator, the lock
states, the fade policy, 42 KB of SRAM — exists because **a receiver has to
follow somebody else's clock**, and the RP2350 has no steerable audio PLL to
follow it with.

Transmitting, this box is the master:

```
UI rate = 128 UI/frame × 48000 = 6.144 MHz
clkdiv  = 307200000 / 6144000  = 50 exactly
```

An integer divider, so every edge lands on a `clk_sys` tick with no dither and
the transmitted jitter is the crystal's and nothing else. That is the same
argument that put MCLK on `CLK_GPOUT` rather than a PIO pin — and here it costs
nothing to have. **There is nothing to track, nothing to resample and no state
to lose.**

## The geometry is what makes it cheap

A subframe is 4 preamble time slots plus 28 data bits, and biphase turns each
data bit into 2 UI — so a subframe is 8 + 56 = **64 UI, exactly two 32-bit
words**, and a frame is exactly four. Nothing carries between frames and no bit
accumulator is needed:

```
word 0 = preamble(8) | nibble0(8) | nibble1(8) | nibble2(8)
word 1 = nibble3(8)  | nibble4(8) | nibble5(8) | nibble6(8)
```

Seven nibbles of four data bits, 8 UI apiece. Encoding is therefore **a table
lookup per nibble — 14 per frame** — rather than a loop over 56 bits. Estimated
under 2 % of a core; not measured on target.

## The ring cannot slip

A block is exactly `AUDIO_BLOCK_FRAMES × 4 = 256` words, and the DMA drains one
block's worth in exactly one block period. Both sides are divided from the same
`clk_sys`, so producer and consumer are locked **by construction rather than by
a control loop**. `write()` targets the slot two ahead of the one being read
and stays there for ever.

## The test is a loopback

`tx/test_tx.cpp` encodes, expands the UI stream to receiver samples, and
decodes it with `spdif_input_module`'s `SpdifRx`. The audio has to come back
**bit exact**:

| | |
|---|---|
| 4000 frames round trip | **0 mismatches**, both channels |
| parity errors / resyncs | **0 / 0** |
| run lengths on the wire | 1 UI and 2 UI present, 3 UI only at preambles, **nothing longer** |
| silence | still a legal biphase line, decodes as silent frames |
| block position | 192 frames returns to the top |

That is worth more than checking the encoder against a table of expected bytes
written by the same person on the same afternoon — it tests preambles, biphase,
parity, framing and sample packing against an independent implementation.

**It bends one rule to do it**: the test includes another module's header,
which `nova_modules.cmake` is arranged to prevent. It is confined to a host
program absent from every `SOURCES`, and written as
`../../spdif_input_module/...` — the form the build makes you type when you
mean it. The three preamble tables in this tree (transmitter, receiver, test
source) are separate copies, and this loopback is what holds them together: if
any of them drifts, nothing decodes.

### What it found

**The receiver could not decode digital silence.** All-zero samples are all
zero bits, and a zero bit in biphase is one 2 UI run — so a silent stream is
~90 % 2 UI runs and the 1 UI cluster collapses to only what the preambles
contain. `SpdifRx`'s threshold rule assumed 1 UI runs were common, picked the
2 UI cluster as the unit, doubled every threshold and decoded **nothing at
all**.

That is not a corner case: it is what a source sends between tracks, which is
exactly when it has to keep working. Every source in the receiver's own suite
was a sine wave, so nothing there had a silent stream in it.

Fixed by using the structure instead of a threshold — the most populated bin is
either the 1 UI or the 2 UI cluster, and the two are told apart by whether
anything sits at half its length. Preambles guarantee the 1 UI bin is never
empty, only sometimes sparse. `test_spdif.cpp` gained the silent-source case.

## What this build gives up

**GP16 stays a plain input, so there is no analogue output.** `attach()`
claims GP22 and does not call `audio_claim_dout()`, which means the PCM5102A's
DIN floats. `output:spdif` is a digital-only configuration.

Wanting both analogue and digital out means either a module that drives DOUT
*and* encodes S/PDIF — a perfectly good thing to write, and the honest place
for it, since it is one decision about where the block goes — or an interface
change to a list of outputs. The list is the wrong answer for the same reason
there is one display.

## What still needs the board

**1. ~~That the pin is actually carrying S/PDIF.~~ Done — the loopback locks.**
See the top of this file. `spdif_tx.pio` shifts on the pin at `clk_sys/50` and
what comes out decodes bit-clean.

**A note for anyone repeating it: it is a feedback loop.** The input then *is*
the output, so whatever effect is live compounds its own gain every 2.7 ms
until it pins. Select Passthru at 0 dB on the knob first. With no external
signal the loop is silent by construction — `in`/`out` reading −120 dBFS in a
loopback is the correct result, not a failure.

**2. The analogue side.** GP22 is the output. A 3V3 logic pin is not an S/PDIF
output: **coax** wants 0.5 Vpp into 75 Ω, which is a divider and a blocking
capacitor at least, and a transformer if isolation matters. **Optical** takes
3V3 logic straight into a TOSLINK transmitter and needs nothing else, which is
why optical is the easy bring-up.

**3. Whether a real receiver locks**, and what it reports the rate and word
length as. The channel status bytes are write-only here — `SpdifRx` does not
parse channel status, so the loopback proves the framing and the audio but not
those five bytes. Getting the rate field wrong presents as a DAC that locks and
plays at the wrong speed, or refuses.

**4. ~~The CPU cost.~~ Done: 2.4 % of the deadline, 148 cyc/frame.** It is on
its own `spdif-tx:` line in a dev build rather than in `dsp`, because
**`dsp` times `effect_process_block()` and nothing else** — which is exactly
how the receiver's decoder hid at 73 % of a core for a whole session while
`dsp` read 12.6 %. `write()` was the second untimed thing in the block path;
it is not any more.

**5. `input:spdif output:spdif` puts two programs and two state machines on
PIO2**, with a 6.144 MHz transmitter beside a receiver resolving 4-sample runs.
The pins are at opposite ends of the header for that reason. It builds; nothing
says it is quiet.
