# Changelog

zOrca's versions follow [semantic versioning](https://semver.org). A change is Breaking when an existing patch plays or sends differently in normal use, or when the public `orca_run` API breaks, and a Breaking change takes a new major version; a fix to a failure path, or a new message at start, stop or quit, is listed under Changed or Fixed. Each version is an annotated git tag, listed on the [tags page](https://github.com/ianhundere/zOrca/tags), and the newest comes first here.

To bring a patch over from boorch/bOrca or from upstream Orca-c, see [Coming from boorch/bOrca 4f349cd](#coming-from-boorchborca-4f349cd) and [Coming from Orca-c](#coming-from-orca-c).

## v5.0.0 (2026-10-17)

### Breaking

The project is now zOrca, and an `--alsa` build's sequencer client is named `zOrca` where it was `bOrca`. Patches play as before; what changes is the name other programs use to find it.

- **The ALSA client is `zOrca`, not `bOrca`.** `aconnect -l` lists `zOrca`, and `aconnect zOrca:0 <synth client>:<port>` connects its `MIDI out` port. A script, a MIDI router or a saved patchbay that connects `bOrca:0`, or finds the client by the name `bOrca`, finds nothing now: change the name to `zOrca`. PortMidi builds name no client and are not affected.
- **`ZORCA_ALSA_CLIENT_NAME` replaces `BORCA_ALSA_CLIENT_NAME`.** It sets the client's name for one run, which a second instance needs. `BORCA_ALSA_CLIENT_NAME` still works through v5.x when `ZORCA_ALSA_CLIENT_NAME` is unset or empty; when both are set, `ZORCA_ALSA_CLIENT_NAME` wins. The messages for a name that is taken or longer than 63 bytes, and `orca -h`, name `ZORCA_ALSA_CLIENT_NAME`.

### Changed

- **The name.** The repository is `ianhundere/zOrca`. `orca --version` and `cli --version` print `zOrca <version>`; the out-of-memory message, the ALSA start and shutdown messages and the debug counter lines (`zOrca wakeups:`, `zOrca events:`, `zOrca ALSA:`, `zOrca ALSA input pool:`, `zOrca MIDI in:`) begin with zOrca; the README and the test docs say zOrca. The program is still `orca`.
- **The dialect keeps its name.** The operator dialect is still called bOrca, because those are boorch's operators: `dialect = borca` in `orca.conf` and `--dialect borca` select it as before, and the menu and the operator guide still say bOrca.

Tests: a unit row, `alsa_client_name_pick`, covers the choice of name (the new variable alone, the old alone, both, the new one empty, neither); CI refuses a 200-byte name through each variable and with both set; the device checks (`tests/alsa-*/devcheck.sh`) use `ZORCA_ALSA_CLIENT_NAME`, and their guard that the live client is untouched accepts the live client as `zOrca` or, before this release, `bOrca`.

## v4.0.2 (2026-10-13)

### Fixed

- **A refused note-off no longer hangs the note.** In an `--alsa` build, when the sequencer refuses a MIDI write because a subscriber's input pool is full, the write returns `-EAGAIN` and bOrca never retries or waits. A lost note-on is a missed note; a lost note-off left the note sounding on the synth until that channel and note was played again, because pause, stop and quit release only the notes still on bOrca's own list. A refused note-off is now kept (up to 256 of them, each for up to 3 seconds) and sent again: the first try 2 ms after the refusal, then at doubling intervals up to 100 ms, at most 16 a pass and after the tick, so a stalled subscriber costs about a dozen failed writes a second and a recovery burst cannot hold up a tick. A later note-on for the same channel and note first sends its held note-off once more, so the pair arrives in order, and drops it if that is refused too; quit makes a last 50 ms try after the stop's note-offs. The sends never sleep. With several subscribers a refusal by one says nothing about the others, so a retry repeats the note-off to the healthy ones as well: harmless to a synth, and the backoff keeps it to about a dozen a second (a healthy second receiver saw 12 copies during a 0.6 s stall, where a retry on every 2 ms wake would have sent about 500). `MIDI err` still shows after the first refusal and stays until you quit, as before. It can happen when a destination is slow to read, such as a Bluetooth MIDI link during a long burst after a hang; the chord-burst test on the Bluetooth chain on 2026-10-13 saw no refused send, so this closes a hole that was found by reading and reproduced with a full pool, not a fault seen in play. It does not change what a patch sends.
- **The Controls box lists what the README lists.** `?` and the main menu's Controls... now show Ctrl+O (Open File), Ctrl+E (Show or Hide the Last Tick's Events), Ctrl+G (Operator Guide) and the `;` `$` `&` glyphs beside the Orca-c ones. The box is 25 rows, which fits the instrument's 100x30 window.

Tests: eleven unit rows (`held_send_*`, `pending_offs_*`, `midi_send_error_is_transient`), driven by a fake write that refuses on cue, each of six mutants of the logic (no cancel, holding note-ons, no backoff, no pass cap, no expiry, no schedule) failing at least one; `tests/alsa-noteoff/devcheck.sh`, a device check on a real sequencer with a receiver whose input pool is full when the note-off goes out and a healthy second receiver, with a negative control: the same run on 50be43f hangs the note (5 note-ons, 4 note-offs), and on this build the off arrives 55 ms after the receiver reads again (5 and 5, 1 held, 1 sent again, 12 copies seen by the healthy receiver; a mutant with no backoff sent 495). CI shellchecks the check and compiles its receiver.

## v4.0.1 (2026-10-13)

### Fixed

- **The Open box keeps long paths.** Ctrl+O, Open... and every other one-line box kept only the first 30 characters of the text they open with, so Ctrl+O then Enter could not reopen a file with a longer path. The whole text is kept now.
- **Folders as file names.** Opening a folder used to leave the grid as it was but take the folder as the file name, so a later Ctrl+S tried to save to the folder. Opening a folder, or saving to one with Ctrl+S or Save As, now says "Is a directory" and keeps the grid and the file name. Save As expands a leading `~` as Open does.
- **Out of memory in the editor.** When a message, menu or form cannot be allocated, `orca` restores the terminal and prints "bOrca: out of memory" before it quits with status 1, instead of leaving the terminal in curses mode with no message.
- **README headings.** The generated tables of scales and chords sit one level under "Scales and chords", with no trailing colon.

### Added

- **`--version`.** `orca --version` and `cli --version` print one line, `bOrca <version>`, and exit 0. The version is `git describe --tags --always --dirty` at build time, or `unknown` without git.

Tests: CI runs `cli --version` and `orca --version`, and the realloc shim's `orca` cases (startup, fill and resize); a unit test pins the numbers of the `bOrca events:` counter line, and another the folder check.

## v4.0.0 (2026-10-13)

### Breaking

This changes how existing patches play in bOrca's dialect; the upstream dialect plays as before.

#### `L`, `I`, `U`, `R`, `D`, `C` and `Z` follow Orca's 2025 rules: an empty input counts as 0

In November 2025 Orca's author removed the defaults of these seven operators from the web Orca (hundredrabbits/Orca), so that an empty input counts as 0 and a patch plays alike in every Orca. His reference is the playground on the [Orca wiki page](https://wiki.xxiivv.com/site/orca). bOrca's dialect now follows the playground:

