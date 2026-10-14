#pragma once
#include "base.h"
#include "tick.h"
#include "transport.h"
#include <errno.h>
#include <stdio.h>

// The MIDI input side of the shell: the pure parts, shared by tui_main.c and
// the unit tests, which link none of tui_main.c, among them the clock-out
// state Clock_out and its inlines, which send the beat clock through the
// tick.h sink, and the PortMidi reader's
// decode, pm_in_decode; it also holds the ALSA client-name scan that the
// startup guard runs (the scan's I/O is the caller's `next` callback) and the
// client-name length limit it checks first (the sequencer keeps 63 bytes).
// Everything here is static inline, reads no clock and does no I/O but through
// a sink or a callback. The backend-specific helpers sit behind their FEAT_
// flags and may include that backend's header (an adapter, not core).
// BORCA_HAS_MIDI_IN is defined when a build has a reader; with none, External
// is hidden and the --clock-source and --midi-in flags are unknown options.

#if defined(FEAT_ALSA) || defined(FEAT_PORTMIDI)
#define BORCA_HAS_MIDI_IN 1
#endif

// The HUD's BPM field under External, at most 8 characters: "ext lost" while
// the clock is lost, otherwise "ext <shown>" while the estimate is valid and
// "ext --" when it is not. out holds at least 9 bytes.
static inline void ext_bpm_text(bool lost, bool valid, U16 shown, char *out) {
  if (lost)
    snprintf(out, 9, "ext lost");
  else if (valid)
    snprintf(out, 9, "ext %u", (unsigned)(shown > 999 ? 999 : shown));
  else
    snprintf(out, 9, "ext --");
}

// Re-arming the internal deadline so the tick grid is kept. The
// shell's deadline fires at stm_since(clock) + accum_secs >= span, where span
// is one tick, S, or a sixth of it with clock-out on. Each case sets clock =
// now and takes accum_secs and the sixths counter from here, given S, e (the
// time since the last deadline: stm_since(clock) + accum_secs) and the sixths
// counter as they are before the change:
//   clock-out off -> on: the next tick is r = S - e away; accum_secs =
//     S / 6 - r (it may be negative) and sixths = 0, so the first F8 and the
//     tick both land at the old due time;
//   clock-out on -> off: r = (S / 6 - e) + ((6 - sixths) mod 6) * S / 6;
//     accum_secs = S - r;
//   the switch to Internal while playing: accum_secs = span - S, with span S
//     or S / 6 by clock_out, so the first tick, and with clock-out its first
//     F8, comes one S after the switch.
typedef enum {
  Rearm_clock_out_on,  // the Send MIDI Beat Clock toggle, off -> on
  Rearm_clock_out_off, // the toggle, on -> off
  Rearm_internal,      // leaving External while playing
} Rearm_case;

typedef struct {
  double accum_secs;
  U8 sixths;
} Rearm;

static inline Rearm rearm_compute(Rearm_case c, double S, double e, U8 sixths,
                                  bool clock_out) {
  Rearm r = {.accum_secs = 0.0, .sixths = 0};
  double sixth = S / 6.0;
  switch (c) {
  case Rearm_clock_out_on: {
    double due = S - e;
    r.accum_secs = sixth - due;
    break;
  }
  case Rearm_clock_out_off: {
    double due = (sixth - e) + (double)((6 - sixths) % 6) * sixth;
    r.accum_secs = S - due;
    break;
  }
  case Rearm_internal: {
    double span = clock_out ? sixth : S;
    r.accum_secs = span - S;
    break;
  }
  }
  return r;
}

// Clock-out: the beat clock bOrca sends, F8, FA, FB and FC, and the
// internal deadline's state, which Ged holds by value. The shell's deadline
// fires at stm_since(clock) + accum_secs >= the span (clock_out_span); the
// crossing (clock = now, accum_secs = the overshoot) and its spin stay in
// ged_do_stuff. These inlines own every clock-out send but the stop's FC,
// which is tick_stop's (tick.h), and every re-arm. Like rearm_compute they
// read no clock: the shell passes now (stm_now() ticks) and e
// (stm_sec(stm_diff(now, clock)) + accum_secs) in. The re-arms set their
// fields before they send FB or FC; the start keeps 4f349cd's order.
typedef struct {
  U64 clock;         // stm_now() at the last deadline, start or re-arm
  double accum_secs; // carried into the next span; may be negative
  U8 sixths;         // 0..5: the sixth of a tick the next F8 falls on
  bool enabled;      // the stored Send MIDI Beat Clock setting
} Clock_out;

