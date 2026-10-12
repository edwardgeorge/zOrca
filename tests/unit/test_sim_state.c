// Unit tests for the per-cell operator state of &, ; and r (spec item B1,
// spine AD-4 to AD-8), run through orca_run with a caller-owned store.
#include "../../gbuffer.h"
#include "../../opstate.h"
#include "../../sim.h"
#include "../../vmio.h"
#include "tests.h"

// One tick as cli and the TUI run it: clear the marks and the event list,
// then run the VM with the store.
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

// Each block of n outputs, from the first, is a permutation of
// [min, min + n). Outputs are the glyphs 0-9 and a-z.
static bool blocks_are_permutations(Glyph const *outputs, Usz count,
                                    Usz min, Usz n) {
  if (n == 0 || count % n != 0)
    return false;
  for (Usz b = 0; b < count; b += n) {
    bool seen[36] = {0};
    for (Usz i = b; i < b + n; ++i) {
      Glyph g = outputs[i];
      Usz v;
      if (g >= '0' && g <= '9')
        v = (Usz)(g - '0');
      else if (g >= 'a' && g <= 'z')
        v = (Usz)(g - 'a') + 10;
      else
        return false;
      if (v < min || v >= min + n || seen[v])
        return false;
      seen[v] = true;
    }
  }
  return true;
}

// The number of outputs equal to the one before them.
static Usz repeats(Glyph const *outputs, Usz count) {
  Usz n = 0;
  for (Usz i = 1; i < count; ++i)
    n += outputs[i] == outputs[i - 1];
  return n;
}

// The events of two lists match, field by field. Only note events are
// expected here; any other type counts as a mismatch.
static bool events_equal(Oevent_list const *a, Oevent_list const *b) {
  if (a->count != b->count)
    return false;
  for (Usz i = 0; i < a->count; ++i) {
    Oevent_midi_note const *p = &a->buffer[i].midi_note;
    Oevent_midi_note const *q = &b->buffer[i].midi_note;
    if (p->oevent_type != Oevent_type_midi_note ||
        q->oevent_type != Oevent_type_midi_note || p->channel != q->channel ||
        p->octave != q->octave || p->note != q->note ||
        p->velocity != q->velocity || p->duration != q->duration ||
        p->mono != q->mono)
      return false;
  }
  return true;
}

// The CAP-6 overflow fixture: a 300x80 grid with &0z12 at every 7th cell
// from cell 1, skipping the positions where the five glyphs do not fit in
// their row, so cell 4096, (13, 196), holds one. The baseline debug build
// reported a global-buffer-overflow there; with the store, 256 ticks run
// clean under ASan and UBSan, and every & outputs on every tick what a lone
// in-range &0z12 outputs. Each output lands on an empty cell (cell index
// 0 mod 7), so the copies never disturb one another. Since I3, rate 1 is a
// 4-tick cycle that samples the sine at steps 0, 32, 64 and 96: 0, f, z, f.
// So that the comparison can still fail, the & at cell 4096 is banged once,
// at tick 5 (odd, so the banged reference is one sample off and no two of
// the four samples 0 f z f coincide; at an even offset some would), with a
// * in its own output cell, 4396, which it overwrites that tick; its left and upper
// neighbours belong to the & at 3795. It is checked against a second lone
// reference banged the same way at the same tick, every other & against
// the unbanged one.
void test_sim_bouncer_overflow_fixture(void) {
  enum {
    H = 80,
    W = 300,
    Ticks = 256,
    Ref_w = 5,
    Bang_tick = 5,
    Banged = 13 * W + 196
  };
  Glyph *grid = malloc(H * W * sizeof *grid);
  Mark *marks = malloc(H * W * sizeof *marks);
  CHECK(grid != NULL && marks != NULL);
  if (!grid || !marks) {
    free(grid);
    free(marks);
    return;
  }
  memset(grid, '.', H * W);
  Usz placed = 0;
  for (Usz c = 1; c < H * W; c += 7) {
    if (c % W + 5 > W)
      continue;
    memcpy(grid + c, "&0z12", 5);
    ++placed;
  }
  CHECK(placed == 3383);
  CHECK(grid[13 * W + 196] == '&' && 13 * W + 196 == 4096);

  Glyph ref[2 * Ref_w], banged_ref[2 * Ref_w];
  Mark ref_marks[2 * Ref_w];
  memcpy(ref, "&0z12.....", sizeof ref);
  memcpy(banged_ref, ref, sizeof banged_ref);

  Opstate_store store, ref_store, banged_ref_store;
  opstate_init(&store);
  opstate_init(&ref_store);
  opstate_init(&banged_ref_store);
  Oevent_list events;
  oevent_list_init(&events);
  Usz wrong = 0, checked = 0, shifted = 0;
  bool seen[128] = {0};
  for (Usz tick = 0; tick < Ticks; ++tick) {
    if (tick == Bang_tick) {
      grid[Banged + W] = '*';
      banged_ref[Ref_w] = '*';
    }
    run_tick(grid, marks, H, W, tick, &events, 0, &store);
    run_tick(ref, ref_marks, 2, Ref_w, tick, &events, 0, &ref_store);
    run_tick(banged_ref, ref_marks, 2, Ref_w, tick, &events, 0,
             &banged_ref_store);
    Glyph want = ref[Ref_w]; // the cell under the reference &
    Glyph want_banged = banged_ref[Ref_w];
    seen[want & 0x7f] = true;
    shifted += want_banged != want;
    for (Usz c = 1; c + W < H * W; c += 7) {
      if (c % W + 5 > W)
        continue;
      ++checked;
      wrong += grid[c + W] != (c == Banged ? want_banged : want);
    }
  }
  Usz distinct = 0;
  for (Usz i = 0; i < 128; ++i)
    distinct += seen[i];
  CHECK(wrong == 0);
  CHECK(checked > 0);
  CHECK(distinct == 3); // 0, f, z: the reference output really moves
  CHECK(shifted == Ticks - Bang_tick); // and the bang really moved the anchor
  CHECK(store.count == placed); // one entry per &
  CHECK(events.count == 0);
  oevent_list_deinit(&events);
  opstate_free(&store);
  opstate_free(&ref_store);
  opstate_free(&banged_ref_store);
  free(grid);
  free(marks);
}

