// components/bsp/src/bsp_button.c
// 移植自 trae_card/components/platform/platform_esp32/src/btn_iot_button.c
#include "bsp_button.h"
#include "bsp_pins.h"
#include "iot_button.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_btn";

static const uint16_t BTN_MV[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

static button_handle_t s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t    s_cb;
static void           *s_user;
static volatile bool   s_ready;
static bool s_sleep_handoff;
static bool s_sleep_prepared;
static bool s_sleep_had_activity;
static bsp_btn_cb_t s_saved_cb;
static void *s_saved_user;
static int s_wake_gpio = -1;
// Persistent barrier objects: a timeout must not free an object which a queued
// esp_timer callback could still use. They are reused for subsequent attempts.
static SemaphoreHandle_t s_poll_drained;
static esp_timer_handle_t s_drain_timer;
static volatile bool s_drain_pending;

// ADC1 是 unit 级独占资源:iot_button 与 bsp_button_read_mv() 必须共用同一个 oneshot
// 句柄。谁第二个调 adc_oneshot_new_unit() 谁就拿到 "adc1 is already in use"。
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

#define BSP_BTN_ATTEN  ADC_ATTEN_DB_12       // 量程约 0~3100mV,覆盖松开态

// Use the public button-driver interface, not button_adc's global registry:
// button 4.2.0 leaves an occupied index behind when its core allocation fails.
// These static drivers and the single ADC/calibration pair are owned by BSP.
typedef struct {
    button_driver_t base;
    unsigned index;
} bsp_adc_button_t;
static bsp_adc_button_t s_drivers[BSP_BTN_COUNT];
static int64_t s_sample_time;
static int s_sample_mv = -1;
static bool s_sample_valid;

static uint8_t button_level(button_driver_t *driver) {
    if (!s_ready) return BUTTON_INACTIVE;
    const bsp_adc_button_t *button = (const bsp_adc_button_t *)driver;
    const int64_t now = esp_timer_get_time();
    // Share one averaged reading across the three keys in a polling cycle.
    if (!s_sample_valid || now - s_sample_time >= 1000) {
        int sum = 0;
        s_sample_time = now;
        s_sample_valid = true;
        s_sample_mv = -1;
        for (int i = 0; i < CONFIG_ADC_BUTTON_SAMPLE_TIMES; ++i) {
            int raw;
            if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) {
                return BUTTON_INACTIVE;
            }
            sum += raw;
        }
        if (adc_cali_raw_to_voltage(s_cali, sum / CONFIG_ADC_BUTTON_SAMPLE_TIMES,
                                   &s_sample_mv) != ESP_OK) {
            s_sample_mv = -1;
        }
    }
    // Half-open windows prevent two keys from matching a shared boundary.
    return s_sample_mv >= BTN_MV[button->index][0] &&
           s_sample_mv < BTN_MV[button->index][1] ? BUTTON_ACTIVE : BUTTON_INACTIVE;
}

static esp_err_t button_driver_delete(button_driver_t *driver) {
    (void)driver; // Static storage; shared ADC is released after all buttons.
    return ESP_OK;
}

// 每个按键把"哪个键"随回调带回来。button 组件的回调签名固定,故用 usr_data 传索引。
static void on_event(void *arg, void *usr_data, bsp_btn_ev_t ev) {
    (void)arg;
    if (!s_ready || !s_cb) return;
    s_cb((bsp_btn_t)(intptr_t)usr_data, ev, s_user);
}
static void cb_press (void *a, void *u) { on_event(a, u, BSP_BTN_PRESS);  }
static void cb_click (void *a, void *u) { on_event(a, u, BSP_BTN_CLICK);  }
static void cb_double(void *a, void *u) { on_event(a, u, BSP_BTN_DOUBLE); }
static void cb_long  (void *a, void *u) { on_event(a, u, BSP_BTN_LONG);   }
static void cb_release(void *a, void *u) { on_event(a, u, BSP_BTN_RELEASE); }

// 初始化中途失败时先停掉所有 button driver，再释放本文件持有的校准与 ADC unit。
// button driver 仍在轮询时不能先删 ADC，否则 timer callback 会访问失效句柄。
static const button_event_t BUTTON_EVENTS[] = {
    BUTTON_PRESS_DOWN, BUTTON_SINGLE_CLICK, BUTTON_DOUBLE_CLICK,
    BUTTON_LONG_PRESS_START, BUTTON_PRESS_UP,
};

