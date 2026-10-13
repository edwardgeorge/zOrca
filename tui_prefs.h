#pragma once
#include "base.h"
#include "sim.h"
#include "transport.h"

// The orca.conf settings (architecture spine AD-20), as a testable shell
// module (AD-1, AD-2, AD-21): the key table and its order, the reading of
// one conf line into a value and a touched bit, the command-line overrides
// over those values, the setters the menus and forms call, and the text
// saved after each `key = `. Like tick.c and tooltips.c it does no I/O,
// reads no clock, holds no writable statics and includes only the core
// allow-list, never midi_in.h or a backend header, and it holds no FEAT_ or
// BORCA_HAS_MIDI_IN ifdef: the build's input reader comes in as
// tui_prefs_init's has_midi_in and the backend's input key. tui_main.c owns
// the file (ezconf_r_step and ezconf_w_step in sysmisc.c), curses, the
// menus and forms, the PortMidi output device's save, and every engine call
// (ged_set_dialect's clear, cancel and re-mark among them, AD-6).
//
// The touched bits. A key's bit is set when its line was read from the file
// with a value taken, or when a setter changed its value; tui_save_prefs writes
// the keys tui_prefs_saves_key names, and ezconf writes every other key line
// back with its value unchanged (as `key = value`: ezconf normalises the
// spacing, as it always has) and every comment line as it was (an
// unterminated last line gains its newline; a CRLF comment line keeps its \r;
// key lines are written with \n). A NUL ends a line's text: the bytes after
// it are lost on save, as before P4-4. A line that does not fit ezconf's
// 1024-byte buffer, an unterminated last line of 1023 bytes among them, still
// fails the save as a whole. An override clears its key's bit, whether or not
// the override took effect (External without a reader; an input name that
// does not fit), so a flag lasts for the session and the stored line is kept
// until a menu change touches the key again (AD-20).
// portmidi_output_device and the input key are never touched by a read.

// The keys, in the order ezconf appends missing keys (TUI_PREFS_KEYS order
// is behaviour: a key absent from the file is appended in this order). A
// new key is appended in snake_case (AD-20).
#define TUI_PREFS_KEYS(_)                                                      \
  _(portmidi_output_device)                                                    \
  _(osc_output_address)                                                        \
  _(osc_output_port)                                                           \
  _(osc_output_enabled)                                                        \
  _(midi_beat_clock)                                                           \
  _(margins)                                                                   \
  _(grid_dot_type)                                                             \
  _(grid_ruler_type)                                                           \
  _(dialect)                                                                   \
  _(glide_hz)                                                                  \
  _(clock_source)                                                              \
  _(portmidi_input_device)                                                     \
  _(alsa_input_device)

#define CONFOPT_ENUM(x) Confopt_##x,
typedef enum { TUI_PREFS_KEYS(CONFOPT_ENUM) Confoptslen } Confopt;
#undef CONFOPT_ENUM

// The key names, in Confopt order, for ezconf_r_step and ezconf_w_addopt.
extern char const *const confopts[Confoptslen];

// Use this to create a bitflag out of a Confopt_. These flags are used to
// indicate that a setting has been touched by the user. In other words, these
// flags are used to check whether or not a particular setting should be
// written back to the conf file during a conf file save.
#define TOUCHFLAG(_confopt) (1u << (Usz)_confopt)

enum {
  // A string value's room, with its terminator: ezconf's line buffer
  // (Ezconf_r in sysmisc.h), so every value read from the file fits.
  Tui_prefs_value_cap = 1024,
  // tui_prefs_saved_value's buffer: room for "<x>x<y>" of any two ints.
  Tui_prefs_number_cap = 32,
};

// Every key's value for this session. tui_prefs_init sets every field.
typedef struct {
  char portmidi_output_device[Tui_prefs_value_cap]; // read, never touched
  char osc_output_address[Tui_prefs_value_cap];     // empty is loopback
  char osc_output_port[Tui_prefs_value_cap];
  char midi_in[Tui_prefs_value_cap]; // the backend's input name; empty: none
  int softmargin_y, softmargin_x;
  Orca_dialect dialect;
  Usz glide_hz; // 50, 100 or 200
  Clock_source clock_source;
  U32 touched;         // TOUCHFLAG(Confopt_...) of each key to save
  Confopt midi_in_key; // the compiled backend's input key
  bool osc_output_enabled;
  bool midi_bclock; // the stored Send MIDI Beat Clock setting
  bool fancy_grid_dots, fancy_grid_rulers;
  bool has_midi_in; // the build has an input reader
} Tui_prefs;

// Settings given on the command line (AD-20). main() parses each flag into
// a pending override before curses starts: it sets the key's TOUCHFLAG in
// keys and its value below. tui_prefs_apply_overrides applies them after
// the conf has loaded, so a flag beats orca.conf. A new override adds its
// value here, its key to Tui_overrides_keys and one branch to
// tui_prefs_apply_overrides.
typedef struct {
  U32 keys;                           // TOUCHFLAG(Confopt_...) of each key
  Orca_dialect dialect;               // --dialect
  Clock_source clock_source;          // --clock-source
  char midi_in[Tui_prefs_value_cap];  // --midi-in, the backend's input key
} Tui_overrides;

