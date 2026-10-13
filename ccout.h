#pragma once
#include "base.h"
#include "vmio.h"

// The CC engine: every CC from the tick path goes through it. It owns the
// glides, the last value sent on each controller, the glide_hz step limit, the
// de-duplication of glide steps and the aggregate budget, and it is the only
// place a `!` rate index maps to a duration. It is core: it reads no clock,
// does no I/O and keeps no writable globals. The shell holds one engine by
// value (Ged), and tests build theirs on the stack.
//
// Times are U64 microseconds of the shell's one clock (now_us() in
// tui_main.c). Output is Oevent_midi_cc items appended to the caller's list,
// and the shell sends every one: "sent" means emitted by the engine.
//
// Instants. An Oevent_midi_cc, or a `!` with rate index 0 or 35 (`0`, `z`),
// is instant: it always goes out, even when it equals the last value, updates
// the last value and cancels an active glide on its controller.
//
// Glides. Rates 1-34 glide from the current value to the target over
// (36 - rate) ticks: t_end = submit time + (36 - rate) * tick_len, fixed at
// submit, so a later tempo change does not alter it. The current value is
// the glide's interpolant if one is active, otherwise the last value sent.
// A retarget's start keeps Ccout_frac_bits fractional bits of the
// interpolant, floored, so it continues from the glide's progress to within
// 2^-16, not from a rounded value; a value is rounded half up, as 4f349cd's
// (U8)(v + 0.5), only when it goes out or is compared. In integer
// arithmetic, with d = t_end - t0 and el = now - t0 clamped to [0, d], the
// interpolant is start + floor((target - start) * el / d).
//
// A poll before t_end sends a step only if at least 1 000 000 / glide_hz us
// have passed since that controller's last emission (none if now is before
// it), the step's value differs from the last value, and the budget has a
// token for it. The first poll at or after t_end sends the target unless the
// last value already equals it; neither glide_hz nor the budget ever holds
// the target back. A glide ends at its first emission equal to its target,
// at t_end, or by a cancel, an instant, a retarget or a no-op submit.
//
// A glide submit takes the first of these that applies:
//   1. rate index 0 or 35: an instant;
//   2. the last value is unknown: the target goes out at once, as an instant
//      would, cancelling any active glide;
//   3. the same target and rate as the active glide: nothing changes, and
//      tick_len is ignored (the glide keeps its start and t_end);
//   4. the target equals the current value: a no-op. If the last value
//      differs from the target, because glide_hz or the budget held back the
//      step to it, the target goes out first, as a target emission; then any
//      active glide ends;
//   5. otherwise a new glide, or a retarget of the active one, from the
//      current value.
//
// The budget: a token bucket of Ccout_burst tokens that refills at
// Ccout_cap per second, deterministic from the times it is given. It starts
// full, and cancel, forget and configure leave it alone. It keeps its
// balance in token-us, 1 000 000 / Ccout_cap per token, so a refill is exact:
// it adds the us since the last refill, and a poll that emits nothing does
// not change when the next token arrives. A time earlier than the last
// refill adds nothing and never moves the refill time back. A step (a poll's
// emission before t_end) needs a token and is held back without one;
// instants and target emissions (a poll's target at or after t_end, and a
// no-op's catch-up) always go out and debit one, down to a floor of
// -Ccout_burst tokens, so an instant flood above the cap turns active glides
// into a jump at t_end. The cap is fixed; there is no setting for it.
//
// Debug builds assert ch <= 15, cc <= 127, values <= 127 and rate_idx <= 35.

enum {
  Ccout_frac_bits = 16, // the fractional bits of a glide's start
  Ccout_channels = 16,
  Ccout_controls = 128,
  Ccout_slots = Ccout_channels * Ccout_controls, // channel * 128 + cc
  Ccout_glide_hz_default = 100,
  Ccout_cap = 800,  // the budget's refill, in messages per second
  Ccout_burst = 80, // the budget's size in tokens, Ccout_cap / 10
};

