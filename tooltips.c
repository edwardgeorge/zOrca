#include "tooltips.h"
#include "gbuffer.h"
#include "music.h"
#include "portlabel.h"
#include <string.h>

// The tooltips (spec item I5, spine AD-19). Testable shell: no I/O, no
// writable globals, and only the core include allow-list, so the unit runner
// links this file (spine AD-1, AD-2, AD-21). See tooltips.h.

// Copies text into buf, which holds Tooltip_line_max bytes, cut to fit.
static void put_text(char *buf, char const *text) {
  Usz n = strlen(text);
  if (n > Tooltip_line_max - 1)
    n = Tooltip_line_max - 1;
  memcpy(buf, text, n);
  buf[n] = '\0';
}

// Writes v in decimal after the prefix, in place of snprintf (the core
// include allow-list has no <stdio.h>). Cut to fit, like put_text.
static void put_text_uint(char *buf, char const *prefix, Usz v) {
  char digits[sizeof(Usz) * 3 + 1];
  Usz n = 0;
  do {
    digits[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v > 0);
  Usz at = strlen(prefix);
  if (at > Tooltip_line_max - 1)
    at = Tooltip_line_max - 1;
  memcpy(buf, prefix, at);
  while (n > 0 && at < Tooltip_line_max - 1)
    buf[at++] = digits[--n];
  buf[at] = '\0';
}

bool tooltip_at(Port_label_cell const *labels, Usz labels_h, Usz labels_w,
                Glyph const *gbuffer, Usz field_h, Usz field_w, Usz cursor_y,
                Usz cursor_x, char *line1, char *line2) {
  line1[0] = '\0';
  line2[0] = '\0';
  // A buffer sized for another grid is stale, and may be smaller than the
  // field: no tooltip until the next run sizes it. Height and width are
  // compared one by one, since 10 x 20 and 20 x 10 hold the same count.
  if (!labels || labels_h != field_h || labels_w != field_w)
    return false;
  if (cursor_y >= field_h || cursor_x >= field_w)
    return false;
  Usz at = cursor_y * field_w + cursor_x;
  Port_label_cell const *cell = labels + at;
  char const *text = port_label_text((Port_label)cell->id);
  if (text[0] == '\0')
    return false; // no port, or an output, which shows nothing
  Glyph g = gbuffer[at];
  if (g != '.') {
    switch ((Port_label)cell->id) {
    case Port_label_chord_type:
      if (music_selector_name(Music_op_midichord, g, line2,
                              Tooltip_line_max)) {
        put_text(line1, "Chord type:");
        return true;
      }
      break;
    case Port_label_scale_selector:
      if (music_selector_name(Music_op_scale, g, line2, Tooltip_line_max)) {
        put_text(line1, g >= '0' && g <= '9' ? "Scale:" : "Chord:");
        return true;
      }
      break;
    case Port_label_cc_value: {
      Usz v = glyph_index_of(g) * 4;
      put_text(line1, "Value:");
      put_text_uint(line2, "", v > 127 ? 127 : v);
      return true;
    }
    case Port_label_cc_value_upstream:
      put_text(line1, "Value:");
      put_text_uint(line2, "", glyph_index_of(g) * 127 / 35);
      return true;
    case Port_label_glide_rate: {
      Usz rate = glyph_index_of(g);
      put_text(line1, "Interpolation:");
      if (rate == 0 || rate == 35) // the instant class (ccout.h)
        put_text(line2, "instant");
      else
        put_text_uint(line2, "rate ", rate);
      return true;
    }
    default:
      break;
    }
  }
  put_text(line1, text);
  line2[0] = '\0';
  return true;
}

int tooltip_place(int limit, int len, int width, int *x) {
  int right = width - 1 - len;
  int at = limit + 1 > right ? limit + 1 : right;
  *x = at;
  return width - 1 - at;
}