- `L` counts an empty input as 0, where it wrote `.`.
- `I`'s empty step is 0, where it was 1, and an empty or `0` modulo gives `0`, where it was 36.
- `U`'s empty step is 0, where it was 1, and an empty or `0` max is 1, where it was 8.
- `D`'s and `C`'s empty or `0` modulo is 1, where it was 8. So `.C.` writes `0` on every frame, and `D` bangs when the frame is a multiple of rate × modulo, so `.D.` bangs on every frame and `2D.` on every 2nd.
- `Z`'s empty rate is 0, where it was 1, so its output does not move.
- Uppercase `R` outputs a value from min to max, both included, with the inputs in either order and an empty input counting as 0, so `.R.` always outputs `0` and `5R.` 0 to 5. Equal inputs give that value in the max's case: `aRA` gives `A`. `R` still draws from the same hash of the seed, the cell and the tick, so a run is the same for each seed, but a range now holds one more value, so the values it picks change.

This breaks a patch that has an empty input on one of these seven operators, or an `R` with two different inputs: in bOrca's dialect it plays differently. The upstream dialect keeps Orca-c's rules for all seven, byte for byte. Lowercase `c`, `d`, `i`, `l`, `u` and `z` follow the same rules when banged; lowercase `r` (the shuffle bag) and every other operator are unchanged. See [Dialects](README.md#dialects).

Before → after, bOrca's dialect, the output under each operator, from an empty output cell, at seed 0:

```
.L5   before: .                                after: 0
.I4   before: 1 2 3 0 1 2 3 0 …                after: 0 on every frame
.U8   before: a bang on every 8th frame        after: never a bang
3U.   before: 3 bangs in every 8 frames        after: a bang on every frame
.D.   before: a bang on every 8th frame        after: a bang on every frame
2D.   before: a bang on every 16th frame       after: a bang on every 2nd frame
.C.   before: 0 1 2 3 4 5 6 7 0 1 …            after: 0 on every frame
.Z5   before: 1 2 3 4 5 5 5 …                  after: 0 on every frame
1R4   before: 1 to 3                           after: 1 to 4
5R.   before: 5 to z                           after: 0 to 5
.R.   before: 0 to z                           after: 0 on every frame
aRA   before: a                                after: A
```

To keep a patch playing as before, write the values out: `1C8`, `1D8`, `1I4`, `1U8` and `1Z5` play what `.C.`, `.D.`, `.I4`, `.U8` and `.Z5` played, and they play the same in both dialects. For `R`, write the larger input one lower: `1R3` plays what `1R4` played, at the same seed, and `3R1` what `4R1` played. A max of `.` or `0` becomes `z`: `5Rz` plays what `5R.` played, and `0Rz` what `.R.` played. Equal inputs now keep the max's case, so write `aRa` for the `a` that `aRA` played. An `L` with an empty input, and an `I` with a step and an empty or `0` max (`.I.` counted to `z` and wrapped), have no written form. An input that another operator writes as `.` or `0` while the patch plays follows the new rule too, and writing a value out cannot change that.

Six example patches are rewritten this way so that they keep playing: `basics/g`, `basics/u`, `basics/z`, `benchmarks/families`, `misc/gates` and `misc/tower`. `basics/g`, `basics/u` and `basics/z` play as before. `misc/gates` now computes with `0` and `1`, since its AND relied on `L` writing `.` for an empty input; its three gates bang as before. `benchmarks/families` and `misc/tower` come close but do not match: in families, `1Iz` counts 35 steps where `.I.` counted 36, and `.L.` shows `0`; in tower, the inner clocks read a modulo that another `C` writes as `0`, which now counts as 1, so its values change, though it still animates. Four more examples are not rewritten and play by the new rules: `basics/h`, `basics/j`, `benchmarks/logic` and `misc/bang`. For example, the `2D.` in `basics/h` and `misc/bang` now bangs on every 2nd frame, where it banged on every 16th.

Tests: unit tests for the seven operators in both dialects; [the operator self-test](tests/README.md#the-operator-self-test)'s golden now bangs all 65 comparators in bOrca's dialect; 12 goldens regenerated.

## v3.0.4 (2026-10-13)

Documentation, comments and tests only: the programs are unchanged.

### Changed

- **README.md** is reorganised for bOrca's users: a quick start, the uConsole appliance, the editor's keys and HUD, a summary of bOrca's operators, the clock in three parts, every `orca.conf` key and every `orca` and `cli` option, building and testing, and credits and licence. The release notes moved from its Changelog section into this file, by version.
- **Comments** in the sources, tests, scripts and CI describe each rule and each earlier change in plain words.

### Fixed

- **Bounce's period.** The README gave the period of the arpeggiator's Bounce pattern (`d`) as `4·max(1, ⌊t/3⌋)` steps, but its output repeats every `2·max(1, ⌊t/3⌋)` steps, as the README's [Period column](README.md#arpeggio-patterns) now says.
- **Gaps in the README.** It now says that `=` and `!`, like `:`, run only when banged; which inputs make `;`, `&` and `!` send nothing; that `orca` starts playing at once on its internal clock; and that a stall in bOrca does not count as a lost clock once the queued pulses arrive, whichever the MIDI backend, where it said so of an ALSA build only. It no longer says that the appliance's `verify` check cannot see the `MIDI in` port.

Tests: the Bounce rows of the arpeggiator's unit test, and the period it checks them against, follow the corrected Period column; and [the operator self-test](tests/README.md#the-operator-self-test) that Orca's author posted to check implementations against each other is a new golden, which the upstream comparison also runs through Orca-c's `cli`, byte for byte.

## v3.0.3 (2026-10-13)

### Fixed

- **Memory.** The shell's unchecked mallocs are checked. If the grid cannot be allocated at startup, bOrca quits with "Cannot start: not enough memory for the grid" instead of crashing, and a startup load that fails part way no longer leaks the partial grid. The message, menu and form constructors exit cleanly, and OSC stays off if its device cannot be allocated.
- **Clock follow after a long hang.** An ALSA build's input now holds 500 events, about 10 seconds of clock at 120 BPM, so after a hang that long the missed steps play at once and bOrca lands back on the master's bar. After a longer hang it keeps following instead of stopping at `ext lost`, behind the master until its next Start or song position. A PortMidi build on Linux still stops there. See [When the clock stops or bOrca stalls](README.md#when-the-clock-stops-or-borca-stalls).

### Added

- **Counters.** A debug counter build also prints how many events were dropped (`bOrca events:`), and an `--alsa` one prints the input pool it got (`bOrca ALSA input pool:`). See [Debug counters](README.md#debug-counters).

Tests: CI also rejects calls to `random`, `initstate` and `setstate` in core code, and reads of `environ`; a new device check stalls a real `--alsa` `orca` under a scratch clock master and checks that it lands back on the bar, or keeps following past the buffer.

## v3.0.2 (2026-10-12)

### Fixed

- **Failed reallocs.** Four reallocs stored their result straight into the structure they grow, so a failed one leaked the old buffer and the next write went through NULL. The event list, the field and the mark buffer now keep their old state on a failed realloc, and the shell checks it where a failure would leave the grid, the mark buffer or the undo history inconsistent. Every successful path is unchanged.
- **The last line of `orca.conf`.** When its last line had no newline and was not a setting bOrca reads, such as a comment, it lost its last byte, and a save wrote it back short. The line is now kept whole.

Internal API: `tui_prefs_saves_key` decides which keys a save writes, for the shell and its test alike; nothing a user sees changes.

Tests: the tests share one event recorder; CI's canary and banned set are wider; the ALSA name guard's device check moved into `tests/`; CI checks the shell's arguments to `tick_stop`, `clock_out_toggle` and `clock_out_return_internal`; a realloc shim fails the shell's reallocs on purpose; and a new device check shows that a note reaches an ALSA subscriber and that a failed send shows a bold `MIDI err`.

## v3.0.1 (2026-10-12)

### Fixed

- **Long ALSA client names.** ALSA keeps at most 63 bytes of a client name and cuts a longer one short without an error, so the duplicate-name guard could not match a long `BORCA_ALSA_CLIENT_NAME`, and two instances could run under one cut name. An `--alsa` bOrca now refuses such a name before it opens the sequencer, with its own message, and exits 1. The default name, `bOrca`, is unaffected.

## v3.0.0 (2026-10-12)

### Breaking

This changes what goes out over MIDI for an existing patch.

#### The same note twice in one tick sends one note-off and one note-on

When one tick emitted the same channel and MIDI note twice, as two `:` on one note do, or a `=` chord that shares a note with a `:` beside it, the second was taken for a retrigger of the first: the wire carried a note-off for a note that had not sounded yet, then two note-ons, then one note-off at the later length. A synth that starts a voice per note-on was left with a voice that never got its off. Now the batch of a tick's note-ons holds each channel and note once: a second event for a note already in the batch replaces that entry's velocity and length and takes no slot of its own, so the tick sends at most one note-off, only when the note was already sounding, and one note-on per channel and note, with the later event's velocity and length (below the tick's 512-note batch limit, past which further notes are dropped, as before). The note-on keeps the first event's place among the tick's note-ons, so every other note-on keeps its order. The rule is the same channel and MIDI note, not the same glyph: `:09Y` and `:09z`, which both clamp to note 127, merge too.

- This breaks patches that play one note twice in a tick. With nothing sounding they sent a note-off, then both note-ons; they now send one note-on, at the later event's velocity. With the note already sounding they sent two note-offs, then both note-ons; they now send one note-off, then one note-on. In both cases the note ends at the later event's length, as it did before.
- A mono note (`%`) is unchanged: one note per channel with the later velocity and length, as before. A `:` and a `%` on one channel and note in the same tick still send the `:`'s note-on, its note-off, then the `%`'s note-on.

Nothing else about what goes out, or its order, changes.

Before → after, two `:` on note 36 banged in one tick, `:03C.4` (velocity 127, length 4) then `:03Ca6` (velocity 79, length 6), as the unit test's timeline records the wire (`aseqdump` shows the same messages); the second case has note 36 already sounding and the two the other way round, `:03Ca6` then `:03C.4`, so the later event is the shorter one:

```
nothing sounding   before: Note off 36, Note on 36 vel 127, Note on 36 vel 79 … Note off 36, 6 ticks later
                   after:                                   Note on 36 vel 79 … Note off 36, 6 ticks later
already sounding   before: Note off 36, Note off 36, Note on 36 vel 79, Note on 36 vel 127 … Note off 36, 4 ticks later
                   after:  Note off 36,                                 Note on 36 vel 127 … Note off 36, 4 ticks later
```

No example patch: every patch under `examples/` plays as before (none emits one channel and note twice in a tick). Repro patch: `tests/patches/note_same_tick.orca`, those two `:` banged at tick 0, whose golden pins the VM's two NOTE lines (`cli --events` runs no tick body, so its output does not change); the unit test `tick_note_same_batch` in `tests/unit/test_tick.c` checks the wire for both cases and for the order. A chord and a `:` on one note show it too: `=03C0ff` and `:03Ef4` banged in the same tick both emit note 40 on channel 0 at velocity 119, so such a tick sent a note-off for 40 with nothing sounding and then five note-ons, 40 twice in a tick; it now sends four note-ons, 40 once, and the E still ends 4 ticks later, at the `:`'s length, as it did.

### Fixed

- **The ALSA name guard.** The `--alsa` name guard scans the sequencer again after taking the name, and a failed scan is no longer read as a free name.

Internal API:

- For code that uses `tick.h`: `susnote_list_add_notes` is unchanged, and now expects a batch that holds each channel and note at most once, which `tick_body` guarantees.
- The shell's pause, stop and patch-forget sequences, the clock-out gate and its re-arms, and the PortMidi input decode are unit-tested seams, with the wire order unchanged byte for byte.
- The `orca.conf` key table, line reading, flag overrides and menu setters live in a testable `tui_prefs.c`; `orca.conf` is unchanged.

Tests: a tiered include check and a wider list of calls banned in core code, an `nm` pass at `-O0`, CI greps of the debug counters, a clang sanitizer job and a realloc hook; one `tests/lib.sh` for the sanitizer pattern, `UBSAN_OPTIONS`, the timeout and the crash rule; and CI runs the unit tests at 32 bits (armhf, under qemu), as a required job.

## v2.0.0 (2026-10-11)

### Breaking

Each of these changes how an existing patch plays.

#### `;` takes a Length and a Step, so an arpeggio fits its chord and can be stepped from the grid

The arpeggiator assumed a 7-note scale: range 1 spanned 7 degrees, so fed to a `$` playing a triad it climbed two whole octaves, C to C to C, and Up repeated every 7 bangs instead of every 3. Its step lived only in the cell's hidden state, so the grid could neither sync it nor reset it. `;` now reads two more inputs right of Pattern. Length (x+3) is the number of notes per octave of the `$` selector it feeds; `.` is 7, and `0` or any glyph worth 0 counts as 1. Step (x+4) is `.` to keep the cell's own counter; any other glyph is the step to output, taken modulo the pattern's period, so a `C` or a `T` can drive the arpeggio. The arpeggio spans Length × Range degrees, at most 36, so the output is always one glyph, and at a total of 1 every pattern outputs `0`. A Length change restarts the counter, as a Range or Pattern change does, and `;` still needs a bang to output, Step or not. See the README's [Arpeggiator](README.md#arpeggiator-).

This breaks patches that keep anything in the two cells right of a `;`'s Pattern: an operator or a value at x+3 or x+4 now sets the `;`'s Length or Step, and is locked whenever the `;` is banged. A `*` that a neighbouring `D` leaves at x+4 is step 0, which freezes the arpeggio on one degree, so move such operators right: `tests/patches/arp_patterns.orca` and `tests/patches/seeded_random.orca` now space their `;`s 6 columns apart for this. With both cells empty, a `;` plays exactly as before.

Before → after, the first seven notes of `;103` banged every 2 ticks into `$3Ca` (`cli --events`):

```
before: 36 40 43 48 52 55 60    the 3 at x+3 is ignored: 7 degrees, two octaves
after:  36 40 43 36 40 43 36    Length 3: C E G, repeating every 3 bangs
```

Example: `examples/misc/arpeggiator.orca`, whose first voice is that `;103`, and whose second voice takes its Step from a `C` (`;224` into `$4Ce`, Up-Down over two octaves of Major7, a period of 14), and the repro patch `tests/patches/arp_length_step.orca` (Length `z` at range 4, a total of 1 under Up-Down and Down-Up, Length `0`, and a Step from a clock).

#### `&` cycles in beats, counts its phase from tick 0 or the last bang, and keeps it across rate and shape changes

The bouncer's Rate was a step size: every tick moved `rate` steps through its 128-step waveform, so a cycle took 128 / rate ticks, 42.7 at rate `3`, which drifted against the bars. Its phase started wherever the cell first ran, a rate or shape change restarted it, and the output was scaled through `float`, which truncated. Now the Rate is the length of one cycle in beats, at 4 ticks to a beat, so a cycle is 4 × rate ticks (`1` is a beat, `4` one bar of 4/4) and always lands on the beat grid. The phase counts from tick 0, or from the last tick a neighbouring bang hit the `&`: a `*` restarts one; a rate or shape change keeps the anchor, the tick the cycle is counted from, so it stays in step with other `&`s (a shape change keeps the phase; a rate change re-takes the elapsed ticks mod the new cycle); every `&` with the same rate is in step; and an external MIDI start (`FA`) or song position (`F2`) sets every anchor back to tick 0. `.` and `0` hold the phase-0 output: Start for shapes 0, 2, 4 and 6, End for 1, 3, 5 and 7. The output is scaled in integer math, rounded half up instead of truncating. The waveform table itself is unchanged, quirks included. This changes the sound of every patch with an `&`.

Before → after, the output of each `&` on ticks 0–15 (`cli --events`):

```
&0z46   before: 123456789abcdefg    a 32-tick cycle, starting one step in
        after:  02468acegikmpsux    16 ticks, one bar, from phase 0
&3a22   before: 3333333344444566    a 64-tick cycle
        after:  3469a9643469a964    8 ticks, two beats
```

The nearest new rate for an old one is in [Coming from boorch/bOrca 4f349cd](#coming-from-boorchborca-4f349cd).

Example: `examples/misc/midiccbouncerdemo.orca`, whose `&0y10` became `&0yw0` (`w` is 32 beats, 128 ticks), so it keeps its 128-tick cycle (it now starts at phase 0 one tick earlier, and the rounded output holds 0 for 4 ticks instead of 8), and the repro patches `tests/patches/bouncer_rate4.orca` (every shape at rate `4`) and `tests/patches/bouncer_shapes.orca` (a bang, a clocked rate and a clocked shape).

#### CC glides step between ticks, and a held-back final step is sent

A `!` glide moved only when a tick ran, so it sent at most one step per tick, 8 a second at 120 BPM, and a sweep stepped audibly. The CC engine that v1.0.0 brought in ([CC glides continue from the last value sent](#cc-glides-continue-from-the-last-value-sent)) already worked in wall-clock time, but the loop asked it for steps only at ticks. Now the event loop also wakes when the engine's next step is due, so a glide sends the values it passes through as it reaches them, at most Glide Rate times a second on one control: 100 unless set to 50 or 200 under Clock & Timing (Ctrl+D), or with `glide_hz` in `orca.conf`. The engine also gets its budget, 800 CC messages a second in bursts of up to 80, shared by every CC it sends; glide steps wait for it. This breaks patches that rely on the old sound:

- Glides send many more, smaller steps. At 120 BPM a glide from 0 to 127 at `4` (32 ticks) sent 32 steps, 4, 8, 12 … 123, 127, one per tick; it now sends all 127 values, 1, 2, 3 … 127, about one every 31 ms, and reaches 127 16 ms before its end rather than at it. A fast glide is now limited by the glide rate rather than the tempo: a synth or a MIDI link that cannot take 100 CCs a second on one control may want a Glide Rate of 50.
- A bang whose target equals where a running glide has got to, while the step to it is still held back by the glide rate or the cap, now sends that target and then stops the glide. Before, it stopped the glide with nothing sent, so the control could stay one step short of where the glide had got to: after an instant 0, a glide to 64 at `w` and, 5 ms later, a glide to 1, the control stayed at 0, and now goes to 1 at once.
- The 800 a second cap is new. A step over it waits, and glides running together take turns. Instant CCs and glide targets always go out and count against it, so while instant CCs alone go past 800 a second, running glides send no steps and jump to their targets at their ends. Before, nothing capped them.
- A running glide's next step can only shorten the event loop's wait, never below 1 ms and never through the `--strict-timing` table. With no glide running, the loop reads no extra clock and makes no extra call. `tests/wakeups/ab.sh` checks the idle wakeup rate against `4f349cd`, and a glide's wakeups against the same patch without its `!`.

Before → after, the first second of `examples/misc/glide_rate.orca` at 120 BPM, CC 74 on channel 0, as the engine sends it when polled at each of its deadlines (a `|` marks each new tick; the loop rounds each wait up to whole milliseconds, so on the wire a step can fall up to 1 ms later):

```
before: 0 | | 64 | 32 | 79 | 40 | 83 | 42 | 84                                 8 CCs
after:  0 | 1 6 11 16 21 26 31 36 41 46 51 56 61 | 62 59 57 54 … | 32 36 40 …   89 CCs
```

Example: `examples/misc/glide_rate.orca`, a `!` at `y` whose target alternates 0 and 127 on every bang, so its glide never ends and steps at the glide rate; it is also the glide patch of the wakeup check.

### Changed

This changes what bOrca sends when beat clock is turned on, not how a patch plays.

#### Turning beat clock on while playing sends MIDI continue, and keeps the tick grid

bOrca can now follow an external MIDI clock (see [Following an external clock](README.md#following-an-external-clock)), and every clock byte it sends goes through one gate, which is closed while it follows. With that, turning Send MIDI Beat Clock on while playing on the internal clock sends `FB` (continue) instead of `FA` (start): the downstream device carries on from where it is, which is also where bOrca is, rather than restarting from its first beat. The tick grid is kept as well: the next frame keeps its due time, and the first `F8` comes on that frame, up to one frame after the `FB`; turning beat clock off, which still sends `FC`, keeps the grid too. Before, the toggle restarted the deadline, so the next frame came late or early by up to a frame. Nothing changes while paused, and nothing changes for a patch: no patch plays differently, so there is no example patch.

Before → after, the bytes of turning beat clock on while playing at 120 BPM, as `aseqdump` shows them:

```
before: Start          then Clock every 20.8 ms from a restarted frame
after:  Continue       then Clock every 20.8 ms from the frame already due
```

### Added

- **Dialects.** An operator dialect, bOrca (the default) or upstream Orca-c `9df9786`, chosen with `--dialect`, the `dialect` key in `orca.conf` or the main menu (Ctrl+D), so Orca-c patches play as written. See [Dialects](README.md#dialects).
- **Following an external MIDI clock.** Clock, start, continue, stop and song position, from an ALSA or a PortMidi input, with one gate for every clock byte bOrca sends; the HUD shows the measured tempo. See [Following an external clock](README.md#following-an-external-clock).
- **Tooltips from the simulation.** Each operator labels the ports it marks, so the HUD names the right input however close operators sit, and follows the dialect; in the upstream dialect `$` and `&` draw as unknown glyphs, bold red. See [The editor](README.md#the-editor).

Internal API:

- `Opstate_arp` (`opstate.h`) gains `Usz last_length`, the effective Length the counter last ran at, and `Opstate_entry` keeps its size. While Step is set, `;` does not look up the op-state store.
- `Opstate_bouncer` (`opstate.h`) is now `{ bool initialized; Usz anchor; }`, the tick of the last bang; `current_index`, `last_rate` and `last_shape` are gone, and `opstate_reset_anchors` sets every `&`'s anchor to 0.
- For builds that list the core sources themselves: add `transport.c`, the new core module (`transport.h`), the clock-follow machine.
- For code that uses `ccout.h` or `tick.h`: `Ccout_engine` gains `budget`, `refill_time` and `start_slot`, which `ccout_init` sets, and `ccout_poll` now starts from `start_slot`; `ccout_next_deadline` counts the budget, and a no-op `ccout_submit_glide` can now emit. `tick.h` gains `tick_poll_engine`, the poll that `tick_body`'s step 3 makes and that the TUI's loop makes between ticks whenever `ccout_next_deadline` has passed. A caller that polls only in `tick_body` keeps one step per tick.

Tests: the upstream dialect is compared byte for byte with upstream Orca-c's own `cli` on its examples, and `tests/wakeups/ab.sh` checks the event loop's wakeups against `4f349cd`'s.

## v1.0.0 (2026-10-10)

The first release of bOrca's line, on top of boorch/bOrca `4f349cd`.

### Breaking

Each of these changes how an existing patch plays or what it sends, or breaks the `orca_run` API.

#### Lowercase `r` never sends one value twice in a row, and takes its right input's case

With per-cell state (see [the `orca_run` entry](#orca_run-takes-a-context-with-a-caller-owned-operator-state-store-and---and-r-keep-their-state-per-cell)), each lowercase `r` keeps its own bag, but it could still send one value twice in a row, in two ways. A new bag, shuffled after the last value of the old one, could start with that value: `0r1` repeated about one bang in four. And a bag rebuilt after min or max changed could start with the value just sent. Each `r` now remembers the last value it sent, and whenever it shuffles a bag, a bag that would start with that value swaps its first slot with a random other slot. Every bag still holds each value from min to max once, max included. Only an `r` whose min equals max repeats, since it has one value, and a two-value `r` now strictly alternates: `0r1` gives 1010… forever, where before it did not. `r` also ignored its right input's case, so an uppercase max still gave lowercase letters; its output now takes the right input's case, as `R`'s does (`R` skips it when min equals max; `r` does not). This breaks patches that use `r`:

- `r` draws a new sequence from its first bag that would have started with the value just sent; until then, its values are the same as before. The test repro `0r3` at `--seed 0` changes from tick 28, its eighth bag, and the `0r9` beside it in `r_shared` is unchanged over its 40 ticks.
- An uppercase max now gives uppercase letters: `arC` into a note input played A#, B#, C# and now plays A, B, C. A digit or lowercase max still gives lowercase letters, and digits stay digits. An uppercase letter directly left of `r` is an operator itself, and most (`A`, `B`, `C`, `D` …) run first and take `r` as their input, so give `r` a lowercase or digit min. Like any uppercase letter, an uppercase max, and the uppercase letters `r` now writes, run as operators on every tick `r` is not banged; so bang `r` every tick, or feed its output to an input that locks it, such as `:`'s note.

The seeding is unchanged: each `r` still seeds its PCG32 from the run's seed and its cell, as the per-cell state change set it up, so its sequence follows `--seed` and is the same on every platform.

Before → after, `cli --events --seed 0`: ticks 24 to 39 of the `0r3` in `tests/patches/r_single.orca`, four bangs per bag, and the first ten bangs of the `crG` in `examples/misc/random_unique.orca`, which feeds the note input of `:13.f1`:

```
          before                  after
0r3       0213 3120 0213 1320     0213 2130 3201 2013
crG       c d g f e d e c f g     C D G F E D E C F G
```

Before, that `crG` played C#, D#, G# and so on, and repeated a note 3 times in 64 bangs; now it plays C, D, G and never repeats one.

Example: `examples/misc/random_unique.orca`, where `crg` and `crG` bang notes on channels 0 and 1 every tick, `crg` lowercase notes, which are sharps (`e` is E#, which is F), and `crG` the naturals C to G, and neither plays a note twice in a row; repro patches: `tests/patches/r_single.orca` and `tests/patches/r_shared.orca`.

#### `=` reads velocity like `:` and `%`, and a velocity worth 0 sends nothing

`=` mapped its velocity glyph as v × 127 / 35, while `:` and `%` use v × 8 − 1, at most 127, so one glyph played at two velocities: `f` was 54 on `=` and 119 on `:`. `=` also had no zero case: a velocity glyph worth 0 sent every note of the chord at velocity 0, which synths read as a note-off, and the real note-offs followed later. All three operators now share the mapping `:` and `%` always used, described in the README's [Notes and velocity](README.md#notes-and-velocity): `.` is 127, a glyph worth 0 sends nothing, and any other value v gives v × 8 − 1, at most 127. This breaks patches that use `=`:

- `=` plays louder for the same glyph: `1` went from 3 to 7, `7` from 25 to 55, and `f` from 54 to 119.
- `g` to `z` all give 127 on `=`; before, they rose from 58 to 127.
- `0`, or any other glyph worth 0 (`*`, `#`, `;` …), now sends nothing, where `=` used to send velocity-0 note-ons. Those cut off any of the chord's pitches still sounding on that channel; now those notes ring for their full length: `=13Caf8` at tick 0 then `=13Ca08` at tick 2 cut the chord at tick 2, and now it ends at tick 8.
- A patch that wants the old level of `=..0f` uses velocity `7` (55): `=03C0ff` goes from 54 to 119, and `=03C07f` plays at 55.

To keep an `=` near its old velocity, use the table in [Coming from boorch/bOrca 4f349cd](#coming-from-boorchborca-4f349cd).

`:` and `%` are unchanged: they already used this mapping, and a velocity worth 0 already sent nothing.

Before → after, the repro cells with `cli --events -t 1`:

```
           before               after
=13Caf1    36 40 43 at 54       36 40 43 at 119
=13C0f1    36 40 43 48 at 54    36 40 43 48 at 119
=13Ca01    36 40 43 at 0        nothing
```

Example: `examples/misc/velocity.orca`, which steps `:`, `%` and `=` together through the velocity glyphs `1`, `7`, `f`, `F`, `g` and `z`, at the same velocity on all three, beside an `=` at `0` that sends nothing; repro patch: `tests/patches/midichord_vel.orca`.

#### Uppercase `$` and `=` selectors play first inversions

`$` and `=` looked up their selector with a helper that folds case, so an uppercase selector, `A` to `Z`, played its lowercase chord in root position. The 26 first-inversion tables that uppercase was meant to reach were wrong anyway: Major's, 0 3 8, is C-D#-G# on C. Every scale and chord now lives once, with its name, in a new core module, `music.c`, which decodes the selector from its raw glyph: `0`-`9` are `$`'s scales and `=`'s enriched chords, `a`-`z` the chords in root position, and `A`-`Z` the first inversion of the same chord. The inversion is built from root position: the root moves up an octave, the notes stay in ascending order, and the note count does not change. This breaks patches that use an uppercase selector, whether typed or written by an operator that copies its right input's case (`C`, `R`, `I`, `A`, `B`, `M`, `L` or `Z` with an uppercase right input): a `1CZ` clock feeding `$3C.2`'s selector writes `A` at tick 10, where `$` printed `3G` and now prints `4C`.

- `$` outputs the inversion's notes. `$3CA2` (C major first inversion, degree 2) printed `3G` and now prints `4C`; `$3CA0` printed `3C` and now prints `3E`.
- `=` plays the inversion. `=13CAf1` played notes 36, 40, 43 (C-E-G) and now plays 40, 43, 48 (E-G-C). Major9, `k` (C-E-G-B-D), inverts to E-G-B-C-D, so `=13CK.1` plays 40, 43, 47, 48, 50.
- Near octave 9 the raised root can leave the range. `$` then writes nothing, and its previous output stays on the grid: `$9CA2` printed `9G` and now writes nothing. `=` drops the notes above 127: `=19aA.1` sent 118, 122, 125 and now sends 122, 125, dropping 130.

Digits, lowercase letters and every other glyph that the editor and the file loader accept decode as before: a glyph that is not `0`-`9`, `a`-`z` or `A`-`Z` is scale 0 for `$` and enriched chord 0 for `=`. The tooltips show the same names. The README's chord table, whose names and notes were wrong from `h` on, now matches the code, and the scale, chord and enriched-chord tables sit in one block, now under [Scales and chords](README.md#scales-and-chords).

Before → after, the repro cells with `cli --events -t 1`:

```
           before            after
$3CA2      3G                4C
$3CA0      3C                3E
=13CAf1    36 40 43          40 43 48
=13CK.1    36 40 43 47 50    40 43 47 48 50
```

Example: `examples/misc/chord_inversions.orca`, which plays every letter selector of `$` and `=`, each lowercase and then uppercase; repro patches: `tests/patches/scale_inv.orca` and `tests/patches/midichord_inv.orca`.

#### CC glides continue from the last value sent

Superseded in v2.0.0: glides now step between ticks (see [the v2.0.0 glide entry](#cc-glides-step-between-ticks-and-a-held-back-final-step-is-sent)). This entry describes glides as v1.0.0 sent them.

A `!` glide (any rate but `.`) started from 0 whenever no glide was running on its channel and control, so a glide that followed a finished one restarted from 0. Every CC now goes through one CC engine, which remembers the last value sent on each channel and control and starts each glide from there, or from where a running glide has got to. The engine is also the only place a rate becomes a length, and it de-duplicates glide steps. This breaks patches that rely on the old sound:

- A glide continues from the last value sent. After a glide to 64, a glide to 80 at `w` went 20, 40, 60, 80 and now goes 68, 72, 76, 80.
- Banging the same glide again (the same target and rate) while it runs no longer restarts it from where it is. A `!` banged on every tick at `w` toward 64, from a control at 0 (after an instant `!`), went 16, 28, 37, 44 … and reached 64 only after about 17 ticks; it now goes 16, 32, 48, 64 and stops. On a control with nothing sent yet, it now jumps to 64 once.
- A glide sends a step only when the value changes, and a target already reached is no longer sent again on every tick. A glide from 0 to 2 at rate `s` (28, 8 ticks) went 0, 1, 1, 1, 1, 2, 2, 2 and now goes 1, 2. A synth that missed the target gets it again only from an instant `!`.
- Rates `0` and `z` are instant, like `.`: their CC now goes out in the tick that bangs the operator, in order with that tick's other CCs. Before, it went out one tick later, ahead of that tick's events.
- A glide on a control that bOrca has sent nothing to jumps straight to its target. Before, it glided up from 0. To glide from a known value, send it first with an instant `!` (as the example patch does).
- An instant `!` on a control stops a glide running there. Before, the glide kept stepping over it.
- Pausing, opening a file and creating a new one stop every running glide; before, a paused glide carried on when playback resumed. Opening, creating a new file and changing the MIDI output (the PortMidi output menu, or restarting OSC under `--osc-midi-bidule`) also forget the last values sent, so the next glide on each control jumps to its target.
- A glide's length is fixed in time when it starts: 36 − rate ticks at the tempo of that moment. A tempo change while it runs no longer changes when it ends; before, it always took 36 − rate ticks at whatever tempo followed.
- A glide's steps follow the clock: each tick sends the value for the time it runs. A tick that runs late can send the target minus 1 and the target one tick later, and ticks that catch up after a stall send fewer steps. Before, each tick moved a glide one step, however late it ran.

The engine still steps glides only on ticks, as before, and at most 100 times per second on one control, a limit that tempos above about 1,500 BPM reach, and so can ticks catching up back to back after a stall. Nothing else about what goes out, or its order, changes: an instant CC from a `!` with rate `.` always goes out, even when it repeats the last value.

Before → after, one 12-tick period of `examples/misc/glide_continuity.orca` (CC 74 on channel 0: 0 at tick 0, a glide to 64 at `w` from tick 2, a glide to 80 at `w` from tick 7), as `aseqdump` shows it on the uConsole:

```
before: 0, 16, 32, 48, 64, 20, 40, 60, 80
after:  0, 16, 32, 48, 64, 68, 72, 76, 80
```

Since v2.0.0 ([CC glides step between ticks](#cc-glides-step-between-ticks-and-a-held-back-final-step-is-sent)), a glide sends the values it passes through as it reaches them, at most 100 a second at the default Glide Rate of 100, so one period is now 66 or 67 CCs: 0, then 1, 2, 3, 4, 6 … 64 (a glide to 64 at `w` wants 128 a second and gets 100), then 65, 66 … 80. What holds is the property: the period starts at 0, rises strictly, reaches 64 and then 80, the second glide continuing from 64.

Example: `examples/misc/glide_continuity.orca`; repro patch for the instant rates: `tests/patches/cc_rates.orca`.

#### With beat clock on, notes last their written length

While Send MIDI Beat Clock was on (Ctrl+D, then Clock & Timing, or `midi_beat_clock = 1` in `orca.conf`), every note of length 1 or more lasted six times its written length. A note's remaining time was kept in seconds, one tick per unit of length. Under beat clock the loop wakes six times per tick to send the clock pulses, but the tick itself, where notes age, runs on only one of those wakeups, and it aged notes by a sixth of a tick, so a note took six times as many ticks to run out. Now a note's remaining time is a count of ticks: it is set to the note's length when the note sounds or is retriggered, and every tick takes one off it, whatever the clock source. A note that sounds in tick T goes off in tick T + its length, and lengths 0 and 1 go off in the next tick, as they always did with beat clock off.

- This breaks patches made with beat clock on: their notes are now six times shorter than before, at their written length. Written lengths top out at `z`, 35 ticks, so a beat-clock note that relied on lasting longer than 35 ticks now needs a retrigger (a slower `D`) or chained notes.
- With beat clock off and a steady tempo, every note starts and stops on the same ticks as before, at any tempo below 15,000 BPM. Above that, notes used to end early, and now get their full length.
- A held note keeps its count of ticks across a tempo change. Before, its remaining time stayed in seconds, so a note held while the BPM changed lasted more or fewer ticks than written.
- The loop no longer wakes for a note-off deadline of its own, since note-offs only ever go out on a tick. If the sustained-note list cannot grow, the new notes it cannot hold are not sent, so none is left sounding without a note-off; before, bOrca crashed.

Nothing else about what goes out, or its order, changes.

Before → after, a length-4 note at 120 BPM with beat clock on (`Dz` on row 0, `:03C.4` two rows below, so it is banged only every 35 ticks), as `aseqdump` shows it on the uConsole:

```
before: Note on 36, 144 × Clock (3000 ms), Note off 36
after:  Note on 36,  24 × Clock  (500 ms), Note off 36
```

No example patch: beat clock is a setting, not part of a patch, so the `aseqdump` check in this entry shows the change, with any note of length 2 or more that is not banged again before it ends, and beat clock on. The unit tests `tick_note_length` and `tick_note_length_beat_clock` in `tests/unit/test_tick.c` check the lengths, the second with the shell's six clock pulses per tick modelled in the test.

#### `orca_run` takes a context with a caller-owned operator-state store, and `&`, `;` and `r` keep their state per cell

API break: `orca_run` keeps its seven arguments and takes an eighth, `Orca_run_ctx const *ctx` (`sim.h`). Its field `opstate` is required and points at an `Opstate_store` (`opstate.h`) that the caller sets up with `opstate_init`, keeps across ticks and releases with `opstate_free`. Every existing caller must change. Every other field defaults to its zero value: `dialect`, added later, is then bOrca's operator list (see [Dialects](README.md#dialects)), so the caller in this entry's example runs bOrca's operators, and `labels`, added with the tooltips (`portlabel.h`), is then NULL, so no port label is recorded, as `cli` runs. The store holds the per-cell state of `&`, `;` and lowercase `r`, which used to live in fixed-size arrays and one global shuffle inside `sim.c`; the arrays overflowed or gave up on large grids, and the state never reset on open.

What changes in patches:

- `&` and `;` work on any grid size. Before, an `&` at cell index y × width + x of 4096 or more read and wrote past its state array, and a `;` there output nothing.
- Lowercase `r` now draws a different sequence. Each `r` keeps its own bag, shuffled by a PCG32 seeded from the run's seed (`--seed`) and its cell, so it is the same on every platform, follows `--seed`, and no longer shares one shuffle with every other `r` in the patch, which broke each one's no-repeat cycle. It could still repeat a value across a reshuffle, until [Lowercase `r` never sends one value twice in a row](#lowercase-r-never-sends-one-value-twice-in-a-row-and-takes-its-right-inputs-case), in the same release, changed `r` again. The TUI's default seed is 1 and `cli`'s is 0, so run the TUI with `--seed 0` to hear the sequences in the test goldens.
- The state resets on file open, new file and Ctrl+R, and a resize keeps it for every cell still inside the grid. Before, only Ctrl+R reset anything, and only `r`'s shuffle. Undo never touches the state: undoing an open, a new file or a resize brings back the grid, but not the state its `&`, `;` and `r` had.
- Editing while paused no longer advances `&`, or a banged `;` or `r`: the redraw pass runs on a throwaway copy of the state (since `&` counts in beats, from v2.0.0, it has no phase to advance; a fresh `&` lines up with tick 0 instead).
- Switching a cell directly between two stateful operators starts the new one fresh: `&` → `;` → `&` restarts the bouncer's phase, which the old separate arrays kept (since `&` counts in beats, from v2.0.0, a fresh `&` lines up with tick 0 instead). Deleting an operator and typing it again, or passing through a stateless glyph (`&` → `A` → `&`), resumes its old state, as before.

Before → after, a caller:

```c
// before
orca_run(gbuf, mbuf, height, width, tick, &events, seed);

// after
Opstate_store store;
opstate_init(&store); // once, before the first tick
Orca_run_ctx const ctx = {.opstate = &store};
orca_run(gbuf, mbuf, height, width, tick, &events, seed, &ctx);
opstate_free(&store); // once, after the last tick
```

Before → after, the first 12 outputs of `tests/patches/r_single.orca` (one `0r3` banged every tick, `--seed 0`):

```
before: 0 2 1 3  0 2 1 3  3 2 0 1   (glibc rand(), one shuffle for every r)
after:  3 1 2 0  3 0 2 1  2 3 1 0   (this cell's own PCG32 stream)
```

No example patch: every patch under `examples/` plays as before; the repro patches are `tests/patches/r_single.orca` and `tests/patches/r_shared.orca`.

#### Lowercase `j` and `y` wires grow when banged

Upstream Orca-c `723638f` ("Allow wires to grow"), cherry-picked. `J` and `Y` now check whether the cell they read holds the literal `J`/`Y` instead of their own glyph, so a wire of lowercase `j` or `y` can lengthen itself from a bang rather than by hand: a banged lowercase `j` directly below another `j` appends a `j` at the end of the run, and a banged `y` directly right of another `y` appends a `y` at the end of the run. Before, such a bang was consumed and nothing else changed.

Before → after, banging the lower `j`:

```
.j..    .j..
.j*.    .j..
....    .j..
```

Before → after, banging the right-hand `y`:

```
.yy.    .yyy
..*.    ....
```

A third case changes by the same rule: a banged lowercase `j` whose input cell holds a locked `J` (inside a `#` comment, say) used to copy that `J` downward and now stays silent; `y` reading a locked `Y` likewise. Uppercase `J`/`Y` chains behave as before. Example: `examples/misc/wires.orca`; repro patches: `tests/patches/j_banged_under_j.orca`, `y_banged_right_of_y.orca`, `j_reads_locked_J.orca`.

### Changed

These change what bOrca sends on resume and on quit, not how a patch plays.

#### Resuming after a pause no longer re-sends the paused grid's notes and CCs

While paused, bOrca runs the patch once on a throwaway copy whenever the grid needs redrawing, to colour the operators and their ports. That pass wrote its events into a list that the next tick also sent, and every tick asks for a redraw, so the first tick after every resume sent them too, edit or not: each note played twice (note-on, note-off, note-on), each CC and pitch bend went out twice, and a `!` with an interpolation rate registered its glide a second time. Two changes fix it, so the first tick after resume sends only that tick's events:

- The redraw writes into its own event list, which is never sent. No unit test drives the TUI's redraw, so the manual `aseqdump` check in this entry covers this.
- The tick clears the glide engine's list before each engine run, so nothing left in it goes out. The unit test `tick_cap9_resume_after_edit` in `tests/unit/test_tick.c` covers this: it hands a tick, as its engine list, the list a preview filled, and compares what goes out with a run that had no preview.

Nothing else about what goes out, or its order, changes.

Before → after, the first tick after resuming a patch whose `D1` bangs `:03C.1`, as `aseqdump` shows it:

```
before: Note on 36, Note off 36, Note on 36
after:  Note on 36
```

No example patch: any patch with a note operator shows it.

#### Quitting while playing sends MIDI stop when beat clock is on, and `/orca/stopped` when OSC output is set

Quitting now stops playback first, as pausing does, so a device that follows bOrca's beat clock stops with it and an OSC listener hears that playback ended. Before, quitting only released the sustained notes. This applies to every build; in `--alsa` builds SIGTERM, SIGHUP and SIGINT quit the same way. Quitting while paused sends nothing new, because pausing already sent these messages.

Before → after, quitting while playing with beat clock on and OSC output set:

```
before: note-offs
after:  note-offs, OSC /orca/stopped, MIDI stop (FC)
```

No example patch: the change concerns quitting, not how a patch plays.

### Added

- **The `--alsa` backend.** A native ALSA sequencer client for Linux, named `bOrca`, and `make appliance`, the uConsole build. See [The ALSA client](README.md#the-alsa-client).
- **`cli --events` and `--seed`.** `cli` prints each tick's events and takes a seed, for the golden tests.
- **Generated tables.** The README's scale and chord tables are printed by `readme-gen` from the code and checked by CI, and the upstream Orca-c examples moved to `examples/upstream/`.

### Fixed

- **Per-cell state.** `&`, `;` and `r` keep their state per cell, on any grid size, with no overflow past cell 4096; see [the `orca_run` entry](#orca_run-takes-a-context-with-a-caller-owned-operator-state-store-and---and-r-keep-their-state-per-cell).

Internal API:

- `reset_last_unique_value()` is removed; `opstate_clear()` replaces it. A build that lists the core sources itself must add `opstate.c`. Clear the store when a patch is loaded, call `opstate_prune` on a resize, and run any preview pass on an `opstate_copy` of the store, never on the store itself.
- `Opstate_random` (`opstate.h`) gains two fields, `has_last` and `last_value`. They fit in the struct's padding, so its size does not change (64 bytes on the uConsole). A zeroed entry has no last value, so a new cell and `opstate_clear` still start an `r` fresh, and its first bag may then start with the value it sent before the clear.
- For builds that list the core sources themselves: add `music.c`, the new core module (`music.h`), from which `sim.c` and `tooltips.c` read their scales, chords and names. For embedders that pass `orca_run` other bytes: a selector byte of 0x80 or above now decodes like any other glyph, as scale 0 or enriched chord 0; before, its top bit was ignored, so 0xE1 played chord `a`.
- An API break: `sim.h` drops `process_interpolated_midi_cc_event`, `advance_midi_cc_interpolations`, `last_random_unique` and `midi_panic`. A build that lists the core sources itself must add `ccout.c`, the new engine (`ccout.h`). For code that uses `tick.h`: `Tick_ctx` gains `ccout`, the engine, which the caller sets up with `ccout_init` and keeps across ticks, and `now_us` and `tick_len`, the tick's time and one tick's length in microseconds. The caller drives the engine as the TUI does: `ccout_cancel` on pause, `ccout_cancel` then `ccout_forget` on open and new, `ccout_forget` when the output changes, and a `now_us` that never goes back.
- For code that uses `tick.h`: `Susnote.remaining` is a `U8` count of ticks; `Tick_ctx` loses `age_secs`, `frame_secs` and `next_note_off`, and `tick_release_all` its `next_note_off` argument; `susnote_list_add_notes` returns `false`, leaving the list unchanged, when the list cannot grow; `susnote_list_advance_time` becomes `susnote_list_advance_tick`, which ages every note by one tick; and `susnote_list_soonest_deadline` is removed.
- For builds that list the sources themselves: the tick and the sustained-note list moved out of `tui_main.c` and `osc_out.c` into the new `tick.c`, which joins the core sources, and `Susnote`, `Susnote_list` and the `susnote_list_*` functions moved from `osc_out.h` to `tick.h`.

Tests: the golden suite (`tests/run.sh`) with repro patches, the unit-test runner, the core include and `nm` checks, the README table and example checks, and CI.

## Coming from boorch/bOrca 4f349cd

A patch made for boorch/bOrca `4f349cd` plays differently in places. The Breaking entries of v1.0.0, v2.0.0 and v4.0.0 say what changed; this section says how to adapt such a patch.

**`&` rates.** A cycle is now 4 × rate ticks, where it was 128 / rate.

The nearest new rate for an old one is 32 / old rate beats, exact for `1`, `2`, `4`, `8`, `g` and `w`:

| Old rate | `1` | `2` | `3` | `4` | `5` | `6`–`7` | `8`–`9` | `a`–`c` | `d`–`l` | `m`–`z` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| New rate | `w` | `g` | `b` | `8` | `6` | `5` | `4` | `3` | `2` | `1` |

**`=` velocities.** `=` now reads its velocity as `:` and `%` do, so it plays louder for the same glyph.

To keep an `=` near its old velocity, replace its velocity glyph with the new glyph below it in this table, which gives the nearest velocity; every old velocity lands within 4:

```
old =  1-3 4-5 6-7 8-9 a-c d-e f-g h-i j-k l-n o-p q-r s-t u-v w-y z
new =  1   2   3   4   5   6   7   8   9   a   b   c   d   e   f   g
```

Ties (`c`, `n`, `y`) round down. `x` (119) maps exactly to the new `f`, and `z` and `.` stay 127. Where an operator writes the velocity (`R`, `T`, `C` …), rescale its range instead: the new velocity is about 8v and the old about 3.6v, so roughly halve it.

**`;` Length and Step.** The two cells right of a `;`'s Pattern are now its Length and Step, so move any operator or value out of them; with both empty, a `;` plays as before. Set Length to the number of notes of the `$` selector it feeds, so that an arpeggio fits its chord; see the README's [Arpeggiator](README.md#arpeggiator-).

**Beat clock note lengths.** With Send MIDI Beat Clock on, notes used to last six times their written length; they now last their written length. Written lengths top out at `z`, 35 ticks, so a beat-clock note that relied on lasting longer than 35 ticks now needs a retrigger (a slower `D`) or chained notes.

**Empty inputs on L, I, U, R, D, C and Z.** bOrca's dialect follows Orca's 2025 rules for these seven: an empty input counts as 0, and `R`'s max is included. Write the values out to keep a patch playing as before: `1C8`, `1D8`, `1I4`, `1U8` and `1Z5` for `.C.`, `.D.`, `.I4`, `.U8` and `.Z5`; for `R`, the larger input one lower (`1R3` for `1R4`, `3R1` for `4R1`), `z` for a max of `.` or `0` (`5Rz` for `5R.`, `0Rz` for `.R.`), and `aRa` for `aRA`. An `L` with an empty input, an `I` with a step and an empty or `0` max, and an input that another operator writes as `.` or `0` while playing have no written form. See v4.0.0.

**`--seed 0`.** `cli` defaults to seed 0 and `orca` to seed 1, so run `orca --seed 0` to hear the sequences of `R`, `r` and `;`'s Random pattern that `cli` and the golden tests show.

## Coming from Orca-c

Patches written for upstream Orca-c play as written in the upstream dialect: pick Upstream under Operator Dialect... in the main menu (Ctrl+D), or run `orca --dialect upstream`. In that dialect `!`, `;`, `=` and lowercase `r` are upstream's, and `$` and `&` do nothing. In bOrca's dialect `L`, `I`, `U`, `R`, `D`, `C` and `Z` follow Orca's 2025 rules instead, so an empty input counts as 0 there. See [Dialects](README.md#dialects).
