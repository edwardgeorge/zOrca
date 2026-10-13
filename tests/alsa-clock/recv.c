// The receiver of tests/alsa-clock/devcheck.sh: a
// sequencer client NAME whose port "in" subscribes to SRC (client:port,
// names allowed), with an input pool of 2000 events, so a burst of notes
// from a resuming orca is not lost here. It prints "<us> on <channel> <note>
// <velocity>" for each note-on with a velocity above 0 and "<us> off
// <channel> <note>" for each note-off (or note-on at velocity 0), the
// CLOCK_MONOTONIC time in microseconds at read time, one line each, for
// SECS seconds or until a signal ends it.
//
//   recv NAME SRC SECS
//
// Exit status: 0, 1 when the sequencer, the port or the subscription fails,
// 2 on usage.
#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long long mono_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: recv NAME SRC SECS\n");
    return 2;
  }
  snd_seq_t *seq;
  int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
  if (err < 0) {
    fprintf(stderr, "recv: open: %s\n", snd_strerror(err));
    return 1;
  }
  snd_seq_set_client_name(seq, argv[1]);
  (void)snd_seq_set_client_pool_input(seq, 2000);
  int port = snd_seq_create_simple_port(
      seq, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_APPLICATION);
  snd_seq_addr_t src;
  if (port < 0 || snd_seq_parse_address(seq, &src, argv[2]) < 0 ||
      snd_seq_connect_from(seq, port, src.client, src.port) < 0) {
    fprintf(stderr, "recv: cannot subscribe to %s\n", argv[2]);
    snd_seq_close(seq);
    return 1;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  long long end = mono_us() + atoll(argv[3]) * 1000000LL;
  int nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
  if (nfds < 1 || nfds > 4) {
    fprintf(stderr, "recv: %d poll descriptors\n", nfds);
    snd_seq_close(seq);
    return 1;
  }
  struct pollfd pfd[4];
  snd_seq_poll_descriptors(seq, pfd, (unsigned)nfds, POLLIN);
  while (mono_us() < end) {
    if (poll(pfd, (nfds_t)nfds, 100) <= 0)
      continue;
    snd_seq_event_t *ev;
    while (snd_seq_event_input(seq, &ev) >= 0) {
      long long t = mono_us();
      if (ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0)
        printf("%lld on %d %d %d\n", t, ev->data.note.channel,
               ev->data.note.note, ev->data.note.velocity);
      else if (ev->type == SND_SEQ_EVENT_NOTEOFF ||
               ev->type == SND_SEQ_EVENT_NOTEON)
        printf("%lld off %d %d\n", t, ev->data.note.channel,
               ev->data.note.note);
    }
  }
  snd_seq_close(seq);
  return 0;
}
