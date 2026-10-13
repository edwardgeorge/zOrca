// The non-call canary for tests/check-includes.sh (architecture spine
// AD-1, AD-2). random, initstate and setstate are banned as calls only: the
// name, then optional white space and "(". This file holds the three only
// as words that are not calls, as CORE does (the R operator's X-macro token
// random, prng.h's parameter name initstate) and more, and none of the
// banned names. The check scans it after the canary, the same way as a
// CORE file, and nothing may be reported for it: if anything is, the call
// style matches a non-call use and the run fails. Its code must also keep
// each of the three as a word, and canary_random( as the witness for the
// call style's left boundary, or the check is blind and the run fails.
// No build target compiles this file. It is valid C under gcc -std=gnu99
// (or -std=c99 -D_DEFAULT_SOURCE), where glibc's <stdlib.h> declares
// random_r, initstate_r, setstate_r and struct random_data; CORE's
// -std=c99 declares none of them.
#include <stdint.h>
#include <stdlib.h>

struct canary_state {
  int random;
  long (*setstate)(long);
};

#define CANARY_OPERATORS(_) _('R', random)

// Comments do not count: random(1), initstate(1, buf, 8), setstate (buf).
static unsigned long canary_seed(unsigned long *state,
                                 unsigned long initstate,
                                 unsigned long initseq) {
  *state += initstate;
  return initseq;
}

static long canary_identity(long x) { return x; }
static int canary_random(int x) { return x; }

int canary_noncalls(struct canary_state *s);
int canary_noncalls(struct canary_state *s) {
  int random = s->random;
  long (*setstate)(long) = canary_identity;
  struct random_data data = {0};
  int32_t result = 0;
  char buf[64] = {0};
  unsigned long state = 0;
  s->setstate = setstate;
  (void)canary_seed(&state, (unsigned long)random, 1);
  (void)random_r(&data, &result);
  (void)initstate_r(1, buf, sizeof buf, &data);
  (void)setstate_r(buf, &data);
  // A directive that ends in a name, then a line that starts with "(".
#define CANARY_SEED_NAME initstate
  (void)state;
  return canary_random(random) + (random & 1) + (int)result;
}
