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
#include "reset_idle.h"
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
/* Preserve darkness only over our own controlled automatic-shutdown restart. */
static RTC_NOINIT_ATTR uint32_t rtc_dark_restart;
#define RTC_DARK_MAGIC UINT32_C(0x4441524b)
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
static void cancel_sleep(reset_controls_t *controls, unsigned *phase, reset_idle_t *idle, bool automatic) {
    reset_idle_defer_after_cancel_or_failure(idle, milliseconds());
    rtc_magic = 0;
    if (reset_power_cancel_deep_sleep() != ESP_OK) {
        ESP_LOGE(TAG, "Button/power rollback failed; restarting safely");
        esp_restart();
    }
    if (reset_wifi_resume() != ESP_OK) ESP_LOGE(TAG, "Radio resume request failed");
    reset_feed_resume(false);
    rtc_magic = 0;
    *phase = 0;
    if (!automatic) {
        controls->page = RESET_UI_SETTINGS;
        controls->editing = true;
    }
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
    bool dark_restart = esp_reset_reason() == ESP_RST_SW && rtc_dark_restart == RTC_DARK_MAGIC;
    rtc_dark_restart = 0;
    bsp_display_backlight(dark_restart ? 0 : settings.brightness);
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
    int applied_brightness = dark_restart ? 0 : settings.brightness;
    uint64_t throttle_until = 0, next_battery = 0;
    uint64_t sleep_started = 0, message_until = settings_error == ESP_OK ? 0 : milliseconds() + 8000;
    unsigned sleep_phase = 0, message = settings_error == ESP_OK ? 0 : 1;
    int battery = -1;
    reset_input_gate_t input_gate = {0};
    reset_input_cutoff_t input_cutoff = {0};
    reset_hold_t hold; reset_hold_init(&hold);
    reset_idle_t idle_policy = {0};
    reset_idle_screen_t idle_screen = {.screen_off = dark_restart};
    bool automatic_sleep = false;
    if (dark_restart) reset_idle_defer_after_cancel_or_failure(&idle_policy, milliseconds());
    unsigned reading_page = 0, reading_page_count = 1;
    static reset_ui_model_t model;
    static reset_reader_snapshot_t reader_snapshot;
    reset_ui_page_t displayed_page = RESET_UI_HOME;
    bool history_selected = false;
    reset_direction_tap_t direction[2] = {0};
    reset_ui_page_t hold_page = RESET_UI_HOME;
    for (;;) {
        input_t input;
        bool input_was_ready = atomic_load(&input_ready);
        bool got_input = input_queue && xQueueReceive(input_queue, &input, pdMS_TO_TICKS(hold.active ? 40 : 100)) == pdTRUE;
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
            if (sleep_phase) cancel_sleep(&controls, &sleep_phase, &idle_policy, automatic_sleep);
            atomic_store(&input_ready, false);
            input_gate.release_seen = false;
            xQueueReset(input_queue);
            got_input = false;
        }
        bool user_activity = false;
        unsigned drained_inputs = 0;
        do {
        action = RESET_ACTION_NONE;
        now_ms = milliseconds();
        if (got_input && !reset_input_cutoff_accepts(&input_cutoff, input.at_ms)) got_input = false;
        if (got_input) {
            user_activity = true;
            bool wake_only = applied_brightness == 0;
            if (wake_only) {
                if (sleep_phase) cancel_sleep(&controls, &sleep_phase, &idle_policy, automatic_sleep);
                automatic_sleep = false;
                rtc_dark_restart = 0;
                idle_screen = (reset_idle_screen_t){0};
                direction[0].pressed = direction[1].pressed = false;
                reset_hold_cancel(&hold);
                atomic_store(&input_ready, false);
                input_gate.release_seen = false;
                xQueueReset(input_queue);
            } else if (sleep_phase) {
                if (input.event == BSP_BTN_PRESS)
                    action = RESET_ACTION_CANCEL_SLEEP;
            } else if (hold.cancelling) {
                /* The unwind/quarantine owns every key: never leak a late
                 * release into a navigation tap or restart a Confirm gesture. */
                direction[0].pressed = direction[1].pressed = false;
                if (input.event == BSP_BTN_PRESS)
                    reset_hold_press(&hold, input.at_ms, wifi.provisioning || !buttons_ok);
                else if (input.event == BSP_BTN_RELEASE)
                    (void)reset_hold_release(&hold, input.at_ms, now_ms);
            } else if (input.button == BSP_BTN_OK) {
                if (input.event == BSP_BTN_PRESS) {
                    hold_page = controls.page;
                    reset_hold_press(&hold, input.at_ms, wifi.provisioning || !buttons_ok);
                } else if (input.event == BSP_BTN_RELEASE) {
                    reset_hold_event_t result = reset_hold_release(&hold, input.at_ms, now_ms);
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
        {
                switch (action) {
                case RESET_ACTION_READING_OPEN:
                    /* Snapshot was captured only after a successful HOME render.
                     * Even a failed LVGL lock or a newer feed cannot change the
                     * record behind the visible time. No empty reader entry. */
                    if (reset_controls_admit_reader(&controls, displayed_page, reader_snapshot.valid))
                        reading_page = 0;
                    break;
                case RESET_ACTION_READING_PREVIOUS:
                    reading_page = (reading_page + reading_page_count - 1) % reading_page_count;
                    break;
                case RESET_ACTION_READING_NEXT:
                    reading_page = (reading_page + 1) % reading_page_count;
                    break;
                case RESET_ACTION_HISTORY_REFRESH:
                    (void)reset_feed_request_history();
                    break;
                case RESET_ACTION_REFRESH:
                    if (!reset_feed_request_refresh()) throttle_until = now_ms + 5000;
                    break;
                case RESET_ACTION_CHANGE_SETTING:
                    if (controls.selected == 2) { draft_offset = settings.utc_offset_minutes; controls.page = RESET_UI_TIMEZONE; }
                    else if (controls.selected == 5) controls.page = RESET_UI_SETUP;
                    else {
                        reset_settings_t next = settings;
                        if (controls.selected == 0) next.wifi_enabled = !next.wifi_enabled;
                        else if (controls.selected == 1) next.interval_minutes = next.interval_minutes == 5 ? 15 : next.interval_minutes == 15 ? 30 : next.interval_minutes == 30 ? 60 : 5;
                        else if (controls.selected == 3) next.brightness = next.brightness == 20 ? 45 : next.brightness == 45 ? 75 : 20;
                        else if (controls.selected == 4) next.sleep_minutes = next.sleep_minutes == 5 ? 10 : next.sleep_minutes == 10 ? 30 : next.sleep_minutes == 30 ? 0 : 5;
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
                case RESET_ACTION_CANCEL_SLEEP: cancel_sleep(&controls, &sleep_phase, &idle_policy, automatic_sleep); break;
                default: break;
                }
        }
        /* Drain already-queued releases before rendering a newly visible
         * hold: a delayed short tap must never flash the sleep popup. */
        ++drained_inputs;
        got_input = input_was_ready && atomic_load(&input_ready) && input_queue &&
                    xQueueReceive(input_queue, &input, 0) == pdTRUE;
        } while (got_input && drained_inputs < 8);
        if (got_input) {
            /* A sustained event storm must not keep the UI task unbounded or
             * leave a partially observed gesture armed. */
            reset_hold_cancel(&hold);
            if (sleep_phase) cancel_sleep(&controls, &sleep_phase, &idle_policy, automatic_sleep);
            atomic_store(&input_ready, false);
            input_gate.release_seen = false;
            direction[0].pressed = direction[1].pressed = false;
            xQueueReset(input_queue);
        }
        if (controls.page == RESET_UI_HISTORY && !history_selected)
            (void)reset_feed_request_history();
        history_selected = controls.page == RESET_UI_HISTORY;
        now_ms = milliseconds();
        if (hold.active && (controls.page != hold_page || (wifi.provisioning && !hold.blocked)))
            reset_hold_cancel(&hold);
        if (hold.active) {
            bool was_cancelling = hold.cancelling;
            bool released = false;
            if (hold.cancelling || (hold.waiting_release && !hold.pressed)) {
                int mv = bsp_button_read_mv();
                released = mv >= BSP_BTN_RELEASE_MIN_MV && mv <= BSP_BTN_RELEASE_MAX_MV;
            }
            /* ADC can block while callbacks enqueue more input. Fence the
             * entire observed cancellation interval, never its stale start. */
            now_ms = milliseconds();
            reset_hold_event_t hold_result = reset_hold_tick(&hold, now_ms, released);
            if (was_cancelling && !hold.active) {
                reset_input_cutoff_mark(&input_cutoff, now_ms);
                direction[0].pressed = direction[1].pressed = false;
            }
            if (hold_result == RESET_HOLD_SLEEP) {
                if (wifi.provisioning || !buttons_ok) {
                    controls.page = RESET_UI_SETTINGS; message = 2; message_until = now_ms + 6000;
                } else {
                    automatic_sleep = false;
                    rtc_dark_restart = 0;
                    controls.page = RESET_UI_SLEEP_WAIT; sleep_phase = 1; sleep_started = now_ms;
                }
            }
        }
        bool idle_keys_released = false;
        if (!sleep_phase && buttons_ok && atomic_load(&input_ready) && !hold.active) {
            int mv = bsp_button_read_mv();
            idle_keys_released = mv >= BSP_BTN_RELEASE_MIN_MV && mv <= BSP_BTN_RELEASE_MAX_MV;
        }
        /* Only button activity resets idle. Pairing/held keys/gesture handling
         * pause it; periodic API checks never keep an unused device awake. */
        now_ms = milliseconds();
        bool queued_input = input_queue && uxQueueMessagesWaiting(input_queue) > 0;
        bool idle_paused = sleep_phase || wifi.provisioning || hold.active || controls.editing ||
                           controls.page == RESET_UI_TIMEZONE ||
                           !atomic_load(&input_ready) || !idle_keys_released || queued_input;
        reset_idle_observe(&idle_policy, now_ms, user_activity, idle_paused);
        if (!sleep_phase && reset_idle_screen_tick(&idle_screen, &idle_policy,
                settings.sleep_minutes, now_ms, user_activity)) {
            /* No sleep page and no hardware shutdown during the 15 s grace. */
            automatic_sleep = true;
            rtc_dark_restart = RTC_DARK_MAGIC;
            sleep_phase = 1;
            sleep_started = now_ms;
        }
        /* Apply screen-off before starting its grace or any potentially slow
         * service work. Automatic preparation must never restore the light. */
        if (idle_screen.screen_off && applied_brightness != 0) {
            bsp_display_backlight(0);
            applied_brightness = 0;
            idle_screen.grace_started_ms = milliseconds();
        }
        if (sleep_phase == 1) {
            if (reset_feed_pause_and_wait(0)) {
                if (reset_wifi_suspend() == ESP_OK) { sleep_phase = 2; sleep_started = now_ms; }
                else { cancel_sleep(&controls, &sleep_phase, &idle_policy, automatic_sleep); message = 2; message_until = now_ms + 6000; }
            } else if (now_ms - sleep_started > 35000) {
                cancel_sleep(&controls, &sleep_phase, &idle_policy, automatic_sleep); message = 2; message_until = now_ms + 6000;
            }
        } else if (sleep_phase == 2) {
            if (wifi.suspended && wifi.radio_stopped) sleep_phase = 3;
            else if (now_ms - sleep_started > 8000) {
                cancel_sleep(&controls, &sleep_phase, &idle_policy, automatic_sleep); message = 2; message_until = now_ms + 6000;
            }
        }
        /* No timer wake and no background updates during actual deep sleep. */
        if (sleep_phase == 3) {
            /* Keep callbacks accepting input through the blocking release
             * handoff. Once it returns, the producer has been drained/stopped;
             * its queue is a final fence for taps racing radio shutdown/ADC. */
            esp_err_t prepare = reset_power_prepare_deep_sleep(2000);
            bool late_input = input_queue && uxQueueMessagesWaiting(input_queue) > 0;
            late_input = atomic_exchange(&input_overflow, false) || late_input;
            late_input = (automatic_sleep && bsp_button_deep_sleep_had_activity()) || late_input;
            atomic_store(&input_ready, false);
            if (late_input) {
                user_activity = true;
                reset_idle_observe(&idle_policy, milliseconds(), true, true);
                idle_screen = (reset_idle_screen_t){0};
                rtc_dark_restart = 0;
            }
            if (prepare == ESP_OK && !late_input && reset_feed_export_rtc(&rtc_feed)) {
                rtc_magic = RTC_MAGIC;
                (void)reset_power_enter_deep_sleep(); /* Only pre-terminal errors return. */
                if (automatic_sleep && bsp_button_deep_sleep_had_activity()) {
                    reset_idle_observe(&idle_policy, milliseconds(), true, true);
                    idle_screen = (reset_idle_screen_t){0};
                    rtc_dark_restart = 0;
                }
            }
            cancel_sleep(&controls, &sleep_phase, &idle_policy, automatic_sleep); message = 2; message_until = now_ms + 6000;
        }
        if (automatic_sleep && !sleep_phase) {
            /* Reversible failure: restore services, keep LCD dark, retry only
             * after the five-minute cooldown and a fresh grace interval. */
            reset_idle_screen_failed(&idle_screen);
            automatic_sleep = false;
            rtc_dark_restart = 0;
        }
        int backlight = reset_idle_brightness(&idle_screen, &idle_policy, settings.brightness);
        if (!idle_screen.screen_off && (wifi.provisioning || sleep_phase || hold.active))
            backlight = settings.brightness;
        if (backlight != applied_brightness) { bsp_display_backlight(backlight); applied_brightness = backlight; }
        reset_feed_set_ready(settings.wifi_enabled && wifi.connected && !wifi.provisioning && !wifi.suspended, atomic_load(&clock_ready));
        static reset_feed_snapshot_t feed; /* Main-owner snapshot, not a large task-stack frame. */
        reset_feed_get_snapshot(&feed);
        static reset_history_snapshot_t history;
        reset_feed_get_history_snapshot(&history);
        if (now_ms >= next_battery && !sleep_phase && !hold.active) { battery = bsp_battery_soc(); next_battery = now_ms + 60000; }
        reset_presenter_state_t state = {
            .page = controls.page, .history = &history, .connected = wifi.connected, .has_credentials = wifi.has_credentials,
            .provisioning = wifi.provisioning, .wifi_error = wifi.state == RESET_WIFI_ERROR,
            .wifi_initialized = wifi.initialized, .wifi_connecting = wifi.state == RESET_WIFI_CONNECTING,
            .wifi_attempts = wifi.attempts, .wifi_disconnect_reason = wifi.disconnect_reason,
            .wifi_last_error = wifi.last_error, .wifi_persistence_error = wifi.persistence_error,
            .clock_ready = atomic_load(&clock_ready), .refresh_throttled = now_ms < throttle_until,
            .battery = battery, .provisioning_seconds = wifi.provisioning_seconds_left,
            .now = time(NULL), .settings = settings, .settings_editing = controls.editing,
            .selected_setting = controls.selected, .draft_utc_offset = draft_offset,
            .reading_page = reading_page, .reader_snapshot = &reader_snapshot,
            .sleep_phase = sleep_phase, .message = now_ms < message_until ? message : 0,
            .restored_cache = restored, .radio_stopped = wifi.radio_stopped,
            .radio_control_pending = wifi.control_pending || (!wifi.initialized && wifi.state != RESET_WIFI_ERROR),
        };
        reset_presenter_build(&feed, &state, &model);
        model.hold_visible = hold.visible;
        model.hold_progress = hold.progress1000;
        model.hold_armed = hold.armed;
        model.hold_blocked = hold.blocked;
        model.hold_released = hold.waiting_release && !hold.pressed;
        if (!idle_screen.screen_off && bsp_lvgl_lock(100)) {
            reset_ui_update(&model);
            if (!model.hold_visible) {
                displayed_page = model.page;
                if (model.page == RESET_UI_HOME)
                    (void)reset_presenter_capture_reader(&model, &reader_snapshot);
            }
            reading_page_count = reset_ui_reading_page_count();
            if (!reading_page_count) reading_page_count = 1;
            if (reading_page >= reading_page_count) reading_page = reading_page_count - 1;
            bsp_lvgl_unlock();
        }
        static uint64_t next_log;
        if (now_ms >= next_log) {
            next_log = now_ms + 60000;
            ESP_LOGI(TAG, "Heap free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }
}
