#pragma once
#include "reset_feed.h"
#include "reset_ui.h"
#include "reset_settings.h"
typedef struct {
    reset_ui_page_t page;
    bool connected, has_credentials, provisioning, wifi_error, clock_ready;
    bool refresh_throttled, settings_editing, restored_cache;
    bool radio_stopped, radio_control_pending;
    int battery;
    unsigned provisioning_seconds;
    unsigned selected_setting;
    unsigned sleep_phase;
    unsigned message; /* 1: save error; 2: sleep error; 3: Wi-Fi off. */
    int16_t draft_utc_offset;
    int64_t now;
    reset_settings_t settings;
} reset_presenter_state_t;
void reset_presenter_build(const reset_feed_snapshot_t *feed,
                           const reset_presenter_state_t *state,
                           reset_ui_model_t *out);
