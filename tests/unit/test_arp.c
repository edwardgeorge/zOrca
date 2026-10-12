// Unit tests for the arpeggiator's Length and Step inputs (spec item I4,
// CAP-16; spine AD-5, AD-7), run through orca_run with a caller-owned store:
// one test per row of the story's I/O matrix, plus a sweep over every
// pattern, range 1-4 and Length `.` and `0`-`z`, at every Step and over 150
// counter bangs.
#include "../../gbuffer.h"
#include "../../opstate.h"
#include "../../sim.h"
#include "../../vmio.h"
#include "tests.h"

// One tick as cli and the TUI run it: clear the marks and the event list,
// then run the VM with the store. (test_sim_state.c's copy is static.)
static void run_tick(Glyph *grid, Mark *marks, Usz height, Usz width,
                     Usz tick, Oevent_list *events, Usz seed,
                     Opstate_store *store) {
  mbuffer_clear(marks, height, width);
  oevent_list_clear(events);
  Orca_run_ctx const ctx = {.opstate = store};
  orca_run(grid, marks, height, width, tick, events, seed, &ctx);
}

// Writes text into the grid at (y, x).
static void put(Glyph *grid, Usz width, Usz y, Usz x, char const *text) {
  memcpy(grid + y * width + x, text, strlen(text));
}

// The 36 glyphs a ; can output, in index order.
static char const arp_glyphs[] = "0123456789abcdefghijklmnopqrstuvwxyz";

// The index of g among arp_glyphs, or 36 when g is not one of them.
static Usz arp_index(Glyph g) {
  for (Usz i = 0; i < 36; ++i)
    if (arp_glyphs[i] == g)
      return i;
  return 36;
}

// The MIDI note number of a note event, or 0 for any other event.
static Usz note_number(Oevent const *e) {
  if (e->any.oevent_type != Oevent_type_midi_note)
    return 0;
  return (Usz)e->midi_note.octave * 12 + e->midi_note.note;
}

// A rig of one ;: a D1 bangs the ; at (1, 1) on every tick, its four inputs
// (Range, Pattern, Length, Step) follow it, and its output lands at (2, 1).
enum { Rig_h = 3, Rig_w = 8 };
typedef struct {
  Glyph grid[Rig_h * Rig_w];
  Mark marks[Rig_h * Rig_w];
  Opstate_store store;
  Oevent_list events;
  Usz tick;
} Rig;

static void rig_init(Rig *r, char const *inputs) {
  memset(r->grid, '.', sizeof r->grid);
  put(r->grid, Rig_w, 0, 0, "D1");
  put(r->grid, Rig_w, 1, 1, ";");
  put(r->grid, Rig_w, 1, 2, inputs);
  opstate_init(&r->store);
  oevent_list_init(&r->events);
  r->tick = 0;
}

static void rig_free(Rig *r) {
  opstate_free(&r->store);
  oevent_list_deinit(&r->events);
}

// Sets input `input` (1 = Range, 2 = Pattern, 3 = Length, 4 = Step) to g.
static void rig_set(Rig *r, Usz input, Glyph g) {
  r->grid[Rig_w + 1 + input] = g;
}

// Bangs the ; n times and writes its outputs to out, NUL-terminated.
static void rig_bangs(Rig *r, Usz n, char *out) {
  for (Usz i = 0; i < n; ++i) {
    run_tick(r->grid, r->marks, Rig_h, Rig_w, r->tick++, &r->events, 0,
             &r->store);
    out[i] = (char)r->grid[2 * Rig_w + 1];
  }
  out[n] = '\0';
}

// CAP-16: ;103 (range 1, Up, Length 3) banged on every tick into $3Ca, the
// C major triad, gives the degrees 0 1 2 0 1 2 ..., and the : plays 36 40 43,
// repeating every 3 bangs. Before I4 the 7-degree range played 36 40 43 48
// 52 55 60.
enum { Cap_h = 4, Cap_w = 13 };
static char const cap16_patch[Cap_h * Cap_w + 1] = ".......D1...."
                                                   "........;103."
                                                   "D1..$3Ca....."
                                                   ".:0..a4......";

