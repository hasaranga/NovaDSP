# NovaDSP — User Manual

A real-time audio processor. Audio comes in, an effect works on it, audio goes
out — with a small screen and one knob to choose the effect and adjust it.

This manual is about *using* the box. If you want to know how it is built or
why, `README.md` and `SESSION.md` cover that.

---

## Contents

- [Connecting it](#connecting-it)
- [The knob, and the two things it does](#the-knob-and-the-two-things-it-does)
- [Getting around the menu](#getting-around-the-menu)
- [Saving your settings](#saving-your-settings)
- [The effects](#the-effects)
  - [Passthru](#passthru) · [BBE Maximizer](#bbe-maximizer) · [Equalizer](#equalizer)
  - [Noise Gate](#noise-gate) · [Auto Gain](#auto-gain) · [Leveller](#leveller)
  - [FX Chain](#fx-chain)
- [The DSP STATUS page](#the-dsp-status-page)
- [The spectrum screen](#the-spectrum-screen)
- [Setting up the Noise Gate](#setting-up-the-noise-gate)
- [Setting up Auto Gain](#setting-up-auto-gain)
- [Using it without the screen](#using-it-without-the-screen)
- [Troubleshooting](#troubleshooting)

---

## Connecting it

| | |
|---|---|
| **Audio in** | Line level, stereo, into the ADC board |
| **Audio out** | Line level, stereo, from the DAC board |
| **Power** | USB |

The box runs at **48 kHz, 24-bit**, and adds about **2.7 ms** of delay. Auto
Gain adds a further 1.3 ms when it is switched on.

**Set your source as loud as it will go without distorting.** This matters more
than any setting in this manual. A PC volume slider throws away quality as well
as level, so leave it at 100 % and control loudness with `Auto Gain`'s `Target`
instead. Check the `in` meters on [DSP STATUS](#the-dsp-status-page) — you want
them peaking around **−6 to −3** on loud passages, and never touching 0.

---

## The knob, and the two things it does

There is one control. It **rotates** and it **presses**.

- **Rotating moves the cursor.** It never changes anything on its own — you can
  scroll past every effect in the list without switching to any of them.
- **Pressing does the thing** the cursor is on.

That split is deliberate: nothing changes until you press.

### Waking the screen

After **ten seconds** without input the screen switches to the
[spectrum display](#the-spectrum-screen). Any turn or press brings back the page
you were on.

**That first input is used up by waking.** Nudging the knob to see the screen
cannot accidentally change a setting.

---

## Getting around the menu

### The effect list

The first page. It has a title bar reading `Nova DSP` and lists everything you
can run:

```
Passthru
BBE Maximizer
Equalizer
Noise Gate
Auto Gain
Leveller
FX Chain
Save
DSP Status
About
```

That is the full set. A given box may list fewer: which effects are built in is
chosen when the firmware is compiled, so a build made for one job carries only
what that job needs.

**`Passthru` is at the top, and it is what the box comes up running.** A box
that has just been switched on — or that has just been reflashed — passes your
audio through unchanged, so what you hear first is your source and not an effect
somebody chose for you. Everything below it is one press away.

**A filled square marks the effect that is currently processing your audio.**
Only one effect runs at a time. The list is longer than the screen, so a
scrollbar appears on the right as you turn.

The last three rows are **not** effects — they are `Save`, `DSP Status` and
`About`. You can tell because they can never carry the square.

### Two presses, two different things

This is the one piece of navigation worth learning:

| Press on… | What happens |
|---|---|
| an effect that is **not** running | it starts running — nothing else |
| the effect that **is** running | its parameter page opens |

So reaching the settings of an effect you are not listening to takes **two
presses**. That is on purpose: the first press is a decision about what you are
hearing, the second is about adjusting it.

### The parameter page

The title bar shows the effect's name. Every parameter page looks the same:

```
< Back           <- always first
Param 1     value
Param 2     value
   ...
Reset            <- always last
```

- **`< Back`** returns to the effect list.
- **`Reset`** puts every parameter back to the values the effect powers on with.
  There is no "are you sure" — the row just says `done` for a moment. Nothing is
  written permanently until you press `Save`, so a `Reset` you did not mean is
  undone by simply not saving.

To change a value: turn to the parameter, **press** to start editing, **turn**
to change it, **press again** to stop.

**The cursor tells you which mode you are in.** When you are moving around, the
whole row is highlighted. When you are editing, only the value is highlighted.

---

## Saving your settings

**Nothing is stored permanently until you press `Save` on the effect list.**

Not when you leave a parameter page, not when the screen goes idle, not when you
switch effects. If you power the box off without saving, everything returns to
however it was last saved.

The `Save` row tells you where you stand:

| The row reads | Meaning |
|---|---|
| `Save   ok` | what you are hearing is what is stored |
| `Save   unsaved` | you have made changes a power cycle would lose |
| `Save   saved` | it just saved — this shows briefly after you press |

**One press saves everything** — every effect's settings and which effect is
active, not just the page you were last on. There is no separate save inside
each effect.

---

## The effects

They are listed in the order they appear in the menu.

### Passthru

**A plain volume trim, −12 to +12 dB, and nothing else.** At 0 dB it passes audio
through completely untouched. **It is the first row of the menu and what the box
comes up running**, so a freshly switched-on box is doing nothing to your audio
until you tell it to.

It also exists so you can hear what an effect is actually doing. Set `Passthru` to
match the effect's loudness, then switch between the two. Comparing an effect
against silence, or against a louder version of itself, tells you nothing —
louder almost always sounds better.

### BBE Maximizer

A **clarity and punch** effect. It works out how much the bass is masking the
treble and lifts the treble by that much — so busy, bass-heavy passages get more
brightening than sparse ones. That is what makes it different from simply
turning the treble up.

| Parameter | Range | What it does |
|---|---|---|
| `Process` | 0–10 | How much high-frequency lift. The main control |
| `Contour` | 0–10 | Bass boost |
| `Dynamic` | 0–10 | How strongly the lift follows the music |
| `LoFreq` | 60 Hz – 400 Hz | Where the bass band ends |
| `HiFreq` | 600 Hz – 4 kHz | Where the treble band starts |
| `Output` | −12…+12 dB | Overall level |
| `Bypass` | on / off | Passes audio through untouched |

Start with `Process` and leave the crossovers alone unless something sounds
wrong. There is a limiter built in that you cannot switch off, so pushing
`Process` to 10 will not clip the output.

### Equalizer

A **ten-band graphic EQ**, the familiar kind.

```
60   80   125   250   500   1k   2k   4k   8k   16k  Hz
```

From 125 Hz up these are the **standard octave centres** — each one double the
one before it. The bottom two sit at 60 and 80 Hz, which is where small
speakers actually have output, rather than at the textbook 31.5 and 63 Hz.

Every band adjusts **−12 to +12 dB**, and there is a **`Preamp`** over the same
range at the top of the list.

> **Use the `Preamp` when you boost.** Ten bands each pushed up by 12 dB add to
> far more than 12 dB, and the result distorts. Pull the `Preamp` down by
> roughly as much as your biggest boost and the problem goes away. This is why
> it sits first in the list.

Two things that are normal and are not faults:

- **Neighbouring bands overlap.** Each band is about an octave wide, so turning
  one up lifts a little of the two beside it. That is how a graphic EQ works —
  it is a tone curve, not ten separate filters — and it is why a row of bands
  moved together gives a smooth shelf rather than a row of bumps.
- **`60Hz` and `80Hz` move together more than the rest.** They are much closer
  to each other than the other bands are, so they overlap a lot: raising both
  by 12 gives about 19, not 12, and the bass ends up the loudest part of the
  curve. Treat them as one bass control with a tilt, and reach for the
  `Preamp` when you use them. There is nothing to be adjusted below 60 Hz.
- **A band at 0 does nothing at all** — it is skipped entirely, which is what
  keeps a mostly-flat EQ cheap to run.

### Noise Gate

**For a source that is clean while something is playing and hissy in the gaps.**
Below a level you set, it shuts the signal off; above it, the signal passes
completely untouched. Tape hiss, mains hum, a microphone picking up the room —
all of it disappears between the notes and none of it is audible under them
anyway.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| `Thresh` | −80…0 dB | −40 | The level below which the gate shuts |
| `Attack` | 0.1…50 ms | 2 | How fast it opens once the sound comes back |
| `Hold` | off…500 ms | 50 | How long it stays open after the sound drops away |
| `Release` | 5…2000 ms | 150 | How fast it shuts once `Hold` has run out |

There is a setup guide below — see
[Setting up the Noise Gate](#setting-up-the-noise-gate). It is short, because
**only one of these four controls really matters** and the other three are how
it sounds doing its job.

`Attack`, `Hold` and `Release` step through a fixed set of useful values rather
than counting up one millisecond at a time, so a couple of clicks covers the
whole range. `Thresh` is 1 dB a click, because it is the one you need to place
precisely.

> **`Thresh` is an absolute level, so it moves with your input.** There is no
> value worth copying from anyone else, including from this manual. Set it
> against your own source.

**The Noise Gate belongs first** if you put it in an `FX Chain` — the opposite
end from `Auto Gain`. It decides whether to let the signal through at all, so
it wants to see the signal as it arrived, before anything has lifted the noise
it is there to remove.

> **This effect is newer than the rest and has had less use.** It has been
> tested thoroughly on a computer but not listened to for long on the box.
> Keep an eye on `ovr` on the DSP STATUS page the first time you run it.

### Auto Gain

**The one that stops you reaching for the volume control between tracks.** It
measures how loud the music actually is and moves the level until every track
arrives at the same loudness — with a hard limiter behind it so it can never
distort.

| Parameter | Range | Default | What it does |
|---|---|---|---|
| `Target` | −36…−6 dB | −30 | The loudness everything ends up at |
| `MaxGain` | 0…24 dB | 18 | The most it may adjust, **either way** |
| `Boost` | 0…12 s | 4 | How long a quiet stretch must last before it lifts it |
| `Output` | −36…0 dB | −1 | The ceiling. Nothing gets past it, ever |

There is a full setup guide below — see [Setting up Auto Gain](#setting-up-auto-gain).
It is worth reading, because two of these controls behave in ways people
reliably guess wrong.

**Auto Gain belongs last** if you put it in an `FX Chain`. It is the only thing
in the box that limits the output, and anything placed after it can push the
level straight back up.

### Leveller

Evens out the **quiet and loud parts of one performance**. Where Auto Gain stops
one track arriving louder than the last, this works *inside* a piece — the
whispered line and the shouted one end up closer together.

| Parameter | Range | What it does |
|---|---|---|
| `Degree` | 1–5 | How hard it works. **2 is the power-on value and where to start** |
| `Thresh` | −80 to −20 dB | The bottom of the lifted region |
| `Bypass` | on / off | Passes audio through untouched |

**`Degree` compounds.** It is how many times the same curve is applied, so 5 is
not "two and a half times 2" — it is the curve run five times. 4 and 5 are
audible as levelling rather than as a fixed balance.

**`Thresh` is not a gate threshold.** It is the line between "quiet part of the
music" that should be lifted and "the noise floor" that should not. Set it below
the quietest passage worth hearing. The default of −70 dB is deliberately low
and suits a clean source; raise it on a hissy one until the gaps stay quiet. If
the gaps *are* the problem, use the Noise Gate instead — that is what it is for.

> **`Bypass` does not give you the processing back.** The level follower keeps
> running while bypassed, which is what stops the effect swelling when you
> switch it back in. Use `Passthru` if you need the CPU.

`Degree` is the only control here that changes how much work the box is doing.

### FX Chain

Runs **up to four effects one after another**, in an order you choose.

Press it once to make it active, press again to open the editor. The editor
lists the effects a chain can hold; pressing one **adds** it to the end and shows
its position number, pressing one already in the chain **removes** it and closes
the gap so the numbers stay 1, 2, 3, 4 with no holes.

If all four slots are full, the row says `full` instead of doing nothing.

`Reset` empties the chain.

**Two effects are missing from the list on purpose:** `FX Chain` itself (a chain
containing itself would crash), and `Passthru` (it exists to be the thing you
compare *against*).

To adjust an effect that is inside a chain, select that effect on its own, tune
it, then select `FX Chain` again. Its settings carry over.

> **Nothing warns you if a chain is too much work for the processor.** Watch
> `ovr` on the DSP STATUS page — if it starts counting up, the chain is too
> heavy and you will hear dropouts.

---

## The DSP STATUS page

Reached from the last rows of the effect list. It stays up until you press —
it does not time out.

```
        DSP STATUS
Auto Gain            ovr 0
in   ########  ######
     -22.7      -25.8
out  ##########  ####
     -18.5      -21.7
dsp 4%      ram 280/512k
```

| | |
|---|---|
| top line | which effect is running, and `ovr` |
| `ovr` | **dropout counter. It must stay at 0.** Anything else means the box is running out of processing time |
| `in` | the level arriving, left and right |
| `out` | the level leaving, left and right |
| `dsp` | how much of the available processing time is being used |
| `ram` | memory used — a fixed number, only interesting to developers |

**The bars and the numbers under them are different readings and they will not
match.** The bar moves fast so you can see the music; the number is the peak over
the last quarter second so it sits still long enough to read. Both are correct.

**Read `in` and `out` together, not separately.** They sit side by side so you
can compare them, and the difference between them is what any levelling effect is
actually doing.

---

## The spectrum screen

The idle display: **32 bars showing the frequency content** of whatever is
coming out of the box, with a peak marker above each that holds briefly and then
falls.

It shows the output of the effect that is running, so you can see what the effect
is doing to the sound.

Two things that look wrong and are not:

- **The bars are tilted.** The display is deliberately tilted so ordinary music
  reads roughly level across the screen. Without it, all music would show as a
  slope down to the right — accurate, and useless to look at.
- **The lowest bars move together.** Below about 1.8 kHz the display has reached
  the limit of its resolution, so neighbouring bars carry similar information. A
  single bass note will light up three bars.

Content above 16 kHz is not shown.

---

## Setting up the Noise Gate

**`Thresh` is the whole job.** It has to sit above your noise and below your
quietest music, and there is a way to find that gap rather than guess at it.
You need the DSP STATUS page and about two minutes.

1. **Pause the music and read `in` on the DSP STATUS page.** That is your noise
   floor — the thing you are trying to shut out. Note the number.
2. **Play the quietest music you care about and read `in` again.** That is what
   has to get through. Note that number too.
3. **Set `Thresh` between the two, nearer the noise than the music.** If the
   floor reads −60 and the quiet music reads −35, somewhere around −50 is a
   sensible first try.
4. **Listen to the gaps, then listen to the quiet parts.** The gaps should go
   silent. The quiet parts should be untouched.
5. **Press `Save` on the effect list.** Nothing is kept until you do.

**If it seems to do nothing, `Thresh` is below your noise floor** — raise it.
**If it swallows the music, `Thresh` is above it** — lower it. There is no third
possibility, and no other control can fix either one.

### The other three

Only reach for these once `Thresh` is right.

- **`Hold` first.** If the gate closes in the middle of a phrase, raise it. If
  it hangs open too long after the music stops, lower it.
- **`Release` next.** If the end of a note is heard as a cut rather than as a
  fade, raise it.
- **`Attack` last, and usually not at all.** 2 ms is fast enough that you will
  not hear the start of a note clipped, and slow enough that a single cycle of a
  bass note cannot make the gate chatter.

> **The failure to listen for is an absence, not a noise.** Every other effect
> here announces itself. This one's whole job is to do nothing most of the time,
> so a `Thresh` set slightly too high does not sound broken — it sounds like a
> quiet note that never arrived. If something seems to be missing rather than
> wrong, lower `Thresh` a few dB.

---

## Setting up Auto Gain

Two of these controls are reliably misread. This section is worth following in
order.

### `Target` — the loudness everything ends up at

**It is a destination, not an amount of correction.** `Target −36` does not mean
"turn it down by 36". It means "make everything as quiet as −36" — which, on
music that is already quiet, does nothing at all.

Bigger number on the panel = louder. `−6` is loud, `−36` is quiet.

> **If Auto Gain seems to be doing nothing, look here first.**

**`Target` is invisible on the meters, and that is normal.** It sets an *average*
level while the DSP STATUS page shows *peaks*, and music peaks around 8–15 dB
above its average depending on how it was produced. So `Target −18` shows as
somewhere around **−4 to −10** on the meter rather than −18. Nothing is wrong,
and there is no setting that makes the two numbers agree.

The way to set it is backwards from what you would expect:

1. Play typical loud music.
2. Look at `out` on DSP STATUS.
3. Move `Target` up or down by however many dB you want `out` to move — **it
   tracks one-for-one**.
4. Stop when `out` reads where you want it.

You never need to know what `Target` "really" is. Aim for `out` peaking around
**−6 to −8**, which leaves comfortable room under the ceiling.

Adjust in steps of 2 dB — you have to leave the parameter page to see DSP STATUS,
so fewer trips is better.

### `MaxGain` — how far it may reach

`MaxGain 18` lets it lift a quiet track by up to 18 dB, and pull a loud one down
by up to 18 dB.

**The thing to understand is what happens when it runs out.** Once it hits the
limit, levelling stops completely — every track gets the same maximum adjustment,
so loud and quiet tracks come out exactly as far apart as they went in. It looks
like it is working, because everything did get louder or quieter, and it is doing
nothing you wanted.

**How to spot it:** compare `in` and `out`. If the gap between them is stuck at
exactly 18 and never changes, you have hit the limit.

The usual fix is a better input level, not more `MaxGain` — winding it up just
amplifies hiss along with the music.

### `Boost` — how patient it is

A quiet passage and a quiet track look identical; the only difference is how long
they last. This is where you draw the line.

- **Lower it** if quiet tracks take too long to come up.
- **Raise it** if quiet bits *inside* a track get lifted and then squashed when
  the music comes back.

4 seconds suits most music. `0` turns the wait off entirely and will pump on
every quiet moment — it is there so the bottom of the range is a real value, not
because it is a good setting.

**A track that arrives too loud is always fixed immediately**, whatever `Boost`
says. Turning things down is never delayed.

### `Output` — the ceiling

The absolute maximum. No sound gets past it, ever. Its job is to catch the
moments when the leveller is briefly caught too high — a track kicking in after a
quiet intro — and turn what would have been distortion into a fraction of a
second of limiting.

**−1 suits nearly everything.** Use −3 if the output feeds something that
re-compresses the audio.

> **`Output` is not a volume control.** It can only ever turn things *down* — it
> can do nothing for a quiet track. And setting it close to `Target` makes it
> limit constantly instead of catching the occasional peak, which flattens the
> music. **If the output is too loud, change `Target`.**

### In order

1. **Get the input right first.** Source at 100 %, `in` peaking −6 to −3.
   Nothing below will work properly if this is wrong.
2. **Set `Target`** for how loud you want the result, using the method above.
3. **Set `MaxGain`** to comfortably more than your music varies by.
4. **Leave `Boost` at 4** unless you have a reason.
5. **Leave `Output` at −1.**
6. **Press `Save`.**

### Checking it works

`in` and `out` sit side by side on DSP STATUS. **Change tracks. `in` should move
and `out` should not.**

That is the entire behaviour this effect exists to produce, and you can watch it
happen.

---

## Using it without the screen

Some boxes are built without a panel and a knob. If yours is one of them, **the
same menu is on the USB socket** — plug it into a computer, open the serial port
it appears as, and you get the whole thing on screen with the keyboard standing
in for the knob.

You will know which kind you have as soon as you open the port: either a blue
menu appears, or it does not.

### Opening the port

The board shows up as a `USB Serial Device (COMxx)` on Windows, or
`/dev/ttyACM0` or similar elsewhere. Open it with any terminal program — PuTTY
is the usual one. **The speed setting does not matter**; pick anything.

Two things to check if the screen looks wrong rather than absent:

- **Make the window at least 80 columns by 25 rows.** The page is drawn at
  exactly that size, and a narrower window wraps every line into a mess.
- **If the box borders come out as strings of odd characters**, set the terminal
  to UTF-8. In PuTTY that is Window → Translation → Remote character set. Recent
  versions already are.

Closing the terminal and opening it again is harmless — the screen redraws
itself.

### The keys

| Key | What it does |
|---|---|
| **Up** / **Down** | Move the cursor. This is the knob being turned |
| **Enter** | Select, or open, or start editing. This is the knob being pressed |
| **Esc** | Go back a page, or finish editing |
| **Page Up** / **Page Down** | Move ten rows at once |
| **S** | Save your settings, from anywhere |
| **R** | Redraw the screen, if something has scribbled on it |

`Left` and `Right` do the same as `Esc` and `Enter` if you prefer them, and `q`
does the same as `Esc`.

**Everything works exactly as it does on the panel**, including the two-press
rule: pressing an effect that is not running selects it, and pressing the one
that is already running opens its settings.

### The two differences

**There is no spectrum screen**, and the display never times out — the page you
leave open stays open.

**The bottom line always shows the numbers.** Which effect is running, the
sample rate, the overrun count, the DSP load, the memory used, and — on the
right — whether you have unsaved changes. On the panel you have to open
`DSP STATUS` to see any of that.

---

## Troubleshooting

**The screen is blank.**
It has gone idle and is showing the spectrum. Turn the knob. If it is genuinely
dark, check the display's connections.

**Turning the knob fast skips steps.**
Known behaviour of this type of encoder. It is reliable up to about six turns a
second, which is fine for deliberate adjustment.

**An effect seems to do nothing.**
Check the filled square is next to it in the list — selecting an effect and
opening its parameters are two different presses. Also check `BYPASS` or
`Enable` if that effect has one.

**Auto Gain seems to do nothing.**
Almost always `Target`. See above — it is a destination level, and a low setting
on already-quiet music correctly does nothing.

**Everything is quiet and hissy.**
Input level. Check `in` on DSP STATUS. If it reads −30 when it should read −5,
the problem is upstream and no setting here can fix it.

**The sound distorts or breaks up.**
Check `ovr` on DSP STATUS. If it is counting up, you are asking for more
processing than there is time for — usually too many effects in a chain. If
`ovr` stays 0, check `in` is not hitting 0.

**My settings vanished after switching off.**
They were not saved. Press `Save` on the effect list — the row says `unsaved`
whenever there is something to lose.

**There is a click when I change effects.**
Normal. Nothing fades between effects.

**The bass sounds absent and muddy.**
If this is a newly built or repaired unit, it is a hardware issue with the input
board rather than a setting. `README.md` covers it under *Module rework*.
