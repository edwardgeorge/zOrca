// Unit tests for the held note-offs in midi_in.h: a note-off the sequencer
// refused is kept and sent again, so the note does not hang on the synth. The
// list is pure (the caller passes the time, and every write goes through a
// Midi_write), so these tests drive it with a fake write that refuses on cue
// and need no sequencer; they link libc alone.
#include "../../midi_in.h"
#include "tests.h"

// A fake write: records each message and answers from a script, then with a
// success (28, what a successful ALSA write returns) once the script is spent.
enum { Fake_max = 64 };
typedef struct {
  int calls;
  int status[Fake_max], d1[Fake_max], d2[Fake_max];
  int script[Fake_max]; // answers for the first calls; 0 means "success"
  int scripted;
  U64 time;
} Fake;

static int fake_write(void *u, int status, int d1, int d2) {
  Fake *f = u;
  int i = f->calls++;
  if (i < Fake_max) {
    f->status[i] = status;
    f->d1[i] = d1;
    f->d2[i] = d2;
  }
  return i < f->scripted && f->script[i] != 0 ? f->script[i] : 28;
}

static U64 fake_now(void *u) { return ((Fake *)u)->time; }

#define FAKE_WRITE(f) ((Midi_write){(f), fake_write, fake_now})

// An accepted message changes nothing and returns the write's own result.
void test_held_send_passes_a_message_through(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {0};
  Midi_write w = FAKE_WRITE(&f);
  CHECK(held_send(&p, &w, 0x90, 60, 100) == 28);
  CHECK(held_send(&p, &w, 0x80, 60, 0) == 28);
  CHECK(f.calls == 2 && p.count == 0 && p.held == 0);
}

// A refused note-off is kept with the channel and note it was for and the
// time of the refusal; the caller still gets the error.
void test_held_send_holds_a_refused_note_off(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {.scripted = 1, .script = {-EAGAIN}, .time = 5000};
  Midi_write w = FAKE_WRITE(&f);
  CHECK(held_send(&p, &w, 0x83, 60, 0) == -EAGAIN);
  CHECK(p.count == 1 && p.held == 1);
  CHECK(p.entries[0].chan == 3 && p.entries[0].note == 60);
  CHECK(p.entries[0].since_us == 5000);
}

// Only a note-off that the sequencer may take later is kept: a refused
// note-on is a missed note, a refused clock byte is gone, and an error that
// will not pass by itself (-EINVAL, -ENODEV) is not retried.
void test_held_send_holds_nothing_else(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {.scripted = 6,
            .script = {-EAGAIN, -EAGAIN, -EAGAIN, -EINVAL, -ENODEV, -EAGAIN}};
  Midi_write w = FAKE_WRITE(&f);
  CHECK(held_send(&p, &w, 0x90, 60, 100) == -EAGAIN); // note-on
  CHECK(held_send(&p, &w, 0x90, 60, 0) == -EAGAIN);   // note-on, velocity 0
  CHECK(held_send(&p, &w, 0xf8, 0, 0) == -EAGAIN);    // clock
  CHECK(held_send(&p, &w, 0x80, 60, 0) == -EINVAL);   // note-off, bad message
  CHECK(held_send(&p, &w, 0x80, 61, 0) == -ENODEV);   // note-off, device gone
  CHECK(held_send(&p, &w, 0xb0, 7, 100) == -EAGAIN);  // controller
  CHECK(p.count == 0 && p.held == 0);
}

// A note-on for the channel and note of a held note-off sends that note-off
// once more first, so the pair arrives in order, and the list is empty after.
void test_held_send_note_on_sends_the_held_off_first(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {.scripted = 1, .script = {-EAGAIN}};
  Midi_write w = FAKE_WRITE(&f);
  CHECK(held_send(&p, &w, 0x85, 40, 0) == -EAGAIN);
  CHECK(p.count == 1);
  CHECK(held_send(&p, &w, 0x95, 40, 99) == 28);
  CHECK(f.calls == 3);
  CHECK(f.status[1] == 0x85 && f.d1[1] == 40 && f.d2[1] == 0); // the off
  CHECK(f.status[2] == 0x95 && f.d1[2] == 40 && f.d2[2] == 99); // then the on
  CHECK(p.count == 0 && p.resent == 1 && p.lost == 0);
}

