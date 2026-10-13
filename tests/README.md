# Tests

## Golden tests

`tests/run.sh <path-to-cli> [name-filter]` runs every patch under `examples/`
and `tests/patches/` through `cli --events` and compares the output byte for
byte with the expected file. Run it on both builds:

```sh
./tool build cli      && tests/run.sh build/cli
./tool build -d cli   && tests/run.sh build/debug/cli    # the debug build
```

On the uConsole prefix both with `nice -n 19 taskset -c 0-2`, so the live
engine on core 3 is left alone. The optional second argument runs only the
cases whose path contains it (`tests/run.sh build/cli midichord`).

Default flags are `-t 64 --seed 0 --dialect borca`. The line formats are the
ones `cli --events` prints (`t<tick> NOTE ch<c> note<n> vel<v> len<l>
mono<m>`, `CC`, `CCI`, `PB`, `OSC`, `UDP`, then `t<tick> GRID` and the grid
rows); see `events_print.c` and `cli -h`. The script exits 0 only when every
case is PASS or XFAIL and no expected file is orphaned.

### Layout

| Input | Expected output | Sidecars (same directory) |
| --- | --- | --- |
| `examples/<dir>/<name>.orca` (referenced in place, never copied) | `tests/expected/examples/<dir>/<name>.events` | `<name>.args`, `<name>.xfail` |
| `tests/patches/<name>.orca` (repro and feature patches) | `tests/expected/patches/<name>.events` | `<name>.args`, `<name>.xfail` |

