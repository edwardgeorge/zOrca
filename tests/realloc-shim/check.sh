#!/bin/sh
# The realloc shim check: when realloc or malloc fails, bOrca
# keeps its grid and mark buffer as they were, and says so where it cannot
# go on; and a failed startup load leaks nothing. CI runs the cli case and
# the release orca cases; the leak case needs a debug build and is a
# development check.
#
#   tests/realloc-shim/check.sh <cli> [<orca> [<debug-orca>]]
#
# <cli> and <orca> must be release builds: ASan refuses a preloaded library
# ahead of its runtime. <debug-orca> must be a -d build (ASan, whose
# LeakSanitizer the leak case reads); nothing is preloaded into it. Build
# both orcas without --alsa or --portmidi, so they open no MIDI device: the
# check refuses (exit 2, before any run) an <orca> or <debug-orca> whose
# `readelf -d` lists libasound or libportmidi, and every orca run also has
# ZORCA_ALSA_CLIENT_NAME=borca-realloc-shim-<pid>. It unsets the caller's
# SHIM_MALLOC_MAX and SHIM_REALLOC_MAX, so each case sets only the limits it
# names. The check compiles shim.c, beside this script, with $CC (default
# cc) into a scratch directory: an LD_PRELOAD library that fails every
# realloc above SHIM_REALLOC_MAX bytes and every malloc above
# SHIM_MALLOC_MAX bytes (each unset or empty: none fails). Then:
# - cli: `cli -t 1` on a 100 x 50 grid file with SHIM_REALLOC_MAX=4096 must
#   exit 1, not on a signal, with "File load error: Not enough memory for
#   the grid."; a 20 x 10 file under the same shim must run.
# - startup (with <orca>): `orca --initial-size 200x200` under script and
#   `timeout 10`, with SHIM_REALLOC_MAX=30000, must exit 1 with "Cannot
#   start: not enough memory for the grid".
# - fill (with <orca>): the same run with SHIM_MALLOC_MAX=30000 and no
#   realloc limit, so the 40000-byte startup grid cannot be allocated, must
#   exit 1 with the same message.
# - resize (with <orca>): `orca --initial-size 1000x25` in a private tmux
#   server, keys + + (the ruler of 8 grows 25 rows to 33, 33000 bytes), then
#   Ctrl+Q. Without a limit, the control, the HUD must read 1000x41 after the
#   two +; with SHIM_REALLOC_MAX=30000 orca must still run, its HUD read
#   1000x25 and its frame counter advance, and it must exit 0 on Ctrl+Q.
#   timeout runs with --foreground: in its own process group it would leave
#   orca in the background, stopped by SIGTTIN.
# - leak (with <debug-orca>, no shim): the binary must reference
#   __asan_init, else FAIL (a release build would pass vacuously). In a
#   private tmux server, under `timeout --foreground 15`, it is started on a
#   three-row file whose third row is shorter than the first two, which fails
#   to load mid-file ("Grid file is not a rectangle") after two rows; after
#   3 s (looked for again every 0.5 s for up to 5 s more) the pane must show
#   the "File load error" message, then Escape dismisses it and Ctrl+Q
#   quits. It must exit 0 with no "LeakSanitizer" on stderr: the partial
#   grid is freed before the startup grid is filled. It runs with
#   ASAN_OPTIONS=detect_leaks=1 and an empty LSAN_OPTIONS over the caller's,
#   so a caller's detect_leaks=0 cannot make it pass, and in a tmux server
#   of its own, apart from the resize case's.
# Each orca run has a scratch XDG_CONFIG_HOME. Prints PASS or FAIL for each
# check and exits 0 when all pass, 1 when one fails, 2 on usage.
set -u
unset SHIM_MALLOC_MAX SHIM_REALLOC_MAX

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ] || [ ! -x "$1" ] ||
  { [ "$#" -ge 2 ] && [ ! -x "$2" ]; } ||
  { [ "$#" = 3 ] && [ ! -x "$3" ]; }; then
  echo "usage: $0 <cli> [<orca> [<debug-orca>]]" >&2
  exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
