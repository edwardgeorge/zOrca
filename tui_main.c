#include "base.h"
#include "ccout.h"
#include "field.h"
#include "gbuffer.h"
#include "midi_in.h"
#include "opstate.h"
#include "osc_out.h"
#include "oso.h"
#include "sim.h"
#include "sysmisc.h"
#include "term_util.h"
#include "tick.h"
#include "tooltips.h"
#include "transport.h"
#include "tui_prefs.h"
#include "vmio.h"
#include <getopt.h>
#include <locale.h>

#define SOKOL_IMPL
#include "sokol_time.h"
#undef SOKOL_IMPL

#ifdef FEAT_PORTMIDI
#include <portmidi.h>
#endif

#ifdef FEAT_ALSA
#include <alloca.h> // the snd_seq_*_alloca macros, under -std=c99
#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>

// SIGTERM, SIGHUP and SIGINT quit gracefully in --alsa builds: the handler
// only sets this flag, which the event loop checks. zOrca has one thread,
// and this flag is the only data that crosses a signal handler.
static volatile sig_atomic_t alsa_shutdown_requested;
static void alsa_request_shutdown(int signo) {
  (void)signo;
  alsa_shutdown_requested = 1;
}
#endif

// Microseconds since stm_setup(), as a U64: the one clock for core seams,
// and the only clock read the tick path added (v1.0.0); 4f349cd's stm_now()
// reads were left unchanged. stm_now() returns nanoseconds on every platform
// (sokol_time.h:169-185), so this divides in integer arithmetic rather than
// calling stm_us(), which goes through a double. The tick body reads it once,
// for the CC engine's poll and submits.
static U64 now_us(void) { return stm_now() / 1000; }

#if NCURSES_VERSION_PATCH < 20081122
int _nc_has_mouse(void);
#define has_mouse _nc_has_mouse
#endif

#define TIME_DEBUG 0
#if TIME_DEBUG
static int spin_track_timeout = 0;
#endif

#define staticni ORCA_NOINLINE static

#ifndef ZORCA_VERSION
#define ZORCA_VERSION "unknown"
#endif

staticni void usage(void) { // clang-format off
fprintf(stderr,
"Usage: orca [options] [file]\n\n"
"General options:\n"
"    --undo-limit <number>  Set the maximum number of undo steps.\n"
"                           If you plan to work with large files,\n"
"                           set this to a low number.\n"
"                           Default: 100\n"
"    --initial-size <nxn>   When creating a new grid file, use these\n"
"                           starting dimensions.\n"
"    --bpm <number>         Set the tempo (beats per minute).\n"
"                           Default: 120\n"
"    --seed <number>        Set the seed for R, for lowercase r (each\n"
"                           cell seeds its own shuffle from it) and for\n"
"                           the random pattern (c) of ;.\n"
"                           Default: 1\n"
"    --dialect <name>       Set the operator dialect for this session:\n"
"                           borca (bOrca's operators) or upstream\n"
"                           (upstream Orca-c's: ! sends a plain CC, ;\n"
"                           UDP, = OSC, r is a banged R, $ and & do\n"
"                           nothing, and C, D, I, L, R, U and Z keep\n"
"                           Orca-c's rules for empty inputs and R's\n"
"                           range). Overrides the dialect saved in\n"
"                           orca.conf without changing it; the Operator\n"
"                           Dialect menu changes and saves it. Picking\n"
"                           the active dialect there saves nothing, so to\n"
"                           save this one, pick the other, then this.\n"
"                           Default: the saved dialect, else borca\n"
"    --version              Print version and exit.\n"
"    -h or --help           Print this message and exit.\n"
"\n"
"OSC/MIDI options:\n"
"    --strict-timing\n"
"        Attempt to reduce timing jitter of outgoing MIDI and OSC\n"
"        messages. Uses more CPU time. May have no effect.\n"
"\n"
"    --osc-midi-bidule <path>\n"
"        Set MIDI to be sent via OSC formatted for Plogue Bidule.\n"
"        The path argument is the path of the Plogue OSC MIDI device.\n"
"        Example: /OSC_MIDI_0/MIDI\n"
);
#ifdef ZORCA_HAS_MIDI_IN
fprintf(stderr,
"\n"
"    --clock-source internal|external\n"
"        Set the clock source for this session: internal (orca's own\n"
"        tempo, the default) or external (orca follows the MIDI clock\n"
"        of the MIDI input, which runs, stops and positions playback;\n"
"        sending MIDI beat clock is suspended meanwhile). Overrides the\n"
"        clock_source saved in orca.conf without changing it; the\n"
"        Clock & Timing menu changes and saves it.\n"
"    --midi-in <name>\n"
"        Set the MIDI input to follow for this session, over the one\n"
"        saved in orca.conf, without changing it. In an ALSA build the\n"
"        name is client:port, as the MIDI Input menu lists them; in a\n"
"        PortMidi build it is the device name. A name that is not found\n"
"        is reported on screen, and an ALSA build then waits for it.\n"
);
#endif
#ifdef FEAT_ALSA
fprintf(stderr,
"\n"
"ALSA MIDI (this build):\n"
"    MIDI goes out through an ALSA sequencer client named zOrca, or\n"
"    $ZORCA_ALSA_CLIENT_NAME when that is set and non-empty, with one\n"
"    port named MIDI out, and comes in through its port MIDI in, which\n"
"    orca itself subscribes to the MIDI input while the clock source is\n"
"    external. The client opens at startup even with --osc-midi-bidule,\n"
"    whose output then replaces it. If the sequencer cannot be opened,\n"
"    or another client already has the name (checked at startup, before\n"
"    and after taking the name), or the sequencer cannot list its\n"
"    clients, or the name is over 63 bytes (ALSA keeps 63; set a shorter\n"
"    ZORCA_ALSA_CLIENT_NAME), orca exits with status 1. SIGTERM, SIGHUP\n"
"    and SIGINT quit cleanly, stopping playback first, unless the signal\n"
"    was already ignored when orca started.\n"
);
#endif
} // clang-format on

typedef enum {
  Glyph_class_unknown,
  Glyph_class_grid,
  Glyph_class_comment,
  Glyph_class_uppercase,
  Glyph_class_lowercase,
  Glyph_class_movement,
  Glyph_class_numeric,
  Glyph_class_bang,
} Glyph_class;

// The class a glyph draws as. In the upstream dialect (sim.h) `$` and `&`
// are no operators, so they draw as unknown glyphs (bold red), as Orca-c
// 9df9786 draws them; bOrca's colours do not change.
static Glyph_class glyph_class_of(Glyph glyph, Orca_dialect dialect) {
  if (glyph == '.')
    return Glyph_class_grid;
  if (glyph >= '0' && glyph <= '9')
    return Glyph_class_numeric;
  if (dialect == Orca_dialect_upstream && (glyph == '$' || glyph == '&'))
    return Glyph_class_unknown;
  switch (glyph) {
  case 'N':
  case 'n':
  case 'E':
  case 'e':
  case 'S':
  case 's':
  case 'W':
  case 'w':
    return Glyph_class_movement;
  case '!':
  case ':':
  case ';':
  case '=':
  case '%':
  case '?':
  case '&':
    return Glyph_class_lowercase;
  case '$':
    return Glyph_class_uppercase;
  case '*':
    return Glyph_class_bang;
  case '#':
    return Glyph_class_comment;
  }
  if (glyph >= 'A' && glyph <= 'Z')
    return Glyph_class_uppercase;
  if (glyph >= 'a' && glyph <= 'z')
    return Glyph_class_lowercase;
  return Glyph_class_unknown;
}

static attr_t term_attrs_of_cell(Glyph g, Mark m, Orca_dialect dialect) {
  Glyph_class gclass = glyph_class_of(g, dialect);
  attr_t attr = A_normal;
  switch (gclass) {
  case Glyph_class_unknown:
    attr = A_bold | fg_bg(C_red, C_natural);
    break;
  case Glyph_class_grid:
    attr = A_dim | fg_bg(C_blue, C_natural);
    break;
  case Glyph_class_comment:
    attr = A_dim | Cdef_normal;
    break;
  case Glyph_class_uppercase:
    attr = A_normal | fg_bg(C_black, C_cyan);
    break;
  case Glyph_class_lowercase:
  case Glyph_class_movement:
  case Glyph_class_numeric:
    attr = A_bold | Cdef_normal;
    break;
  case Glyph_class_bang:
    attr = A_bold | Cdef_normal;
    break;
  }
  if (gclass != Glyph_class_comment) {
    if ((m & (Mark_flag_lock | Mark_flag_input)) ==
        (Mark_flag_lock | Mark_flag_input)) {
      // Standard locking input
      attr = A_normal | Cdef_normal;
    } else if ((m & Mark_flag_input) == Mark_flag_input) {
      // Non-locking input
      attr = A_normal | Cdef_normal;
    } else if (m & Mark_flag_lock) {
      // Locked only
      attr = A_dim | Cdef_normal;
    }
  }
  if (m & Mark_flag_output) {
    attr = A_reverse;
  }
  if (m & Mark_flag_haste_input) {
    attr = A_bold | fg_bg(C_cyan, C_natural);
  }
  return attr;
}

typedef enum {
  Ged_input_mode_normal = 0,
  Ged_input_mode_append,
  Ged_input_mode_selresize,
  Ged_input_mode_slide,
} Ged_input_mode;

typedef struct {
  Usz y, x, h, w;
} Ged_cursor;

void ged_cursor_init(Ged_cursor *tc) {
  tc->x = tc->y = 0;
  tc->w = tc->h = 1;
}

static void ged_cursor_move_relative(Ged_cursor *tc, Usz field_h, Usz field_w,
                                     Isz delta_y, Isz delta_x) {
  Isz y0 = (Isz)tc->y + delta_y;
  Isz x0 = (Isz)tc->x + delta_x;
  if (y0 >= (Isz)field_h)
    y0 = (Isz)field_h - 1;
  if (y0 < 0)
    y0 = 0;
  if (x0 >= (Isz)field_w)
    x0 = (Isz)field_w - 1;
  if (x0 < 0)
    x0 = 0;
  tc->y = (Usz)y0;
  tc->x = (Usz)x0;
}

staticni void draw_grid_cursor(WINDOW *win, int draw_y, int draw_x, int draw_h,
                               int draw_w, Glyph const *gbuffer, Usz field_h,
                               Usz field_w, int scroll_y, int scroll_x,
                               Usz cursor_y, Usz cursor_x, Usz cursor_h,
                               Usz cursor_w, Ged_input_mode input_mode,
                               bool is_playing) {
  (void)input_mode;
  if (cursor_y >= field_h || cursor_x >= field_w)
    return;
  if (scroll_y < 0) {
    draw_y += -scroll_y;
    scroll_y = 0;
  }
  if (scroll_x < 0) {
    draw_x += -scroll_x;
    scroll_x = 0;
  }
  Usz offset_y = (Usz)scroll_y;
  Usz offset_x = (Usz)scroll_x;
  if (offset_y >= field_h || offset_x >= field_w)
    return;
  if (draw_y >= draw_h || draw_x >= draw_w)
    return;
  attr_t const curs_attr = A_reverse | A_bold | fg_bg(C_yellow, C_natural);
  if (offset_y <= cursor_y && offset_x <= cursor_x) {
    Usz cdraw_y = cursor_y - offset_y + (Usz)draw_y;
    Usz cdraw_x = cursor_x - offset_x + (Usz)draw_x;
    if (cdraw_y < (Usz)draw_h && cdraw_x < (Usz)draw_w) {
      Glyph beneath = gbuffer[cursor_y * field_w + cursor_x];
      char displayed;
      if (beneath == '.') {
        displayed = is_playing ? '@' : '~';
      } else {
        displayed = beneath;
      }
      chtype ch = (chtype)displayed | curs_attr;
      wmove(win, (int)cdraw_y, (int)cdraw_x);
      waddchnstr(win, &ch, 1);
    }
  }

  // Early out for selection area that won't have any visual effect
  if (cursor_h <= 1 && cursor_w <= 1)
    return;

  // Now mutate visually selected area under grid to have the selection color
  // attributes. (This will rewrite the attributes on the cursor character we
  // wrote above, but if it was the only character that would have been
  // changed, we already early-outed.)
  //
  // We'll do this by reading back the characters on the grid from the curses
  // window buffer, changing the attributes, then writing it back. This is
  // easier than pulling the glyphs from the gbuffer, since we already did the
  // ruler calculations to turn . into +, and we don't need special behavior
  // for any other attributes (e.g. we don't show a special state for selected
  // uppercase characters.)
  //
  // First, confine cursor selection to the grid field/gbuffer that actually
  // exists, in case the cursor selection exceeds the area of the field.
  Usz sel_rows = field_h - cursor_y;
  if (cursor_h < sel_rows)
    sel_rows = cursor_h;
  Usz sel_cols = field_w - cursor_x;
  if (cursor_w < sel_cols)
    sel_cols = cursor_w;
  // Now, confine the selection area to what's visible on screen. Kind of
  // tricky since we have to handle it being partially visible from any edge on
  // any axis, and we have to be mindful overflow.
  Usz vis_sel_y;
  Usz vis_sel_x;
  if (offset_y > cursor_y) {
    vis_sel_y = 0;
    Usz sub_y = offset_y - cursor_y;
    if (sub_y > sel_rows)
      sel_rows = 0;
    else
      sel_rows -= sub_y;
  } else {
    vis_sel_y = cursor_y - offset_y;
  }
  if (offset_x > cursor_x) {
    vis_sel_x = 0;
    Usz sub_x = offset_x - cursor_x;
    if (sub_x > sel_cols)
      sel_cols = 0;
    else
      sel_cols -= sub_x;
  } else {
    vis_sel_x = cursor_x - offset_x;
  }
  vis_sel_y += (Usz)draw_y;
  vis_sel_x += (Usz)draw_x;
  if (vis_sel_y >= (Usz)draw_h || vis_sel_x >= (Usz)draw_w)
    return;
  Usz vis_sel_h = (Usz)draw_h - vis_sel_y;
  Usz vis_sel_w = (Usz)draw_w - vis_sel_x;
  if (sel_rows < vis_sel_h)
    vis_sel_h = sel_rows;
  if (sel_cols < vis_sel_w)
    vis_sel_w = sel_cols;
  if (vis_sel_w == 0 || vis_sel_h == 0)
    return;
  enum { Bufcount = 4096 };
  chtype chbuffer[Bufcount];
  if (Bufcount < vis_sel_w)
    vis_sel_w = Bufcount;
  for (Usz iy = 0; iy < vis_sel_h; ++iy) {
    int at_y = (int)(vis_sel_y + iy);
    int num = mvwinchnstr(win, at_y, (int)vis_sel_x, chbuffer, (int)vis_sel_w);
    for (int ix = 0; ix < num; ++ix) {
      chbuffer[ix] = (chtype)((chbuffer[ix] & (A_CHARTEXT | A_ALTCHARSET)) |
                              (chtype)curs_attr);
    }
    waddchnstr(win, chbuffer, (int)num);
  }
}

typedef struct Undo_node {
  Field field;
  Usz tick_num;
  struct Undo_node *prev, *next;
} Undo_node;

typedef struct {
  Undo_node *first, *last;
  Usz count, limit;
} Undo_history;

static void undo_history_init(Undo_history *hist, Usz limit) {
  *hist = (Undo_history){0};
  hist->limit = limit;
}
static void undo_history_deinit(Undo_history *hist) {
  Undo_node *a = hist->first;
  while (a) {
    Undo_node *b = a->next;
    field_deinit(&a->field);
    free(a);
    a = b;
  }
}

staticni bool undo_history_push(Undo_history *hist, Field *field,
                                Usz tick_num) {
  if (hist->limit == 0)
    return false;
  Undo_node *new_node;
  bool const reused = hist->count == hist->limit;
  if (reused) {
    new_node = hist->first;
    if (new_node == hist->last) {
      hist->first = NULL;
      hist->last = NULL;
    } else {
      hist->first = new_node->next;
      hist->first->prev = NULL;
    }
  } else {
    new_node = malloc(sizeof(Undo_node));
    if (!new_node)
      return false;
    ++hist->count;
    field_init(&new_node->field);
  }
  if (!field_copy(field, &new_node->field)) {
    if (reused) {
      // The oldest step, unchanged, goes back in front; the count stays.
      new_node->prev = NULL;
      new_node->next = hist->first;
      if (hist->first)
        hist->first->prev = new_node;
      else
        hist->last = new_node;
      hist->first = new_node;
    } else {
      field_deinit(&new_node->field);
      free(new_node);
      --hist->count;
    }
    return false;
  }
  new_node->tick_num = tick_num;
  if (hist->last) {
    hist->last->next = new_node;
    new_node->prev = hist->last;
  } else {
    hist->first = new_node;
    hist->last = new_node;
    new_node->prev = NULL;
  }
  new_node->next = NULL;
  hist->last = new_node;
  return true;
}

staticni void undo_history_pop(Undo_history *hist, Field *out_field,
                               Usz *out_tick_num) {
  Undo_node *last = hist->last;
  if (!last)
    return;
  // A failed copy leaves the grid, the tick number and the history as they
  // were.
  if (!field_copy(&last->field, out_field)) {
    beep();
    return;
  }
  *out_tick_num = last->tick_num;
  if (hist->first == last) {
    hist->first = NULL;
    hist->last = NULL;
  } else {
    Undo_node *new_last = last->prev;
    new_last->next = NULL;
    hist->last = new_last;
  }
  field_deinit(&last->field);
  free(last);
  --hist->count;
}

staticni void undo_history_apply(Undo_history *hist, Field *out_field,
                                 Usz *out_tick_num) {
  Undo_node *last = hist->last;
  if (!last)
    return;
  if (!field_copy(&last->field, out_field)) {
    beep();
    return;
  }
  *out_tick_num = last->tick_num;
}

static Usz undo_history_count(Undo_history *hist) { return hist->count; }

staticni void print_activity_indicator(WINDOW *win, Usz activity_counter) {
  // 7 segments that can each light up as Colors different colors.
  // This gives us Colors^Segments total configurations.
  enum { Segments = 7, Colors = 4 };
  Usz states = 1; // calculate Colors^Segments
  for (Usz i = 0; i < Segments; ++i)
    states *= Colors;
  // Wrap the counter to the range of displayable configurations.
  Usz val = activity_counter % states;
  chtype lamps[Colors];
#if 1 // Appearance where segments are always lit
  lamps[0] = ACS_HLINE | fg_bg(C_black, C_natural) | A_bold;
  lamps[1] = ACS_HLINE | fg_bg(C_white, C_natural) | A_normal;
  lamps[2] = ACS_HLINE | A_bold;
  lamps[3] = lamps[1];
#elif 0 // Brighter appearance where segments are always lit
  lamps[0] = ACS_HLINE | fg_bg(C_black, C_natural) | A_bold;
  lamps[1] = ACS_HLINE | A_normal;
  lamps[2] = ACS_HLINE | A_bold;
  lamps[3] = lamps[1];
#else   // Appearance where segments can turn off completely
  lamps[0] = ' ';
  lamps[1] = ACS_HLINE | fg_bg(C_black, C_natural) | A_bold;
  lamps[2] = ACS_HLINE | A_normal;
  lamps[3] = lamps[1];
#endif
  chtype buffer[Segments];
  for (Usz i = 0; i < Segments; ++i) {
    // Instead of a left-to-right, straightforward ascending least-to-most
    // significant digits display, we'll display it as a spiral.
    Usz j = i % 2 ? (6 - i / 2) : (i / 2);
    buffer[j] = lamps[val % Colors];
    val = val / Colors;
  }
  waddchnstr(win, buffer, Segments);
  // If you want to see what various combinations of colors and attributes look
  // like in different terminals.
#if 0
  waddch(win, 'a' | fg_bg(C_black, C_natural) | A_dim);
  waddch(win, 'b' | fg_bg(C_black, C_natural) | A_normal);
  waddch(win, 'c' | fg_bg(C_black, C_natural) | A_bold);
  waddch(win, 'd' | A_dim);
  waddch(win, 'e' | A_normal);
  waddch(win, 'f' | A_bold);
  waddch(win, 'g' | fg_bg(C_white, C_natural) | A_dim);
  waddch(win, 'h' | fg_bg(C_white, C_natural) | A_normal);
  waddch(win, 'i' | fg_bg(C_white, C_natural) | A_bold);
#endif
}

staticni void advance_faketab(WINDOW *win, int offset_x, int tabstop) {
  if (tabstop < 1)
    return;
  int y, x, h, w;
  getyx(win, y, x);
  getmaxyx(win, h, w);
  (void)h;
  x = ((x + tabstop - 1) / tabstop) * tabstop + offset_x % tabstop;
  if (w < 1)
    w = 1;
  if (x >= w)
    x = w - 1;
  wmove(win, y, x);
}

// bpm_text, when set, replaces the BPM number: the External clock's text
// (ext_bpm_text in midi_in.h), at most 8 characters, printed left-aligned in
// 9 columns, so the next field sits two tab stops on whatever the text or
// the HUD offset, and the HUD does not jump when the estimate appears.
// Internal keeps its %zu at one tab stop, as it always was, so a switch of
// source shifts the fields after this one by 8 columns.
staticni void draw_hud(WINDOW *win, int win_y, int win_x, int height, int width,
                       char const *filename, Usz field_h, Usz field_w,
                       Usz ruler_spacing_y, Usz ruler_spacing_x, Usz tick_num,
                       Usz bpm, char const *bpm_text,
                       Ged_cursor const *ged_cursor,
                       Ged_input_mode input_mode, Usz activity_counter,
                       Glyph const *gbuffer, Port_label_cell const *labels,
                       Usz labels_h, Usz labels_w, bool midi_err) {
  // Two rows: the tooltip's lines go on win_y and win_y + 1. Release builds
  // compile the assert out, so the parameter is also used by the cast.
  assert(height >= 2);
  (void)height;
  enum { Tabstop = 8 };
  wmove(win, win_y, win_x);
  wprintw(win, "%zux%zu", field_w, field_h);
  advance_faketab(win, win_x, Tabstop);
  wprintw(win, "%zu/%zu", ruler_spacing_x, ruler_spacing_y);
  advance_faketab(win, win_x, Tabstop);
  wprintw(win, "%zuf", tick_num);
  advance_faketab(win, win_x, Tabstop);
  if (bpm_text)
    wprintw(win, "%-9s", bpm_text);
  else
    wprintw(win, "%zu", bpm);
  advance_faketab(win, win_x, Tabstop);
  print_activity_indicator(win, activity_counter);
  // Where the MIDI err field goes, if a send has failed; it is drawn last.
  // The indicator is drawn with waddchnstr, which leaves the cursor on its
  // first column, so step past its 7 columns (Segments in
  // print_activity_indicator) before moving to the next tab stop.
  {
    enum { Indicator_width = 7 };
    int past_indicator = getcurx(win) + Indicator_width;
    wmove(win, win_y, past_indicator < width ? past_indicator : width - 1);
  }
  advance_faketab(win, win_x, Tabstop);
  int midi_err_x = getcurx(win);
  wmove(win, win_y + 1, win_x);
  wprintw(win, "%zu,%zu", ged_cursor->x, ged_cursor->y);
  advance_faketab(win, win_x, Tabstop);
  wprintw(win, "%zu:%zu", ged_cursor->w, ged_cursor->h);
  advance_faketab(win, win_x, Tabstop);
  switch (input_mode) {
  case Ged_input_mode_normal:
    wattrset(win, A_normal);
    waddstr(win, "insert");
    break;
  case Ged_input_mode_append:
    wattrset(win, A_bold);
    waddstr(win, "append");
    break;
  case Ged_input_mode_selresize:
    wattrset(win, A_bold);
    waddstr(win, "select");
    break;
  case Ged_input_mode_slide:
    wattrset(win, A_reverse);
    waddstr(win, "slide");
    break;
  }
  advance_faketab(win, win_x, Tabstop);
  wattrset(win, A_normal);
  // Clipped to the row, so a long name never wraps below the HUD.
  int filename_x = getcurx(win);
  int filename_len = (int)strlen(filename);
  if (filename_x < width)
    waddnstr(win, filename, width - filename_x);

  // The tooltip for the port under the cursor (tooltips.h), on the HUD's
  // two rows, right of each row's last field, cut from the end to fit. Row
  // 1's last field is the 8-column MIDI err slot at midi_err_x, reserved
  // whether or not the error shows; row 2's is the filename. Every field
  // is drawn before it, and MIDI err after it.
  char tip1[Tooltip_line_max], tip2[Tooltip_line_max];
  if (tooltip_at(labels, labels_h, labels_w, gbuffer, field_h, field_w,
                 ged_cursor->y, ged_cursor->x, tip1, tip2)) {
    enum { Midi_err_width = 8 };
    char const *lines[2] = {tip1, tip2};
    int limits[2] = {midi_err_x + Midi_err_width, filename_x + filename_len};
    for (int row = 0; row < 2; ++row) {
      int len = (int)strlen(lines[row]);
      if (len == 0)
        continue;
      int limit = limits[row] < width ? limits[row] : width;
      int x;
      int n = tooltip_place(limit, len, width, &x);
      if (n <= 0)
        continue;
      wmove(win, win_y + row, x);
      wattrset(win, A_reverse);
      waddnstr(win, lines[row], n);
      wattrset(win, A_normal);
    }
  }
  if (midi_err) {
    // Sticky until quit. Drawn after the tooltip, which sits right of its
    // slot, and pulled left to fit a narrow window, so neither can hide it.
    static char const midi_err_text[] = "MIDI err";
    int text_len = (int)sizeof midi_err_text - 1;
    int x = midi_err_x;
    if (x > width - text_len)
      x = width - text_len;
    if (x < 0)
      x = 0;
    if (x < width) {
      wmove(win, win_y, x);
      wattrset(win, A_bold);
      waddnstr(win, midi_err_text, width - x);
      wattrset(win, A_normal);
    }
  }
}

staticni void draw_glyphs_grid(WINDOW *win, int draw_y, int draw_x, int draw_h,
                               int draw_w, Glyph const *restrict gbuffer,
                               Mark const *restrict mbuffer, Usz field_h,
                               Usz field_w, Usz offset_y, Usz offset_x,
                               Usz ruler_spacing_y, Usz ruler_spacing_x,
                               bool use_fancy_dots, bool use_fancy_rulers,
                               Orca_dialect dialect) {
  assert(draw_y >= 0 && draw_x >= 0);
  assert(draw_h >= 0 && draw_w >= 0);
  enum { Bufcount = 4096 };
  chtype chbuffer[Bufcount];
  // todo buffer limit
  if (offset_y >= field_h || offset_x >= field_w)
    return;
  if (draw_y >= draw_h || draw_x >= draw_w)
    return;
  Usz rows = (Usz)(draw_h - draw_y);
  if (field_h - offset_y < rows)
    rows = field_h - offset_y;
  Usz cols = (Usz)(draw_w - draw_x);
  if (field_w - offset_x < cols)
    cols = field_w - offset_x;
  if (Bufcount < cols)
    cols = Bufcount;
  if (rows == 0 || cols == 0)
    return;
  bool use_rulers = ruler_spacing_y != 0 && ruler_spacing_x != 0;
  chtype bullet = use_fancy_dots ? ACS_BULLET : '.';
  enum { T = 1 << 0, B = 1 << 1, L = 1 << 2, R = 1 << 3 };
  chtype rs[(T | B | L | R) + 1];
  if (use_rulers) {
    for (Usz i = 0; i < sizeof rs / sizeof(chtype); ++i)
      rs[i] = '+';
    if (use_fancy_rulers) {
      rs[T | L] = ACS_ULCORNER;
      rs[T | R] = ACS_URCORNER;
      rs[B | L] = ACS_LLCORNER;
      rs[B | R] = ACS_LRCORNER;
      rs[T] = ACS_TTEE;
      rs[B] = ACS_BTEE;
      rs[L] = ACS_LTEE;
      rs[R] = ACS_RTEE;
    }
  }
  for (Usz iy = 0; iy < rows; ++iy) {
    Usz line_offset = (offset_y + iy) * field_w + offset_x;
    Glyph const *g_row = gbuffer + line_offset;
    Mark const *m_row = mbuffer + line_offset;
    bool use_y_ruler = use_rulers && (iy + offset_y) % ruler_spacing_y == 0;
    for (Usz ix = 0; ix < cols; ++ix) {
      Glyph g = g_row[ix];
      Mark m = m_row[ix];
      chtype ch;
      if (g == '.') {
        if (use_y_ruler && (ix + offset_x) % ruler_spacing_x == 0) {
          int p = 0; // clang-format off
          if (iy + offset_y     == 0      ) p |= T;
          if (iy + offset_y + 1 == field_h) p |= B;
          if (ix + offset_x     == 0      ) p |= L;
          if (ix + offset_x + 1 == field_w) p |= R;
          ch = rs[p]; // clang-format on
        } else {
          ch = bullet;
        }
      } else {
        ch = (chtype)g;
      }
      attr_t attrs = term_attrs_of_cell(g, m, dialect);
      chbuffer[ix] = ch | attrs;
    }
    wmove(win, draw_y + (int)iy, draw_x);
    waddchnstr(win, chbuffer, (int)cols);
  }
}

