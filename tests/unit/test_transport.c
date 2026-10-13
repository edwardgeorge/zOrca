// Unit tests for transport.c (spec item I6, CAP-18; spine AD-13, AD-15): the
// machine driven by synthetic events, a model shell that applies each
// event's actions in the shell's order over a real tick_body, the estimate
// with its refills and drops, the display value's hysteresis, the deadline
// query, the parser, and the re-arm, clock-out (Clock_out) and HUD helpers
// of midi_in.h. Stamps are U64 us, as the shell's now_us(); a pulse is
// 20 833 us, one F8 at 120 BPM.
// The model is this file's own; the recording sink and the two-note
// fixture are recorder.h's, shared with test_tick.c.
#include "../../ccout.h"
#include "../../gbuffer.h"
#include "../../midi_in.h"
#include "../../opstate.h"
#include "../../tick.h"
#include "../../transport.h"
#include "../../vmio.h"
#include "recorder.h"

enum { Pulse_us = 20833, Lost_us = Transport_lost_us };
enum { Bodies_capacity = 64, Hist_capacity = 128 };

static bool triple_is(Triple t, int status, int d1, int d2) {
  return t.status == status && t.d1 == d1 && t.d2 == d2;
}

// The model shell: the grid, store, lists, sustained notes and CC engine a
// tick body needs, tick_num and the playing flag, which the machine's
// actions drive as tui_main.c's do (AD-15): stop or clock lost is
// tick_stop with the clock-out gate shut, as ged_set_playing(false) under
// External (note-offs, glides cancelled, /orca/stopped, no FC), then pause,
// keeping tick_num; set tick; reset
// anchors; start; then one tick body if a tick is due and the model is
// playing. tick_len is the estimate while valid, else 120 BPM's. The model
// also counts what the shell would send as /orca/bpm: the shown value on
// shown_changed while the estimate is valid, nothing on becoming invalid.
enum { Model_h = 2, Model_w = 24 };

typedef struct {
  Glyph grid[Model_h * Model_w];
  Mark marks[Model_h * Model_w];
  Opstate_store store;
  Oevent_list tick_list, engine_list, seed_list;
  Susnote_list susnotes;
  Ccout_engine ccout;
  Usz tick_num;
  bool playing;
  U64 tick_len;
  Transport m;
  Recording rec;
  Tick_sink sink;
  Usz bodies;                      // tick bodies run
  Usz body_tick[Bodies_capacity];  // the tick each body ran
  Usz body_pulse[Bodies_capacity]; // the F8, counted from 1, it ran on
  Usz pulses;                      // F8s fed so far
  Usz osc_bpm_count;
  U16 osc_bpm_last;
  Usz shown_changes; // actions with shown_changed set
  Transport_actions last;
} Model;

static void model_init(Model *md) {
  memset(md, 0, sizeof *md);
  memset(md->grid, '.', sizeof md->grid);
  opstate_init(&md->store);
  oevent_list_init(&md->tick_list);
  oevent_list_init(&md->engine_list);
  oevent_list_init(&md->seed_list);
  susnote_list_init(&md->susnotes);
  ccout_init(&md->ccout);
  md->tick_len = tick_len_us(120);
  md->sink = rec_sink(&md->rec);
  transport_reset(&md->m);
}

static void model_deinit(Model *md) {
  susnote_list_deinit(&md->susnotes);
  oevent_list_deinit(&md->tick_list);
  oevent_list_deinit(&md->engine_list);
  oevent_list_deinit(&md->seed_list);
  opstate_free(&md->store);
}

// Writes text into the grid at (y, x).
static void put(Model *md, Usz y, Usz x, char const *text) {
  memcpy(md->grid + y * Model_w + x, text, strlen(text));
}

// The CAP-18 stop grid: a D banging a note every tick, with the bang landing
// west of the ':' (D1 over .:03C.f), plus the glide fixture of the FA-while-
// running row (D1 over .!3..7g5): a glide on channel 3, control 7, at rate
// 5. seed_glide gives that controller a last value, so the first bang starts
// a glide rather than jumping (ccout.h, rule 2).
static void cap18_grid(Model *md) {
  put(md, 0, 0, "D1");
  put(md, 1, 1, ":03C.f");
  put(md, 0, 8, "D1");
  put(md, 1, 9, "!3..7g5");
}

static void seed_glide(Model *md) {
  ccout_submit_instant(&md->ccout, 3, 7, 0, 0, &md->seed_list);
}

static void model_apply(Model *md, Transport_actions const *act, U64 now) {
  if (act->stop || act->clock_lost) {
    tick_stop(&md->sink, &md->susnotes, &md->ccout, false);
    md->playing = false;
  }
  if (act->set_tick)
    md->tick_num = act->tick;
  if (act->reset_anchors)
    opstate_reset_anchors(&md->store);
  if (act->start)
    md->playing = true;
  if (act->tick_due && md->playing) {
    if (md->bodies < Bodies_capacity) {
      md->body_tick[md->bodies] = md->tick_num;
      md->body_pulse[md->bodies] = md->pulses;
    }
    ++md->bodies;
    Tick_ctx const ctx = {.gbuffer = md->grid,
                          .mbuffer = md->marks,
                          .height = Model_h,
                          .width = Model_w,
                          .random_seed = 0,
                          .opstate = &md->store,
                          .dialect = Orca_dialect_borca,
                          .tick_num = &md->tick_num,
                          .tick_list = &md->tick_list,
                          .engine_list = &md->engine_list,
                          .susnotes = &md->susnotes,
                          .ccout = &md->ccout,
                          .now_us = now,
                          .tick_len = md->tick_len};
    tick_body(&ctx, &md->sink);
  }
  md->tick_len = act->est_valid ? act->tick_len : tick_len_us(120);
  if (act->shown_changed) {
    ++md->shown_changes;
    if (act->est_valid) {
      ++md->osc_bpm_count;
      md->osc_bpm_last = act->shown;
    }
  }
  md->last = *act;
}

// Feeds one event. The tick body's time is a fresh read after the stamp,
// never the stamp itself (AD-13): here one us later.
static void model_event(Model *md, Transport_type type, U16 value,
                        U64 stamp) {
  Transport_event const e = {.type = type, .value = value, .stamp = stamp};
  if (type == Transport_clock)
    ++md->pulses;
  Transport_actions act;
  transport_event(&md->m, &e, &act);
  model_apply(md, &act, stamp + 1);
}

static void pulse(Model *md, U64 stamp) {
  model_event(md, Transport_clock, 0, stamp);
}

static void model_poll(Model *md, U64 now) {
  Transport_actions act;
  transport_poll(&md->m, now, &act);
  model_apply(md, &act, now);
}

// n F8s, one pulse apart, after `from`; returns the last stamp.
static U64 clocks_from(Model *md, U64 from, Usz n) {
  U64 stamp = from;
  for (Usz i = 0; i < n; ++i) {
    stamp += Pulse_us;
    pulse(md, stamp);
  }
  return stamp;
}

static bool no_actions(Transport_actions const *a) {
  return !a->tick_due && !a->set_tick && !a->reset_anchors && !a->start &&
         !a->stop && !a->clock_lost && !a->shown_changed;
}

