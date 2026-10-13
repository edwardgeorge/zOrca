// Unit tests for the port labels and the tooltips: what PORT records in the
// label buffer (the id, whether the port counted as an input, and the owner's
// offset), the recording rule (an input beats an output, the later port of the
// same kind wins), the moved ports of G, K, O, P, Q, T and X, every operator
// body's ports in its dialect, the upstream dialect's labels, tooltips.c's
// lookup with its size check and its id-keyed enhancements, the placement of a
// tooltip line on a HUD row, and the tick path: tick_run_vm clears the buffer
// and tick_body passes it on. Positions are (row, column); an owner offset is
// (dy, dx) from the cell to its owner.
#include "../../ccout.h"
#include "../../gbuffer.h"
#include "../../opstate.h"
#include "../../portlabel.h"
#include "../../sim.h"
#include "../../tick.h"
#include "../../tooltips.h"
#include "../../vmio.h"
#include "tests.h"

enum { Fx_max_cells = 1024 };

// The id fx_load fills the labels with, so a test sees that a run cleared
// them: any id that no fixture records at the cells a test checks.
enum { Stale_id = Port_label_shape };

// A fixture: a grid with its marks, labels, events and store.
typedef struct {
  Usz h, w;
  Glyph grid[Fx_max_cells];
  Mark marks[Fx_max_cells];
  Port_label_cell labels[Fx_max_cells];
  Oevent_list events;
  Opstate_store store;
} Fx;

// Loads rows, all of one width, into f. The labels are filled with a stale
// record, so a test sees that a run cleared them; it counts as an output,
// which any port replaces, so a port recorded off the grid is seen too.
static void fx_load(Fx *f, char const *const *rows, Usz nrows) {
  f->h = nrows;
  f->w = strlen(rows[0]);
  CHECK(f->h * f->w <= Fx_max_cells);
  for (Usz y = 0; y < nrows; ++y) {
    CHECK(strlen(rows[y]) == f->w);
    memcpy(f->grid + y * f->w, rows[y], f->w);
  }
  memset(f->marks, 0, sizeof f->marks);
  for (Usz i = 0; i < Fx_max_cells; ++i)
    f->labels[i] = (Port_label_cell){.id = (U8)Stale_id,
                                     .is_input = false,
                                     .owner_dy = 7,
                                     .owner_dx = -7};
  oevent_list_init(&f->events);
  opstate_init(&f->store);
}

static void fx_free(Fx *f) {
  oevent_list_deinit(&f->events);
  opstate_free(&f->store);
}

// One tick as the TUI runs it, through tick_run_vm with the buffer (or
// without one, as cli runs it).
static void fx_run(Fx *f, Usz tick, Orca_dialect dialect, bool with_labels) {
  Orca_run_ctx const ctx = {.opstate = &f->store,
                            .dialect = dialect,
                            .labels = with_labels ? f->labels : NULL};
  tick_run_vm(f->grid, f->marks, f->h, f->w, tick, &f->events, 0, &ctx);
}

static Port_label_cell const *rec(Fx const *f, Usz y, Usz x) {
  return f->labels + y * f->w + x;
}

// The record at (y, x) is id, counted as an input or not, with owner offset
// (dy, dx).
static bool rec_is(Fx const *f, Usz y, Usz x, Port_label id, bool in, int dy,
                   int dx) {
  Port_label_cell const *c = rec(f, y, x);
  return c->id == (U8)id && c->is_input == in && c->owner_dy == dy &&
         c->owner_dx == dx;
}

// The number of records whose owner is (oy, ox).
static Usz owned_by(Fx const *f, Usz oy, Usz ox) {
  Usz n = 0;
  for (Usz y = 0; y < f->h; ++y) {
    for (Usz x = 0; x < f->w; ++x) {
      Port_label_cell const *c = rec(f, y, x);
      if (c->id != 0 && (Isz)y + c->owner_dy == (Isz)oy &&
          (Isz)x + c->owner_dx == (Isz)ox)
        ++n;
    }
  }
  return n;
}

static bool rec_empty(Fx const *f, Usz y, Usz x) {
  Port_label_cell const *c = rec(f, y, x);
  return c->id == 0 && !c->is_input && c->owner_dy == 0 && c->owner_dx == 0;
}

// An expected record.
typedef struct {
  U8 y, x;
  Port_label id;
  bool in;
  I16 dy, dx;
} Exp;

// The cell holds fx_load's fill, so no run touched it.
static bool rec_is_fill(Port_label_cell const *c) {
  return c->id == (U8)Stale_id && !c->is_input && c->owner_dy == 7 &&
         c->owner_dx == -7;
}

// The buffer holds exactly the expected records: each is there, and every
// other cell is empty. The cells past h x w still hold fx_load's fill, so a
// port recorded off the grid's bottom edge is seen.
static bool fx_matches(Fx const *f, Exp const *exps, Usz n) {
  bool ok = true;
  for (Usz i = f->h * f->w; i < Fx_max_cells; ++i)
    if (!rec_is_fill(f->labels + i))
      ok = false;
  for (Usz y = 0; y < f->h; ++y) {
    for (Usz x = 0; x < f->w; ++x) {
      Exp const *e = NULL;
      for (Usz i = 0; i < n; ++i) {
        if (exps[i].y == y && exps[i].x == x)
          e = exps + i;
      }
      if (e ? !rec_is(f, y, x, e->id, e->in, e->dy, e->dx) : !rec_empty(f, y, x))
        ok = false;
    }
  }
  return ok;
}

// A record sits exactly where the mark has an input or an output: an input
// record where the mark has the input flag, an output record where it has
// the output flag, and every id is below the count.
static bool fx_marks_match(Fx const *f) {
  bool ok = true;
  for (Usz i = 0; i < f->h * f->w; ++i) {
    Mark m = f->marks[i];
    Port_label_cell const *c = f->labels + i;
    bool port = (m & (Mark_flag_input | Mark_flag_output)) != 0;
    if (port != (c->id != 0))
      ok = false;
    if (c->id >= Port_label_id_count)
      ok = false;
    if (c->id != 0 && c->is_input && !(m & Mark_flag_input))
      ok = false;
    if (c->id != 0 && !c->is_input && !(m & Mark_flag_output))
      ok = false;
    if (c->id == 0 && (c->is_input || c->owner_dy || c->owner_dx))
      ok = false;
  }
  return ok;
}

