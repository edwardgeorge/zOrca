// Unit tests for tui_prefs.c, the orca.conf settings (P3-4b; spec items I1,
// I2 and I6; spine AD-20): the key table and its order, each key's parse and
// saved text, the flags over the conf with their touched bits cleared, and
// the setters' same-value no-op, on prefs built on the stack in the three
// build shapes (an ALSA reader, a PortMidi reader, no reader). What the
// module cannot reach, ged_set_dialect's clear, cancel and re-mark, the
// preview's context and the file itself, is checked by the P3-4b tmux smoke
// and by code reading.
#include "../../ccout.h"
#include "../../tui_prefs.h"
#include "tests.h"

static void init_alsa(Tui_prefs *p) {
  tui_prefs_init(p, true, Confopt_alsa_input_device);
}
static void init_portmidi(Tui_prefs *p) {
  tui_prefs_init(p, true, Confopt_portmidi_input_device);
}
static void init_no_reader(Tui_prefs *p) {
  tui_prefs_init(p, false, Confoptslen);
}

// An ALSA-keyed prefs with a good value read for every key, none of them
// the default, so a bad value that writes a field shows whatever it writes.
static void init_loaded(Tui_prefs *p) {
  init_alsa(p);
  static struct {
    Confopt key;
    char const *value;
  } const lines[] = {
      {Confopt_portmidi_output_device, "Foo"},
      {Confopt_osc_output_address, "10.0.0.2"},
      {Confopt_osc_output_port, "7000"},
      {Confopt_osc_output_enabled, "1"},
      {Confopt_midi_beat_clock, "1"},
      {Confopt_margins, "4x3"},
      {Confopt_grid_dot_type, "plain"},
      {Confopt_grid_ruler_type, "plain"},
      {Confopt_dialect, "upstream"},
      {Confopt_glide_hz, "200"},
      {Confopt_clock_source, "external"},
      {Confopt_alsa_input_device, "In:0"},
  };
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(lines); ++i)
    (void)tui_prefs_read_line(p, lines[i].key, lines[i].value);
}

// Every field, the strings by content.
static bool prefs_equal(Tui_prefs const *a, Tui_prefs const *b) {
  return strcmp(a->portmidi_output_device, b->portmidi_output_device) == 0 &&
         strcmp(a->osc_output_address, b->osc_output_address) == 0 &&
         strcmp(a->osc_output_port, b->osc_output_port) == 0 &&
         strcmp(a->midi_in, b->midi_in) == 0 &&
         a->softmargin_y == b->softmargin_y &&
         a->softmargin_x == b->softmargin_x && a->dialect == b->dialect &&
         a->glide_hz == b->glide_hz && a->clock_source == b->clock_source &&
         a->touched == b->touched && a->midi_in_key == b->midi_in_key &&
         a->osc_output_enabled == b->osc_output_enabled &&
         a->midi_bclock == b->midi_bclock &&
         a->fancy_grid_dots == b->fancy_grid_dots &&
         a->fancy_grid_rulers == b->fancy_grid_rulers &&
         a->has_midi_in == b->has_midi_in;
}

// A string of n 'a's in buf, which holds n + 1 bytes.
static char const *long_string(char *buf, Usz n) {
  memset(buf, 'a', n);
  buf[n] = '\0';
  return buf;
}

// The 13 keys in their order, which is behaviour: ezconf appends a key
// missing from the file in the save loop's order, which is this one.
void test_prefs_key_table(void) {
  static char const *const keys[] = {
      "portmidi_output_device", "osc_output_address", "osc_output_port",
      "osc_output_enabled",     "midi_beat_clock",    "margins",
      "grid_dot_type",          "grid_ruler_type",    "dialect",
      "glide_hz",               "clock_source",       "portmidi_input_device",
      "alsa_input_device"};
  CHECK(Confoptslen == 13);
  CHECK(ORCA_ARRAY_COUNTOF(keys) == (Usz)Confoptslen);
  bool names_ok = true;
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(keys) && i < (Usz)Confoptslen; ++i)
    names_ok = names_ok && confopts[i] && strcmp(confopts[i], keys[i]) == 0;
  CHECK(names_ok);
  CHECK(Confopt_portmidi_output_device == 0 && Confopt_margins == 5 &&
        Confopt_dialect == 8 && Confopt_glide_hz == 9 &&
        Confopt_clock_source == 10 && Confopt_portmidi_input_device == 11 &&
        Confopt_alsa_input_device == 12);
  CHECK(TOUCHFLAG(Confopt_portmidi_output_device) == 1u);
  CHECK(TOUCHFLAG(Confopt_alsa_input_device) == 1u << 12);
}

