#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Tap and wake-release guard are deliberately different durations. */
#define RESET_HOLD_TAP_MS UINT64_C(500)
#define RESET_HOLD_DURATION_MS UINT64_C(2000)
#define RESET_HOLD_RELEASE_GUARD_MS UINT64_C(250)

typedef enum {
    RESET_HOLD_NONE = 0,
    RESET_HOLD_TAP,
    RESET_HOLD_CANCEL,
    RESET_HOLD_SLEEP,
} reset_hold_event_t;

typedef struct {
    /* Read-only presentation state; progress is 0..1000. */
    bool visible;
    uint16_t progress1000;
    bool armed;
    bool blocked;
    bool waiting_release;

    /* Private timing/input state. Initialize with reset_hold_init() or {0}. */
    bool pressed;
    bool release_seen;
    uint64_t pressed_at_ms;
    uint64_t released_at_ms;
    uint64_t last_now_ms;
} reset_hold_t;

/* All calls belong to the input owner task, never a button/LVGL callback.
 * Use monotonic milliseconds (including timestamps captured at PRESS/RELEASE),
 * not animation frames or wall-clock time. A RELEASE timestamp may precede a
 * newer tick when input was queued; duration uses its captured timestamp.
 * A RELEASE before PRESS or a backward tick cancels the gesture.
 * The caller must cancel on another key, focus loss, queue overflow, or a new
 * blocking condition. A blocked press remains blocked for the whole gesture. */
void reset_hold_init(reset_hold_t *state);
void reset_hold_press(reset_hold_t *state, uint64_t now_ms, bool blocked);

/* Only an actual RELEASE ends the held interval. <500 ms returns TAP;
 * 500..1999 ms (or a blocked hold) returns CANCEL. A completed hold returns
 * NONE and waits for tick() to verify that ALL keys have been released. */
reset_hold_event_t reset_hold_release(reset_hold_t *state, uint64_t now_ms);

/* An armed hold never emits SLEEP while pressed. After RELEASE, require
 * 250 ms of continuously observed all_keys_released before emitting SLEEP
 * exactly once and clearing the state. False includes ADC/read failure or
 * any key still held, and restarts the release guard. This is only the input
 * intent: the existing BSP/power handoff must still verify safe GPIO HIGH. */
reset_hold_event_t reset_hold_tick(reset_hold_t *state, uint64_t now_ms,
                                   bool all_keys_released);
void reset_hold_cancel(reset_hold_t *state);
