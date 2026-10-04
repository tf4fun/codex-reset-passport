#include "reset_hold.h"

#include <assert.h>
#include <stdio.h>

static void assert_idle(const reset_hold_t *s) {
    assert(!s->active && !s->cancelling && !s->visible && !s->progress1000);
    assert(!s->armed && !s->blocked && !s->waiting_release && !s->pressed);
    assert(!s->release_seen);
}

static reset_hold_event_t release_now(reset_hold_t *s, uint64_t now_ms) {
    return reset_hold_release(s, now_ms, now_ms);
}

static void finish_cancel(reset_hold_t *s, uint64_t now_ms, bool was_visible) {
    assert(s->active && s->cancelling && !s->armed && !s->waiting_release);
    assert(s->visible == was_visible && !s->pressed);
    assert(reset_hold_tick(s, now_ms, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(s, now_ms + 249, true) == RESET_HOLD_NONE);
    assert(s->active && s->cancelling && s->visible == was_visible);
    assert(reset_hold_tick(s, now_ms + 250, true) == RESET_HOLD_NONE);
    assert_idle(s);
}

static void test_release_boundaries(void) {
    const uint64_t durations[] = {0, 1, 249, 250, 499, 500, 501, 1999, 2000, 2001};
    for (unsigned blocked = 0; blocked <= 1; ++blocked) {
        for (unsigned i = 0; i < sizeof(durations) / sizeof(durations[0]); ++i) {
            reset_hold_t s = {0};
            const uint64_t duration = durations[i];
            const uint64_t now = 100 + duration;
            reset_hold_press(&s, 100, blocked);
            assert(s.active && !s.visible && s.pressed && s.blocked == (bool)blocked);
            assert(!s.progress1000 && !s.armed && !s.waiting_release);
            assert(reset_hold_tick(&s, now, false) == RESET_HOLD_NONE);
            assert(s.visible == (duration >= RESET_HOLD_TAP_MS));
            if (blocked) assert(!s.armed && !s.progress1000);
            const reset_hold_event_t event = release_now(&s, now);
            if (duration < RESET_HOLD_TAP_MS) {
                assert(event == RESET_HOLD_TAP);
                assert_idle(&s);
            } else if (duration < RESET_HOLD_DURATION_MS || blocked) {
                assert(event == RESET_HOLD_CANCEL);
                finish_cancel(&s, now, true);
            } else {
                assert(event == RESET_HOLD_NONE);
                assert(s.active && s.visible && s.armed && s.waiting_release && !s.pressed);
                assert(s.progress1000 == 1000);
                assert(reset_hold_tick(&s, now, true) == RESET_HOLD_NONE);
                assert(reset_hold_tick(&s, now + 249, true) == RESET_HOLD_NONE);
                assert(reset_hold_tick(&s, now + 250, true) == RESET_HOLD_SLEEP);
                assert_idle(&s);
            }
            assert(release_now(&s, 10000) == RESET_HOLD_NONE);
            assert(reset_hold_tick(&s, 20000, true) == RESET_HOLD_NONE);
        }
    }
}

static uint16_t expected_progress(uint64_t elapsed) {
    if (elapsed < 500) return 0;
    if (elapsed >= 2000) return 1000;
    return (uint16_t)((elapsed - 500) * 1000 / 1500);
}

static void test_frame_independent_progress_and_explicit_release(void) {
    reset_hold_t dense = {0}, sparse = {0};
    reset_hold_press(&dense, 0, false);
    reset_hold_press(&sparse, 0, false);
    for (uint64_t t = 0; t <= 2100; ++t) {
        assert(reset_hold_tick(&dense, t, false) == RESET_HOLD_NONE);
        assert(dense.visible == (t >= 500));
        assert(dense.progress1000 == expected_progress(t));
        assert(dense.armed == (t >= 2000));
        assert(dense.waiting_release == (t >= 2000));
        if (t % 17 == 0 || t == 499 || t == 500 || t == 501 || t == 1250 ||
            t == 1999 || t == 2000 || t == 2100) {
            assert(reset_hold_tick(&sparse, t, false) == RESET_HOLD_NONE);
            assert(sparse.visible == dense.visible);
            assert(sparse.progress1000 == dense.progress1000);
            assert(sparse.armed == dense.armed);
        }
    }
    /* Neither a full ring nor an inconsistent HIGH sample replaces RELEASE. */
    assert(reset_hold_tick(&dense, 2250, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&dense, UINT64_C(1000000000), true) == RESET_HOLD_NONE);
    assert(dense.active && dense.visible && dense.pressed && dense.progress1000 == 1000);
    assert(release_now(&dense, UINT64_C(1000000000)) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&dense, UINT64_C(1000000000), true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&dense, UINT64_C(1000000250), true) == RESET_HOLD_SLEEP);
    for (unsigned i = 0; i < 5; ++i) {
        assert(reset_hold_tick(&dense, UINT64_C(1000000500) + i, true) == RESET_HOLD_NONE);
    }
    assert_idle(&dense);
}

