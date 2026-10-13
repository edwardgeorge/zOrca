#!/bin/sh
# The ALSA runtime contract device check, the in-tree ALSA backend's
# contract: an --alsa orca's "MIDI out" can be subscribed to, the notes it
# plays reach a subscriber as note-on and note-off pairs, and a send that
# fails shows a bold "MIDI err" on the HUD. verify checks the installed
# engine's client, ports and duplicate refusal; no test links tui_main.c's
# send path or HUD, so this check covers the rest on a real binary. It is a
# development check: CI only shellchecks it, since the runners have no
# /dev/snd/seq.
#
#   tests/alsa-contract/devcheck.sh <orca>
#
# <orca> is any --alsa build: release, -d (nothing is preloaded, so ASan
# runs) or `make appliance`. A scratch directory (mktemp -d) holds
# xdg/orca.conf, the single line `midi_beat_clock = 1`, and note.orca, a 7x3
# grid whose D4 bangs :03C every 4 ticks: note 36 on channel 0, velocity
# 127, twice a second at 120 BPM. The script runs `<orca> --bpm 120
# note.orca` (it plays at start) in a 120 x 40 pane of its own tmux server
# (`tmux -L alsa-contract-<pid> -f /dev/null`, SHELL=/bin/sh), with
# XDG_CONFIG_HOME=<scratch>/xdg, BORCA_ALSA_CLIENT_NAME=alsa-contract-<pid>
# and tests/lib.sh's UBSAN_OPTIONS (halt_on_error=1), its stderr in
# <scratch>/err, and reads the HUD's first line, the pane line that holds
# 7x3, with `tmux capture-pane -e -p`. Each aseqdump subscribes with
# `aseqdump -p alsa-contract-<pid>:0`, and its time counts from when
# /proc/asound/seq/clients shows a subscriber on port 0 (waited for up to
# 3 s, every 0.1 s). Checks, one PASS or FAIL line each:
#   C1  within 10 s, `aconnect -l` lists the client (its number is <n>);
#       `aconnect -i` lists its port 0 "MIDI out" (readable and
#       subscribable) and not port 1, and `aconnect -o` its port 1 "MIDI in"
#       and not port 0. If the client never appears, C2 to C6 fail as not
#       run.
#   C2  a first aseqdump, subscribed for 3 s and then ended with SIGTERM,
#       saw at least 4 "Note on 0, note 36, velocity 127" and no other Note
#       on, each followed by a "Note off 0, note 36" before the next Note on
#       (Note offs before the first Note on are skipped; the last Note on
#       may lack its off) and no other Note off, at least 100 Clocks (beat
#       clock on), and every event from <n>:0. An aseqdump that never
#       subscribes, or exits by itself (a refused subscription, quoted),
#       fails C2.
#   C3  1 s after it is gone, the HUD shows no "MIDI err": a send with no
#       subscriber is normal. It runs before C4, as MIDI err stays until quit.
#   C4  a second aseqdump gets SIGSTOP 0.5 s after it subscribes, so its
#       input queue fills (in about 4 s, with beat clock on) and a send
#       fails: within 20 s (a capture every 0.1 s) the HUD shows "MIDI err".
#       It prints the seconds that took. An aseqdump that never subscribes
#       or has exited fails C4, and its output is quoted.
#   C5  in that capture, "MIDI err" is bold: following every SGR sequence
#       from the top of the capture (capture-pane -e carries attributes
#       from line to line) up to it, bold is on. 0 or an empty sequence
#       resets, 1 sets bold, 22 clears it, and the arguments of 38, 48 and
#       58 (5;n or 2;r;g;b) are skipped, so a colour index 1 is not bold.
#   C6  the second aseqdump gets SIGCONT and SIGTERM, Ctrl+Q quits orca,
#       within 15 s its exit status is 0, and its stderr holds no report
#       that matches tests/lib.sh's sanitizer pattern (quoted if it does).
#   Live  the live bOrca client's lines in /proc/asound/seq/clients (its
#       Client line, its Port lines and their "Connecting To:" and
#       "Connected From:" lines) are the same before and after, or absent
#       both times, and no alsa-contract-<pid> client is left:
#       tests/alsa-guard/devcheck.sh's rule.
# Device rules: the client is alsa-contract-<pid>, never bOrca, and the
# config a scratch XDG_CONFIG_HOME; the live engine's client and
# borca-tty.service are only observed (aconnect only lists; no systemctl or
# sudo). orca and every aseqdump run under `nice -n 19 taskset -c 0-2`, so
# core 3 stays free; run the script the same way on the uConsole. Only the
# aseqdumps it started are signalled, by their PIDs, and only its own tmux
# server is killed. On exit it resumes and ends those aseqdumps, kills its
# tmux server and removes the scratch directory, ignoring further INT, TERM
# and HUP meanwhile. It takes about 10 s (13 s for a debug build, 25 s when
# C4 times out).
#
# Needs tests/lib.sh, tmux, aseqdump, aconnect, readelf, taskset and a
# readable /proc/asound/seq/clients. Exit status: 0 when every check passes,
# 1 when one fails, 2 on usage (no argument, or one that is not an
# executable file), a missing tool or tests/lib.sh, a binary that is not an
# --alsa build (no libasound.so NEEDED), an alsa-contract-<pid> client
# already present, or a scratch directory, scratch file or tmux server that
# cannot be made; 129 on SIGHUP, 130 on SIGINT and 143 on SIGTERM.
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