// The rate-4 table (I3, CAP-15): with the range 0-z, shape N at tick e is
// waveforms[N][8 e] (sim.c), and rate 4 is a 16-tick cycle, so ticks 16-63
// repeat ticks 0-15. The table's quirks (s up against r down in the
// triangle, the saw's uneven steps) are the table's, kept on purpose.
static char const *const rate4_rows[8] = {
    "048cgkoszrnjfb73", // 0 triangle
    "zvrnjfb708cgkosw", // 1 inverted triangle
    "0138fowyzyuof831", // 2 sine
    "zywrkb31015bkrwy", // 3 inverted sine
    "00000000zzzzzzzz", // 4 square
    "zzzzzzzz00000000", // 5 inverted square
    "02468acegikmpsux", // 6 saw
    "zxvtrpnljhfda752", // 7 inverted saw
};

// Every shape at rate 4 over 0-z, laid out as tests/patches/bouncer_rate4.orca
// is: eight &0z4N rows, each over an empty row, for 64 ticks. Tick t
// outputs rate4_rows[N][t % 16].
void test_sim_bouncer_rate4_shapes(void) {
  enum { H = 16, W = 7, Ticks = 64 };
  Glyph grid[H * W];
  Mark marks[H * W];
  memset(grid, '.', sizeof grid);
  for (Usz n = 0; n < 8; ++n) {
    put(grid, W, 2 * n, 1, "&0z4");
    grid[2 * n * W + 5] = (Glyph) "01234567"[n];
  }
  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);
  Usz wrong = 0;
  for (Usz tick = 0; tick < Ticks; ++tick) {
    run_tick(grid, marks, H, W, tick, &events, 0, &store);
    for (Usz n = 0; n < 8; ++n)
      wrong += grid[(2 * n + 1) * W + 1] != rate4_rows[n][tick % 16];
  }
  CHECK(wrong == 0);
  CHECK(store.count == 8);
  CHECK(events.count == 0);
  oevent_list_deinit(&events);
  opstate_free(&store);
}

// One & at (0, 1) of a 2x7 grid, its output read from the cell below it.
enum { B_h = 2, B_w = 7, B_y = 0, B_x = 1, B_out = (B_y + 1) * B_w + B_x };

static void bouncer_grid(Glyph *grid, char const *cell) {
  memset(grid, '.', B_h * B_w);
  put(grid, B_w, B_y, B_x, cell);
}

// Runs one tick on the & grid and returns the &'s output.
static Glyph bouncer_tick(Glyph *grid, Mark *marks, Usz tick,
                          Oevent_list *events, Opstate_store *store) {
  run_tick(grid, marks, B_h, B_w, tick, events, 0, store);
  return grid[B_out];
}

