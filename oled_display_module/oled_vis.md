# Adding a visualisation to the OLED idle screen

How to take a video and end up with it playing on the 128x64 OLED, in rotation
with the spectrum analyser. Three firmware edits and a rebuild; no core file is
touched and no other module knows about any of it.

> **Two documents, two halves.** [`../oled_vis/README.md`](../oled_vis/README.md)
> is the *generator* — how to load a video, crop it, dither it and export it.
> **This file is the firmware half**: what to do with the files it wrote. Read
> that one to make an animation, this one to make the box play it.
>
> One thing that document is out of date about: its *Playing it* section shows a
> counter-driven loop writing straight into `fb`. This module does neither —
> see *Why it is written this way* below. Follow this file for the wiring.

## What is already there

| | |
|---|---|
| `oled_anim1.{c,h}` | 61 frames, 15 fps, 4.07 s loop, 62,464 bytes |
| `oled_anim2.{c,h}` | 101 frames, 20 fps, 5.05 s loop, 103,424 bytes |
| `oled_anim3.{c,h}` | 61 frames, 20 fps, 3.05 s loop, 62,464 bytes |
| `oled_anim4.{c,h}` | 76 frames, 25 fps, 3.04 s loop, 77,824 bytes |
| `oled_anim5.{c,h}` | 81 frames, 20 fps, 4.05 s loop, 82,944 bytes |
| `oled_anim6.{c,h}` | 89 frames, 20 fps, 4.45 s loop, 91,136 bytes |
| `kAnims[]` in `ui.c` | the table the rotation picks from |
| `sh1106_blit()` | one frame into the framebuffer, as a `memcpy` |
| `sh1106_snapshot()` / `sh1106_slide()` | the transition between two visuals |

Every animation array is `__in_flash`, so all 480,256 bytes sit in flash and
**none of it is in SRAM**. Verified on the link: `oled_anim1_frames` at
`0x100001ec` through `oled_anim6_frames` at `0x1005f1ec`, all in the `0x10……`
window, whose `.rodata` has VMA == LMA and so is never copied to RAM. `ram` on
`DSP STATUS` reads **286/512k** — six animations cost about a kilobyte of SRAM
between them, and all but a few bytes of that is the transition's held frame.

> **`arm-none-eabi-size` will tell you the animations are in `text`.** They are
> not in SRAM. `size` counts flash-resident `.rodata` in that column, so the
> honest SRAM figure is `__bss_end__`, which is what `ram` shows. The ST7789
> module's 398 KB of artwork has the same reading; the top-level `README.md`
> says so under *Leaving things out*.

## The short version

1. Run `oled_vis/animtool.exe`, set **Name** to `oled_anim3` and **Dir** to this
   directory, layout **page-packed**, **in flash** on, `.h` + `.c` pair.
2. `#include "oled_anim3.h"` in `ui.c`, beside the other two.
3. Add a row to `kAnims[]` in `ui.c`.
4. Add `oled_anim3.c` to `SOURCES` in `module.cmake`.
5. `make_firmware.bat oled flash`

Nothing else. No page enum to extend, no count to bump, no timer to add.

---

## Step 1 — generate

```bat
cd oled_vis
animtool.exe
```

It opens a browser. [`../oled_vis/README.md`](../oled_vis/README.md) covers the
source, the crop, the frame rate, the threshold and the dither modes. Four
export settings are not free choices for this module:

| Setting | Must be | Why |
|---|---|---|
| **Name** | `oled_anim<N>`, unique | It becomes every macro and both C symbols. Two animations exported under the same name are a duplicate-symbol link error at best, and silently the same animation twice at worst |
| **Dir** | this directory | So the `.c` sits beside the `module.cmake` that lists it |
| **Layout** | **page-packed** | `data[page * 128 + x]`, bit `(y & 7)`, bit 0 topmost. That is `fb[page][x]` exactly, which is what makes drawing a frame a `memcpy` instead of 8192 `sh1106_pixel()` calls. Row-packed will compile and draw garbage |
| **In flash** | **on** | Off puts the whole animation in SRAM, and this is a `copy_to_ram` build with about 228 KB spare. One 100-frame animation would take half of it |