for t in tmux aseqdump aconnect readelf taskset; do
  command -v "$t" >/dev/null 2>&1 || die "$t is not on PATH"
done
# tests/lib.sh's sanitizer pattern and UBSAN_OPTIONS (exported), one copy.
# shellcheck source=../lib.sh source-path=SCRIPTDIR disable=SC1091
. "$here/../lib.sh" || die "cannot read $here/../lib.sh"
san_re=${sanitizer_re:?}
clients=/proc/asound/seq/clients
[ -r "$clients" ] || die "$clients is not readable (no ALSA sequencer?)"
case $(readelf -d "$orca" 2>/dev/null | grep '(NEEDED)') in
  *libasound.so*) ;;
  *) die "$orca is not an --alsa build: readelf -d lists no libasound.so NEEDED" ;;
esac
name=alsa-contract-$$
sock=$name
if grep -qF "\"$name\"" "$clients"; then
  die "a client named $name already exists"
fi

tm() {
  tmux -L "$sock" -f /dev/null "$@"
}

# ours <pid>: the pid is a child of this shell that has not exited (it runs
# or is stopped), so a signal reaches only an aseqdump this script started.
ours() {
  awk -v me="$$" '{ sub(/.*\) /, ""); exit !($1 != "Z" && $2 == me) }' \
    "/proc/$1/stat" 2>/dev/null
}
# sig <signal> <pid>
sig() {
  if ours "$2"; then
    kill -s "$1" "$2" 2>/dev/null
  fi
}
# end_asd <pid>: resumes and ends an aseqdump this script started, and reaps
# it (SIGKILL when it is still there 3 s after SIGTERM).
end_asd() {
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

# gone: waits up to 15 s for the scratch client to leave the sequencer.
gone() {
  g_i=0
  while grep -qF "\"$name\"" "$clients" && [ "$g_i" -lt 150 ]; do
    sleep 0.1
    g_i=$((g_i + 1))
  done
}

asd1=
asd2=
tmp=
started=0
# cleanup ignores a second signal, so nothing cuts it short.
cleanup() {
  trap '' INT TERM HUP
  for p in $asd1 $asd2; do
    sig CONT "$p"
    sig TERM "$p"
  done
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
case "$orca$tmp$UBSAN_OPTIONS" in
  *"'"*) die "the orca and scratch paths and UBSAN_OPTIONS must not contain a single quote" ;;
esac
mkdir "$tmp/xdg" || die "mkdir $tmp/xdg failed"
echo 'midi_beat_clock = 1' >"$tmp/xdg/orca.conf" ||
  die "cannot write $tmp/xdg/orca.conf"
printf '.D4....\n..:03C.\n.......\n' >"$tmp/note.orca" ||
  die "cannot write $tmp/note.orca"
esc=$(printf '\033')

# cs: centiseconds since boot, from /proc/uptime.
cs() {
  read -r c_up _ </proc/uptime
  c_frac=${c_up#*.}
  echo $((${c_up%.*} * 100 + ${c_frac#0}))
}
# secs <centiseconds>: seconds, to a tenth.
secs() {
  echo "$(($1 / 100)).$(($1 % 100 / 10))"
}
# quote <file>: its first 3 lines, quoted, on one line.
quote() {
  awk 'NR <= 3 { printf "%s\"%s\"", (NR > 1 ? " / " : ""), $0 }
    END { if (NR == 0) printf "no output" }' "$1"
}
# plain <text>: the text without SGR sequences, its spaces squeezed.
plain() {
  printf '%s\n' "$1" | sed "s/$esc\[[0-9;:]*m//g; s/  */ /g; s/^ //; s/ $//"
}
# oneline <lines>: the lines joined by ", ", or none.
oneline() {
  if [ -n "$1" ]; then
    printf '%s\n' "$1" | awk '{ printf "%s%s", (NR > 1 ? ", " : ""), $0 }'
  else
    echo none
  fi
}
# has <lines> <line>: one of the lines is exactly the line.
has() {
  printf '%s\n' "$1" | grep -qxF -- "$2"
}
# orca_said: when orca has exited, its exit status and first stderr lines.
orca_said() {
  if [ -s "$tmp/rc" ]; then
    printf '; orca exited with %s' "$(cat "$tmp/rc")"
    if [ -s "$tmp/err" ]; then
      printf ': %s' "$(quote "$tmp/err")"
    fi
  fi
}

# client_n: the scratch client's number in `aconnect -l`, or nothing.
client_n() {
  aconnect -l 2>/dev/null | awk -v q="'$name'" '
    $1 == "client" && $3 == q { sub(/:$/, "", $2); print $2; exit }'
}
# ports <option>: the scratch client's ports in `aconnect <option>`, one
# "<port> <name>" line each, the name without its quotes and padding.
ports() {
  aconnect "$1" 2>/dev/null | awk -v q="'$name'" -v sq="'" '
    $1 == "client" { in_c = ($3 == q); next }
    in_c && /^ +[0-9]+ / {
      p = $1
      s = $0
      sub(/^ +[0-9]+ /, "", s)
      gsub(sq, "", s)
      sub(/ +$/, "", s)
      print p " " s
    }'
}
# subscribers: the subscribers of the scratch client's port 0, from the
# "Connecting To:" line under it in /proc/asound/seq/clients, or nothing.
subscribers() {
  awk -v q="\"$name\"" '
    /^Client / { in_c = ($4 == q); port = ""; next }
    in_c && /^  Port / { port = $2; next }
    in_c && port == "0" && /Connecting To:/ {
      sub(/.*Connecting To: */, "")
      print
      exit
    }' "$clients"
}
# subscribed <pid>: waits up to 3 s, every 0.1 s, while the aseqdump of
# that pid runs, for port 0 to have a subscriber, and prints the
# subscribers; prints nothing when none came or the aseqdump exited.
subscribed() {
  s_t0=$(cs)
  while ours "$1"; do
    s_list=$(subscribers)
    if [ -n "$s_list" ]; then
      echo "$s_list"
      return
    fi
    if [ $(($(cs) - s_t0)) -ge 300 ]; then
      return
    fi
    sleep 0.1
  done
}
# hud: captures the pane, with its attributes, into $tmp/cap and prints the
# HUD's first line, the one that holds the grid size 7x3 (nothing when the
# pane is gone or shows no HUD).
hud() {
  tm capture-pane -e -p -t contract >"$tmp/cap" 2>/dev/null || : >"$tmp/cap"
  awk 'index($0, "7x3") { print; exit }' "$tmp/cap"
}
# bold_at_err <capture>: follows the capture's SGR sequences from its top to
# the first "MIDI err" on the HUD line (capture-pane -e carries attributes
# from line to line) and prints "1" or "0", whether bold is on there, then
# the HUD line's sequences before it from its last reset (or none). 0 or an
# empty sequence resets, 1 sets bold, 22 clears it, and the arguments of 38,
# 48 and 58 (5;n or 2;r;g;b; a colon form is one parameter) are skipped.
bold_at_err() {
  awk -v esc="$esc" '
    BEGIN { re = esc "\\[[0-9;:]*m"; bold = 0; seen = "" }
    {
      s = $0
      hud = index(s, "7x3") > 0
      if (hud) {
        i = index(s, "MIDI err")
        s = i ? substr(s, 1, i - 1) : s
      }
      while (match(s, re)) {
        seq = substr(s, RSTART + 2, RLENGTH - 3)
        n = split(seq, p, ";")
        reset = (n == 0)
        if (n == 0) bold = 0
        for (k = 1; k <= n; k++) {
          v = p[k]
          if (v ~ /:/) continue
          v += 0
          if (v == 0) { bold = 0; reset = 1 }
          else if (v == 1) bold = 1
          else if (v == 22) bold = 0
          else if (v == 38 || v == 48 || v == 58) {
            if (p[k + 1] + 0 == 5) k += 2
            else if (p[k + 1] + 0 == 2) k += 4
          }
        }
        if (hud) seen = (reset ? "" : seen) "ESC[" seq "m"
        s = substr(s, RSTART + RLENGTH)
      }
      if (hud) {
        printf "%d %s\n", bold, (seen == "" ? "none" : seen)
        exit
      }
    }' "$1"
}
# wait_rc <centiseconds>: waits that long at most for orca's exit status and
# prints it, or none.
wait_rc() {
  w_t0=$(cs)
  while [ ! -s "$tmp/rc" ] && [ $(($(cs) - w_t0)) -lt "$1" ]; do
    sleep 0.1
  done
  if [ -s "$tmp/rc" ]; then
    cat "$tmp/rc"
  else
    echo none
  fi
}

# live: the live bOrca client's Client line, Port lines and their
# "Connecting To:" and "Connected From:" lines.
live() {
  awk '/^Client / { in_live = ($0 ~ /^Client +[0-9]+ : "bOrca" /) }
    in_live && (/^Client / || /^  Port / || /Connect(ing To|ed From):/) { print }' "$clients"
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

live_before=$(live)

# tmux runs the pane's command with default-shell, which is $SHELL.
SHELL=/bin/sh
export SHELL
started=1
tm new-session -d -s contract -x 120 -y 40 \
  "env XDG_CONFIG_HOME='$tmp/xdg' BORCA_ALSA_CLIENT_NAME=$name UBSAN_OPTIONS='$UBSAN_OPTIONS' nice -n 19 taskset -c 0-2 '$orca' --bpm 120 '$tmp/note.orca' 2>'$tmp/err'; echo \$? >'$tmp/rc'" ||
  die "tmux could not start the pane"

# C1: the client and its ports. The ports follow the client's rename (after
# the name guard's second scan), so the wait is for port 1 too.
t0=$(cs)
n=
while :; do
  n=$(client_n)
  if [ -n "$n" ] && ports -l | grep -q '^1 '; then
    break
  fi
  if [ -s "$tmp/rc" ] || [ $(($(cs) - t0)) -ge 1000 ]; then
    break
  fi
  sleep 0.1
done

# contract: C1's port checks and C2 to C6, on client <n>.
contract() {
  in_list=$(ports -i)
  out_list=$(ports -o)
  ok=0
  if has "$in_list" "0 MIDI out" &&
    ! printf '%s\n' "$in_list" | grep -q '^1 ' &&
    has "$out_list" "1 MIDI in" &&
    ! printf '%s\n' "$out_list" | grep -q '^0 '; then
    ok=1
  fi
  result "$ok" C1 "client $n; aconnect -i: $(oneline "$in_list"); aconnect -o: $(oneline "$out_list")"

  # C2: a subscriber for 3 s.
  nice -n 19 taskset -c 0-2 aseqdump -p "$name:0" >"$tmp/asd1.txt" 2>&1 &
  asd1=$!
  sub=$(subscribed "$asd1")
  early=0
  if [ -n "$sub" ]; then
    sleep 3
  fi
  ours "$asd1" || early=1
  end_asd "$asd1"
  asd1=
  if [ "$early" = 1 ] && [ -z "$sub" ]; then
    result 0 C2 "aseqdump exited by itself before it subscribed: $(quote "$tmp/asd1.txt")"
  elif [ "$early" = 1 ]; then
    result 0 C2 "aseqdump exited by itself within its 3 s: $(quote "$tmp/asd1.txt")"
  elif [ -z "$sub" ]; then
    result 0 C2 "aseqdump never subscribed: no subscriber on port 0 within 3 s"
  else
    c2=$(awk -v src="$n:0" '
      $1 ~ /^[0-9]+:[0-9]+$/ {
        events++
        if ($1 != src) bad_src++
        if ($2 == "Clock") { clocks++; next }
        if ($2 == "Note" && $3 == "on") {
          if (pending) unpaired++
          started = 1
          pending = 1
          if ($0 ~ /Note on +0, note 36, velocity 127$/) ons++
          else other_ons++
          next
        }
        if ($2 == "Note" && $3 == "off") {
          if (!started) { leading++; next }
          if (pending && $0 ~ /Note off +0, note 36,/) { pending = 0; offs++ }
          else stray_offs++
        }
      }
      END {
        ok = (ons >= 4 && other_ons == 0 && unpaired == 0 && stray_offs == 0 && bad_src == 0 && clocks >= 100)
        printf "%d %d Note on 0, note 36, velocity 127 (%d other), %d paired, %d not followed by its off, %d leading Note off skipped, %d stray Note off; %d Clock; %d of %d events not from %s\n", ok, ons, other_ons, offs, unpaired, leading, stray_offs, clocks, bad_src, events, src
      }' "$tmp/asd1.txt")
    result "${c2%% *}" C2 "subscriber $sub; ${c2#* }"
  fi

  # C3: the subscriber has gone; a send with no subscriber is no error.
  sleep 1
  line=$(hud)
  if [ -z "$line" ]; then
    result 0 C3 "no HUD line (7x3) in the pane$(orca_said)"
  else
    case $line in
      *"MIDI err"*) result 0 C3 "MIDI err with no subscriber: $(plain "$line")" ;;
      *) result 1 C3 "no MIDI err 1 s after the subscriber left: $(plain "$line")" ;;
    esac
  fi

  # C4: a stopped subscriber, whose queue fills, so a send fails.
  nice -n 19 taskset -c 0-2 aseqdump -p "$name:0" >"$tmp/asd2.txt" 2>&1 &
  asd2=$!
  sub=$(subscribed "$asd2")
  found=0
  if [ -n "$sub" ]; then
    sleep 0.5
  fi
  if ! ours "$asd2"; then
    if [ -z "$sub" ]; then
      result 0 C4 "the second aseqdump exited by itself before it subscribed: $(quote "$tmp/asd2.txt")"
    else
      result 0 C4 "the second aseqdump exited by itself within 0.5 s of subscribing: $(quote "$tmp/asd2.txt")"
    fi
  elif [ -z "$sub" ]; then
    result 0 C4 "the second aseqdump never subscribed: no subscriber on port 0 within 3 s"
  else
    sig STOP "$asd2"
    t0=$(cs)
    polls=0
    while :; do
      polls=$((polls + 1))
      line=$(hud)
      case $line in
        *"MIDI err"*)
          found=1
          cp "$tmp/cap" "$tmp/cap-c4"
          break
          ;;
      esac
      if [ -s "$tmp/rc" ] || [ $(($(cs) - t0)) -ge 2000 ]; then
        break
      fi
      sleep 0.1
    done
    took=$(secs $(($(cs) - t0)))
    if [ "$found" = 1 ]; then
      result 1 C4 "MIDI err $took s after SIGSTOP ($polls captures; subscriber $sub)"
    else
      msg="no MIDI err $took s after SIGSTOP ($polls captures; subscriber $sub)"
      if ! ours "$asd2"; then
        msg="$msg; the second aseqdump had exited: $(quote "$tmp/asd2.txt")"
      fi
      result 0 C4 "$msg$(orca_said)"
    fi
  fi

  # C5: bold.
  if [ "$found" = 1 ]; then
    c5=$(bold_at_err "$tmp/cap-c4")
    if [ "${c5%% *}" = 1 ]; then
      result 1 C5 "bold at MIDI err (the HUD line's SGR before it: ${c5#* })"
    else
      result 0 C5 "MIDI err not bold (the HUD line's SGR before it: ${c5#* })"
    fi
  else
    result 0 C5 "no MIDI err to check"
  fi

  # C6: quit, with no sanitizer report.
  end_asd "$asd2"
  asd2=
  tm send-keys -t contract C-q 2>/dev/null
  t0=$(cs)
  rc=$(wait_rc 1500)
  took=$(secs $(($(cs) - t0)))
  san=
  if [ -s "$tmp/err" ] && grep -Eq "$san_re" "$tmp/err"; then
    grep -E "$san_re" "$tmp/err" >"$tmp/san"
    san="; sanitizer report: $(quote "$tmp/san")"
  fi
  if [ "$rc" = 0 ] && [ -z "$san" ]; then
    result 1 C6 "Ctrl+Q: exit status 0 after $took s, no sanitizer report"
  elif [ "$rc" = none ]; then
    result 0 C6 "Ctrl+Q: orca still running after $took s$san"
  elif [ -n "$san" ]; then
    result 0 C6 "Ctrl+Q: exit status $rc after $took s$san"
  else
    result 0 C6 "Ctrl+Q: exit status $rc after $took s$(orca_said)"
  fi
}

if [ -n "$n" ]; then
  contract
else
  result 0 C1 "no client named '$name' within $(secs $(($(cs) - t0))) s$(orca_said)"
  for c in C2 C3 C4 C5 C6; do
    result 0 "$c" "not run"
  done
  tm send-keys -t contract C-q 2>/dev/null
  wait_rc 1500 >/dev/null
fi
if [ ! -s "$tmp/rc" ]; then
  tm kill-server 2>/dev/null
  gone
fi

# Live: the live bOrca client untouched, and no scratch client left.
live_after=$(live)
left=$(grep -cF "\"$name\"" "$clients")
ok=0
if [ "$live_before" = "$live_after" ] && [ "$left" = 0 ]; then
  ok=1
fi
result "$ok" Live "before: $(echo "${live_before:-none}" | tr -s ' \n' '  '); after: $(echo "${live_after:-none}" | tr -s ' \n' '  '); $name clients left $left"

echo "alsa contract check: $n_checks checks, $n_failed failed"
[ "$n_failed" = 0 ]
