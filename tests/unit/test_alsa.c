// Adapter tests for ALSA. They build only with --alsa (FEAT_ALSA) and may
// include its header. Without the flag this file holds only the declarations
// from tests.h. They need no sequencer device.
#include "tests.h"

#ifdef FEAT_ALSA
#include "../../midi_in.h"
#include <alsa/asoundlib.h>

// Proves that the FEAT_ALSA test build compiles, links and runs libasound
// code, and pins the open-mode facts the duplex client (MIDI in) relies on.
void test_alsa_version_and_open_modes(void) {
  char const *version = snd_asoundlib_version();
  CHECK(version != NULL);
  CHECK(version != NULL && version[0] != '\0');
  // The client opens duplex for MIDI in: exactly output plus input.
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

// The event mapping: CLOCK, START, CONTINUE, STOP and SONGPOS,
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

// The announce helper with a stored address 20:0 and MIDI in at
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

// The `client:port` split: at the last ':', both sides non-empty.
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

// A fake Alsa_name_next over a fixed list of steps: each step is what one
// call returns, with the client filled in when that is >= 0. The list ends
// in its own terminator step (-ENOENT, or an error); running past it returns
// -EINVAL, so a scan that ignores the end shows up.
typedef struct {
  int result;
  int client;
  char const *name;
} Name_step;

typedef struct {
  Name_step const *steps;
  Usz count, at;
} Name_list;

static int fake_name_next(void *arg, int *client, char const **name) {
  Name_list *l = arg;
  if (l->at >= l->count)
    return -EINVAL;
  Name_step const *s = &l->steps[l->at++];
  if (s->result < 0)
    return s->result;
  *client = s->client;
  *name = s->name;
  return s->result;
}

static int scan_of(Name_step const *steps, Usz count, Usz *calls) {
  Name_list l = {.steps = steps, .count = count, .at = 0};
  int r = alsa_name_scan(fake_name_next, &l, 128, "p35");
  *calls = l.at;
  return r;
}

// The client-name scan, with own client 128 named p35: free
// when the list ends on -ENOENT with only the own client of that name, or
// with other clients whose names only resemble it, or at once; taken as soon
// as another client has it; a scan error before the end of the list is
// returned, mid-list or at once, never read as a free name.
void test_alsa_name_scan(void) {
  Name_step const free_list[] = {{0, 0, "System"},
                                 {0, 14, "Midi Through"},
                                 {0, 128, "p35"},
                                 {.result = -ENOENT}};
  // Near misses: a longer name with p35 as its prefix, and another case.
  Name_step const near_list[] = {{0, 128, "p35"},
                                 {0, 130, "p35x"},
                                 {0, 131, "P35"},
                                 {.result = -ENOENT}};
  Name_step const empty_list[] = {{.result = -ENOENT}};
  Name_step const taken_list[] = {{0, 0, "System"},
                                  {0, 14, "Midi Through"},
                                  {0, 128, "p35"},
                                  {0, 130, "p35"},
                                  {.result = -ENOENT}};
  Name_step const eio_list[] = {{0, 0, "System"},
                                {0, 14, "Midi Through"},
                                {.result = -EIO},
                                {.result = -ENOENT}};
  Name_step const enomem_list[] = {
      {.result = -ENOMEM}, {0, 0, "System"}, {.result = -ENOENT}};
  Usz calls;
  CHECK(scan_of(free_list, ORCA_ARRAY_COUNTOF(free_list), &calls) == 0);
  CHECK(calls == 4); // the whole list, through its -ENOENT
  CHECK(scan_of(near_list, ORCA_ARRAY_COUNTOF(near_list), &calls) == 0);
  CHECK(calls == 4);
  CHECK(scan_of(empty_list, ORCA_ARRAY_COUNTOF(empty_list), &calls) == 0);
  CHECK(calls == 1);
  CHECK(scan_of(taken_list, ORCA_ARRAY_COUNTOF(taken_list), &calls) == 1);
  CHECK(calls == 4); // stops at the hit
  CHECK(scan_of(eio_list, ORCA_ARRAY_COUNTOF(eio_list), &calls) == -EIO);
  CHECK(calls == 3);
  CHECK(scan_of(enomem_list, ORCA_ARRAY_COUNTOF(enomem_list), &calls) ==
        -ENOMEM);
  CHECK(calls == 1);
}

// The client-name length limit: the sequencer keeps 63 bytes, so a
// 63-byte name fits and a 64- or 200-byte one does not. The names are built
// with literal lengths, never from Alsa_client_name_max. The limit is in
// bytes: with a two-byte UTF-8 character at the end, 63 bytes fit and 64 do
// not. The empty string fits, though the shell never asks: alsa_client_name
// falls back to bOrca.
void test_alsa_client_name_fits(void) {
  char name[201];
  CHECK(alsa_client_name_fits("bOrca"));
  memset(name, 'x', 63);
  name[63] = '\0';
  CHECK(alsa_client_name_fits(name));
  memset(name, 'x', 64);
  name[64] = '\0';
  CHECK(!alsa_client_name_fits(name));
  memset(name, 'x', 200);
  name[200] = '\0';
  CHECK(!alsa_client_name_fits(name));
  memset(name, 'x', 62); // 62 + the two bytes of U+00E9: 64 bytes
  memcpy(name + 62, "\xc3\xa9", 3);
  CHECK(!alsa_client_name_fits(name));
  memset(name, 'x', 61); // 61 + 2: 63 bytes
  memcpy(name + 61, "\xc3\xa9", 3);
  CHECK(alsa_client_name_fits(name));
  CHECK(alsa_client_name_fits(""));
}
#endif