// The anchor (I3): &0z36 is a 12-tick saw, 0 2 5 8 a d g i l p s w. A bang
// moves the anchor to its tick, which outputs phase 0; an anchor above the
// tick, as after an undo, gives the floor-mod phase; opstate_reset_anchors
// puts the anchor back to 0 and touches no other operator's state; a new &
// lines up with tick 0 however late it first runs; and a rate or a shape
// change keeps the anchor. P = 12 on purpose: 2^32 and 2^64 are both 4 mod
// 12, so an unsigned wrap gives phase 9 (p) where floor-mod gives 5 (d),
// on every Usz width, and a truncating signed % gives a negative index.
void test_sim_bouncer_anchor(void) {
  static char const saw12[13] = "0258adgilpsw";
  Glyph grid[B_h * B_w];
  Mark marks[B_h * B_w];
  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);

  // Unbanged from tick 0: the saw, over and over.
  bouncer_grid(grid, "&0z36");
  Usz wrong = 0;
  for (Usz tick = 0; tick < 20; ++tick)
    wrong +=
        bouncer_tick(grid, marks, tick, &events, &store) != saw12[tick % 12];
  CHECK(wrong == 0);

  // A bang at tick 20, from the output cell below the &, which the & then
  // overwrites: tick 20 outputs phase 0, and tick 20 + e outputs out(e).
  grid[B_out] = '*';
  CHECK(bouncer_tick(grid, marks, 20, &events, &store) == '0');
  for (Usz e = 1; e <= 10; ++e)
    wrong += bouncer_tick(grid, marks, 20 + e, &events, &store) != saw12[e];
  CHECK(wrong == 0);
  CHECK(store.count == 1);

  // The anchor, 20, above the tick, 13: floor-mod gives 5, d. An unsigned
  // wrap would give 9, p, and ignoring the anchor 1, 2.
  CHECK(bouncer_tick(grid, marks, 13, &events, &store) == 'd');

  // A tick a whole period below the anchor, 8: floor-mod gives 0, so the
  // inverted saw outputs End, z. Without the trailing % period, elapsed
  // would be 12 and the index 128, past the table (the literal's NUL reads
  // as 0, which would output Start).
  grid[B_y * B_w + B_x + 4] = '7';
  CHECK(bouncer_tick(grid, marks, 8, &events, &store) == 'z');
  grid[B_y * B_w + B_x + 4] = '6';

  // Reset anchors: the & is back at tick 0, so tick 25 outputs out(25 mod
  // 12), where the anchor of 20 would give out(5), d. The ; and r entries
  // beside it keep their state.
  Opstate_entry *e = opstate_lookup(&store, 1, 5, ';');
  CHECK(e != NULL);
  if (e) {
    e->u.arp.initialized = true;
    e->u.arp.step_counter = 9;
  }
  e = opstate_lookup(&store, 1, 6, 'r');
  CHECK(e != NULL);
  if (e) {
    e->u.random.initialized = true;
    e->u.random.current_index = 2;
  }
  opstate_reset_anchors(&store);
  CHECK(bouncer_tick(grid, marks, 25, &events, &store) == '2');
  e = opstate_lookup(&store, 1, 5, ';');
  CHECK(e != NULL && e->u.arp.initialized && e->u.arp.step_counter == 9);
  e = opstate_lookup(&store, 1, 6, 'r');
  CHECK(e != NULL && e->u.random.initialized &&
        e->u.random.current_index == 2);
  CHECK(store.count == 3);

  // A bang moves the anchor whatever the rate: &0z.6 banged at tick 5
  // outputs phase 0, and at rate 1 from tick 6 counts from 5 (phase 1 of
  // the 4-tick saw, 8), not from 0 (phase 2, g).
  opstate_clear(&store);
  bouncer_grid(grid, "&0z.6");
  for (Usz tick = 0; tick < 5; ++tick)
    bouncer_tick(grid, marks, tick, &events, &store);
  grid[B_out] = '*';
  CHECK(bouncer_tick(grid, marks, 5, &events, &store) == '0');
  grid[B_y * B_w + B_x + 3] = '1';
  CHECK(bouncer_tick(grid, marks, 6, &events, &store) == '8');

  // A late first visit: a new & that first runs at tick 37 is at phase 1.
  opstate_clear(&store);
  bouncer_grid(grid, "&0z36");
  CHECK(bouncer_tick(grid, marks, 37, &events, &store) == '2');

  // A rate change keeps the anchor: &0z36 for ticks 0-9, then &0z46 (the
  // 16-tick saw) for ticks 10-12, which outputs the rate-4 saw at 10, 11
  // and 12. A restart would give 0 2 4.
  opstate_clear(&store);
  bouncer_grid(grid, "&0z36");
  for (Usz tick = 0; tick < 10; ++tick)
    bouncer_tick(grid, marks, tick, &events, &store);
  grid[B_y * B_w + B_x + 3] = '4';
  CHECK(bouncer_tick(grid, marks, 10, &events, &store) == 'k');
  CHECK(bouncer_tick(grid, marks, 11, &events, &store) == 'm');
  CHECK(bouncer_tick(grid, marks, 12, &events, &store) == 'p');

  // A shape change keeps it too: &0z37 at ticks 10 and 11 is the inverted
  // saw at phases 10 and 11, 7 and 3. A restart would give z x.
  opstate_clear(&store);
  bouncer_grid(grid, "&0z36");
  for (Usz tick = 0; tick < 10; ++tick)
    bouncer_tick(grid, marks, tick, &events, &store);
  grid[B_y * B_w + B_x + 4] = '7';
  CHECK(bouncer_tick(grid, marks, 10, &events, &store) == '7');
  CHECK(bouncer_tick(grid, marks, 11, &events, &store) == '3');

  CHECK(events.count == 0);
  oevent_list_deinit(&events);
  opstate_free(&store);
}

