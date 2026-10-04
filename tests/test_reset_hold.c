#include "reset_hold.h"

#include <assert.h>
#include <stdio.h>

static void assert_idle(const reset_hold_t *state) {
    assert(!state->visible && !state->progress1000 && !state->armed);
    assert(!state->blocked && !state->waiting_release && !state->pressed);
    assert(!state->release_seen);
}

static void test_release_boundaries(void) {
    const uint64_t durations[] = {0, 1, 249, 250, 499, 500, 501, 1999, 2000, 2001};
    for (unsigned blocked = 0; blocked <= 1; ++blocked) {
        for (unsigned i = 0; i < sizeof(durations) / sizeof(durations[0]); ++i) {
            reset_hold_t s = {0};
            uint64_t duration = durations[i];
            reset_hold_press(&s, 100, blocked);
            assert(s.visible && s.pressed && s.blocked == (bool)blocked);
            assert(!s.progress1000 && !s.armed && !s.waiting_release);
            reset_hold_event_t event = reset_hold_release(&s, 100 + duration);
            if (duration < RESET_HOLD_TAP_MS) {
                assert(event == RESET_HOLD_TAP);
                assert_idle(&s);
            } else if (duration < RESET_HOLD_DURATION_MS || blocked) {
                assert(event == RESET_HOLD_CANCEL);
                assert_idle(&s);
            } else {
                assert(event == RESET_HOLD_NONE);
                assert(s.visible && s.armed && s.waiting_release && !s.pressed);
                assert(s.progress1000 == 1000);
                assert(reset_hold_tick(&s, 100 + duration, true) == RESET_HOLD_NONE);
                assert(reset_hold_tick(&s, 349 + duration, true) == RESET_HOLD_NONE);
                assert(reset_hold_tick(&s, 350 + duration, true) == RESET_HOLD_SLEEP);
                assert_idle(&s);
            }
            assert(reset_hold_release(&s, 10000) == RESET_HOLD_NONE);
            assert(reset_hold_tick(&s, 20000, true) == RESET_HOLD_NONE);
        }
    }
}

static void test_progress_and_explicit_release(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 0, false);
    assert(reset_hold_tick(&s, 1, false) == RESET_HOLD_NONE && !s.progress1000);
    assert(reset_hold_tick(&s, 2, false) == RESET_HOLD_NONE && s.progress1000 == 1);
    assert(reset_hold_tick(&s, 1000, false) == RESET_HOLD_NONE && s.progress1000 == 500);
    assert(reset_hold_tick(&s, 1999, false) == RESET_HOLD_NONE && s.progress1000 == 999);
    assert(!s.armed && !s.waiting_release);
    assert(reset_hold_tick(&s, 2000, false) == RESET_HOLD_NONE && s.progress1000 == 1000);
    assert(s.armed && s.waiting_release);
    /* Even an inconsistent released sample cannot replace a lost RELEASE. */
    assert(reset_hold_tick(&s, 2250, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, UINT64_C(1000000000), true) == RESET_HOLD_NONE);
    assert(s.visible && s.pressed && s.progress1000 == 1000);
    assert(reset_hold_release(&s, UINT64_C(1000000000)) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, UINT64_C(1000000000), true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, UINT64_C(1000000250), true) == RESET_HOLD_SLEEP);
    for (unsigned i = 0; i < 5; ++i) {
        assert(reset_hold_tick(&s, UINT64_C(1000000500) + i, true) == RESET_HOLD_NONE);
    }
    assert_idle(&s);
}

static void test_release_stability(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 5000, false);
    assert(reset_hold_release(&s, 7000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 9000, false) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 10000, false) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 10001, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 10250, true) == RESET_HOLD_NONE);
    /* Any LOW/invalid ADC reading restarts the complete 250 ms guard. */
    assert(reset_hold_tick(&s, 10251, false) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 10252, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 10501, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 10502, true) == RESET_HOLD_SLEEP);
    assert_idle(&s);

    /* Waiting time before the first known HIGH never counts as stable HIGH. */
    reset_hold_press(&s, 20000, false);
    assert(reset_hold_release(&s, 22000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 99000, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 99249, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 99250, true) == RESET_HOLD_SLEEP);
}

static void test_blocking_and_cancellation(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 0, true);
    assert(reset_hold_tick(&s, 2000, false) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 10000, true) == RESET_HOLD_NONE);
    assert(s.visible && s.blocked && !s.armed && !s.waiting_release && !s.progress1000);
    assert(reset_hold_release(&s, 10001) == RESET_HOLD_CANCEL);
    assert_idle(&s);
    /* External other-key/focus/overflow cancellations consume the release,
     * including short releases that would otherwise trigger an ordinary tap. */
    const uint64_t cancel_at[] = {100, 499, 500, 1999, 2000, 2500};
    for (unsigned i = 0; i < sizeof(cancel_at) / sizeof(cancel_at[0]); ++i) {
        reset_hold_press(&s, 0, false);
        assert(reset_hold_tick(&s, cancel_at[i], false) == RESET_HOLD_NONE);
        reset_hold_cancel(&s);
        assert_idle(&s);
        assert(reset_hold_release(&s, cancel_at[i] + 1) == RESET_HOLD_NONE);
        assert(reset_hold_tick(&s, cancel_at[i] + 1000, true) == RESET_HOLD_NONE);
    }
    reset_hold_press(&s, 0, false);
    assert(reset_hold_release(&s, 2000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2000, true) == RESET_HOLD_NONE);
    reset_hold_cancel(&s);
    assert(reset_hold_tick(&s, 2250, true) == RESET_HOLD_NONE);
    assert_idle(&s);
    /* Recovery allows a fresh ordinary press, without stale hold state. */
    reset_hold_press(&s, 3000, false);
    assert(reset_hold_release(&s, 3499) == RESET_HOLD_TAP);
    assert_idle(&s);
}

