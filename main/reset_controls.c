#include "reset_controls.h"
#include "reset_hold.h"

bool reset_input_gate_observe(reset_input_gate_t *gate, bool released, uint64_t now_ms) {
    if (!gate) return false;
    if (!released) { gate->release_seen = false; return false; }
    if (!gate->release_seen) { gate->release_seen = true; gate->released_at_ms = now_ms; return false; }
    if (now_ms < gate->released_at_ms) { gate->released_at_ms = now_ms; return false; }
    return now_ms - gate->released_at_ms >= RESET_WAKE_RELEASE_GUARD_MS;
}

void reset_direction_press(reset_direction_tap_t *state, uint64_t at_ms) {
    if (state && !state->pressed) *state = (reset_direction_tap_t){true, at_ms};
}
bool reset_direction_release(reset_direction_tap_t *state, uint64_t at_ms) {
    if (!state || !state->pressed) return false;
    state->pressed = false;
    return at_ms >= state->pressed_at_ms && at_ms - state->pressed_at_ms < RESET_HOLD_TAP_MS;
}

void reset_input_cutoff_mark(reset_input_cutoff_t *cutoff, uint64_t observed_at_ms) {
    if (!cutoff) return;
    if (!cutoff->valid || observed_at_ms > cutoff->through_ms) cutoff->through_ms = observed_at_ms;
    cutoff->valid = true;
}
bool reset_input_cutoff_accepts(const reset_input_cutoff_t *cutoff, uint64_t event_at_ms) {
    return !cutoff || !cutoff->valid || event_at_ms > cutoff->through_ms;
}

reset_action_t reset_controls_handle(reset_controls_t *s, reset_input_t input) {
    if (!s) return RESET_ACTION_NONE;
    /* Derived driver LONG is not an admission path. reset_hold owns timing. */
    if (input == RESET_INPUT_OK_LONG) return RESET_ACTION_NONE;
    if (s->page == RESET_UI_SLEEP_WAIT) {
        s->page = RESET_UI_SETTINGS; s->editing = true;
        return RESET_ACTION_CANCEL_SLEEP;
    }
    if (s->page == RESET_UI_READING) {
        if (input == RESET_INPUT_UP) return RESET_ACTION_READING_PREVIOUS;
        if (input == RESET_INPUT_DOWN) return RESET_ACTION_READING_NEXT;
        s->page = RESET_UI_HOME;
        return RESET_ACTION_NONE;
    }
    if (s->page == RESET_UI_CLEAR) {
        s->page = RESET_UI_SETUP;
        return input == RESET_INPUT_OK ? RESET_ACTION_CLEAR : RESET_ACTION_NONE;
    }
    if (s->page == RESET_UI_TIMEZONE) {
        if (input == RESET_INPUT_UP) return RESET_ACTION_TIMEZONE_UP;
        if (input == RESET_INPUT_DOWN) return RESET_ACTION_TIMEZONE_DOWN;
        s->page = RESET_UI_SETTINGS; s->editing = true;
        return RESET_ACTION_TIMEZONE_SAVE;
    }
    if (s->page == RESET_UI_SETUP) {
        if (input == RESET_INPUT_UP) return RESET_ACTION_START_SETUP;
        if (input == RESET_INPUT_DOWN) { s->page = RESET_UI_CLEAR; return RESET_ACTION_NONE; }
        s->page = RESET_UI_SETTINGS; s->editing = true;
        return RESET_ACTION_STOP_SETUP;
    }
    if (s->page == RESET_UI_SETTINGS && s->editing) {
        if (input == RESET_INPUT_UP) s->selected = (s->selected + 6) % 7;
        else if (input == RESET_INPUT_DOWN) s->selected = (s->selected + 1) % 7;
        else if (s->selected == 6) { s->page = RESET_UI_HOME; s->editing = false; }
        else return RESET_ACTION_CHANGE_SETTING;
        return RESET_ACTION_NONE;
    }
    if (input == RESET_INPUT_UP) s->page = (reset_ui_page_t)((s->page + 3) % 4);
    else if (input == RESET_INPUT_DOWN) s->page = (reset_ui_page_t)((s->page + 1) % 4);
    else if (s->page == RESET_UI_HOME) {
        s->reader_parent = s->page;
        s->page = RESET_UI_READING;
        return RESET_ACTION_READING_OPEN;
    }
    else if (s->page == RESET_UI_SETTINGS) s->editing = true;
    else if (s->page == RESET_UI_HISTORY) return RESET_ACTION_HISTORY_REFRESH;
    else return RESET_ACTION_REFRESH;
    return RESET_ACTION_NONE;
}

bool reset_controls_admit_reader(reset_controls_t *s, reset_ui_page_t displayed_page, bool available) {
    if (!s || s->page != RESET_UI_READING) return false;
    if (s->reader_parent != RESET_UI_HOME || displayed_page != RESET_UI_HOME || !available) {
        s->page = RESET_UI_HOME;
        return false;
    }
    return true;
}
