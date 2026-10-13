#!/bin/sh
# No-writable-globals check for the CORE files.
# Linux/ELF only: it relies on GNU-style nm symbol classes and gcc's
# -no-pie. See tests/README.md.
#
#   tests/check-nm.sh
#
# Compiles each file that `./tool sources core` lists on its own, with
#   gcc -c -std=c99 -O2 -DNDEBUG -g0 -fno-pie -no-pie -fno-lto
# and again with -O0 in place of -O2 (never LTO or strip, which hide
# symbols, and never PIE, under which const pointer tables show as
# writable), and reads each symbol's class, field 2 of `nm -P`. A symbol of
# class B b C D d G g S s V v is writable data or bss and fails the run
# unless a marker covers it. An object for which nm lists nothing fails too,
# so a check that sees nothing cannot pass.
#
# Two passes, because at -O2 gcc places a non-const table that is never
# written and whose address never escapes in r, so the -O2 pass alone cannot
# tell such a table from a const one; at -O0 it is d. Every PASS, FAIL and
# XFAIL line names its pass (or passes), so a log shows which recipe listed
# a symbol.
#
# The canary, tests/nm/canary.c, holds one writable symbol of each class b,
# d, B and D, each with its address escaping, so -O2 keeps them. Each pass
# must flag all four by name on every run, and in each pass the marker code
# must sort them with a built-in marker set: canary_static_* matches both
# statics, canary_extern_init its one symbol, canary_O0_only its own,
# canary_absent nothing (an XPASS), and canary_extern_zero, which no marker
# covers, is unmarked. A fifth symbol, canary_O0_only, a non-const table
# that is never written and whose address never escapes, gives the -O0 pass
# its teeth: it must not be writable in the first pass (-O2 folds it away)
# and must be d in every later pass, and there must be at least two passes.
# The canary prints one line per pass; if any of that fails, the check is
# blind and the run fails.
#
# tests/xfail/nm-check lists the expected writable symbols, one marker per
# line, as `<file> <symbol|glob> <item>`; # starts a comment. The pattern is
# a symbol name, or a literal prefix of at least 3 characters followed by one
# trailing * (scale_*); the item is the one item tag the fix waits for,
# matching ^(B[1-8]|I[1-6])$ (B1, I3). Markers are not split by pass: each
# marker line reports on its own, over both passes: XFAIL, naming its item
# and every writable symbol it matched (class, name and pass), or XPASS when
# it matches none in either pass, which fails the run, so the series that
# removes a symbol deletes its line in the same commit. A symbol that is
# writable only at -O0 is therefore markable. A writable symbol that no
# marker of its file matches fails the run as an unmarked symbol, in the
# pass that lists it. A marker never hides an empty object or a failed
# compile. A marker naming a file outside CORE, a malformed line, any other
# pattern or a duplicate (file, pattern) fails the run.
#
# Exit status: 0 when the canary is flagged in both passes and every result
# is PASS or XFAIL, 1 otherwise, 2 on usage or on a system other than Linux.
set -uf

root=$(cd "$(dirname "$0")/.." && pwd)
if [ "$#" != 0 ]; then
  echo "usage: $0" >&2
  exit 2
fi
if [ "$(uname -s)" != Linux ]; then
  echo "error: $0 is Linux/ELF only; nm's symbol classes differ elsewhere" >&2
  exit 2
fi
marker_rel=tests/xfail/nm-check
canary_rel=tests/nm/canary.c
# The canary's symbols, one per writable class (b d B D); each must be flagged.
canary_symbols='canary_static_zero canary_static_init canary_extern_zero canary_extern_init'
# The recipe without its optimisation level, and the levels, one pass each:
# the first optimises, the second does not (the canary checks both).
nm_flags='-c -std=c99 -DNDEBUG -g0 -fno-pie -no-pie -fno-lto'
nm_levels='-O2 -O0'
# The levels as a PASS line names them: "-O2, -O0".
# shellcheck disable=SC2086  # split the level list
nm_levels_text=$(printf '%s\n' $nm_levels | awk '{ printf "%s%s", (NR > 1 ? ", " : ""), $0 }')

for tool_name in gcc nm; do
  if ! command -v "$tool_name" >/dev/null 2>&1; then
    echo "error: $tool_name not found" >&2
    exit 1
  fi
done
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

