#include "reset_idle.h"
#include "reset_settings.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MINUTE_MS UINT64_C(60000)

static void test_settings(void) {
    reset_settings_t settings;
    reset_settings_defaults(&settings);
    assert(reset_settings_valid(&settings));
    assert(settings.sleep_minutes == 5);
    const uint16_t valid[] = {0, 5, 10, 30};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        settings.sleep_minutes = valid[i];
        assert(reset_settings_valid(&settings));
    }
    const uint16_t invalid[] = {1, 4, 6, 15, 60, UINT16_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        settings.sleep_minutes = invalid[i];
        assert(!reset_settings_valid(&settings));
    }
    assert(!reset_settings_valid(NULL));
    reset_settings_defaults(NULL);
}

static void test_thresholds_and_polls(void) {
    const uint16_t choices[] = {5, 10, 30};
    for (unsigned i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) {
        reset_idle_t state = {0};
        uint64_t threshold = choices[i] * MINUTE_MS;
        assert(!reset_idle_due(&state, choices[i], UINT64_MAX));
        reset_idle_observe(&state, 17, false, false);
        /* Repeated polling never sets user_activity and never restarts idle. */
        for (uint64_t elapsed = 0; elapsed < threshold; elapsed += 1000) {
            reset_idle_observe(&state, 17 + elapsed, false, false);
            assert(!reset_idle_due(&state, choices[i], 17 + elapsed));
        }
        assert(!reset_idle_due(&state, choices[i], 17 + threshold - 1));
        assert(reset_idle_due(&state, choices[i], 17 + threshold));
        reset_idle_observe(&state, 17 + threshold, false, false);
        assert(reset_idle_due(&state, choices[i], 17 + threshold));
        assert(!reset_idle_due(&state, 0, UINT64_MAX));
        assert(!reset_idle_due(&state, 15, UINT64_MAX));
        assert(!reset_idle_due(&state, UINT16_MAX, UINT64_MAX));
    }
}

static void test_pauses_and_user_activity(void) {
    reset_idle_t state = {0};
    reset_idle_observe(&state, 0, false, false);
    reset_idle_observe(&state, 4 * MINUTE_MS, false, true);
    assert(state.elapsed_ms == 4 * MINUTE_MS);
    /* Pairing, a held key, hold animation, quarantine and sleep admission all
     * use the same pause contract. None permit automatic sleep while blocked. */
    for (unsigned blocker = 0; blocker < 5; ++blocker) {
        uint64_t now = (blocker + 5) * MINUTE_MS;
        reset_idle_observe(&state, now, false, true);
        assert(state.elapsed_ms == 4 * MINUTE_MS);
        assert(!reset_idle_due(&state, 5, UINT64_MAX));
    }
    reset_idle_observe(&state, 10 * MINUTE_MS, false, false);
    assert(!reset_idle_due(&state, 5, 11 * MINUTE_MS - 1));
    assert(reset_idle_due(&state, 5, 11 * MINUTE_MS));
    reset_idle_observe(&state, 11 * MINUTE_MS, true, false);
    assert(state.elapsed_ms == 0);
    assert(!reset_idle_due(&state, 5, 16 * MINUTE_MS - 1));
    assert(reset_idle_due(&state, 5, 16 * MINUTE_MS));

    reset_idle_observe(&state, 16 * MINUTE_MS, false, true);
    reset_idle_observe(&state, 20 * MINUTE_MS, true, true);
    assert(state.elapsed_ms == 0);
    reset_idle_observe(&state, 30 * MINUTE_MS, false, false);
    assert(!reset_idle_due(&state, 5, 35 * MINUTE_MS - 1));
    assert(reset_idle_due(&state, 5, 35 * MINUTE_MS));
}

