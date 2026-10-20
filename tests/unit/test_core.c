// Unit tests for the CORE modules. They link CORE against libc only.
#include "../../gbuffer.h"
#include "../../opstate.h"
#include "../../sim.h"
#include "../../vmio.h"
#include "tests.h"

// The relative peek and poke ignore cells outside the grid: a peek returns
// '.', a poke writes nothing, and neither wraps from the end of one row to
// the start of the next.
void test_gbuffer_bounds(void) {
  enum { H = 3, W = 4, Guard = 4 };
  // The grid sits between two guard runs that catch a stray write.
  Glyph mem[Guard + H * W + Guard];
  Glyph *grid = mem + Guard;
  memset(mem, '#', sizeof mem);
  for (Usz i = 0; i < H * W; ++i)
    grid[i] = (Glyph)('a' + i);

  // In bounds: (1, 1) + (1, 2) is (2, 3), the last cell.
  CHECK(gbuffer_peek_relative(grid, H, W, 1, 1, 1, 2) == 'a' + 11);
  // Out of bounds on each side.
  CHECK(gbuffer_peek_relative(grid, H, W, 0, 0, -1, 0) == '.');
  CHECK(gbuffer_peek_relative(grid, H, W, 0, 0, 0, -1) == '.');
  CHECK(gbuffer_peek_relative(grid, H, W, H - 1, 0, 1, 0) == '.');
  CHECK(gbuffer_peek_relative(grid, H, W, 0, W - 1, 0, 1) == '.');
  // Far out of bounds, and past the end of a row instead of wrapping.
  CHECK(gbuffer_peek_relative(grid, H, W, 0, 0, -1000, 1000) == '.');
  CHECK(gbuffer_peek_relative(grid, H, W, 1, W - 1, 0, 1) == '.');

  gbuffer_poke_relative(grid, H, W, 0, 0, -1, 0, 'X');
  gbuffer_poke_relative(grid, H, W, 0, 0, 0, -1, 'X');
  gbuffer_poke_relative(grid, H, W, H - 1, 0, 1, 0, 'X');
  gbuffer_poke_relative(grid, H, W, 0, W - 1, 0, 1, 'X');
  gbuffer_poke_relative(grid, H, W, 1, W - 1, 0, 1, 'X');
  gbuffer_poke_relative(grid, H, W, 0, 0, -1000, 1000, 'X');
  Usz changed = 0;
  for (Usz i = 0; i < H * W; ++i)
    changed += grid[i] != (Glyph)('a' + i);
  CHECK(changed == 0);

  // In bounds, the relative and absolute pokes write the one cell.
  gbuffer_poke_relative(grid, H, W, 0, 0, 2, 3, 'Y');
  CHECK(grid[2 * W + 3] == 'Y');
  gbuffer_poke(grid, H, W, 1, 2, 'Z');
  CHECK(grid[1 * W + 2] == 'Z');

  Usz guards_hit = 0;
  for (Usz i = 0; i < Guard; ++i) {
    guards_hit += mem[i] != '#';
    guards_hit += mem[Guard + H * W + i] != '#';
  }
  CHECK(guards_hit == 0);
}

// The reference implementation in sim.c's comment, the table's oracle.
static Usz glyph_index_reference(Glyph c) {
  if (c >= '0' && c <= '9')
    return (Usz)(c - '0');
  if (c >= 'A' && c <= 'Z')
    return (Usz)(c - 'A' + 10);
  if (c >= 'a' && c <= 'z')
    return (Usz)(c - 'a' + 10);
  return 0;
}

// gbuffer.h's one glyph index table against the reference, over
// every byte: '0'..'9' are 0..9, letters 10..35 in either case, anything
// else 0, and a byte above 127 reads as its low 7 bits. A typo in any of
// the 128 entries fails here.
void test_glyph_index_of_reference(void) {
  Usz mismatches = 0;
  for (int i = 0; i < 256; ++i) {
    Glyph c = (Glyph)i;
    Glyph displayed = orca_glyph_is_tagged(c)
                         ? orca_glyph_character(c)
                         : (Glyph)(i & 0x7f);
    if (glyph_index_of(c) != glyph_index_reference(displayed))
      ++mismatches;
  }
  CHECK(mismatches == 0);
  CHECK(glyph_index_of('0') == 0 && glyph_index_of('9') == 9);
  CHECK(glyph_index_of('a') == 10 && glyph_index_of('Z') == 35);
  CHECK(glyph_index_of('.') == 0 && glyph_index_of('#') == 0);
  CHECK(glyph_index_of(orca_glyph_with_dialect('f', Orca_dialect_upstream)) ==
        15);
}

