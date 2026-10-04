#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RESET_IDLE_RETRY_MS UINT64_C(300000)

typedef struct {
    /* Private policy state. Initialize with {0}; owned by the input task. */
    uint64_t last_now_ms;
    uint64_t elapsed_ms;
    uint64_t retry_remaining_ms;
    bool initialized;
    bool paused;
} reset_idle_t;

/* Use monotonic milliseconds, never wall-clock time. Only actual user input
 * sets user_activity: HTTP polls, refresh results and animation ticks do not.
 * A pause freezes accumulated idle time without discarding it. Observe both
 * pause transitions, then continue observing while paused so input still
 * resets idle. The elapsed interval belongs to the preceding pause state.
 * Pass paused for pairing, held keys/gestures, cancellation quarantine and
 * sleep admission. This helper does not access the device or schedule wakeup.
 * A backward clock observation resets idle and restarts any active retry
 * cooldown, rather than allowing unsigned wraparound to trigger sleep. */
void reset_idle_observe(reset_idle_t *state, uint64_t now_ms,
                        bool user_activity, bool paused);

/* Read-only query. Valid settings are 5/10/30 minutes, or 0 to disable.
 * An uninitialized, paused or backward-time query is never due. Prefer query
 * immediately after observe so current input and admission blockers apply. */
bool reset_idle_due(const reset_idle_t *state, uint16_t sleep_minutes,
                    uint64_t now_ms);

/* Preserve elapsed idle time, but wait at least five monotonic minutes before
 * retrying a cancelled/failed sleep. Cooldown elapses even while paused. Call
 * this for reversible admission failure or cancellation, never to enter sleep. */
void reset_idle_defer_after_cancel_or_failure(reset_idle_t *state,
                                             uint64_t now_ms);

#define RESET_IDLE_DIM_MS UINT64_C(30000)
#define RESET_IDLE_GRACE_MS UINT64_C(15000)
typedef struct {
    bool screen_off;
    bool grace_active;
    uint64_t grace_started_ms;
} reset_idle_screen_t;

/* Called after observe. The grace starts at actual screen-off, not a stale
 * idle deadline. Blockers cancel admission, failure preserves the dark screen.
 * Off or real input cancels both immediately. No hardware is touched here. */
bool reset_idle_screen_tick(reset_idle_screen_t *screen, const reset_idle_t *idle,
                            uint16_t minutes, uint64_t now_ms, bool activity);
void reset_idle_screen_failed(reset_idle_screen_t *screen);
int reset_idle_brightness(const reset_idle_screen_t *screen,
                         const reset_idle_t *idle, int brightness);