staticni void draw_glyphs_grid_scrolled(
    WINDOW *win, int draw_y, int draw_x, int draw_h, int draw_w,
    Glyph const *restrict gbuffer, Mark const *restrict mbuffer, Usz field_h,
    Usz field_w, int scroll_y, int scroll_x, Usz ruler_spacing_y,
    Usz ruler_spacing_x, bool use_fancy_dots, bool use_fancy_rulers,
    Orca_dialect dialect) {
  if (scroll_y < 0) {
    draw_y += -scroll_y;
    scroll_y = 0;
  }
  if (scroll_x < 0) {
    draw_x += -scroll_x;
    scroll_x = 0;
  }
  draw_glyphs_grid(win, draw_y, draw_x, draw_h, draw_w, gbuffer, mbuffer,
                   field_h, field_w, (Usz)scroll_y, (Usz)scroll_x,
                   ruler_spacing_y, ruler_spacing_x, use_fancy_dots,
                   use_fancy_rulers, dialect);
}

static void ged_cursor_confine(Ged_cursor *tc, Usz height, Usz width) {
  if (height == 0 || width == 0)
    return;
  if (tc->y >= height)
    tc->y = height - 1;
  if (tc->x >= width)
    tc->x = width - 1;
}

staticni void draw_oevent_list(WINDOW *win, Oevent_list const *oevent_list) {
  wmove(win, 0, 0);
  int win_h = getmaxy(win);
  wprintw(win, "Count: %d", (int)oevent_list->count);
  for (Usz i = 0, num_events = oevent_list->count; i < num_events; ++i) {
    int cury = getcury(win);
    if (cury + 1 >= win_h)
      return;
    wmove(win, cury + 1, 0);
    Oevent const *ev = oevent_list->buffer + i;
    Oevent_types evt = ev->any.oevent_type;
    switch (evt) {
    case Oevent_type_midi_note: {
      Oevent_midi_note const *em = &ev->midi_note;
      wprintw(
          win,
          "MIDI Note\tchannel %d\toctave %d\tnote %d\tvelocity %d\tlength %d",
          (int)em->channel, (int)em->octave, (int)em->note, (int)em->velocity,
          (int)em->duration);
      break;
    }
    case Oevent_type_midi_cc: {
      Oevent_midi_cc const *ec = &ev->midi_cc;
      wprintw(win, "MIDI CC\tchannel %d\tcontrol %d\tvalue %d",
              (int)ec->channel, (int)ec->control, (int)ec->value);
      break;
    }
    case Oevent_type_midi_cc_interpolated: {
      Oevent_midi_cc_interpolated const *eci = &ev->midi_cc_interpolated;
      wprintw(win, "MIDI CC Interp\tchannel %d\tcontrol %d\ttarget %d\trate %d",
              (int)eci->channel, (int)eci->control, (int)eci->target_value, (int)eci->interpolation_rate);
      break;
    }
    case Oevent_type_midi_pb: {
      Oevent_midi_pb const *ep = &ev->midi_pb;
      wprintw(win, "MIDI PB\tchannel %d\tmsb %d\tlsb %d", (int)ep->channel,
              (int)ep->msb, (int)ep->lsb);
      break;
    }
    case Oevent_type_osc_ints: {
      Oevent_osc_ints const *eo = &ev->osc_ints;
      wprintw(win, "OSC\t%c\tcount: %d ", eo->glyph, eo->count);
      waddch(win, ACS_VLINE);
      for (Usz j = 0; j < eo->count; ++j) {
        wprintw(win, " %d", eo->numbers[j]);
      }
      break;
    }
    case Oevent_type_udp_string: {
      Oevent_udp_string const *eo = &ev->udp_string;
      wprintw(win, "UDP\tcount %d\t", (int)eo->count);
      for (Usz j = 0; j < (Usz)eo->count; ++j) {
        waddch(win, (chtype)eo->chars[j]);
      }
      break;
    }
    }
  }
}

// Returns false when an allocation fails, with the grid, the undo history,
// the operator state and the cursor as they were (the mark buffer may have
// grown, which is harmless). The mark buffer grows first, so it covers every
// grid that has been live and undo can restore any of them; the undo step is
// the copy of the old grid in scratch_field, pushed once the grid has its new
// size.
staticni bool ged_resize_grid(Field *field, Mbuf_reusable *mbr, Usz new_height,
                              Usz new_width, Usz tick_num, Field *scratch_field,
                              Undo_history *undo_hist, Ged_cursor *ged_cursor,
                              Opstate_store *opstate) {
  assert(new_height > 0 && new_width > 0);
  if (!mbuf_reusable_ensure_size(mbr, new_height, new_width) ||
      !field_copy(field, scratch_field) ||
      !field_resize_raw(field, new_height, new_width)) {
    beep(); // for the keys, Autofit and the Set grid dims form alike
    return false;
  }
  undo_history_push(undo_hist, scratch_field, tick_num);
  // Drop the operator state of the cells the new size cuts off; every cell
  // inside keeps its state. If the rebuild cannot allocate, clear the
  // store instead (clear never allocates): kept entries outside the grid
  // would come back with stale state on a later enlarge or undo.
  if (!opstate_prune(opstate, new_height, new_width))
    opstate_clear(opstate);
  // junky copies until i write a smarter thing
  memset(field->buffer, '.', new_height * new_width * sizeof(Glyph));
  gbuffer_copy_subrect(scratch_field->buffer, field->buffer,
                       scratch_field->height, scratch_field->width,
                       field->height, field->width, 0, 0, 0, 0,
                       scratch_field->height, scratch_field->width);
  ged_cursor_confine(ged_cursor, new_height, new_width);
  return true;
}

staticni Usz adjust_rulers_humanized(Usz ruler, Usz in, Isz delta_rulers) {
  // slightly more confusing because desired grid sizes are +1 (e.g. ruler of
  // length 8 wants to snap to 25 and 33, not 24 and 32). also this math is
  // sloppy.
  assert(ruler > 0);
  if (in == 0)
    return delta_rulers > 0 ? ruler * (Usz)delta_rulers : 1;
  // could overflow if inputs are big
  if (delta_rulers < 0)
    in += ruler - 1;
  Isz n = ((Isz)in - 1) / (Isz)ruler + delta_rulers;
  if (n < 0)
    n = 0;
  return ruler * (Usz)n + 1;
}

// Resizes by number of ruler divisions, and snaps size to closest division in
// a way a human would expect. Adds +1 to the output, so grid resulting size is
// 1 unit longer than the actual ruler length.
staticni bool ged_resize_grid_snap_ruler(Field *field, Mbuf_reusable *mbr,
                                         Usz ruler_y, Usz ruler_x, Isz delta_h,
                                         Isz delta_w, Usz tick_num,
                                         Field *scratch_field,
                                         Undo_history *undo_hist,
                                         Ged_cursor *ged_cursor,
                                         Opstate_store *opstate) {
  assert(ruler_y > 0);
  assert(ruler_x > 0);
  Usz field_h = field->height;
  Usz field_w = field->width;
  assert(field_h > 0);
  assert(field_w > 0);
  if (ruler_y == 0 || ruler_x == 0 || field_h == 0 || field_w == 0)
    return false;
  Usz new_field_h = field_h;
  Usz new_field_w = field_w;
  if (delta_h != 0)
    new_field_h = adjust_rulers_humanized(ruler_y, field_h, delta_h);
  if (delta_w != 0)
    new_field_w = adjust_rulers_humanized(ruler_x, field_w, delta_w);
  if (new_field_h > ORCA_Y_MAX)
    new_field_h = ORCA_Y_MAX;
  if (new_field_w > ORCA_X_MAX)
    new_field_w = ORCA_X_MAX;
  if (new_field_h == field_h && new_field_w == field_w)
    return false;
  return ged_resize_grid(field, mbr, new_field_h, new_field_w, tick_num,
                         scratch_field, undo_hist, ged_cursor, opstate);
}

typedef enum {
  Midi_mode_type_null,
  Midi_mode_type_osc_bidule,
#ifdef FEAT_ALSA
  Midi_mode_type_alsa,
#endif
#ifdef FEAT_PORTMIDI
  Midi_mode_type_portmidi,
#endif
} Midi_mode_type;

typedef struct {
  Midi_mode_type type;
} Midi_mode_any;

typedef struct {
  Midi_mode_type type;
  char const *path;
} Midi_mode_osc_bidule;

#ifdef FEAT_ALSA
// The shell's ALSA sequencer context: one nonblocking duplex client per
// process, opened before initscr in every --alsa build, independent of
// Midi_mode. The alsa Midi_mode variant borrows it for its sends, the input
// reader borrows it through its own pointer (so the reader also works under
// --osc-midi-bidule), and only quit closes it.
// The send path sees the Midi_mode as const, so the counters live here,
// reached through the variant's pointer.
typedef struct {
  snd_seq_t *seq;
  int port;    // "MIDI out": readable and subscribable, port 0
  int in_port; // "MIDI in": write-only, the input reader's port
  // The source zOrca has subscribed to MIDI in, while one is stored; the
  // subscriptions exist only while the clock source is External.
  Alsa_in_addr source;
  bool announce_subscribed; // System Announce (0:1) is connected to MIDI in
  // Sends that did not go out, dropped or failed; saturating. Nonzero shows
  // the sticky "MIDI err" field on the HUD.
  Usz send_failures;
  // Note-offs the sequencer refused, kept to be sent again (midi_in.h); its
  // held, resent and lost totals are printed to stderr after endwin in a
  // counters build.
  Pending_offs pending_offs;
#ifdef ZORCA_DEBUG_COUNTERS
  // Of those, the sends dropped on -EAGAIN or -EINTR. Printed to stderr after
  // endwin.
  Usz send_drops;
  // The input pool the client has after alsa_ctx_open asked for
  // Alsa_input_pool, read back with snd_seq_get_client_pool; -1 when it could
  // not be read. Printed to stderr after endwin.
  long input_pool;
#endif
} Alsa_ctx;

// The client name: zOrca, or $ZORCA_ALSA_CLIENT_NAME when it is set and
// non-empty.
static char const *alsa_client_name(void) {
  return alsa_client_name_pick(getenv("ZORCA_ALSA_CLIENT_NAME"));
}

// One step of the name scan over the sequencer (midi_in.h alsa_name_scan):
// snd_seq_query_next_client, whose cursor is the client info itself.
typedef struct {
  snd_seq_t *seq;
  snd_seq_client_info_t *info;
} Alsa_name_query;

static int alsa_name_next_client(void *arg, int *client, char const **name) {
  Alsa_name_query *q = arg;
  int err = snd_seq_query_next_client(q->seq, q->info);
  if (err < 0)
    return err;
  *client = snd_seq_client_info_get_client(q->info);
  *name = snd_seq_client_info_get_name(q->info);
  return err;
}

// Returns 1 if a client other than seq's own is named name, 0 if none is, or
// a negative ALSA error code, from the malloc or from a scan that failed
// before the end of the list: a failed scan is never a free name.
staticni int alsa_client_name_taken(snd_seq_t *seq, char const *name) {
  snd_seq_client_info_t *info;
  int err = snd_seq_client_info_malloc(&info);
  if (err < 0)
    return err;
  snd_seq_client_info_set_client(info, -1);
  Alsa_name_query q = {.seq = seq, .info = info};
  int taken =
      alsa_name_scan(alsa_name_next_client, &q, snd_seq_client_id(seq), name);
  snd_seq_client_info_free(info);
  return taken;
}

// The input pool alsa_ctx_open asks for, in events. 500 is chosen
// between the kernel's default 200 and its maximum 2000
// (a larger request than 2000 is ignored and still returns 0). It holds
// about 10 s of F8s alone at 120 BPM (4.2 s at 300), fewer when other events
// reach the client during a stall, and it bounds the burst of missed ticks on
// resume near 83 steps (500 / 6) at any tempo; past it the kernel flushes the
// input FIFO, and the reader's overflow input keeps the clock following
// (transport.h). It costs about 23 KiB of kernel memory (500 cells of 48
// bytes on arm64 without CONFIG_SND_SEQ_UMP), against 9.4 KiB for the default
// 200. A failure leaves the default, and the overflow rule covers either.
enum { Alsa_input_pool = 500 };