// The defaults, then one good line per key: each is taken and touched, but
// portmidi_output_device and the input key, which are stored only.
void test_prefs_read_line(void) {
  Tui_prefs p;
  init_alsa(&p);
  CHECK(p.softmargin_y == 1 && p.softmargin_x == 2);
  CHECK(p.fancy_grid_dots && p.fancy_grid_rulers);
  CHECK(strcmp(p.osc_output_port, "49162") == 0);
  CHECK(p.osc_output_address[0] == '\0');
  CHECK(!p.osc_output_enabled && !p.midi_bclock);
  CHECK(p.dialect == Orca_dialect_borca);
  CHECK(p.glide_hz == 100 && p.glide_hz == Ccout_glide_hz_default);
  CHECK(p.clock_source == Clock_source_internal);
  CHECK(p.midi_in[0] == '\0' && p.portmidi_output_device[0] == '\0');
  CHECK(p.touched == 0);
  CHECK(p.has_midi_in && p.midi_in_key == Confopt_alsa_input_device);

  U32 touched = 0;
  CHECK(tui_prefs_read_line(&p, Confopt_portmidi_output_device, "Foo"));
  CHECK(strcmp(p.portmidi_output_device, "Foo") == 0 && p.touched == touched);

  CHECK(tui_prefs_read_line(&p, Confopt_osc_output_address, "10.0.0.2"));
  touched |= TOUCHFLAG(Confopt_osc_output_address);
  CHECK(strcmp(p.osc_output_address, "10.0.0.2") == 0 && p.touched == touched);
  CHECK(tui_prefs_read_line(&p, Confopt_osc_output_address, ""));
  CHECK(p.osc_output_address[0] == '\0' && p.touched == touched);

  CHECK(tui_prefs_read_line(&p, Confopt_osc_output_port, "7000"));
  touched |= TOUCHFLAG(Confopt_osc_output_port);
  CHECK(strcmp(p.osc_output_port, "7000") == 0 && p.touched == touched);
  // The port line is stored as given, empty included, and saves back as ""
  // (the pre-P3-4b load did the same); only the form's setter refuses empty.
  {
    char buf[Tui_prefs_number_cap];
    CHECK(tui_prefs_read_line(&p, Confopt_osc_output_port, ""));
    CHECK(p.osc_output_port[0] == '\0' && p.touched == touched);
    CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_osc_output_port, buf),
                 "") == 0);
    CHECK(tui_prefs_read_line(&p, Confopt_osc_output_port, "7000"));
  }

  // The boolish set, each word flipping the value it names.
  static char const *const words[] = {"1", "0", "true", "false", "yes", "no"};
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(words); ++i) {
    bool want = i % 2 == 0;
    Tui_prefs q;
    init_alsa(&q);
    q.osc_output_enabled = !want;
    q.midi_bclock = !want;
    CHECK(tui_prefs_read_line(&q, Confopt_osc_output_enabled, words[i]));
    CHECK(q.osc_output_enabled == want &&
          q.touched == TOUCHFLAG(Confopt_osc_output_enabled));
    CHECK(tui_prefs_read_line(&q, Confopt_midi_beat_clock, words[i]));
    CHECK(q.midi_bclock == want &&
          q.touched == (TOUCHFLAG(Confopt_osc_output_enabled) |
                        TOUCHFLAG(Confopt_midi_beat_clock)));
  }
  CHECK(tui_prefs_read_line(&p, Confopt_osc_output_enabled, "1"));
  touched |= TOUCHFLAG(Confopt_osc_output_enabled);
  CHECK(p.osc_output_enabled && p.touched == touched);
  CHECK(tui_prefs_read_line(&p, Confopt_midi_beat_clock, "yes"));
  touched |= TOUCHFLAG(Confopt_midi_beat_clock);
  CHECK(p.midi_bclock && p.touched == touched);

  // margins is x first: 2x1 is x 2, y 1, and one number is both.
  CHECK(tui_prefs_read_line(&p, Confopt_margins, "2x1"));
  touched |= TOUCHFLAG(Confopt_margins);
  CHECK(p.softmargin_x == 2 && p.softmargin_y == 1 && p.touched == touched);
  CHECK(tui_prefs_read_line(&p, Confopt_margins, "3"));
  CHECK(p.softmargin_x == 3 && p.softmargin_y == 3 && p.touched == touched);

  CHECK(tui_prefs_read_line(&p, Confopt_grid_dot_type, "plain"));
  touched |= TOUCHFLAG(Confopt_grid_dot_type);
  CHECK(!p.fancy_grid_dots && p.touched == touched);
  CHECK(tui_prefs_read_line(&p, Confopt_grid_dot_type, "fancy"));
  CHECK(p.fancy_grid_dots && p.touched == touched);
  CHECK(tui_prefs_read_line(&p, Confopt_grid_ruler_type, "fancy"));
  touched |= TOUCHFLAG(Confopt_grid_ruler_type);
  CHECK(p.fancy_grid_rulers && p.touched == touched);
  CHECK(tui_prefs_read_line(&p, Confopt_grid_ruler_type, "plain"));
  CHECK(!p.fancy_grid_rulers && p.touched == touched);

  CHECK(tui_prefs_read_line(&p, Confopt_dialect, "upstream"));
  touched |= TOUCHFLAG(Confopt_dialect);
  CHECK(p.dialect == Orca_dialect_upstream && p.touched == touched);

  CHECK(tui_prefs_read_line(&p, Confopt_glide_hz, "200"));
  touched |= TOUCHFLAG(Confopt_glide_hz);
  CHECK(p.glide_hz == 200 && p.touched == touched);
  CHECK(tui_prefs_read_line(&p, Confopt_glide_hz, "50"));
  CHECK(p.glide_hz == 50 && p.touched == touched);

  CHECK(tui_prefs_read_line(&p, Confopt_clock_source, "external"));
  touched |= TOUCHFLAG(Confopt_clock_source);
  CHECK(p.clock_source == Clock_source_external && p.touched == touched);

  // The backend's input key: stored, never touched, so its line is written
  // back with its value unchanged until the MIDI Input menu changes it.
  CHECK(tui_prefs_read_line(&p, Confopt_alsa_input_device, "MidiHub:0"));
  CHECK(strcmp(p.midi_in, "MidiHub:0") == 0 && p.touched == touched);

  // Every key but those two touched.
  U32 const all = (1u << Confoptslen) - 1u;
  CHECK(touched == (all & ~(TOUCHFLAG(Confopt_portmidi_output_device) |
                            TOUCHFLAG(Confopt_portmidi_input_device) |
                            TOUCHFLAG(Confopt_alsa_input_device))));

  // The longest value that fits, ezconf's 1023 bytes, is taken.
  char big[Tui_prefs_value_cap + 1];
  CHECK(tui_prefs_read_line(&p, Confopt_osc_output_address,
                            long_string(big, Tui_prefs_value_cap - 1)));
  CHECK(strlen(p.osc_output_address) == Tui_prefs_value_cap - 1);
}

