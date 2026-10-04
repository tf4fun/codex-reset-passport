#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_SLEEP_WAKEUP_ALL 0
#define ESP_GPIO_WAKEUP_GPIO_LOW 0
esp_err_t esp_sleep_disable_wakeup_source(int source);
bool esp_sleep_is_valid_wakeup_gpio(int gpio);
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int mode);
void esp_deep_sleep_start(void);