// Opens the client named alsa_client_name(), duplex, with the port "MIDI
// out" that others can read and subscribe to, then "MIDI in", write-only,
// that the input reader subscribes sources to. The appliance's router and
// verify match the name, "MIDI out", its port number and its capability set
// exactly, and neither sees a write-only port. Returns 0, -ENAMETOOLONG if
// the name is over Alsa_client_name_max (63) bytes, which the sequencer would
// cut short (checked before the sequencer is opened, so nothing is), -EEXIST
// if another client already has the name, or another negative ALSA error
// code, with nothing left open; *scan_failed is set when that error came from
// a name scan (the sequencer could not list its clients, or the client info
// could not be allocated), so the caller can say so.
staticni int alsa_ctx_open(Alsa_ctx *ctx, bool *scan_failed) {
  *ctx = (Alsa_ctx){.seq = NULL, .port = -1, .in_port = -1};
  pending_offs_init(&ctx->pending_offs);
  *scan_failed = false;
  char const *client_name = alsa_client_name();
  // A longer name would be stored cut short, and the scans below compare the
  // whole name, so they could never find it (midi_in.h).
  if (!alsa_client_name_fits(client_name))
    return -ENAMETOOLONG;
  snd_seq_t *seq;
  int err =
      snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
  if (err < 0)
    return err;
  // Two clients with one name make the appliance router refuse to route
  // either, so refuse a name another client holds. No wait: the kernel drops
  // a client when its process exits, and borca-tty.service
  // (KillMode=control-group) starts a new instance only after the old one
  // has stopped; if the name is still held, its Restart=always retries.
  // This scan refuses an ordinary duplicate before it takes the name. A
  // second scan, right after the rename and before any port exists, catches
  // two instances started at the same moment: at most one same-name instance
  // survives, possibly none, and Restart=always retries.
  err = alsa_client_name_taken(seq, client_name);
  if (err < 0) {
    *scan_failed = true;
    goto fail;
  }
  if (err > 0) {
    err = -EEXIST;
    goto fail;
  }
  err = snd_seq_set_client_name(seq, client_name);
  if (err < 0)
    goto fail;
  // The second scan: of two instances that renamed together, whichever
  // rescans second sees the other. No port exists yet, so a refused client
  // never shows a MIDI out port to the router.
  err = alsa_client_name_taken(seq, client_name);
  if (err < 0) {
    *scan_failed = true;
    goto fail;
  }
  if (err > 0) {
    err = -EEXIST;
    goto fail;
  }
  err = snd_seq_create_simple_port(
      seq, "MIDI out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (err < 0)
    goto fail;
  ctx->port = err;
  err = snd_seq_create_simple_port(
      seq, "MIDI in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (err < 0)
    goto fail;
  ctx->in_port = err;
  (void)snd_seq_set_client_pool_input(seq, Alsa_input_pool);
#ifdef ZORCA_DEBUG_COUNTERS
  {
    snd_seq_client_pool_t *pool;
    snd_seq_client_pool_alloca(&pool);
    ctx->input_pool =
        snd_seq_get_client_pool(seq, pool) < 0
            ? -1
            : (long)snd_seq_client_pool_get_input_pool(pool);
  }
#endif
  ctx->seq = seq;
  return 0;
fail:
  snd_seq_close(seq);
  return err;
}

staticni void alsa_ctx_close(Alsa_ctx *ctx) {
  if (ctx->seq)
    snd_seq_close(ctx->seq);
  ctx->seq = NULL;
}

// The input reader's subscriptions. They exist only while the clock
// source is External; the shell waits for System Announce while no source
// address is stored, and never retries on a timer.

// Finds the first readable, subscribable port whose client name is the
// first client_len bytes of name and whose port name is port_name, both
// exact. True with its address.
staticni bool alsa_in_find(snd_seq_t *seq, char const *name, Usz client_len,
                           char const *port_name, int *client, int *port) {
  snd_seq_client_info_t *cinfo;
  snd_seq_port_info_t *pinfo;
  snd_seq_client_info_alloca(&cinfo);
  snd_seq_port_info_alloca(&pinfo);
  snd_seq_client_info_set_client(cinfo, -1);
  while (snd_seq_query_next_client(seq, cinfo) >= 0) {
    char const *cname = snd_seq_client_info_get_name(cinfo);
    if (strlen(cname) != client_len || strncmp(cname, name, client_len) != 0)
      continue;
    int cid = snd_seq_client_info_get_client(cinfo);
    snd_seq_port_info_set_client(pinfo, cid);
    snd_seq_port_info_set_port(pinfo, -1);
    while (snd_seq_query_next_port(seq, pinfo) >= 0) {
      // A write-only port of the same name cannot be subscribed to.
      unsigned const want = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
      if ((snd_seq_port_info_get_capability(pinfo) & want) != want)
        continue;
      if (strcmp(snd_seq_port_info_get_name(pinfo), port_name) == 0) {
        *client = cid;
        *port = snd_seq_port_info_get_port(pinfo);
        return true;
      }
    }
  }
  return false;
}

// Matches the `client:port` name and subscribes it to MIDI in, storing its
// address. Returns 0 when subscribed (-EBUSY, already connected, counts as
// subscribed, so a re-match is idempotent), 1 when no port matches, 2 when
// the name does not split (so no port ever will), or a negative ALSA error.
staticni int alsa_in_match(Alsa_ctx *ctx, char const *name) {
  Usz client_len;
  char const *port_name;
  if (!alsa_in_split(name, &client_len, &port_name))
    return 2;
  int client, port;
  if (!alsa_in_find(ctx->seq, name, client_len, port_name, &client, &port))
    return 1;
  int err = snd_seq_connect_from(ctx->seq, ctx->in_port, client, port);
  if (err < 0 && err != -EBUSY)
    return err;
  ctx->source = (Alsa_in_addr){.has = true, .client = client, .port = port};
  return 0;
}

// Subscribes System Announce (0:1) first, then the source. The caller drops
// the queued input before this, never after: an announce queued once 0:1 is
// connected must be read. Returns as alsa_in_match does; with 1 the shell
// waits for PORT_START.
staticni int alsa_in_subscribe(Alsa_ctx *ctx, char const *name) {
  if (!ctx->announce_subscribed) {
    int err = snd_seq_connect_from(ctx->seq, ctx->in_port,
                                   SND_SEQ_CLIENT_SYSTEM,
                                   SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    if (err < 0 && err != -EBUSY)
      return err;
    ctx->announce_subscribed = true;
  }
  return alsa_in_match(ctx, name);
}

// Leaving External: disconnects the source and System Announce.
staticni void alsa_in_unsubscribe(Alsa_ctx *ctx) {
  if (ctx->source.has) {
    snd_seq_disconnect_from(ctx->seq, ctx->in_port, ctx->source.client,
                            ctx->source.port);
    ctx->source.has = false;
  }
  if (ctx->announce_subscribed) {
    snd_seq_disconnect_from(ctx->seq, ctx->in_port, SND_SEQ_CLIENT_SYSTEM,
                            SND_SEQ_PORT_SYSTEM_ANNOUNCE);
    ctx->announce_subscribed = false;
  }
}

// After initscr, libasound's default error handler would print over the
// screen; this one discards the message. Failed sends still show through the
// MIDI err field.
static void alsa_silent_error_handler(char const *file, int line,
                                      char const *function, int err,
                                      char const *fmt, ...) {
  (void)file;
  (void)line;
  (void)function;
  (void)err;
  (void)fmt;
}

// Counts a send that did not go out. Never sleeps or prints, and sends
// nothing again itself: a refused note-off is kept by send_midi_3bytes in
// pending_offs, and the main loop tries it again (ged_retry_note_offs).
static void alsa_ctx_count_send_failure(Alsa_ctx *ctx, int err) {
#ifdef ZORCA_DEBUG_COUNTERS
  if (err == -EAGAIN || err == -EINTR)
    ++ctx->send_drops;
#else
  (void)err;
#endif
  if (ctx->send_failures < SIZE_MAX)
    ++ctx->send_failures;
}

typedef struct {
  Midi_mode_type type;
  Alsa_ctx *ctx;             // borrowed; only quit closes it
  snd_midi_event_t *encoder; // owned by this variant
} Midi_mode_alsa;
#endif

#ifdef FEAT_PORTMIDI
typedef struct {
  Midi_mode_type type;
  PmDeviceID device_id;
  PortMidiStream *stream;
} Midi_mode_portmidi;
// Not sure whether it's OK to call Pm_Terminate() without having a successful
// call to Pm_Initialize() -- let's just treat it with tweezers.
static bool portmidi_is_initialized = false;
#endif

typedef union {
  Midi_mode_any any;
  Midi_mode_osc_bidule osc_bidule;
#ifdef FEAT_ALSA
  Midi_mode_alsa alsa;
#endif
#ifdef FEAT_PORTMIDI
  Midi_mode_portmidi portmidi;
#endif
} Midi_mode;

void midi_mode_init_null(Midi_mode *mm) { mm->any.type = Midi_mode_type_null; }
#ifdef FEAT_ALSA
// Borrows ctx, which must stay open until the variant is deinitialized.
// Returns 0, or a negative ALSA error code with mm unchanged.
staticni int midi_mode_init_alsa(Midi_mode *mm, Alsa_ctx *ctx) {
  snd_midi_event_t *encoder;
  int err = snd_midi_event_new(16, &encoder);
  if (err < 0)
    return err;
  mm->alsa = (Midi_mode_alsa){
      .type = Midi_mode_type_alsa, .ctx = ctx, .encoder = encoder};
  return 0;
}
#endif
// Whether a MIDI send has failed since startup. Sticky until quit.
static bool midi_mode_send_failed(Midi_mode const *mm) {
#ifdef FEAT_ALSA
  if (mm->any.type == Midi_mode_type_alsa)
    return mm->alsa.ctx->send_failures != 0;
#endif
  (void)mm;
  return false;
}
void midi_mode_init_osc_bidule(Midi_mode *mm, char const *path) {
  mm->osc_bidule.type = Midi_mode_type_osc_bidule;
  mm->osc_bidule.path = path;
}
#ifdef FEAT_PORTMIDI
enum {
  Portmidi_artificial_latency = 1,
};
struct {
  U64 clock_base;
  bool did_init;
} portmidi_global_data;
static PmTimestamp portmidi_timestamp_now(void) {
  if (!portmidi_global_data.did_init) {
    portmidi_global_data.did_init = true;
    portmidi_global_data.clock_base = stm_now();
  }
  return (PmTimestamp)(stm_ms(stm_since(portmidi_global_data.clock_base)));
}
static PmTimestamp portmidi_timeproc(void *time_info) {
  (void)time_info;
  return portmidi_timestamp_now();
}
static PmError portmidi_init_if_necessary(void) {
  if (portmidi_is_initialized)
    return 0;
  PmError e = Pm_Initialize();
  if (e)
    return e;
  portmidi_is_initialized = true;
  return 0;
}
staticni PmError midi_mode_init_portmidi(Midi_mode *mm, PmDeviceID dev_id) {
  PmError e = portmidi_init_if_necessary();
  if (e)
    goto fail;
  e = Pm_OpenOutput(&mm->portmidi.stream, dev_id, NULL, 128, portmidi_timeproc,
                    NULL, Portmidi_artificial_latency);
  if (e)
    goto fail;
  mm->portmidi.type = Midi_mode_type_portmidi;
  mm->portmidi.device_id = dev_id;
  return pmNoError;
fail:
  midi_mode_init_null(mm);
  return e;
}
// Returns true on success. todo currently output only
staticni bool portmidi_find_device_id_by_name(char const *name, Usz namelen,
                                              PmError *out_pmerror,
                                              PmDeviceID *out_id) {
  *out_pmerror = portmidi_init_if_necessary();
  if (*out_pmerror)
    return false;
  int num = Pm_CountDevices();
  for (int i = 0; i < num; ++i) {
    PmDeviceInfo const *info = Pm_GetDeviceInfo(i);
    if (!info || !info->output)
      continue;
    Usz len = strlen(info->name);
    if (len != namelen)
      continue;
    if (strncmp(name, info->name, namelen) == 0) {
      *out_id = i;
      return true;
    }
  }
  return false;
}
static bool portmidi_find_name_of_device_id(PmDeviceID id, PmError *out_pmerror,
                                            oso **out_name) {
  *out_pmerror = portmidi_init_if_necessary();
  if (*out_pmerror)
    return false;
  int num = Pm_CountDevices();
  if (id < 0 || id >= num)
    return false;
  PmDeviceInfo const *info = Pm_GetDeviceInfo(id);
  if (!info || !info->output)
    return false;
  osoput(out_name, info->name);
  return true;
}
// The input-direction variants: an exact name, and the first hit wins.
staticni bool portmidi_find_input_id_by_name(char const *name,
                                             PmError *out_pmerror,
                                             PmDeviceID *out_id) {
  *out_pmerror = portmidi_init_if_necessary();
  if (*out_pmerror)
    return false;
  int num = Pm_CountDevices();
  for (int i = 0; i < num; ++i) {
    PmDeviceInfo const *info = Pm_GetDeviceInfo(i);
    if (!info || !info->input)
      continue;
    if (strcmp(name, info->name) == 0) {
      *out_id = i;
      return true;
    }
  }
  return false;
}
static bool portmidi_find_name_of_input_id(PmDeviceID id, PmError *out_pmerror,
                                           oso **out_name) {
  *out_pmerror = portmidi_init_if_necessary();
  if (*out_pmerror)
    return false;
  int num = Pm_CountDevices();
  if (id < 0 || id >= num)
    return false;
  PmDeviceInfo const *info = Pm_GetDeviceInfo(id);
  if (!info || !info->input)
    return false;
  osoput(out_name, info->name);
  return true;
}
#endif
staticni void midi_mode_deinit(Midi_mode *mm) {
  switch (mm->any.type) {
#ifdef FEAT_ALSA
  case Midi_mode_type_alsa:
    // The context is borrowed: only quit closes it.
    snd_midi_event_free(mm->alsa.encoder);
    break;
#endif
  case Midi_mode_type_null:
  case Midi_mode_type_osc_bidule:
    break;
#ifdef FEAT_PORTMIDI
  case Midi_mode_type_portmidi:
    // Because PortMidi seems to work correctly ony more platforms when using
    // its timing stuff, we are using it. And because we are using it, and
    // because it may be buffering events for sending 'later', we might have
    // pending outgoing MIDI events. We'll need to wait until they finish being
    // before calling Pm_Close, otherwise users could have problems like MIDI
    // notes being stuck on. This is slow and blocking, but not much we can do
    // about it right now.
    //
    // TODO use nansleep on platforms that support it.
    for (U64 start = stm_now();
         stm_ms(stm_since(start)) <= (double)Portmidi_artificial_latency;)
      sleep(0);
    Pm_Close(mm->portmidi.stream);
    break;
#endif
  }
}

typedef struct {
  Field field;
  Field scratch_field;
  // The per-cell state of &, ; and r. Ticks and step-forward run on opstate;
  // the paused re-mark runs on scratch_opstate, overwritten with a copy of
  // opstate before each pass.
  Opstate_store opstate;
  Opstate_store scratch_opstate;
  // The operator list every VM run uses: ticks, step-forward and the
  // preview. Changed only through ged_set_dialect.
  Orca_dialect dialect;
  Field clipboard_field;
  Mbuf_reusable mbuf_r;
  // The label buffer (ids from portlabel.h): one reusable buffer that
  // the tick, step-forward and the paused preview all pass to the VM, sized
  // for the field before every run by ged_labels_for_run, which remembers
  // the height and width it sized it for; draw_hud passes those to the
  // tooltip lookup, which shows nothing while they differ from the field's.
  Port_label_cell *labels;
  Usz labels_capacity; // cells allocated
  Usz labels_h, labels_w; // the size of the last run, 0 x 0 after a failure
  Undo_history undo_hist;
  // The three event lists. oevent_list is the tick list: the VM's
  // events on each tick, sent, and shown by Ctrl+E. scratch_oevent_list is
  // the paused re-mark's (the preview's) and is never sent.
  // engine_oevent_list is the CC engine's output, cleared before each poll
  // or submit and then sent.
  Oevent_list oevent_list;
  Oevent_list scratch_oevent_list;
  Oevent_list engine_oevent_list;
  // The CC engine: every CC of the tick path goes through it,
  // and it keeps the glides and the last value sent on each controller. The
  // shell drives its lifecycle: cancel on pause, cancel and forget on open
  // and new, forget when the output changes, configure on the glide_hz
  // setting. The tick body polls it, and so does the wait point between
  // ticks whenever its deadline has passed (ged_poll_ccout).
  Ccout_engine ccout;
  Susnote_list susnote_list;
  Ged_cursor ged_cursor;
  // The clock source. Internal is the 4f349cd timing path:
  // ged_do_stuff runs the ticks on its deadline. Under External the
  // transport machine runs them from the MIDI input's clock, is_playing
  // changes only through its actions, and clock-out is suspended. Changed
  // only through tui_set_clock_source, which resets the machine.
  Clock_source clock_source;
  Transport transport;
  Usz tick_num;
  Usz ruler_spacing_y, ruler_spacing_x;
  Ged_input_mode input_mode;
  Usz bpm;
  // One tick at bpm, in microseconds, set with bpm; under External
  // it is the transport machine's estimate while that is valid, and bpm's
  // otherwise, so glide lengths follow the master's tempo.
  U64 tick_len;
  // The internal deadline and the beat clock (midi_in.h): the
  // deadline's clock and carry, the sixths counter and the stored Send MIDI
  // Beat Clock setting.
  Clock_out clock_out;
  Oosc_dev *oosc_dev;
  Midi_mode midi_mode;
  Usz activity_counter;
  Usz random_seed;
  Usz drag_start_y, drag_start_x;
  int win_h, win_w;
  int softmargin_y, softmargin_x;
  int grid_h;
  int grid_scroll_y, grid_scroll_x; // not sure if i like this being int
  bool needs_remarking : 1;
  bool is_draw_dirty : 1;
  bool is_playing : 1;
  bool draw_event_list : 1;
  bool is_mouse_down : 1;
  bool is_mouse_dragging : 1;
  bool is_hud_visible : 1;
} Ged;

// Every write of the tempo goes through here, so tick_len always matches it.
// The tick body hands it to the CC engine's glide submits (tick.h). Under
// External nothing writes the tempo (Set BPM is blocked, < and > are
// ignored) and tick_len follows the estimate instead; every return to
// Internal comes through here, adopting the estimate's shown value when one
// is valid, so the invariant holds again.
static void ged_set_bpm(Ged *a, Usz bpm) {
  a->bpm = bpm;
  a->tick_len = tick_len_us(bpm);
}

// Every change of the operator dialect goes through here, from
// the menu, orca.conf or --dialect, playing or not. The per-cell state
// belongs to the old list's operators, so the store is cleared, as Ctrl+R
// clears it; running CC glides stop and keep the last values sent; and the
// grid is re-marked under the new list. Returns false, changing nothing,
// when d is already the dialect.
static bool ged_set_dialect(Ged *a, Orca_dialect d) {
  if (d == a->dialect)
    return false;
  a->dialect = d;
  opstate_clear(&a->opstate);
  ccout_cancel(&a->ccout);
  a->needs_remarking = true;
  a->is_draw_dirty = true;
  return true;
}

static void ged_init(Ged *a, Usz undo_limit, Usz init_bpm, Usz init_seed) {
  field_init(&a->field);
  field_init(&a->scratch_field);
  opstate_init(&a->opstate);
  opstate_init(&a->scratch_opstate);
  // tui_apply_overrides overwrites this from the prefs; the default that
  // counts is tui_prefs_init's.
  a->dialect = Orca_dialect_borca;
  field_init(&a->clipboard_field);
  mbuf_reusable_init(&a->mbuf_r);
  a->labels = NULL;
  a->labels_capacity = 0;
  a->labels_h = 0;
  a->labels_w = 0;
  undo_history_init(&a->undo_hist, undo_limit);
  oevent_list_init(&a->oevent_list);
  oevent_list_init(&a->scratch_oevent_list);
  oevent_list_init(&a->engine_oevent_list);
  // No clock here: ged_init runs before stm_setup().
  ccout_init(&a->ccout);
  // glide_hz, until orca.conf's glide_hz key or the Glide Rate menu sets it.
  // tui_apply_overrides overwrites it from the prefs; the default that
  // counts is tui_prefs_init's.
  ccout_configure(&a->ccout, Ccout_glide_hz_default);
  susnote_list_init(&a->susnote_list);
  ged_cursor_init(&a->ged_cursor);
  a->clock_source = Clock_source_internal;
  transport_reset(&a->transport);
  a->tick_num = 0;
  a->ruler_spacing_y = a->ruler_spacing_x = 8;
  a->input_mode = Ged_input_mode_normal;
  ged_set_bpm(a, init_bpm);
  a->clock_out.clock = 0;
  a->clock_out.accum_secs = 0.0;
  a->clock_out.sixths = 0;
  // tui_apply_overrides overwrites this from the prefs; the default that
  // counts is tui_prefs_init's.
  a->clock_out.enabled = false;
  a->oosc_dev = NULL;
  midi_mode_init_null(&a->midi_mode);
  a->activity_counter = 0;
  a->random_seed = init_seed;
  a->drag_start_y = a->drag_start_x = 0;
  a->win_h = a->win_w = 0;
  a->softmargin_y = a->softmargin_x = 0;
  a->grid_h = 0;
  a->grid_scroll_y = a->grid_scroll_x = 0;
  a->needs_remarking = true;
  a->is_draw_dirty = false;
  a->is_playing = false;
  a->draw_event_list = false;
  a->is_mouse_down = false;
  a->is_mouse_dragging = false;
  a->is_hud_visible = false;
}

static void ged_deinit(Ged *a) {
  field_deinit(&a->field);
  field_deinit(&a->scratch_field);
  opstate_free(&a->opstate);
  opstate_free(&a->scratch_opstate);
  field_deinit(&a->clipboard_field);
  mbuf_reusable_deinit(&a->mbuf_r);
  free(a->labels);
  undo_history_deinit(&a->undo_hist);
  oevent_list_deinit(&a->oevent_list);
  oevent_list_deinit(&a->scratch_oevent_list);
  oevent_list_deinit(&a->engine_oevent_list);
  susnote_list_deinit(&a->susnote_list);
  if (a->oosc_dev)
    oosc_dev_destroy(a->oosc_dev);
  midi_mode_deinit(&a->midi_mode);
}

static bool ged_is_draw_dirty(Ged *a) {
  return a->is_draw_dirty || a->needs_remarking;
}

#ifdef FEAT_ALSA
// What the ALSA Midi_write needs: the client and the Ged's encoder.
typedef struct {
  Alsa_ctx *ctx;
  snd_midi_event_t *encoder;
} Alsa_write;

// One three-byte message to the subscribers (the Midi_write of midi_in.h):
// >= 0 when it went out, -errno when the sequencer refused it. It counts
// nothing and sends nothing again; held_send keeps a refused note-off and the
// main loop sends it again. No subscribers is normal; direct nonblocking
// output survives hotplug. A full output (-EAGAIN) or an interrupted write
// (-EINTR) refuses the message, with no sleep, like any other failure; the HUD
// shows it. Nothing prints here: curses owns the terminal.
static int alsa_write_message(void *u, int status, int d1, int d2) {
  Alsa_write *aw = u;
  unsigned char bytes[3] = {(unsigned char)status, (unsigned char)d1,
                            (unsigned char)d2};
  long length = status >= 0xf8 ? 1 : ((status & 0xe0) == 0xc0 ? 2 : 3);
  snd_seq_event_t event;
  snd_seq_ev_clear(&event);
  snd_midi_event_reset_encode(aw->encoder);
  if (snd_midi_event_encode(aw->encoder, bytes, length, &event) <= 0 ||
      event.type == SND_SEQ_EVENT_NONE)
    return -EINVAL; // the encoder failed or made no event: dropped too
  snd_seq_ev_set_source(&event, (unsigned char)aw->ctx->port);
  snd_seq_ev_set_subs(&event);
  snd_seq_ev_set_direct(&event);
  return snd_seq_event_output_direct(aw->ctx->seq, &event);
}

static U64 alsa_write_now(void *u) {
  (void)u;
  return now_us();
}
#endif

staticni void send_midi_3bytes(Oosc_dev *oosc_dev, Midi_mode const *midi_mode,
                               int status, int byte1, int byte2) {
  switch (midi_mode->any.type) {
#ifdef FEAT_ALSA
  case Midi_mode_type_alsa: {
    Alsa_ctx *ctx = midi_mode->alsa.ctx;
    Alsa_write aw = {ctx, midi_mode->alsa.encoder};
    Midi_write w = {&aw, alsa_write_message, alsa_write_now};
    // A refused note-off would leave the note hanging on the synth, so
    // held_send keeps it (midi_in.h); a refused note-on is a missed note.
    int err = held_send(&ctx->pending_offs, &w, status, byte1, byte2);
    if (err < 0)
      alsa_ctx_count_send_failure(ctx, err);
    break;
  }
#endif
  case Midi_mode_type_null:
    break;
  case Midi_mode_type_osc_bidule: {
    if (!oosc_dev)
      break;
    oosc_send_int32s(oosc_dev, midi_mode->osc_bidule.path,
                     (int[]){status, byte1, byte2}, 3);
    break;
  }
#ifdef FEAT_PORTMIDI
  case Midi_mode_type_portmidi: {
    // timestamp is totally fake, to prevent problems with some MIDI systems
    // getting angry if there's no timestamping info.
    //
    // Eventually, we will want to create real timestamps based on a real orca
    // clock, instead of ad-hoc at the last moment like this. When we do that,
    // we'll need to thread the timestamping/timing info through the function
    // calls, instead of creating it at the last moment here. (This timestamp
    // is actually 'useless', because it doesn't convey any additional
    // information. But if we don't provide it, at least to PortMidi, some
    // people's MIDI setups may malfunction and have terrible timing problems.)
    PmTimestamp pm_timestamp = portmidi_timestamp_now();
    PmError pme = Pm_WriteShort(midi_mode->portmidi.stream, pm_timestamp,
                                Pm_Message(status, byte1, byte2));
    (void)pme;
    break;
  }
#endif
  }
}

#ifdef FEAT_ALSA
// The ALSA client, while ALSA is the MIDI output, else NULL.
static Alsa_ctx *ged_alsa_ctx(Ged *a) {
  return a->midi_mode.any.type == Midi_mode_type_alsa ? a->midi_mode.alsa.ctx
                                                      : NULL;
}

// True while a refused note-off is held; then *wait_us is how long until the
// next try is due (0: now), so the main loop wakes for it instead of waiting
// out its idle ceiling. The clock is read only while one is held.
static bool ged_note_off_wait_us(Ged *a, U64 *wait_us) {
  Alsa_ctx *ctx = ged_alsa_ctx(a);
  return ctx && ctx->pending_offs.count != 0 &&
         pending_offs_wait_us(&ctx->pending_offs, now_us(), wait_us);
}

// One pass over the held note-offs, from the main loop after the tick: sent
// again oldest first, when their time is due, at most 16 a pass, until one is
// refused again or its 3 s are up (midi_in.h). Nothing is held in the usual
// case, and then this is a load and a compare.
static void ged_retry_note_offs(Ged *a) {
  Alsa_ctx *ctx = ged_alsa_ctx(a);
  if (!ctx || ctx->pending_offs.count == 0)
    return;
  Alsa_write aw = {ctx, a->midi_mode.alsa.encoder};
  Midi_write w = {&aw, alsa_write_message, alsa_write_now};
  (void)pending_offs_retry(&ctx->pending_offs, now_us(), &w);
}

// At quit, after the stop's note-offs: the last chance for the held ones,
// whatever the schedule said. Tries for at most limit_us, 1 ms apart. The
// engine is stopped by now, so the wait costs no timing; poll() is the sleep.
static void ged_flush_note_offs(Ged *a, U64 limit_us) {
  Alsa_ctx *ctx = ged_alsa_ctx(a);
  if (!ctx)
    return;
  Alsa_write aw = {ctx, a->midi_mode.alsa.encoder};
  Midi_write w = {&aw, alsa_write_message, alsa_write_now};
  U64 start = now_us();
  ctx->pending_offs.next_us = 0; // the first try at once, whatever was due
  while (ctx->pending_offs.count != 0) {
    U64 now = now_us();
    if (pending_offs_retry(&ctx->pending_offs, now, &w) == 0 ||
        now - start >= limit_us)
      break;
    // Then on the list's own schedule, not every millisecond: a subscriber
    // that is still full repeats each try to the healthy ones too.
    U64 later = now_us();
    U64 wait_us = 0;
    (void)pending_offs_wait_us(&ctx->pending_offs, later, &wait_us);
    U64 left_us = later - start >= limit_us ? 0 : limit_us - (later - start);
    if (wait_us > left_us)
      wait_us = left_us;
    (void)poll(NULL, 0, (int)(wait_us / 1000 + 1)); // rounded up, at least 1 ms
  }
}
#endif

// The send sink over the backend dispatch above. tick.c sends every
// event through it, and midi_in.h's clock-out inlines send the shell's F8,
// FA, FB and FC through it.
// Each call reads the OSC device and the MIDI mode as they are at that
// moment: both change at runtime (the PortMidi output menu, the OSC form,
// ged_clear_osc_udp).
static void ged_sink_midi3(void *u, int status, int d1, int d2) {
  Ged *a = u;
  send_midi_3bytes(a->oosc_dev, &a->midi_mode, status, d1, d2);
}

static void ged_sink_midi1(void *u, int byte) {
  Ged *a = u;
  // PortMidi wants 0 and 0 for the unused bytes. Likewise, Bidule's
  // MIDI-via-OSC won't accept the message unless there are at least all 3
  // bytes, with the second 2 set to zero.
  send_midi_3bytes(a->oosc_dev, &a->midi_mode, byte, 0, 0);
}

static void ged_sink_osc(void *u, Oevent const *e) {
  Ged *a = u;
  switch ((Oevent_types)e->any.oevent_type) {
  case Oevent_type_osc_ints: {
    // kinda lame
    if (!a->oosc_dev)
      break;
    Oevent_osc_ints const *eo = &e->osc_ints;
    char path[] = {'/', eo->glyph, '\0'};
    I32 ints[ORCA_ARRAY_COUNTOF(eo->numbers)];
    Usz nnum = eo->count;
    for (Usz inum = 0; inum < nnum; ++inum) {
      ints[inum] = eo->numbers[inum];
    }
    oosc_send_int32s(a->oosc_dev, path, ints, nnum);
    break;
  }
  case Oevent_type_udp_string: {
    // The upstream dialect's ; (sim.c udp): the glyphs as one datagram, to
    // the OSC output's address and port.
    if (!a->oosc_dev)
      break;
    Oevent_udp_string const *eu = &e->udp_string;
    oosc_send_datagram(a->oosc_dev, eu->chars, eu->count);
    break;
  }
  case Oevent_type_midi_note:
  case Oevent_type_midi_cc:
  case Oevent_type_midi_cc_interpolated:
  case Oevent_type_midi_pb:
    break; // never passed here: tick.c sends these as MIDI
  }
}

static void send_control_message(Oosc_dev *oosc_dev, char const *osc_address) {
  if (!oosc_dev)
    return;
  oosc_send_int32s(oosc_dev, osc_address, NULL, 0);
}

// The sink's control callback: an OSC message with no arguments, such
// as /orca/stopped, when OSC output is set.
static void ged_sink_control(void *u, char const *osc_address) {
  Ged *a = u;
  send_control_message(a->oosc_dev, osc_address);
}

static Tick_sink ged_sink(Ged *a) {
  return (Tick_sink){.u = a,
                     .midi3 = ged_sink_midi3,
                     .midi1 = ged_sink_midi1,
                     .osc = ged_sink_osc,
                     .control = ged_sink_control};
}

// The sub-tick poll: the wait point runs it between
// ticks whenever the CC engine's deadline has passed, so glides step on
// wall-clock time at glide_hz. now is a now_us() read made after the tick's.
static void ged_poll_ccout(Ged *a, U64 now) {
  Tick_sink const sink = ged_sink(a);
  tick_poll_engine(&a->ccout, now, &a->engine_oevent_list, &sink);
}

static void send_num_message(Oosc_dev *oosc_dev, char const *osc_address,
                             I32 num) {
  if (!oosc_dev)
    return;
  I32 nums[1];
  nums[0] = num;
  oosc_send_int32s(oosc_dev, osc_address, nums, ORCA_ARRAY_COUNTOF(nums));
}

staticni void ged_stop_all_sustained_notes(Ged *a) {
  Tick_sink const sink = ged_sink(a);
  tick_release_all(&sink, &a->susnote_list);
}

staticni void ged_clear_osc_udp(Ged *a) {
  if (a->oosc_dev) {
    if (a->midi_mode.any.type == Midi_mode_type_osc_bidule) {
      ged_stop_all_sustained_notes(a);
    }
    oosc_dev_destroy(a->oosc_dev);
    a->oosc_dev = NULL;
  }
}
static bool ged_is_using_osc_udp(Ged *a) { return (bool)a->oosc_dev; }
static bool ged_set_osc_udp(Ged *a, char const *dest_addr,
                            char const *dest_port) {
  ged_clear_osc_udp(a);
  if (dest_port) {
    Oosc_udp_create_error err =
        oosc_dev_create_udp(&a->oosc_dev, dest_addr, dest_port);
    if (err) {
      return false;
    }
  }
  return true;
}

static ORCA_FORCEINLINE double ms_to_sec(double ms) { return ms / 1000.0; }

// Seconds to the next tick or beat-clock deadline, and no other: the tick
// body sends its note-offs itself, so they need no deadline of their own. A
// refused note-off that is held for a retry has one, which the wait point
// adds on its own (ged_note_off_wait_us), as it does for the CC engine's.
// Under External, as when stopped, there is none: the transport machine runs
// the ticks, so the internal tick deadline drops out of the wait.
static double ged_secs_to_deadline(Ged const *a) {
  if (!a->is_playing || a->clock_source == Clock_source_external)
    return 1.0;
  // If MIDI beat clock output is enabled, we need to send an event every 24
  // parts per quarter note. Since we've already divided quarter notes into 4
  // for ORCA's timing semantics, divide it by a further 6 (clock_out_span).
  // ged_do_stuff() uses the same span.
  double secs_span = clock_out_span(&a->clock_out, a->clock_source, a->bpm);
  double rem = secs_span - (stm_sec(stm_since(a->clock_out.clock)) +
                            a->clock_out.accum_secs);
  if (rem < 0.0)
    rem = 0.0;
  return rem;
}

// The label buffer for a VM run on a height x width grid: grown when the
// grid outgrew it, amortized like mbuf_r, with no allocation otherwise, and
// its size remembered for draw_hud. If growth fails the old allocation is
// kept (the realloc goes into a temporary), the remembered size becomes
// 0 x 0 and the run gets NULL, so nothing is recorded and no tooltip shows
// until a later run succeeds.
static Port_label_cell *ged_labels_for_run(Ged *a, Usz height, Usz width) {
  Usz count = height * width;
  if (count > a->labels_capacity) {
    Port_label_cell *grown = realloc(a->labels, count * sizeof *grown);
    if (!grown) {
      a->labels_h = 0;
      a->labels_w = 0;
      return NULL;
    }
    a->labels = grown;
    a->labels_capacity = count;
  }
  a->labels_h = height;
  a->labels_w = width;
  return a->labels;
}

// One tick body, for both callers: the internal deadline in
// ged_do_stuff and the transport machine's tick due under External. The tick
// body (tick.c) sends everything for the tick: note-offs that are due, the
// CC engine's glide steps, then the VM's events, each CC through the engine.
// The clock is read once, here, after the caller's sixths gate or the
// event's stamp, never from the stamp itself: the engine's poll and every
// submit in the body share this time.
staticni void ged_run_tick_body(Ged *a) {
  Tick_sink const sink = ged_sink(a);
  Tick_ctx const ctx = {.gbuffer = a->field.buffer,
                        .mbuffer = a->mbuf_r.buffer,
                        .height = a->field.height,
                        .width = a->field.width,
                        .random_seed = a->random_seed,
                        .opstate = &a->opstate,
                        .dialect = a->dialect,
                        .labels = ged_labels_for_run(a, a->field.height,
                                                     a->field.width),
                        .tick_num = &a->tick_num,
                        .tick_list = &a->oevent_list,
                        .engine_list = &a->engine_oevent_list,
                        .susnotes = &a->susnote_list,
                        .ccout = &a->ccout,
                        .now_us = now_us(),
                        .tick_len = a->tick_len};
  tick_body(&ctx, &sink);
  a->needs_remarking = true;
  a->is_draw_dirty = true;
  // Engine events do not count as activity.
  a->activity_counter += a->oevent_list.count;
}

staticni void ged_do_stuff(Ged *a) {
  if (!a->is_playing || a->clock_source == Clock_source_external)
    return;
  // The span to the next deadline: one tick, or a sixth of a tick under beat
  // clock. It only times the deadlines: sustained notes age by one tick per
  // tick body (tick.c), so they keep their length under beat clock.
  double secs_span = clock_out_span(&a->clock_out, a->clock_source, a->bpm);
  bool crossed_deadline = false;
#if TIME_DEBUG
  Usz spins = 0;
  U64 spin_start = stm_now();
  (void)spin_start;
#endif
  for (;;) {
    U64 now = stm_now();
    U64 diff = stm_diff(now, a->clock_out.clock);
    double sdiff = stm_sec(diff) + a->clock_out.accum_secs;
    if (sdiff >= secs_span) {
      a->clock_out.clock = now;
      a->clock_out.accum_secs = sdiff - secs_span;
#if TIME_DEBUG
      if (a->clock_out.accum_secs > 0.000001) {
        fprintf(stderr, "late: %.2f u-secs\n",
                a->clock_out.accum_secs * 1000 * 1000);
        if (a->clock_out.accum_secs > 0.00005) {
          fprintf(stderr, "guilty timeout: %d\n", spin_track_timeout);
        }
      }
#endif
      crossed_deadline = true;
      break;
    }
    if (secs_span - sdiff > ms_to_sec(0.1))
      break;
#if TIME_DEBUG
    ++spins;
#endif
  }
#if TIME_DEBUG
  if (spins > 0) {
    fprintf(stderr, "%d spins in %f us with timeout %d\n", (int)spins,
            stm_us(stm_since(spin_start)), spin_track_timeout);
  }
#endif
  if (!crossed_deadline)
    return;
  // The F8 gate: with clock-out active, an F8 at every sixth of a tick and
  // the tick body on every sixth one (midi_in.h).
  Tick_sink const sink = ged_sink(a);
  if (!clock_out_on_deadline(&a->clock_out, a->clock_source, &sink))
    return;
  ged_run_tick_body(a);
}

static inline Isz isz_clamp(Isz x, Isz low, Isz high) {
  return x < low ? low : x > high ? high : x;
}

// todo cleanup to use proper unsigned/signed w/ overflow check
staticni Isz scroll_offset_on_axis_for_cursor_pos(Isz win_len, Isz cont_len,
                                                  Isz cursor_pos, Isz pad,
                                                  Isz cur_scroll) {
  if (win_len <= 0 || cont_len <= 0)
    return 0;
  if (cont_len <= win_len)
    return -((win_len - cont_len) / 2);
  if (pad * 2 >= win_len) {
    pad = (win_len - 1) / 2;
  }
  Isz min_vis_scroll = cursor_pos - win_len + 1 + pad;
  Isz max_vis_scroll = cursor_pos - pad;
  Isz new_scroll;
  if (cur_scroll < min_vis_scroll)
    new_scroll = min_vis_scroll;
  else if (cur_scroll > max_vis_scroll)
    new_scroll = max_vis_scroll;
  else
    new_scroll = cur_scroll;
  return isz_clamp(new_scroll, 0, cont_len - win_len);
}

staticni void ged_make_cursor_visible(Ged *a) {
  int grid_h = a->grid_h;
  int cur_scr_y = a->grid_scroll_y;
  int cur_scr_x = a->grid_scroll_x;
  int new_scr_y = (int)scroll_offset_on_axis_for_cursor_pos(
      grid_h, (Isz)a->field.height, (Isz)a->ged_cursor.y, 5, cur_scr_y);
  int new_scr_x = (int)scroll_offset_on_axis_for_cursor_pos(
      a->win_w, (Isz)a->field.width, (Isz)a->ged_cursor.x, 5, cur_scr_x);
  if (new_scr_y == cur_scr_y && new_scr_x == cur_scr_x)
    return;
  a->grid_scroll_y = new_scr_y;
  a->grid_scroll_x = new_scr_x;
  a->is_draw_dirty = true;
}

enum { Hud_height = 2 };

staticni void ged_update_internal_geometry(Ged *a) {
  int win_h = a->win_h;
  int softmargin_y = a->softmargin_y;
  bool show_hud = win_h > Hud_height + 1;
  int grid_h = show_hud ? win_h - Hud_height : win_h;
  if (grid_h > a->field.height) {
    int halfy = (grid_h - a->field.height + 1) / 2;
    grid_h -= halfy < softmargin_y ? halfy : softmargin_y;
  }
  a->grid_h = grid_h;
  a->is_draw_dirty = true;
  a->is_hud_visible = show_hud;
}

staticni void ged_set_window_size(Ged *a, int win_h, int win_w,
                                  int softmargin_y, int softmargin_x) {
  if (a->win_h == win_h && a->win_w == win_w &&
      a->softmargin_y == softmargin_y && a->softmargin_x == softmargin_x)
    return;
  a->win_h = win_h;
  a->win_w = win_w;
  a->softmargin_y = softmargin_y;
  a->softmargin_x = softmargin_x;
  ged_update_internal_geometry(a);
  ged_make_cursor_visible(a);
}

staticni void ged_draw(Ged *a, WINDOW *win, char const *filename,
                       bool use_fancy_dots, bool use_fancy_rulers) {
  // We can predictavely step the next simulation tick and then use the
  // resulting mark buffer for better UI visualization. If we don't do this,
  // after loading a fresh file or after the user performs some edit (or even
  // after a regular simulation step), the new glyph buffer won't have had
  // phase 0 of the simulation run, which means the ports and other flags won't
  // be set on the mark buffer, so the colors for disabled cells, ports, etc.
  // won't be set.
  //
  // We can just perform a simulation step using the current state, keep the
  // mark buffer that it produces, then roll back the glyph buffer to where it
  // was before. This should produce results similar to having specialized UI
  // code that looks at each glyph and figures out the ports, etc.
  //
  // If scratch_field or the mark buffer cannot grow to the grid's size, the
  // pass is skipped and needs_remarking stays set, so the next draw retries.
  if (a->needs_remarking && !a->is_playing &&
      field_copy(&a->field, &a->scratch_field) &&
      mbuf_reusable_ensure_size(&a->mbuf_r, a->field.height, a->field.width)) {
    // The pass advances operator state, so it runs on a throwaway copy of
    // the store, as it does on scratch_field. If the copy cannot
    // allocate, it runs on an empty store; the live one is untouched.
    (void)opstate_copy(&a->opstate, &a->scratch_opstate);
    // Its events go to the preview list, which is never sent, and its labels
    // to the live label buffer, so the tooltips follow the marks.
    Orca_run_ctx const preview_ctx = {
        .opstate = &a->scratch_opstate,
        .dialect = a->dialect,
        .labels = ged_labels_for_run(a, a->field.height, a->field.width)};
    tick_run_vm(a->scratch_field.buffer, a->mbuf_r.buffer, a->field.height,
                a->field.width, a->tick_num, &a->scratch_oevent_list,
                a->random_seed, &preview_ctx);
    a->needs_remarking = false;
  }
  int win_w = a->win_w;
  draw_glyphs_grid_scrolled(
      win, 0, 0, a->grid_h, win_w, a->field.buffer, a->mbuf_r.buffer,
      a->field.height, a->field.width, a->grid_scroll_y, a->grid_scroll_x,
      a->ruler_spacing_y, a->ruler_spacing_x, use_fancy_dots, use_fancy_rulers,
      a->dialect);
  draw_grid_cursor(win, 0, 0, a->grid_h, win_w, a->field.buffer,
                   a->field.height, a->field.width, a->grid_scroll_y,
                   a->grid_scroll_x, a->ged_cursor.y, a->ged_cursor.x,
                   a->ged_cursor.h, a->ged_cursor.w, a->input_mode,
                   a->is_playing);
  if (a->is_hud_visible) {
    filename = filename ? filename : "unnamed";
    int hud_x = win_w > 50 + a->softmargin_x * 2 ? a->softmargin_x : 0;
    // Under External the BPM field shows the clock's state: ext lost, ext
    // <shown> or ext --.
    char ext_text[9];
    char const *bpm_text = NULL;
    if (a->clock_source == Clock_source_external) {
      ext_bpm_text(a->transport.lost, a->transport.est_valid,
                   a->transport.shown, ext_text);
      bpm_text = ext_text;
    }
    draw_hud(win, a->grid_h, hud_x, Hud_height, win_w, filename,
             a->field.height, a->field.width, a->ruler_spacing_y,
             a->ruler_spacing_x, a->tick_num, a->bpm, bpm_text, &a->ged_cursor,
             a->input_mode, a->activity_counter, a->field.buffer, a->labels,
             a->labels_h, a->labels_w, midi_mode_send_failed(&a->midi_mode));
  }
  if (a->draw_event_list)
    draw_oevent_list(win, &a->oevent_list);
  a->is_draw_dirty = false;
}

staticni void ged_send_osc_bpm(Ged *a, I32 bpm) {
  send_num_message(a->oosc_dev, "/orca/bpm", bpm);
}

staticni void ged_adjust_bpm(Ged *a, Isz delta_bpm) {
  Isz new_bpm = (Isz)a->bpm;
  if (delta_bpm < 0 || new_bpm < INT_MAX - delta_bpm)
    new_bpm += delta_bpm;
  else
    new_bpm = INT_MAX;
  if (new_bpm < 1)
    new_bpm = 1;
  if ((Usz)new_bpm != a->bpm) {
    ged_set_bpm(a, (Usz)new_bpm);
    a->is_draw_dirty = true;
    ged_send_osc_bpm(a, (I32)new_bpm);
  }
}

static void ged_move_cursor_relative(Ged *a, Isz delta_y, Isz delta_x) {
  ged_cursor_move_relative(&a->ged_cursor, a->field.height, a->field.width,
                           delta_y, delta_x);
  ged_make_cursor_visible(a);
  a->is_draw_dirty = true;
}

static Usz guarded_selection_axis_resize(Usz x, int delta) {
  if (delta < 0) {
    if (delta > INT_MIN && (Usz)(-delta) < x) {
      x -= (Usz)(-delta);
    }
  } else if (x < SIZE_MAX - (Usz)delta) {
    x += (Usz)delta;
  }
  return x;
}

staticni void ged_modify_selection_size(Ged *a, int delta_y, int delta_x) {
  Usz cur_h = a->ged_cursor.h, cur_w = a->ged_cursor.w;
  Usz new_h = guarded_selection_axis_resize(cur_h, delta_y);
  Usz new_w = guarded_selection_axis_resize(cur_w, delta_x);
  if (cur_h != new_h || cur_w != new_w) {
    a->ged_cursor.h = new_h;
    a->ged_cursor.w = new_w;
    a->is_draw_dirty = true;
  }
}

staticni bool ged_try_selection_clipped_to_field(Ged const *a, Usz *out_y,
                                                 Usz *out_x, Usz *out_h,
                                                 Usz *out_w) {
  Usz curs_y = a->ged_cursor.y, curs_x = a->ged_cursor.x;
  Usz curs_h = a->ged_cursor.h, curs_w = a->ged_cursor.w;
  Usz field_h = a->field.height, field_w = a->field.width;
  if (curs_y >= field_h || curs_x >= field_w)
    return false;
  if (field_h - curs_y < curs_h)
    curs_h = field_h - curs_y;
  if (field_w - curs_x < curs_w)
    curs_w = field_w - curs_x;
  *out_y = curs_y;
  *out_x = curs_x;
  *out_h = curs_h;
  *out_w = curs_w;
  return true;
}

staticni bool ged_slide_selection(Ged *a, int delta_y, int delta_x) {
  Usz curs_y_0, curs_x_0, curs_h_0, curs_w_0;
  Usz curs_y_1, curs_x_1, curs_h_1, curs_w_1;
  if (!ged_try_selection_clipped_to_field(a, &curs_y_0, &curs_x_0, &curs_h_0,
                                          &curs_w_0))
    return false;
  ged_move_cursor_relative(a, delta_y, delta_x);
  if (!ged_try_selection_clipped_to_field(a, &curs_y_1, &curs_x_1, &curs_h_1,
                                          &curs_w_1))
    return false;
  // Don't create a history entry if nothing is going to happen.
  if (curs_y_0 == curs_y_1 && curs_x_0 == curs_x_1 && curs_h_0 == curs_h_1 &&
      curs_w_0 == curs_w_1)
    return false;
  undo_history_push(&a->undo_hist, &a->field, a->tick_num);
  Usz field_h = a->field.height;
  Usz field_w = a->field.width;
  gbuffer_copy_subrect(a->field.buffer, a->field.buffer, field_h, field_w,
                       field_h, field_w, curs_y_0, curs_x_0, curs_y_1, curs_x_1,
                       curs_h_0, curs_w_0);
  // Erase/clear the area that was within the selection rectangle in the
  // starting position, but wasn't written to during the copy. (In other words,
  // this is the area that was 'left behind' when we moved the selection
  // rectangle, plus any area that was along the bottom and right edge of the
  // field that didn't have anything to copy to it when the selection rectangle
  // extended outside of the field.)
  Usz ey, eh, ex, ew;
  if (curs_y_1 > curs_y_0) {
    ey = curs_y_0;
    eh = curs_y_1 - curs_y_0;
  } else {
    ey = curs_y_1 + curs_h_0;
    eh = (curs_y_0 + curs_h_0) - ey;
  }
  if (curs_x_1 > curs_x_0) {
    ex = curs_x_0;
    ew = curs_x_1 - curs_x_0;
  } else {
    ex = curs_x_1 + curs_w_0;
    ew = (curs_x_0 + curs_w_0) - ex;
  }
  gbuffer_fill_subrect(a->field.buffer, field_h, field_w, ey, curs_x_0, eh,
                       curs_w_0, '.');
  gbuffer_fill_subrect(a->field.buffer, field_h, field_w, curs_y_0, ex,
                       curs_h_0, ew, '.');
  a->needs_remarking = true;
  return true;
}

typedef enum {
  Ged_dir_up,
  Ged_dir_down,
  Ged_dir_left,
  Ged_dir_right,
} Ged_dir;

staticni void ged_dir_input(Ged *a, Ged_dir dir, int step_length) {
  switch (a->input_mode) {
  case Ged_input_mode_normal:
  case Ged_input_mode_append:
    switch (dir) {
    case Ged_dir_up:
      ged_move_cursor_relative(a, -step_length, 0);
      break;
    case Ged_dir_down:
      ged_move_cursor_relative(a, step_length, 0);
      break;
    case Ged_dir_left:
      ged_move_cursor_relative(a, 0, -step_length);
      break;
    case Ged_dir_right:
      ged_move_cursor_relative(a, 0, step_length);
      break;
    }
    break;
  case Ged_input_mode_selresize:
    switch (dir) {
    case Ged_dir_up:
      ged_modify_selection_size(a, -step_length, 0);
      break;
    case Ged_dir_down:
      ged_modify_selection_size(a, step_length, 0);
      break;
    case Ged_dir_left:
      ged_modify_selection_size(a, 0, -step_length);
      break;
    case Ged_dir_right:
      ged_modify_selection_size(a, 0, step_length);
      break;
    }
    break;
  case Ged_input_mode_slide:
    switch (dir) {
    case Ged_dir_up:
      ged_slide_selection(a, -step_length, 0);
      break;
    case Ged_dir_down:
      ged_slide_selection(a, step_length, 0);
      break;
    case Ged_dir_left:
      ged_slide_selection(a, 0, -step_length);
      break;
    case Ged_dir_right:
      ged_slide_selection(a, 0, step_length);
      break;
    }
    break;
  }
}

static Usz view_to_scrolled_grid(Usz field_len, Usz visual_coord,
                                 int scroll_offset) {
  if (field_len == 0)
    return 0;
  if (scroll_offset < 0) {
    if ((Usz)(-scroll_offset) <= visual_coord) {
      visual_coord -= (Usz)(-scroll_offset);
    } else {
      visual_coord = 0;
    }
  } else {
    visual_coord += (Usz)scroll_offset;
  }
  if (visual_coord >= field_len)
    visual_coord = field_len - 1;
  return visual_coord;
}

ORCA_OK_IF_UNUSED staticni void ged_mouse_event(Ged *a, Usz vis_y, Usz vis_x,
                                                mmask_t mouse_bstate) {
  if (mouse_bstate & BUTTON1_RELEASED) {
    // hard-disables tracking, but also disables further mouse stuff.
    // mousemask() with our original parameters seems to work to get into the
    // state we want, though.
    //
    // printf("\033[?1003l\n");
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    a->is_mouse_down = false;
    a->is_mouse_dragging = false;
    a->drag_start_y = 0;
    a->drag_start_x = 0;
  } else if ((mouse_bstate & BUTTON1_PRESSED) || a->is_mouse_down) {
    Usz y = view_to_scrolled_grid(a->field.height, vis_y, a->grid_scroll_y);
    Usz x = view_to_scrolled_grid(a->field.width, vis_x, a->grid_scroll_x);
    if (!a->is_mouse_down) {
      // some sequence to hopefully make terminal start reporting all further
      // mouse movement events. 'REPORT_MOUSE_POSITION' alone in the mousemask
      // doesn't seem to work, at least not for xterm. we need to set it only
      // only when needed, otherwise some terminals will send movement updates
      // when we don't want them.
      printf("\033[?1003h\n");
      // need to do this or double clicking can cause terminal state to get
      // corrupted, since we're bypassing curses here. might cause flicker.
      // wish i could figure out why report mouse position isn't working on its
      // own.
      fflush(stdout);
      wclear(stdscr);
      a->is_mouse_down = true;
      a->ged_cursor.y = y;
      a->ged_cursor.x = x;
      a->ged_cursor.h = 1;
      a->ged_cursor.w = 1;
      a->is_draw_dirty = true;
    } else {
      if (!a->is_mouse_dragging &&
          (y != a->ged_cursor.y || x != a->ged_cursor.x)) {
        a->is_mouse_dragging = true;
        a->drag_start_y = a->ged_cursor.y;
        a->drag_start_x = a->ged_cursor.x;
      }
      if (a->is_mouse_dragging) {
        Usz tcy = a->drag_start_y;
        Usz tcx = a->drag_start_x;
        Usz loy = y < tcy ? y : tcy;
        Usz lox = x < tcx ? x : tcx;
        Usz hiy = y > tcy ? y : tcy;
        Usz hix = x > tcx ? x : tcx;
        a->ged_cursor.y = loy;
        a->ged_cursor.x = lox;
        a->ged_cursor.h = hiy - loy + 1;
        a->ged_cursor.w = hix - lox + 1;
        a->is_draw_dirty = true;
      }
    }
  }
#if defined(NCURSES_MOUSE_VERSION) && NCURSES_MOUSE_VERSION >= 2
  else {
    if (mouse_bstate & BUTTON4_PRESSED) {
      a->grid_scroll_y -= 1;
      a->is_draw_dirty = true;
    } else if (mouse_bstate & BUTTON5_PRESSED) {
      a->grid_scroll_y += 1;
      a->is_draw_dirty = true;
    }
  }
#endif
}

staticni void ged_adjust_rulers_relative(Ged *a, Isz delta_y, Isz delta_x) {
  Isz new_y = (Isz)a->ruler_spacing_y + delta_y;
  Isz new_x = (Isz)a->ruler_spacing_x + delta_x;
  if (new_y < 4)
    new_y = 4;
  else if (new_y > 16)
    new_y = 16;
  if (new_x < 4)
    new_x = 4;
  else if (new_x > 16)
    new_x = 16;
  if ((Usz)new_y == a->ruler_spacing_y && (Usz)new_x == a->ruler_spacing_x)
    return;
  a->ruler_spacing_y = (Usz)new_y;
  a->ruler_spacing_x = (Usz)new_x;
  a->is_draw_dirty = true;
}

staticni void ged_resize_grid_relative(Ged *a, Isz delta_y, Isz delta_x) {
  ged_resize_grid_snap_ruler(&a->field, &a->mbuf_r, a->ruler_spacing_y,
                             a->ruler_spacing_x, delta_y, delta_x, a->tick_num,
                             &a->scratch_field, &a->undo_hist, &a->ged_cursor,
                             &a->opstate);
  a->needs_remarking = true; // could check if we actually resized
  a->is_draw_dirty = true;
  ged_update_internal_geometry(a);
  ged_make_cursor_visible(a);
}

staticni void ged_write_character(Ged *a, char c) {
  undo_history_push(&a->undo_hist, &a->field, a->tick_num);
  gbuffer_poke(a->field.buffer, a->field.height, a->field.width,
               a->ged_cursor.y, a->ged_cursor.x, c);
  // Indicate we want the next simulation step to be run predictavely,
  // so that we can use the reulsting mark buffer for UI visualization.
  // This is "expensive", so it could be skipped for non-interactive
  // input in situations where max throughput is necessary.
  a->needs_remarking = true;
  if (a->input_mode == Ged_input_mode_append) {
    ged_cursor_move_relative(&a->ged_cursor, a->field.height, a->field.width, 0,
                             1);
  }
  a->is_draw_dirty = true;
}

staticni bool ged_fill_selection_with_char(Ged *a, Glyph c) {
  Usz curs_y, curs_x, curs_h, curs_w;
  if (!ged_try_selection_clipped_to_field(a, &curs_y, &curs_x, &curs_h,
                                          &curs_w))
    return false;
  gbuffer_fill_subrect(a->field.buffer, a->field.height, a->field.width, curs_y,
                       curs_x, curs_h, curs_w, c);
  return true;
}

staticni bool ged_copy_selection_to_clipbard(Ged *a) {
  Usz curs_y, curs_x, curs_h, curs_w;
  if (!ged_try_selection_clipped_to_field(a, &curs_y, &curs_x, &curs_h,
                                          &curs_w))
    return false;
  Usz field_h = a->field.height;
  Usz field_w = a->field.width;
  Field *cb_field = &a->clipboard_field;
  // A clipboard that cannot grow stays as it was, and a cut erases nothing.
  if (!field_resize_raw_if_necessary(cb_field, curs_h, curs_w))
    return false;
  gbuffer_copy_subrect(a->field.buffer, cb_field->buffer, field_h, field_w,
                       curs_h, curs_w, curs_y, curs_x, 0, 0, curs_h, curs_w);
  return true;
}

staticni void ged_input_character(Ged *a, char c) {
  switch (a->input_mode) {
  case Ged_input_mode_append:
    ged_write_character(a, c);
    break;
  case Ged_input_mode_normal:
  case Ged_input_mode_selresize:
  case Ged_input_mode_slide:
    if (a->ged_cursor.h <= 1 && a->ged_cursor.w <= 1) {
      ged_write_character(a, c);
    } else {
      undo_history_push(&a->undo_hist, &a->field, a->tick_num);
      ged_fill_selection_with_char(a, c);
      a->needs_remarking = true;
      a->is_draw_dirty = true;
    }
    break;
  }
}

typedef enum {
  Ged_input_cmd_undo,
  Ged_input_cmd_toggle_append_mode,
  Ged_input_cmd_toggle_selresize_mode,
  Ged_input_cmd_toggle_slide_mode,
  Ged_input_cmd_step_forward,
  Ged_input_cmd_toggle_show_event_list,
  Ged_input_cmd_toggle_play_pause,
  Ged_input_cmd_cut,
  Ged_input_cmd_copy,
  Ged_input_cmd_paste,
  Ged_input_cmd_escape,
} Ged_input_cmd;

// Play or pause on Internal, and the stop under External too (the transport
// machine's stop and clock lost, and the switch to External, come here with
// false): tick_stop sends the note-offs, cancels the glides, sends
// /orca/stopped, and FC only while clock-out is active.
// Starting under External never comes here; see ged_external_start.
staticni void ged_set_playing(Ged *a, bool playing) {
  if (playing == a->is_playing)
    return;
  if (playing) {
    undo_history_push(&a->undo_hist, &a->field, a->tick_num);
    a->is_playing = true;
    // The deadline armed, and FA when clock-out is active (midi_in.h).
    Tick_sink const sink = ged_sink(a);
    clock_out_on_start(&a->clock_out, a->clock_source, a->bpm, stm_now(),
                       &sink);
    send_control_message(a->oosc_dev, "/orca/started");
  } else {
    // Pause, and quit through it (tick.h, tick_stop): the note-offs, every
    // glide ended with the last values sent kept, so the next glide starts
    // from them, /orca/stopped, then FC only through the gate.
    bool clock_out = clock_out_active(&a->clock_out, a->clock_source);
    a->is_playing = false;
    Tick_sink const sink = ged_sink(a);
    tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
  }
  a->is_draw_dirty = true;
}

// Quit's stop: ged_set_playing(false), then a second release-all, which finds
// nothing sustained after a stop and sends nothing; it keeps quit's shape.
staticni void ged_stop(Ged *a) {
  ged_set_playing(a, false);
  ged_stop_all_sustained_notes(a);
}

#ifdef ZORCA_HAS_MIDI_IN
// The transport machine's start: FA or FB from the master. Unlike
// ged_set_playing(true) it pushes no undo snapshot, re-arms no internal
// deadline, sends no MIDI and never cancels glides (FA, FB and F2 leave
// them alone); it only marks the shell playing and tells OSC.
static void ged_external_start(Ged *a) {
  if (a->is_playing)
    return;
  a->is_playing = true;
  a->is_draw_dirty = true;
  send_control_message(a->oosc_dev, "/orca/started");
}

// Applies one event's or poll's actions, in transport.h's order: stop
// or clock lost; set tick; reset anchors; start; one tick body if a tick is
// due and the shell is playing. Then the estimate: tick_len follows it while
// valid, and on shown_changed the HUD redraws and /orca/bpm carries the shown
// value while it is valid (nothing on becoming invalid).
staticni void ged_apply_transport_actions(Ged *a,
                                          Transport_actions const *act) {
  if (act->stop || act->clock_lost) {
    ged_set_playing(a, false); // note-offs, glides cancelled, no FC
    a->is_draw_dirty = true;   // the HUD shows ext lost even with no estimate
  }
  if (act->set_tick) {
    a->tick_num = act->tick;
    a->needs_remarking = true;
    a->is_draw_dirty = true;
  }
  if (act->reset_anchors)
    opstate_reset_anchors(&a->opstate);
  if (act->start)
    ged_external_start(a);
  if (act->tick_due && a->is_playing)
    ged_run_tick_body(a);
  a->tick_len = act->est_valid ? act->tick_len : tick_len_us(a->bpm);
  if (act->shown_changed) {
    a->is_draw_dirty = true;
    if (act->est_valid)
      ged_send_osc_bpm(a, (I32)act->shown);
  }
}

// One input event, stamped by the reader at read time, through the machine.
static void ged_transport_event(Ged *a, Transport_event const *e) {
  Transport_actions act;
  transport_event(&a->transport, e, &act);
  ged_apply_transport_actions(a, &act);
}

// The machine's timeouts. now is a fresh now_us() read made after the drain.
static void ged_transport_poll(Ged *a, U64 now) {
  Transport_actions act;
  transport_poll(&a->transport, now, &act);
  ged_apply_transport_actions(a, &act);
}

// The reader lost queued input (-ENOSPC or pmBufferOverflow): an overflow,
// stamped with now_us() at read time, through the machine (transport.h).
static void ged_transport_overflow(Ged *a) {
  Transport_event const ov = {.type = Transport_overflow, .stamp = now_us()};
  ged_transport_event(a, &ov);
}
#endif // ZORCA_HAS_MIDI_IN

// Returns false when a cut or copy could not fill the internal clipboard.
staticni bool ged_input_cmd(Ged *a, Ged_input_cmd ev) {
  bool copied = true;
  switch (ev) {
  case Ged_input_cmd_undo:
    if (undo_history_count(&a->undo_hist) == 0)
      break;
    if (a->is_playing)
      undo_history_apply(&a->undo_hist, &a->field, &a->tick_num);
    else
      undo_history_pop(&a->undo_hist, &a->field, &a->tick_num);
    ged_cursor_confine(&a->ged_cursor, a->field.height, a->field.width);
    ged_update_internal_geometry(a);
    ged_make_cursor_visible(a);
    a->needs_remarking = true;
    a->is_draw_dirty = true;
    break;
  case Ged_input_cmd_toggle_append_mode:
    a->input_mode = a->input_mode == Ged_input_mode_append
                        ? Ged_input_mode_normal
                        : Ged_input_mode_append;
    a->is_draw_dirty = true;
    break;
  case Ged_input_cmd_toggle_selresize_mode:
    a->input_mode = a->input_mode == Ged_input_mode_selresize
                        ? Ged_input_mode_normal
                        : Ged_input_mode_selresize;
    a->is_draw_dirty = true;
    break;
  case Ged_input_cmd_toggle_slide_mode:
    a->input_mode = a->input_mode == Ged_input_mode_slide
                        ? Ged_input_mode_normal
                        : Ged_input_mode_slide;
    a->is_draw_dirty = true;
    break;
  case Ged_input_cmd_step_forward: {
    undo_history_push(&a->undo_hist, &a->field, a->tick_num);
    // The VM only, into the tick list: nothing is sent and no note ages.
    Orca_run_ctx const step_ctx = {
        .opstate = &a->opstate,
        .dialect = a->dialect,
        .labels = ged_labels_for_run(a, a->field.height, a->field.width)};
    tick_run_vm(a->field.buffer, a->mbuf_r.buffer, a->field.height,
                a->field.width, a->tick_num, &a->oevent_list, a->random_seed,
                &step_ctx);
    ++a->tick_num;
    a->activity_counter += a->oevent_list.count;
    a->needs_remarking = true;
    a->is_draw_dirty = true;
    break;
  }
  case Ged_input_cmd_toggle_play_pause:
    ged_set_playing(a, !a->is_playing);
    break;
  case Ged_input_cmd_toggle_show_event_list:
    a->draw_event_list = !a->draw_event_list;
    a->is_draw_dirty = true;
    break;
  case Ged_input_cmd_cut:
    copied = ged_copy_selection_to_clipbard(a);
    if (copied) {
      undo_history_push(&a->undo_hist, &a->field, a->tick_num);
      ged_fill_selection_with_char(a, '.');
      a->needs_remarking = true;
      a->is_draw_dirty = true;
    }
    break;
  case Ged_input_cmd_copy:
    copied = ged_copy_selection_to_clipbard(a);
    break;
  case Ged_input_cmd_paste: {
    Usz field_h = a->field.height;
    Usz field_w = a->field.width;
    Usz curs_y = a->ged_cursor.y;
    Usz curs_x = a->ged_cursor.x;
    if (curs_y >= field_h || curs_x >= field_w)
      break;
    Field *cb_field = &a->clipboard_field;
    Usz cbfield_h = cb_field->height;
    Usz cbfield_w = cb_field->width;
    Usz cpy_h = cbfield_h;
    Usz cpy_w = cbfield_w;
    if (field_h - curs_y < cpy_h)
      cpy_h = field_h - curs_y;
    if (field_w - curs_x < cpy_w)
      cpy_w = field_w - curs_x;
    if (cpy_h == 0 || cpy_w == 0)
      break;
    undo_history_push(&a->undo_hist, &a->field, a->tick_num);
    gbuffer_copy_subrect(cb_field->buffer, a->field.buffer, cbfield_h,
                         cbfield_w, field_h, field_w, 0, 0, curs_y, curs_x,
                         cpy_h, cpy_w);
    a->ged_cursor.h = cpy_h;
    a->ged_cursor.w = cpy_w;
    a->needs_remarking = true;
    a->is_draw_dirty = true;
    break;
  }
  case Ged_input_cmd_escape:
    if (a->input_mode != Ged_input_mode_normal) {
      a->input_mode = Ged_input_mode_normal;
      a->is_draw_dirty = true;
    } else if (a->ged_cursor.h != 1 || a->ged_cursor.w != 1) {
      a->ged_cursor.h = 1;
      a->ged_cursor.w = 1;
      a->is_draw_dirty = true;
    }
    break;
  }
  return copied;
}

static bool hacky_try_save(Field *field, char const *filename) {
  if (!filename)
    return false;
  if (field->height == 0 || field->width == 0)
    return false;
  if (path_is_dir(filename))
    return false;
  FILE *f = fopen(filename, "w");
  if (!f)
    return false;
  field_fput(field, f);
  fclose(f);
  return true;
}

//
// menu stuff
//

enum {
  Main_menu_id = 1,
  Open_form_id,
  Save_as_form_id,
  Set_tempo_form_id,
  Set_grid_dims_form_id,
  Autofit_menu_id,
  Confirm_new_file_menu_id,
  Cosmetics_menu_id,
  Osc_menu_id,
  Osc_output_address_form_id,
  Osc_output_port_form_id,
  Playback_menu_id,
  Set_soft_margins_form_id,
  Set_fancy_grid_dots_menu_id,
  Set_fancy_grid_rulers_menu_id,
  Dialect_menu_id,
  Glide_rate_menu_id,
#ifdef FEAT_PORTMIDI
  Portmidi_output_device_menu_id,
#endif
#ifdef ZORCA_HAS_MIDI_IN
  Clock_source_menu_id,
  Midi_input_menu_id,
#endif
};
enum {
  Autofit_nicely_id = 1,
  Autofit_tightly_id,
};
enum {
  Confirm_new_file_reject_id = 1,
  Confirm_new_file_accept_id,
};
enum {
  Main_menu_quit = 1,
  Main_menu_controls,
  Main_menu_opers_guide,
  Main_menu_new,
  Main_menu_open,
  Main_menu_save,
  Main_menu_save_as,
  Main_menu_set_tempo,
  Main_menu_set_grid_dims,
  Main_menu_autofit_grid,
  Main_menu_about,
  Main_menu_cosmetics,
  Main_menu_playback,
  Main_menu_osc,
  Main_menu_dialect,
#ifdef FEAT_PORTMIDI
  Main_menu_choose_portmidi_output,
#endif
};

static void push_main_menu(void) {
  Qmenu *qm = qmenu_create(Main_menu_id);
  qmenu_set_title(qm, "ORCA");
  qmenu_add_choice(qm, Main_menu_new, "New");
  qmenu_add_choice(qm, Main_menu_open, "Open...");
  qmenu_add_choice(qm, Main_menu_save, "Save");
  qmenu_add_choice(qm, Main_menu_save_as, "Save As...");
  qmenu_add_spacer(qm);
  qmenu_add_choice(qm, Main_menu_set_tempo, "Set BPM...");
  qmenu_add_choice(qm, Main_menu_set_grid_dims, "Set Grid Size...");
  qmenu_add_choice(qm, Main_menu_autofit_grid, "Auto-fit Grid");
  qmenu_add_spacer(qm);
  qmenu_add_choice(qm, Main_menu_osc, "OSC Output...");
#ifdef FEAT_PORTMIDI
  qmenu_add_choice(qm, Main_menu_choose_portmidi_output, "MIDI Output...");
#endif
  qmenu_add_spacer(qm);
  qmenu_add_choice(qm, Main_menu_playback, "Clock & Timing...");
  qmenu_add_choice(qm, Main_menu_dialect, "Operator Dialect...");
  qmenu_add_choice(qm, Main_menu_cosmetics, "Appearance...");
  qmenu_add_spacer(qm);
  qmenu_add_choice(qm, Main_menu_controls, "Controls...");
  qmenu_add_choice(qm, Main_menu_opers_guide, "Operators...");
  qmenu_add_choice(qm, Main_menu_about, "About ORCA...");
  qmenu_add_spacer(qm);
  qmenu_add_choice(qm, Main_menu_quit, "Quit");
  qmenu_push_to_nav(qm);
}

staticni void pop_qnav_if_main_menu(void) {
  Qblock *qb = qnav_top_block();
  if (qb && qb->tag == Qblock_type_qmenu &&
      qmenu_id(qmenu_of(qb)) == Main_menu_id)
    qnav_stack_pop();
}

static void push_confirm_new_file_menu(void) {
  Qmenu *qm = qmenu_create(Confirm_new_file_menu_id);
  qmenu_set_title(qm, "Are you sure?");
  qmenu_add_choice(qm, Confirm_new_file_reject_id, "Cancel");
  qmenu_add_choice(qm, Confirm_new_file_accept_id, "Create New File");
  qmenu_push_to_nav(qm);
}

static void push_autofit_menu(void) {
  Qmenu *qm = qmenu_create(Autofit_menu_id);
  qmenu_set_title(qm, "Auto-fit Grid");
  qmenu_add_choice(qm, Autofit_nicely_id, "Nicely");
  qmenu_add_choice(qm, Autofit_tightly_id, "Tightly");
  qmenu_push_to_nav(qm);
}

enum {
  Cosmetics_soft_margins_id = 1,
  Cosmetics_grid_dots_id,
  Cosmetics_grid_rulers_id,
};
static void push_cosmetics_menu(void) {
  Qmenu *qm = qmenu_create(Cosmetics_menu_id);
  qmenu_set_title(qm, "Appearance");
  qmenu_add_choice(qm, Cosmetics_soft_margins_id, "Margins...");
  qmenu_add_choice(qm, Cosmetics_grid_dots_id, "Grid dots...");
  qmenu_add_choice(qm, Cosmetics_grid_rulers_id, "Grid rulers...");
  qmenu_push_to_nav(qm);
}
static void push_soft_margins_form(int init_y, int init_x) {
  char buff[128];
  int snres = snprintf(buff, sizeof buff, "%dx%d", init_x, init_y);
  char const *inistr = snres > 0 && (Usz)snres < sizeof buff ? buff : "2x1";
  qform_single_line_input(Set_soft_margins_form_id, "Set Margins", inistr);
}
static void push_plainorfancy_menu(int menu_id, char const *title,
                                   bool initial_fancy) {
  Qmenu *qm = qmenu_create(menu_id);
  qmenu_set_title(qm, title);
  qmenu_add_printf(qm, 1, "(%c) Fancy", initial_fancy ? '*' : ' ');
  qmenu_add_printf(qm, 2, "(%c) Plain", !initial_fancy ? '*' : ' ');
  if (!initial_fancy)
    qmenu_set_current_item(qm, 2);
  qmenu_push_to_nav(qm);
}
enum {
  Osc_menu_output_enabledisable = 1,
  Osc_menu_output_address,
  Osc_menu_output_port,
};
static void push_osc_menu(bool output_enabled) {
  Qmenu *qm = qmenu_create(Osc_menu_id);
  qmenu_set_title(qm, "OSC Output");
  qmenu_add_printf(qm, Osc_menu_output_enabledisable, "[%c] OSC Output Enabled",
                   output_enabled ? '*' : ' ');
  qmenu_add_choice(qm, Osc_menu_output_address, "OSC Output Address...");
  qmenu_add_choice(qm, Osc_menu_output_port, "OSC Output Port...");
  qmenu_push_to_nav(qm);
}
static void push_osc_output_address_form(char const *initial) {
  qform_single_line_input(Osc_output_address_form_id, "Set OSC Output Address",
                          initial);
}
static void push_osc_output_port_form(char const *initial) {
  qform_single_line_input(Osc_output_port_form_id, "Set OSC Output Port",
                          initial);
}
// The operator dialect (sim.h): a radio with the active one
// marked. Picking it again changes nothing.
enum {
  Dialect_menu_borca = 1,
  Dialect_menu_upstream,
};
static void push_dialect_menu(Orca_dialect dialect) {
  bool upstream = dialect == Orca_dialect_upstream;
  Qmenu *qm = qmenu_create(Dialect_menu_id);
  qmenu_set_title(qm, "Operator Dialect");
  qmenu_add_printf(qm, Dialect_menu_borca, "(%c) bOrca", !upstream ? '*' : ' ');
  qmenu_add_printf(qm, Dialect_menu_upstream, "(%c) Upstream",
                   upstream ? '*' : ' ');
  if (upstream)
    qmenu_set_current_item(qm, Dialect_menu_upstream);
  qmenu_push_to_nav(qm);
}
enum {
  Playback_menu_midi_bclock = 1,
  Playback_menu_glide_rate,
  Playback_menu_clock_source,
  Playback_menu_midi_input,
};
// Clock & Timing. The beat-clock item shows the stored setting; under
// External it reads (suspended), since clock-out is gated off. Clock
// Source and MIDI Input exist only in a build with an input reader.
static void push_playback_menu(bool midi_bclock_enabled, bool external) {
  Qmenu *qm = qmenu_create(Playback_menu_id);
  qmenu_set_title(qm, "Clock & Timing");
  qmenu_add_printf(qm, Playback_menu_midi_bclock, "[%c] Send MIDI Beat Clock%s",
                   midi_bclock_enabled ? '*' : ' ',
                   external ? " (suspended)" : "");
  qmenu_add_choice(qm, Playback_menu_glide_rate, "Glide Rate...");
#ifdef ZORCA_HAS_MIDI_IN
  qmenu_add_choice(qm, Playback_menu_clock_source, "Clock Source...");
  qmenu_add_choice(qm, Playback_menu_midi_input, "MIDI Input...");
#else
  (void)external;
#endif
  qmenu_push_to_nav(qm);
}
#ifdef ZORCA_HAS_MIDI_IN
// The clock source: a radio with the active one marked, as
// the dialect radio. Picking it again changes nothing.
enum {
  Clock_source_menu_internal = 1,
  Clock_source_menu_external,
};
static void push_clock_source_menu(Clock_source source) {
  bool external = source == Clock_source_external;
  Qmenu *qm = qmenu_create(Clock_source_menu_id);
  qmenu_set_title(qm, "Clock Source");
  qmenu_add_printf(qm, Clock_source_menu_internal, "(%c) Internal",
                   !external ? '*' : ' ');
  qmenu_add_printf(qm, Clock_source_menu_external, "(%c) External",
                   external ? '*' : ' ');
  if (external)
    qmenu_set_current_item(qm, Clock_source_menu_external);
  qmenu_push_to_nav(qm);
}
#endif
// The CC engine's glide_hz, the most glide steps per second on one
// controller (ccout.h): a radio of the three valid rates with
// the active one marked, as the dialect radio. Picking it again changes
// nothing.
enum {
  Glide_rate_menu_50 = 1,
  Glide_rate_menu_100,
  Glide_rate_menu_200,
};
static void push_glide_rate_menu(Usz glide_hz) {
  Qmenu *qm = qmenu_create(Glide_rate_menu_id);
  qmenu_set_title(qm, "Glide Rate");
  qmenu_add_printf(qm, Glide_rate_menu_50, "(%c) 50 Hz",
                   glide_hz == 50 ? '*' : ' ');
  qmenu_add_printf(qm, Glide_rate_menu_100, "(%c) 100 Hz",
                   glide_hz == 100 ? '*' : ' ');
  qmenu_add_printf(qm, Glide_rate_menu_200, "(%c) 200 Hz",
                   glide_hz == 200 ? '*' : ' ');
  if (glide_hz == 100)
    qmenu_set_current_item(qm, Glide_rate_menu_100);
  else if (glide_hz == 200)
    qmenu_set_current_item(qm, Glide_rate_menu_200);
  qmenu_push_to_nav(qm);
}
static void push_about_msg(void) {
  // clang-format off
  static char const* logo[] = {
  "lqqqk|lqqqk|lqqqk|lqqqk",
  "x   x|x   j|x    |lqqqu",
  "mqqqj|m    |mqqqj|m   j",
  };
  static char const* footer =
  "Live Programming Environment";
  // clang-format on
  int rows = (int)ORCA_ARRAY_COUNTOF(logo);
  int cols = (int)strlen(logo[0]);
  int hpad = 5, tpad = 2, bpad = 2;
  int sep = 1;
  int footer_len = (int)strlen(footer);
  int width = footer_len;
  if (cols > width)
    width = cols;
  width += hpad * 2;
  int logo_left_pad = (width - cols) / 2;
  int footer_left_pad = (width - footer_len) / 2;
  Qmsg *qm = qmsg_push(tpad + rows + sep + 1 + bpad, width);
  WINDOW *w = qmsg_window(qm);
  for (int row = 0; row < rows; ++row) {
    wmove(w, row + tpad, logo_left_pad);
    wattrset(w, A_BOLD);
    for (int col = 0; col < cols; ++col) {
      char c = logo[row][col];
      chtype ch;
      if (c == ' ')
        ch = (chtype)' ';
      else if (c == '|')
        ch = ACS_VLINE | (chtype)fg_bg(C_black, C_natural) | A_BOLD;
      else
        ch = NCURSES_ACS(c) | A_BOLD;
      waddch(w, ch);
    }
  }
  wattrset(w, A_DIM);
  wmove(w, tpad + rows + sep, footer_left_pad);
  waddstr(w, footer);
}
static void push_controls_msg(void) {
  struct Ctrl_item {
    char const *input;
    char const *desc;
  };
  static struct Ctrl_item items[] = {
      {"Ctrl+Q", "Quit"},
      {"Arrow Keys", "Move Cursor"},
      {"Ctrl+D or F1", "Open Main Menu"},
      {"0-9, A-Z, a-z,", "Insert Character"},
      {"! : % / = # * ; $ &", NULL},
      {"Spacebar", "Play/Pause"},
      {"Ctrl+Z or Ctrl+U", "Undo"},
      {"Ctrl+X", "Cut"},
      {"Ctrl+C", "Copy"},
      {"Ctrl+V", "Paste"},
      {"Ctrl+O", "Open File"},
      {"Ctrl+S", "Save"},
      {"Ctrl+F", "Frame Step Forward"},
      {"Ctrl+R", "Reset Frame Number"},
      {"Ctrl+I or Insert", "Append/Overwrite Mode"},
      // {"/", "Key Trigger Mode"},
      {"' (quote)", "Rectangle Selection Mode"},
      {"Shift+Arrow Keys", "Adjust Rectangle Selection"},
      {"Alt+Arrow Keys", "Slide Selection"},
      {"` (grave) or ~", "Slide Selection Mode"},
      {"Escape", "Return to Normal Mode or Deselect"},
      {"( ) _ + [ ] { }", "Adjust Grid Size and Rulers"},
      {"< and >", "Adjust BPM"},
      {"Ctrl+E", "Show or Hide the Last Tick's Events"},
      {"Ctrl+G", "Operator Guide"},
      {"?", "Controls (this message)"},
  };
  int w_input = 0;
  int w_desc = 0;
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(items); ++i) {
    // use wcswidth instead of strlen if you need wide char support. but note
    // that won't be useful for UTF-8 or unicode chars in higher plane (emoji,
    // complex zwj, etc.)
    if (items[i].input) {
      int wl = (int)strlen(items[i].input);
      if (wl > w_input)
        w_input = wl;
    }
    if (items[i].desc) {
      int wr = (int)strlen(items[i].desc);
      if (wr > w_desc)
        w_desc = wr;
    }
  }
  int mid_pad = 2;
  int total_width = 1 + w_input + mid_pad + w_desc + 1;
  Qmsg *qm = qmsg_push(ORCA_ARRAY_COUNTOF(items), total_width);
  qmsg_set_title(qm, "Controls");
  WINDOW *w = qmsg_window(qm);
  for (int i = 0; i < (int)ORCA_ARRAY_COUNTOF(items); ++i) {
    if (items[i].input) {
      wmove(w, i, 1 + w_input - (int)strlen(items[i].input));
      waddstr(w, items[i].input);
    }
    if (items[i].desc) {
      wmove(w, i, 1 + w_input + mid_pad);
      waddstr(w, items[i].desc);
    }
  }
}
// The operator guide (Ctrl+G, and Operators... in the main menu) lists the
// active dialect's operators (sim.c's lists), each description at most 42
// characters.
static void push_opers_guide_msg(Orca_dialect dialect) {
  enum { Guide_borca = 1, Guide_upstream = 2, Guide_both = 3 };
  struct Guide_item {
    char glyph;
    U8 dialects;
    char const *name;
    char const *desc;
  };
  static struct Guide_item const items[] = {
      {'A', Guide_both, "add", "Outputs sum of inputs."},
      {'B', Guide_both, "subtract", "Outputs difference of inputs."},
      {'C', Guide_both, "clock", "Outputs modulo of frame."},
      {'D', Guide_both, "delay", "Bangs on modulo of frame."},
      {'E', Guide_both, "east", "Moves eastward, or bangs."},
      {'F', Guide_both, "if", "Bangs if inputs are equal."},
      {'G', Guide_both, "generator", "Writes operands with offset."},
      {'H', Guide_both, "halt", "Halts southward operand."},
      {'I', Guide_both, "increment", "Increments southward operand."},
      {'J', Guide_both, "jumper", "Outputs northward operand."},
      {'K', Guide_both, "konkat", "Reads multiple variables."},
      {'L', Guide_both, "lesser", "Outputs smallest input."},
      {'M', Guide_both, "multiply", "Outputs product of inputs."},
      {'N', Guide_both, "north", "Moves Northward, or bangs."},
      {'O', Guide_both, "read", "Reads operand with offset."},
      {'P', Guide_both, "push", "Writes eastward operand."},
      {'Q', Guide_both, "query", "Reads operands with offset."},
      {'R', Guide_both, "random", "Outputs random value."},
      {'r', Guide_borca, "random unique",
       "Shuffles min to max; never twice in a row."},
      {'S', Guide_both, "south", "Moves southward, or bangs."},
      {'T', Guide_both, "track", "Reads eastward operand."},
      {'U', Guide_both, "uclid", "Bangs on Euclidean rhythm."},
      {'V', Guide_both, "variable", "Reads and writes variable."},
      {'W', Guide_both, "west", "Moves westward, or bangs."},
      {'X', Guide_both, "write", "Writes operand with offset."},
      {'Y', Guide_both, "jymper", "Outputs westward operand."},
      {'Z', Guide_both, "lerp", "Transitions operand to target."},
      {'*', Guide_both, "bang", "Bangs neighboring operands."},
      {'#', Guide_both, "comment", "Halts line."},
      {':', Guide_both, "midi", "Sends MIDI note."},
      {'!', Guide_borca, "cc", "Sends MIDI CC; rates 1-y glide from last."},
      {'!', Guide_upstream, "cc", "Sends MIDI control change."},
      {'?', Guide_both, "pb", "Sends MIDI pitch bend."},
      {'$', Guide_borca, "scale", "Outputs note base on root, scale, degree."},
      {'%', Guide_both, "mono", "Sends MIDI monophonic note."},
      {'=', Guide_borca, "midichord", "Sends preset chords over MIDI."},
      {'=', Guide_upstream, "osc", "Sends OSC message."},
      {';', Guide_borca, "arpeggiator",
       "Outputs scale degrees by length and step."},
      {';', Guide_upstream, "udp", "Sends UDP message."},
      {'&', Guide_borca, "bouncer", "LFO; rate is its cycle in beats."},
  };
  U8 const shown =
      dialect == Orca_dialect_upstream ? Guide_upstream : Guide_borca;
  int rows = 0;
  int w_desc = 0;
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(items); ++i) {
    if (!(items[i].dialects & shown))
      continue;
    ++rows;
    int wr = (int)strlen(items[i].desc);
    if (wr > w_desc)
      w_desc = wr;
  }
  int left_pad = 1, mid_pad = 1, right_pad = 1;
  int total_width = left_pad + 1 + mid_pad + w_desc + right_pad;
  Qmsg *qm = qmsg_push(rows, total_width);
  qmsg_set_title(qm, dialect == Orca_dialect_upstream ? "Upstream Operators"
                                                      : "bOrca Operators");
  WINDOW *w = qmsg_window(qm);
  int row = 0;
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(items); ++i) {
    if (!(items[i].dialects & shown))
      continue;
    wmove(w, row, left_pad);
    waddch(w, (chtype)items[i].glyph | A_bold);
    wmove(w, row, left_pad + 1 + mid_pad);
    wattrset(w, A_normal);
    waddstr(w, items[i].desc);
    ++row;
  }
}
static void push_open_form(char const *initial) {
  qform_single_line_input(Open_form_id, "Open", initial);
}
staticni bool try_save_with_msg(Field *field, oso const *str) {
  if (!osolen(str))
    return false;
  if (path_is_dir(osoc(str))) {
    qmsg_printf_push("Error Saving File", "%s:\nIs a directory", osoc(str));
    return false;
  }
  bool ok = hacky_try_save(field, osoc(str));
  if (ok) {
    Qmsg *qm = qmsg_printf_push(NULL, "Saved to:\n%s", osoc(str));
    qmsg_set_dismiss_mode(qm, Qmsg_dismiss_mode_passthrough);
  } else {
    qmsg_printf_push("Error Saving File", "Unable to save file to:\n%s",
                     osoc(str));
  }
  return ok;
}
static void push_save_as_form(char const *initial) {
  qform_single_line_input(Save_as_form_id, "Save As", initial);
}
static void push_set_tempo_form(Usz initial) {
  char buff[64];
  int snres = snprintf(buff, sizeof buff, "%zu", initial);
  char const *inistr = snres > 0 && (Usz)snres < sizeof buff ? buff : "120";
  qform_single_line_input(Set_tempo_form_id, "Set BPM", inistr);
}
static void push_set_grid_dims_form(Usz init_height, Usz init_width) {
  char buff[128];
  int snres = snprintf(buff, sizeof buff, "%zux%zu", init_width, init_height);
  char const *inistr = snres > 0 && (Usz)snres < sizeof buff ? buff : "57x25";
  qform_single_line_input(Set_grid_dims_form_id, "Set Grid Size", inistr);
}

