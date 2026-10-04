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
    if (state->blocked) return;
    const uint64_t elapsed = now_ms - state->pressed_at_ms;
    if (elapsed >= RESET_HOLD_DURATION_MS) {
        state->progress1000 = 1000;
        state->armed = true;
        state->waiting_release = true;
    } else {
        /* Multiply only after bounding elapsed, so long uptime cannot wrap. */
        state->progress1000 = (uint16_t)(elapsed * 1000 / RESET_HOLD_DURATION_MS);
    }
}

void reset_hold_press(reset_hold_t *state, uint64_t now_ms, bool blocked) {
    if (!state) return;
    if (state->visible) {
        if (!observe_time(state, now_ms)) return;
        /* Duplicate DOWN never restarts a held gesture. A new DOWN during
         * the release guard invalidates the pending sleep intent. */
        if (!state->pressed || (blocked && !state->blocked)) {
            reset_hold_cancel(state);
        }
        return;
    }
    *state = (reset_hold_t){
        .visible = true,
        .blocked = blocked,
        .pressed = true,
        .pressed_at_ms = now_ms,
        .last_now_ms = now_ms,
    };
}

reset_hold_event_t reset_hold_release(reset_hold_t *state, uint64_t now_ms) {
    if (!state || !state->visible || !state->pressed) return RESET_HOLD_NONE;
    /* Queued RELEASE can be older than the latest animation/owner-task tick.
     * Preserve the actual held interval rather than stretching it by queue
     * latency, but reject a release captured before this gesture began. */
    if (now_ms < state->pressed_at_ms) {
        reset_hold_cancel(state);
        return RESET_HOLD_CANCEL;
    }
    if (now_ms > state->last_now_ms) state->last_now_ms = now_ms;
    const uint64_t elapsed = now_ms - state->pressed_at_ms;
    if (elapsed < RESET_HOLD_TAP_MS) {
        reset_hold_cancel(state);
        return RESET_HOLD_TAP;
    }
    if (state->blocked || elapsed < RESET_HOLD_DURATION_MS) {
        reset_hold_cancel(state);
        return RESET_HOLD_CANCEL;
    }
    update_progress(state, now_ms);
    state->pressed = false;
    /* RELEASE alone does not prove every key is physically released. */
    state->release_seen = false;
    return RESET_HOLD_NONE;
}

reset_hold_event_t reset_hold_tick(reset_hold_t *state, uint64_t now_ms,
                                   bool all_keys_released) {
    if (!state || !state->visible) return RESET_HOLD_NONE;
    if (!observe_time(state, now_ms)) return RESET_HOLD_CANCEL;
    if (state->pressed) {
        update_progress(state, now_ms);
        return RESET_HOLD_NONE;
    }
    if (!state->armed || !all_keys_released) {
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
    reset_hold_cancel(state);
    return RESET_HOLD_SLEEP;
}
