# shellcheck shell=sh
# Shared by tests/run.sh, tests/check-examples.sh and tests/upstream/compare.sh,
# which source it with `set -u` on, after setting root. It holds the one copy
# of the sanitizer pattern, the UBSAN_OPTIONS they run cli under and the 60 s
# timeout, and the crash rule of the first two (run_checked). compare.sh
# takes the three constants only and keeps its own run().
# tests/alsa-contract/devcheck.sh sources it too, for the sanitizer pattern
# and UBSAN_OPTIONS, which it passes to the orca it runs.
#
# It reads nothing of the caller's but UBSAN_OPTIONS and PATH. Besides
# sanitizer_re, UBSAN_OPTIONS, timeout_cmd and run_checked, it sets the
# globals rc_status, rc_crash and lib_out, so a caller must not use those
# names for anything else.

sanitizer_re='AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:'
# Make UBSan stop at its first report, so it fails the exit status as well.
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
export UBSAN_OPTIONS
# CI's planted-crash canary shims timeout(1) by dropping exactly one argument
# (the duration): keep this `timeout <seconds>`, or change the shim with it.
timeout_cmd=
if command -v timeout >/dev/null 2>&1; then
  timeout_cmd="timeout 60"
fi

# run_checked <output file> <command> <args...>: runs the command, under the
# timeout when timeout(1) is on PATH, with its stdout and stderr in the file.
# Sets rc_status to its exit status, and rc_crash to "cli exited <n> (signal
# or timeout)" when that is 124 or more, else to "sanitizer report" when the
# output matches sanitizer_re, else to nothing. A plain non-zero exit is the
# caller's to judge: tests/run.sh counts it as a mismatch that a marker may
# cover, and tests/check-examples.sh as a failure.
# shellcheck disable=SC2034  # the callers read rc_crash
run_checked() {
  lib_out=$1
  shift
  # shellcheck disable=SC2086  # $timeout_cmd is a word list
  $timeout_cmd "$@" >"$lib_out" 2>&1
  rc_status=$?
  rc_crash=
  if [ "$rc_status" -ge 124 ]; then
    rc_crash="cli exited $rc_status (signal or timeout)"
  elif grep -Eq "$sanitizer_re" "$lib_out"; then
    rc_crash="sanitizer report"
  fi
}