// Growing past the initial capacity and several reallocations keeps every
// item, in order. Copy and clear keep them too.
void test_oevent_list_growth(void) {
  enum { N = 300 };
  Oevent_list list;
  oevent_list_init(&list);
  CHECK(list.count == 0);
  Usz reallocations = 0;
  Usz last_capacity = list.capacity;
  for (Usz i = 0; i < N; ++i) {
    Oevent *e = oevent_list_alloc_item(&list);
    memset(e, 0, sizeof *e);
    e->midi_note.oevent_type = Oevent_type_midi_note;
    e->midi_note.channel = (U8)(i % 16);
    e->midi_note.note = (U8)(i % 128);
    e->midi_note.velocity = (U8)(i / 128);
    if (list.capacity != last_capacity) {
      ++reallocations;
      last_capacity = list.capacity;
    }
  }
  CHECK(list.count == N);
  CHECK(list.capacity >= N);
  CHECK(reallocations >= 3);
  // Read only the items the list holds, so a growth bug fails the checks
  // instead of ending the run in an ASan abort.
  Usz held = list.count < N ? list.count : N;
  Usz wrong = 0;
  for (Usz i = 0; i < held; ++i) {
    Oevent_midi_note const *n = &list.buffer[i].midi_note;
    wrong += n->oevent_type != Oevent_type_midi_note ||
             n->channel != i % 16 || n->note != i % 128 ||
             n->velocity != i / 128;
  }
  CHECK(wrong == 0);

  Oevent_list copy;
  oevent_list_init(&copy);
  CHECK(oevent_list_copy(&list, &copy));
  CHECK(copy.count == list.count);
  // A count mismatch is already recorded above; compare only equal counts.
  if (copy.count == list.count && list.count == N)
    CHECK(copy.buffer != NULL && list.buffer != NULL &&
          memcmp(copy.buffer, list.buffer, N * sizeof(Oevent)) == 0);

  Usz capacity = list.capacity;
  oevent_list_clear(&list);
  CHECK(list.count == 0);
  CHECK(list.capacity == capacity);
  oevent_list_deinit(&copy);
  oevent_list_deinit(&list);
}

