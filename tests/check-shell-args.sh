#!/bin/sh
# Shell argument check (P4-3): pins the arguments tui_main.c passes to the
# three clock-out helpers P3-4a moved under unit test, tick_stop (tick.c),
# clock_out_toggle and clock_out_return_internal (midi_in.h). No test target
# compiles tui_main.c, and the unit tests give the helpers arguments they
# compute themselves, so a wrong argument here passes every build and test.
# See tests/README.md.
#
#   tests/check-shell-args.sh
#
# tui_main.c is read through `gcc -w -fpreprocessed -dD -E -P`, which strips
# comments and expands nothing. Each preprocessor line is dropped with its
# backslash continuations, so the code of every #if branch is scanned, but
# the stack of open conditionals is kept: #if, #ifdef and #ifndef push their
# directive (whitespace collapsed), #elif and #else replace the top with
# "#elif ..." or "#else of ...", #endif pops. The rest is joined into one
# string, the inside of every string and character literal is blanked, and
# each call of tick_stop, clock_out_toggle and clock_out_return_internal
# (whole word, spaces allowed before the parenthesis) is split into its
# arguments at top-level commas, all whitespace dropped. If the braces do not
# balance, the file fails and no call is checked. The rules, by spelling:
#   - tick_stop, 4 arguments: the gate (the fourth) is
#     clock_out_active(&a->clock_out,a->clock_source), passed inline or as a
#     local declared once from it (below);
#   - clock_out_toggle, 7 arguments: src (the second) is a->clock_source; now
#     (the fourth) is a local declared once from stm_now(); e (the fifth) is
#     stm_sec(stm_diff(<now>,a->clock_out.clock))+a->clock_out.accum_secs,
#     <now> the fourth argument, inline or as a local declared once from it;
#     playing (the sixth) is a->is_playing;
#   - clock_out_return_internal, 5 arguments: now (the third) is stm_now()
#     and playing (the fourth) is a->is_playing;
#   - every tick_stop and clock_out_toggle call sits under no conditional,
#     and every clock_out_return_internal call under exactly
#     #ifdef BORCA_HAS_MIDI_IN, so a call compiled out never counts;
#   - each helper has at least one call, every call found is checked, and a
#     helper name used any other way (in a #define, its address taken, a
#     parenthesised (tick_stop)(...)) fails where it is used.
# A local declared once from x: the block of the call (the text from the {
# of the innermost block holding it up to the call), at its own brace depth
# (nested {...} skipped), holds exactly one assignment to it, and that is a
# declaration with an initializer (only type words since the previous ; {
# or }), under the same conditionals as the call, from x; and the name is
# assigned nowhere else in the enclosing function. An assignment to v is v
# (not after an identifier character, . or >), optional whitespace, then =,
# a compound op= or <<= or >>= (never ==), or v++, v--, ++v or --v; its
# right-hand side runs to its top-level ; or ,. A call is reported once, by
# its first failing rule (conditionals, argument count, then the arguments
# in order). The spellings are pinned, so renaming a, now, e or the fields
# fails the check loudly, never passes it; whoever renames them updates the
# spellings here and the canary in the same commit.
#
# The canary, tests/shell-args/canary.c, is never compiled. It is scanned
# first, the same way as tui_main.c but without the no-call rule, and its
# lines are not printed. Each of the functions in canary_bad below (spelled
# out here, never read from the file) plants one mistake and must fail
# exactly once; canary_ok must pass its three correct calls with no FAIL,
# although it holds a wrong call in a comment and in a string literal, each
# with a {, and the character literals '{' and '"' before its calls; no
# other function may appear. Two fixtures are scanned with the no-call rule:
# tests/shell-args/nocall.c must give exactly the three no-call FAILs, and
# tests/shell-args/unbalanced.c exactly the unbalanced-braces FAIL.
# Otherwise the check is blind and the run fails.
#
# Known edges: an assignment through a pointer or a macro is not seen; a gate
# held in a struct field is refused, not followed; only tui_main.c is
# scanned, so a call added to another shell file is not checked, but the
# no-call rule fails if the call leaves tui_main.c; a declaration of a helper
# inside tui_main.c reads as a call and fails; the conditionals are pinned by
# the directive's spelling, so #if defined(BORCA_HAS_MIDI_IN), or a call
# moved under another guard, fails until the script is updated with it.
# Correct code can fail loudly: a local declared outside the call's block,
# one declared after another declarator (bool x = y, clock_out = ...) and a
# name reused in another block of the same function. Not pinned: the sink
# and the other pointer arguments (&a->clock_out, &a->susnote_list,
# &a->ccout), bpm, and the calls of clock_out_on_start and
# clock_out_on_deadline, which this check does not read.
#
# Exit status: 0 when the canary is detected, no line says FAIL and every
# helper has a call; 1 otherwise; 2 on usage.
set -uf

