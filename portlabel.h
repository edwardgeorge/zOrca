#pragma once
#include "base.h"

// The port labels, held once here for sim.c and the tooltips. Each PORT call
// in sim.c names its port with one id from the X-macro below, and when the
// caller gives orca_run a label buffer (Orca_run_ctx.labels, sim.h), PORT
// records the id and the owner's offset in the port's cell. tooltips.c reads
// the cursor's cell from that buffer and keys its enhancements on the id
// alone, never on the owner glyph or the dialect. The texts are the labels
// the tooltip shows; the output id's text is empty, so an output port
// records its owner but shows nothing.
//
// Ids differ where the display differs: the chord selector (`=`'s 4th
// input), the scale selector (`$`'s 3rd), the CC value scaled x 4 (bOrca's
// `!`), the CC value scaled x 127 / 35 (upstream's `!`) and the glide rate
// (bOrca's `!`) each have their own id, since tooltips.c decodes them. Ports
// whose text and display are the same share one id. Id 0 means no port.
#define PORTLABEL_LIST(X)                                                      \
  X(none, "")                                                                  \
  X(output, "")                                                                \
  X(channel, "Channel")                                                        \
  X(control_hundreds, "Control (hundreds)")                                    \
  X(control_tens, "Control (tens)")                                            \
  X(control_ones, "Control (ones)")                                            \
  X(cc_value, "Value")              /* bOrca's `!`: index x 4, at most 127 */  \
  X(glide_rate, "Interpolation Rate") /* bOrca's `!`: rate 0-35; `.` is no glide */   \
  X(octave, "Octave")                                                          \
  X(note, "Note")                                                              \
  X(velocity, "Velocity")                                                      \
  X(length, "Length")                                                          \
  X(msb, "MSB")                                                                \
  X(lsb, "LSB")                                                                \
  X(value_a, "Value A")                                                        \
  X(value_b, "Value B")                                                        \
  X(factor_a, "Factor A")                                                      \
  X(factor_b, "Factor B")                                                      \
  X(rate, "Rate")                                                              \
  X(modulo, "Modulo")                                                          \
  X(x_offset, "X offset")                                                      \
  X(y_offset, "Y offset")                                                      \
  X(input, "Input")                                                            \
  X(max, "Max")                                                                \
  X(min, "Min")                                                                \
  X(variable, "Variable")                                                      \
  X(value, "Value")                 /* a plain value: V's, upstream `=`'s */     \
  X(key, "Key")                                                                \
  X(steps, "Steps")                                                            \
  X(target, "Target")                                                          \
  X(root, "Root")                                                              \
  X(scale_selector, "Scale")        /* `$`: 0-9 a scale, else a chord */       \
  X(degree, "Degree")                                                          \
  X(root_note, "Root note")                                                    \
  X(chord_type, "Chord type")       /* `=`: the chord selector */              \
  X(range, "Range")                                                            \
  X(pattern, "Pattern")                                                        \
  X(step, "Step")                                                              \
  X(start, "Start")                                                            \
  X(end, "End")                                                                \
  X(shape, "Shape")                                                            \
  X(control, "Control")                                                        \
  X(cc_value_upstream, "Value")     /* upstream's `!`: index x 127 / 35 */     \
  X(path, "Path")                                                              \
  X(count, "Count")                                                            \
  X(chance, "Chance")                                                          \
  X(phase, "Phase")                                                            \
  X(division, "Division")                                                      \
  X(multiplication, "Multiplication")                                          \
  X(scale, "Scale")                                                            \
  X(pitch, "Pitch")                                                            \
  X(pulse_width, "Pulse width")                                                \
  X(dialect, "Dialect")                                                        \
  X(reset, "Reset")


// The ids, Port_label_<name>, in the order above; Port_label_none is 0 and
// every id is below Port_label_id_count.
typedef enum {
#define PORTLABEL_ENUM(name, text) Port_label_##name,
  PORTLABEL_LIST(PORTLABEL_ENUM)
#undef PORTLABEL_ENUM
      Port_label_id_count
} Port_label;

// One cell of the label buffer: the port's label id, whether the port
// counted as an input, and the offset from the cell to the owner, so that
// owner = cell + (owner_dy, owner_dx). PORT records them (sim.c): an input
// that is not also an output replaces any record; any other port (an output,
// or an input that is also an output, as I's and Z's read-write cell)
// replaces only a record that counted as an output, or an empty cell. So an
// input beats an output, and between ports of the same kind the later PORT
// call, from the operator scanned later, wins. The offset is within
// -256..+256 on each axis, the farthest an operator reaches (J and Y), and
// debug builds assert it. The buffer is sized like the mark buffer, height x
// width, and cleared with it (tick_run_vm, tick.h). A zeroed cell is no
// port.
typedef struct {
  U8 id;        // a Port_label; 0 is no port
  bool is_input; // the port counted as an input
  I16 owner_dy, owner_dx;
  U8 dialect;
} Port_label_cell;

// The label text of id, "" for Port_label_none, Port_label_output and any
// value at or above Port_label_id_count. The table is const at every level and
// lives here, inside the function, so only the files that look labels up
// define it (tooltips.c; sim.c records ids and never reads a text).
static inline char const *port_label_text(Port_label id) {
#define PORTLABEL_TEXT(name, text) text,
  static char const *const texts[] = {PORTLABEL_LIST(PORTLABEL_TEXT)};
#undef PORTLABEL_TEXT
  return (Usz)id < ORCA_ARRAY_COUNTOF(texts) ? texts[id] : "";
}