// The one predicate that gates clock-out: the stored setting while the
// source is Internal. F8, FA and FB are sent only while it is true; the
// toggle's FC is sent as it turns false, and tick_stop's FC is gated by the
// caller's copy of it, taken before is_playing clears. External suspends
// clock-out for the session without clearing the setting, so nothing echoes
// transport back to a master.
static inline bool clock_out_active(Clock_out const *co, Clock_source src) {
  return co->enabled && src == Clock_source_internal;
}

// The span to the next deadline at bpm: one tick, or a sixth of a tick while
// clock-out is active, since beat clock is 24 parts per quarter note.
static inline double clock_out_span(Clock_out const *co, Clock_source src,
                                    Usz bpm) {
  double secs_span = 60.0 / (double)bpm / 4.0;
  if (clock_out_active(co, src))
    secs_span /= 6.0;
  return secs_span;
}

// The start (ged_set_playing(true)): clock = now, sixths = 0 and accum_secs
// just under one span, so the first deadline (the first tick, and with
// clock-out the first F8) comes about 0.1 ms after the start; FA when
// clock-out is active. The shell then sends /orca/started.
static inline void clock_out_on_start(Clock_out *co, Clock_source src,
                                      Usz bpm, U64 now,
                                      Tick_sink const *sink) {
  co->clock = now;
  co->sixths = 0;
  // dumb'n'dirty, get us close to the next step time, but not quite
  co->accum_secs = 60.0 / (double)bpm / 4.0;
  if (clock_out_active(co, src)) {
    sink->midi1(sink->u, 0xFA); // "start"
    co->accum_secs /= 6.0;
  }
  co->accum_secs -= 0.0001;
}

// A crossed deadline. True when the tick body runs. While clock-out is
// active it sends F8 and advances sixths mod 6, and the body runs only on
// the sixth that was 0; otherwise it sends nothing and the body runs.
static inline bool clock_out_on_deadline(Clock_out *co, Clock_source src,
                                         Tick_sink const *sink) {
  if (!clock_out_active(co, src))
    return true;
  sink->midi1(sink->u, 0xF8); // MIDI beat clock
  U8 sixths = co->sixths;
  co->sixths = (U8)((sixths + 1) % 6);
  return sixths == 0;
}

// The switch to Internal (the source is already Internal). While playing,
// re-arms with rearm_compute's Rearm_internal, so the first tick comes one
// tick after the switch, sets clock = now, then sends FB, never FA, when
// clock-out is active. Nothing at all while stopped.
static inline void clock_out_return_internal(Clock_out *co, Usz bpm, U64 now,
                                             bool playing,
                                             Tick_sink const *sink) {
  if (!playing)
    return;
  bool active = clock_out_active(co, Clock_source_internal);
  double S = 60.0 / (double)bpm / 4.0;
  Rearm r = rearm_compute(Rearm_internal, S, 0.0, 0, active);
  co->clock = now;
  co->accum_secs = r.accum_secs;
  co->sixths = r.sixths;
  if (active)
    sink->midi1(sink->u, 0xFB); // "continue"
}

// The Send MIDI Beat Clock toggle: the setting flips first, whatever the
// source. While playing on Internal it re-arms with rearm_compute, so the
// next tick keeps its due time, sets clock = now, then sends FB (now on) or
// FC (now off). Under External or while stopped it sends nothing and leaves
// the other fields alone. e is the time since the last deadline, from the
// shell.
static inline void clock_out_toggle(Clock_out *co, Clock_source src, Usz bpm,
                                    U64 now, double e, bool playing,
                                    Tick_sink const *sink) {
  bool on = !co->enabled;
  co->enabled = on;
  if (!playing || src != Clock_source_internal)
    return;
  double S = 60.0 / (double)bpm / 4.0;
  Rearm r = rearm_compute(on ? Rearm_clock_out_on : Rearm_clock_out_off, S,
                          e, co->sixths, on);
  co->clock = now;
  co->accum_secs = r.accum_secs;
  co->sixths = r.sixths;
  if (clock_out_active(co, src))
    sink->midi1(sink->u, 0xFB); // "continue"
  else
    sink->midi1(sink->u, 0xFC); // "stop"
}

// Splits an ALSA input name, `client:port`, at its last ':' into the client
// name, the first *client_len bytes of name, and the port name at *port.
// False when there is no ':' or either side is empty; then nothing matches.
static inline bool alsa_in_split(char const *name, Usz *client_len,
                                 char const **port) {
  char const *colon = strrchr(name, ':');
  if (!colon || colon == name || colon[1] == '\0')
    return false;
  *client_len = (Usz)(colon - name);
  *port = colon + 1;
  return true;
}

