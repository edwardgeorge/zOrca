// Adapter tests for ALSA. They build only with --alsa (FEAT_ALSA) and may
// include its header. Without the flag this file holds only the declarations
// from tests.h. They need no sequencer device.
#include "tests.h"

#ifdef FEAT_ALSA
#include "../../midi_in.h"
#include <alsa/asoundlib.h>

// Proves that the FEAT_ALSA test build compiles, links and runs libasound
// code, and pins the open-mode facts I6's duplex client relies on.
void test_alsa_version_and_open_modes(void) {
  char const *version = snd_asoundlib_version();
  CHECK(version != NULL);
  CHECK(version != NULL && version[0] != '\0');
  // I6 reopens the client duplex; duplex must be exactly output plus input.
  int duplex = SND_SEQ_OPEN_DUPLEX;
  int output = SND_SEQ_OPEN_OUTPUT;
  int input = SND_SEQ_OPEN_INPUT;
  CHECK(duplex == (output | input));
  // The client is opened nonblocking, so the mode flag must be set.
  int nonblock = SND_SEQ_NONBLOCK;
  CHECK(nonblock != 0);
}

static bool maps_to(snd_seq_event_type_t type, int value, Transport_type want,
                    U16 want_value) {
  snd_seq_event_t ev;
  memset(&ev, 0, sizeof ev);
  ev.type = type;
  ev.data.control.value = value;
  ev.time.tick = 12345; // never read
  Transport_event te = {.stamp = 0};
  return alsa_in_map_event(&ev, 77, &te) && te.type == want &&
         te.value == want_value && te.stamp == 77;
}

static bool maps_to_nothing(snd_seq_event_type_t type) {
  snd_seq_event_t ev;
  memset(&ev, 0, sizeof ev);
  ev.type = type;
  Transport_event te = {.stamp = 99};
  return !alsa_in_map_event(&ev, 77, &te) && te.stamp == 99;
}

// The event mapping (I6, AD-17): CLOCK, START, CONTINUE, STOP and SONGPOS,
// whose value is clamped to 0..16383; every other event maps to nothing.
void test_alsa_in_mapping(void) {
  CHECK(maps_to(SND_SEQ_EVENT_CLOCK, 0, Transport_clock, 0));
  CHECK(maps_to(SND_SEQ_EVENT_START, 0, Transport_start, 0));
  CHECK(maps_to(SND_SEQ_EVENT_CONTINUE, 0, Transport_continue, 0));
  CHECK(maps_to(SND_SEQ_EVENT_STOP, 0, Transport_stop, 0));
  CHECK(maps_to(SND_SEQ_EVENT_SONGPOS, 16, Transport_songpos, 16));
  CHECK(maps_to(SND_SEQ_EVENT_SONGPOS, 16383, Transport_songpos, 16383));
  CHECK(maps_to(SND_SEQ_EVENT_SONGPOS, -1, Transport_songpos, 0));
  CHECK(maps_to(SND_SEQ_EVENT_SONGPOS, 20000, Transport_songpos, 16383));
  CHECK(maps_to_nothing(SND_SEQ_EVENT_NOTEON));
  CHECK(maps_to_nothing(SND_SEQ_EVENT_SENSING));
  CHECK(maps_to_nothing(SND_SEQ_EVENT_TICK));
  CHECK(maps_to_nothing(SND_SEQ_EVENT_PORT_SUBSCRIBED));
}

static snd_seq_event_t addr_event(snd_seq_event_type_t type, int client,
                                  int port) {
  snd_seq_event_t ev;
  memset(&ev, 0, sizeof ev);
  ev.type = type;
  ev.data.addr.client = (unsigned char)client;
  ev.data.addr.port = (unsigned char)port;
  return ev;
}

static snd_seq_event_t unsub_event(int sender_client, int sender_port,
                                   int dest_client, int dest_port) {
  snd_seq_event_t ev;
  memset(&ev, 0, sizeof ev);
  ev.type = SND_SEQ_EVENT_PORT_UNSUBSCRIBED;
  ev.data.connect.sender.client = (unsigned char)sender_client;
  ev.data.connect.sender.port = (unsigned char)sender_port;
  ev.data.connect.dest.client = (unsigned char)dest_client;
  ev.data.connect.dest.port = (unsigned char)dest_port;
  return ev;
}

// The announce helper (I6, AD-17) with a stored address 20:0 and MIDI in at
// 130:1: cleared on CLIENT_EXIT of its client, PORT_EXIT of its address and
// PORT_UNSUBSCRIBED of its connection; kept for another client; a re-match
// asked on PORT_START only while nothing is stored.
void test_alsa_in_announce(void) {
  Alsa_in_addr const stored = {.has = true, .client = 20, .port = 0};
  Alsa_in_addr const none = {.has = false, .client = 0, .port = 0};
  snd_seq_event_t ev;
  ev = addr_event(SND_SEQ_EVENT_CLIENT_EXIT, 20, 0);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_clear);
  ev = addr_event(SND_SEQ_EVENT_PORT_EXIT, 20, 0);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_clear);
  ev = unsub_event(20, 0, 130, 1);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_clear);
  ev = addr_event(SND_SEQ_EVENT_CLIENT_EXIT, 21, 0);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_keep);
  ev = addr_event(SND_SEQ_EVENT_PORT_EXIT, 21, 0);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_keep);
  ev = unsub_event(21, 0, 130, 1);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_keep);
  // The same port's connection to another client's port is not ours.
  ev = unsub_event(20, 0, 131, 0);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_keep);
  ev = addr_event(SND_SEQ_EVENT_PORT_START, 20, 0);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_keep);
  CHECK(alsa_in_announce(&ev, &none, 130, 1) == Alsa_in_rematch);
  ev = addr_event(SND_SEQ_EVENT_CLIENT_EXIT, 20, 0);
  CHECK(alsa_in_announce(&ev, &none, 130, 1) == Alsa_in_keep);
  ev = addr_event(SND_SEQ_EVENT_PORT_SUBSCRIBED, 20, 0);
  CHECK(alsa_in_announce(&ev, &stored, 130, 1) == Alsa_in_keep);
}

static bool splits_to(char const *name, char const *client, char const *port) {
  Usz client_len = 0;
  char const *got_port = NULL;
  if (!alsa_in_split(name, &client_len, &got_port))
    return false;
  return client_len == strlen(client) &&
         strncmp(name, client, client_len) == 0 && strcmp(got_port, port) == 0;
}

// The `client:port` split (I6, AD-17): at the last ':', both sides non-empty.
void test_alsa_in_split(void) {
  CHECK(splits_to("Midi Through:Midi Through Port-0", "Midi Through",
                  "Midi Through Port-0"));
  CHECK(splits_to("a:b:c", "a:b", "c"));
  Usz len;
  char const *port;
  CHECK(!alsa_in_split("abc", &len, &port));
  CHECK(!alsa_in_split(":b", &len, &port));
  CHECK(!alsa_in_split("a:", &len, &port));
}
#endif