// The margins parser against the %dx%d results it replaces: the P3-4b
// drafter's probe, input by input.
static void check_nxn(char const *s, bool ok, int a, int b) {
  int x = -77, y = -77;
  bool got = tui_prefs_read_nxn_or_n(s, &x, &y);
  CHECK(got == ok);
  if (ok)
    CHECK(x == a && y == b);
  else
    CHECK(x == -77 && y == -77);
}

// A bad value is refused: false, the field and the touched bits unchanged,
// so the save loop never rewrites the line and ezconf writes its value back
// unchanged.
void test_prefs_bad_values(void) {
  struct {
    Confopt key;
    char const *value;
  } const bad[] = {
      {Confopt_osc_output_enabled, "maybe"},
      {Confopt_osc_output_enabled, "2"},
      {Confopt_osc_output_enabled, "True"},
      {Confopt_osc_output_enabled, ""},
      {Confopt_midi_beat_clock, "maybe"},
      {Confopt_midi_beat_clock, "2"},
      {Confopt_margins, "-1x2"},
      {Confopt_margins, "2x-1"},
      {Confopt_margins, "-3"},
      {Confopt_margins, "abc"},
      {Confopt_margins, "x3"},
      {Confopt_margins, ""},
      {Confopt_grid_dot_type, "Fancy"},
      {Confopt_grid_dot_type, "plain "},
      {Confopt_grid_ruler_type, "Plain"},
      {Confopt_dialect, "Upstream"},
      {Confopt_dialect, "bOrca"},
      {Confopt_glide_hz, "150"},
      {Confopt_glide_hz, "abc"},
      {Confopt_glide_hz, ""},
      {Confopt_glide_hz, "050"},
      {Confopt_clock_source, "sideways"},
      {Confopt_clock_source, "External"},
      {Confoptslen, "1"},
  };
  // Each against the defaults and against a prefs loaded with other
  // values, so writing a default is caught too.
  for (int loaded = 0; loaded < 2; ++loaded) {
    for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(bad); ++i) {
      Tui_prefs p, before;
      if (loaded) {
        init_loaded(&p);
        init_loaded(&before);
      } else {
        init_alsa(&p);
        init_alsa(&before);
      }
      CHECK(!tui_prefs_read_line(&p, bad[i].key, bad[i].value));
      CHECK(prefs_equal(&p, &before));
    }
  }
  // The loaded prefs really holds no default.
  {
    Tui_prefs d, l;
    init_alsa(&d);
    init_loaded(&l);
    CHECK(strcmp(d.portmidi_output_device, l.portmidi_output_device) != 0 &&
          strcmp(d.osc_output_address, l.osc_output_address) != 0 &&
          strcmp(d.osc_output_port, l.osc_output_port) != 0 &&
          strcmp(d.midi_in, l.midi_in) != 0 &&
          d.softmargin_y != l.softmargin_y &&
          d.softmargin_x != l.softmargin_x && d.dialect != l.dialect &&
          d.glide_hz != l.glide_hz && d.clock_source != l.clock_source &&
          d.osc_output_enabled != l.osc_output_enabled &&
          d.midi_bclock != l.midi_bclock &&
          d.fancy_grid_dots != l.fancy_grid_dots &&
          d.fancy_grid_rulers != l.fancy_grid_rulers);
  }
  // An unknown glide_hz line on top of a good one keeps the good value, and
  // a file with only the unknown line leaves 100 and the key untouched,
  // which is what keeps `glide_hz = 150` byte-identical on save (I2).
  {
    Tui_prefs p;
    init_alsa(&p);
    CHECK(!tui_prefs_read_line(&p, Confopt_glide_hz, "150"));
    CHECK(p.glide_hz == 100 && !(p.touched & TOUCHFLAG(Confopt_glide_hz)));
    CHECK(tui_prefs_read_line(&p, Confopt_glide_hz, "200"));
    CHECK(!tui_prefs_read_line(&p, Confopt_glide_hz, "150"));
    CHECK(p.glide_hz == 200 && p.touched == TOUCHFLAG(Confopt_glide_hz));
  }
  // A value of Tui_prefs_value_cap bytes or more is refused for every key
  // (it cannot come from ezconf), even where any string is a value.
  {
    char big[Tui_prefs_value_cap + 1];
    long_string(big, Tui_prefs_value_cap);
    for (int k = 0; k < Confoptslen; ++k) {
      Tui_prefs p, before;
      init_loaded(&p);
      init_loaded(&before);
      CHECK(!tui_prefs_read_line(&p, (Confopt)k, big));
      CHECK(prefs_equal(&p, &before));
    }
  }
  // The margins oddities, through the conf line.
  {
    Tui_prefs p;
    init_alsa(&p);
    CHECK(tui_prefs_read_line(&p, Confopt_margins, "2x1 extra"));
    CHECK(p.softmargin_x == 2 && p.softmargin_y == 1);
    CHECK(tui_prefs_read_line(&p, Confopt_margins, "3y"));
    CHECK(p.softmargin_x == 3 && p.softmargin_y == 3);
    CHECK(tui_prefs_read_line(&p, Confopt_margins, " 4x5"));
    CHECK(p.softmargin_x == 4 && p.softmargin_y == 5);
    CHECK(tui_prefs_read_line(&p, Confopt_margins, "2X1"));
    CHECK(p.softmargin_x == 2 && p.softmargin_y == 2);
    CHECK(tui_prefs_read_line(&p, Confopt_margins, "007x08"));
    CHECK(p.softmargin_x == 7 && p.softmargin_y == 8);
    CHECK(tui_prefs_read_line(&p, Confopt_margins, "0x0"));
    CHECK(p.softmargin_x == 0 && p.softmargin_y == 0);
    CHECK(p.touched == TOUCHFLAG(Confopt_margins));
  }
  // The parser itself, on the probe's inputs: what %dx%d gave.
  check_nxn("2x1", true, 2, 1);
  check_nxn("3", true, 3, 3);
  check_nxn("3x", true, 3, 3);
  check_nxn("3y", true, 3, 3);
  check_nxn("-1x2", true, -1, 2);
  check_nxn("+4x+5", true, 4, 5);
  check_nxn(" 4x5", true, 4, 5);
  check_nxn("4x 5", true, 4, 5);
  check_nxn("4 x5", true, 4, 4);
  check_nxn("2x1 extra", true, 2, 1);
  check_nxn("2X1", true, 2, 2);
  check_nxn("1e2", true, 1, 1);
  check_nxn("007x08", true, 7, 8);
  check_nxn("2x1x9", true, 2, 1);
  check_nxn("0x0", true, 0, 0);
  check_nxn("5x-", true, 5, 5);
  // The int bounds, on every target (where long is int, strtol clamps the
  // overflow to INT_MAX and only ERANGE tells): the bounds themselves are
  // taken, past them is refused as the first number and none as the second,
  // so b falls back to a (where this parser differs from %dx%d, on overflow
  // only).
  check_nxn("2147483647", true, 2147483647, 2147483647);
  check_nxn("-2147483648", true, -2147483647 - 1, -2147483647 - 1);
  check_nxn("2147483648", false, 0, 0);
  check_nxn("-2147483649", false, 0, 0);
  check_nxn("1x2147483648", true, 1, 1);
  check_nxn("x3", false, 0, 0);
  check_nxn("abc", false, 0, 0);
  check_nxn("-", false, 0, 0);
  check_nxn("", false, 0, 0);
  check_nxn("   ", false, 0, 0);
}

