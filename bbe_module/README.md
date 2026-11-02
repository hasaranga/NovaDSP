<!-- Moved out of the root README.md: this module is compiled in only when its
     build option selects it. File paths below are relative to the repo root.
     See ../README.md for the core firmware. -->

# The BBE Maximizer

`bbe_module/bbe_maximizer/SonicMaximizer.{h,cpp}` — vendored, single precision, no
allocation. `bbe_module/module.cpp` is the glue. **444 bytes per instance.**

The high band is boosted by an amount driven by how much low-band energy is
masking it, so bass-heavy passages get more lift than sparse ones — that program
dependence is what separates it from a fixed treble shelf. A Lo Contour bass
shelf and a small complementary mid dip complete it. Signal flow is an LR4
three-way split, per-band shaping, sum, output trim, safety limiter.

| Menu | Engine | Note |
|---|---|---|
| `Process` 0–10 | 0–10 | |
| `Contour` 0–10 | 0–10 | |
| `Dynamic` 0–10 | 0–100 % | coarsened; 100 detents is five turns of the knob |
| `LoFreq` enum | 60–400 Hz log | nine stops, including the engine's 150 Hz default |
| `HiFreq` enum | 600–4000 Hz log | nine stops, including the engine's 1200 Hz default |
| `Output` −12…+12 dB | same | |
| `Bypass` on/off | same | |

The engine's safety limiter is pinned on at −0.2 dBFS and is deliberately not a
parameter. `get()` maps the crossovers back by **nearest stop** rather than
trusting a stored index, so a value set from anywhere still lands on the right
row.