// A stamp history for feeding windows of a chosen span: each new stamp is
// `span` after the one 24 back, so that F8's window is exactly span wide.
typedef struct {
  U64 stamps[Hist_capacity];
  Usz n;
} Hist;

static void hist_clock(Model *md, Hist *h, U64 stamp) {
  CHECK(h->n < Hist_capacity);
  CHECK(h->n == 0 || stamp >= h->stamps[h->n - 1]);
  h->stamps[h->n++] = stamp;
  pulse(md, stamp);
}

static void feed_span(Model *md, Hist *h, U64 span) {
  CHECK(h->n >= 24);
  hist_clock(md, h, h->stamps[h->n - 24] + span);
}

// FA at 0, then 24 F8s `interval` apart and a 25th closing a window of
// `span`: the estimate becomes valid at that window.
static void start_valid(Model *md, Hist *h, U64 interval, U64 span) {
  model_event(md, Transport_start, 0, 0);
  for (Usz k = 1; k <= 24; ++k)
    hist_clock(md, h, k * interval);
  feed_span(md, h, span);
  CHECK(md->m.est_valid);
}

static bool near(double a, double b, double tol) {
  double d = a - b;
  return d < tol && d > -tol;
}

// FA, then 24 F8s: tick bodies at ticks 0 to 3 on pulses 1, 7, 13 and 19,
// and tick_num ends at 4.
void test_transport_cap18_start(void) {
  Model md;
  model_init(&md);
  cap18_grid(&md);
  model_event(&md, Transport_start, 0, 0);
  CHECK(md.last.start && md.last.set_tick && md.last.reset_anchors);
  CHECK(md.playing);
  clocks_from(&md, 0, 24);
  CHECK(md.bodies == 4);
  CHECK(md.body_tick[0] == 0 && md.body_tick[1] == 1 && md.body_tick[2] == 2 &&
        md.body_tick[3] == 3);
  CHECK(md.body_pulse[0] == 1 && md.body_pulse[1] == 7 &&
        md.body_pulse[2] == 13 && md.body_pulse[3] == 19);
  CHECK(md.tick_num == 4);
  CHECK(!md.rec.overflow && md.rec.midi1_count == 0);
  model_deinit(&md);
}

// F2 (10 00), FB, 6 F8s: exactly one body, at tick 16; tick_num 17.
void test_transport_cap18_songpos(void) {
  Model md;
  model_init(&md);
  Transport_event e;
  CHECK(transport_parse(0xF2, 0x10, 0x00, 7, &e));
  CHECK(e.type == Transport_songpos && e.value == 16 && e.stamp == 7);
  model_event(&md, Transport_songpos, e.value, 7);
  CHECK(md.last.set_tick && md.last.tick == 16 && md.last.reset_anchors);
  CHECK(!md.last.start && !md.playing && md.tick_num == 16);
  model_event(&md, Transport_continue, 0, 8);
  CHECK(md.last.start && md.playing);
  clocks_from(&md, 8, 6);
  CHECK(md.bodies == 1);
  CHECK(md.body_tick[0] == 16);
  CHECK(md.tick_num == 17);
  model_deinit(&md);
}

// FA + 6 F8s sound the note and start the glide; FC releases the note and
// ends the glide; 12 more F8s run nothing.
void test_transport_cap18_stop(void) {
  Model md;
  model_init(&md);
  cap18_grid(&md);
  seed_glide(&md);
  model_event(&md, Transport_start, 0, 0);
  U64 last = clocks_from(&md, 0, 6);
  CHECK(md.bodies == 1 && md.tick_num == 1);
  CHECK(md.rec.midi3_count == 1 && triple_is(md.rec.midi3[0], 0x90, 36, 127));
  CHECK(md.susnotes.count == 1);
  CHECK(md.ccout.active_count > 0);
  model_event(&md, Transport_stop, 0, last + Pulse_us);
  CHECK(md.last.stop && !md.last.clock_lost);
  CHECK(!md.playing);
  CHECK(md.rec.midi3_count == 2 && triple_is(md.rec.midi3[1], 0x80, 36, 0));
  CHECK(md.susnotes.count == 0);
  CHECK(md.ccout.active_count == 0);
  clocks_from(&md, last + Pulse_us, 12);
  CHECK(md.bodies == 1 && md.tick_num == 1);
  CHECK(!md.playing);
  CHECK(md.rec.midi3_count == 2 && !md.rec.overflow);
  model_deinit(&md);
}

// FA + 3 F8s, then the deadline is last + 1 s: a poll 1 us before it does
// nothing, the poll at it stops with clock lost, once, and nothing follows.
void test_transport_cap18_clock_lost(void) {
  Model md;
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  U64 last = clocks_from(&md, 0, 3);
  U64 t = 0;
  CHECK(transport_next_deadline(&md.m, &t) && t == last + Lost_us);
  model_poll(&md, last + Lost_us - 1);
  CHECK(no_actions(&md.last) && md.playing && md.m.running && !md.m.lost);
  model_poll(&md, last + Lost_us);
  CHECK(md.last.stop && md.last.clock_lost);
  CHECK(!md.playing && !md.m.running && md.m.lost);
  model_poll(&md, last + 2 * Lost_us);
  CHECK(no_actions(&md.last));
  CHECK(!transport_next_deadline(&md.m, &t));
  model_deinit(&md);
}

// A poll earlier than the last stamp does nothing and never underflows.
void test_transport_stale_poll(void) {
  Model md;
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  pulse(&md, Pulse_us);
  U64 before = 0, after = 0;
  CHECK(transport_next_deadline(&md.m, &before));
  model_poll(&md, Pulse_us - 1);
  CHECK(no_actions(&md.last) && md.playing && md.m.running);
  CHECK(transport_next_deadline(&md.m, &after) && after == before);
  model_deinit(&md);
}

// After a loss, F8 and F2 never restart the machine; FB does, from the tick
// F2 set.
void test_transport_restart_after_loss(void) {
  Model md;
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  U64 last = clocks_from(&md, 0, 3);
  model_poll(&md, last + Lost_us);
  CHECK(md.last.clock_lost && md.bodies == 1);
  U64 t = last + Lost_us;
  t = clocks_from(&md, t, 12);
  CHECK(md.bodies == 1 && !md.playing && !md.m.running);
  model_event(&md, Transport_songpos, 8, t);
  CHECK(md.last.set_tick && md.last.tick == 8 && !md.last.start);
  CHECK(md.tick_num == 8 && md.m.lost);
  t = clocks_from(&md, t, 6);
  CHECK(md.bodies == 1 && !md.playing);
  model_event(&md, Transport_continue, 0, t);
  CHECK(md.last.start && md.playing && !md.m.lost);
  clocks_from(&md, t, 6);
  CHECK(md.bodies == 2 && md.body_tick[1] == 8);
  model_deinit(&md);
}

// FA, 3 F8s, FC, FB, 4 F8s: FB keeps the phase, so pulses 4 to 6 run
// nothing and pulse 7 runs tick 1.
void test_transport_continue_keeps_phase(void) {
  Model md;
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  U64 t = clocks_from(&md, 0, 3);
  CHECK(md.bodies == 1 && md.body_pulse[0] == 1 && md.body_tick[0] == 0);
  model_event(&md, Transport_stop, 0, t);
  model_event(&md, Transport_continue, 0, t);
  CHECK(md.last.start && md.playing);
  clocks_from(&md, t, 4);
  CHECK(md.bodies == 2);
  CHECK(md.body_pulse[1] == 7 && md.body_tick[1] == 1);
  model_deinit(&md);
}