// If the held note-off is refused again at that moment it is dropped, not
// kept for later: it would end the note-on that follows. A refused note-on
// is not kept either.
void test_held_send_drops_a_held_off_it_cannot_send(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {.scripted = 3, .script = {-EAGAIN, -EAGAIN, -EAGAIN}};
  Midi_write w = FAKE_WRITE(&f);
  CHECK(held_send(&p, &w, 0x80, 36, 0) == -EAGAIN);
  CHECK(held_send(&p, &w, 0x90, 36, 80) == -EAGAIN);
  CHECK(f.calls == 3); // the off, the held off again, the on
  CHECK(p.count == 0 && p.lost == 1 && p.resent == 0);
}

// A note-on for another note, or the same note on another channel, leaves the
// held note-off alone; so does a controller or a note-off.
void test_held_send_other_messages_keep_the_held_off(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {.scripted = 1, .script = {-EAGAIN}};
  Midi_write w = FAKE_WRITE(&f);
  CHECK(held_send(&p, &w, 0x82, 60, 0) == -EAGAIN);
  CHECK(held_send(&p, &w, 0x92, 61, 100) == 28); // another note
  CHECK(held_send(&p, &w, 0x93, 60, 100) == 28); // another channel
  CHECK(held_send(&p, &w, 0xb2, 60, 100) == 28); // a controller
  CHECK(held_send(&p, &w, 0x82, 62, 0) == 28);   // another note-off
  CHECK(p.count == 1 && p.entries[0].chan == 2 && p.entries[0].note == 60);
}

// The schedule. The first retry is due Pending_offs_first_us after the
// refusal; each refusal doubles the wait up to the cap; a success resets it;
// nothing is tried before the time.
void test_pending_offs_retry_backs_off(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {.scripted = Fake_max};
  for (int i = 0; i < Fake_max; ++i)
    f.script[i] = -EAGAIN;
  Midi_write w = FAKE_WRITE(&f);
  CHECK(pending_offs_add(&p, 0, 60, 1000));
  U64 due = 1000 + Pending_offs_first_us;
  CHECK(p.next_us == due);
  CHECK(pending_offs_retry(&p, due - 1, &w) == 1 && f.calls == 0);
  U64 wait = 99;
  CHECK(pending_offs_wait_us(&p, due - 500, &wait) && wait == 500);
  CHECK(pending_offs_wait_us(&p, due + 7, &wait) && wait == 0);
  U64 expect_wait = Pending_offs_first_us;
  for (int step = 0; step < 9; ++step) {
    CHECK(pending_offs_retry(&p, due, &w) == 1);
    CHECK(f.calls == step + 1);
    CHECK(p.next_us == due + expect_wait);
    due = p.next_us;
    expect_wait = expect_wait * 2 > Pending_offs_backoff_max_us
                      ? Pending_offs_backoff_max_us
                      : expect_wait * 2;
    CHECK(p.backoff_us == expect_wait);
  }
  CHECK(p.backoff_us == Pending_offs_backoff_max_us);
  // The subscriber has room again: it goes out and the wait starts over.
  f.scripted = 0;
  CHECK(pending_offs_retry(&p, due, &w) == 0);
  CHECK(p.resent == 1 && p.backoff_us == Pending_offs_first_us);
}