`examples/upstream/<dir>/<name>.orca` follows the first row: its expected
files sit under `tests/expected/examples/upstream/<dir>/` (see
[Upstream examples](#upstream-examples)).

- `<name>.args` holds extra flags appended after the defaults; a later flag
  wins, so `-t 1` in a sidecar overrides the default `-t 64`.
- `<name>.xfail` names the one item tag whose fix the case waits for:
  exactly one id matching `^(B[1-8]|I[1-6])$` (`B2`). A repro that waits for
  two items is two cases, one per item, so each flips in the commit that
  lands its item. While the fix is missing the case reports XFAIL and the
  run stays green. Once the output matches, the case reports XPASS and the
  run fails: the commit that lands the item deletes the marker. The tags
  name the fork's original fix items: `B1` to `B8` shipped in v1.0.0 and
  `I1` to `I6` in v2.0.0; none is pending, so no marker is live.
- A marker that holds no id, anything but one such id (`TODO`, `B9`, `b2`)
  or two ids fails the case as `FAIL <case> (bad marker: …)`, in both modes;
  `--update` never rewrites that case's expected file, and the other cases
  still run.
- A marker never hides a crash. A sanitizer report in the output, a signal or
  the 60 s per-case timeout (applied when `timeout(1)` is on `PATH`) fails
  the run whatever the marker says, so the
  debug build must stay clean on every case. A plain non-zero exit (such as a
  rejected flag) counts as a mismatch and may be marked. This crash rule,
  with the sanitizer pattern, `UBSAN_OPTIONS` and the timeout, lives in
  `tests/lib.sh`, which `tests/check-examples.sh` sources too;
  `tests/upstream/compare.sh` takes only the pattern, `UBSAN_OPTIONS` and
  the timeout from it.
- An expected file or sidecar whose input no longer exists under `examples/`
  or `tests/patches/` is reported as ORPHAN and fails the run, so renamed or
  removed patches cannot leave stale goldens behind.

### Updating expected files

`tests/run.sh --update <cli>` rewrites every expected file from the current
output, except cases that carry a `.xfail` marker, valid or bad, whose
expected files describe the fixed behaviour and are written by hand. Use it
when a change is meant to alter output; the diff of `tests/expected/` is
then part of the review.

### Adding a repro patch

1. Put the patch in `tests/patches/<name>.orca`, and `-t <n>` in
   `tests/expected/patches/<name>.args` when 64 ticks are more than it needs.
2. Write `tests/expected/patches/<name>.events` by hand with the fixed
   behaviour (start from the current output and edit the lines the fix
   changes).
3. Put the one item id in `tests/expected/patches/<name>.xfail`. A repro
   that two items change is split so that each case waits for one.
4. Run the suite: the case must report XFAIL, not FAIL.

No marked repro remains. `scale_inv` (`$3CA2`, `$3CA0`) and `midichord_inv`
(`=13CA.1`) waited for uppercase selectors to play first inversions, and
`midichord_vel` (`=13Caf1`, `=13C0f1`, `=13Ca01`) for `=` to read velocity
like `:`, both in v1.0.0; each fix deleted the markers that waited for it, and
the hand-written goldens now pass unchanged. `midichord_inv` used velocity `.`
so that the velocity change could not change it; the inversion example
`=13CAf1`, which both fixes changed, is checked by the unit test
`music_midichord_inversion`, its notes and, since the velocity change, its
velocity of 119.
When these fixed goldens were written, a throwaway script derived them
from the current output and asserted each line it edited; the script is
not committed.

### Goldens that record current behaviour on purpose

`r_single` and `r_shared` record lowercase `r` as v1.0.0 left it. Each `r`
keeps its own bag in the op-state store and shuffles it with the PCG32 in
`prng.h`, seeded from `--seed`, its row and its column, so the goldens hold
the same stream on every platform, and `r_shared`'s `0r3` emits exactly
`r_single`'s sequence. A bag that would start with the value just sent
swaps its first slot with a random other slot, so neither golden sends a
value twice in a row. They were replaced, not marked expected-fail, twice,
both in v1.0.0: the per-cell state store replaced the glibc `rand()` stream
they held before, when it removed `rand()` from `sim.c` (core code never calls
`rand`), and the fix that stopped `r` repeating replaced them again; only the
`0r3` column changed, from tick 28, where its eighth bag started with the
value just sent. The unit tests `sim_r_*` in
`test_sim_state.c` prove no-repeat and permutation over every range from
`0r1` to `0rz`, and range change, seed, case, single value and first visit
on fixed patches.

Eight characterization goldens pin, unmarked, what the operators that the
v1.0.0 fixes touch produce today, so a refactor that changes their output
fails a case. Three were replaced, not marked, by the v2.0.0 change that
redefines their operator. `bouncer_shapes` was replaced that way when `&`
began to count in beats, and now records the anchored, tempo-synced `&`,
regenerated with `--update`. `;`'s Length and Step replaced `arp_patterns` and
the `;` rows of `seeded_random`, whose `;`s now sit in 6-column groups, so
that no `D` bangs the new Step input at x+4; with Length and Step empty, each
`;` outputs exactly the sequence
the old golden held. `seeded_random` keeps its width and its first four rows
byte for byte, because uppercase `R` hashes y × width + x, so that change
left its `R` outputs unchanged too. Its `R` cells were regenerated in v4.0.0
for the range of bOrca's dialect, which includes the max, with the hash
unchanged.

| Case | Pins | `.args` | Replaced by |
| --- | --- | --- | --- |
| `bouncer_shapes` | `&` shapes 0–7 at rate 5 over `0`–`z`, an end below the start, a partial range, rate `.`, shape `z`, a `D` bang every 7 ticks, and a rate and a shape that a `C` clock changes; a bang restarts the phase; rate and shape changes keep the anchor (a shape change keeps the phase, a rate change re-takes the elapsed ticks mod the new cycle) | none | none (replaced when `&` began to count in beats, v2.0.0) |
| `arp_patterns` | `;` ranges 1–4 with every non-random pattern, ranges `0` and `z`, a bang every 3 ticks, and a clocked pattern change that restarts the step, each `;` in a 6-column group with Length and Step empty | none | none (replaced with `;`'s Length and Step, v2.0.0) |
| `seeded_random` | `R` with a lowercase, an uppercase and a `0` max, and `;` pattern `c` with Length and Step empty, under a non-zero seed | `--seed 7` | none (its `;` rows replaced with `;`'s Length and Step, v2.0.0; its `R` cells regenerated for bOrca's dialect's `R` range, hash unchanged, v4.0.0) |
| `cc_instant` | `!` at rate `.` on CC 1 and CC 74 with values `0`, `g`, `v` and `w` (clamped to 127), sent again on every tick; a channel above 15 that sends nothing, a hundreds digit (CC 100), and controller 130 clamped to 127 | `-t 2` | none |
| `cc_rates` | the VM's classification of `!` at rates `0` and `z`: still `CCI`, with the rate index (0 and 35), as in 4f349cd, since only the CC engine maps a rate to a duration. `cli` runs no CC engine, so the unit test `tick_cci_same_tick` checks that both go out in the tick that bangs them | `-t 2` | none |
| `pitch_bend` | `?` with MSB and LSB at `0`, `z` and between, and a channel above 15 that sends nothing | `-t 2` | none |
| `scale_selectors` | `$` with every digit and lowercase selector at degrees 0, 2, 5 and 9 (with `$3Ca2` and `$3C02`), other roots, no octave, input octave `a` clamped to 9, and a result above octave 9 that writes nothing | `-t 1` | none |
| `chord_notes` | `=` with every digit and lowercase chord at velocity `.` and at `z`, an octave-9 chord that drops the notes above 127, input octave `a` clamped to 9, a channel above 15, and `:13Cf1` | `-t 1` | none |
| `note_same_tick` | the VM's two NOTE lines for one tick: `:03C.4` then `:03Ca6` (channel 0, note 36, velocity 127 then 79, length 4 then 6), both banged at tick 0 by a `D8`. Since v3.0.0 `tick.c` merges them on the wire, and `cli` never runs it, so the golden is identical before and after; the unit test `tick_note_same_batch` checks the wire: one note-on at 79 and one note-off 6 ticks later | `-t 1` | none |

Their patches avoid what the v1.0.0 fixes change on purpose: no lowercase `r`
operator (the per-cell state store and the no-repeat fix changed it; `r`
appears only as a `$` or `=` selector, which those operators lock), no
uppercase `$` or `=` selector (the first inversions changed them;
`examples/misc/chord_inversions.orca` and `tests/unit/test_music.c` cover
them), and no `=` velocity other than `.` or `z`, which gave 127 before and
after the velocity change. Every `&` and `;` cell keeps y × width + x below
4096: before the per-cell state store, `&` indexed its state with no bounds
check and `;` returned early from cell 4096 on. Each case was captured with
`--update`. When the cases
were written, a throwaway model of the operators' source checked each cell;
it is not committed.

`note_same_tick`, the table's last row, is the repro patch of the
one-note-per-tick change (v3.0.0), not a characterization of the v1.0.0 fixes,
and cannot catch a regression of that change: it pins the VM's two NOTE lines
for one tick, which the change leaves as they were, since it is in `tick.c`
and `cli` never runs `tick.c`. The unit test `tick_note_same_batch` in
`tests/unit/test_tick.c` checks the wire.

The `R` and `;` pattern `c` goldens assume a 64-bit `Usz`: their hashes
differ where `Usz` is 32-bit, as on armhf, whose CI job runs the unit tests
but never `cli`.

`j_banged_under_j` and `y_banged_right_of_y` were captured before the
upstream "Allow wires to grow" cherry-pick (v1.0.0) and replaced by it; together
with `j_reads_locked_J`, added with that pick, they now pin the grown-wire
behaviour.

`arp_length_step` pins `;`'s Length and Step (v2.0.0): Length `z` at range 4,
which counts `0` to `z` and wraps; a total of 1 under Up-Down and Down-Up,
and Length `0`, which output `0` on every bang; and a Step written by a
`1C3` into `;103` and `$3Ca`, banged every 2 ticks, which plays 36 43 40
36 43 where the counter would play 36 40 43 36 40, so a Step that is
ignored changes the golden. Captured with `--update`. The unit tests
`arp_*` in `tests/unit/test_arp.c` cover the rest: the 36 cap and the
total-1 guard on every pattern, range 1–4 and Length at every Step and over
150 counter bangs, Step as the counter's step, and a Step that never
touches the op-state store.

`bouncer_rate4` pins all eight shapes of the tempo-synced `&` at rate `4`
over `0`–`z`, which repeat every 16 ticks: ticks 0–15 are the rate-4 table in
`tests/unit/test_sim_state.c`, and ticks 16–63 repeat them. Captured with
`--update`.

The `&` state-overflow fixture is a unit test, not a golden
(`sim_bouncer_overflow_fixture` in `tests/unit/test_sim_state.c`): before
the per-cell state store (v1.0.0) its output differed between builds,
because the overflow read whatever
lay past the array. It builds the 300×80 grid in memory, runs 256 ticks
under the debug runner's ASan and UBSan, and checks that every `&` matches
a lone in-range reference `&0z12`; the one at cell 4096 is banged once, at
tick 5, from its own output cell, and matches a second reference banged at
the same tick.

### Upstream examples

The five patches in `examples/upstream/` (`basics/_osc`, `basics/_udp`,
`benchmarks/io`, `misc/udp+loop` and `setups/knobs`) are upstream Orca-c
examples, byte-identical to Orca-c `9df9786`'s and at its relative paths
(`examples/upstream/README.md`). They use upstream's OSC, UDP and CC
operators, which bOrca's dialect replaces, so they run in the upstream
dialect (`--dialect upstream`). Each has two files under
`tests/expected/examples/upstream/<dir>/`:

- `<name>.events`: Orca-c `9df9786`'s own `cli --events -t 96 --seed 0`
  output, 96 ticks long because the comparison with Orca-c runs 96 ticks;
- `<name>.args`: `-t 96 --dialect upstream`.

They pass: bOrca's upstream dialect prints exactly what Orca-c does. Each
golden holds event lines, which is the proof that every patch under
`examples/upstream/` does something in the upstream dialect
(`tests/check-examples.sh` covers the rest of `examples/` in bOrca's).
Until the dialects arrived (v2.0.0), `cli` rejected `--dialect upstream`
and each case carried a marker that waited for them; the change that added
the dialects deleted the markers.

`benchmarks/io` drives `:`, `!`, `;`, `=`, `?` and `$` from clocked tracks.
Its bOrca-dialect coverage stays in `tests/patches/io_borca.orca`, a copy of
the patch, whose `tests/expected/patches/io_borca.events` is the golden
`benchmarks/io` had before the move but for one cell: since `;` took a
Length input (v2.0.0) it reads x+3, which
the K rewrites every tick, as Length on each banged tick, so the `;` output
cell changed on those ticks while every event line stayed as it was.

The unit tests `dialect_*` in `tests/unit/test_dialect.c` check the rest of
the dialect on small grids: a context that names no dialect runs bOrca's
list; upstream `!` sends a plain CC from three inputs at value × 127 / 35;
upstream `r` is a banged `R`, cell for cell and tick for tick; upstream `;`
locks and sends at most 16 glyphs up to a `.`; upstream `=` sends its path
and values; both stop at the grid's east edge; upstream `$` and `&` write,
mark and keep nothing; the tick body runs `Tick_ctx`'s dialect, so upstream
`;` and `=` reach the sink's `osc` and upstream `!` goes out as a plain CC;
and only `borca` and `upstream` are dialect names.

The unit tests `rules_*` in `tests/unit/test_rules.c` check the seven
operators whose rules differ between the dialects, Orca's 2025 rules in
bOrca's and Orca-c's in upstream. `rules_lesser`, `rules_increment`,
`rules_uclid`, `rules_delay`, `rules_clock` and `rules_lerp` each run their
rows, such as `.L5`, `2D.`, or `.C.` over an output cell that holds `5`, on
row 1 of a 5×3 grid for ticks 0-15 at seed 0, in both dialects, and compare
the 16 glyphs of the output cell below the operator with what each dialect
must give; at tick 0 they also check that the two dialects mark the same
ports. `rules_random_range` checks `R`'s range in both dialects by the set
of values it outputs over 400 ticks at seeds 0 and 7: `1R4` gives 1 to 4 in
bOrca's dialect and 1 to 3 upstream, `.R.` always `0` in bOrca's and 0 to z
upstream, and `aRA` gives `A` in bOrca's and `a` upstream; and `4R1` outputs
what `1R4` does on every tick. `rules_random_hash` checks that both dialects
draw from one hash: bOrca's `xRy` outputs what the upstream `xR(y+1)` does
on every tick, and bOrca's `0Rz` what the upstream `.R.` does. The `R` tests
compare value sets and the two dialects, never a sequence, so they hold
where `Usz` is 32-bit. Rows that start from a value, `.I9` over `5` and
`.Z5` over `3`, check that an empty step or rate leaves it where it is; rows
whose left input is a `*` that a `D` above writes check that bOrca's dialect
counts it as 0 and upstream as 1; and lowercase rows check that `c`, `d`,
`i`, `l`, `u` and `z` do nothing unbanged and run the same body when banged.
A failing row prints its input, its dialect, and the glyphs it got and
wanted, before its `FAIL` line.

The unit tests `labels_*` and `tooltip_place` in `tests/unit/test_labels.c`
check the port labels and the tooltips: what each `PORT`
records in the label buffer (`portlabel.h`), the id, whether the port
counted as an input, and the owner's offset; the recording rule (an input
beats an output, the later port of the same kind wins, `I`'s and `Z`'s
read-write cell counts as an output); the moved ports of `G`, `K`, `O`,
`P`, `Q`, `T` and `X`; a `J` 256 rows from its output; one fixture per
operator body in its dialect, with the buffer holding exactly its ports;
every fixture in both dialects, with a record exactly where the mark has
an input or an output and the marks, grid and events identical with and
without a buffer; the upstream labels of `!` (Value × 127 / 35), `;`, `=`,
`$` and `&`; the selector, value and rate enhancements, "instant" for rates
`0` and `z` among them; the lookup's size check (a 10 × 20 buffer against a
30 × 20 and a 20 × 10 field shows nothing); the placement of a line on a
HUD row over widths 0-200, lengths 0-40 and every limit; and the tick path,
`tick_run_vm` clearing the buffer and `tick_body` passing it on.

### The upstream comparison

`tests/upstream/` holds an Orca-c build, which made the goldens above, and
the check that compares bOrca's upstream dialect with it:

- `orca-c-cli.patch` adds `--events` and `--seed` to Orca-c's `cli_main.c`,
  with the output and parsing of bOrca's `cli`, and `events_print.c` to the
  `cli` sources in Orca-c's `tool`;
- `build-cli.sh` gets Orca-c at `9df9786e2ad3c01955cdf4cdd5ae1fffad8fa5cc`
  with `git archive`: from this repository when it has the commit (as when
  the `upstream` remote, hundredrabbits/Orca-c, is fetched), or else from a
  depth-1 fetch of that commit by URL from
  `https://github.com/hundredrabbits/Orca-c.git` into
  `build/upstream/orca-c.git`, which later runs reuse. It extracts the tree
  to `build/upstream/orca-c/`, copies this repository's `events_print.c` and
  `events_print.h` into it, so both `cli`s print through one file, applies
  the patch with `patch -p1`, and builds `build/upstream/cli` with Orca-c's
  own `tool`. It needs git, tar, patch and a C compiler.
- `compare.sh <bOrca cli>` needs that build, and checks, reporting each
  failure and carrying on:
  1. the tree's `events_print.c` and `events_print.h` are this repository's;
  2. every `examples/**/*.orca` of the Orca-c tree, 43 files: Orca-c's
     `cli --events -t 96 --seed 0` and bOrca's, with `--dialect upstream`
     added, print byte-identical events and grids, and both exit 0 with no
     sanitizer report. Fewer than 43 files fails. Then the same for its
     extras, files of this repository that are not Orca-c's examples:
     `tests/patches/op_selftest.orca`, [the operator
     self-test](#the-operator-self-test). They are counted apart from the
     43; a missing extra fails, and so does a list with none;
  3. each `examples/upstream/<rel>.orca` is Orca-c's `examples/<rel>.orca`,
     and its golden is what Orca-c's `cli` prints for it, so neither can
     drift from Orca-c, and holds at least one event line;
  4. its list of the six exceptions is exactly the paths of the list in
     `tests/check-examples.sh`, so the two change together, and each is
     Orca-c's file at the same path.

  It exits 0 only when every check passes, 1 otherwise, and 2 on usage or
  when the Orca-c build is missing. CI runs it in both `build` legs, on the
  debug and the release `cli`, and proves it can fail with three canaries:
  the release `cli` with `--dialect borca` appended, an upstream golden with
  a byte appended, and a fake `cli` that, for `examples/basics/_midi.orca`,
  prints an AddressSanitizer report on stderr or on stdout
  (`bOrca cli sanitizer report`) or exits 3 (`bOrca cli exited 3`). To
  remake a golden, write the Orca-c `cli`'s output over it
  (`build/upstream/cli --events -t 96 --seed 0
  examples/upstream/<rel>.orca >tests/expected/examples/upstream/<rel>.events`).

```text
$ nice -n 19 taskset -c 0-2 tests/upstream/build-cli.sh
…
Built …/build/upstream/cli
$ nice -n 19 taskset -c 0-2 tests/upstream/compare.sh build/cli
PASS  events_print.c is the Orca-c build's copy
…
PASS  examples/basics/_midi.orca
…
upstream check: 43 examples, 43 identical; extras: 1, 1 identical; 5 goldens, 5 match; 13 files, 13 match; 0 fail
```

### The operator self-test

`tests/patches/op_selftest.orca` is the operator self-test that neauoire,
Orca's author, posted to the lines forum on 2025-11-12, in
[post #2283](https://llllllll.co/t/orca-livecoding-tool/17689/2283): "it
might be useful to anyone else who wants to test parity". The file is the
post's patch, byte for byte: 21 rows of 68 glyphs, 1449 bytes, sha256
`6aeb49b31c6a77fba3195a241ba9b040b64d7941a38238dca07a7e33d409b066`. It
ships with this credit and link; the post states no licence.

Each block puts an operator with its inputs in one row and its result in
the row below. The result is left of an `F` and the expected glyph right of
it, and the `F` writes `*` under itself when the two match (two empty cells
match too): `2A9` on row 2 writes `b` below its `A`, and the `bFb` there
bangs on row 4. Rows count from 0. Row 0 sets the variables `a` and `0`
that the `V` and `K` blocks of row 2 read. Row 2's `.Vb` reads `b`, which
row 19's `bV5` writes only later in the same frame, and variables do not
carry over between frames, so it reads empty. The four `F`s on row 10 are
operators under test, checked by the `F`s below them, which leaves 65
comparators. The `H`s on row 10 hold the `N`, `S`, `E`, `W` and `*` below
them in place for the `F` beside each. In rows 13 to 16, `Q`, `G`, `T`,
`P`, `O` and `X`, and the `J` chain, write their result where their ports
put it, with an `F` beside that cell. Row 19 tests `Y`, with its bangs on
row 20.

Its golden, `tests/expected/patches/op_selftest.events`, runs `run.sh`'s
default 64 ticks at seed 0 and has no `.args` file. 64 ticks cover every
8-tick cycle, and, by Orca-c's rules, `.I.`'s first wrap at tick 35. The
patch sends nothing, so the golden holds grids and no event line.
[The upstream comparison](#the-upstream-comparison) also runs it, for 96
ticks, through Orca-c's `cli` and bOrca's `--dialect upstream`, which print
the same bytes. In bOrca's dialect its three `R` cells, `.R.`, `zRz` and
`2R2`, have equal or empty inputs and hash nothing, so the golden holds
whatever `Usz`'s width.

In bOrca's dialect, which follows the 2025 rules, all 65 comparators bang
on every tick. In the upstream dialect and in Orca-c, 55 do; these 10 do
not, counted with `cli --events --dialect upstream -t 64 --seed 0`:

| Cell | Expects | Upstream dialect and Orca-c | Bangs (of 64 ticks, seed 0) | Why |
| --- | --- | --- | --- | --- |
| `.L.` | `0` | `.` | 0 | an empty input makes `L` write `.` |
| `2L.` | `0` | `.` | 0 | the same |
| `.L9` | `0` | `.` | 0 | the same |
| `.I.` | `0` | 1, 2, … z, 0 | 1 (tick 35) | an empty step is 1 and an empty mod is 36 |
| `.U.` | `.` | a bang every 8 ticks | 56 | an empty step is 1 and an empty max is 8 |
| `1U.` | `*` | a bang every 8 ticks | 8 | an empty max is 8 |
| `.U1` | `.` | a bang every tick | 0 | an empty step is 1 |
| `.R.` | `0` | a random 0 to z | 1 (tick 7) | an empty max is 36, exclusive |
| `.D.` | `*` | a bang every 8 ticks | 8 | an empty mod is 8 |
| `.C.` | `0` | 0 to 7 | 8 | an empty mod is 8 |

The post's expected glyphs follow the operator rules that the JS Orca
adopted on 2025-11-16, in hundredrabbits/Orca commits `6079f10` to
`223e45c`. bOrca's dialect follows them from v4.0.0, and where the JS Orca
and its author's playground on the Orca wiki page differ, on `C` with an
empty modulo and `D` with a modulo of 1, it follows the playground. The
upstream dialect and Orca-c keep the older rules, which the JS Orca also had
before then. The same commits made `R` inclusive at both ends for every
range, where this patch tests only `.R.` and equal inputs, and dropped `Z`'s
default rate, which it does not test.

The playground's ROM, `orca.rom` dated 16 May 2026, is 10596 bytes with
sha256 `7a6ed30117af9c7af48adda086e9e6e20793854aa1d4a48329ae3f956bc4a44e`.
To re-check it, fetch https://wiki.xxiivv.com/site/orca, take the `#r=`
fragment of the playground's iframe, decode it from URL-safe base64,
decompress it (ULZ) and compare the sha256: a different hash means the
playground has changed, and its rules need reading again.

The self-test was tried against changes to `I`, `U`, `D`, `C`, `L`, `F`,
`B`, `M` and `A`, and the rest of the suite already failed on each, so for
those operators it adds no new coverage. It is a record of parity in the
author's own terms, and a compact regression file whose failing diff points
at the block that broke.

## README tables and examples

### readme-gen and doc-sync

`tools/readme-gen.c` prints the Markdown between `README.md`'s
`<!-- tables:begin -->` and `<!-- tables:end -->` markers: the scale, chord
and enriched-chord tables, with their headings and header rows and one blank
line between tables. It reads `music.h` only through
`music_selector_name` and `music_decode` and copies no table, so the README
cannot drift from what `$` and `=` play. Note names come from one spelling
map per table: C Db D Eb E F Gb G G# A Bb B for 0 to 11, continuing an
octave up. The maps differ at 15, which is D# in the chord table (Dom7#9) and
Eb in the enriched table (Minor7+Oct3rd). A semitone outside its map, a
selector with no name or a failed write exits 1. `./tool build readme-gen`
builds CORE plus that file into `build/readme-gen` (`build/debug/readme-gen`
with `-d`).

`tests/check-doc-sync.sh <readme-gen> [README]` (the README defaults to the
repository's) requires exactly one line of each marker, each alone on its
line and begin first, and rejects a readme-gen run that fails or prints
nothing. It then `diff -u`s the lines between the markers (`-`) with
readme-gen's output (`+`), and exits 0 only when they are identical; 1 for a
difference or any of the failures above, 2 on usage. After a change to the
tables in `music.c`, replace the lines between the markers with
`build/readme-gen`'s output.

```sh
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build readme-gen
tests/check-doc-sync.sh build/readme-gen
```

### Non-empty check of the examples

`tests/check-examples.sh <cli>` runs every `examples/**/*.orca` outside
`examples/upstream/` through `cli --events -t 64 --seed 0`, and requires each
to do something in bOrca's dialect:

- a file that contains an event operator glyph (`!`, `%`, `:`, `=` or `?`,
  anywhere in the file) passes on at least one event line;
- any other file passes on at least one tick whose grid differs from the
  file as loaded, which is the grid `cli -t 0 <file>` prints (it adds the
  final newline that many example files lack).

Six named exceptions fail that rule on purpose. The script lists each with
its reason, reports it as EXCEPT, and they stay byte-identical to upstream
Orca-c's examples, which `tests/upstream/compare.sh` checks:

| Exception | Why it fails the rule |
| --- | --- |
| `examples/basics/a.orca` | its `A` outputs already hold their sums, so no tick changes the grid |
| `examples/basics/k.orca` | its `K` outputs already hold the variables, so no tick changes the grid |
| `examples/basics/l.orca` | its `L` outputs already hold the lesser inputs, so no tick changes the grid |
| `examples/misc/multiplication.orca` | its `K` and `O` outputs already hold their values, so no tick changes the grid |
| `examples/misc/colors.orca` | its `:` is never banged (and is a locked `$` input), so it sends nothing |
| `examples/setups/sequencer.orca` | an empty template: its `:` are banged every tick, but every note input is empty, so it sends nothing |

Any other failing file fails the run. So does a listed exception that
passes, or that names a missing file or one under `examples/upstream/`, so
the list cannot go stale. A non-zero `cli` exit, a sanitizer report, a
signal or the 60 s per-run timeout (applied when `timeout(1)` is on `PATH`)
fails the file, exception or not, as does output that is not 64 ticks of
event lines and grids. The sanitizer, signal and timeout part is the golden
suite's crash rule, from `tests/lib.sh`; the non-zero exit is this script's
own addition. The script exits 0 only when every file passes or is a listed
exception. An example run, with the examples as they stood in v1.0.0:

```text
$ nice -n 19 taskset -c 0-2 tests/check-examples.sh build/debug/cli
PASS   examples/basics/_midi.orca (26 event lines)
EXCEPT examples/basics/a.orca (no grid change: its A outputs already hold their sums, so no tick changes the grid)
…
examples check: 44 files, 38 pass, 6 exceptions, 0 fail
```

## Unit tests and core checks

### Running the unit tests

`./tool build test` builds `build/unit_tests` (`build/debug/unit_tests` with
`-d`) from the CORE list, `field.c`, `sysmisc.c`, `thirdparty/oso.c` and
every `tests/unit/*.c`, against libc only. `field.c`, `sysmisc.c` and
`thirdparty/oso.c` are shell code outside CORE, linked so `field.c`'s
failure contract runs under the realloc hook and `sysmisc.c`'s
`conf_read_line` has a unit test; they join no `nm` or include
check. For `sysmisc.c`, which includes `"oso.h"` and calls `popen`, `tool`
gives `test` `-isystem thirdparty` and, except on mac and bsd,
`-D_POSIX_C_SOURCE=200809L`, as it gives `orca`. `tool` compiles the runner
in one command, so every file in it, CORE included, compiles with the
define; CORE's strict `-std=c99` check is the `cli` build's and the armhf
loop's, which gives the two flags to `sysmisc.c` alone. The runner prints
`ok <name>` for each passing test and
`FAIL <name>: <file>:<line>: <expr>` for each failed check, then
`<n> tests, <m> failed`, and exits 0 only when no test failed. A failed check
does not stop the run, but a crash, such as an ASan abort, ends it with a
non-zero exit status and no summary line.

```sh
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build -d test
UBSAN_OPTIONS=halt_on_error=1 nice -n 19 taskset -c 0-2 build/debug/unit_tests
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build test
nice -n 19 taskset -c 0-2 build/unit_tests
```

The debug runner is built with ASan and UBSan. UBSan on its own prints a
report and carries on, so the runner defines `__ubsan_default_options()` to
return `halt_on_error=1:print_stacktrace=1`, and a report fails the run even
without `UBSAN_OPTIONS`. CI and the commands above still set
`UBSAN_OPTIONS=halt_on_error=1` explicitly; `UBSAN_OPTIONS` overrides the
runner's defaults.

`tool` defines the CORE list once (today `gbuffer.c vmio.c sim.c
opstate.c ccout.c music.c transport.c tick.c tooltips.c tui_prefs.c`).
`tick.c`, `tooltips.c` and `tui_prefs.c` are shell code held to the core
rules, the testable shell: `tick.c` is the tick
body, the sustained-note list and the shell's pause, stop and patch-forget
sequences (`tick_pause`, `tick_stop`, `tick_forget_patch`), which the
unit tests drive through a recording sink that logs every call in order
(`tests/unit/recorder.h`, shared by `test_tick.c` and `test_transport.c`
with its two-note fixture `seed_notes`),
`tooltips.c` the tooltip text and placement, which reads the
label buffer the VM fills (`portlabel.h`) and formats its integers itself,
with no `<stdio.h>`, and `tui_prefs.c` the `orca.conf` settings:
the key table and its order, the reading of one conf line into a
value and a touched bit, the flags over the conf, the menus' setters with
their same-value no-op, and the text saved after each `key = `, while
`tui_main.c` keeps the file, curses, the menus and every engine call.
`cli`, `orca` and `test` link all of it, and
`./tool sources <core|cli|orca|test>` prints a list, one file per line, so
the checks below and the CI `armhf` job read the same lists.
`midi_in.h`'s static inlines are shell code under test too, outside the
CORE list: `test_transport.c` covers `rearm_compute` and the HUD text, and
also `Clock_out` (the clock-out gate, span, start, F8 gate and re-arms)
on that recording sink, which snapshots the `Clock_out` it watches at
each byte.
`tests/unit/test_sysmisc.c` covers `sysmisc.c`'s `conf_read_line`, a shell
file linked into the runner outside CORE: a last line that is not
`key = value` (a comment, say) and has no newline is read whole, and a save
through an `Ezconf_w` over `tmpfile()`s writes it back with a newline; only
a final `'\n'` is cleared (a CRLF line keeps its `'\r'`); a NUL ends a
line's text, so the bytes after it are lost, as before v3.0.2, and a line
whose first byte is NUL reads as empty, with no write before the buffer,
which only the debug runner's ASan sees; and an unterminated last line one
byte short of the buffer is still too long for it, where one two bytes
short reads whole.
`tests/unit/test_tui_prefs.c` covers `tui_prefs.c` on settings built on the
stack, one row per key's parse and saved text, the key order, the flags
over the conf and the setters' no-op; what it cannot reach,
`ged_set_dialect`'s clear, cancel and re-mark and the file itself, a smoke
run of the TUI under tmux checked when the module was split out (v3.0.0).

Adapter `FEAT_` flags apply to `test` as they do to `orca`: `--portmidi`
builds the PortMidi adapter tests, `--alsa` builds the ALSA adapter tests,
and `--no-mouse`, which has no adapter, is ignored. A build that sets a flag
but registers no test for it fails at run time with a message naming the
flag. What `orca.conf` does per backend (the input key each reads, External
refused without a reader) is no adapter test: `tests/unit/test_tui_prefs.c`
builds each backend's shape through `tui_prefs_init`'s reader flag and key,
so every runner runs those rows. The adapter lists in `tests/unit/tests.h`
today:

| Flag | List | Tests |
| --- | --- | --- |
| `--portmidi` (`FEAT_PORTMIDI`) | `PORTMIDI_TESTS_ALL` | `portmidi_error_text_and_filters`: `Pm_GetErrorText` returns text; the clock, play and song-position filter bits are set and disjoint. `portmidi_in_filter`: PortMidi 217's filter logic over `BORCA_PM_IN_DROP` passes F8, FA, FB, FC and F2 and drops notes, controls and realtime noise. `portmidi_in_decode`: the reader's `pm_in_decode` reads status, Data1 and Data2 in that order (F2 with Data1 16 is song position 16), and F2(16), FB and six F8s on a reset machine give tick 17 |
| `--alsa` (`FEAT_ALSA`, Linux only) | `ALSA_TESTS_ALL` | `alsa_version_and_open_modes`: `snd_asoundlib_version()` is non-empty; `SND_SEQ_OPEN_DUPLEX` is `SND_SEQ_OPEN_OUTPUT \| SND_SEQ_OPEN_INPUT`; `SND_SEQ_NONBLOCK` is non-zero. `alsa_in_mapping`: sequencer events map to transport events, SONGPOS clamped. `alsa_in_announce`: the stored source is cleared on its CLIENT_EXIT, PORT_EXIT or PORT_UNSUBSCRIBED and re-matched on PORT_START only while none is stored. `alsa_in_split`: `client:port` splits at the last colon, both sides non-empty. `alsa_name_scan`: the client-name scan is free only when its list ends on `-ENOENT`, taken on another client of the name, and returns any other error. `alsa_client_name_fits`: a client name fits at 63 bytes and not at 64 or 200; the empty string fits |

The PortMidi build needs `libportmidi-dev`, so it runs in CI only, not on
the uConsole. The ALSA build needs `libasound2-dev`, which the uConsole has;
its test opens no sequencer, so it also runs where `/dev/snd/seq` is
missing, as on the CI runners:

```sh
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build -d --alsa test
UBSAN_OPTIONS=halt_on_error=1 nice -n 19 taskset -c 0-2 build/debug/unit_tests
```

`-DUNIT_TESTS_CANARY` (through `CFLAGS_EXTRA`) adds
`unit_check_canary`, a test whose check is false; CI uses it to prove the
runner reports a failure and exits 1. `-DUNIT_TESTS_UNREGISTERED_CANARY`
defines `test_unregistered_canary` with its own prototype and in no list,
so it builds but never runs; CI uses it to prove that
`tests/check-debug-build.sh` names it. `./tool build test` stops with an
error if `tests/unit/main.c`, the runner, is missing.

### The realloc hook

On Linux with gcc or clang, `tool` links the `test` target with
`-Wl,--wrap=realloc -fno-builtin-realloc -DUNIT_TESTS_REALLOC_HOOK`, so
every `realloc` call in the runner, CORE's included, reaches
`__wrap_realloc` in `tests/unit/alloc_hook.c`. A test arms the hook
by setting `unit_realloc_fail` just before the call under test and disarms
it right after, before any `CHECK`; while it is set every `realloc` returns
NULL and touches nothing. `unit_realloc_calls` counts every call, armed or
not: a test copies it before its window and checks that exactly one call
ran inside, which proves no other realloc entered the window (malloc and
calloc are not wrapped; a second realloc shows as a failure, not as a
silent second NULL). `main.c` disarms the hook and zeroes the counter
before and after each test, so a test that returns armed cannot fail
another test's `realloc`.

The three flags travel together. The define without the wrap fails the
link on `__real_realloc`. Without `-fno-builtin-realloc`, any optimised
build (the `-Og` debug runner or the `-O2 -flto` release runner) may fold a
`realloc` whose pointer the compiler proves NULL into `malloc`, past the
wrap, so the hook never sees it. On the uConsole's gcc 12.2 and binutils
2.40, a probe program, which was not committed and arms the hook around
`realloc(b, 64)` with `b = NULL` in a
local, counts 0 calls at `-Og`, `-O2` and `-O2 -flto`, and 1 call at `-O0`
or with the flag. The hook test `realloc_hook_builtin_null` makes the same
call, `realloc(NULL, 64)`, armed, and fails unless the hook saw it and
returned NULL, so it is what proves the flag is present: with the flag
dropped from `tool`'s line it fails in both the debug and the release
runner. `tick_note_alloc_fail` cannot prove it, because each of its armed
`realloc` calls reads the list's pointer from memory, which gcc 12.2 does
not prove NULL, so it passes with or without the flag. Only the link
routes `realloc` to the hook, so LTO sees no reader of the armed flag and
could delete the arming store before the disarm; `volatile` keeps it (and
`-fno-builtin-realloc` already makes `realloc` opaque, so `volatile` is a
second safeguard). clang's LTO link is unprobed: CI's `clang` job builds
the debug runner only.

Hook tests sit in `HOOK_TESTS_ALL` in `tests/unit/tests.h`, which
`HOOK_TESTS` runs only under the define. Elsewhere (macOS, tcc, any
non-Linux link) the runner has those tests fewer, so CI's `build`,
`unsigned-char`, `clang` and `armhf` jobs each grep their runner log for
`ok tick_note_alloc_fail`, the `build` and `unsigned-char` jobs also for
`ok realloc_hook_builtin_null`, and a `tool` that lost the flags on Linux
cannot drop the tests silently. Today there are five hook tests.
`realloc_hook_builtin_null`, in `tests/unit/alloc_hook.c`, proves the
flags (above). `opstate_copy_alloc_fail`, in `tests/unit/test_opstate.c`,
fails the `realloc` of an `opstate_copy` whose destination must grow: the
copy returns false and leaves the destination empty (the store itself
grows by `calloc`, which is not wrapped). `tick_note_alloc_fail`, in
`tests/unit/test_tick.c`: the sustained-note list's `realloc` fails
under an empty list through `tick_body` (nothing goes out, the list stays
NULL/0/0), under a direct `susnote_list_add_notes` that must grow (false,
and buffer, count, capacity, every slot and the out-params unchanged) and
under a retrigger through `tick_body` (the seeds keep their entries, aged
by one; neither the retriggered seed's off nor any on goes out), then
recovers on the same list. It pins `tick.c`'s batch skip: a batch whose
list cannot grow sends none of its note-offs or note-ons, so no note
sounds without an entry to release it. `oevent_list_alloc_fail`, in
`tests/unit/test_core.c`: an event list whose growth fails, empty
or full at 16, returns its discard slot, so the event is dropped, and
keeps its buffer, count, capacity and items; the same call unarmed grows
the list to 32; and an `oevent_list_copy` whose destination
must grow returns false and leaves the destination empty with its buffer
and capacity. `field_alloc_fail`, in `tests/unit/test_field.c`:
a failed `field_resize_raw` or `field_copy` leaves the field's buffer,
size and contents as they were, a resize to 0 x 0 frees the buffer with
no `realloc`, and a mark buffer that cannot grow keeps its buffer and
capacity, while a size that fits needs no `realloc`; a load whose first
row cannot grow returns `Field_load_error_no_memory` with its file closed
and the field unchanged.

### Adding a unit test

1. Write `void test_<name>(void)` in a top-level `tests/unit/*.c` file that
   includes `tests.h`; `tool` picks up every such file, but not files in
   subdirectories. Include core headers by relative path (`"../../sim.h"`),
   so no build needs `-I.`. A test that drives a `Tick_sink` includes
   `recorder.h` (which includes `tests.h`) and records through its
   `rec_sink`, so a new sink callback is recorded in one place.
2. Check with `CHECK(expr)`, which records the failure and lets the test
   continue. Never use `assert`: release builds define `NDEBUG`, which
   removes it.
3. In `tests/unit/tests.h`, add `X(<name>)` to `CORE_TESTS`, to
   `HOOK_TESTS_ALL` if it arms the realloc hook, or to the full list for its
   adapter flag (`PORTMIDI_TESTS_ALL`, `ALSA_TESTS_ALL`).
   `tests.h` generates every test's prototype from these lists, outside any
   `#ifdef`, and makes `-Wmissing-prototypes` an error, so a `test_<name>`
   that no list names fails to build instead of never running. A test that
   declares its own prototype gets past that, and
   `tests/check-debug-build.sh` (below) catches it by count. Make helper
   functions `static`.
4. An adapter test sits inside `#ifdef FEAT_<flag>` in its file and may
   include that backend's header; a hook test inside
   `#ifdef UNIT_TESTS_REALLOC_HOOK`; a core test includes only core headers
   (the testable shell's among them, as `test_tui_prefs.c` includes
   `tui_prefs.h`) and libc; a test of a shell file the runner links
   includes that file's header (`test_sysmisc.c` includes `sysmisc.h`).
   The declarations in `tests.h` keep a file whose flag is off
   from being an empty translation unit, which `-Wpedantic` rejects.
5. Free whatever the test allocates: the debug runner also runs
   LeakSanitizer.

### Core checks

Both scripts check every file in `./tool sources core` (an empty list is an
error), call `gcc` by name, run from any directory and exit 0 only when their
canary is detected and every result is PASS or XFAIL; the include check also
needs its non-call canary to pass.

- `tests/check-includes.sh` strips comments with
  `gcc -fpreprocessed -dD -E -P`, then reports each `#include` in the CORE
  file, or in a core header it includes, that is not allowed for that
  file. The allow-list has two tiers. `core_headers` (`base.h`, `vmio.h`,
  `gbuffer.h`, `sim.h`, `opstate.h`, `prng.h`, `ccout.h`, `transport.h`,
  `music.h` and `portlabel.h`, which `sim.h` includes) and the system
  headers `<assert.h>`, `<errno.h>`, `<limits.h>`, `<stdbool.h>`, `<stddef.h>`,
  `<stdint.h>`, `<stdlib.h>` and `<string.h>` are allowed everywhere.
  `shell_headers` (`tick.h`, `tooltips.h`, `tui_prefs.h`, the testable
  shell's) are allowed only when the CORE file under scan is in
  `shell_files` (`tick.c`, `tooltips.c`, `tui_prefs.c`): the tier is the
  CORE file's, whichever header is being read, so `sim.c` including
  `tooltips.h` fails as `FAIL  sim.c "tooltips.h"`, and `tick.h` reached
  through `music.h` fails every CORE file outside the shell tier that
  includes `music.h`, naming `music.h`, while the shell tier's files pass. A
  testable-shell module adds its `.c` to `shell_files` and its header to
  `shell_headers`, never to `core_headers`, where any core file could
  include it (`tui_prefs.c` and `tui_prefs.h`, in v3.0.0, were the latest).
  It also reports each of the fourteen banned names, `rand`, `srand`,
  `srandom`, `drand48`, `lrand48`, `srand48`, `seed48`, `mrand48`,
  `lcong48`, `getenv`, `secure_getenv`, `environ`, `__environ` and
  `_environ`, the calls that use a hidden global PRNG state and the reads
  of the environment, found as a whole word in code (no letter, digit, `_`
  or `$` on its left, no letter, digit or `_` on its right), each as its
  own pattern (`banned_names`). A second style, `banned_calls`, reports
  each of `random`, `initstate` and `setstate`, libc's other hidden-state
  PRNG calls, only as a call, each as its own pattern: the name with no
  letter, digit, `_` or `$` on its left, then optional white space and
  `(`, in the code with its backslash-newlines spliced, each preprocessor
  directive line ended with ` ;` and its lines then joined, so `setstate`
  with its `(` on the next line counts, but `#define X random` before a
  line that starts with `(` does not. A whole word would hit the `R`
  operator's token `random` and `prng.h`'s parameter name `initstate`,
  which no `(` follows. `erand48`, `nrand48`, `jrand48`, `rand_r`,
  `random_r`, `srandom_r`, `initstate_r` and `setstate_r` take their state
  from the caller and are not banned. Nor are `arc4random`,
  `arc4random_buf`, `arc4random_uniform` and `getentropy`: CORE's
  `-std=c99` declares none of them, so a call is an implicit declaration,
  which CI's `-Werror` build rejects (a prototype written in the file
  would pass). `base.h` is shared foundation, allowed whole and not
  scanned. Comments do not count. Known edges: a banned name or call in a
  string literal or an `#if 0` block is reported. So is a call through a
  member or local function pointer named like a banned call
  (`w->setstate(1)`), a function declared or defined with a banned call's
  name (`long random(void);`), and any identifier spelled like a banned
  name (a member or local named `environ` or `rand`); rename it. A macro
  defined in a scanned file whose body calls one
  (`#define BAD_SEED() random()`) is reported too, since the scan keeps
  `#define` lines (`-dD`; only `#include` lines are dropped). Not
  reported: `(random)()`, the address (`&random`, `fp = random`), a call a
  macro builds from its arguments (`CALL(random)`; the script expands no
  macros), and a call in the body of a macro defined in an unscanned
  header (`base.h`, system headers). `base.h` includes `<unistd.h>`, so
  `read()`, `write()`, `usleep()` and the rest of it reach every core file
  and pass the check. The canary `tests/includes/canary.c` includes
  `<stdio.h>`, calls or reads each of the fourteen names once, calls each
  of the three once (`initstate` with a backslash-newline before its `(`,
  `random ()` with a space, `setstate` with its `(` on the next line, so
  each form has one name to pin it) and includes `"tick.h"`; the canary is
  not in `shell_files`, so `"tick.h"` is never allowed for it and is
  reported, never queued. It is scanned first, the same way as a CORE
  file, and all nineteen patterns must be reported, or the check is blind
  to the missing one and the run fails. The list of nineteen is spelled
  out in the script, so a name dropped from either banned list is caught
  too, and a name in either list that the canary's list lacks fails the
  canary, so a new name gets a canary. The non-call canary
  `tests/includes/canary-noncalls.c` holds `random`, `initstate` and
  `setstate` only as words that are not calls (an X-macro's `R` token,
  members, locals, a parameter, a `#define` ending in one before a line
  that starts with `(`, `random_r(`, `canary_random(` and a comment) and
  no banned name. It is scanned next, the same way, and nothing may be
  reported for it: if anything is, the call style matches a non-call use,
  and the run fails with `canary NOT detected`. Its code must also still
  hold each banned call as a word and `canary_random(`, the witness for
  the call style's left boundary, or the non-call canary is blind and the
  run fails the same way.
- `tests/check-nm.sh` compiles each CORE file on its own with
  `gcc -c -std=c99 -O2 -DNDEBUG -g0 -fno-pie -no-pie -fno-lto`, and again
  with `-O0` in place of `-O2`, and fails each writable symbol (class
  `B b C D d G g S s V v`) that `nm -P` lists in either pass and no marker
  covers, and a file with no symbol at all. The second pass is there
  because at `-O2` gcc places a non-`const` table that is never written and
  whose address never escapes in `r`, so the `-O2` pass alone cannot tell
  it from a `const` one; at `-O0` it is `d`, and `const` dropped from
  `sim.c`'s `note_sequence` fails as `FAIL  sim.c d note_sequence (unmarked
  symbol at -O0)`. Every PASS, FAIL and XFAIL line names its pass or
  passes. Markers name a symbol or a prefix and span both passes (below);
  the marker set is empty since v1.0.0, so any writable global in a CORE file
  fails. The canary `tests/nm/canary.c` holds one writable symbol of each
  class `b`, `d`, `B` and `D`, each with its address escaping. In each pass
  the check must flag all four by name, and its marker code must sort them
  with a built-in marker set: `canary_static_*` matches both statics,
  `canary_extern_init` its one symbol, `canary_absent` nothing (the XPASS
  path), and `canary_extern_zero`, which no marker covers, is reported
  unmarked. A fifth symbol, `canary_O0_only`, is the `-O0` pass's own
  canary: a non-`const` table that is never written and whose address
  never escapes, read once at a constant index. It must not be writable in
  the first pass (`-O2` folds it away) and must be `d` in the second, and
  there must be two passes, so a second pass that optimises like the first
  fails the canary; it has its own built-in marker, so it is never
  unmarked. The canary prints one line per pass; otherwise the check is
  blind and the run fails. Symbols are listed in C collation. The script is
  Linux/ELF only: elsewhere it exits 2.

Today they print, on the uConsole's gcc 12.2:

```text
$ tests/check-includes.sh
PASS  canary (detected: <stdio.h>, rand, srand, srandom, drand48, lrand48, srand48, seed48, mrand48, lcong48, getenv, secure_getenv, environ, __environ, _environ, random, initstate, setstate, "tick.h")
PASS  non-call canary (random, initstate, setstate as words, not calls: none reported)
PASS  gbuffer.c
PASS  vmio.c
PASS  sim.c
PASS  opstate.c
PASS  ccout.c
PASS  music.c
PASS  transport.c
PASS  tick.c
PASS  tooltips.c
PASS  tui_prefs.c
include check: 10 files, 10 pass, 0 xfail, 0 fail, 0 xpass; canary detected
$ tests/check-nm.sh
PASS  canary -O2 (detected: D canary_extern_init, B canary_extern_zero, d canary_static_init, b canary_static_zero; canary_O0_only not writable; marker path: canary_extern_zero unmarked, canary_absent matches none)
PASS  canary -O0 (detected: d canary_O0_only, D canary_extern_init, B canary_extern_zero, d canary_static_init, b canary_static_zero; canary_O0_only d; marker path: canary_extern_zero unmarked, canary_absent matches none)
PASS  gbuffer.c (-O2, -O0)
PASS  vmio.c (-O2, -O0)
PASS  sim.c (-O2, -O0)
PASS  opstate.c (-O2, -O0)
PASS  ccout.c (-O2, -O0)
PASS  music.c (-O2, -O0)
PASS  transport.c (-O2, -O0)
PASS  tick.c (-O2, -O0)
PASS  tooltips.c (-O2, -O0)
PASS  tui_prefs.c (-O2, -O0)
nm check: 10 files, 10 pass, 0 xfail, 0 fail, 0 xpass; canary detected in every pass
```

In the summaries, `pass` counts clean files, and `xfail`, `fail` and `xpass`
count results, so one file can add several.

### Check markers

`tests/xfail/<check>` lists the violations a check expects, one per line;
`#` starts a comment.

| Marker file | Line format | Today |
| --- | --- | --- |
| `tests/xfail/include-check` | `<file> <pattern> <item...>`, where the pattern is a banned name (`rand`, `srand`, `srandom`, `drand48`, `lrand48`, `srand48`, `seed48`, `mrand48`, `lcong48`, `getenv`, `secure_getenv`, `environ`, `__environ`, `_environ`) or banned call (`random`, `initstate`, `setstate`), `<name.h>` or `"name.h"` (a shell header's `"tick.h"` or `"tooltips.h"` among them) | none (the per-cell state store deleted its one line, for `sim.c` and `rand`, in v1.0.0) |
| `tests/xfail/nm-check` | `<file> <symbol\|glob> <item>`, where the pattern is a symbol name, or a literal prefix of at least 3 characters followed by one trailing `*` (`chord_*`); a line spans both passes | none (v1.0.0 deleted its seven lines: three with the per-cell state store and four with the CC engine) |

- The items are the item tags the fix waits for, each matching
  `^(B[1-8]|I[1-6])$` (`B1`, `I3`); an nm marker names exactly one. A
  marked violation reports XFAIL and the run stays green.
- A marker whose violation is gone reports XPASS and fails the run: the
  series that turns a check green deletes the marker line in the same
  commit.
- Each nm marker line reports on its own, over both passes: XFAIL with its
  item and every writable symbol it matched (class, name and pass, as
  `d note_sequence at -O0`), or XPASS once it matches none in either pass,
  so each fix removes the lines of the symbols it deletes, as the per-cell
  state store did in v1.0.0. A
  symbol that is writable only at `-O0` is therefore markable, and
  `sim.c note_sequence B3` over a `note_sequence` without `const` reports
  one XFAIL naming `-O0` and exits 0. A writable symbol that no line of its
  file matches fails as `FAIL <file> <class> <name> (unmarked symbol at
  <pass>)`.
- The change that brought the CC engine (v1.0.0) made every lookup table in
  `sim.c` `const` at every pointer level and deleted the `chord_*` and
  `scale_*` lines, which covered the 72 tables that stayed writable because
  `scales` and `scales_and_chords` held their addresses. At `-O2` a table that
  nothing addresses or writes shows as `r` even when it is not `const`,
  because gcc places never-written statics in read-only data, so the `-O2`
  pass alone cannot prove the no-writable-globals rule; that change and the
  first inversions (which moved the scale and chord tables to `music.c`), both
  in v1.0.0, each recorded a one-off `-O0` `nm`. Since v3.0.0 the script makes
  that `-O0` pass itself on every run, so the rule no longer rests on a
  one-off check, and a commit that drops `const` from such a table fails CI.
  An XFAIL line names every symbol it matched, so a CI log records the runner
  gcc's list, which may differ from the uConsole's; that log is the evidence
  for any marker change.
- A marker that names a file outside CORE, a malformed line, any other nm
  pattern (`*_*`, `_*`, `sc*`, `sca?`) or a duplicate (file, pattern) fails
  the run. An nm marker never hides an object with no symbols or a failed
  compile.

### Debug-build check

`tests/check-debug-build.sh <binary> [runner-log]` fails a debug binary
whose symbol table (`nm`) lacks `__asan_init` or a `__ubsan_handle_*`
symbol, each of class `U` (gcc's shared runtimes) or `T` (a runtime linked
in statically, as clang does): `tool` only warns when it cannot detect the
compiler, and then builds without sanitizers, which no other step would
notice. `__ubsan_default_options`, which the runner defines, never counts.
Given the log of one `build/debug/unit_tests` run, it also fails unless
the run ended with `<n> tests, 0 failed`, the log holds no sanitizer line
(`Sanitizer` or `runtime error:`, including a LeakSanitizer report printed
after the summary), every `T test_*` function the binary defines is
reported by `ok` or `FAIL` (it names each one that is not), no test is
reported twice, and `<n>` equals the number of `T test_*` functions. A test
registered twice cannot hide an unregistered one by keeping the count
equal. Release and unsigned-char binaries are exempt and never checked:
they link with `-flto -s`, so `nm` lists nothing and the script fails them,
as it fails any stripped binary. The script is Linux/ELF only: macOS
prefixes symbol names with an underscore, so elsewhere it exits 2.

```sh
nice -n 19 taskset -c 0-2 tests/check-debug-build.sh build/debug/cli
UBSAN_OPTIONS=halt_on_error=1 nice -n 19 taskset -c 0-2 build/debug/unit_tests 2>&1 | tee /tmp/unit_tests.log
nice -n 19 taskset -c 0-2 tests/check-debug-build.sh build/debug/unit_tests /tmp/unit_tests.log
```

Each `--portmidi` or `--alsa` test build overwrites
`build/debug/unit_tests`, so check each runner right after its own run.

### Shell argument check

`tests/check-shell-args.sh` pins the arguments `tui_main.c` passes to the
three helpers that moved under unit test in v3.0.0: `tick_stop` (`tick.c`),
`clock_out_toggle` and `clock_out_return_internal` (`midi_in.h`). The unit
tests give the helpers arguments they compute themselves, and no test
target compiles `tui_main.c`, so a wrong argument at a call site would pass
every build, test and CI job:

- a `tick_stop` gate other than
  `clock_out_active(&a->clock_out, a->clock_source)`, such as the stored
  setting `a->clock_out.enabled`, sends `FC` on stop under External, back
  to the master;
- an `e` without `+ a->clock_out.accum_secs`, or one taken from another
  instant than `now`, moves the next tick after a Send MIDI Beat Clock
  toggle;
- a `clock_out_return_internal` `playing` other than `a->is_playing` sends
  `FB` while stopped, or skips the re-arm while playing;
- a toggle `src` or `playing` fixed to Internal or `true` sends `FB` or `FC`
  under External or while stopped, and a `clock_out_return_internal` `now`
  other than the present re-arms from the wrong instant.

Like the core checks, it reads the file through
`gcc -w -fpreprocessed -dD -E -P`, which strips comments and expands
nothing, calls `gcc` by name, runs from any directory and needs no build;
it takes no argument (any is a usage error, exit 2). Each preprocessor line
is dropped with its continuations, so the code of every `#if` branch is
scanned, but the conditionals open at each point are kept. The rest is
joined into one string, the inside of each string and character literal is
blanked, and each call of the three helpers (whole word, spaces allowed
before the `(`) is split into its arguments at top-level commas, all
whitespace dropped. If the braces do not balance, the file fails
(`FAIL  tui_main.c: unbalanced braces …`) and no call is checked. The
rules:

- `tick_stop`: four arguments. The gate, the fourth, is
  `clock_out_active(&a->clock_out,a->clock_source)` passed inline, or a
  local declared once from that expression (below).
- `clock_out_toggle`: seven arguments. `src`, the second, is
  `a->clock_source`. `now`, the fourth, is a local declared once from
  `stm_now()`. `e`, the fifth, is
  `stm_sec(stm_diff(<now>,a->clock_out.clock))+a->clock_out.accum_secs`,
  `<now>` being the fourth argument's name, inline or a local declared once
  from it. `playing`, the sixth, is `a->is_playing`.
- `clock_out_return_internal`: five arguments. `now`, the third, is
  `stm_now()`, and `playing`, the fourth, is `a->is_playing`.
- Every `tick_stop` and `clock_out_toggle` call sits under no conditional,
  and every `clock_out_return_internal` call under exactly
  `#ifdef BORCA_HAS_MIDI_IN`, as today's does. Any other stack (`#if 0`, an
  `#else` or `#elif` arm, another macro, an extra level) fails the call, so
  a call compiled out never counts as present.
- Each helper has at least one call in `tui_main.c`
  (`FAIL  tui_main.c: no call to <helper>` otherwise), and every call found
  is checked, so a second call site meets the same rule. A helper name used
  any other way fails where it is used: in a `#define` (a call through a
  macro, whose body the scan drops), or not followed by `(` (its address
  taken, or `(tick_stop)(…)`).

A local declared once from `x`: the call's block (the text from the `{` of
the innermost block that holds the call up to the call), at its own brace
depth (the inside of nested `{…}` skipped), holds exactly one assignment to
it, and that is a declaration with an initializer (only type words since
the previous `;`, `{` or `}`, so not after `if (…)` or `else`), under the
same conditionals as the call, from `x`; and the name is assigned nowhere
else in the enclosing function. So a local declared further out fails
loudly, as does one assigned again, assigned on one path only, or declared
under `#if 0`. An assignment to `v` is `v` (not after an identifier
character, `.` or `>`), then `=`, a compound `op=`, `<<=` or `>>=` (never
`==`), or `v++`, `v--`, `++v` or `--v`; its right-hand side runs to its
top-level `;` or `,`, and a compound one or an increment never counts as
the expected value. Each call or other use gets one line, by its first
failing rule (conditionals, argument count, then the arguments in order):
`PASS  tui_main.c <function>: <what was found>` or
`FAIL  tui_main.c <function>: <helper>'s <argument> is <found>, expected
<expected>`, where an assignment failure also gives the counts and the last
right-hand side seen. A summary line follows,
`shell-args check: <n> calls, <p> pass, <f> fail; canary detected`, where
the counts are of those lines and `, <k> file-level fail` follows `<f>`
when a file-level line (no call, unbalanced braces, gcc or awk failed)
says FAIL. The exit status is 0 only when the canary is detected, no line
says FAIL and every helper has a call.

The spellings are pinned, not the meaning: that is the only reading
possible without compiling the shell. Renaming `a`, `now`, `e` or the
fields (`Ged *g`, `double dt`, …) therefore fails the check loudly, naming
the expected spelling, and is never a silent pass; whoever renames them
updates the script's spellings and the canary in the same commit. There is
no xfail file for this check: a wrong argument is fixed in the same commit,
never marked.

The canary `tests/shell-args/canary.c` is never compiled. It is scanned
first, the same way, without the no-call rule, and only its one
`PASS  canary (…)` or `FAIL  canary (…)` line is printed. Thirty functions
each plant one mistake, and each must fail exactly once:

- the gate: from `a->clock_out.enabled` as a local (`canary_gate_enabled`)
  and passed directly (`canary_gate_direct_enabled`), the predicate with
  `Clock_source_internal` (`canary_gate_internal`), the right local widened
  by `|=` (`canary_gate_reassigned`), a parameter never assigned
  (`canary_gate_param`), an outer default with the predicate assigned in a
  nested `if` (`canary_gate_outer_default`), the right declaration under
  `#if 0` (`canary_gate_if0_decl`), assigned by an `if` without braces
  (`canary_gate_if_unbraced`), declared in a nested block that closes
  before the call (`canary_gate_nested_decl`), reassigned after the call
  that a `goto` runs again (`canary_gate_after_call`), and only ever
  assigned by `|=` (`canary_gate_compound_only`);
- the toggle: `e` without the carry (`canary_e_no_carry`) and from
  `stm_since` (`canary_e_since`), `stm_now()` passed as `now`
  (`canary_now_call`), `now` from `a->clock_out.clock` (`canary_now_stale`),
  `now++` (`canary_now_postinc`), `--e` (`canary_e_predec`), `src`
  `Clock_source_internal` (`canary_toggle_src_internal`) and `playing`
  `true` (`canary_toggle_playing_true`);
- `clock_out_return_internal`: `true` and `a->clock_out.enabled` passed as
  `playing` (`canary_playing_true`, `canary_playing_enabled`),
  `a->clock_out.clock` as `now` (`canary_return_now_stale`), four arguments
  (`canary_return_four_args`), and a correct call outside any conditional
  (`canary_return_unguarded`), in the `#else` arm of
  `#ifdef BORCA_HAS_MIDI_IN` (`canary_return_else_arm`) and in an `#elif`
  arm (`canary_return_elif`);
- other uses: a correct `tick_stop` call under `#if 0` (`canary_call_if0`),
  a call in a `#define` (`canary_call_in_define`), the address taken
  (`canary_address_taken`) and `(tick_stop)(…)`
  (`canary_call_parenthesised`).

Every other `clock_out_return_internal` call in it sits under
`#ifdef BORCA_HAS_MIDI_IN`, so each function fails only through its
planted mistake. `canary_ok` holds one correct call of each helper, the
gate and `e` passed inline, which must pass, and a comment and a string
literal each holding a wrong `tick_stop` call and a `{`, and the character
literals `'{'` and `'"'` before its calls, which must count for nothing.
The script lists the thirty names in its own text, so a name dropped from
the list fails the canary, as does any other function that shows up. Two
fixtures are then scanned with the no-call rule:
`tests/shell-args/nocall.c`, which holds only names that start like the
helpers' (`tick_stop_soon`, …), must give exactly the three
`no call to <helper>` FAILs, and `tests/shell-args/unbalanced.c`, whose
two `#if` arms each open a `{`, exactly the unbalanced-braces FAIL.
Otherwise it prints `FAIL  canary (the check is blind: …)`, naming each
function missed or wrongly reported and each fixture's lines, followed by
the reason when gcc or awk could not scan a file, and the run fails.

Known edges:

- An assignment through a pointer or a macro is not seen.
- A gate held in a struct field is refused, not followed.
- Only `tui_main.c` is scanned. A call added to another shell file is not
  checked, but the no-call rule fails if the call leaves `tui_main.c`.
- A declaration of a helper inside `tui_main.c` would be read as a call and
  fail.
- The conditionals are pinned by the directive's spelling
  (`#ifdef BORCA_HAS_MIDI_IN`). Respelling it (`#if defined(…)`) or moving
  a call under another guard fails loudly and needs the script updated in
  the same commit.
- Correct code can fail loudly: a local declared outside the call's block,
  one declared after another declarator (`bool x = y, clock_out = …;`), and
  a name reused in another block of the same function.
- Not pinned: the sink and the other pointer arguments (`&a->clock_out`,
  `&a->susnote_list`, `&a->ccout`), `bpm`, and the calls of
  `clock_out_on_start` and `clock_out_on_deadline`, which this check does
  not read.

Today it prints:

```text
$ tests/check-shell-args.sh
PASS  canary (rejected: canary_gate_enabled, canary_gate_direct_enabled, canary_gate_internal, canary_gate_reassigned, canary_gate_param, canary_e_no_carry, canary_e_since, canary_now_call, canary_now_stale, canary_playing_true, canary_playing_enabled, canary_call_if0, canary_gate_outer_default, canary_gate_if0_decl, canary_gate_if_unbraced, canary_gate_nested_decl, canary_gate_after_call, canary_now_postinc, canary_e_predec, canary_call_in_define, canary_address_taken, canary_call_parenthesised, canary_toggle_src_internal, canary_toggle_playing_true, canary_return_now_stale, canary_return_unguarded, canary_return_else_arm, canary_return_elif, canary_gate_compound_only, canary_return_four_args; accepted: canary_ok's 3 calls; fixtures: nocall.c's 3 no-call FAILs, unbalanced.c's unbalanced braces)
PASS  tui_main.c ged_set_playing: tick_stop's gate is clock_out, declared from clock_out_active(&a->clock_out,a->clock_source)
PASS  tui_main.c tui_drive_menus: clock_out_toggle's src is a->clock_source, now is now, declared from stm_now(), e is e, declared from stm_sec(stm_diff(now,a->clock_out.clock))+a->clock_out.accum_secs, and playing is a->is_playing
PASS  tui_main.c tui_set_clock_source: clock_out_return_internal's now is stm_now() and playing is a->is_playing, under #ifdef BORCA_HAS_MIDI_IN
shell-args check: 3 calls, 3 pass, 0 fail; canary detected
```

## Wakeup check

`tests/wakeups/ab.sh` is a development check of the event loop's wakeups,
since glides step between ticks (v2.0.0); CI only shellchecks it. It needs
no arguments:

```sh
nice -n 19 taskset -c 0-2 tests/wakeups/ab.sh
```

It builds two release `orca`s without ALSA or PortMidi, each in its own
tree under `build/wakeups/`, and never writes `build/orca`: `4f349cd`, from
`git archive` with `tests/wakeups/4f349cd-counter.patch` applied, and this
working tree's tracked files with `CFLAGS_EXTRA=-DBORCA_DEBUG_COUNTERS`.
Both count the loop's waits and print
`bOrca wakeups: <n> waits, <z> zero-timeout in <s> s` to stderr on quit:
`<n>` counts the waits (`wgetch` calls) entered with a timeout above 0,
`<z>` those entered with a timeout of 0. The patch adds only that counter,
at the same code point, unconditionally, since `4f349cd`'s `tool` has no
`CFLAGS_EXTRA`.

Each run lasts 30 s with `--bpm 120`, in a pty of 40 rows by 120 columns
(util-linux `script`), with its own scratch `XDG_CONFIG_HOME` holding only
`midi_beat_clock`, so `glide_hz` is 100; Ctrl+Q then quits it. `script`
runs the command with `$SHELL`, so the script pins `SHELL=/bin/sh`. There
are two configurations: `strict`, with `--strict-timing` and
`midi_beat_clock = 1`, and `off`, with neither and `midi_beat_clock = 0`.

- **Idle**: an empty grid, on both builds in both configurations. It fails
  unless bOrca's waits per second are within 0.90 to 1.10 times
  `4f349cd`'s, and its zero-timeout waits per second at most 1.5 times
  `4f349cd`'s + 5, which shows that no busy-wait was added. (`4f349cd`'s
  deadline also waits on note-offs, hence the empty grid.)
- **Glide**, bOrca only: `examples/misc/glide_rate.orca`, whose glide never
  ends and steps at `glide_hz`, against a control, the same file with its
  `!` replaced by `.`, in the same configuration. In `off` the glide's
  waits per second must be at least the control's + 50, which shows the
  loop waking for the CC engine's deadline. In `strict` the table already
  wakes the loop about 190 times a second, so waits cannot show that; there
  its zero-timeout waits per second must be at most 1.5 times the
  control's, which shows the engine's deadline staying off the strict
  table (folding it into the table measured about 2.2 times). In both, the
  glide's waits per second must be at most the control's + 2 ×
  `glide_hz`, 200: a wait point that stops polling the engine leaves its
  deadline passed and waits 1 ms on every pass, about 770 more a second.

It prints one line per run and a PASS or FAIL line per bound, and exits 0
when every bound holds, 1 when one fails, and 2 when a build or a run
fails. It takes about five minutes. The final figures for the appliance
come from the installed candidate at the repin, not from this script.

## Realloc shim check

`tests/realloc-shim/check.sh` checks, end to end, that bOrca
keeps its grid and mark buffer when `realloc` or `malloc` fails, and says
so where it cannot go on, and that a failed startup load leaks nothing. It
takes a release `cli` and, optionally, a release `orca` and then a `-d`
`orca`, both built without `--alsa` or `--portmidi` (ASan refuses a
preloaded library ahead of its runtime, so the shim cases need release
builds, and the leak case reads the debug build's LeakSanitizer):

```sh
nice -n 19 taskset -c 0-2 tests/realloc-shim/check.sh build/cli build/orca build/debug/orca
```

It refuses, with exit 2 and before any run, an `orca` or `-d` `orca` whose
`readelf -d` lists `libasound` or `libportmidi` (the build block below
leaves an `--alsa` `build/debug/orca`: rebuild `-d orca` first), and it
unsets the caller's `SHIM_MALLOC_MAX` and `SHIM_REALLOC_MAX`, so each case
sets only the limits it names.

It compiles `tests/realloc-shim/shim.c` with `$CC` (default `cc`) into a
scratch directory, an `LD_PRELOAD` library that fails every `realloc` above
`SHIM_REALLOC_MAX` bytes and every `malloc` above `SHIM_MALLOC_MAX` bytes
(each unset or empty: none fails; a `malloc` within its limit goes to
glibc's `__libc_malloc`). `cli -t 1` on a 100 x 50 grid file with a
`realloc` limit of 4096 must exit 1, not on a signal, with
`File load error: Not enough memory for the grid.`, and a 20 x 10 file must
run. With `orca`: `--initial-size 200x200` under `script` and `timeout 10`,
`realloc` limit 30000, must exit 1 with
`Cannot start: not enough memory for the grid`; the fill case, the same run
with a `malloc` limit of 30000 and no `realloc` limit (the 40000-byte
startup grid fails), must exit 1 with the same message; and
`--initial-size 1000x25` in a private tmux server, keys `+` `+` (25 rows to
33, 33000 bytes) then Ctrl+Q, must grow the HUD's `1000x25` to `1000x41`
without a limit, and with `realloc` limit 30000 keep running with `1000x25`
and an advancing frame counter and exit 0. With the `-d` `orca`, the leak
case, no shim: the binary must reference `__asan_init` (a release build
would pass vacuously); started in a private tmux server under
`timeout --foreground 15` on a three-row file whose third row is shorter
than the first two (the load fails mid-file, `Grid file is not a
rectangle`), its pane must show the `File load error` message (looked for
after 3 s, then every 0.5 s for up to 5 s more), then Escape and Ctrl+Q
must quit it with exit 0 and no `LeakSanitizer` on stderr; it runs with
`ASAN_OPTIONS=detect_leaks=1` and an empty `LSAN_OPTIONS`, over any the
caller exported. Each `orca` run has a scratch `XDG_CONFIG_HOME` and
`BORCA_ALSA_CLIENT_NAME=borca-realloc-shim-<pid>`, and each tmux run a
server of its own. CI runs the `cli` case and the release `orca` cases
(startup, fill and resize) in the `build` job.

## ALSA name-guard check

`tests/alsa-guard/devcheck.sh` is a development check
of the ALSA client-name guard in a real `--alsa` `orca`; CI shellchecks it
and compiles its shim but cannot run it, since the runners have no
sequencer. The unit row
`alsa_name_scan` covers only the pure scan helper in `midi_in.h`; this
check proves what it cannot see: that `alsa_ctx_open` scans the sequencer a
second time after it takes the name and before it creates any port, and
that the adapter, `alsa_name_next_client`, passes a scan error through
rather than ending the list as a free name. Run it on a release `--alsa`
`orca` (a debug build links ASan, which refuses `LD_PRELOAD`, so the script
exits 2 naming ASan; a build without `--alsa` exits 2 naming `libasound`):

```sh
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build --alsa orca
nice -n 19 taskset -c 0-2 tests/alsa-guard/devcheck.sh build/orca
```

It builds `tests/alsa-guard/shim.c` into an `LD_PRELOAD` library in a
scratch directory and runs the `orca` on a scratch three-line patch, with
`TERM=xterm`, under util-linux `script` (with `SHELL=/bin/sh`) and
`timeout --foreground -k 1 3` (6 s for D1's first instance), so a run that
gets past the guard exits 124.
The shim's mode, in `BORCA_GUARD_SHIM`, makes a lost race deterministic
(`hold`: a second client of the same process takes the name just before
`orca`'s rename) or fails the scans (`eio`: every scan; `eio2`: only the one
after the rename), and it prints `pid <n> ports created <k>` on exit. One
PASS or FAIL line per check:

- **D1**, no shim: once a first instance holds the name (polled every
  0.1 s for up to 3 s), a second instance under the same name exits 1 with
  `Cannot start: an ALSA client named '<name>' is already running (set
  BORCA_ALSA_CLIENT_NAME to run another instance)`, one client has the name
  before and after it, and the first runs until the timeout.
- **D2**, `hold`: exit 1 with the same message, `ports created 0`, and no
  client of the name 1 s into the run. Without the second scan the run gets
  past the guard (two clients of the name, rc 124); with the scan moved
  after the ports it exits 1 with `ports created 2`.
- **D4**, `eio`: exit 1 with `Cannot start: the scan for an ALSA client
  named '<name>' failed: Input/output error` and `ports created 0`.
- **D5**, `eio2`: the same for the second scan, and no client of the name
  1 s into the run. An adapter that maps an error to the end of the list
  fails D4 and D5 (rc 124, ports created), while the unit runner passes.
- **Live**: the live `bOrca` client's lines in `/proc/asound/seq/clients`
  (its `Client` line, its `Port` lines and their `Connecting To:` and
  `Connected From:` lines, but not its pool counters, which move while it
  plays) are the same before and after, and no guard client is left.

It prints `alsa guard check: <n> checks, <f> failed` and exits 0 when none
failed, 1 when one did, and 2 on usage, a missing tool (`script`, `timeout`,
`gcc`, `readelf`, `taskset`), an unreadable `/proc/asound/seq/clients`, a
binary that is not a release `--alsa` build, a `guard-<pid>-` client already
present, a shim that does not build or a scratch directory that cannot be
made. It takes about 10 s, and an interrupt stops its runs in the background
before it exits. Every client name is `guard-<pid>-<check>`, never `bOrca`,
and every run gets a fresh scratch `XDG_CONFIG_HOME`; it never touches the
live `bOrca` client or `borca-tty.service` (it only reads
`/proc/asound/seq/clients`, and calls no `systemctl` or `aconnect`), and it
runs every `gcc` and `orca` under `nice -n 19 taskset -c 0-2`. The twenty
racing pairs run when the guard was written (check D3, v3.0.0) are not part
of it: `hold` makes the race deterministic, and the pairs stay in that
release's record.

## ALSA runtime contract check

`tests/alsa-contract/devcheck.sh`, for the in-tree ALSA backend's runtime
contract, is a development check of what `verify` cannot see on the
installed engine and no test links: `tui_main.c`'s send path and the HUD's
`MIDI err`. CI only shellchecks it, since hosted runners have no
`/dev/snd/seq`. Run it on any `--alsa` `orca`: release, `-d` (nothing is
preloaded, so ASan runs) or `make appliance`. A build without `--alsa`
exits 2 naming `libasound`.

```sh
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build --alsa orca
nice -n 19 taskset -c 0-2 tests/alsa-contract/devcheck.sh build/orca
```

It writes a scratch directory (`mktemp -d`) holding `xdg/orca.conf`, the
single line `midi_beat_clock = 1`, and `note.orca`, a 7x3 grid whose `D4`
bangs `:03C` every 4 ticks: note 36 on channel 0, velocity 127, twice a
second at 120 BPM. It runs `<orca> --bpm 120 note.orca`, which plays at
start, in a 120 x 40 pane of its own tmux server
(`tmux -L alsa-contract-<pid> -f /dev/null`, with `SHELL=/bin/sh`), under
the client name `alsa-contract-<pid>` (`BORCA_ALSA_CLIENT_NAME`),
`XDG_CONFIG_HOME=<scratch>/xdg` and `tests/lib.sh`'s `UBSAN_OPTIONS`
(`halt_on_error=1`), with its stderr in the scratch directory. It reads
the HUD's first line, the pane line that holds `7x3`, with
`tmux capture-pane -e -p`, and subscribes with
`aseqdump -p alsa-contract-<pid>:0`. Each subscriber's time counts from
when `/proc/asound/seq/clients` shows it on port 0 (waited for up to 3 s),
not from its start. One PASS or FAIL line per check:

- **C1**: within 10 s `aconnect -l` lists the client, `aconnect -i` lists
  its port 0 `MIDI out` (readable and subscribable) and not port 1, and
  `aconnect -o` its port 1 `MIDI in` and not port 0.
  If the client never appears, C2 to C6 fail as not run.
- **C2**: a subscriber for 3 s gets at least 4 `Note on 0, note 36,
  velocity 127` and no other `Note on`, each followed by its `Note off`
  before the next `Note on` and no other `Note off` (leading `Note off`s
  are skipped, since aseqdump can subscribe mid-note), at least 100
  `Clock`s, and every event from the client's port 0. An aseqdump that
  never subscribes fails C2, as does one refused its subscription, whose
  message is quoted.
- **C3**: 1 s after that subscriber has gone, the HUD shows no `MIDI err`:
  a send with no subscriber is normal (`send_midi_3bytes`). It runs before
  C4, since `MIDI err` stays until quit.
- **C4**: a second subscriber is stopped with SIGSTOP 0.5 s after it
  subscribes; within 20 s (a capture every 0.1 s) a send to it fails and
  the HUD shows `MIDI err`. The line gives the seconds it took (3.9 s on
  the uConsole). A subscriber that never subscribes or has exited fails
  C4, with its output quoted.
- **C5**: in that capture `MIDI err` is bold. The check follows every SGR
  sequence from the top of the capture to `MIDI err`, since
  `capture-pane -e` carries attributes from line to line and writes bold
  and a colour as separate sequences: 0 (or an empty sequence) resets, 1
  sets bold, 22 clears it, and the arguments of 38, 48 and 58 (`5;n` or
  `2;r;g;b`) are skipped, so a colour index 1 does not count as bold.
- **C6**: the second subscriber is resumed and ended, Ctrl+Q quits `orca`,
  within 15 s its exit status is 0 (a debug build's LeakSanitizer pass
  takes about 4.5 s), and its stderr holds nothing that matches
  `tests/lib.sh`'s sanitizer pattern; a report is quoted.
- **Live**: the live `bOrca` client's lines in `/proc/asound/seq/clients`
  (its `Client` line, its `Port` lines and their `Connecting To:` and
  `Connected From:` lines) are the same before and after, or absent both
  times, and no `alsa-contract-<pid>` client is left: the ALSA name-guard
  check's rule.

Beat clock is on because the notes alone send 4 events a second, while 48
`Clock`s a second fill a stopped subscriber's input queue in about 4 s, so
C4 sees a failed send quickly. A subscriber that leaves is not a failure,
so a stopped one is the ordinary way to make a send fail on a working
sequencer.

It prints `alsa contract check: <k> checks, <f> failed` and exits 0 when
none failed, 1 when one did, 2 on usage (no argument, or one that is not
an executable file), a missing tool (`tmux`, `aseqdump`, `aconnect`,
`readelf`, `taskset`) or `tests/lib.sh`, an unreadable
`/proc/asound/seq/clients`, a binary that is not an `--alsa` build, an
`alsa-contract-<pid>` client already present, or a scratch directory,
scratch file or tmux server that cannot be made, and 129, 130 or 143 when
SIGHUP, SIGINT or SIGTERM stops it. It takes about 10 s (13 s for a debug
build, 25 s when C4 times out).
It never touches the live `bOrca` client or `borca-tty.service`: `aconnect`
only lists, no `systemctl` or `sudo` runs, and it signals only the
aseqdumps it started, by PID, and kills only its own tmux server. It runs
`orca` and each aseqdump under `nice -n 19 taskset -c 0-2`, and on exit or
a signal it resumes and ends its aseqdumps, kills its tmux server and
removes the scratch directory, ignoring a second signal meanwhile.

After installing a bOrca binary, run it next to `verify`:

```sh
nice -n 19 taskset -c 0-2 tests/alsa-contract/devcheck.sh /usr/local/bin/borca
```

It runs that binary under the scratch name and config, so the live service
is untouched. The live client's own `aseqdump` at install stays a service
check, outside this script.

## ALSA clock-follow check

`tests/alsa-clock/devcheck.sh` is a
development check of the ALSA input reader under External, which no test
links: `alsa_ctx_open`'s request for a 500-event input pool and
`tui_input_drain`'s overflow event on `-ENOSPC`. The unit runner pins the
machine's side (`transport_overflow_*`); this check stops a real `orca`
while a clock master keeps clocking and checks what it plays when it
resumes. CI shellchecks it and compiles its two C sources, but cannot run
it, since hosted runners have no `/dev/snd/seq`. Run it on any `--alsa`
`orca`: release, `-d` or `make appliance`. A build without `--alsa` exits 2
naming `libasound`.

```sh
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build --alsa orca
nice -n 19 taskset -c 0-2 tests/alsa-clock/devcheck.sh build/orca
```

It builds `tests/alsa-clock/master.c` (a clock master: `FA`, then `F8`s at
a tempo, each logged with its `CLOCK_MONOTONIC` time) and
`tests/alsa-clock/recv.c` (a receiver with a 2000-event pool that logs each
note-on and note-off with its time) into a scratch directory
(`mktemp -d`) with `cc -lasound`. It writes `xdg/orca.conf` there
(`clock_source = external`, `alsa_input_device =
dev-clock-<pid>-master:clock`) and `clock.orca`, a 7x3 grid whose `D1`
bangs `:03C` every tick, one note per tick. For each stall it runs
`<orca> clock.orca` in a 100 x 30 pane of its own tmux server
(`tmux -L dev-clock-<pid> -f /dev/null`, with `SHELL=/bin/sh`) as the
client `dev-clock-<pid>` (`BORCA_ALSA_CLIENT_NAME`), with
`XDG_CONFIG_HOME=<scratch>/xdg`; then the receiver
(`dev-clock-<pid>-recv`, subscribed to `dev-clock-<pid>:0`) and the master
(`dev-clock-<pid>-master`), which sends `FA` 1.5 s later and then 120 BPM
for 3 s, the stall and 5 s more. 3 s after the `FA` it stops the `orca`
with SIGSTOP for the stall and resumes it with SIGCONT, reading the HUD's
`ext` field every 0.05 s until the master's last `F8`; then Ctrl+Q quits
the `orca`. A note's offset is the index of the master's last `F8` at or
before it minus 6 times the note's index (note n is tick n), and a burst is
a run of note-ons less than 40 ms apart (a step is 125 ms). One PASS or
FAIL line per check, each with the HUD's readings, the burst, the offsets,
the client's input pool (size, peak cells, alloc failures) and the `orca`'s
exit status:

- **S1**, a 10 s stall, inside the pool: no `ext lost` while the master
  runs, exactly one burst of 70 to 85 note-ons (80 missed steps), and every
  note in the master's last 3 s (from 2 s after the resume; at least 16) at
  an offset of -1 to 1: back on the master's bar. Without the pool call it
  follows 480 pulses behind with no burst; bOrca before v3.0.3 stops at
  `ext lost`.
- **S2**, an 11.1 s stall, past the pool: the input pool shows alloc
  failures (the kernel flushed it), no `ext lost` while the master runs, no
  burst, and every note in the master's last 3 s (at least 16) at an offset
  of 6 or more that is not a multiple of 6: it keeps following, behind by
  the flushed pulses (about 533). Without the `-ENOSPC` overflow event it
  stops at `ext lost`; with a pool of 2000 it lands on the bar (offset 0).
- **Live**: the live `bOrca` client's lines in `/proc/asound/seq/clients`
  (its `Client` line, its `Port` lines and their `Connecting To:` and
  `Connected From:` lines) are the same before and after, or absent both
  times, and no `dev-clock-<pid>` client is left: the ALSA name-guard
  check's rule.

S1's pool peaks near 486 of 500 cells, so other traffic to the client
during the stall (port changes of other clients, which reach it through
System Announce) can push it past the pool on a busy sequencer; the pool
figures on its line show it.

It prints `alsa clock check: <k> checks, <f> failed` and exits 0 when none
failed, 1 when one did, 2 on usage (no argument, or one that is not an
executable file), a missing tool (`tmux`, `cc`, `pgrep`, `readlink`,
`readelf`, `taskset`), an unreadable `/proc/asound/seq/clients`, a binary
that is not an `--alsa` build, a `dev-clock-<pid>` client already present,
a master or receiver that does not build, or a scratch directory, scratch
file or tmux server that cannot be made, and 129, 130 or 143 when SIGHUP,
SIGINT or SIGTERM stops it. It takes about 50 s.
It never touches the live `bOrca` client or `borca-tty.service`: it reads
`/proc/asound/seq/clients` and runs no `aconnect`, `systemctl` or `sudo`.
It signals only the `orca` it started (the pane shell's child, running the
named binary) and the master and receiver it started, by PID, and kills
only its own tmux server. It runs `cc`, the `orca`, the master and the
receiver under `nice -n 19 taskset -c 0-2`, and on exit or a signal it
resumes and ends them, kills its tmux server and removes the scratch
directory, ignoring a second signal meanwhile.

## CI

`.github/workflows/ci.yml` runs on pushes and pull requests to
`spec/borca-fixes`, the working branch, and `main`, bOrca's release line
from `v1.0.0`, and on `v*` tags; start a manual run with
`gh workflow run ci.yml --ref <branch>`.

- **build** (pinned `ubuntu-24.04` and `ubuntu-24.04-arm`, never `-latest`):
  first checks that `CFLAGS_EXTRA` reaches the compiler, then builds the
  debug and release `cli` with `CFLAGS_EXTRA=-Werror`, checks the debug
  `cli` with `tests/check-debug-build.sh`, and runs the golden suite on
  both and `tests/realloc-shim/check.sh` on the release `cli` and on the
  release `orca` without mouse support. It runs
  `tests/check-examples.sh` on the debug `cli`, builds
  `readme-gen` with `-Werror` and runs `tests/check-doc-sync.sh` on it. It
  then runs `tests/check-includes.sh`, `tests/check-nm.sh` and
  `tests/check-shell-args.sh`, which need no build, and a grep that fails
  unless `tui_save_prefs` in `tui_main.c` calls
  `tui_prefs_saves_key(&t->prefs, …)` exactly once and holds no save test
  of its own (`TOUCHFLAG(i)`, `prefs.touched`, a comparison with
  `Confopt_portmidi_output_device`), builds and runs the debug unit tests
  without and with `--portmidi` and with `--alsa` (ASan, UBSan with
  `halt_on_error=1`; the plain run fails unless it prints
  `ok tick_note_alloc_fail` and `ok realloc_hook_builtin_null`, the
  `--portmidi` run unless it prints both
  `ok portmidi_error_text_and_filters` and `ok portmidi_in_decode`, the
  `--alsa` run unless it prints `ok alsa_version_and_open_modes`), and runs
  `tests/check-debug-build.sh` on each runner with its log before the next
  build replaces it. Seven steps prove the checks' failure paths on every
  run: `tests/run.sh` must fail a throwaway `pitch_bend.xfail` holding
  `TODO`, then `B2 B5`, as a bad marker, and `--update` must leave an
  edited `pitch_bend.events` alone, after which the tree must be clean;
  `tests/check-examples.sh` must exit 1 with a FAIL line for a throwaway
  dots-only `examples/ci-canary.orca`, after which the tree must be clean;
  a fake `cli` under `$RUNNER_TEMP`, which runs the release `cli` for every
  file but `tests/patches/pitch_bend.orca` and
  `examples/basics/_midi.orca`, must make `tests/run.sh <fake> pitch_bend`
  and `tests/check-examples.sh <fake>` each exit 1 with its exact FAIL
  line, once when it prints a `runtime error:` line and exits 0 for those
  two (`sanitizer report`, with the report excerpt and summary line), once
  each when it prints `==1==ERROR: AddressSanitizer: planted` (then
  `LeakSanitizer`, then `UndefinedBehaviorSanitizer`) on stderr after the
  real output and exits 0 (`sanitizer report`, with the excerpt), once
  when it sleeps 5 s under a `timeout` shim on `PATH` that runs the real
  `timeout` with 1 s (`cli exited 124 (signal or timeout)`), and once when
  it prints the AddressSanitizer line and kills itself with SIGSEGV
  (`cli exited 139 (signal or timeout)`: the status wins over the report),
  proving `tests/lib.sh`'s shared crash rule and each of its sanitizer
  alternatives in both scripts; when it prints the `UBSAN_OPTIONS` it was
  given and exits 3, both must print
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` with `UBSAN_OPTIONS`
  removed from the environment, and the same with `:report_error_type=1`
  appended when the caller sets that, proving `tests/lib.sh`'s export and
  prepend; with `MODE` unset and `PATH` a directory of links to every tool
  the scripts and the fake use but `timeout` (the step first checks that
  `command -v timeout` finds nothing there), `tests/run.sh` must pass the
  file and `tests/check-examples.sh` must report `0 fail`, each exiting
  0, proving the `command -v timeout` guard; when it exits 3
  after the real output, `tests/check-examples.sh` must fail the file
  (`cli exited 3`) and `tests/run.sh`, under a throwaway
  `pitch_bend.xfail` holding `B2`, must report XFAIL and exit 0, after
  which the tree must be clean;
  `tests/upstream/compare.sh` must exit 1 on a fake `cli` that, for
  `examples/basics/_midi.orca`, after the real output prints the
  AddressSanitizer line on stderr and exits 0, then prints it on stdout
  and exits 0, then exits 3, with `FAIL  examples/basics/_midi.orca (bOrca
  cli sanitizer report)` (with the excerpt the first time) or
  `FAIL  examples/basics/_midi.orca (bOrca cli exited 3)` and
  `upstream check: 43 examples, 42 identical; extras: 1, 1 identical; …;
  1 fail`, proving compare.sh's own crash rule (its `run()`) and that the
  extras ran;
  `tests/check-doc-sync.sh` must exit 1 on a README copy under
  `$RUNNER_TEMP` whose `k/K` row has one cell changed, printing that row,
  and on one with its end marker deleted, as bad markers;
  `tests/check-debug-build.sh` must exit 1 on a program built with plain
  `cc`, naming both sanitizers; and a debug runner built with
  `-DUNIT_TESTS_UNREGISTERED_CANARY`, which defines
  `test_unregistered_canary` with its own prototype and in no list, must
  make it exit 1, naming that test. It checks that `tool` refuses
  `--alsa --portmidi` with `Choose ALSA or PortMidi`, then builds the debug
  `orca`, the debug `orca` with PortMidi (`make debug`) and with ALSA, the
  release `orca` without mouse support, the release `orca` with PortMidi,
  the release `orca` with `-DBORCA_DEBUG_COUNTERS`, without and then with
  ALSA, the release `orca` with ALSA and the appliance `orca`
  (`make appliance`, which is `--alsa --harden --pie`), all with `-Werror`.
  After the first counter build `grep -a` must find `bOrca wakeups:` and
  `bOrca events:` in `build/orca`, after the ALSA one those two,
  `bOrca ALSA:` and `bOrca ALSA input pool:`, and after the plain ALSA
  build and `make appliance` none of the first three, which proves that the
  counters stay out of the shipped builds; the positive greps are the
  canary for the negative ones. The plain ALSA and appliance binaries must
  list `libasound.so.2` as `NEEDED` in `readelf -d`,
  `tests/alsa-guard/shim.c` must compile with `-Wall -Werror`, and
  `tests/alsa-clock/master.c` and `tests/alsa-clock/recv.c` with
  `-Wall -Wextra -Werror`; the appliance binary must also be of type `DYN`
  in `readelf -h` (PIE) and import `__stack_chk_fail` in
  `nm -D --undefined-only` (hardened). Both runners install
  `libasound2-dev` for the ALSA builds.
- **unsigned-char** (`ubuntu-24.04`): runs the golden suite on a release
  `cli`, and the release unit tests, both built with `-funsigned-char`; the
  release runner is an `-flto -s` link, and its run fails unless the log
  holds `ok tick_note_alloc_fail` and `ok realloc_hook_builtin_null`, the
  test that proves `tool`'s `-fno-builtin-realloc` (see
  [The realloc hook](#the-realloc-hook)). It checks that a release runner
  built
  with `-DUNIT_TESTS_CANARY` exits 1 and prints `FAIL unit_check_canary:`,
  and runs `shellcheck -s sh` over `tool`, `tests/run.sh`,
  `tests/check-includes.sh`, `tests/check-nm.sh`, `tests/check-shell-args.sh`,
  `tests/check-debug-build.sh`, `tests/check-doc-sync.sh`,
  `tests/check-examples.sh`, `tests/lib.sh`, `tests/upstream/build-cli.sh`,
  `tests/upstream/compare.sh`, `tests/wakeups/ab.sh`,
  `tests/realloc-shim/check.sh`, `tests/check-version.sh`,
  `tests/alsa-guard/devcheck.sh`, `tests/alsa-contract/devcheck.sh` and
  `tests/alsa-clock/devcheck.sh`.
  `tests/lib.sh` must be in that list: without `-x`, shellcheck follows a
  sourced file only when it is given too.
- **clang** (`ubuntu-24.04`, since v3.0.0): builds the debug
  `cli` and the debug unit runner with `CC=clang` and
  `CFLAGS_EXTRA="-fsanitize=undefined,pointer-overflow
  -fno-sanitize=implicit-conversion,unsigned-integer-overflow"`, runs the
  golden suite on the `cli`, the runner (which must print
  `ok tick_note_alloc_fail`) and `tests/check-debug-build.sh` on both.
  clang's UBSan (its pointer-overflow check, part of
  `-fsanitize=undefined`) reports `NULL + 0` and gcc's does not, so the
  empty-list guard in `tick_release_all` is machine-checked; the explicit
  flags in `CFLAGS_EXTRA` are belt and braces. The job proves it on
  every run: in a `git archive` copy of the tree with that two-line guard
  deleted (its return line is the only line of `tick.c` that carries
  `NULL + 0 is undefined`), the runner must exit non-zero with
  `applying zero offset to null pointer`, which `test_tick_note_empty_list`
  forms on its fresh list. No `-Werror` (the gcc legs own it; a clang
  warning is noted for the maintainer, not fixed or silenced): both clang
  builds keep their output, and a non-fatal step prints each `warning:` line
  as a `::warning::` annotation, with a count. The two
  `-fno-sanitize` checks are off because unsigned wrap is defined C, which
  `prng.h`'s PCG32 relies on, and neither is about UB. `check-nm.sh`,
  `check-includes.sh` and `check-shell-args.sh` call gcc by name, so the
  gcc legs own them, with the examples, doc-sync, upstream and shellcheck
  steps.
- **armhf** (`ubuntu-24.04-arm`, required since v3.0.0):
  the one job that runs the unit tests where `Usz` is 32-bit. It
  cross-compiles each file in the union of `./tool sources cli`,
  `./tool sources test` and `./tool sources readme-gen` with
  `arm-linux-gnueabihf-gcc` and the warning flags copied by hand from
  `tool`, plus, for `sysmisc.c` alone, the `test` target's own
  `-isystem thirdparty` and `-D_POSIX_C_SOURCE=200809L`, which it needs, so
  every other file keeps the strict `-std=c99` check; that step compiles
  `tests/unit/alloc_hook.c` with no define, where
  the file holds only `tests.h`'s declarations. It then builds the release
  unit runner through `tool` with `CC=arm-linux-gnueabihf-gcc` and
  `--static`; on the arm64 image `tool` adds no `-march`, so the runner gets
  every flag `tool` gives `test`, the realloc hook's included, in that
  hook's first static link. `readelf` must show `ELF32`, `ARM` and no
  `NEEDED` entry, so a runner linked by the host's `cc` stops there, and
  `qemu-arm-static build/unit_tests` must exit 0 with
  `ok prng_seed_fixed_vectors` and `ok tick_note_alloc_fail` in its log,
  so the hook's 32-bit `-Werror` build runs too. The first test pins
  `prng_seed`'s first eight outputs for two cells and, on a
  mismatch, prints the observed values before its `FAIL` line. The runner
  is release, not debug (ASan does not run under qemu-user), and the job
  never builds `cli`, so the `R` and `;` pattern `c` goldens stay out of
  it. A unit test whose expected values legitimately differ at 32 bits is
  guarded with `#if SIZE_MAX == 0xffffffff` in its file (`Usz` is
  `size_t`, chosen over `UINTPTR_MAX`, which
  follows pointer width and is optional in C99), with a comment naming the
  `Usz` arithmetic that differs; none does today. A red result fails the
  run.

`tool` adds `CFLAGS_EXTRA` after its own compiler flags, split on spaces. To
reproduce a red `build` or `unsigned-char` step on the uConsole:

```sh
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build -d cli
nice -n 19 taskset -c 0-2 tests/check-debug-build.sh build/debug/cli
nice -n 19 taskset -c 0-2 tests/run.sh build/debug/cli
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build cli
nice -n 19 taskset -c 0-2 tests/run.sh build/cli
nice -n 19 taskset -c 0-2 tests/check-examples.sh build/debug/cli
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build readme-gen
tests/check-doc-sync.sh build/readme-gen
nice -n 19 taskset -c 0-2 tests/check-includes.sh
nice -n 19 taskset -c 0-2 tests/check-nm.sh
nice -n 19 taskset -c 0-2 tests/check-shell-args.sh
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build -d test
UBSAN_OPTIONS=halt_on_error=1 nice -n 19 taskset -c 0-2 build/debug/unit_tests 2>&1 | tee /tmp/unit_tests.log
nice -n 19 taskset -c 0-2 tests/check-debug-build.sh build/debug/unit_tests /tmp/unit_tests.log
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build test
nice -n 19 taskset -c 0-2 build/unit_tests
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build -d --alsa test
UBSAN_OPTIONS=halt_on_error=1 nice -n 19 taskset -c 0-2 build/debug/unit_tests 2>&1 | tee /tmp/unit_tests.log
nice -n 19 taskset -c 0-2 tests/check-debug-build.sh build/debug/unit_tests /tmp/unit_tests.log
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build -d orca
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build --no-mouse orca
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build -d --alsa orca
CFLAGS_EXTRA="-Werror -DBORCA_DEBUG_COUNTERS" nice -n 19 taskset -c 0-2 ./tool build orca
grep -aq 'bOrca wakeups:' build/orca && grep -aq 'bOrca events:' build/orca  # must succeed
CFLAGS_EXTRA="-Werror -DBORCA_DEBUG_COUNTERS" nice -n 19 taskset -c 0-2 ./tool build --alsa orca
grep -aq 'bOrca wakeups:' build/orca && grep -aq 'bOrca ALSA:' build/orca &&
  grep -aq 'bOrca events:' build/orca                                  # must succeed
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build --alsa orca
! grep -aq 'bOrca wakeups:' build/orca && ! grep -aq 'bOrca ALSA:' build/orca &&
  ! grep -aq 'bOrca events:' build/orca                                # none of the three
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 make appliance
! grep -aq 'bOrca wakeups:' build/orca && ! grep -aq 'bOrca ALSA:' build/orca &&
  ! grep -aq 'bOrca events:' build/orca                                # none of the three
```

`-DBORCA_DEBUG_COUNTERS`, passed through `CFLAGS_EXTRA`, is a development
switch for `orca`, with or without `--alsa`. The event loop counts its waits
and, on quit, after the screen closes, prints
`bOrca wakeups: <n> waits, <z> zero-timeout in <s> s` to stderr, which the
wakeup check above compares with `4f349cd`. In an `--alsa` build the ALSA
output also counts the messages it drops on `EAGAIN` or `EINTR`, and prints
`bOrca ALSA: <n> MIDI sends dropped on EAGAIN/EINTR, <m> not sent in all`
before it (`<m>` counts every message that did not go out). In every
build each event list counts the events it drops into its discard slot
when it cannot grow, which only memory exhaustion reaches, and the
switch prints the counts just before the wakeup line as
`bOrca events: <n> dropped (tick <t>, preview <p>, engine <e>)`, where
`<n>` is the total and the others are the tick's, the preview's and the
engine's lists. The switch is never part of a release or appliance build,
and since v3.0.0 CI proves that on every run: the four
grep checks above are its four check steps, and the positive ones show that
`grep -a` finds the literals in an `-flto -s` binary. The MIDI-in counter
line is not grepped: the same define gates all four lines, so a leak shows
in `bOrca wakeups:` and `bOrca events:`.

Building an `--alsa` `orca` is safe while the appliance runs; running one is
not, unless it uses another client name: the appliance router refuses to
route two `bOrca` clients. Run a test build with
`BORCA_ALSA_CLIENT_NAME=bOrca-test` (any name but `bOrca`, at most 63
bytes), a scratch `XDG_CONFIG_HOME` and a copy of a patch, or stop
`borca-tty.service` first, with the maintainer's approval.

On aarch64 plain `char` is already unsigned, so the release `cli` and
release unit-test runs above cover the `unsigned-char` job. The x86_64
`build` leg uses signed `char`; reproduce a failure that only it shows with:

```sh
CFLAGS_EXTRA="-Werror -fsigned-char" nice -n 19 taskset -c 0-2 ./tool build cli
nice -n 19 taskset -c 0-2 tests/run.sh build/cli
CFLAGS_EXTRA="-Werror -fsigned-char" nice -n 19 taskset -c 0-2 ./tool build -d test
UBSAN_OPTIONS=halt_on_error=1 nice -n 19 taskset -c 0-2 build/debug/unit_tests
```

Some steps cannot be reproduced on the uConsole as it stands: the PortMidi
builds, including `-d --portmidi test`, need `libportmidi-dev` (installing
it needs the maintainer's approval), the `armhf` job needs the armhf cross
compiler, and the `clang` job needs clang, which the device lacks
(installing it needs the maintainer's approval too), so its CI runs are the
only clang evidence.
`tests/check-nm.sh` results depend on the architecture, so an x86_64
result from the `build` leg may differ from the uConsole's aarch64 one.
The runners' gcc can warn where the uConsole's gcc 12.2 does not; each job
prints its compiler version in its "Toolchain versions" step.

The device has `/usr/bin/qemu-arm-static` (qemu 7.2; no package owns it)
but no `arm-linux-gnueabihf-gcc`, and its own gcc has no 32-bit ABI (`-m32`
is unknown and `-mabi=ilp32` finds no libgcc), so the `armhf` job's runs
are the only 32-bit evidence. Installing `gcc-arm-linux-gnueabihf` needs
the maintainer's approval; with it, the job's runner build and run should
reproduce it (untested on the device: its kernel uses 16K pages and qemu
7.2). The
cross build replaces the native `build/unit_tests`, so rebuild it with
`./tool build test` afterwards:

```sh
CC=arm-linux-gnueabihf-gcc CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build --static test
nice -n 19 taskset -c 0-2 qemu-arm-static build/unit_tests
CFLAGS_EXTRA=-Werror nice -n 19 taskset -c 0-2 ./tool build test
```