// 25 F8s with no FA feed the estimate only: valid, no ticks, phase unchanged.
void test_transport_clock_while_stopped(void) {
  Model md;
  model_init(&md);
  U8 phase = md.m.phase;
  clocks_from(&md, 0, 25);
  CHECK(md.m.est_valid && md.last.est_valid);
  CHECK(md.bodies == 0 && !md.playing && !md.m.running);
  CHECK(md.m.phase == phase);
  model_deinit(&md);
}

// FA while running, at tick 9 with a glide active: tick 0, anchors reset, no
// stop; tick 0 runs on the next F8; the glide stays active.
void test_transport_start_while_running(void) {
  Model md;
  model_init(&md);
  cap18_grid(&md);
  seed_glide(&md);
  model_event(&md, Transport_start, 0, 0);
  U64 t = clocks_from(&md, 0, 54);
  CHECK(md.tick_num == 9 && md.bodies == 9);
  CHECK(md.ccout.active_count == 1);
  Usz notes_before = md.rec.midi3_count;
  model_event(&md, Transport_start, 0, t);
  CHECK(md.last.set_tick && md.last.tick == 0 && md.last.reset_anchors);
  CHECK(!md.last.stop && !md.last.start && !md.last.tick_due);
  CHECK(md.playing && md.tick_num == 0);
  CHECK(md.ccout.active_count == 1);
  CHECK(md.rec.midi3_count == notes_before); // no note-offs: no stop
  pulse(&md, t + Pulse_us);
  CHECK(md.bodies == 10 && md.body_tick[9] == 0 && md.tick_num == 1);
  CHECK(md.ccout.active_count == 1);
  model_deinit(&md);
}

// FB while running and FC while stopped return nothing and leave a partly
// filled ring alone.
void test_transport_redundant(void) {
  Model md;
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  U64 t = clocks_from(&md, 0, 10);
  CHECK(md.m.ring_count == 10);
  model_event(&md, Transport_continue, 0, t);
  CHECK(no_actions(&md.last) && md.m.ring_count == 10 && md.playing);
  model_event(&md, Transport_stop, 0, t);
  CHECK(md.last.stop && !md.playing);
  t = clocks_from(&md, t, 5);
  CHECK(md.m.ring_count == 15);
  model_event(&md, Transport_stop, 0, t);
  CHECK(no_actions(&md.last) && md.m.ring_count == 15 && !md.playing);
  model_deinit(&md);
}

// FA, then 12 F8s with one stamp: exactly 2 ticks due, no catch-up.
void test_transport_burst(void) {
  Model md;
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  for (int i = 0; i < 12; ++i)
    pulse(&md, 0);
  CHECK(md.bodies == 2);
  CHECK(md.body_pulse[0] == 1 && md.body_pulse[1] == 7);
  CHECK(md.tick_num == 2);
  model_deinit(&md);
}

// The estimate: invalid through 24 F8s; a window of 500 000 us gives
// tick_len 125 000 and bpm 120; 499 994 gives 124 999 (half-up); 25 F8s with
// one stamp give tick_len 1 and shown 999.
void test_transport_estimate(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  for (Usz k = 1; k <= 24; ++k)
    hist_clock(&md, &h, k * Pulse_us);
  CHECK(!md.m.est_valid && !md.last.est_valid && md.osc_bpm_count == 0);
  feed_span(&md, &h, 500000);
  CHECK(md.last.est_valid && md.last.tick_len == 125000);
  CHECK(near(md.last.bpm, 120.0, 1e-9) && md.last.shown == 120);
  CHECK(md.last.shown_changed && md.osc_bpm_count == 1 &&
        md.osc_bpm_last == 120);
  CHECK(md.tick_len == 125000);
  feed_span(&md, &h, 499994);
  CHECK(md.last.est_valid && md.last.tick_len == 124999);
  CHECK(md.last.shown == 120 && !md.last.shown_changed);
  U64 one = h.stamps[h.n - 1];
  for (int i = 0; i < 25; ++i)
    hist_clock(&md, &h, one);
  CHECK(md.last.est_valid && md.last.tick_len == 1);
  CHECK(near(md.last.bpm, 999.0, 1e-9) && md.last.shown == 999);
  CHECK(md.tick_len == 1 && md.osc_bpm_last == 999);
  model_deinit(&md);
}

// A refill (FA, FB after FC, F2) empties the ring and holds the estimate for
// the HUD and tick_len; the 25th F8 after it replaces tick_len and bpm at
// once, and shown follows at the 48th through the hysteresis.
void test_transport_window_refills(void) {
  enum { New_interval = 17857 }; // about 140 BPM
  for (int kind = 0; kind < 3; ++kind) {
    Model md;
    Hist h = {.n = 0};
    model_init(&md);
    start_valid(&md, &h, Pulse_us, 500000);
    CHECK(md.last.shown == 120 && md.tick_len == 125000);
    U64 t = h.stamps[h.n - 1] + Pulse_us;
    if (kind == 0) {
      model_event(&md, Transport_start, 0, t);
    } else if (kind == 1) {
      model_event(&md, Transport_stop, 0, t);
      model_event(&md, Transport_continue, 0, t);
    } else {
      model_event(&md, Transport_songpos, 4, t);
    }
    CHECK(md.m.ring_count == 0);
    CHECK(md.last.est_valid && md.last.tick_len == 125000 &&
          md.last.shown == 120 && !md.last.shown_changed);
    for (Usz k = 1; k <= 24; ++k) {
      hist_clock(&md, &h, t + k * New_interval);
      CHECK(md.last.est_valid && md.last.tick_len == 125000);
      CHECK(md.last.shown == 120 && !md.last.shown_changed);
      CHECK(md.tick_len == 125000);
    }
    hist_clock(&md, &h, t + 25 * New_interval);
    CHECK(md.last.est_valid && md.last.tick_len == 107142);
    CHECK(near(md.last.bpm, 140.0, 0.01));
    CHECK(md.last.shown == 120 && !md.last.shown_changed);
    CHECK(md.tick_len == 107142);
    for (Usz k = 26; k <= 47; ++k) {
      hist_clock(&md, &h, t + k * New_interval);
      CHECK(md.last.shown == 120 && !md.last.shown_changed);
    }
    hist_clock(&md, &h, t + 48 * New_interval);
    CHECK(md.last.shown == 140 && md.last.shown_changed);
    CHECK(md.osc_bpm_count == 2 && md.osc_bpm_last == 140);
    model_deinit(&md);
  }
}

// A refill keeps the anchors: after FC and an FB 950 000 us after the last
// F8, the poll 1 s after that F8 does nothing, and the poll 1 s after the FB
// is clock lost, which drops the estimate.
void test_transport_refill_anchor(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  U64 last = h.stamps[h.n - 1];
  model_event(&md, Transport_stop, 0, last + Pulse_us);
  model_event(&md, Transport_continue, 0, last + 950000);
  CHECK(md.playing && md.m.est_valid);
  model_poll(&md, last + Lost_us);
  CHECK(no_actions(&md.last) && md.playing && md.m.est_valid);
  CHECK(md.last.est_valid && md.last.shown == 120);
  model_poll(&md, last + 950000 + Lost_us);
  CHECK(md.last.stop && md.last.clock_lost && !md.playing);
  CHECK(!md.last.est_valid && md.last.shown_changed && !md.m.est_valid);
  model_deinit(&md);
}