static void test_frame_independent_unwind(void) {
    /* Zero and half rings, and a full displayed ring with a queued early UP. */
    const uint64_t shown_at[] = {500, 1250, 2200};
    const uint16_t progress[] = {0, 500, 1000};
    for (unsigned i = 0; i < sizeof(shown_at) / sizeof(shown_at[0]); ++i) {
        reset_hold_t dense = {0}, sparse = {0};
        reset_hold_press(&dense, 0, false);
        reset_hold_press(&sparse, 0, false);
        assert(reset_hold_tick(&dense, shown_at[i], false) == RESET_HOLD_NONE);
        assert(reset_hold_tick(&sparse, shown_at[i], false) == RESET_HOLD_NONE);
        assert(dense.progress1000 == progress[i]);
        const uint64_t released_at = i == 2 ? 1999 : shown_at[i];
        const uint64_t observed_at = shown_at[i] + 100;
        assert(reset_hold_release(&dense, released_at, observed_at) == RESET_HOLD_CANCEL);
        assert(reset_hold_release(&sparse, released_at, observed_at) == RESET_HOLD_CANCEL);
        uint16_t previous = progress[i];
        for (uint64_t t = 0; t <= 300; ++t) {
            assert(reset_hold_tick(&dense, observed_at + t, false) == RESET_HOLD_NONE);
            const uint16_t expected = t >= 250 ? 0 : (uint16_t)(progress[i] * (250 - t) / 250);
            assert(dense.progress1000 == expected && dense.progress1000 <= previous);
            assert(dense.visible == (t < 250));
            assert(dense.active && dense.cancelling && !dense.armed);
            if (t % 17 == 0 || t == 125 || t == 249 || t == 250 || t == 300) {
                assert(reset_hold_tick(&sparse, observed_at + t, false) == RESET_HOLD_NONE);
                assert(sparse.progress1000 == dense.progress1000 && sparse.visible == dense.visible);
            }
            previous = dense.progress1000;
        }
        assert(reset_hold_tick(&dense, observed_at + 301, true) == RESET_HOLD_NONE);
        assert(reset_hold_tick(&dense, observed_at + 550, true) == RESET_HOLD_NONE);
        assert(dense.active && !dense.visible);
        assert(reset_hold_tick(&dense, observed_at + 551, true) == RESET_HOLD_NONE);
        assert_idle(&dense);
    }
}

static void test_release_stability(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 5000, false);
    assert(release_now(&s, 7000) == RESET_HOLD_NONE);
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

    /* Time before the first known HIGH never counts as stable HIGH. */
    reset_hold_press(&s, 20000, false);
    assert(release_now(&s, 22000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 99000, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 99249, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 99250, true) == RESET_HOLD_SLEEP);
}

