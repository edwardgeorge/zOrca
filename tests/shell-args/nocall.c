// A fixture for tests/check-shell-args.sh, never compiled. It holds
// no call of the three helpers, only names that start like them, and the
// check scans it with the no-call rule: it must give exactly the three
// "no call to" FAILs, or the check is blind and the run fails.

static void nocall_near_misses(Ged *a) {
  Tick_sink const sink = ged_sink(a);
  tick_stop_soon(&sink);
  clock_out_toggled(&a->clock_out);
  clock_out_return_internally(&a->clock_out, a->is_playing);
}