// The drops: clock lost (ext lost), an F8 interval of 1 s (ext --), 1 s with
// no F8 while stopped (ext --) and transport_reset (ext --), each sending no
// /orca/bpm; an interval of 999 999 and an idle poll at 999 999 keep it.
void test_transport_estimate_drops(void) {
  char text[9];
  for (int kind = 0; kind < 6; ++kind) {
    Model md;
    Hist h = {.n = 0};
    model_init(&md);
    start_valid(&md, &h, Pulse_us, 500000);
    U64 last = h.stamps[h.n - 1];
    Usz sends = md.osc_bpm_count;
    bool expect_valid = kind >= 4;
    switch (kind) {
    case 0:
      model_poll(&md, last + Lost_us);
      CHECK(md.last.clock_lost);
      break;
    case 1:
      pulse(&md, last + Lost_us);
      break;
    case 2:
      model_event(&md, Transport_stop, 0, last + Pulse_us);
      model_poll(&md, last + Lost_us);
      CHECK(!md.last.clock_lost);
      break;
    case 3:
      transport_reset(&md.m);
      md.playing = false;
      break;
    case 4:
      pulse(&md, last + Lost_us - 1);
      break;
    case 5:
      model_event(&md, Transport_stop, 0, last + Pulse_us);
      model_poll(&md, last + Lost_us - 1);
      break;
    }
    CHECK(md.m.est_valid == expect_valid);
    if (kind != 3) {
      CHECK(md.last.est_valid == expect_valid);
      CHECK(md.last.shown_changed == !expect_valid);
    }
    CHECK(md.osc_bpm_count == sends);
    ext_bpm_text(md.m.lost, md.m.est_valid, md.m.shown, text);
    if (kind == 0)
      CHECK(strcmp(text, "ext lost") == 0);
    else if (expect_valid)
      CHECK(strcmp(text, "ext 120") == 0);
    else
      CHECK(strcmp(text, "ext --") == 0);
    model_deinit(&md);
  }
}

// The display value: valid at 120.4 shows 120; 120.6 and 119.6 are in band;
// 121.1 moves it only on the 24th consecutive out-of-band F8. 120.9 never
// moves it (the band, not rounding), and one in-band F8 resets the count.
void test_transport_hysteresis(void) {
  enum {
    Span_120_0 = 499992,
    Span_120_4 = 498336,
    Span_120_6 = 497544,
    Span_119_6 = 501672,
    Span_121_1 = 495456,
    Span_120_9 = 496272,
    Span_120_5 = 497928,
  };
  {
    Model md;
    Hist h = {.n = 0};
    model_init(&md);
    start_valid(&md, &h, Span_120_4 / 24, Span_120_4);
    CHECK(near(md.last.bpm, 120.4, 0.01));
    CHECK(md.last.shown == 120 && md.shown_changes == 1);
    feed_span(&md, &h, Span_120_6);
    feed_span(&md, &h, Span_119_6);
    CHECK(md.last.shown == 120 && md.shown_changes == 1);
    for (int i = 0; i < 23; ++i)
      feed_span(&md, &h, Span_121_1);
    CHECK(near(md.last.bpm, 121.1, 0.01));
    CHECK(md.last.shown == 120 && md.shown_changes == 1);
    feed_span(&md, &h, Span_121_1);
    CHECK(md.last.shown == 121 && md.last.shown_changed &&
          md.shown_changes == 2);
    CHECK(md.osc_bpm_count == 2 && md.osc_bpm_last == 121);
    model_deinit(&md);
  }
  {
    Model md;
    Hist h = {.n = 0};
    model_init(&md);
    start_valid(&md, &h, Pulse_us, Span_120_0);
    CHECK(md.last.shown == 120);
    for (int i = 0; i < 30; ++i)
      feed_span(&md, &h, Span_120_9);
    CHECK(near(md.last.bpm, 120.9, 0.01));
    CHECK(md.last.shown == 120 && md.shown_changes == 1);
    model_deinit(&md);
  }
  {
    Model md;
    Hist h = {.n = 0};
    model_init(&md);
    start_valid(&md, &h, Pulse_us, Span_120_0);
    for (int i = 0; i < 23; ++i)
      feed_span(&md, &h, Span_121_1);
    feed_span(&md, &h, Span_120_5);
    for (int i = 0; i < 23; ++i)
      feed_span(&md, &h, Span_121_1);
    CHECK(md.last.shown == 120 && md.shown_changes == 1);
    model_deinit(&md);
  }
}

// transport_reset, then F8: no tick, no estimate, no deadline.
void test_transport_reset(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  CHECK(md.m.running && md.m.est_valid);
  Usz bodies = md.bodies;
  transport_reset(&md.m);
  md.playing = false; // the shell stops playback before the reset
  CHECK(!md.m.running && !md.m.lost && md.m.phase == 0 && !md.m.est_valid);
  CHECK(md.m.ring_count == 0 && !md.m.has_pulse && !md.m.has_anchor);
  pulse(&md, h.stamps[h.n - 1] + Pulse_us);
  CHECK(!md.last.tick_due && md.bodies == bodies);
  CHECK(!md.last.est_valid && !md.m.est_valid);
  U64 t;
  CHECK(!transport_next_deadline(&md.m, &t));
  model_deinit(&md);
}

// A step model of ged_do_stuff's firing rule (tui_main.c): the deadline
// crosses at (t - clock) + accum >= span, where span is S, or S / 6 with
// clock-out on; then clock = t and accum carries the overshoot. With
// clock-out on every crossing sends an F8 and the tick runs only when the
// sixths counter is 0 before it increments. Time runs in steps of Step from
// `from`; the first F8 and the first tick are recorded, or stay negative.
enum { Rearm_steps = 20000 };
static double const Rearm_step = 1e-4;

typedef struct {
  double clock, accum;
  U8 sixths;
  bool clock_out;
  double first_f8, first_tick;
} Rearm_model;

static void rearm_model_run(Rearm_model *m, double S, double from) {
  double span = m->clock_out ? S / 6.0 : S;
  m->first_f8 = -1.0;
  m->first_tick = -1.0;
  for (int i = 1; i <= Rearm_steps; ++i) {
    double t = from + (double)i * Rearm_step;
    double sdiff = (t - m->clock) + m->accum;
    if (sdiff < span)
      continue;
    m->clock = t;
    m->accum = sdiff - span;
    bool tick = true;
    if (m->clock_out) {
      if (m->first_f8 < 0.0)
        m->first_f8 = t;
      tick = m->sixths == 0;
      m->sixths = (U8)((m->sixths + 1) % 6);
    }
    if (tick && m->first_tick < 0.0) {
      m->first_tick = t;
      return;
    }
  }
}