root=$(cd "$(dirname "$0")/.." && pwd)
if [ "$#" != 0 ]; then
  echo "usage: $0" >&2
  exit 2
fi
shell_rel=tui_main.c
canary_rel=tests/shell-args/canary.c
nocall_rel=tests/shell-args/nocall.c
unbalanced_rel=tests/shell-args/unbalanced.c
# The canary's bad functions, one planted mistake each, spelled out here and
# never read from the canary.
canary_bad='canary_gate_enabled canary_gate_direct_enabled canary_gate_internal
canary_gate_reassigned canary_gate_param canary_e_no_carry canary_e_since
canary_now_call canary_now_stale canary_playing_true canary_playing_enabled
canary_call_if0 canary_gate_outer_default canary_gate_if0_decl
canary_gate_if_unbraced canary_gate_nested_decl canary_gate_after_call
canary_now_postinc canary_e_predec canary_call_in_define canary_address_taken
canary_call_parenthesised canary_toggle_src_internal canary_toggle_playing_true
canary_return_now_stale canary_return_unguarded canary_return_else_arm
canary_return_elif canary_gate_compound_only canary_return_four_args'

if ! command -v gcc >/dev/null 2>&1; then
  echo "error: gcc not found (the comment stripping needs gcc)" >&2
  exit 1
fi

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' INT TERM