// A rate worth 0 (., 0, or a * the & locks as an input) holds the phase-0
// output, Start for an even shape and End for an odd one; a partial and a
// descending range are scaled with round-half-up; a shape above 7, or .,
// is shape 0; and an & whose Start is empty writes nothing and keeps no
// entry. The rows sit one over an empty row, as in bouncer_shapes.orca.
void test_sim_bouncer_hold_and_range(void) {
  enum { H = 20, W = 7, Ticks = 16, Rows = 10 };
  static char const *const cells[Rows] = {
      "&3a01", "&3a.1", "&3a00", "&3a*0", "&3a46",
      "&a346", "&0z4z", "&0z4.", "&0z40", "&.z46",
  };
  char const *const want[Rows] = {
      "aaaaaaaaaaaaaaaa", "aaaaaaaaaaaaaaaa", "3333333333333333",
      "3333333333333333", "334455566777899a", "aa99888776665443",
      rate4_rows[0],      rate4_rows[0],      rate4_rows[0],
      "................",
  };
  Glyph grid[H * W];
  Mark marks[H * W];
  memset(grid, '.', sizeof grid);
  for (Usz r = 0; r < Rows; ++r)
    put(grid, W, 2 * r, 1, cells[r]);
  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);
  Usz wrong = 0;
  for (Usz tick = 0; tick < Ticks; ++tick) {
    run_tick(grid, marks, H, W, tick, &events, 0, &store);
    for (Usz r = 0; r < Rows; ++r)
      wrong += grid[(2 * r + 1) * W + 1] != want[r][tick];
  }
  CHECK(wrong == 0);
  CHECK(store.count == Rows - 1); // &.z46 returned before its lookup

  // The longest cycle, rate z, is 140 ticks, and its last tick reaches z.
  // An uppercase rate glyph reads as its lowercase one (index_of folds
  // case), so &0zG6 outputs what &0zg6 does, tick by tick.
  enum { Longest = 140 };
  Glyph one[B_h * B_w], upper[B_h * B_w], lower[B_h * B_w];
  Mark one_marks[B_h * B_w];
  Glyph out_z[2 * Longest];
  opstate_clear(&store);
  bouncer_grid(one, "&0zz6");
  for (Usz tick = 0; tick < 2 * Longest; ++tick)
    out_z[tick] = bouncer_tick(one, one_marks, tick, &events, &store);
  wrong = 0;
  for (Usz tick = 0; tick < Longest; ++tick)
    wrong += out_z[tick + Longest] != out_z[tick];
  CHECK(wrong == 0);
  CHECK(out_z[0] == '0' && out_z[Longest - 1] == 'z');
  Opstate_store lower_store;
  opstate_init(&lower_store);
  opstate_clear(&store);
  bouncer_grid(upper, "&0zG6");
  bouncer_grid(lower, "&0zg6");
  wrong = 0;
  for (Usz tick = 0; tick < 70; ++tick)
    wrong += bouncer_tick(upper, one_marks, tick, &events, &store) !=
             bouncer_tick(lower, one_marks, tick, &events, &lower_store);
  CHECK(wrong == 0);
  CHECK(lower[B_out] == '2'); // tick 69 of the 64-tick saw: phase 5
  opstate_free(&lower_store);
  CHECK(events.count == 0);
  oevent_list_deinit(&events);
  opstate_free(&store);
}

// ; and r at row 70 of a 265-wide grid run as they do near the origin. The
// far ; sits at cell 18,550, (70, 0); the far r at (70, 21). Each is driven
// by the same D two rows above it as its reference copy near the origin.
// The far ; outputs the reference's glyph sequence over 32 ticks; each r
// emits one full permutation of 0-3 per 4 bangs.
void test_sim_far_cell(void) {
  enum { H = 73, W = 265, Ticks = 32 };
  Glyph *grid = malloc(H * W * sizeof *grid);
  Mark *marks = malloc(H * W * sizeof *marks);
  CHECK(grid != NULL && marks != NULL);
  if (!grid || !marks) {
    free(grid);
    free(marks);
    return;
  }
  memset(grid, '.', H * W);
  // Reference copies near the origin.
  put(grid, W, 0, 0, "D2");
  put(grid, W, 2, 0, ";23");
  put(grid, W, 0, 21, "D1");
  put(grid, W, 2, 20, "0r3");
  // Far copies, rows 68 and 70.
  put(grid, W, 68, 0, "D2");
  put(grid, W, 70, 0, ";23");
  put(grid, W, 68, 21, "D1");
  put(grid, W, 70, 20, "0r3");
  CHECK(70 * W + 0 == 18550);

  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);
  Glyph arp_ref[Ticks], arp_far[Ticks], r_ref[Ticks], r_far[Ticks];
  for (Usz tick = 0; tick < Ticks; ++tick) {
    run_tick(grid, marks, H, W, tick, &events, 0, &store);
    arp_ref[tick] = grid[3 * W + 0];
    arp_far[tick] = grid[71 * W + 0];
    r_ref[tick] = grid[3 * W + 21];
    r_far[tick] = grid[71 * W + 21];
  }
  CHECK(memcmp(arp_ref, arp_far, sizeof arp_ref) == 0);
  bool seen[128] = {0};
  Usz distinct = 0;
  for (Usz i = 0; i < Ticks; ++i) {
    distinct += !seen[arp_ref[i] & 0x7f];
    seen[arp_ref[i] & 0x7f] = true;
  }
  CHECK(distinct >= 3 && !seen['.']); // the reference really arpeggiates
  CHECK(blocks_are_permutations(r_ref, Ticks, 0, 4));
  CHECK(blocks_are_permutations(r_far, Ticks, 0, 4));
  CHECK(memcmp(r_ref, r_far, sizeof r_ref) != 0); // the cell feeds the seed
  CHECK(store.count == 4);
  oevent_list_deinit(&events);
  opstate_free(&store);
  free(grid);
  free(marks);
}

