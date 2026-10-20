#pragma once
#include "base.h"

// Marks bOrca's event set. events_print.c guards bOrca-only event types with
// it, so the same file also compiles against upstream Orca-c's vmio.h.
#define ORCA_VMIO_BORCA 1

typedef enum {
  Oevent_type_midi_note,
  Oevent_type_midi_cc,
  Oevent_type_midi_cc_interpolated,
  Oevent_type_midi_pb,
  Oevent_type_osc_ints,
  Oevent_type_udp_string,
} Oevent_types;

typedef struct {
  U8 oevent_type;
} Oevent_any;

typedef struct {
  U8 oevent_type;
  U8 channel;
  U8 octave;
  U8 note;
  U8 velocity;
  U8 duration; // Instead of "duration:7"
  U8 mono;     // Instead of "mono:1"
  U8 tuned;    // Set pitch_bend for the note and reset it on note-off.
  U8 subtick;  // 1: immediate note-on; 2: immediate Seaflake sequence note-off.
  U16 pitch_bend; // 14-bit bend value, centered at 8192.
} Oevent_midi_note;

typedef struct {
  U8 oevent_type;
  U8 channel, control, value;
} Oevent_midi_cc;

typedef struct {
  U8 oevent_type;
  U8 channel, control;
  U8 target_value;
  U8 interpolation_rate; // the rate index: 0 and 35 are instant, 1-34 glide
} Oevent_midi_cc_interpolated;

typedef struct {
  U8 oevent_type;
  U8 channel, lsb, msb;
} Oevent_midi_pb;

enum { Oevent_osc_int_count = 35 };

typedef struct {
  U8 oevent_type;
  Glyph glyph;
  U8 count;
  U8 numbers[Oevent_osc_int_count];
} Oevent_osc_ints;

enum { Oevent_udp_string_count = 16 };

typedef struct {
  U8 oevent_type;
  U8 count;
  char chars[Oevent_udp_string_count];
} Oevent_udp_string;

typedef union {
  Oevent_any any;
  Oevent_midi_note midi_note;
  Oevent_midi_cc midi_cc;
  Oevent_midi_cc_interpolated midi_cc_interpolated;
  Oevent_midi_pb midi_pb;
  Oevent_osc_ints osc_ints;
  Oevent_udp_string udp_string;
} Oevent;

typedef struct {
  Oevent *buffer;
  Usz count, capacity;
  // The number of events dropped into the discard slot since
  // oevent_list_init. oevent_list_clear keeps it. Only orca reports it: a
  // ZORCA_DEBUG_COUNTERS build prints it on quit; cli does not.
  Usz dropped;
  // The slot oevent_list_alloc_item returns when the list cannot grow: the
  // caller writes its event here and it is dropped. Never read or sent; each
  // drop adds one to dropped. A dropped CC is recorded by ccout as sent, the
  // same as a failed send, so a dropped glide target is not resent.
  Oevent discard;
} Oevent_list;

void oevent_list_init(Oevent_list *olist);
void oevent_list_deinit(Oevent_list *olist);
void oevent_list_clear(Oevent_list *olist);
// Copies src's events into dest. Returns false when dest cannot grow to hold
// them: dest is then empty (count 0) and keeps its buffer and capacity.
ORCA_NOINLINE ORCA_WARN_UNUSED
bool oevent_list_copy(Oevent_list const *src, Oevent_list *dest);
// Returns the slot for one more event. When the list cannot grow it returns
// &olist->discard, leaves the list's events as they were and adds one to
// dropped, so the event is dropped.
ORCA_NOINLINE
Oevent *oevent_list_alloc_item(Oevent_list *olist);