# Markers, validated, as "<file> <pattern> <item>" lines in $tmp/markers.
: >"$tmp/markers"
if [ -f "$root/$marker_rel" ]; then
  if ! awk -v core="$core" -v name="$marker_rel" -v out="$tmp/markers" '
    function bad(why) { printf "FAIL  %s:%d: %s\n", name, FNR, why; errors++ }
    BEGIN { n = split(core, c, " "); for (i = 1; i <= n; i++) incore[c[i]] = 1 }
    {
      sub(/#.*/, "")
      if (NF == 0) next
      if (NF != 3) { bad("expected <file> <symbol|glob> <item>"); next }
      if (!($1 in incore)) { bad($1 " is not in CORE"); next }
      if ($2 !~ /^[A-Za-z0-9_.]+$/ &&
          $2 !~ /^[A-Za-z0-9_.][A-Za-z0-9_.][A-Za-z0-9_.]+\*$/) {
        bad("bad pattern " $2 " (expected a symbol name, or a prefix of at" \
          " least 3 characters and one trailing *)"); next
      }
      if ($3 !~ /^(B[1-8]|I[1-6])$/) { bad("bad item id " $3); next }
      if (($1 " " $2) in seen) { bad("duplicate marker for " $1 " " $2); next }
      seen[$1 " " $2] = 1
      print $1, $2, $3 > out
    }
    END { exit errors > 0 }
  ' "$root/$marker_rel"; then
    n_fail=$((n_fail + 1))
  fi
fi

# glob_match <symbol> <pattern>: whether the shell glob matches the symbol.
# set -f turns off pathname expansion only; case still matches patterns.
glob_match() {
  # shellcheck disable=SC2254  # the pattern is a glob on purpose
  case $1 in
    $2) return 0 ;;
  esac
  return 1
}

# marked <symbol>: whether a marker in $tmp/file_markers matches it.
marked() {
  while read -r mk_pattern _; do
    if glob_match "$1" "$mk_pattern"; then
      return 0
    fi
  done <"$tmp/file_markers"
  return 1
}

# matched_by <pattern> <list>: prints the writable symbols in <list> that
# the pattern matches, as "<class> <name>, ..." in list order, each with
# " at <pass>" when the list's lines carry a third field; nothing when it
# matches none.
matched_by() {
  mb_list=
  while read -r mb_class mb_name mb_pass; do
    if glob_match "$mb_name" "$1"; then
      mb_list="$mb_list${mb_list:+, }$mb_class $mb_name${mb_pass:+ at $mb_pass}"
    fi
  done <"$2"
  printf '%s' "$mb_list"
}

# names_of <matched_by output>: the same list without the classes.
names_of() {
  printf '%s\n' "$1" | awk -F', ' '{
    for (i = 1; i <= NF; i++) {
      split($i, part, " ")
      printf "%s%s", (i > 1 ? ", " : ""), part[2]
    }
  }'
}

# inspect <source relative to root> <level>: compiles it with the recipe at
# that optimisation level and sets status to error, empty, writable or
# clean; $tmp/writable then holds the writable symbols as "<class> <name>"
# lines.
inspect() {
  rm -f "$tmp/obj.o"
  : >"$tmp/writable"
  # shellcheck disable=SC2086  # $nm_flags is a word list
  if ! gcc $nm_flags "$2" "$root/$1" -o "$tmp/obj.o" 2>"$tmp/err"; then
    status=error
    return
  fi
  # C collation, so the symbols list in the same order on every machine.
  if ! LC_ALL=C nm -P "$tmp/obj.o" >"$tmp/syms" 2>"$tmp/err"; then
    status=error
    return
  fi
  if ! [ -s "$tmp/syms" ]; then
    status=empty
    return
  fi
  awk '$2 ~ /^[BbCDdGgSsVv]$/ { print $2, $1 }' "$tmp/syms" >"$tmp/writable"
  if [ -s "$tmp/writable" ]; then
    status=writable
  else
    status=clean
  fi
}