// Note-offs the sequencer refused. A MIDI send never sleeps or waits, so when
// a subscriber's input pool is full the write returns -EAGAIN (found with
// two scratch clients, a pool of 3 and a receiver that does not read) and the
// message is gone. A lost note-on is a missed note; a lost note-off is a note
// that hangs on the synth until the same channel and note is played again,
// because pause, stop and quit only release the notes still in the sustained
// list. So a refused note-off is kept here and sent again, and a note-on for
// the same channel and note drops the held one, since sending it afterwards
// would cut the new note short.
// Bounded on purpose. At most Pending_offs_cap entries, each kept at most
// Pending_offs_hold_us. A pass sends at most Pending_offs_pass_max, so a
// recovery burst cannot hold up a tick. The first retry follows the refusal
// by Pending_offs_first_us and every further refusal doubles the wait, up to
// Pending_offs_backoff_max_us, so a stalled subscriber costs about a dozen
// failed writes a second. With several subscribers a refusal by one says
// nothing about the others (a probe with a full and a healthy receiver: the
// sender saw -EAGAIN and the healthy one got every event), so a retry repeats
// the note-off to the healthy ones too; a note-off for a note that is already
// off is ignored by a synth, and the backoff keeps the repeats to that dozen.
// The list reads no clock and does no I/O: the caller passes the time, and
// every write goes through the Midi_write callbacks, so the unit tests drive
// it with a fake that refuses on cue.
enum { Pending_offs_cap = 256 };
enum { Pending_offs_hold_us = 3000000 };
enum { Pending_offs_first_us = 2000 };
enum { Pending_offs_backoff_max_us = 100000 };
enum { Pending_offs_pass_max = 16 };

// How the shell writes one three-byte message to its subscribers. write
// returns >= 0 when the message went out and -errno when it was refused; it
// counts nothing and sends nothing again. now returns the time in us, and is
// called only after a refusal.
typedef struct {
  void *u;
  int (*write)(void *u, int status, int d1, int d2);
  U64 (*now)(void *u);
} Midi_write;

typedef struct {
  U64 since_us;
  U8 chan;
  U8 note;
} Pending_off;

typedef struct {
  Pending_off entries[Pending_offs_cap]; // oldest first
  Usz count;
  U64 next_us;           // the next pass is due at this time
  U64 backoff_us;        // the wait after the next refusal
  Usz held, resent, lost; // totals: kept, sent again, given up
} Pending_offs;

static inline void pending_offs_init(Pending_offs *p) {
  *p = (Pending_offs){.backoff_us = Pending_offs_first_us};
}

// The errors worth another try: the pool was full (-EAGAIN, -ENOMEM) or the
// write was interrupted (-EINTR). Any other error will not pass by itself.
static inline bool midi_send_error_is_transient(int err) {
  return err == -EAGAIN || err == -EINTR || err == -ENOMEM;
}

static inline Usz pending_offs_find(Pending_offs const *p, int chan, int note) {
  for (Usz i = 0; i < p->count; ++i)
    if (p->entries[i].chan == chan && p->entries[i].note == note)
      return i;
  return p->count;
}

static inline void pending_offs_remove_at(Pending_offs *p, Usz index) {
  for (Usz i = index + 1; i < p->count; ++i)
    p->entries[i - 1] = p->entries[i];
  --p->count;
}

// Keeps a refused note-off. An entry for the same channel and note is kept
// as it is, with its first time. False when the list is full: that note-off
// is lost, as it was before this list. The first entry of an empty list sets
// the schedule: its first retry is due Pending_offs_first_us from now.
static inline bool pending_offs_add(Pending_offs *p, int chan, int note,
                                    U64 now_us) {
  if (pending_offs_find(p, chan, note) != p->count)
    return true;
  if (p->count == Pending_offs_cap) {
    ++p->lost;
    return false;
  }
  if (p->count == 0) {
    p->next_us = now_us + Pending_offs_first_us;
    p->backoff_us = Pending_offs_first_us;
  }
  p->entries[p->count++] =
      (Pending_off){.since_us = now_us, .chan = (U8)chan, .note = (U8)note};
  ++p->held;
  return true;
}

// Called before a three-byte message is sent. A note-on (velocity above 0)
// for a channel and note whose note-off is held drops that note-off: sent
// afterwards it would end the new note. True when one was dropped.
static inline bool pending_offs_before_send(Pending_offs *p, int status,
                                            int d1, int d2) {
  if (p->count == 0 || (status & 0xf0) != 0x90 || d2 <= 0)
    return false;
  Usz index = pending_offs_find(p, status & 0x0f, d1);
  if (index == p->count)
    return false;
  pending_offs_remove_at(p, index);
  return true;
}