static void test_cancellation_consumes_input(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 0, false);
    assert(reset_hold_tick(&s, 1250, false) == RESET_HOLD_NONE && s.progress1000 == 500);
    assert(release_now(&s, 1250) == RESET_HOLD_CANCEL);
    assert(reset_hold_tick(&s, 1250, true) == RESET_HOLD_NONE);
    reset_hold_press(&s, 1300, false);
    assert(reset_hold_tick(&s, 1300, false) == RESET_HOLD_NONE && s.progress1000 == 400);
    assert(release_now(&s, 1310) == RESET_HOLD_NONE); /* Never a TAP. */
    assert(reset_hold_tick(&s, 1310, true) == RESET_HOLD_NONE && s.progress1000 == 380);
    assert(reset_hold_tick(&s, 1500, true) == RESET_HOLD_NONE);
    assert(s.active && s.cancelling && !s.visible && !s.progress1000);
    /* A new held key keeps the invisible quarantine alive indefinitely. */
    reset_hold_press(&s, 1501, false);
    assert(reset_hold_tick(&s, 5000, false) == RESET_HOLD_NONE);
    assert(s.active && s.cancelling && !s.visible);
    assert(release_now(&s, 5001) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 5001, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 5250, true) == RESET_HOLD_NONE && s.active);
    assert(reset_hold_tick(&s, 5251, true) == RESET_HOLD_NONE);
    assert_idle(&s);
    /* A queued old DOWN cannot re-open the popup after it became idle. */
    reset_hold_press(&s, 1300, false);
    assert_idle(&s);
    assert(reset_hold_release(&s, 1310, 5251) == RESET_HOLD_NONE);
    reset_hold_press(&s, 5251, false);
    assert_idle(&s);
    reset_hold_press(&s, 5252, false);
    assert(s.active && !s.visible);
    assert(release_now(&s, 5352) == RESET_HOLD_TAP);
    assert_idle(&s);
}

static void test_blocking_and_hard_cancellation(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 0, true);
    assert(reset_hold_tick(&s, 499, false) == RESET_HOLD_NONE && !s.visible);
    assert(reset_hold_tick(&s, 500, false) == RESET_HOLD_NONE && s.visible);
    assert(reset_hold_tick(&s, 10000, true) == RESET_HOLD_NONE);
    assert(s.blocked && !s.armed && !s.waiting_release && !s.progress1000);
    assert(release_now(&s, 10001) == RESET_HOLD_CANCEL);
    finish_cancel(&s, 10001, true);

    /* A release processed before any display tick cannot create a popup. */
    reset_hold_init(&s);
    reset_hold_press(&s, 0, false);
    assert(reset_hold_release(&s, 500, 800) == RESET_HOLD_CANCEL);
    finish_cancel(&s, 800, false);

    /* Other-key/focus/overflow hard resets consume the old release. The owner
     * separately quarantines unsafe input after lost/overflowed events. */
    const uint64_t cancel_at[] = {100, 499, 500, 1999, 2000, 2500};
    for (unsigned i = 0; i < sizeof(cancel_at) / sizeof(cancel_at[0]); ++i) {
        reset_hold_init(&s);
        reset_hold_press(&s, 0, false);
        assert(reset_hold_tick(&s, cancel_at[i], false) == RESET_HOLD_NONE);
        reset_hold_cancel(&s);
        assert_idle(&s);
        assert(release_now(&s, cancel_at[i] + 1) == RESET_HOLD_NONE);
        assert(reset_hold_tick(&s, cancel_at[i] + 1000, true) == RESET_HOLD_NONE);
    }
    reset_hold_press(&s, 0, false);
    assert(release_now(&s, 2000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2000, true) == RESET_HOLD_NONE);
    reset_hold_cancel(&s);
    assert(reset_hold_tick(&s, 2250, true) == RESET_HOLD_NONE);
    reset_hold_press(&s, 3000, false);
    assert(release_now(&s, 3499) == RESET_HOLD_TAP);
    assert_idle(&s);
}