void test_arp_cap16_triad(void) {
  Glyph grid[Cap_h * Cap_w];
  Mark marks[Cap_h * Cap_w];
  memcpy(grid, cap16_patch, sizeof grid);
  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);
  static Usz const notes[3] = {36, 40, 43};
  Usz wrong_degree = 0, wrong_note = 0;
  for (Usz t = 0; t < 12; ++t) {
    run_tick(grid, marks, Cap_h, Cap_w, t, &events, 0, &store);
    wrong_degree += grid[2 * Cap_w + 8] != (Glyph)arp_glyphs[t % 3];
    wrong_note += events.count != 1 || events.buffer[0].midi_note.channel != 0 ||
                  note_number(&events.buffer[0]) != notes[t % 3];
  }
  CHECK(wrong_degree == 0);
  CHECK(wrong_note == 0);
  oevent_list_deinit(&events);
  opstate_free(&store);
}

// The cap: Length z at range 4 is 140 degrees, capped at 36, so ;40z. counts
// 0 to z and wraps to 0. Length reads its glyph's value, so case folds: Z is
// z.
void test_arp_cap_36(void) {
  char out[38];
  Rig r;
  rig_init(&r, "40z.");
  rig_bangs(&r, 37, out);
  CHECK(strcmp(out, "0123456789abcdefghijklmnopqrstuvwxyz0") == 0);
  rig_free(&r);
  rig_init(&r, "40Z.");
  rig_bangs(&r, 37, out);
  CHECK(strcmp(out, "0123456789abcdefghijklmnopqrstuvwxyz0") == 0);
  rig_free(&r);
}

// A total of 1 outputs 0 under every pattern: ;121. and ;131. (Up-Down and
// Down-Up, which alternated 0 and 1 there without the guard), and Length 0,
// or a glyph worth 0, which counts as 1, as ;100. does.
void test_arp_total_1(void) {
  static char const *const inputs[] = {"121.", "131.", "100.", "120.",
                                       "130.", "1d0.", "1c*.", "11*."};
  char out[13];
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(inputs); ++i) {
    Rig r;
    rig_init(&r, inputs[i]);
    rig_bangs(&r, 12, out);
    CHECK(strcmp(out, "000000000000") == 0);
    rig_free(&r);
  }
}

// A Length change mid-cycle restarts the counter, as a Pattern or Range
// change does: the next output is 0. The effective Length is compared, so
// `.` and 7 are one Length and switching between them does not restart it.
// Two Lengths that cap to the same total still differ.
void test_arp_length_change(void) {
  char out[8];
  Rig r;
  rig_init(&r, "103.");
  rig_bangs(&r, 2, out);
  CHECK(strcmp(out, "01") == 0);
  rig_set(&r, 3, '4');
  rig_bangs(&r, 5, out);
  CHECK(strcmp(out, "01230") == 0);
  rig_free(&r);

  rig_init(&r, "10..");
  rig_bangs(&r, 3, out);
  CHECK(strcmp(out, "012") == 0);
  rig_set(&r, 3, '7');
  rig_bangs(&r, 1, out);
  CHECK(strcmp(out, "3") == 0);
  rig_set(&r, 3, '.');
  rig_bangs(&r, 1, out);
  CHECK(strcmp(out, "4") == 0);
  rig_free(&r);

  rig_init(&r, "40y.");
  rig_bangs(&r, 5, out);
  CHECK(strcmp(out, "01234") == 0);
  rig_set(&r, 3, 'z');
  rig_bangs(&r, 2, out);
  CHECK(strcmp(out, "01") == 0);
  rig_free(&r);
}

// Step from C: ;103 into $3Ca with its Step written by 1C3 and banged by a D2
// on even ticks reads the clock, 0 2 1 0 2, and plays 36 43 40 36 43. The
// same patch with the C one cell further right, off the Step input, plays
// the counter's 36 40 43 36 40.
enum { Clock_h = 4, Clock_w = 16 };
static char const step_clock_patch[Clock_h * Clock_w + 1] = ".......D2..1C3.."
                                                            "........;103...."
                                                            "D2..$3Ca........"
                                                            ".:0..a4.........";