// Two r with different ranges in one patch (as tests/patches/r_shared.orca),
// banged every tick: each emits a full permutation of its range per cycle,
// and the 0-3 r emits exactly what it emits with no other r in the patch.
void test_sim_two_r_permutations(void) {
  enum { H = 4, W = 12, Ticks = 40 };
  static char const both[H * W + 1] = "..D1.....D1."
                                      "............"
                                      ".0r3....0r9."
                                      "............";
  static char const alone[H * W + 1] = "..D1........"
                                       "............"
                                       ".0r3........"
                                       "............";
  Glyph grid[H * W], grid_alone[H * W];
  Mark marks[H * W];
  memcpy(grid, both, sizeof grid);
  memcpy(grid_alone, alone, sizeof grid_alone);
  Opstate_store store, store_alone;
  opstate_init(&store);
  opstate_init(&store_alone);
  Oevent_list events;
  oevent_list_init(&events);
  Glyph small[Ticks], large[Ticks], small_alone[Ticks];
  for (Usz tick = 0; tick < Ticks; ++tick) {
    run_tick(grid, marks, H, W, tick, &events, 0, &store);
    run_tick(grid_alone, marks, H, W, tick, &events, 0, &store_alone);
    small[tick] = grid[3 * W + 2];
    large[tick] = grid[3 * W + 9];
    small_alone[tick] = grid_alone[3 * W + 2];
  }
  CHECK(blocks_are_permutations(small, Ticks, 0, 4));
  CHECK(blocks_are_permutations(large, Ticks, 0, 10));
  CHECK(memcmp(small, small_alone, sizeof small) == 0);
  // The r_single and r_shared repros (B6): no value twice in a row.
  CHECK(repeats(small, Ticks) == 0);
  CHECK(repeats(large, Ticks) == 0);
  // A different seed gives a different sequence, and is still a permutation.
  memcpy(grid, both, sizeof grid);
  opstate_clear(&store);
  Glyph seeded[Ticks];
  for (Usz tick = 0; tick < Ticks; ++tick) {
    run_tick(grid, marks, H, W, tick, &events, 7, &store);
    seeded[tick] = grid[3 * W + 9];
  }
  CHECK(blocks_are_permutations(seeded, Ticks, 0, 10));
  CHECK(memcmp(seeded, large, sizeof seeded) != 0);
  oevent_list_deinit(&events);
  opstate_free(&store);
  opstate_free(&store_alone);
}

// Lowercase r after B6 (CAP-11). Each test bangs one r at (2, 2) of a 4x6
// grid, the cell r_single's r sits in, from a D1 two rows above it, and
// reads its output from the cell below it.
enum { R_h = 4, R_w = 6, R_y = 2, R_x = 2, R_out = (R_y + 1) * R_w + R_x };

static void r_grid(Glyph *grid, char min, char max) {
  memcpy(grid,
         "..D1.."
         "......"
         ".?r?.."
         "......",
         R_h * R_w);
  grid[R_y * R_w + R_x - 1] = (Glyph)min;
  grid[R_y * R_w + R_x + 1] = (Glyph)max;
}

// Runs min r max for count ticks under seed on a fresh store and records
// each output. A non-zero alt_max replaces the max on every odd tick, so the
// range changes on every bang.
static void run_r(char min, char max, char alt_max, Usz seed, Usz count,
                  Glyph *outputs) {
  Glyph grid[R_h * R_w];
  Mark marks[R_h * R_w];
  r_grid(grid, min, max);
  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);
  for (Usz tick = 0; tick < count; ++tick) {
    if (alt_max)
      grid[R_y * R_w + R_x + 1] = (Glyph)(tick % 2 ? alt_max : max);
    run_tick(grid, marks, R_h, R_w, tick, &events, seed, &store);
    outputs[tick] = grid[R_out];
  }
  oevent_list_deinit(&events);
  opstate_free(&store);
}