static esp_err_t unregister_callbacks(button_handle_t button) {
    for (unsigned i = 0; i < sizeof(BUTTON_EVENTS) / sizeof(BUTTON_EVENTS[0]); ++i) {
        esp_err_t e = iot_button_unregister_cb(button, BUTTON_EVENTS[i], NULL);
        // Partial init / repeated cleanup may already have removed an event.
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    }
    return ESP_OK;
}

static esp_err_t button_cleanup(void) {
    esp_err_t first_error = ESP_OK;
    s_cb = NULL;
    s_user = NULL;
    s_ready = false;
    s_sample_valid = false;

    for (int i = BSP_BTN_COUNT - 1; i >= 0; i--) {
        if (!s_btn[i]) continue;
        // iot_button_delete frees callbacks before driver->del. Remove them
        // explicitly so a driver deletion failure leaves no dangling callback
        // allocation to double-free on recovery/retry.
        esp_err_t e = unregister_callbacks(s_btn[i]);
        if (e == ESP_OK) e = iot_button_delete(s_btn[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "按键 %d 回滚失败: %s", i, esp_err_to_name(e));
            if (first_error == ESP_OK) first_error = e;
            continue;
        }
        s_btn[i] = NULL;
    }

    // Never free the ADC beneath a driver whose deletion failed.
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (s_btn[i]) return first_error == ESP_OK ? ESP_FAIL : first_error;
    }

    if (s_cali) {
        esp_err_t e = adc_cali_delete_scheme_curve_fitting(s_cali);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ADC 校准回滚失败: %s", esp_err_to_name(e));
            return e; // Preserve the matching ADC for a safe recovery.
        }
        s_cali = NULL;
    }
    if (s_adc) {
        esp_err_t e = adc_oneshot_del_unit(s_adc);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit 回滚失败: %s", esp_err_to_name(e));
            return e;
        }
        s_adc = NULL;
    }
    return first_error;
}

static esp_err_t register_callbacks(button_handle_t button, void *index) {
    esp_err_t e = iot_button_register_cb(button, BUTTON_PRESS_DOWN, NULL, cb_press, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, cb_click, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_DOUBLE_CLICK, NULL, cb_double, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_LONG_PRESS_START, NULL, cb_long, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_PRESS_UP, NULL, cb_release, index);
    return e;
}

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user) {
    if (s_sleep_handoff) return ESP_ERR_INVALID_STATE;
    if (s_ready) {
        s_cb = cb;
        s_user = user;
        return ESP_OK;
    }
    if (s_adc || s_cali) {
        ESP_LOGE(TAG, "上次按键初始化回滚不完整，拒绝覆盖仍存活的 ADC 句柄");
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        if (s_btn[i]) {
            ESP_LOGE(TAG, "上次按键 %d 回滚不完整，拒绝重复分配资源", i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_cb = cb; s_user = user;

    // BSP owns one ADC unit and one calibration handle for polling and reads.
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BSP_BTN_ADC_UNIT };
    esp_err_t ae = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit 创建失败 (%s)", esp_err_to_name(ae));
        s_adc = NULL;
        button_cleanup();
        return ae;
    }

    const adc_oneshot_chan_cfg_t channel = {
        .atten = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_oneshot_config_channel(s_adc, BSP_BTN_ADC_CHANNEL, &channel);
    if (ae != ESP_OK) { button_cleanup(); return ae; }
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = BSP_BTN_ADC_UNIT,
        .chan = BSP_BTN_ADC_CHANNEL,
        .atten = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC 校准创建失败: %s", esp_err_to_name(ae));
        button_cleanup();
        return ae; // No guessed mV: an unavailable reading is not an UP press.
    }

    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        s_drivers[i] = (bsp_adc_button_t){
            // Suppress automatic timer start until every callback is installed.
            .base = { .enable_power_save = true, .get_key_level = button_level,
                      .del = button_driver_delete },
            .index = (unsigned)i,
        };
        // 判定门限由 BSP 显式下发(见 bsp_pins.h):组件 Kconfig 默认的长按 1500ms 偏迟钝。
        const button_config_t bc = {
            .short_press_time = BSP_BTN_SHORT_PRESS_MS,
            .long_press_time  = BSP_BTN_LONG_PRESS_MS,
        };
        esp_err_t e = iot_button_create(&bc, &s_drivers[i].base, &s_btn[i]);
        if (e != ESP_OK || !s_btn[i]) {
            ESP_LOGE(TAG, "按键 %d 创建失败 (%s) —— 检查 GPIO%d 的 ADC 配置与分压电阻",
                     i, esp_err_to_name(e), BSP_BTN_ADC_CHANNEL);
            e = e == ESP_OK ? ESP_FAIL : e;
            button_cleanup();
            return e;
        }
        void *idx = (void *)(intptr_t)i;
        e = register_callbacks(s_btn[i], idx);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "按键 %d 回调注册失败: %s", i, esp_err_to_name(e));
            button_cleanup();
            return e;
        }
    }

    for (int i = 0; i < BSP_BTN_COUNT; ++i) s_drivers[i].base.enable_power_save = false;
    ae = iot_button_resume();
    if (ae != ESP_OK) { button_cleanup(); return ae; }
    s_sample_valid = false;
    s_ready = true;
    ESP_LOGI(TAG, "按键就绪:ADC1_CH%d 三键分压,短按 %dms 长按 %dms",
             BSP_BTN_ADC_CHANNEL, BSP_BTN_SHORT_PRESS_MS, BSP_BTN_LONG_PRESS_MS);
    return ESP_OK;
}

