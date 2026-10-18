#!/bin/sh
# The ALSA held-note-off device check: a note-off the sequencer refuses is sent
# again, so the note does not hang. A refused MIDI send never sleeps, and a lost
# note-on is only a missed note, but a lost note-off leaves the note sounding on
# the synth until that channel and note is played again. The unit tests cover
# the held list (pending_offs_*, midi_in.h); no test links tui_main.c, so this
# check covers the wiring on a real binary and a real sequencer. It is a
# development check: CI only shellchecks it and compiles recv.c, since the
# runners have no /dev/snd/seq.
#
#   tests/alsa-noteoff/devcheck.sh <fixed orca> [<baseline orca>]
#
# Each is an --alsa build. The fixed one is the build under test; build it with
# CFLAGS_EXTRA=-DZORCA_DEBUG_COUNTERS to have the check also read the held and
# resent counts the orca prints on exit. The optional baseline is the same
# source without the held list (a build of 6788188 or 50be43f): the negative
# control, which must hang the note, or the check proves nothing.
#
# A scratch directory (mktemp -d) holds recv, built from recv.c beside this
# script with `cc -std=c99 -O2 ... -lasound`, an empty xdg/ for
# XDG_CONFIG_HOME, and noteoff.orca, a 7x3 grid whose D8 bangs :03C every 8th
# tick: one note a second at the default 120 BPM, off a tick (125 ms) after on.
# Each run starts `<orca> noteoff.orca` in a 100 x 30 pane of its own tmux
# server (`tmux -L dev-noteoff-<pid> -f /dev/null`, SHELL=/bin/sh) as the client
# dev-noteoff-<pid> (ZORCA_ALSA_CLIENT_NAME). Once the client is up it starts
# the receiver, dev-noteoff-<pid>-recv, subscribed to dev-noteoff-<pid>:0 with
# an input pool of 6 events, and a healthy one, dev-noteoff-<pid>-recv2, with
# a pool of 500 that always reads. On the first note-on the first receiver
# fills its own pool and stops reading for 600 ms, so the note-off, 125 ms
# after its on, meets a full pool and the write is refused (-EAGAIN) although
# the healthy receiver got it; then it reads on, 5 s in all. Ctrl+Q ends the
# orca.
# Checks, one PASS or FAIL line each:
#   fixed     the first note's off arrives 450 ms to 1.5 s after its on and
#             within 250 ms of the receiver reading again (the retry, once the
#             pool drained), no on follows an on of the same channel and note
#             without an off, every on has its off, and the healthy receiver
#             sees that first note-off at most 25 times (a retry repeats it to
#             every subscriber; the backoff keeps it to about a dozen a second,
#             where a retry on every 2 ms wake would repeat it hundreds of
#             times); with
#             a counters build, the orca's exit line says N note-offs held and
#             N sent again, N >= 1; and the orca exits with 0 (a sanitized
#             build fails here on a memory or undefined-behavior report).
#   baseline  (only when given) the first note hangs: the receiver sees its
#             on, then the next on, and no off in between, and the healthy
#             receiver sees that note-off once. This is the control.
#   live      the live zOrca client's lines in /proc/asound/seq/clients are the
#             same before and after, and no dev-noteoff-<pid> client is left.
# Device rules: every client is dev-noteoff-<pid>[-recv], never bOrca or zOrca, with a
# scratch XDG_CONFIG_HOME; the live engine and borca-tty.service are only
# observed (no aconnect, systemctl or sudo). Everything runs under `nice -n 19
# taskset -c 0-2`, so core 3 stays free; run the script the same way. It
# signals only the processes it started, and kills only its own tmux server.
# About 15 s a binary. Needs tmux, cc, pgrep, readlink, readelf, taskset, the
# ALSA headers (libasound2-dev) and a readable /proc/asound/seq/clients. Exit
# status: 0 when every check passes, 1 when one fails, 2 on usage, a missing
# tool, a binary that is not an --alsa build, a dev-noteoff-<pid> client
# already present, or a scratch directory, file or tmux server that cannot be
# made; 129 on SIGHUP, 130 on SIGINT and 143 on SIGTERM.
set -u

