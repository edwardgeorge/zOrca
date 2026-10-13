// The canary for tests/check-includes.sh (the core include and purity rules).
// The check scans this file first, the same way as a CORE file, and must
// report all nineteen planted violations for it on every run: <stdio.h>;
// each of the fourteen banned names (rand, srand, srandom, drand48,
// lrand48, srand48, seed48, mrand48, lcong48, getenv, secure_getenv,
// environ, __environ, _environ), one call or read each; each of the three
// banned calls (random, initstate, setstate), one call each, initstate's
// with a backslash-newline before its "(", random's with a space and
// setstate's with its "(" on the next line, so each form has one name to
// pin it; and "tick.h", a shell header included from outside the shell
// tier. If it misses one, the check is blind to that pattern and fails.
// __environ needs no declaration here: glibc's <unistd.h> declares it. This
// file is not in the check's shell_files, so "tick.h" is never allowed for
// it: it is reported, never queued. No build target compiles this file.
#include <stdio.h>
#include <stdlib.h>
#include "tick.h"

extern char **environ;
extern char **_environ;

int canary_rand(void);
int canary_rand(void) {
  unsigned short seed[3] = {1, 2, 3}, param[7] = {1, 2, 3, 4, 5, 6, 7};
  static char buf[64];
  srand(1);
  srandom(1);
  srand48(1);
  (void)seed48(seed);
  lcong48(param);
  (void)initstate\
(1, buf, sizeof buf);
  (void)setstate
      (buf);
  return rand() + (int)lrand48() + (int)drand48() + (int)mrand48() +
         (int)random () + (getenv("HOME") != NULL) +
         (secure_getenv("HOME") != NULL) + (environ != NULL) +
         (__environ != NULL) + (_environ != NULL);
}