// The input keys by backend, and External without a reader.
void test_prefs_input_keys(void) {
  {
    // ALSA: the PortMidi key is refused and its line kept.
    Tui_prefs p, before;
    init_alsa(&p);
    init_alsa(&before);
    CHECK(!tui_prefs_read_line(&p, Confopt_portmidi_input_device, "X"));
    CHECK(prefs_equal(&p, &before));
    CHECK(tui_prefs_read_line(&p, Confopt_alsa_input_device, "Y:0"));
    CHECK(strcmp(p.midi_in, "Y:0") == 0 && p.touched == 0);
    CHECK(!tui_prefs_read_line(&p, Confopt_portmidi_input_device, "X"));
    CHECK(strcmp(p.midi_in, "Y:0") == 0 && p.touched == 0);
  }
  {
    // PortMidi: the ALSA key is refused.
    Tui_prefs p, before;
    init_portmidi(&p);
    init_portmidi(&before);
    CHECK(!tui_prefs_read_line(&p, Confopt_alsa_input_device, "Y:0"));
    CHECK(prefs_equal(&p, &before));
    CHECK(tui_prefs_read_line(&p, Confopt_portmidi_input_device, "X"));
    CHECK(strcmp(p.midi_in, "X") == 0 && p.touched == 0);
    // A pick touches the PortMidi key; both input keys save the name.
    CHECK(tui_prefs_set_midi_in(&p, "Z"));
    CHECK(p.touched == TOUCHFLAG(Confopt_portmidi_input_device));
  }
  {
    // No reader: both keys are ignored, and no name can be set.
    Tui_prefs p, before;
    init_no_reader(&p);
    init_no_reader(&before);
    CHECK(!p.has_midi_in);
    CHECK(!tui_prefs_read_line(&p, Confopt_alsa_input_device, "Y:0"));
    CHECK(!tui_prefs_read_line(&p, Confopt_portmidi_input_device, "X"));
    CHECK(!tui_prefs_set_midi_in(&p, "X"));
    CHECK(prefs_equal(&p, &before));
  }
  {
    // External without a reader is refused by the line, the setter and the
    // override; internal is still taken and touched.
    Tui_prefs p, before;
    init_no_reader(&p);
    init_no_reader(&before);
    CHECK(!tui_prefs_read_line(&p, Confopt_clock_source, "external"));
    CHECK(!tui_prefs_set_clock_source(&p, Clock_source_external));
    Tui_overrides o = {.keys = TOUCHFLAG(Confopt_clock_source),
                       .clock_source = Clock_source_external};
    tui_prefs_apply_overrides(&p, &o);
    CHECK(prefs_equal(&p, &before));
    CHECK(tui_prefs_read_line(&p, Confopt_clock_source, "internal"));
    CHECK(p.clock_source == Clock_source_internal &&
          p.touched == TOUCHFLAG(Confopt_clock_source));
  }
  {
    // With a reader External is taken by each of the three.
    Tui_prefs p;
    init_alsa(&p);
    CHECK(tui_prefs_read_line(&p, Confopt_clock_source, "external"));
    CHECK(p.clock_source == Clock_source_external);
    init_portmidi(&p);
    CHECK(tui_prefs_set_clock_source(&p, Clock_source_external));
    CHECK(p.clock_source == Clock_source_external);
    init_alsa(&p);
    Tui_overrides o = {.keys = TOUCHFLAG(Confopt_clock_source),
                       .clock_source = Clock_source_external};
    tui_prefs_apply_overrides(&p, &o);
    CHECK(p.clock_source == Clock_source_external);
  }
}