static void test_duplicate_and_invalid_events(void) {
    reset_hold_t s = {0};
    reset_hold_init(&s);
    assert_idle(&s);
    assert(reset_hold_release(&s, 100) == RESET_HOLD_NONE);
    reset_hold_press(&s, 1000, false);
    reset_hold_press(&s, 1500, false); /* Duplicate DOWN does not reset origin. */
    assert(s.pressed_at_ms == 1000);
    assert(reset_hold_release(&s, 3000) == RESET_HOLD_NONE && s.armed);
    assert(reset_hold_tick(&s, 3000, true) == RESET_HOLD_NONE);
    assert(reset_hold_release(&s, 3100) == RESET_HOLD_NONE); /* Duplicate UP. */
    assert(reset_hold_tick(&s, 3250, true) == RESET_HOLD_SLEEP);

    reset_hold_press(&s, 0, false);
    assert(reset_hold_release(&s, 2000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2000, true) == RESET_HOLD_NONE);
    reset_hold_press(&s, 2200, false); /* New key-down cancels pending sleep. */
    assert_idle(&s);
    assert(reset_hold_release(&s, 2300) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2500, true) == RESET_HOLD_NONE);

    reset_hold_press(&s, 0, false);
    reset_hold_press(&s, 1000, true); /* A newly blocked gesture cannot arm. */
    assert_idle(&s);
    assert(reset_hold_tick(&s, 10000, true) == RESET_HOLD_NONE);

    reset_hold_init(NULL);
    reset_hold_press(NULL, 0, false);
    reset_hold_cancel(NULL);
    assert(reset_hold_release(NULL, 0) == RESET_HOLD_NONE);
    assert(reset_hold_tick(NULL, 0, true) == RESET_HOLD_NONE);
}

static void test_monotonic_time_and_long_uptime(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 1000, false);
    assert(reset_hold_release(&s, 999) == RESET_HOLD_CANCEL);
    assert_idle(&s);
    reset_hold_press(&s, 1000, false);
    assert(reset_hold_tick(&s, 1500, false) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 1499, false) == RESET_HOLD_CANCEL);
    assert_idle(&s);
    reset_hold_press(&s, 1000, false);
    reset_hold_press(&s, 999, false);
    assert_idle(&s);
    reset_hold_press(&s, 0, false);
    assert(reset_hold_release(&s, 2000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2010, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2009, true) == RESET_HOLD_CANCEL);
    assert_idle(&s);

    const uint64_t start = UINT64_MAX - UINT64_C(3000);
    reset_hold_press(&s, start, false);
    assert(reset_hold_tick(&s, start + 1999, false) == RESET_HOLD_NONE);
    assert(s.progress1000 == 999);
    assert(reset_hold_release(&s, start + 2000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, start + 2000, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, start + 2250, true) == RESET_HOLD_SLEEP);
    reset_hold_press(&s, 0, false);
    assert(reset_hold_tick(&s, UINT64_MAX, false) == RESET_HOLD_NONE);
    assert(s.progress1000 == 1000 && s.armed);
    assert(reset_hold_tick(&s, 0, true) == RESET_HOLD_CANCEL); /* Wrap is unsafe. */
    assert_idle(&s);
}

static void test_queued_release_timestamps(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 100, false);
    assert(reset_hold_tick(&s, 200, false) == RESET_HOLD_NONE);
    assert(reset_hold_release(&s, 140) == RESET_HOLD_TAP);
    assert_idle(&s);

    /* Owner latency can display a full ring before the queued release is
     * consumed. The captured release still decides TAP / CANCEL / armed. */
    const uint64_t durations[] = {499, 500, 1999, 2000};
    for (unsigned i = 0; i < sizeof(durations) / sizeof(durations[0]); ++i) {
        reset_hold_press(&s, 100, false);
        assert(reset_hold_tick(&s, 2500, false) == RESET_HOLD_NONE);
        assert(s.armed);
        reset_hold_event_t event = reset_hold_release(&s, 100 + durations[i]);
        if (durations[i] < 500) {
            assert(event == RESET_HOLD_TAP);
            assert_idle(&s);
        } else if (durations[i] < 2000) {
            assert(event == RESET_HOLD_CANCEL);
            assert_idle(&s);
        } else {
            assert(event == RESET_HOLD_NONE && s.armed && !s.pressed);
            assert(s.last_now_ms == 2500);
            assert(reset_hold_tick(&s, 2500, true) == RESET_HOLD_NONE);
            assert(reset_hold_tick(&s, 2749, true) == RESET_HOLD_NONE);
            assert(reset_hold_tick(&s, 2750, true) == RESET_HOLD_SLEEP);
            assert_idle(&s);
        }
    }
    reset_hold_press(&s, 100, false);
    assert(reset_hold_tick(&s, 2500, false) == RESET_HOLD_NONE);
    assert(reset_hold_release(&s, 2100) == RESET_HOLD_NONE);
    /* Accepting an older captured release must not move the tick clock back. */
    assert(reset_hold_tick(&s, 2499, true) == RESET_HOLD_CANCEL);
    assert_idle(&s);
}

int main(void) {
    test_release_boundaries();
    test_progress_and_explicit_release();
    test_release_stability();
    test_blocking_and_cancellation();
    test_duplicate_and_invalid_events();
    test_monotonic_time_and_long_uptime();
    test_queued_release_timestamps();
    puts("Reset Confirm hold/release state-machine tests: PASS");
    return 0;
}
