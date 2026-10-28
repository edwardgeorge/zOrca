// The registry of unit tests in build/unit_tests (./tool build test). See
// tests/README.md for how to add a test.
#pragma once
#include "../../vmio.h"
#include "check.h"
#include <string.h>

// The bytes an event of this type occupies: its own struct's size, not the
// union's. oevent_list_alloc_item never zeroes a slot's tail, so a test that
// compares two event lists compares each pair over this size.
static inline Usz oevent_size(U8 type) {
  switch ((Oevent_types)type) {
  case Oevent_type_midi_note:
    return sizeof(Oevent_midi_note);
  case Oevent_type_midi_cc:
    return sizeof(Oevent_midi_cc);
  case Oevent_type_midi_cc_interpolated:
    return sizeof(Oevent_midi_cc_interpolated);
  case Oevent_type_midi_pb:
    return sizeof(Oevent_midi_pb);
  case Oevent_type_osc_ints:
    return sizeof(Oevent_osc_ints);
  case Oevent_type_udp_string:
    return sizeof(Oevent_udp_string);
  default:
    return sizeof(Oevent);
  }
}

// Two events are equal when their types match and the bytes the writer
// filled match: a struct's own fields, and for OSC and UDP only the first
// count numbers or characters, since sim.c writes no more and the slot's tail
// holds whatever the allocator left (found by the armhf run under qemu, where
// the two lists' tails differ).
static inline bool oevent_equal(Oevent const *a, Oevent const *b) {
  if (a->any.oevent_type != b->any.oevent_type)
    return false;
  switch ((Oevent_types)a->any.oevent_type) {
  case Oevent_type_osc_ints:
    return a->osc_ints.glyph == b->osc_ints.glyph &&
           a->osc_ints.count == b->osc_ints.count &&
           a->osc_ints.count <= Oevent_osc_int_count &&
           memcmp(a->osc_ints.numbers, b->osc_ints.numbers,
                  a->osc_ints.count) == 0;
  case Oevent_type_udp_string:
    return a->udp_string.count == b->udp_string.count &&
           a->udp_string.count <= Oevent_udp_string_count &&
           memcmp(a->udp_string.chars, b->udp_string.chars,
                  a->udp_string.count) == 0;
  default:
    return memcmp(a, b, oevent_size(a->any.oevent_type)) == 0;
  }
}

