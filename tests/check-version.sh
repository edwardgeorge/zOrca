#!/bin/sh
# Version check: cli --version (and optionally orca --version) must print
# one line "zOrca <version>" to stdout and exit 0.
#
#   tests/check-version.sh <cli> [<orca>]
#
# Exit status: 0 when every check passes, 1 on failure, 2 on usage.
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <cli> [<orca>]" >&2
  exit 2
fi

cli=$1
tmpd=$(mktemp -d) || exit 1
trap 'rm -rf "$tmpd"' EXIT
trap 'exit 130' INT TERM

check_binary() {
  bin_name=$1
  bin_path=$2
  out="$tmpd/$bin_name.out"
  err="$tmpd/$bin_name.err"

  status=0
  "$bin_path" --version >"$out" 2>"$err" || status=$?

  if [ "$status" != 0 ]; then
    echo "FAIL  $bin_name --version: exited status $status"
    cat "$err" >&2
    return 1
  fi

  if [ -s "$err" ]; then
    echo "FAIL  $bin_name --version: unexpected output on stderr"
    cat "$err" >&2
    return 1
  fi

  line_count=$(wc -l <"$out" | tr -d ' ')
  if [ "$line_count" != 1 ]; then
    echo "FAIL  $bin_name --version: expected 1 line, got $line_count"
    return 1
  fi

  line=$(cat "$out")
  case "$line" in
    "zOrca "?*)
      echo "PASS  $bin_name --version: $line"
      ;;
    *)
      echo "FAIL  $bin_name --version: unexpected output: $line"
      return 1
      ;;
  esac
}

fails=0
check_binary "cli" "$cli" || fails=$((fails + 1))

if [ "$#" -ge 2 ]; then
  orca=$2
  check_binary "orca" "$orca" || fails=$((fails + 1))
fi

[ "$fails" = 0 ]