// Called after a send was refused with err (negative). A note-off that the
// sequencer may take later is kept; a note-on or anything else is not.
static inline bool pending_offs_after_refusal(Pending_offs *p, int status,
                                              int d1, int err, U64 now_us) {
  if ((status & 0xf0) != 0x80 || !midi_send_error_is_transient(err))
    return false;
  return pending_offs_add(p, status & 0x0f, d1, now_us);
}

// Sends one three-byte message through w and keeps a refused note-off. A held
// note-off for the note of a note-on goes out once more first, so the pair
// arrives in order when the subscriber has room again; if that is refused too
// it is dropped. Returns what the message's own write returned.
static inline int held_send(Pending_offs *p, Midi_write const *w, int status,
                            int d1, int d2) {
  if (pending_offs_before_send(p, status, d1, d2)) {
    if (w->write(w->u, 0x80 | (status & 0x0f), d1, 0) >= 0)
      ++p->resent;
    else
      ++p->lost;
  }
  int err = w->write(w->u, status, d1, d2);
  if (err < 0)
    (void)pending_offs_after_refusal(p, status, d1, err, w->now(w->u));
  return err;
}

// One pass. Nothing happens before the schedule's time. Entries older than
// Pending_offs_hold_us are given up without a try. The rest are tried oldest
// first; one that goes out is removed and resets the wait, the first refusal
// ends the pass and doubles the wait, because the subscriber is still full
// for the ones behind it. At most Pending_offs_pass_max go out in a pass; the
// next pass then follows at once. Returns how many entries are still held.
static inline Usz pending_offs_retry(Pending_offs *p, U64 now_us,
                                     Midi_write const *w) {
  if (p->count == 0 || now_us < p->next_us)
    return p->count;
  Usz sent = 0;
  while (p->count != 0 && sent < Pending_offs_pass_max) {
    Pending_off const first = p->entries[0];
    if (now_us - first.since_us >= (U64)Pending_offs_hold_us) {
      pending_offs_remove_at(p, 0);
      ++p->lost;
      continue;
    }
    if (w->write(w->u, 0x80 | first.chan, first.note, 0) < 0) {
      p->next_us = now_us + p->backoff_us;
      p->backoff_us = p->backoff_us * 2 < Pending_offs_backoff_max_us
                          ? p->backoff_us * 2
                          : Pending_offs_backoff_max_us;
      return p->count;
    }
    pending_offs_remove_at(p, 0);
    ++p->resent;
    ++sent;
    p->backoff_us = Pending_offs_first_us;
  }
  p->next_us = now_us;
  return p->count;
}

// True while anything is held; then *wait_us is how long until the next pass
// is due, 0 when it is due now. The main loop shortens its wait to that.
static inline bool pending_offs_wait_us(Pending_offs const *p, U64 now_us,
                                        U64 *wait_us) {
  if (p->count == 0)
    return false;
  *wait_us = p->next_us > now_us ? p->next_us - now_us : 0;
  return true;
}

#ifdef FEAT_PORTMIDI
#include <portmidi.h>

// What the PortMidi input drops: an explicit list, never a
// complement, so transport_parse, which accepts only F8, FA, FB, FC and F2,
// sees no notes, controls or realtime noise. PortMidi has no filter bit for
// F4 and F5, which pass the mask and are rejected by the parser.
// Pm_SetFilter takes it right after Pm_OpenInput.
#define BORCA_PM_IN_DROP                                                       \
  (PM_FILT_ACTIVE | PM_FILT_SYSEX | PM_FILT_TICK | PM_FILT_UNDEFINED |         \
   PM_FILT_RESET | PM_FILT_MTC | PM_FILT_SONG_SELECT | PM_FILT_TUNE |          \
   PM_FILT_NOTE | PM_FILT_AFTERTOUCH | PM_FILT_PROGRAM | PM_FILT_CONTROL |     \
   PM_FILT_PITCHBEND)

// The PortMidi reader's decode: a message's status, Data1 and
// Data2, in that order, to transport_parse, with stamp, the shell's now_us()
// at read time; the PortMidi timestamp is never read. False, leaving *out
// alone, for anything but F8, FA, FB, FC and F2.
static inline bool pm_in_decode(PmMessage msg, U64 stamp,
                                Transport_event *out) {
  return transport_parse(Pm_MessageStatus(msg), Pm_MessageData1(msg),
                         Pm_MessageData2(msg), stamp, out);
}
#endif

#ifdef FEAT_ALSA
#include <alsa/asoundlib.h>
#include <errno.h>