typedef struct {
  U64 t0, t_end; // the glide's start and end, us
  U32 start;     // the start value, 0-127, with Ccout_frac_bits fraction bits
  U8 target;     // 0-127
  U8 rate;       // the rate index, 1-34
  bool active;
} Ccout_glide;

// Fixed size and no heap: about 67.6 KB. ccout_init sets every field.
typedef struct {
  Ccout_glide glides[Ccout_slots];        // indexed by channel * 128 + cc
  U64 last_emit[Ccout_slots];             // UINT64_MAX if none
  I8 last_value[Ccout_channels][Ccout_controls]; // -1 is unknown
  Usz active_count;                       // glides with active set
  Usz glide_hz;
  U64 step_gap; // ceil(1 000 000 / glide_hz): the shortest gap before a step
  // The budget's balance at refill_time, in token-us: from -Ccout_burst to
  // Ccout_burst tokens, 1 000 000 / Ccout_cap token-us each.
  I64 budget;
  U64 refill_time; // the latest time the budget refilled to, us
  Usz start_slot;  // the slot the next poll starts from
} Ccout_engine;

// No glide active, every last value unknown, no emission times, glide_hz at
// its default, 100, the budget full and the poll starting at slot 0.
void ccout_init(Ccout_engine *engine);

// Sets the per-controller step limit, in steps per second; 0 means the
// default, 100. It applies to active glides from the next poll, and leaves
// the budget alone. The shell configures its glide_hz setting at init and
// on every change of it.
void ccout_configure(Ccout_engine *engine, Usz glide_hz);

// The names orca.conf's glide_hz key takes: "50", "100" and "200", exactly,
// the three settings of the Glide Rate menu. False, leaving *out alone, for
// anything else.
bool ccout_glide_hz_from_name(char const *name, Usz *out);
// The name ccout_glide_hz_from_name reads as hz: "50", "100" or "200"; NULL
// for any other value.
char const *ccout_glide_hz_name(Usz hz);

// An instant CC: an Oevent_midi_cc, or a CCI at rate 0 or 35.
void ccout_submit_instant(Ccout_engine *engine, U8 ch, U8 cc, U8 value, U64 now,
                          Oevent_list *out);

// A CCI: the rules 1-5 above. tick_len is one tick in us; 0 counts as 1,
// and more than 2^32 us (about 71.6 minutes) counts as 2^32, which keeps the
// integer maths in range.
void ccout_submit_glide(Ccout_engine *engine, U8 ch, U8 cc, U8 target,
                        U8 rate_idx, U64 tick_len, U64 now, Oevent_list *out);

// Advances every active glide to now_us, emitting in slot order (channel,
// then cc) from the engine's start slot, wrapping around after the last. The
// start slot begins at 0 and moves only when a poll holds a step back for
// lack of a token: the first slot it held starts the next poll, so under
// contention no slot always waits, and without it the order stays put. A
// poll that emits nothing and ends no glide changes nothing else.
void ccout_poll(Ccout_engine *engine, U64 now_us, Oevent_list *out);

// False when no glide is active. Otherwise sets *t to the earliest time, never
// before now, at which ccout_poll would emit or end a glide: a poll at *t
// does, and a poll from now up to *t - 1 does neither, unless another engine
// call comes between. *t counts glide_hz, t_end, the next change of the
// rounded value and, for a step, the time the budget's next token arrives,
// which the poll reads from the same function.
bool ccout_next_deadline(Ccout_engine const *engine, U64 now, U64 *t);

// Sets every last value to unknown, so the next glide on each controller
// jumps to its target. Glides, emission times and the budget are left alone,
// so an active glide continues. Used when the output changes, and on open
// and new.
void ccout_forget(Ccout_engine *engine);

// Ends every active glide, keeping the last values, emission times and the
// budget. Used on pause, open and new.
void ccout_cancel(Ccout_engine *engine);
