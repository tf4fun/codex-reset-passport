#pragma once
#include "reset_feed.h"
#include "reset_ui.h"
#include "reset_settings.h"
/* Pointer-free bounded reading selection. Captured when entering READING so
 * live refreshes and timezone changes cannot mix body, date and source. */
typedef struct {
    bool valid;
    char text[257];
    char type[48];
    char date[48];
    char zone[32];
    char source_url[121];
    bool source_original;
    bool truncated;
} reset_reader_snapshot_t;
typedef struct {
    reset_ui_page_t page;
    const reset_reader_snapshot_t *reader_snapshot;
    const reset_history_snapshot_t *history;
    bool connected, has_credentials, provisioning, wifi_error, clock_ready;
    bool refresh_throttled, settings_editing, restored_cache;
    bool radio_stopped, radio_control_pending;
    bool wifi_initialized, wifi_connecting;
    unsigned wifi_attempts;
    uint16_t wifi_disconnect_reason;
    int32_t wifi_last_error;
    int32_t wifi_persistence_error; /* Independent: an IP does not prove a saved credential. */
    int battery;
    unsigned provisioning_seconds;
    unsigned selected_setting;
    unsigned reading_page;
    unsigned sleep_phase;
    unsigned message; /* 1: save error; 2: sleep error; 3: Wi-Fi off. */
    int16_t draft_utc_offset;
    int64_t now;
    reset_settings_t settings;
} reset_presenter_state_t;
void reset_presenter_build(const reset_feed_snapshot_t *feed,
                           const reset_presenter_state_t *state,
                           reset_ui_model_t *out);

/* Clear *snapshot on failure. No whole-model local copy or dynamic allocation. */
bool reset_presenter_capture_reader(const reset_ui_model_t *model,
                                    reset_reader_snapshot_t *snapshot);