// X-macro lists, in run order. X(name) registers the function test_<name>.
//
// CORE_TESTS: tests that link the runner's sources (CORE and the shell files
// tool adds, field.c, sysmisc.c and thirdparty/oso.c) against libc only.
#define CORE_TESTS(X)                                                          \
  X(gbuffer_bounds)                                                            \
  X(glyph_index_of_reference)                                                  \
  X(oevent_list_growth)                                                        \
  X(orca_run_smoke)                                                            \
  X(prng_pcg32_reference)                                                      \
  X(prng_seed_fixed_vectors)                                                   \
  X(opstate_edge_keys)                                                         \
  X(opstate_growth)                                                            \
  X(opstate_retag)                                                             \
  X(opstate_clear_keeps_capacity)                                              \
  X(opstate_prune)                                                             \
  X(opstate_copy)                                                              \
  X(opstate_reset_anchors)                                                     \
  X(sim_bouncer_overflow_fixture)                                              \
  X(sim_bouncer_rate4_shapes)                                                  \
  X(sim_bouncer_anchor)                                                        \
  X(sim_bouncer_hold_and_range)                                                \
  X(sim_far_cell)                                                              \
  X(sim_two_r_permutations)                                                    \
  X(sim_r_no_repeat)                                                           \
  X(sim_r_range_change)                                                        \
  X(sim_r_single_value)                                                        \
  X(sim_r_seed)                                                                \
  X(sim_r_case)                                                                \
  X(sim_r_first_visit_and_clear)                                               \
  X(sim_preview_isolation)                                                     \
  X(sim_clear_and_prune)                                                       \
  X(arp_cap16_triad)                                                           \
  X(arp_cap_36)                                                                \
  X(arp_total_1)                                                               \
  X(arp_length_change)                                                         \
  X(arp_step_from_clock)                                                       \
  X(arp_step_wraps)                                                            \
  X(arp_step_star)                                                             \
  X(arp_step_skips_store)                                                      \
  X(arp_step_unset_resumes)                                                    \
  X(arp_small_totals)                                                          \
  X(arp_bounds)                                                                \
  X(arp_step_is_counter_step)                                                  \
  X(arp_length_dot_and_zero)                                                   \
  X(dialect_names)                                                             \
  X(dialect_default_is_borca)                                                  \
  X(dialect_zorca_is_borca)                                                    \
  X(dialect_upstream_cc)                                                       \
  X(dialect_upstream_r)                                                        \
  X(dialect_upstream_udp_osc)                                                  \
  X(dialect_upstream_inert)                                                    \
  X(dialect_tick_body_upstream)                                                \
  X(dialect_seaflake_operators)                                               \
  X(dialect_seaflake_due_action_limit)                                         \
  X(dialect_seaflake_burst_trigger_direction)                                 \
  X(dialect_seaflake_subtick_variables)                                        \
  X(dialect_cell_buffer)                                                       \
  X(rules_lesser)                                                              \
  X(rules_increment)                                                           \
  X(rules_uclid)                                                               \
  X(rules_delay)                                                               \
  X(rules_clock)                                                               \
  X(rules_lerp)                                                                \
  X(rules_random_range)                                                        \
  X(rules_random_hash)                                                         \
  X(ccout_glide_continuity)                                                    \
  X(ccout_glide_from_instant)                                                  \
  X(ccout_unknown_last_value)                                                  \
  X(ccout_instant_rates)                                                       \
  X(ccout_instant_cancels_glide)                                               \
  X(ccout_noop_submit)                                                         \
  X(ccout_noop_step_held_back)                                                 \
  X(ccout_early_end)                                                           \
  X(ccout_dedup)                                                               \
  X(ccout_glide_hz)                                                            \
  X(ccout_fixed_length)                                                        \
  X(ccout_retarget)                                                            \
  X(ccout_same_glide_again)                                                    \
  X(ccout_same_target_new_rate)                                                \
  X(ccout_same_glide_new_tempo)                                                \
  X(ccout_cancel_keeps_last_value)                                             \
  X(ccout_forget)                                                              \
  X(ccout_next_deadline)                                                       \
  X(ccout_time_goes_back)                                                      \
  X(ccout_worst_case_magnitude)                                                \
  X(ccout_moving_target)                                                       \
  X(ccout_rule_unknown_before_same)                                            \
  X(ccout_rule_same_before_noop)                                               \
  X(ccout_tick_len_guards)                                                     \
  X(ccout_poll_before_start)                                                   \
  X(ccout_poll_slot_order)                                                     \
  X(ccout_init_and_configure)                                                  \
  X(ccout_cap14_glide)                                                         \
  X(ccout_budget_flood)                                                        \
  X(ccout_budget_instant_flood)                                                \
  X(ccout_budget_empty)                                                        \
  X(ccout_budget_time_back)                                                    \
  X(ccout_budget_rotation)                                                     \
  X(ccout_budget_flood_ends)                                                   \
  X(ccout_budget_lifecycle)                                                    \
  X(ccout_noop_budget_held_back)                                               \
  X(ccout_glide_hz_change_applies)                                             \
  X(ccout_budget_first_held_slot)                                              \
  X(ccout_glide_rate_example)                                                  \
  X(ccout_glide_hz_names)                                                      \
  X(tick_cap9_resume_after_edit)                                               \
  X(tick_cap9_resume_without_edit)                                             \
  X(tick_wire_order)                                                           \
  X(tick_cci_same_tick)                                                        \
  X(tick_engine_poll_order)                                                    \
  X(tick_engine_cc_path)                                                       \
  X(tick_poll_engine)                                                          \
  X(tick_release_all)                                                          \
  X(tick_pause)                                                                \
  X(tick_pause_idle)                                                           \
  X(tick_forget_patch)                                                         \
  X(tick_stop_order)                                                           \
  X(tick_stop_no_clock_out)                                                    \
  X(tick_stop_control_optional)                                                \
  X(tick_note_length)                                                          \
  X(tick_note_length_beat_clock)                                               \
  X(tick_note_length_0_and_1)                                                  \
  X(tick_note_release_order)                                                   \
  X(tick_note_retrigger)                                                       \
  X(tick_note_mono)                                                            \
  X(tick_note_same_batch)                                                      \
  X(tick_note_empty_list)                                                      \
  X(tick_run_vm_clears_list)                                                   \
  X(tick_len_us)                                                               \
  X(music_scale_selectors)                                                     \
  X(music_midichord_selectors)                                                 \
  X(music_scale_inversion)                                                     \
  X(music_midichord_inversion)                                                 \
  X(music_inversion_sorts_12)                                                  \
  X(music_other_glyphs)                                                        \
  X(music_octave_overflow)                                                     \
  X(music_names)                                                               \
  X(music_shared_intervals)                                                    \
  X(music_velocity_midichord)                                                  \
  X(music_velocity_scale_points)                                               \
  X(music_velocity_midi_unchanged)                                             \
  X(music_velocity_zero)                                                       \
  X(transport_cap18_start)                                                     \
  X(transport_cap18_songpos)                                                   \
  X(transport_cap18_stop)                                                      \
  X(transport_cap18_clock_lost)                                                \
  X(transport_stale_poll)                                                      \
  X(transport_restart_after_loss)                                              \
  X(transport_continue_keeps_phase)                                            \
  X(transport_clock_while_stopped)                                             \
  X(transport_start_while_running)                                             \
  X(transport_redundant)                                                       \
  X(transport_burst)                                                           \
  X(transport_estimate)                                                        \
  X(transport_window_refills)                                                  \
  X(transport_refill_anchor)                                                   \
  X(transport_estimate_drops)                                                  \
  X(transport_hysteresis)                                                      \
  X(transport_reset)                                                           \
  X(transport_rearm)                                                           \
  X(clock_out_external_silent)                                                 \
  X(clock_out_internal_wire)                                                   \
  X(clock_out_return_internal)                                                 \
  X(clock_out_toggle)                                                          \
  X(clock_out_span)                                                            \
  X(transport_parse)                                                           \
  X(transport_hud_text)                                                        \
  X(transport_stale_stamp_refill)                                              \
  X(transport_deadline_while_stopped)                                          \
  X(transport_gap_keeps_running)                                               \
  X(transport_songpos_moves_anchor)                                            \
  X(transport_start_clears_lost)                                               \
  X(transport_overflow_keeps_following)                                        \
  X(transport_overflow_stale_lost_stopped)                                     \
  X(transport_overflow_then_silence)                                           \
  X(transport_overflow_partial_window)                                         \
  X(transport_overflow_after_start)                                            \
  X(transport_overflow_gap_drops)                                              \
  X(labels_ids)                                                                \
  X(labels_in_in_shared)                                                       \
  X(labels_in_over_out)                                                        \
  X(labels_out_keeps_in)                                                       \
  X(labels_q_fixture)                                                          \
  X(labels_g)                                                                  \
  X(labels_k)                                                                  \
  X(labels_t)                                                                  \
  X(labels_o)                                                                  \
  X(labels_i_on_input)                                                         \
  X(labels_p)                                                                  \
  X(labels_x)                                                                  \
  X(labels_far_owner)                                                          \
  X(labels_match_marks)                                                        \
  X(labels_no_buffer)                                                          \
  X(labels_upstream_cc)                                                        \
  X(labels_upstream_inert)                                                     \
  X(labels_upstream_udp)                                                       \
  X(labels_upstream_osc)                                                       \
  X(labels_selectors)                                                          \
  X(labels_borca_cc)                                                           \
  X(labels_stale_size)                                                         \
  X(labels_per_operator)                                                       \
  X(tooltip_place)                                                             \
  X(labels_tick_run_vm_clears)                                                 \
  X(labels_tick_body)                                                          \
  X(prefs_key_table)                                                           \
  X(prefs_read_line)                                                           \
  X(prefs_bad_values)                                                          \
  X(prefs_input_keys)                                                          \
  X(prefs_flag_over_conf)                                                      \
  X(prefs_menu_noop)                                                           \
  X(prefs_saved_value)                                                         \
  X(conf_read_line_tail)                                                       \
  X(events_dropped_format)                                                     \
  X(path_is_dir)                                                               \
  X(held_send_passes_a_message_through)                                        \
  X(held_send_holds_a_refused_note_off)                                        \
  X(held_send_holds_nothing_else)                                              \
  X(held_send_note_on_sends_the_held_off_first)                                \
  X(held_send_drops_a_held_off_it_cannot_send)                                 \
  X(held_send_other_messages_keep_the_held_off)                                \
  X(pending_offs_retry_backs_off)                                              \
  X(pending_offs_retry_pass_cap)                                               \
  X(pending_offs_retry_expires)                                                \
  X(pending_offs_duplicates_and_cap)                                           \
  X(midi_send_error_is_transient)

