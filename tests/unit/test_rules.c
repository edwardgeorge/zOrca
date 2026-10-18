// Unit tests for the seven operators whose rules differ between the
// dialects: C, D, I, L, U, Z and uppercase R. bOrca's dialect follows Orca's
// 2025 rules, in which an empty input counts as 0 and R's range includes its
// max; the upstream dialect keeps Orca-c 9df9786's rules, and zorca bOrca's.
// Each row runs in all three dialects, against the output each must give
// (zorca's is bOrca's), and checks that the three mark the same ports.
// Lowercase rows check that c, d, i, l, u and z do nothing unbanged and run
// the same body when banged.
#include "../../gbuffer.h"
#include "../../opstate.h"
#include "../../sim.h"
#include "../../vmio.h"
#include "tests.h"
#include <stdio.h>

// The grid, the ticks of a fixed-glyph row, and the ticks of an R row.
enum { Rules_h = 3, Rules_w = 5, Rules_ticks = 16, Rules_r_ticks = 400 };

// A row's grid, 5x3 and '.' but for: in, "abc", as ".abc." on row 1, so
// the operator is at (1,2); above, when not NULL, as row 0 (5 glyphs); and
// out0 in the output cell (2,2). With bang set, a '*' goes into the output
// cell before each tick: it bangs a lowercase operator from below, which
// then overwrites it.
typedef struct {
  char const *above, *in;
  Glyph out0;
  bool bang;
} Rules_setup;

// Runs a row's grid for ticks 0 to ticks - 1 at seed, in dialect, with a
// fresh store, each tick as cli runs it (marks and events cleared, then
// orca_run). Writes each tick's output glyph to outs, and the marks after
// tick 0 to marks0.
static void rules_run(Rules_setup const *su, Usz seed, Orca_dialect dialect,
                      Usz ticks, Glyph *outs, Mark *marks0) {
  Glyph grid[Rules_h * Rules_w];
  Mark marks[Rules_h * Rules_w];
  memset(grid, '.', sizeof grid);
  if (su->above)
    memcpy(grid, su->above, Rules_w);
  memcpy(grid + 1 * Rules_w + 1, su->in, 3);
  grid[2 * Rules_w + 2] = su->out0;
  Oevent_list events;
  oevent_list_init(&events);
  Opstate_store store;
  opstate_init(&store);
  Orca_run_ctx const ctx = {.opstate = &store, .dialect = dialect};
  for (Usz tick = 0; tick < ticks; ++tick) {
    if (su->bang)
      grid[2 * Rules_w + 2] = '*';
    mbuffer_clear(marks, Rules_h, Rules_w);
    oevent_list_clear(&events);
    orca_run(grid, marks, Rules_h, Rules_w, tick, &events, seed, &ctx);
    outs[tick] = grid[2 * Rules_w + 2];
    if (tick == 0)
      memcpy(marks0, marks, sizeof marks);
  }
  oevent_list_deinit(&events);
  opstate_free(&store);
}

// Indexed by Orca_dialect. zorca runs bOrca's operators, so each row's
// zorca output is its bOrca one.
enum { Rules_dialects = 3 };
static char const *const rules_dialect_name[Rules_dialects] = {
    "borca", "upstream", "zorca"};
static Orca_dialect const rules_dialect[Rules_dialects] = {
    Orca_dialect_borca, Orca_dialect_upstream, Orca_dialect_zorca};

// One row of a fixed-glyph test: the setup, over ticks 0-15 at seed 0,
// must output borca in bOrca's dialect and in zorca, and upstream in the
// upstream one, and the three dialects' marks after tick 0 must be equal.
// Prints each mismatch, naming the row and the dialect.
static bool rules_check(Rules_setup su, char const *borca,
                        char const *upstream) {
  char const *want[Rules_dialects] = {borca, upstream, borca};
  Glyph got[Rules_dialects][Rules_ticks + 1];
  Mark marks[Rules_dialects][Rules_h * Rules_w];
  bool ok = true;
  for (int d = 0; d < Rules_dialects; ++d) {
    rules_run(&su, 0, rules_dialect[d], Rules_ticks, got[d], marks[d]);
    got[d][Rules_ticks] = '\0';
    if (strcmp(got[d], want[d]) != 0) {
      printf("  %s %s (output cell %c%s%s%s): got %s, want %s\n",
             rules_dialect_name[d], su.in, su.out0,
             su.bang ? ", banged" : "", su.above ? ", above " : "",
             su.above ? su.above : "", got[d], want[d]);
      ok = false;
    }
  }
  for (int d = 1; d < Rules_dialects; ++d) {
    if (memcmp(marks[0], marks[d], sizeof marks[0]) != 0) {
      printf("  %s: %s marks different ports from borca at tick 0\n", su.in,
             rules_dialect_name[d]);
      ok = false;
    }
  }
  return ok;
}