#ifdef FEAT_PORTMIDI
staticni void push_portmidi_output_device_menu(Midi_mode const *midi_mode) {
  Qmenu *qm = qmenu_create(Portmidi_output_device_menu_id);
  qmenu_set_title(qm, "PortMidi Device Selection");
  PmError e = portmidi_init_if_necessary();
  if (e) {
    qmenu_destroy(qm);
    qmsg_printf_push("PortMidi Error",
                     "PortMidi error during initialization:\n%s",
                     Pm_GetErrorText(e));
    return;
  }
  int num = Pm_CountDevices();
  int output_devices = 0;
  int cur_dev_id = 0;
  bool has_cur_dev_id = false;
  if (midi_mode->any.type == Midi_mode_type_portmidi) {
    cur_dev_id = midi_mode->portmidi.device_id;
    has_cur_dev_id = true;
  }
  for (int i = 0; i < num; ++i) {
    PmDeviceInfo const *info = Pm_GetDeviceInfo(i);
    if (!info || !info->output)
      continue;
    bool is_cur_dev_id = has_cur_dev_id && cur_dev_id == i;
    qmenu_add_printf(qm, i, "(%c) #%d - %s", is_cur_dev_id ? '*' : ' ', i,
                     info->name);
    ++output_devices;
  }
  if (output_devices == 0) {
    qmenu_destroy(qm);
    qmsg_printf_push("No PortMidi Devices",
                     "No PortMidi output devices found.");
    return;
  }
  if (has_cur_dev_id) {
    qmenu_set_current_item(qm, cur_dev_id);
  }
  qmenu_push_to_nav(qm);
}
#endif

