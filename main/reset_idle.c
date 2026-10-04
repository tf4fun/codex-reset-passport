#include "reset_idle.h"

#include <stddef.h>

static uint64_t saturating_add(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

void reset_idle_observe(reset_idle_t *s, uint64_t now_ms,
                        bool user_activity, bool paused) {
    if (!s) return;
    if (!s->initialized) {
        *s = (reset_idle_t){.last_now_ms = now_ms, .initialized = true,
                            .paused = paused};
        return;
    }
    if (now_ms < s->last_now_ms) {
        s->elapsed_ms = 0;
        if (s->retry_remaining_ms) s->retry_remaining_ms = RESET_IDLE_RETRY_MS;
    } else {
        uint64_t delta_ms = now_ms - s->last_now_ms;
        if (!s->paused) s->elapsed_ms = saturating_add(s->elapsed_ms, delta_ms);
        s->retry_remaining_ms = delta_ms >= s->retry_remaining_ms ? 0 :
            s->retry_remaining_ms - delta_ms;
    }
    if (user_activity) s->elapsed_ms = 0;
    s->last_now_ms = now_ms;
    s->paused = paused;
}

bool reset_idle_due(const reset_idle_t *s, uint16_t sleep_minutes,
                    uint64_t now_ms) {
    if (!s || !s->initialized || s->paused || now_ms < s->last_now_ms ||
        (sleep_minutes != 5 && sleep_minutes != 10 && sleep_minutes != 30))
        return false;
    uint64_t delta_ms = now_ms - s->last_now_ms;
    if (delta_ms < s->retry_remaining_ms) return false;
    return saturating_add(s->elapsed_ms, delta_ms) >=
        (uint64_t)sleep_minutes * UINT64_C(60000);
}

void reset_idle_defer_after_cancel_or_failure(reset_idle_t *s,
                                             uint64_t now_ms) {
    if (!s) return;
    reset_idle_observe(s, now_ms, false, s->paused);
    s->retry_remaining_ms = RESET_IDLE_RETRY_MS;
}

bool reset_idle_screen_tick(reset_idle_screen_t *screen, const reset_idle_t *idle,
                            uint16_t minutes, uint64_t now_ms, bool activity) {
    if (!screen || !idle) return false;
    if (!minutes || activity) {
        *screen = (reset_idle_screen_t){0};
        return false;
    }
    if (!reset_idle_due(idle, minutes, now_ms)) {
        screen->grace_active = false;
        return false;
    }
    if (!screen->grace_active || now_ms < screen->grace_started_ms) {
        screen->screen_off = true;
        screen->grace_active = true;
        screen->grace_started_ms = now_ms;
        return false;
    }
    return now_ms - screen->grace_started_ms >= RESET_IDLE_GRACE_MS;
}

void reset_idle_screen_failed(reset_idle_screen_t *screen) {
    if (screen) screen->grace_active = false;
}

int reset_idle_brightness(const reset_idle_screen_t *screen,
                         const reset_idle_t *idle, int brightness) {
    if (screen && screen->screen_off) return 0;
    if (idle && !idle->paused && idle->elapsed_ms >= RESET_IDLE_DIM_MS)
        return brightness < 8 ? brightness : 8;
    return brightness;
}
