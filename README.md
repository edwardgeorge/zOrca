# zOrca

zOrca is a terminal [Orca](https://wiki.xxiivv.com/site/orca.html) for hardware MIDI. Orca is a livecoding language for procedural sequencers: every letter of the alphabet is an operator, lowercase operators run when banged, and uppercase ones run on every frame. zOrca adds scales and chords, chord and arpeggiator operators, an LFO, CC glides and following an external MIDI clock, and it sends MIDI notes, CC with glides, MIDI clock and OSC status messages. It runs on Linux and macOS, and as an appliance on a ClockworkPi uConsole. The program is `orca`. The code is at [github.com/ianhundere/zOrca](https://github.com/ianhundere/zOrca), each release is a [tag](https://github.com/ianhundere/zOrca/tags), and the release notes are in [CHANGELOG.md](CHANGELOG.md).

The bOrca dialect's operators are the default; patches written for Orca-c play in the upstream [dialect](#dialects). The patches in `examples/upstream/` are upstream Orca-c examples, which play in that dialect; see [`examples/upstream/README.md`](examples/upstream/README.md).

## Quick start

On Debian or Ubuntu, install a C compiler and ncurses, plus the library of one MIDI backend: ALSA, on Linux only, or PortMidi.

```sh
sudo apt install build-essential libncurses-dev libportmidi-dev   # for PortMidi
sudo apt install build-essential libncurses-dev libasound2-dev    # or for ALSA, Linux only
```

On macOS, install PortMidi and ncurses with Homebrew, `brew install portmidi ncurses`, and build with `./tool build --portmidi orca`; `--alsa` is Linux only.

Then clone the repository, build `orca` with the backend you installed, and run an example:

```sh
git clone https://github.com/ianhundere/zOrca.git
cd zOrca
./tool build --portmidi orca   # or: ./tool build --alsa orca
build/orca examples/misc/chord.orca
```

`orca` starts playing at once on its internal clock; Space pauses and resumes. With the clock source set to External it waits for the master to start instead (see [Clock & Timing](#clock--timing)).

To hear it, connect zOrca's MIDI output to a synth:

- In an ALSA build, `aconnect -l` lists the sequencer's clients and ports, and `aconnect zOrca:0 <synth client>:<port>` connects zOrca's `MIDI out` port to one.
- In a PortMidi build, open the main menu with Ctrl+D, choose MIDI Output... and pick the device.

## On the uConsole

On a ClockworkPi uConsole, zOrca runs as an appliance:

- `make appliance` builds the engine, an ALSA-only `orca`, and a systemd service runs it.
- A separate provisioning setup installs it, and its `verify` check reports on it.
- The engine's settings are in `~/.config/borca/orca.conf`. A hand edit there may be overwritten when the device is provisioned again.
- The appliance's MIDI router connects `zOrca:0` by itself.
- A development build runs beside it as `ZORCA_ALSA_CLIENT_NAME=zOrca-dev nice -n 19 build/orca`, optionally under `taskset -c 0-2`. Without its own client name an `--alsa` build exits at startup, because `zOrca` is taken. It reads its own `orca.conf` (`~/.config/orca.conf`), not the engine's. Its MIDI out is not connected to the synths while the appliance plays, so only one sequencer drives them.

## The editor

| Key | What it does |
| --- | --- |
| Ctrl+Q | Quit |
| Arrow keys, or Ctrl+H, Ctrl+J, Ctrl+K and Ctrl+L | Move the cursor; Ctrl+H, Ctrl+J, Ctrl+K and Ctrl+L move it left, down, up and right |
| Ctrl+Arrow keys | Move the cursor by the rulers' spacing |
| Ctrl+D or F1 | Open the main menu |
| Ctrl+O | Open a file |
| Ctrl+S | Save |
| `0`-`9`, `A`-`Z`, `a`-`z`, `!` `:` `%` `=` `#` `*` `;` `$` `&` `.` | Insert that glyph; `/` inserts a `?` |
| Backspace or Delete | Write a `.` in the cell under the cursor, or over the selection; in append mode Backspace writes it one cell to the left and moves there, and Delete moves right after it |
| Space | Play or pause; under External it does nothing, and in append mode it inserts a `.` |
| Ctrl+Z or Ctrl+U | Undo |
| Ctrl+X | Cut |
| Ctrl+C | Copy |
| Ctrl+V | Paste |
| Ctrl+F | Step one frame forward |
| Ctrl+R | Reset the frame number to 0, and clear the per-cell state of every operator that keeps one (`&`, `;` and `r`) |
| Ctrl+I or Insert | Switch between append and overwrite mode |
| `'` (quote) | Rectangle selection mode |
| Shift+Arrow keys | Grow or shrink the selection |
| Shift+Ctrl+Arrow keys | Grow or shrink the selection by the rulers' spacing |
| Alt+Arrow keys | Slide the selection |
| `` ` `` (grave) or `~` | Slide selection mode |
| Escape | Return to normal mode, or deselect |
| `(` `)` `_` `+` | Change the grid's width (`(` `)`) and height (`_` `+`) |
| `[` `]` `{` `}` | Change the rulers' spacing, across (`[` `]`) and down (`{` `}`) |
| `<` and `>` | Lower or raise the BPM; under External they do nothing |
| Ctrl+E | Show or hide the list of the events the last tick sent |
| `?` | Show these controls, which the main menu's Controls... also shows |
| Ctrl+G | List the active dialect's operators, which the main menu's Operators... also lists |

In a build with the mouse, the default (`--no-mouse` leaves it out), a click puts the cursor on a cell, a drag selects a rectangle, and the wheel scrolls the grid.

The HUD's two rows at the foot of the screen show, from the left:

- the grid's width and height, such as `57x25`;
- the rulers' spacing, across and down, such as `8/8`;
- the frame number, such as `120f`;
- the tempo in BPM, or under External the clock's state: `ext --`, `ext 120` or `ext lost` (see [Following an external clock](#following-an-external-clock));
- the activity indicator, which changes as events go out;
- `MIDI err`, in an ALSA build only: bold, and there until you quit, after the sequencer refused a MIDI message. A refused note-off is not lost: it is kept and sent again, for up to 3 seconds, so the note does not hang on the synth. To clear it, check the subscriber or the Bluetooth link to the synth, then restart `orca`;
- on the second row, the cursor's position, the selection's size, the input mode (`insert`, `append`, `select` or `slide`) and the file name.

With the cursor on an operator's input, the HUD names it: "Channel", "Octave", "Velocity" and so on, right of the fields on its two rows, cut short when the window is narrow. The name comes from the operator itself, so it is right however close operators sit, it follows the inputs that move (`G`'s and `Q`'s, `K`'s variables, `T`'s selected input, `O`'s offset cell), and it follows the [dialect](#dialects): upstream `!` names Channel, Control and Value, upstream `=` names Path, Count and Value, and upstream `;`, `$` and `&` name nothing. Where a cell is both one operator's output and another's input, the input's name shows. An output shows nothing. An operator that needs a bang (lowercase, and `;`) names its inputs only when it is banged; while playing, the names are those of the last tick. Some inputs show their value on the second row: `=`'s and `$`'s selectors name the chord or scale, `!`'s value shows the MIDI value it sends (× 4 in bOrca, × 127 / 35 upstream), and `!`'s rate shows "rate N", or "instant" for `0`, `z` and `Z`; a `.` rate shows the plain label.

## bOrca's operators

| Glyph | Name | Inputs | Purpose | Demo patch |
| --- | --- | --- | --- | --- |
| `$` | [Scale](#scale-) | Octave, Root, Scale, Degree | Outputs the note and octave of a degree of a scale or chord | `examples/misc/chord_inversions.orca` |
| `=` | [Midichord](#midichord-) | Channel, Octave, Root, Chord, Velocity, Length | Plays a chord when banged | `examples/misc/chord_inversions.orca` |
| `;` | [Arpeggiator](#arpeggiator-) | Range, Pattern, Length, Step | Counts out degrees for `$` when banged | `examples/misc/arpeggiator.orca` |
| `&` | [Bouncer](#bouncer-) | Start, End, Rate, Shape | An LFO: cycles between two values over a number of beats | `examples/misc/midiccbouncerdemo.orca` |
| `!` | [MIDI CC](#midi-cc--and-glides) | Channel, three Control digits, Value, Rate | Sends a CC when banged, at once or as a glide | `examples/misc/glide_rate.orca` |
| `r` | [Random](#random-r-and-r) | Min, Max | Deals values from a shuffled bag when banged, never one twice in a row | `examples/misc/random_unique.orca` |
| `R` | [Random](#random-r-and-r) | Min, Max | Outputs a random value on every frame | `examples/basics/z.orca` |
| `:` `%` `=` | [Velocity](#notes-and-velocity) | Velocity | One velocity mapping for the note, mono note and chord operators | `examples/misc/velocity.orca` |

### Notes and velocity

The MIDI note operator (`:`), the monophonic note operator (`%`) and the Midichord operator (`=`) read their velocity input the same way:

- `.` is 127.
- `0`, or any other glyph worth 0 (any glyph but `.` that is not `0`-`9`, `a`-`z` or `A`-`Z`, such as `*`, `#` or `;`), sends nothing.
- Any other glyph is worth v, `1`-`9` for 1 to 9 and `a`-`z` for 10 to 35, and plays at v × 8 − 1, at most 127: `1` is 7, `7` is 55, `f` is 119, and `g` to `z` are 127.

Uppercase reads like lowercase, so `F` is 119 too. `examples/misc/velocity.orca` plays all three operators through the same velocity glyphs.

A channel glyph `0` to `f`, on these operators and on `!`, is MIDI channel 1 to 16 as most synths number them, so channel 1 in the examples here is a synth's channel 2.

### Scale `$`

`$` outputs a note and an octave from a root note, a scale or chord, and a degree. Its selector is one of 62: 10 scales (`0`-`9`), 26 chords in root position (`a`-`z`) and the same 26 chords in first inversion (`A`-`Z`), listed in [Scales and chords](#scales-and-chords).

| Operator | Octave | RootNote | Scale/Chord | Degree |
|:------:|:--------:|:--------:|:-----:|:------:|
|   $    |    O     |    R     |   S   |   D    |

**Output:** The Scale operator outputs the octave below and to the left of the operator, and the note directly below it. With no octave input (`.`), it outputs only the note.

Examples, with a scale (`0`-`9`) and with a chord (`a`-`z` in root position, `A`-`Z` in first inversion):

- `$3C02` - C Major scale, 3rd degree → outputs octave '3' and note 'E'
- `$.C12` - C Minor scale, 3rd degree → outputs note 'd' (D#/Eb, no octave)
- `$3Ca2` - C Major chord, 3rd note → outputs '3G' (G3)
- `$3CA2` - C Major first inversion, 3rd note → outputs '4C' (C4)

<!-- The three tables in this section are generated: build/readme-gen prints them from music.c, and CI fails when they differ from its output (tests/check-doc-sync.sh). Change music.c and paste the new output; never edit them by hand. -->
### Scales and chords

<!-- tables:begin -->
#### Available Scales (0-9, `$` only)
| Value | Scale Type |
|:-----:|------------|
|   0   | Major |
|   1   | Minor |
|   2   | Dorian |
|   3   | Lydian |
|   4   | Mixolydian |
|   5   | Pentatonic |
|   6   | Hirajoshi |
|   7   | Iwato |
|   8   | Tetratonic |
|   9   | Fifths |

#### Available Chords (a-z = root, A-Z = first inversion)
| Value | Chord Type | Root Notes | First Inversion |
|:-----:|------------|:----------:|:---------------:|
|   a/A   | Major | C-E-G | E-G-C |
|   b/B   | Minor | C-Eb-G | Eb-G-C |
|   c/C   | Sus4 | C-F-G | F-G-C |
|   d/D   | Sus2 | C-D-G | D-G-C |
|   e/E   | Major7 | C-E-G-B | E-G-B-C |
|   f/F   | Minor7 | C-Eb-G-Bb | Eb-G-Bb-C |
|   g/G   | Dom7 | C-E-G-Bb | E-G-Bb-C |
|   h/H   | MinorMaj7 | C-Eb-G-B | Eb-G-B-C |
|   i/I   | Minor6 | C-Eb-G-A | Eb-G-A-C |
|   j/J   | Major6 | C-E-G-A | E-G-A-C |
|   k/K   | Major9 | C-E-G-B-D | E-G-B-C-D |
|   l/L   | Minor9 | C-Eb-G-Bb-D | Eb-G-Bb-C-D |
|   m/M   | Major add9 | C-E-G-D | E-G-C-D |
|   n/N   | Minor add9 | C-Eb-G-D | Eb-G-C-D |
|   o/O   | Dim | C-Eb-Gb | Eb-Gb-C |
|   p/P   | Half Dim7 | C-Eb-Gb-Bb | Eb-Gb-Bb-C |
|   q/Q   | Dim7 | C-Eb-Gb-A | Eb-Gb-A-C |
|   r/R   | Aug | C-E-G# | E-G#-C |
|   s/S   | Aug7 | C-E-G#-Bb | E-G#-Bb-C |
|   t/T   | Dom9 | C-E-G-Bb-D | E-G-Bb-C-D |
|   u/U   | Dom7b9 | C-E-G-Bb-Db | E-G-Bb-C-Db |
|   v/V   | Dom7#9 | C-E-G-Bb-D# | E-G-Bb-C-D# |
|   w/W   | Major 6/9 | C-E-G-A-D | E-G-A-C-D |
|   x/X   | Minor 6/9 | C-Eb-G-A-D | Eb-G-A-C-D |
|   y/Y   | Minor11 | C-Eb-G-Bb-F | Eb-G-Bb-C-F |
|   z/Z   | Minor7b5 | C-Eb-Gb-Bb | Eb-Gb-Bb-C |

#### Enriched Chords (0-9, `=` only)
| Index | Name | Intervals | Notes (C root) |
|:-----:|:----:|:---------:|:--------------:|
| 0 | Major+Oct | 0,4,7,12 | C-E-G-C |
| 1 | Minor+Oct | 0,3,7,12 | C-Eb-G-C |
| 2 | Sus4+Oct | 0,5,7,12 | C-F-G-C |
| 3 | Sus2+Oct | 0,2,7,12 | C-D-G-C |
| 4 | Major7+Oct3rd | 0,4,7,11,16 | C-E-G-B-E |
| 5 | Minor7+Oct3rd | 0,3,7,10,15 | C-Eb-G-Bb-Eb |
| 6 | Dom7+Oct5th | 0,4,7,10,19 | C-E-G-Bb-G |
| 7 | Major6+Oct | 0,4,7,9,12 | C-E-G-A-C |
| 8 | Minor6+Oct | 0,3,7,9,12 | C-Eb-G-A-C |
| 9 | Dim+Oct | 0,3,6,12 | C-Eb-Gb-C |
<!-- tables:end -->

### Midichord `=`

`=` plays a chord as MIDI notes. Its chord selector is an enriched chord (`0`-`9`), a chord in root position (`a`-`z`) or a first inversion (`A`-`Z`), the same chords `$` plays, listed in [Scales and chords](#scales-and-chords). In the bOrca dialect `=` is Midichord; the upstream [dialect](#dialects) keeps OSC.

| Operator | Channel | Octave | Root Note | Chord Type | Velocity | Length |
|:--------:|:-------:|:------:|:---------:|:----------:|:--------:|:--------:|
|    =     |    C    |   O    |     R     |     T      |    V     |    L     |

Like `:`, it runs only when banged. Here a `D4` bangs it every 4 frames, and it plays C, E and G (MIDI notes 36, 40 and 43) on channel 1 at velocity 119 with a length of 15 frames; each bang retriggers the chord before that length runs out:

```
D4......
.=13Caff
```

The velocity reads as in [Notes and velocity](#notes-and-velocity). Examples:

- `=13C0ff` - Plays enriched C major chord (C-E-G-C) on channel 1, octave 3
- `=13C1ff` - Plays enriched C minor chord (C-Eb-G-C) on channel 1, octave 3
- `=13Caff` - Plays C major chord (C-E-G) on channel 1, octave 3
- `=13CAff` - Plays C major first inversion (E-G-C) on channel 1, octave 3
- `=13Cbff` - Plays C minor chord (C-Eb-G) on channel 1, octave 3

### Arpeggiator `;`

`;` is a degree-based arpeggiator: it outputs degree numbers (0, 1, 2, 3 …), not MIDI. It feeds them to the Scale operator (`$`), which feeds notes to the MIDI operator (`:`), and so it leaves the notes, timing, velocity and length to the operators it feeds:

- `;` (arpeggiator) → `$` (scale) → `:` (midi)

It runs only when banged: like a lowercase operator, `;` outputs only on a tick when a bang (`*`) sits next to it, whether its Step input is set or not. It outputs nothing when its Range or Pattern is `.`.

| Operator | Range | Pattern | Length | Step |
|:--------:|:-----:|:-------:|:------:|:----:|
|    ;     |   R   |    P    |   L    |  S   |

- `R`: Range (1-4) - Sets the octave range for arpeggiation; `0` counts as 1, and anything above 4 as 4
- `P`: Pattern (0-9, a-d) - Selects the arpeggiation pattern, from [Arpeggio patterns](#arpeggio-patterns); a higher glyph wraps round, so `e` is Up
- `L`: Length - The number of notes in one octave of the scale or chord, `.` for 7. Any other glyph is its value, `1`-`9` and `a`-`z` for 10 to 35, and uppercase reads like lowercase; `0`, or any other glyph worth 0 (such as `*` or `#`), counts as 1, as Range `0` does
- `S`: Step - `.` steps through the pattern by itself: each bang outputs the next step. Any other glyph is the step to output: `0`-`9`, then `a`-`z` for 10 to 35, uppercase reading like lowercase, and any other glyph worth 0, so a `*` is step 0. A `C`, a `T` or any other operator can write it, which drives the arpeggio from the grid

Like any uppercase letter, an uppercase Length or Step runs as an operator on the ticks when `;` is not banged, so write them as digits or lowercase.

Set Length to the number of notes of the `$` selector the arpeggiator feeds: 7 for scales `0`-`4`, 5 for `5`-`7`, 4 for `8` and 2 for `9`, and for a chord the number of notes in its Root Notes column (3, 4 or 5). `$` carries a degree past its last note into the next octave, so only then does each octave of the range start on the root. A chord with a note above the octave (`k`, `l`, `m`, `n` and `t` to `y`, and the inversion `Y`) still dips back at each octave boundary: `$3Cm` fed by `;204` plays 36 40 43 50 48 52 55 62. With Length `.` a triad over range 1 spans 7 degrees, two whole octaves.

The arpeggio spans Length × Range degrees, at most 36, so the output is always one glyph, `0` to `z`: `;40z` asks for 35 × 4 = 140 degrees, which caps at 36, so it counts from `0` to `z` and wraps.

A set Step is taken modulo the pattern's period, the Period column of [Arpeggio patterns](#arpeggio-patterns): Up over 3 degrees at Step `4` outputs degree 1. Step is one glyph, so in a pattern whose period is longer than 36 (Up-Down+ over 36 degrees repeats every 72 steps) it reaches only the first 36 positions. Random has no period: each Step gives the random degree for that step, which follows the seed (`--seed`). While Step is set the arpeggiator keeps no state of its own: its output depends only on its inputs and the seed, so Ctrl+R or reopening the file cannot shift it. When Step goes back to `.`, its own count carries on from where it stopped. The count starts over when a bang finds Range, Pattern or Length different from the last time it counted (a change undone before then goes unnoticed; Range above 4 counts as 4), and after Ctrl+R, a new file or an open.

`;` writes its degree directly below itself. An example, the first voice of `examples/misc/arpeggiator.orca`:

```
.......D2............
........;103.........
D2..$3Ca.............
.:0..a4..............
```

1. Delay (`D`) with value 2 bangs the arpeggiator every 2 ticks
2. Arpeggiator (`;`) uses range 1, pattern 0 (Up) and length 3, the three notes of a triad, so it outputs the degrees 0 1 2 0 1 2 …
3. Scale operator (`$`) converts degrees to notes using octave 3, root C and chord `a` (C major), degree received from Arpeggiator
4. MIDI operator (`:`), banged from the west by a second `D2` every 2 ticks, plays C, E and G (MIDI notes 36, 40 and 43) over and over on channel 0, with velocity a and duration 4

The example's second voice, `;224` with its Step written by `1Ce`, plays Up-Down over two octaves of Major7 (`$4Ce`, length 4): 8 degrees, a period of 14 steps, which the `C` counts out one per tick.

#### Arpeggio patterns

In the Period column, t is the total number of degrees, min(Length × Range, 36), and the period is the number of steps after which the pattern repeats. At a total of 1 every pattern outputs `0`.

| Pattern | Description | Pattern Style | Period |
|:-------:|-------------|---------------|:------:|
| 0 | Up | Ascending notes in scale | t |
| 1 | Down | Descending notes in scale | t |
| 2 | Up-Down | Ascending then descending (no repeat at turn points) | 2t−2 |
| 3 | Down-Up | Descending then ascending (no repeat at turn points) | 2t−2 |
| 4 | Up-Down+ | Ascending then descending (with repeated turn points) | 2t |
| 5 | Down-Up+ | Descending then ascending (with repeated turn points) | 2t |
| 6 | Converge | Outside in (highest, lowest, 2nd highest, 2nd lowest...) | t |
| 7 | Diverge | Inside out (middle notes outward) | t |
| 8 | Pinky Up | Alternate between notes and highest note | 2t−2 |
| 9 | Thumb Up | Alternate between lowest and other notes | 2t−2 |
| a | Up-Down Alt | Same as 4 | 2t |
| b | Down-Up Alt | Same as 5 | 2t |
| c | Random | Random selection from scale degrees | none |
| d | Bounce | Bouncing pattern | 2·max(1, ⌊t/3⌋) |

Bounce moves over only the lowest ⌊t/3⌋ degrees, and outputs only `0` below a total of 6: a triad, a four-note or a five-note chord at range 1 (`;1d3`, `;1d4`, `;1d5`) outputs `0` on every bang, so use range 2 or more with a chord.

### Bouncer `&`

`&` is an LFO: it cycles its output between two values along one of eight waveforms. Each waveform has 128 steps; some steps repeat, so that the output follows the curve more closely, as near the peaks of the sine.

The Rate parameter is the length of one cycle in beats, at 4 ticks to a beat: `1` cycles every 4 ticks, `4` every 16 ticks, which is one bar of 4/4, and `z` every 140 ticks, so a cycle always ends on a beat, and on a bar when the rate is a multiple of 4. `.` and `0` hold the phase-0 output: Start for shapes 0, 2, 4 and 6, End for shapes 1, 3, 5 and 7. The phase counts from tick 0, or from the last tick a neighbouring bang (`*`) hit the operator, so every `&` with the same rate is in step wherever it sits, and a bang restarts the cycle; changing the rate or the shape keeps the anchor, the tick the cycle is counted from, so it stays in step with other `&`s (a shape change keeps the phase; a rate change re-takes the elapsed ticks mod the new cycle). An external MIDI start (`FA`) or song position (`F2`) sets every `&`'s anchor back to tick 0, so the cycles line up with tick 0 and the song position (see [Clock & Timing](#clock--timing)); Ctrl+R, opening a file and a new file also start every `&` from tick 0 (they clear its state, and Ctrl+R resets the tick count), while Stop and Play keep the tick count, so the cycle carries on. Start and End set the range of the output; each step is scaled to that range with rounding, so it lands on its nearest value; shapes 0-5 reach both ends at every rate, the saws reach their far end only at rates `b` and up.

| Operator | Start Value | End Value | Rate | Shape |
|:--------:|:------:|:------:|:------:|:------:|
|    &     |   A    |   B    |   R    |   S    |

- `A`: Start value (0-z)
- `B`: End value (0-z)
- `R`: Rate - The cycle length in beats (1-z; `.`, `0` or any non-alphanumeric glyph holds the phase-0 output)
- `S`: Shape (0-7) - Selects the waveform; `.` or anything above 7 is shape 0

`&` outputs nothing when its Start or End is `.`. An example:

- `&3a22`
- Cycles between 3 and a every 2 beats (8 ticks) with waveform shape 2 (Sine): from tick 0 it outputs `3469a964`, repeating every 8 ticks

| Value | Pattern           | Description                                    |
|-------|------------------|------------------------------------------------|
| 0     | Triangle         | Linear up then down                            |
| 1     | Inv. Triangle    | Linear down then up                           |
| 2     | Sine            | Smooth curved transition up then down          |
| 3     | Inv. Sine       | Smooth curved transition down then up          |
| 4     | Square          | Instant switch between min and max            |
| 5     | Inv. Square     | Instant switch between max and min            |
| 6     | Saw             | Linear up, instant down                       |
| 7     | Inv. Saw        | Linear down, instant up                       |

### MIDI CC `!` and glides

The MIDI CC operator sends MIDI Control Change messages. The control number is specified using three decimal digits (hundreds, tens, ones) allowing access to all MIDI CC numbers from 0-127. Missing digits can be omitted using `.` (e.g., `003` or `..3` for CC#3, `049` or `.49` for CC#49).

| Operator | Channel | Control Hundreds | Control Tens | Control Ones | Value | Interpolation Rate |
|:--------:|:-------:|:---------------:|:------------:|:------------:|:-----:|:-----------------:|
|    !     |    C    |       Ch        |      Ct      |      Co      |   V   |         R         |

- `C`: MIDI channel (0-F)
- `Ch`: Hundreds digit of control number (0-9) or `.` to omit
- `Ct`: Tens digit of control number (0-9) or `.` to omit
- `Co`: Ones digit of control number (0-9) or `.` to omit
- `V`: Control value (0-z) - maps to MIDI CC values in increments of 4
- `R`: Interpolation rate (0-z) - `.`, `0` and `z` for instant, `1`-`y` to glide over 36 − rate ticks

Like `:`, it runs only when banged. Here a `D4` bangs it every 4 frames, and it sends CC 7 on channel 3 with value 64:

```
D4......
.!3..7g.
```

It sends nothing when its Channel is above `f`, or when its Channel or Value is `.`. Control numbers above 127 are clamped to 127. Values go in steps of 4:

- `0` → 0 (minimum)
- `8` → 32 (25% intensity)
- `g` → 64 (50% intensity, center)
- `o` → 96 (75% intensity)
- `w` → 127 (maximum)

Examples:

- `!3..70.` - Sends MIDI CC #7 (volume) on channel 3 with value 0 (silent), instant
- `!3..7g.` - Sends MIDI CC #7 (volume) on channel 3 with value 64 (half volume), instant
- `!3..7w.` - Sends MIDI CC #7 (volume) on channel 3 with value 127 (full volume), instant
- `!3.01o.` - Sends MIDI CC #1 (mod wheel) on channel 3 with value 96, instant
- `!3..7g5` - Sends MIDI CC #7 on channel 3 with value 64, interpolated at rate 5 from the last value sent (it jumps to 64 if nothing has been sent on that control yet)
- `!3..7w1` - Sends MIDI CC #7 on channel 3 with value 127, slowly interpolated (rate 1) from the last value sent (it jumps to 127 if nothing has been sent on that control yet)

#### Glides

When the rate, the 6th input, is `1` to `y`, `!` glides the CC from its current value to the target instead of jumping, in steps sent on the clock between ticks as well as on them, so a sweep moves smoothly rather than once a tick.

- **Rates `.`, `0` and `z`**: instant. The value goes out at once, in the tick that bangs the operator. Uppercase reads like lowercase, so `Z` is instant too, and any other glyph counts as `0`, so it is instant.
- **Rates `1` to `y`**: a glide over 36 − rate ticks: `1` takes 35 ticks (the slowest), `w` 4 and `y` 2. Its length is fixed in time when it starts, so a tempo change while it runs does not move its end.
- **Where a glide starts**: from the CC's current value, which is where a glide already running on that channel and control has got to, or else the last value zOrca sent to it. If zOrca has sent nothing to that control since it started, since the file was opened or created, or since the MIDI output changed, the glide jumps straight to its target. To glide from a known value, send it first with an instant `!`.
- **Banging it again**: banging the same glide again (the same target and rate) while it runs leaves it running, so a `!` banged on every tick still reaches its target on time. A new target or rate starts a new glide from the current value, and a target equal to the current value stops the glide. That target goes out first if the last value sent differs from it, because the glide's step to it was held back by the glide rate or the CC cap, so the control never stays a step short; otherwise nothing goes out. An instant `!` on the same control stops a running glide.
- **Steps**: a glide sends a step whenever its value changes, on wall-clock time: between ticks, as soon as the step is due, and not only when a tick runs. One control gets at most Glide Rate steps a second, 100 unless the Glide Rate setting changes it, and a glide that runs to its end finishes on its target, which goes out at its end whatever the glide rate. A value already reached is not sent again while the operator keeps banging.
- **Glide Rate**: open the main menu (Ctrl+D), choose Clock & Timing..., then Glide Rate..., and pick 50, 100 or 200 Hz, the most steps a second one glide sends. A new rate applies at once, to running glides too. It is saved in `orca.conf` as `glide_hz = 50`, `100` or `200`; any other value is ignored, which leaves 100, and its value is kept. Picking the rate that is already active saves nothing.
- **The cap**: zOrca keeps one budget of 800 CC messages a second, with bursts of up to 80, that every CC it sends draws on. A glide's step goes out only when the budget has room for it, and otherwise waits; when many glides run at once, their steps take turns, so none of them always waits. Instant CCs and glide targets always go out and draw on the budget, even past the cap, so while instants alone go past 800 a second, running glides send no steps and jump to their targets at their ends. The cap is fixed; there is no setting for it.
- **Pausing** stops every running glide where it is; the next glide starts from the last value sent.

Each channel and control has its own glide, so several CCs glide independently. `examples/misc/glide_rate.orca` keeps one glide running, its target changing on every tick, so it steps at the glide rate.

### Random `R` and `r`

The `R` operator (uppercase) provides pure random generation that runs every tick. The `r` operator (lowercase) requires bang and deals its values from a shuffled bag, so it never sends the same value twice in a row.

- **`R` (uppercase)**: Pure random, runs every tick. In bOrca's dialect it outputs a value from min to max, both included, with the inputs in either order and an empty input counting as 0: `.R.` always outputs `0`, and `5R.` 0 to 5. Equal inputs give that value. The upstream [dialect](#dialects) keeps Orca-c's range: min up to, but not including, max, with a max of `0` standing for 36. Both draw from one hash of the seed, the cell and the tick.
- **`r` (lowercase)**: Shuffle-based random, requires bang. Its max is inclusive: each bang sends the next value of a bag that holds every value from min to max once, shuffled for each `r` from the seed and its cell. A max of `0` is 0: `5r0` deals 0 to 5, and `0r0` always sends 0. `r` writes nothing when an input is `.`, where bOrca's `R` counts it as 0. A new bag, after the last value or after min or max changes, never starts with the value just sent, so `r` never sends the same value twice in a row (except after Ctrl+R, a new file or an opened file, which clear its state: it starts over, and its first value may repeat the last one sent). A two-value `r` therefore strictly alternates: `0r1` gives 1010… forever, and the seed only picks which value comes first. Only when min equals max does its one value repeat.

Both swap min and max when min is the larger, and both take their right input's case: an uppercase max gives uppercase letters, and digits stay digits. When min equals max, bOrca's `R` and `r` keep the max's case too, so `aRA` gives `A` and `crC` gives `C`; the upstream `R` writes that value in lowercase. An uppercase letter directly left of `r` is an operator itself, and most (`A`, `B`, `C`, `D` …) run first and take `r` as their input, so give `r` a lowercase or digit min: `arC`, not `ArC`. Like any uppercase letter, an uppercase max, and the uppercase letters `r` writes, run as operators on every tick `r` is not banged; so bang `r` every tick, or feed its output to an input that locks it, such as `:`'s note, as `examples/misc/random_unique.orca` does. That patch plays two `r`.

More patches: `examples/basics/` has a short patch for many single operators, `examples/misc/` holds zOrca's demos and other small patches, `examples/benchmarks/` and `examples/setups/` larger ones, and `examples/upstream/` the patches that need the upstream dialect.

## Clock & Timing

zOrca runs on its own clock, or follows an external MIDI clock: an iPad, a hardware sequencer or a DAW. Open the main menu (Ctrl+D) and choose Clock & Timing... for these settings; the Glide Rate setting of `!` lives there too (see [Glides](#glides)).

### Tempo and beat clock out

Internal, the default clock source, is zOrca's own tempo, set with Set BPM..., `<` and `>` or `--bpm` (120 by default). One frame is a sixteenth note: 4 frames to a beat.

Send MIDI Beat Clock, under Clock & Timing..., sends the beat clock to downstream gear while the source is Internal: `FA` (start) when playback starts, 24 `F8` pulses to a beat while it plays, and `FC` (stop) when it pauses or when zOrca quits while playing. Turning it on while playing sends `FB` (continue) and keeps the tick grid; turning it off while playing sends `FC`. It is saved in `orca.conf` as `midi_beat_clock = 1` or `0`. With OSC output on, `/orca/bpm` carries the tempo whenever it changes, and `/orca/started` and `/orca/stopped` mark playback.

### Following an external clock

Following needs a build with a MIDI input: an ALSA or a PortMidi build.

1. **Pick the input.** Under Clock & Timing..., MIDI Input... lists the inputs. An ALSA build lists the sequencer ports that can be read and subscribed to, as `client:port` (Midi Through included, which is how a software master on the same machine is usually reached), and saves the pick in `orca.conf` as `alsa_input_device = Midi Through:Midi Through Port-0`; while the clock source is External, zOrca subscribes that port to its own `MIDI in` port itself, and resubscribes it when its client leaves and comes back, so there is nothing to connect with `aconnect`. A connection made by hand to `MIDI in` also feeds the clock, so two clocks there double-count. A PortMidi build lists the input devices and saves the pick as `portmidi_input_device = <name>`; a device plugged in after `orca` started needs a restart to be seen. `orca --midi-in <name>` picks the input for one session, over the saved one, without changing it: `client:port` in an ALSA build, the device name in a PortMidi build. An input that is not there when External is chosen is reported on screen; an ALSA build then waits for it to appear, a PortMidi build needs the menu or a restart. An input line in `orca.conf` naming a device that is not found is kept as it is.
2. **Choose External** under Clock Source.... The choice is saved in `orca.conf` as `clock_source = internal` or `external`; a value other than these two is ignored, which leaves Internal, and so is `external` in a build with no MIDI input. `orca --clock-source external` (or `internal`) picks the source for one session, over the saved one, without changing it. Picking the source that is already active saves nothing.
3. **Start the master.** Its `FA` (start) starts playback from tick 0, `FB` (continue) resumes it, `FC` (stop) pauses it, releasing the sustained notes and stopping running glides, and `F2` (song position) sets the frame, in sixteenths, so each song-position unit is one frame; the first `F8` after `FA` runs frame 0 and every sixth pulse after it runs the next. Only the master runs playback: Space (outside append mode), `<` and `>` are ignored, Set BPM... says that the BPM follows the external clock, and zOrca does not auto-play when it starts. Ctrl+R and Ctrl+F still work.
4. **Watch the HUD.** The BPM field shows the clock while the source is External: `ext --` until a tempo has been measured, and `ext 120` once a beat of pulses has been measured (the 25th pulse, 24 intervals). The shown tempo moves only when the measured one has stayed at least 1 BPM away from it for a whole beat, so the jitter of a Bluetooth link does not flicker it. CC glides take their length from the measured tempo, and `/orca/bpm` carries the shown value whenever it changes; `/orca/started` and `/orca/stopped` follow the master's transport.

While zOrca follows, Send MIDI Beat Clock is suspended: the setting stays as it is and is saved as it is, the item reads `(suspended)`, and zOrca sends none of `F8`, `FA`, `FB` and `FC`, so nothing echoes transport back to the master. Switching to External stops playback without sending `FC` to downstream gear: zOrca is about to follow another master, so it transmits nothing from then on. Switching back to Internal adopts the shown tempo as the BPM when one has been measured, and keeps the stored BPM otherwise; if zOrca was playing it keeps playing, its first internal frame coming one frame later, with `FB` and the pulses if beat clock is on. A master that stops its own clock on `FC`, as hardware sequencers do, leaves zOrca with no pulses, so its tempo is dropped a second later and a switch to Internal after that keeps the stored BPM.

### When the clock stops or zOrca stalls

The HUD shows `ext lost` when the pulses stop for a second while playing, which also pauses playback and releases the notes, until the master sends Start or Continue. A gap of a second or more between two pulses that do arrive drops the measured tempo (`ext --`) but does not stop playback: only a second with no pulse at all is a lost clock. If zOrca itself stalls while the master's pulses queue, the stall does not count as a lost clock once the queued pulses arrive.

If zOrca itself hangs while the master keeps clocking, because the device stalls or a terminal stops reading, its notes go silent and a note that was sounding holds until it recovers. On recovery, the steps it missed play at once as very short notes (up to about 83), so it lands back on the master's frame; the HUD can show a wrong tempo, up to `ext 999`, for about a beat (half a second at 120 BPM), and a glide started then arrives early. An ALSA build keeps up to 500 queued events, about 10 seconds of clock alone at 120 BPM (4 seconds at 300 BPM), less when other MIDI messages or port changes reach zOrca's input during the hang. After a longer hang, the queued clock is lost and the missed steps never play: zOrca carries on from the frame where it stopped, behind the master by the length of the hang, and off the master's sixteenths by part of a step unless the hang lasted a whole number of steps, until the master next sends Start (`FA`) or a song position (`F2`). A Start, Stop or song position the master sent during such a hang is lost too, so zOrca can play on after the master stopped, until the master's next Start or Stop, or stay stopped while the master plays, until its next Start or Continue. A PortMidi build on Linux still stops at `ext lost` after a hang of about four seconds at 120 BPM; use an `--alsa` build.

## Settings and command line

`orca` keeps its settings in `orca.conf`: `$XDG_CONFIG_HOME/orca.conf`, or `~/.config/orca.conf` when `XDG_CONFIG_HOME` is unset or empty. On the uConsole appliance it is `~/.config/borca/orca.conf`. Each line is `key = value`, and a line that starts with `#` or `;` is a comment. The menus save at once: a save writes the keys that were read from the file or changed in a menu, and keeps every other line as it is, comments included.

| Key | Values | Default | Set with | Notes |
| --- | --- | --- | --- | --- |
| `portmidi_output_device` | a PortMidi device name | none | MIDI Output... (PortMidi build) | Free text: a read never changes it; every pick in the menu saves |
| `osc_output_address` | a host name or address; empty is this machine | empty | OSC Output..., OSC Output Address... | Entering the address already set saves nothing |
| `osc_output_port` | a UDP port | `49162` | OSC Output..., OSC Output Port... | Entering the port already set saves nothing |
| `osc_output_enabled` | `1` or `0` (also `true`, `yes`, `false`, `no`) | `0` | OSC Output..., OSC Output Enabled | Another value is ignored |
| `midi_beat_clock` | `1` or `0` (also `true`, `yes`, `false`, `no`) | `0` | Clock & Timing..., Send MIDI Beat Clock | Another value is ignored |
| `margins` | `<x>x<y>`, or one number for both | `2x1` | Appearance..., Margins... | Another value is ignored; entering the margins already set saves nothing |
| `grid_dot_type` | `fancy` or `plain` | `fancy` | Appearance..., Grid dots... | Another value is ignored; picking the active one saves nothing |
| `grid_ruler_type` | `fancy` or `plain` | `fancy` | Appearance..., Grid rulers... | Another value is ignored; picking the active one saves nothing |
| `dialect` | `borca` or `upstream` | `borca` | Operator Dialect... | Another value is ignored; picking the active one saves nothing; `--dialect` overrides it for a session without saving |
| `glide_hz` | `50`, `100` or `200` | `100` | Clock & Timing..., Glide Rate... | Another value is ignored; picking the active one saves nothing |
| `clock_source` | `internal` or `external` | `internal` | Clock & Timing..., Clock Source... (ALSA or PortMidi build) | Another value is ignored, and so is `external` in a build with no MIDI input; picking the active one saves nothing; `--clock-source` overrides it for a session without saving |
| `portmidi_input_device` | a PortMidi input device name | none | Clock & Timing..., MIDI Input... (PortMidi build) | Free text: a read never changes it, and a name not found is kept as it is; picking the active one saves nothing; `--midi-in` overrides it for a session without saving |
| `alsa_input_device` | `client:port` | none | Clock & Timing..., MIDI Input... (ALSA build) | Free text: a read never changes it, and a name not found is kept as it is; picking the active one saves nothing; `--midi-in` overrides it for a session without saving |

An ignored value leaves the key at its default, and its line is written back as it is. A build reads only its own backend's input key, and keeps the other one's line.

`orca [options] [file]` opens the patch `file`, or a new grid. Its options, which `orca -h` prints:

| Option | What it does | Default | Build |
| --- | --- | --- | --- |
| `--undo-limit <number>` | The most undo steps to keep; set it low for large files | 100 | any |
| `--initial-size <x>x<y>` | The size of a new grid, such as `40x30` | fits the window | any |
| `--bpm <number>` | The tempo in beats per minute | 120 | any |
| `--seed <number>` | The seed for `R`, for lowercase `r` (each cell seeds its own shuffle from it) and for the Random pattern (`c`) of `;` | 1 | any |
| `--dialect <name>` | The operator dialect for this session, `borca` or `upstream`, over the saved one, without saving it | the saved dialect, else `borca` | any |
| `--strict-timing` | Tries to reduce the timing jitter of outgoing MIDI and OSC; uses more CPU time, and may have no effect | off | any |
| `--osc-midi-bidule <path>` | Sends MIDI as OSC formatted for Plogue Bidule, to the path of its OSC MIDI device, such as `/OSC_MIDI_0/MIDI` | off | any |
| `--clock-source <source>` | The clock source for this session, `internal` or `external`, over the saved one, without saving it | the saved source, else `internal` | ALSA or PortMidi |
| `--midi-in <name>` | The MIDI input to follow for this session, over the saved one, without saving it: `client:port` in an ALSA build, the device name in a PortMidi build | the saved input | ALSA or PortMidi |
| `--version` | Prints the version and exits | | any |
| `-h`, `--help` | Prints the options and exits | | any |

`orca` also accepts `--hard-margins <x>x<y>`, or one number for both, which `-h` does not print: it leaves that many blank columns and rows around the whole screen.

`cli [options] infile` runs a patch headless, with no screen and no MIDI, and prints the grid after the last tick. Its options, which `cli -h` prints:

| Option | What it does | Default |
| --- | --- | --- |
| `-t <number>` | The number of ticks to run, 0 or more | 1 |
| `-q`, `--quiet` | Prints no grid | off |
| `--events` | After each tick, prints that tick's events, one per line (`NOTE`, `CC`, `CCI`, `PB`, `OSC` and `UDP`, each after `t<tick>`), then `t<tick> GRID` and the grid; with `-q`, only the events | off |
| `--seed <n>` | The seed for the random operators | 0 |
| `--dialect <d>` | The operator dialect, `borca` or `upstream` | `borca` |
| `--version` | Prints the version and exits | |
| `-h`, `--help` | Prints the options and exits | |

`--seed` defaults to 1 in `orca` and 0 in `cli`, so run `orca --seed 0` to hear the sequences that `cli` and the golden tests show.

## Dialects

zOrca runs one of two operator lists, its dialect:

- **bOrca** (the default) is this fork's: `!` is the MIDI CC operator with three control digits and glides, `$` the Scale operator, `;` the Arpeggiator, `=` the Midichord operator, `&` the Bouncer, and lowercase `r` deals from a shuffled bag. [bOrca's operators](#borcas-operators) describes them. `L`, `I`, `U`, `R`, `D`, `C` and `Z` follow Orca's 2025 rules, below.
- **Upstream** is upstream Orca-c's (at `9df9786`), so its patches play unchanged. `!` sends a MIDI CC from three inputs, channel, control and value: the control is one glyph, `0`-`z` for controls 0-35, the value's `0`-`z` is scaled to 0-127 (× 127 / 35), and it never glides; it sends nothing when the channel is above `f` or the channel or control is empty. `;` sends the glyphs east of it, up to the first `.` and at most 16, as one UDP datagram. `=` sends an OSC message to `/` plus its first input, with as many integers as its second input says, read from the inputs east of that. Lowercase `r` is a banged `R`, so its max is exclusive. `$` and `&` do nothing. `L`, `I`, `U`, `R`, `D`, `C` and `Z` keep Orca-c's defaults, and `R` its range.

In November 2025 Orca's author removed the defaults of `L`, `I`, `U`, `R`, `D`, `C` and `Z` from the web Orca (hundredrabbits/Orca), so that an empty input counts as 0 and a patch plays alike in every Orca. His reference is the playground on the [Orca wiki page](https://wiki.xxiivv.com/site/orca), whose `orca.rom` is dated 16 May 2026. The web Orca follows it, except that there a `C` with an empty modulo leaves its cell alone and a `D` with a modulo of 1 bangs on every frame. bOrca's dialect follows the playground. Orca-c has not changed, so the upstream dialect keeps its rules:

| Operator | bOrca's dialect (Orca's 2025 rules) | Upstream dialect (Orca-c) |
| --- | --- | --- |
| `L` | An empty input counts as 0: `.L5` gives `0` | An empty input gives `.` |
| `I` | An empty step is 0, and an empty or `0` modulo gives `0`: `.I4` stays at `0` | An empty step is 1, and an empty or `0` modulo is 36: `.I4` counts 1 2 3 0 |
| `U` | An empty step is 0 and an empty or `0` max 1: `.U8` never bangs, `3U.` bangs on every frame | An empty step is 1, and an empty or `0` max is 8 |
| `R` | Min to max, both included, in either order; empty is 0, so `.R.` is always `0` | Min up to, not including, max; a max of `.` or `0` stands for 36 |
| `D` | An empty or `0` modulo is 1: `.D.` bangs on every frame, `2D.` on every 2nd | An empty or `0` modulo is 8 |
| `C` | An empty or `0` modulo is 1, so `.C.` writes `0` | An empty or `0` modulo is 8: `.C.` counts 0 to 7 |
| `Z` | An empty rate is 0: `.Z5` does not move | An empty rate is 1 |

A bang (`*`) in the left input of `I`, `U` or `Z` counts as an empty input: 0 in bOrca's dialect, and upstream a step or rate of 1.

Each implementation's `R` plays its own sequence. zOrca's two dialects share Orca-c's hash, so with the same seed bOrca's `1R4` plays what upstream `1R5` plays.

To write a patch that plays the same in both dialects, write the values out: `1C8`, `1D8`, `1I4`, `1U8`, `1Z5`. `R`'s top value is included in bOrca's dialect, so write the larger input one lower: `1R3` there plays what `1R4` plays upstream, and `3R1` what `4R1` plays. A max of `.` or `0` becomes `z`: `5Rz` plays what `5R.` plays, and `0Rz` what `.R.` plays. Equal inputs keep the max's case in bOrca's dialect, so `aRa` plays the `a` that `aRA` plays upstream. An `L` with an empty input, and an `I` with a step and an empty or `0` max (`.I.` counts to `z` and wraps), have no written form. An input that another operator writes as `.` or `0` while the patch plays follows the new rule too, and writing a value out cannot change that.

Every other operator, `A`-`Z` but these seven, and `*`, `#`, `:`, `%` and `?`, is the same in both. The editor colours ports bOrca's way in both dialects: `:`, `%`, `?`, `G` and `K` mark some inputs as parameters where upstream Orca-c marks them as plain inputs, which changes only how they look, never the events or the grid. In the upstream dialect `$` and `&` draw as unknown glyphs (bold red), as they do in Orca-c, since they do nothing there. OSC and UDP go to the address and port set under OSC Output in the main menu, and nowhere while OSC output is off. The `/orca/bpm`, `/orca/started` and `/orca/stopped` messages, and MIDI sent as OSC with `--osc-midi-bidule`, go to the same address.

To switch, open the main menu (Ctrl+D), choose Operator Dialect..., and pick bOrca or Upstream. The choice is saved in `orca.conf` as `dialect = borca` or `dialect = upstream`; a value other than these two is ignored, which leaves bOrca. `orca --dialect upstream` (or `borca`) picks the dialect for one session, over the saved one, without changing what is saved. Picking the dialect that is already active saves nothing, even when `--dialect` chose it; to save the dialect `--dialect` chose, pick the other one and then pick it back. `cli --dialect upstream` runs a patch headless in the upstream dialect. Changing the dialect, even while playing, clears every operator's per-cell state, as Ctrl+R does, and stops running CC glides. The operator guide (Ctrl+G) lists the active dialect's operators.

For the patches in `examples/upstream/`, and how they are checked against Orca-c, see [`examples/upstream/README.md`](examples/upstream/README.md).

## Building and development

### Build targets

`tool`, a POSIX `sh` script, builds everything; `./tool help` lists every option. It needs a C99 compiler and, for the livecoding environment, ncursesw (Debian and Ubuntu: `libncurses-dev`).

```sh
./tool build orca              # livecoding environment, no hardware MIDI: build/orca
./tool build --portmidi orca   # with PortMidi MIDI output (libportmidi-dev)
./tool build --alsa orca       # with native ALSA MIDI output, Linux only (libasound2-dev)
./tool build -d orca           # debug build with ASan and UBSan: build/debug/orca
./tool build cli               # headless interpreter: build/cli
./tool build test              # unit tests: build/unit_tests, see tests/README.md
./tool build readme-gen        # prints the README's scale and chord tables: build/readme-gen
```

The `make` wrapper:

```sh
make release     # ./tool build --portmidi orca (the default target)
make debug       # ./tool build -d --portmidi orca
make appliance   # ./tool build --alsa --harden --pie orca, Linux only: the uConsole appliance build
make clean       # removes build/
```

`make appliance` is ALSA only, so it needs `libasound2-dev` and never PortMidi.

### Which backend

ALSA, on Linux only, is native: `orca` opens its own sequencer client, which `aconnect` and MIDI routers see by name, and needs only `libasound`. PortMidi works on Linux and macOS, and picks its output under MIDI Output... in the main menu. Either can follow an external clock, but after a long hang only an ALSA build keeps following (see [When the clock stops or zOrca stalls](#when-the-clock-stops-or-zorca-stalls)). `--alsa` cannot be combined with `--portmidi` or `--static`, and fails on anything but Linux. A build with neither sends no hardware MIDI; it can still send OSC.

### The ALSA client

An `--alsa` build opens one ALSA sequencer client when it starts, named `zOrca`, or the value of `ZORCA_ALSA_CLIENT_NAME` when that is set and non-empty (else `BORCA_ALSA_CLIENT_NAME`, its name before v5.0.0), with one output port, `MIDI out`, that other programs subscribe to (`aconnect -l` lists it), and one input port, `MIDI in`, which zOrca itself subscribes to the chosen MIDI Input while the clock source is External (see [Following an external clock](#following-an-external-clock)). `MIDI in` is write-only, so a router that connects readable ports never connects it. The client opens even with `--osc-midi-bidule`, whose output then replaces it, so such a run also needs a sequencer and, while another `zOrca` runs, its own client name. If the sequencer cannot be opened, or another client already has the name (checked at startup, before and after taking the name: when restarting `orca` by hand, wait for the old instance to exit; two instances started at the same moment may both exit; start one again), or the sequencer cannot list its clients, or the name is over 63 bytes (ALSA keeps 63; set a shorter `ZORCA_ALSA_CLIENT_NAME`), `orca` prints an error and exits with status 1 before the screen starts; two clients with one name would make the uConsole appliance's router refuse to route either. A MIDI message the sequencer does not take is dropped and the HUD's first line then shows a bold `MIDI err` until you quit; only a refused note-off is kept and sent again, for up to 3 seconds, so the note does not hang. SIGTERM, SIGHUP or SIGINT quits cleanly, stopping playback first, unless that signal was already ignored when `orca` started. `orca -h` repeats the client's name, ports and exit rules in an `--alsa` build.

### Testing

```sh
./tool build cli && tests/run.sh build/cli                          # golden tests: every patch through cli --events
./tool build test && build/unit_tests                               # unit tests
./tool build readme-gen && tests/check-doc-sync.sh build/readme-gen # the README's generated tables
tests/check-includes.sh && tests/check-nm.sh                        # core checks
```

CI runs these and more on every push and pull request to `main` and `spec/borca-fixes`, and on tags: on x86_64 and arm64, with `-funsigned-char`, under clang's sanitizers, and at 32 bits (armhf, under qemu); see [CI](tests/README.md#ci). The device checks `tests/alsa-guard/devcheck.sh`, `tests/alsa-contract/devcheck.sh` and `tests/alsa-clock/devcheck.sh` need a sequencer (`/dev/snd/seq`), which CI runners lack, so they are run by hand on an `--alsa` build. CI checks the upstream dialect byte for byte against upstream Orca-c's own `cli` on all 43 of its examples and on Orca's operator self-test (see [`tests/README.md`](tests/README.md)). [`tests/README.md`](tests/README.md) describes every test and check.

### Debug counters

For development, `CFLAGS_EXTRA=-DBORCA_DEBUG_COUNTERS ./tool build orca`, with or without `--alsa`, counts the event loop's wakeups and, after the screen closes on quit, prints its lines to stderr, the wakeup line last. The counters are never in a release or appliance build; CI checks this.

- `zOrca wakeups: <n> waits, <z> zero-timeout in <s> s`: `<n>` counts the loop's waits for input entered with a timeout above 0, `<z>` those entered with a timeout of 0, and `<s>` the seconds since the loop started. Under External with ALSA the loop's wait is a `poll()` over the keyboard and the sequencer, and this line counts those waits in place of the keyboard reads. `tests/wakeups/ab.sh` compares it with `4f349cd`'s (see [Wakeup check](tests/README.md#wakeup-check)).
- `zOrca ALSA: <n> MIDI sends dropped on EAGAIN/EINTR, <m> not sent in all`, in an `--alsa` build: `<n>` counts the messages dropped because the sequencer's output was full or the write was interrupted, and `<m>` every message that did not go out.
- `zOrca MIDI in: <n> overflows, <m> read errors`, in a build with a MIDI input (`--alsa` or `--portmidi`): `<n>` counts the input overflows the reader met (the sequencer's `-ENOSPC`, which empties its input queue, or PortMidi's buffer overflow), after each of which it went on reading, and `<m>` the other read errors, each of which ended one pass over the input.
- `zOrca ALSA input pool: <p> events (asked for 500)`, in an `--alsa` build: `<p>` is the input pool its sequencer client has, read back after it asked for 500 events at startup (`-1` when it could not be read).
- `zOrca events: <n> dropped (tick <t>, preview <p>, engine <e>)`, just before the wakeup line in every counter build: the events each event list dropped because it could not grow, which happens only when memory runs out, `<n>` in all and then the tick's, the preview's and the engine's lists.

### The README's tables

The tables in [Scales and chords](#scales-and-chords) are generated. `build/readme-gen` prints them from `music.c`, where every scale and chord is defined once, and `tests/check-doc-sync.sh build/readme-gen` fails when the README's copy differs from that output; CI runs it. To change a table, change `music.c`, then replace the old tables with readme-gen's output. See [readme-gen and doc-sync](tests/README.md#readme-gen-and-doc-sync).

### Versions and branches

Each release is an annotated tag, `vX.Y.Z`, listed on the [tags page](https://github.com/ianhundere/zOrca/tags); the newest tag is the latest release. `git checkout vX.Y.Z` checks a release out to build it, and `git describe --tags` names the release a checkout is at or after; `orca --version` and `cli --version` print it. `main` is zOrca's release line from v1.0.0 and moves only at a release; changes land on `spec/borca-fixes` first. The JS Orca that `main` held before v1.0.0 is kept on the `js-orca` branch.

## Changelog

The release notes are in [CHANGELOG.md](CHANGELOG.md), newest first.

Each release is a git tag; see the [tags page](https://github.com/ianhundere/zOrca/tags).

## Credits and licence

[ORCΛ](https://wiki.xxiivv.com/site/orca.html) is by Hundred Rabbits, and Orca-c, its C implementation, is the code zOrca grew from:

| Main git repo | GitHub mirror |
| ------------- | ------------- |
| [git.sr.ht/~rabbits/orca](https://git.sr.ht/~rabbits/orca) | [github.com/hundredrabbits/Orca-c](https://github.com/hundredrabbits/Orca-c) |

zOrca is a fork of [boorch/bOrca](https://github.com/boorch/bOrca), at `4f349cd`. boorch added the operators `$`, `=`, `;`, `&`, `r` and the CC `!`. His note on the fork reads:

> I'll try to add new operators based on my needs. I'm not a professional programmer, just a hobbyist, so most of the code here is generated by various AI assistants, based on 'painfully' extensive directions I provided. I've tried to include comments as much as possible to remember what I did and why, but I'm fairly certain it's not the most elegant code that adheres to proper standards. You should consider this fork to be 'somewhat' functional. Currently I'm focused on Orca-c only, since my use cases are:
> - Using it through SSH on a Raspberry Pi Zero 2W, connected to an iPad Pro 11", outputting MIDI data via USB, acting as a MIDI Gadget
> - Using it on my uConsole terminal, paired to a CME WIDI Jack via Bluetooth, which is connected to the MIDI ports of a Torso S4.

zOrca is maintained by Ian Hundere.

MIT; see [LICENSE.md](LICENSE.md).