int bsp_button_read_mv(void) {
    // 读的是 bsp_button_init() 建好、并与 iot_button 共用的那一路 ADC。
    // 单次采样与组件的按键轮询互不干扰(oneshot 内部自带锁)。
    if (!s_adc || !s_cali) return -1;

    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}


#define BUTTON_RELEASE_STABLE_US 100000LL
#define BUTTON_RELEASE_POLL_MS 10
#define BUTTON_RELEASE_MAX_MS 2000

static TickType_t delay_ticks(uint32_t milliseconds) {
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    return ticks ? ticks : 1;
}

static void poll_drained(void *arg) {
    (void)arg;
    xSemaphoreGive(s_poll_drained);
    s_drain_pending = false;
}

static esp_err_t drain_polling(int64_t deadline_us) {
    // Never let a late barrier from a timed-out attempt acknowledge a new one.
    if (s_drain_pending) return ESP_ERR_INVALID_STATE;
    if (!s_poll_drained) {
        s_poll_drained = xSemaphoreCreateBinary();
        if (!s_poll_drained) return ESP_ERR_NO_MEM;
    }
    if (!s_drain_timer) {
        const esp_timer_create_args_t args = {
            .callback = poll_drained,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "button_drain",
        };
        esp_err_t e = esp_timer_create(&args, &s_drain_timer);
        if (e != ESP_OK) return e;
    }
    // A completed barrier may have left a signal after an earlier timeout.
    while (xSemaphoreTake(s_poll_drained, 0) == pdTRUE) {}
    esp_err_t e = iot_button_stop();
    if (e != ESP_OK) return e;
    s_drain_pending = true;
    e = esp_timer_start_once(s_drain_timer, 1);
    if (e != ESP_OK) {
        s_drain_pending = false;
        (void)iot_button_resume();
        return e;
    }
    int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
    if (remaining_ms > 200) remaining_ms = 200;
    if (remaining_ms <= 0 ||
        xSemaphoreTake(s_poll_drained, delay_ticks((uint32_t)remaining_ms)) != pdTRUE) {
        (void)iot_button_resume();
        return ESP_ERR_TIMEOUT;
    }
    // The one-shot runs in the same ESP_TIMER_TASK as iot_button. Its completion
    // proves any already-running poll finished before we touch handles or ADC.
    return ESP_OK;
}

static esp_err_t wait_released(bool digital, int64_t deadline_us) {
    int64_t high_since = -1;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline_us) return ESP_ERR_TIMEOUT;
        bool released;
        if (digital) {
            released = gpio_get_level(s_wake_gpio) == 1;
        } else {
            int mv = bsp_button_read_mv();
            if (mv < 0) return ESP_FAIL; // Read failure is NEVER release.
            released = mv >= BSP_BTN_RELEASE_MIN_MV &&
                       mv <= BSP_BTN_RELEASE_MAX_MV;
        }
        if (!released) {
            s_sleep_had_activity = true;
            high_since = -1;
        }
        else if (high_since < 0) high_since = now;
        else if (now - high_since >= BUTTON_RELEASE_STABLE_US) return ESP_OK;
        vTaskDelay(delay_ticks(BUTTON_RELEASE_POLL_MS));
    }
}