static void test_failure_and_cancel_cooldown(void) {
    reset_idle_t state = {0};
    reset_idle_observe(&state, 0, false, false);
    reset_idle_observe(&state, 30 * MINUTE_MS, false, false);
    assert(reset_idle_due(&state, 30, 30 * MINUTE_MS));
    reset_idle_defer_after_cancel_or_failure(&state, 30 * MINUTE_MS);
    assert(state.elapsed_ms == 30 * MINUTE_MS);
    assert(!reset_idle_due(&state, 30, 35 * MINUTE_MS - 1));
    assert(reset_idle_due(&state, 30, 35 * MINUTE_MS));
    reset_idle_observe(&state, 31 * MINUTE_MS, false, true);
    assert(state.retry_remaining_ms == 4 * MINUTE_MS);
    reset_idle_observe(&state, 36 * MINUTE_MS, false, true);
    assert(!state.retry_remaining_ms);
    assert(!reset_idle_due(&state, 30, 36 * MINUTE_MS));
    reset_idle_observe(&state, 36 * MINUTE_MS, false, false);
    assert(reset_idle_due(&state, 30, 36 * MINUTE_MS));
    /* Another failed attempt starts a full cooldown, not a tight retry loop. */
    reset_idle_defer_after_cancel_or_failure(&state, 36 * MINUTE_MS);
    assert(!reset_idle_due(&state, 30, 41 * MINUTE_MS - 1));
    assert(reset_idle_due(&state, 30, 41 * MINUTE_MS));
    /* Cancelling with real input independently resets idle. */
    reset_idle_observe(&state, 37 * MINUTE_MS, true, false);
    assert(!reset_idle_due(&state, 30, 42 * MINUTE_MS));
    assert(reset_idle_due(&state, 30, 67 * MINUTE_MS));

    state = (reset_idle_t){0};
    reset_idle_defer_after_cancel_or_failure(&state, 123);
    assert(state.initialized && state.retry_remaining_ms == RESET_IDLE_RETRY_MS);
    assert(!reset_idle_due(&state, 5, 123 + RESET_IDLE_RETRY_MS - 1));
    assert(reset_idle_due(&state, 5, 123 + RESET_IDLE_RETRY_MS));
}

static void test_clock_discontinuities(void) {
    reset_idle_t state = {0};
    reset_idle_observe(&state, 0, false, false);
    reset_idle_observe(&state, 10 * MINUTE_MS, false, false);
    assert(!reset_idle_due(&state, 5, 9 * MINUTE_MS));
    reset_idle_observe(&state, 9 * MINUTE_MS, false, false);
    assert(state.elapsed_ms == 0);
    assert(!reset_idle_due(&state, 5, 14 * MINUTE_MS - 1));
    assert(reset_idle_due(&state, 5, 14 * MINUTE_MS));
    reset_idle_defer_after_cancel_or_failure(&state, 10 * MINUTE_MS);
    reset_idle_observe(&state, 11 * MINUTE_MS, false, false);
    assert(state.retry_remaining_ms == 4 * MINUTE_MS);
    reset_idle_observe(&state, 1, false, false);
    assert(state.elapsed_ms == 0 && state.retry_remaining_ms == RESET_IDLE_RETRY_MS);
    assert(!reset_idle_due(&state, 5, RESET_IDLE_RETRY_MS));
    assert(reset_idle_due(&state, 5, 1 + RESET_IDLE_RETRY_MS));

    state = (reset_idle_t){0};
    reset_idle_observe(&state, UINT64_MAX - MINUTE_MS, false, false);
    assert(!reset_idle_due(&state, 5, UINT64_MAX));
    reset_idle_defer_after_cancel_or_failure(&state, UINT64_MAX - 1);
    assert(!reset_idle_due(&state, 5, UINT64_MAX));
    reset_idle_observe(&state, 0, false, false); /* Clock wrap is a rollback. */
    assert(!reset_idle_due(&state, 5, RESET_IDLE_RETRY_MS - 1));
    assert(reset_idle_due(&state, 5, RESET_IDLE_RETRY_MS));

    state = (reset_idle_t){0};
    reset_idle_observe(&state, 0, false, false);
    reset_idle_observe(&state, UINT64_MAX, false, false);
    assert(state.elapsed_ms == UINT64_MAX && reset_idle_due(&state, 30, UINT64_MAX));
    /* Defensive saturation cannot turn a huge elapsed value into a small one. */
    state.last_now_ms = 0;
    assert(reset_idle_due(&state, 30, 1));
    reset_idle_observe(&state, 1, false, false);
    assert(state.elapsed_ms == UINT64_MAX);
    state = (reset_idle_t){0}; /* Explicit owner reset starts a new idle period. */
    assert(!reset_idle_due(&state, 5, UINT64_MAX));
    reset_idle_observe(&state, UINT64_MAX - 5, true, true);
    assert(!state.elapsed_ms && state.paused && !state.retry_remaining_ms);

    reset_idle_observe(NULL, 0, true, false);
    reset_idle_defer_after_cancel_or_failure(NULL, 0);
    assert(!reset_idle_due(NULL, 5, UINT64_MAX));
}

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "nvs_flash.h"

