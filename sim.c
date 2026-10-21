#include "sim.h"
#include "gbuffer.h"
#include "music.h"
#include <math.h>
#include <string.h>
#include <sys/_types/_ct_rune_t.h>

// Note Sequence
static char const note_sequence[] = "CcDdEFfGgAaB";

Usz find_note_index(Glyph root_note_glyph) {
  for (Usz i = 0; i < sizeof(note_sequence) - 1; i++) {
    if (note_sequence[i] == root_note_glyph)
      return i;
  }
  return UINT_MAX; // Indicate not found
}

//////// Utilities

static Glyph const glyph_table[36] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', //  0-11
    'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', // 12-23
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', // 24-35
};
enum { Glyphs_index_count = sizeof glyph_table };
static inline Glyph glyph_of(Usz index) {
  assert(index < Glyphs_index_count);
  return glyph_table[index];
}

static ORCA_FORCEINLINE Usz random_key(Usz seed, Usz y, Usz x, Usz width,
                                       Usz tick);

// A glyph's value: gbuffer.h's glyph_index_of, the one table.
static ORCA_FORCEINLINE Usz index_of(Glyph c) { return glyph_index_of(c); }

static ORCA_FORCEINLINE bool seaflake_is_variable(Glyph glyph) {
  return (glyph >= '0' && glyph <= '9') || (glyph >= 'A' && glyph <= 'Z') ||
         (glyph >= 'a' && glyph <= 'z');
}

// Reference implementation (test_core.c's glyph_index_of_reference checks
// the table against it):
// static Usz index_of(Glyph c) {
//   if (c >= '0' && c <= '9') return (Usz)(c - '0');
//   if (c >= 'A' && c <= 'Z') return (Usz)(c - 'A' + 10);
//   if (c >= 'a' && c <= 'z') return (Usz)(c - 'a' + 10);
//   return 0;
// }

static ORCA_FORCEINLINE bool glyph_is_lowercase(Glyph g) { return g & 1 << 5; }
static ORCA_FORCEINLINE Glyph glyph_lowered_unsafe(Glyph g) {
  return (Glyph)(g | 1 << 5);
}
static inline Glyph glyph_with_case(Glyph g, Glyph caser) {
  enum { Case_bit = 1 << 5, Alpha_bit = 1 << 6 };
  return (Glyph)((g & ~Case_bit) | ((~g & Alpha_bit) >> 1) |
                 (caser & Case_bit));
}

static ORCA_PURE bool oper_has_neighboring_bang(Glyph const *gbuf, Usz h, Usz w,
                                                Usz y, Usz x) {
  Glyph const *gp = gbuf + w * y + x;
  if (x < w - 1 && gp[1] == '*')
    return true;
  if (x > 0 && *(gp - 1) == '*')
    return true;
  if (y < h - 1 && gp[w] == '*')
    return true;
  // note: negative array subscript on rhs of short-circuit, may cause ub if
  // the arithmetic under/overflows, even if guarded the guard on lhs is false
  if (y > 0 && *(gp - w) == '*')
    return true;
  return false;
}

// Returns UINT8_MAX if not a valid note.
static U8 midi_note_number_of(Glyph g) {
  int sharp = (g & 1 << 5) >> 5; // sharp=1 if lowercase
  g &= (Glyph) ~(1 << 5);        // make uppercase
  if (g < 'A' || g > 'Z')        // A through Z only
    return UINT8_MAX;
  // We want C=0, D=1, E=2, etc. A and B are equivalent to H and I.
  int deg = g <= 'B' ? 'G' - 'B' + g - 'A' : g - 'C';
  return (U8)(deg / 7 * 12 + (I8[]){0, 2, 4, 5, 7, 9, 11}[deg % 7] + sharp);
}

// The one velocity mapping of `:`, `%` and `=`. `.` is 127. The value comes
// from index_of, which folds case (`F` is `f`) and ignores the top bit, so a
// byte 0x80 or above reads like its low seven bits. A glyph worth 0, `0` or
// any other glyph that is not 0-9, a-z or A-Z, returns 0: send no note. Any
// other value v gives min(v * 8 - 1, 127), so `1` is 7, `f` 119 and `g` to
// `z` 127. The test is on the value, not the glyph `'0'`: 0 * 8 - 1 would
// wrap and clamp to 127.
static U8 midi_velocity_of(Glyph g) {
  if (g == '.')
    return 127;
  Usz v = index_of(g);
  if (v == 0)
    return 0;
  v = v * 8 - 1;
  return (U8)(v > 127 ? 127 : v);
}

typedef struct {
  Glyph *vars_slots;
  char (*vars_masks)[13];
  Oevent_list *oevent_list;
  Usz random_seed;
  Orca_run_ctx const *ctx;
  // ctx->labels, copied so PORT loads one pointer per port; NULL records
  // nothing
  Port_label_cell *labels;
  Orca_dialect active_dialect;
} Oper_extra_params;

// PORT's record (portlabel.h holds the ids): the label id and the owner's
// offset go into the port's cell when the caller gave a buffer and the cell
// is in the grid, the bounds the mark uses. An input that is not an output
// replaces any record; any other port replaces only a record that counted
// as an output, or an empty cell. The NULL test is the only cost on cli's
// path, which passes no buffer.
static ORCA_FORCEINLINE void port_label_record(Port_label_cell *labels,
                                               Usz height, Usz width, Usz y,
                                               Usz x, Isz delta_y, Isz delta_x,
                                               Mark_flags flags, Port_label id) {
  if (!labels)
    return;
  // Asserted for every port, in or out of the grid (portlabel.h).
  assert(delta_y >= -256 && delta_y <= 256 && delta_x >= -256 &&
         delta_x <= 256);
  assert(id != Port_label_none && id < Port_label_id_count);
  Isz y0 = (Isz)y + delta_y;
  Isz x0 = (Isz)x + delta_x;
  if (y0 >= (Isz)height || x0 >= (Isz)width || y0 < 0 || x0 < 0)
    return;
  Port_label_cell *cell = labels + (Usz)y0 * width + (Usz)x0;
  bool in = (flags & Mark_flag_input) && !(flags & Mark_flag_output);
  if (in || !cell->is_input) {
    cell->id = (U8)id;
    cell->is_input = in;
    cell->owner_dy = (I16)-delta_y;
    cell->owner_dx = (I16)-delta_x;
  }
}

static void oper_poke_and_stun(Glyph *restrict gbuffer, Mark *restrict mbuffer,
                               Usz height, Usz width, Usz y, Usz x, Isz delta_y,
                               Isz delta_x, Glyph g) {
  Isz y0 = (Isz)y + delta_y;
  Isz x0 = (Isz)x + delta_x;
  if (y0 < 0 || x0 < 0 || (Usz)y0 >= height || (Usz)x0 >= width)
    return;
  Usz offs = (Usz)y0 * width + (Usz)x0;
  gbuffer[offs] = g;
  mbuffer[offs] |= Mark_flag_sleep;
}

// For anyone editing this in the future: the "no inline" here is deliberate.
// You may think that inlining is always faster. Or even just letting the
// compiler decide. You would be wrong. Try it. If you really want this VM to
// run faster, you will need to use computed goto or assembly.
#define OPER_FUNCTION_ATTRIBS ORCA_NOINLINE static void

