// The clock-follow machine: core code that returns actions to the shell. See
// transport.h.
#include "transport.h"
#include "base.h"

static void ring_clear(Transport *m) {
  m->ring_count = 0;
  m->ring_next = 0;
}

// A refill: the ring empties and the estimate is held (transport.h).
static void refill(Transport *m) {
  ring_clear(m);
  m->out_band = 0;
}

// A drop: the ring empties and the estimate becomes invalid. A change of
// validity sets shown_changed.
static void drop(Transport *m, Transport_actions *out) {
  ring_clear(m);
  m->out_band = 0;
  if (m->est_valid) {
    m->est_valid = false;
    out->shown_changed = true;
  }
}

// The later of the anchor and stamp: a stale stamp never moves it back.
static void anchor(Transport *m, U64 stamp) {
  if (!m->has_anchor || stamp > m->last_anchor) {
    m->has_anchor = true;
    m->last_anchor = stamp;
  }
}

static void fill_estimate(Transport const *m, Transport_actions *out) {
  out->est_valid = m->est_valid;
  out->tick_len = m->tick_len;
  out->bpm = m->bpm;
  out->shown = m->shown;
}

// bpm is 1.0..999.0, so the half-up rounding stays in 1..999.
static U16 round_bpm(double bpm) { return (U16)(bpm + 0.5); }

static void ring_push(Transport *m, U64 stamp) {
  m->ring[m->ring_next] = stamp;
  m->ring_next = (m->ring_next + 1) % Transport_window;
  if (m->ring_count < Transport_window)
    ++m->ring_count;
}

static void on_clock(Transport *m, U64 stamp, Transport_actions *out) {
  if (m->has_pulse) {
    if (stamp < m->last_pulse)
      refill(m); // a stamp earlier than the last: never underflows
    else if (stamp - m->last_pulse >= Transport_lost_us)
      drop(m, out); // the interval drop
  }
  ring_push(m, stamp);
  m->has_pulse = true;
  m->last_pulse = stamp;
  anchor(m, stamp);
  if (m->running && stamp > m->last_run)
    m->last_run = stamp;
  if (m->ring_count == Transport_window) {
    // Stamps never go back within a ring (a stale one empties it first), so
    // the oldest, at ring_next, is at or before this one.
    U64 span = stamp - m->ring[m->ring_next];
    U64 tick_len = (span + 2) / 4;
    m->tick_len = tick_len > 0 ? tick_len : 1;
    double bpm = 60e6 / (double)(span > 0 ? span : 1);
    if (bpm < 1.0)
      bpm = 1.0;
    if (bpm > 999.0)
      bpm = 999.0;
    m->bpm = bpm;
    if (!m->est_valid) {
      m->est_valid = true;
      m->shown = round_bpm(bpm);
      m->out_band = 0;
      out->shown_changed = true;
    } else {
      double diff = bpm - (double)m->shown;
      if (diff < 0.0)
        diff = -diff;
      if (diff >= 1.0) {
        if (++m->out_band >= Transport_hysteresis) {
          m->shown = round_bpm(bpm);
          m->out_band = 0;
          out->shown_changed = true;
        }
      } else {
        m->out_band = 0;
      }
    }
  }
  if (m->running) {
    if (m->phase == 0)
      out->tick_due = true;
    m->phase = (U8)((m->phase + 1) % 6);
  }
}

static void on_start(Transport *m, U64 stamp, Transport_actions *out) {
  bool was_running = m->running;
  m->running = true;
  m->lost = false;
  m->phase = 0;
  m->last_run = stamp;
  anchor(m, stamp);
  refill(m);
  out->set_tick = true;
  out->tick = 0;
  out->reset_anchors = true;
  if (!was_running)
    out->start = true;
}

static void on_continue(Transport *m, U64 stamp, Transport_actions *out) {
  if (m->running)
    return; // redundant: nothing, and the ring is left alone
  m->running = true;
  m->lost = false;
  m->last_run = stamp;
  anchor(m, stamp);
  refill(m);
  out->start = true;
}

