// Execute the real BSP with public-interface stubs and a fake ADC (no SDK needed).
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../components/bsp/src/bsp_button.c"

_Static_assert(BSP_BTN_PRESS == 0 && BSP_BTN_CLICK == 1 && BSP_BTN_DOUBLE == 2 &&
               BSP_BTN_LONG == 3 && BSP_BTN_RELEASE == 4, "Append-only BSP events");
enum { CALLBACKS_PER_BUTTON = 5 };
struct button_dev_t {
    button_driver_t *driver;
    bool live;
    unsigned callbacks;
    button_cb_t callback[BUTTON_PRESS_END + 1];
    void *callback_user[BUTTON_PRESS_END + 1];
};
static struct button_dev_t buttons[BSP_BTN_COUNT];
static int adc_token, cal_token, adc_live, cal_live, live_buttons;
static int create_calls, callback_calls, fail_create, fail_callback;
static int fail_adc, fail_channel, fail_cal, fail_read, fail_convert, fail_delete;
static int raw_mv, reads, events;
static bsp_btn_t expected_btn = BSP_BTN_OK;
static bsp_btn_ev_t expected_ev = BSP_BTN_CLICK;
static int fail_delete_at, delete_calls;
static int64_t clock_us;
static int gpio_level = 1, fail_gpio, fail_map, timer_running, poll_is_drained;
static int fail_stop, fail_barrier, fail_unregister, fail_adc_delete, fail_cal_delete;
static int adc_dip_at = -1, gpio_dip_at = -1;
static int sem_value, timer_token;
static esp_timer_create_args_t barrier;
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &sem_value; }
int xSemaphoreGive(SemaphoreHandle_t s) { assert(s == &sem_value); sem_value = 1; return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    assert(s == &sem_value);
    if (!sem_value && ticks) clock_us += ticks * 1000;
    if (!sem_value) return pdFALSE;
    sem_value = 0; return pdTRUE;
}
void vTaskDelay(TickType_t ticks) { clock_us += (int64_t)ticks * 1000; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer) {
    assert(args->dispatch_method == ESP_TIMER_TASK);
    barrier = *args; *timer = &timer_token; return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t us) {
    assert(timer == &timer_token && us == 1 && !timer_running);
    if (!fail_barrier) { poll_is_drained = 1; barrier.callback(barrier.arg); }
    return ESP_OK;
}
esp_err_t iot_button_stop(void) {
    if (fail_stop) return ESP_FAIL;
    assert(timer_running); timer_running = 0; poll_is_drained = 0; return ESP_OK;
}
esp_err_t iot_button_resume(void) { timer_running = 1; return ESP_OK; }
esp_err_t adc_oneshot_channel_to_io(int unit, int channel, int *gpio) {
    assert(unit == BSP_BTN_ADC_UNIT && channel == BSP_BTN_ADC_CHANNEL);
    if (fail_map) return ESP_FAIL;
    *gpio = 0; return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t *cfg) {
    assert(!adc_live && !cal_live && !live_buttons && poll_is_drained);
    assert(cfg->pin_bit_mask == 1 && cfg->mode == GPIO_MODE_INPUT);
    assert(cfg->pull_up_en == GPIO_PULLUP_DISABLE && cfg->pull_down_en == GPIO_PULLDOWN_DISABLE);
    return fail_gpio ? ESP_FAIL : ESP_OK;
}
int gpio_get_level(gpio_num_t gpio) {
    assert(gpio == 0 && !adc_live && !live_buttons);
    return clock_us / 1000 == gpio_dip_at ? 0 : gpio_level;
}

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *h) {
    assert(cfg->unit_id == BSP_BTN_ADC_UNIT && !adc_live);
    if (fail_adc) return ESP_ERR_NO_MEM;
    adc_live = 1; *h = &adc_token; return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h) {
    assert(h == &adc_token && adc_live && !live_buttons);
    if (fail_adc_delete) return ESP_FAIL;
    adc_live = 0; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, int channel, const adc_oneshot_chan_cfg_t *cfg) {
    assert(h == &adc_token && channel == BSP_BTN_ADC_CHANNEL && cfg->atten == ADC_ATTEN_DB_12);
    return fail_channel ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, int channel, int *raw) {
    assert(h == &adc_token && adc_live && channel == BSP_BTN_ADC_CHANNEL);
    ++reads;
    if (fail_read) return ESP_FAIL;
    *raw = clock_us / 1000 == adc_dip_at ? 1800 : raw_mv; return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg, adc_cali_handle_t *h) {
    assert(adc_live && !cal_live && cfg->atten == ADC_ATTEN_DB_12);
    if (fail_cal) return ESP_ERR_NO_MEM;
    cal_live = 1; *h = &cal_token; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h) {
    assert(h == &cal_token && cal_live && !live_buttons);
    if (fail_cal_delete) return ESP_FAIL;
    cal_live = 0; return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv) {
    assert(h == &cal_token && cal_live);
    if (fail_convert) { *mv = 0; return ESP_FAIL; }
    *mv = raw; return ESP_OK;
}
int64_t esp_timer_get_time(void) { return clock_us; }
esp_err_t iot_button_create(const button_config_t *cfg, const button_driver_t *driver, button_handle_t *h) {
    // 判定门限必须由 BSP 显式下发(bsp_pins.h),不能退回组件默认的 180 / 1500ms。
    assert(cfg->short_press_time == BSP_BTN_SHORT_PRESS_MS);
    assert(cfg->long_press_time == BSP_BTN_LONG_PRESS_MS);
    if (++create_calls == fail_create) return ESP_ERR_NO_MEM;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (buttons[i].live) continue;
        buttons[i] = (struct button_dev_t){ .driver = (button_driver_t *)driver, .live = true };
        *h = &buttons[i]; ++live_buttons;
        if (!driver->enable_power_save) timer_running = 1;
        assert(driver->get_key_level((button_driver_t *)driver) == BUTTON_INACTIVE);
        return ESP_OK;
    }
    assert(false); return ESP_FAIL;
}
esp_err_t iot_button_delete(button_handle_t h) {
    assert(h && h->live && adc_live && cal_live && !h->callbacks);
    if (s_sleep_handoff) assert(poll_is_drained);
    if (fail_delete || ++delete_calls == fail_delete_at) return ESP_FAIL;
    assert(h->driver->del(h->driver) == ESP_OK);
    h->live = false; --live_buttons;
    if (!live_buttons) timer_running = 0;
    return ESP_OK;
}
esp_err_t iot_button_register_cb(button_handle_t h, button_event_t ev, button_event_args_t *args, button_cb_t cb, void *u) {
    (void)args;
    assert(h->live);
    assert(ev >= BUTTON_PRESS_DOWN && ev < BUTTON_PRESS_END);
    assert(!h->callback[ev]);
    h->callbacks |= 1U << ev;
    h->callback[ev] = cb;
    h->callback_user[ev] = u;
    cb(h, u); // No user callbacks may escape a partial initialization.
    return ++callback_calls == fail_callback ? ESP_ERR_NO_MEM : ESP_OK;
}
esp_err_t iot_button_unregister_cb(button_handle_t h, button_event_t ev, button_event_args_t *args) {
    (void)args;
    assert(h->live);
    if (fail_unregister) return ESP_FAIL;
    h->callbacks &= ~(1U << ev);
    h->callback[ev] = NULL;
    h->callback_user[ev] = NULL;
    return ESP_OK;
}
static void event_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *u) {
    assert(btn == expected_btn && ev == expected_ev && u == &events);
    ++events;
}
static void reset_faults(void) {
    fail_adc = fail_channel = fail_cal = fail_read = fail_convert = fail_delete = 0;
    fail_create = fail_callback = create_calls = callback_calls = 0;
    fail_gpio = fail_map = fail_stop = fail_barrier = fail_unregister = 0;
    fail_adc_delete = fail_cal_delete = 0;
    adc_dip_at = gpio_dip_at = -1; gpio_level = 1;
    fail_delete_at = delete_calls = 0;
}
static void assert_clean(void) {
    assert(!adc_live && !cal_live && !live_buttons && !s_ready && !s_adc && !s_cali);
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!s_btn[i]);
}
static void retry_success(void) {
    assert_clean(); reset_faults();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT && live_buttons == BSP_BTN_COUNT);
    assert(callback_calls == BSP_BTN_COUNT * CALLBACKS_PER_BUTTON);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT);
    assert(callback_calls == BSP_BTN_COUNT * CALLBACKS_PER_BUTTON);
    button_cleanup(); assert_clean();
}
static void check_voltage(int mv, int expected) {
    raw_mv = mv; clock_us += 2000;
    const int before = reads;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        assert(s_drivers[i].base.get_key_level(&s_drivers[i].base) == (i == expected));
    }
    assert(reads - before == CONFIG_ADC_BUTTON_SAMPLE_TIMES);
}
static void check_registered_events(void) {
    const button_event_t actual[] = {BUTTON_PRESS_DOWN, BUTTON_SINGLE_CLICK,
        BUTTON_DOUBLE_CLICK, BUTTON_LONG_PRESS_START, BUTTON_PRESS_UP};
    const bsp_btn_ev_t forwarded[] = {BSP_BTN_PRESS, BSP_BTN_CLICK,
        BSP_BTN_DOUBLE, BSP_BTN_LONG, BSP_BTN_RELEASE};
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        assert(s_btn[i]->callbacks == (1U << CALLBACKS_PER_BUTTON) - 1);
        /* PRESS_END is delayed by click discrimination and must stay unused. */
        assert(!s_btn[i]->callback[BUTTON_PRESS_END]);
        for (unsigned j = 0; j < CALLBACKS_PER_BUTTON; ++j) {
            expected_btn = (bsp_btn_t)i;
            expected_ev = forwarded[j];
            int before = events;
            s_btn[i]->callback[actual[j]](s_btn[i], s_btn[i]->callback_user[actual[j]]);
            assert(events == before + 1);
        }
    }
    expected_btn = BSP_BTN_OK;
    expected_ev = BSP_BTN_CLICK;
}
int main(void) {
    for (int i = 1; i <= BSP_BTN_COUNT; ++i) {
        reset_faults(); fail_create = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    for (int i = 1; i <= BSP_BTN_COUNT * CALLBACKS_PER_BUTTON; ++i) {
        reset_faults(); fail_callback = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    reset_faults(); fail_adc = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_channel = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_cal = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    assert(events == 0);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    cb_click(NULL, (void *)(intptr_t)BSP_BTN_OK); assert(events == 1);
    check_registered_events();
    check_voltage(0, BSP_BTN_UP); check_voltage(149, BSP_BTN_UP);
    check_voltage(150, BSP_BTN_DOWN); check_voltage(446, BSP_BTN_DOWN);
    check_voltage(447, BSP_BTN_OK); check_voltage(595, BSP_BTN_OK);
    check_voltage(1899, BSP_BTN_OK);
    check_voltage(1900, -1); check_voltage(3300, -1);
    assert(bsp_button_read_mv() == 3300);
    fail_read = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_read = 0; fail_convert = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_delete = 1; button_cleanup();
    assert(adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_ERR_INVALID_STATE);
    fail_delete = 0; button_cleanup(); retry_success();
    // Manual handoff: actual ADC released window and HIGH each need 100 ms.
    reset_faults(); raw_mv = 3300; clock_us = 0;
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    int gpio = 99;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_OK);
    assert(gpio == 0 && clock_us >= 200000 && bsp_button_deep_sleep_level() == 1);
    assert(!adc_live && !cal_live && !live_buttons && !s_ready);
    assert(bsp_button_resume_after_deep_sleep_cancel() == ESP_OK);
    assert(s_ready && adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    assert(s_cb == event_cb && s_user == &events);
    assert(bsp_button_deep_sleep_level() == -1);
    check_registered_events();

    // Every one of the five callback registrations per key may fail during
    // restoration as well as initial setup. Retry must rebuild without a
    // second ADC owner, duplicated callbacks, or partially live user events.
    for (int i = 1; i <= BSP_BTN_COUNT * CALLBACKS_PER_BUTTON; ++i) {
        assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_OK);
        fail_callback = callback_calls + i;
        const int before = events;
        assert(bsp_button_resume_after_deep_sleep_cancel() != ESP_OK);
        assert(!s_ready && adc_live && cal_live && live_buttons > 0);
        assert(events == before);
        fail_callback = 0;
        assert(bsp_button_resume_after_deep_sleep_cancel() == ESP_OK);
        assert(s_ready && adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
        check_registered_events();
    }

    // Held keys, merely-outside-key-window voltages, over-range, read failure
    // and unstable release all refuse sleep before ownership is released.
    const int not_released[] = {0, 300, 595, 1900, 2499, 3601};
    for (unsigned i = 0; i < sizeof(not_released) / sizeof(not_released[0]); ++i) {
        raw_mv = not_released[i]; clock_us = 0;
        assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_ERR_TIMEOUT);
        assert(clock_us <= 2000000 && s_ready && live_buttons == BSP_BTN_COUNT);
    }
    raw_mv = 3300; fail_read = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL && s_ready);
    fail_read = 0; fail_convert = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL && s_ready);
    fail_convert = 0; clock_us = 0; adc_dip_at = 90;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_OK);
    assert(clock_us >= 300000); // Stable-window clock reset by the dip.
    assert(bsp_button_resume_after_deep_sleep_cancel() == ESP_OK);
    adc_dip_at = -1; clock_us = 0; gpio_dip_at = 190;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_OK);
    assert(clock_us >= 300000);
    assert(bsp_button_resume_after_deep_sleep_cancel() == ESP_OK);

    // Digital LOW / configuration errors restore buttons after ADC handoff.
    gpio_dip_at = -1; gpio_level = 0; clock_us = 0;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_ERR_TIMEOUT);
    assert(s_ready && adc_live && live_buttons == BSP_BTN_COUNT);
    gpio_level = 1; fail_gpio = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL && s_ready);
    fail_gpio = 0;
    fail_delete = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL);
    assert(s_ready && adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    fail_delete = 0; fail_delete_at = delete_calls + 2;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL);
    assert(s_ready && adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    fail_delete_at = 0; fail_adc_delete = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL);
    assert(s_ready && adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    fail_adc_delete = 0; fail_cal_delete = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL);
    assert(s_ready && adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    fail_cal_delete = 0; fail_unregister = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL);
    assert(!s_ready && adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    fail_unregister = 0;
    assert(bsp_button_resume_after_deep_sleep_cancel() == ESP_OK && s_ready);
    fail_stop = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_FAIL && s_ready);
    fail_stop = 0; fail_barrier = 1;
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_ERR_TIMEOUT && s_ready);
    assert(adc_live && cal_live && live_buttons == BSP_BTN_COUNT && timer_running);
    // Late barrier cannot accidentally satisfy the next attempt.
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_ERR_INVALID_STATE);
    fail_barrier = 0; barrier.callback(barrier.arg);
    assert(bsp_button_prepare_deep_sleep(2000, &gpio) == ESP_OK);
    assert(bsp_button_resume_after_deep_sleep_cancel() == ESP_OK);
    button_cleanup(); assert_clean();
    puts("BSP button fault-injection and deep-sleep handoff tests: PASS");
}
