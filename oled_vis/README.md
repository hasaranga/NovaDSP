# animtool

Turns a piece of a video into an animation header for the 128×64 OLED — a C
array of 1-bit frames you `memcpy` straight into the SH1106 framebuffer.

You pick the region, the start and end frames, and how long the animation
should run. It plays back at 25 fps in the browser, showing the exact pixels
that end up in the header.

---

## Setup

Install ffmpeg and Go, once:

    winget install Gyan.FFmpeg
    winget install GoLang.Go

Close and reopen your terminal, then build:

    go build -o animtool.exe .

That makes one `.exe` you can run anywhere. Double-click it and a browser tab
opens with the tool.

---

## The short version

1. Paste your video path, click **Open video**
2. Scrub to the first frame you want, press <kbd>[</kbd>
3. Scrub to the last frame, press <kbd>]</kbd>
4. Drag the box over the part of the picture you want to keep
5. Type how many seconds the animation should run
6. Click **Extract frames** — it jumps to the preview and starts playing
7. Drag the threshold slider until it reads well
8. Remove any frames that spoil the loop with <kbd>Del</kbd>
9. Go to Export, click **Write header**

---

## Step 1 — Source and range

### The timeline

Picking a range always counts in **25 fps frames**. Frame 137 means 137/25 =
5.48 seconds in, on the scrubber and in the from/to fields alike. The source's
own frame rate never appears, so there is nothing to convert and nothing to get
wrong — and because this grid is fixed, range picking stays accurate to 40 ms
however slowly the animation itself runs.

Opening a video decodes the whole thing to small JPEGs at 25 fps first. That is
the one slow step — a few seconds for a short clip — and it is why scrubbing
afterwards is instant.

**It does not open on frame 0.** Clips routinely start black or fade in, and a
black frame tells you nothing about where to put the crop box or where the tone
controls should sit. So it scans the first five seconds, opens on the first
frame that has a picture on it, and starts the range there. It says which frame
that was.

<kbd>←</kbd> <kbd>→</kbd> step one frame, hold <kbd>Shift</kbd> to jump a
second. <kbd>[</kbd> sets the start, <kbd>]</kbd> sets the end. The green bar
under the scrubber shows the range you have marked.

### Typing a range

**from** and **to** in the sidebar take the range in seconds, for when you
already know which piece of the clip you want and would rather not hunt for it
with the scrubber. Typing in either one jumps the picture to that frame, so you
see what you asked for. The frame number is shown beside each.

The scrubber and these two fields are two views of the same range — moving
either updates the other, and an end earlier than the start collapses the range
rather than inverting it.

### The crop box

Whatever is inside the box is what ends up in the header. Drag the middle to
move it, drag a corner to resize.

**Lock ratio** keeps the box the same shape as the panel, so nothing gets
squashed. Turn it off if you want to crop freely and let **fit** deal with the
mismatch:

- **cover** fills the panel and trims whatever hangs over the sides
- **contain** fits the whole crop in and leaves the gaps black

**Crop tighter than feels natural.** The panel is 128×64 and every pixel is
either on or off. A whole person in shot becomes an unreadable smudge. Crop to
one shape that changes clearly — a face, a hand, a meter, a silhouette.

The small picture in the sidebar is the crop at real panel size. If you cannot
tell what is happening there, the header will not work either.

### Frame rate and duration

**fps** is how fast the animation plays once it is on the panel — separate from
the 25 fps grid the range is picked on, and free to change without re-opening
the video. 1 to 50; the panel cannot flush much past 43 anyway.

**duration** is how long it should run. Together they decide the frame count
and therefore the flash cost, both shown right underneath:

    25 fps × 2.0 s  →  50 frames  →  50 KB
    10 fps × 2.0 s  →  20 frames  →  20 KB