#define BEGIN_OPERATOR(_oper_name)                                             \
  OPER_FUNCTION_ATTRIBS oper_behavior_##_oper_name(                            \
      Glyph *const restrict gbuffer, Mark *const restrict mbuffer,             \
      Usz const height, Usz const width, Usz const y, Usz const x,             \
      Usz Tick_number, Oper_extra_params *const extra_params,                  \
      Mark const cell_flags, Glyph const This_oper_char) {                     \
    (void)gbuffer;                                                             \
    (void)mbuffer;                                                             \
    (void)height;                                                              \
    (void)width;                                                               \
    (void)y;                                                                   \
    (void)x;                                                                   \
    (void)Tick_number;                                                         \
    (void)extra_params;                                                        \
    (void)cell_flags;                                                          \
    (void)This_oper_char;

#define END_OPERATOR }

#define PEEK(_delta_y, _delta_x)                                               \
  gbuffer_peek_relative(gbuffer, height, width, y, x, _delta_y, _delta_x)
#define POKE(_delta_y, _delta_x, _glyph)                                       \
  gbuffer_poke_relative(gbuffer, height, width, y, x, _delta_y, _delta_x,      \
                        _glyph)
#define STUN(_delta_y, _delta_x)                                               \
  mbuffer_poke_relative_flags_or(mbuffer, height, width, y, x, _delta_y,       \
                                 _delta_x, Mark_flag_sleep)
#define POKE_STUNNED(_delta_y, _delta_x, _glyph)                               \
  oper_poke_and_stun(gbuffer, mbuffer, height, width, y, x, _delta_y,          \
                     _delta_x, _glyph)
#define LOCK(_delta_y, _delta_x)                                               \
  mbuffer_poke_relative_flags_or(mbuffer, height, width, y, x, _delta_y,       \
                                 _delta_x, Mark_flag_lock)

#define IN Mark_flag_input
#define OUT Mark_flag_output
#define NONLOCKING Mark_flag_lock
#define PARAM Mark_flag_haste_input

#define LOWERCASE_REQUIRES_BANG                                                \
  if (glyph_is_lowercase(This_oper_char) &&                                    \
      !oper_has_neighboring_bang(gbuffer, height, width, y, x))                \
  return

#define STOP_IF_NOT_BANGED                                                     \
  if (!oper_has_neighboring_bang(gbuffer, height, width, y, x))                \
  return

// A port: the mark, then its label record (portlabel.h). _label is a
// Port_label; an output passes Port_label_output, never none, and so does a
// port that is both an input and an output (I's and Z's read-write cell).
#define PORT(_delta_y, _delta_x, _flags, _label)                               \
  do {                                                                         \
    mbuffer_poke_relative_flags_or(mbuffer, height, width, y, x, _delta_y,     \
                                   _delta_x, (_flags) ^ Mark_flag_lock);       \
    port_label_record(extra_params->labels, height, width, y, x, _delta_y,     \
                      _delta_x, (_flags), (_label));                           \
  } while (0)
//////// Operators

// The operator lists, one per dialect (sim.h's Orca_dialect). Each dialect
// runs the shared lists plus its own. orca_run picks one dialect's lists
// once per call, outside the cell loop, so no operator body reads the
// dialect. An ALPHA list names the uppercase glyph, and ALPHA_CASE sends its
// lowercase to the same body.
#define SHARED_UNIQUE_OPERATORS(_)                                             \
  _('#', comment)                                                              \
  _('%', midi)                                                                 \
  _('*', bang)                                                                 \
  _(':', midi)                                                                 \
  _('?', midipb)

#define BORCA_UNIQUE_OPERATORS(_)                                              \
  _('!', midicc)                                                               \
  _('$', scale)                                                                \
  _(';', arpeggiator)                                                          \
  _('=', midichord)                                                            \
  _('&', bouncer)

// Orca-c 9df9786's own. It has no $ or &, so those glyphs do nothing here.
#define UPSTREAM_UNIQUE_OPERATORS(_)                                           \
  _('!', midicc_upstream)                                                      \
  _(';', udp)                                                                  \
  _('=', osc)

#define SEAFLAKE_UNIQUE_OPERATORS(_)                                           \
  _('!', midicc_upstream)                                                      \
  _('&', seaflake_phase)                                                       \
  _('(', seaflake_midi_just)                                                   \
  _(')', seaflake_midi_scale)                                                  \
  _('+', seaflake_binary_scale)                                                \
  _(';', udp)                                                                  \
  _('=', osc)                                                                  \
  _('^', seaflake_probability)                                                 \
  _('~', seaflake_burst)                                                       \
  _('@', seaflake_midi_sequence)

#define MORFIL_UNIQUE_OPERATORS(_)                                             \
  BORCA_UNIQUE_OPERATORS(_)                                                    \
  _('`', morfil_switch_dialect)

#define SHARED_ALPHA_OPERATORS(_)                                              \
  _('A', add)                                                                  \
  _('B', subtract)                                                             \
  _('E', movement)                                                             \
  _('F', if)                                                                   \
  _('G', generator)                                                            \
  _('H', halt)                                                                 \
  _('J', jump)                                                                 \
  _('K', konkat)                                                               \
  _('M', multiply)                                                             \
  _('N', movement)                                                             \
  _('O', offset)                                                               \
  _('P', push)                                                                 \
  _('Q', query)                                                                \
  _('S', movement)                                                             \
  _('T', track)                                                                \
  _('V', variable)                                                             \
  _('W', movement)                                                             \
  _('X', teleport)                                                             \
  _('Y', yump)

// These seven letters differ between the dialects. bOrca's runs Orca's
// 2025 rules for them, in which an empty input counts as 0 and R's range
// includes its max; upstream runs Orca-c's bodies, the *_upstream ones,
// with their defaults for an empty input and R's range. Both R draw from
// one hash, random_key. Lowercase r is bOrca's shuffle bag, and upstream's
// banged R.
#define BORCA_ALPHA_OPERATORS(_)                                               \
  _('C', clock)                                                                \
  _('D', delay)                                                                \
  _('I', increment)                                                            \
  _('L', lesser)                                                               \
  _('R', random)                                                               \
  _('U', uclid)                                                                \
  _('Z', lerp)

#define UPSTREAM_ALPHA_OPERATORS(_)                                            \
  _('C', clock_upstream)                                                       \
  _('D', delay_upstream)                                                       \
  _('I', increment_upstream)                                                   \
  _('L', lesser_upstream)                                                      \
  _('R', random_upstream)                                                      \
  _('U', uclid_upstream)                                                       \
  _('Z', lerp_upstream)

// zOrca's own: bOrca's lists, plus zOrca's operators, which land here one at
// a time (none yet, so zorca plays as borca). A glyph listed twice in one
// dialect is a duplicate case in its switch and does not compile, so an
// operator here cannot silently replace one of bOrca's. One that takes a
// glyph bOrca leaves inert changes what an existing patch plays under the
// default dialect: that is Breaking by the CHANGELOG's rule.
#define ZORCA_UNIQUE_OPERATORS(_) BORCA_UNIQUE_OPERATORS(_)
#define ZORCA_ALPHA_OPERATORS(_) BORCA_ALPHA_OPERATORS(_)

#define MORFIL_ALPHA_OPERATORS(_) BORCA_ALPHA_OPERATORS(_)

#define SEAFLAKE_ALPHA_OPERATORS(_)                                            \
  _('C', clock_upstream)                                                       \
  _('D', delay_upstream)                                                       \
  _('I', increment_upstream)                                                   \
  _('L', lesser_upstream)                                                      \
  _('R', random_upstream)                                                      \
  _('U', uclid_upstream)                                                       \
  _('Z', lerp_upstream)

BEGIN_OPERATOR(movement)
  if (glyph_is_lowercase(This_oper_char) &&
      !oper_has_neighboring_bang(gbuffer, height, width, y, x))
    return;
  Isz delta_y, delta_x;
  switch (glyph_lowered_unsafe(This_oper_char)) {
  case 'n':
    delta_y = -1;
    delta_x = 0;
    break;
  case 'e':
    delta_y = 0;
    delta_x = 1;
    break;
  case 's':
    delta_y = 1;
    delta_x = 0;
    break;
  case 'w':
    delta_y = 0;
    delta_x = -1;
    break;
  default:
    // could cause strict aliasing problem, maybe
    delta_y = 0;
    delta_x = 0;
    break;
  }
  Isz y0 = (Isz)y + delta_y;
  Isz x0 = (Isz)x + delta_x;
  if (y0 >= (Isz)height || x0 >= (Isz)width || y0 < 0 || x0 < 0) {
    gbuffer[y * width + x] = '*';
    return;
  }
  Glyph *restrict g_at_dest = gbuffer + (Usz)y0 * width + (Usz)x0;
  if (*g_at_dest == '.') {
    *g_at_dest = This_oper_char;
    gbuffer[y * width + x] = '.';
    mbuffer[(Usz)y0 * width + (Usz)x0] |= Mark_flag_sleep;
  } else {
    gbuffer[y * width + x] = '*';
  }
END_OPERATOR

BEGIN_OPERATOR(midicc)
  PORT(0, 1, IN | PARAM, Port_label_channel);
  PORT(0, 2, IN | PARAM, Port_label_control_hundreds);
  PORT(0, 3, IN | PARAM, Port_label_control_tens);
  PORT(0, 4, IN | PARAM, Port_label_control_ones);
  PORT(0, 5, IN | PARAM, Port_label_cc_value);
  PORT(0, 6, IN | PARAM, Port_label_glide_rate);
  STOP_IF_NOT_BANGED;
  Glyph channel_g = PEEK(0, 1);
  Glyph control_h = PEEK(0, 2);  // hundreds
  Glyph control_t = PEEK(0, 3);  // tens
  Glyph control_o = PEEK(0, 4);  // ones
  Glyph value_g = PEEK(0, 5);
  Glyph interp_rate_g = PEEK(0, 6);

  if (channel_g == '.' || value_g == '.')
    return;

  Usz channel = index_of(channel_g);
  if (channel > 15)
    return;

  // Calculate CC number from hundreds, tens, ones (support . for missing digits)
  Usz control_num = 0;
  if (control_h != '.') control_num += index_of(control_h) * 100;
  if (control_t != '.') control_num += index_of(control_t) * 10;
  if (control_o != '.') control_num += index_of(control_o);

  // Clamp to valid MIDI CC range (0-127)
  if (control_num > 127) control_num = 127;

  // Map ORCA values (0-35) to MIDI CC values in increments of 4: 0→0, 1→4, 2→8, etc.
  Usz value_index = index_of(value_g);
  Usz midi_value = value_index * 4;
  if (midi_value > 127) midi_value = 127; // Clamp to MIDI CC range

  PORT(0, 0, OUT, Port_label_output);

  // Handle interpolation rate
  if (interp_rate_g == '.') {
    // No interpolation - send immediate CC
    Oevent_midi_cc *oe =
        (Oevent_midi_cc *)oevent_list_alloc_item(extra_params->oevent_list);
    oe->oevent_type = Oevent_type_midi_cc;
    oe->channel = (U8)channel;
    oe->control = (U8)control_num;
    oe->value = (U8)midi_value;
  } else {
    // Send interpolated CC event
    Oevent_midi_cc_interpolated *oe =
        (Oevent_midi_cc_interpolated *)oevent_list_alloc_item(extra_params->oevent_list);
    oe->oevent_type = Oevent_type_midi_cc_interpolated;
    oe->channel = (U8)channel;
    oe->control = (U8)control_num;
    oe->target_value = (U8)midi_value;
    oe->interpolation_rate = (U8)index_of(interp_rate_g);
  }
END_OPERATOR

BEGIN_OPERATOR(comment)
  // restrict probably ok here...
  Glyph const *restrict gline = gbuffer + y * width;
  Mark *restrict mline = mbuffer + y * width;
  Usz max_x = x + 255;
  if (width < max_x)
    max_x = width;
  for (Usz x0 = x + 1; x0 < max_x; ++x0) {
    Glyph g = gline[x0];
    mline[x0] |= (Mark)Mark_flag_lock;
    if (g == '#')
      break;
  }
END_OPERATOR

static void seaflake_start_burst(Glyph *gbuffer, Usz height, Usz width,
                                 Usz y, Usz x, Orca_run_ctx const *ctx) {
  Glyph repeats_glyph =
      gbuffer_peek_relative(gbuffer, height, width, y, x, 0, 1);
  Usz repeats = index_of(repeats_glyph == '.' ? '4' : repeats_glyph);
  if (repeats == 0)
    return;
  Usz division =
      index_of(gbuffer_peek_relative(gbuffer, height, width, y, x, 0, 2));
  Usz multiplication =
      index_of(gbuffer_peek_relative(gbuffer, height, width, y, x, 0, 3));
  if (division == 0)
    division = 1;
  if (multiplication == 0)
    multiplication = 1;
  if (repeats > 35)
    repeats = 35;
  if (division > 35)
    division = 35;
  if (multiplication > 35)
    multiplication = 35;
  Opstate_entry *entry = opstate_lookup(ctx->opstate, y, x, '~');
  if (!entry)
    return;
  U64 frame = ctx->tick_len_us;
  if (frame == 0)
    frame = UINT64_C(125000);
  Opstate_seaflake_burst *state = &entry->u.seaflake_burst;
  state->active = true;
  state->remaining = (U8)repeats;
  state->interval_us = frame * division / multiplication;
  if (state->interval_us == 0)
    state->interval_us = 1;
  state->next_us = ctx->now_us;
}

BEGIN_OPERATOR(bang)
  if (extra_params->ctx->dialect == Orca_dialect_seaflake &&
      !extra_params->ctx->is_seaflake_subtick) {
    static Isz const dy[] = {1, 0, 0};
    static Isz const dx[] = {0, -1, 1};
    for (Usz i = 0; i < ORCA_ARRAY_COUNTOF(dx); ++i) {
      Isz source_y = (Isz)y + dy[i], source_x = (Isz)x + dx[i];
      if (source_y < 0 || source_x < 0 || (Usz)source_y >= height ||
          (Usz)source_x >= width ||
          gbuffer[(Usz)source_y * width + (Usz)source_x] != '~')
        continue;
      seaflake_start_burst(gbuffer, height, width, (Usz)source_y,
                           (Usz)source_x, extra_params->ctx);
    }
  }
  gbuffer_poke(gbuffer, height, width, y, x, '.');
END_OPERATOR

BEGIN_OPERATOR(midi)
  PORT(0, 1, IN | PARAM, Port_label_channel);
  PORT(0, 2, IN | PARAM, Port_label_octave);
  PORT(0, 3, IN | PARAM, Port_label_note);
  PORT(0, 4, IN | PARAM, Port_label_velocity);
  PORT(0, 5, IN | PARAM, Port_label_length);
  STOP_IF_NOT_BANGED;
  Glyph channel_g = PEEK(0, 1);
  Glyph octave_g = PEEK(0, 2);
  Glyph note_g = PEEK(0, 3);
  Glyph velocity_g = PEEK(0, 4);
  Glyph length_g = PEEK(0, 5);
  U8 octave_num = (U8)index_of(octave_g);
  if (octave_g == '.')
    return;
  if (octave_num > 9)
    octave_num = 9;
  U8 note_num = midi_note_number_of(note_g);
  if (note_num == UINT8_MAX)
    return;
  Usz channel_num = index_of(channel_g);
  if (channel_num > 15)
    channel_num = 15;
  U8 velocity = midi_velocity_of(velocity_g);
  if (velocity == 0)
    return;
  PORT(0, 0, OUT, Port_label_output);
  Oevent_midi_note *oe =
      (Oevent_midi_note *)oevent_list_alloc_item(extra_params->oevent_list);
  oe->oevent_type = (U8)Oevent_type_midi_note;
  oe->channel = (U8)channel_num;
  oe->octave = octave_num;
  oe->note = note_num;
  oe->velocity = velocity;
  oe->duration = (U8)(index_of(length_g) & 0x7Fu);
  oe->mono = This_oper_char == '%' ? 1 : 0;
  oe->tuned = 0;
  oe->subtick = 0;
  oe->pitch_bend = 0;
END_OPERATOR

BEGIN_OPERATOR(midipb)
  PORT(0, 1, IN | PARAM, Port_label_channel);
  PORT(0, 2, IN | PARAM, Port_label_msb);
  PORT(0, 3, IN | PARAM, Port_label_lsb);
  PORT(0, 0, OUT, Port_label_output); // Mark output immediately
  STOP_IF_NOT_BANGED;
  Glyph channel_g = PEEK(0, 1);
  Glyph msb_g = PEEK(0, 2);
  Glyph lsb_g = PEEK(0, 3);
  if (channel_g == '.')
    return;
  Usz channel = index_of(channel_g);
  if (channel > 15)
    return;
  Oevent_midi_pb *oe =
      (Oevent_midi_pb *)oevent_list_alloc_item(extra_params->oevent_list);
  oe->oevent_type = Oevent_type_midi_pb;
  oe->channel = (U8)channel;
  oe->msb = (U8)(index_of(msb_g) * 127 / 35); // 0~35 -> 0~127
  oe->lsb = (U8)(index_of(lsb_g) * 127 / 35);
END_OPERATOR

BEGIN_OPERATOR(morfil_switch_dialect)
    PORT(0, 1, IN | PARAM, Port_label_dialect);
    Glyph dialect_glyph = PEEK(0, 1);
    switch (dialect_glyph) {
        case 'z':
            extra_params->active_dialect = Orca_dialect_zorca;
            break;
        case 'b':
            extra_params->active_dialect = Orca_dialect_borca;
            break;
        case 'u':
            extra_params->active_dialect = Orca_dialect_upstream;
            break;
        case 's':
            extra_params->active_dialect = Orca_dialect_seaflake;
            break;
        case 'm':
            extra_params->active_dialect = Orca_dialect_morfil;
            break;
        case '.':
            extra_params->active_dialect = extra_params->ctx->dialect;
            break;
    }
END_OPERATOR

BEGIN_OPERATOR(seaflake_probability)
  PORT(0, 1, IN | PARAM, Port_label_chance);
  PORT(1, 0, OUT, Port_label_output);
  STOP_IF_NOT_BANGED;
  Glyph chance_glyph = PEEK(0, 1);
  Usz chance = index_of(chance_glyph == '.' ? 'i' : chance_glyph);
  if (chance > 35)
    chance = 35;
  Usz draw = random_key(extra_params->random_seed, y, x, width, Tick_number) %
             35;
  POKE(1, 0, draw < chance ? '*' : '.');
END_OPERATOR

BEGIN_OPERATOR(seaflake_phase)
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN | PARAM, Port_label_length);
  PORT(-1, 0, IN | PARAM, Port_label_phase);
  PORT(1, 0, OUT, Port_label_output);
  Usz rate = index_of(PEEK(0, -1));
  Glyph length_glyph = PEEK(0, 1);
  Usz length = index_of(length_glyph == '.' ? '4' : length_glyph);
  Usz phase = index_of(PEEK(-1, 0));
  if (rate == 0)
    rate = 1;
  if (length == 0)
    length = 1;
  if (phase > 35)
    phase = 35;
  bool active = Tick_number % rate == 0 &&
                (Tick_number / rate) % length == phase % length;
  POKE(1, 0, active ? '*' : '.');
