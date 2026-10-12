#include "tui_prefs.h"
#include "ccout.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// The orca.conf settings (P3-4b, spine AD-20). Testable shell: no I/O, no
// clock, no writable statics, and only the core include allow-list, so the
// unit runner links this file (spine AD-1, AD-2, AD-21). See tui_prefs.h.

#define CONFOPT_STRING(x) #x,
char const *const confopts[Confoptslen] = {TUI_PREFS_KEYS(CONFOPT_STRING)};
#undef CONFOPT_STRING

static char const *const prefval_plain = "plain";
static char const *const prefval_fancy = "fancy";

// "1", "true" and "yes" are true; "0", "false" and "no" are false.
static bool read_boolish(char const *value, bool *out) {
  static char const *const trues[] = {"1", "true", "yes"};
  static char const *const falses[] = {"0", "false", "no"};
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(trues); ++i) {
    if (strcmp(value, trues[i]) == 0) {
      *out = true;
      return true;
    }
  }
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(falses); ++i) {
    if (strcmp(value, falses[i]) == 0) {
      *out = false;
      return true;
    }
  }
  return false;
}

// "plain" is false and "fancy" true.
static bool read_plain_or_fancy(char const *value, bool *out_fancy) {
  if (strcmp(value, prefval_plain) == 0) {
    *out_fancy = false;
    return true;
  }
  if (strcmp(value, prefval_fancy) == 0) {
    *out_fancy = true;
    return true;
  }
  return false;
}

// Whether a string fits a Tui_prefs_value_cap array with its terminator.
static bool fits(char const *s) { return strlen(s) < Tui_prefs_value_cap; }

// Copies s, which fits, into a Tui_prefs_value_cap array.
static void put_value(char *dst, char const *s) {
  memcpy(dst, s, strlen(s) + 1);
}

// One number as %d reads it: leading blanks, a sign, then digits. False
// when there is none, or it is outside int; otherwise *end is just past it.
static bool read_int_prefix(char const *s, int *out, char const **end) {
  char *e;
  errno = 0;
  long v = strtol(s, &e, 10);
  // Outside int is seen two ways: compared as long long where long is wider
  // than int, and by ERANGE where long is int (armhf), since strtol then
  // clamps an overflow to LONG_MAX == INT_MAX, which the compare passes.
  long long w = v;
  if (e == s || errno == ERANGE || w < INT_MIN || w > INT_MAX)
    return false;
  *out = (int)v;
  *end = e;
  return true;
}

bool tui_prefs_read_nxn_or_n(char const *str, int *out_a, int *out_b) {
  int a, b;
  char const *end;
  if (!read_int_prefix(str, &a, &end))
    return false;
  if (*end != 'x' || !read_int_prefix(end + 1, &b, &end))
    b = a;
  *out_a = a;
  *out_b = b;
  return true;
}