// A flag beats the conf for the session, and clears its key's touched bit
// so the stored line's value is written back unchanged until a menu touches
// the key again
// (AD-20); keys the flags do not name keep their bits.
void test_prefs_flag_over_conf(void) {
  Tui_prefs p;
  init_alsa(&p);
  CHECK(tui_prefs_read_line(&p, Confopt_dialect, "borca"));
  CHECK(tui_prefs_read_line(&p, Confopt_clock_source, "internal"));
  CHECK(tui_prefs_read_line(&p, Confopt_glide_hz, "200"));
  // A loaded input line is never touched, so a pick sets the input bit,
  // which makes "the override clears it" falsifiable.
  CHECK(tui_prefs_set_midi_in(&p, "A:0"));
  U32 const input = TOUCHFLAG(Confopt_alsa_input_device);
  CHECK(p.touched ==
        (TOUCHFLAG(Confopt_dialect) | TOUCHFLAG(Confopt_clock_source) |
         TOUCHFLAG(Confopt_glide_hz) | input));

  // No keys, nothing changes.
  {
    Tui_prefs before = p;
    Tui_overrides none = {.keys = 0,
                          .dialect = Orca_dialect_upstream,
                          .clock_source = Clock_source_external};
    memcpy(none.midi_in, "B:0", 4);
    tui_prefs_apply_overrides(&p, &none);
    CHECK(prefs_equal(&p, &before));
  }

  Tui_overrides o = {.keys = TOUCHFLAG(Confopt_dialect) |
                             TOUCHFLAG(Confopt_clock_source) | input,
                     .dialect = Orca_dialect_upstream,
                     .clock_source = Clock_source_external};
  memcpy(o.midi_in, "B:0", 4);
  tui_prefs_apply_overrides(&p, &o);
  CHECK(p.dialect == Orca_dialect_upstream);
  CHECK(p.clock_source == Clock_source_external);
  CHECK(strcmp(p.midi_in, "B:0") == 0);
  CHECK(p.glide_hz == 200);
  CHECK(p.touched == TOUCHFLAG(Confopt_glide_hz));

  // A menu change after the flag touches the key again, so it is saved.
  CHECK(tui_prefs_set_dialect(&p, Orca_dialect_borca));
  CHECK(p.touched ==
        (TOUCHFLAG(Confopt_glide_hz) | TOUCHFLAG(Confopt_dialect)));

  // Without a reader the input override is ignored (its bit is cleared as
  // every overridden key's is).
  {
    Tui_prefs q;
    init_no_reader(&q);
    Tui_overrides oq = {.keys = TOUCHFLAG(Confopt_alsa_input_device) |
                                TOUCHFLAG(Confopt_portmidi_input_device)};
    memcpy(oq.midi_in, "B:0", 4);
    tui_prefs_apply_overrides(&q, &oq);
    CHECK(q.midi_in[0] == '\0' && q.touched == 0);
  }
  // With the PortMidi reader the ALSA key's override is not the input's.
  {
    Tui_prefs q;
    init_portmidi(&q);
    Tui_overrides oq = {.keys = TOUCHFLAG(Confopt_alsa_input_device)};
    memcpy(oq.midi_in, "B:0", 4);
    tui_prefs_apply_overrides(&q, &oq);
    CHECK(q.midi_in[0] == '\0');
    oq.keys = TOUCHFLAG(Confopt_portmidi_input_device);
    tui_prefs_apply_overrides(&q, &oq);
    CHECK(strcmp(q.midi_in, "B:0") == 0);
  }
  // An input override with no terminator in its array (it does not fit) is
  // dropped: midi_in keeps the held name, and the key's bit is cleared all
  // the same, as every overridden key's is.
  {
    Tui_prefs q;
    init_alsa(&q);
    CHECK(tui_prefs_set_midi_in(&q, "A:0"));
    CHECK(q.touched == TOUCHFLAG(Confopt_alsa_input_device));
    Tui_overrides oq = {.keys = TOUCHFLAG(Confopt_alsa_input_device)};
    memset(oq.midi_in, 'a', sizeof oq.midi_in);
    tui_prefs_apply_overrides(&q, &oq);
    CHECK(strcmp(q.midi_in, "A:0") == 0 && q.touched == 0);
  }
}