// The re-arm helper (midi_in.h) against the model: the toggle keeps the
// next tick's due time, both ways, with the first F8 on it when clock-out
// turns on; the switch to Internal puts the first tick, and its F8 with
// clock-out, one S after the switch; every sixths value, and e values that
// give a negative accum_secs.
void test_transport_rearm(void) {
  double const S = 0.5;
  double const tol = 1.5 * Rearm_step;
  double const es[] = {0.0, 0.01, 0.07, 0.3, 0.49};
  for (U8 sixths = 0; sixths < 6; ++sixths) {
    for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(es); ++i) {
      double e = es[i]; // the toggle happens e after the last deadline, at 0
      // off -> on: the untouched run has clock-out off.
      Rearm_model untouched = {.clock = 0.0, .accum = 0.0, .sixths = sixths,
                               .clock_out = false};
      rearm_model_run(&untouched, S, 0.0);
      CHECK(untouched.first_tick > e); // no deadline before the toggle
      Rearm on = rearm_compute(Rearm_clock_out_on, S, e, sixths, true);
      CHECK(on.sixths == 0);
      if (e < 5.0 * S / 6.0)
        CHECK(on.accum_secs < 0.0);
      Rearm_model rearmed = {.clock = e, .accum = on.accum_secs,
                             .sixths = on.sixths, .clock_out = true};
      rearm_model_run(&rearmed, S, e);
      CHECK(near(rearmed.first_tick, untouched.first_tick, tol));
      CHECK(near(rearmed.first_f8, untouched.first_tick, tol));
      // on -> off: the untouched run has clock-out on from sixths.
      Rearm_model untouched_on = {.clock = 0.0, .accum = 0.0,
                                  .sixths = sixths, .clock_out = true};
      rearm_model_run(&untouched_on, S, 0.0);
      if (e >= S / 6.0)
        continue; // a sixth deadline would have fired before the toggle
      Rearm off = rearm_compute(Rearm_clock_out_off, S, e, sixths, false);
      Rearm_model rearmed_off = {.clock = e, .accum = off.accum_secs,
                                 .sixths = off.sixths, .clock_out = false};
      rearm_model_run(&rearmed_off, S, e);
      CHECK(near(rearmed_off.first_tick, untouched_on.first_tick, tol));
    }
    // The switch to Internal at time 1.0, with clock-out on and off.
    for (int co = 0; co < 2; ++co) {
      bool clock_out = co == 1;
      Rearm sw = rearm_compute(Rearm_internal, S, 0.0, sixths, clock_out);
      CHECK(sw.sixths == 0);
      Rearm_model m = {.clock = 1.0, .accum = sw.accum_secs,
                       .sixths = sw.sixths, .clock_out = clock_out};
      rearm_model_run(&m, S, 1.0);
      CHECK(near(m.first_tick, 1.0 + S, tol));
      if (clock_out)
        CHECK(near(m.first_f8, 1.0 + S, tol));
    }
  }
}

// Clock-out (midi_in.h, Clock_out; spine AD-16): the gate, the span, the
// start, the F8 gate at each deadline, the re-arms and the stop's FC, on a
// recording sink whose midi1 snapshots the Clock_out it watches, so a row
// can tell the state a byte went out with. Every row starts from a stale
// Clock_out (clock 7, accum_secs 9.0, sixths 3 unless the row sweeps it), so
// a dropped assignment shows. 120 BPM: one tick, S, is 0.125 s.
enum { Co_bpm = 120 };

static Clock_out stale_clock_out(bool enabled, U8 sixths) {
  return (Clock_out){
      .clock = 7, .accum_secs = 9.0, .sixths = sixths, .enabled = enabled};
}

// The Internal span at Co_bpm, as the shell's expression computes it.
static double co_tick_span(void) { return 60.0 / (double)Co_bpm / 4.0; }

static bool clock_out_is(Clock_out const *co, U64 clock, double accum_secs,
                         U8 sixths, bool enabled) {
  return co->clock == clock && co->accum_secs == accum_secs &&
         co->sixths == sixths && co->enabled == enabled;
}

// The log holds exactly one call, a midi1 of byte, with a snapshot.
static bool log_one_midi1(Recording const *r, int byte) {
  return !r->log_overflow && r->log_count == 1 &&
         r->log[0].kind == Log_midi1 && r->log[0].status == byte &&
         r->log[0].has_snap;
}

// The log's entries from `from` are the fixture's two note-offs then
// /orca/stopped, then FC when fc is set, and nothing else.
static bool log_tail_is_stop(Recording const *r, Usz from, bool fc) {
  Usz want = from + 3 + (fc ? 1 : 0);
  if (r->log_overflow || r->log_count != want)
    return false;
  Log_entry const *e = r->log + from;
  return e[0].kind == Log_midi3 && e[0].status == 0x80 && e[0].d1 == 36 &&
         e[0].d2 == 0 && e[1].kind == Log_midi3 && e[1].status == 0x89 &&
         e[1].d1 == 127 && e[1].d2 == 0 && e[2].kind == Log_control &&
         e[2].address && strcmp(e[2].address, "/orca/stopped") == 0 &&
         (!fc || (e[3].kind == Log_midi1 && e[3].status == 0xFC));
}

// Under External clock-out is suspended: the gate is shut whatever the
// setting, the span is a whole tick, the start, the deadlines and the
// toggle send nothing, and the toggle flips only the setting.
void test_clock_out_external_silent(void) {
  Clock_source const ext = Clock_source_external;
  double const S = co_tick_span();
  Clock_out co = stale_clock_out(true, 3);
  CHECK(!clock_out_active(&co, ext));
  CHECK(clock_out_span(&co, ext, Co_bpm) == S);

  Recording rec;
  Tick_sink const sink = rec_sink(&rec);
  rec.watch = &co;
  clock_out_on_start(&co, ext, Co_bpm, 1000, &sink);
  double accum = S;
  accum -= 0.0001;
  CHECK(clock_out_is(&co, 1000, accum, 0, true));
  CHECK(rec.log_count == 0);

  co = stale_clock_out(true, 3);
  for (int i = 0; i < 12; ++i)
    CHECK(clock_out_on_deadline(&co, ext, &sink));
  CHECK(rec.log_count == 0);
  CHECK(clock_out_is(&co, 7, 9.0, 3, true)); // no sixths advance

  co = stale_clock_out(true, 3);
  clock_out_toggle(&co, ext, Co_bpm, 5000, 0.01, true, &sink);
  CHECK(clock_out_is(&co, 7, 9.0, 3, false));
  clock_out_toggle(&co, ext, Co_bpm, 6000, 0.01, true, &sink);
  CHECK(clock_out_is(&co, 7, 9.0, 3, true));
  CHECK(rec.log_count == 0);
}