static char const counter_clock_patch[Clock_h * Clock_w + 1] =
    ".......D2...1C3."
    "........;103...."
    "D2..$3Ca........"
    ".:0..a4.........";

// Runs 10 ticks of a clock patch and returns the notes of the 5 bangs.
static void clock_notes(char const *patch, Usz notes[5], Glyph degrees[5]) {
  Glyph grid[Clock_h * Clock_w];
  Mark marks[Clock_h * Clock_w];
  memcpy(grid, patch, sizeof grid);
  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);
  for (Usz t = 0; t < 10; ++t) {
    run_tick(grid, marks, Clock_h, Clock_w, t, &events, 0, &store);
    if (t % 2 == 0) {
      notes[t / 2] = events.count == 1 ? note_number(&events.buffer[0]) : 0;
      degrees[t / 2] = grid[2 * Clock_w + 8];
    }
  }
  oevent_list_deinit(&events);
  opstate_free(&store);
}

void test_arp_step_from_clock(void) {
  static Usz const step_expected[5] = {36, 43, 40, 36, 43};
  static Usz const counter_expected[5] = {36, 40, 43, 36, 40};
  Usz notes[5];
  Glyph degrees[5];
  clock_notes(step_clock_patch, notes, degrees);
  CHECK(memcmp(notes, step_expected, sizeof notes) == 0);
  CHECK(memcmp(degrees, "02102", sizeof degrees) == 0);
  clock_notes(counter_clock_patch, notes, degrees);
  CHECK(memcmp(notes, counter_expected, sizeof notes) == 0);
  CHECK(memcmp(degrees, "01201", sizeof degrees) == 0);
}

// Step wraps modulo the pattern's period: Up over a total of 3 at Step 4 is
// 1, on every bang.
void test_arp_step_wraps(void) {
  char out[4];
  Rig r;
  rig_init(&r, "1034");
  rig_bangs(&r, 3, out);
  CHECK(strcmp(out, "111") == 0);
  rig_free(&r);
}

// A * on the Step input, as a neighbouring D leaves one, is step 0: banged on
// 3 consecutive ticks (each bang locks the *, so it does not erase itself),
// Down over 7 degrees outputs its step-0 degree, 6, every time, and the
// store holds no entry.
void test_arp_step_star(void) {
  char out[4];
  Rig r;
  rig_init(&r, "11.*");
  rig_bangs(&r, 3, out);
  CHECK(strcmp(out, "666") == 0);
  CHECK(r.grid[Rig_w + 5] == '*');
  CHECK(r.store.count == 0);
  rig_free(&r);
}

// With Step set the store is never looked up (AD-5, AD-7): 100 passes run
// straight on the live store at a banged tick, as a leaky preview would, and
// a clear mid-run, as Ctrl+R or a file open does, leave every tick's grid
// equal to a control's, and neither store holds an entry. The step comes
// from a C. The same harness with the C one cell to the right, so the ; runs
// on its counter, diverges, so the check can fail.
enum { Store_h = 3, Store_w = 10, Store_ticks = 20 };
static char const step_store_patch[Store_h * Store_w + 1] = "D1..1Cc..."
                                                            ".;23......"
                                                            "..........";
static char const counter_store_patch[Store_h * Store_w + 1] = "D1...1Cc.."
                                                               ".;23......"
                                                               "..........";

