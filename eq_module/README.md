<!-- Moved out of the root README.md: this module is compiled in only when its
     build option selects it. File paths below are relative to the repo root.
     See ../README.md for the core firmware. -->

# The Equalizer

`eq_module/equalizer/Equalizer.{h,cpp}`, ten band graphic EQ.
`eq_module/module.cpp` is the glue.
**468 bytes per instance.**

    60  80  125  250  500  1k  2k  4k  8k  16k  Hz

The standard **octave centres from 125 Hz up** — each one twice the one below
it — with the bottom two placed at **60 and 80 Hz** rather than the ISO values
of 31.5 and 63. (It used to be the Winamp set, 70/180/320/600/1k/3k/6k/12k/14k/
16k, whose top three sat about a fifth of an octave apart and piled onto each
other.)

±12 dB each, plus a **preamp** over the same range. Ten bands that each boost
12 dB add to far more than 12 dB, and with nothing in front of them that lands
as clipping in the output conversion — the preamp is the usual answer, and it
sits first in the parameter list because it is what you reach for as soon as you
push bands up.

Two things about the layout worth knowing before calling either a bug:

- **Q is 1.4 for every band, which is the derived number for the octave-spaced
  eight and too wide for the bottom two.** A bell exactly one octave wide has
  Q = 1/(2^0.5 - 2^-0.5) = 1.4142, so from 125 Hz up neighbours cross near
  their half-power points and add into a smooth curve. There is one Q for the
  whole set, so the bottom two get it too.
- **60 and 80 Hz overlap heavily, and that is the deliberate trade.** They are
  0.415 of an octave apart, well inside each other's skirts, so they steer
  together rather than independently — the same behaviour the old 12k/14k/16k
  had. Measured at 48 kHz:

  | | at 60 Hz | at 69 Hz | at 80 Hz |
  |---|---|---|---|
  | 60 Hz band alone at +12 | **+12.0** | +11.0 | +8.1 |
  | both at +12 | **+19.0** | +20.0 | **+19.0** |

  With all ten at +12 dB the bass becomes the hottest part of the curve:
  **+25 dB** at 80 Hz against +18.5 dB across the octave-spaced middle. Use the
  preamp.

  The reason to accept it is the other end of the trade: the bottom of the
  range now sits where small speakers actually reproduce. The ISO 31.5 Hz band
  moves nothing you can hear on a desk speaker; 60 and 80 Hz both do.
- **There is no control below 60 Hz.** All ten at +12 dB reaches only +1.9 dB at
  20 Hz and +9.0 dB at 40 Hz. Sub-bass is shaped by the skirt of the 60 Hz band
  or not at all.
- **A band at 0 dB is skipped, not computed**, which is what keeps a mostly-flat
  EQ cheap. Its filter state is deliberately not cleared, so nudging a gain back
  through zero picks up where it was rather than clicking.