// The reader lost queued input: input arrived and was flushed
// (usually pulses). It counts as pulses that arrived. While running it
// restarts the clock-lost timeout from its stamp, so with no F8 after it
// clock lost comes 1 s later; a stale stamp never moves it back. It moves
// the idle-drop anchor, and it refills, since the window's stamps are from
// before the flush. It stands as the last pulse for the interval drop, as an
// F8 does: the next F8 measures from it, not from a pulse before the flush,
// and a gap of 1 s or more after it drops the estimate as after any pulse.
// It sets no action: it never ticks, never starts, never clears lost and
// keeps the phase, so the flushed pulses are not counted.
static void on_overflow(Transport *m, U64 stamp) {
  if (m->running && stamp > m->last_run)
    m->last_run = stamp;
  anchor(m, stamp);
  refill(m);
  m->has_pulse = true;
  m->last_pulse = stamp;
}

static void on_stop(Transport *m, Transport_actions *out) {
  if (!m->running)
    return; // redundant: nothing, and the ring is left alone
  m->running = false;
  out->stop = true;
}

static void on_songpos(Transport *m, U16 value, U64 stamp,
                       Transport_actions *out) {
  m->phase = 0;
  anchor(m, stamp);
  refill(m);
  out->set_tick = true;
  out->tick = value > 16383 ? 16383 : value;
  out->reset_anchors = true;
}

void transport_reset(Transport *m) {
  *m = (Transport){.running = false};
}

void transport_event(Transport *m, Transport_event const *e,
                     Transport_actions *out) {
  *out = (Transport_actions){.tick_due = false};
  switch (e->type) {
  case Transport_clock:
    on_clock(m, e->stamp, out);
    break;
  case Transport_start:
    on_start(m, e->stamp, out);
    break;
  case Transport_continue:
    on_continue(m, e->stamp, out);
    break;
  case Transport_stop:
    on_stop(m, out);
    break;
  case Transport_songpos:
    on_songpos(m, e->value, e->stamp, out);
    break;
  case Transport_overflow:
    on_overflow(m, e->stamp);
    break;
  }
  fill_estimate(m, out);
}

void transport_poll(Transport *m, U64 now, Transport_actions *out) {
  *out = (Transport_actions){.tick_due = false};
  // Clock lost, once: running becomes false, so a later poll does nothing.
  if (m->running && now >= m->last_run &&
      now - m->last_run >= Transport_lost_us) {
    m->running = false;
    m->lost = true;
    out->stop = true;
    out->clock_lost = true;
    drop(m, out);
  }
  // The idle drop, in any state, from the later anchor.
  if (m->est_valid && m->has_anchor && now >= m->last_anchor &&
      now - m->last_anchor >= Transport_lost_us)
    drop(m, out);
  fill_estimate(m, out);
}

bool transport_next_deadline(Transport const *m, U64 *t) {
  // While running the clock-lost deadline is the earlier of the two: the
  // idle anchor is never before last_run.
  if (m->running) {
    *t = m->last_run + Transport_lost_us;
    return true;
  }
  if (m->est_valid && m->has_anchor) {
    *t = m->last_anchor + Transport_lost_us;
    return true;
  }
  return false;
}

bool transport_parse(int status, int d1, int d2, U64 stamp,
                     Transport_event *out) {
  Transport_type type;
  U16 value = 0;
  switch (status) {
  case 0xF8:
    type = Transport_clock;
    break;
  case 0xFA:
    type = Transport_start;
    break;
  case 0xFB:
    type = Transport_continue;
    break;
  case 0xFC:
    type = Transport_stop;
    break;
  case 0xF2:
    type = Transport_songpos;
    value = (U16)(((unsigned)d1 & 0x7Fu) | (((unsigned)d2 & 0x7Fu) << 7u));
    break;
  default:
    return false;
  }
  *out = (Transport_event){.type = type, .value = value, .stamp = stamp};
  return true;
}
