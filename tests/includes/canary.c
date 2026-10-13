// The canary for tests/check-includes.sh (architecture spine AD-1, AD-2).
// The check scans this file first, the same way as a CORE file, and must
// report all thirteen planted violations for it on every run: <stdio.h>,
// each of the eleven banned names (rand, srand, srandom, drand48, lrand48,
// srand48, seed48, mrand48, lcong48, getenv, secure_getenv), one call each,
// and "tick.h", a shell header included from outside the shell tier. If it
// misses one, the check is blind to that pattern and fails. This file is not
// in the check's shell_files, so "tick.h" is never allowed for it: it is
// reported, never queued. No build target compiles this file.
#include <stdio.h>
#include <stdlib.h>
#include "tick.h"

int canary_rand(void);
int canary_rand(void) {
  unsigned short seed[3] = {1, 2, 3}, param[7] = {1, 2, 3, 4, 5, 6, 7};
  srand(1);
  srandom(1);
  srand48(1);
  (void)seed48(seed);
  lcong48(param);
  return rand() + (int)lrand48() + (int)drand48() + (int)mrand48() +
         (getenv("HOME") != NULL) + (secure_getenv("HOME") != NULL);
}
