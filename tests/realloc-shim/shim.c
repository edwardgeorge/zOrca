// The allocation shim for tests/realloc-shim/check.sh: an
// LD_PRELOAD library that fails every realloc above SHIM_REALLOC_MAX bytes
// and every malloc above SHIM_MALLOC_MAX bytes with ENOMEM (each unset or
// empty: none fails), as an allocator out of memory would. A malloc within
// its limit goes to glibc's __libc_malloc, not to a dlsym lookup, which can
// itself allocate. For release builds only: ASan refuses a preloaded library
// ahead of its runtime.
//   cc -O2 -shared -fPIC -o shim.so shim.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

extern void *__libc_malloc(size_t size);

static void *(*real_realloc)(void *, size_t);
static size_t limit = SIZE_MAX;
static int malloc_limit_read;
static size_t malloc_limit = SIZE_MAX;

void *malloc(size_t size) {
  if (!malloc_limit_read) {
    char const *s = getenv("SHIM_MALLOC_MAX");
    if (s && *s)
      malloc_limit = (size_t)strtoull(s, NULL, 10);
    malloc_limit_read = 1;
  }
  if (size > malloc_limit) {
    errno = ENOMEM;
    return NULL;
  }
  return __libc_malloc(size);
}

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