static void test_duplicate_and_invalid_events(void) {
    reset_hold_t s = {0};
    reset_hold_init(&s);
    assert_idle(&s);
    assert(release_now(&s, 100) == RESET_HOLD_NONE);
    reset_hold_press(&s, 1000, false);
    reset_hold_press(&s, 1500, false); /* Duplicate DOWN preserves origin. */
    assert(s.pressed_at_ms == 1000);
    assert(release_now(&s, 3000) == RESET_HOLD_NONE && s.armed);
    assert(reset_hold_tick(&s, 3000, true) == RESET_HOLD_NONE);
    assert(release_now(&s, 3100) == RESET_HOLD_NONE); /* Duplicate UP. */
    assert(reset_hold_tick(&s, 3250, true) == RESET_HOLD_SLEEP);

    reset_hold_press(&s, 0, false);
    assert(release_now(&s, 2000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2000, true) == RESET_HOLD_NONE);
    reset_hold_press(&s, 2200, false); /* New DOWN invalidates pending sleep. */
    assert_idle(&s);
    assert(release_now(&s, 2300) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2500, true) == RESET_HOLD_NONE);

    reset_hold_press(&s, 0, false);
    reset_hold_press(&s, 1000, true); /* Newly blocked gestures cannot arm. */
    assert_idle(&s);
    assert(reset_hold_tick(&s, 10000, true) == RESET_HOLD_NONE);

    reset_hold_init(NULL);
    reset_hold_press(NULL, 0, false);
    reset_hold_cancel(NULL);
    assert(reset_hold_release(NULL, 0, 0) == RESET_HOLD_NONE);
    assert(reset_hold_tick(NULL, 0, true) == RESET_HOLD_NONE);
}

static void test_monotonic_time_and_long_uptime(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 1000, false);
    assert(reset_hold_release(&s, 999, 1000) == RESET_HOLD_CANCEL);
    assert_idle(&s);
    reset_hold_press(&s, 1000, false);
    assert(reset_hold_release(&s, 1500, 1499) == RESET_HOLD_CANCEL); /* Future UP. */
    assert_idle(&s);
    reset_hold_press(&s, 1000, false);
    assert(reset_hold_tick(&s, 1500, false) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 1499, false) == RESET_HOLD_CANCEL);
    assert_idle(&s);
    reset_hold_press(&s, 1000, false);
    reset_hold_press(&s, 999, false);
    assert_idle(&s);
    reset_hold_press(&s, 0, false);
    assert(release_now(&s, 2000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2010, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, 2009, true) == RESET_HOLD_CANCEL);
    assert_idle(&s);
    reset_hold_press(&s, 0, false);
    assert(reset_hold_tick(&s, 1250, false) == RESET_HOLD_NONE);
    assert(release_now(&s, 1250) == RESET_HOLD_CANCEL);
    assert(reset_hold_tick(&s, 1300, false) == RESET_HOLD_NONE);
    reset_hold_press(&s, 1200, false); /* Stale DOWN is swallowed in cancel. */
    assert(s.active && s.cancelling && s.progress1000 == 400);
    assert(reset_hold_tick(&s, 1299, true) == RESET_HOLD_CANCEL);
    assert_idle(&s);

    const uint64_t start = UINT64_MAX - UINT64_C(3000);
    reset_hold_press(&s, start, false);
    assert(reset_hold_tick(&s, start + 1999, false) == RESET_HOLD_NONE);
    assert(s.progress1000 == 999);
    assert(release_now(&s, start + 2000) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, start + 2000, true) == RESET_HOLD_NONE);
    assert(reset_hold_tick(&s, start + 2250, true) == RESET_HOLD_SLEEP);
    reset_hold_press(&s, start, false);
    assert(reset_hold_tick(&s, start + 1250, false) == RESET_HOLD_NONE);
    assert(release_now(&s, start + 1250) == RESET_HOLD_CANCEL);
    finish_cancel(&s, start + 1250, true);
    reset_hold_init(&s);
    reset_hold_press(&s, 0, false);
    assert(reset_hold_tick(&s, UINT64_MAX, false) == RESET_HOLD_NONE);
    assert(s.progress1000 == 1000 && s.armed);
    assert(reset_hold_tick(&s, 0, true) == RESET_HOLD_CANCEL); /* Wrap is unsafe. */
    assert_idle(&s);
    reset_hold_press(&s, UINT64_MAX - 1000, false);
    assert(reset_hold_tick(&s, UINT64_MAX - 100, false) == RESET_HOLD_NONE);
    assert(release_now(&s, UINT64_MAX - 100) == RESET_HOLD_CANCEL);
    assert(reset_hold_tick(&s, 0, true) == RESET_HOLD_CANCEL);
    assert_idle(&s);
}