// Calls setter k (0..10) with the value prefs init holds when same is
// true, and with another value otherwise; *key is the key it touches.
static bool call_setter(Tui_prefs *p, int k, bool same, Confopt *key) {
  switch (k) {
  case 0:
    *key = Confopt_dialect;
    return tui_prefs_set_dialect(p, same ? Orca_dialect_borca
                                         : Orca_dialect_upstream);
  case 1:
    *key = Confopt_glide_hz;
    return tui_prefs_set_glide_hz(p, same ? 100 : 200);
  case 2:
    *key = Confopt_clock_source;
    return tui_prefs_set_clock_source(p, same ? Clock_source_internal
                                              : Clock_source_external);
  case 3:
    *key = Confopt_alsa_input_device;
    return tui_prefs_set_midi_in(p, same ? "In:0" : "Other:1");
  case 4:
    *key = Confopt_grid_dot_type;
    return tui_prefs_set_grid_dots(p, same);
  case 5:
    *key = Confopt_grid_ruler_type;
    return tui_prefs_set_grid_rulers(p, same);
  case 6:
    *key = Confopt_osc_output_enabled;
    return tui_prefs_set_osc_output_enabled(p, !same);
  case 7:
    *key = Confopt_midi_beat_clock;
    return tui_prefs_set_midi_bclock(p, !same);
  case 8:
    *key = Confopt_margins;
    return same ? tui_prefs_set_margins(p, 2, 1)
                : tui_prefs_set_margins(p, 10, 3);
  case 9:
    *key = Confopt_osc_output_address;
    return tui_prefs_set_osc_output_address(p, same ? "" : "10.0.0.2");
  default:
    *key = Confopt_osc_output_port;
    return tui_prefs_set_osc_output_port(p, same ? "49162" : "7000");
  }
}

