// Unit tests for field.c, the shell's grid and mark buffers. tool links
// field.c into the runner outside the CORE list: it is libc-only shell code
// (P4-1).
#ifdef UNIT_TESTS_REALLOC_HOOK
#define _POSIX_C_SOURCE 200809L // mkstemp, open, write, close, unlink
#endif
#include "../../field.h"
#include "tests.h"
#ifdef UNIT_TESTS_REALLOC_HOOK
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef UNIT_TESTS_REALLOC_HOOK
// P4-1: grid and mark buffers that cannot grow. The runner's realloc hook
// (alloc_hook.c) returns NULL from every realloc while unit_realloc_fail is
// set; each part arms it just before the call under test, disarms it right
// after, before any CHECK, and checks how many reallocs ran in its window.
// Part 1, field_resize_raw of a 2 x 3 field of 'a' to 40 x 50: false, and
// the buffer, the size and the contents as they were. Part 2, field_copy of
// such a field into a 1 x 1 field holding 'z': false, the destination as it
// was; unarmed, a full copy. Part 3, a resize to 0 x 0: true with no realloc
// (glibc's realloc(p, 0) frees p and returns NULL), the buffer NULL. Part 4,
// a mark buffer of capacity 6 that cannot grow to 40 x 50: false, the buffer
// and the capacity as they were; a 2 x 2 request fits with no realloc.
// Part 5, field_load_file of a 2 x 3 grid file into a 1 x 1 field holding
// 'z', whose first row cannot grow the field: Field_load_error_no_memory,
// the file closed and the field as it was. Part 6,
// field_resize_raw_if_necessary to the size the field has: true with no
// realloc. Each part has its own buffers, and reads a buffer only at the size
// it holds, so a broken resize fails the checks instead of ending the run in
// an ASan abort.
void test_field_alloc_fail(void) {
  // Part 1: the resize fails.
  Field f;
  field_init_fill(&f, 2, 3, 'a');
  Glyph *buffer = f.buffer;
  size_t calls = unit_realloc_calls;
  unit_realloc_fail = true;
  bool resized = field_resize_raw(&f, 40, 50);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 1);
  CHECK(!resized);
  CHECK(f.buffer == buffer && f.height == 2 && f.width == 3);
  if (f.buffer == buffer)
    CHECK(memcmp(f.buffer, "aaaaaa", 6) == 0);
  field_deinit(&f);

  // Part 2: the copy's resize fails, then succeeds.
  Field src, dest;
  field_init_fill(&src, 2, 3, 'a');
  field_init_fill(&dest, 1, 1, 'z');
  Glyph *dest_buffer = dest.buffer;
  calls = unit_realloc_calls;
  unit_realloc_fail = true;
  bool copied = field_copy(&src, &dest);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 1);
  CHECK(!copied);
  CHECK(dest.buffer == dest_buffer && dest.height == 1 && dest.width == 1);
  if (dest.buffer == dest_buffer)
    CHECK(dest.buffer[0] == 'z');
  // Unarmed, the same copy. Only into an intact destination: one that kept
  // a size its buffer does not hold would be written past its end.
  if (dest.buffer == dest_buffer && dest.height == 1 && dest.width == 1) {
    copied = field_copy(&src, &dest);
    CHECK(copied && dest.height == 2 && dest.width == 3);
    if (copied && dest.height == 2 && dest.width == 3)
      CHECK(memcmp(dest.buffer, "aaaaaa", 6) == 0);
  }
  field_deinit(&src);
  field_deinit(&dest);

  // Part 3: zero cells, armed, so a realloc would fail.
  Field g;
  field_init_fill(&g, 2, 3, 'a');
  calls = unit_realloc_calls;
  unit_realloc_fail = true;
  resized = field_resize_raw(&g, 0, 0);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 0);
  CHECK(resized);
  CHECK(g.buffer == NULL && g.height == 0 && g.width == 0);
  field_deinit(&g);

  // Part 4: the mark buffer.
  Mbuf_reusable mbr;
  mbuf_reusable_init(&mbr);
  CHECK(mbuf_reusable_ensure_size(&mbr, 2, 3));
  CHECK(mbr.buffer != NULL && mbr.capacity == 6);
  Mark *marks = mbr.buffer;
  calls = unit_realloc_calls;
  unit_realloc_fail = true;
  bool grown = mbuf_reusable_ensure_size(&mbr, 40, 50);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 1);
  CHECK(!grown);
  CHECK(mbr.buffer == marks && mbr.capacity == 6);
  calls = unit_realloc_calls;
  unit_realloc_fail = true;
  grown = mbuf_reusable_ensure_size(&mbr, 2, 2);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 0);
  CHECK(grown);
  CHECK(mbr.buffer == marks && mbr.capacity == 6);
  mbuf_reusable_deinit(&mbr);

  // Part 5: a load whose first row cannot grow the field.
  CHECK(strcmp(field_load_error_string(Field_load_error_no_memory),
               "Not enough memory for the grid") == 0);
  char path[] = "/tmp/borca-unit-field-XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  if (fd >= 0) {
    static char const text[] = "abc\ndef\n";
    CHECK(write(fd, text, sizeof text - 1) == (ssize_t)(sizeof text - 1));
    close(fd);
    Field h;
    field_init_fill(&h, 1, 1, 'z');
    Glyph *h_buffer = h.buffer;
    // The lowest free descriptor: free again after the load only if the
    // load closed its file.
    int lowest = open("/dev/null", O_RDONLY);
    if (lowest >= 0)
      close(lowest);
    calls = unit_realloc_calls;
    unit_realloc_fail = true;
    Field_load_error fle = field_load_file(path, &h);
    unit_realloc_fail = false;
    CHECK(unit_realloc_calls - calls == 1);
    CHECK(fle == Field_load_error_no_memory);
    int after = open("/dev/null", O_RDONLY);
    CHECK(lowest >= 0 && after == lowest);
    if (after >= 0)
      close(after);
    CHECK(h.buffer == h_buffer && h.height == 1 && h.width == 1);
    if (h.buffer == h_buffer)
      CHECK(h.buffer[0] == 'z');
    field_deinit(&h);
    unlink(path);
  }

  // Part 6: the size the field already has needs no realloc.
  Field k;
  field_init_fill(&k, 2, 3, 'a');
  Glyph *k_buffer = k.buffer;
  calls = unit_realloc_calls;
  unit_realloc_fail = true;
  resized = field_resize_raw_if_necessary(&k, 2, 3);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 0);
  CHECK(resized);
  CHECK(k.buffer == k_buffer && k.height == 2 && k.width == 3);
  field_deinit(&k);
}
#endif
