// The canary for tests/check-includes.sh (architecture spine AD-1, AD-2).
// The check scans this file first, the same way as a CORE file, and must
// report all eight planted violations for it on every run: <stdio.h>, each
// of the six banned names (rand, srand, srandom, drand48, lrand48, getenv),
// one call each, and "tick.h", a shell header included from outside the
// shell tier. If it misses one, the check is blind to that pattern and
// fails. This file is not in the check's shell_files, so "tick.h" is never
// allowed for it: it is reported, never queued. No build target compiles
// this file.
#include <stdio.h>
#include <stdlib.h>
#include "tick.h"

int canary_rand(void);
int canary_rand(void) {
  srand(1);
  srandom(1);
  return rand() + (int)lrand48() + (int)drand48() + (getenv("HOME") != NULL);
}
