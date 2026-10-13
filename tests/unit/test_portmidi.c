// Adapter tests for PortMidi. They build only with --portmidi
// (FEAT_PORTMIDI) and may include its header. Without the flag this file
// holds only the declarations from tests.h.
#include "tests.h"

#ifdef FEAT_PORTMIDI
#include "../../midi_in.h"
#include <portmidi.h>
#include <stddef.h>
#include <stdint.h>

// Proves that the FEAT_PORTMIDI test build compiles, links and runs PortMidi
// code, and that the filter bits the clock input needs do not overlap.
void test_portmidi_error_text_and_filters(void) {
  // pmNoError returns "" in PortMidi 217, so probe an error code instead.
  char const *text = Pm_GetErrorText(pmInvalidDeviceId);
  CHECK(text != NULL);
  CHECK(text != NULL && text[0] != '\0');
  // The filter bits the clock input needs are set and do not overlap.
  int filt_clock = PM_FILT_CLOCK;
  int filt_play = PM_FILT_PLAY;
  int filt_song_position = PM_FILT_SONG_POSITION;
  CHECK(filt_clock != 0);
  CHECK(filt_play != 0);
  CHECK(filt_song_position != 0);
  CHECK((filt_clock & filt_play) == 0);
  CHECK((filt_clock & filt_song_position) == 0);
  CHECK((filt_play & filt_song_position) == 0);
}

// PortMidi 217's input filter, as pm_read_short applies it (portmidi.c:
// pm_status_filtered, is_real_time and pm_realtime_filtered), with unsigned
// shifts: a message is dropped when the bit 16 + (status >> 4) is set, or
// when it is real time (status & 0xF8 == 0xF8) and the bit status & 0xF is
// set. Returns true when the status is dropped.
static bool pm217_drops(int status, uint32_t filters) {
  uint32_t status_bit = 1u << (16u + ((unsigned)status >> 4u));
  bool status_filtered = (status_bit & filters) != 0;
  bool real_time = (status & 0xF8) == 0xF8;
  bool realtime_filtered =
      ((status & 0xF0) == 0xF0) &&
      ((1u << ((unsigned)status & 0xFu)) & filters) != 0;
  return status_filtered || (real_time && realtime_filtered);
}

// The drop mask: F8, FA, FB, FC and F2 pass; active sensing,
// notes and controls drop; bit 31 is clear; the mask shares no bit with
// CLOCK, PLAY or SONG_POSITION; and its complement, which is never used,
// would drop F8.
void test_portmidi_in_filter(void) {
  uint32_t const mask = (uint32_t)BORCA_PM_IN_DROP;
  CHECK(!pm217_drops(0xF8, mask));
  CHECK(!pm217_drops(0xFA, mask));
  CHECK(!pm217_drops(0xFB, mask));
  CHECK(!pm217_drops(0xFC, mask));
  CHECK(!pm217_drops(0xF2, mask));
  CHECK(pm217_drops(0xFE, mask));
  CHECK(pm217_drops(0x90, mask));
  CHECK(pm217_drops(0xB0, mask));
  CHECK(pm217_drops(0xF9, mask));
  CHECK(pm217_drops(0xFD, mask));
  CHECK(pm217_drops(0xFF, mask));
  // PortMidi has no filter bit for F4 and F5: they pass, and the parser
  // rejects them.
  CHECK(!pm217_drops(0xF4, mask));
  CHECK(!pm217_drops(0xF5, mask));
  Transport_event e;
  CHECK(!transport_parse(0xF4, 0, 0, 1, &e));
  CHECK(!transport_parse(0xF5, 0, 0, 1, &e));
  CHECK((mask & (1u << 31u)) == 0);
  CHECK((mask & (uint32_t)PM_FILT_CLOCK) == 0);
  CHECK((mask & (uint32_t)PM_FILT_PLAY) == 0);
  CHECK((mask & (uint32_t)PM_FILT_SONG_POSITION) == 0);
  CHECK(pm217_drops(0xF8, ~mask));
  // The mask is the explicit list, so each listed kind is in it.
  CHECK((mask & (uint32_t)PM_FILT_ACTIVE) != 0);
  CHECK((mask & (uint32_t)PM_FILT_SYSEX) != 0);
  CHECK((mask & (uint32_t)PM_FILT_NOTE) == (uint32_t)PM_FILT_NOTE);
  CHECK((mask & (uint32_t)PM_FILT_CONTROL) != 0);
  CHECK((mask & (uint32_t)PM_FILT_PITCHBEND) != 0);
}

// The reader's decode (pm_in_decode) on PortMidi's own message
// packing: F2 with Data1 16 is song position 16 (Data1 and Data2 swapped
// would give 16 << 7) and F2 with Data1 16 and Data2 1 is 144 (Data2 is the
// high seven bits), FA is start, FB continue, FC stop and F8 a clock, each
// with its stamp, and a note is refused with *out left alone. Applied in
// order to a reset machine, as the shell applies them: F2 sets tick
// 16, FB starts, and of six F8s only the first makes a tick due, so a tick
// counter ends at 17.
void test_portmidi_in_decode(void) {
  Transport_event ev[8];
  CHECK(pm_in_decode(Pm_Message(0xF2, 16, 0), 1, &ev[0]));
  CHECK(ev[0].type == Transport_songpos && ev[0].value == 16 &&
        ev[0].stamp == 1);
  CHECK(pm_in_decode(Pm_Message(0xFB, 0, 0), 2, &ev[1]));
  CHECK(ev[1].type == Transport_continue && ev[1].stamp == 2);
  for (int k = 0; k < 6; ++k) {
    U64 stamp = (U64)(3 + k);
    CHECK(pm_in_decode(Pm_Message(0xF8, 0, 0), stamp, &ev[2 + k]));
    CHECK(ev[2 + k].type == Transport_clock && ev[2 + k].stamp == stamp);
  }
  Transport_event note = {.type = Transport_stop, .value = 7, .stamp = 99};
  CHECK(!pm_in_decode(Pm_Message(0x90, 60, 100), 9, &note));
  CHECK(note.type == Transport_stop && note.value == 7 && note.stamp == 99);
  Transport_event e;
  CHECK(pm_in_decode(Pm_Message(0xF2, 16, 1), 10, &e));
  CHECK(e.type == Transport_songpos && e.value == 144 && e.stamp == 10);
  CHECK(pm_in_decode(Pm_Message(0xFA, 0, 0), 11, &e));
  CHECK(e.type == Transport_start && e.stamp == 11);
  CHECK(pm_in_decode(Pm_Message(0xFC, 0, 0), 12, &e));
  CHECK(e.type == Transport_stop && e.stamp == 12);

  Transport m;
  transport_reset(&m);
  Usz tick = 0, due = 0;
  bool playing = false, set_16 = false, first_due = false;
  for (int i = 0; i < 8; ++i) {
    Transport_actions act;
    transport_event(&m, &ev[i], &act);
    if (act.stop || act.clock_lost)
      playing = false;
    if (act.set_tick) {
      tick = act.tick;
      set_16 = i == 0 && act.tick == 16;
    }
    if (act.start)
      playing = true;
    if (act.tick_due && playing) {
      ++tick;
      ++due;
      first_due = i == 2;
    }
  }
  CHECK(set_16 && playing);
  CHECK(due == 1 && first_due);
  CHECK(tick == 17);
}
#endif
