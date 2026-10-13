// LD_PRELOAD shim for the ALSA name-guard device check,
// tests/alsa-guard/devcheck.sh (the ALSA name guard, since v3.0.0). It
// wraps three libasound calls that alsa_ctx_open makes, by mode in
// BORCA_GUARD_SHIM:
//
//   hold  before the first real snd_seq_set_client_name, open a second
//         snd_seq_t in this process and give it the same name: the state a
//         lost race leaves (another client took the name between the first
//         scan and the rename), made deterministic.
//   eio   snd_seq_query_next_client returns -EIO on every call, so the first
//         scan fails.
//   eio2  snd_seq_query_next_client returns -EIO only once
//         snd_seq_set_client_name has been called, so only the second scan
//         fails.
//   (any other value, or unset: every wrapper passes through)
//
// It also counts the snd_seq_create_simple_port calls that succeed, and an
// atexit handler prints "pid <n> ports created <k>" to stderr, but only in a
// process that called one of the wrappers.
//
// Build: gcc -shared -fPIC -o shim.so shim.c -ldl -lasound
// Run (the shim loaded by orca only, never by timeout, script or the shell):
//   script -qec "timeout --foreground -k 1 3 env XDG_CONFIG_HOME=<scratch>
//                BORCA_ALSA_CLIENT_NAME=guard-<pid>-<check>
//                LD_PRELOAD=shim.so BORCA_GUARD_SHIM=hold orca <patch>"
//                /dev/null
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int used, renamed, ports;
static snd_seq_t *holder;

static void report(void) {
  fprintf(stderr, "pid %d ports created %d\n", (int)getpid(), ports);
}

static void note_used(void) {
  if (!used) {
    used = 1;
    atexit(report);
  }
}

static char const *mode(void) {
  char const *m = getenv("BORCA_GUARD_SHIM");
  return m ? m : "";
}

int snd_seq_set_client_name(snd_seq_t *seq, const char *name) {
  int (*real)(snd_seq_t *, const char *) =
      (int (*)(snd_seq_t *, const char *))dlsym(RTLD_NEXT,
                                                "snd_seq_set_client_name");
  note_used();
  if (strcmp(mode(), "hold") == 0 && !holder) {
    int e = snd_seq_open(&holder, "default", SND_SEQ_OPEN_DUPLEX,
                         SND_SEQ_NONBLOCK);
    if (e >= 0) {
      e = real(holder, name);
      fprintf(stderr, "shim: holder client %d named '%s' (rename %d)\n",
              snd_seq_client_id(holder), name, e);
    } else {
      fprintf(stderr, "shim: holder open failed: %s\n", snd_strerror(e));
    }
  }
  int err = real(seq, name);
  renamed = 1;
  return err;
}

int snd_seq_query_next_client(snd_seq_t *seq, snd_seq_client_info_t *info) {
  int (*real)(snd_seq_t *, snd_seq_client_info_t *) =
      (int (*)(snd_seq_t *, snd_seq_client_info_t *))dlsym(
          RTLD_NEXT, "snd_seq_query_next_client");
  note_used();
  char const *m = mode();
  if (strcmp(m, "eio") == 0)
    return -EIO;
  if (strcmp(m, "eio2") == 0 && renamed)
    return -EIO;
  return real(seq, info);
}

int snd_seq_create_simple_port(snd_seq_t *seq, const char *name,
                               unsigned int caps, unsigned int type) {
  int (*real)(snd_seq_t *, const char *, unsigned int, unsigned int) =
      (int (*)(snd_seq_t *, const char *, unsigned int, unsigned int))dlsym(
          RTLD_NEXT, "snd_seq_create_simple_port");
  note_used();
  int err = real(seq, name, caps, type);
  if (err >= 0)
    ports++;
  return err;
}