// One rule for every menu and form: the value already held is a no-op
// (false, nothing changes, nothing is touched), a new one is stored and
// touches its key and no other.
void test_prefs_menu_noop(void) {
  enum { Setters = 11 };
  for (int k = 0; k < Setters; ++k) {
    Tui_prefs p, before;
    init_alsa(&p);
    // The input name is held as a loaded line would hold it: untouched.
    CHECK(tui_prefs_read_line(&p, Confopt_alsa_input_device, "In:0"));
    before = p;
    Confopt key;
    CHECK(!call_setter(&p, k, true, &key));
    CHECK(prefs_equal(&p, &before));
    CHECK(call_setter(&p, k, false, &key));
    CHECK(p.touched == TOUCHFLAG(key));
    CHECK(!prefs_equal(&p, &before));
    // Setting the new value again is a no-op too.
    Tui_prefs after = p;
    CHECK(!call_setter(&p, k, false, &key));
    CHECK(prefs_equal(&p, &after));
    // And with other bits already touched, only this key's bit is added.
    Tui_prefs q;
    init_alsa(&q);
    CHECK(tui_prefs_read_line(&q, Confopt_alsa_input_device, "In:0"));
    q.touched = ~TOUCHFLAG(key) & ((1u << Confoptslen) - 1u);
    CHECK(call_setter(&q, k, false, &key));
    CHECK(q.touched == (1u << Confoptslen) - 1u);
  }
  // The fields each change sets.
  {
    Tui_prefs p;
    init_alsa(&p);
    Confopt key;
    for (int k = 0; k < Setters; ++k)
      CHECK(call_setter(&p, k, false, &key));
    CHECK(p.dialect == Orca_dialect_upstream && p.glide_hz == 200 &&
          p.clock_source == Clock_source_external &&
          strcmp(p.midi_in, "Other:1") == 0 && !p.fancy_grid_dots &&
          !p.fancy_grid_rulers && p.osc_output_enabled && p.midi_bclock &&
          p.softmargin_x == 10 && p.softmargin_y == 3 &&
          strcmp(p.osc_output_address, "10.0.0.2") == 0 &&
          strcmp(p.osc_output_port, "7000") == 0);
    // Back to empty is a change for the address, which may be empty.
    CHECK(tui_prefs_set_osc_output_address(&p, ""));
    CHECK(p.osc_output_address[0] == '\0');
  }
  // The setters' domains: refused, nothing changes.
  {
    Tui_prefs p, before;
    init_alsa(&p);
    before = p;
    char big[Tui_prefs_value_cap + 1];
    long_string(big, Tui_prefs_value_cap);
    CHECK(!tui_prefs_set_glide_hz(&p, 150));
    CHECK(!tui_prefs_set_glide_hz(&p, 0));
    CHECK(!tui_prefs_set_margins(&p, -1, 2));
    CHECK(!tui_prefs_set_margins(&p, 2, -1));
    CHECK(!tui_prefs_set_midi_in(&p, ""));
    CHECK(!tui_prefs_set_midi_in(&p, big));
    CHECK(!tui_prefs_set_osc_output_port(&p, ""));
    CHECK(!tui_prefs_set_osc_output_port(&p, big));
    CHECK(!tui_prefs_set_osc_output_address(&p, big));
    CHECK(prefs_equal(&p, &before));
    // One byte shorter fits.
    long_string(big, Tui_prefs_value_cap - 1);
    CHECK(tui_prefs_set_osc_output_address(&p, big));
    CHECK(tui_prefs_set_midi_in(&p, big));
    CHECK(tui_prefs_set_osc_output_port(&p, big));
  }
}