staticni bool read_int(char const *str, int *out) {
  int a;
  int res = sscanf(str, "%d", &a);
  if (res != 1)
    return false;
  *out = a;
  return true;
}

typedef enum {
  Brackpaste_seq_none = 0,
  Brackpaste_seq_begin,
  Brackpaste_seq_end,
} Brackpaste_seq;

// Try to getch to complete the sequence of a start or end of brackpet paste
// escape sequence. If it doesn't turn out to be one, unwind it back into the
// event queue with ungetch. Yeah this is golfed let me have fun.
staticni Brackpaste_seq brackpaste_seq_getungetch(WINDOW *win) {
  int chs[5], n = 0, begorend; // clang-format off
  if ((chs[n++] = wgetch(win)) != '[') goto unwind;
  if ((chs[n++] = wgetch(win)) != '2') goto unwind;
  if ((chs[n++] = wgetch(win)) != '0') goto unwind;
  chs[n++] = begorend = wgetch(win);
  if (begorend != '0' && begorend != '1') goto unwind;
  if ((chs[n++] = wgetch(win)) != '~') goto unwind;
  return begorend == '0' ? Brackpaste_seq_begin : Brackpaste_seq_end;
unwind:
  while (n > 0) ungetch(chs[--n]);
  return Brackpaste_seq_none; // clang-format on
}

staticni void try_send_to_gui_clipboard(Ged const *a,
                                        bool *io_use_gui_clipboard) {
  if (!*io_use_gui_clipboard)
    return;
#if 0 // If we want to use grid directly
  Usz curs_y, curs_x, curs_h, curs_w;
  if (!ged_try_selection_clipped_to_field(a, &curs_y, &curs_x, &curs_h,
                                          &curs_w))
    return;
  Cboard_error cberr =
      cboard_copy(a->clipboard_field.buffer, a->clipboard_field.height,
                  a->clipboard_field.width, curs_y, curs_x, curs_h, curs_w);
#endif
  Usz cb_h = a->clipboard_field.height, cb_w = a->clipboard_field.width;
  if (cb_h < 1 || cb_w < 1)
    return;
  Cboard_error cberr =
      cboard_copy(a->clipboard_field.buffer, cb_h, cb_w, 0, 0, cb_h, cb_w);
  if (cberr)
    *io_use_gui_clipboard = false;
}

// The settings file; its keys and their rules are tui_prefs.h's.
static char const *const conf_file_name = "orca.conf";

// The input key of the compiled backend: the ALSA `client:port`
// name, or the PortMidi input device name.
#ifdef FEAT_ALSA
#define Confopt_midi_input_device Confopt_alsa_input_device
#elif defined(FEAT_PORTMIDI)
#define Confopt_midi_input_device Confopt_portmidi_input_device
#endif

typedef struct {
  Ged ged;
  // The orca.conf settings for this session (tui_prefs.h): loaded, then the
  // flags over them, then handed to the engine once (tui_apply_overrides);
  // each menu or form change goes through a setter, then the engine, then
  // tui_save_prefs. prefs.midi_in is the MIDI input in the compiled
  // backend's naming (Confopt_midi_input_device), empty when none is set;
  // prefs.clock_source is applied once at startup by
  // tui_apply_clock_source, after the overrides and before auto-play.
  Tui_prefs prefs;
  Tui_overrides overrides;
  oso *file_name;
  oso *osc_midi_bidule_path;
#ifdef FEAT_ALSA
  Alsa_ctx *alsa; // the input reader's own pointer to the one ALSA context
#endif
#ifdef FEAT_PORTMIDI
  PortMidiStream *midi_in_stream; // open only while External
#endif
#if defined(ZORCA_DEBUG_COUNTERS) && defined(ZORCA_HAS_MIDI_IN)
  // The input reader's overflows (-ENOSPC, pmBufferOverflow) and other read
  // errors. Printed to stderr after endwin.
  Usz midi_in_overflows, midi_in_errors;
#endif
  int undo_history_limit;
  int hardmargin_y, hardmargin_x;
  bool use_gui_cboard; // not bitfields due to taking address of
  bool strict_timing;
} Tui;

ORCA_OK_IF_UNUSED staticni void print_loading_message(char const *s) {
  Usz len = strlen(s);
  if (len > INT_MAX)
    return;
  int h, w;
  getmaxyx(stdscr, h, w);
  int y = h / 2;
  int x = (int)len < w ? (w - (int)len) / 2 : 0;
  werase(stdscr);
  wmove(stdscr, y, x);
  waddstr(stdscr, s);
  refresh();
}

// Reads orca.conf (if it exists) into the settings, one line at a time
// (tui_prefs_read_line), then opens the PortMidi output it names.
staticni void tui_load_conf(Tui *t) {
  Ezconf_r ez;
  for (ezconf_r_start(&ez, conf_file_name);
       ezconf_r_step(&ez, confopts, Confoptslen);)
    (void)tui_prefs_read_line(&t->prefs, (Confopt)ez.index, ez.value);
#ifdef FEAT_PORTMIDI
  char const *portmidi_output_device = t->prefs.portmidi_output_device;
  if (t->ged.midi_mode.any.type == Midi_mode_type_null &&
      portmidi_output_device[0] != '\0') {
    // PortMidi can be hilariously slow to initialize. Since it will be
    // initialized automatically if the user has a prefs entry for PortMidi
    // devices, we should show a message to the user letting them know why
    // orca is locked up/frozen. (When it's done via menu action, that's
    // fine, since they can figure out why.)
    print_loading_message("Waiting on PortMidi...");
    PmError pmerr;
    PmDeviceID devid;
    if (portmidi_find_device_id_by_name(portmidi_output_device,
                                        strlen(portmidi_output_device), &pmerr,
                                        &devid)) {
      midi_mode_deinit(&t->ged.midi_mode);
      pmerr = midi_mode_init_portmidi(&t->ged.midi_mode, devid);
      if (pmerr) {
        // todo stuff
      }
    }
  }
#endif
}

