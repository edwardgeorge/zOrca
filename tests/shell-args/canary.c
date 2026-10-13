// The canary for tests/check-shell-args.sh (P4-3). It is never compiled: no
// build target, tool list or include names it, and it would not compile on
// its own. The check scans it first, the same way as tui_main.c, and on every
// run each function below but canary_ok must fail exactly once, through the
// one mistake it plants, while canary_ok's three correct calls pass. If one
// does not, the check is blind to that mistake and the run fails. The
// functions keep tui_main.c's spellings (Ged *a, now, e, sink), and every
// clock_out_return_internal call sits under #ifdef BORCA_HAS_MIDI_IN, as the
// shell's does, unless its conditionals are the planted mistake, so each bad
// function fails only through its mistake. A function added here is added
// to the script's canary_bad list too.

// tick_stop's gate from the stored setting: FC echoes to an external master.
static void canary_gate_enabled(Ged *a) {
  bool clock_out = a->clock_out.enabled;
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
}

// The stored setting passed as the gate.
static void canary_gate_direct_enabled(Ged *a) {
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, a->clock_out.enabled);
}

// The predicate with the source fixed to Internal.
static void canary_gate_internal(Ged *a) {
  bool clock_out = clock_out_active(&a->clock_out, Clock_source_internal);
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
}

// The right local, then widened by the stored setting.
static void canary_gate_reassigned(Ged *a) {
  bool clock_out = clock_out_active(&a->clock_out, a->clock_source);
  clock_out |= a->clock_out.enabled;
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
}

// The gate a parameter, never assigned in the block.
static void canary_gate_param(Ged *a, bool clock_out) {
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
}

// e without the carry: the next tick moves after a toggle.
static void canary_e_no_carry(Ged *a) {
  U64 now = stm_now();
  double e = stm_sec(stm_diff(now, a->clock_out.clock));
  Tick_sink const sink = ged_sink(a);
  clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, now, e,
                   a->is_playing, &sink);
}

// e from another instant than the now passed.
static void canary_e_since(Ged *a) {
  U64 now = stm_now();
  double e = stm_sec(stm_since(a->clock_out.clock)) + a->clock_out.accum_secs;
  Tick_sink const sink = ged_sink(a);
  clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, now, e,
                   a->is_playing, &sink);
}

// A second stm_now() passed as now.
static void canary_now_call(Ged *a) {
  U64 now = stm_now();
  double e = stm_sec(stm_diff(now, a->clock_out.clock)) + a->clock_out.accum_secs;
  Tick_sink const sink = ged_sink(a);
  clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, stm_now(), e,
                   a->is_playing, &sink);
}

// now not the present instant.
static void canary_now_stale(Ged *a) {
  U64 now = a->clock_out.clock;
  double e = stm_sec(stm_diff(now, a->clock_out.clock)) + a->clock_out.accum_secs;
  Tick_sink const sink = ged_sink(a);
  clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, now, e,
                   a->is_playing, &sink);
}

#ifdef BORCA_HAS_MIDI_IN
// FB while stopped.
static void canary_playing_true(Ged *a) {
  Tick_sink const sink = ged_sink(a);
  clock_out_return_internal(&a->clock_out, a->bpm, stm_now(), true, &sink);
}

// The stored setting in place of is_playing.
static void canary_playing_enabled(Ged *a) {
  Tick_sink const sink = ged_sink(a);
  clock_out_return_internal(&a->clock_out, a->bpm, stm_now(),
                            a->clock_out.enabled, &sink);
}
#endif

// A correct call compiled out.
static void canary_call_if0(Ged *a) {
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
#if 0
  tick_stop(&sink, &a->susnote_list, &a->ccout,
            clock_out_active(&a->clock_out, a->clock_source));
#endif
}

// An outer default from the stored setting, the predicate only on one path
// (the review's p-outer-init shape).
static void canary_gate_outer_default(Ged *a, bool playing) {
  bool clock_out = a->clock_out.enabled;
  if (playing) {
    a->is_playing = true;
  } else {
    if (a->clock_source == Clock_source_internal) {
      clock_out = clock_out_active(&a->clock_out, a->clock_source);
    }
    a->is_playing = false;
    Tick_sink const sink = ged_sink(a);
    tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
  }
}

// The right declaration compiled out: the gate is the parameter.
static void canary_gate_if0_decl(Ged *a, bool clock_out) {
  a->is_playing = false;
#if 0
  bool clock_out = clock_out_active(&a->clock_out, a->clock_source);
#endif
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
}

// The predicate assigned only on one path, by an if without braces.
static void canary_gate_if_unbraced(Ged *a) {
  bool clock_out;
  if (a->clock_source == Clock_source_internal)
    clock_out = clock_out_active(&a->clock_out, a->clock_source);
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
}

// The right declaration in a nested block that closes before the call: the
// gate is the parameter.
static void canary_gate_nested_decl(Ged *a, bool clock_out) {
  a->is_playing = false;
  {
    bool clock_out = clock_out_active(&a->clock_out, a->clock_source);
    (void)clock_out;
  }
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
}

// The gate reassigned after the call, which a goto runs again.
static void canary_gate_after_call(Ged *a, int times) {
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  bool clock_out = clock_out_active(&a->clock_out, a->clock_source);
again:
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
  clock_out = a->clock_out.enabled;
  if (--times > 0)
    goto again;
}