// The full list for the realloc hook, for each adapter FEAT_ flag, and for
// the runner's failure canary, whatever the flags.
//
// HOOK_TESTS_ALL: tests that arm the realloc hook (tests/unit/alloc_hook.c),
// which tool builds with -DUNIT_TESTS_REALLOC_HOOK, -Wl,--wrap=realloc and
// -fno-builtin-realloc on Linux with gcc or clang only; elsewhere the list
// is empty and the runner has these tests fewer. CI greps for
// realloc_hook_builtin_null, which proves the flags, and
// tick_note_alloc_fail.
#define HOOK_TESTS_ALL(X)                                                      \
  X(realloc_hook_builtin_null)                                                 \
  X(tick_note_alloc_fail)                                                      \
  X(opstate_copy_alloc_fail)                                                   \
  X(oevent_list_alloc_fail)                                                    \
  X(field_alloc_fail)
#define PORTMIDI_TESTS_ALL(X)                                                  \
  X(portmidi_error_text_and_filters)                                           \
  X(portmidi_in_filter)                                                        \
  X(portmidi_in_decode)
#define ALSA_TESTS_ALL(X)                                                      \
  X(alsa_version_and_open_modes)                                               \
  X(alsa_in_mapping)                                                           \
  X(alsa_in_announce)                                                          \
  X(alsa_in_split)                                                             \
  X(alsa_name_scan)                                                            \
  X(alsa_client_name_fits)                                                     \
  X(alsa_client_name_pick)
