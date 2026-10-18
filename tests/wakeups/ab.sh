#!/bin/sh
# The wakeup A/B for the glides that step between ticks (v2.0.0), read from
# the debug wakeup counter: zOrca's event loop wakes as often as
# 4f349cd's when no glide runs, and a running glide wakes it for the CC
# engine's deadline without going through the strict-timing table. It is a
# development check: CI only shellchecks it.
#
#   tests/wakeups/ab.sh
#
# 1. Builds two release orcas, without ALSA or PortMidi, each in its own tree
#    under build/wakeups/, as tests/upstream/build-cli.sh builds Orca-c's
#    cli; the repository's build/orca is never written:
#    - base: 4f349cd from `git archive`, with
#      tests/wakeups/4f349cd-counter.patch applied (`patch -p1`), built with
#      its own tool;
#    - zorca: a copy of this working tree's tracked files, built with
#      CFLAGS_EXTRA=-DZORCA_DEBUG_COUNTERS. A new source file must be
#      tracked (`git add`) before the A/B copies it: the copy is plain
#      `git ls-files`, since the untracked files include planning trees
#      whose names tar cannot read.
#    Both print `<name> wakeups: <n> waits, <z> zero-timeout in <s> s` (<name> is
#    bOrca for 4f349cd, zOrca for this build) to
#    stderr on quit: <n> counts the loop's waits entered with a timeout
#    above 0, <z> those entered with timeout 0, over <s> seconds.
# 2. Runs each case for 30 s with `--bpm 120` in a pty of 40 rows by 120
#    columns (util-linux script), with its own scratch XDG_CONFIG_HOME whose
#    orca.conf holds only midi_beat_clock, so glide_hz is 100, then quits it
#    with Ctrl+Q and reads that line. Two configurations: `strict`, with
#    --strict-timing and midi_beat_clock = 1, and `off`, with neither and
#    midi_beat_clock = 0.
#    - Idle: an empty grid (glide_rate.orca with every cell a dot), on both
#      builds in both configurations. zOrca's waits per second must be
#      within 0.90 to 1.10 times 4f349cd's, and its zero-timeout waits per
#      second at most 1.5 times 4f349cd's + 5, so no busy-wait was added.
#    - Glide, zOrca only: examples/misc/glide_rate.orca, which keeps a glide
#      running whose steps glide_hz limits, against a control, the same file
#      with its ! replaced by a dot, in the same configuration. In `off` the
#      glide's waits per second must be at least the control's + 50, so the
#      loop wakes for the engine's deadline. In `strict` the table already
#      wakes the loop about 190 times a second, so waits cannot show that;
#      there its zero-timeout waits per second must be at most 1.5 times the
#      control's, so the engine's deadline stays off the strict table. In
#      both, the glide's waits per second must be at most the control's +
#      2 x glide_hz (200), so a wait point that stops polling the engine,
#      whose deadline then stays passed and asks for a 1 ms wait every pass,
#      fails.
#
# Needs git, tar, patch, a C compiler, ncursesw, util-linux script, timeout
# and awk. It takes about five minutes. On the uConsole run it as
# `nice -n 19 taskset -c 0-2 tests/wakeups/ab.sh`. Exit status: 0 when
# every bound holds, 1 when one fails, 2 on usage or when a build or a run
# fails.
set -eu

base_sha=4f349cdb0a8a4bb157df67a5178df7df100f9dcf
secs=30
rows=40
cols=120
glide_hz=100 # no glide_hz line in the scratch orca.conf

if [ "$#" != 0 ]; then
  echo "usage: $0" >&2
  exit 2
fi

root=$(cd "$(dirname "$0")/../.." && pwd)
out="$root/build/wakeups"
glide_patch="$root/examples/misc/glide_rate.orca"
case $out in
  *"'"*)
    echo "$0: the repository path must not contain a single quote" >&2
    exit 2
    ;;
esac

die() {
  echo "$0: $*" >&2
  exit 2
}

mkdir -p "$out"
rm -rf "$out/base" "$out/zorca" "$out/runs" "$out/base.tar" "$out/zorca.tar" \
  "$out/files.txt"

# 1. The builds. Each goes through a file, not a pipe, so a failure stops
# the script.
echo "4f349cd: git archive, counter patch, build"
git -C "$root" archive --format=tar -o "$out/base.tar" "$base_sha" ||
  die "git archive $base_sha failed"
mkdir "$out/base"
tar -x -f "$out/base.tar" -C "$out/base"
rm "$out/base.tar"
patch -p1 -N -s -d "$out/base" -i "$root/tests/wakeups/4f349cd-counter.patch" ||
  die "the counter patch does not apply to $base_sha"
(cd "$out/base" && ./tool build orca) >"$out/base-build.log" 2>&1 ||
  die "the 4f349cd build failed; see $out/base-build.log"

