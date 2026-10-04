#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    RESET_UI_HOME = 0, RESET_UI_OVERVIEW = 1, RESET_UI_STATS = 2, RESET_UI_SETTINGS = 3,
    RESET_UI_READING,
    RESET_UI_SETUP, RESET_UI_CLEAR, RESET_UI_TIMEZONE,
    RESET_UI_SLEEP_WAIT
} reset_ui_page_t;
typedef struct {
    reset_ui_page_t page;
    char battery[12];
    char status[48];
    char hero[48];
    char announcement_text[257];
    char announcement_type[48];
    char announcement_age[48];
    char announcement_date[48];
    bool announcement_truncated;
    bool next_truncated;
    bool timing_is_forecast;
    uint8_t reading_page;
    char date[48];
    char zone[32];
    char next_title[48];
    char next_body[128];
    char freshness[64];
    char detail_type[48];
    char detail_source[48];
    char detail_generated[48];
    char setup_line[72];
    char setup_timer[64];
    char setup_diagnostic[64];
    char stats_total[24];
    char stats_average[24];
    char stats_elapsed[24];
    char settings_values[7][32];
    char sleep_progress[64];
    uint8_t selected_setting;
    bool warning;
    bool connected;
    bool settings_editing;
    bool has_notice;
    /* Transient overlay fields stay last: progress never rebuilds page content. */
    bool hold_visible;
    uint16_t hold_progress; /* 0..1000, supplied from monotonic hold elapsed time. */
    bool hold_armed;
    bool hold_blocked;
    bool hold_released;
} reset_ui_model_t;
void reset_ui_create(void);
void reset_ui_update(const reset_ui_model_t *model);

/* After reset_ui_update, under the same LVGL lock. Reader index is clamped. */
uint8_t reset_ui_reading_page_count(void);
