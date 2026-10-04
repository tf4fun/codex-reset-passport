#pragma once

#include <stdint.h>
#include "esp_err.h"

// Manual hold or policy-admitted automatic sleep. Call from the application owner
// task, never from a button/LVGL callback. Stop feed/radio work before prepare.
// Waits at most release_timeout_ms (capped at 2000 ms) for stable ADC release,
// then safely releases ADC buttons and arms only the C3 GPIO LOW wake source.
// On failure buttons are restored; no display/I2C/audio pins are detached.
esp_err_t reset_power_prepare_deep_sleep(uint32_t release_timeout_ms);

// Reversible cancellation before enter's terminal boundary; disarms wake and
// restores the original BSP button callback. Main must also resume its services.
esp_err_t reset_power_cancel_deep_sleep(void);

// Main must save RTC cache/settings and finish stopping every network, battery,
// audio and UI producer before this call. Returns errors only before terminal
// peripheral suspend. CW2017 suspend begins the terminal boundary (no wake API):
// thereafter sleep or controlled restart is mandatory, including on failure.
// Does not save application data, kill tasks, enable timer wake or use networking.
esp_err_t reset_power_enter_deep_sleep(void);