static char const r_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
static Usz const r_seeds[] = {0, 1, 7};

// No value twice in a row, over every range from 0r1 to 0rz and three
// seeds, and every full bag a permutation of min..max, max included. 0r1
// repeated about one bang in four before B6.
void test_sim_r_no_repeat(void) {
  enum { Bangs = 1000 };
  Glyph out[Bangs];
  Usz cases = 0;
  for (Usz s = 0; s < sizeof r_seeds / sizeof r_seeds[0]; ++s) {
    for (Usz max = 1; max < 36; ++max) {
      Usz n = max + 1;
      run_r('0', r_digits[max], 0, r_seeds[s], Bangs, out);
      CHECK(repeats(out, Bangs) == 0);
      CHECK(blocks_are_permutations(out, Bangs / n * n, 0, n));
      ++cases;
    }
  }
  CHECK(cases == 105);
}

// A range change rebuilds the bag, which must not start with the value just
// sent: the max alternates 3 and 4 on every bang. A swap only at the end of
// a bag repeated about one bang in five here.
void test_sim_r_range_change(void) {
  enum { Bangs = 1000 };
  Glyph out[Bangs];
  for (Usz s = 0; s < sizeof r_seeds / sizeof r_seeds[0]; ++s) {
    run_r('0', '3', '4', r_seeds[s], Bangs, out);
    CHECK(repeats(out, Bangs) == 0);
    Usz fours = 0, outside = 0;
    for (Usz i = 0; i < Bangs; ++i) {
      fours += out[i] == '4';
      outside += out[i] < '0' || out[i] > '4' || (out[i] == '4' && i % 2 == 0);
    }
    CHECK(fours > 0); // the range really changes
    CHECK(outside == 0);
  }
}

// One value repeats, by definition, and keeps the right input's case.
void test_sim_r_single_value(void) {
  enum { Bangs = 20 };
  Glyph out[Bangs];
  run_r('5', '5', 0, 0, Bangs, out);
  for (Usz i = 0; i < Bangs; ++i)
    CHECK(out[i] == '5');
  run_r('c', 'C', 0, 0, Bangs, out);
  for (Usz i = 0; i < Bangs; ++i)
    CHECK(out[i] == 'C');
}

// The seed still drives r as B1 set it up: two runs of r_shared's patch at
// the same seed give the same two sequences, and a different seed changes
// each of them.
void test_sim_r_seed(void) {
  enum { H = 4, W = 12, Ticks = 40 };
  static char const patch[H * W + 1] = "..D1.....D1."
                                       "............"
                                       ".0r3....0r9."
                                       "............";
  static Usz const seeds[3] = {0, 0, 1};
  Glyph small[3][Ticks], large[3][Ticks];
  for (Usz s = 0; s < 3; ++s) {
    Glyph grid[H * W];
    Mark marks[H * W];
    memcpy(grid, patch, sizeof grid);
    Opstate_store store;
    opstate_init(&store);
    Oevent_list events;
    oevent_list_init(&events);
    for (Usz tick = 0; tick < Ticks; ++tick) {
      run_tick(grid, marks, H, W, tick, &events, seeds[s], &store);
      small[s][tick] = grid[3 * W + 2];
      large[s][tick] = grid[3 * W + 9];
    }
    oevent_list_deinit(&events);
    opstate_free(&store);
    CHECK(repeats(small[s], Ticks) == 0 && repeats(large[s], Ticks) == 0);
    CHECK(blocks_are_permutations(small[s], Ticks, 0, 4));
    CHECK(blocks_are_permutations(large[s], Ticks, 0, 10));
  }
  CHECK(memcmp(small[0], small[1], sizeof small[0]) == 0);
  CHECK(memcmp(large[0], large[1], sizeof large[0]) == 0);
  CHECK(memcmp(small[0], small[2], sizeof small[0]) != 0);
  CHECK(memcmp(large[0], large[2], sizeof large[0]) != 0);
}

// Each glyph of outputs is in the set, and each full bag of n holds n
// distinct glyphs.
static bool bags_within(Glyph const *outputs, Usz count, Usz n,
                        char const *set) {
  if (n == 0 || count % n != 0)
    return false;
  for (Usz b = 0; b < count; b += n) {
    bool seen[128] = {0};
    for (Usz i = b; i < b + n; ++i) {
      Glyph g = outputs[i];
      if (!g || (g & 0x80) || !strchr(set, g) || seen[(int)g])
        return false;
      seen[(int)g] = true;
    }
  }
  return true;
}

