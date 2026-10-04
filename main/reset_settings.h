#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define RESET_DEFAULT_SLEEP_MINUTES 5
typedef struct {
    bool wifi_enabled;
    uint16_t interval_minutes;
    int16_t utc_offset_minutes; /* Explicit fixed offset, never inferred; no DST. */
    uint8_t brightness;
    uint16_t sleep_minutes; /* Persisted key unchanged: 0 = off; 5/10/30 minutes to screen-off. */
} reset_settings_t;
void reset_settings_defaults(reset_settings_t *settings);
bool reset_settings_valid(const reset_settings_t *settings);
void reset_settings_zone_label(int minutes, char *out, size_t capacity);
#ifdef ESP_PLATFORM
#include "esp_err.h"
esp_err_t reset_settings_load(reset_settings_t *settings);
esp_err_t reset_settings_save(const reset_settings_t *settings);
#endif