static reset_settings_t stored;
static bool namespace_present, sleep_present;
static esp_err_t init_error, open_error, sleep_read_error, commit_error;
static const char *set_failure_key;
static unsigned opens, closes, commits, sleep_writes;
static nvs_open_mode_t open_mode;

esp_err_t nvs_flash_init(void) { return init_error; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle) {
    assert(!strcmp(name, "reset_settings"));
    ++opens;
    if (open_error != ESP_OK) return open_error;
    if (!namespace_present && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    namespace_present = true;
    open_mode = mode;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); ++closes; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value) {
    assert(handle == 1 && open_mode == NVS_READONLY);
    if (!strcmp(key, "wifi")) *value = stored.wifi_enabled;
    else { assert(!strcmp(key, "brightness")); *value = stored.brightness; }
    return ESP_OK;
}
esp_err_t nvs_get_u16(nvs_handle_t handle, const char *key, uint16_t *value) {
    assert(handle == 1 && open_mode == NVS_READONLY);
    if (!strcmp(key, "interval")) *value = stored.interval_minutes;
    else {
        assert(!strcmp(key, "sleep_minutes"));
        if (sleep_read_error != ESP_OK) return sleep_read_error;
        if (!sleep_present) return ESP_ERR_NVS_NOT_FOUND;
        *value = stored.sleep_minutes;
    }
    return ESP_OK;
}
esp_err_t nvs_get_i16(nvs_handle_t handle, const char *key, int16_t *value) {
    assert(handle == 1 && open_mode == NVS_READONLY && !strcmp(key, "offset"));
    *value = stored.utc_offset_minutes;
    return ESP_OK;
}
static bool set_fails(nvs_handle_t handle, const char *key) {
    assert(handle == 1 && open_mode == NVS_READWRITE);
    return set_failure_key && !strcmp(set_failure_key, key);
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    if (set_fails(handle, key)) return ESP_FAIL;
    if (!strcmp(key, "wifi")) stored.wifi_enabled = value != 0;
    else { assert(!strcmp(key, "brightness")); stored.brightness = value; }
    return ESP_OK;
}
esp_err_t nvs_set_u16(nvs_handle_t handle, const char *key, uint16_t value) {
    if (set_fails(handle, key)) return ESP_FAIL;
    if (!strcmp(key, "interval")) stored.interval_minutes = value;
    else {
        assert(!strcmp(key, "sleep_minutes"));
        stored.sleep_minutes = value;
        sleep_present = true;
        ++sleep_writes;
    }
    return ESP_OK;
}
esp_err_t nvs_set_i16(nvs_handle_t handle, const char *key, int16_t value) {
    if (set_fails(handle, key)) return ESP_FAIL;
    assert(!strcmp(key, "offset"));
    stored.utc_offset_minutes = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1 && open_mode == NVS_READWRITE);
    ++commits;
    return commit_error;
}