esp_err_t bsp_button_resume_after_deep_sleep_cancel(void) {
    if (!s_sleep_handoff) return ESP_OK;
    s_sleep_prepared = false;
    s_ready = false;
    // A failed deletion intentionally kept ADC/calibration alive. Rebuild only
    // missing resources, without allocating a second ADC unit or losing handles.
    esp_err_t e;
    if (!s_adc) {
        const adc_oneshot_unit_init_cfg_t cfg = { .unit_id = BSP_BTN_ADC_UNIT };
        e = adc_oneshot_new_unit(&cfg, &s_adc);
        if (e != ESP_OK) return e;
    }
    const adc_oneshot_chan_cfg_t channel = {
        .atten = BSP_BTN_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    e = adc_oneshot_config_channel(s_adc, BSP_BTN_ADC_CHANNEL, &channel);
    if (e != ESP_OK) return e;
    if (!s_cali) {
        const adc_cali_curve_fitting_config_t cfg = {
            .unit_id = BSP_BTN_ADC_UNIT, .chan = BSP_BTN_ADC_CHANNEL,
            .atten = BSP_BTN_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        e = adc_cali_create_scheme_curve_fitting(&cfg, &s_cali);
        if (e != ESP_OK) return e;
    }
    // Polling stays stopped until all old/new callbacks are installed. New
    // drivers temporarily opt out of create's automatic shared-timer start.
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (!s_btn[i]) continue;
        e = unregister_callbacks(s_btn[i]);
        if (e == ESP_OK) e = register_callbacks(s_btn[i], (void *)(intptr_t)i);
        if (e != ESP_OK) return e;
    }
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (s_btn[i]) continue;
        s_drivers[i] = (bsp_adc_button_t){
            // Suppress automatic timer start until every callback is installed.
            .base = { .enable_power_save = true, .get_key_level = button_level,
                      .del = button_driver_delete },
            .index = (unsigned)i,
        };
        const button_config_t cfg = {
            .short_press_time = BSP_BTN_SHORT_PRESS_MS,
            .long_press_time = BSP_BTN_LONG_PRESS_MS,
        };
        e = iot_button_create(&cfg, &s_drivers[i].base, &s_btn[i]);
        if (e != ESP_OK || !s_btn[i]) return e == ESP_OK ? ESP_FAIL : e;
        e = register_callbacks(s_btn[i], (void *)(intptr_t)i);
        if (e != ESP_OK) return e;
    }
    for (int i = 0; i < BSP_BTN_COUNT; ++i) s_drivers[i].base.enable_power_save = false;
    e = iot_button_resume();
    if (e != ESP_OK) return e;
    s_cb = s_saved_cb;
    s_user = s_saved_user;
    s_sample_valid = false;
    s_sleep_handoff = false;
    s_wake_gpio = -1;
    s_ready = true;
    return ESP_OK;
}

esp_err_t bsp_button_prepare_deep_sleep(uint32_t timeout_ms, int *wake_gpio) {
    s_sleep_had_activity = false;
    if (!wake_gpio || timeout_ms < 250) return ESP_ERR_INVALID_ARG;
    *wake_gpio = -1;
    if (!s_ready || s_sleep_handoff) return ESP_ERR_INVALID_STATE;
    if (timeout_ms > BUTTON_RELEASE_MAX_MS) timeout_ms = BUTTON_RELEASE_MAX_MS;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int gpio = -1;
    esp_err_t e = adc_oneshot_channel_to_io(BSP_BTN_ADC_UNIT,
                                         BSP_BTN_ADC_CHANNEL, &gpio);
    if (e != ESP_OK || gpio < 0 || gpio >= 64) return e == ESP_OK ? ESP_FAIL : e;
    e = wait_released(false, deadline);
    if (e != ESP_OK) return e;
    e = drain_polling(deadline);
    if (e != ESP_OK) return e;

    s_saved_cb = s_cb;
    s_saved_user = s_user;
    s_sleep_handoff = true;
    s_wake_gpio = gpio;
    e = button_cleanup();
    if (e == ESP_OK) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << (unsigned)gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        e = gpio_config(&cfg);
    }
    if (e == ESP_OK) e = wait_released(true, deadline);
    if (e != ESP_OK) {
        esp_err_t restore = bsp_button_resume_after_deep_sleep_cancel();
        if (restore != ESP_OK) {
            ESP_LOGE(TAG, "Button wake handoff rollback failed: %s", esp_err_to_name(restore));
            return restore;
        }
        return e;
    }
    s_sleep_prepared = true;
    *wake_gpio = gpio;
    return ESP_OK;
}

int bsp_button_deep_sleep_level(void) {
    int level = s_sleep_prepared ? gpio_get_level(s_wake_gpio) : -1;
    if (level == 0) s_sleep_had_activity = true;
    return level;
}

bool bsp_button_deep_sleep_had_activity(void) {
    return s_sleep_had_activity;
}
