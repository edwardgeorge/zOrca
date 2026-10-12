#!/bin/sh
# Include allow-list, core/shell tiers and banned-name check for the CORE
# files (architecture spine AD-1, AD-2). See tests/README.md.
#
#   tests/check-includes.sh
#
# For each file that `./tool sources core` lists, and each core header it
# includes (directly or through another core header), comments are stripped
# with `gcc -fpreprocessed -dD -E -P`, which keeps #include and #define lines
# and expands no macros. The script then reports:
#   - every #include that is not allowed for the file, as the pattern
#     <name.h> or "name.h", spelled as in the source;
#   - each banned name (rand, srand, srandom, drand48, lrand48, getenv) found
#     as a whole word in code, as the pattern of that name. Comments do not
#     count, and random is not banned: the whole-word grep never matches it.
#
# Two tiers. core_headers is allowed in every CORE file. shell_headers (the
# testable shell's: tick.h, tooltips.h, tui_prefs.h) is allowed only when
# the CORE file under scan is in shell_files (tick.c, tooltips.c,
# tui_prefs.c); anywhere else a shell header is a violation, so a core module
# cannot include tick.h, tooltips.h or tui_prefs.h, nor reach one through a
# core header. The tier is decided by the CORE file, not by the header being
# read: a shell header pulled in through a core header is reported against
# the CORE file and names that header, as every violation found in a header
# is. A testable-shell module adds its .c to shell_files and its header to
# shell_headers, never to core_headers. base.h is shared foundation: it is
# allowed whole and not scanned. portlabel.h is core: sim.h includes it.
#
# Known edges: a banned name inside a string literal or an #if 0 block is
# reported. base.h includes <unistd.h>, so every core file sees read(),
# write(), usleep() and the rest of it, and calling them passes this check.
#
# The canary, tests/includes/canary.c, includes <stdio.h>, calls each of the
# six banned names and includes "tick.h" from outside the shell tier. It is
# scanned first, the same way as a CORE file, and all eight patterns must be
# reported for it on every run; if one is not, the check is blind to it and
# the run fails. The canary is not in shell_files, so "tick.h" is never
# allowed for it: it is reported, never queued. A name in banned_names that
# the canary's pattern list lacks also fails the canary.
#
# tests/xfail/include-check lists the expected violations, one per line, as
# `<file> <pattern> <item...>`; # starts a comment. The pattern is a banned
# name, <name.h> or "name.h" (a shell header's "tick.h" among them). The
# items are the spec items the fix waits for, each matching
# ^(B[1-8]|I[1-6])$ (B1, I3). A marked violation reports XFAIL and does not
# fail the run. A marker whose violation is gone reports XPASS and fails the
# run, so the series that removes the violation deletes the line in the same
# commit. A marker naming a file outside CORE, a malformed line or a
# duplicate fails the run.
#
# Exit status: 0 when the canary is detected and every result is PASS or
# XFAIL, 1 otherwise, 2 on usage.
set -uf

root=$(cd "$(dirname "$0")/.." && pwd)
if [ "$#" != 0 ]; then
  echo "usage: $0" >&2
  exit 2
fi
marker_rel=tests/xfail/include-check
canary_rel=tests/includes/canary.c

# AD-2's allow-list, in two tiers: the core headers, the port-label header
# (portlabel.h, I5) among them, for every CORE file; the testable shell's
# headers only for the testable shell's files.
allowed_system='<assert.h> <errno.h> <limits.h> <stdbool.h> <stddef.h> <stdint.h> <stdlib.h> <string.h>'
core_headers='base.h vmio.h gbuffer.h sim.h opstate.h prng.h ccout.h transport.h music.h portlabel.h'
shell_headers='tick.h tooltips.h tui_prefs.h'
shell_files='tick.c tooltips.c tui_prefs.c'
# AD-1's banned names, each a pattern of its own.
banned_names='rand srand srandom drand48 lrand48 getenv'

if ! command -v gcc >/dev/null 2>&1; then
  echo "error: gcc not found (the comment stripping needs gcc)" >&2
  exit 1
fi
if ! core=$(cd "$root" && ./tool sources core); then
  echo "error: ./tool sources core failed" >&2
  exit 1
fi
if [ -z "$core" ]; then
  echo "error: ./tool sources core listed no files" >&2
  exit 1
fi
# One line, space-separated (no core file name contains a space).
# shellcheck disable=SC2086  # split the one-per-line list
core=$(printf '%s ' $core)

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' INT TERM