// Returns the number of ticks whose grid differs from the control's, and
// the entries left in the two stores.
static Usz store_harness(char const *patch, Usz *entries) {
  enum { N = Store_h * Store_w };
  Glyph live[N], control[N], scratch[N];
  Mark marks[N];
  memcpy(live, patch, sizeof live);
  memcpy(control, patch, sizeof control);
  Opstate_store store, control_store;
  opstate_init(&store);
  opstate_init(&control_store);
  Oevent_list events, scratch_events;
  oevent_list_init(&events);
  oevent_list_init(&scratch_events);
  Usz differ = 0;
  for (Usz t = 0; t < Store_ticks; ++t) {
    if (t == 3) {
      for (Usz i = 0; i < 100; ++i) {
        memcpy(scratch, live, sizeof scratch);
        run_tick(scratch, marks, Store_h, Store_w, t, &scratch_events, 0,
                 &store);
      }
    }
    if (t == 11)
      opstate_clear(&store);
    run_tick(live, marks, Store_h, Store_w, t, &events, 0, &store);
    run_tick(control, marks, Store_h, Store_w, t, &events, 0, &control_store);
    differ += memcmp(live, control, sizeof live) != 0;
  }
  *entries = store.count + control_store.count;
  oevent_list_deinit(&events);
  oevent_list_deinit(&scratch_events);
  opstate_free(&store);
  opstate_free(&control_store);
  return differ;
}

void test_arp_step_skips_store(void) {
  Usz entries = 1;
  CHECK(store_harness(step_store_patch, &entries) == 0);
  CHECK(entries == 0);
  CHECK(store_harness(counter_store_patch, &entries) > 0);
  CHECK(entries == 2);
}

// When Step goes back to `.`, the counter resumes where it stopped: the Step
// bangs neither read nor advanced it. A Pattern change made while Step was
// set restarts it at the first lookup after.
void test_arp_step_unset_resumes(void) {
  char out[4];
  Rig r;
  rig_init(&r, "10..");
  rig_bangs(&r, 3, out);
  CHECK(strcmp(out, "012") == 0);
  rig_set(&r, 4, '5');
  rig_bangs(&r, 2, out);
  CHECK(strcmp(out, "55") == 0);
  CHECK(r.store.count == 1);
  rig_set(&r, 4, '.');
  rig_bangs(&r, 2, out);
  CHECK(strcmp(out, "34") == 0);

  rig_set(&r, 4, '5');
  rig_set(&r, 2, '1'); // Down: step 5 of 7 degrees is 1
  rig_bangs(&r, 1, out);
  CHECK(strcmp(out, "1") == 0);
  rig_set(&r, 4, '.');
  rig_bangs(&r, 2, out);
  CHECK(strcmp(out, "65") == 0);
  rig_free(&r);
}

// Each periodic pattern's first two periods at the small totals I4 makes
// reachable, 2 to 6 (Length 2-6, range 1), written out from the patterns'
// definitions, not from sim.c. The length of each row is twice the README's
// Period column, which period_of restates. Bounce moves over only the lowest
// max(1, t / 3) degrees, so below a total of 6 it outputs only 0. Random has
// no period and is left to the sweep.
static char const small_patterns[] = "0123456789abd";
static char const *const small_totals[13][5] = {
    {"0101", "012012", "01230123", "0123401234", "012345012345"}, // Up
    {"1010", "210210", "32103210", "4321043210", "543210543210"}, // Down
    {"0101", "01210121", "012321012321", "0123432101234321",
     "01234543210123454321"}, // Up-Down
    {"1010", "21012101", "321012321012", "4321012343210123",
     "54321012345432101234"}, // Down-Up
    {"01100110", "012210012210", "0123321001233210", "01234432100123443210",
     "012345543210012345543210"}, // Up-Down+
    {"10011001", "210012210012", "3210012332100123", "43210012344321001234",
     "543210012345543210012345"}, // Down-Up+
    {"1010", "201201", "30213021", "4031240312", "504132504132"}, // Converge
    {"1010", "102102", "21302130", "2130421304", "324150324150"}, // Diverge
    {"0101", "02120212", "031323031323", "0414243404142434",
     "05152535450515253545"}, // Pinky Up
    {"0101", "01020102", "010203010203", "0102030401020304",
     "01020304050102030405"}, // Thumb Up
    {"01100110", "012210012210", "0123321001233210", "01234432100123443210",
     "012345543210012345543210"}, // Up-Down Alt, the same as Up-Down+
    {"10011001", "210012210012", "3210012332100123", "43210012344321001234",
     "543210012345543210012345"}, // Down-Up Alt, the same as Down-Up+
    {"00000000", "00000000", "00000000", "00000000",
     "0110011001100110"}, // Bounce
};