END_OPERATOR

BEGIN_OPERATOR(seaflake_burst)
  PORT(0, 1, IN | PARAM, Port_label_count);
  PORT(0, 2, IN | PARAM, Port_label_division);
  PORT(0, 3, IN | PARAM, Port_label_multiplication);
  PORT(1, 0, OUT, Port_label_output);
  Glyph const *gp = gbuffer + y * width + x;
  if (!((x > 0 && gp[-1] == '*') || (x < width - 1 && gp[1] == '*') ||
        (y > 0 && gp[-(Isz)width] == '*')))
    return;
  seaflake_start_burst(gbuffer, height, width, y, x, extra_params->ctx);
END_OPERATOR

BEGIN_OPERATOR(seaflake_binary_scale)
  PORT(0, -1, IN | PARAM, Port_label_variable);
  static char const c_major[] = "101011010101";
  Glyph key = PEEK(0, -1);
  for (Usz pitch = 0; pitch < 12; ++pitch) {
    PORT(0, (Isz)pitch + 1, IN | PARAM, Port_label_pitch);
  }
  if (!seaflake_is_variable(key))
    return;
  Usz key_index = index_of(key);
  if (key_index >= Opstate_random_max_size)
    return;
  for (Usz pitch = 0; pitch < 12; ++pitch) {
    Glyph value = PEEK(0, (Isz)pitch + 1);
    Usz enabled =
        value == '.' ? (Usz)(c_major[pitch] - '0') : index_of(value);
    extra_params->vars_masks[key_index][pitch] =
        enabled == 0 ? '0' : '1';
  }
  extra_params->vars_masks[key_index][12] = '\0';
END_OPERATOR

static void seaflake_add_midi_note(Oper_extra_params *extras, U8 channel,
                                   U8 octave, U8 note, U8 velocity, U8 length,
                                   bool tuned, U16 pitch_bend) {
  Oevent_midi_note *oe =
      (Oevent_midi_note *)oevent_list_alloc_item(extras->oevent_list);
  *oe = (Oevent_midi_note){.oevent_type = Oevent_type_midi_note,
                           .channel = channel,
                           .octave = octave,
                           .note = note,
                           .velocity = velocity,
                           .duration = length,
                           .mono = 0,
                           .tuned = tuned,
                           .subtick = 0,
                           .pitch_bend = pitch_bend};
}

static void seaflake_add_subtick_note(Oevent_list *events, U8 channel, U8 note,
                                      U8 velocity, bool note_off) {
  Oevent_midi_note *oe =
      (Oevent_midi_note *)oevent_list_alloc_item(events);
  *oe = (Oevent_midi_note){.oevent_type = Oevent_type_midi_note,
                           .channel = channel,
                           .octave = note / 12,
                           .note = note % 12,
                           .velocity = velocity,
                           .duration = 0,
                           .mono = 0,
                           .tuned = 0,
                           .subtick = note_off ? 2 : 1,
                           .pitch_bend = 0};
}

static U8 seaflake_velocity(Glyph glyph) {
  Usz velocity = index_of(glyph == '.' ? 'f' : glyph);
  if (velocity > 16)
    velocity = 16;
  return (U8)(velocity * 127 / 16);
}

static U8 seaflake_length(Glyph glyph) {
  Usz length = glyph == '.' ? 1 : index_of(glyph);
  return (U8)(length > 32 ? 32 : length);
}

static U8 seaflake_octave(Glyph glyph) {
  Usz octave = glyph == '.' ? 4 : index_of(glyph);
  return (U8)(octave > 8 ? 8 : octave);
}

static U64 seaflake_sequence_offset(Opstate_seaflake_sequence const *state,
                                   Usz index) {
  double progress =
      state->repeats < 2 ? 0.0 : (double)index / state->repeats;
  double position = progress;
  switch (state->shape) {
  case 1:
    position = 1.0 - sqrt(1.0 - progress);
    break;
  case 2:
    position = sqrt(progress);
    break;
  case 3:
    position = progress < 0.5
                   ? sqrt(progress / 2.0)
                   : 1.0 - sqrt((1.0 - progress) / 2.0);
    break;
  case 4:
    position = progress * state->pulsewidth / 36.0;
    break;
  default:
    break;
  }
  double offset = position * state->duration_us;
  U64 at = offset > 0.0 ? (U64)offset : 0;
  if (at >= state->duration_us && state->duration_us > 0)
    at = state->duration_us - 1;
  return at;
}

BEGIN_OPERATOR(seaflake_midi_scale)
  static Port_label const labels[] = {
      Port_label_channel, Port_label_octave, Port_label_degree,
      Port_label_variable, Port_label_velocity, Port_label_length};
  for (Isz i = 1; i <= 6; ++i)
    PORT(0, i, IN | PARAM, labels[i - 1]);
  PORT(0, 0, OUT, Port_label_output);
  STOP_IF_NOT_BANGED;
  Usz channel = PEEK(0, 1) == '.' ? 0 : index_of(PEEK(0, 1));
  if (channel > 15)
    channel = 15;
  Usz octave = seaflake_octave(PEEK(0, 2));
  Usz degree = PEEK(0, 3) == '.' ? 1 : index_of(PEEK(0, 3));
  if (degree < 1)
    degree = 1;
  if (degree > 35)
    degree = 35;
  Glyph key_glyph = PEEK(0, 4);
  if (!seaflake_is_variable(key_glyph))
    return;
  Usz key = index_of(key_glyph);
  if (key >= Opstate_random_max_size)
    return;
  char const *mask = extra_params->vars_masks[key];
  Usz notes[12], note_count = 0;
  for (Usz pitch = 0; pitch < 12; ++pitch)
    if (mask[pitch] == '1')
      notes[note_count++] = pitch;
  if (note_count == 0)
    return;
  Usz idx = degree - 1;
  Usz absolute = 24 + (octave + idx / note_count) * 12 +
                 notes[idx % note_count];
  if (absolute > 127)
    absolute = 127;
  U8 velocity = seaflake_velocity(PEEK(0, 5));
  seaflake_add_midi_note(extra_params, (U8)channel, (U8)(absolute / 12),
                         (U8)(absolute % 12), velocity,
                         seaflake_length(PEEK(0, 6)), false, 0);
END_OPERATOR

BEGIN_OPERATOR(seaflake_midi_just)
  static Port_label const labels[] = {Port_label_channel, Port_label_octave,
                                      Port_label_note, Port_label_velocity,
                                      Port_label_length};
  for (Isz i = 1; i <= 5; ++i)
    PORT(0, i, IN | PARAM, labels[i - 1]);
  PORT(0, 0, OUT, Port_label_output);
  STOP_IF_NOT_BANGED;
  Usz channel = PEEK(0, 1) == '.' ? 0 : index_of(PEEK(0, 1));
  if (channel > 15)
    channel = 15;
  Glyph note_glyph = PEEK(0, 3);
  U8 note = midi_note_number_of(note_glyph);
  if (note == UINT8_MAX)
    return;
  Usz absolute = 24 + seaflake_octave(PEEK(0, 2)) * 12 + note;
  if (absolute > 127)
    absolute = 127;
  U8 velocity = seaflake_velocity(PEEK(0, 4));
  static double const ratios[12] = {1.0, 16.0 / 15.0, 9.0 / 8.0, 6.0 / 5.0,
                                    5.0 / 4.0, 4.0 / 3.0, 45.0 / 32.0,
                                    3.0 / 2.0, 8.0 / 5.0, 5.0 / 3.0,
                                    16.0 / 9.0, 15.0 / 8.0};
  Usz pitch_class = note % 12;
  double semitones = 12.0 * log2(ratios[pitch_class]) - pitch_class;
  double bend = 8192.0 + semitones * 4096.0;
  Usz pitch_bend = (Usz)(bend + 0.5);
  if (pitch_bend > 16383)
    pitch_bend = 16383;
  seaflake_add_midi_note(extra_params, (U8)channel, (U8)(absolute / 12),
                         (U8)(absolute % 12), velocity,
                         seaflake_length(PEEK(0, 5)), true,
                         (U16)pitch_bend);
END_OPERATOR

BEGIN_OPERATOR(seaflake_midi_sequence)
  static Port_label const labels[] = {
      Port_label_channel, Port_label_octave, Port_label_note,
      Port_label_velocity, Port_label_shape, Port_label_count,
      Port_label_length, Port_label_pulse_width};
  for (Isz i = 1; i <= 8; ++i)
    PORT(0, i, IN | PARAM, labels[i - 1]);
  bool banged = oper_has_neighboring_bang(gbuffer, height, width, y, x);
  Opstate_entry *entry = opstate_lookup(extra_params->ctx->opstate, y, x,
                                        This_oper_char);
  if (!entry)
    return;
  Opstate_seaflake_sequence *state = &entry->u.seaflake_sequence;
  if (!banged && !state->active)
    return;
  Usz channel = PEEK(0, 1) == '.' ? 0 : index_of(PEEK(0, 1));
  Usz octave = seaflake_octave(PEEK(0, 2));
  U8 note = midi_note_number_of(PEEK(0, 3));
  Usz velocity = seaflake_velocity(PEEK(0, 4));
  Usz shape = PEEK(0, 5) == '.' ? 1 : index_of(PEEK(0, 5));
  Usz repeats = PEEK(0, 6) == '.' ? 1 : index_of(PEEK(0, 6));
  Usz duration = PEEK(0, 7) == '.' ? 1 : index_of(PEEK(0, 7));
  Usz pulsewidth = PEEK(0, 8) == '.' ? 18 : index_of(PEEK(0, 8));
  if (channel > 15)
    channel = 15;
  if (shape < 1)
    shape = 1;
  if (shape > 4)
    shape = 4;
  if (repeats > 35)
    repeats = 35;
  if (duration < 1)
    duration = 1;
  if (duration > 36)
    duration = 36;
  if (pulsewidth < 1)
    pulsewidth = 1;
  if (pulsewidth > 35)
    pulsewidth = 35;
  if (note == UINT8_MAX) {
    if (state->note_active)
      seaflake_add_subtick_note(extra_params->oevent_list,
                                state->active_channel, state->active_note, 0,
                                true);
    state->note_active = false;
    state->active = false;
    return;
  }
  if (banged) {
    if (state->note_active) {
      seaflake_add_subtick_note(extra_params->oevent_list,
                                state->active_channel, state->active_note, 0,
                                true);
      state->note_active = false;
    }
    if (repeats == 0) {
      state->active = false;
      return;
    }
    state->active = true;
    state->emitted = 0;
    state->repeats = (U8)repeats;
    state->started_us = extra_params->ctx->now_us;
  } else {
    state->repeats = (U8)repeats;
  }
  state->channel = (U8)channel;
  state->octave = (U8)octave;
  state->note = note;
  state->velocity = (U8)velocity;
  state->shape = (U8)shape;
  state->pulsewidth = (U8)pulsewidth;
  U64 frame = extra_params->ctx->tick_len_us;
  if (frame == 0)
    frame = UINT64_C(125000);
  state->duration_us = frame * duration;
  if (state->emitted < state->repeats)
    state->next_us =
        state->started_us + seaflake_sequence_offset(state, state->emitted);
  else
    state->next_us = state->started_us + state->duration_us;