n_files=0 n_pass=0 n_fail=0 n_xfail=0 n_xpass=0

# Markers, validated, as "<file> <pattern> <items>" lines in $tmp/markers.
: >"$tmp/markers"
if [ -f "$root/$marker_rel" ]; then
  if ! awk -v core="$core" -v banned="$banned_names" -v name="$marker_rel" \
    -v out="$tmp/markers" '
    function bad(why) { printf "FAIL  %s:%d: %s\n", name, FNR, why; errors++ }
    BEGIN {
      n = split(core, c, " "); for (i = 1; i <= n; i++) incore[c[i]] = 1
      n = split(banned, b, " "); for (i = 1; i <= n; i++) isbanned[b[i]] = 1
    }
    {
      sub(/#.*/, "")
      if (NF == 0) next
      if (NF < 3) { bad("expected <file> <pattern> <item...>"); next }
      if (!($1 in incore)) { bad($1 " is not in CORE"); next }
      if (!($2 in isbanned) && $2 !~ /^<[^<>"]+>$/ && $2 !~ /^"[^<>"]+"$/) {
        bad("bad pattern " $2 " (expected one of " banned ", <name.h> or \"name.h\")"); next
      }
      items = ""
      for (i = 3; i <= NF; i++) {
        if ($i !~ /^(B[1-8]|I[1-6])$/) { bad("bad item id " $i); next }
        items = items (i > 3 ? " " : "") $i
      }
      if (($1 " " $2) in seen) { bad("duplicate marker for " $1 " " $2); next }
      seen[$1 " " $2] = 1
      print $1, $2, items > out
    }
    END { exit errors > 0 }
  ' "$root/$marker_rel"; then
    n_fail=$((n_fail + 1))
  fi
fi

# in_list <word> <space-separated list>
in_list() {
  case " $2 " in
    *" $1 "*) return 0 ;;
  esac
  return 1
}

# scan <core file>: writes "<pattern> <where>" lines to $tmp/found for the
# file and every core header it pulls in. Returns 1 if gcc cannot read one.
scan() {
  : >"$tmp/found"
  sc_core=$1
  sc_queue=$1
  sc_seen=
  # Whether the CORE file under scan may include the testable shell's
  # headers: the tier is the CORE file's, whichever header is being read.
  sc_shell=0
  if in_list "$sc_core" "$shell_files"; then
    sc_shell=1
  fi
  while [ -n "$sc_queue" ]; do
    # shellcheck disable=SC2086  # a space-separated work list
    set -- $sc_queue
    sc_f=$1
    shift
    sc_queue=$*
    if in_list "$sc_f" "$sc_seen"; then
      continue
    fi
    sc_seen="$sc_seen $sc_f"
    if ! gcc -w -fpreprocessed -dD -E -P "$root/$sc_f" >"$tmp/pp" 2>"$tmp/err"; then
      printf 'FAIL  %s (gcc could not read %s)\n' "$sc_core" "$sc_f"
      head -n 3 "$tmp/err" | sed 's/^/      /'
      return 1
    fi
    case $sc_f in
      */*) sc_dir=${sc_f%/*}/ ;;
      *) sc_dir= ;;
    esac
    # Each #include's header name, as written; a macro include gives its
    # first token, which is never on the allow-list.
    grep -E '^[[:space:]]*#[[:space:]]*include' "$tmp/pp" |
      sed -e 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*//' \
        -e 's/^\(<[^>]*>\).*/\1/' -e 's/^\("[^"]*"\).*/\1/' \
        -e 's/[[:space:]].*//' >"$tmp/includes"
    while IFS= read -r sc_inc; do
      case $sc_inc in
        \<*)
          in_list "$sc_inc" "$allowed_system" && continue
          ;;
        \"*)
          sc_h=${sc_inc#\"}
          sc_h=${sc_h%\"}
          sc_allowed=0
          if in_list "$sc_h" "$core_headers"; then
            sc_allowed=1
          elif [ "$sc_shell" = 1 ] && in_list "$sc_h" "$shell_headers"; then
            sc_allowed=1
          fi
          if [ "$sc_allowed" = 1 ]; then
            if [ "$sc_h" != base.h ] && [ -f "$root/$sc_dir$sc_h" ]; then
              sc_queue="$sc_queue $sc_dir$sc_h"
            fi
            continue
          fi
          ;;
      esac
      printf '%s %s\n' "$sc_inc" "$sc_f" >>"$tmp/found"
    done <"$tmp/includes"
    # Each banned name as a whole word in the non-#include lines.
    grep -Ev '^[[:space:]]*#[[:space:]]*include' "$tmp/pp" >"$tmp/code"
    for sc_name in $banned_names; do
      if grep -Eq "(^|[^A-Za-z0-9_])$sc_name([^A-Za-z0-9_]|\$)" "$tmp/code"; then
        printf '%s %s\n' "$sc_name" "$sc_f" >>"$tmp/found"
      fi
    done
  done
  return 0
}

# The canary must be reported for every one of its planted violations: the
# eight patterns, one per line in $tmp/canary_patterns. The list is spelled
# out, not built from banned_names, so a name dropped from that list is
# missed in the canary and fails the run; and a name in banned_names that
# the list lacks fails the canary too, so a name added there has a canary.
canary_ok=0
canary_missing=
printf '%s\n' '<stdio.h>' rand srand srandom drand48 lrand48 getenv '"tick.h"' \
  >"$tmp/canary_patterns"
canary_unlisted=
for canary_name in $banned_names; do
  if ! grep -Fqx "$canary_name" "$tmp/canary_patterns"; then
    canary_unlisted="$canary_unlisted${canary_unlisted:+, }$canary_name"
  fi
done
if [ -n "$canary_unlisted" ]; then
  printf 'FAIL  canary (the canary list lacks %s, which banned_names checks: plant it in %s and add it to the list)\n' \
    "$canary_unlisted" "$canary_rel"
elif scan "$canary_rel"; then
  while IFS= read -r canary_pattern; do
    if ! awk -v p="$canary_pattern" '$1 == p { found = 1 } END { exit !found }' \
      "$tmp/found"; then
      canary_missing="$canary_missing${canary_missing:+, }$canary_pattern"
    fi
  done <"$tmp/canary_patterns"
  if [ -z "$canary_missing" ]; then
    printf 'PASS  canary (detected: %s)\n' \
      "$(awk '{ printf "%s%s", (NR > 1 ? ", " : ""), $0 }' "$tmp/canary_patterns")"
    canary_ok=1
  else
    printf 'FAIL  canary (the check is blind: %s not reported in %s)\n' \
      "$canary_missing" "$canary_rel"
  fi
else
  printf 'FAIL  canary (the check is blind: gcc could not read %s)\n' \
    "$canary_rel"
fi

for file in $core; do
  n_files=$((n_files + 1))
  if ! scan "$file"; then
    n_fail=$((n_fail + 1))
    continue
  fi
  # One result per (file, pattern), keeping the first place it was found.
  awk '!seen[$1]++' "$tmp/found" >"$tmp/violations"
  awk -v f="$file" '$1 == f { $1 = ""; sub(/^ /, ""); print }' "$tmp/markers" \
    >"$tmp/file_markers"
  clean=1
  while read -r pattern where; do
    clean=0
    at=
    if [ "$where" != "$file" ]; then
      at=" in $where"
    fi
    items=$(awk -v p="$pattern" '$1 == p { $1 = ""; sub(/^ /, ""); print }' "$tmp/file_markers")
    if [ -n "$items" ]; then
      printf 'XFAIL %s %s (%s%s)\n' "$file" "$pattern" "$items" "${at:+;$at}"
      n_xfail=$((n_xfail + 1))
    else
      printf 'FAIL  %s %s%s\n' "$file" "$pattern" "${at:+ (found$at)}"
      n_fail=$((n_fail + 1))
    fi
  done <"$tmp/violations"
  while read -r pattern items; do
    if ! awk -v p="$pattern" '$1 == p { found = 1 } END { exit !found }' "$tmp/violations"; then
      clean=0
      printf 'XPASS %s %s (marked xfail for %s: remove the line from %s)\n' \
        "$file" "$pattern" "$items" "$marker_rel"
      n_xpass=$((n_xpass + 1))
    fi
  done <"$tmp/file_markers"
  if [ "$clean" = 1 ]; then
    printf 'PASS  %s\n' "$file"
    n_pass=$((n_pass + 1))
  fi
done

if [ "$canary_ok" = 1 ]; then
  canary_result="canary detected"
else
  canary_result="canary NOT detected"
fi
printf 'include check: %s files, %s pass, %s xfail, %s fail, %s xpass; %s\n' \
  "$n_files" "$n_pass" "$n_xfail" "$n_fail" "$n_xpass" "$canary_result"
[ "$canary_ok" = 1 ] && [ "$n_fail" = 0 ] && [ "$n_xpass" = 0 ]
