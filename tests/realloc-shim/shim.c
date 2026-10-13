// The realloc shim for tests/realloc-shim/check.sh (P4-1): an LD_PRELOAD
// library that fails every realloc above SHIM_REALLOC_MAX bytes (unset or
// empty: none fails), as an allocator out of memory would. For release
// builds only: ASan refuses a preloaded library ahead of its runtime.
//   cc -O2 -shared -fPIC -o shim.so shim.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

static void *(*real_realloc)(void *, size_t);
static size_t limit = SIZE_MAX;

void *realloc(void *ptr, size_t size) {
  if (!real_realloc) {
    real_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    char const *s = getenv("SHIM_REALLOC_MAX");
    if (s && *s)
      limit = (size_t)strtoull(s, NULL, 10);
  }
  if (size > limit) {
    errno = ENOMEM;
    return NULL;
  }
  return real_realloc(ptr, size);
}
