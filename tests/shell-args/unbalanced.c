// A fixture for tests/check-shell-args.sh (P4-3), never compiled. Each arm of
// its #if opens a {, so once the directives are dropped one { is left open.
// The check scans it with the no-call rule: it must give exactly the
// unbalanced-braces FAIL, or the check is blind and the run fails.

#if defined(BORCA_HAS_MIDI_IN)
static void unbalanced_arms(Ged *a) {
#else
static void unbalanced_arms(Ged *a, int unused) {
#endif
  Tick_sink const sink = ged_sink(a);
  tick_stop(&sink, &a->susnote_list, &a->ccout,
            clock_out_active(&a->clock_out, a->clock_source));
}
