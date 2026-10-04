/* Four-page reset observer. All button callbacks enqueue; no I/O in callbacks. */
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "reset_feed.h"
#include "reset_wifi.h"
#include "reset_power.h"
#include "reset_ui.h"
#include "reset_presenter.h"
#include "reset_settings.h"
#include "reset_controls.h"
#include "reset_hold.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <time.h>

static const char *TAG = "reset_observer";
typedef struct { bsp_btn_t button; bsp_btn_ev_t event; uint64_t at_ms; } input_t;
static QueueHandle_t input_queue;
static atomic_bool clock_ready, input_ready, input_overflow;
static RTC_DATA_ATTR reset_feed_rtc_state_t rtc_feed;
static RTC_DATA_ATTR uint32_t rtc_magic;
#define RTC_MAGIC UINT32_C(0x52535432)
static void clock_synced(struct timeval *tv) { (void)tv; atomic_store(&clock_ready, true); }
static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *user) {
    (void)user;
    bool wanted = event == BSP_BTN_PRESS || event == BSP_BTN_RELEASE;
    if (atomic_load(&input_ready) && input_queue && wanted) {
        const input_t input = {button, event, (uint64_t)esp_timer_get_time() / 1000};
        if (xQueueSend(input_queue, &input, 0) != pdTRUE) atomic_store(&input_overflow, true);
    }
}
static uint64_t milliseconds(void) { return (uint64_t)esp_timer_get_time() / 1000; }
static reset_input_t convert_input(const input_t *input) {
    if (input->event == BSP_BTN_LONG) return RESET_INPUT_OK_LONG;
    return input->button == BSP_BTN_UP ? RESET_INPUT_UP : input->button == BSP_BTN_DOWN ? RESET_INPUT_DOWN : RESET_INPUT_OK;
}
static void cancel_sleep(reset_controls_t *controls, unsigned *phase) {
    rtc_magic = 0;
    if (reset_power_cancel_deep_sleep() != ESP_OK) {
        ESP_LOGE(TAG, "Button/power rollback failed; restarting safely");
        esp_restart();
    }
    if (reset_wifi_resume() != ESP_OK) ESP_LOGE(TAG, "Radio resume request failed");
    reset_feed_resume(false);
    rtc_magic = 0;
    *phase = 0;
    controls->page = RESET_UI_SETTINGS;
    controls->editing = true;
    if (input_queue) xQueueReset(input_queue);
    atomic_store(&input_ready, false);
}
void app_main(void) {
    reset_settings_t settings;
    esp_err_t settings_error = reset_settings_load(&settings);
    bool restored = false;
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && rtc_magic == RTC_MAGIC)
        restored = reset_feed_import_rtc(&rtc_feed);
    rtc_magic = 0; /* A controlled restart must never automatically re-enter sleep. */
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display initialization failed"); return;
    }
    (void)bsp_i2c_init();
    (void)bsp_battery_init();
    bsp_display_backlight(settings.brightness);
    if (!bsp_lvgl_lock(1000)) return;
    reset_ui_create();
    bsp_lvgl_unlock();
    input_queue = xQueueCreate(8, sizeof(input_t));
    bool buttons_ok = input_queue && bsp_button_init(on_key, NULL) == ESP_OK;
    if (!buttons_ok) ESP_LOGE(TAG, "Buttons unavailable");
    if (reset_wifi_init(settings.wifi_enabled) != ESP_OK) ESP_LOGE(TAG, "Wi-Fi service unavailable");
    (void)reset_feed_set_auto_interval(settings.interval_minutes);
    if (reset_feed_start() != ESP_OK) ESP_LOGE(TAG, "Feed service unavailable");
    bool sntp_started = false;
    reset_controls_t controls = {.page = RESET_UI_HOME};
    int16_t draft_offset = settings.utc_offset_minutes;
    int applied_brightness = settings.brightness;
    uint64_t last_input = milliseconds(), throttle_until = 0, next_battery = 0;
    uint64_t sleep_started = 0, message_until = settings_error == ESP_OK ? 0 : milliseconds() + 8000;
    unsigned sleep_phase = 0, message = settings_error == ESP_OK ? 0 : 1;
    int battery = -1;
    reset_input_gate_t input_gate = {0};
    reset_hold_t hold; reset_hold_init(&hold);
    reset_direction_tap_t direction[2] = {0};
    reset_ui_page_t hold_page = RESET_UI_HOME;
    for (;;) {
        input_t input;
        bool input_was_ready = atomic_load(&input_ready);
        bool got_input = input_queue && xQueueReceive(input_queue, &input, pdMS_TO_TICKS(hold.visible ? 40 : 100)) == pdTRUE;
        if (!input_queue) vTaskDelay(pdMS_TO_TICKS(100));
        /* A queued callback racing a gate transition belongs to the consumed
         * wake/cancel gesture, even if this iteration finishes release gating. */
        if (!input_was_ready) {
            got_input = false;
            direction[0].pressed = direction[1].pressed = false;
        }
        uint64_t now_ms = milliseconds();
        if (!atomic_load(&input_ready) && buttons_ok && !sleep_phase) {
            int mv = bsp_button_read_mv();
            bool released = mv >= BSP_BTN_RELEASE_MIN_MV && mv <= BSP_BTN_RELEASE_MAX_MV;
            if (reset_input_gate_observe(&input_gate, released, now_ms)) {
                xQueueReset(input_queue);
                atomic_store(&input_ready, true);
            }
        }
        if (atomic_load(&input_ready)) input_gate.release_seen = false;
        reset_wifi_status_t wifi = {0};
        reset_wifi_get_status(&wifi);
        if (wifi.connected && !sntp_started && !sleep_phase) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            config.sync_cb = clock_synced;
            if (esp_netif_sntp_init(&config) == ESP_OK) sntp_started = true;
        }
        reset_action_t action = RESET_ACTION_NONE;
        if (atomic_exchange(&input_overflow, false)) {
            reset_hold_cancel(&hold);
            if (sleep_phase) cancel_sleep(&controls, &sleep_phase);
            atomic_store(&input_ready, false);
            input_gate.release_seen = false;
            xQueueReset(input_queue);
            got_input = false;
        }
        if (got_input) {
            bool wake_only = applied_brightness == 0;
            last_input = now_ms;
            if (wake_only) {
                reset_hold_cancel(&hold);
                atomic_store(&input_ready, false);
                input_gate.release_seen = false;
                xQueueReset(input_queue);
            } else if (sleep_phase) {
                if (input.event == BSP_BTN_PRESS)
                    action = RESET_ACTION_CANCEL_SLEEP;
            } else if (input.button == BSP_BTN_OK) {
                if (input.event == BSP_BTN_PRESS) {
                    hold_page = controls.page;
                    reset_hold_press(&hold, input.at_ms, wifi.provisioning || !buttons_ok);
                } else if (input.event == BSP_BTN_RELEASE) {
                    reset_hold_event_t result = reset_hold_release(&hold, input.at_ms);
                    if (result == RESET_HOLD_TAP)
                        action = reset_controls_handle(&controls, RESET_INPUT_OK);
                }
            } else {
                if (input.event == BSP_BTN_PRESS) {
                    reset_hold_cancel(&hold);
                    reset_direction_press(&direction[input.button], input.at_ms);
                } else if (input.event == BSP_BTN_RELEASE &&
                           reset_direction_release(&direction[input.button], input.at_ms)) {
                    action = reset_controls_handle(&controls, convert_input(&input));
                }
            }
        }
        if (hold.visible && (controls.page != hold_page || (wifi.provisioning && !hold.blocked)))
            reset_hold_cancel(&hold);
        if (hold.visible) {
            bool released = false;
            if (hold.waiting_release && !hold.pressed) {
                int mv = bsp_button_read_mv();
                released = mv >= BSP_BTN_RELEASE_MIN_MV && mv <= BSP_BTN_RELEASE_MAX_MV;
            }
            if (reset_hold_tick(&hold, now_ms, released) == RESET_HOLD_SLEEP)
                action = RESET_ACTION_SLEEP;
        }
        {
                switch (action) {
                case RESET_ACTION_REFRESH:
                    if (!reset_feed_request_refresh()) throttle_until = now_ms + 5000;
                    break;
                case RESET_ACTION_CHANGE_SETTING:
                    if (controls.selected == 2) { draft_offset = settings.utc_offset_minutes; controls.page = RESET_UI_TIMEZONE; }
                    else if (controls.selected == 4) controls.page = RESET_UI_SETUP;
                    else {
                        reset_settings_t next = settings;
                        if (controls.selected == 0) next.wifi_enabled = !next.wifi_enabled;
                        else if (controls.selected == 1) next.interval_minutes = next.interval_minutes == 5 ? 15 : next.interval_minutes == 15 ? 30 : next.interval_minutes == 30 ? 60 : 5;
                        else next.brightness = next.brightness == 20 ? 45 : next.brightness == 45 ? 75 : 20;
                        if (reset_settings_save(&next) == ESP_OK) {
                            settings = next;
                            (void)reset_feed_set_auto_interval(settings.interval_minutes);
                            if (controls.selected == 0) {
                                reset_feed_set_ready(false, atomic_load(&clock_ready));
                                if (reset_wifi_set_enabled(settings.wifi_enabled) != ESP_OK) { message = 1; message_until = now_ms + 6000; }
                            }
                        } else { message = 1; message_until = now_ms + 6000; }
                    }
                    break;
                case RESET_ACTION_TIMEZONE_UP: draft_offset = draft_offset >= 840 ? -720 : draft_offset + 15; break;
                case RESET_ACTION_TIMEZONE_DOWN: draft_offset = draft_offset <= -720 ? 840 : draft_offset - 15; break;
                case RESET_ACTION_TIMEZONE_SAVE: {
                    reset_settings_t next = settings; next.utc_offset_minutes = draft_offset;
                    if (reset_settings_save(&next) == ESP_OK) settings = next;
                    else { message = 1; message_until = now_ms + 6000; }
                    break;
                }
                case RESET_ACTION_START_SETUP:
                    if (settings.wifi_enabled) (void)reset_wifi_start_provisioning();
                    break;
                case RESET_ACTION_STOP_SETUP: (void)reset_wifi_stop_provisioning(); break;
                case RESET_ACTION_CLEAR: (void)reset_wifi_clear_credentials(); break;
                case RESET_ACTION_SLEEP:
                    if (wifi.provisioning || !buttons_ok) {
                        controls.page = RESET_UI_SETTINGS; message = 2; message_until = now_ms + 6000;
                    } else { controls.page = RESET_UI_SLEEP_WAIT; sleep_phase = 1; sleep_started = now_ms; }
                    break;
                case RESET_ACTION_CANCEL_SLEEP: cancel_sleep(&controls, &sleep_phase); break;
                default: break;
                }
        }
        if (sleep_phase == 1) {
            if (reset_feed_pause_and_wait(0)) {
                if (reset_wifi_suspend() == ESP_OK) { sleep_phase = 2; sleep_started = now_ms; }
                else { cancel_sleep(&controls, &sleep_phase); message = 2; message_until = now_ms + 6000; }
            } else if (now_ms - sleep_started > 35000) {
                cancel_sleep(&controls, &sleep_phase); message = 2; message_until = now_ms + 6000;
            }
        } else if (sleep_phase == 2) {
            if (wifi.suspended && wifi.radio_stopped) sleep_phase = 3;
            else if (now_ms - sleep_started > 8000) {
                cancel_sleep(&controls, &sleep_phase); message = 2; message_until = now_ms + 6000;
            }
        }
        /* No timer wake and no background updates during actual deep sleep. */
        if (sleep_phase == 3) {
            atomic_store(&input_ready, false);
            if (reset_power_prepare_deep_sleep(2000) == ESP_OK && reset_feed_export_rtc(&rtc_feed)) {
                rtc_magic = RTC_MAGIC;
                (void)reset_power_enter_deep_sleep(); /* Only pre-terminal errors return. */
            }
            cancel_sleep(&controls, &sleep_phase); message = 2; message_until = now_ms + 6000;
        }
        uint64_t idle = now_ms - last_input;
        int backlight = idle >= 120000 ? 0 : idle >= 30000 ? 8 : settings.brightness;
        if (wifi.provisioning || sleep_phase || hold.visible || controls.page >= RESET_UI_SETTINGS) backlight = settings.brightness;
        if (backlight != applied_brightness) { bsp_display_backlight(backlight); applied_brightness = backlight; }
        reset_feed_set_ready(settings.wifi_enabled && wifi.connected && !wifi.provisioning && !wifi.suspended, atomic_load(&clock_ready));
        reset_feed_snapshot_t feed = {0};
        reset_feed_get_snapshot(&feed);
        if (now_ms >= next_battery && !sleep_phase && !hold.visible) { battery = bsp_battery_soc(); next_battery = now_ms + 60000; }
        reset_presenter_state_t state = {
            .page = controls.page, .connected = wifi.connected, .has_credentials = wifi.has_credentials,
            .provisioning = wifi.provisioning, .wifi_error = wifi.state == RESET_WIFI_ERROR,
            .clock_ready = atomic_load(&clock_ready), .refresh_throttled = now_ms < throttle_until,
            .battery = battery, .provisioning_seconds = wifi.provisioning_seconds_left,
            .now = time(NULL), .settings = settings, .settings_editing = controls.editing,
            .selected_setting = controls.selected, .draft_utc_offset = draft_offset,
            .sleep_phase = sleep_phase, .message = now_ms < message_until ? message : 0,
            .restored_cache = restored, .radio_stopped = wifi.radio_stopped,
            .radio_control_pending = wifi.control_pending || (!wifi.initialized && wifi.state != RESET_WIFI_ERROR),
        };
        reset_ui_model_t model;
        reset_presenter_build(&feed, &state, &model);
        model.hold_visible = hold.visible;
        model.hold_progress = hold.progress1000;
        model.hold_armed = hold.armed;
        model.hold_blocked = hold.blocked;
        model.hold_released = hold.waiting_release && !hold.pressed;
        if (bsp_lvgl_lock(100)) { reset_ui_update(&model); bsp_lvgl_unlock(); }
        static uint64_t next_log;
        if (now_ms >= next_log) {
            next_log = now_ms + 60000;
            ESP_LOGI(TAG, "Heap free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }
}
