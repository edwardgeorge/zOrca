#pragma once
#include "base.h"
#include "opstate.h"
#include "portlabel.h"
#include "vmio.h"

// The operator list orca_run runs, one per dialect. Zero is bOrca,
// so a zero-initialized context runs bOrca's operators; orca and cli
// default to zorca, which a caller of orca_run names itself.
//   borca:    bOrca's list: ! (CC with glides), $, ; (arpeggiator),
//             = (midichord), & and the shuffle-bag r; C, D, I, L, R, U
//             and Z by Orca's 2025 rules, in which an empty input counts
//             as 0 and R's range includes its max.
//   upstream: Orca-c 9df9786's list: ! (plain CC, value x 127 / 35),
//             ; (UDP), = (OSC) and r as a banged R; $ and & do nothing;
//             C, D, I, L, R, U and Z by Orca-c's rules for empty inputs
//             and R's range.
//   zorca:    zOrca's own: bOrca's whole list, to which zOrca's operators
//             are added one at a time. It has none of its own yet, so it
//             plays exactly as borca does.
//   seaflake: the JavaScript fork's operators, including its own meanings
//             for ^, &, ~, +, ), ( and @. The shared alphabet operators
//             follow upstream Orca-c rules.
//   morfil: currently runs bOrca's full list, reserved for its own set.
// Values are appended, so borca stays 0 and upstream 1.
typedef enum {
  Orca_dialect_borca = 0,
  Orca_dialect_upstream = 1,
  Orca_dialect_zorca = 2,
  Orca_dialect_seaflake = 3,
  Orca_dialect_morfil = 4,
} Orca_dialect;

// What orca_run takes beyond its first seven arguments, in one context struct.
// The pointer is valid for one call and core keeps nothing between calls; the
// data it points to stays mutable. Later fields default to their zero value.
// The op-state store is the exception: it is required, and debug builds assert
// it.
typedef struct {
  Opstate_store *opstate; // stateful operator data and Seaflake scales
  Orca_dialect dialect;   // the operator list, chosen once per call
  U64 now_us;             // the shell's monotonic time for timed operators
  U64 tick_len_us;        // one grid frame, for tempo-relative operators
  bool preserve_seaflake_scales; // subtick dispatch keeps the latest + values
  bool is_seaflake_subtick; // generated bangs must not restart their source
  // The label buffer (ids from portlabel.h): height x width cells that
  // the caller owns and clears with the marks (tick_run_vm, tick.h). Every
  // PORT records its label id and owner into it. NULL, the default, records
  // nothing; cli passes NULL, and the marks, grid and events are the same
  // either way.
  Port_label_cell *labels;
} Orca_run_ctx;

// The dialect names that cli's and orca's --dialect and the dialect key of
// orca.conf take: "borca", "upstream", "zorca", "seaflake" and
// "morfil", exactly.
// Returns false, leaving *out alone, for any other string.
bool orca_dialect_from_name(char const *name, Orca_dialect *out);
// The name of d. Invalid enum values report "borca", as orca_run falls back
// to that dialect.
char const *orca_dialect_name(Orca_dialect d);

void orca_run(Glyph *restrict gbuffer, Mark *restrict mbuffer, Usz height,
              Usz width, Usz tick_number, Oevent_list *oevent_list,
              Usz random_seed, Orca_run_ctx const *ctx);
bool orca_run_bang_at(Glyph *gbuffer, Usz height, Usz width, Usz y, Usz x,
                      Usz tick_number, Oevent_list *oevent_list,
                      Usz random_seed, Orca_run_ctx const *ctx);
bool orca_seaflake_next_deadline(Opstate_store const *store, U64 *deadline);
bool orca_seaflake_process_due(Glyph *gbuffer, Usz height, Usz width,
                               Usz tick_number, Oevent_list *events,
                               Usz random_seed, Orca_run_ctx const *ctx,
                               U64 now_us);
