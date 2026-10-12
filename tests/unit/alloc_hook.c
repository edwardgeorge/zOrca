// The unit runner's realloc hook (P3-2, deferred-work #17). tool links the
// test target with -Wl,--wrap=realloc -fno-builtin-realloc
// -DUNIT_TESTS_REALLOC_HOOK on Linux with gcc or clang, so every realloc
// call in the runner, CORE's included, reaches __wrap_realloc. A test arms
// the hook by setting unit_realloc_fail just before the call under test and
// disarms it right after, before any CHECK: while it is set every realloc
// returns NULL and touches nothing. unit_realloc_calls counts every call,
// armed or not, so a test can prove that exactly one realloc ran in its
// window and no other realloc entered it (malloc and calloc are not
// wrapped). Never arm it where an Oevent_list can grow: vmio.c writes
// through an unchecked realloc, so the test would crash instead of failing.
// Only the link routes realloc to the hook, so LTO sees no reader of the
// armed flag and could delete the arming store before the disarm; volatile
// keeps it (and -fno-builtin-realloc already makes realloc opaque, so
// volatile is a second safeguard). main.c resets both around each test, so
// a test that returns armed cannot fail another test's realloc.
//
// Without the define this file holds only the declarations from tests.h.
#include "tests.h"

#ifdef UNIT_TESTS_REALLOC_HOOK
volatile bool unit_realloc_fail = false;
volatile size_t unit_realloc_calls = 0;

void *__wrap_realloc(void *ptr, size_t size) {
  ++unit_realloc_calls;
  if (unit_realloc_fail)
    return NULL;
  return __real_realloc(ptr, size);
}

// The hook reaches a realloc whose pointer the compiler can prove NULL.
// Without -fno-builtin-realloc, any optimised build (the debug runner's -Og
// or the release runner's -O2 -flto) may fold such a call into malloc past
// the wrap: it would succeed and leave the count alone. So this test is what
// proves the flag is in tool's line; tick_note_alloc_fail's armed calls read
// their pointer from memory and pass either way.
void test_realloc_hook_builtin_null(void) {
  size_t calls = unit_realloc_calls;
  unit_realloc_fail = true;
  void *p = realloc(NULL, 64);
  unit_realloc_fail = false;
  CHECK(p == NULL && unit_realloc_calls - calls == 1);
  free(p);
}
#endif