// The output takes the right input's case, as R's does: an uppercase max
// gives uppercase letters, a digit or lowercase max lowercase ones, and
// digits stay digits. The min's case does not count.
void test_sim_r_case(void) {
  enum { Bangs = 13 * 8 };
  Glyph out[Bangs];
  for (Usz s = 0; s < sizeof r_seeds / sizeof r_seeds[0]; ++s) {
    run_r('0', 'C', 0, r_seeds[s], Bangs, out);
    CHECK(bags_within(out, Bangs, 13, "0123456789ABC"));
    CHECK(repeats(out, Bangs) == 0);
    run_r('c', '0', 0, r_seeds[s], Bangs, out);
    CHECK(bags_within(out, Bangs, 13, "0123456789abc"));
    CHECK(repeats(out, Bangs) == 0);
    run_r('a', 'C', 0, r_seeds[s], 3 * 8, out);
    CHECK(bags_within(out, 3 * 8, 3, "ABC"));
    CHECK(repeats(out, 3 * 8) == 0);
    run_r('c', 'C', 0, r_seeds[s], 8, out);
    CHECK(bags_within(out, 8, 1, "C"));
  }
}

// The first bag of a new r: B1's seeding, prng_seed(seed, y, x), and its
// Fisher-Yates shuffle, with no swap, since a zeroed entry has no last value.
static void reference_first_bag(Usz seed, Usz y, Usz x, Usz n, U8 *bag) {
  Prng rng;
  prng_seed(&rng, seed, y, x);
  for (Usz i = 0; i < n; ++i)
    bag[i] = (U8)i;
  for (Usz i = n - 1; i > 0; --i) {
    Usz j = prng_bounded(&rng, (U32)(i + 1));
    U8 t = bag[i];
    bag[i] = bag[j];
    bag[j] = t;
  }
}

// A new cell and a cleared store never swap against stale state. 0r3's first
// bag is the reference shuffle exactly, including the seeds whose bag starts
// with 0, the value a zeroed last_value holds. After the r sends the value
// its first bag starts with, a clear starts it over, and the next bag is the
// reference again, though it repeats that value.
void test_sim_r_first_visit_and_clear(void) {
  enum { N = 4, Seeds = 32, Limit = 200 };
  Usz zero_first = 0;
  for (Usz seed = 0; seed < Seeds; ++seed) {
    U8 ref[N];
    reference_first_bag(seed, R_y, R_x, N, ref);
    zero_first += ref[0] == 0;
    Glyph grid[R_h * R_w];
    Mark marks[R_h * R_w];
    r_grid(grid, '0', '3');
    Opstate_store store;
    opstate_init(&store);
    Oevent_list events;
    oevent_list_init(&events);
    Usz tick = 0;
    for (; tick < N; ++tick) {
      run_tick(grid, marks, R_h, R_w, tick, &events, seed, &store);
      CHECK(grid[R_out] == r_digits[ref[tick]]);
    }
    do {
      run_tick(grid, marks, R_h, R_w, tick, &events, seed, &store);
      ++tick;
    } while (grid[R_out] != r_digits[ref[0]] && tick < Limit);
    CHECK(grid[R_out] == r_digits[ref[0]]);
    opstate_clear(&store);
    for (Usz i = 0; i < N; ++i, ++tick) {
      run_tick(grid, marks, R_h, R_w, tick, &events, seed, &store);
      CHECK(grid[R_out] == r_digits[ref[i]]);
    }
    oevent_list_deinit(&events);
    opstate_free(&store);
  }
  CHECK(zero_first > 0); // the check above can see a swap against 0
}

// A patch with each stateful operator: a banged ; and r, an unbanged &, and
// a note whose velocity is r's output, so the events depend on r's state
// too. Since I3 the & holds only its anchor, which the same tick sets the
// same way in every pass, so ; and r carry the state that can leak.
enum { Patch_h = 6, Patch_w = 20 };
static char const state_patch[Patch_h * Patch_w + 1] = "D1.....D1..........."
                                                       "...D1..............."
                                                       ";12...1r5...&3a21..."
                                                       "...:03C.1..........."
                                                       "...................."
                                                       "....................";

