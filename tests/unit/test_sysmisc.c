// Unit tests for sysmisc.c, shell code outside CORE that tool links into the
// runner with thirdparty/oso.c; both use libc only. They read lines
// from a tmpfile() through conf_read_line, the reader under ezconf's load
// and save of orca.conf, and save through an Ezconf_w over tmpfile()s.
#define _POSIX_C_SOURCE 200809L // fileno, dup, lseek, read, close
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "../../sysmisc.h"
#include "../../thirdparty/oso.h"
#include "tests.h"
#include <unistd.h>

// A CHECK reported at the row's own line, so each FAIL line names its row
// and the check that failed.
#define CHECK_AT(line, expr)                                                   \
  ((expr) ? (void)0 : unit_check_failed(__FILE__, line, #expr))

// Writes the n bytes to a tmpfile(), rewinds it and reads one line into a
// 64-byte array on the stack, so the debug runner's ASan sees a write before
// it, passing conf_read_line bufsize (at most 64). The read must give want:
// for Conf_read_irrelevant, left is buf holding want_left, leftsz is its
// length and there is no right side; for Conf_read_left_and_right, a key
// line split into want_left and want_right; for any other result, neither
// side. Either way the next read is Conf_read_eof. A file tmpfile() cannot
// make fails the row, never skips it.
static void read_one(int line, char const *bytes, size_t n, Usz bufsize,
                     Conf_read_result want, char const *want_left,
                     char const *want_right) {
  FILE *f = tmpfile();
  CHECK_AT(line, f != NULL);
  if (!f)
    return;
  size_t wrote = fwrite(bytes, 1, n, f);
  CHECK_AT(line, wrote == n);
  rewind(f);
  char buf[64];
  char *left = buf + 1, *right = buf + 1; // neither NULL nor buf
  Usz leftsz = 99, rightsz = 99;
  Conf_read_result res =
      conf_read_line(f, buf, bufsize, &left, &leftsz, &right, &rightsz);
  CHECK_AT(line, res == want);
  if (want == Conf_read_irrelevant) {
    CHECK_AT(line, left == buf);
    CHECK_AT(line, left && strcmp(left, want_left) == 0);
    CHECK_AT(line, leftsz == strlen(want_left));
    CHECK_AT(line, right == NULL && rightsz == 0);
  } else if (want == Conf_read_left_and_right) {
    CHECK_AT(line, left && strcmp(left, want_left) == 0);
    CHECK_AT(line, leftsz == strlen(want_left));
    CHECK_AT(line, right && strcmp(right, want_right) == 0);
    CHECK_AT(line, rightsz == strlen(want_right));
  } else {
    CHECK_AT(line, left == NULL && leftsz == 0);
    CHECK_AT(line, right == NULL && rightsz == 0);
  }
  res = conf_read_line(f, buf, bufsize, &left, &leftsz, &right, &rightsz);
  CHECK_AT(line, res == Conf_read_eof);
  fclose(f);
}

// Saves `key = 1` as tui_save_prefs does (ezconf_w_step until it returns
// false, the value written after each true) through an Ezconf_w whose
// original and temporary files are tmpfile()s holding the n bytes and
// nothing, in place of ezconf_w_start's files by path. Its paths are empty,
// so the commit closes the temporary file and then fails to rename it:
// the save ends in Ezconf_w_rename_failed, and the written bytes, read back
// through a dup of the temporary file's descriptor, must be want.
static void save_one(int line, char const *bytes, size_t n,
                     char const *want) {
  FILE *in = tmpfile(), *out = tmpfile();
  int fd = out ? dup(fileno(out)) : -1;
  CHECK_AT(line, in != NULL && out != NULL && fd != -1);
  if (!in || !out || fd == -1) {
    if (in)
      fclose(in);
    if (out)
      fclose(out);
    if (fd != -1)
      close(fd);
    return;
  }
  size_t wrote = fwrite(bytes, 1, n, in);
  CHECK_AT(line, wrote == n);
  rewind(in);
  Ezconf_opt opts[1];
  Ezconf_w ez = {.save = {.origfile = in, .tempfile = out},
                 .opts = opts,
                 .optscap = 1,
                 .file = out};
  osoput(&ez.save.canonpath, "");
  osoput(&ez.save.temppath, "");
  CHECK_AT(line, ez.save.canonpath && ez.save.temppath);
  if (!ez.save.canonpath || !ez.save.temppath) {
    conf_save_cancel(&ez.save); // closes both files
    close(fd);
    return;
  }
  ezconf_w_addopt(&ez, "key", 0);
  while (ezconf_w_step(&ez))
    fputs("1", ez.file);
  CHECK_AT(line, ez.error == Ezconf_w_rename_failed);
  char got[128];
  ssize_t got_n = -1;
  if (lseek(fd, 0, SEEK_SET) == 0)
    got_n = read(fd, got, sizeof got);
  close(fd);
  CHECK_AT(line, got_n >= 0 && (size_t)got_n == strlen(want) &&
                     memcmp(got, want, strlen(want)) == 0);
}

// Rows: the file's bytes (a string literal, which may hold a NUL), then
// what is wanted: the line's text for a line that is not a key line, the
// two sides for a key line, or a result and the text in a smaller buffer.
#define ROW(bytes, left)                                                       \
  read_one(__LINE__, bytes, sizeof(bytes) - 1, 64, Conf_read_irrelevant,       \
           left, NULL)
#define KEY_ROW(bytes, left, right)                                            \
  read_one(__LINE__, bytes, sizeof(bytes) - 1, 64, Conf_read_left_and_right,   \
           left, right)
#define ROW_SIZED(bytes, bufsize, want, left)                                  \
  read_one(__LINE__, bytes, sizeof(bytes) - 1, bufsize, want, left, NULL)
#define SAVE(bytes, want) save_one(__LINE__, bytes, sizeof(bytes) - 1, want)

// conf_read_line clears a line's last byte only when it is '\n', and
// leftsz is the length without it. Until v3.0.2 its ignore: path cleared the
// last byte whatever it was, so an unterminated last line that is not a key
// line (a comment at the end of orca.conf) lost its last byte on the next
// save, and a line whose first byte is NUL (strlen 0) got a write at
// buf[-1].
void test_conf_read_line_tail(void) {
  // Rows 1-3: comments, unterminated, terminated, and CRLF, which keeps its
  // '\r' (only '\n' is cleared).
  ROW("# a comment at the end", "# a comment at the end");
  // Row 1 through a save: the comment is written back whole, with a newline.
  SAVE("key = 0\n# a comment at the end", "key = 1\n# a comment at the end\n");
  ROW("# a comment\n", "# a comment");
  ROW("# crlf\r\n", "# crlf\r");
  // Rows 4-6: the other ignore exits (blank, no '=', '=' first),
  // unterminated.
  ROW("   ", "   ");
  ROW("orphan", "orphan");
  ROW("=x", "=x");
  // Row 7: a NUL first byte reads as an empty line; the bytes after it are
  // lost, as they were before v3.0.2. That nothing is written before buf is
  // seen only by the debug runner's ASan.
  ROW("\0abc\n", "");
  // Row 8: an unterminated key line splits as it always has.
  KEY_ROW("key = value", "key", "value");
  // Row 9: a NUL inside a line ends its text. The byte before it stays
  // (until v3.0.2 it was cleared); the bytes after it are lost, as before.
  ROW("ab\0cd\n", "ab");
  // Rows 10-11: an unterminated last line of bufsize - 1 bytes is still too
  // long for the buffer; one of bufsize - 2 bytes reads whole.
  ROW_SIZED("# 34567", 8, Conf_read_buffer_too_small, NULL);
  ROW_SIZED("# 3456", 8, Conf_read_irrelevant, "# 3456");
}

// Pins the formatting of the zOrca events dropped counter line: the total
// is the sum of the three counts, and each count goes into its respective
// position (tick, preview, engine).
void test_events_dropped_format(void) {
  char buf[128];
  int n = events_dropped_format(buf, sizeof buf, 0, 0, 0);
  CHECK(n == (int)strlen(buf));
  CHECK(strcmp(buf, "zOrca events: 0 dropped (tick 0, preview 0, engine 0)\n") == 0);

  // Isolate each count to pin its position.
  events_dropped_format(buf, sizeof buf, 7, 0, 0);
  CHECK(strcmp(buf, "zOrca events: 7 dropped (tick 7, preview 0, engine 0)\n") == 0);

  events_dropped_format(buf, sizeof buf, 0, 8, 0);
  CHECK(strcmp(buf, "zOrca events: 8 dropped (tick 0, preview 8, engine 0)\n") == 0);

  events_dropped_format(buf, sizeof buf, 0, 0, 9);
  CHECK(strcmp(buf, "zOrca events: 9 dropped (tick 0, preview 0, engine 9)\n") == 0);

  // Distinct counts: check exact string and parse back with sscanf.
  events_dropped_format(buf, sizeof buf, 11, 22, 33);
  CHECK(strcmp(buf, "zOrca events: 66 dropped (tick 11, preview 22, engine 33)\n") == 0);

  Usz total = 0, tick = 0, preview = 0, engine = 0;
  int matched = sscanf(
      buf,
      "zOrca events: %zu dropped (tick %zu, preview %zu, engine %zu)\n",
      &total, &tick, &preview, &engine);
  CHECK(matched == 4);
  CHECK(tick == 11);
  CHECK(preview == 22);
  CHECK(engine == 33);
  CHECK(total == tick + preview + engine);
  CHECK(total == 66);
}

// Verifies path_is_dir distinguishes directories from regular files and
// non-existent paths.
void test_path_is_dir(void) {
  CHECK(!path_is_dir(NULL));
  CHECK(!path_is_dir(""));
  CHECK(!path_is_dir("/nonexistent_path_zorca_unit_test"));
  CHECK(path_is_dir("."));
  CHECK(path_is_dir("/tmp"));

  char file_path[] = "/tmp/zorca-unit-isdir-file-XXXXXX";
  int fd = mkstemp(file_path);
  CHECK(fd >= 0);
  if (fd >= 0) {
    close(fd);
    CHECK(!path_is_dir(file_path));
    unlink(file_path);
  }

  char dir_path[] = "/tmp/zorca-unit-isdir-dir-XXXXXX";
  char *d = mkdtemp(dir_path);
  CHECK(d != NULL);
  if (d) {
    CHECK(path_is_dir(dir_path));
    rmdir(dir_path);
  }
}