#define CANARY_TESTS_ALL(X) X(unit_check_canary)

// Every test's prototype, generated from the lists and outside any #ifdef:
// a test file whose flag is off still holds these declarations, so it is
// never an empty translation unit (which -Wpedantic rejects).
#define UNIT_TEST_DECLARE(name) void test_##name(void);
CORE_TESTS(UNIT_TEST_DECLARE)
HOOK_TESTS_ALL(UNIT_TEST_DECLARE)
PORTMIDI_TESTS_ALL(UNIT_TEST_DECLARE)
ALSA_TESTS_ALL(UNIT_TEST_DECLARE)
CANARY_TESTS_ALL(UNIT_TEST_DECLARE)
#undef UNIT_TEST_DECLARE

// The realloc hook. While unit_realloc_fail is set every realloc in the
// runner returns NULL; unit_realloc_calls counts every call. A test arms
// the hook just before the call under test and disarms it right after,
// before any CHECK. Both are volatile so the release runner's -flto link
// keeps the arming store. __wrap_realloc is what the linker routes realloc
// to, and __real_realloc is libc's.
#ifdef UNIT_TESTS_REALLOC_HOOK
extern volatile bool unit_realloc_fail;
extern volatile size_t unit_realloc_calls;
void *__wrap_realloc(void *ptr, size_t size);
void *__real_realloc(void *ptr, size_t size);
#endif

// The lists the runner runs. The hook list is empty unless the hook is
// built. An adapter list is empty unless its flag is set, and the runner
// fails a build that sets the flag but registers nothing in its list, so
// adapter tests cannot be compiled out silently.
#ifdef UNIT_TESTS_REALLOC_HOOK
#define HOOK_TESTS(X) HOOK_TESTS_ALL(X)
#else
#define HOOK_TESTS(X)
#endif
#ifdef FEAT_PORTMIDI
#define PORTMIDI_TESTS(X) PORTMIDI_TESTS_ALL(X)
#else
#define PORTMIDI_TESTS(X)
#endif
#ifdef FEAT_ALSA
#define ALSA_TESTS(X) ALSA_TESTS_ALL(X)
#else
#define ALSA_TESTS(X)
#endif

// -DUNIT_TESTS_CANARY adds a test whose check is false; CI uses it to prove
// that the runner reports a failure and exits 1.
#ifdef UNIT_TESTS_CANARY
#define CANARY_TESTS(X) CANARY_TESTS_ALL(X)
#else
#define CANARY_TESTS(X)
#endif

// A test_* function that no list declares has no prototype, so defining it
// is an error rather than a test that never runs. Helpers in test files must
// be static.
#pragma GCC diagnostic error "-Wmissing-prototypes"
