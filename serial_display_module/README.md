<!-- Moved out of the root README.md: this module is compiled in only when its
     build option selects it. File paths below are relative to the repo root.
     See ../README.md for the core firmware. -->

# The serial display module

`-DNOVA_DISPLAY=serial`, and it needs `-DNOVA_DIST=ON` with it. The whole menu
on the USB serial port, in colour, driven from the keyboard: the effect list,
every parameter page, the chain editor, DSP STATUS with live meters, and the
Save row. Open the board's COM port in PuTTY and it is there.

```bat
cmake -B build-serial -DNOVA_DIST=ON -DNOVA_DISPLAY=serial
cmake --build build-serial
```

It exists for a box with no panel fitted. Until now that build was
`-DNOVA_DISPLAY=none`, which on a `dist` image leaves **no interface at all** —
the board runs whatever was last saved to flash for ever, and the only way to
change it is to reflash. This is the other answer.

## It is dist-only, and the build says so

There is one USB CDC device, and a display that draws on it cannot share it with
a console that prints on it. On a `dev` build core 0 writes a status line to
stdout once a second and reads single keys from stdin in the main loop; both
would land in the middle of this module's escape sequences, and its keystrokes
would be eaten by whichever core read them first.

`dist` is what frees the port. `NOVA_LOG` compiles to nothing, the banner and
the benchmark are gone, and the `getchar` in `../NovaDSP.c`'s main loop is inside
`#if NOVA_TELEMETRY` — so **in a dist build core 0 never touches stdio after
`stdio_init_all()`, and core 1 owns the port outright.**

That also disposes of the firmware's standing rule that core 1 must never call
`printf`. The rule was never about formatting; it was that stdio's mutex is
shared with core 0's status line, so blocking on it would put the audio deadline
at the mercy of the USB host. With nothing on core 0 contending for that mutex
there is nobody to block, and the worst case is a slow frame on the terminal.

Building it into a `dev` image is a **compile error** naming the fix, not a
warning. The result would be a garbled screen and a menu that dropped every
other keystroke, and both would look like bugs in the module.

## Setting up the terminal

Any terminal works; PuTTY is what this was written against, and the baud rate is
ignored by USB CDC.

```bat
putty -serial COM17 -sercfg 115200
```

Two settings matter, and current PuTTY has both right by default:

- **Window → Translation → Remote character set: UTF-8.** The frame is drawn
  with box-drawing characters. A terminal set to something else draws the frame
  out of mojibake and leaves everything else readable; build with
  `NOVA_SERIAL_ASCII=1` in the module's `DEFINES` to get `+`, `-` and `|`
  instead.
- **DTR asserted.** The module only writes when the CDC host is attached, which
  it decides from DTR. PuTTY asserts it; a `System.IO.Ports` script does not
  unless you set `DtrEnable` first, and the screen then stays blank — the same
  trap `../SESSION.md` records for capturing the console.

Resize the window to at least 80x25. The page is drawn at exactly that size and
does not adapt; a narrower window wraps every row.

## The keys

| Key | Does |
|---|---|
| **Up / Down** | move the cursor — the knob, turned. `k`/`j` and `-`/`+` and `,`/`.` do the same |
| **Enter** | press the knob. Space and **Right** do the same |
| **Esc** | leave the page, or leave an edit. **Left**, `q` and Backspace do the same |
| **PgUp / PgDn** | ten rows at a time |
| **`s`** | save settings, from any page |
| **`r`** | repaint, for a terminal something else has scribbled on. Ctrl-L does the same |

**`Esc` is the one thing the panel does not have**, and it is an addition rather
than a replacement: `< Back` is still the first row of every list and still
works. A lone `Esc` is held for 50 ms before it is delivered, because it is
indistinguishable from the start of an arrow sequence until the rest arrives or
does not; `q` is there for anyone who would rather not wait.

## What it does differently, and why

**No idle screen.** The panel switches to the spectrum analyser after ten
seconds; this does not, and it has no spectrum page at all. Thirty-two bars
redrawn at 25 fps is a poor use of a serial link, and an idle screen with
nothing on it would be worse than the page the user left open. So the page stays
where it was put. The transform still runs on core 0 — it is core work, not the
display's — and `DSP STATUS` is the page you leave up.

**The status bar is on every page.** The bottom line carries the active effect,
the measured sample rate, `ovr`, `dsp` and the SRAM figure, plus `UNSAVED` when
a power cycle would lose something. The panel could not afford that — six 9 px
rows is the whole screen — so `DSP STATUS` becomes the page you go to for the
meters rather than for the overrun count.

**The meters are 40 cells wide and their readings sit beside them**, where the
panel has to put the number on a separate row. The reading is still the peak
over 250 ms rather than the bar's own value, for the reason given under
*`DSP STATUS`*: the ballistics that make a good bar make a bad number.

Everything else is deliberately identical, including the parts that look like
quirks and are not — rotating has no side effect, pressing a row that is not the
active effect selects it and pressing the one already running opens its
parameters, `< Back` first and `Reset` last, and nothing reaches flash until
`Save` is pressed.

## How it draws

`term.c` is an 80x25 cell buffer with one byte of glyph and one byte of
attribute per cell, which is the IBM PC text mode this is imitating. It is the
same shape as `../oled_display_module/sh1106.c` and for the same reason: **draw into a buffer, then
flush only what changed.** A full repaint with colour changes is about 6 KB of
escape sequences, which at any usable frame rate would saturate the link and
flicker; the diff of a moving meter is a few hundred bytes.

The cost is **4 KB of `.bss` for the pair of buffers**, which is most of what
this module spends — 7.1 KB in total against the OLED module's, against a 1 KB
framebuffer there.

- **It polls at 20 ms and repaints at 10 fps**, or immediately when a key
  arrived. There is no edge to catch — the CDC stack has already buffered the
  keystrokes — so the poll rate only has to beat typing, and it also sets how
  long a lone `Esc` waits.
- **A full repaint every four seconds, and on reconnect.** A terminal that
  attaches part-way through has been told none of what the shadow buffer thinks
  it knows. The reconnect is spotted through DTR; the periodic repaint is the
  belt to that pair of braces, and costs about 6 KB every four seconds on a link
  that is otherwise idle.
- **A frame with no host attached is dropped, not queued**, and the next
  attached frame is a full repaint. Queueing would be worse than useless: the
  CDC stack discards it anyway, and the shadow would then claim the terminal had
  been told things it never saw.

