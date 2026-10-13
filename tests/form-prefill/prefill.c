// A one-line box keeps the whole of the text it opens with. Builds a form
// field the way the Open and Save As boxes do, with no screen, and reads the
// text back: a field is 30 columns wide, and before the fix only the first 30
// characters were kept.
#include "term_util.h"
#include "oso.h"
#include <stdio.h>
#include <string.h>

static int check_length(Usz len) {
  char text[256];
  for (Usz i = 0; i < len; ++i)
    text[i] = (char)('a' + (char)(i % 26));
  text[len] = '\0';
  Qform *qf = qform_create(1);
  qform_add_line_input(qf, 1, text);
  oso *got = NULL;
  bool ok = qform_get_text_line(qf, 1, &got);
  int fail = !ok || !got || osolen(got) != len || memcmp(osoc(got), text, len);
  printf("%s  %zu characters: kept %zu\n", fail ? "FAIL" : "PASS", len,
         got ? osolen(got) : (Usz)0);
  osofree(got);
  return fail;
}

int main(void) {
  static Usz const lengths[] = {1, 29, 30, 31, 47, 200};
  int fails = 0;
  for (Usz i = 0; i < sizeof lengths / sizeof lengths[0]; ++i)
    fails += check_length(lengths[i]);
  printf("form prefill check: %d failed\n", fails);
  return fails ? 1 : 0;
}