# The canary, once per pass; the run is blind unless every pass passes.
canary_ok=1
canary_blind=
pass_n=0
for level in $nm_levels; do
  pass_n=$((pass_n + 1))
  inspect "$canary_rel" "$level"
  missing=
  for sym in $canary_symbols; do
    if ! awk -v s="$sym" '$2 == s { found = 1 } END { exit !found }' "$tmp/writable"; then
      missing="$missing $sym"
    fi
  done
  why=
  case $status in
    empty) why="nm lists no symbols for $canary_rel" ;;
    error) why="could not compile or read $canary_rel" ;;
    *)
      if [ -n "$missing" ] || [ "$status" != writable ]; then
        why="not flagged in $canary_rel:$missing"
      fi
      ;;
  esac
  # canary_O0_only: not writable in the first pass, which optimises and
  # folds the never-written table away, and d in every later pass, so a
  # second pass that optimises like the first is blind.
  o0_text=
  if [ -z "$why" ]; then
    o0_class=$(awk '$2 == "canary_O0_only" { print $1 }' "$tmp/writable")
    if [ "$pass_n" = 1 ]; then
      o0_text="canary_O0_only not writable"
      if [ -n "$o0_class" ]; then
        why="canary_O0_only is $o0_class in the first pass, which must fold it away"
      fi
    else
      o0_text="canary_O0_only $o0_class"
      if [ "$o0_class" != d ]; then
        why="canary_O0_only is ${o0_class:-not writable} in this pass, not d: the pass cannot see a never-written non-const table"
      fi
    fi
  fi
  # The marker path, through the same marked and matched_by as the CORE
  # files. canary_O0_only has its own marker, so it is never unmarked.
  if [ -z "$why" ]; then
    printf '%s B1\n' canary_static_* canary_extern_init canary_O0_only \
      canary_absent >"$tmp/file_markers"
    unmarked=
    while read -r _ name; do
      if ! marked "$name"; then
        unmarked="$unmarked${unmarked:+ }$name"
      fi
    done <"$tmp/writable"
    statics=$(names_of "$(matched_by 'canary_static_*' "$tmp/writable")")
    extern=$(names_of "$(matched_by canary_extern_init "$tmp/writable")")
    absent=$(matched_by canary_absent "$tmp/writable")
    if [ "$unmarked" != canary_extern_zero ]; then
      why="the markers left \"$unmarked\" unmarked, not canary_extern_zero"
    elif [ "$statics" != "canary_static_init, canary_static_zero" ]; then
      why="canary_static_* matched \"$statics\""
    elif [ "$extern" != canary_extern_init ]; then
      why="canary_extern_init matched \"$extern\""
    elif [ -n "$absent" ]; then
      why="canary_absent matched \"$absent\", not nothing"
    fi
  fi
  if [ -z "$why" ]; then
    printf 'PASS  canary %s (detected: %s; %s; marker path: canary_extern_zero unmarked, canary_absent matches none)\n' \
      "$level" \
      "$(awk '{ printf "%s%s %s", (NR > 1 ? ", " : ""), $1, $2 }' "$tmp/writable")" \
      "$o0_text"
  else
    printf 'FAIL  canary %s (the check is blind: %s)\n' "$level" "$why"
    head -n 3 "$tmp/err" | sed 's/^/      /'
    canary_ok=0
    canary_blind="$canary_blind${canary_blind:+, }$level"
  fi
done
if [ "$pass_n" -lt 2 ]; then
  printf 'FAIL  canary (the check is blind: one pass only, so no pass flags canary_O0_only as d)\n'
  canary_ok=0
  canary_blind="$canary_blind${canary_blind:+, }a second pass"
fi

for file in $core; do
  n_files=$((n_files + 1))
  awk -v f="$file" '$1 == f { print $2, $3 }' "$tmp/markers" >"$tmp/file_markers"
  clean=1
  broken=0
  # The writable symbols of every pass, as "<class> <name> <pass>" lines.
  : >"$tmp/writable_all"
  for level in $nm_levels; do
    inspect "$file" "$level"
    case $status in
      error)
        printf 'FAIL  %s %s (could not compile or read it)\n' "$file" "$level"
        head -n 3 "$tmp/err" | sed 's/^/      /'
        n_fail=$((n_fail + 1))
        broken=1
        continue
        ;;
      empty)
        printf 'FAIL  %s %s (nm lists no symbols)\n' "$file" "$level"
        n_fail=$((n_fail + 1))
        broken=1
        continue
        ;;
    esac
    # Every writable symbol needs a marker of its file.
    while read -r class name; do
      printf '%s %s %s\n' "$class" "$name" "$level" >>"$tmp/writable_all"
      if ! marked "$name"; then
        clean=0
        printf 'FAIL  %s %s %s (unmarked symbol at %s)\n' "$file" "$class" "$name" "$level"
        n_fail=$((n_fail + 1))
      fi
    done <"$tmp/writable"
  done
  # A marker never hides a failed compile or an empty object.
  if [ "$broken" = 1 ]; then
    continue
  fi
  # Each marker line reports on its own, over both passes.
  while read -r pattern item; do
    clean=0
    matched=$(matched_by "$pattern" "$tmp/writable_all")
    if [ -n "$matched" ]; then
      printf 'XFAIL %s %s (%s: %s)\n' "$file" "$pattern" "$item" "$matched"
      n_xfail=$((n_xfail + 1))
    else
      printf 'XPASS %s %s (%s: no writable symbol matches in any pass; remove the line from %s)\n' \
        "$file" "$pattern" "$item" "$marker_rel"
      n_xpass=$((n_xpass + 1))
    fi
  done <"$tmp/file_markers"
  if [ "$clean" = 1 ]; then
    printf 'PASS  %s (%s)\n' "$file" "$nm_levels_text"
    n_pass=$((n_pass + 1))
  fi
done

if [ "$canary_ok" = 1 ]; then
  canary_result="canary detected in every pass"
else
  canary_result="canary NOT detected at $canary_blind"
fi
printf 'nm check: %s files, %s pass, %s xfail, %s fail, %s xpass; %s\n' \
  "$n_files" "$n_pass" "$n_xfail" "$n_fail" "$n_xpass" "$canary_result"
[ "$canary_ok" = 1 ] && [ "$n_fail" = 0 ] && [ "$n_xpass" = 0 ]
