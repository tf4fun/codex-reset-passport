#pragma once
static inline void reset_power_test_log(const char *tag, const char *fmt, ...) {
    (void)tag; (void)fmt;
}
#define ESP_LOGE(...) reset_power_test_log(__VA_ARGS__)
#define ESP_LOGW(...) reset_power_test_log(__VA_ARGS__)
#define ESP_LOGI(...) reset_power_test_log(__VA_ARGS__)