END_OPERATOR

BEGIN_OPERATOR(add)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_value_a);
  PORT(0, 1, IN, Port_label_value_b);
  PORT(1, 0, OUT, Port_label_output);
  Glyph a = PEEK(0, -1);
  Glyph b = PEEK(0, 1);
  Glyph g = glyph_table[(index_of(a) + index_of(b)) % Glyphs_index_count];
  POKE(1, 0, glyph_with_case(g, b));
END_OPERATOR

BEGIN_OPERATOR(subtract)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_value_a);
  PORT(0, 1, IN, Port_label_value_b);
  PORT(1, 0, OUT, Port_label_output);
  Glyph a = PEEK(0, -1);
  Glyph b = PEEK(0, 1);
  Isz val = (Isz)index_of(b) - (Isz)index_of(a);
  if (val < 0)
    val = -val;
  POKE(1, 0, glyph_with_case(glyph_of((Usz)val), b));
END_OPERATOR

// C in bOrca's dialect, by Orca's 2025 rules: an empty or 0 rate or modulo
// counts as 1, so with an empty modulo C writes 0 on every frame, over
// whatever its output cell held.
BEGIN_OPERATOR(clock)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN, Port_label_modulo);
  PORT(1, 0, OUT, Port_label_output);
  Glyph b = PEEK(0, 1);
  Usz rate = index_of(PEEK(0, -1));
  Usz mod_num = index_of(b);
  if (rate == 0)
    rate = 1;
  if (mod_num == 0)
    mod_num = 1;
  Glyph g = glyph_of(Tick_number / rate % mod_num);
  POKE(1, 0, glyph_with_case(g, b));
END_OPERATOR

// C in the upstream dialect: Orca-c 9df9786's body, in which an empty or 0
// modulo counts as 8.
BEGIN_OPERATOR(clock_upstream)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN, Port_label_modulo);
  PORT(1, 0, OUT, Port_label_output);
  Glyph b = PEEK(0, 1);
  Usz rate = index_of(PEEK(0, -1));
  Usz mod_num = index_of(b);
  if (rate == 0)
    rate = 1;
  if (mod_num == 0)
    mod_num = 8;
  Glyph g = glyph_of(Tick_number / rate % mod_num);
  POKE(1, 0, glyph_with_case(g, b));
END_OPERATOR

// D in bOrca's dialect, by Orca's 2025 rules: an empty or 0 rate or modulo
// counts as 1, and D bangs when the frame is a multiple of rate x modulo, so
// .D. bangs on every frame and 2D. on every 2nd.
BEGIN_OPERATOR(delay)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN, Port_label_modulo);
  PORT(1, 0, OUT, Port_label_output);
  Usz rate = index_of(PEEK(0, -1));
  Usz mod_num = index_of(PEEK(0, 1));
  if (rate == 0)
    rate = 1;
  if (mod_num == 0)
    mod_num = 1;
  Glyph g = Tick_number % (rate * mod_num) == 0 ? '*' : '.';
  POKE(1, 0, g);
END_OPERATOR

// D in the upstream dialect: Orca-c 9df9786's body, in which an empty or 0
// modulo counts as 8.
BEGIN_OPERATOR(delay_upstream)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN, Port_label_modulo);
  PORT(1, 0, OUT, Port_label_output);
  Usz rate = index_of(PEEK(0, -1));
  Usz mod_num = index_of(PEEK(0, 1));
  if (rate == 0)
    rate = 1;
  if (mod_num == 0)
    mod_num = 8;
  Glyph g = Tick_number % (rate * mod_num) == 0 ? '*' : '.';
  POKE(1, 0, g);
END_OPERATOR

BEGIN_OPERATOR(if)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_value_a);
  PORT(0, 1, IN, Port_label_value_b);
  PORT(1, 0, OUT, Port_label_output);
  Glyph g0 = PEEK(0, -1);
  Glyph g1 = PEEK(0, 1);
  POKE(1, 0, g0 == g1 ? '*' : '.');
END_OPERATOR

BEGIN_OPERATOR(generator)
  LOWERCASE_REQUIRES_BANG;
  Isz out_x = (Isz)index_of(PEEK(0, -3));
  Isz out_y = (Isz)index_of(PEEK(0, -2)) + 1;
  Isz len = (Isz)index_of(PEEK(0, -1));
  PORT(0, -3, IN | PARAM, Port_label_x_offset); // x
  PORT(0, -2, IN | PARAM, Port_label_y_offset); // y
  PORT(0, -1, IN | PARAM, Port_label_length); // len
  for (Isz i = 0; i < len; ++i) {
    PORT(0, i + 1, IN | PARAM, Port_label_input);
    PORT(out_y, out_x + i, OUT | NONLOCKING, Port_label_output);
    Glyph g = PEEK(0, i + 1);
    POKE_STUNNED(out_y, out_x + i, g);
  }
END_OPERATOR

BEGIN_OPERATOR(halt)
  LOWERCASE_REQUIRES_BANG;
  PORT(1, 0, IN | PARAM, Port_label_input);
END_OPERATOR

// I in bOrca's dialect, by Orca's 2025 rules: an empty step adds 0, and an
// empty or 0 maximum counts as 1, which makes the output 0.
BEGIN_OPERATOR(increment)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN, Port_label_max);
  PORT(1, 0, IN | OUT, Port_label_output);
  Glyph gb = PEEK(0, 1);
  Usz rate = index_of(PEEK(0, -1));
  Usz max = index_of(gb);
  Usz val = index_of(PEEK(1, 0));
  if (max == 0)
    max = 1;
  val = (val + rate) % max;
  POKE(1, 0, glyph_with_case(glyph_of(val), gb));
END_OPERATOR

// I in the upstream dialect: Orca-c 9df9786's body, in which an empty step
// is 1 and an empty or 0 maximum counts as 36.
BEGIN_OPERATOR(increment_upstream)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN, Port_label_max);
  PORT(1, 0, IN | OUT, Port_label_output);
  Glyph ga = PEEK(0, -1);
  Glyph gb = PEEK(0, 1);
  Usz rate = 1;
  if (ga != '.' && ga != '*')
    rate = index_of(ga);
  Usz max = index_of(gb);
  Usz val = index_of(PEEK(1, 0));
  if (max == 0)
    max = 36;
  val = val + rate;
  val = val % max;
  POKE(1, 0, glyph_with_case(glyph_of(val), gb));
END_OPERATOR

BEGIN_OPERATOR(jump)
  LOWERCASE_REQUIRES_BANG;
  Glyph g = PEEK(-1, 0);
  if (g == 'J')
    return;
  PORT(-1, 0, IN, Port_label_input);
  for (Isz i = 1; i <= 256; ++i) {
    if (PEEK(i, 0) != This_oper_char) {
      PORT(i, 0, OUT, Port_label_output);
      POKE(i, 0, g);
      break;
    }
    STUN(i, 0);
  }
END_OPERATOR

// Note: this is merged from a pull request without being fully tested or
// optimized
BEGIN_OPERATOR(konkat)
  LOWERCASE_REQUIRES_BANG;
  Isz len = (Isz)index_of(PEEK(0, -1));
  if (len == 0)
    len = 1;
  PORT(0, -1, IN | PARAM, Port_label_length);
  for (Isz i = 0; i < len; ++i) {
    PORT(0, i + 1, IN | PARAM, Port_label_variable);
    Glyph var = PEEK(0, i + 1);
    if (var != '.') {
      Usz var_idx = index_of(var);
      Glyph result = extra_params->vars_slots[var_idx];
      PORT(1, i + 1, OUT, Port_label_output);
      POKE(1, i + 1, result);
    }
  }
END_OPERATOR

// L in bOrca's dialect, by Orca's 2025 rules: the lesser of the two inputs,
// an empty one counting as 0, in the right input's case.
BEGIN_OPERATOR(lesser)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_value_a);
  PORT(0, 1, IN, Port_label_value_b);
  PORT(1, 0, OUT, Port_label_output);
  Glyph gb = PEEK(0, 1);
  Usz ia = index_of(PEEK(0, -1));
  Usz ib = index_of(gb);
  Usz out = ia < ib ? ia : ib;
  POKE(1, 0, glyph_with_case(glyph_of(out), gb));
END_OPERATOR

// L in the upstream dialect: Orca-c 9df9786's body, which writes . when
// either input is empty.
BEGIN_OPERATOR(lesser_upstream)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_value_a);
  PORT(0, 1, IN, Port_label_value_b);
  PORT(1, 0, OUT, Port_label_output);
  Glyph ga = PEEK(0, -1);
  Glyph gb = PEEK(0, 1);
  if (ga == '.' || gb == '.') {
    POKE(1, 0, '.');
  } else {
    Usz ia = index_of(ga);
    Usz ib = index_of(gb);
    Usz out = ia < ib ? ia : ib;
    POKE(1, 0, glyph_with_case(glyph_of(out), gb));
  }
END_OPERATOR

BEGIN_OPERATOR(multiply)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_factor_a);
  PORT(0, 1, IN, Port_label_factor_b);
  PORT(1, 0, OUT, Port_label_output);
  Glyph a = PEEK(0, -1);
  Glyph b = PEEK(0, 1);
  Glyph g = glyph_table[(index_of(a) * index_of(b)) % Glyphs_index_count];
  POKE(1, 0, glyph_with_case(g, b));
END_OPERATOR

BEGIN_OPERATOR(offset)
  LOWERCASE_REQUIRES_BANG;
  Isz in_x = (Isz)index_of(PEEK(0, -2)) + 1;
  Isz in_y = (Isz)index_of(PEEK(0, -1));
  PORT(0, -1, IN | PARAM, Port_label_y_offset);
  PORT(0, -2, IN | PARAM, Port_label_x_offset);
  PORT(in_y, in_x, IN, Port_label_input);
  PORT(1, 0, OUT, Port_label_output);
  POKE(1, 0, PEEK(in_y, in_x));
END_OPERATOR

BEGIN_OPERATOR(push)
  LOWERCASE_REQUIRES_BANG;
  Usz key = index_of(PEEK(0, -2));
  Usz len = index_of(PEEK(0, -1));
  PORT(0, -1, IN | PARAM, Port_label_length);
  PORT(0, -2, IN | PARAM, Port_label_key);
  PORT(0, 1, IN, Port_label_input);
  if (len == 0)
    return;
  Isz out_x = (Isz)(key % len);
  for (Usz i = 0; i < len; ++i) {
    LOCK(1, (Isz)i);
  }
  PORT(1, out_x, OUT, Port_label_output);
  POKE(1, out_x, PEEK(0, 1));
END_OPERATOR

