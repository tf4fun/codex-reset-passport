#pragma once
#include "reset_ui.h"
#include <stdbool.h>
typedef enum { RESET_INPUT_UP, RESET_INPUT_DOWN, RESET_INPUT_OK, RESET_INPUT_OK_LONG } reset_input_t;
typedef enum {
    RESET_ACTION_NONE, RESET_ACTION_REFRESH, RESET_ACTION_CHANGE_SETTING,
    RESET_ACTION_READING_OPEN, RESET_ACTION_READING_PREVIOUS, RESET_ACTION_READING_NEXT,
    RESET_ACTION_TIMEZONE_UP, RESET_ACTION_TIMEZONE_DOWN, RESET_ACTION_TIMEZONE_SAVE,
    RESET_ACTION_START_SETUP, RESET_ACTION_STOP_SETUP, RESET_ACTION_CLEAR,
    RESET_ACTION_SLEEP, RESET_ACTION_CANCEL_SLEEP
} reset_action_t;
typedef struct { reset_ui_page_t page; unsigned selected; bool editing; } reset_controls_t;
reset_action_t reset_controls_handle(reset_controls_t *state, reset_input_t input);

/* Longer than the board single-click delay: consume the wake/cancel key. */
#define RESET_WAKE_RELEASE_GUARD_MS UINT64_C(250)
typedef struct { bool release_seen; uint64_t released_at_ms; } reset_input_gate_t;
bool reset_input_gate_observe(reset_input_gate_t *gate, bool released, uint64_t now_ms);

/* Direction taps use immediate debounced RELEASE too, preserving ordering
 * against Confirm taps instead of waiting for delayed driver CLICK events. */
typedef struct { bool pressed; uint64_t pressed_at_ms; } reset_direction_tap_t;
void reset_direction_press(reset_direction_tap_t *state, uint64_t at_ms);
bool reset_direction_release(reset_direction_tap_t *state, uint64_t at_ms);

/* Owner-level fence applies to every key, including events enqueued while a
 * blocking ADC sample completes the cancellation quarantine. */
typedef struct { bool valid; uint64_t through_ms; } reset_input_cutoff_t;
void reset_input_cutoff_mark(reset_input_cutoff_t *cutoff, uint64_t observed_at_ms);
bool reset_input_cutoff_accepts(const reset_input_cutoff_t *cutoff, uint64_t event_at_ms);
