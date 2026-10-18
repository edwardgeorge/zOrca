#!/bin/sh
# The ALSA clock-follow device check:
# an --alsa orca following an external MIDI clock survives a stall of its
# own. Within the client's 500-event input pool the queued pulses are kept
# and it lands back on the master's bar; past the pool the kernel flushes the
# input FIFO, the reader passes an overflow, and it keeps following behind
# the master instead of stopping at "ext lost". No test links tui_main.c's
# reader (alsa_ctx_open's pool call, tui_input_drain's -ENOSPC event), so
# this check covers them on a real binary. It is a development check: CI
# only shellchecks it and compiles its two C sources, since the runners have
# no /dev/snd/seq.
#
#   tests/alsa-clock/devcheck.sh <orca>
#
# <orca> is any --alsa build: release, -d or `make appliance`. A scratch
# directory (mktemp -d) holds master and recv, built from master.c and
# recv.c beside this script with `cc -std=c99 -O2 ... -lasound`;
# xdg/orca.conf, `clock_source = external` and `alsa_input_device =
# dev-clock-<pid>-master:clock`; and clock.orca, a 7x3 grid whose D1 bangs
# :03C every tick, one note per tick. Each stall run starts `<orca>
# clock.orca` in a 100 x 30 pane of its own tmux server (`tmux -L
# dev-clock-<pid> -f /dev/null`, SHELL=/bin/sh) as the client
# dev-clock-<pid> (ZORCA_ALSA_CLIENT_NAME), with XDG_CONFIG_HOME=<scratch>/xdg
# and its stderr in the scratch directory; once the client and its ports
# are up it starts the receiver, dev-clock-<pid>-recv, subscribed to
# dev-clock-<pid>:0 with a 2000-event pool, and the master,
# dev-clock-<pid>-master, which sends FA 1.5 s later and then F8s at 120 BPM
# (48 a second, one step = 6 pulses = 125 ms) for 3 s + the stall + 5 s.
# 3 s after the FA it stops the orca with SIGSTOP for the stall, then
# resumes it with SIGCONT. Until the master's last F8 it reads the HUD every
# 0.05 s with `tmux capture-pane -p` (its "ext ..." field). Then Ctrl+Q
# quits the orca. The master and the receiver log CLOCK_MONOTONIC
# microseconds: the offset of a note is the index of the master's last F8
# sent at or before it, minus 6 x the note's index (note n is tick n), and a
# burst is a run of note-ons less than 40 ms apart (a step is 125 ms).
# Checks, one PASS or FAIL line each:
#   S1  a 10 s stall, inside the pool: no "ext lost" on the HUD while the
#       master runs; exactly one burst, of 70 to 85 note-ons (80 missed
#       steps); and every note in the master's last 3 s (2 s after the
#       resume onward, at least 16 of them) at an offset of -1 to 1: back on
#       the master's bar. Without the pool call it follows 480 pulses behind
#       with no burst; with no overflow event and no pool, it stops at
#       "ext lost".
#   S2  an 11.1 s stall, past the pool: the client's input pool shows alloc
#       failures (the flush happened), no "ext lost" while the master runs,
#       no burst, and every note in the master's last 3 s (at least 16) at
#       an offset of 6 or more that is not a multiple of 6: it keeps
#       following, behind by the flushed pulses (about 533). Without the
#       -ENOSPC overflow event it stops at "ext lost"; with a pool of 2000 it
#       lands on the bar (offset 0).
#   Live  the live zOrca client's lines in /proc/asound/seq/clients (its
#       Client line, its Port lines and their "Connecting To:" and
#       "Connected From:" lines) are the same before and after, or absent
#       both times, and no dev-clock-<pid> client is left:
#       tests/alsa-guard/devcheck.sh's rule.
# Each S line also gives the HUD's readings, the burst, the offsets, the
# input pool (size, peak cells, alloc failures) and the orca's exit status.
# Other traffic to the client during a stall takes pool cells too (port
# changes of other clients, through System Announce), so a busy sequencer
# can push the 10 s stall past the pool; S1's pool figures show it.
# Device rules: every client is dev-clock-<pid>[-master|-recv], never bOrca or zOrca,
# and the config a scratch XDG_CONFIG_HOME; the live engine's client and
# zorca-tty.service are only observed (the script reads
# /proc/asound/seq/clients and calls no aconnect, systemctl or sudo). cc,
# the orca, the master and the receiver run under `nice -n 19 taskset -c
# 0-2`, so core 3 stays free; run the script the same way on the uConsole.
# It signals only the orca it started (the pane shell's child, running the
# named binary), the master and receiver it started, by PID, and kills only
# its own tmux server. On exit it resumes and ends them, kills its tmux
# server and removes the scratch directory, ignoring further INT, TERM and
# HUP meanwhile. It takes about 50 s.
#
# Needs tmux, cc, pgrep, readlink, readelf, taskset, the ALSA headers
# (libasound2-dev) and a readable /proc/asound/seq/clients. Exit status: 0
# when every check passes, 1 when one fails, 2 on usage (no argument, or one
# that is not an executable file), a missing tool, a binary that is not an
# --alsa build (no libasound.so NEEDED), a dev-clock-<pid> client already
# present, a master or receiver that does not build, or a scratch directory,
# scratch file or tmux server that cannot be made; 129 on SIGHUP, 130 on
# SIGINT and 143 on SIGTERM.
set -u