BEGIN_OPERATOR(query)
  LOWERCASE_REQUIRES_BANG;
  Isz in_x = (Isz)index_of(PEEK(0, -3)) + 1;
  Isz in_y = (Isz)index_of(PEEK(0, -2));
  Isz len = (Isz)index_of(PEEK(0, -1));
  Isz out_x = 1 - len;
  PORT(0, -3, IN | PARAM, Port_label_x_offset); // x
  PORT(0, -2, IN | PARAM, Port_label_y_offset); // y
  PORT(0, -1, IN | PARAM, Port_label_length); // len
  // todo direct buffer manip
  for (Isz i = 0; i < len; ++i) {
    PORT(in_y, in_x + i, IN, Port_label_input);
    PORT(1, out_x + i, OUT, Port_label_output);
    Glyph g = PEEK(in_y, in_x + i);
    POKE(1, out_x + i, g);
  }
END_OPERATOR

BEGIN_OPERATOR(track)
  LOWERCASE_REQUIRES_BANG;
  Usz key = index_of(PEEK(0, -2));
  Usz len = index_of(PEEK(0, -1));
  PORT(0, -2, IN | PARAM, Port_label_key);
  PORT(0, -1, IN | PARAM, Port_label_length);
  if (len == 0)
    return;
  Isz read_val_x = (Isz)(key % len) + 1;
  for (Usz i = 0; i < len; ++i) {
    LOCK(0, (Isz)(i + 1));
  }
  PORT(0, (Isz)read_val_x, IN, Port_label_input);
  PORT(1, 0, OUT, Port_label_output);
  POKE(1, 0, PEEK(0, read_val_x));
END_OPERATOR

// https://www.computermusicdesign.com/
// simplest-euclidean-rhythm-algorithm-explained/
//
// U in bOrca's dialect, by Orca's 2025 rules: an empty step is 0, so U never
// bangs, and an empty or 0 maximum counts as 1, so any other step bangs on
// every frame.
BEGIN_OPERATOR(uclid)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_steps);
  PORT(0, 1, IN, Port_label_max);
  PORT(1, 0, OUT, Port_label_output);
  Usz steps = index_of(PEEK(0, -1));
  Usz max = index_of(PEEK(0, 1));
  if (max == 0)
    max = 1;
  Usz bucket = (steps * (Tick_number + max - 1)) % max + steps;
  Glyph g = (bucket >= max) ? '*' : '.';
  POKE(1, 0, g);
END_OPERATOR

// U in the upstream dialect: Orca-c 9df9786's body, in which an empty step
// is 1 and an empty or 0 maximum counts as 8.
BEGIN_OPERATOR(uclid_upstream)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_steps);
  PORT(0, 1, IN, Port_label_max);
  PORT(1, 0, OUT, Port_label_output);
  Glyph left = PEEK(0, -1);
  Usz steps = 1;
  if (left != '.' && left != '*')
    steps = index_of(left);
  Usz max = index_of(PEEK(0, 1));
  if (max == 0)
    max = 8;
  Usz bucket = (steps * (Tick_number + max - 1)) % max + steps;
  Glyph g = (bucket >= max) ? '*' : '.';
  POKE(1, 0, g);
END_OPERATOR

BEGIN_OPERATOR(variable)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_variable);
  PORT(0, 1, IN, Port_label_value);
  Glyph left = PEEK(0, -1);
  Glyph right = PEEK(0, 1);
  if (left != '.') {
    // Write
    Usz var_idx = index_of(left);
    extra_params->vars_slots[var_idx] = right;
  } else if (right != '.') {
    // Read
    PORT(1, 0, OUT, Port_label_output);
    Usz var_idx = index_of(right);
    Glyph result = extra_params->vars_slots[var_idx];
    POKE(1, 0, result);
  }
END_OPERATOR

BEGIN_OPERATOR(teleport)
  LOWERCASE_REQUIRES_BANG;
  Isz out_x = (Isz)index_of(PEEK(0, -2));
  Isz out_y = (Isz)index_of(PEEK(0, -1)) + 1;
  PORT(0, -2, IN | PARAM, Port_label_x_offset); // x
  PORT(0, -1, IN | PARAM, Port_label_y_offset); // y
  PORT(0, 1, IN, Port_label_input);
  PORT(out_y, out_x, OUT | NONLOCKING, Port_label_output);
  POKE_STUNNED(out_y, out_x, PEEK(0, 1));
END_OPERATOR

BEGIN_OPERATOR(yump)
  LOWERCASE_REQUIRES_BANG;
  Glyph g = PEEK(0, -1);
  if (g == 'Y')
    return;
  PORT(0, -1, IN, Port_label_input);
  for (Isz i = 1; i <= 256; ++i) {
    if (PEEK(0, i) != This_oper_char) {
      PORT(0, i, OUT, Port_label_output);
      POKE(0, i, g);
      break;
    }
    STUN(0, i);
  }
END_OPERATOR

// Z in bOrca's dialect, by Orca's 2025 rules: an empty rate is 0, so the
// output does not move toward its target.
BEGIN_OPERATOR(lerp)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN, Port_label_target);
  PORT(1, 0, IN | OUT, Port_label_output);
  Glyph b = PEEK(0, 1);
  Isz rate = (Isz)index_of(PEEK(0, -1));
  Isz goal = (Isz)index_of(b);
  Isz val = (Isz)index_of(PEEK(1, 0));
  Isz mod = val <= goal - rate ? rate : val >= goal + rate ? -rate : goal - val;
  POKE(1, 0, glyph_with_case(glyph_of((Usz)(val + mod)), b));
END_OPERATOR

// Z in the upstream dialect: Orca-c 9df9786's body, in which an empty rate
// is 1.
BEGIN_OPERATOR(lerp_upstream)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_rate);
  PORT(0, 1, IN, Port_label_target);
  PORT(1, 0, IN | OUT, Port_label_output);
  Glyph g = PEEK(0, -1);
  Glyph b = PEEK(0, 1);
  Isz rate = g == '.' || g == '*' ? 1 : (Isz)index_of(g);
  Isz goal = (Isz)index_of(b);
  Isz val = (Isz)index_of(PEEK(1, 0));
  Isz mod = val <= goal - rate ? rate : val >= goal + rate ? -rate : goal - val;
  POKE(1, 0, glyph_with_case(glyph_of((Usz)(val + mod)), b));
END_OPERATOR

// BOORCH's Scale operator. Its scales and chords come from music.h.
BEGIN_OPERATOR(scale)
  PORT(0, 1, IN | PARAM, Port_label_octave);   // Octave input
  PORT(0, 2, IN | PARAM, Port_label_root);   // Root note (like C, c, D etc)
  PORT(0, 3, IN | PARAM, Port_label_scale_selector);   // Selector: 0-9 scales, a-z chords, A-Z first inversions
  PORT(0, 4, IN | PARAM, Port_label_degree);   // Degree
  PORT(1, -1, OUT, Port_label_output); // Octave output
  PORT(1, 0, OUT, Port_label_output);  // Note output

  // Lock inputs
  LOCK(0, 1);
  LOCK(0, 2);
  LOCK(0, 3);
  LOCK(0, 4);

  Glyph octave_g = PEEK(0, 1);
  Glyph root_note_glyph = PEEK(0, 2);
  Glyph scale_glyph = PEEK(0, 3);
  Glyph degree_glyph = PEEK(0, 4);

  // Handle empty inputs
  if (root_note_glyph == '.' || scale_glyph == '.' || degree_glyph == '.') {
    POKE(1, 0, '.');
    return;
  }

  // Get root note number (0-11)
  U8 root_note_num = midi_note_number_of(root_note_glyph);
  if (root_note_num == UINT8_MAX)
    return;

  // Get base octave if provided
  Usz base_octave = 0;
  if (octave_g != '.') {
    base_octave = index_of(octave_g);
    if (base_octave > 9)
      base_octave = 9;
  }

  // The selector's intervals, decoded from its raw glyph (music.h): 0-9
  // scales, a-z chords in root position, A-Z their first inversions, and
  // scale 0 for any other glyph.
  Music_intervals intervals;
  music_decode(Music_op_scale, scale_glyph, &intervals);

  // Degrees past the last interval carry into the next octave.
  Usz degree_index = index_of(degree_glyph);
  Usz scale_length = intervals.count;
  Usz octave_increment = degree_index / scale_length;
  degree_index = degree_index % scale_length;
  Usz scale_offset = intervals.semitones[degree_index];

  // Calculate total semitones including octave increment
  Usz total_semitones = root_note_num + scale_offset + (octave_increment * 12);

  // Calculate final note and octave
  Usz final_note = total_semitones % 12;
  Usz octave_offset = total_semitones / 12;
  Usz final_octave = base_octave + octave_offset;

  if (final_octave > 9)
    return;

  // Output note
  Glyph output_note_glyph = note_sequence[final_note];
  POKE(1, 0, output_note_glyph);

  // Output octave if input octave was provided
  if (octave_g != '.') {
    POKE(1, -1, glyph_of(final_octave));
  }
END_OPERATOR

// BOORCH's MIDIChord operator. Its chords come from music.h.
BEGIN_OPERATOR(midichord)
  // Check all required input ports
  PORT(0, 1, IN | PARAM, Port_label_channel);
  PORT(0, 2, IN | PARAM, Port_label_octave);
  PORT(0, 3, IN | PARAM, Port_label_root_note);
  PORT(0, 4, IN | PARAM, Port_label_chord_type);
  PORT(0, 5, IN | PARAM, Port_label_velocity);
  PORT(0, 6, IN | PARAM, Port_label_length);
  PORT(0, 0, OUT, Port_label_output); // Mark output immediately
  STOP_IF_NOT_BANGED;

  // Get base note information with local scope
  Usz channel = index_of(PEEK(0, 1));
  if (channel > 15)
    channel = 15;

  // Get velocity and duration with standardized handling
  Glyph velocity_g = PEEK(0, 5);
  Glyph length_g = PEEK(0, 6);

  // The velocity `:` and `%` use; a glyph worth 0 sends no chord.
  U8 velocity = midi_velocity_of(velocity_g);
  if (velocity == 0)
    return;

  // Get initial octave
  int current_octave = (int)index_of(PEEK(0, 2));
  if (current_octave > 9)
    current_octave = 9;

  // Get root note and validate
  U8 root_note = midi_note_number_of(PEEK(0, 3));
  if (root_note == UINT8_MAX)
    return;

  // The chord's intervals, decoded from the selector's raw glyph (music.h):
  // 0-9 enriched chords, a-z chords in root position, A-Z their first
  // inversions, and enriched chord 0 for any other glyph.
  Music_intervals chord;
  music_decode(Music_op_midichord, PEEK(0, 4), &chord);

  // Track highest note played so far
  int last_note_absolute = (current_octave * 12) + root_note - 1;

  // Create and output midi events for each note in the chord
  for (Usz i = 0; i < chord.count; i++) {
    // Calculate this note's absolute MIDI note number
    int note_absolute =
        (current_octave * 12) + root_note + (int)chord.semitones[i];

    // If this note would be lower than previous note, move it up an octave
    while (note_absolute <= last_note_absolute) {
      current_octave++;
      note_absolute += 12;
    }

    // Skip if we exceeded MIDI range
    if (current_octave > 9 || note_absolute > 127)
      continue;

    // Keep track of highest note
    last_note_absolute = note_absolute;

    // Calculate final octave and note numbers
    U8 final_octave = (U8)(note_absolute / 12);
    U8 final_note = (U8)(note_absolute % 12);

    // Create MIDI event
    Oevent_midi_note *oe =
        (Oevent_midi_note *)oevent_list_alloc_item(extra_params->oevent_list);
    oe->oevent_type = Oevent_type_midi_note;
    oe->channel = (U8)channel;
    oe->octave = final_octave;
    oe->note = final_note;
    oe->velocity = velocity;
    oe->duration = (U8)(index_of(length_g) & 0x7Fu);
    oe->mono = 0;
    oe->tuned = 0;
    oe->subtick = 0;
    oe->pitch_bend = 0;
  }

END_OPERATOR