// The README's Period column.
static Usz period_of(Glyph pattern, Usz t) {
  switch (pattern) {
  case '0': case '1': case '6': case '7':
    return t;
  case '2': case '3': case '8': case '9':
    return 2 * t - 2;
  case '4': case '5': case 'a': case 'b':
    return 2 * t;
  default: // 'd', Bounce
    return 4 * (t / 3 > 1 ? t / 3 : 1);
  }
}

// On its counter, each pattern gives the row; at Step v it gives the row's
// v-th glyph, so Step wraps modulo the period.
void test_arp_small_totals(void) {
  Usz bad_length = 0, counter_wrong = 0, step_wrong = 0;
  char out[32];
  for (Usz p = 0; small_patterns[p]; ++p) {
    for (Usz t = 2; t <= 6; ++t) {
      char const *want = small_totals[p][t - 2];
      Usz n = strlen(want);
      bad_length += n != 2 * period_of((Glyph)small_patterns[p], t);
      char inputs[5] = {'1', small_patterns[p], (char)('0' + t), '.', '\0'};
      Rig r;
      rig_init(&r, inputs);
      rig_bangs(&r, n, out);
      counter_wrong += strcmp(out, want) != 0;
      for (Usz v = 0; v < n; ++v) {
        rig_set(&r, 4, (Glyph)arp_glyphs[v]);
        rig_bangs(&r, 1, out);
        step_wrong += out[0] != want[v];
      }
      rig_free(&r);
    }
  }
  CHECK(bad_length == 0);
  CHECK(counter_wrong == 0);
  CHECK(step_wrong == 0);
}

// The sweep: for one pattern and range, a ; for each Length `.` and `0`-`z`,
// each in its own 6-column group so no D bangs a Step input, run 150 ticks on
// its counter, then once at each Step `z` to `0` on a cleared store.
enum {
  Sweep_lengths = 37,
  Sweep_bangs = 150,
  Sweep_steps = 36,
  Sweep_h = 3,
  Sweep_w = Sweep_lengths * 6
};
static char const sweep_lengths[Sweep_lengths + 1] =
    ".0123456789abcdefghijklmnopqrstuvwxyz";
static char const sweep_patterns[] = "0123456789abcd";

typedef struct {
  Glyph counter[Sweep_lengths][Sweep_bangs];
  Glyph step[Sweep_lengths][Sweep_steps];
  Usz step_entries; // the store's entries after the Step runs
} Sweep;

static void sweep_grid(Glyph *grid, Glyph range, Glyph pattern, Glyph step) {
  memset(grid, '.', Sweep_h * Sweep_w);
  for (Usz i = 0; i < Sweep_lengths; ++i) {
    Glyph *top = grid + i * 6;
    Glyph *row = grid + Sweep_w + i * 6 + 1;
    top[0] = 'D';
    top[1] = '1';
    row[0] = ';';
    row[1] = range;
    row[2] = pattern;
    row[3] = (Glyph)sweep_lengths[i];
    row[4] = step;
  }
}

static void sweep_run(Glyph range, Glyph pattern, Sweep *s) {
  Glyph grid[Sweep_h * Sweep_w];
  Mark marks[Sweep_h * Sweep_w];
  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);
  sweep_grid(grid, range, pattern, '.');
  for (Usz t = 0; t < Sweep_bangs; ++t) {
    run_tick(grid, marks, Sweep_h, Sweep_w, t, &events, 7, &store);
    for (Usz i = 0; i < Sweep_lengths; ++i)
      s->counter[i][t] = grid[2 * Sweep_w + i * 6 + 1];
  }
  // The Steps run from z down to 0, so a ; that ignored Step and ran on its
  // counter would not pass for one that reads it.
  opstate_clear(&store);
  for (Usz k = 0; k < Sweep_steps; ++k) {
    Usz v = Sweep_steps - 1 - k;
    sweep_grid(grid, range, pattern, (Glyph)arp_glyphs[v]);
    run_tick(grid, marks, Sweep_h, Sweep_w, k, &events, 7, &store);
    for (Usz i = 0; i < Sweep_lengths; ++i)
      s->step[i][v] = grid[2 * Sweep_w + i * 6 + 1];
  }
  s->step_entries = store.count;
  oevent_list_deinit(&events);
  opstate_free(&store);
}

