#!/bin/sh
# The ALSA name-guard device check (since v3.0.0): an --alsa orca refuses a
# client name another client holds, scans the sequencer a second time after
# it takes the name and before it creates any port, and reports a failed scan
# as a scan error, never as a free name. The unit test of alsa_name_scan sees
# only the pure helper (midi_in.h); this check runs the real binary, so it
# sees the second scan's place in alsa_ctx_open and the adapter
# (alsa_name_next_client) passing a scan error through. It is a development
# check: CI shellchecks it and compiles shim.c, but cannot run it, since the
# runners have no sequencer.
#
#   tests/alsa-guard/devcheck.sh <orca>
#
# <orca> is a release --alsa build (`./tool build --alsa orca`): a debug
# build links ASan, which refuses LD_PRELOAD. The script builds shim.c
# (beside it) into an LD_PRELOAD library in a scratch directory and runs
# <orca> on a scratch three-line patch, once per run, with TERM=xterm under
# util-linux script (so the TUI has a terminal) and `timeout --foreground
# -k 1 <s>` (3 s; 6 s for D1's first instance), which ends a run that
# starts with SIGTERM, so a run that gets past the guard exits 124. The
# shim, by BORCA_GUARD_SHIM, makes a lost race deterministic (hold: a
# second client of this process takes the name just before orca's rename)
# or fails the scans (eio: every scan; eio2: only the one after the
# rename), and prints `pid <n> ports created <k>` on exit. Checks, one PASS
# or FAIL line each:
#   D1  no shim: once a first instance holds the name (polled for up to
#       3 s), a second instance under it exits 1 with "Cannot start: an
#       ALSA client named '<name>' is already running ...", one client has
#       the name before and after it, and the first runs until the timeout
#       (124).
#   D2  hold: exit 1 with the same message, no port created, and no client
#       of the name 1 s into the run: the second scan refused the name
#       before any port.
#   D4  eio: exit 1 with "Cannot start: the scan for an ALSA client named
#       '<name>' failed: Input/output error", no port created.
#   D5  eio2: the same as D4 for the second scan, and no client of the name
#       1 s into the run.
#   Live  the live bOrca client's lines in /proc/asound/seq/clients (its
#       Client line, its Port lines and their "Connecting To:" and
#       "Connected From:" lines; not its pool counters, which move while
#       it plays) are the same before and after, and no guard client is
#       left.
# Device rules: every client name is guard-<pid>-<check>, never bOrca;
# every run gets a fresh scratch XDG_CONFIG_HOME, and the patch is a
# scratch file; the live engine's client and borca-tty.service are only
# observed (no systemctl, no aconnect). Every gcc and orca runs under
# `nice -n 19 taskset -c 0-2`, so core 3 stays free; run the script the
# same way on the uConsole. It takes about 10 s. An interrupt stops the
# runs in the background before the script exits.
#
# Needs script, timeout, gcc, readelf, taskset and a readable
# /proc/asound/seq/clients. Exit status: 0 when every check passes, 1 when
# one fails, 2 on usage, a missing tool, a binary that is not a release
# --alsa build, a guard client already present, a shim that does not build
# or a scratch directory that cannot be made.
set -u

usage() {
  echo "usage: $0 <release --alsa orca>" >&2
  exit 2
}
die() {
  echo "$0: $*" >&2
  exit 2
}

[ "$#" = 1 ] || usage
orca=$1
if [ ! -f "$orca" ] || [ ! -x "$orca" ]; then
  die "$orca is not an executable file"