// Internal with beat clock: FA at the start, with the clock, the sixths
// counter and the start's accum_secs set; 12 deadlines send 12 F8s and run
// the tick body on the 1st and 7th only; the stop then sends the note-offs,
// /orca/stopped and FC, after the 12th F8. Without beat clock nothing is
// sent but the stop's note-offs and /orca/stopped, and every deadline runs
// the body.
void test_clock_out_internal_wire(void) {
  Clock_source const in = Clock_source_internal;
  double const S = co_tick_span();
  for (int on = 1; on >= 0; --on) {
    Clock_out co = stale_clock_out(on == 1, 3);
    Recording rec;
    Tick_sink const sink = rec_sink(&rec);
    rec.watch = &co;
    CHECK(clock_out_active(&co, in) == (on == 1));
    clock_out_on_start(&co, in, Co_bpm, 1000, &sink);
    double accum = 60.0 / (double)Co_bpm / 4.0; // the start's arithmetic
    if (on)
      accum /= 6.0;
    accum -= 0.0001;
    CHECK(clock_out_is(&co, 1000, accum, 0, on == 1));
    if (on) {
      CHECK(log_one_midi1(&rec, 0xFA));
      CHECK(accum == S / 6.0 - 0.0001);
    } else {
      CHECK(rec.log_count == 0);
      CHECK(accum == S - 0.0001);
    }

    Usz bodies = 0;
    bool body_on[13] = {false};
    for (int call = 1; call <= 12; ++call) {
      bool body = clock_out_on_deadline(&co, in, &sink);
      body_on[call] = body;
      bodies += body;
    }
    if (on) {
      CHECK(bodies == 2 && body_on[1] && body_on[7]);
      CHECK(rec.log_count == 13 && rec.midi1_count == 13);
      Usz f8s = 0;
      for (Usz i = 1; i < rec.log_count; ++i)
        f8s += rec.log[i].kind == Log_midi1 && rec.log[i].status == 0xF8;
      CHECK(f8s == 12);
      CHECK(co.sixths == 0);
    } else {
      CHECK(bodies == 12);
      CHECK(rec.log_count == 0);
    }
    CHECK(co.clock == 1000 && co.accum_secs == accum); // deadlines leave them

    Susnote_list susnotes;
    seed_notes(&susnotes);
    Ccout_engine ccout;
    ccout_init(&ccout);
    Usz before = rec.log_count;
    tick_stop(&sink, &susnotes, &ccout, clock_out_active(&co, in));
    CHECK(log_tail_is_stop(&rec, before, on == 1));
    CHECK(rec.midi1_count == (on ? 14u : 0u));
    CHECK(susnotes.count == 0);
    susnote_list_deinit(&susnotes);
  }
}

// The switch to Internal while playing: with beat clock, exactly one FB,
// never FA, sent after the re-arm (the snapshot holds it), with the clock
// at now, the sixths counter at 0 and accum_secs from rearm_compute (S / 6 -
// S); without it, nothing is sent and accum_secs is 0; while stopped,
// nothing is sent and nothing changes.
void test_clock_out_return_internal(void) {
  double const S = co_tick_span();
  Recording rec;
  Tick_sink const sink = rec_sink(&rec);

  Clock_out co = stale_clock_out(true, 3);
  rec.watch = &co;
  clock_out_return_internal(&co, Co_bpm, 2000, true, &sink);
  Rearm const r_on = rearm_compute(Rearm_internal, S, 0.0, 0, true);
  CHECK(r_on.accum_secs == S / 6.0 - S);
  CHECK(clock_out_is(&co, 2000, r_on.accum_secs, 0, true));
  CHECK(log_one_midi1(&rec, 0xFB));
  CHECK(clock_out_is(&rec.log[0].snap, 2000, r_on.accum_secs, 0, true));

  Tick_sink const sink_off = rec_sink(&rec);
  co = stale_clock_out(false, 3);
  rec.watch = &co;
  clock_out_return_internal(&co, Co_bpm, 2000, true, &sink_off);
  CHECK(rec.log_count == 0);
  CHECK(clock_out_is(&co, 2000, 0.0, 0, false));

  for (int on = 0; on < 2; ++on) {
    co = stale_clock_out(on == 1, 3);
    clock_out_return_internal(&co, Co_bpm, 2000, false, &sink_off);
    CHECK(rec.log_count == 0);
    CHECK(clock_out_is(&co, 7, 9.0, 3, on == 1));
  }
}

// The Send MIDI Beat Clock toggle while playing on Internal, for each e and
// sixths value: off -> on sends one FB and on -> off one FC, each after the
// setting flips and the re-arm lands (the snapshot at the byte holds both),
// with the fields from rearm_compute and the clock at now. While stopped,
// or under External, only the setting flips.
void test_clock_out_toggle(void) {
  double const S = co_tick_span();
  double const es[] = {0.0, 0.01, 0.07};
  U64 const now = 3000;
  for (U8 sixths = 0; sixths < 6; ++sixths) {
    for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(es); ++i) {
      double e = es[i];
      for (int was_on = 0; was_on < 2; ++was_on) {
        bool on = was_on == 0;
        Clock_out co = stale_clock_out(was_on == 1, sixths);
        Recording rec;
        Tick_sink const sink = rec_sink(&rec);
        rec.watch = &co;
        clock_out_toggle(&co, Clock_source_internal, Co_bpm, now, e, true,
                         &sink);
        Rearm const r = rearm_compute(
            on ? Rearm_clock_out_on : Rearm_clock_out_off, S, e, sixths, on);
        CHECK(clock_out_is(&co, now, r.accum_secs, r.sixths, on));
        CHECK(log_one_midi1(&rec, on ? 0xFB : 0xFC));
        CHECK(clock_out_is(&rec.log[0].snap, now, r.accum_secs, r.sixths, on));

        // Stopped on Internal, then playing under External.
        for (int k = 0; k < 2; ++k) {
          Clock_source src =
              k == 0 ? Clock_source_internal : Clock_source_external;
          co = stale_clock_out(was_on == 1, sixths);
          Tick_sink const quiet = rec_sink(&rec);
          clock_out_toggle(&co, src, Co_bpm, now, e, k == 1, &quiet);
          CHECK(rec.log_count == 0);
          CHECK(clock_out_is(&co, 7, 9.0, sixths, on));
        }
      }
    }
  }
}

// The deadline span at 120 BPM: a sixth of a tick with the gate open, a
// whole tick with it shut or under External, equal to the shell's
// expression.
void test_clock_out_span(void) {
  double tick = 60.0 / (double)Co_bpm / 4.0;
  double sixth = tick;
  sixth /= 6.0;
  CHECK(tick == 0.125);
  Clock_out on = stale_clock_out(true, 3);
  Clock_out off = stale_clock_out(false, 3);
  CHECK(clock_out_span(&on, Clock_source_internal, Co_bpm) == sixth);
  CHECK(clock_out_span(&off, Clock_source_internal, Co_bpm) == tick);
  CHECK(clock_out_span(&on, Clock_source_external, Co_bpm) == tick);
  CHECK(clock_out_span(&off, Clock_source_external, Co_bpm) == tick);
  CHECK(clock_out_is(&on, 7, 9.0, 3, true)); // a query changes nothing
}

// A stamp earlier than the last empties the ring as a refill does: the
// estimate is held, the ring restarts at 1, the phase still advances; the
// next 24 rising pulses replace the estimate with a sane tick_len.
void test_transport_stale_stamp_refill(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  U64 last = h.stamps[h.n - 1];
  U8 phase = md.m.phase;
  Usz bodies = md.bodies;
  pulse(&md, last - 1);
  CHECK(md.m.est_valid && md.last.est_valid && md.last.tick_len == 125000);
  CHECK(md.m.ring_count == 1);
  CHECK(md.m.phase == (phase + 1) % 6);
  CHECK(!md.last.tick_due && md.bodies == bodies); // phase 1: no tick
  for (Usz k = 1; k <= 23; ++k) {
    pulse(&md, last - 1 + k * Pulse_us);
    CHECK(md.last.tick_len == 125000);
  }
  pulse(&md, last - 1 + 24 * Pulse_us);
  CHECK(md.m.ring_count == 25 && md.last.est_valid);
  CHECK(md.last.tick_len == (24 * Pulse_us + 2) / 4);
  CHECK(md.last.tick_len < 200000);
  model_deinit(&md);
}