abs() { case $1 in /*) printf '%s\n' "$1" ;; *) printf '%s\n' "$PWD/$1" ;; esac; }
cli=$(abs "$1")
orca=
[ "$#" -ge 2 ] && orca=$(abs "$2")
dorca=
[ "$#" = 3 ] && dorca=$(abs "$3")
# No MIDI output: refuse an --alsa or --portmidi orca before any run.
for b in "$orca" "$dorca"; do
  [ -n "$b" ] || continue
  if ! command -v readelf >/dev/null 2>&1; then
    echo "$0: readelf (binutils) is needed to check that $b has no MIDI output" >&2
    exit 2
  fi
  if readelf -d "$b" 2>/dev/null | grep -q 'NEEDED.*\[lib\(asound\|portmidi\)'; then
    echo "$0: $b links libasound or libportmidi: build orca without --alsa or --portmidi" >&2
    exit 2
  fi
done
alsa_name=borca-realloc-shim-$$
tmp=$(mktemp -d "${TMPDIR:-/tmp}/borca-realloc-shim.XXXXXX") || exit 2
# Each tmux run has a server of its own, $sock-<run>: a new session on a
# server that kill-server is still shutting down fails ("server exited
# unexpectedly"). Each server's socket is added to socks as it is created,
# and cleanup kills exactly those. Each run's exit status is written to a
# .new file and renamed, so a waiting check never reads it half written.
sock=borca-realloc-shim-$$
socks=
cleanup() {
  for s in $socks; do tmux -L "$s" kill-server 2>/dev/null; done
  rm -rf "$tmp"
}
trap cleanup EXIT
mkdir "$tmp/cfg"
so=$tmp/shim.so
if ! ${CC:-cc} -O2 -shared -fPIC -o "$so" "$here/shim.c" -ldl; then
  echo "FAIL  realloc shim: cannot compile $here/shim.c"
  exit 1
fi

fails=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; fails=$((fails + 1)); }

# grid <rows> <cols> <file>: dots, with an adder (1A2) at the top left.
grid() {
  awk -v r="$1" -v c="$2" 'BEGIN {
    for (y = 0; y < r; y++) {
      s = ""
      for (x = 0; x < c; x++) s = s "."
      if (y == 0) s = "1A2" substr(s, 4)
      print s
    }
  }' >"$3"
}

grid 50 100 "$tmp/g100x50.orca"
grid 10 20 "$tmp/g20x10.orca"
status=0
LD_PRELOAD=$so SHIM_REALLOC_MAX=4096 "$cli" -t 1 "$tmp/g100x50.orca" \
  >/dev/null 2>"$tmp/c50.err" || status=$?
echo "cli, 100 x 50 file, realloc limit 4096: exit $status: $(cat "$tmp/c50.err")"
if [ "$status" = 1 ] &&
  grep -qxF 'File load error: Not enough memory for the grid.' "$tmp/c50.err"; then
  pass "cli refuses a grid it cannot allocate"
else
  [ "$status" -gt 128 ] && echo "      (signal $((status - 128)))"
  fail "cli refuses a grid it cannot allocate (exit 1 and the message)"
fi
status=0
LD_PRELOAD=$so SHIM_REALLOC_MAX=4096 "$cli" -t 1 "$tmp/g20x10.orca" \
  >"$tmp/c10.out" 2>&1 || status=$?
if [ "$status" = 0 ] && sed -n 2p "$tmp/c10.out" | grep -q '^\.3\.'; then
  pass "cli runs a 20 x 10 grid under the same limit"
else
  fail "cli runs a 20 x 10 grid under the same limit (exit $status)"
fi

if [ -n "$orca" ]; then
  status=0
  SHELL=/bin/sh TERM=xterm-256color timeout 10 script -qec \
    "env LD_PRELOAD='$so' SHIM_REALLOC_MAX=30000 XDG_CONFIG_HOME='$tmp/cfg' ZORCA_ALSA_CLIENT_NAME='$alsa_name' '$orca' --initial-size 200x200" \
    /dev/null </dev/null >"$tmp/startup.out" 2>&1 || status=$?
  # The message follows endwin's escape sequences on its line.
  if [ "$status" = 1 ] && tr -d '\r' <"$tmp/startup.out" |
    grep -aq 'Cannot start: not enough memory for the grid$'; then
    pass "orca refuses to start without a mark buffer"
  else
    fail "orca refuses to start without a mark buffer (exit $status)"
  fi

  status=0
  SHELL=/bin/sh TERM=xterm-256color timeout 10 script -qec \
    "env LD_PRELOAD='$so' SHIM_MALLOC_MAX=30000 XDG_CONFIG_HOME='$tmp/cfg' ZORCA_ALSA_CLIENT_NAME='$alsa_name' '$orca' --initial-size 200x200" \
    /dev/null </dev/null >"$tmp/fill.out" 2>&1 || status=$?
  echo "orca, 200x200, malloc limit 30000: exit $status"
  if [ "$status" = 1 ] && tr -d '\r' <"$tmp/fill.out" |
    grep -aq 'Cannot start: not enough memory for the grid$'; then
    pass "orca refuses to start without a grid"
  else
    [ "$status" -gt 128 ] && echo "      (signal $((status - 128)))"
    fail "orca refuses to start without a grid (exit $status)"
  fi

  # resize <limit>: sets before, after (the HUD's grid size), f_before,
  # f_after (its frame counter), running (after the two +) and rc.
  resize() {
    rs=$sock-r${1:-0}
    socks="$socks $rs"
    rm -f "$tmp/resize.rc"
    cat >"$tmp/resize.sh" <<RUN
#!/bin/sh
TERM=xterm-256color timeout --foreground 10 env LD_PRELOAD='$so' \
  SHIM_REALLOC_MAX='$1' XDG_CONFIG_HOME='$tmp/cfg' \
  ZORCA_ALSA_CLIENT_NAME='$alsa_name' '$orca' \
  --initial-size 1000x25 2>'$tmp/resize.err'
echo \$? >'$tmp/resize.rc.new' && mv '$tmp/resize.rc.new' '$tmp/resize.rc'
sleep 30
RUN
    tmux -L "$rs" -f /dev/null new-session -d -x 100 -y 30 -s r \
      "sh '$tmp/resize.sh'"
    sleep 2
    tmux -L "$rs" capture-pane -p -t r >"$tmp/before.txt"
    tmux -L "$rs" send-keys -t r +
    sleep 1
    tmux -L "$rs" send-keys -t r +
    sleep 1.5
    tmux -L "$rs" capture-pane -p -t r >"$tmp/after.txt"
    running=no
    [ -f "$tmp/resize.rc" ] || running=yes
    before=$(grep -o '1000x[0-9]*' "$tmp/before.txt" | head -n 1)
    after=$(grep -o '1000x[0-9]*' "$tmp/after.txt" | head -n 1)
    f_before=$(grep -o '[0-9]*f ' "$tmp/before.txt" | head -n 1)
    f_after=$(grep -o '[0-9]*f ' "$tmp/after.txt" | head -n 1)
    tmux -L "$rs" send-keys -t r C-q
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      [ -f "$tmp/resize.rc" ] && break
      sleep 1
    done
    rc=$(cat "$tmp/resize.rc" 2>/dev/null || echo none)
    tmux -L "$rs" kill-server 2>/dev/null
    echo "orca, 1000x25, + + then Ctrl+Q, realloc limit ${1:-none}:" \
      "grid $before -> $after, frame $f_before-> $f_after, running $running, exit $rc"
  }
  resize ''
  if [ "$before" = 1000x25 ] && [ "$after" = 1000x41 ] && [ "$rc" = 0 ]; then
    pass "control: + + grow the grid without a limit"
  else
    fail "control: + + grow the grid without a limit"
  fi
  resize 30000
  if [ "$running" = yes ] && [ "$before" = 1000x25 ] &&
    [ "$after" = 1000x25 ] && [ -n "$f_after" ] &&
    [ "$f_before" != "$f_after" ] && [ "$rc" = 0 ]; then
    pass "orca keeps its grid and plays on when it cannot grow it"
  else
    fail "orca keeps its grid and plays on when it cannot grow it"
  fi
fi

if [ -n "$dorca" ]; then
  if ! grep -aq __asan_init "$dorca"; then
    fail "orca frees a partial grid after a failed load: $dorca is not an ASan build (no __asan_init)"
  else
    printf '%s\n' '......' '......' '...' >"$tmp/short.orca"
    rm -f "$tmp/leak.rc"
    cat >"$tmp/leak.sh" <<RUN
#!/bin/sh
TERM=xterm-256color timeout --foreground 15 env ASAN_OPTIONS=detect_leaks=1 \
  LSAN_OPTIONS= XDG_CONFIG_HOME='$tmp/cfg' ZORCA_ALSA_CLIENT_NAME='$alsa_name' \
  '$dorca' '$tmp/short.orca' 2>'$tmp/leak.err'
echo \$? >'$tmp/leak.rc.new' && mv '$tmp/leak.rc.new' '$tmp/leak.rc'
sleep 30
RUN
    socks="$socks $sock-leak"
    tmux -L "$sock-leak" -f /dev/null new-session -d -x 100 -y 30 -s l \
      "sh '$tmp/leak.sh'"
    sleep 3
    # A loaded device can start the debug build later: look again every
    # 0.5 s, for up to 5 s more.
    seen=no
    for _ in 1 2 3 4 5 6 7 8 9 10 11; do
      tmux -L "$sock-leak" capture-pane -p -t l >"$tmp/leak.txt" 2>/dev/null
      grep -q 'File load error' "$tmp/leak.txt" && { seen=yes; break; }
      sleep 0.5
    done
    tmux -L "$sock-leak" send-keys -t l Escape
    sleep 1
    tmux -L "$sock-leak" send-keys -t l C-q
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
      [ -f "$tmp/leak.rc" ] && break
      sleep 1
    done
    rc=$(cat "$tmp/leak.rc" 2>/dev/null || echo none)
    tmux -L "$sock-leak" kill-server 2>/dev/null
    lsan=none
    grep -qs LeakSanitizer "$tmp/leak.err" &&
      lsan=$(grep -m 1 'SUMMARY' "$tmp/leak.err" || echo 'a report')
    echo "orca -d, rows of 6, 6 and 3 columns, Escape then Ctrl+Q:" \
      "load error shown $seen, exit $rc, LeakSanitizer $lsan"
    if [ "$seen" = yes ] && [ "$rc" = 0 ] && [ "$lsan" = none ]; then
      pass "orca frees a partial grid after a failed load"
    else
      fail "orca frees a partial grid after a failed load"
    fi
  fi
fi

echo "realloc shim check: $fails failed"
[ "$fails" = 0 ]