usage() {
  [ "$#" = 0 ] || echo "$0: $*" >&2
  echo "usage: $0 <--alsa orca>" >&2
  exit 2
}
die() {
  echo "$0: $*" >&2
  exit 2
}

[ "$#" = 1 ] || usage
orca=$1
if [ ! -f "$orca" ] || [ ! -x "$orca" ]; then
  usage "$orca is not an executable file"
fi
case $orca in
  /*) ;;
  *) orca=$PWD/$orca ;;
esac
here=$(cd "$(dirname "$0")" && pwd)

for t in tmux cc pgrep readlink readelf taskset; do
  command -v "$t" >/dev/null 2>&1 || die "$t is not on PATH"
done
clients=/proc/asound/seq/clients
[ -r "$clients" ] || die "$clients is not readable (no ALSA sequencer?)"
case $(readelf -d "$orca" 2>/dev/null | grep '(NEEDED)') in
  *libasound.so*) ;;
  *) die "$orca is not an --alsa build: readelf -d lists no libasound.so NEEDED" ;;
esac
orca_exe=$(readlink -f "$orca") || die "readlink -f $orca failed"
name=dev-clock-$$
master_name=$name-master
recv_name=$name-recv
sock=$name

# present: a client of ours (any of the three names) is in the sequencer.
present() {
  grep -qE "^Client +[0-9]+ : \"$name(-master|-recv)?\"" "$clients"
}
present && die "a client named $name, $master_name or $recv_name already exists"

tm() {
  tmux -L "$sock" -f /dev/null "$@"
}

# ours <pid>: the pid is a child of this shell that has not exited (it runs
# or is stopped), so a signal reaches only a master or receiver this script
# started.
ours() {
  [ -n "$1" ] || return 1
  awk -v me="$$" '{ sub(/.*\) /, ""); exit !($1 != "Z" && $2 == me) }' \
    "/proc/$1/stat" 2>/dev/null
}
# sig <signal> <pid>
sig() {
  if ours "$2"; then
    kill -s "$1" "$2" 2>/dev/null
  fi
}
# end_child <pid>: resumes and ends a master or receiver this script
# started, and reaps it (SIGKILL when it is still there 3 s after SIGTERM).
end_child() {
  sig CONT "$1"
  sig TERM "$1"
  e_i=0
  while ours "$1" && [ "$e_i" -lt 30 ]; do
    sleep 0.1
    e_i=$((e_i + 1))
  done
  sig KILL "$1"
  wait "$1" 2>/dev/null
}
# orca_ours: $opid is the child of this run's pane shell and runs <orca>.
orca_ours() {
  [ -n "$opid" ] && [ -n "$pane_pid" ] || return 1
  o_pp=$(awk '{ sub(/.*\) /, ""); print $2 }' "/proc/$opid/stat" 2>/dev/null)
  [ "$o_pp" = "$pane_pid" ] &&
    [ "$(readlink "/proc/$opid/exe" 2>/dev/null)" = "$orca_exe" ]
}
# sig_orca <signal>
sig_orca() {
  if orca_ours; then
    kill -s "$1" "$opid" 2>/dev/null
  fi
}

# gone: waits up to 15 s for our clients to leave the sequencer.
gone() {
  g_i=0
  while present && [ "$g_i" -lt 150 ]; do
    sleep 0.1
    g_i=$((g_i + 1))
  done
}

tmp=
run=
started=0
opid=
pane_pid=
mp=
rp=
sp=
# cleanup ignores a second signal, so nothing cuts it short.
cleanup() {
  trap '' INT TERM HUP
  sig_orca CONT
  if [ -n "$run" ]; then
    : >"$run/master-done" 2>/dev/null
  fi
  for p in $mp $rp; do
    sig CONT "$p"
    sig TERM "$p"
  done
  if [ -n "$sp" ]; then
    wait "$sp" 2>/dev/null
  fi
  if [ "$started" = 1 ]; then
    tm kill-server 2>/dev/null
    gone
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
case "$orca$tmp" in
  *"'"*) die "the orca and scratch paths must not contain a single quote" ;;
esac
for prog in master recv; do
  nice -n 19 taskset -c 0-2 cc -std=c99 -O2 -Wall -Wextra \
    -o "$tmp/$prog" "$here/$prog.c" -lasound >"$tmp/cc-$prog.txt" 2>&1 ||
    die "$here/$prog.c does not build: $(head -n 3 "$tmp/cc-$prog.txt")"
done
mkdir "$tmp/xdg" || die "mkdir $tmp/xdg failed"
printf 'clock_source = external\nalsa_input_device = %s:clock\n' \
  "$master_name" >"$tmp/xdg/orca.conf" ||
  die "cannot write $tmp/xdg/orca.conf"
printf '.D1....\n..:03C.\n.......\n' >"$tmp/clock.orca" ||
  die "cannot write $tmp/clock.orca"

# cs: centiseconds since boot, from /proc/uptime.
cs() {
  read -r c_up _ </proc/uptime
  c_frac=${c_up#*.}
  echo $((${c_up%.*} * 100 + ${c_frac#0}))
}
# quote <file>: its first 3 lines, quoted, on one line.
quote() {
  awk 'NR <= 3 { printf "%s\"%s\"", (NR > 1 ? " / " : ""), $0 }
    END { if (NR == 0) printf "no output" }' "$1"
}
# up: our orca's client is in the sequencer with its port 1 (MIDI in).
up() {
  awk -v q="\"$name\"" '
    /^Client / { in_c = ($4 == q); next }
    in_c && /^  Port +1 / { found = 1 }
    END { exit !found }' "$clients"
}
# pool: our orca's input pool, "<size> <peak> <failures>", or nothing.
pool() {
  awk -v q="\"$name\"" '
    /^Client / { in_c = ($4 == q); in_p = 0; next }
    in_c && /Input pool/ { in_p = 1; next }
    in_c && /Output pool/ { in_p = 0; next }
    in_p && /Pool size/ { size = $NF }
    in_p && /Peak cells in use/ { peak = $NF }
    in_p && /Alloc failures/ { fail = $NF }
    END { if (size != "") print size, peak, fail }' "$clients"
}
# live: the live zOrca client's Client line, Port lines and their
# "Connecting To:" and "Connected From:" lines.
live() {
  awk '/^Client / { in_live = ($0 ~ /^Client +[0-9]+ : "(bOrca|zOrca)" /) }
    in_live && (/^Client / || /^  Port / || /Connect(ing To|ed From):/) { print }' "$clients"
}
# hud_seq <file>: the HUD readings in order, repeats collapsed, at most 8.
hud_seq() {
  awk 'NF && $0 != last { if (n < 8) printf "%s%s", (n ? ", " : ""), $0;
      n++; last = $0 }
    END { if (n > 8) printf ", ..."; if (n == 0) printf "none" }' "$1"
}
# analyse <run dir>: from master.log and recv.log, prints "<F8s> <note-ons>
# <bursts> <largest burst> <its spread in us> <notes in the last 3 s>
# <lowest offset> <highest offset> <offsets that are multiples of 6>".
analyse() {
  awk '
    FILENAME == ARGV[1] && $2 == "F8" { f[nf++] = $1 + 0 }
    FILENAME == ARGV[2] && $2 == "on" { n[nn++] = $1 + 0 }
    END {
      bursts = 0; big = 0; spread = 0; size = 1; first = 0
      for (j = 1; j <= nn; j++) {
        if (j < nn && n[j] - n[j - 1] < 40000) {
          if (size == 1) first = n[j - 1]
          size++
          continue
        }
        if (size >= 2) {
          bursts++
          if (size > big) { big = size; spread = n[j - 1] - first }
        }
        size = 1
      }
      wn = 0; lo = ""; hi = ""; m6 = 0
      if (nf > 0) {
        last = f[nf - 1]
        k = 0
        for (j = 0; j < nn; j++) {
          if (n[j] < last - 3000000 || n[j] > last) continue
          while (k + 1 < nf && f[k + 1] <= n[j]) k++
          off = k - 6 * j
          wn++
          if (lo == "" || off < lo) lo = off
          if (hi == "" || off > hi) hi = off
          if (off % 6 == 0) m6++
        }
      }
      if (lo == "") { lo = "none"; hi = "none" }
      print nf, nn, bursts, big, spread, wn, lo, hi, m6
    }' "$1/master.log" "$1/recv.log"
}

n_checks=0
n_failed=0
# result <ok: 1 or 0> <check> <detail>
result() {
  n_checks=$((n_checks + 1))
  if [ "$1" = 1 ]; then
    printf 'PASS %s: %s\n' "$2" "$3"
  else
    printf 'FAIL %s: %s\n' "$2" "$3"
    n_failed=$((n_failed + 1))
  fi
}

# stall <check> <stall seconds>: one stall run into $run; sets $why when it
# could not run, and leaves the logs, $pool_fig (the input pool after the
# master's last F8, or at SIGCONT), $hud_lost and $rc.
stall() {
  run=$tmp/$1
  why=
  pool_fig=
  hud_lost=0
  rc=none
  opid=
  pane_pid=
  mkdir "$run" || die "mkdir $run failed"
  : >"$run/master.log"
  : >"$run/recv.log"
  : >"$run/hud.txt"
  clocks=$(awk -v s="$2" 'BEGIN { printf "%d", (3 + s + 5) * 48 }')
  started=1
  tm new-session -d -s clock -x 100 -y 30 \
    "env XDG_CONFIG_HOME='$tmp/xdg' ZORCA_ALSA_CLIENT_NAME=$name nice -n 19 taskset -c 0-2 '$orca' '$tmp/clock.orca' 2>'$run/err'; echo \$? >'$run/rc'" ||
    die "tmux could not start the pane"
  t0=$(cs)
  while ! up; do
    if [ -s "$run/rc" ] || [ $(($(cs) - t0)) -ge 1000 ]; then
      why="no client named '$name' with its MIDI in port within 10 s"
      if [ -s "$run/rc" ]; then
        why="$why; orca exited with $(cat "$run/rc"): $(quote "$run/err")"
      fi
      break
    fi
    sleep 0.1
  done
  if [ -z "$why" ]; then
    pane_pid=$(tm display-message -p -t clock '#{pane_pid}' 2>/dev/null)
    opid=$(pgrep -P "$pane_pid" 2>/dev/null)
    case $opid in
      '' | *[!0-9]*)
        why="no single orca process under the pane shell ($pane_pid): $opid"
        opid=
        ;;
      *) orca_ours || why="process $opid under the pane shell is not $orca_exe" ;;
    esac
  fi
  if [ -z "$why" ]; then
    nice -n 19 taskset -c 0-2 "$tmp/recv" "$recv_name" "$name:0" 120 \
      >"$run/recv.log" 2>"$run/recv.err" &
    rp=$!
    sleep 0.5
    nice -n 19 taskset -c 0-2 "$tmp/master" "$master_name" 120 1500 "$clocks" \
      >"$run/master.log" 2>"$run/master.err" &
    mp=$!
    t0=$(cs)
    while ! grep -q ' FA$' "$run/master.log"; do
      if ! ours "$mp" || [ $(($(cs) - t0)) -ge 500 ]; then
        why="the master sent no FA within 5 s: $(quote "$run/master.err")"
        break
      fi
      sleep 0.05
    done
  fi
  if [ -z "$why" ]; then
    (
      while [ ! -e "$run/master-done" ]; do
        tm capture-pane -p -t clock 2>/dev/null | grep -o 'ext [a-z0-9-]*' |
          head -n 1
        sleep 0.05
      done
    ) >"$run/hud.txt" &
    sp=$!
    sleep 3
    sig_orca STOP
    sleep "$2"
    pool_fig=$(pool)
    sig_orca CONT
    wait "$mp"
    mp=
    : >"$run/master-done"
    wait "$sp"
    sp=
    sleep 0.5
    pool_end=$(pool)
    [ -n "$pool_end" ] && pool_fig=$pool_end
    if grep -q 'ext lost' "$run/hud.txt"; then
      hud_lost=1
    fi
  fi
  tm send-keys -t clock C-q 2>/dev/null
  t0=$(cs)
  while [ ! -s "$run/rc" ] && [ $(($(cs) - t0)) -lt 1500 ]; do
    sleep 0.1
  done
  if [ -s "$run/rc" ]; then
    rc=$(cat "$run/rc")
  fi
  sig_orca CONT
  tm kill-server 2>/dev/null
  started=0
  end_child "$rp"
  rp=
  if [ -n "$mp" ]; then
    end_child "$mp"
    mp=
  fi
  gone
  opid=
  pane_pid=
}

# figures: sets f8 notes bursts big spread wn lo hi m6 (analyse's words)
# and psize ppeak pfail (the input pool, or none) for the run in $run.
figures() {
  # shellcheck disable=SC2046 # analyse prints nine words, split on purpose
  set -- $(analyse "$run")
  f8=$1 notes=$2 bursts=$3 big=$4 spread=$5 wn=$6 lo=$7 hi=$8 m6=$9
  # shellcheck disable=SC2086 # three words, split on purpose
  set -- ${pool_fig:-none none none}
  psize=$1 ppeak=$2 pfail=$3
}
# summary: the run's figures, on one line.
summary() {
  s_lost="no ext lost while the master ran"
  [ "$hud_lost" = 1 ] && s_lost="ext lost while the master ran"
  printf '%s' "HUD $(hud_seq "$run/hud.txt") ($s_lost); $bursts burst(s)"
  printf '%s' ", the largest $big note-ons within $spread us"
  printf '%s' "; offsets $lo..$hi over $wn notes in the master's last 3 s"
  printf '%s' " ($m6 a multiple of 6); input pool $psize, peak $ppeak"
  printf '%s' ", alloc failures $pfail; $f8 F8s, $notes note-ons"
  printf '%s' "; orca exit $rc"
}

live_before=$(live)

# tmux runs the pane's command with default-shell, which is $SHELL.
SHELL=/bin/sh
export SHELL

# S1: 10 s, inside the pool: one burst, back on the bar.
stall S1 10
if [ -n "$why" ]; then
  result 0 S1 "10 s stall not run: $why"
else
  figures
  ok=0
  if [ "$hud_lost" = 0 ] && [ "$bursts" = 1 ] && [ "$big" -ge 70 ] &&
    [ "$big" -le 85 ] && [ "$wn" -ge 16 ] && [ "$lo" != none ] &&
    [ "$lo" -ge -1 ] && [ "$hi" -le 1 ]; then
    ok=1
  fi
  result "$ok" S1 "10 s stall at 120 BPM: $(summary)"
fi

# S2: 11.1 s, past the pool: no burst, following off the bar.
stall S2 11.1
if [ -n "$why" ]; then
  result 0 S2 "11.1 s stall not run: $why"
else
  figures
  ok=0
  if [ "$hud_lost" = 0 ] && [ "$bursts" = 0 ] && [ "$wn" -ge 16 ] &&
    [ "$lo" != none ] && [ "$lo" -ge 6 ] && [ "$m6" = 0 ] &&
    [ "$pfail" != none ] && [ "$pfail" -gt 0 ]; then
    ok=1
  fi
  result "$ok" S2 "11.1 s stall at 120 BPM: $(summary)"
fi
run=

# Live: the live zOrca client untouched, and no scratch client left.
live_after=$(live)
left=$(grep -cE "^Client +[0-9]+ : \"$name(-master|-recv)?\"" "$clients")
ok=0
if [ "$live_before" = "$live_after" ] && [ "$left" = 0 ]; then
  ok=1
fi
result "$ok" Live "before: $(echo "${live_before:-none}" | tr -s ' \n' '  '); after: $(echo "${live_after:-none}" | tr -s ' \n' '  '); $name clients left $left"

echo "alsa clock check: $n_checks checks, $n_failed failed"
[ "$n_failed" = 0 ]
