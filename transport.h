#pragma once
#include "base.h"

// The clock-follow machine: zOrca's side of an external MIDI clock. It is core:
// it reads no clock, does no I/O and keeps no writable globals. The shell's
// input readers turn F8, FA, FB, FC and F2, and an overflow of their input
// queue, into Transport_events, each stamped with the shell's now_us() at read
// time, and pass them to transport_event one at a time, applying each event's
// actions before the next. transport_poll handles the timeouts. The shell owns
// tick_num, is_playing and the clock source; the machine owns the pulse phase,
// the 1 s clock-lost timeout and the tempo estimate.
//
// Pulses. While running, the F8 at phase 0 makes a tick due, and the phase
// then advances mod 6, so every sixth pulse is one Orca tick (a sixteenth).
// Each event makes at most one tick due: a burst of queued F8s never catches
// up by time. FA and F2 set phase 0, so the first F8 after FA runs tick 0; FB
// and an overflow keep the phase. F8 while stopped feeds the estimate only
// and never ticks.
//
// Transitions. start and stop fire only on a change of state: FB while
// running and FC while stopped return nothing. FA while running restarts in
// place (tick 0, phase 0, anchors reset, no stop). F2 is honoured running or
// stopped: it sets the tick and phase 0 and resets the anchors. The song
// position is in sixteenths, which are Orca ticks, with no scaling.
//
// Clock lost. While running, a poll at now - last >= 1 s fires once: stop,
// clock lost and lost = true, where last is the last F8, or the FA/FB that
// started the run. An overflow while running restarts the 1 s timeout from
// its stamp, so with no F8 after it clock lost comes 1 s later; a stale one
// never moves it back. A poll whose now is earlier than last does nothing.
// After a loss, F8, F2 and an overflow never restart the machine; only FA or
// FB does, and each of those, like transport_reset, clears lost.
//
// Estimate. A ring holds the last 25 F8 stamps; a full window is 25 F8s
// since the last window reset, which is 24 intervals (FA is not a pulse).
// Each full window gives span = t[n] - t[n - 24], tick_len = (span + 2) / 4
// in integer us (half-up, at least 1) and bpm = 60e6 / max(span, 1) as a
// double clamped to 1.0..999.0, for display and adoption only. The estimate
// is valid from the first full window, and every later window replaces it.
//   Refills (the ring empties, the estimate stays valid): FA, F2, the FB
//   that starts a run, and an overflow. The last valid estimate is held
//   until the refilled window replaces it. A refill keeps the anchors below.
//   Drops (the ring empties, the estimate becomes invalid): clock lost;
//   transport_reset; any F8 interval of 1 s or more; and 1 s with no F8 in
//   any state, from the later of the last F8 and the last FA/FB/F2 or
//   overflow, checked by transport_poll while running or while the estimate
//   is valid. For the interval drop an overflow stands as the last pulse, as
//   an F8 does: the first F8 after it measures from it, and a gap of 1 s or
//   more after it drops the estimate.
//   An F8 that arrives 1 s or more after the previous one while running
//   drops the estimate but does not stop the machine: only 1 s with no F8
//   at all, seen by transport_poll, is clock lost. So a stall of over a
//   second whose pulses are drained before the poll resumes mid-phase with
//   the estimate dropped and no loss. Past an ALSA reader's input pool
//   (zOrca's client asks for 500 events, Alsa_input_pool in tui_main.c:
//   about 10 s of F8s alone at 120 BPM, less with other input) the kernel
//   flushes every queued event, Start, Stop and song position included; the
//   ALSA reader then passes an overflow: input arrived and was flushed
//   (usually pulses). The machine keeps running with the estimate held and
//   the phase kept, and the flushed pulses never tick: it follows behind by
//   the flushed pulses until the next FA or F2. A flushed FC leaves it
//   running, while pulses arrive, until the next FA or FC, and a flushed FA
//   or FB leaves a stopped machine stopped until the next FA or FB. With no
//   F8 after the overflow, clock lost comes 1 s after it. PortMidi on Linux
//   never reports that flush, so a PortMidi build still loses the clock
//   there.
//   A stamp earlier than the last empties the ring as a refill does.
//
// Display value. On becoming valid, shown = bpm rounded half-up, 1..999.
// After that, shown changes only when |bpm - shown| >= 1.0 on 24 consecutive
// F8s, one beat; the count resets on any in-band F8, refill or drop. A
// change of validity also sets shown_changed, so the HUD redraws. That band
// keeps the jitter of a BLE link from flickering the HUD.