echo "zOrca: the working tree's tracked files, build"
(cd "$root" && git ls-files) >"$out/files.txt"
(cd "$root" && tar -c -f "$out/zorca.tar" -T "$out/files.txt") ||
  die "copying the tracked files failed"
mkdir "$out/zorca"
tar -x -f "$out/zorca.tar" -C "$out/zorca"
rm "$out/zorca.tar" "$out/files.txt"
(cd "$out/zorca" && CFLAGS_EXTRA=-DZORCA_DEBUG_COUNTERS ./tool build orca) \
  >"$out/zorca-build.log" 2>&1 ||
  die "the zOrca build failed; see $out/zorca-build.log"

# The grids: the glide patch, its control, and the empty grid.
mkdir -p "$out/runs"
cp "$glide_patch" "$out/runs/glide.orca"
sed 's/!/./g' "$glide_patch" >"$out/runs/control.orca"
sed 's/[^.]/./g' "$glide_patch" >"$out/runs/empty.orca"
grep -q '!' "$out/runs/glide.orca" || die "$glide_patch has no !"

# run <name> <build> <grid> <strict|off>: runs one case and sets $waits_s
# and $zero_s, its waits and zero-timeout waits per second.
run() {
  name=$1
  bin="$out/$2/build/orca"
  grid="$out/runs/$3.orca"
  dir="$out/runs/$name"
  mkdir -p "$dir/config"
  cp "$grid" "$dir/grid.orca"
  if [ "$4" = strict ]; then
    flags="--bpm 120 --strict-timing"
    echo 'midi_beat_clock = 1' >"$dir/config/orca.conf"
  else
    flags="--bpm 120"
    echo 'midi_beat_clock = 0' >"$dir/config/orca.conf"
  fi
  # script runs cmd with $SHELL, so it is pinned to sh.
  cmd="stty rows $rows cols $cols && exec env XDG_CONFIG_HOME='$dir/config'"
  cmd="$cmd TERM=xterm '$bin' $flags '$dir/grid.orca' 2>'$dir/stderr'"
  {
    sleep "$secs"
    printf '\021' # Ctrl+Q quits
  } | timeout $((secs + 30)) env SHELL=/bin/sh script -q -e -c "$cmd" \
    /dev/null >/dev/null ||
    die "$name: orca did not run and quit cleanly; see $dir/stderr"
  line=$(grep -E '^[bz]Orca wakeups: ' "$dir/stderr") ||
    die "$name: no wakeup line in $dir/stderr"
  waits_s=$(echo "$line" | awk '{ printf "%.2f", $3 / $8 }')
  zero_s=$(echo "$line" | awk '{ printf "%.2f", $5 / $8 }')
  printf '%-22s %9s waits/s %9s zero-timeout/s   (%s)\n' "$name" \
    "$waits_s" "$zero_s" "${line#*Orca wakeups: }"
}

# check <description> <awk condition on a and b> <a> <b>
failed=0
check() {
  if awk -v a="$3" -v b="$4" "BEGIN { exit !($2) }"; then
    echo "PASS $1"
  else
    echo "FAIL $1"
    failed=1
  fi
}

# 2. The runs, 4f349cd and zOrca back to back in each configuration.
echo "Runs of ${secs} s at 120 BPM in a ${rows}x${cols} pty:"
for conf in strict off; do
  run "idle-$conf-4f349cd" base empty "$conf"
  base_waits=$waits_s
  base_zero=$zero_s
  run "idle-$conf-zorca" zorca empty "$conf"
  check "idle $conf: zOrca $waits_s waits/s is 0.90-1.10 x 4f349cd's $base_waits" \
    'a >= 0.90 * b && a <= 1.10 * b' "$waits_s" "$base_waits"
  check "idle $conf: zOrca $zero_s zero-timeout/s <= 1.5 x 4f349cd's $base_zero + 5" \
    'a <= 1.5 * b + 5' "$zero_s" "$base_zero"
done
ceiling=$((2 * glide_hz))
run glide-off-control zorca control off
control_waits=$waits_s
run glide-off zorca glide off
check "glide off: $waits_s waits/s >= the control's $control_waits + 50" \
  'a >= b + 50' "$waits_s" "$control_waits"
check "glide off: $waits_s waits/s <= the control's $control_waits + $ceiling" \
  "a <= b + $ceiling" "$waits_s" "$control_waits"
run glide-strict-control zorca control strict
control_waits=$waits_s
control_zero=$zero_s
run glide-strict zorca glide strict
check "glide strict: $zero_s zero-timeout/s <= 1.5 x the control's $control_zero" \
  'a <= 1.5 * b' "$zero_s" "$control_zero"
check "glide strict: $waits_s waits/s <= the control's $control_waits + $ceiling" \
  "a <= b + $ceiling" "$waits_s" "$control_waits"

exit "$failed"
