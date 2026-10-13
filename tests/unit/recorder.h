// The recording sink and the two-note fixture shared by test_tick.c and
// test_transport.c, all static inline: a Tick_sink (tick.h) whose callbacks
// record every call, and the two sustained notes the pause, stop and
// patch-forget tests seed. A new Tick_sink callback is recorded here, once.
#pragma once
#include "../../midi_in.h"
#include "../../tick.h"
#include "tests.h"

enum { Rec_capacity = 64, Log_capacity = 128 };

typedef struct {
  int status, d1, d2;
} Triple;

// One call to the sink, in the combined log: a midi3 with its triple, a
// midi1 with its byte in status, an osc, or a control with its address. A
// midi1 also carries a copy of the watched Clock_out as it was at the call,
// when the recording watches one.
typedef enum { Log_midi3, Log_midi1, Log_osc, Log_control } Log_kind;

typedef struct {
  Log_kind kind;
  int status, d1, d2;
  char const *address;
  bool has_snap;
  Clock_out snap;
} Log_entry;

// A sink that records every call, in order. Each MIDI triple is stamped with
// body, which a test may set before each tick body, and with the number of
// midi1 calls (the F8s, in the note-length tests) made before it. The
// combined log holds every call of every kind in call order, so the order
// across MIDI and OSC control messages is visible; it has its own overflow
// flag. When watch is set, each midi1 snapshots the Clock_out it points to.
typedef struct {
  Triple midi3[Rec_capacity];
  Usz midi3_body[Rec_capacity];
  Usz midi3_pulses[Rec_capacity];
  Usz midi3_count;
  Usz midi1_count;
  Usz osc_count;
  Usz control_count;
  Usz body;
  bool overflow;
  Log_entry log[Log_capacity];
  Usz log_count;
  bool log_overflow;
  Clock_out const *watch; // snapshotted at each midi1 when set
} Recording;

static inline void rec_log(Recording *r, Log_entry e) {
  if (r->log_count == Log_capacity) {
    r->log_overflow = true;
    return;
  }
  r->log[r->log_count++] = e;
}

static inline void rec_midi3(void *u, int status, int d1, int d2) {
  Recording *r = u;
  rec_log(r, (Log_entry){.kind = Log_midi3, .status = status, .d1 = d1,
                         .d2 = d2});
  if (r->midi3_count == Rec_capacity) {
    r->overflow = true;
    return;
  }
  r->midi3_body[r->midi3_count] = r->body;
  r->midi3_pulses[r->midi3_count] = r->midi1_count;
  r->midi3[r->midi3_count++] = (Triple){status, d1, d2};
}

static inline void rec_midi1(void *u, int byte) {
  Recording *r = u;
  Log_entry e = {.kind = Log_midi1, .status = byte};
  if (r->watch) {
    e.has_snap = true;
    e.snap = *r->watch;
  }
  rec_log(r, e);
  ++r->midi1_count;
}

static inline void rec_osc(void *u, Oevent const *e) {
  Recording *r = u;
  (void)e;
  rec_log(r, (Log_entry){.kind = Log_osc});
  ++r->osc_count;
}

static inline void rec_control(void *u, char const *osc_address) {
  Recording *r = u;
  rec_log(r, (Log_entry){.kind = Log_control, .address = osc_address});
  ++r->control_count;
}

// Empties r and returns a sink that records into it, watching nothing.
static inline Tick_sink rec_sink(Recording *r) {
  memset(r, 0, sizeof *r);
  return (Tick_sink){.u = r,
                     .midi3 = rec_midi3,
                     .midi1 = rec_midi1,
                     .osc = rec_osc,
                     .control = rec_control};
}

// The two sustained notes: ch 0 note 36, then ch 9 note 127, in list order.
static inline void seed_notes(Susnote_list *susnotes) {
  susnote_list_init(susnotes);
  Susnote const seeded[] = {
      {.remaining = 3, .chan_note = 0 << 8 | 36},
      {.remaining = 20, .chan_note = 9 << 8 | 127},
  };
  Usz start_removed, end_removed;
  CHECK(susnote_list_add_notes(susnotes, seeded, ORCA_ARRAY_COUNTOF(seeded),
                               &start_removed, &end_removed));
  CHECK(susnotes->count == 2);
}