// BOORCH's Simple Arpeggiator - outputs degree numbers for Scale operator
typedef enum {
  ARP_UP = 0,        // 0: Ascending
  ARP_DOWN,          // 1: Descending
  ARP_UP_DOWN,       // 2: Up and down (without repeating highest)
  ARP_DOWN_UP,       // 3: Down and up (without repeating lowest)
  ARP_UP_DOWN_PLUS,  // 4: Up and down with repeated end points
  ARP_DOWN_UP_PLUS,  // 5: Down and up with repeated end points
  ARP_CONVERGE,      // 6: Outside in (highest, lowest, 2nd highest, 2nd lowest...)
  ARP_DIVERGE,       // 7: Inside out (middle outward)
  ARP_PINKY_UP,      // 8: Alternate between notes and highest note
  ARP_THUMB_UP,      // 9: Alternate between lowest and other notes
  ARP_UP_DOWN_ALT,   // a: Up-down alternating pattern
  ARP_DOWN_UP_ALT,   // b: Down-up alternating pattern
  ARP_RANDOM,        // c: Random selection
  ARP_BOUNCE,        // d: Bouncing pattern
  ARP_PATTERN_COUNT
} ArpPatternType;

// The degree of a pattern at a step, below total_degrees, which is 1 to 36.
// Each pattern takes the step modulo its own period, so any step is valid,
// whether it comes from the cell's counter or from the Step input; Random has
// no period and hashes the seed and the step. At a total of 1 every pattern
// gives 0: without that guard Up-Down and Down-Up would alternate 0 and 1.
static Usz get_arp_degree(ArpPatternType pattern, Usz step, Usz total_degrees,
                          Usz random_seed) {
  if (total_degrees <= 1)
    return 0;

  switch (pattern) {
  case ARP_UP:
    return step % total_degrees;

  case ARP_DOWN:
    return (total_degrees - 1) - (step % total_degrees);

  case ARP_UP_DOWN: {
    Usz period = (total_degrees * 2) - 2;
    Usz pos = step % period;
    return pos < total_degrees ? pos : period - pos;
  }

  case ARP_DOWN_UP: {
    Usz period = (total_degrees * 2) - 2;
    Usz pos = step % period;
    return pos < total_degrees ? (total_degrees - 1) - pos : pos - total_degrees + 1;
  }

  case ARP_UP_DOWN_PLUS: {
    Usz period = total_degrees * 2;
    Usz pos = step % period;
    return pos < total_degrees ? pos : period - pos - 1;
  }

  case ARP_DOWN_UP_PLUS: {
    Usz period = total_degrees * 2;
    Usz pos = step % period;
    return pos < total_degrees ? total_degrees - pos - 1 : pos - total_degrees;
  }

  case ARP_CONVERGE: {
    Usz pos = step % total_degrees;
    Usz left = 0, right = total_degrees - 1;
    for (Usz i = 0; i < pos; i++) {
      if (i % 2 == 0) right--; else left++;
    }
    return (pos % 2 == 0) ? right : left;
  }

  case ARP_DIVERGE: {
    Usz pos = step % total_degrees;
    Usz mid = total_degrees / 2;
    Usz offset = pos / 2;
    if (pos % 2 == 0) {
      return (mid + offset < total_degrees) ? mid + offset : total_degrees - 1;
    } else {
      return (mid > offset) ? mid - offset - 1 : 0;
    }
  }

  case ARP_PINKY_UP: {
    Usz highest = total_degrees - 1;
    Usz pos = step % (total_degrees * 2 - 2);
    return (pos % 2 == 0) ? pos / 2 : highest;
  }

  case ARP_THUMB_UP: {
    Usz pos = step % ((total_degrees - 1) * 2);
    return (pos % 2 == 0) ? 0 : (pos / 2) + 1;
  }

  case ARP_UP_DOWN_ALT: {
    Usz half_period = total_degrees;
    Usz pos = step % (half_period * 2);
    if (pos < half_period) {
      return pos;
    } else {
      return total_degrees - 1 - (pos - half_period);
    }
  }

  case ARP_DOWN_UP_ALT: {
    Usz half_period = total_degrees;
    Usz pos = step % (half_period * 2);
    if (pos < half_period) {
      return total_degrees - 1 - pos;
    } else {
      return pos - half_period;
    }
  }

  case ARP_RANDOM: {
    Usz key = (random_seed + step) ^ (step << 16);
    key = (key ^ 61) ^ (key >> 16);
    key = key + (key << 3);
    key = key ^ (key >> 4);
    key = key * 0x27d4eb2d;
    key = key ^ (key >> 15);
    return key % total_degrees;
  }

  case ARP_BOUNCE: {
    Usz bounce_size = total_degrees / 3;
    if (bounce_size < 1) bounce_size = 1;
    Usz pos = step % (bounce_size * 4);
    if (pos < bounce_size) return pos;
    else if (pos < bounce_size * 2) return bounce_size - 1 - (pos - bounce_size);
    else if (pos < bounce_size * 3) return pos - bounce_size * 2;
    else return bounce_size - 1 - (pos - bounce_size * 3);
  }

  default:
    return step % total_degrees;
  }
}

// The arpeggiator. Length is the number of notes per octave of the $ it feeds,
// `.` for 7; total degrees = min(Length x Range, 36), so the output is always
// one glyph. Step `.` steps the cell's own counter in the op-state store; any
// other glyph is the step itself, and then the store is not touched, so
// Ctrl+R, a file open or the paused preview cannot change the output. The
// counter resumes where it stopped when Step goes back to `.`, and restarts
// when Pattern, Range or Length changes.
BEGIN_OPERATOR(arpeggiator)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, 1, IN | PARAM, Port_label_range);   // Range (1-4)
  PORT(0, 2, IN | PARAM, Port_label_pattern); // Pattern (0-9, a-d)
  PORT(0, 3, IN | PARAM, Port_label_length);  // Notes per octave (`.` is 7)
  PORT(0, 4, IN | PARAM, Port_label_step);    // `.` for the counter, else the step
  PORT(1, 0, OUT, Port_label_output);               // Degree output

  // Get inputs
  Glyph range_g = PEEK(0, 1);
  Glyph pattern_g = PEEK(0, 2);
  Glyph length_g = PEEK(0, 3);
  Glyph step_g = PEEK(0, 4);

  if (range_g == '.' || pattern_g == '.')
    return;

  Usz range = index_of(range_g);
  Usz pattern = index_of(pattern_g) % ARP_PATTERN_COUNT;
  if (range == 0) range = 1;
  if (range > 4) range = 4;
  // A glyph worth 0 counts as 1, as Range 0 does.
  Usz length = length_g == '.' ? 7 : index_of(length_g);
  if (length == 0) length = 1;
  Usz total = length * range;
  if (total > Glyphs_index_count) total = Glyphs_index_count;

  // A set Step: no store lookup, so the counter is not read, advanced or
  // created.
  if (step_g != '.') {
    POKE(1, 0,
         glyph_of(get_arp_degree((ArpPatternType)pattern, index_of(step_g),
                                 total, extra_params->random_seed)));
    return;
  }

  // This cell's state, from the caller's op-state store
  Opstate_entry *entry = opstate_lookup(extra_params->ctx->opstate, y, x,
                                        This_oper_char);
  if (!entry)
    return;
  Opstate_arp *state = &entry->u.arp;

  // Reset counter on the first visit or if pattern, range or length changed
  if (!state->initialized || state->last_pattern != pattern ||
      state->last_range != range || state->last_length != length) {
    state->step_counter = 0;
    state->initialized = true;
    state->last_pattern = pattern;
    state->last_range = range;
    state->last_length = length;
  }

  // Get degree for current step
  Usz degree = get_arp_degree((ArpPatternType)pattern, state->step_counter,
                              total, extra_params->random_seed);

  // Output degree
  POKE(1, 0, glyph_of(degree));

  // Increment step counter for next bang
  state->step_counter++;
END_OPERATOR

// BOORCH's new Random Unique. Each lowercase r keeps its own bag in its
// op-state entry (Opstate_random) and shuffles it with its own PRNG, seeded
// from random_seed and the cell on its first visit (prng.h). It never sends
// one value twice in a row unless min equals max.

// Fisher-Yates shuffle of the bag.
static void shuffle_sequence(U8 *array, Usz n, Prng *prng) {
  if (n <= 1)
    return;

  for (Usz i = n - 1; i > 0; i--) {
    Usz j = prng_bounded(prng, (U32)(i + 1));
    // Swap
    U8 temp = array[i];
    array[i] = array[j];
    array[j] = temp;
  }
}

// Shuffles the bag and starts it over. A bag that would start with the last
// value sent swaps its first slot with a random other slot, so no value
// repeats across a reshuffle or a range change; it stays a permutation. A
// bag of one value repeats it, and a zeroed entry, with no last value, never
// swaps.
static void shuffle_bag(Opstate_random *state) {
  Usz n = state->sequence_size;
  shuffle_sequence(state->sequence, n, &state->prng);
  if (n > 1 && state->has_last && state->sequence[0] == state->last_value) {
    Usz j = 1 + prng_bounded(&state->prng, (U32)(n - 1));
    U8 temp = state->sequence[0];
    state->sequence[0] = state->sequence[j];
    state->sequence[j] = temp;
  }
  state->current_index = 0;
}

static void initialize_sequence(Opstate_random *state, Usz min, Usz max) {
  Usz size = (max >= min) ? (max - min + 1) : 0;
  if (size > Opstate_random_max_size)
    size = Opstate_random_max_size;
  state->sequence_size = (U8)size;

  // Fill sequence with values from min to max
  for (Usz i = 0; i < size; i++) {
    state->sequence[i] = (U8)(min + i);
  }

  shuffle_bag(state);
}

// Uppercase R's hash, in both dialects, and upstream's banged r's: Orca-c
// 9df9786's, unchanged, of the seed, the cell and the frame, so every R run
// is reproducible from its seed. The dialects differ only in the range they
// map it onto, random_hashed's or random_hashed_inclusive's.
static ORCA_FORCEINLINE Usz random_key(Usz seed, Usz y, Usz x, Usz width,
                                       Usz tick) {
  // Initial input params for the hash
  Usz key = (seed + y * width + x) ^ (tick << UINT32_C(16));
  // 32-bit shift_mult hash to evenly distribute bits
  key = (key ^ UINT32_C(61)) ^ (key >> UINT32_C(16));
  key = key + (key << UINT32_C(3));
  key = key ^ (key >> UINT32_C(4));
  key = key * UINT32_C(0x27d4eb2d);
  key = key ^ (key >> UINT32_C(15));
  return key;
}

// Orca-c's range, the upstream R's and r's: min up to, but not including,
// max. The caller has min < max.
static ORCA_FORCEINLINE Usz random_hashed(Usz seed, Usz y, Usz x, Usz width,
                                          Usz tick, Usz min, Usz max) {
  return random_key(seed, y, x, width, tick) % (max - min) + min;
}

// Orca's 2025 range, bOrca's R's: lo to hi, both included. The caller has
// lo < hi.
static ORCA_FORCEINLINE Usz random_hashed_inclusive(Usz seed, Usz y, Usz x,
                                                    Usz width, Usz tick,
                                                    Usz lo, Usz hi) {
  return random_key(seed, y, x, width, tick) % (hi - lo + 1) + lo;
}

