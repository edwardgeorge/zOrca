/* The receiver of tests/alsa-noteoff/devcheck.sh.
 *
 *   recv <own client name> <orca client name> <input pool> <window ms> <run ms>
 *        [reader]
 *
 * A sequencer client with a tiny input pool, subscribed to the orca's "MIDI
 * out" (port 0). It reads the orca's notes and logs one line each, on stdout:
 *
 *   <CLOCK_MONOTONIC us> on|off <channel> <note>
 *
 * On the first note-on it fills its own input pool, with controller events
 * sent to itself, and then does not read for <window ms>. The orca's
 * note-off, a tick after its note-on, meets a full pool and is refused
 * (-EAGAIN). After the window the receiver reads everything and carries on
 * until <run ms> have passed. Lines starting with '#' are notes for the
 * reader: how many filler events fitted and how the first fill ended.
 * With the sixth argument "reader" it is the other kind of subscriber, the
 * healthy one: a large pool is the caller's choice, it never fills its pool
 * or stops reading, and it logs the same lines. Two receivers on one orca
 * show what a refusal by one subscriber does to the other.
 * It talks only to the orca it is told to and to itself.
 */
#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int find_client(snd_seq_t *seq, char const *name) {
  snd_seq_client_info_t *info = NULL;
  if (snd_seq_client_info_malloc(&info) < 0)
    return -1;
  int found = -1;
  snd_seq_client_info_set_client(info, -1);
  while (found < 0 && snd_seq_query_next_client(seq, info) >= 0)
    if (strcmp(snd_seq_client_info_get_name(info), name) == 0)
      found = snd_seq_client_info_get_client(info);
  snd_seq_client_info_free(info);
  return found;
}

int main(int argc, char **argv) {
  if (argc != 6 && argc != 7) {
    fprintf(stderr, "usage: recv <own name> <orca name> <pool> <window ms> "
                    "<run ms> [reader]\n");
    return 2;
  }
  bool reader = argc == 7 && strcmp(argv[6], "reader") == 0;
  char const *own_name = argv[1];
  char const *orca_name = argv[2];
  int pool = atoi(argv[3]);
  long long window_us = atoll(argv[4]) * 1000LL;
  long long run_us = atoll(argv[5]) * 1000LL;

  snd_seq_t *seq = NULL;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
    fprintf(stderr, "recv: cannot open the sequencer\n");
    return 1;
  }
  snd_seq_set_client_name(seq, own_name);
  int own = snd_seq_client_id(seq);
  int in_port = snd_seq_create_simple_port(
      seq, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC);
  int fill_port = snd_seq_create_simple_port(
      seq, "fill", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC);
  if (in_port < 0 || fill_port < 0 ||
      snd_seq_set_client_pool_input(seq, (size_t)pool) < 0 ||
      snd_seq_connect_to(seq, fill_port, own, in_port) < 0) {
    fprintf(stderr, "recv: cannot set up its ports and pool\n");
    return 1;
  }
  int orca = find_client(seq, orca_name);
  if (orca < 0 || snd_seq_connect_from(seq, in_port, orca, 0) < 0) {
    fprintf(stderr, "recv: cannot subscribe to %s:0\n", orca_name);
    return 1;
  }

  struct pollfd fds[4];
  int nfds = snd_seq_poll_descriptors(seq, fds, 4, POLLIN);
  long long start = now_us();
  bool filled = false;
  while (now_us() - start < run_us) {
    (void)poll(fds, (nfds_t)nfds, 20);
    snd_seq_event_t *ev;
    while (snd_seq_event_input(seq, &ev) >= 0) {
      if (ev->source.client != orca)
        continue;
      bool on = ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0;
      bool off = ev->type == SND_SEQ_EVENT_NOTEOFF ||
                 (ev->type == SND_SEQ_EVENT_NOTEON &&
                  ev->data.note.velocity == 0);
      if (!on && !off)
        continue;
      printf("%lld %s %d %d\n", now_us(), on ? "on" : "off",
             ev->data.note.channel, ev->data.note.note);
      fflush(stdout);
      if (on && !filled && !reader) {
        filled = true;
        int sent = 0;
        int err = 0;
        for (int i = 0; i < 64; ++i) {
          snd_seq_event_t fill;
          snd_seq_ev_clear(&fill);
          fill.type = SND_SEQ_EVENT_CONTROLLER;
          fill.data.control.channel = 15;
          fill.data.control.param = 1;
          fill.data.control.value = i;
          snd_seq_ev_set_source(&fill, (unsigned char)fill_port);
          snd_seq_ev_set_subs(&fill);
          snd_seq_ev_set_direct(&fill);
          err = snd_seq_event_output_direct(seq, &fill);
          if (err < 0)
            break;
          ++sent;
        }
        printf("# %lld filled the pool: %d filler events fitted, then %d (%s)\n",
               now_us(), sent, err, err < 0 ? snd_strerror(err) : "none");
        fflush(stdout);
        (void)poll(NULL, 0, (int)(window_us / 1000));
        printf("# %lld reading again\n", now_us());
        fflush(stdout);
      }
    }
  }
  printf("# %lld done\n", now_us());
  snd_seq_close(seq);
  return 0;
}
