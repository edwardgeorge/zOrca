#pragma once
#include "base.h"

// A glyph's value as an operand, kept in this one table: '0'..'9' are 0..9,
// letters are 10..35 in either case ('F' is 'f'), and any other glyph is 0. A
// byte above 127 reads as its low 7 bits. sim.c and tooltips.c both read it;
// test_core.c checks it against the reference implementation in sim.c.
static U8 const glyph_index_table[128] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  //   0-15
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  //  16-31
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  //  32-47
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0,  0,  0,  0,  0,  0,  //  48-63
    0,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, //  64-79
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0,  0,  0,  0,  0,  //  80-95
    0,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, //  96-111
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0,  0,  0,  0,  0}; // 112-127
ORCA_FORCEINLINE static Usz glyph_index_of(Glyph c) {
  return glyph_index_table[c & 0x7f];
}

ORCA_PURE static inline Glyph gbuffer_peek_relative(Glyph *gbuf, Usz height,
                                                    Usz width, Usz y, Usz x,
                                                    Isz delta_y, Isz delta_x) {
  Isz y0 = (Isz)y + delta_y;
  Isz x0 = (Isz)x + delta_x;
  if (y0 < 0 || x0 < 0 || (Usz)y0 >= height || (Usz)x0 >= width)
    return '.';
  return gbuf[(Usz)y0 * width + (Usz)x0];
}

static inline void gbuffer_poke(Glyph *gbuf, Usz height, Usz width, Usz y,
                                Usz x, Glyph g) {
  assert(y < height && x < width);
  (void)height;
  gbuf[y * width + x] = g;
}

static inline void gbuffer_poke_relative(Glyph *gbuf, Usz height, Usz width,
                                         Usz y, Usz x, Isz delta_y, Isz delta_x,
                                         Glyph g) {
  Isz y0 = (Isz)y + delta_y;
  Isz x0 = (Isz)x + delta_x;
  if (y0 < 0 || x0 < 0 || (Usz)y0 >= height || (Usz)x0 >= width)
    return;
  gbuf[(Usz)y0 * width + (Usz)x0] = g;
}

ORCA_NOINLINE
void gbuffer_copy_subrect(Glyph *src, Glyph *dest, Usz src_grid_h,
                          Usz src_grid_w, Usz dest_grid_h, Usz dest_grid_w,
                          Usz src_y, Usz src_x, Usz dest_y, Usz dest_x,
                          Usz height, Usz width);

ORCA_NOINLINE
void gbuffer_fill_subrect(Glyph *gbuf, Usz grid_h, Usz grid_w, Usz y, Usz x,
                          Usz height, Usz width, Glyph fill_char);

typedef enum {
  Mark_flag_none = 0,
  Mark_flag_input = 1 << 0,
  Mark_flag_output = 1 << 1,
  Mark_flag_haste_input = 1 << 2,
  Mark_flag_lock = 1 << 3,
  Mark_flag_sleep = 1 << 4,
  Mark_flag_dialect_end = 1 << 5,
} Mark_flags;

ORCA_OK_IF_UNUSED
static Mark_flags mbuffer_peek(Mark *mbuf, Usz height, Usz width, Usz y,
                               Usz x) {
  (void)height;
  return mbuf[y * width + x];
}

ORCA_OK_IF_UNUSED
static Mark_flags mbuffer_peek_relative(Mark *mbuf, Usz height, Usz width,
                                        Usz y, Usz x, Isz offs_y, Isz offs_x) {
  Isz y0 = (Isz)y + offs_y;
  Isz x0 = (Isz)x + offs_x;
  if (y0 >= (Isz)height || x0 >= (Isz)width || y0 < 0 || x0 < 0)
    return Mark_flag_none;
  return mbuf[(Usz)y0 * width + (Usz)x0];
}

ORCA_OK_IF_UNUSED
static void mbuffer_poke_flags_or(Mark *mbuf, Usz height, Usz width, Usz y,
                                  Usz x, Mark_flags flags) {
  (void)height;
  mbuf[y * width + x] |= (Mark)flags;
}

ORCA_OK_IF_UNUSED
static void mbuffer_poke_relative_flags_or(Mark *mbuf, Usz height, Usz width,
                                           Usz y, Usz x, Isz offs_y, Isz offs_x,
                                           Mark_flags flags) {
  Isz y0 = (Isz)y + offs_y;
  Isz x0 = (Isz)x + offs_x;
  if (y0 >= (Isz)height || x0 >= (Isz)width || y0 < 0 || x0 < 0)
    return;
  mbuf[(Usz)y0 * width + (Usz)x0] |= (Mark)flags;
}

void mbuffer_clear(Mark *mbuf, Usz height, Usz width);