// Applies the command-line overrides over orca.conf's values, clearing
// their touched bits (tui_prefs_apply_overrides), then hands the
// settings to the engine once: the dialect, glide_hz and the stored beat
// clock setting. Nothing plays and no glide runs yet, so this one hand-off
// after the flags is what applying the conf during the load and the flags
// again gave. The clock source follows later, in tui_apply_clock_source.
staticni void tui_apply_overrides(Tui *t) {
  tui_prefs_apply_overrides(&t->prefs, &t->overrides);
  (void)ged_set_dialect(&t->ged, t->prefs.dialect);
  ccout_configure(&t->ged.ccout, t->prefs.glide_hz);
  t->ged.clock_out.enabled = t->prefs.midi_bclock;
}

staticni void tui_save_prefs(Tui *t) {
  Ezconf_opt optsbuff[Confoptslen];
  Ezconf_w ez;
  ezconf_w_start(&ez, optsbuff, ORCA_ARRAY_COUNTOF(optsbuff), conf_file_name);
  oso *midi_output_device_name = NULL;
  switch (t->ged.midi_mode.any.type) {
#ifdef FEAT_ALSA
  case Midi_mode_type_alsa:
    break;
#endif
  case Midi_mode_type_null:
    break;
  case Midi_mode_type_osc_bidule:
    // TODO
    break;
#ifdef FEAT_PORTMIDI
  case Midi_mode_type_portmidi: {
    PmError pmerror;
    if (!portmidi_find_name_of_device_id(t->ged.midi_mode.portmidi.device_id,
                                         &pmerror, &midi_output_device_name) ||
        osolen(midi_output_device_name) < 1) {
      osowipe(&midi_output_device_name);
      break;
    }
    ezconf_w_addopt(&ez, confopts[Confopt_portmidi_output_device],
                    Confopt_portmidi_output_device);
    break;
  }
#endif
  }
  // Add each key a save writes from the prefs (tui_prefs_saves_key: touched,
  // that is read from the file with a value taken or changed by a menu, and
  // not portmidi_output_device). The PortMidi output line is added by the
  // switch above.
  for (int i = 0; i < Confoptslen; i++) {
    if (tui_prefs_saves_key(&t->prefs, (Confopt)i))
      ezconf_w_addopt(&ez, confopts[i], i);
  }
  char number[Tui_prefs_number_cap]; // tui_prefs_saved_value's margins
  while (ezconf_w_step(&ez)) {
    switch (ez.optid) {
#ifdef FEAT_PORTMIDI
    case Confopt_portmidi_output_device:
      fputs(osoc(midi_output_device_name), ez.file);
      break;
#endif
    default:
      fputs(tui_prefs_saved_value(&t->prefs, (Confopt)ez.optid, number),
            ez.file);
      break;
    }
  }
  osofree(midi_output_device_name);
  if (ez.error) {
    char const *msg = ezconf_w_errorstring(ez.error);
    qmsg_printf_push("Config Error",
                     "Error when writing configuration file:\n%s", msg);
  }
}

staticni bool tui_suggest_nice_grid_size(Tui *t, int win_h, int win_w,
                                         Usz *out_grid_h, Usz *out_grid_w) {
  int softmargin_y = t->prefs.softmargin_y,
      softmargin_x = t->prefs.softmargin_x;
  int ruler_spacing_y = (int)t->ged.ruler_spacing_y,
      ruler_spacing_x = (int)t->ged.ruler_spacing_x;
  if (win_h < 1 || win_w < 1 || softmargin_y < 0 || softmargin_x < 0 ||
      ruler_spacing_y < 1 || ruler_spacing_x < 1)
    return false;
  // TODO overflow checks
  int h = (win_h - softmargin_y - Hud_height - 1) / ruler_spacing_y;
  h *= ruler_spacing_y;
  int w = (win_w - softmargin_x * 2 - 1) / ruler_spacing_x;
  w *= ruler_spacing_x;
  if (h < ruler_spacing_y)
    h = ruler_spacing_y;
  if (w < ruler_spacing_x)
    w = ruler_spacing_x;
  h++;
  w++;
  if (h >= ORCA_Y_MAX || w >= ORCA_X_MAX)
    return false;
  *out_grid_h = (Usz)h;
  *out_grid_w = (Usz)w;
  return true;
}

staticni bool tui_suggest_tight_grid_size(Tui *t, int win_h, int win_w,
                                          Usz *out_grid_h, Usz *out_grid_w) {
  int softmargin_y = t->prefs.softmargin_y,
      softmargin_x = t->prefs.softmargin_x;
  if (win_h < 1 || win_w < 1 || softmargin_y < 0 || softmargin_x < 0)
    return false;
  // TODO overflow checks
  int h = win_h - softmargin_y - Hud_height;
  int w = win_w - softmargin_x * 2;
  if (h < 1 || w < 1 || h >= ORCA_Y_MAX || w >= ORCA_X_MAX)
    return false;
  *out_grid_h = (Usz)h;
  *out_grid_w = (Usz)w;
  return true;
}

// set is tui_prefs_set_grid_dots or tui_prefs_set_grid_rulers.
staticni void plainorfancy_menu_was_picked(Tui *t, int picked_id,
                                           bool (*set)(Tui_prefs *, bool)) {
  bool is_fancy = picked_id == 1; // 1 -> fancy, 2 -> plain
  qnav_stack_pop();
  // ^- doesn't actually matter when we do this, with our current code
  if (!set(&t->prefs, is_fancy))
    return; // already the style: nothing changes, nothing is saved
  t->ged.is_draw_dirty = true;
  tui_save_prefs(t);
}

staticni void dialect_menu_was_picked(Tui *t, int picked_id) {
  Orca_dialect dialect = picked_id == Dialect_menu_upstream
                             ? Orca_dialect_upstream
                             : Orca_dialect_borca;
  qnav_stack_pop();
  if (!tui_prefs_set_dialect(&t->prefs, dialect))
    return; // already the dialect: nothing changes, nothing is saved
  (void)ged_set_dialect(&t->ged, dialect);
  tui_save_prefs(t);
}

// A new glide_hz applies at once, to running glides too (every glide_hz
// change configures the engine), and is saved.
staticni void glide_rate_menu_was_picked(Tui *t, int picked_id) {
  Usz glide_hz = picked_id == Glide_rate_menu_50    ? 50
                 : picked_id == Glide_rate_menu_200 ? 200
                                                    : 100;
  qnav_stack_pop();
  if (!tui_prefs_set_glide_hz(&t->prefs, glide_hz))
    return; // already the rate: nothing changes, nothing is saved
  ccout_configure(&t->ged.ccout, glide_hz); // at once, running glides too
  tui_save_prefs(t);
}

#ifdef ZORCA_HAS_MIDI_IN
// The MIDI input. It is open, as an ALSA subscription or a PortMidi
// input stream, only while the clock source is External. A missing or failed
// source shows a qnav message and never prints after initscr.

// Closes the input: unsubscribes the source and System Announce, or closes
// the PortMidi stream. Nothing happens when it is not open.
staticni void tui_input_close(Tui *t) {
#ifdef FEAT_ALSA
  alsa_in_unsubscribe(t->alsa);
#endif
#ifdef FEAT_PORTMIDI
  if (t->midi_in_stream) {
    Pm_Close(t->midi_in_stream);
    t->midi_in_stream = NULL;
  }
#endif
}

// Opens the input named by prefs.midi_in: subscribes it to MIDI in (ALSA),
// or opens its stream with the drop list and discards what arrived before
// the filter (PortMidi). On a failure the message says so; under ALSA the
// shell then waits for PORT_START.
staticni void tui_input_open(Tui *t) {
  char const *name = t->prefs.midi_in;
#ifdef FEAT_ALSA
  int result = alsa_in_subscribe(t->alsa, name);
  if (!*name) {
    qmsg_printf_push("MIDI Input", "No MIDI input chosen. Pick one under "
                                   "Clock & Timing, MIDI Input...");
  } else if (result == 2) {
    qmsg_printf_push("MIDI Input", "'%s' is not a client:port name", name);
  } else if (result == 1) {
    qmsg_printf_push("MIDI Input",
                     "No MIDI input matches:\n%s\n\nWaiting for it to appear. "
                     "Pick one under Clock & Timing, MIDI Input...",
                     name);
  } else if (result < 0) {
    qmsg_printf_push("MIDI Input", "Cannot subscribe to %s:\n%s", name,
                     snd_strerror(result));
  }
#endif
#ifdef FEAT_PORTMIDI
  if (t->midi_in_stream)
    return;
  if (!portmidi_is_initialized)
    print_loading_message("Waiting on PortMidi...");
  PmError pmerr = pmNoError; // an empty name never asks PortMidi
  PmDeviceID devid;
  if (!*name || !portmidi_find_input_id_by_name(name, &pmerr, &devid)) {
    if (pmerr) {
      qmsg_printf_push("PortMidi Error",
                       "PortMidi error during initialization:\n%s",
                       Pm_GetErrorText(pmerr));
    } else {
      qmsg_printf_push("MIDI Input",
                       "No PortMidi input matches:\n%s\n\nPick one under "
                       "Clock & Timing, MIDI Input... (a device plugged in "
                       "after orca started needs a restart)",
                       name);
    }
    return;
  }
  enum { Portmidi_in_buffer = 256 };
  pmerr = Pm_OpenInput(&t->midi_in_stream, devid, NULL, Portmidi_in_buffer,
                       portmidi_timeproc, NULL);
  if (pmerr) {
    t->midi_in_stream = NULL;
    qmsg_printf_push("PortMidi Error", "Error opening MIDI input %s:\n%s",
                     name, Pm_GetErrorText(pmerr));
    return;
  }
  Pm_SetFilter(t->midi_in_stream, ZORCA_PM_IN_DROP);
  // Discard whatever arrived before the filter.
  PmEvent discard[32];
  while (Pm_Read(t->midi_in_stream, discard, 32) > 0) {
  }
#endif
}

// Drains every queued input event, each stamped with now_us() at read time
// and passed through the machine, its actions applied before the next is
// read. -ENOSPC and pmBufferOverflow are counted and passed
// to the machine as an overflow: input arrived and was flushed (usually
// pulses), so it keeps following, and with no F8 after it clock lost comes
// 1 s later (transport.h). The drain goes on; any other error is counted and
// ends it. Nothing happens while the input is not open.
staticni void tui_input_drain(Tui *t) {
#ifdef FEAT_ALSA
  Alsa_ctx *ctx = t->alsa;
  int own_client = snd_seq_client_id(ctx->seq);
  for (;;) {
    snd_seq_event_t *ev;
    int err = snd_seq_event_input(ctx->seq, &ev);
    if (err == -EAGAIN)
      break;
    if (err == -ENOSPC) {
#ifdef ZORCA_DEBUG_COUNTERS
      ++t->midi_in_overflows;
#endif
      // The kernel's overflow flush emptied the whole input FIFO, announces
      // included, so a CLIENT_EXIT, PORT_EXIT or PORT_UNSUBSCRIBED for the
      // stored source may be lost: clear it if its port is gone, and while
      // none is stored match once, as a lost PORT_START would have.
      if (ctx->source.has) {
        snd_seq_port_info_t *pinfo;
        snd_seq_port_info_alloca(&pinfo);
        if (snd_seq_get_any_port_info(ctx->seq, ctx->source.client,
                                      ctx->source.port, pinfo) < 0)
          ctx->source.has = false;
      }
      if (!ctx->source.has && t->prefs.midi_in[0] != '\0')
        (void)alsa_in_match(ctx, t->prefs.midi_in);
      ged_transport_overflow(&t->ged);
      continue;
    }
    if (err < 0) {
#ifdef ZORCA_DEBUG_COUNTERS
      ++t->midi_in_errors;
#endif
      break;
    }
    U64 stamp = now_us();
    if (ev->source.client == SND_SEQ_CLIENT_SYSTEM &&
        ev->source.port == SND_SEQ_PORT_SYSTEM_ANNOUNCE) {
      // System Announce first: the stored address is cleared when the kernel
      // has dropped the subscription, and matched again on PORT_START.
      switch (alsa_in_announce(ev, &ctx->source, own_client, ctx->in_port)) {
      case Alsa_in_clear:
        ctx->source.has = false;
        break;
      case Alsa_in_rematch:
        if (t->prefs.midi_in[0] != '\0')
          (void)alsa_in_match(ctx, t->prefs.midi_in);
        break;
      case Alsa_in_keep:
        break;
      }
      continue;
    }
    Transport_event te;
    if (alsa_in_map_event(ev, stamp, &te))
      ged_transport_event(&t->ged, &te);
    // Every other event is read and discarded.
  }
#endif
#ifdef FEAT_PORTMIDI
  if (!t->midi_in_stream)
    return;
  PmEvent buffer[64];
  for (;;) {
    int n = Pm_Read(t->midi_in_stream, buffer, 64);
    if (n == 0)
      break;
    if (n < 0) {
      if (n == pmBufferOverflow) {
#ifdef ZORCA_DEBUG_COUNTERS
        ++t->midi_in_overflows;
#endif
        ged_transport_overflow(&t->ged);
        continue;
      }
#ifdef ZORCA_DEBUG_COUNTERS
      ++t->midi_in_errors;
#endif
      break;
    }
    for (int i = 0; i < n; ++i) {
      PmMessage msg = buffer[i].message; // the timestamp is never read
      Transport_event te;
      if (pm_in_decode(msg, now_us(), &te))
        ged_transport_event(&t->ged, &te);
    }
  }
#endif
}

// Stops without transmitting, resets the machine, drains and discards the
// queued input, then opens the input: entering External, and changing the
// input while External.
staticni void tui_input_restart(Tui *t) {
  Ged *a = &t->ged;
  ged_set_playing(a, false); // clock_out_active() is already false: no FC
  transport_reset(&a->transport);
  a->tick_len = tick_len_us(a->bpm);
  tui_input_close(t);
#ifdef FEAT_ALSA
  snd_seq_drop_input(t->alsa->seq);
#endif
  tui_input_open(t);
  a->is_draw_dirty = true;
}

// The clock-source switch. Entering External stops playback
// without transmitting (the source changes first, so clock_out_active() is
// false), resets the machine, drains the input and opens it. Leaving
// External closes the input, adopts the estimate's shown value as the BPM
// when one is valid (sending /orca/bpm when it changed), and if playing
// re-arms the internal deadline with no undo push, so the first internal
// tick comes one tick span after the switch, with FB when clock-out is then
// active. Nothing else is sent, and nothing at all while stopped. The same
// source changes nothing.
staticni void tui_set_clock_source(Tui *t, Clock_source source) {
  Ged *a = &t->ged;
  if (source == a->clock_source)
    return;
  if (source == Clock_source_external) {
    a->clock_source = Clock_source_external;
    tui_input_restart(t);
  } else {
    tui_input_close(t);
    bool est_valid = a->transport.est_valid;
    U16 shown = a->transport.shown;
    transport_reset(&a->transport);
    a->clock_source = Clock_source_internal;
    Usz old_bpm = a->bpm;
    ged_set_bpm(a, est_valid ? (Usz)shown : a->bpm);
    if (a->bpm != old_bpm)
      ged_send_osc_bpm(a, (I32)a->bpm);
    Tick_sink const sink = ged_sink(a);
    clock_out_return_internal(&a->clock_out, a->bpm, stm_now(), a->is_playing,
                              &sink);
    a->is_draw_dirty = true;
  }
}

// Once, at startup, after tui_apply_overrides and before auto-play.
staticni void tui_apply_clock_source(Tui *t) {
  tui_set_clock_source(t, t->prefs.clock_source);
}

staticni void clock_source_menu_was_picked(Tui *t, int picked_id) {
  Clock_source source = picked_id == Clock_source_menu_external
                            ? Clock_source_external
                            : Clock_source_internal;
  qnav_stack_pop();
  if (!tui_prefs_set_clock_source(&t->prefs, source))
    return; // already the source: nothing changes, nothing is saved
  tui_set_clock_source(t, source);
  tui_save_prefs(t);
}

#ifdef FEAT_ALSA
// The ports a user may follow: readable and subscribable, not NO_EXPORT,
// not System's and not zOrca's own, in client then port order, shown as
// client:port. An item's id encodes the address, so a pick needs no list.
enum { Alsa_in_menu_ports = 256 };
static int alsa_in_menu_id(int client, int port) {
  return client * Alsa_in_menu_ports + port + 1;
}
staticni void push_alsa_input_menu(Tui *t) {
  snd_seq_t *seq = t->alsa->seq;
  int own = snd_seq_client_id(seq);
  Qmenu *qm = qmenu_create(Midi_input_menu_id);
  qmenu_set_title(qm, "MIDI Input");
  snd_seq_client_info_t *cinfo;
  snd_seq_port_info_t *pinfo;
  snd_seq_client_info_alloca(&cinfo);
  snd_seq_port_info_alloca(&pinfo);
  snd_seq_client_info_set_client(cinfo, -1);
  int listed = 0;
  int current = 0;
  Usz cur_client_len = 0;
  char const *cur_port = NULL;
  char const *cur = t->prefs.midi_in;
  bool has_cur =
      cur[0] != '\0' && alsa_in_split(cur, &cur_client_len, &cur_port);
  while (snd_seq_query_next_client(seq, cinfo) >= 0) {
    int client = snd_seq_client_info_get_client(cinfo);
    if (client == SND_SEQ_CLIENT_SYSTEM || client == own)
      continue;
    char const *cname = snd_seq_client_info_get_name(cinfo);
    snd_seq_port_info_set_client(pinfo, client);
    snd_seq_port_info_set_port(pinfo, -1);
    while (snd_seq_query_next_port(seq, pinfo) >= 0) {
      unsigned caps = snd_seq_port_info_get_capability(pinfo);
      unsigned const want = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
      if ((caps & want) != want || (caps & SND_SEQ_PORT_CAP_NO_EXPORT))
        continue;
      char const *pname = snd_seq_port_info_get_name(pinfo);
      int port = snd_seq_port_info_get_port(pinfo);
      int id = alsa_in_menu_id(client, port);
      bool is_cur = has_cur && strlen(cname) == cur_client_len &&
                    strncmp(cname, cur, cur_client_len) == 0 &&
                    strcmp(pname, cur_port) == 0;
      if (is_cur && !current)
        current = id;
      qmenu_add_printf(qm, id, "(%c) %s:%s", is_cur ? '*' : ' ', cname, pname);
      ++listed;
    }
  }
  if (listed == 0) {
    qmenu_destroy(qm);
    qmsg_printf_push("No MIDI Inputs", "No ALSA ports to follow were found.");
    return;
  }
  if (current)
    qmenu_set_current_item(qm, current);
  qmenu_push_to_nav(qm);
}
// The picked address's names, as client:port; false if it is gone.
staticni bool alsa_in_menu_name(Tui *t, int id, oso **out_name) {
  snd_seq_t *seq = t->alsa->seq;
  int client = (id - 1) / Alsa_in_menu_ports;
  int port = (id - 1) % Alsa_in_menu_ports;
  snd_seq_client_info_t *cinfo;
  snd_seq_port_info_t *pinfo;
  snd_seq_client_info_alloca(&cinfo);
  snd_seq_port_info_alloca(&pinfo);
  if (snd_seq_get_any_client_info(seq, client, cinfo) < 0 ||
      snd_seq_get_any_port_info(seq, client, port, pinfo) < 0)
    return false;
  osoput(out_name, snd_seq_client_info_get_name(cinfo));
  osocat(out_name, ":");
  osocat(out_name, snd_seq_port_info_get_name(pinfo));
  return true;
}
#endif
#ifdef FEAT_PORTMIDI
// The PortMidi input devices, by device id, with the current one marked.
staticni void push_portmidi_input_menu(Tui *t) {
  Qmenu *qm = qmenu_create(Midi_input_menu_id);
  qmenu_set_title(qm, "MIDI Input");
  PmError e = portmidi_init_if_necessary();
  if (e) {
    qmenu_destroy(qm);
    qmsg_printf_push("PortMidi Error",
                     "PortMidi error during initialization:\n%s",
                     Pm_GetErrorText(e));
    return;
  }
  int num = Pm_CountDevices();
  int listed = 0;
  int current = -1;
  char const *cur = t->prefs.midi_in[0] != '\0' ? t->prefs.midi_in : NULL;
  for (int i = 0; i < num; ++i) {
    PmDeviceInfo const *info = Pm_GetDeviceInfo(i);
    if (!info || !info->input)
      continue;
    bool is_cur = cur && strcmp(cur, info->name) == 0;
    if (is_cur && current < 0)
      current = i;
    qmenu_add_printf(qm, i, "(%c) #%d - %s", is_cur ? '*' : ' ', i,
                     info->name);
    ++listed;
  }
  if (listed == 0) {
    qmenu_destroy(qm);
    qmsg_printf_push("No MIDI Inputs", "No PortMidi input devices found.");
    return;
  }
  if (current >= 0)
    qmenu_set_current_item(qm, current);
  qmenu_push_to_nav(qm);
}
#endif

staticni void push_midi_input_menu(Tui *t) {
#ifdef FEAT_ALSA
  push_alsa_input_menu(t);
#endif
#ifdef FEAT_PORTMIDI
  push_portmidi_input_menu(t);
#endif
}

// A pick saves only on change. Under External the new input is a source
// switch: stop without transmitting, reset, drain, then open it.
staticni void midi_input_menu_was_picked(Tui *t, int picked_id) {
  oso *name = NULL;
  bool found = false;
#ifdef FEAT_ALSA
  found = alsa_in_menu_name(t, picked_id, &name);
#endif
#ifdef FEAT_PORTMIDI
  {
    PmError pmerr;
    found = portmidi_find_name_of_input_id(picked_id, &pmerr, &name);
  }
#endif
  qnav_stack_pop();
  if (!found || !osolen(name)) {
    qmsg_printf_push("MIDI Input", "That MIDI input is no longer there.");
    osofree(name);
    return;
  }
  bool changed = tui_prefs_set_midi_in(&t->prefs, osoc(name));
  osofree(name);
  if (!changed) {
    // Already the input (the name is not empty, and no device name nears
    // Tui_prefs_value_cap, so that is the setter's false): nothing is saved.
    // Under External with the input not open (its source was cleared and no
    // PORT_START followed, or the stream failed to open), picking it again
    // opens it again.
    bool open = false;
#ifdef FEAT_ALSA
    open = t->alsa->source.has;
#endif
#ifdef FEAT_PORTMIDI
    open = t->midi_in_stream != NULL;
#endif
    if (t->ged.clock_source == Clock_source_external && !open)
      tui_input_restart(t);
    return;
  }
  if (t->ged.clock_source == Clock_source_external)
    tui_input_restart(t);
  tui_save_prefs(t);
}
#endif // ZORCA_HAS_MIDI_IN

staticni bool tui_restart_osc_udp_if_enabled_diderror(Tui *t) {
  bool error = false;
  if (t->prefs.osc_output_enabled) {
    // An empty address goes as NULL, which getaddrinfo resolves to
    // loopback; the port goes as held.
    char const *address = t->prefs.osc_output_address;
    error = !ged_set_osc_udp(&t->ged, address[0] != '\0' ? address : NULL,
                             t->prefs.osc_output_port);
  } else {
    ged_clear_osc_udp(&t->ged);
  }
  // Under --osc-midi-bidule, MIDI goes out over this OSC connection, so
  // restarting it changes the output: the CC engine forgets the last values
  // sent. This also runs once at startup, where the engine is fresh.
  if (t->ged.midi_mode.any.type == Midi_mode_type_osc_bidule)
    ccout_forget(&t->ged.ccout);
  return error;
}
staticni void tui_restart_osc_udp_showerror(void) {
  qmsg_printf_push("OSC Networking Error", "Failed to set up OSC networking");
}
staticni void tui_restart_osc_udp_if_enabled(Tui *t) {
  bool old_inuse = ged_is_using_osc_udp(&t->ged);
  bool did_error = tui_restart_osc_udp_if_enabled_diderror(t);
  bool new_inuse = ged_is_using_osc_udp(&t->ged);
  if (old_inuse != new_inuse) {
    Qblock *qb = qnav_top_block();
    if (qb && qb->tag == Qblock_type_qmenu &&
        qmenu_id(qmenu_of(qb)) == Osc_menu_id) {
      int itemid = qmenu_current_item(qmenu_of(qb));
      qnav_stack_pop();
      push_osc_menu(new_inuse);
      qmenu_set_current_item(qmenu_of(qnav_top_block()), itemid);
    }
  }
  if (did_error)
    tui_restart_osc_udp_showerror();
}
staticni void tui_adjust_term_size(Tui *t, WINDOW **cont_window) {
  int term_h, term_w;
  getmaxyx(stdscr, term_h, term_w);
  assert(term_h >= 0 && term_w >= 0);
  int content_y = 0, content_x = 0;
  int content_h = term_h, content_w = term_w;
  if (t->hardmargin_y > 0 && term_h > t->hardmargin_y * 2 + 2) {
    content_y += t->hardmargin_y;
    content_h -= t->hardmargin_y * 2;
  }
  if (t->hardmargin_x > 0 && term_w > t->hardmargin_x * 2 + 2) {
    content_x += t->hardmargin_x;
    content_w -= t->hardmargin_x * 2;
  }
  bool remake_window = true;
  if (*cont_window) {
    int cwin_y, cwin_x, cwin_h, cwin_w;
    getbegyx(*cont_window, cwin_y, cwin_x);
    getmaxyx(*cont_window, cwin_h, cwin_w);
    remake_window = cwin_y != content_y || cwin_x != content_x ||
                    cwin_h != content_h || cwin_w != content_w;
  }
  if (remake_window) {
    if (*cont_window)
      delwin(*cont_window);
    wclear(stdscr);
    *cont_window = derwin(stdscr, content_h, content_w, content_y, content_x);
    t->ged.is_draw_dirty = true;
  }
  // OK to call this unconditionally -- deriving the sub-window areas is
  // more than a single comparison, and we don't want to split up or
  // duplicate the math and checks for it, so this routine will calculate
  // the stuff it needs to and then early-out if there's no further work.
  ged_set_window_size(&t->ged, content_h, content_w, t->prefs.softmargin_y,
                      t->prefs.softmargin_x);
}

static void tui_try_save(Tui *t) {
  if (osolen(t->file_name) > 0)
    try_save_with_msg(&t->ged.field, t->file_name);
  else
    push_save_as_form("");
}

typedef enum {
  Tui_menus_nothing = 0,
  Tui_menus_quit,
  Tui_menus_consumed_input,
} Tui_menus_result;