Keep the `.h` + `.c` pair. Header-only puts a copy of the array in flash for
every translation unit that includes it.

**Size it against the flash budget.** Every frame is exactly 1024 bytes,
whatever is in it. With the six current animations in, the image is 586,904
bytes of the 4 MB part, leaving **3.43 MB — about 3,500 more frames** after the
8 KB settings store. Length is essentially free here; it is SRAM that is scarce,
and `__in_flash` is what keeps this out of it. Re-measure with
`arm-none-eabi-size` or the `.bin` rather than trusting this figure once a few
more are in.

## Step 2 — three edits

### `ui.c`, the include

```c
#include "oled_anim1.h"
#include "oled_anim2.h"
#include "oled_anim3.h"     // new
```

### `ui.c`, the table

Find `kAnims[]` under `---- the animations ----`:

```c
static const oled_anim_t kAnims[] = {
    { oled_anim1_frames, OLED_ANIM1_FRAMES, OLED_ANIM1_FRAME_US },
    { oled_anim2_frames, OLED_ANIM2_FRAMES, OLED_ANIM2_FRAME_US },
    { oled_anim3_frames, OLED_ANIM3_FRAMES, OLED_ANIM3_FRAME_US },   // new
};
```

**Table order is rotation order.** `ANIM_COUNT` is `sizeof kAnims / sizeof
kAnims[0]`, so there is no count to keep in step, and the rate and the length
come off the row rather than out of the drawing code — which is why animations
recorded at different frame rates need no special case.

Add the pair of `_Static_assert`s next to the others while you are there. They
are cheap and they turn "the panel draws noise" into a build failure:

```c
_Static_assert(OLED_ANIM3_W == SH1106_WIDTH && OLED_ANIM3_H == SH1106_HEIGHT,
               "oled_anim3 is not this panel's size");
_Static_assert(OLED_ANIM3_BYTES == ANIM_BYTES,
               "oled_anim3 frame is not one framebuffer");
```

### `module.cmake`

```cmake
SOURCES  module.c sh1106.c encoder.c ui.c oled_anim1.c oled_anim2.c oled_anim3.c
```

**Forgetting this is the most likely mistake**, and the symptom names itself: an
undefined reference to `oled_anim3_frames` at link. Editing `module.cmake`
re-runs the CMake configure on its own — only a whole *new module directory*
needs a manual reconfigure.

## Step 3 — build and flash

```bat
make_firmware.bat oled flash
```

Or build and flash separately:

```bat
make_firmware.bat oled
flash.bat nobuild
```