// Modified random operator for lowercase 'r' - requires bang, uses shuffle to avoid consecutive duplicates
BEGIN_OPERATOR(random)
  // Check if this is lowercase 'r' (shuffle/unique random) or uppercase 'R' (pure random)
  if (glyph_is_lowercase(This_oper_char)) {
    // Lowercase 'r' - requires bang and uses shuffle algorithm
    LOWERCASE_REQUIRES_BANG;
    PORT(0, -1, IN | PARAM, Port_label_min); // Min
    PORT(0, 1, IN, Port_label_max);          // Max
    PORT(1, 0, OUT, Port_label_output);         // Output

    Glyph min_glyph = PEEK(0, -1);
    Glyph max_glyph = PEEK(0, 1);

    if (min_glyph == '.' || max_glyph == '.') {
      return;
    }

    // This cell's state, from the caller's op-state store
    Opstate_entry *entry = opstate_lookup(extra_params->ctx->opstate, y, x,
                                          This_oper_char);
    if (!entry)
      return;
    Opstate_random *state = &entry->u.random;

    // First visit: seed this cell's generator (prng.h)
    if (!state->initialized)
      prng_seed(&state->prng, extra_params->random_seed, y, x);

    Usz min = index_of(min_glyph);
    Usz max = index_of(max_glyph);

    if (max < min) {
      Usz temp = min;
      min = max;
      max = temp;
    }

    // Initialize or reinitialize if needed
    if (!state->initialized || state->current_index >= state->sequence_size ||
        min != (Usz)state->last_min || max != (Usz)state->last_max) {
      initialize_sequence(state, min, max);
      state->initialized = true;
      state->last_min = (U8)min;
      state->last_max = (U8)max;
    }

    // Get next value from sequence, and remember it as the last value sent
    Usz result = state->sequence[state->current_index];
    state->current_index++;
    state->last_value = (U8)result;
    state->has_last = true;

    // Reshuffle if we've used all values
    if (state->current_index >= state->sequence_size)
      shuffle_bag(state);

    // The output takes the right input's case, even when min equals max
    POKE(1, 0, glyph_with_case(glyph_of(result), max_glyph));
  } else {
    // Uppercase R, on every frame, by Orca's 2025 rules: a value from min to
    // max, both included, with the inputs in either order and an empty one
    // counting as 0, so .R. is always 0. Equal inputs give that value. The
    // output takes the right input's case on every path: aRA gives A.
    PORT(0, -1, IN | PARAM, Port_label_min);
    PORT(0, 1, IN, Port_label_max);
    PORT(1, 0, OUT, Port_label_output);
    Glyph gb = PEEK(0, 1);
    Usz a = index_of(PEEK(0, -1));
    Usz b = index_of(gb);
    if (a == b) {
      POKE(1, 0, glyph_with_case(glyph_of(a), gb));
      return;
    }
    Usz lo = a < b ? a : b;
    Usz hi = a < b ? b : a;
    Usz val = random_hashed_inclusive(extra_params->random_seed, y, x, width,
                                      Tick_number, lo, hi);
    POKE(1, 0, glyph_with_case(glyph_of(val), gb));
  }
END_OPERATOR

// Orca-c 9df9786's R and r, the upstream dialect's: r is a banged R. The
// output is min up to, but not including, max, with the inputs in either
// order; a right input of 0 counts as 36, and the output takes the right
// input's case unless the two are equal, when it is lowercase.
BEGIN_OPERATOR(random_upstream)
  LOWERCASE_REQUIRES_BANG;
  PORT(0, -1, IN | PARAM, Port_label_min);
  PORT(0, 1, IN, Port_label_max);
  PORT(1, 0, OUT, Port_label_output);
  Glyph gb = PEEK(0, 1);
  Usz a = index_of(PEEK(0, -1));
  Usz b = index_of(gb);
  if (b == 0)
    b = 36;
  Usz min, max;
  if (a == b) {
    POKE(1, 0, glyph_of(a));
    return;
  } else if (a < b) {
    min = a;
    max = b;
  } else {
    min = b;
    max = a;
  }
  Usz val = random_hashed(extra_params->random_seed, y, x, width, Tick_number,
                          min, max);
  POKE(1, 0, glyph_with_case(glyph_of(val), gb));
END_OPERATOR

// BOORCH's BOUNCER OP, tempo-synced since v2.0.0 (its rate counts beats).
// Predefined waveform sequences: one cycle of each shape in 128 steps, each
// step the output glyph over the full range 0-z. The table is kept as it was
// written, quirks included, so each shape keeps its documented look.
static char const *const waveforms[] = {
    // Triangle (0)
    "00112233445566778899aabbccddeeffgghhiijjkkllmmnnooppqqrrstuvwxyzzyxwvutsrr"
    "qqppoonnmmllkkjjiihhggffeeddccbbaa99887766554433221100",
    // Inverted Triangle (1)
    "zzyyxxwwvvuuttssrrqqppoonnmmllkkjjiihhggffeeddccbbaa9988765432100123456788"
    "99aabbccddeeffgghhiijjkkllmmnnooppqqrrssttuuvvwwxxyyzz",
    // Sine (2)
    "000000011111133333555558888cccfffiiilllooorrruuuwwwxxxyyyyyyzzzzzzzzyyyyyy"
    "xxxwwwuuurrrooollliiifffccc888855555333331111110000000",
    // Inverted Sine (3)
    "zzzzzzzyyyyyywwwwwtttttrrrrnnnkkkhhheeebbb88855533322211111100000000111111"
    "222333555888bbbeeehhhkkknnnrrrrtttttwwwwwyyyyyyzzzzzzz",
    // Square (4)
    "0000000000000000000000000000000000000000000000000000000000000000zzzzzzzzzz"
    "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
    // Inverted Square (5)
    "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz0000000000"
    "000000000000000000000000000000000000000000000000000000",
    // Saw (6)
    "0000111122223333444455556666777788889999aaaabbbbccccddddeeeeffffgggghhhhii"
    "iijjjjkkklllmmmnnnooopppqqqrrrssstttuuuvvvwwwxxxyyyzzz",
    // Inverted Saw (7)
    "zzzzyyyyxxxxwwwwvvvvuuuuttttssssrrrrqqqqppppoooonnnnmmmmllllkkkkjjjjiiiihh"
    "hhggggfffeeedddcccbbbaaa999888777666555444333222111000"};

#define WAVE_LENGTH 128
enum { Ticks_per_beat = 4 };

// The rate is the length of one cycle in beats, at 4 ticks to a beat, so a
// cycle is P = 4 x rate ticks and always lands on the grid. The phase is the
// number of ticks since the anchor, floor-mod P: the anchor is the tick of
// the last neighbouring bang, and 0 in a new entry, so a fresh & lines up
// with tick 0 and every & with the same rate is in step. A rate or shape
// change keeps the anchor. The shell sets every anchor back to 0 on an
// external MIDI start or song position (opstate_reset_anchors).
BEGIN_OPERATOR(bouncer)
  PORT(0, 1, IN | PARAM, Port_label_start); // Start value (a)
  PORT(0, 2, IN | PARAM, Port_label_end); // End value (b)
  PORT(0, 3, IN | PARAM, Port_label_rate); // Rate (the cycle length in beats)
  PORT(0, 4, IN | PARAM, Port_label_shape); // Shape (0-7 for different waveforms)
  PORT(1, 0, OUT, Port_label_output);

  Glyph start_g = PEEK(0, 1);
  Glyph end_g = PEEK(0, 2);
  Glyph rate_g = PEEK(0, 3);
  Glyph shape_g = PEEK(0, 4);

  if (start_g == '.' || end_g == '.')
    return;

  // This cell's state, from the caller's op-state store
  Opstate_entry *entry = opstate_lookup(extra_params->ctx->opstate, y, x,
                                        This_oper_char);
  if (!entry)
    return;
  Opstate_bouncer *state = &entry->u.bouncer;

  Usz start = index_of(start_g);
  Usz end = index_of(end_g);
  Usz rate = index_of(rate_g);
  Usz shape = index_of(shape_g);

  // A first visit seeds nothing: the zeroed anchor is tick 0. A bang moves
  // the anchor to this tick, which then outputs phase 0, whatever the rate.
  state->initialized = true;
  if (oper_has_neighboring_bang(gbuffer, height, width, y, x))
    state->anchor = Tick_number;

  // The table index: elapsed x 128 / P, floored, with elapsed the floor-mod
  // of (tick - anchor) by P in unsigned arithmetic, so an anchor above the
  // tick (as after an undo) still gives 0 <= elapsed < P, never a wrapped or
  // negative index. A rate worth 0 (`.`, `0`, `*`) holds index 0: the
  // phase-0 output, Start for shapes 0, 2, 4 and 6, End for 1, 3, 5 and 7.
  Usz idx = 0;
  if (rate > 0) {
    Usz period = Ticks_per_beat * rate;
    Usz anchor = state->anchor;
    Usz elapsed = Tick_number >= anchor
                      ? (Tick_number - anchor) % period
                      : (period - (anchor - Tick_number) % period) % period;
    idx = elapsed * WAVE_LENGTH / period;
  }

  shape = shape < 8 ? shape : 0;
  Usz v = index_of(waveforms[shape][idx]); // 0-35: the step over 0-z

  // v x range / 35 rounded half up, in integer math: (2 v range + 35) / 70.
  // 35 is odd, so no product is a half and the rounding never ties.
  Usz range = end > start ? end - start : start - end;
  Usz delta = (2 * v * range + 35) / 70;
  Usz output_value = end >= start ? start + delta : start - delta;

  POKE(1, 0, glyph_of(output_value));
END_OPERATOR

//////// The upstream dialect's own operators

// Orca-c 9df9786's !, ; and = bodies, unchanged but for PORT's label (so !
// names its three input ports one by one), so the upstream dialect's events,
// grid and locks match Orca-c's, as the upstream goldens check. Its C, D, I,
// L, U and Z are the *_upstream bodies beside bOrca's, and its R and r are
// random_upstream, above.

// A plain CC from three inputs: channel, control, and a value 0-z scaled to
// 0-127 by x 127 / 35. Never a glide.
BEGIN_OPERATOR(midicc_upstream)
  PORT(0, 1, IN, Port_label_channel);
  PORT(0, 2, IN, Port_label_control);
  PORT(0, 3, IN, Port_label_cc_value_upstream);
  STOP_IF_NOT_BANGED;
  Glyph channel_g = PEEK(0, 1);
  Glyph control_g = PEEK(0, 2);
  Glyph value_g = PEEK(0, 3);
  if (channel_g == '.' || control_g == '.')
    return;
  Usz channel = index_of(channel_g);
  if (channel > 15)
    return;
  PORT(0, 0, OUT, Port_label_output);
  Oevent_midi_cc *oe =
      (Oevent_midi_cc *)oevent_list_alloc_item(extra_params->oevent_list);
  oe->oevent_type = Oevent_type_midi_cc;
  oe->channel = (U8)channel;
  oe->control = (U8)index_of(control_g);
  oe->value = (U8)(index_of(value_g) * 127 / 35); // 0~35 -> 0~127
END_OPERATOR

// Sends the glyphs east of it, up to the first '.' and at most 16, as one
// UDP datagram. It locks them whether banged or not.
BEGIN_OPERATOR(udp)
  Usz n = width - x - 1;
  if (n > 16)
    n = 16;
  Glyph const *restrict gline = gbuffer + y * width + x + 1;
  Mark *restrict mline = mbuffer + y * width + x + 1;
  Glyph cpy[Oevent_udp_string_count];
  Usz i;
  for (i = 0; i < n; ++i) {
    Glyph g = gline[i];
    if (g == '.')
      break;
    cpy[i] = g;
    mline[i] |= Mark_flag_lock;
  }
  n = i;
  STOP_IF_NOT_BANGED;
  PORT(0, 0, OUT, Port_label_output);
  Oevent_udp_string *oe =
      (Oevent_udp_string *)oevent_list_alloc_item(extra_params->oevent_list);
  oe->oevent_type = (U8)Oevent_type_udp_string;
  oe->count = (U8)n;
  for (i = 0; i < n; ++i) {
    oe->chars[i] = cpy[i];
  }
END_OPERATOR

// Sends an OSC message to the path /<path glyph>, with count integers, each
// a value glyph's 0-35, read east of the count.
BEGIN_OPERATOR(osc)
  PORT(0, 1, IN | PARAM, Port_label_path);
  PORT(0, 2, IN | PARAM, Port_label_count);
  Usz len = index_of(PEEK(0, 2));
  if (len > Oevent_osc_int_count)
    len = Oevent_osc_int_count;
  for (Usz i = 0; i < len; ++i) {
    PORT(0, (Isz)i + 3, IN, Port_label_value);
  }
  STOP_IF_NOT_BANGED;
  Glyph g = PEEK(0, 1);
  if (g != '.') {
    PORT(0, 0, OUT, Port_label_output);
    U8 buff[Oevent_osc_int_count];
    for (Usz i = 0; i < len; ++i) {
      buff[i] = (U8)index_of(PEEK(0, (Isz)i + 3));
    }
    Oevent_osc_ints *oe =
        &oevent_list_alloc_item(extra_params->oevent_list)->osc_ints;
    oe->oevent_type = (U8)Oevent_type_osc_ints;
    oe->glyph = g;
    oe->count = (U8)len;
    for (Usz i = 0; i < len; ++i) {
      oe->numbers[i] = buff[i];
    }
  }
END_OPERATOR

//////// Run simulation