staticni Tui_menus_result tui_drive_menus(Tui *t, int key) {
  Qblock *qb = qnav_top_block();
  if (!qb)
    return Tui_menus_nothing;
  if (key == CTRL_PLUS('q'))
    return Tui_menus_quit;
  switch (qb->tag) {
  case Qblock_type_qmsg: {
    Qmsg *qm = qmsg_of(qb);
    Qmsg_action act;
    if (qmsg_drive(qm, key, &act)) {
      if (act.dismiss)
        qnav_stack_pop();
      if (act.passthrough)
        return Tui_menus_nothing;
    }
    break;
  }
  case Qblock_type_qmenu: {
    Qmenu *qm = qmenu_of(qb);
    Qmenu_action act;
    // special case for main menu: pressing the key to open it will close
    // it again.
    if (qmenu_id(qm) == Main_menu_id &&
        (key == CTRL_PLUS('d') || key == KEY_F(1))) {
      qnav_stack_pop();
      break;
    }
    if (!qmenu_drive(qm, key, &act))
      break;
    switch (act.any.type) {
    case Qmenu_action_type_canceled:
      qnav_stack_pop();
      break;
    case Qmenu_action_type_picked:
      switch (qmenu_id(qm)) {
      case Main_menu_id:
        switch (act.picked.id) {
        case Main_menu_quit:
          return Tui_menus_quit;
        case Main_menu_playback:
          push_playback_menu(t->ged.clock_out.enabled,
                             t->ged.clock_source == Clock_source_external);
          break;
        case Main_menu_cosmetics:
          push_cosmetics_menu();
          break;
        case Main_menu_osc:
          push_osc_menu(ged_is_using_osc_udp(&t->ged));
          break;
        case Main_menu_dialect:
          push_dialect_menu(t->ged.dialect);
          break;
        case Main_menu_controls:
          push_controls_msg();
          break;
        case Main_menu_opers_guide:
          push_opers_guide_msg(t->ged.dialect);
          break;
        case Main_menu_about:
          push_about_msg();
          break;
        case Main_menu_new:
          push_confirm_new_file_menu();
          break;
        case Main_menu_open:
          push_open_form(osoc(t->file_name));
          break;
        case Main_menu_save:
          tui_try_save(t);
          break;
        case Main_menu_save_as:
          push_save_as_form(osoc(t->file_name));
          break;
        case Main_menu_set_tempo:
          // Under External the tempo is the master's.
          if (t->ged.clock_source == Clock_source_external)
            qmsg_printf_push("Set BPM", "BPM follows the external clock");
          else
            push_set_tempo_form(t->ged.bpm);
          break;
        case Main_menu_set_grid_dims:
          push_set_grid_dims_form(t->ged.field.height, t->ged.field.width);
          break;
        case Main_menu_autofit_grid:
          push_autofit_menu();
          break;
#ifdef FEAT_PORTMIDI
        case Main_menu_choose_portmidi_output:
          push_portmidi_output_device_menu(&t->ged.midi_mode);
          break;
#endif
        }
        break;
      case Autofit_menu_id: {
        Usz new_field_h, new_field_w;
        bool did_get_ok_size = false;
        switch (act.picked.id) {
        case Autofit_nicely_id:
          did_get_ok_size = tui_suggest_nice_grid_size(
              t, t->ged.win_h, t->ged.win_w, &new_field_h, &new_field_w);
          break;
        case Autofit_tightly_id:
          did_get_ok_size = tui_suggest_tight_grid_size(
              t, t->ged.win_h, t->ged.win_w, &new_field_h, &new_field_w);
          break;
        }
        if (did_get_ok_size) {
          ged_resize_grid(&t->ged.field, &t->ged.mbuf_r, new_field_h,
                          new_field_w, t->ged.tick_num, &t->ged.scratch_field,
                          &t->ged.undo_hist, &t->ged.ged_cursor,
                          &t->ged.opstate);
          ged_update_internal_geometry(&t->ged);
          t->ged.needs_remarking = true;
          t->ged.is_draw_dirty = true;
          ged_make_cursor_visible(&t->ged);
        }
        qnav_stack_pop();
        pop_qnav_if_main_menu();
        break;
      }
      case Confirm_new_file_menu_id:
        switch (act.picked.id) {
        case Confirm_new_file_reject_id:
          qnav_stack_pop();
          break;
        case Confirm_new_file_accept_id: {
          Usz new_field_h, new_field_w;
          if (tui_suggest_nice_grid_size(t, t->ged.win_h, t->ged.win_w,
                                         &new_field_h, &new_field_w)) {
            // The mark buffer and the new grid grow before anything changes;
            // if either cannot, the menu closes and the grid stays as it was.
            Field fresh;
            field_init(&fresh);
            if (mbuf_reusable_ensure_size(&t->ged.mbuf_r, new_field_h,
                                          new_field_w) &&
                field_resize_raw(&fresh, new_field_h, new_field_w)) {
              undo_history_push(&t->ged.undo_hist, &t->ged.field,
                                t->ged.tick_num);
              field_deinit(&t->ged.field);
              t->ged.field = fresh;
              memset(t->ged.field.buffer, '.',
                     new_field_h * new_field_w * sizeof(Glyph));
              opstate_clear(&t->ged.opstate);
              tick_forget_patch(&t->ged.ccout);
              ged_cursor_confine(&t->ged.ged_cursor, new_field_h, new_field_w);
              ged_update_internal_geometry(&t->ged);
              ged_make_cursor_visible(&t->ged);
              t->ged.needs_remarking = true;
              t->ged.is_draw_dirty = true;
              osoclear(&t->file_name);
            } else {
              field_deinit(&fresh);
              beep();
            }
            qnav_stack_pop();
            pop_qnav_if_main_menu();
          }
          break;
        }
        }
        break;
      case Cosmetics_menu_id:
        switch (act.picked.id) {
        case Cosmetics_soft_margins_id:
          push_soft_margins_form(t->prefs.softmargin_y,
                                 t->prefs.softmargin_x);
          break;
        case Cosmetics_grid_dots_id:
          push_plainorfancy_menu(Set_fancy_grid_dots_menu_id, "Grid Dots",
                                 t->prefs.fancy_grid_dots);
          break;
        case Cosmetics_grid_rulers_id:
          push_plainorfancy_menu(Set_fancy_grid_rulers_menu_id, "Grid Rulers",
                                 t->prefs.fancy_grid_rulers);
          break;
        }
        break;
      case Playback_menu_id:
        switch (act.picked.id) {
        case Playback_menu_midi_bclock: {
          // The stored setting flips and is saved whatever the source.
          // While playing on Internal, clock-out resumes with FB and the
          // sixths counter at 0, or stops with FC; either way the next tick
          // keeps its due time (midi_in.h, clock_out_toggle). Under External
          // nothing is sent: clock-out stays suspended.
          Ged *a = &t->ged;
          bool external = a->clock_source == Clock_source_external;
          U64 now = stm_now();
          double e = stm_sec(stm_diff(now, a->clock_out.clock)) +
                     a->clock_out.accum_secs;
          Tick_sink const sink = ged_sink(a);
          clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, now, e,
                           a->is_playing, &sink);
          // The stored setting follows the flag the toggle flipped.
          bool changed =
              tui_prefs_set_midi_bclock(&t->prefs, a->clock_out.enabled);
          qnav_stack_pop();
          push_playback_menu(a->clock_out.enabled, external);
          if (changed)
            tui_save_prefs(t);
          a->is_draw_dirty = true;
          break;
        }
        case Playback_menu_glide_rate:
          push_glide_rate_menu(t->ged.ccout.glide_hz);
          break;
#ifdef ZORCA_HAS_MIDI_IN
        case Playback_menu_clock_source:
          push_clock_source_menu(t->ged.clock_source);
          break;
        case Playback_menu_midi_input:
          push_midi_input_menu(t);
          break;
#endif
        }
        break;
#ifdef ZORCA_HAS_MIDI_IN
      case Clock_source_menu_id:
        clock_source_menu_was_picked(t, act.picked.id);
        break;
      case Midi_input_menu_id:
        midi_input_menu_was_picked(t, act.picked.id);
        break;
#endif
      case Glide_rate_menu_id:
        glide_rate_menu_was_picked(t, act.picked.id);
        break;
      case Set_fancy_grid_dots_menu_id:
        plainorfancy_menu_was_picked(t, act.picked.id,
                                     tui_prefs_set_grid_dots);
        break;
      case Set_fancy_grid_rulers_menu_id:
        plainorfancy_menu_was_picked(t, act.picked.id,
                                     tui_prefs_set_grid_rulers);
        break;
      case Dialect_menu_id:
        dialect_menu_was_picked(t, act.picked.id);
        break;
      case Osc_menu_id:
        switch (act.picked.id) {
        case Osc_menu_output_enabledisable: {
          // Not an early return on the same value: with the flag held on and
          // UDP not in use (a failed start or address change, which reset
          // nothing), the toggle tries the restart again.
          qnav_stack_pop();
          bool on = !ged_is_using_osc_udp(&t->ged);
          bool changed = tui_prefs_set_osc_output_enabled(&t->prefs, on);
          // Funny dance to keep the qnav stack in good order
          bool diderror = tui_restart_osc_udp_if_enabled_diderror(t);
          push_osc_menu(ged_is_using_osc_udp(&t->ged));
          if (diderror) {
            if (tui_prefs_set_osc_output_enabled(&t->prefs, false))
              changed = true;
            tui_restart_osc_udp_showerror();
          }
          if (changed)
            tui_save_prefs(t);
          break;
        }
        case Osc_menu_output_address:
          push_osc_output_address_form(t->prefs.osc_output_address);
          break;
        case Osc_menu_output_port:
          push_osc_output_port_form(t->prefs.osc_output_port);
          break;
        }
        break;
#ifdef FEAT_PORTMIDI
      case Portmidi_output_device_menu_id: {
        ged_stop_all_sustained_notes(&t->ged);
        midi_mode_deinit(&t->ged.midi_mode);
        PmError pme = midi_mode_init_portmidi(&t->ged.midi_mode, act.picked.id);
        // A new output knows none of the values sent.
        ccout_forget(&t->ged.ccout);
        qnav_stack_pop();
        if (pme) {
          qmsg_printf_push("PortMidi Error",
                           "Error setting PortMidi output device:\n%s",
                           Pm_GetErrorText(pme));
        } else {
          tui_save_prefs(t);
        }
        break;
      }
#endif
      }
      break;
    }
    break;
  }
  case Qblock_type_qform: {
    Qform *qf = qform_of(qb);
    Qform_action act;
    if (qform_drive(qf, key, &act)) {
      switch (act.any.type) {
      case Qform_action_type_canceled:
        qnav_stack_pop();
        break;
      case Qform_action_type_submitted: {
        switch (qform_id(qf)) {
        case Open_form_id: {
          oso *temp_name = qform_get_nonempty_single_line_input(qf);
          if (!temp_name)
            break;
          expand_home_tilde(&temp_name);
          if (!temp_name)
            break;
          if (path_is_dir(osoc(temp_name))) {
            qmsg_printf_push("Error Loading File", "%s:\nIs a directory",
                             osoc(temp_name));
            osofree(temp_name);
            break;
          }
          // The file loads into a temporary and the mark buffer grows before
          // the live grid changes, so a failed load or allocation leaves the
          // grid and the undo history as they were.
          Field loaded;
          field_init(&loaded);
          Field_load_error fle = field_load_file(osoc(temp_name), &loaded);
          if (fle == Field_load_error_ok &&
              !mbuf_reusable_ensure_size(&t->ged.mbuf_r, loaded.height,
                                         loaded.width))
            fle = Field_load_error_no_memory;
          if (fle == Field_load_error_ok) {
            undo_history_push(&t->ged.undo_hist, &t->ged.field,
                              t->ged.tick_num);
            if (loaded.height > 0) {
              field_deinit(&t->ged.field);
              t->ged.field = loaded;
            } else {
              field_deinit(&loaded); // an empty file leaves the grid
            }
            opstate_clear(&t->ged.opstate);
            tick_forget_patch(&t->ged.ccout);
            qnav_stack_pop();
            osoputoso(&t->file_name, temp_name);
            ged_cursor_confine(&t->ged.ged_cursor, t->ged.field.height,
                               t->ged.field.width);
            ged_update_internal_geometry(&t->ged);
            ged_make_cursor_visible(&t->ged);
            t->ged.needs_remarking = true;
            t->ged.is_draw_dirty = true;
            pop_qnav_if_main_menu();
          } else {
            field_deinit(&loaded);
            qmsg_printf_push("Error Loading File", "%s:\n%s", osoc(temp_name),
                             field_load_error_string(fle));
          }
          osofree(temp_name);
          break;
        }
        case Save_as_form_id: {
          oso *temp_name = qform_get_nonempty_single_line_input(qf);
          if (!temp_name)
            break;
          expand_home_tilde(&temp_name);
          if (!temp_name)
            break;
          if (path_is_dir(osoc(temp_name))) {
            qmsg_printf_push("Error Saving File", "%s:\nIs a directory",
                             osoc(temp_name));
            osofree(temp_name);
            break;
          }
          qnav_stack_pop();
          bool saved_ok = try_save_with_msg(&t->ged.field, temp_name);
          if (saved_ok)
            osoputoso(&t->file_name, temp_name);
          osofree(temp_name);
          break;
        }
        case Set_tempo_form_id: {
          oso *tmpstr = qform_get_nonempty_single_line_input(qf);
          if (!tmpstr)
            break;
          int newbpm = atoi(osoc(tmpstr));
          if (newbpm > 0) {
            ged_set_bpm(&t->ged, (Usz)newbpm);
            qnav_stack_pop();
          }
          osofree(tmpstr);
          break;
        }
        case Osc_output_address_form_id: {
          oso *addr = NULL;
          // Empty string is OK here. The restart runs whether or not the
          // address changed (it retries a failed one); the save only on a
          // change.
          if (qform_get_single_text_line(qf, &addr)) {
            qnav_stack_pop();
            bool changed = tui_prefs_set_osc_output_address(
                &t->prefs, osolen(addr) ? osoc(addr) : "");
            tui_restart_osc_udp_if_enabled(t);
            if (changed)
              tui_save_prefs(t);
            else if (osolen(addr) >= Tui_prefs_value_cap)
              qmsg_printf_push("OSC Output Address", "Value too long");
          }
          osofree(addr);
          break;
        }
        case Osc_output_port_form_id: {
          oso *portstr = qform_get_nonempty_single_line_input(qf);
          if (!portstr)
            break;
          qnav_stack_pop();
          // As the address: the restart always, the save on a change.
          bool changed =
              tui_prefs_set_osc_output_port(&t->prefs, osoc(portstr));
          bool too_long = osolen(portstr) >= Tui_prefs_value_cap;
          osofree(portstr);
          tui_restart_osc_udp_if_enabled(t);
          if (changed)
            tui_save_prefs(t);
          else if (too_long)
            qmsg_printf_push("OSC Output Port", "Value too long");
          break;
        }
        case Set_grid_dims_form_id: {
          oso *tmpstr = qform_get_nonempty_single_line_input(qf);
          if (!tmpstr)
            break;
          int newheight, newwidth;
          if (sscanf(osoc(tmpstr), "%dx%d", &newwidth, &newheight) == 2 &&
              newheight > 0 && newwidth > 0 && newheight < ORCA_Y_MAX &&
              newwidth < ORCA_X_MAX) {
            if (t->ged.field.height != (Usz)newheight ||
                t->ged.field.width != (Usz)newwidth) {
              ged_resize_grid(&t->ged.field, &t->ged.mbuf_r, (Usz)newheight,
                              (Usz)newwidth, t->ged.tick_num,
                              &t->ged.scratch_field, &t->ged.undo_hist,
                              &t->ged.ged_cursor, &t->ged.opstate);
              ged_update_internal_geometry(&t->ged);
              t->ged.needs_remarking = true;
              t->ged.is_draw_dirty = true;
              ged_make_cursor_visible(&t->ged);
            }
            qnav_stack_pop();
          }
          osofree(tmpstr);
          break;
        }
        case Set_soft_margins_form_id: {
          oso *tmpstr = qform_get_nonempty_single_line_input(qf);
          if (!tmpstr)
            break;
          // Might push message, so gotta pop old guy first
          qnav_stack_pop();
          int newy, newx;
          if (tui_prefs_read_nxn_or_n(osoc(tmpstr), &newx, &newy) &&
              tui_prefs_set_margins(&t->prefs, newx, newy)) {
            ungetch(KEY_RESIZE); // kinda lame but whatever
            tui_save_prefs(t);
          }
          osofree(tmpstr);
          break;
        }
        }
        break;
      }
      }
    }
    break;
  }
  }
  return Tui_menus_consumed_input;
}

//
// main
//

enum {
  Argopt_hardmargins = UCHAR_MAX + 1,
  Argopt_undo_limit,
  Argopt_init_grid_size,
  Argopt_osc_midi_bidule,
  Argopt_strict_timing,
  Argopt_bpm,
  Argopt_seed,
  Argopt_portmidi_deprecated,
  Argopt_osc_deprecated,
  Argopt_dialect,
  Argopt_version,
  Argopt_clock_source,
  Argopt_midi_in,
};

int main(int argc, char **argv) {
  static struct option tui_options[] = {
      {"hard-margins", required_argument, 0, Argopt_hardmargins},
      {"undo-limit", required_argument, 0, Argopt_undo_limit},
      {"initial-size", required_argument, 0, Argopt_init_grid_size},
      {"help", no_argument, 0, 'h'},
      {"osc-midi-bidule", required_argument, 0, Argopt_osc_midi_bidule},
      {"strict-timing", no_argument, 0, Argopt_strict_timing},
      {"bpm", required_argument, 0, Argopt_bpm},
      {"seed", required_argument, 0, Argopt_seed},
      {"dialect", required_argument, 0, Argopt_dialect},
      {"version", no_argument, 0, Argopt_version},
#ifdef ZORCA_HAS_MIDI_IN
      // Unknown options in a build with no input reader.
      {"clock-source", required_argument, 0, Argopt_clock_source},
      {"midi-in", required_argument, 0, Argopt_midi_in},
#endif
      {"portmidi-list-devices", no_argument, 0, Argopt_portmidi_deprecated},
      {"portmidi-output-device", required_argument, 0,
       Argopt_portmidi_deprecated},
      {"osc-server", required_argument, 0, Argopt_osc_deprecated},
      {"osc-port", required_argument, 0, Argopt_osc_deprecated},
      {NULL, 0, NULL, 0}};
  int init_bpm = 120;
  int init_seed = 1;
  int init_grid_dim_y = 25, init_grid_dim_x = 57;
  bool explicit_initial_grid_size = false;

  Tui t = {.file_name = NULL}; // Weird because of clang warning
  t.undo_history_limit = 100;
  t.use_gui_cboard = true;
  // The settings' defaults, before the flags and orca.conf. Without a reader
  // the input key is ignored, so Confoptslen stands in for the alias, which
  // has no #else arm.
#ifdef ZORCA_HAS_MIDI_IN
  tui_prefs_init(&t.prefs, true, Confopt_midi_input_device);
#else
  tui_prefs_init(&t.prefs, false, Confoptslen);
#endif

  int longindex = 0;
  for (;;) {
    int c = getopt_long(argc, argv, "h", tui_options, &longindex);
    if (c == -1)
      break;
    switch (c) {
    case 'h':
      usage();
      exit(0);
    case Argopt_version:
      printf("zOrca %s\n", ZORCA_VERSION);
      exit(0);
    case '?':
      usage();
      exit(1);
#define OPTFAIL(...)                                                           \
  {                                                                            \
    fprintf(stderr, "Bad %s argument: %s\n", tui_options[longindex].name,      \
            optarg);                                                           \
    fprintf(stderr, __VA_ARGS__);                                              \
    fputc('\n', stderr);                                                       \
    exit(1);                                                                   \
  }
    case Argopt_hardmargins:
      if (tui_prefs_read_nxn_or_n(optarg, &t.hardmargin_x, &t.hardmargin_y) &&
          t.hardmargin_x >= 0 && t.hardmargin_y >= 0)
        break;
      OPTFAIL("Must be 0 or positive integer.");
    case Argopt_undo_limit:
      if (read_int(optarg, &t.undo_history_limit) && t.undo_history_limit >= 0)
        break;
      OPTFAIL("Must be 0 or positive integer.");
    case Argopt_bpm:
      if (read_int(optarg, &init_bpm) && init_bpm >= 1)
        break;
      OPTFAIL("Must be positive integer.");
    case Argopt_seed:
      if (read_int(optarg, &init_seed) && init_seed >= 0)
        break;
      OPTFAIL("Must be 0 or positive integer.");
    case Argopt_dialect:
      if (orca_dialect_from_name(optarg, &t.overrides.dialect)) {
        t.overrides.keys |= TOUCHFLAG(Confopt_dialect);
        break;
      }
      usage(); // then the error, last, where it shows
      OPTFAIL("Must be borca or upstream.");
    case Argopt_clock_source:
      // A session-only override through the pending overrides.
      if (clock_source_from_name(optarg, &t.overrides.clock_source)) {
        t.overrides.keys |= TOUCHFLAG(Confopt_clock_source);
        break;
      }
      usage();
      OPTFAIL("Must be internal or external.");
    case Argopt_midi_in:
#ifdef ZORCA_HAS_MIDI_IN
      // Any non-empty name that fits is accepted here and resolved after
      // initscr, as the conf key is; one that does not match shows a message
      // then.
      if (optarg[0] != '\0') {
        Usz len = strlen(optarg);
        if (len >= sizeof t.overrides.midi_in)
          OPTFAIL("Must be shorter than %d bytes.", (int)Tui_prefs_value_cap);
        memcpy(t.overrides.midi_in, optarg, len + 1);
        t.overrides.keys |= TOUCHFLAG(Confopt_midi_input_device);
        break;
      }
#endif
      OPTFAIL("Must not be empty.");
    case Argopt_init_grid_size:
      if (sscanf(optarg, "%dx%d", &init_grid_dim_x, &init_grid_dim_y) != 2)
        OPTFAIL("Bad format or count. Expected something like: 40x30");
      if (init_grid_dim_x <= 0 || init_grid_dim_x > ORCA_X_MAX)
        OPTFAIL("X dimension for initial-size must be 1 <= n <= %d, was %d.",
                ORCA_X_MAX, init_grid_dim_x);
      if (init_grid_dim_y <= 0 || init_grid_dim_y > ORCA_Y_MAX)
        OPTFAIL("Y dimension for initial-size must be 1 <= n <= %d, was %d.",
                ORCA_Y_MAX, init_grid_dim_y);
      explicit_initial_grid_size = true;
      break;
    case Argopt_osc_midi_bidule:
      osoput(&t.osc_midi_bidule_path, optarg);
      break;
    case Argopt_strict_timing:
      t.strict_timing = true;
      break;
    case Argopt_portmidi_deprecated:
      fprintf(stderr,
              "Option \"--%s\" has been removed.\nInstead, choose "
              "your MIDI output device from within the ORCA menu.\n"
              "This new menu allows you to pick your MIDI device "
              "interactively\n",
              tui_options[longindex].name);
      exit(1);
    case Argopt_osc_deprecated:
      fprintf(
          stderr,
          "Options --osc-server and --osc-port have been removed.\n"
          "Instead, set the OSC server and port from within the ORCA menu.\n");
      exit(1);
    }
  }
#undef OPTFAIL
  if (optind == argc - 1) {
    osoput(&t.file_name, argv[optind]);
  } else if (optind < argc - 1) {
    fprintf(stderr, "Expected only 1 file argument.\n");
    exit(1);
  }
#ifdef FEAT_ALSA
  // The one ALSA client, opened before curses starts so that a failure can
  // still be reported on stderr.
  Alsa_ctx alsa_ctx;
  {
    bool scan_failed;
    int err = alsa_ctx_open(&alsa_ctx, &scan_failed);
    char const *name = alsa_client_name();
    if (err == -ENAMETOOLONG) {
      fprintf(stderr,
              "Cannot start: the ALSA client name '%s' is %zu bytes; ALSA "
              "keeps at most %d (set a shorter ZORCA_ALSA_CLIENT_NAME)\n",
              name, strlen(name), Alsa_client_name_max);
      osofree(t.file_name);
      osofree(t.osc_midi_bidule_path);
      return 1;
    }
    if (scan_failed) {
      fprintf(stderr,
              "Cannot start: the scan for an ALSA client named '%s' failed: "
              "%s\n",
              name, snd_strerror(err));
      osofree(t.file_name);
      osofree(t.osc_midi_bidule_path);
      return 1;
    }
    if (err == -EEXIST) {
      fprintf(stderr,
              "Cannot start: an ALSA client named '%s' is already running "
              "(set ZORCA_ALSA_CLIENT_NAME to run another instance)\n",
              name);
      osofree(t.file_name);
      osofree(t.osc_midi_bidule_path);
      return 1;
    }
    if (err < 0) {
      fprintf(stderr, "Cannot initialize zOrca ALSA source: %s\n",
              snd_strerror(err));
      osofree(t.file_name);
      osofree(t.osc_midi_bidule_path);
      return 1;
    }
    t.alsa = &alsa_ctx; // the input reader's own pointer
  }
#endif
  qnav_init(); // Initialize the menu/navigation global state
  // Initialize the 'Grid EDitor' stuff. This sits underneath the TUI.
  ged_init(&t.ged, (Usz)t.undo_history_limit, (Usz)init_bpm, (Usz)init_seed);
#ifdef FEAT_ALSA
  // MIDI goes out through the ALSA client, unless --osc-midi-bidule takes
  // the output below; the client then stays open and its port sends nothing.
  if (osolen(t.osc_midi_bidule_path) == 0) {
    int err = midi_mode_init_alsa(&t.ged.midi_mode, &alsa_ctx);
    if (err < 0) {
      fprintf(stderr, "Cannot initialize zOrca ALSA source: %s\n",
              snd_strerror(err));
      ged_deinit(&t.ged);
      alsa_ctx_close(&alsa_ctx);
      osofree(t.file_name);
      osofree(t.osc_midi_bidule_path);
      return 1;
    }
  }
#endif
  // This will need to be changed to work with conf/menu
  if (osolen(t.osc_midi_bidule_path) > 0) {
    midi_mode_deinit(&t.ged.midi_mode);
    midi_mode_init_osc_bidule(&t.ged.midi_mode, osoc(t.osc_midi_bidule_path));
  }
  stm_setup(); // Set up timer lib
  // Enable UTF-8 by explicitly initializing our locale before initializing
  // ncurses. Only needed (maybe?) if using libncursesw/wide-chars or UTF-8.
  // Using it unguarded will mess up box drawing chars in Linux virtual
  // consoles unless using libncursesw.
  setlocale(LC_ALL, "");
  initscr(); // Initialize ncurses
#ifdef FEAT_ALSA
  // Nothing prints to stderr while curses owns the terminal.
  snd_lib_error_set_handler(alsa_silent_error_handler);
  // After initscr, which installs its own SIGTERM and SIGINT handlers (they
  // exit without stopping playback); SIGHUP it leaves alone. A signal that
  // was ignored when orca started (nohup, a supervisor) stays ignored, as
  // ncurses leaves it.
  {
    static int const shutdown_signals[] = {SIGTERM, SIGHUP, SIGINT};
    struct sigaction shutdown_action = {0};
    shutdown_action.sa_handler = alsa_request_shutdown;
    sigemptyset(&shutdown_action.sa_mask);
    bool signals_ok = true;
    for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(shutdown_signals); ++i) {
      struct sigaction old_action;
      if (sigaction(shutdown_signals[i], NULL, &old_action) < 0) {
        signals_ok = false;
        break;
      }
      if (old_action.sa_handler == SIG_IGN)
        continue;
      if (sigaction(shutdown_signals[i], &shutdown_action, NULL) < 0) {
        signals_ok = false;
        break;
      }
    }
    if (!signals_ok) {
      int sig_errno = errno;
      endwin();
      snd_lib_error_set_handler(NULL);
      fprintf(stderr, "Cannot install zOrca shutdown handlers: %s\n",
              strerror(sig_errno));
      ged_deinit(&t.ged);
      alsa_ctx_close(&alsa_ctx);
      osofree(t.file_name);
      osofree(t.osc_midi_bidule_path);
      return 1;
    }
  }
#endif
  // Allow ncurses to control newline translation. Fine to use with any modern
  // terminal, and will let ncurses run faster.
  nonl();
  // Set interrupt keys (interrupt, break, quit...) to not flush. Helps keep
  // ncurses state consistent, at the cost of less responsive terminal
  // interrupt. (This will rarely happen.)
  intrflush(stdscr, FALSE);
  // Receive keyboard input immediately without line buffering, and receive
  // ctrl+z, ctrl+c etc. as input instead of having a signal generated. We need
  // to do this even with wtimeout() if we don't want ctrl+z etc. to interrupt
  // the program.
  raw();
  noecho();                // Don't echo keyboard input
  keypad(stdscr, TRUE);    // Also receive arrow keys, etc.
  curs_set(0);             // Hide the terminal cursor
  set_escdelay(1);         // Short delay before triggering escape
  term_util_init_colors(); // Our color init routine
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
  if (has_mouse()) // no waiting for distinguishing click from press
    mouseinterval(0);
  printf("\033[?2004h\n"); // Ask terminal to use bracketed paste.

  tui_load_conf(&t);                  // load orca.conf (if it exists)
  tui_apply_overrides(&t);            // then the flags, which beat it
  tui_restart_osc_udp_if_enabled(&t); // start udp if conf enabled it

  wtimeout(stdscr, 0);
  int cur_timeout = 0;
