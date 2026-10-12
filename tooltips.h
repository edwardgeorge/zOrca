#pragma once
#include "base.h"
#include "music.h"
#include "portlabel.h"

// The tooltips (spec item I5, spine AD-19): the text the HUD shows for the
// port under the cursor, read from the label buffer the VM filled
// (portlabel.h, Orca_run_ctx.labels), and where each line goes on a HUD row.
// tooltips.c is testable shell (spine AD-1, AD-2, AD-21): it obeys the core
// include allow-list, does no I/O and keeps no writable globals, so the unit
// runner links it. Every result goes into a buffer the caller supplies.

// The bytes each line buffer needs: every label, every selector name
// (Music_name_max, music.h) and every "rate N" or value fits, with the NUL.
enum { Tooltip_line_max = Music_name_max };

// The tooltip for the cursor's cell. labels is the buffer the last VM run
// filled, sized labels_h x labels_w (the size the shell sized it for, which
// it remembers); gbuffer is the field, field_h x field_w, and the cursor is
// a cell in it. Writes line1 and line2, each Tooltip_line_max bytes, and
// returns true when there is a tooltip to show; line2 is empty for a
// one-line tooltip. Returns false, with both lines empty, when labels is
// NULL, when labels_h or labels_w differs from field_h or field_w (the
// buffer is stale: the grid was resized and no run has sized it since), when
// the cursor is outside the field, when the cell has no record, or when the
// record's label text is empty (an output port).
//
// Enhancements key on the record's id alone, and only when the cell's glyph
// is not `.`:
//   chord selector:  "Chord type:" and the chord's name (Music_op_midichord);
//   scale selector:  "Scale:" for `0`-`9`, else "Chord:", and the selector's
//                    name (Music_op_scale);
//   CC value x 4:    "Value:" and min(index x 4, 127);
//   CC value x 127/35: "Value:" and index x 127 / 35;
//   glide rate:      "Interpolation:" and "instant" for rate index 0 or 35
//                    (the instant class, ccout.h), else "rate N".
// A `.` cell, or a selector glyph with no name, shows the plain label.
bool tooltip_at(Port_label_cell const *labels, Usz labels_h, Usz labels_w,
                Glyph const *gbuffer, Usz field_h, Usz field_w, Usz cursor_y,
                Usz cursor_x, char *line1, char *line2);

// Where one tooltip line goes on a HUD row of width columns. limit is the
// first free column after the row's right-most field and len the line's
// length. Sets *x to max(limit + 1, width - 1 - len), which leaves one blank
// column after the field and the right-most column free, and returns the
// number of columns to draw, width - 1 - *x, which cuts the line from the
// end; nothing is drawn when that is 0 or less. Pure: no I/O.
int tooltip_place(int limit, int len, int width, int *x);