// The tooltip at (y, x) on f's grid and buffer, sized as f.
static bool tip(Fx const *f, Usz y, Usz x, char *l1, char *l2) {
  return tooltip_at(f->labels, f->h, f->w, f->grid, f->h, f->w, y, x, l1, l2);
}

// The tooltip at (y, x) is a one-line text.
static bool tip_is(Fx const *f, Usz y, Usz x, char const *text) {
  char l1[Tooltip_line_max], l2[Tooltip_line_max];
  return tip(f, y, x, l1, l2) && strcmp(l1, text) == 0 && l2[0] == '\0';
}

// The tooltip at (y, x) is two lines.
static bool tip_is2(Fx const *f, Usz y, Usz x, char const *line1,
                    char const *line2) {
  char l1[Tooltip_line_max], l2[Tooltip_line_max];
  return tip(f, y, x, l1, l2) && strcmp(l1, line1) == 0 &&
         strcmp(l2, line2) == 0;
}

static bool tip_none(Fx const *f, Usz y, Usz x) {
  char l1[Tooltip_line_max], l2[Tooltip_line_max];
  memset(l1, 'x', sizeof l1);
  memset(l2, 'x', sizeof l2);
  return !tip(f, y, x, l1, l2) && l1[0] == '\0' && l2[0] == '\0';
}

// Loads the rows into f (a block, so a test can load several fixtures).
#define ROWS(...)                                                              \
  do {                                                                         \
    char const *const rows[] = {__VA_ARGS__};                                  \
    fx_load(&f, rows, ORCA_ARRAY_COUNTOF(rows));                               \
  } while (0)

// The ids: none is 0, every id is below the count, the output's text is
// empty and the ids that tooltips.c decodes differ from their plain
// namesakes ("Value" three ways, "Scale", "Chord type"), while the texts are
// the PORT strings they replaced.
void test_labels_ids(void) {
  CHECK(Port_label_none == 0);
  CHECK(Port_label_output == 1);
  CHECK(Port_label_id_count < 256);
  CHECK(strcmp(port_label_text(Port_label_none), "") == 0);
  CHECK(strcmp(port_label_text(Port_label_output), "") == 0);
  CHECK(strcmp(port_label_text(Port_label_id_count), "") == 0);
  CHECK(strcmp(port_label_text((Port_label)255), "") == 0);
  CHECK(strcmp(port_label_text(Port_label_cc_value), "Value") == 0);
  CHECK(strcmp(port_label_text(Port_label_cc_value_upstream), "Value") == 0);
  CHECK(strcmp(port_label_text(Port_label_value), "Value") == 0);
  CHECK(Port_label_cc_value != Port_label_cc_value_upstream);
  CHECK(Port_label_cc_value != Port_label_value);
  CHECK(strcmp(port_label_text(Port_label_scale_selector), "Scale") == 0);
  CHECK(strcmp(port_label_text(Port_label_chord_type), "Chord type") == 0);
  CHECK(strcmp(port_label_text(Port_label_glide_rate), "Interpolation Rate") ==
        0);
  CHECK(strcmp(port_label_text(Port_label_control_hundreds),
               "Control (hundreds)") == 0);
  // Every text fits a tooltip line.
  for (Usz id = 0; id < Port_label_id_count; ++id)
    CHECK(strlen(port_label_text((Port_label)id)) < Tooltip_line_max);
  // Each cell is small: the buffer is sized like the marks, cell for cell.
  CHECK(sizeof(Port_label_cell) <= 8);
}