// The clock source, the clock_source setting. Internal is the default and the
// 4f349cd timing path; External follows a MIDI clock through the transport
// machine.
typedef enum {
  Clock_source_internal = 0,
  Clock_source_external = 1,
} Clock_source;

// The names orca.conf's clock_source key and --clock-source take: "internal"
// and "external", exactly. False, leaving *out alone, for anything else.
static inline bool clock_source_from_name(char const *name, Clock_source *out) {
  if (strcmp(name, "internal") == 0) {
    *out = Clock_source_internal;
    return true;
  }
  if (strcmp(name, "external") == 0) {
    *out = Clock_source_external;
    return true;
  }
  return false;
}

static inline char const *clock_source_name(Clock_source s) {
  return s == Clock_source_external ? "external" : "internal";
}

typedef enum {
  Transport_clock,    // F8
  Transport_start,    // FA
  Transport_continue, // FB
  Transport_stop,     // FC
  Transport_songpos,  // F2, with its position in value
  Transport_overflow, // the reader lost queued input (no MIDI byte)
} Transport_type;

typedef struct {
  Transport_type type;
  U16 value; // Transport_songpos: 0..16383 sixteenths, Orca ticks
  U64 stamp; // the shell's now_us() at read time
} Transport_event;

// What the shell does with an event or a poll, in this order: stop or
// clock lost (release notes, cancel glides, pause, keep tick_num); set tick;
// reset anchors; start; then one tick body if a tick is due and the shell is
// playing. No event sets conflicting fields. The estimate fields describe the
// machine's estimate after the call: tick_len, bpm and shown hold when
// est_valid is set, and shown_changed says the HUD's value, or its validity,
// changed in this call.
typedef struct {
  bool tick_due, set_tick, reset_anchors, start, stop, clock_lost;
  bool est_valid, shown_changed;
  Usz tick;     // with set_tick: 0 for FA, the song position for F2
  U64 tick_len; // with est_valid: one tick at the estimate, in us
  double bpm;   // with est_valid: 1.0..999.0
  U16 shown;    // with est_valid: the HUD's value, 1..999
} Transport_actions;

enum {
  Transport_window = 25,       // F8 stamps in a full window: 24 intervals
  Transport_lost_us = 1000000, // clock lost, the idle drop, the interval drop
  Transport_hysteresis = 24,   // out-of-band F8s in a row before shown moves
};

// The machine. transport_reset sets every field; the shell holds one by
// value. Explicit flags say which stamps hold a value; no stamp is a
// sentinel.
typedef struct {
  bool running, lost;
  U8 phase;        // 0..5; the F8 at 0 makes a tick due while running
  U64 last_run;    // clock lost: the last F8, the FA/FB that started the
                   // run or a later overflow; holds a value while running
  bool has_anchor; // last_anchor holds a value
  U64 last_anchor; // the idle drop: the later of the last F8 and the last
                   // FA, FB (that started a run), F2 or overflow
  bool has_pulse;  // last_pulse holds a value
  U64 last_pulse;  // the interval drop: the last F8 or overflow, kept
                   // across refills
  U64 ring[Transport_window];
  Usz ring_count; // 0..Transport_window
  Usz ring_next;  // the slot the next F8 writes; the oldest when full
  bool est_valid;
  U64 tick_len;
  double bpm;
  U16 shown;
  Usz out_band; // consecutive F8s with |bpm - shown| >= 1.0
} Transport;

// The initial state: stopped, phase 0, not lost, no estimate, empty window,
// no stamps. A clock-source switch calls it.
void transport_reset(Transport *m);

// The only input. Applies e and fills *out, which is cleared first.
void transport_event(Transport *m, Transport_event const *e,
                     Transport_actions *out);

// The timeouts: clock lost while running, and the idle drop. now is a fresh
// shell read made after every event it follows. Fills *out, cleared first.
void transport_poll(Transport *m, U64 now, Transport_actions *out);

// A query, not an input: the time at which transport_poll next has something
// to do, as ccout_next_deadline is for the CC engine. True with *t = last_run
// + 1 s while running, or the idle drop's time while the estimate is valid;
// false when neither (stopped with no estimate).
bool transport_next_deadline(Transport const *m, U64 *t);

// Maps a MIDI message to an event: F8, FA, FB, FC and F2 (song position =
// (d1 & 0x7F) | (d2 & 0x7F) << 7). Returns false, leaving *out alone, for
// every other status. The PortMidi reader uses it.
bool transport_parse(int status, int d1, int d2, U64 stamp,
                     Transport_event *out);