// now bumped after its declaration.
static void canary_now_postinc(Ged *a) {
  U64 now = stm_now();
  now++;
  double e = stm_sec(stm_diff(now, a->clock_out.clock)) + a->clock_out.accum_secs;
  Tick_sink const sink = ged_sink(a);
  clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, now, e,
                   a->is_playing, &sink);
}

// e lowered after its declaration.
static void canary_e_predec(Ged *a) {
  U64 now = stm_now();
  double e = stm_sec(stm_diff(now, a->clock_out.clock)) + a->clock_out.accum_secs;
  --e;
  Tick_sink const sink = ged_sink(a);
  clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, now, e,
                   a->is_playing, &sink);
}

// A call through a macro, whose body the scan drops.
static void canary_call_in_define(Ged *a) {
#define CANARY_STOP(s) tick_stop((s), &a->susnote_list, &a->ccout, a->clock_out.enabled)
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  CANARY_STOP(&sink);
#undef CANARY_STOP
}

// The helper called through a pointer.
static void canary_address_taken(Ged *a) {
  void (*stop)(Tick_sink const *, Susnote_list *, Ccout_engine *, bool) =
      tick_stop;
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  stop(&sink, &a->susnote_list, &a->ccout, a->clock_out.enabled);
}

// The helper's name in parentheses, which no call pattern matches.
static void canary_call_parenthesised(Ged *a) {
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  (tick_stop)(&sink, &a->susnote_list, &a->ccout, a->clock_out.enabled);
}

// The toggle told the source is Internal: FB or FC under External.
static void canary_toggle_src_internal(Ged *a) {
  U64 now = stm_now();
  double e = stm_sec(stm_diff(now, a->clock_out.clock)) + a->clock_out.accum_secs;
  Tick_sink const sink = ged_sink(a);
  clock_out_toggle(&a->clock_out, Clock_source_internal, a->bpm, now, e,
                   a->is_playing, &sink);
}

// The toggle told it is playing: FB or FC while stopped.
static void canary_toggle_playing_true(Ged *a) {
  U64 now = stm_now();
  double e = stm_sec(stm_diff(now, a->clock_out.clock)) + a->clock_out.accum_secs;
  Tick_sink const sink = ged_sink(a);
  clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, now, e, true, &sink);
}

#ifdef BORCA_HAS_MIDI_IN
// The re-arm from the last deadline, not the present.
static void canary_return_now_stale(Ged *a) {
  Tick_sink const sink = ged_sink(a);
  clock_out_return_internal(&a->clock_out, a->bpm, a->clock_out.clock,
                            a->is_playing, &sink);
}

// Four arguments, the sink dropped.
static void canary_return_four_args(Ged *a) {
  clock_out_return_internal(&a->clock_out, a->bpm, stm_now(), a->is_playing);
}
#endif

// A correct call outside any conditional.
static void canary_return_unguarded(Ged *a) {
  Tick_sink const sink = ged_sink(a);
  clock_out_return_internal(&a->clock_out, a->bpm, stm_now(), a->is_playing,
                            &sink);
}

// A correct call in the #else arm of #ifdef BORCA_HAS_MIDI_IN.
static void canary_return_else_arm(Ged *a) {
  Tick_sink const sink = ged_sink(a);
#ifdef BORCA_HAS_MIDI_IN
  (void)sink;
#else
  clock_out_return_internal(&a->clock_out, a->bpm, stm_now(), a->is_playing,
                            &sink);
#endif
}

// A correct call in an #elif arm after #ifdef BORCA_HAS_MIDI_IN.
static void canary_return_elif(Ged *a) {
  Tick_sink const sink = ged_sink(a);
#ifdef BORCA_HAS_MIDI_IN
  (void)sink;
#elif defined(BORCA_HAS_MIDI_OUT)
  clock_out_return_internal(&a->clock_out, a->bpm, stm_now(), a->is_playing,
                            &sink);
#endif
}

// The gate's only assignment compound.
static void canary_gate_compound_only(Ged *a) {
  bool clock_out;
  clock_out |= clock_out_active(&a->clock_out, a->clock_source);
  a->is_playing = false;
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout, clock_out);
}

// One correct call of each helper, the gate and e passed inline. The comment
// and the string literal each hold a wrong call and a {, and the character
// literals a { and a ", none of which may count.
static void canary_ok(Ged *a) {
  // tick_stop(&sink, &a->susnote_list, &a->ccout, a->clock_out.enabled); {
  char const *s = "tick_stop(&sink, &a->susnote_list, &a->ccout, true); {";
  char open = '{';
  char quote = '"';
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout,
            clock_out_active(&a->clock_out, a->clock_source));
  U64 now = stm_now();
  clock_out_toggle(&a->clock_out, a->clock_source, a->bpm, now,
                   stm_sec(stm_diff(now, a->clock_out.clock)) +
                       a->clock_out.accum_secs,
                   a->is_playing, &sink);
#ifdef BORCA_HAS_MIDI_IN
  clock_out_return_internal(&a->clock_out, a->bpm, stm_now(), a->is_playing,
                            &sink);
#endif
  (void)s;
  (void)open;
  (void)quote;
}