// Writes v in decimal at buf + at, in place of snprintf (no <stdio.h> here,
// as tooltips.c's put_text_uint), and returns the index just past it.
static Usz put_int(char *buf, Usz at, int v) {
  unsigned u = (unsigned)v;
  if (v < 0) {
    buf[at++] = '-';
    u = 0u - u;
  }
  char digits[sizeof(unsigned) * 3 + 1];
  Usz n = 0;
  do {
    digits[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u > 0);
  while (n > 0)
    buf[at++] = digits[--n];
  return at;
}

void tui_prefs_init(Tui_prefs *p, bool has_midi_in, Confopt midi_in_key) {
  p->portmidi_output_device[0] = '\0';
  p->osc_output_address[0] = '\0';
  put_value(p->osc_output_port, "49162");
  p->midi_in[0] = '\0';
  p->softmargin_y = 1;
  p->softmargin_x = 2;
  p->dialect = Orca_dialect_borca;
  p->glide_hz = Ccout_glide_hz_default;
  p->clock_source = Clock_source_internal;
  p->touched = 0;
  p->midi_in_key = has_midi_in ? midi_in_key : Confoptslen;
  p->osc_output_enabled = false;
  p->midi_bclock = false;
  p->fancy_grid_dots = true;
  p->fancy_grid_rulers = true;
  p->has_midi_in = has_midi_in;
}

bool tui_prefs_read_line(Tui_prefs *p, Confopt key, char const *value) {
  if (!fits(value))
    return false; // ezconf's buffer never holds one
  switch (key) {
  case Confopt_portmidi_output_device:
    // Stored, not touched: its save is the shell's PortMidi device name.
    put_value(p->portmidi_output_device, value);
    return true;
  case Confopt_osc_output_address:
    put_value(p->osc_output_address, value); // empty is loopback
    break;
  case Confopt_osc_output_port:
    put_value(p->osc_output_port, value);
    break;
  case Confopt_osc_output_enabled:
    if (!read_boolish(value, &p->osc_output_enabled))
      return false;
    break;
  case Confopt_midi_beat_clock:
    if (!read_boolish(value, &p->midi_bclock))
      return false;
    break;
  case Confopt_margins: {
    int x, y;
    if (!tui_prefs_read_nxn_or_n(value, &x, &y) || x < 0 || y < 0)
      return false;
    p->softmargin_x = x;
    p->softmargin_y = y;
    break;
  }
  case Confopt_grid_dot_type:
    if (!read_plain_or_fancy(value, &p->fancy_grid_dots))
      return false;
    break;
  case Confopt_grid_ruler_type:
    if (!read_plain_or_fancy(value, &p->fancy_grid_rulers))
      return false;
    break;
  case Confopt_dialect:
    if (!orca_dialect_from_name(value, &p->dialect))
      return false;
    break;
  case Confopt_glide_hz:
    // 50, 100 or 200; anything else is kept verbatim until the Glide Rate
    // menu sets a rate.
    if (!ccout_glide_hz_from_name(value, &p->glide_hz))
      return false;
    break;
  case Confopt_clock_source: {
    // External in a build with no input reader is refused and its line
    // kept, which leaves Internal (AD-20).
    Clock_source source;
    if (!clock_source_from_name(value, &source) ||
        (source == Clock_source_external && !p->has_midi_in))
      return false;
    p->clock_source = source;
    break;
  }
  case Confopt_portmidi_input_device:
  case Confopt_alsa_input_device:
    // Parsed only in a build with its backend, and stored, not touched: the
    // line is kept until the MIDI Input menu changes it. The other
    // backend's key is refused, and its line kept.
    if (!p->has_midi_in || key != p->midi_in_key)
      return false;
    put_value(p->midi_in, value);
    return true;
  default:
    return false;
  }
  p->touched |= TOUCHFLAG(key);
  return true;
}

void tui_prefs_apply_overrides(Tui_prefs *p, Tui_overrides const *o) {
  // A key with no branch here would have its bit cleared below and keep the
  // conf's value: a new override adds a branch and joins Tui_overrides_keys.
  assert((o->keys & ~(U32)Tui_overrides_keys) == 0);
  if (o->keys & TOUCHFLAG(Confopt_dialect))
    p->dialect = o->dialect;
  if ((o->keys & TOUCHFLAG(Confopt_clock_source)) &&
      (o->clock_source != Clock_source_external || p->has_midi_in))
    p->clock_source = o->clock_source;
  // The override's array fits when it holds a terminator (main() refuses a
  // --midi-in that does not, so this is belt and braces): a bounded scan,
  // never strlen over an array that may hold none.
  if (p->has_midi_in && (o->keys & TOUCHFLAG(p->midi_in_key)) &&
      memchr(o->midi_in, '\0', sizeof o->midi_in))
    put_value(p->midi_in, o->midi_in);
  p->touched &= ~o->keys;
}

bool tui_prefs_set_dialect(Tui_prefs *p, Orca_dialect dialect) {
  if (dialect == p->dialect)
    return false;
  p->dialect = dialect;
  p->touched |= TOUCHFLAG(Confopt_dialect);
  return true;
}

bool tui_prefs_set_glide_hz(Tui_prefs *p, Usz glide_hz) {
  if (glide_hz == p->glide_hz || !ccout_glide_hz_name(glide_hz))
    return false;
  p->glide_hz = glide_hz;
  p->touched |= TOUCHFLAG(Confopt_glide_hz);
  return true;
}

bool tui_prefs_set_clock_source(Tui_prefs *p, Clock_source source) {
  if (source == p->clock_source ||
      (source == Clock_source_external && !p->has_midi_in))
    return false;
  p->clock_source = source;
  p->touched |= TOUCHFLAG(Confopt_clock_source);
  return true;
}

bool tui_prefs_set_midi_in(Tui_prefs *p, char const *name) {
  if (!p->has_midi_in || name[0] == '\0' || !fits(name) ||
      strcmp(name, p->midi_in) == 0)
    return false;
  put_value(p->midi_in, name);
  p->touched |= TOUCHFLAG(p->midi_in_key);
  return true;
}

bool tui_prefs_set_grid_dots(Tui_prefs *p, bool fancy) {
  if (fancy == p->fancy_grid_dots)
    return false;
  p->fancy_grid_dots = fancy;
  p->touched |= TOUCHFLAG(Confopt_grid_dot_type);
  return true;
}

bool tui_prefs_set_grid_rulers(Tui_prefs *p, bool fancy) {
  if (fancy == p->fancy_grid_rulers)
    return false;
  p->fancy_grid_rulers = fancy;
  p->touched |= TOUCHFLAG(Confopt_grid_ruler_type);
  return true;
}

bool tui_prefs_set_osc_output_enabled(Tui_prefs *p, bool enabled) {
  if (enabled == p->osc_output_enabled)
    return false;
  p->osc_output_enabled = enabled;
  p->touched |= TOUCHFLAG(Confopt_osc_output_enabled);
  return true;
}

bool tui_prefs_set_midi_bclock(Tui_prefs *p, bool enabled) {
  if (enabled == p->midi_bclock)
    return false;
  p->midi_bclock = enabled;
  p->touched |= TOUCHFLAG(Confopt_midi_beat_clock);
  return true;
}

bool tui_prefs_set_margins(Tui_prefs *p, int x, int y) {
  if (x < 0 || y < 0 || (x == p->softmargin_x && y == p->softmargin_y))
    return false;
  p->softmargin_x = x;
  p->softmargin_y = y;
  p->touched |= TOUCHFLAG(Confopt_margins);
  return true;
}

bool tui_prefs_set_osc_output_address(Tui_prefs *p, char const *address) {
  if (!fits(address) || strcmp(address, p->osc_output_address) == 0)
    return false;
  put_value(p->osc_output_address, address);
  p->touched |= TOUCHFLAG(Confopt_osc_output_address);
  return true;
}

bool tui_prefs_set_osc_output_port(Tui_prefs *p, char const *port) {
  if (port[0] == '\0' || !fits(port) ||
      strcmp(port, p->osc_output_port) == 0)
    return false;
  put_value(p->osc_output_port, port);
  p->touched |= TOUCHFLAG(Confopt_osc_output_port);
  return true;
}

char const *tui_prefs_saved_value(Tui_prefs const *p, Confopt key, char *buf) {
  switch (key) {
  case Confopt_portmidi_output_device:
    return p->portmidi_output_device;
  case Confopt_osc_output_address:
    return p->osc_output_address;
  case Confopt_osc_output_port:
    return p->osc_output_port;
  case Confopt_osc_output_enabled:
    return p->osc_output_enabled ? "1" : "0";
  case Confopt_midi_beat_clock:
    return p->midi_bclock ? "1" : "0";
  case Confopt_margins: {
    Usz at = put_int(buf, 0, p->softmargin_x);
    buf[at++] = 'x';
    at = put_int(buf, at, p->softmargin_y);
    buf[at] = '\0';
    return buf;
  }
  case Confopt_grid_dot_type:
    return p->fancy_grid_dots ? prefval_fancy : prefval_plain;
  case Confopt_grid_ruler_type:
    return p->fancy_grid_rulers ? prefval_fancy : prefval_plain;
  case Confopt_dialect:
    return orca_dialect_name(p->dialect);
  case Confopt_glide_hz: {
    // Only the three names: a rate the setters and reads never store saves
    // as the default's name rather than another form.
    char const *name = ccout_glide_hz_name(p->glide_hz);
    return name ? name : ccout_glide_hz_name(Ccout_glide_hz_default);
  }
  case Confopt_clock_source:
    return clock_source_name(p->clock_source);
  case Confopt_portmidi_input_device:
  case Confopt_alsa_input_device:
    return p->midi_in;
  default:
    return "";
  }
}