static void test_persistence(void) {
    reset_settings_t settings;
    assert(reset_settings_load(&settings) == ESP_OK);
    assert(reset_settings_valid(&settings) && settings.sleep_minutes == 5);
    assert(settings.wifi_enabled && opens == 1 && closes == 0);

    /* Older NVS stores contain every old setting but no sleep_minutes key. */
    namespace_present = true;
    reset_settings_defaults(&stored);
    stored.wifi_enabled = false;
    stored.interval_minutes = 30;
    stored.utc_offset_minutes = 345;
    stored.brightness = 75;
    stored.sleep_minutes = UINT16_MAX;
    assert(reset_settings_load(&settings) == ESP_OK);
    assert(!settings.wifi_enabled && settings.interval_minutes == 30);
    assert(settings.utc_offset_minutes == 345 && settings.brightness == 75);
    assert(settings.sleep_minutes == 5 && closes == 1);

    const uint16_t choices[] = {0, 5, 10, 30};
    for (unsigned i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) {
        settings.sleep_minutes = choices[i];
        assert(reset_settings_save(&settings) == ESP_OK);
        reset_settings_t loaded;
        assert(reset_settings_load(&loaded) == ESP_OK);
        assert(loaded.sleep_minutes == choices[i] && !loaded.wifi_enabled);
        assert(loaded.interval_minutes == 30 && loaded.utc_offset_minutes == 345);
        assert(loaded.brightness == 75);
    }
    assert(commits == 4 && sleep_writes == 4);
    /* Corruption/read failure of the new key cannot reset remembered Wi-Fi. */
    stored.sleep_minutes = 15;
    assert(reset_settings_load(&settings) == ESP_OK);
    assert(settings.sleep_minutes == 5 && !settings.wifi_enabled);
    sleep_read_error = ESP_FAIL;
    assert(reset_settings_load(&settings) == ESP_OK);
    assert(settings.sleep_minutes == 5 && !settings.wifi_enabled);
    sleep_read_error = ESP_OK;

    unsigned previous_opens = opens;
    settings.sleep_minutes = 15;
    assert(reset_settings_save(&settings) == ESP_ERR_INVALID_ARG);
    assert(reset_settings_save(NULL) == ESP_ERR_INVALID_ARG);
    assert(reset_settings_load(NULL) == ESP_ERR_INVALID_ARG);
    assert(opens == previous_opens);
    settings.sleep_minutes = 10;
    set_failure_key = "sleep_minutes";
    unsigned previous_closes = closes, previous_commits = commits;
    assert(reset_settings_save(&settings) == ESP_FAIL);
    assert(commits == previous_commits && closes == previous_closes + 1);
    set_failure_key = NULL;
    commit_error = ESP_FAIL;
    assert(reset_settings_save(&settings) == ESP_FAIL);
    assert(commits == previous_commits + 1 && closes == previous_closes + 2);
    commit_error = ESP_OK;
    open_error = ESP_FAIL;
    assert(reset_settings_load(&settings) == ESP_FAIL);
    assert(settings.sleep_minutes == 5 && reset_settings_valid(&settings));
    assert(reset_settings_save(&settings) == ESP_FAIL);
    open_error = ESP_OK;
    init_error = ESP_FAIL;
    previous_opens = opens;
    assert(reset_settings_load(&settings) == ESP_FAIL && opens == previous_opens);
    init_error = ESP_OK;
    puts("Reset idle settings NVS migration/persistence/failure tests: PASS");
}
#endif

int main(void) {
    test_settings();
    test_thresholds_and_polls();
    test_pauses_and_user_activity();
    test_failure_and_cancel_cooldown();
    test_clock_discontinuities();
#ifdef ESP_PLATFORM
    test_persistence();
#endif
    puts("Reset automatic idle sleep policy/settings tests: PASS");
    return 0;
}