// IN/IN shared: on `1A2A3` the `2` is both As' Value B and Value A. The
// second A is scanned later, so its Value A wins: (0,2) reads Value A with
// its owner one to the right. The old scan said Value B.
void test_labels_in_in_shared(void) {
  Fx f;
  ROWS("1A2A3", ".....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 0, 2, Port_label_value_a, true, 0, 1));
  CHECK(rec_is(&f, 0, 0, Port_label_value_a, true, 0, 1));
  CHECK(rec_is(&f, 0, 4, Port_label_value_b, true, 0, -1));
  CHECK(rec_is(&f, 1, 1, Port_label_output, false, -1, 0));
  CHECK(rec_is(&f, 1, 3, Port_label_output, false, -1, 0));
  CHECK(tip_is(&f, 0, 2, "Value A"));
  CHECK(tip_is(&f, 0, 4, "Value B"));
  CHECK(tip_none(&f, 1, 1)); // an output shows nothing
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// OUT/IN, IN later: C's output cell is T's Key, scanned later. The input
// replaces the output.
void test_labels_in_over_out(void) {
  Fx f;
  ROWS(".C.....", "...T1ab");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 1, 1, Port_label_key, true, 0, 2));
  CHECK(tip_is(&f, 1, 1, "Key"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// OUT/IN, OUT later: H's Input cell is `$`'s octave output, scanned later.
// The output does not replace the input.
void test_labels_out_keeps_in(void) {
  Fx f;
  ROWS("H$......", "........");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 1, 0, Port_label_input, true, -1, 0));
  CHECK(rec_is(&f, 1, 1, Port_label_output, false, -1, 0));
  CHECK(tip_is(&f, 1, 0, "Input"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// OUT/OUT and IN/IN on `C005Q.abcde`: Q's first output replaces C's, and
// Q's X offset replaces C's Modulo; and Q's moved ports, five Inputs to its
// right with their owners, and five outputs below. The old scan said Value
// A, Value B or Modulo for most of them.
void test_labels_q_fixture(void) {
  Fx f;
  ROWS("C005Q.abcde", "...........");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 1, 0, Port_label_output, false, -1, 4));
  CHECK(rec_is(&f, 0, 1, Port_label_x_offset, true, 0, 3));
  CHECK(tip_is(&f, 0, 1, "X offset"));
  for (Usz i = 0; i < 5; ++i) {
    CHECK(rec_is(&f, 0, 5 + i, Port_label_input, true, 0, -1 - (int)i));
    CHECK(tip_is(&f, 0, 5 + i, "Input"));
    CHECK(rec_is(&f, 1, i, Port_label_output, false, -1, 4 - (int)i));
    CHECK(tip_none(&f, 1, i));
  }
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// G's moved ports: the Inputs to its right and the outputs it writes.
void test_labels_g(void) {
  Fx f;
  ROWS("212G.ab", ".......", ".......");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 0, 4, Port_label_input, true, 0, -1));
  CHECK(rec_is(&f, 0, 5, Port_label_input, true, 0, -2));
  CHECK(rec_is(&f, 2, 5, Port_label_output, false, -2, -2));
  CHECK(rec_is(&f, 2, 6, Port_label_output, false, -2, -3));
  CHECK(tip_is(&f, 0, 5, "Input"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// K's moved ports: each Variable to its right.
void test_labels_k(void) {
  Fx f;
  ROWS("2Kab", "....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 0, 2, Port_label_variable, true, 0, -1));
  CHECK(rec_is(&f, 0, 3, Port_label_variable, true, 0, -2));
  CHECK(tip_is(&f, 0, 3, "Variable"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// T's moved port: the Input its Key selects.
void test_labels_t(void) {
  Fx f;
  ROWS("13Tabc", "......");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 0, 4, Port_label_input, true, 0, -2));
  CHECK(tip_is(&f, 0, 4, "Input"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// O's moved port: the Input at its offset.
void test_labels_o(void) {
  Fx f;
  ROWS("21O....", ".......");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 1, 5, Port_label_input, true, -1, -3));
  CHECK(tip_is(&f, 1, 5, "Input"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// I's read-write cell (IN | OUT) counts as an output: scanned later, it does
// not replace O's Input there.
void test_labels_i_on_input(void) {
  Fx f;
  ROWS("11O.I..", ".......");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 1, 4, Port_label_input, true, -1, -2));
  CHECK(tip_is(&f, 1, 4, "Input"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// P's output: the cell its Key selects.
void test_labels_p(void) {
  Fx f;
  ROWS("13P.", "....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 1, 3, Port_label_output, false, -1, -1));
  CHECK(tip_none(&f, 1, 3));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// X's output: the cell at its offset.
void test_labels_x(void) {
  Fx f;
  ROWS("21X..", ".....", ".....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 2, 4, Port_label_output, false, -2, -2));
  CHECK(tip_none(&f, 2, 4));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// Far owner: 256 Js in a column write 256 rows down, the farthest any
// operator reaches, and the offset holds it.
void test_labels_far_owner(void) {
  Fx f;
  char const *rows[258];
  rows[0] = "a";
  for (Usz y = 1; y <= 256; ++y)
    rows[y] = "J";
  rows[257] = ".";
  fx_load(&f, rows, 258);
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 257, 0, Port_label_output, false, -256, 0));
  CHECK(rec_is(&f, 0, 0, Port_label_input, true, 1, 0));
  CHECK(f.grid[257] == 'a');
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// The matrix's buffer fixtures, run again in both dialects: a record sits
// exactly where the mark has an input or an output, with every id below the
// count; and without a buffer (as cli runs) the marks, grid and events are
// identical.
static char const *const matrix_fixtures[][3] = {
    {"1A2A3", ".....", NULL},
    {".C.....", "...T1ab", NULL},
    {"H$......", "........", NULL},
    {"C005Q.abcde", "...........", NULL},
    {"212G.ab", ".......", "......."},
    {"2Kab", "....", NULL},
    {"13Tabc", "......", NULL},
    {"21O....", ".......", NULL},
    {"11O.I..", ".......", NULL},
    {"13P.", "....", NULL},
    {"21X..", ".....", "....."},
    {"!12z", "*...", NULL},
    {"$3C02", ".....", NULL},
    {"&0z42", ".....", NULL},
    {";hello", "*.....", NULL},
    {"=a3123", "*.....", NULL},
    {"=13CN.1", ".......", NULL},
    {"!1074f4", "*......", NULL},
    {"1r5", ".*.", NULL},
    {"1R5", "...", NULL},
    {".Va", "...", NULL},
    {"?12.", "....", NULL},
    {":14C..", "*.....", NULL},
};

static Usz fixture_rows(char const *const *rows) {
  return rows[2] ? 3 : 2;
}

void test_labels_match_marks(void) {
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(matrix_fixtures); ++i) {
    for (Usz d = 0; d < 2; ++d) {
      Fx f;
      fx_load(&f, matrix_fixtures[i], fixture_rows(matrix_fixtures[i]));
      for (Usz tick = 0; tick < 4; ++tick) {
        fx_run(&f, tick, (Orca_dialect)d, true);
        CHECK(fx_marks_match(&f));
      }
      fx_free(&f);
    }
  }
}

void test_labels_no_buffer(void) {
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(matrix_fixtures); ++i) {
    for (Usz d = 0; d < 2; ++d) {
      Fx with, without;
      fx_load(&with, matrix_fixtures[i], fixture_rows(matrix_fixtures[i]));
      fx_load(&without, matrix_fixtures[i], fixture_rows(matrix_fixtures[i]));
      for (Usz tick = 0; tick < 4; ++tick) {
        fx_run(&with, tick, (Orca_dialect)d, true);
        fx_run(&without, tick, (Orca_dialect)d, false);
        Usz n = with.h * with.w;
        CHECK(memcmp(with.grid, without.grid, n) == 0);
        CHECK(memcmp(with.marks, without.marks, n) == 0);
        CHECK(with.events.count == without.events.count);
        for (Usz k = 0; k < with.events.count && k < without.events.count;
             ++k) {
          Oevent const *a = with.events.buffer + k;
          Oevent const *b = without.events.buffer + k;
          CHECK(oevent_equal(a, b));
        }
      }
      // The buffer that was never passed keeps its stale fill.
      CHECK(without.labels[0].id == (U8)Stale_id);
      fx_free(&with);
      fx_free(&without);
    }
  }
}

// Upstream `!`: Channel, Control and Value, whose tooltip scales x 127 / 35
// (`z` is 127, `f` 54; bOrca's x 4 would give 60 for `f`).
void test_labels_upstream_cc(void) {
  Fx f;
  ROWS("!12z", "*...");
  fx_run(&f, 0, Orca_dialect_upstream, true);
  CHECK(rec_is(&f, 0, 1, Port_label_channel, true, 0, -1));
  CHECK(rec_is(&f, 0, 2, Port_label_control, true, 0, -2));
  CHECK(rec_is(&f, 0, 3, Port_label_cc_value_upstream, true, 0, -3));
  CHECK(rec_is(&f, 0, 0, Port_label_output, false, 0, 0));
  CHECK(tip_is(&f, 0, 1, "Channel"));
  CHECK(tip_is(&f, 0, 2, "Control"));
  CHECK(tip_is2(&f, 0, 3, "Value:", "127"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
  ROWS("!12f", "*...");
  fx_run(&f, 0, Orca_dialect_upstream, true);
  CHECK(tip_is2(&f, 0, 3, "Value:", "54"));
  f.grid[3] = '0';
  CHECK(tip_is2(&f, 0, 3, "Value:", "0"));
  f.grid[3] = '.';
  CHECK(tip_is(&f, 0, 3, "Value")); // a `.` shows the plain label
  fx_free(&f);
}

// Upstream `$` and `&` do nothing: no record is owned by either. (The `C`
// in `$3C02` is the clock, whose own ports are there in both dialects, so
// its Modulo at (0,3) is what the selector cell shows upstream.)
void test_labels_upstream_inert(void) {
  Fx f;
  ROWS("$3C02", ".....");
  fx_run(&f, 0, Orca_dialect_upstream, true);
  CHECK(owned_by(&f, 0, 0) == 0);
  CHECK(rec_is(&f, 0, 3, Port_label_modulo, true, 0, -1));
  CHECK(tip_is(&f, 0, 3, "Modulo"));
  CHECK(rec_empty(&f, 0, 4) && rec_empty(&f, 1, 0));
  fx_free(&f);
  ROWS("$3C02", ".....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  // bOrca: four inputs and the note output; the octave output is off the
  // grid, and `$` locks its inputs, so the C is never run.
  CHECK(owned_by(&f, 0, 0) == 5);
  CHECK(rec_is(&f, 0, 3, Port_label_scale_selector, true, 0, -3));
  fx_free(&f);
  ROWS("&0z42", ".....");
  fx_run(&f, 0, Orca_dialect_upstream, true);
  CHECK(owned_by(&f, 0, 0) == 0);
  CHECK(fx_matches(&f, NULL, 0));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
  ROWS("&0z42", ".....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(owned_by(&f, 0, 0) == 5);
  fx_free(&f);
}

// Upstream `;` (UDP), banged from below: only its own output cell, and
// nothing at the glyphs it sends or the bang. bOrca's `;` records Range,
// Pattern and its output there.
void test_labels_upstream_udp(void) {
  Fx f;
  ROWS(";hello", "*.....");
  fx_run(&f, 0, Orca_dialect_upstream, true);
  static Exp const exps[] = {{0, 0, Port_label_output, false, 0, 0}};
  CHECK(fx_matches(&f, exps, 1));
  CHECK(rec_empty(&f, 0, 1) && rec_empty(&f, 0, 2) && rec_empty(&f, 1, 0));
  CHECK(tip_none(&f, 0, 1));
  fx_free(&f);
  ROWS(";hello", "*.....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 0, 1, Port_label_range, true, 0, -1));
  CHECK(rec_is(&f, 0, 2, Port_label_pattern, true, 0, -2));
  CHECK(rec_is(&f, 1, 0, Port_label_output, false, -1, 0));
  CHECK(tip_is(&f, 0, 1, "Range"));
  fx_free(&f);
}

// Upstream `=` (OSC): Path, Count, then Value for each of Count's values;
// (0,4) is a plain Value and never names a chord.
void test_labels_upstream_osc(void) {
  Fx f;
  ROWS("=a3123", "*.....");
  fx_run(&f, 0, Orca_dialect_upstream, true);
  CHECK(rec_is(&f, 0, 1, Port_label_path, true, 0, -1));
  CHECK(rec_is(&f, 0, 2, Port_label_count, true, 0, -2));
  CHECK(rec_is(&f, 0, 3, Port_label_value, true, 0, -3));
  CHECK(rec_is(&f, 0, 4, Port_label_value, true, 0, -4));
  CHECK(rec_is(&f, 0, 5, Port_label_value, true, 0, -5));
  CHECK(tip_is(&f, 0, 1, "Path"));
  CHECK(tip_is(&f, 0, 2, "Count"));
  CHECK(tip_is(&f, 0, 4, "Value"));
  CHECK(fx_marks_match(&f));
  fx_free(&f);
}

// The selectors: `=`'s chord type names the chord, `$`'s selector a scale
// for 0-9 or a chord otherwise, and a selector glyph with no name (`*`)
// shows the plain label.
void test_labels_selectors(void) {
  Fx f;
  ROWS("=13C0.1", ".......");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 0, 4, Port_label_chord_type, true, 0, -4));
  CHECK(tip_is2(&f, 0, 4, "Chord type:", "Major+Oct"));
  fx_free(&f);
  ROWS("$3C02", ".....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 0, 3, Port_label_scale_selector, true, 0, -3));
  CHECK(tip_is2(&f, 0, 3, "Scale:", "Major"));
  fx_free(&f);
  ROWS("$3Ca2", ".....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(tip_is2(&f, 0, 3, "Chord:", "Major"));
  f.grid[3] = '.';
  CHECK(tip_is(&f, 0, 3, "Scale"));
  fx_free(&f);
  ROWS("=13CN.1", ".......");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(tip_is2(&f, 0, 4, "Chord type:", "Minor add9 1st inv"));
  f.grid[4] = '.';
  CHECK(tip_is(&f, 0, 4, "Chord type"));
  fx_free(&f);
  // The bang erases itself when scanned, as it does on the live grid the
  // TUI draws from: the lookup sees the glyph it is given.
  ROWS("=13C*.1", ".......");
  Glyph before[7];
  memcpy(before, f.grid, 7);
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(tip_is(&f, 0, 4, "Chord type"));
  char l1[Tooltip_line_max], l2[Tooltip_line_max];
  CHECK(tooltip_at(f.labels, f.h, f.w, before, f.h, f.w, 0, 4, l1, l2));
  CHECK(strcmp(l1, "Chord type") == 0 && l2[0] == '\0');
  fx_free(&f);
}

// bOrca `!`: the value x 4, and the rate: "rate N", "instant" for the
// instant class (rate index 0 or 35: `0`, `z`, `Z`), and the plain label for
// `.`.
void test_labels_borca_cc(void) {
  Fx f;
  ROWS("!1074f4", "*......");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_is(&f, 0, 5, Port_label_cc_value, true, 0, -5));
  CHECK(rec_is(&f, 0, 6, Port_label_glide_rate, true, 0, -6));
  CHECK(tip_is2(&f, 0, 5, "Value:", "60"));
  CHECK(tip_is2(&f, 0, 6, "Interpolation:", "rate 4"));
  static char const instant[] = "0zZ";
  for (Usz i = 0; i < 3; ++i) {
    f.grid[6] = instant[i];
    CHECK(tip_is2(&f, 0, 6, "Interpolation:", "instant"));
  }
  f.grid[6] = 'y';
  CHECK(tip_is2(&f, 0, 6, "Interpolation:", "rate 34"));
  f.grid[6] = '1';
  CHECK(tip_is2(&f, 0, 6, "Interpolation:", "rate 1"));
  f.grid[6] = '.';
  CHECK(tip_is(&f, 0, 6, "Interpolation Rate"));
  // The value clamps at 127: `w` is 32 x 4 = 128.
  f.grid[5] = 'w';
  CHECK(tip_is2(&f, 0, 5, "Value:", "127"));
  f.grid[5] = 'z';
  CHECK(tip_is2(&f, 0, 5, "Value:", "127"));
  f.grid[5] = '0';
  CHECK(tip_is2(&f, 0, 5, "Value:", "0"));
  f.grid[5] = '.';
  CHECK(tip_is(&f, 0, 5, "Value"));
  CHECK(tip_is(&f, 0, 1, "Channel"));
  CHECK(tip_is(&f, 0, 2, "Control (hundreds)"));
  fx_free(&f);
}

// Stale size: the grid was resized while playing and no run has sized the
// buffer since. The lookup returns nothing when the height or the width
// differs, including 10 x 20 against 20 x 10 (the same count), and reads
// nothing out of the buffer, which here is exactly the old size.
void test_labels_stale_size(void) {
  enum { H = 10, W = 20 };
  Glyph grid[30 * 20];
  Mark marks[30 * 20];
  memset(grid, '.', sizeof grid);
  memset(marks, 0, sizeof marks);
  memcpy(grid, "1A2", 3);
  memcpy(grid + 25 * 20 + 3, "1A2", 3); // a port in the 30 x 20 field only
  Port_label_cell *labels = malloc(H * W * sizeof *labels);
  CHECK(labels != NULL);
  if (!labels)
    return;
  Opstate_store store;
  opstate_init(&store);
  Oevent_list events;
  oevent_list_init(&events);
  Orca_run_ctx const ctx = {.opstate = &store, .labels = labels};
  tick_run_vm(grid, marks, H, W, 0, &events, 0, &ctx);
  CHECK(labels[0].id == Port_label_value_a); // A's Value A at (0,0)
  char l1[Tooltip_line_max], l2[Tooltip_line_max];
  // Sized as it was: the tooltip shows.
  CHECK(tooltip_at(labels, H, W, grid, H, W, 0, 0, l1, l2));
  CHECK(strcmp(l1, "Value A") == 0);
  // Taller: nothing, at a cell inside the buffer and at one beyond it.
  CHECK(!tooltip_at(labels, H, W, grid, 30, W, 0, 0, l1, l2));
  CHECK(l1[0] == '\0' && l2[0] == '\0');
  CHECK(!tooltip_at(labels, H, W, grid, 30, W, 25, 3, l1, l2));
  // The same count, transposed: nothing, even at a cell with a record
  // ((0,0) is cell 0 in both layouts).
  CHECK(!tooltip_at(labels, H, W, grid, 20, 10, 0, 0, l1, l2));
  // No buffer, or a cursor outside the field: nothing.
  CHECK(!tooltip_at(NULL, H, W, grid, H, W, 0, 0, l1, l2));
  CHECK(!tooltip_at(labels, H, W, grid, H, W, H, 0, l1, l2));
  CHECK(!tooltip_at(labels, H, W, grid, H, W, 0, W, l1, l2));
  // A 0 x 0 remembered size, after a failed growth, never matches a field.
  CHECK(!tooltip_at(labels, 0, 0, grid, H, W, 0, 0, l1, l2));
  free(labels);
  oevent_list_deinit(&events);
  opstate_free(&store);
}

// One fixture per operator body that has ports, in its dialect: the buffer
// holds exactly each port's id and owner, and nothing else.
typedef struct {
  char const *rows[3];
  Orca_dialect dialect;
  Exp exps[12];
  Usz n;
} Op_fixture;

#define IN_ true
#define OUT_ false
#define O Port_label_output

static Op_fixture const op_fixtures[] = {
    // bOrca `!` (midicc), banged from below: six inputs and its output.
    {{"!1074f.", "*......"},
     Orca_dialect_borca,
     {{0, 1, Port_label_channel, IN_, 0, -1},
      {0, 2, Port_label_control_hundreds, IN_, 0, -2},
      {0, 3, Port_label_control_tens, IN_, 0, -3},
      {0, 4, Port_label_control_ones, IN_, 0, -4},
      {0, 5, Port_label_cc_value, IN_, 0, -5},
      {0, 6, Port_label_glide_rate, IN_, 0, -6},
      {0, 0, O, OUT_, 0, 0}},
     7},
    // `:` and `%` (midi), banged from below.
    {{":14C..", "*....."},
     Orca_dialect_borca,
     {{0, 1, Port_label_channel, IN_, 0, -1},
      {0, 2, Port_label_octave, IN_, 0, -2},
      {0, 3, Port_label_note, IN_, 0, -3},
      {0, 4, Port_label_velocity, IN_, 0, -4},
      {0, 5, Port_label_length, IN_, 0, -5},
      {0, 0, O, OUT_, 0, 0}},
     6},
    {{"%14C..", "*....."},
     Orca_dialect_borca,
     {{0, 1, Port_label_channel, IN_, 0, -1},
      {0, 2, Port_label_octave, IN_, 0, -2},
      {0, 3, Port_label_note, IN_, 0, -3},
      {0, 4, Port_label_velocity, IN_, 0, -4},
      {0, 5, Port_label_length, IN_, 0, -5},
      {0, 0, O, OUT_, 0, 0}},
     6},
    // `?` (midipb): its output is marked at once.
    {{"?12.", "...."},
     Orca_dialect_borca,
     {{0, 1, Port_label_channel, IN_, 0, -1},
      {0, 2, Port_label_msb, IN_, 0, -2},
      {0, 3, Port_label_lsb, IN_, 0, -3},
      {0, 0, O, OUT_, 0, 0}},
     4},
    {{"1A2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_value_a, IN_, 0, 1},
      {0, 2, Port_label_value_b, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    // One row: A's output port falls off the grid's bottom edge.
    {{"1A2"},
     Orca_dialect_borca,
     {{0, 0, Port_label_value_a, IN_, 0, 1},
      {0, 2, Port_label_value_b, IN_, 0, -1}},
     2},
    // Lowercase a, banged from below, and unbanged (no port at all).
    {{"1a2", ".*."},
     Orca_dialect_borca,
     {{0, 0, Port_label_value_a, IN_, 0, 1},
      {0, 2, Port_label_value_b, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1a2", "..."}, Orca_dialect_borca, {{0, 0, Port_label_none, OUT_, 0, 0}}, 0},
    {{"1B2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_value_a, IN_, 0, 1},
      {0, 2, Port_label_value_b, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1C2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_rate, IN_, 0, 1},
      {0, 2, Port_label_modulo, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1D2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_rate, IN_, 0, 1},
      {0, 2, Port_label_modulo, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1F2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_value_a, IN_, 0, 1},
      {0, 2, Port_label_value_b, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"212G.ab", ".......", "......."},
     Orca_dialect_borca,
     {{0, 0, Port_label_x_offset, IN_, 0, 3},
      {0, 1, Port_label_y_offset, IN_, 0, 2},
      {0, 2, Port_label_length, IN_, 0, 1},
      {0, 4, Port_label_input, IN_, 0, -1},
      {0, 5, Port_label_input, IN_, 0, -2},
      {2, 5, O, OUT_, -2, -2},
      {2, 6, O, OUT_, -2, -3}},
     7},
    {{"H", "."}, Orca_dialect_borca, {{1, 0, Port_label_input, IN_, -1, 0}}, 1},
    // I: its read-write cell is IN | OUT, recorded as an output.
    {{"1I2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_rate, IN_, 0, 1},
      {0, 2, Port_label_max, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1", "J", "."},
     Orca_dialect_borca,
     {{0, 0, Port_label_input, IN_, 1, 0}, {2, 0, O, OUT_, -1, 0}},
     2},
    {{"2Kab", "...."},
     Orca_dialect_borca,
     {{0, 0, Port_label_length, IN_, 0, 1},
      {0, 2, Port_label_variable, IN_, 0, -1},
      {0, 3, Port_label_variable, IN_, 0, -2},
      {1, 2, O, OUT_, -1, -1},
      {1, 3, O, OUT_, -1, -2}},
     5},
    {{"1L2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_value_a, IN_, 0, 1},
      {0, 2, Port_label_value_b, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1M2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_factor_a, IN_, 0, 1},
      {0, 2, Port_label_factor_b, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"21O....", "......."},
     Orca_dialect_borca,
     {{0, 0, Port_label_x_offset, IN_, 0, 2},
      {0, 1, Port_label_y_offset, IN_, 0, 1},
      {1, 5, Port_label_input, IN_, -1, -3},
      {1, 2, O, OUT_, -1, 0}},
     4},
    {{"13P.", "...."},
     Orca_dialect_borca,
     {{0, 0, Port_label_key, IN_, 0, 2},
      {0, 1, Port_label_length, IN_, 0, 1},
      {0, 3, Port_label_input, IN_, 0, -1},
      {1, 3, O, OUT_, -1, -1}},
     4},
    {{"005Q.abcde", ".........."},
     Orca_dialect_borca,
     {{0, 0, Port_label_x_offset, IN_, 0, 3},
      {0, 1, Port_label_y_offset, IN_, 0, 2},
      {0, 2, Port_label_length, IN_, 0, 1},
      {0, 4, Port_label_input, IN_, 0, -1},
      {0, 5, Port_label_input, IN_, 0, -2},
      {0, 6, Port_label_input, IN_, 0, -3},
      {0, 7, Port_label_input, IN_, 0, -4},
      {0, 8, Port_label_input, IN_, 0, -5},
      {1, 0, O, OUT_, -1, 3},
      {1, 1, O, OUT_, -1, 2},
      {1, 2, O, OUT_, -1, 1},
      {1, 3, O, OUT_, -1, 0}},
     12},
    // R (random_hashed) in both dialects, bOrca's r (the bag), and upstream's
    // r (a banged R), each banged from below where it needs one.
    {{"1R5", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_min, IN_, 0, 1},
      {0, 2, Port_label_max, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1R5", "..."},
     Orca_dialect_upstream,
     {{0, 0, Port_label_min, IN_, 0, 1},
      {0, 2, Port_label_max, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1r5", ".*."},
     Orca_dialect_borca,
     {{0, 0, Port_label_min, IN_, 0, 1},
      {0, 2, Port_label_max, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"1r5", ".*."},
     Orca_dialect_upstream,
     {{0, 0, Port_label_min, IN_, 0, 1},
      {0, 2, Port_label_max, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"13Tabc", "......"},
     Orca_dialect_borca,
     {{0, 0, Port_label_key, IN_, 0, 2},
      {0, 1, Port_label_length, IN_, 0, 1},
      {0, 4, Port_label_input, IN_, 0, -2},
      {1, 2, O, OUT_, -1, 0}},
     4},
    {{"1U2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_steps, IN_, 0, 1},
      {0, 2, Port_label_max, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    // V reading a variable: its output; V writing one: no output.
    {{".Va", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_variable, IN_, 0, 1},
      {0, 2, Port_label_value, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{"aV1", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_variable, IN_, 0, 1},
      {0, 2, Port_label_value, IN_, 0, -1}},
     2},
    {{"21X..", ".....", "....."},
     Orca_dialect_borca,
     {{0, 0, Port_label_x_offset, IN_, 0, 2},
      {0, 1, Port_label_y_offset, IN_, 0, 1},
      {0, 3, Port_label_input, IN_, 0, -1},
      {2, 4, O, OUT_, -2, -2}},
     4},
    {{"1Y."},
     Orca_dialect_borca,
     {{0, 0, Port_label_input, IN_, 0, 1}, {0, 2, O, OUT_, 0, -1}},
     2},
    // Z: its read-write cell is IN | OUT, recorded as an output.
    {{"1Z2", "..."},
     Orca_dialect_borca,
     {{0, 0, Port_label_rate, IN_, 0, 1},
      {0, 2, Port_label_target, IN_, 0, -1},
      {1, 1, O, OUT_, -1, 0}},
     3},
    {{".$3C02", "......"},
     Orca_dialect_borca,
     {{0, 2, Port_label_octave, IN_, 0, -1},
      {0, 3, Port_label_root, IN_, 0, -2},
      {0, 4, Port_label_scale_selector, IN_, 0, -3},
      {0, 5, Port_label_degree, IN_, 0, -4},
      {1, 0, O, OUT_, -1, 1},
      {1, 1, O, OUT_, -1, 0}},
     6},
    {{"=13C0.1", "......."},
     Orca_dialect_borca,
     {{0, 1, Port_label_channel, IN_, 0, -1},
      {0, 2, Port_label_octave, IN_, 0, -2},
      {0, 3, Port_label_root_note, IN_, 0, -3},
      {0, 4, Port_label_chord_type, IN_, 0, -4},
      {0, 5, Port_label_velocity, IN_, 0, -5},
      {0, 6, Port_label_length, IN_, 0, -6},
      {0, 0, O, OUT_, 0, 0}},
     7},
    // `;` needs a bang (glyph_is_lowercase takes it as lowercase); from
    // below, since a bang left of it is erased before it is scanned.
    {{";1234", "*...."},
     Orca_dialect_borca,
     {{0, 1, Port_label_range, IN_, 0, -1},
      {0, 2, Port_label_pattern, IN_, 0, -2},
      {0, 3, Port_label_length, IN_, 0, -3},
      {0, 4, Port_label_step, IN_, 0, -4},
      {1, 0, O, OUT_, -1, 0}},
     5},
    {{"&0z42", "....."},
     Orca_dialect_borca,
     {{0, 1, Port_label_start, IN_, 0, -1},
      {0, 2, Port_label_end, IN_, 0, -2},
      {0, 3, Port_label_rate, IN_, 0, -3},
      {0, 4, Port_label_shape, IN_, 0, -4},
      {1, 0, O, OUT_, -1, 0}},
     5},
    // Upstream `!`, `;` (UDP) and `=` (OSC), banged from below.
    {{"!12f", "*..."},
     Orca_dialect_upstream,
     {{0, 1, Port_label_channel, IN_, 0, -1},
      {0, 2, Port_label_control, IN_, 0, -2},
      {0, 3, Port_label_cc_value_upstream, IN_, 0, -3},
      {0, 0, O, OUT_, 0, 0}},
     4},
    {{";hi", "*.."}, Orca_dialect_upstream, {{0, 0, O, OUT_, 0, 0}}, 1},
    {{"=a3123", "*....."},
     Orca_dialect_upstream,
     {{0, 1, Port_label_path, IN_, 0, -1},
      {0, 2, Port_label_count, IN_, 0, -2},
      {0, 3, Port_label_value, IN_, 0, -3},
      {0, 4, Port_label_value, IN_, 0, -4},
      {0, 5, Port_label_value, IN_, 0, -5},
      {0, 0, O, OUT_, 0, 0}},
     6},
};

#undef IN_
#undef OUT_
#undef O

void test_labels_per_operator(void) {
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(op_fixtures); ++i) {
    Op_fixture const *op = op_fixtures + i;
    Usz nrows = op->rows[2] ? 3 : op->rows[1] ? 2 : 1;
    Fx f;
    fx_load(&f, op->rows, nrows);
    fx_run(&f, 0, op->dialect, true);
    bool ok = fx_matches(&f, op->exps, op->n);
    CHECK(ok);
    if (!ok)
      unit_check_failed(__FILE__, __LINE__, op->rows[0]);
    CHECK(fx_marks_match(&f));
    fx_free(&f);
  }
}

// The placement of a tooltip line on a HUD row, over widths 0-200, lengths
// 0-40 and every limit up to the width: the line starts one blank column
// after the field at the earliest, never touches the right-most column, is
// right-aligned when it fits, and is cut from the end otherwise.
void test_tooltip_place(void) {
  int x;
  // The worked cases: an 80-column row, a 4-column line ("Rate").
  CHECK(tooltip_place(40, 4, 80, &x) == 4 && x == 75);
  CHECK(tooltip_place(74, 4, 80, &x) == 4 && x == 75);
  CHECK(tooltip_place(75, 4, 80, &x) == 3 && x == 76);
  CHECK(tooltip_place(76, 4, 80, &x) == 2 && x == 77);
  CHECK(tooltip_place(78, 4, 80, &x) == 0 && x == 79);
  CHECK(tooltip_place(79, 4, 80, &x) <= 0);
  CHECK(tooltip_place(80, 4, 80, &x) <= 0);
  // "Minor add9 1st inv" (18) on a 50-column row after a 31-column field.
  CHECK(tooltip_place(31, 18, 50, &x) == 17 && x == 32);
  CHECK(tooltip_place(0, 0, 0, &x) <= 0);
  for (int width = 0; width <= 200; ++width) {
    for (int len = 0; len <= 40; ++len) {
      for (int limit = 0; limit <= width; ++limit) {
        int n = tooltip_place(limit, len, width, &x);
        CHECK(x >= limit + 1);  // one blank column after the field
        CHECK(x + n <= width - 1 || n <= 0); // the right-most column is free
        CHECK(n <= len);
        if (limit + 1 <= width - 1 - len) {
          // It fits: right-aligned, whole.
          CHECK(x == width - 1 - len && n == len);
        } else {
          // It does not: right after the field, cut from the end.
          CHECK(x == limit + 1 && n == width - 1 - x);
        }
      }
    }
  }
}

// tick_run_vm clears the buffer with the marks: a stale record where no
// port is recorded is gone after the run, and the ports are there.
void test_labels_tick_run_vm_clears(void) {
  Fx f;
  ROWS("1A2A3", ".....");
  fx_run(&f, 0, Orca_dialect_borca, true);
  CHECK(rec_empty(&f, 1, 4)); // the stale fill fx_load put there is gone
  // A record planted after a run goes too, without disturbing the ports.
  f.labels[1 * 5 + 4] = (Port_label_cell){
      .id = Port_label_key, .is_input = true, .owner_dy = 3, .owner_dx = 3};
  f.labels[1 * 5 + 0] = (Port_label_cell){
      .id = Port_label_key, .is_input = true, .owner_dy = 3, .owner_dx = 3};
  fx_run(&f, 1, Orca_dialect_borca, true);
  CHECK(rec_empty(&f, 1, 4));
  CHECK(rec_empty(&f, 1, 0));
  CHECK(rec_is(&f, 0, 2, Port_label_value_a, true, 0, 1));
  fx_free(&f);
}

static void null_midi3(void *u, int status, int d1, int d2) {
  (void)u;
  (void)status;
  (void)d1;
  (void)d2;
}
static void null_midi1(void *u, int byte) {
  (void)u;
  (void)byte;
}
static void null_osc(void *u, Oevent const *e) {
  (void)u;
  (void)e;
}

// tick_body passes Tick_ctx's buffer to the VM: after one body on `1A2A3`,
// (0,2) is Value A with its owner one to the right, and a stale record
// elsewhere is cleared.
void test_labels_tick_body(void) {
  Fx f;
  ROWS("1A2A3", ".....");
  f.labels[1 * 5 + 4] = (Port_label_cell){
      .id = Port_label_key, .is_input = true, .owner_dy = 3, .owner_dx = 3};
  Oevent_list engine;
  oevent_list_init(&engine);
  Susnote_list susnotes;
  susnote_list_init(&susnotes);
  Ccout_engine ccout;
  ccout_init(&ccout);
  Usz tick_num = 0;
  Tick_ctx const ctx = {.gbuffer = f.grid,
                        .mbuffer = f.marks,
                        .height = f.h,
                        .width = f.w,
                        .random_seed = 0,
                        .opstate = &f.store,
                        .dialect = Orca_dialect_borca,
                        .labels = f.labels,
                        .tick_num = &tick_num,
                        .tick_list = &f.events,
                        .engine_list = &engine,
                        .susnotes = &susnotes,
                        .ccout = &ccout,
                        .now_us = 1000000,
                        .tick_len = 125000};
  Tick_sink const sink = {
      .u = NULL, .midi3 = null_midi3, .midi1 = null_midi1, .osc = null_osc};
  tick_body(&ctx, &sink);
  CHECK(tick_num == 1);
  CHECK(rec_is(&f, 0, 2, Port_label_value_a, true, 0, 1));
  CHECK(rec_empty(&f, 1, 4));
  CHECK(tip_is(&f, 0, 2, "Value A"));
  CHECK(fx_marks_match(&f));
  // Without a buffer in the context, nothing is recorded or cleared.
  f.labels[1 * 5 + 4].id = Port_label_key;
  Tick_ctx const no_labels = {.gbuffer = f.grid,
                              .mbuffer = f.marks,
                              .height = f.h,
                              .width = f.w,
                              .opstate = &f.store,
                              .tick_num = &tick_num,
                              .tick_list = &f.events,
                              .engine_list = &engine,
                              .susnotes = &susnotes,
                              .ccout = &ccout,
                              .now_us = 1125000,
                              .tick_len = 125000};
  tick_body(&no_labels, &sink);
  CHECK(f.labels[1 * 5 + 4].id == Port_label_key);
  oevent_list_deinit(&engine);
  susnote_list_deinit(&susnotes);
  fx_free(&f);
}