**Not everything needs 25 fps.** A slow pulse, a breathing meter, a two-pose
blink — these read exactly the same at 8 or 10 fps and cost a third of the
flash. Spend the frame rate on fast motion, where dropping it shows up as
judder, and save it everywhere else. Flash is the real limit here: at 128×64
every frame is 1 KB, so a four second loop at 25 fps is 100 KB. Halving the
rate is the cheapest saving available, and cropping to a smaller panel region
is the next one — a 64×32 animation is a quarter of the price.

**Match selected range** sets the duration so the animation plays at the same
speed as the source. A shorter duration than the range speeds the action up by
dropping frames; a longer one holds frames, which reads as slow motion rather
than smooth motion — the sidebar warns when that happens.

Changing either the rate or the duration needs a re-extract; changing the
threshold or the dither does not.

### Tone

**Brightness / contrast / gamma** are applied before the picture is reduced to
one bit, which is the only place they can do any good — they decide what counts
as "light" when the threshold runs.

**Contrast above 1 nearly always helps.** At one bit per pixel, fine detail is
gone no matter what; what survives is light against dark, and contrast is what
sharpens that boundary. The default of 1.4 is a reasonable starting point.

**Sharpen** restores some of the crispness a heavy downscale takes away. Leave
it on.

The thumbnail in the sidebar follows all three sliders live, running the same
tone curve ffmpeg will — bit-exact for contrast and gamma, within 3/255 for
brightness. So the picture you tune here is the picture that gets extracted,
and you are not guessing between runs. Sharpen is the one control it does not
show; its effect at this size is too small to predict from a scaled proxy.

---

## Step 2 — Preview

The animation plays at the rate you chose, at real size times the zoom. This is not an
approximation: each frame shown is decoded from the exact bytes that go into
the header, so if it looks right here it will look right on the panel.

<kbd>Space</kbd> plays and pauses, <kbd>←</kbd> <kbd>→</kbd> step frames.
Clicking a thumbnail in the strip jumps to it.

Nothing here restarts playback on its own. Move the threshold or remove a frame
while paused and it stays paused, on the frame you were looking at — which is
what makes stepping through and weeding out the bad ones possible.

### The threshold

This is the control that matters. Every pixel brighter than the threshold
lights up; everything else stays dark. Drag it and the animation re-renders
immediately — no re-extraction.

Under the slider is the share of pixels lit. Below about 4% the picture has
broken up into specks; above about 60% it is a bright slab. Either way the
threshold is in the wrong place, and it says so.

### Removing frames

Resampling picks frames by time, so it cannot know that one of them caught a
cut, a blink, a camera flash or a hand crossing the shot. At one bit per pixel
that lands as a hole in the loop.

**Remove frame** takes the frame at the playhead out. <kbd>Del</kbd> does the
same and leaves the playhead where it is, so it now sits on the frame that
followed — hold it down and a bad run disappears in a few presses. Each
thumbnail in the strip also grows a **×** on hover, for a frame you spot while
scanning rather than while playing.

The loop gets shorter as you go: ten frames at 10 fps is a second, nine is
0.9 s. The sidebar tracks the count and the new duration, and the export
follows.

Nothing is thrown away. Removing edits a list of which extracted frames are in
use; the greyscale stays in memory. **Restore all** puts everything back
instantly, with no re-extraction and no ffmpeg.

### Dither modes

**Hard threshold** — no dithering. Clean shapes, hard edges. Right for
graphics, logos, silhouettes, anything with flat areas.

**Ordered dither** — a fixed 8×8 pattern that fakes grey. The pattern does not
move between frames, so it reads as a screen door laid over the picture. This
is usually the right choice for video.

**Floyd–Steinberg** — error diffusion. Sharper detail than ordered dithering,
but the pattern is recomputed per frame, so on anything that moves it crawls
and fizzes. Good for a still, distracting for an animation. Note that the
threshold barely moves the coverage number in this mode: error diffusion
preserves the average brightness by construction, which is the whole point of
it. Judge this one by eye, not by the percentage.