// Stopped with a valid estimate, the deadline is the last anchor + 1 s: the
// last F8 after FC, and the newest F8 while stopped.
void test_transport_deadline_while_stopped(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  U64 last = h.stamps[h.n - 1];
  model_event(&md, Transport_stop, 0, last + Pulse_us);
  U64 t = 0;
  CHECK(transport_next_deadline(&md.m, &t) && t == last + Lost_us);
  pulse(&md, last + 2 * Pulse_us);
  CHECK(transport_next_deadline(&md.m, &t) &&
        t == last + 2 * Pulse_us + Lost_us);
  model_deinit(&md);
}

// An F8 1 s or more after the previous one while running drops the estimate
// but keeps the machine running: no stop, and a tick or not by the phase.
void test_transport_gap_keeps_running(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  CHECK(md.m.phase == 1);
  U64 t = h.stamps[h.n - 1] + Lost_us;
  Usz bodies = md.bodies;
  pulse(&md, t); // at phase 1: no tick
  CHECK(!md.m.est_valid && !md.last.est_valid && md.last.shown_changed);
  CHECK(md.m.running && md.playing && !md.last.stop && !md.last.clock_lost);
  CHECK(!md.last.tick_due && md.bodies == bodies);
  t = clocks_from(&md, t, 4); // to phase 0
  CHECK(md.m.phase == 0);
  bodies = md.bodies;
  pulse(&md, t + Lost_us); // at phase 0: a tick
  CHECK(md.last.tick_due && md.bodies == bodies + 1);
  CHECK(md.m.running && md.playing && !md.last.stop && !md.m.est_valid);
  model_poll(&md, t + Lost_us + 1);
  CHECK(no_actions(&md.last) && md.playing);
  model_deinit(&md);
}

// F2 while stopped moves the idle-drop anchor: the poll 1 s after the last
// F8 does nothing, the poll 1 s after the F2 drops the estimate, with no
// clock lost since nothing runs.
void test_transport_songpos_moves_anchor(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  U64 last = h.stamps[h.n - 1];
  model_event(&md, Transport_stop, 0, last + Pulse_us);
  model_event(&md, Transport_songpos, 4, last + 950000);
  CHECK(!md.playing && md.m.est_valid && md.tick_num == 4);
  model_poll(&md, last + Lost_us);
  CHECK(no_actions(&md.last) && md.m.est_valid && md.last.est_valid);
  model_poll(&md, last + 950000 + Lost_us);
  CHECK(!md.m.est_valid && !md.last.est_valid && md.last.shown_changed);
  CHECK(!md.last.clock_lost && !md.last.stop && !md.m.lost);
  model_deinit(&md);
}

// FA after a clock loss clears lost and starts; the HUD reads ext -- again.
void test_transport_start_clears_lost(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  U64 last = h.stamps[h.n - 1];
  model_poll(&md, last + Lost_us);
  CHECK(md.m.lost && md.last.clock_lost && !md.playing);
  char text[9];
  ext_bpm_text(md.m.lost, md.m.est_valid, md.m.shown, text);
  CHECK(strcmp(text, "ext lost") == 0);
  model_event(&md, Transport_start, 0, last + 2 * Lost_us);
  CHECK(!md.m.lost && md.last.start && md.playing);
  ext_bpm_text(md.m.lost, md.m.est_valid, md.m.shown, text);
  CHECK(strcmp(text, "ext --") == 0);
  model_deinit(&md);
}

// The parser: F2's two 7-bit bytes, the five statuses, the stamp carried,
// and false for everything else.
void test_transport_parse(void) {
  Transport_event e;
  CHECK(transport_parse(0xF2, 0x7F, 0x7F, 1, &e));
  CHECK(e.type == Transport_songpos && e.value == 16383 && e.stamp == 1);
  CHECK(transport_parse(0xF2, 0x10, 0x01, 2, &e));
  CHECK(e.type == Transport_songpos && e.value == 144 && e.stamp == 2);
  CHECK(transport_parse(0xF8, 0, 0, 3, &e));
  CHECK(e.type == Transport_clock && e.stamp == 3);
  CHECK(transport_parse(0xFA, 0, 0, 4, &e));
  CHECK(e.type == Transport_start && e.stamp == 4);
  CHECK(transport_parse(0xFB, 0, 0, 5, &e));
  CHECK(e.type == Transport_continue && e.stamp == 5);
  CHECK(transport_parse(0xFC, 0, 0, 6, &e));
  CHECK(e.type == Transport_stop && e.stamp == 6);
  int const others[] = {0xFE, 0xF9, 0xFD, 0x90, 0xB0, 0xF0};
  for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(others); ++i) {
    e.stamp = 99;
    CHECK(!transport_parse(others[i], 0x40, 0x40, 7, &e));
    CHECK(e.stamp == 99); // left alone
  }
}

// The HUD's BPM field under External: at most 8 characters.
void test_transport_hud_text(void) {
  char text[9];
  ext_bpm_text(true, true, 120, text);
  CHECK(strcmp(text, "ext lost") == 0);
  ext_bpm_text(false, true, 120, text);
  CHECK(strcmp(text, "ext 120") == 0);
  ext_bpm_text(false, true, 999, text);
  CHECK(strcmp(text, "ext 999") == 0 && strlen(text) <= 8);
  ext_bpm_text(false, true, 1, text);
  CHECK(strcmp(text, "ext 1") == 0);
  ext_bpm_text(false, false, 120, text);
  CHECK(strcmp(text, "ext --") == 0);
  Clock_source s = Clock_source_external;
  CHECK(clock_source_from_name("internal", &s) && s == Clock_source_internal);
  CHECK(clock_source_from_name("external", &s) && s == Clock_source_external);
  CHECK(!clock_source_from_name("sideways", &s) && s == Clock_source_external);
  CHECK(strcmp(clock_source_name(Clock_source_internal), "internal") == 0);
  CHECK(strcmp(clock_source_name(Clock_source_external), "external") == 0);
}