fi
case $orca in
  /*) ;;
  *) orca="$PWD/$orca" ;;
esac
here=$(cd "$(dirname "$0")" && pwd)

for t in script timeout gcc readelf taskset; do
  command -v "$t" >/dev/null 2>&1 || die "$t is not on PATH"
done
clients=/proc/asound/seq/clients
[ -r "$clients" ] || die "$clients is not readable (no ALSA sequencer?)"
needed=$(readelf -d "$orca" 2>/dev/null | grep '(NEEDED)')
case $needed in
  *libasound.so*) ;;
  *) die "$orca is not an --alsa build: readelf -d lists no libasound.so NEEDED" ;;
esac
case $needed in
  *libasan*) die "$orca is a debug build: it links libasan, and ASan refuses LD_PRELOAD (build a release --alsa orca)" ;;
esac

base="guard-$$"
if grep -qF "\"$base-" "$clients"; then
  die "a client named $base-... already exists"
fi

# The pids of the runs in the background, which an interrupt stops: each
# is a subshell whose child is script, which passes SIGTERM on to the run.
bg_pids=
stop_runs() {
  for p in $bg_pids; do
    if command -v pkill >/dev/null 2>&1; then
      pkill -TERM -P "$p" 2>/dev/null
    fi
    kill "$p" 2>/dev/null
  done
}
tmp=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$tmp"' EXIT
trap 'stop_runs; exit 130' INT TERM
case "$orca$tmp" in
  *"'"*) die "the orca and scratch paths must not contain a single quote" ;;
esac

if ! nice -n 19 taskset -c 0-2 gcc -shared -fPIC -o "$tmp/shim.so" \
  "$here/shim.c" -ldl -lasound >"$tmp/shim-build.log" 2>&1; then
  cat "$tmp/shim-build.log" >&2
  die "the shim did not build"
fi
printf '...\n.D.\n...\n' >"$tmp/patch.orca"

# script runs its command with $SHELL, so it is pinned to sh.
SHELL=/bin/sh
export SHELL

# xdg: prints a fresh scratch XDG_CONFIG_HOME. Called by the parent shell,
# as `x=$(xdg) || die ...`, so a failure stops the script with exit 2.
xdg() {
  mktemp -d "$tmp/xdg.XXXXXX"
}

# run <check> <shim mode, or - for none> <log> <xdg> [<seconds>]: one orca,
# under the name guard-<pid>-<check>, the XDG_CONFIG_HOME xdg and a timeout
# of seconds (default 3); writes its output to the log with each \r
# removed, and prints its exit status.
run() {
  r_env="TERM=xterm XDG_CONFIG_HOME='$4' ZORCA_ALSA_CLIENT_NAME=$base-$1"
  if [ "$2" != - ]; then
    r_env="$r_env LD_PRELOAD='$tmp/shim.so' BORCA_GUARD_SHIM=$2"
  fi
  nice -n 19 taskset -c 0-2 script -qec \
    "timeout --foreground -k 1 ${5-3} env $r_env '$orca' '$tmp/patch.orca'" \
    /dev/null >"$3.raw" 2>&1
  r_status=$?
  tr -d '\r' <"$3.raw" >"$3"
  echo "$r_status"
}

# count <name>: the number of clients named exactly name.
count() {
  grep -cF "\"$1\"" "$clients"
}

# live: the live bOrca client's Client line, Port lines and their
# "Connecting To:" and "Connected From:" lines.
live() {
  awk '/^Client / { in_live = ($0 ~ /^Client +[0-9]+ : "(bOrca|zOrca)" /) }
    in_live && (/^Client / || /^  Port / || /Connect(ing To|ed From):/) { print }' "$clients"
}

n_checks=0
n_failed=0
# result <ok: 1 or 0> <check> <details>
result() {
  n_checks=$((n_checks + 1))
  if [ "$1" = 1 ]; then
    printf 'PASS  %s (%s)\n' "$2" "$3"
  else
    printf 'FAIL  %s (%s)\n' "$2" "$3"
    n_failed=$((n_failed + 1))
  fi
}

# has <log> <line>: the log holds the line, exactly.
has() {
  grep -qxF -- "$2" "$1"
}

# ports <log>: the shim's count of ports created, or ? when it printed none.
ports() {
  p_line=$(grep -Eo 'ports created [0-9]+$' "$1" | tail -n 1)
  echo "${p_line:-ports created ?}" | sed 's/^ports created //'
}

live_before=$(live)

taken() {
  echo "Cannot start: an ALSA client named '$base-$1' is already running (set ZORCA_ALSA_CLIENT_NAME to run another instance)"
}
scan_failed() {
  echo "Cannot start: the scan for an ALSA client named '$base-$1' failed: Input/output error"
}

# D1: an ordinary duplicate, the second instance started once the first
# holds the name (polled every 0.1 s for up to 3 s of its 6).
x1=$(xdg) || die "mktemp failed"
x2=$(xdg) || die "mktemp failed"
run d1 - "$tmp/d1-first.log" "$x1" 6 >"$tmp/d1-first.rc" &
first=$!
bg_pids=$first
i=0
d1_during=$(count "$base-d1")
while [ "$d1_during" = 0 ] && [ "$i" -lt 30 ]; do
  sleep 0.1
  i=$((i + 1))
  d1_during=$(count "$base-d1")
done
d1_rc2=$(run d1 - "$tmp/d1-second.log" "$x2")
d1_after=$(count "$base-d1")
wait "$first"
bg_pids=
d1_rc1=$(cat "$tmp/d1-first.rc")
ok=0
if [ "$d1_during" = 1 ] && [ "$d1_rc2" = 1 ] &&
  has "$tmp/d1-second.log" "$(taken d1)" && [ "$d1_after" = 1 ] &&
  [ "$d1_rc1" = 124 ]; then
  ok=1
fi
result "$ok" "D1 ordinary duplicate, no shim" \
  "clients before the second $d1_during (after $i polls); second rc $d1_rc2$(has "$tmp/d1-second.log" "$(taken d1)" && echo ', refused by name'); clients after it $d1_after; first rc $d1_rc1"

# guarded <check> <mode> <message> <need no client 1 s in: 1 or 0>: one
# run with the shim, which must exit 1 with the message and create no port.
guarded() {
  g_xdg=$(xdg) || die "mktemp failed"
  g_log="$tmp/$1.log"
  run "$1" "$2" "$g_log" "$g_xdg" >"$tmp/$1.rc" &
  g_pid=$!
  bg_pids=$g_pid
  sleep 1
  g_during=$(count "$base-$1")
  wait "$g_pid"
  bg_pids=
  g_rc=$(cat "$tmp/$1.rc")
  g_after=$(count "$base-$1")
  g_ports=$(ports "$g_log")
  g_msg=no
  has "$g_log" "$3" && g_msg=yes
  g_ok=0
  if [ "$g_rc" = 1 ] && [ "$g_msg" = yes ] && [ "$g_ports" = 0 ]; then
    g_ok=1
    if [ "$4" = 1 ] && [ "$g_during" != 0 ]; then
      g_ok=0
    fi
  fi
}

guarded d2 hold "$(taken d2)" 1
result "$g_ok" "D2 hold, a lost race" \
  "rc $g_rc; message $g_msg; ports created $g_ports; clients at 1 s $g_during, after $g_after"
guarded d4 eio "$(scan_failed d4)" 0
result "$g_ok" "D4 eio, the first scan fails" \
  "rc $g_rc; message $g_msg; ports created $g_ports; clients at 1 s $g_during, after $g_after"
guarded d5 eio2 "$(scan_failed d5)" 1
result "$g_ok" "D5 eio2, the second scan fails" \
  "rc $g_rc; message $g_msg; ports created $g_ports; clients at 1 s $g_during, after $g_after"

live_after=$(live)
left=$(grep -cF "\"$base-" "$clients")
ok=0
if [ "$live_before" = "$live_after" ] && [ "$left" = 0 ]; then
  ok=1
fi
result "$ok" "Live, the bOrca client untouched" \
  "before: $(echo "${live_before:-none}" | tr -s ' \n' '  '); after: $(echo "${live_after:-none}" | tr -s ' \n' '  '); guard clients left $left"

# On a failure, the runs' guard lines, for the log, or the last lines of a
# run that printed none.
if [ "$n_failed" != 0 ]; then
  for f in d1-first d1-second d2 d4 d5; do
    printf -- '--- %s:\n' "$f"
    if ! grep -E 'Cannot|shim:|ports created' "$tmp/$f.log" | head -n 4 | grep .; then
      tail -n 4 "$tmp/$f.log"
    fi
  done
fi
echo "alsa guard check: $n_checks checks, $n_failed failed"
[ "$n_failed" = 0 ]