// in, with the output cell starting as out0.
static bool rules_row(char const *in, Glyph out0, char const *borca,
                      char const *upstream) {
  return rules_check((Rules_setup){NULL, in, out0, false}, borca, upstream);
}

// in, banged from its output cell on every tick.
static bool rules_banged(char const *in, char const *borca,
                         char const *upstream) {
  return rules_check((Rules_setup){NULL, in, '*', true}, borca, upstream);
}

// in, its left input the '*' that ".D1.." above writes on every tick (a
// '*' written there by hand would clear itself before the operator runs).
static bool rules_star(char const *in, char const *borca,
                       char const *upstream) {
  return rules_check((Rules_setup){".D1..", in, '.', false}, borca, upstream);
}

// L: the lesser of the two inputs. bOrca's counts an empty one as 0; the
// upstream L writes . when either is empty.
void test_rules_lesser(void) {
  CHECK(rules_row(".L5", '.', "0000000000000000", "................"));
  CHECK(rules_row("3L.", '.', "0000000000000000", "................"));
  CHECK(rules_row(".L.", '.', "0000000000000000", "................"));
  CHECK(rules_row("3L5", '.', "3333333333333333", "3333333333333333"));
  CHECK(rules_row("cLB", '.', "BBBBBBBBBBBBBBBB", "BBBBBBBBBBBBBBBB"));
  // Lowercase: unbanged it does nothing; banged it runs the same body.
  CHECK(rules_row(".l5", '.', "................", "................"));
  CHECK(rules_banged(".l5", "0000000000000000", "................"));
}

// I: adds its step to its output, modulo its max. bOrca's empty step adds 0,
// and an empty or 0 max gives 0; upstream's empty step is 1, and an empty
// or 0 max is 36.
void test_rules_increment(void) {
  CHECK(rules_row(".I.", '.', "0000000000000000", "123456789abcdefg"));
  CHECK(rules_row(".I5", '.', "0000000000000000", "1234012340123401"));
  CHECK(rules_row("1I.", '.', "0000000000000000", "123456789abcdefg"));
  CHECK(rules_row("1I0", '.', "0000000000000000", "123456789abcdefg"));
  CHECK(rules_row(".I4", '.', "0000000000000000", "1230123012301230"));
  CHECK(rules_row("2I5", '.', "2413024130241302", "2413024130241302"));
  // An empty step leaves a value where it is, in bOrca's dialect.
  CHECK(rules_row(".I9", '5', "5555555555555555", "6780123456780123"));
  // A '*' step counts as 0 in bOrca's dialect, as 1 upstream.
  CHECK(rules_star(".I5", "0000000000000000", "1234012340123401"));
  CHECK(rules_row(".i5", '.', "................", "................"));
}

// U: a Euclidean rhythm of steps in max. bOrca's empty step is 0, which
// never bangs, and an empty or 0 max is 1, which bangs on every frame for
// any other step; upstream's empty step is 1 and empty max 8.
void test_rules_uclid(void) {
  CHECK(rules_row(".U.", '.', "................", "*.......*......."));
  CHECK(rules_row(".U8", '.', "................", "*.......*......."));
  CHECK(rules_row("1U.", '.', "****************", "*.......*......."));
  CHECK(rules_row("3U.", '.', "****************", "*..*..*.*..*..*."));
  CHECK(rules_row(".U1", '.', "................", "****************"));
  CHECK(rules_row("3U8", '.', "*..*..*.*..*..*.", "*..*..*.*..*..*."));
  // A '*' step counts as 0 in bOrca's dialect, as 1 upstream.
  CHECK(rules_star(".U8", "................", "*.......*......."));
  CHECK(rules_row(".u8", '.', "................", "................"));
  CHECK(rules_banged(".u8", "................", "*.......*......."));
}

