#pragma once
#include "base.h"
#include "transport.h"
#include <stdio.h>

// The MIDI input side of the shell (architecture spine AD-2, AD-16, AD-17):
// the pure parts, shared by tui_main.c and the unit tests, which link none of
// tui_main.c. Everything here is static inline, reads no clock and does no
// I/O. The backend-specific helpers sit behind their FEAT_ flags and may
// include that backend's header (an adapter, not core). BORCA_HAS_MIDI_IN is
// defined when a build has a reader; with none, External is hidden and the
// --clock-source and --midi-in flags are unknown options.

#if defined(FEAT_ALSA) || defined(FEAT_PORTMIDI)
#define BORCA_HAS_MIDI_IN 1
#endif

// The clock source (AD-15, AD-20). Internal is the default and the 4f349cd
// timing path; External follows a MIDI clock through the transport machine.
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

// Re-arming the internal deadline so the tick grid is kept (AD-16). The
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

#ifdef FEAT_PORTMIDI
#include <portmidi.h>

// What the PortMidi input drops (AD-17): an explicit list, never a
// complement, so transport_parse, which accepts only F8, FA, FB, FC and F2,
// sees no notes, controls or realtime noise. PortMidi has no filter bit for
// F4 and F5, which pass the mask and are rejected by the parser.
// Pm_SetFilter takes it right after Pm_OpenInput.
#define BORCA_PM_IN_DROP                                                       \
  (PM_FILT_ACTIVE | PM_FILT_SYSEX | PM_FILT_TICK | PM_FILT_UNDEFINED |         \
   PM_FILT_RESET | PM_FILT_MTC | PM_FILT_SONG_SELECT | PM_FILT_TUNE |          \
   PM_FILT_NOTE | PM_FILT_AFTERTOUCH | PM_FILT_PROGRAM | PM_FILT_CONTROL |     \
   PM_FILT_PITCHBEND)
#endif

#ifdef FEAT_ALSA
#include <alsa/asoundlib.h>

// The source bOrca has subscribed, while one is stored (AD-17). The shell is
// unsubscribed while has is false, and System Announce decides when it is
// cleared or matched again.
typedef struct {
  bool has;
  int client, port;
} Alsa_in_addr;

// Maps a sequencer event to a transport event: CLOCK, START, CONTINUE, STOP
// and SONGPOS, whose position (data.control.value) is clamped to 0..16383.
// False for every other event, which the reader discards. ev->time is never
// read: stamp is the shell's now_us() at read time (AD-13).
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

// What a System Announce (0:1) event means for the stored source (AD-17):
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
#endif