// The source bOrca has subscribed, while one is stored. The shell is
// unsubscribed while has is false, and System Announce decides when it is
// cleared or matched again.
typedef struct {
  bool has;
  int client, port;
} Alsa_in_addr;

// Maps a sequencer event to a transport event: CLOCK, START, CONTINUE, STOP
// and SONGPOS, whose position (data.control.value) is clamped to 0..16383.
// False for every other event, which the reader discards. ev->time is never
// read: stamp is the shell's now_us() at read time.
static inline bool alsa_in_map_event(snd_seq_event_t const *ev, U64 stamp,
                                     Transport_event *out) {
  Transport_type type;
  U16 value = 0;
  switch (ev->type) {
  case SND_SEQ_EVENT_CLOCK:
    type = Transport_clock;
    break;
  case SND_SEQ_EVENT_START:
    type = Transport_start;
    break;
  case SND_SEQ_EVENT_CONTINUE:
    type = Transport_continue;
    break;
  case SND_SEQ_EVENT_STOP:
    type = Transport_stop;
    break;
  case SND_SEQ_EVENT_SONGPOS: {
    int v = ev->data.control.value;
    if (v < 0)
      v = 0;
    if (v > 16383)
      v = 16383;
    value = (U16)v;
    type = Transport_songpos;
    break;
  }
  default:
    return false;
  }
  *out = (Transport_event){.type = type, .value = value, .stamp = stamp};
  return true;
}

// What a System Announce (0:1) event means for the stored source:
// clear it on CLIENT_EXIT naming its client, PORT_EXIT naming its address,
// or PORT_UNSUBSCRIBED for its connection to MIDI in (own_client:own_in_port),
// since the kernel has already dropped the subscription (a whole client's
// exit announces only CLIENT_EXIT); ask for a re-match on PORT_START while
// nothing is stored; keep it otherwise.
typedef enum {
  Alsa_in_keep,
  Alsa_in_clear,
  Alsa_in_rematch,
} Alsa_in_announce_action;

static inline Alsa_in_announce_action
alsa_in_announce(snd_seq_event_t const *ev, Alsa_in_addr const *stored,
                 int own_client, int own_in_port) {
  switch (ev->type) {
  case SND_SEQ_EVENT_CLIENT_EXIT:
    return stored->has && ev->data.addr.client == stored->client
               ? Alsa_in_clear
               : Alsa_in_keep;
  case SND_SEQ_EVENT_PORT_EXIT:
    return stored->has && ev->data.addr.client == stored->client &&
                   ev->data.addr.port == stored->port
               ? Alsa_in_clear
               : Alsa_in_keep;
  case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
    return stored->has && ev->data.connect.sender.client == stored->client &&
                   ev->data.connect.sender.port == stored->port &&
                   ev->data.connect.dest.client == own_client &&
                   ev->data.connect.dest.port == own_in_port
               ? Alsa_in_clear
               : Alsa_in_keep;
  case SND_SEQ_EVENT_PORT_START:
    return stored->has ? Alsa_in_keep : Alsa_in_rematch;
  default:
    return Alsa_in_keep;
  }
}

// The sequencer keeps at most this many bytes of a client name (its
// snd_seq_client_info name is 64 bytes with the terminator) and cuts a
// longer name short without an error, so the name scan could never match it.
// Bytes, not characters: the limit also keeps ALSA from splitting a
// multi-byte UTF-8 character.
enum { Alsa_client_name_max = 63 };

// True when the sequencer would keep name whole; alsa_ctx_open refuses a
// name that does not fit, before it opens the sequencer.
static inline bool alsa_client_name_fits(char const *name) {
  return strlen(name) <= Alsa_client_name_max;
}

// The client-name scan that alsa_ctx_open runs before and after taking its
// name. next(arg, &client, &name) yields one client per call: >= 0
// with both filled in, -ENOENT at the end of the list, or another negative
// error. alsa_name_scan returns 1 as soon as a client other than own is
// named name, 0 when next returns -ENOENT first, and otherwise next's
// negative result, so a failed scan is an error and never a free name (the
// appliance router's find_source ends its list on -ENOENT the same way).
typedef int (*Alsa_name_next)(void *arg, int *client, char const **name);

static inline int alsa_name_scan(Alsa_name_next next, void *arg, int own,
                                 char const *name) {
  for (;;) {
    int client;
    char const *client_name;
    int err = next(arg, &client, &client_name);
    if (err < 0)
      return err == -ENOENT ? 0 : err;
    if (client != own && strcmp(client_name, name) == 0)
      return 1;
  }
}
#endif