// D: bangs when the frame is a multiple of rate x modulo. bOrca's empty or
// 0 modulo is 1, with no shortcut for a modulo of 1, so 2D. and 2D1 bang on
// every 2nd frame; upstream's empty or 0 modulo is 8.
void test_rules_delay(void) {
  CHECK(rules_row(".D.", '.', "****************", "*.......*......."));
  CHECK(rules_row(".D0", '.', "****************", "*.......*......."));
  CHECK(rules_row("2D.", '.', "*.*.*.*.*.*.*.*.", "*..............."));
  CHECK(rules_row("2D1", '.', "*.*.*.*.*.*.*.*.", "*.*.*.*.*.*.*.*."));
  CHECK(rules_row(".D4", '.', "*...*...*...*...", "*...*...*...*..."));
  CHECK(rules_row("1D8", '.', "*.......*.......", "*.......*......."));
  CHECK(rules_row(".d.", '.', "................", "................"));
  CHECK(rules_banged(".d.", "****************", "*.......*......."));
}

// C: frame / rate, modulo its modulo. bOrca's empty or 0 modulo is 1, so it
// writes 0 on every frame, over whatever the output cell held; upstream's
// is 8.
void test_rules_clock(void) {
  CHECK(rules_row(".C.", '.', "0000000000000000", "0123456701234567"));
  CHECK(rules_row(".C0", '.', "0000000000000000", "0123456701234567"));
  CHECK(rules_row("2C.", '.', "0000000000000000", "0011223344556677"));
  CHECK(rules_row(".C.", '5', "0000000000000000", "0123456701234567"));
  CHECK(rules_row(".C4", '.', "0123012301230123", "0123012301230123"));
  CHECK(rules_row("1C8", '.', "0123456701234567", "0123456701234567"));
  CHECK(rules_row("2C4", '.', "0011223300112233", "0011223300112233"));
  CHECK(rules_row(".CB", '.', "0123456789A01234", "0123456789A01234"));
  CHECK(rules_row(".c.", '.', "................", "................"));
  CHECK(rules_banged(".c.", "0000000000000000", "0123456701234567"));
}

// Z: moves its output toward the target by rate per frame. bOrca's empty
// rate is 0, so it does not move; upstream's is 1.
void test_rules_lerp(void) {
  CHECK(rules_row(".Z5", '.', "0000000000000000", "1234555555555555"));
  CHECK(rules_row("2Z9", '.', "2468999999999999", "2468999999999999"));
  CHECK(rules_row("1Z5", '.', "1234555555555555", "1234555555555555"));
  CHECK(rules_row("1ZC", '.', "123456789ABCCCCC", "123456789ABCCCCC"));
  // An empty rate leaves a value where it is, in bOrca's dialect.
  CHECK(rules_row(".Z5", '3', "3333333333333333", "4555555555555555"));
  // A '*' rate counts as 0 in bOrca's dialect, as 1 upstream.
  CHECK(rules_star(".Z5", "0000000000000000", "1234555555555555"));
  CHECK(rules_row(".z5", '.', "................", "................"));
}

// The set of glyphs an R row outputs over 400 ticks at seed, in dialect,
// must be exactly want's glyphs; and its marks after tick 0 must equal those
// of bOrca's dialect, or of upstream when dialect is bOrca's. Value sets, not sequences, so the rows hold whatever
// Usz's width (the hash differs where Usz is 32-bit).
static bool rules_r_set(char const *in, Usz seed, Orca_dialect dialect,
                        char const *want) {
  static Glyph outs[2][Rules_r_ticks];
  Mark marks[2][Rules_h * Rules_w];
  Rules_setup const su = {NULL, in, '.', false};
  rules_run(&su, seed, dialect, Rules_r_ticks, outs[0], marks[0]);
  rules_run(&su, seed,
            dialect == Orca_dialect_borca ? Orca_dialect_upstream
                                          : Orca_dialect_borca,
            1, outs[1], marks[1]);
  bool seen[256] = {false}, wanted[256] = {false};
  for (Usz i = 0; i < Rules_r_ticks; ++i)
    seen[(unsigned char)outs[0][i]] = true;
  for (char const *p = want; *p; ++p)
    wanted[(unsigned char)*p] = true;
  bool ok = true;
  if (memcmp(seen, wanted, sizeof seen) != 0) {
    printf("  %s %s seed %u: saw ", rules_dialect_name[dialect], in,
           (unsigned)seed);
    for (int g = 0; g < 256; ++g)
      if (seen[g])
        putchar(g);
    printf(", want %s\n", want);
    ok = false;
  }
  if (memcmp(marks[0], marks[1], sizeof marks[0]) != 0) {
    printf("  %s: the dialects mark different ports at tick 0\n", in);
    ok = false;
  }
  return ok;
}