#ifdef UNIT_TESTS_REALLOC_HOOK
// An event list that cannot grow. The runner's realloc hook
// (alloc_hook.c) returns NULL from every realloc while unit_realloc_fail is
// set; each part arms it just before the call under test, disarms it right
// after, before any CHECK, and checks that exactly one realloc ran in its
// window. Part 1, an empty list's first growth: the result is the list's
// discard slot, the list stays NULL/0/0, writing the event there is
// harmless and its dropped count is 1. Part 2, a full list of 16
// (dropped 0): the discard slot again, the buffer, count, capacity and items
// as they were, and dropped 1; a second armed call, the same, and dropped 2.
// Part 3, the same call unarmed: the list grows to 32, the result is the
// 17th slot and dropped stays 2, as it does after oevent_list_clear. Part 4,
// oevent_list_copy into a one-item destination that must grow: false, the
// destination empty with its buffer and capacity; unarmed, a full copy.
// Each part has its own lists, and reads only what a list holds, so a
// broken growth fails the checks instead of ending the run in an ASan abort.
void test_oevent_list_alloc_fail(void) {
  enum { Cap = 16 };
  // Part 1: the empty list.
  Oevent_list empty;
  oevent_list_init(&empty);
  size_t calls = unit_realloc_calls;
  unit_realloc_fail = true;
  Oevent *e = oevent_list_alloc_item(&empty);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 1);
  CHECK(e == &empty.discard);
  e->midi_note.oevent_type = Oevent_type_midi_note; // the caller's write
  e->midi_note.note = 60;
  CHECK(empty.buffer == NULL && empty.count == 0 && empty.capacity == 0);
  CHECK(empty.dropped == 1);
  oevent_list_deinit(&empty);

  // Part 2: a full list of 16 distinct items.
  Oevent_list full;
  oevent_list_init(&full);
  for (Usz i = 0; i < Cap; ++i) {
    Oevent *item = oevent_list_alloc_item(&full);
    memset(item, 0, sizeof *item);
    item->midi_cc.oevent_type = Oevent_type_midi_cc;
    item->midi_cc.channel = (U8)i;
    item->midi_cc.control = (U8)(20 + i);
    item->midi_cc.value = (U8)(40 + i);
  }
  CHECK(full.count == Cap && full.capacity == Cap);
  CHECK(full.dropped == 0);
  Oevent before[Cap];
  memset(before, 0, sizeof before);
  if (full.count == Cap)
    memcpy(before, full.buffer, sizeof before);
  Oevent *buffer = full.buffer;
  calls = unit_realloc_calls;
  unit_realloc_fail = true;
  e = oevent_list_alloc_item(&full);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 1);
  CHECK(e == &full.discard);
  CHECK(full.buffer == buffer && full.count == Cap && full.capacity == Cap);
  CHECK(full.dropped == 1);
  // A second drop: dropped counts on, it is not a flag.
  calls = unit_realloc_calls;
  unit_realloc_fail = true;
  e = oevent_list_alloc_item(&full);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 1);
  CHECK(e == &full.discard);
  CHECK(full.buffer == buffer && full.count == Cap && full.capacity == Cap);
  CHECK(full.dropped == 2);
  if (full.buffer == buffer)
    CHECK(memcmp(full.buffer, before, sizeof before) == 0);

  // Part 3: the same call, unarmed.
  calls = unit_realloc_calls;
  e = oevent_list_alloc_item(&full);
  CHECK(unit_realloc_calls - calls == 1);
  CHECK(e == full.buffer + Cap);
  CHECK(full.count == Cap + 1 && full.capacity == 2 * Cap);
  if (full.buffer != NULL && full.capacity == 2 * Cap)
    CHECK(memcmp(full.buffer, before, sizeof before) == 0);
  CHECK(full.dropped == 2);
  oevent_list_clear(&full);
  CHECK(full.dropped == 2);
  oevent_list_deinit(&full);

  // Part 4: oevent_list_copy into a one-item destination that must grow.
  Oevent_list src, dest;
  oevent_list_init(&src);
  oevent_list_init(&dest);
  for (Usz i = 0; i < Cap + 1; ++i) {
    Oevent *item = oevent_list_alloc_item(&src);
    memset(item, 0, sizeof *item);
    item->midi_pb.oevent_type = Oevent_type_midi_pb;
    item->midi_pb.lsb = (U8)i;
  }
  memset(oevent_list_alloc_item(&dest), 0, sizeof(Oevent));
  CHECK(src.count == Cap + 1 && dest.count == 1 && dest.capacity == Cap);
  Oevent *dest_buffer = dest.buffer;
  calls = unit_realloc_calls;
  unit_realloc_fail = true;
  bool copied = oevent_list_copy(&src, &dest);
  unit_realloc_fail = false;
  CHECK(unit_realloc_calls - calls == 1);
  CHECK(!copied);
  CHECK(dest.count == 0 && dest.buffer == dest_buffer &&
        dest.capacity == Cap);
  copied = oevent_list_copy(&src, &dest);
  CHECK(copied && dest.count == src.count);
  if (copied && dest.count == src.count)
    CHECK(memcmp(dest.buffer, src.buffer, src.count * sizeof(Oevent)) == 0);
  oevent_list_deinit(&src);
  oevent_list_deinit(&dest);
}
#endif

// One tick of a stable operator: A adds its two inputs and writes the sum
// below itself, emits no event and keeps no operator state.
void test_orca_run_smoke(void) {
  enum { H = 3, W = 3 };
  Glyph grid[H * W];
  Mark marks[H * W];
  memcpy(grid, "1A2"
               "..."
               "...",
         sizeof grid);
  memset(marks, 0, sizeof marks);
  Oevent_list events;
  oevent_list_init(&events);
  Opstate_store store;
  opstate_init(&store);
  Orca_run_ctx const ctx = {.opstate = &store};
  orca_run(grid, marks, H, W, 0, &events, 0, &ctx);
  CHECK(grid[1 * W + 1] == '3');
  CHECK(memcmp(grid, "1A2", 3) == 0);
  CHECK(events.count == 0);
  CHECK(store.count == 0);
  oevent_list_deinit(&events);
  opstate_free(&store);
}