#ifdef ZORCA_DEBUG_COUNTERS
  // The wakeup counter: the event loop's waits, the wgetch calls,
  // counted apart by whether they enter with a timeout above 0 or with 0,
  // from the start of the loop. Printed to stderr after endwin.
  // tests/wakeups/ compares it with a counter-only build of 4f349cd.
  Usz debug_waits = 0, debug_zero_waits = 0;
  U64 debug_waits_since = stm_now();
#endif
  Usz brackpaste_starting_x = 0, brackpaste_y = 0, brackpaste_x = 0,
      brackpaste_max_y = 0, brackpaste_max_x = 0;
  bool is_in_brackpaste = false;

  WINDOW *cont_window = NULL;
  tui_adjust_term_size(&t, &cont_window);

  bool startup_no_memory = false;
  bool grid_initialized = false;
  if (osolen(t.file_name)) {
    Field_load_error fle = field_load_file(osoc(t.file_name), &t.ged.field);
    switch (fle) {
    case Field_load_error_ok:
      if (t.ged.field.height < 1 || t.ged.field.width < 1) {
        // Opening an empty file or attempting to open a directory can lead us
        // here.
        qmsg_printf_push("Unusable File", "Not a usable file:\n%s",
                         (osoc(t.file_name)));
        break;
      }
      grid_initialized = true;
      break;
    case Field_load_error_cant_open_file: {
      // Probably a new file, though TODO we should add an explicit
      // differentiation between "file exists and can't open it" and "file
      // doesn't seem to exist."
      Qmsg *qm = qmsg_printf_push(NULL, "New file: %s", osoc(t.file_name));
      qmsg_set_dismiss_mode(qm, Qmsg_dismiss_mode_passthrough);
      break;
    }
    case Field_load_error_no_memory:
      // Quit rather than start on a blank grid, which a save would write
      // over the file. ged_deinit frees the partial grid.
      startup_no_memory = true;
      goto quit;
    default:
      qmsg_printf_push("File Load Error", "File load error:\n%s.",
                       field_load_error_string(fle));
      break;
    }
  }
  // If we haven't yet initialized the grid, because we were waiting for the
  // terminal size, do it now.
  if (!grid_initialized) {
    Usz new_field_h, new_field_w;
    if (explicit_initial_grid_size ||
        !tui_suggest_nice_grid_size(&t, t.ged.win_h, t.ged.win_w, &new_field_h,
                                    &new_field_w)) {
      new_field_h = (Usz)init_grid_dim_y;
      new_field_w = (Usz)init_grid_dim_x;
    }
    // A load that failed mid-file leaves a partial grid: free it before the
    // fill (a field that holds none has a NULL buffer).
    field_deinit(&t.ged.field);
    if (!field_init_fill(&t.ged.field, (Usz)new_field_h, (Usz)new_field_w,
                         '.')) {
      startup_no_memory = true;
      goto quit;
    }
  }
  // With no mark buffer there is no grid to run: quit before the clock
  // source, the startup /orca/bpm and auto-play, and say why after endwin.
  startup_no_memory = !mbuf_reusable_ensure_size(
      &t.ged.mbuf_r, t.ged.field.height, t.ged.field.width);
  if (startup_no_memory)
    goto quit;
  ged_make_cursor_visible(&t.ged);
  // The clock source and its input, once: after the overrides, before
  // auto-play. Under External the startup /orca/bpm is skipped (the
  // estimate's shown value goes out when it becomes valid) and so is
  // auto-play: the master starts playback.
#ifdef ZORCA_HAS_MIDI_IN
  tui_apply_clock_source(&t);
#endif
  bool const startup_external = t.ged.clock_source == Clock_source_external;
  if (!startup_external)
    ged_send_osc_bpm(&t.ged, (I32)t.ged.bpm); // Send initial BPM
#ifdef FEAT_ALSA
  // A shutdown signal before auto-play quits without starting playback.
  if (alsa_shutdown_requested)
    goto quit;
#endif
  if (!startup_external)
    ged_set_playing(&t.ged, true); // Auto-play
#ifdef ZORCA_DEBUG_COUNTERS
  debug_waits_since = stm_now();
#endif
  // Enter main loop. Process events as they arrive.
event_loop:;
#ifdef FEAT_ALSA
  if (alsa_shutdown_requested)
    goto quit;
#endif
#ifdef ZORCA_DEBUG_COUNTERS
  // Under External with ALSA the wait is the poll() below, counted there.
#ifdef FEAT_ALSA
  if (t.ged.clock_source != Clock_source_external)
#endif
  {
    if (cur_timeout > 0)
      ++debug_waits;
    else
      ++debug_zero_waits;
  }
#endif
  int key = wgetch(stdscr);
  if (cur_timeout != 0) {
    wtimeout(stdscr, 0); // Until we run out, don't wait between events.
    cur_timeout = 0;
  }
  switch (key) {
  case ERR: { // ERR indicates no more events.
#ifdef ZORCA_HAS_MIDI_IN
    // Under External: every queued input event is drained
    // first, each stamped at read time, and only then the machine's
    // timeouts are polled, with a fresh now read after the drain, so a wait
    // that ends with an F8 already queued never false-fires clock lost. On
    // Internal nothing here runs: that path is 4f349cd's.
    bool const external = t.ged.clock_source == Clock_source_external;
    if (external) {
      tui_input_drain(&t);
      ged_transport_poll(&t.ged, now_us());
    }
#endif
    ged_do_stuff(&t.ged);
#ifdef FEAT_ALSA
    // After the tick, so a recovery burst never goes ahead of one that is due.
    ged_retry_note_offs(&t.ged);
#endif
    bool drew_any = false;
    if (ged_is_draw_dirty(&t.ged) || qnav_stack.occlusion_dirty) {
      werase(cont_window);
      ged_draw(&t.ged, cont_window, osoc(t.file_name), t.prefs.fancy_grid_dots,
               t.prefs.fancy_grid_rulers);
      wnoutrefresh(cont_window);
      drew_any = true;
    }
    drew_any |= qnav_draw(); // clears qnav_stack.occlusion_dirty
    if (drew_any)
      doupdate();
    double secs_to_d = ged_secs_to_deadline(&t.ged);
#define DEADTIME(_millisecs, _new_timeout)                                     \
  else if (secs_to_d < ms_to_sec(_millisecs)) new_timeout = _new_timeout;
    int new_timeout;
    // These values are tuned to work OK with the normal scheduling behavior
    // on Linux, Mac, and Windows. All of the usual caveats of trying to
    // guess what the scheduler will do apply.
    //
    // Of course, there's no guarantee about how the scheduler will work, so
    // if you are using a modified kernel or something, or the measurements
    // here are bad, or it's some OS that behaves differently than expected,
    // this won't be very good. But there's not really much we can do about
    // it, and it's better than doing nothing and burning up the CPU!
    if (t.strict_timing) { // clang-format off
      if (false) {}
      // "If there's less than 1.5 milliseconds to the deadline, use a curses
      // timeout value of 0."
      DEADTIME(  1.5,  0)
      DEADTIME(  3.0,  1)
      DEADTIME(  5.0,  2)
      DEADTIME(  7.0,  3)
      DEADTIME(  9.0,  4)
      DEADTIME( 11.0,  5)
      DEADTIME( 13.0,  6)
      DEADTIME( 15.0,  7)
      DEADTIME( 25.0, 12)
      DEADTIME( 50.0, 20)
      DEADTIME(100.0, 40)
      else new_timeout = 50;
    } else {
      if (false) {}
      DEADTIME(  1.0,  0)
      DEADTIME(  2.0,  1)
      DEADTIME(  7.0,  2)
      DEADTIME( 15.0,  5)
      DEADTIME( 25.0, 10)
      DEADTIME( 50.0, 20)
      DEADTIME(100.0, 40)
      else new_timeout = 50;
    } // clang-format on
#undef DEADTIME
#ifdef FEAT_ALSA
    // A refused note-off is held: wake when its next try is due (2 ms after
    // the refusal, then at growing intervals up to 100 ms), not at the idle
    // ceiling, and never with a zero timeout. Only while one is held; the
    // usual path is unchanged.
    {
      U64 wait_us;
      if (ged_note_off_wait_us(&t.ged, &wait_us)) {
        U64 wait_ms = wait_us / 1000 + (wait_us % 1000 != 0); // rounded up
        if (wait_ms < 1)
          wait_ms = 1;
        if (wait_ms < (U64)new_timeout)
          new_timeout = (int)wait_ms;
      }
    }
#endif
    // The CC engine's deadline, only while a glide runs, so
    // with none this is 4f349cd's path: no clock read, no call. It never
    // goes through the table above: when it has passed, the sub-tick poll
    // sends what is due, and the next one shortens the wait, rounded up to
    // whole milliseconds and never below 1, so it never asks for a
    // zero-timeout wait.
    if (t.ged.ccout.active_count > 0) {
      U64 now = now_us();
      U64 deadline;
      bool engine_busy = ccout_next_deadline(&t.ged.ccout, now, &deadline);
      if (engine_busy && deadline <= now) {
        ged_poll_ccout(&t.ged, now);
        engine_busy = ccout_next_deadline(&t.ged.ccout, now, &deadline);
      }
      if (engine_busy) {
        U64 wait_us = deadline - now; // never negative: deadline >= now
        U64 wait_ms = wait_us / 1000;
        if (wait_us % 1000 != 0)
          ++wait_ms; // rounded up
        if (wait_ms < 1)
          wait_ms = 1;
        if (wait_ms < (U64)new_timeout)
          new_timeout = (int)wait_ms;
      }
    }
#ifdef ZORCA_HAS_MIDI_IN
    if (external) {
      // The transport machine's deadline (clock lost, or the idle drop of
      // the estimate), a sibling of the engine's: the tick deadline is
      // absent under External, so the table gave its 50 ms ceiling, which
      // stays the cap. A deadline at or before now counts as 1 ms.
      U64 now = now_us();
      U64 deadline;
      if (transport_next_deadline(&t.ged.transport, &deadline)) {
        U64 wait_ms = 1;
        if (deadline > now) {
          U64 wait_us = deadline - now;
          wait_ms = wait_us / 1000;
          if (wait_us % 1000 != 0)
            ++wait_ms; // rounded up
          if (wait_ms < 1)
            wait_ms = 1;
        }
        if (wait_ms < (U64)new_timeout)
          new_timeout = (int)wait_ms;
      }
#ifdef FEAT_PORTMIDI
      // PortMidi has no descriptor to wait on: while a stream is open the
      // input is read each pass; with none there is nothing to read.
      if (t.midi_in_stream && new_timeout > 1)
        new_timeout = 1;
#endif
#ifdef FEAT_ALSA
      // The wait is a poll() over stdin and the sequencer's descriptors;
      // curses stays at timeout 0. EINTR (a shutdown signal) and any other
      // error go back to event_loop, as a timeout does, where the shutdown
      // flag is checked.
      {
        enum { Alsa_poll_max = 4 };
        struct pollfd fds[1 + Alsa_poll_max];
        fds[0] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        int n = snd_seq_poll_descriptors(t.alsa->seq, fds + 1, Alsa_poll_max,
                                         POLLIN);
        if (n < 0)
          n = 0;
#ifdef ZORCA_DEBUG_COUNTERS
        if (new_timeout > 0)
          ++debug_waits;
        else
          ++debug_zero_waits;
#endif
        (void)poll(fds, (nfds_t)(1 + n), new_timeout);
      }
      goto event_loop;
#endif
    }
#endif
    if (new_timeout != cur_timeout) {
      wtimeout(stdscr, new_timeout);
      cur_timeout = new_timeout;
#if TIME_DEBUG
      spin_track_timeout = cur_timeout;
#endif
    }
    goto event_loop;
  }
  case KEY_RESIZE:
    tui_adjust_term_size(&t, &cont_window);
    qnav_adjust_term_size();
    goto event_loop;
#ifndef FEAT_NOMOUSE
  case KEY_MOUSE: {
    MEVENT mevent;
    if (cont_window && getmouse(&mevent) == OK) {
      int win_y, win_x;
      int win_h, win_w;
      getbegyx(cont_window, win_y, win_x);
      getmaxyx(cont_window, win_h, win_w);
      int inwin_y = mevent.y - win_y;
      int inwin_x = mevent.x - win_x;
      if (inwin_y >= win_h)
        inwin_y = win_h - 1;
      if (inwin_y < 0)
        inwin_y = 0;
      if (inwin_x >= win_w)
        inwin_x = win_w - 1;
      if (inwin_x < 0)
        inwin_x = 0;
      ged_mouse_event(&t.ged, (Usz)inwin_y, (Usz)inwin_x, mevent.bstate);
    }
    goto event_loop;
  }
#endif
  }

  // If we have the menus open, we'll let the menus do what they want with
  // the input before the regular editor (which will be displayed
  // underneath.) The menus may tell us to quit, that they didn't do anything
  // with the input, or that they consumed the input and therefore we
  // shouldn't pass the input key to the rest of the editing system.
  switch (tui_drive_menus(&t, key)) {
  case Tui_menus_nothing:
    break;
  case Tui_menus_quit:
    goto quit;
  case Tui_menus_consumed_input:
    goto event_loop;
  }

  // If this key input is intended to reach the grid, check to see if we're
  // in bracketed paste and use alternate 'filtered input for characters'
  // mode. We'll ignore most control sequences here.
  if (is_in_brackpaste) {
    if (key == 27 /* escape */) {
      if (brackpaste_seq_getungetch(stdscr) == Brackpaste_seq_end) {
        is_in_brackpaste = false;
        if (brackpaste_max_y > t.ged.ged_cursor.y)
          t.ged.ged_cursor.h = brackpaste_max_y - t.ged.ged_cursor.y + 1;
        if (brackpaste_max_x > t.ged.ged_cursor.x)
          t.ged.ged_cursor.w = brackpaste_max_x - t.ged.ged_cursor.x + 1;
        t.ged.needs_remarking = true;
        t.ged.is_draw_dirty = true;
      }
      goto event_loop;
    }
    if (key == KEY_ENTER)
      key = '\r';
    if (key >= CHAR_MIN && key <= CHAR_MAX) {
      if ((char)key == '\r' || (char)key == '\n') {
        brackpaste_x = brackpaste_starting_x;
        ++brackpaste_y;
        goto event_loop;
      }
      if (key != ' ') {
        char cleaned = (char)key;
        if (!orca_is_valid_glyph((Glyph)key))
          cleaned = '.';
        if (brackpaste_y < t.ged.field.height &&
            brackpaste_x < t.ged.field.width) {
          gbuffer_poke(t.ged.field.buffer, t.ged.field.height,
                       t.ged.field.width, brackpaste_y, brackpaste_x, cleaned);
          // Could move this out one level if we wanted the final selection
          // size to reflect even the pasted area which didn't fit on the
          // grid.
          if (brackpaste_y > brackpaste_max_y)
            brackpaste_max_y = brackpaste_y;
          if (brackpaste_x > brackpaste_max_x)
            brackpaste_max_x = brackpaste_x;
        }
      }
      ++brackpaste_x;
    }
    goto event_loop;
  }

  // Regular inputs when we're not in a menu and not in bracketed paste.
  switch (key) {
  // Checking again for 'quit' here, because it's only listened for if we're
  // in the menus or *not* in bracketed paste mode.
  case CTRL_PLUS('q'):
    goto quit;
  case CTRL_PLUS('o'):
    push_open_form(osoc(t.file_name));
    break;
  case 127: // backspace in terminal.app, apparently
  case KEY_BACKSPACE:
    if (t.ged.input_mode == Ged_input_mode_append) {
      ged_dir_input(&t.ged, Ged_dir_left, 1);
      ged_input_character(&t.ged, '.');
      ged_dir_input(&t.ged, Ged_dir_left, 1);
    } else {
      ged_input_character(&t.ged, '.');
    }
    break;
  case CTRL_PLUS('z'):
  case CTRL_PLUS('u'):
    ged_input_cmd(&t.ged, Ged_input_cmd_undo);
    break;
  case CTRL_PLUS('r'):
    t.ged.tick_num = 0;
    t.ged.needs_remarking = true;
    t.ged.is_draw_dirty = true;
    opstate_clear(&t.ged.opstate); // every &, ; and r starts over
    break;
  case '[':
    ged_adjust_rulers_relative(&t.ged, 0, -1);
    break;
  case ']':
    ged_adjust_rulers_relative(&t.ged, 0, 1);
    break;
  case '{':
    ged_adjust_rulers_relative(&t.ged, -1, 0);
    break;
  case '}':
    ged_adjust_rulers_relative(&t.ged, 1, 0);
    break;
  case '(':
    ged_resize_grid_relative(&t.ged, 0, -1);
    break;
  case ')':
    ged_resize_grid_relative(&t.ged, 0, 1);
    break;
  case '_':
    ged_resize_grid_relative(&t.ged, -1, 0);
    break;
  case '+':
    ged_resize_grid_relative(&t.ged, 1, 0);
    break;
  case '\r':
  case KEY_ENTER:
    break; // Currently unused.
  case CTRL_PLUS('i'):
  case KEY_IC:
    ged_input_cmd(&t.ged, Ged_input_cmd_toggle_append_mode);
    break;
  case '/':
    // Formerly 'piano'/trigger mode toggle. We're repurposing it here to
    // input a '?' instead of a '/' because '?' opens the help guide, and it
    // might be a bad idea to take that away, since orca will take over the
    // TTY and may leave users confused. I know of at least 1 person who was
    // saved by pressing '?' after they didn't know what to do. Hmm.
    ged_input_character(&t.ged, '?');
    break;
  case '<':
    // Under External the tempo is the master's: < and > are ignored.
    if (t.ged.clock_source == Clock_source_internal)
      ged_adjust_bpm(&t.ged, -1);
    break;
  case '>':
    if (t.ged.clock_source == Clock_source_internal)
      ged_adjust_bpm(&t.ged, 1);
    break;
  case CTRL_PLUS('f'):
    ged_input_cmd(&t.ged, Ged_input_cmd_step_forward);
    break;
  case CTRL_PLUS('e'):
    ged_input_cmd(&t.ged, Ged_input_cmd_toggle_show_event_list);
    break;
  case CTRL_PLUS('x'):
    if (ged_input_cmd(&t.ged, Ged_input_cmd_cut))
      try_send_to_gui_clipboard(&t.ged, &t.use_gui_cboard);
    break;
  case CTRL_PLUS('c'):
    if (ged_input_cmd(&t.ged, Ged_input_cmd_copy))
      try_send_to_gui_clipboard(&t.ged, &t.use_gui_cboard);
    break;
  case CTRL_PLUS('v'):
    if (t.use_gui_cboard) {
      bool added_hist =
          undo_history_push(&t.ged.undo_hist, &t.ged.field, t.ged.tick_num);
      Usz pasted_h, pasted_w;
      Cboard_error cberr = cboard_paste(
          t.ged.field.buffer, t.ged.field.height, t.ged.field.width,
          t.ged.ged_cursor.y, t.ged.ged_cursor.x, &pasted_h, &pasted_w);
      if (cberr) {
        if (added_hist)
          undo_history_pop(&t.ged.undo_hist, &t.ged.field, &t.ged.tick_num);
        t.use_gui_cboard = false;
        ged_input_cmd(&t.ged, Ged_input_cmd_paste);
      } else {
        if (pasted_h > 0 && pasted_w > 0) {
          t.ged.ged_cursor.h = pasted_h;
          t.ged.ged_cursor.w = pasted_w;
        }
      }
      t.ged.needs_remarking = true;
      t.ged.is_draw_dirty = true;
    } else {
      ged_input_cmd(&t.ged, Ged_input_cmd_paste);
    }
    break;
  case '\'':
    ged_input_cmd(&t.ged, Ged_input_cmd_toggle_selresize_mode);
    break;
  case '`':
  case '~':
    ged_input_cmd(&t.ged, Ged_input_cmd_toggle_slide_mode);
    break;
  case ' ':
    if (t.ged.input_mode == Ged_input_mode_append)
      ged_input_character(&t.ged, '.');
    else if (t.ged.clock_source == Clock_source_internal)
      ged_input_cmd(&t.ged, Ged_input_cmd_toggle_play_pause);
    // Under External the master runs playback: Space is ignored.
    break;
  case 27: // Escape
    // Check for escape sequences we're interested in that ncurses didn't
    // handle. Such as bracketed paste.
    if (brackpaste_seq_getungetch(stdscr) == Brackpaste_seq_begin) {
      is_in_brackpaste = true;
      undo_history_push(&t.ged.undo_hist, &t.ged.field, t.ged.tick_num);
      brackpaste_y = t.ged.ged_cursor.y;
      brackpaste_x = t.ged.ged_cursor.x;
      brackpaste_starting_x = brackpaste_x;
      brackpaste_max_y = brackpaste_y;
      brackpaste_max_x = brackpaste_x;
      break;
    }
    ged_input_cmd(&t.ged, Ged_input_cmd_escape);
    break;

  case 330: // delete?
    ged_input_character(&t.ged, '.');
    break;

  // Cursor movement
  case KEY_UP:
  case CTRL_PLUS('k'):
    ged_dir_input(&t.ged, Ged_dir_up, 1);
    break;
  case CTRL_PLUS('j'):
  case KEY_DOWN:
    ged_dir_input(&t.ged, Ged_dir_down, 1);
    break;
  case CTRL_PLUS('h'):
  case KEY_LEFT:
    ged_dir_input(&t.ged, Ged_dir_left, 1);
    break;
  case CTRL_PLUS('l'):
  case KEY_RIGHT:
    ged_dir_input(&t.ged, Ged_dir_right, 1);
    break;

  // Selection size modification. These may not work in all terminals. (Only
  // tested in xterm so far.)
  case 337: // shift-up
    ged_modify_selection_size(&t.ged, -1, 0);
    break;
  case 336: // shift-down
    ged_modify_selection_size(&t.ged, 1, 0);
    break;
  case 393: // shift-left
    ged_modify_selection_size(&t.ged, 0, -1);
    break;
  case 402: // shift-right
    ged_modify_selection_size(&t.ged, 0, 1);
    break;
  case 567: // shift-control-up
    ged_modify_selection_size(&t.ged, -(int)t.ged.ruler_spacing_y, 0);
    break;
  case 526: // shift-control-down
    ged_modify_selection_size(&t.ged, (int)t.ged.ruler_spacing_y, 0);
    break;
  case 546: // shift-control-left
    ged_modify_selection_size(&t.ged, 0, -(int)t.ged.ruler_spacing_x);
    break;
  case 561: // shift-control-right
    ged_modify_selection_size(&t.ged, 0, (int)t.ged.ruler_spacing_x);
    break;

  // Move cursor further if control is held
  case 566: // control-up
    ged_dir_input(&t.ged, Ged_dir_up, (int)t.ged.ruler_spacing_y);
    break;
  case 525: // control-down
    ged_dir_input(&t.ged, Ged_dir_down, (int)t.ged.ruler_spacing_y);
    break;
  case 545: // control-left
    ged_dir_input(&t.ged, Ged_dir_left, (int)t.ged.ruler_spacing_x);
    break;
  case 560: // control-right
    ged_dir_input(&t.ged, Ged_dir_right, (int)t.ged.ruler_spacing_x);
    break;

  // Slide selection on alt-arrow
  case 564: // alt-up
    ged_slide_selection(&t.ged, -1, 0);
    break;
  case 523: // alt-down
    ged_slide_selection(&t.ged, 1, 0);
    break;
  case 543: // alt-left
    ged_slide_selection(&t.ged, 0, -1);
    break;
  case 558: // alt-right
    ged_slide_selection(&t.ged, 0, 1);
    break;

  case CTRL_PLUS('d'):
  case KEY_F(1):
    push_main_menu();
    break;
  case '?':
    push_controls_msg();
    break;
  case CTRL_PLUS('g'):
    push_opers_guide_msg(t.ged.dialect);
    break;
  case CTRL_PLUS('s'):
    tui_try_save(&t);
    break;

  default:
    if (key >= CHAR_MIN && key <= CHAR_MAX && orca_is_valid_glyph((Glyph)key))
      ged_input_character(&t.ged, (char)key);
#if 0
      else
        fprintf(stderr, "Unknown key number: %d\n", key);
#endif
    break;
  }
  goto event_loop;
quit:
  // Stop playback: note-offs, OSC /orca/stopped when OSC output is set, and
  // MIDI stop (FC) when clock-out is active.
  ged_stop(&t.ged);
#ifdef FEAT_ALSA
  ged_flush_note_offs(&t.ged, 50000); // the stop's refused note-offs, 50 ms
#endif
#ifdef ZORCA_HAS_MIDI_IN
  tui_input_close(&t); // the PortMidi input before Pm_Terminate
#endif
  qnav_deinit();
  if (cont_window)
    delwin(cont_window);
#ifndef FEAT_NOMOUSE
  printf("\033[?1003l\n"); // turn off console mouse events if they were active
#endif
  printf("\033[?2004h\n"); // Tell terminal to not use bracketed paste
  endwin();
#ifdef FEAT_ALSA
  snd_lib_error_set_handler(NULL); // libasound's default handler again
#endif
#if defined(FEAT_ALSA) && defined(ZORCA_DEBUG_COUNTERS)
  fprintf(stderr,
          "zOrca ALSA: %zu MIDI sends dropped on EAGAIN/EINTR, %zu not sent "
          "in all; %zu note-offs held for a retry, %zu sent again, %zu "
          "given up\n",
          alsa_ctx.send_drops, alsa_ctx.send_failures,
          alsa_ctx.pending_offs.held, alsa_ctx.pending_offs.resent,
          alsa_ctx.pending_offs.lost);
#endif
#if defined(ZORCA_DEBUG_COUNTERS) && defined(ZORCA_HAS_MIDI_IN)
  fprintf(stderr, "zOrca MIDI in: %zu overflows, %zu read errors\n",
          t.midi_in_overflows, t.midi_in_errors);
#endif
#if defined(FEAT_ALSA) && defined(ZORCA_DEBUG_COUNTERS)
  fprintf(stderr, "zOrca ALSA input pool: %ld events (asked for %d)\n",
          alsa_ctx.input_pool, (int)Alsa_input_pool);
#endif
#ifdef ZORCA_DEBUG_COUNTERS
  char events_line[160];
  events_dropped_format(events_line, sizeof events_line,
                        t.ged.oevent_list.dropped,
                        t.ged.scratch_oevent_list.dropped,
                        t.ged.engine_oevent_list.dropped);
  fputs(events_line, stderr);
  fprintf(stderr, "zOrca wakeups: %zu waits, %zu zero-timeout in %.3f s\n",
          debug_waits, debug_zero_waits,
          stm_sec(stm_since(debug_waits_since)));
#endif
  ged_deinit(&t.ged);
#ifdef FEAT_ALSA
  alsa_ctx_close(&alsa_ctx); // after ged_deinit, which frees the encoder
#endif
  osofree(t.file_name);
  osofree(t.osc_midi_bidule_path);
#ifdef FEAT_PORTMIDI
  if (portmidi_is_initialized)
    Pm_Terminate();
#endif
  if (startup_no_memory) {
    fputs("Cannot start: not enough memory for the grid\n", stderr);
    return 1;
  }
}

#undef staticni
