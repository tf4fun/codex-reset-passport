#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The hidden tap window is part of the total hold duration. */
#define RESET_HOLD_TAP_MS UINT64_C(500)
#define RESET_HOLD_DURATION_MS UINT64_C(2000)
#define RESET_HOLD_CANCEL_MS UINT64_C(250)
#define RESET_HOLD_RELEASE_GUARD_MS UINT64_C(250)

typedef enum {
    RESET_HOLD_NONE = 0,
    RESET_HOLD_TAP,
    RESET_HOLD_CANCEL,
    RESET_HOLD_SLEEP,
} reset_hold_event_t;

typedef struct {
    /* Read-only owner/presentation state; progress is 0..1000.
     * Tick while active, including the hidden delay and release quarantine.
     * Swallow ALL ordinary input while cancelling, even when invisible. */
    bool active;
    bool cancelling;
    bool visible;
    uint16_t progress1000;
    bool armed;
    bool blocked;
    bool waiting_release;

    /* Private timing/input state. Initialize with reset_hold_init() or {0}. */
    bool pressed;
    bool release_seen;
    bool input_cutoff;
    uint64_t pressed_at_ms;
    uint64_t released_at_ms;
    uint64_t last_now_ms;
    uint64_t cancelled_at_ms;
    uint16_t cancel_progress1000;
} reset_hold_t;

/* All calls belong to the input owner task, never a button/LVGL callback.
 * Use monotonic milliseconds (including timestamps captured at PRESS/RELEASE),
 * not animation frames or wall-clock time. A RELEASE timestamp may precede a
 * newer tick when input was queued; duration uses its captured timestamp.
 * A RELEASE before PRESS or a backward observation cancels the gesture.
 * The caller must cancel on another key, focus loss, queue overflow, or a new
 * blocking condition. A blocked press remains blocked for the whole gesture. */
void reset_hold_init(reset_hold_t *state);
/* After cancellation becomes idle, queued DOWN events captured at/before the
 * final idle observation are ignored. A genuinely later press starts fresh. */
void reset_hold_press(reset_hold_t *state, uint64_t now_ms, bool blocked);

/* Only an actual RELEASE ends the held interval. event_at_ms is its captured
 * timestamp; observed_at_ms is the current owner-task time. <500 ms returns
 * TAP and clears the state. 500..1999 ms (or a blocked hold) returns CANCEL,
 * unarms, and unwinds the displayed progress over 250 ms from observed_at_ms.
 * CANCEL does NOT mean the state is idle: tick until active becomes false.
 * A completed hold returns NONE and waits for tick() to verify ALL keys up.
 * Presses/releases during cancellation are consumed and cannot create TAP. */
reset_hold_event_t reset_hold_release(reset_hold_t *state, uint64_t event_at_ms,
                                     uint64_t observed_at_ms);

/* An armed hold never emits SLEEP while pressed. After RELEASE, require
 * 250 ms of continuously observed all_keys_released before emitting SLEEP
 * exactly once and clearing the state. False includes ADC/read failure or
 * any key still held, and restarts the release guard. Cancellation requires
 * the same stable release observation before becoming idle, but never sleeps.
 * Its popup disappears after 250 ms even if input remains quarantined.
 * This is only the input intent: the BSP/power handoff must verify GPIO HIGH. */
reset_hold_event_t reset_hold_tick(reset_hold_t *state, uint64_t now_ms,
                                   bool all_keys_released);
/* Immediate hard reset for focus loss, other keys, overflow or blocking.
 * The owner must separately quarantine input after unsafe/lost events. */
void reset_hold_cancel(reset_hold_t *state);
