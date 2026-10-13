#pragma once
#include "base.h"
#include "opstate.h"
#include "portlabel.h"
#include "vmio.h"

// The operator list orca_run runs, one per dialect. Zero is bOrca,
// so a zero-initialized context runs bOrca's operators.
//   borca:    bOrca's list: ! (CC with glides), $, ; (arpeggiator),
//             = (midichord), & and the shuffle-bag r; C, D, I, L, R, U
//             and Z by Orca's 2025 rules, in which an empty input counts
//             as 0 and R's range includes its max.
//   upstream: Orca-c 9df9786's list: ! (plain CC, value x 127 / 35),
//             ; (UDP), = (OSC) and r as a banged R; $ and & do nothing;
//             C, D, I, L, R, U and Z by Orca-c's rules for empty inputs
//             and R's range.
// The other 19 letters, and *, #, :, % and ?, are the same in both.
typedef enum {
  Orca_dialect_borca = 0,
  Orca_dialect_upstream = 1,
} Orca_dialect;

// What orca_run takes beyond its first seven arguments, in one context struct.
// The pointer is valid for one call and core keeps nothing between calls; the
// data it points to stays mutable. Later fields default to their zero value.
// The op-state store is the exception: it is required, and debug builds assert
// it.
typedef struct {
  Opstate_store *opstate; // the per-cell state of &, ; and r (opstate.h)
  Orca_dialect dialect;   // the operator list, chosen once per call
  // The label buffer (ids from portlabel.h): height x width cells that
  // the caller owns and clears with the marks (tick_run_vm, tick.h). Every
  // PORT records its label id and owner into it. NULL, the default, records
  // nothing; cli passes NULL, and the marks, grid and events are the same
  // either way.
  Port_label_cell *labels;
} Orca_run_ctx;

// The dialect names that cli's and orca's --dialect and the dialect key of
// orca.conf take: "borca" and "upstream", exactly. Returns false, leaving
// *out alone, for any other string.
bool orca_dialect_from_name(char const *name, Orca_dialect *out);
// The name orca_dialect_from_name reads as d. Any value but
// Orca_dialect_upstream is "borca", as orca_run runs it.
char const *orca_dialect_name(Orca_dialect d);

void orca_run(Glyph *restrict gbuffer, Mark *restrict mbuffer, Usz height,
              Usz width, Usz tick_number, Oevent_list *oevent_list,
              Usz random_seed, Orca_run_ctx const *ctx);