**`flash.bat nobuild` flashes whatever is already in `build\`.** If you built
into somewhere else with `out:<dir>`, use `flash.bat dir:<dir>` or you will
re-flash a stale image and conclude the change did not work.

---

## How the rotation works

Three constants, all at the top of `ui.c`:

```c
#define IDLE_MS         10000u   // no input -> the idle screen
#define VIS_ROTATE_MS   20000u   // each visual gets this long
#define TRANS_MS          500u   // the slide between two of them
```

The spectrum comes up first, then **a randomly chosen different visual** every
twenty seconds, each arriving on a horizontal slide:

    spectrum --slide--> ? --slide--> ? --slide--> ? ...
       20 s      0.5 s     20 s        20 s

- **The spectrum always goes first, and always gets a full turn.**
  `vis_since_ms` is reset when the idle screen is entered, so it never inherits
  the remainder of somebody else's turn.
- **The choice is uniform over everything except what is showing.** `vis_go_to()`
  picks out of the `VIS_TOTAL - 1` that are not current and steps over the one
  that is — one modulo, no retry loop, because "draw again until it differs"
  has no bound on how long it takes. The spectrum is index 0 and `kAnims[i]` is
  `i + 1`.
- **Every part wakes identically, and waking never slides.** Any input returns
  to `resume_page`, cancels a transition in progress and is consumed by the
  wake, so which part was showing when the knob moved is not something the user
  has to have noticed.
- **Rotating is not input.** It does not touch `last_input_ms` and does not
  restart the idle timeout.
- **`PAGE_ANIM` is one page, not one per animation.** `vis_anim` says which.
  Adding an animation therefore does not touch `page_t`.

Since the order is random, `kAnims[]` order no longer matters. To drop one from
the rotation without deleting it, comment out its row — the `.c` still compiles
into the image, so also remove it from `SOURCES` if you want the flash back.

### The transition

`sh1106_slide()` composes the outgoing frame and the incoming one as two images
end to end, seen through a window sliding along them. **One of four directions —
left, right, up, down — chosen per transition**, for the same reason the choice
of visual is random: a fixed direction is a tell.

**The duration is fixed and the pixel rate is not.** A vertical slide crosses
64 px where a horizontal one crosses 128, and both take `TRANS_MS`, so the
screen is wiped in the same half second whichever way it went — which is what
reads as one effect rather than two. `sh1106_slide_travel(dir)` is what keeps
that true without the UI knowing which axis it picked; ask it rather than
assuming `SH1106_WIDTH`.

**The two axes cost very different things, and the layout is why.** `x` is the
byte index within a page, so a horizontal slide is three `memcpy`s per page and
moves no bits at all. Vertically, every byte is eight stacked pixels, so the
same idea has to cross page boundaries — per pixel that would be 8192 bit tests
a frame. `slide_vertical()` does it a column at a time instead: a column's 64
rows are eight bytes strided by `SH1106_WIDTH`, and packing them into a
`uint64_t` with bit *y* as row *y* turns the whole slide into one `>>` or `<<`.
A `_Static_assert` holds `SH1106_PAGES * 8 == 64`, because that packing is only
valid while a column fits a `uint64_t`.

> **`test_slide.py` in this directory checks all four directions** against a
> per-pixel reference, for every shift in the travel — 1,140 combinations. Run
> it after touching `sh1106_slide()`: a sign error in a vertical shift draws
> something perfectly plausible, which is exactly the failure mode the spectrum
> analyser's own host test exists for.
>
> ```bat
> python oled_display_module\test_slide.py
> ```

The sequencing is the part worth understanding, because it is what lets the
effect work between *any* two visuals without either one knowing about it:

1. At the switch, `vis_go_to()` calls `sh1106_snapshot(trans_prev)`. This runs
   **before `sh1106_clear()`**, which is the one moment the framebuffer still
   holds the frame the panel is showing.
2. Every tick, the normal draw path renders the *incoming* visual into the
   framebuffer as if nothing unusual were happening.
3. After the draw switch, `sh1106_slide()` rewrites the framebuffer as the
   composite. `shift <= 0` gives the outgoing frame alone and
   `shift >= SH1106_WIDTH` leaves the incoming one untouched, so neither end of
   the travel needs a special case.

It costs **1 KB of SRAM** — `trans_prev`, the outgoing frame held whole. The
page-at-a-time 128-byte scratch inside `sh1106_slide()` is what avoids needing a
second one.

500 ms is twelve frames at the 25 fps redraw, about 10 px a step. Slower reads
as the panel being sluggish; faster is a jump cut. Every one of those frames
changes the whole screen, so each costs a full ~1 KB flush — about 280 ms of
I2C spread over the half second, all of it on core 1.

### The random source

A xorshift32 in `ui.c`, seeded lazily from `time_us_32()` and stirred from the
same timer on every input. It is not `pico_rand` and does not need to be: this
decides which cartoon plays next. Seeding lazily matters — the first rotation is
thirty seconds after the last knob movement, and a dev build's boot waits on a
USB host, so the seed is genuinely variable rather than the same every power-on.

## Why it is written this way

Four decisions that look like they could be simpler and cannot be.

**The frame comes from the clock, not a counter.** `ui_tick()` is reached from
encoder events as well as from the 40 ms redraw, so a `f = (f + 1) % FRAMES`
counter would play at whatever rate the knob happened to be turned. The frame
index is derived from elapsed time instead:

```c
frame = (elapsed_ms * 1000) / a->frame_us % a->count;
```

That also lets a 15 fps or 20 fps animation sit on a panel redrawing at 25 fps
with no resampling: some redraws land on the frame already showing, and those
cost nothing on the bus because the flush diffs against the shadow.

**`FRAME_US`, never `FRAME_MS`.** The generator rounds `FRAME_MS`, and at rates
that do not divide 1000 evenly it is out by up to half a millisecond — anim1's
66.667 ms becomes 67, which is 1.5 frames of drift across one 20 s turn. The
header says so when it applies.

**`sh1106_blit()` rather than reaching into `fb`.** The framebuffer is `static`
inside `sh1106.c` and should stay there. The blit is one `memcpy` and
deliberately does not touch the shadow, so a repeated frame still costs nothing
on the bus.

**All of it is core 1.** A full flush is ~23 ms — seventeen audio block
deadlines. Nothing here may ever run on core 0, and nothing here may call
`printf`; use `sh1106_printf()`, which formats into a stack buffer and never
touches stdout.

## What it costs, and how to find out

Every frame is 1024 bytes of flash. On the bus, a frame costs whatever actually
changed — and the flush sends a **contiguous column span per page**
(`sh1106.c`), so the cost is driven by the horizontal extent of change in each
8-row band, not the pixel count. Two lit pixels at x=0 and x=127 in the same
page cost that whole 128-byte page.

A video frame changes most of the screen, so expect close to the full 1024
bytes and ~23 ms per flush:

| Animation rate | I2C duty on core 1 |
|---|---|
| 15 fps | ~35 % |
| 20 fps | ~46 % |
| 25 fps | ~57 % |
| 43 fps | 100 % — the ceiling |

None of that touches audio. But **those are arguments, not measurements**, and
this panel has already produced two frame bugs that arguments missed — it read
16 fps for months while asking for 25, and re-sent its whole framebuffer on a
static page. Measure before believing.

Set `UI_SHOW_GFX` to 1 at the top of `ui.c`, then rebuild:

```bat
make_firmware.bat oled flash
```

`gfx` then replaces the `dsp`/`ram` row on `DSP STATUS` and reads
`<frames>f <worst>ms <bytes>b` over a one-second window: the rate achieved, the
longest single flush, and the mean payload per frame. On an animation page
expect the byte figure near 1024; on a still menu page it should be 0. Put the
`#define` back to 0 afterwards — the counter takes a row `DSP STATUS` needs.