bool orca_dialect_from_name(char const *name, Orca_dialect *out) {
  if (strcmp(name, "borca") == 0) {
    *out = Orca_dialect_borca;
    return true;
  }
  if (strcmp(name, "upstream") == 0) {
    *out = Orca_dialect_upstream;
    return true;
  }
  if (strcmp(name, "zorca") == 0) {
    *out = Orca_dialect_zorca;
    return true;
  }
  if (strcmp(name, "seaflake") == 0) {
    *out = Orca_dialect_seaflake;
    return true;
  }
  if (strcmp(name, "morfil") == 0) {
    *out = Orca_dialect_morfil;
    return true;
  }
  return false;
}

char const *orca_dialect_name(Orca_dialect d) {
  switch (d) {
  case Orca_dialect_upstream:
    return "upstream";
  case Orca_dialect_zorca:
    return "zorca";
  case Orca_dialect_seaflake:
    return "seaflake";
  case Orca_dialect_morfil:
    return "morfil";
  case Orca_dialect_borca:
    return "borca";
  }
  return "borca"; // any other value runs as bOrca
}

void orca_run(Glyph *restrict gbuf, Mark *restrict mbuf, Usz height, Usz width,
              Usz tick_number, Oevent_list *oevent_list, Usz random_seed,
              Orca_run_ctx const *ctx) {
  assert(ctx && ctx->opstate);
  Glyph vars_slots[Glyphs_index_count];
  Glyph *vars = vars_slots;
  if (ctx->dialect == Orca_dialect_seaflake) {
    // Seaflake's variables live in the store, so a subtick sees the ones its
    // grid tick wrote; only a grid tick clears them.
    vars = ctx->opstate->seaflake_vars;
    if (!ctx->is_seaflake_subtick)
      memset(vars, '.', sizeof ctx->opstate->seaflake_vars);
  } else {
    memset(vars, '.', sizeof vars_slots);
  }
  if (!ctx->preserve_seaflake_scales)
    memset(ctx->opstate->seaflake_scales, 0,
           sizeof ctx->opstate->seaflake_scales);
  Oper_extra_params extras;
  extras.vars_slots = vars;
  extras.vars_masks = ctx->opstate->seaflake_scales;
  extras.oevent_list = oevent_list;
  extras.random_seed = random_seed;
  extras.ctx = ctx;
  extras.labels = ctx->labels;
  extras.active_dialect = ctx->dialect;

#define UNIQUE_CASE(_oper_char, _oper_name)                                    \
  case _oper_char:                                                             \
    oper_behavior_##_oper_name(gbuf, mbuf, height, width, iy, ix, tick_number, \
                               &extras, cell_flags, glyph_char);               \
    break;

#define ALPHA_CASE(_upper_oper_char, _oper_name)                               \
  case _upper_oper_char:                                                       \
  case (char)(_upper_oper_char | 1 << 5):                                      \
    oper_behavior_##_oper_name(gbuf, mbuf, height, width, iy, ix, tick_number, \
                               &extras, cell_flags, glyph_char);               \
    break;

#define DISPATCH(_dialect_unique_list, _dialect_alpha_list)                    \
  switch (glyph_char) {                                                        \
    SHARED_UNIQUE_OPERATORS(UNIQUE_CASE)                                       \
    _dialect_unique_list(UNIQUE_CASE)                                          \
    SHARED_ALPHA_OPERATORS(ALPHA_CASE)                                         \
    _dialect_alpha_list(ALPHA_CASE)                                            \
  }

  for (Usz iy = 0; iy < height; ++iy) {
    extras.active_dialect = ctx->dialect;
    Glyph const *glyph_row = gbuf + iy * width;
    Mark const *mark_row = mbuf + iy * width;
    for (Usz ix = 0; ix < width; ++ix) {
      Glyph glyph_char = glyph_row[ix];
      if (ORCA_LIKELY(glyph_char == '.'))
        continue;
      Mark cell_flags = mark_row[ix] & (Mark_flag_lock | Mark_flag_sleep);
      if (cell_flags & (Mark_flag_lock | Mark_flag_sleep))
        continue;

      switch (extras.active_dialect) {
      case Orca_dialect_upstream:
        DISPATCH(UPSTREAM_UNIQUE_OPERATORS, UPSTREAM_ALPHA_OPERATORS);
        break;
      case Orca_dialect_zorca:
        DISPATCH(ZORCA_UNIQUE_OPERATORS, ZORCA_ALPHA_OPERATORS);
        break;
      case Orca_dialect_seaflake:
        DISPATCH(SEAFLAKE_UNIQUE_OPERATORS, SEAFLAKE_ALPHA_OPERATORS);
        break;
      case Orca_dialect_morfil:
        DISPATCH(MORFIL_UNIQUE_OPERATORS, MORFIL_ALPHA_OPERATORS);
        break;
      case Orca_dialect_borca:
      default:
        // bOrca is the fallback, including for an invalid dialect value.
        DISPATCH(BORCA_UNIQUE_OPERATORS, BORCA_ALPHA_OPERATORS);
        break;
      }
    }
  }
#undef DISPATCH
#undef UNIQUE_CASE
#undef ALPHA_CASE
}

bool orca_run_bang_at(Glyph *gbuf, Usz height, Usz width, Usz y, Usz x,
                      Usz tick_number, Oevent_list *events, Usz random_seed,
                      Orca_run_ctx const *ctx) {
  if (!gbuf || !ctx || !ctx->opstate || y >= height || x >= width ||
      (width && height > SIZE_MAX / width))
    return false;
  Usz count = height * width;
  Mark *marks = malloc(count * sizeof *marks);
  Glyph *before = malloc(count * sizeof *before);
  bool *visited = calloc(count, sizeof *visited);
  bool *queued = calloc(count, sizeof *queued);
  Usz *queue = malloc(count * sizeof *queue);
  if (!marks || !before || !visited || !queued || !queue) {
    free(marks);
    free(before);
    free(visited);
    free(queued);
    free(queue);
    return false;
  }

  Orca_run_ctx subtick_ctx = *ctx;
  subtick_ctx.labels = NULL;
  subtick_ctx.preserve_seaflake_scales = true;
  subtick_ctx.is_seaflake_subtick = true;
  Usz head = 0, tail = 0;
  Usz first = y * width + x;
  gbuf[first] = '*';
  queue[tail++] = first;
  queued[first] = true;
  while (head < tail) {
    Usz bang = queue[head++];
    queued[bang] = false;
    if (visited[bang])
      continue;
    visited[bang] = true;
    memset(marks, (Mark)Mark_flag_lock, count * sizeof *marks);
    marks[bang] = (Mark)Mark_flag_lock;
    Usz by = bang / width, bx = bang % width;
    static Isz const dy[] = {0, 0, -1, 1};
    static Isz const dx[] = {-1, 1, 0, 0};
    for (Usz n = 0; n < ORCA_ARRAY_COUNTOF(dx); ++n) {
      Isz ny = (Isz)by + dy[n], nx = (Isz)bx + dx[n];
      if (ny < 0 || nx < 0 || (Usz)ny >= height || (Usz)nx >= width)
        continue;
      Usz cell = (Usz)ny * width + (Usz)nx;
      Glyph glyph = gbuf[cell];
      if (glyph == '.' || glyph == '*' || visited[cell])
        continue;
      marks[cell] = 0;
    }
    memcpy(before, gbuf, count * sizeof *before);
    orca_run(gbuf, marks, height, width, tick_number, events, random_seed,
             &subtick_ctx);
    if (gbuf[bang] == '*')
      gbuf[bang] = '.';
    for (Usz i = 0; i < count; ++i) {
      if (before[i] == '*' || gbuf[i] != '*' || visited[i] || queued[i])
        continue;
      if (tail == count) {
        free(marks);
        free(before);
        free(visited);
        free(queued);
        free(queue);
        return false;
      }
      queue[tail++] = i;
      queued[i] = true;
    }
  }
  free(marks);
  free(before);
  free(visited);
  free(queued);
  free(queue);
  return true;
}

bool orca_seaflake_next_deadline(Opstate_store const *store, U64 *deadline) {
  bool found = false;
  for (Usz i = 0; i < store->capacity; ++i) {
    Opstate_entry const *entry = store->slots + i;
    U64 candidate;
    if (entry->glyph == '~' && entry->u.seaflake_burst.active)
      candidate = entry->u.seaflake_burst.next_us;
    else if (entry->glyph == '@' && entry->u.seaflake_sequence.active)
      candidate = entry->u.seaflake_sequence.next_us;
    else
      continue;
    if (!found || candidate < *deadline) {
      *deadline = candidate;
      found = true;
    }
  }
  return found;
}

bool orca_seaflake_process_due(Glyph *gbuf, Usz height, Usz width,
                               Usz tick_number, Oevent_list *events,
                               Usz random_seed, Orca_run_ctx const *ctx,
                               U64 now_us) {
  Opstate_store *store = ctx->opstate;
  for (;;) {
    bool found = false;
    U64 due_us = 0;
    Usz y = 0, x = 0;
    Glyph glyph = 0;
    for (Usz i = 0; i < store->capacity; ++i) {
      Opstate_entry const *candidate = store->slots + i;
      U64 candidate_due;
      if (candidate->glyph == '~' && candidate->u.seaflake_burst.active)
        candidate_due = candidate->u.seaflake_burst.next_us;
      else if (candidate->glyph == '@' &&
               candidate->u.seaflake_sequence.active)
        candidate_due = candidate->u.seaflake_sequence.next_us;
      else
        continue;
      if (candidate_due <= now_us && (!found || candidate_due < due_us)) {
        found = true;
        due_us = candidate_due;
        y = candidate->y;
        x = candidate->x;
        glyph = candidate->glyph;
      }
    }
    if (!found)
      return true;

    Opstate_entry *entry = opstate_lookup(store, y, x, glyph);
    if (!entry)
      return false;
    if (glyph == '~') {
      if (gbuf[y * width + x] != '~') {
        entry->u.seaflake_burst.active = false;
        continue;
      }
      Orca_run_ctx event_ctx = *ctx;
      event_ctx.now_us = due_us;
      if (y + 1 < height &&
          !orca_run_bang_at(gbuf, height, width, y + 1, x, tick_number,
                            events, random_seed, &event_ctx)) {
        entry = opstate_lookup(store, y, x, '~');
        if (entry)
          entry->u.seaflake_burst.active = false;
        return false;
      }
      entry = opstate_lookup(store, y, x, '~');
      if (!entry)
        return false;
      Opstate_seaflake_burst *burst = &entry->u.seaflake_burst;
      if (burst->remaining > 0)
        --burst->remaining;
      if (burst->remaining == 0)
        burst->active = false;
      else
        burst->next_us += burst->interval_us;
    } else {
      Opstate_seaflake_sequence *sequence = &entry->u.seaflake_sequence;
      if (sequence->emitted < sequence->repeats) {
        Usz absolute = 24 + sequence->octave * 12 + sequence->note;
        if (absolute > 127)
          absolute = 127;
        if (sequence->note_active) {
          seaflake_add_subtick_note(events, sequence->active_channel,
                                    sequence->active_note, 0, true);
          sequence->note_active = false;
        }
        Oevent_midi_note *oe =
            (Oevent_midi_note *)oevent_list_alloc_item(events);
        *oe = (Oevent_midi_note){.oevent_type = Oevent_type_midi_note,
                                 .channel = sequence->channel,
                                 .octave = (U8)(absolute / 12),
                                 .note = (U8)(absolute % 12),
                                 .velocity = sequence->velocity,
                                 .duration = 1,
                                 .mono = 0,
                                 .tuned = 0,
                                 .subtick = 1,
                                 .pitch_bend = 0};
        sequence->active_channel = sequence->channel;
        sequence->active_note = (U8)absolute;
        sequence->note_active = true;
        ++sequence->emitted;
        if (sequence->emitted < sequence->repeats)
          sequence->next_us =
              sequence->started_us +
              seaflake_sequence_offset(sequence, sequence->emitted);
        else
          sequence->next_us =
              sequence->started_us + sequence->duration_us;
      } else {
        if (sequence->note_active) {
          seaflake_add_subtick_note(events, sequence->active_channel,
                                    sequence->active_note, 0, true);
          sequence->note_active = false;
        }
        sequence->active = false;
      }
    }
  }
}