// Two R rows output the same glyph on each of ticks frames at seed.
static bool rules_r_same(char const *in_a, Orca_dialect dialect_a,
                         char const *in_b, Orca_dialect dialect_b, Usz seed,
                         Usz ticks) {
  static Glyph outs[2][Rules_r_ticks];
  Mark marks[Rules_h * Rules_w];
  Rules_setup const su_a = {NULL, in_a, '.', false};
  Rules_setup const su_b = {NULL, in_b, '.', false};
  rules_run(&su_a, seed, dialect_a, ticks, outs[0], marks);
  rules_run(&su_b, seed, dialect_b, ticks, outs[1], marks);
  for (Usz i = 0; i < ticks; ++i) {
    if (outs[0][i] != outs[1][i]) {
      printf("  %s %s and %s %s seed %u differ at tick %u: %c, %c\n",
             rules_dialect_name[dialect_a], in_a,
             rules_dialect_name[dialect_b], in_b, (unsigned)seed,
             (unsigned)i, outs[0][i], outs[1][i]);
      return false;
    }
  }
  return true;
}

#define RULES_DIGITS "0123456789"
#define RULES_ALL RULES_DIGITS "abcdefghijklmnopqrstuvwxyz"

// R's range in each dialect, by the values it outputs. bOrca's: min to max,
// both included, in either order, an empty input as 0, equal inputs giving
// that value in the max's case. Upstream's: min up to, but not including,
// max, a max of . or 0 standing for 36, equal inputs in lowercase.
void test_rules_random_range(void) {
  static struct {
    char const *in, *borca, *upstream;
  } const rows[] = {
      {"1R4", "1234", "123"},
      {"4R1", "1234", "123"},
      {"aRc", "abc", "ab"},
      {".R5", "012345", "01234"},
      {"5R.", "012345", "56789abcdefghijklmnopqrstuvwxyz"},
      {"0Rz", RULES_ALL, "0123456789abcdefghijklmnopqrstuvwxy"},
      {".R.", "0", RULES_ALL},
      {"2R2", "2", "2"},
      {"aRA", "A", "a"},
  };
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(rows); ++i) {
    for (Usz seed = 0; seed < 8; seed += 7) {
      CHECK(rules_r_set(rows[i].in, seed, Orca_dialect_borca, rows[i].borca));
      CHECK(rules_r_set(rows[i].in, seed, Orca_dialect_upstream,
                        rows[i].upstream));
      CHECK(rules_r_set(rows[i].in, seed, Orca_dialect_zorca, rows[i].borca));
    }
  }
  // Either order: 4R1 outputs what 1R4 does, on every tick, in both.
  for (Usz seed = 0; seed < 8; seed += 7) {
    CHECK(rules_r_same("4R1", Orca_dialect_borca, "1R4", Orca_dialect_borca,
                       seed, Rules_r_ticks));
    CHECK(rules_r_same("4R1", Orca_dialect_upstream, "1R4",
                       Orca_dialect_upstream, seed, Rules_r_ticks));
    // zorca's R is bOrca's, tick for tick.
    CHECK(rules_r_same("1R4", Orca_dialect_zorca, "1R4", Orca_dialect_borca,
                       seed, Rules_r_ticks));
    CHECK(rules_r_same(".R.", Orca_dialect_zorca, ".R.", Orca_dialect_borca,
                       seed, Rules_r_ticks));
  }
}

// One hash in both dialects: bOrca's xRy outputs what upstream's xR(y+1)
// does, and bOrca's 0Rz what upstream's .R. does, on every tick.
void test_rules_random_hash(void) {
  static char const *const pairs[][2] = {
      {"1R4", "1R5"},
      {"0R8", "0R9"},
      {"aRy", "aRz"},
      {"0Rz", ".R."},
  };
  static Usz const seeds[] = {0, 1, 7};
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(pairs); ++i)
    for (Usz s = 0; s < ORCA_ARRAY_COUNTOF(seeds); ++s)
      CHECK(rules_r_same(pairs[i][0], Orca_dialect_borca, pairs[i][1],
                         Orca_dialect_upstream, seeds[s], 200));
}