> **`-DUI_SHOW_GFX=1` on the CMake command line does nothing**, despite what the
> top-level `README.md` and the comment in `ui.c` both say. `UI_SHOW_GFX` is a
> plain `#ifndef` in `ui.c` and is not wired to any CMake option, so the flag
> lands in the cache and never reaches the compiler — CMake reports it as an
> unused variable and the build is silently unchanged. Editing the `#define` is
> the only way that works today. Wiring it up properly belongs in this module's
> `module.cmake`, not in a core file.

## Making one react to the audio

Nothing here does — each animation is a fixed loop on a timer. If you want one
driven by the signal, the approach that works is to pick the frame from the
level's place in its **own running distribution** rather than from the level
directly: that is gain-invariant, and it gives every frame an equal share of
the time whatever the material. It costs 480 bytes and no transcendentals.

(Three simpler algorithms were tried first and all failed for the same reason —
any direct level-to-frame mapping spends most of its time in a handful of
frames, because music does not visit its whole dynamic range evenly.)

In this module that is a change to `draw_anim()` and nothing else: replace the
elapsed-time index with the level index, and leave the table, the rotation and
the wiring alone. A per-animation flag in `oled_anim_t` saying which animations
are level-driven would keep both kinds in the same rotation.

## Removing one

1. Delete its row from `kAnims[]` in `ui.c`.
2. Delete its `#include` and its two `_Static_assert`s.
3. Remove its `.c` from `SOURCES` in `module.cmake`.
4. Delete `oled_anim<N>.{c,h}`.

`ANIM_COUNT` follows the table, so nothing else needs adjusting. A build with no
animations at all fails the `_Static_assert(ANIM_COUNT >= 1)` rather than
leaving `PAGE_ANIM` reading off the end of an empty table — if you genuinely
want the spectrum alone, that assert is the thing to read, and the honest fix is
to drop `PAGE_ANIM` from the rotation branch in `ui_tick()`.
