// Exercise the real application coordinator. No hardware behavior is inferred.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../main/reset_power.c"

static char trace[256];
static size_t used;
static const char *fail_at;
static int gpio_level, gpio_reads, gpio_low_at, wake_mask, prepared_button;
static int wake_disabled, terminal, lock_depth, sleep_returns, invalid_gpio;
static jmp_buf jump;

static esp_err_t step(const char *name) {
    assert(used + strlen(name) + 2 < sizeof(trace));
    if (used) trace[used++] = ',';
    strcpy(trace + used, name); used += strlen(name);
    return fail_at && strcmp(fail_at, name) == 0 ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_sleep_disable_wakeup_source(int source) {
    assert(source == ESP_SLEEP_WAKEUP_ALL);
    esp_err_t e = step("disable_all");
    if (e == ESP_OK) { wake_mask = 0; ++wake_disabled; }
    return e;
}
bool esp_sleep_is_valid_wakeup_gpio(int gpio) { assert(gpio == 0); return !invalid_gpio; }
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int mode) {
    assert(wake_disabled && mask == 1 && mode == ESP_GPIO_WAKEUP_GPIO_LOW && prepared_button);
    esp_err_t e = step("gpio_low");
    if (e == ESP_OK) wake_mask = (int)mask;
    return e;
}
esp_err_t bsp_button_prepare_deep_sleep(uint32_t timeout, int *gpio) {
    assert(timeout == 2000 && !terminal);
    esp_err_t e = step("button_prepare");
    if (e == ESP_OK) { *gpio = 0; prepared_button = 1; }
    return e;
}
esp_err_t bsp_button_resume_after_deep_sleep_cancel(void) {
    assert(!terminal);
    esp_err_t e = step("button_resume");
    if (e == ESP_OK) prepared_button = 0;
    return e;
}
int bsp_button_deep_sleep_level(void) {
    assert(prepared_button);
    ++gpio_reads;
    return gpio_reads == gpio_low_at ? 0 : gpio_level;
}
esp_err_t bsp_audio_init(void) { assert(!terminal); return step("audio_init"); }
bool bsp_lvgl_lock(int timeout) {
    assert(timeout == (terminal ? 1000 : 250));
    if (step(terminal ? "lock_terminal" : "lock_preflight") != ESP_OK) return false;
    ++lock_depth; return true;
}
void bsp_lvgl_unlock(void) { assert(!terminal && lock_depth == 1); --lock_depth; (void)step("unlock"); }
esp_err_t bsp_battery_sleep(void) { assert(!terminal); terminal = 1; return step("battery"); }
esp_err_t bsp_audio_sleep(void) { return step(terminal ? "audio_sleep" : "audio_cancel"); }
esp_err_t bsp_audio_prepare_deep_sleep(void) { assert(terminal); return step("i2s"); }
esp_err_t bsp_i2c_prepare_deep_sleep(void) { assert(terminal); return step("i2c"); }
esp_err_t bsp_display_prepare_deep_sleep(void) { assert(terminal && lock_depth == 1); return step("lcd"); }
void esp_deep_sleep_start(void) {
    assert(terminal && lock_depth == 1 && wake_mask == 1);
    (void)step("sleep");
    if (!sleep_returns) longjmp(jump, 1);
}
void esp_restart(void) { assert(terminal); (void)step("restart"); longjmp(jump, 2); }
void vTaskDelay(TickType_t ticks) { (void)ticks; assert(!"terminal restart must not return"); }

static void reset(void) {
    memset(trace, 0, sizeof(trace)); used = 0; fail_at = NULL;
    s_prepared = false; s_audio_control_ready = false; prepared_button = 0; gpio_level = 1; gpio_reads = 0;
    gpio_low_at = -1; wake_mask = wake_disabled = terminal = lock_depth = 0;
    sleep_returns = invalid_gpio = 0;
}
static void prepare(void) {
    assert(reset_power_prepare_deep_sleep(2000) == ESP_OK);
    assert(s_prepared && prepared_button && wake_mask == 1);
}
int main(void) {
    reset();
    assert(reset_power_enter_deep_sleep() == ESP_ERR_INVALID_STATE && !trace[0]);
    prepare();
    if (!setjmp(jump)) {
        (void)reset_power_enter_deep_sleep();
        assert(!"successful deep sleep must not return");
    }
    assert(strcmp(trace, "disable_all,button_prepare,gpio_low,audio_init,lock_preflight,unlock,battery,audio_sleep,i2s,i2c,lock_terminal,lcd,sleep") == 0);

    const char *early[] = {"disable_all", "button_prepare", "gpio_low"};
    for (unsigned i = 0; i < sizeof(early) / sizeof(early[0]); ++i) {
        reset(); fail_at = early[i];
        assert(reset_power_prepare_deep_sleep(2000) == ESP_FAIL);
        assert(!terminal && !s_prepared && !prepared_button && !wake_mask);
    }
    reset(); invalid_gpio = 1;
    assert(reset_power_prepare_deep_sleep(2000) == ESP_ERR_INVALID_ARG);
    assert(!s_prepared && !prepared_button && !wake_mask);
    reset(); prepare();
    assert(reset_power_prepare_deep_sleep(2000) == ESP_ERR_INVALID_STATE);
    assert(reset_power_cancel_deep_sleep() == ESP_OK);
    assert(!s_prepared && !prepared_button && !wake_mask);
    assert(reset_power_cancel_deep_sleep() == ESP_OK);

    // Failures before battery suspend are reversible and never detach buses.
    const char *reversible[] = {"audio_init", "lock_preflight"};
    for (unsigned i = 0; i < sizeof(reversible) / sizeof(reversible[0]); ++i) {
        reset(); prepare(); fail_at = reversible[i];
        assert(reset_power_enter_deep_sleep() != ESP_OK && !terminal);
        fail_at = NULL;
        assert(reset_power_cancel_deep_sleep() == ESP_OK);
        assert(!prepared_button && !wake_mask);
    }
    for (int n = 1; n <= 2; ++n) {
        reset(); prepare(); gpio_low_at = n;
        assert(reset_power_enter_deep_sleep() == ESP_ERR_INVALID_STATE && !terminal);
        assert(reset_power_cancel_deep_sleep() == ESP_OK);
    }

    // A write may succeed before failed readback: battery is already terminal.
    const char *late[] = {"battery", "audio_sleep", "i2s", "i2c", "lock_terminal", "lcd"};
    for (unsigned i = 0; i < sizeof(late) / sizeof(late[0]); ++i) {
        reset(); prepare(); fail_at = late[i];
        int outcome = setjmp(jump);
        if (!outcome) {
            (void)reset_power_enter_deep_sleep();
            assert(!"terminal failure returned to UI");
        }
        assert(outcome == 2 && terminal && strstr(trace, ",restart"));
        assert(!strstr(trace, ",sleep"));
    }
    reset(); prepare(); gpio_low_at = 3;
    int outcome = setjmp(jump);
    if (!outcome) { (void)reset_power_enter_deep_sleep(); assert(false); }
    assert(outcome == 2 && strstr(trace, "lcd,restart"));
    reset(); prepare(); sleep_returns = 1;
    outcome = setjmp(jump);
    if (!outcome) { (void)reset_power_enter_deep_sleep(); assert(false); }
    assert(outcome == 2 && strstr(trace, "sleep,restart"));
    puts("Reset manual sleep sequencing/failure tests: PASS");
}