// The paused re-mark (AD-7): 100 preview passes, each on a fresh copy of
// the live store, between two real ticks leave the second tick's grid and
// events as they are with no preview. The same passes on the live store
// would change them, through the ; step and the r bag, so the check can
// fail.
void test_sim_preview_isolation(void) {
  enum { H = Patch_h, W = Patch_w, Passes = 100 };
  Glyph live[H * W], control[H * W], scratch[H * W], leaky[H * W];
  Mark marks[H * W];
  memcpy(live, state_patch, sizeof live);
  Opstate_store store, control_store, scratch_store, leaky_store;
  opstate_init(&store);
  opstate_init(&control_store);
  opstate_init(&scratch_store);
  opstate_init(&leaky_store);
  Oevent_list events, control_events, preview_events, leaky_events;
  oevent_list_init(&events);
  oevent_list_init(&control_events);
  oevent_list_init(&preview_events);
  oevent_list_init(&leaky_events);

  // Tick 0, then fork a control and a leaky copy of the grid and the store.
  run_tick(live, marks, H, W, 0, &events, 0, &store);
  memcpy(control, live, sizeof control);
  memcpy(leaky, live, sizeof leaky);
  CHECK(opstate_copy(&store, &control_store));
  CHECK(opstate_copy(&store, &leaky_store));

  for (Usz i = 0; i < Passes; ++i) {
    memcpy(scratch, live, sizeof scratch);
    CHECK(opstate_copy(&store, &scratch_store));
    run_tick(scratch, marks, H, W, 1, &preview_events, 0, &scratch_store);
  }
  run_tick(live, marks, H, W, 1, &events, 0, &store);
  run_tick(control, marks, H, W, 1, &control_events, 0, &control_store);
  CHECK(memcmp(live, control, sizeof live) == 0);
  CHECK(events_equal(&events, &control_events));
  CHECK(events.count > 0);

  // The same passes straight on a store do advance it.
  for (Usz i = 0; i < Passes; ++i) {
    memcpy(scratch, leaky, sizeof scratch);
    run_tick(scratch, marks, H, W, 1, &preview_events, 0, &leaky_store);
  }
  run_tick(leaky, marks, H, W, 1, &leaky_events, 0, &leaky_store);
  CHECK(memcmp(leaky, control, sizeof leaky) != 0);

  oevent_list_deinit(&events);
  oevent_list_deinit(&control_events);
  oevent_list_deinit(&preview_events);
  oevent_list_deinit(&leaky_events);
  opstate_free(&store);
  opstate_free(&control_store);
  opstate_free(&scratch_store);
  opstate_free(&leaky_store);
}

// Clear (file open, new file, Ctrl+R): a tick after a clear equals the same
// tick run on a fresh store. A resize that prunes the store keeps the state
// of every cell inside the new bounds: the cropped grid ticks exactly as it
// does on an unpruned store.
void test_sim_clear_and_prune(void) {
  enum { H = Patch_h, W = Patch_w, Ticks = 10, Crop_w = 12 };
  Glyph grid[H * W], fresh[H * W], kept[H * W];
  Mark marks[H * W];
  memcpy(grid, state_patch, sizeof grid);
  Opstate_store store, fresh_store, kept_store;
  opstate_init(&store);
  opstate_init(&fresh_store);
  opstate_init(&kept_store);
  Oevent_list events, fresh_events, kept_events;
  oevent_list_init(&events);
  oevent_list_init(&fresh_events);
  oevent_list_init(&kept_events);
  for (Usz tick = 0; tick < Ticks; ++tick)
    run_tick(grid, marks, H, W, tick, &events, 0, &store);
  CHECK(store.count == 3);

  memcpy(fresh, grid, sizeof fresh);
  memcpy(kept, grid, sizeof kept);
  CHECK(opstate_copy(&store, &kept_store));
  Usz capacity = store.capacity;
  opstate_clear(&store);
  CHECK(store.count == 0 && store.capacity == capacity);
  run_tick(grid, marks, H, W, Ticks, &events, 0, &store);
  run_tick(fresh, marks, H, W, Ticks, &fresh_events, 0, &fresh_store);
  run_tick(kept, marks, H, W, Ticks, &kept_events, 0, &kept_store);
  CHECK(memcmp(grid, fresh, sizeof grid) == 0);
  CHECK(events_equal(&events, &fresh_events));
  CHECK(memcmp(kept, fresh, sizeof kept) != 0); // the clear mattered

  // Crop the kept grid to its first Crop_w columns, which cuts off the &,
  // and prune a copy of its store to match.
  Glyph cropped[H * Crop_w], unpruned_grid[H * Crop_w];
  Mark cropped_marks[H * Crop_w];
  gbuffer_copy_subrect(kept, cropped, H, W, H, Crop_w, 0, 0, 0, 0, H, Crop_w);
  memcpy(unpruned_grid, cropped, sizeof cropped);
  Opstate_store unpruned;
  opstate_init(&unpruned);
  CHECK(opstate_copy(&kept_store, &unpruned));
  CHECK(opstate_prune(&kept_store, H, Crop_w));
  CHECK(kept_store.count == 2 && unpruned.count == 3);
  for (Usz tick = Ticks + 1; tick < Ticks + 5; ++tick) {
    run_tick(cropped, cropped_marks, H, Crop_w, tick, &kept_events, 0,
             &kept_store);
    run_tick(unpruned_grid, cropped_marks, H, Crop_w, tick, &events, 0,
             &unpruned);
    CHECK(memcmp(cropped, unpruned_grid, sizeof cropped) == 0);
    CHECK(events_equal(&kept_events, &events));
  }
  opstate_free(&unpruned);
  oevent_list_deinit(&events);
  oevent_list_deinit(&fresh_events);
  oevent_list_deinit(&kept_events);
  opstate_free(&store);
  opstate_free(&fresh_store);
  opstate_free(&kept_store);
}