**Invert** flips the whole thing, for a dark subject on a lit panel.

---

## Step 3 — Export

**Layout** decides how the pixels are packed into bytes:

- **page-packed** — `data[page * W + x]`, bit `(y & 7)`, bit 0 the topmost row
  of the page. This is exactly `sh1106.c`'s `fb[page][x]`, so drawing a frame
  is one `memcpy` and nothing else.
- **row-packed** — `data[y * STRIDE + (x >> 3)]`, bit `(7 - (x & 7))`. The XBM
  order, for a generic blitter or a different panel.

**Files** — a `.h` + `.c` pair (the data lives in the `.c`, the header declares
it) or one self-contained header holding a `static const` array. The pair is
the right default; header-only is convenient for a quick test but puts a copy
in flash for every translation unit that includes it.

**In flash** wraps the array in `__in_flash()`, which keeps 50 KB of animation
out of the Pico's 264 KB of SRAM even in a `copy_to_ram` build. Leave it on for
NovaDSP.

### What you get

    #define OLED_ANIM_W        128
    #define OLED_ANIM_H        64
    #define OLED_ANIM_PAGES    8
    #define OLED_ANIM_BYTES    1024   // bytes in one frame
    #define OLED_ANIM_FRAMES   50
    #define OLED_ANIM_FPS      25
    #define OLED_ANIM_FRAME_MS 40
    #define OLED_ANIM_FRAME_US 40000

`FRAME_MS` is rounded. At rates that do not divide 1000 evenly — 30, 15, 12 —
it is out by up to half a millisecond, which is a whole frame every few
seconds on a long loop. Time those off `FRAME_US` instead; the header says so
when it applies.

    extern const uint8_t oled_anim_frames[OLED_ANIM_FRAMES][OLED_ANIM_BYTES];

### Playing it

A frame every `FRAME_MS`, from the display loop on core 1:

```c
#include "oled_anim.h"

static uint32_t last;
static int      f;

if (to_ms_since_boot(get_absolute_time()) - last >= OLED_ANIM_FRAME_MS) {
    last += OLED_ANIM_FRAME_MS;
    f = (f + 1) % OLED_ANIM_FRAMES;
    memcpy(fb, oled_anim_frames[f], OLED_ANIM_BYTES);
    sh1106_flush();
}
```

Accumulating `last += FRAME_MS` rather than resetting it to now keeps the
animation on time when a frame is late.

**None of this may run on core 0.** A full flush is about 23 ms at 400 kHz,
which is seventeen audio block deadlines.

`sh1106_flush()` only sends the columns that changed, so a mostly-still
animation costs far less than 1 KB per frame on the bus. An animation where
everything moves costs the full ~23 ms every frame, which caps you at about
43 fps — 25 fps fits with room to spare, and anything slower is free.

---

## When the result is wrong

**It is a mess of speckle.** The threshold is near the middle of a low-contrast
picture. Raise contrast, and try hard threshold instead of a dither.

**It is nearly blank, or nearly solid.** Read the coverage number under the
slider and move the threshold the way it tells you.

**It shimmers even where nothing moves.** You are on Floyd–Steinberg. Switch to
ordered dither.

**You cannot tell what it is.** The crop is too wide. Go back and crop to one
shape, not a scene.

**It stutters.** The duration is longer than the range you selected, so frames
repeat. Either extend the range or shorten the duration.

**One frame jumps out of nowhere.** The source had a cut or a flash there.
Park on it and press <kbd>Del</kbd>.

**It is too big for flash.** Drop the frame rate first — half the rate is half
the size and most animations do not miss it. Then shorten it, then crop to a
smaller panel region. Cost is `W × H / 8 × fps × seconds` bytes.

---

## Command line

    animtool.exe                 # opens a browser
    animtool.exe -noopen         # just prints the URL
    animtool.exe -port 8080      # fixed port instead of a free one