// A pass sends at most Pending_offs_pass_max, oldest first, and the next pass
// follows at once, so a recovery burst cannot hold up a tick.
void test_pending_offs_retry_pass_cap(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {0};
  Midi_write w = FAKE_WRITE(&f);
  for (int i = 0; i < 40; ++i)
    CHECK(pending_offs_add(&p, i % 16, i, 0));
  U64 now = Pending_offs_first_us;
  CHECK(pending_offs_retry(&p, now, &w) == 40 - Pending_offs_pass_max);
  CHECK(f.calls == Pending_offs_pass_max && p.next_us == now);
  for (int i = 0; i < Pending_offs_pass_max; ++i)
    CHECK(f.d1[i] == i && f.status[i] == (0x80 | (i % 16)) && f.d2[i] == 0);
  CHECK(pending_offs_retry(&p, now, &w) == 40 - 2 * Pending_offs_pass_max);
  CHECK(pending_offs_retry(&p, now, &w) == 0);
  CHECK(f.calls == 40 && p.resent == 40);
}

// An entry older than Pending_offs_hold_us is given up without a try; a
// younger one behind it is still tried. A destination that never recovers is
// not asked for ever.
void test_pending_offs_retry_expires(void) {
  Pending_offs p;
  pending_offs_init(&p);
  Fake f = {0};
  Midi_write w = FAKE_WRITE(&f);
  CHECK(pending_offs_add(&p, 2, 50, 1000));
  CHECK(pending_offs_add(&p, 2, 51, 2000000));
  U64 now = 1000 + Pending_offs_hold_us - 1; // one us short for the first
  CHECK(pending_offs_retry(&p, now, &w) == 0 && f.calls == 2);
  pending_offs_init(&p);
  f.calls = 0;
  CHECK(pending_offs_add(&p, 2, 50, 1000));
  CHECK(pending_offs_add(&p, 2, 51, 2000000));
  now = 1000 + Pending_offs_hold_us;
  CHECK(pending_offs_retry(&p, now, &w) == 0);
  CHECK(f.calls == 1 && f.d1[0] == 51 && p.lost == 1 && p.resent == 1);
}

// The same channel and note twice is one entry that keeps its first time; the
// list is bounded, a full one refuses a new entry (that note-off is lost) but
// still accepts a repeat; and a note-on drops only its own note-off.
void test_pending_offs_duplicates_and_cap(void) {
  Pending_offs p;
  pending_offs_init(&p);
  CHECK(pending_offs_add(&p, 3, 70, 10));
  CHECK(pending_offs_add(&p, 3, 70, 99));
  CHECK(p.count == 1 && p.entries[0].since_us == 10 && p.held == 1);
  pending_offs_init(&p);
  for (int i = 0; i < Pending_offs_cap; ++i)
    CHECK(pending_offs_add(&p, i / 128, i % 128, 0));
  CHECK(p.count == Pending_offs_cap);
  CHECK(!pending_offs_add(&p, 15, 127, 0));
  CHECK(p.count == Pending_offs_cap && p.lost == 1);
  CHECK(pending_offs_add(&p, 0, 0, 0)); // a repeat of a held entry
  CHECK(pending_offs_before_send(&p, 0x91, 5, 100)); // chan 1 note 5 held
  CHECK(!pending_offs_before_send(&p, 0x91, 5, 100)); // gone now
  CHECK(!pending_offs_before_send(&p, 0x81, 6, 100)); // a note-off: no
  CHECK(!pending_offs_before_send(&p, 0x91, 6, 0));   // velocity 0: no
  CHECK(p.count == Pending_offs_cap - 1);
}

// Which send errors are worth another try: a full pool or an interrupted
// write, not a bad message or a gone device.
void test_midi_send_error_is_transient(void) {
  CHECK(midi_send_error_is_transient(-EAGAIN));
  CHECK(midi_send_error_is_transient(-EINTR));
  CHECK(midi_send_error_is_transient(-ENOMEM));
  CHECK(!midi_send_error_is_transient(-EINVAL));
  CHECK(!midi_send_error_is_transient(-ENODEV));
  CHECK(!midi_send_error_is_transient(-EBADF));
  CHECK(!midi_send_error_is_transient(0));
  CHECK(!midi_send_error_is_transient(28)); // a successful write's byte count
}