# scan <file> <name> <nocall> <out>: one PASS or FAIL line per call or other
# use of the three helpers in <file> (relative to root), shown as <name>, to
# <out>; with <nocall> 1, a helper with no call fails too. Returns 1 if gcc
# cannot read the file or awk fails. The awk program holds no single quote.
scan() {
  if ! gcc -w -fpreprocessed -dD -E -P "$root/$1" >"$tmp/pp" 2>"$tmp/err"; then
    printf 'FAIL  %s: gcc could not read it\n' "$2" >"$4"
    head -n 3 "$tmp/err" | sed 's/^/      /' >>"$4"
    return 1
  fi
  if ! awk -v file="$2" -v nocall="$3" '
    function nows(s) { gsub(/[ \t\r\f]+/, "", s); return s }
    function isident(s) { return s ~ /^[A-Za-z_][A-Za-z0-9_]*$/ }
    function spaces(n) { while (length(SP) < n) SP = SP SP " "; return substr(SP, 1, n) }
    function show(stk) { return stk == "" ? "none" : stk }
    function pass(fn, msg) { printf "PASS  %s %s: %s\n", file, fn, msg }
    function fail(fn, msg) { printf "FAIL  %s %s: %s\n", file, fn, msg }

    # A directive (continuations joined). A #define naming a helper is kept
    # for a FAIL; otherwise only the conditionals matter, and the stack after
    # one applies from text position TL on.
    function directive(d,   kw, i, s, h) {
      gsub(/[ \t\r\f]+/, " ", d); sub(/^ /, "", d); sub(/ $/, "", d)
      sub(/^# /, "#", d)
      kw = substr(d, 2); sub(/[^A-Za-z].*/, "", kw)
      if (kw == "define") {
        for (h = 1; h <= 3; h++)
          if (match(" " d " ", "[^A-Za-z0-9_]" H[h] "[^A-Za-z0-9_]")) {
            ND++; DPOS[ND] = TL + 1; DHELP[ND] = H[h]
            s = d; sub(/^#define /, "", s); sub(/[^A-Za-z0-9_].*/, "", s); DNAME[ND] = s
          }
        return
      }
      if (kw == "if" || kw == "ifdef" || kw == "ifndef") COND[++ncond] = d
      else if (kw ~ /^elif/) { if (ncond) COND[ncond] = d }
      else if (kw == "else") { if (ncond) COND[ncond] = "#else of " COND[ncond] }
      else if (kw == "endif") { if (ncond) ncond-- }
      else return
      s = ""
      for (i = 1; i <= ncond; i++) s = s (i > 1 ? " > " : "") COND[i]
      NCH++; CHPOS[NCH] = TL; CHSTK[NCH] = s
    }

    # The conditionals open at text position pos.
    function stackat(pos,   i) {
      for (i = NCH; i >= 1; i--) if (CHPOS[i] < pos) return CHSTK[i]
      return ""
    }

    # The line with the inside of each string and character literal blanked.
    # lit holds the quote of a literal still open at the end of the line, as
    # it would be in the joined text.
    function blank(s,   out, m, n, i, c) {
      out = ""
      for (;;) {
        if (lit == "") {
          m = match(s, QUOTE)
          if (m == 0) return out s
          lit = substr(s, m, 1)
          out = out substr(s, 1, m)
          s = substr(s, m + 1)
        }
        n = length(s)
        for (i = 1; i <= n; i++) {
          c = substr(s, i, 1)
          if (c == "\\") i++
          else if (c == lit) break
        }
        if (i > n) return out spaces(n)
        out = out spaces(i - 1) lit
        s = substr(s, i + 1)
        lit = ""
      }
    }

    # The lines joined, pairwise, so no long string is copied once per line.
    function joined(   n, i, j) {
      n = NL
      if (n == 0) return ""
      while (n > 1) {
        j = 0
        for (i = 1; i <= n; i += 2) { j++; LINE[j] = (i < n) ? LINE[i] LINE[i + 1] : LINE[i] }
        n = j
      }
      return LINE[1]
    }

    # The arguments of the call whose ( is at p, without whitespace, in ARG.
    function args(p,   d, i, c, n, st, L) {
      d = 0; n = 0; st = p + 1; L = length(T)
      for (i = p; i <= L; i++) {
        c = substr(T, i, 1)
        if (c == "(" || c == "[" || c == "{") d++
        else if (c == ")" || c == "]" || c == "}") {
          if (--d == 0) { ARG[++n] = nows(substr(T, st, i - st)); return n }
        } else if (c == "," && d == 1) { ARG[++n] = nows(substr(T, st, i - st)); st = i + 1 }
      }
      return -1
    }

    # Whether the v at position i of t is declared there: only type words
    # (no statement keyword) since the previous ; { or }.
    function isdecl(t, i,   j, c, w, n, W, x) {
      for (j = i - 1; j >= 1; j--) {
        c = substr(t, j, 1)
        if (c == ";" || c == "{" || c == "}") break
      }
      w = substr(t, j + 1, i - j - 1)
      gsub(/^[ \t]+|[ \t]+$/, "", w)
      if (w !~ /^[A-Za-z_][A-Za-z0-9_]*([ \t]+[A-Za-z_][A-Za-z0-9_]*)*$/) return 0
      n = split(w, W, /[ \t]+/)
      for (x = 1; x <= n; x++) if (W[x] in NOTTYPE) return 0
      return 1
    }

    # Whether ++ or -- comes before the v at position i of t; PREOP holds it.
    function preinc(t, i,   j) {
      for (j = i - 1; j >= 1 && substr(t, j, 1) ~ /[ \t]/; j--) { }
      PREOP = substr(t, j - 1, 2)
      return j >= 2 && (PREOP == "++" || PREOP == "--")
    }

    # The assignments to v in s, which starts at position base of T: their
    # count; for the k-th, its position in T (AP), whether it is a
    # declaration with an initializer (AD) and its right-hand side without
    # whitespace (AR), after its operator when that is not =, or the
    # increment itself. ARHS is the last one.
    function assigns(v, s, base,   t, re, cnt, m, i, j, k, post, op, rest, d, c, L) {
      t = s " "; cnt = 0; ARHS = "none"; k = 0
      re = "[^A-Za-z0-9_.>]" v "[^A-Za-z0-9_]"
      while ((m = match(substr(t, k + 1), re)) > 0) {
        i = k + m + 1; k = i + length(v) - 1
        post = substr(t, k + 1)
        if (match(post, /^[ \t]*(<<|>>|[-+*\/%&|^])?=/) && substr(post, RLENGTH + 1, 1) != "=") {
          op = nows(substr(post, 1, RLENGTH))
          rest = substr(post, RLENGTH + 1)
          d = 0; L = length(rest)
          for (j = 1; j <= L; j++) {
            c = substr(rest, j, 1)
            if (c == "(" || c == "[" || c == "{") d++
            else if (c == ")" || c == "]" || c == "}") { if (--d < 0) break }
            else if ((c == ";" || c == ",") && d == 0) break
          }
          AR[++cnt] = (op == "=" ? "" : op) nows(substr(rest, 1, j - 1))
          AD[cnt] = (op == "=" && isdecl(t, i))
        } else if (match(post, /^[ \t]*(\+\+|--)/)) {
          AR[++cnt] = v nows(substr(post, 1, RLENGTH)); AD[cnt] = 0
        } else if (preinc(t, i)) {
          AR[++cnt] = PREOP v; AD[cnt] = 0
        } else continue
        AP[cnt] = base + i - 1
        ARHS = AR[cnt]
      }
      return cnt
    }

    # The position of the } that closes the { of brace event oi.
    function closeof(oi,   d, i) {
      d = 0
      for (i = oi; i <= NB; i++) {
        if (BC[i] > 0) d++
        else if (--d == 0) return BP[i]
      }
      return length(T)
    }

    # The block that opens at brace event bi (position ib) up to position at,
    # with the inside of each nested {...} blanked, so only its own depth
    # is left.
    function flat(bi, ib, at,   out, from, d, i, st) {
      out = ""; from = ib; d = 0
      for (i = bi + 1; i <= NB && BP[i] < at; i++) {
        if (BC[i] > 0) { if (d++ == 0) st = BP[i] }
        else if (--d == 0) { out = out substr(T, from, st + 1 - from) spaces(BP[i] - st - 1); from = BP[i] }
      }
      return out substr(T, from, at - from)
    }

    # The position of the outermost { open at pos, or 0.
    function outerat(pos,   i, top, first) {
      top = 0; first = 0
      for (i = 1; i <= NB && BP[i] < pos; i++) {
        if (BC[i] > 0) { if (top++ == 0) first = BP[i] }
        else top--
      }
      return top ? first : 0
    }

    # The name of the top-level function whose body opens at ob: the
    # identifier before the last top-level ( since the previous top-level ;
    # or }.
    function funcname(ob,   tb, i, h, d, c, lp) {
      if (!ob) return "(file-scope)"
      tb = 0
      for (i = 1; i <= NT && TOP[i] < ob; i++) tb = TOP[i]
      h = substr(T, tb + 1, ob - tb - 1)
      d = 0; lp = 0
      for (i = 1; i <= length(h); i++) {
        c = substr(h, i, 1)
        if (c == "(") { if (d == 0) lp = i; d++ }
        else if (c == ")") d--
      }
      if (!lp) return "(file-scope)"
      h = substr(h, 1, lp - 1); sub(/[ \t]+$/, "", h)
      if (!match(h, /[A-Za-z_][A-Za-z0-9_]*$/)) return "(file-scope)"
      return substr(h, RSTART)
    }

    # Why v is not a local declared once from want, or "" when it is: one
    # assignment at the depth of the block before the call, a declaration,
    # under the conditionals stk of the call, from want, and no other
    # assignment in the function.
    function follow(v, want, stk,   n, nf, last) {
      nf = assigns(v, FTEXT, FBASE); last = ARHS
      n = assigns(v, FLAT, IB)
      if (n != 1 || nf != 1)
        return " with " n " assignment" (n == 1 ? "" : "s") " at its block depth before the call and " nf " in its function" (nf ? " (the last " last ")" : "")
      if (!AD[1]) return ", assigned without a declaration (" AR[1] ")"
      if (stackat(AP[1]) != stk) return ", declared under " show(stackat(AP[1])) ", the call under " show(stk)
      if (AR[1] != want) return ", declared from " AR[1]
      return ""
    }

    # One call of name at position at, its ( at paren: one PASS or FAIL.
    function check(name, at, paren,   i, top, fn, stk, want, n, g, why, nw, e, we) {
      top = 0
      for (i = 1; i <= NB && BP[i] < at; i++) {
        if (BC[i] > 0) { OPEN[++top] = BP[i]; OPENI[top] = i }
        else top--
      }
      IB = top ? OPEN[top] : 1
      fn = funcname(top ? OPEN[1] : 0)
      FLAT = flat(top ? OPENI[top] : 0, IB, at)
      FBASE = top ? OPEN[1] : 1
      FTEXT = top ? substr(T, FBASE, closeof(OPENI[1]) - FBASE + 1) : ""

      stk = stackat(at)
      want = (name == "clock_out_return_internal") ? "#ifdef BORCA_HAS_MIDI_IN" : ""
      if (stk != want)
        return fail(fn, name Q "s conditionals are " show(stk) ", expected " show(want))

      n = args(paren)
      if (name == "tick_stop") {
        if (n != 4) return fail(fn, name Q "s argument count is " n ", expected 4")
        g = ARG[4]
        if (g == PRED) return pass(fn, name Q "s gate is " PRED ", inline")
        if (!isident(g))
          return fail(fn, name Q "s gate is " g ", expected " PRED ", inline or from " LOCAL)
        why = follow(g, PRED, stk)
        if (why == "") return pass(fn, name Q "s gate is " g ", declared from " PRED)
        return fail(fn, name Q "s gate is " g why ", expected " LOCAL ", from " PRED)
      }
      if (name == "clock_out_toggle") {
        if (n != 7) return fail(fn, name Q "s argument count is " n ", expected 7")
        if (ARG[2] != "a->clock_source")
          return fail(fn, name Q "s src is " ARG[2] ", expected a->clock_source")
        nw = ARG[4]; e = ARG[5]
        if (!isident(nw))
          return fail(fn, name Q "s now is " nw ", expected " LOCAL ", from stm_now()")
        why = follow(nw, "stm_now()", stk)
        if (why != "") return fail(fn, name Q "s now is " nw why ", expected " LOCAL ", from stm_now()")
        we = "stm_sec(stm_diff(" nw ",a->clock_out.clock))+a->clock_out.accum_secs"
        if (e != we) {
          if (!isident(e))
            return fail(fn, name Q "s e is " e ", expected " we ", inline or from " LOCAL)
          why = follow(e, we, stk)
          if (why != "") return fail(fn, name Q "s e is " e why ", expected " LOCAL ", from " we)
        }
        if (ARG[6] != "a->is_playing")
          return fail(fn, name Q "s playing is " ARG[6] ", expected a->is_playing")
        return pass(fn, name Q "s src is a->clock_source, now is " nw ", declared from stm_now(), e is " \
          (e == we ? we ", inline" : e ", declared from " we) ", and playing is a->is_playing")
      }
      if (n != 5) return fail(fn, name Q "s argument count is " n ", expected 5")
      if (ARG[3] != "stm_now()") return fail(fn, name Q "s now is " ARG[3] ", expected stm_now()")
      if (ARG[4] != "a->is_playing")
        return fail(fn, name Q "s playing is " ARG[4] ", expected a->is_playing")
      return pass(fn, name Q "s now is stm_now() and playing is a->is_playing, under #ifdef BORCA_HAS_MIDI_IN")
    }

    BEGIN {
      QUOTE = "[\"\047]"; Q = "\047"
      PRED = "clock_out_active(&a->clock_out,a->clock_source)"
      LOCAL = "a local declared once in its block"
      split("tick_stop clock_out_toggle clock_out_return_internal", H, " ")
      n = split("if else while for do switch case default return goto break continue sizeof", W, " ")
      for (i = 1; i <= n; i++) NOTTYPE[W[i]] = 1
      lit = ""; indir = 0; TL = 0; NL = 0; NB = 0; NT = 0; NCH = 0; ND = 0; ncond = 0
      depth = 0; neg = 0
    }
    {
      line = $0
      if (indir || line ~ /^[ \t]*#/) {
        cont = (line ~ /\\$/)
        if (cont) sub(/\\$/, "", line)
        dir = (indir ? dir " " : "") line
        indir = cont
        if (!indir) directive(dir)
        next
      }
      b = blank(line)
      # Braces and top-level semicolons, by position in the joined text,
      # where b starts at TL + 2, after the joining space.
      s = b; k = 0
      while ((m = match(s, /[{};]/)) > 0) {
        k += m; c = substr(s, m, 1); s = substr(s, m + 1)
        if (c == "{") { BP[++NB] = TL + 1 + k; BC[NB] = 1; depth++ }
        else if (c == "}") {
          BP[++NB] = TL + 1 + k; BC[NB] = -1
          if (--depth < 0) neg = 1
          if (depth == 0) TOP[++NT] = TL + 1 + k
        } else if (depth == 0) TOP[++NT] = TL + 1 + k
      }
      LINE[++NL] = " " b
      TL += 1 + length(b)
    }
    END {
      if (indir) directive(dir)
      if (neg || depth != 0) {
        printf "FAIL  %s: unbalanced braces (%s), no call checked\n", file, \
          (neg ? "a } closes no block" : depth " { left open at the end")
        exit 0
      }
      T = joined(); U = T " "
      for (h = 1; h <= 3; h++) {
        name = H[h]; found = 0; k = 0
        re = "[^A-Za-z0-9_]" name "[^A-Za-z0-9_]"
        while ((m = match(substr(U, k + 1), re)) > 0) {
          at = k + m + 1; k = at + length(name) - 1
          if (match(substr(U, k + 1), /^[ \t]*[(]/)) {
            found++
            check(name, at, k + RLENGTH)
          } else
            fail(funcname(outerat(at)), name Q "s name is used without a call, expected direct calls only")
        }
        for (i = 1; i <= ND; i++)
          if (DHELP[i] == name)
            fail(funcname(outerat(DPOS[i])), name " is in #define " DNAME[i] ", expected direct calls only")
        if (!found && nocall) printf "FAIL  %s: no call to %s\n", file, name
      }
    }
  ' "$tmp/pp" >"$4"; then
    printf 'FAIL  %s: awk failed\n' "$2" >>"$4"
    return 1
  fi
}

# oneline <file>: its lines joined with " | ".
oneline() { awk '{ printf "%s%s", (NR > 1 ? " | " : ""), $0 }' "$1"; }

# The canary: each bad function fails exactly once, canary_ok passes three
# times and fails never, and no other function appears; the verdict is "ok"
# or the reasons the check is blind, so an awk that prints nothing fails.
# Then the fixtures. A scan gcc or awk cannot finish leaves its lines in
# $tmp/captured, printed after the FAIL  canary line.
canary_ok=0
blind=
: >"$tmp/captured"
if scan "$canary_rel" canary.c 0 "$tmp/canary"; then
  verdict=$(awk -v bad="$canary_bad" '
    function why(s) { W = W (W == "" ? "" : "; ") s }
    BEGIN { nb = split(bad, B, /[ \n]+/); for (i = 1; i <= nb; i++) listed[B[i]] = 1 }
    $2 == "canary.c:" { w = $0; sub(/^[A-Z]+  /, "", w); why(w); next }
    ($1 == "PASS" || $1 == "FAIL") && $2 == "canary.c" {
      fn = $3; sub(/:$/, "", fn); seen[fn] = 1
      if ($1 == "PASS") np[fn]++; else nf[fn]++
      next
    }
    NF { why("unexpected line: " $0) }
    END {
      for (i = 1; i <= nb; i++) {
        f = B[i]
        if (nf[f] + 0 != 1 || np[f] + 0 != 0)
          why(f " not rejected once (" (nf[f] + 0) " FAIL, " (np[f] + 0) " PASS)")
      }
      if (np["canary_ok"] + 0 != 3 || nf["canary_ok"] + 0 != 0)
        why("canary_ok " (np["canary_ok"] + 0) " PASS, " (nf["canary_ok"] + 0) " FAIL, expected 3 PASS")
      for (f in seen) if (!(f in listed) && f != "canary_ok") why(f " reported but not listed")
      printf "%s", (W == "" ? "ok" : W)
    }
  ' "$tmp/canary")
  [ "$verdict" = ok ] || blind=$verdict
else
  blind="could not scan $canary_rel"
  sed 's/^/      /' "$tmp/canary" >>"$tmp/captured"
fi
want=$(printf 'FAIL  nocall.c: no call to %s\n' tick_stop clock_out_toggle \
  clock_out_return_internal)
if scan "$nocall_rel" nocall.c 1 "$tmp/nocall"; then
  if [ "$(cat "$tmp/nocall")" != "$want" ]; then
    blind="$blind${blind:+; }nocall.c gave [$(oneline "$tmp/nocall")], expected the three no-call FAILs"
  fi
else
  blind="$blind${blind:+; }could not scan $nocall_rel"
  sed 's/^/      /' "$tmp/nocall" >>"$tmp/captured"
fi
if scan "$unbalanced_rel" unbalanced.c 1 "$tmp/unbalanced"; then
  if [ "$(grep -c '' "$tmp/unbalanced")" != 1 ] ||
    ! grep -q '^FAIL  unbalanced\.c: unbalanced braces' "$tmp/unbalanced"; then
    blind="$blind${blind:+; }unbalanced.c gave [$(oneline "$tmp/unbalanced")], expected one unbalanced-braces FAIL"
  fi
else
  blind="$blind${blind:+; }could not scan $unbalanced_rel"
  sed 's/^/      /' "$tmp/unbalanced" >>"$tmp/captured"
fi
if [ -z "$blind" ]; then
  # shellcheck disable=SC2086  # one name per word
  printf "PASS  canary (rejected: %s; accepted: canary_ok's 3 calls; fixtures: nocall.c's 3 no-call FAILs, unbalanced.c's unbalanced braces)\n" \
    "$(printf '%s\n' $canary_bad | paste -sd, - | sed 's/,/, /g')"
  canary_ok=1
else
  printf 'FAIL  canary (the check is blind: %s)\n' "$blind"
  cat "$tmp/captured"
fi

scan "$shell_rel" "$shell_rel" 1 "$tmp/shell"
cat "$tmp/shell"
n_pass=$(grep -c '^PASS  tui_main\.c [^ :]*:' "$tmp/shell")
n_fail=$(grep -c '^FAIL  tui_main\.c [^ :]*:' "$tmp/shell")
n_file=$(grep -c '^FAIL  tui_main\.c:' "$tmp/shell")
file_fail=
if [ "$n_file" != 0 ]; then
  file_fail=", $n_file file-level fail"
fi
if [ "$canary_ok" = 1 ]; then
  canary_result="canary detected"
else
  canary_result="canary NOT detected"
fi
printf 'shell-args check: %s calls, %s pass, %s fail%s; %s\n' \
  "$((n_pass + n_fail))" "$n_pass" "$n_fail" "$file_fail" "$canary_result"
[ "$canary_ok" = 1 ] && ! grep -q '^FAIL' "$tmp/shell"
