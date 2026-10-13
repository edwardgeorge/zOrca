#!/bin/sh
# Form prefill check: builds tests/form-prefill/prefill.c against term_util.c
# and ncurses' form library, then runs it. A one-line box (Open, Save As) must
# keep its whole initial text, not the first 30 characters.
#
#   tests/form-prefill/check.sh
#
# Uses $CC (default cc). Exit status: 0 when every length passes, 1 otherwise.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
tmpd=$(mktemp -d) || exit 1
trap 'rm -rf "$tmpd"' EXIT
trap 'exit 130' INT TERM
"${CC:-cc}" -std=c99 -Wall -Wextra -I"$root" -I"$root/thirdparty" \
  -o "$tmpd/prefill" "$root/tests/form-prefill/prefill.c" \
  "$root/term_util.c" "$root/thirdparty/oso.c" -lform -lncurses
"$tmpd/prefill"
