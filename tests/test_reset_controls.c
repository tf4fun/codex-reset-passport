#include "reset_controls.h"
#include "reset_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    reset_controls_t s = {.page = RESET_UI_HOME};
    assert(reset_controls_handle(&s, RESET_INPUT_UP) == RESET_ACTION_NONE && s.page == RESET_UI_SETTINGS);
    assert(!s.editing);
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_NONE && s.editing);
    reset_controls_handle(&s, RESET_INPUT_UP); assert(s.selected == 6);
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_NONE);
    assert(s.page == RESET_UI_HOME && !s.editing); /* Explicit return row. */
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_READING_OPEN && s.page == RESET_UI_READING);
    assert(reset_controls_handle(&s, RESET_INPUT_UP) == RESET_ACTION_READING_PREVIOUS);
    assert(reset_controls_handle(&s, RESET_INPUT_DOWN) == RESET_ACTION_READING_NEXT);
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_NONE && s.page == RESET_UI_HOME);
    reset_controls_handle(&s, RESET_INPUT_DOWN); assert(s.page == RESET_UI_OVERVIEW);
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_REFRESH);
    reset_controls_handle(&s, RESET_INPUT_DOWN); assert(s.page == RESET_UI_STATS);
    reset_controls_handle(&s, RESET_INPUT_DOWN); assert(s.page == RESET_UI_SETTINGS);
    reset_controls_handle(&s, RESET_INPUT_DOWN); assert(s.page == RESET_UI_HOME);
    const reset_ui_page_t pages[] = {RESET_UI_HOME, RESET_UI_OVERVIEW, RESET_UI_STATS, RESET_UI_READING,
        RESET_UI_SETTINGS, RESET_UI_TIMEZONE, RESET_UI_SETUP, RESET_UI_CLEAR};
    for (unsigned i = 0; i < sizeof(pages)/sizeof(pages[0]); ++i) {
        s = (reset_controls_t){.page = pages[i], .editing = true};
        assert(reset_controls_handle(&s, RESET_INPUT_OK_LONG) == RESET_ACTION_NONE);
        assert(s.page == pages[i]); /* Derived LONG is ignored; reset_hold owns gesture. */
    }
    s.page = RESET_UI_SLEEP_WAIT;
    assert(reset_controls_handle(&s, RESET_INPUT_DOWN) == RESET_ACTION_CANCEL_SLEEP && s.page == RESET_UI_SETTINGS);
    s.page = RESET_UI_CLEAR;
    assert(reset_controls_handle(&s, RESET_INPUT_DOWN) == RESET_ACTION_NONE && s.page == RESET_UI_SETUP);
    s.page = RESET_UI_CLEAR;
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_CLEAR && s.page == RESET_UI_SETUP);
    s.page = RESET_UI_TIMEZONE;
    assert(reset_controls_handle(&s, RESET_INPUT_UP) == RESET_ACTION_TIMEZONE_UP && s.page == RESET_UI_TIMEZONE);
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_TIMEZONE_SAVE && s.page == RESET_UI_SETTINGS);
    s.page = RESET_UI_SETUP;
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_STOP_SETUP && s.page == RESET_UI_SETTINGS);

    /* Wake input is blocked while any key is held, then past the 180ms click
     * delay until 250ms of continuous release. The wake Confirm is consumed. */
    reset_input_gate_t gate = {0};
    assert(!reset_input_gate_observe(&gate, false, 0));
    assert(!reset_input_gate_observe(&gate, false, 5000));
    assert(!reset_input_gate_observe(&gate, true, 5001));
    assert(!reset_input_gate_observe(&gate, true, 5181));
    assert(!reset_input_gate_observe(&gate, true, 5250));
    assert(reset_input_gate_observe(&gate, true, 5251));
    assert(!reset_input_gate_observe(&gate, false, 5260));
    assert(!reset_input_gate_observe(&gate, true, 5270));
    assert(!reset_input_gate_observe(&gate, true, 5269)); /* Clock rollback resets guard. */
    assert(reset_input_gate_observe(&gate, true, 5519));

    /* A released Down takes effect before the following fast Confirm tap.
     * Delayed driver CLICK is not used by either input path. */
    reset_direction_tap_t down = {0};
    s = (reset_controls_t){.page = RESET_UI_SETTINGS, .editing = true, .selected = 0};
    reset_direction_press(&down, 100);
    assert(reset_direction_release(&down, 200));
    assert(reset_controls_handle(&s, RESET_INPUT_DOWN) == RESET_ACTION_NONE && s.selected == 1);
    assert(reset_controls_handle(&s, RESET_INPUT_OK) == RESET_ACTION_CHANGE_SETTING && s.selected == 1);
    assert(!reset_direction_release(&down, 210)); /* No duplicate release. */
    reset_direction_press(&down, 1000);
    reset_direction_press(&down, 1100); /* Duplicate PRESS does not restart. */
    assert(!reset_direction_release(&down, 1500)); /* Held direction is not a tap. */
    reset_direction_press(&down, 2000);
    assert(!reset_direction_release(&down, 1999));

    /* Cancellation ended after a delayed ADC observation at1640. Events
     * queued during that sample must not navigate or become Confirm taps. */
    reset_input_cutoff_t cutoff = {0};
    assert(reset_input_cutoff_accepts(&cutoff, 0));
    reset_input_cutoff_mark(&cutoff, 1640);
    assert(!reset_input_cutoff_accepts(&cutoff, 1540));
    assert(!reset_input_cutoff_accepts(&cutoff, 1620));
    assert(!reset_input_cutoff_accepts(&cutoff, 1640));
    assert(reset_input_cutoff_accepts(&cutoff, 1641));
    reset_input_cutoff_mark(&cutoff, 1530); /* Never move a fence backwards. */
    assert(cutoff.through_ms == 1640);
    s = (reset_controls_t){.page = RESET_UI_SETTINGS, .editing = true, .selected = 0};
    down = (reset_direction_tap_t){0};
    if (reset_input_cutoff_accepts(&cutoff, 1540)) reset_direction_press(&down, 1540);
    if (reset_input_cutoff_accepts(&cutoff, 1620) && reset_direction_release(&down, 1620))
        (void)reset_controls_handle(&s, RESET_INPUT_DOWN);
    assert(s.selected == 0);
    reset_direction_press(&down, 1700);
    assert(reset_input_cutoff_accepts(&cutoff, 1780) && reset_direction_release(&down, 1780));
    (void)reset_controls_handle(&s, RESET_INPUT_DOWN);
    assert(s.selected == 1); /* A genuinely new tap still works. */

    reset_settings_t settings; reset_settings_defaults(&settings); assert(reset_settings_valid(&settings));
    settings.interval_minutes = 1; assert(!reset_settings_valid(&settings));
    settings.interval_minutes = 15; settings.utc_offset_minutes = 841; assert(!reset_settings_valid(&settings));
    settings.utc_offset_minutes = 345; assert(reset_settings_valid(&settings));
    char zone[32]; reset_settings_zone_label(345, zone, sizeof(zone)); assert(!strcmp(zone, "UTC+05:45"));
    reset_settings_zone_label(-720, zone, sizeof(zone)); assert(!strcmp(zone, "UTC-12:00"));
    reset_settings_zone_label(840, zone, sizeof(zone)); assert(!strcmp(zone, "UTC+14:00"));
    puts("Reset tap navigation/settings and wake gate tests: PASS");
    return 0;
}
