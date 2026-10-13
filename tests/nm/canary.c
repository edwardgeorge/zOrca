// The canary for tests/check-nm.sh. The check must flag each of the four
// symbols below by name on every run; if it misses one, the check is blind
// to that symbol class and fails.
//
// One writable symbol per class: a zero-initialised static (b), an
// initialised static (d), a zero-initialised external (B; the explicit
// initialiser keeps it out of common under -fcommon) and an initialised
// external (D). canary_address returns each one's address, so -O2 keeps
// them all: an unused or read-only static is dropped, and the check would then
// fail the object only for being empty. No build target links this file.
//
// A fifth symbol, canary_O0_only, is the -O0 pass's own canary: a non-const
// table that is never written and whose address never escapes, read once at
// a constant index. At -O2 gcc folds the read and drops the table, so it is
// not writable there; at -O0 it is d. The check requires exactly that, so a
// second pass that optimises like the first cannot pass.
extern int canary_extern_zero;
extern int canary_extern_init;
int *canary_address(int which);
int canary_read_O0_only(void);

static int canary_static_zero = 0;
static int canary_static_init = 1;
int canary_extern_zero = 0;
int canary_extern_init = 1;
static int canary_O0_only[4] = {1, 2, 3, 4};

int *canary_address(int which) {
  switch (which) {
  case 0:
    return &canary_static_zero;
  case 1:
    return &canary_static_init;
  case 2:
    return &canary_extern_zero;
  default:
    return &canary_extern_init;
  }
}

int canary_read_O0_only(void) { return canary_O0_only[2]; }