usage() {
  [ "$#" = 0 ] || echo "$0: $*" >&2
  echo "usage: $0 <--alsa orca> [<baseline --alsa orca>]" >&2
  exit 2
}
die() {
  echo "$0: $*" >&2
  exit 2
}

[ "$#" = 1 ] || [ "$#" = 2 ] || usage
fixed=$1
baseline=${2:-}
for bin in "$fixed" "$baseline"; do
  [ -z "$bin" ] && continue
  if [ ! -f "$bin" ] || [ ! -x "$bin" ]; then
    usage "$bin is not an executable file"
  fi
done
case $fixed in
  /*) ;;
  *) fixed=$PWD/$fixed ;;
esac
case $baseline in
  '' | /*) ;;
  *) baseline=$PWD/$baseline ;;
esac
here=$(cd "$(dirname "$0")" && pwd)

for t in tmux cc pgrep readlink readelf taskset; do
  command -v "$t" >/dev/null 2>&1 || die "$t is not on PATH"
done
clients=/proc/asound/seq/clients
[ -r "$clients" ] || die "$clients is not readable (no ALSA sequencer?)"
for bin in "$fixed" "$baseline"; do
  [ -z "$bin" ] && continue
  case $(readelf -d "$bin" 2>/dev/null | grep '(NEEDED)') in
    *libasound.so*) ;;
    *) die "$bin is not an --alsa build: readelf -d lists no libasound.so NEEDED" ;;
  esac
done
name=dev-noteoff-$$
recv_name=$name-recv
recv2_name=$name-recv2
sock=$name

present() {
  grep -qE "^Client +[0-9]+ : \"$name(-recv2?)?\"" "$clients"
}
present && die "a client named $name, $recv_name or $recv2_name already exists"

tm() {
  tmux -L "$sock" -f /dev/null "$@"
}

tmp=
started=0
rp=
rp2=
# shellcheck disable=SC2317 # run by the EXIT trap below
cleanup() {
  trap '' INT TERM HUP
  for p in $rp $rp2; do
    if kill -0 "$p" 2>/dev/null; then
      kill -s TERM "$p" 2>/dev/null
      wait "$p" 2>/dev/null
    fi
  done
  if [ "$started" = 1 ]; then
    tm kill-server 2>/dev/null
  fi
  if [ -n "$tmp" ]; then
    rm -rf "$tmp"
  fi
}
trap 'cleanup' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
tmp=$(mktemp -d) || die "mktemp failed"
case "$fixed$baseline$tmp" in
  *"'"*) die "the orca and scratch paths must not contain a single quote" ;;
esac
nice -n 19 taskset -c 0-2 cc -std=c99 -O2 -Wall -Wextra \
  -o "$tmp/recv" "$here/recv.c" -lasound >"$tmp/cc.txt" 2>&1 ||
  die "$here/recv.c does not build: $(head -n 3 "$tmp/cc.txt")"
mkdir "$tmp/xdg" || die "mkdir $tmp/xdg failed"
printf '.D8....\n..:03C.\n.......\n' >"$tmp/noteoff.orca" ||
  die "cannot write $tmp/noteoff.orca"

# cs: centiseconds since boot, from /proc/uptime.
cs() {
  read -r c_up _ </proc/uptime
  c_frac=${c_up#*.}
  echo $((${c_up%.*} * 100 + ${c_frac#0}))
}
# up: our orca's client is in the sequencer with its port 1 (MIDI in).
up() {
  awk -v q="\"$name\"" '
    /^Client / { in_c = ($4 == q); next }
    in_c && /^  Port +1 / { found = 1 }
    END { exit !found }' "$clients"
}
# live: the live zOrca client's Client line, Port lines and their
# "Connecting To:" and "Connected From:" lines.
live() {
  awk '/^Client / { in_live = ($0 ~ /^Client +[0-9]+ : "(bOrca|zOrca)" /) }
    in_live && (/^Client / || /^  Port / || /Connect(ing To|ed From):/) { print }' "$clients"
}
# gone: waits up to 15 s for our clients to leave the sequencer.
gone() {
  g_i=0
  while present && [ "$g_i" -lt 150 ]; do
    sleep 0.1
    g_i=$((g_i + 1))
  done
}
# analyse <recv.log> <recv2.log>: prints "<ons> <offs> <ons after an on>
# <stray offs> <first note: delay in us from its on to its off, or hung, or
# none> <us from the receiver reading again to that off, or none> <times the
# healthy receiver saw that note-off, or none>".
analyse() {
  awk '
    FILENAME == ARGV[1] && $1 == "#" && $3 == "reading" && $4 == "again" {
      t_resume = $2
    }
    FILENAME == ARGV[1] && ($2 == "on" || $2 == "off") {
      key = $3 ":" $4
      if ($2 == "on") {
        n_on++
        if (state[key] == 1) double_on++
        state[key] = 1
        if (!have_first) { have_first = 1; first_key = key; t_on = $1; next }
      } else {
        n_off++
        if (state[key] == 1) state[key] = 0; else stray++
      }
      if (have_first && !decided && key == first_key) {
        decided = 1
        if ($2 == "off") {
          delay = sprintf("%d", $1 - t_on)
          if (t_resume != "") resume = sprintf("%d", $1 - t_resume)
        } else delay = "hung"
      }
    }
    # The healthy receiver: its copy of the same first note (its on within
    # 50 ms before the first receiver saw it, or later), and the note-offs it
    # saw for that note before the next on of the note.
    FILENAME == ARGV[2] && ($2 == "on" || $2 == "off") && have_first {
      if (($3 ":" $4) != first_key) next
      if ($2 == "on") {
        if (!seen_on2) { if ($1 >= t_on - 50000) seen_on2 = 1 }
        else done2 = 1
      } else if (seen_on2 && !done2) offs2++
    }
    END {
      if (!have_first) delay = "none"
      else if (!decided) delay = "hung"
      if (resume == "") resume = "none"
      seen2 = (!have_first || !seen_on2) ? "none" : sprintf("%d", offs2)
      print n_on + 0, n_off + 0, double_on + 0, stray + 0, delay, resume, seen2
    }' "$1" "$2"
}

n_failed=0
# result <ok: 1 or 0> <check> <detail>
result() {
  if [ "$1" = 1 ]; then
    printf 'PASS %s: %s\n' "$2" "$3"
  else
    printf 'FAIL %s: %s\n' "$2" "$3"
    n_failed=$((n_failed + 1))
  fi
}

# one_run <label> <binary>: one run into $tmp/<label>; sets $why when it could
# not run; leaves recv.log, err and $rc.
one_run() {
  run=$tmp/$1
  why=
  rc=none
  mkdir "$run" || die "mkdir $run failed"
  : >"$run/recv.log"
  : >"$run/recv2.log"
  started=1
  tm new-session -d -s note -x 100 -y 30 \
    "env XDG_CONFIG_HOME='$tmp/xdg' ZORCA_ALSA_CLIENT_NAME=$name nice -n 19 taskset -c 0-2 '$2' '$tmp/noteoff.orca' 2>'$run/err'; echo \$? >'$run/rc'" ||
    die "tmux could not start the pane"
  t0=$(cs)
  while ! up; do
    if [ -s "$run/rc" ] || [ $(($(cs) - t0)) -ge 1000 ]; then
      why="no client named '$name' with its MIDI in port within 10 s"
      break
    fi
    sleep 0.1
  done
  if [ -z "$why" ]; then
    nice -n 19 taskset -c 0-2 "$tmp/recv" "$recv2_name" "$name" 500 0 5000 reader \
      >"$run/recv2.log" 2>"$run/recv2.err" &
    rp2=$!
    nice -n 19 taskset -c 0-2 "$tmp/recv" "$recv_name" "$name" 6 600 5000 \
      >"$run/recv.log" 2>"$run/recv.err" &
    rp=$!
    wait "$rp"
    rp=
    wait "$rp2"
    rp2=
  fi
  tm send-keys -t note C-q 2>/dev/null
  t0=$(cs)
  while [ ! -s "$run/rc" ] && [ $(($(cs) - t0)) -lt 1500 ]; do
    sleep 0.1
  done
  if [ -s "$run/rc" ]; then
    rc=$(cat "$run/rc")
  fi
  tm kill-server 2>/dev/null
  started=0
  gone
}

live_before=$(live)

# tmux runs the pane's command with default-shell, which is $SHELL.
SHELL=/bin/sh
export SHELL

one_run fixed "$fixed"
if [ -n "$why" ]; then
  result 0 fixed "not run: $why"
else
  # shellcheck disable=SC2046 # analyse prints seven words, split on purpose
  set -- $(analyse "$run/recv.log" "$run/recv2.log")
  ons=$1 offs=$2 dbl=$3 stray=$4 delay=$5 resume=$6 seen2=$7
  filler=$(sed -n 's/^# [0-9]* filled the pool: //p' "$run/recv.log" | head -n 1)
  counters=$(sed -n 's/.* \([0-9][0-9]*\) note-offs held for a retry, \([0-9][0-9]*\) sent again.*/\1 \2/p' "$run/err" | head -n 1)
  ok=0
  case $delay in
    none | hung) ;;
    *)
      if [ "$ons" -ge 3 ] && [ "$ons" = "$offs" ] && [ "$dbl" = 0 ] &&
        [ "$delay" -ge 450000 ] && [ "$delay" -le 1500000 ] && [ "$rc" = 0 ] &&
        [ "$resume" != none ] && [ "$resume" -ge 0 ] && [ "$resume" -le 250000 ] &&
        [ "$seen2" != none ] && [ "$seen2" -ge 1 ] && [ "$seen2" -le 25 ]; then
        ok=1
      fi
      ;;
  esac
  note="counters: not a ZORCA_DEBUG_COUNTERS build"
  if [ -n "$counters" ]; then
    # shellcheck disable=SC2086 # two words, split on purpose
    set -- $counters
    note="the orca held $1 note-off(s) and sent $2 again"
    if [ "$1" -lt 1 ] || [ "$1" != "$2" ]; then
      ok=0
    fi
  fi
  result "$ok" fixed "$ons note-ons, $offs note-offs, $dbl on after an on, $stray stray off(s); the first note's off came ${delay} us after its on (450000 to 1500000 wanted) and ${resume} us after the receiver read again (250000 or less wanted); the healthy receiver saw it ${seen2} time(s) (1 to 25 wanted); $note; receiver: $filler; orca exit $rc"
fi

if [ -n "$baseline" ]; then
  one_run baseline "$baseline"
  if [ -n "$why" ]; then
    result 0 baseline "not run: $why"
  else
    # shellcheck disable=SC2046 # analyse prints seven words, split on purpose
    set -- $(analyse "$run/recv.log" "$run/recv2.log")
    ok=0
    [ "$5" = hung ] && [ "$7" = 1 ] && ok=1
    result "$ok" baseline "control: $1 note-ons, $2 note-offs, $3 on after an on; the first note's off: $5 (hung wanted: without the held list the note hangs); the healthy receiver saw it $7 time(s) (once wanted)"
  fi
else
  echo "NOTE baseline: none given, so the check shows the fixed build does not hang but not that an unfixed build would"
fi

live_after=$(live)
if [ "$live_before" = "$live_after" ] && ! present; then
  result 1 live "the live zOrca client's lines are the same before and after, and no $name client is left"
else
  result 0 live "the live zOrca client's lines changed, or a $name client is left"
fi

[ "$n_failed" = 0 ] || exit 1
exit 0
