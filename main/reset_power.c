#include "reset_power.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "sdkconfig.h"

#if CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS
#error "The ADC button ladder uses an external pull-up; disable internal sleep GPIO resistors"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "reset_power";
static bool s_prepared;
static bool s_audio_control_ready;

static void terminal_restart(const char *step, esp_err_t error)
{
    ESP_LOGE(TAG, "Terminal sleep failed at %s: %s; restarting", step,
             esp_err_to_name(error));
    esp_restart();
    // esp_restart is noreturn on hardware. Even a faulty replacement may not
    // return to an application whose peripheral buses have been detached.
    for (;;) vTaskDelay(portMAX_DELAY);
}

esp_err_t reset_power_cancel_deep_sleep(void)
{
    // ALL also removes any earlier timer wake configuration. Never add a timer
    // as a fallback: this manual mode must stay asleep until the physical key.
    esp_err_t wake = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    s_prepared = false;
    esp_err_t buttons = bsp_button_resume_after_deep_sleep_cancel();
    // A late key/LVGL cancellation after audio_init must not leave I2S clocks
    // running in an application which otherwise never uses audio.
    esp_err_t audio = s_audio_control_ready ? bsp_audio_sleep() : ESP_OK;
    if (audio == ESP_OK) s_audio_control_ready = false;
    if (buttons != ESP_OK) return buttons;
    return audio != ESP_OK ? audio : wake;
}

esp_err_t reset_power_prepare_deep_sleep(uint32_t release_timeout_ms)
{
    if (s_prepared) return ESP_ERR_INVALID_STATE;
    esp_err_t e = esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (e != ESP_OK) return e;
    int gpio = -1;
    e = bsp_button_prepare_deep_sleep(release_timeout_ms, &gpio);
    if (e != ESP_OK) return e;
    if (!esp_sleep_is_valid_wakeup_gpio(gpio)) {
        e = ESP_ERR_INVALID_ARG;
    } else {
        e = esp_deep_sleep_enable_gpio_wakeup(1ULL << (unsigned)gpio,
                                             ESP_GPIO_WAKEUP_GPIO_LOW);
    }
    if (e != ESP_OK) {
        esp_err_t restore = reset_power_cancel_deep_sleep();
        return restore != ESP_OK ? restore : e;
    }
    s_prepared = true;
    ESP_LOGI(TAG, "Manual deep sleep prepared: GPIO%d LOW, no timer wake", gpio);
    return ESP_OK;
}

esp_err_t reset_power_enter_deep_sleep(void)
{
    if (!s_prepared) return ESP_ERR_INVALID_STATE;
    if (bsp_button_deep_sleep_level() != 1) return ESP_ERR_INVALID_STATE;

    // The normal countdown app never initializes audio. bsp_audio_sleep alone
    // would then return success without touching ES8311. Initialize its control
    // interface explicitly so the terminal suspend is real and readback-checked.
    esp_err_t e = bsp_audio_init();
    if (e != ESP_OK) return e;
    s_audio_control_ready = true;
    // Check LVGL availability before the first terminal action. Release now;
    // acquire and hold again immediately before LCD shutdown below.
    if (!bsp_lvgl_lock(250)) return ESP_ERR_TIMEOUT;
    bsp_lvgl_unlock();
    if (bsp_button_deep_sleep_level() != 1) return ESP_ERR_INVALID_STATE;

    // Terminal boundary: CW2017 sleep has no public reversible wake API.
    // A write can succeed despite a failed readback. Nothing from here may
    // return to the awake UI. Main has already quiesced feed/radio/producers,
    // saved RTC state and removed any pending automatic sleep intent.
    e = bsp_battery_sleep();
    if (e != ESP_OK) terminal_restart("CW2017 suspend", e);
    e = bsp_audio_sleep();
    if (e != ESP_OK) terminal_restart("ES8311 suspend", e);
    e = bsp_audio_prepare_deep_sleep();
    if (e != ESP_OK) terminal_restart("I2S release", e);
    e = bsp_i2c_prepare_deep_sleep();
    if (e != ESP_OK) terminal_restart("I2C release", e);
    if (!bsp_lvgl_lock(1000)) terminal_restart("LVGL lock", ESP_ERR_TIMEOUT);
    e = bsp_display_prepare_deep_sleep();
    if (e != ESP_OK) terminal_restart("LCD suspend", e);
    // Never unlock LVGL after the display terminal path. A late held key must
    // reboot safely rather than entering an immediate-wake/auto-resleep loop.
    if (bsp_button_deep_sleep_level() != 1) {
        terminal_restart("wake key pressed during shutdown", ESP_ERR_INVALID_STATE);
    }
    esp_deep_sleep_start();
    terminal_restart("deep-sleep entry returned", ESP_FAIL);
    return ESP_FAIL; // Unreachable; keeps host compilers without noreturn happy.
}
