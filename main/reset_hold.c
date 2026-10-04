#include "reset_hold.h"

void reset_hold_init(reset_hold_t *state) {
    if (state) *state = (reset_hold_t){0};
}

void reset_hold_cancel(reset_hold_t *state) {
    reset_hold_init(state);
}

static bool observe_time(reset_hold_t *state, uint64_t now_ms) {
    if (now_ms < state->last_now_ms) {
        reset_hold_cancel(state);
        return false;
    }
    state->last_now_ms = now_ms;
    return true;
}

static void update_progress(reset_hold_t *state, uint64_t now_ms) {
    const uint64_t elapsed = now_ms - state->pressed_at_ms;
    if (elapsed < RESET_HOLD_TAP_MS) return;
    state->visible = true;
    if (state->blocked) return;
    if (elapsed >= RESET_HOLD_DURATION_MS) {
        state->progress1000 = 1000;
        state->armed = true;
        state->waiting_release = true;
    } else {
        /* Multiply only after bounding elapsed, so long uptime cannot wrap. */
        state->progress1000 = (uint16_t)((elapsed - RESET_HOLD_TAP_MS) * 1000 /
            (RESET_HOLD_DURATION_MS - RESET_HOLD_TAP_MS));
    }
}

void reset_hold_press(reset_hold_t *state, uint64_t now_ms, bool blocked) {
    if (!state) return;
    if (state->cancelling) {
        /* Captured input may be older than the last tick. Consume it without
         * restarting the animation or dropping the invisible quarantine. */
        state->release_seen = false;
        return;
    }
    if (state->active) {
        if (!observe_time(state, now_ms)) return;
        /* Duplicate DOWN never restarts a held gesture. A new DOWN during
         * the release guard invalidates the pending sleep intent. */
        if (!state->pressed || (blocked && !state->blocked)) {
            reset_hold_cancel(state);
        }
        return;
    }
    if (state->input_cutoff && now_ms <= state->last_now_ms) return;
    *state = (reset_hold_t){
        .active = true,
        .blocked = blocked,
        .pressed = true,
        .pressed_at_ms = now_ms,
        .last_now_ms = now_ms,
    };
}

reset_hold_event_t reset_hold_release(reset_hold_t *state, uint64_t event_at_ms,
                                     uint64_t observed_at_ms) {
    if (!state || !state->active) return RESET_HOLD_NONE;
    if (!observe_time(state, observed_at_ms)) return RESET_HOLD_CANCEL;
    if (state->cancelling) {
        state->release_seen = false;
        return RESET_HOLD_NONE;
    }
    if (!state->pressed) return RESET_HOLD_NONE;
    /* Queued RELEASE can be older than the latest animation/owner-task tick.
     * Preserve the actual held interval rather than stretching it by queue
     * latency, but reject a release captured before this gesture began. */
    if (event_at_ms < state->pressed_at_ms || event_at_ms > observed_at_ms) {
        reset_hold_cancel(state);
        return RESET_HOLD_CANCEL;
    }
    const uint64_t elapsed = event_at_ms - state->pressed_at_ms;
    if (elapsed < RESET_HOLD_TAP_MS) {
        reset_hold_cancel(state);
        return RESET_HOLD_TAP;
    }
    if (state->blocked || elapsed < RESET_HOLD_DURATION_MS) {
        /* Do not advance to the captured release's progress: unwind the last
         * value actually presented, including a prematurely displayed full
         * ring if the queued release predates a newer tick. */
        state->cancelling = true;
        state->armed = false;
        state->waiting_release = false;
        state->pressed = false;
        state->release_seen = false;
        state->cancelled_at_ms = observed_at_ms;
        state->cancel_progress1000 = state->progress1000;
        return RESET_HOLD_CANCEL;
    }
    update_progress(state, event_at_ms);
    state->pressed = false;
    /* RELEASE alone does not prove every key is physically released. */
    state->release_seen = false;
    return RESET_HOLD_NONE;
}

reset_hold_event_t reset_hold_tick(reset_hold_t *state, uint64_t now_ms,
                                   bool all_keys_released) {
    if (!state || !state->active) return RESET_HOLD_NONE;
    if (!observe_time(state, now_ms)) return RESET_HOLD_CANCEL;
    if (state->pressed) {
        update_progress(state, now_ms);
        return RESET_HOLD_NONE;
    }
    if (state->cancelling) {
        const uint64_t elapsed = now_ms - state->cancelled_at_ms;
        if (elapsed >= RESET_HOLD_CANCEL_MS) {
            state->progress1000 = 0;
            state->visible = false;
        } else {
            state->progress1000 = (uint16_t)(state->cancel_progress1000 *
                (RESET_HOLD_CANCEL_MS - elapsed) / RESET_HOLD_CANCEL_MS);
        }
    }
    if ((!state->armed && !state->cancelling) || !all_keys_released) {
        state->release_seen = false;
        return RESET_HOLD_NONE;
    }
    if (!state->release_seen) {
        state->release_seen = true;
        state->released_at_ms = now_ms;
        return RESET_HOLD_NONE;
    }
    if (now_ms - state->released_at_ms < RESET_HOLD_RELEASE_GUARD_MS) {
        return RESET_HOLD_NONE;
    }
    if (state->cancelling && state->visible) return RESET_HOLD_NONE;
    const bool cancelled = state->cancelling;
    const reset_hold_event_t event = cancelled ? RESET_HOLD_NONE : RESET_HOLD_SLEEP;
    reset_hold_cancel(state);
    if (cancelled) {
        state->input_cutoff = true;
        state->last_now_ms = now_ms;
    }
    return event;
}