// The text after `key = ` for every key, then the save loop simulated: the
// touched keys in TUI_PREFS_KEYS order, which is the order ezconf appends
// keys in.
void test_prefs_saved_value(void) {
  char buf[Tui_prefs_number_cap];
  Tui_prefs p;
  init_alsa(&p);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_osc_output_address, buf),
               "") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_osc_output_port, buf),
               "49162") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_osc_output_enabled, buf),
               "0") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_midi_beat_clock, buf),
               "0") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_margins, buf), "2x1") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_grid_dot_type, buf),
               "fancy") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_grid_ruler_type, buf),
               "fancy") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_dialect, buf), "borca") ==
        0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_glide_hz, buf), "100") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_clock_source, buf),
               "internal") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_alsa_input_device, buf),
               "") == 0);

  CHECK(tui_prefs_read_line(&p, Confopt_portmidi_output_device, "Foo"));
  CHECK(tui_prefs_read_line(&p, Confopt_osc_output_address, "10.0.0.2"));
  CHECK(tui_prefs_set_osc_output_port(&p, "7000"));
  CHECK(tui_prefs_read_line(&p, Confopt_osc_output_enabled, "yes"));
  CHECK(tui_prefs_set_midi_bclock(&p, true));
  CHECK(tui_prefs_set_margins(&p, 10, 3));
  CHECK(tui_prefs_set_grid_dots(&p, false));
  CHECK(tui_prefs_read_line(&p, Confopt_grid_ruler_type, "plain"));
  CHECK(tui_prefs_set_dialect(&p, Orca_dialect_upstream));
  CHECK(tui_prefs_set_glide_hz(&p, 200));
  CHECK(tui_prefs_read_line(&p, Confopt_clock_source, "external"));
  CHECK(tui_prefs_set_midi_in(&p, "MidiHub:MidiHub MIDI 1"));
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_portmidi_output_device, buf),
               "Foo") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_margins, buf), "10x3") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_alsa_input_device, buf),
               "MidiHub:MidiHub MIDI 1") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_portmidi_input_device, buf),
               "MidiHub:MidiHub MIDI 1") == 0);

  // Every key touched, written as tui_save_prefs' loop does (its PortMidi
  // output line is the shell's own and is skipped here, as there).
  p.touched = (1u << Confoptslen) - 1u;
  char text[1024];
  Usz at = 0;
  bool room = true;
  for (int i = 0; i < Confoptslen; ++i) {
    if (i == Confopt_portmidi_output_device ||
        !(p.touched & TOUCHFLAG(i)))
      continue;
    char const *parts[] = {confopts[i], " = ",
                           tui_prefs_saved_value(&p, (Confopt)i, buf), "\n"};
    for (Usz j = 0; j < ORCA_ARRAY_COUNTOF(parts); ++j) {
      Usz n = strlen(parts[j]);
      if (at + n >= sizeof text) {
        room = false;
        break;
      }
      memcpy(text + at, parts[j], n);
      at += n;
    }
  }
  text[at] = '\0';
  CHECK(room);
  CHECK(strcmp(text, "osc_output_address = 10.0.0.2\n"
                     "osc_output_port = 7000\n"
                     "osc_output_enabled = 1\n"
                     "midi_beat_clock = 1\n"
                     "margins = 10x3\n"
                     "grid_dot_type = plain\n"
                     "grid_ruler_type = plain\n"
                     "dialect = upstream\n"
                     "glide_hz = 200\n"
                     "clock_source = external\n"
                     "portmidi_input_device = MidiHub:MidiHub MIDI 1\n"
                     "alsa_input_device = MidiHub:MidiHub MIDI 1\n") == 0);

  // The formatter: big and negative ints (which no read or setter stores)
  // still format whole within Tui_prefs_number_cap.
  p.softmargin_x = 2147483647;
  p.softmargin_y = -2147483647 - 1;
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_margins, buf),
               "2147483647x-2147483648") == 0);
  p.softmargin_x = 0;
  p.softmargin_y = 0;
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_margins, buf), "0x0") == 0);
  // glide_hz saves only as one of the three names.
  p.glide_hz = 150;
  CHECK(strcmp(tui_prefs_saved_value(&p, Confopt_glide_hz, buf), "100") == 0);
  CHECK(strcmp(tui_prefs_saved_value(&p, Confoptslen, buf), "") == 0);
}
