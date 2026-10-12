#pragma once
#include "base.h"

// The clock-follow machine (architecture spine AD-13, AD-15): bOrca's side of
// an external MIDI clock. It is core: it reads no clock, does no I/O and keeps
// no writable globals. The shell's input readers turn F8, FA, FB, FC and F2
// into Transport_events, each stamped with the shell's now_us() at read time,
// and pass them to transport_event one at a time, applying each event's
// actions before the next (AD-15). transport_poll handles the timeouts. The
// shell owns tick_num, is_playing and the clock source; the machine owns the
// pulse phase, the 1 s clock-lost timeout and the tempo estimate.
//
// Pulses. While running, the F8 at phase 0 makes a tick due, and the phase
// then advances mod 6, so every sixth pulse is one Orca tick (a sixteenth).
// Each event makes at most one tick due: a burst of queued F8s never catches
// up by time. FA and F2 set phase 0, so the first F8 after FA runs tick 0; FB
// keeps the phase. F8 while stopped feeds the estimate only and never ticks.
//
// Transitions. start and stop fire only on a change of state: FB while
// running and FC while stopped return nothing. FA while running restarts in
// place (tick 0, phase 0, anchors reset, no stop). F2 is honoured running or
// stopped: it sets the tick and phase 0 and resets the anchors. The song
// position is in sixteenths, which are Orca ticks, with no scaling.
//
// Clock lost. While running, a poll at now - last >= 1 s fires once: stop,
// clock lost and lost = true, where last is the last F8, or the FA/FB that
// started the run. A poll whose now is earlier than last does nothing. After
// a loss, F8 and F2 never restart the machine; only FA or FB does, and each
// of those, like transport_reset, clears lost.
//
// Estimate. A ring holds the last 25 F8 stamps; a full window is 25 F8s
// since the last window reset, which is 24 intervals (FA is not a pulse).
// Each full window gives span = t[n] - t[n - 24], tick_len = (span + 2) / 4
// in integer us (half-up, at least 1) and bpm = 60e6 / max(span, 1) as a
// double clamped to 1.0..999.0, for display and adoption only. The estimate
// is valid from the first full window, and every later window replaces it.
//   Refills (the ring empties, the estimate stays valid): FA, F2, and the FB
//   that starts a run. The last valid estimate is held until the refilled
//   window replaces it. A refill keeps the anchors below.
//   Drops (the ring empties, the estimate becomes invalid): clock lost;
//   transport_reset; any F8 interval of 1 s or more; and 1 s with no F8 in
//   any state, from the later of the last F8 and the last FA/FB/F2, checked
//   by transport_poll while running or while the estimate is valid.
//   An F8 that arrives 1 s or more after the previous one while running
//   drops the estimate but does not stop the machine: only 1 s with no F8
//   at all, seen by transport_poll, is clock lost. So a stall of over a
//   second whose pulses are drained before the poll resumes mid-phase with
//   the estimate dropped and no loss.
//   A stamp earlier than the last empties the ring as a refill does.
//
// Display value. On becoming valid, shown = bpm rounded half-up, 1..999.
// After that, shown changes only when |bpm - shown| >= 1.0 on 24 consecutive
// F8s, one beat; the count resets on any in-band F8, refill or drop. A
// change of validity also sets shown_changed, so the HUD redraws. That band
// keeps the jitter of a BLE link from flickering the HUD.

typedef enum {
  Transport_clock,    // F8
  Transport_start,    // FA
  Transport_continue, // FB
  Transport_stop,     // FC
  Transport_songpos,  // F2, with its position in value
} Transport_type;

typedef struct {
  Transport_type type;
  U16 value; // Transport_songpos: 0..16383 sixteenths, Orca ticks
  U64 stamp; // the shell's now_us() at read time (AD-13)
} Transport_event;

// What the shell does with an event or a poll, in this order (AD-15): stop or
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
  U64 last_run;    // clock lost: the last F8 or the FA/FB that started the
                   // run; holds a value while running
  bool has_anchor; // last_anchor holds a value
  U64 last_anchor; // the idle drop: the later of the last F8 and the last
                   // FA, FB (that started a run) or F2
  bool has_pulse;  // last_pulse holds a value
  U64 last_pulse;  // the interval drop: the last F8, kept across refills
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