// The keys an override may name: tui_prefs_apply_overrides asserts keys
// holds no other, since a key without a branch there would be cleared and
// keep the conf's value.
enum {
  Tui_overrides_keys = TOUCHFLAG(Confopt_dialect) |
                       TOUCHFLAG(Confopt_clock_source) |
                       TOUCHFLAG(Confopt_portmidi_input_device) |
                       TOUCHFLAG(Confopt_alsa_input_device)
};

// The defaults: margins 1 (y) and 2 (x), fancy grid dots and rulers, OSC
// output off to an empty address (loopback) on port 49162, beat clock off,
// bOrca, glide_hz Ccout_glide_hz_default, Internal, no input and nothing
// touched. has_midi_in says the build has an input reader and midi_in_key
// is its backend's input key (Confopt_alsa_input_device or
// Confopt_portmidi_input_device); without a reader the key is ignored.
void tui_prefs_init(Tui_prefs *p, bool has_midi_in, Confopt midi_in_key);

// Reads "5x3" or "5" as %dx%d did: leading blanks and a sign, a number,
// then 'x' and a second number; one number, or anything but 'x' and a
// number after it, gives both the first. A number outside int is none: as
// the first it is false, as the second it gives both the first (where the
// %dx%d form differed, on overflow only). Trailing text is ignored. False,
// leaving both alone, when there is no leading number.
bool tui_prefs_read_nxn_or_n(char const *str, int *out_a, int *out_b);

// Takes one conf line's value for key, as tui_load_conf read it before P3-4b.
// True when the value is taken: it is stored and the key touched (but for
// portmidi_output_device and the input key, stored only). The two OSC strings
// are stored as given, empty included: the port line too, which
// tui_prefs_set_osc_output_port alone refuses empty. One difference from
// before P3-4b: a later empty osc_output_address line now wins (loopback),
// where tui_load_conf kept an earlier non-empty address; only a hand-edited
// file holds the key twice, since a save comments out a repeated key. False,
// with nothing changed, for a value that is not one of the key's (that line is
// then written back with its value unchanged on save), for External or an
// input key without a reader, for the other backend's input key, and for a
// value of Tui_prefs_value_cap bytes or more.
bool tui_prefs_read_line(Tui_prefs *p, Confopt key, char const *value);

// Applies each key in o->keys (Tui_overrides_keys only, asserted) over the
// conf's value (the dialect; the clock source, dropping External without a
// reader; midi_in only with a reader and when the array holds a terminated
// name), then clears those keys' touched bits, whether or not each override
// took effect.
void tui_prefs_apply_overrides(Tui_prefs *p, Tui_overrides const *o);

// The setters, one per setting a menu or form changes. Each returns false
// and changes nothing when the value is already held or is not one the key
// takes; otherwise it stores the value, sets the key's touched bit and
// returns true. Refused: a glide_hz that ccout_glide_hz_name does not name,
// External without a reader, an empty input name or one in a build with no
// reader, a negative margin, an empty port, and a string that does not fit.
bool tui_prefs_set_dialect(Tui_prefs *p, Orca_dialect dialect);
bool tui_prefs_set_glide_hz(Tui_prefs *p, Usz glide_hz);
bool tui_prefs_set_clock_source(Tui_prefs *p, Clock_source source);
bool tui_prefs_set_midi_in(Tui_prefs *p, char const *name);
bool tui_prefs_set_grid_dots(Tui_prefs *p, bool fancy);
bool tui_prefs_set_grid_rulers(Tui_prefs *p, bool fancy);
bool tui_prefs_set_osc_output_enabled(Tui_prefs *p, bool enabled);
bool tui_prefs_set_midi_bclock(Tui_prefs *p, bool enabled);
bool tui_prefs_set_margins(Tui_prefs *p, int x, int y);
bool tui_prefs_set_osc_output_address(Tui_prefs *p, char const *address);
bool tui_prefs_set_osc_output_port(Tui_prefs *p, char const *port);

// True when a save writes key's line from the prefs: the key is in the
// table, is touched and is not portmidi_output_device, whose save is the
// shell's own (the PortMidi device's name). False for any other value.
bool tui_prefs_saves_key(Tui_prefs const *p, Confopt key);

// The text written after `<key> = ` for key: the strings as held (empty
// writes nothing), 1 or 0, plain or fancy, the dialect, glide_hz and clock
// source names, margins as <x>x<y> formatted into buf (at least
// Tui_prefs_number_cap bytes), and midi_in for both input keys. For
// portmidi_output_device it is the stored string, which the shell does not
// write: its save is the PortMidi device's name. Never NULL.
char const *tui_prefs_saved_value(Tui_prefs const *p, Confopt key, char *buf);