// min(Length x Range, 36), with Length `.` as 7 and 0 as 1.
static Usz sweep_total(Glyph range, Glyph length) {
  Usz r = arp_index(range);
  Usz l = length == '.' ? 7 : arp_index(length);
  if (l == 0)
    l = 1;
  return l * r > 36 ? 36 : l * r;
}

// Every output is one of the 36 glyphs and below the total, for every
// pattern, range 1-4 and Length, at every Step and on 150 counter bangs. The
// glyph itself is checked against the table, so removing the 36 cap fails
// here in release too, where glyph_of does not assert; removing the total-1
// guard fails on Up-Down and Down-Up at Length 0 and 1, range 1.
void test_arp_bounds(void) {
  Sweep s;
  Usz runs = 0, outputs = 0, out_of_range = 0;
  for (Usz p = 0; sweep_patterns[p]; ++p) {
    for (Glyph range = '1'; range <= '4'; ++range) {
      sweep_run(range, (Glyph)sweep_patterns[p], &s);
      ++runs;
      for (Usz i = 0; i < Sweep_lengths; ++i) {
        Usz total = sweep_total(range, (Glyph)sweep_lengths[i]);
        for (Usz t = 0; t < Sweep_bangs; ++t, ++outputs)
          out_of_range += arp_index(s.counter[i][t]) >= total;
        for (Usz v = 0; v < Sweep_steps; ++v, ++outputs)
          out_of_range += arp_index(s.step[i][v]) >= total;
      }
    }
  }
  CHECK(runs == 14 * 4);
  CHECK(outputs == 14 * 4 * Sweep_lengths * (Sweep_bangs + Sweep_steps));
  CHECK(out_of_range == 0);
}

// Step v feeds the degree function exactly as the counter's v-th bang does,
// for every pattern (Random too: both hash the seed and the step), range and
// Length, and no Step run creates a store entry.
void test_arp_step_is_counter_step(void) {
  Sweep s;
  Usz differ = 0, entries = 0;
  for (Usz p = 0; sweep_patterns[p]; ++p) {
    for (Glyph range = '1'; range <= '4'; ++range) {
      sweep_run(range, (Glyph)sweep_patterns[p], &s);
      for (Usz i = 0; i < Sweep_lengths; ++i)
        differ += memcmp(s.step[i], s.counter[i], sizeof s.step[i]) != 0;
      entries += s.step_entries;
    }
  }
  CHECK(differ == 0);
  CHECK(entries == 0);
}

// Length `.` is 7 and Length 0 is 1, on the counter and at every Step, for
// every pattern and range: with x+3 and x+4 empty a ; plays as before I4.
void test_arp_length_dot_and_zero(void) {
  Sweep s;
  Usz dot_differs = 0, zero_differs = 0;
  enum { Dot = 0, Zero = 1, One = 2, Seven = 8 };
  for (Usz p = 0; sweep_patterns[p]; ++p) {
    for (Glyph range = '1'; range <= '4'; ++range) {
      sweep_run(range, (Glyph)sweep_patterns[p], &s);
      dot_differs +=
          memcmp(s.counter[Dot], s.counter[Seven], sizeof s.counter[0]) != 0 ||
          memcmp(s.step[Dot], s.step[Seven], sizeof s.step[0]) != 0;
      zero_differs +=
          memcmp(s.counter[Zero], s.counter[One], sizeof s.counter[0]) != 0 ||
          memcmp(s.step[Zero], s.step[One], sizeof s.step[0]) != 0;
    }
  }
  CHECK(sweep_lengths[Dot] == '.' && sweep_lengths[Zero] == '0' &&
        sweep_lengths[One] == '1' && sweep_lengths[Seven] == '7');
  CHECK(dot_differs == 0);
  CHECK(zero_differs == 0);
}