static void test_queued_release_timestamps(void) {
    reset_hold_t s = {0};
    reset_hold_press(&s, 100, false);
    assert(reset_hold_tick(&s, 200, false) == RESET_HOLD_NONE);
    assert(reset_hold_release(&s, 140, 300) == RESET_HOLD_TAP);
    assert_idle(&s);

    /* Captured UP decides intent even if a delayed queue let a tick show a
     * full ring. Cancellation starts now, not at the old event or last tick. */
    const uint64_t durations[] = {499, 500, 501, 1999, 2000};
    for (unsigned i = 0; i < sizeof(durations) / sizeof(durations[0]); ++i) {
        reset_hold_init(&s);
        reset_hold_press(&s, 100, false);
        assert(reset_hold_tick(&s, 2500, false) == RESET_HOLD_NONE);
        assert(s.armed);
        const reset_hold_event_t event = reset_hold_release(&s, 100 + durations[i], 3000);
        if (durations[i] < 500) {
            assert(event == RESET_HOLD_TAP);
            assert_idle(&s);
        } else if (durations[i] < 2000) {
            assert(event == RESET_HOLD_CANCEL && !s.armed && s.cancelling);
            assert(s.progress1000 == 1000 && s.cancelled_at_ms == 3000);
            assert(reset_hold_tick(&s, 3000, true) == RESET_HOLD_NONE && s.progress1000 == 1000);
            assert(reset_hold_tick(&s, 3125, true) == RESET_HOLD_NONE && s.progress1000 == 500);
            assert(reset_hold_tick(&s, 3249, true) == RESET_HOLD_NONE && s.visible);
            assert(reset_hold_tick(&s, 3250, true) == RESET_HOLD_NONE);
            assert_idle(&s);
        } else {
            assert(event == RESET_HOLD_NONE && s.armed && !s.pressed);
            assert(s.last_now_ms == 3000);
            assert(reset_hold_tick(&s, 3000, true) == RESET_HOLD_NONE);
            assert(reset_hold_tick(&s, 3249, true) == RESET_HOLD_NONE);
            assert(reset_hold_tick(&s, 3250, true) == RESET_HOLD_SLEEP);
            assert_idle(&s);
        }
    }
    reset_hold_press(&s, 0, false);
    assert(reset_hold_tick(&s, 1250, false) == RESET_HOLD_NONE && s.progress1000 == 500);
    assert(reset_hold_release(&s, 1000, 5000) == RESET_HOLD_CANCEL);
    assert(reset_hold_tick(&s, 5000, true) == RESET_HOLD_NONE && s.progress1000 == 500);
    assert(reset_hold_tick(&s, 5125, true) == RESET_HOLD_NONE && s.progress1000 == 250);
    assert(reset_hold_tick(&s, 5250, true) == RESET_HOLD_NONE);
    assert_idle(&s);
    reset_hold_init(&s);
    reset_hold_press(&s, 100, false);
    assert(reset_hold_tick(&s, 2500, false) == RESET_HOLD_NONE);
    assert(reset_hold_release(&s, 2100, 2499) == RESET_HOLD_CANCEL);
    assert_idle(&s);
}

int main(void) {
    test_release_boundaries();
    test_frame_independent_progress_and_explicit_release();
    test_frame_independent_unwind();
    test_release_stability();
    test_cancellation_consumes_input();
    test_blocking_and_hard_cancellation();
    test_duplicate_and_invalid_events();
    test_monotonic_time_and_long_uptime();
    test_queued_release_timestamps();
    puts("Reset Confirm hold/release state-machine tests: PASS");
    return 0;
}