// A stall past the input pool: the reader's overflow, stamped 5 s
// after the last F8, stands for pulses that arrived and were flushed. The
// machine keeps running with no clock lost, holds the estimate (the window
// refills, and the next F8 takes no interval drop), keeps the phase, and the
// pulses that follow carry on from the tick it stopped at.
void test_transport_overflow_keeps_following(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  CHECK(md.m.phase == 1 && md.bodies == 5 && md.tick_num == 5);
  U64 const last = h.stamps[h.n - 1];
  U64 const tick_len = md.m.tick_len;
  double const bpm = md.m.bpm;
  U16 const shown = md.m.shown;
  U64 const ov = last + 5 * (U64)Lost_us;
  model_event(&md, Transport_overflow, 0, ov);
  CHECK(no_actions(&md.last) && md.last.est_valid);
  CHECK(md.last.tick_len == tick_len && md.last.shown == shown);
  CHECK(md.m.running && md.playing && !md.m.lost && md.m.phase == 1);
  U64 t = 0;
  CHECK(transport_next_deadline(&md.m, &t) && t == ov + Lost_us);
  model_poll(&md, ov);
  CHECK(no_actions(&md.last) && md.playing && md.m.est_valid);
  model_poll(&md, ov + Lost_us - 1);
  CHECK(no_actions(&md.last) && md.playing && md.m.est_valid);
  // Pulses 2 to 6 of the tick in progress run nothing; the next runs tick 5.
  t = clocks_from(&md, ov, 5);
  CHECK(md.bodies == 5 && md.m.phase == 0);
  CHECK(md.m.est_valid && md.m.tick_len == tick_len && md.m.bpm == bpm);
  CHECK(md.m.ring_count == 5);
  pulse(&md, t + Pulse_us);
  CHECK(md.last.tick_due && md.bodies == 6 && md.body_tick[5] == 5);
  CHECK(md.tick_num == 6 && md.playing && md.m.shown == shown);
  model_deinit(&md);
}

// The overflow's other states: a stamp earlier than the last F8 moves
// neither clock back; after a loss it restarts nothing (only FA or FB does);
// while stopped it moves the idle-drop anchor, starts nothing, and holds the
// estimate, so an F8 a second after the last one keeps it.
void test_transport_overflow_stale_lost_stopped(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  U64 const last = h.stamps[h.n - 1];
  model_event(&md, Transport_overflow, 0, last - 1);
  CHECK(no_actions(&md.last) && md.m.running);
  model_poll(&md, last + Lost_us - 1);
  CHECK(no_actions(&md.last) && md.playing);
  model_poll(&md, last + Lost_us);
  CHECK(md.last.clock_lost && md.m.lost && !md.playing);
  Usz const bodies = md.bodies;
  model_event(&md, Transport_overflow, 0, last + 2 * (U64)Lost_us);
  CHECK(no_actions(&md.last) && md.m.lost && !md.m.running);
  U64 t = clocks_from(&md, last + 2 * (U64)Lost_us, 12);
  CHECK(md.bodies == bodies && !md.playing && md.m.lost);
  model_event(&md, Transport_continue, 0, t);
  CHECK(md.last.start && md.playing && !md.m.lost);
  model_deinit(&md);

  // Stopped: the overflow moves the idle drop to its own stamp + 1 s.
  Hist h2 = {.n = 0};
  model_init(&md);
  start_valid(&md, &h2, Pulse_us, 500000);
  U64 const l2 = h2.stamps[h2.n - 1];
  model_event(&md, Transport_stop, 0, l2 + Pulse_us);
  CHECK(!md.playing && md.m.est_valid);
  model_event(&md, Transport_overflow, 0, l2 + 950000);
  CHECK(no_actions(&md.last) && !md.playing && !md.m.running);
  model_poll(&md, l2 + Lost_us);
  CHECK(no_actions(&md.last) && md.m.est_valid);
  model_poll(&md, l2 + 950000 + Lost_us);
  CHECK(!md.m.est_valid && md.last.shown_changed);
  CHECK(!md.last.clock_lost && !md.last.stop && !md.m.lost);
  model_deinit(&md);

  // Stopped: the estimate is held too, so the first F8 after the overflow,
  // over 1 s after the last one, takes no interval drop.
  Hist h3 = {.n = 0};
  model_init(&md);
  start_valid(&md, &h3, Pulse_us, 500000);
  U64 const l3 = h3.stamps[h3.n - 1];
  U64 const tick_len = md.m.tick_len;
  U16 const shown = md.m.shown;
  model_event(&md, Transport_stop, 0, l3 + Pulse_us);
  model_event(&md, Transport_overflow, 0, l3 + 950000);
  CHECK(no_actions(&md.last) && !md.m.running);
  pulse(&md, l3 + Lost_us + 1);
  CHECK(md.m.est_valid && md.last.est_valid && md.m.ring_count == 1);
  CHECK(!md.last.shown_changed && md.m.shown == shown);
  CHECK(md.last.tick_len == tick_len && !md.playing);
  model_deinit(&md);
}

// An overflow and then no F8 (the master stopped clocking during the hang):
// clock lost comes 1 s after the overflow, with the stop and the drop.
void test_transport_overflow_then_silence(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  U64 const ov = h.stamps[h.n - 1] + 5 * (U64)Lost_us;
  model_event(&md, Transport_overflow, 0, ov);
  CHECK(md.m.running && md.playing && md.m.est_valid);
  model_poll(&md, ov + Lost_us);
  CHECK(md.last.clock_lost && md.last.stop && md.m.lost && !md.m.running);
  CHECK(!md.playing && !md.m.est_valid && !md.last.est_valid);
  CHECK(md.last.shown_changed);
  model_deinit(&md);
}

// An overflow inside a partial window, before any estimate: the window
// refills, so the estimate becomes valid only at the 25th F8 after it, from
// those pulses alone.
void test_transport_overflow_partial_window(void) {
  Model md;
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  U64 t = clocks_from(&md, 0, 10);
  CHECK(!md.m.est_valid && md.m.ring_count == 10);
  U64 const ov = t + 3 * (U64)Lost_us;
  model_event(&md, Transport_overflow, 0, ov);
  CHECK(no_actions(&md.last) && !md.last.est_valid && md.m.ring_count == 0);
  t = clocks_from(&md, ov, 24);
  CHECK(!md.m.est_valid && !md.last.est_valid && md.m.ring_count == 24);
  pulse(&md, t + Pulse_us);
  CHECK(md.m.est_valid && md.last.est_valid && md.last.shown_changed);
  CHECK(md.last.tick_len == (24 * Pulse_us + 2) / 4 && md.m.shown == 120);
  model_deinit(&md);
}

// FA, then an overflow before the first F8: the phase stays 0, so the first
// F8 runs tick 0.
void test_transport_overflow_after_start(void) {
  Model md;
  model_init(&md);
  model_event(&md, Transport_start, 0, 0);
  model_event(&md, Transport_overflow, 0, 10);
  CHECK(no_actions(&md.last) && md.playing && md.m.running);
  CHECK(md.m.phase == 0 && md.bodies == 0);
  pulse(&md, 10 + Pulse_us);
  CHECK(md.last.tick_due && md.bodies == 1 && md.body_tick[0] == 0);
  CHECK(md.tick_num == 1 && md.m.phase == 1);
  model_deinit(&md);
}

// The interval drop after an overflow: the overflow stands as the last
// pulse, so an F8 1.5 s after it drops the estimate, as after any pulse,
// and the machine keeps running.
void test_transport_overflow_gap_drops(void) {
  Model md;
  Hist h = {.n = 0};
  model_init(&md);
  start_valid(&md, &h, Pulse_us, 500000);
  U64 const ov = h.stamps[h.n - 1] + 5 * (U64)Lost_us;
  model_event(&md, Transport_overflow, 0, ov);
  CHECK(md.m.est_valid && md.m.running);
  pulse(&md, ov + 3 * (U64)Lost_us / 2);
  CHECK(!md.m.est_valid && !md.last.est_valid && md.last.shown_changed);
  CHECK(md.m.running && md.playing && !md.last.stop && !md.last.clock_lost);
  CHECK(md.m.ring_count == 1);
  model_deinit(&md);
}
