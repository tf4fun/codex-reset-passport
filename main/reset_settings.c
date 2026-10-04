#include "reset_settings.h"
#include <stdio.h>
#include <string.h>
void reset_settings_defaults(reset_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->wifi_enabled = true;
    s->interval_minutes = 15;
    s->brightness = 45;
}
bool reset_settings_valid(const reset_settings_t *s) {
    return s && (s->interval_minutes == 5 || s->interval_minutes == 15 ||
        s->interval_minutes == 30 || s->interval_minutes == 60) &&
        s->utc_offset_minutes >= -720 && s->utc_offset_minutes <= 840 &&
        s->utc_offset_minutes % 15 == 0 &&
        (s->brightness == 20 || s->brightness == 45 || s->brightness == 75);
}
void reset_settings_zone_label(int minutes, char *out, size_t size) {
    if (!out || !size) return;
    if (minutes < -720 || minutes > 840 || minutes % 15 != 0 || minutes == 0) {
        snprintf(out, size, "UTC");
        return;
    }
    unsigned value = minutes < 0 ? (unsigned)-minutes : (unsigned)minutes;
    snprintf(out, size, "UTC%c%02u:%02u", minutes < 0 ? '-' : '+', value / 60, value % 60);
}
#ifdef ESP_PLATFORM
#include "nvs.h"
#include "nvs_flash.h"
esp_err_t reset_settings_load(reset_settings_t *s) {
    reset_settings_defaults(s);
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err; /* Never erase unrelated NVS on failure. */
    nvs_handle_t handle;
    err = nvs_open("reset_settings", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    reset_settings_t candidate = *s;
    uint8_t enabled = 1;
    (void)nvs_get_u8(handle, "wifi", &enabled);
    candidate.wifi_enabled = enabled != 0;
    (void)nvs_get_u16(handle, "interval", &candidate.interval_minutes);
    (void)nvs_get_i16(handle, "offset", &candidate.utc_offset_minutes);
    (void)nvs_get_u8(handle, "brightness", &candidate.brightness);
    nvs_close(handle);
    if (enabled <= 1 && reset_settings_valid(&candidate)) *s = candidate;
    return ESP_OK;
}
esp_err_t reset_settings_save(const reset_settings_t *s) {
    if (!reset_settings_valid(s)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("reset_settings", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "wifi", s->wifi_enabled);
    if (err == ESP_OK) err = nvs_set_u16(handle, "interval", s->interval_minutes);
    if (err == ESP_OK) err = nvs_set_i16(handle, "offset", s->utc_offset_minutes);
    if (err == ESP_OK) err = nvs_set_u8(handle, "brightness", s->brightness);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
#endif
