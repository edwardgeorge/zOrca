// The clock master of tests/alsa-clock/devcheck.sh: a
// sequencer client NAME with one port, "clock", that others can read and
// subscribe to. After DELAY_MS it sends FA, then CLOCKS F8 pulses at BPM
// (24 a beat), timed with clock_nanosleep on CLOCK_MONOTONIC, each to the
// port's subscribers. After each send it prints "<us> FA" or "<us> F8", the
// CLOCK_MONOTONIC time in microseconds, one line each. A failed send is
// reported on stderr and the run goes on.
//
//   master NAME BPM DELAY_MS CLOCKS
//
// Exit status: 0, 1 when the sequencer cannot be opened or the port made,
// 2 on usage.
#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static snd_seq_t *seq;
static int port;

static long long mono_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static void send_type(snd_seq_event_type_t type, char const *label) {
  snd_seq_event_t ev;
  snd_seq_ev_clear(&ev);
  ev.type = type;
  snd_seq_ev_set_source(&ev, (unsigned char)port);
  snd_seq_ev_set_subs(&ev);
  snd_seq_ev_set_direct(&ev);
  int err = snd_seq_event_output_direct(seq, &ev);
  if (err < 0)
    fprintf(stderr, "master: send %s: %s\n", label, snd_strerror(err));
  printf("%lld %s\n", mono_us(), label);
}

static void add_ns(struct timespec *t, long ns) {
  t->tv_nsec += ns;
  while (t->tv_nsec >= 1000000000L) {
    t->tv_nsec -= 1000000000L;
    t->tv_sec += 1;
  }
}

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: master NAME BPM DELAY_MS CLOCKS\n");
    return 2;
  }
  double bpm = atof(argv[2]);
  long delay_ms = atol(argv[3]);
  long clocks = atol(argv[4]);
  if (bpm <= 0.0 || delay_ms < 0 || clocks < 0) {
    fprintf(stderr,
            "master: BPM must be above 0, DELAY_MS and CLOCKS 0 or more\n");
    return 2;
  }
  int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0);
  if (err < 0) {
    fprintf(stderr, "master: open: %s\n", snd_strerror(err));
    return 1;
  }
  snd_seq_set_client_name(seq, argv[1]);
  port = snd_seq_create_simple_port(
      seq, "clock", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  if (port < 0) {
    fprintf(stderr, "master: port: %s\n", snd_strerror(port));
    snd_seq_close(seq);
    return 1;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  struct timespec next;
  clock_gettime(CLOCK_MONOTONIC, &next);
  add_ns(&next, delay_ms * 1000000L);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {
  }
  send_type(SND_SEQ_EVENT_START, "FA");
  long interval_ns = (long)(60e9 / bpm / 24.0);
  for (long k = 0; k < clocks; ++k) {
    send_type(SND_SEQ_EVENT_CLOCK, "F8");
    add_ns(&next, interval_ns);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {
    }
  }
  snd_seq_close(seq);
  return 0;
}
