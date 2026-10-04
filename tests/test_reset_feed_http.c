#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ESP_PLATFORM 1
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 1
#include "reset_feed_test_stubs.h"

static time_t fake_wall_time = 1791100000;
static int64_t fake_monotonic_us;
static bool fail_body_alloc;
static time_t fake_time(time_t *result)
{
    if (result) *result = fake_wall_time;
    return fake_wall_time;
}
static void *body_malloc(size_t size)
{
    return fail_body_alloc ? NULL : malloc(size);
}

/* Compile the production HTTPS code against minimal fault-injection stubs. */
#define time fake_time
#define malloc body_malloc
#include "../main/reset_feed.c"
#undef malloc
#undef time

static const char body_json[] =
    "{\"data\":{\"latest_reset\":null,\"scheduled_reset\":null,"
    "\"active_watch\":null,\"stats\":{\"total\":0,\"last_reset_at\":null,"
    "\"days_since_last\":null,\"avg_interval_days\":null}},\"meta\":{\"api_version\":\"v1\","
    "\"generated_at\":\"2026-10-04T06:48:33.122Z\"}}";

static esp_http_client_config_t saved_config;
static const char *wire_body;
static size_t wire_length;
static size_t wire_position;
static int64_t announced_length;
static int status_code;
static const char *etag_header;
static const char *retry_header;
static const char *content_type;
static const char *content_encoding;
static bool init_fail;
static bool open_fail;
static bool read_fail;
static bool body_incomplete;
static bool cached_body_at_headers;
static int64_t read_advance_us;
static int init_calls;
static int cleanup_calls;
static int read_calls;
static char sent_etag[RESET_ETAG_CAPACITY];
static bool task_creation_fail;
static unsigned notifications;

static void reset_fixture(void)
{
    wire_body = body_json;
    wire_length = strlen(body_json);
    wire_position = 0;
    announced_length = (int64_t)wire_length;
    status_code = 200;
    etag_header = "\"good-v1\"";
    retry_header = NULL;
    content_type = "application/json; charset=utf-8";
    content_encoding = NULL;
    init_fail = false;
    open_fail = false;
    read_fail = false;
    body_incomplete = false;
    cached_body_at_headers = false;
    fail_body_alloc = false;
    read_advance_us = 0;
    init_calls = 0;
    cleanup_calls = 0;
    read_calls = 0;
    sent_etag[0] = 0;
    fake_wall_time = 1791100000;
    fake_monotonic_us = 0;
    s_wifi_ready = true;
    s_clock_ready = true;
    s_fetching = false;
    s_paused = false;
    s_refresh_pending = false;
    s_cache_needs_verification = false;
    s_restore_pending = false;
    s_restore_hard_backoff_at = 0;
    s_restore_next_auto_at = 0;
    s_starting = false;
    s_worker = NULL;
    s_last_checked_ms = 0;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&s_poll, 0, sizeof(s_poll));
    memset(s_etag, 0, sizeof(s_etag));
    notifications = 0;
    task_creation_fail = false;
}

esp_err_t esp_crt_bundle_attach(void *config)
{
    (void)config;
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    return fake_monotonic_us;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    ++init_calls;
    assert(strcmp(config->url, "https://codex-resets.com/api/v1/status") == 0);
    assert(config->transport_type == HTTP_TRANSPORT_OVER_SSL);
    assert(config->method == HTTP_METHOD_GET);
    assert(config->crt_bundle_attach == esp_crt_bundle_attach);
    assert(!config->skip_cert_common_name_check);
    assert(config->disable_auto_redirect);
    assert(config->max_authorization_retries == -1);
    assert(config->timeout_ms > 0 && config->timeout_ms <= 10000);
    saved_config = *config;
    return init_fail ? NULL : &saved_config;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    assert(client);
    if (strcmp(key, "If-None-Match") == 0) {
        assert(strlen(value) < sizeof(sent_etag));
        strcpy(sent_etag, value);
    } else if (strcmp(key, "Accept-Encoding") == 0) {
        assert(strcmp(value, "identity") == 0);
    } else {
        assert(strcmp(key, "Accept") == 0 && strcmp(value, "application/json") == 0);
    }
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int length)
{
    assert(client && length == 0);
    return open_fail ? ESP_FAIL : ESP_OK;
}

static void emit_header(const char *key, const char *value)
{
    if (!value) return;
    esp_http_client_event_t event = {
        .event_id = HTTP_EVENT_ON_HEADER,
        .user_data = saved_config.user_data,
        .header_key = (char *)key,
        .header_value = (char *)value,
    };
    assert(saved_config.event_handler(&event) == ESP_OK);
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    assert(client);
    emit_header("eTaG", etag_header);
    emit_header("retry-after", retry_header);
    emit_header("Content-Type", content_type);
    emit_header("Content-Encoding", content_encoding);
    return announced_length;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    assert(client);
    return status_code;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{
    assert(client);
    return !body_incomplete && (cached_body_at_headers || wire_position == wire_length);
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout)
{
    assert(client && timeout > 0 && timeout <= RESET_HTTP_TIMEOUT_MS);
    return ESP_OK;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int length)
{
    assert(client && length > 0 && length <= 1024);
    ++read_calls;
    fake_monotonic_us += read_advance_us;
    if (read_fail) return -1;
    size_t amount = wire_length - wire_position;
    if (amount > (size_t)length) amount = (size_t)length;
    memcpy(buffer, wire_body + wire_position, amount);
    wire_position += amount;
    return (int)amount;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    assert(client);
    ++cleanup_calls;
    return ESP_OK;
}

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    assert(fn == feed_worker && name && stack == 8192 && arg == NULL && priority > 0);
    if (task_creation_fail) return 0;
    *task = &saved_config;
    return pdPASS;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    assert(task);
    ++notifications;
}

static bool finish_fetch_on_delay;
void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    fake_monotonic_us += (int64_t)ticks * 1000;
    if (finish_fetch_on_delay) {
        s_fetching = false;
        finish_fetch_on_delay = false;
    }
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    (void)clear;
    (void)timeout;
    assert(!"The infinite worker is not run in a host test");
    return 0;
}

static response_t fetch(void)
{
    response_t result;
    fetch_status(&result);
    assert(cleanup_calls == init_calls - (init_fail ? 1 : 0));
    return result;
}

static void test_fetch_and_cache(void)
{
    reset_fixture();
    cached_body_at_headers = true;
    response_t response = fetch();
    assert(response.error == RESET_FEED_ERROR_NONE && !response.unchanged);
    assert(read_calls >= 1 && response.data.generated_at == INT64_C(1791096513));
    apply_response(&response, 1000, fake_wall_time);
    assert(s_snapshot.has_data && s_snapshot.last_checked_at == fake_wall_time);
    assert(strcmp(s_etag, "\"good-v1\"") == 0);
    reset_feed_data_t old_data = s_snapshot.data;

    status_code = 304;
    etag_header = NULL;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NONE && response.unchanged);
    assert(strcmp(sent_etag, "\"good-v1\"") == 0);
    apply_response(&response, 301000, fake_wall_time + 300);
    assert(memcmp(&s_snapshot.data, &old_data, sizeof(old_data)) == 0);
    assert(s_snapshot.last_checked_at == fake_wall_time + 300);
    assert(strcmp(s_etag, "\"good-v1\"") == 0);

    status_code = 503;
    etag_header = "\"error-page\"";
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_HTTP);
    apply_response(&response, 601000, fake_wall_time + 600);
    assert(s_snapshot.error == RESET_FEED_ERROR_HTTP && s_snapshot.has_data);
    assert(memcmp(&s_snapshot.data, &old_data, sizeof(old_data)) == 0);
    assert(s_snapshot.last_checked_at == fake_wall_time + 300);
    assert(strcmp(s_etag, "\"good-v1\"") == 0);

    reset_fixture();
    status_code = 304;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_HTTP);
    /* Also protect against a detached ETag with no usable cached data. */
    strcpy(s_etag, "\"orphan\"");
    response = fetch();
    apply_response(&response, 1000, fake_wall_time);
    assert(!s_snapshot.has_data && s_snapshot.error == RESET_FEED_ERROR_HTTP);
}

static void test_readiness_and_failure(void)
{
    response_t response;
    reset_fixture();
    s_clock_ready = false;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NETWORK && init_calls == 0);
    s_clock_ready = true;
    s_wifi_ready = false;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NETWORK && init_calls == 0);
    s_wifi_ready = true;
    fake_wall_time = 100;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NETWORK && init_calls == 0);

    reset_fixture();
    init_fail = true;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_MEMORY);
    reset_fixture();
    open_fail = true;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NETWORK);
    reset_fixture();
    read_fail = true;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NETWORK);
    reset_fixture();
    fail_body_alloc = true;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_MEMORY);
    reset_fixture();
    announced_length = -1;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NETWORK);
    reset_fixture();
    body_incomplete = true;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NETWORK);
    reset_fixture();
    read_advance_us = 31000000;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NETWORK);
    reset_fixture();
    fake_wall_time -= 86400;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_FORMAT);
}

static void test_limits_headers_and_status(void)
{
    response_t response;
    reset_fixture();
    announced_length = RESET_FEED_BODY_LIMIT + 1;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_TOO_LARGE && read_calls == 0);
    char *oversize = malloc(RESET_FEED_BODY_LIMIT + 1);
    assert(oversize);
    memset(oversize, ' ', RESET_FEED_BODY_LIMIT + 1);
    reset_fixture();
    announced_length = 0; /* Chunked response, unknown size. */
    wire_body = oversize;
    wire_length = RESET_FEED_BODY_LIMIT + 1;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_TOO_LARGE);
    free(oversize);

    const char *bad_types[] = {NULL, "text/html", "application/jsonp", "application/problem+json"};
    for (size_t i = 0; i < sizeof(bad_types) / sizeof(bad_types[0]); ++i) {
        reset_fixture();
        content_type = bad_types[i];
        response = fetch();
        assert(response.error == RESET_FEED_ERROR_FORMAT && read_calls == 0);
    }
    reset_fixture();
    content_encoding = "gzip";
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_FORMAT);
    reset_fixture();
    etag_header = "\"good\"\r\nInjected: unsafe";
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NONE && response.headers.etag[0] == 0);
    reset_fixture();
    content_type = "Application/JSON";
    content_encoding = "identity";
    announced_length = 0;
    response = fetch();
    assert(response.error == RESET_FEED_ERROR_NONE);
    const int codes[] = {301, 302, 401, 403, 404, 500, 503};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        reset_fixture();
        status_code = codes[i];
        response = fetch();
        assert(response.error == RESET_FEED_ERROR_HTTP && read_calls == 0);
    }
}

static void test_rate_limits_and_manual_refresh(void)
{
    const char *retry_values[] = {NULL, "", "-1", "1", "900", "4294967295", "4294967296",
        "Sun, 04 Oct 2026 08:01:40 GMT", "99999999999999999999999999999999"};
    const uint64_t delays[] = {300000, 300000, 300000, 300000, 900000,
        UINT64_C(4294967295000), UINT64_C(4294967296000), 900000, UINT64_MAX};
    for (size_t i = 0; i < sizeof(retry_values) / sizeof(retry_values[0]); ++i) {
        reset_fixture();
        status_code = 429;
        retry_header = retry_values[i];
        response_t response = fetch();
        assert(response.error == RESET_FEED_ERROR_RATE_LIMIT && read_calls == 0);
        apply_response(&response, 1000, fake_wall_time);
        assert(s_poll.hard_backoff_until_ms == add_ms(1000, delays[i]));
        uint64_t auto_delay = delays[i] > 900000 ? delays[i] : 900000;
        assert(s_poll.next_attempt_ms == add_ms(1000, auto_delay));
    }
    reset_fixture();
    assert(reset_feed_start() == ESP_OK);
    assert(reset_feed_start() == ESP_ERR_INVALID_STATE);
    assert(reset_feed_request_refresh() && notifications == 1);
    assert(!reset_feed_request_refresh()); /* Coalesced before worker admission. */
    assert(admit_fetch(0, fake_wall_time));
    assert(!admit_fetch(0, fake_wall_time));
    response_t response = fetch();
    apply_response(&response, 0, fake_wall_time);
    assert(!reset_feed_request_refresh());
    reset_feed_snapshot_t snapshot;
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.refresh_block == RESET_FEED_BLOCK_DEBOUNCE && !snapshot.can_refresh);
    assert(snapshot.retry_in_seconds == 30 && snapshot.retry_at == fake_wall_time + 30);
    assert(snapshot.next_auto_in_seconds == 900 && snapshot.auto_interval_minutes == 15);
    fake_monotonic_us = 29999000;
    assert(!reset_feed_request_refresh());
    fake_monotonic_us = RESET_FEED_MANUAL_DEBOUNCE_MS * 1000;
    fake_wall_time += 30;
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.can_refresh && snapshot.refresh_block == RESET_FEED_BLOCK_NONE);
    assert(snapshot.retry_in_seconds == 0 && snapshot.next_auto_in_seconds == 870);
    assert(reset_feed_request_refresh());
    assert(admit_fetch(30000, fake_wall_time));
    assert(!reset_feed_request_refresh());
    s_fetching = false;
    reset_feed_set_ready(false, true);
    assert(!reset_feed_request_refresh());
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.status == RESET_FEED_WAITING_WIFI && snapshot.refresh_block == RESET_FEED_BLOCK_WIFI);
    reset_feed_set_ready(true, false);
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.status == RESET_FEED_WAITING_CLOCK && snapshot.refresh_block == RESET_FEED_BLOCK_CLOCK);
    reset_feed_set_ready(true, true);
    fake_monotonic_us = (RESET_FEED_STALE_AFTER_MS + 1) * 1000;
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.stale && snapshot.has_data && snapshot.status == RESET_FEED_CURRENT);

    reset_fixture();
    task_creation_fail = true;
    assert(reset_feed_start() == ESP_ERR_NO_MEM);
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.status == RESET_FEED_ERROR && snapshot.error == RESET_FEED_ERROR_MEMORY);
    assert(snapshot.refresh_block == RESET_FEED_BLOCK_NOT_STARTED);
    assert(!reset_feed_request_refresh());
    reset_feed_get_snapshot(NULL);
}

static void test_interval_and_pause(void)
{
    reset_fixture();
    assert(reset_feed_start() == ESP_OK);
    assert(admit_fetch(0, fake_wall_time));
    response_t response = fetch();
    apply_response(&response, 0, fake_wall_time);
    assert(reset_feed_set_auto_interval(5));
    assert(s_poll.next_attempt_ms == 300000);
    assert(reset_feed_set_auto_interval(60));
    assert(s_poll.next_attempt_ms == 3600000);
    assert(!reset_feed_set_auto_interval(1));
    assert(s_poll.auto_interval_minutes == 60);
    assert(!reset_feed_export_rtc(&(reset_feed_rtc_state_t){0}));
    assert(reset_feed_pause_and_wait(0));
    assert(!admit_fetch(7200000, fake_wall_time + 7200));
    assert(!reset_feed_request_refresh());
    reset_feed_snapshot_t snapshot;
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.status == RESET_FEED_PAUSED && snapshot.refresh_block == RESET_FEED_BLOCK_PAUSED);
    reset_feed_rtc_state_t state;
    assert(reset_feed_export_rtc(&state));
    assert(!reset_feed_export_rtc(NULL));
    reset_feed_resume(false);
    assert(!s_paused && !s_refresh_pending);
    assert(!admit_fetch(30000, fake_wall_time + 30)); /* Automatic interval still 60m. */
    reset_feed_resume(true);
    assert(admit_fetch(30000, fake_wall_time + 30)); /* Explicit resume refresh. */
    assert(!reset_feed_pause_and_wait(25));
    assert(s_paused && s_fetching && fake_monotonic_us == 25000);
    assert(!reset_feed_export_rtc(&state));
    finish_fetch_on_delay = true;
    assert(reset_feed_pause_and_wait(100));
    assert(!s_fetching && s_paused);
    assert(reset_feed_export_rtc(&state));
    assert(!admit_fetch(7200000, fake_wall_time + 7200));
}

static reset_feed_rtc_state_t cached_state(bool rate_limited)
{
    reset_fixture();
    assert(reset_feed_start() == ESP_OK);
    assert(admit_fetch(0, fake_wall_time));
    response_t response = fetch();
    apply_response(&response, 0, fake_wall_time);
    if (rate_limited) {
        fake_monotonic_us = 30000000;
        fake_wall_time += 30;
        assert(reset_feed_request_refresh());
        assert(admit_fetch(30000, fake_wall_time));
        status_code = 429;
        retry_header = "900";
        response = fetch();
        apply_response(&response, 30000, fake_wall_time);
    }
    assert(reset_feed_pause_and_wait(0));
    reset_feed_rtc_state_t state;
    assert(reset_feed_export_rtc(&state));
    return state;
}

static void test_rtc_wake_and_corruption(void)
{
    reset_feed_rtc_state_t state = cached_state(false);
    assert(state.has_data && state.last_checked_at && state.last_attempt_at);
    assert(state.auto_interval_minutes == 15);
    reset_feed_rtc_state_t bad = state;
    bad.data.stats.total ^= 1;
    reset_fixture();
    assert(!reset_feed_import_rtc(&bad));
    assert(!reset_feed_import_rtc(NULL));
    bad = state;
    bad.version++;
    bad.checksum = rtc_checksum(&bad);
    assert(!reset_feed_import_rtc(&bad));
    bad = state;
    bad.size--;
    bad.checksum = rtc_checksum(&bad);
    assert(!reset_feed_import_rtc(&bad));
    bad = state;
    bad.auto_interval_minutes = 1;
    bad.checksum = rtc_checksum(&bad);
    assert(!reset_feed_import_rtc(&bad));
    bad = state;
    bad.last_attempt_at = -1;
    bad.checksum = rtc_checksum(&bad);
    assert(!reset_feed_import_rtc(&bad));
    assert(!s_snapshot.has_data && !s_restore_pending); /* Invalid RTC is atomic. */

    fake_wall_time = (time_t)(state.last_attempt_at + 20);
    s_clock_ready = false;
    assert(reset_feed_import_rtc(&state));
    assert(reset_feed_start() == ESP_OK);
    assert(!reset_feed_import_rtc(&state)); /* Cannot replace live policy. */
    reset_feed_snapshot_t snapshot;
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.has_data && snapshot.stale && snapshot.status == RESET_FEED_WAITING_CLOCK);
    assert(!admit_fetch(0, fake_wall_time));
    assert(init_calls == 0 && s_restore_pending);
    reset_feed_set_ready(true, true);
    assert(!s_restore_pending);
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.refresh_block == RESET_FEED_BLOCK_DEBOUNCE && snapshot.retry_in_seconds == 10);
    assert(snapshot.next_auto_in_seconds == 880);
    assert(!admit_fetch(9999, fake_wall_time + 9));
    fake_monotonic_us = 10000000;
    fake_wall_time += 10;
    assert(admit_fetch(10000, fake_wall_time)); /* Wake bypasses remaining 870s auto wait. */
    response_t response = fetch();
    assert(sent_etag[0] == 0 && !response.unchanged);
    apply_response(&response, 10000, fake_wall_time);
    reset_feed_get_snapshot(&snapshot);
    assert(!snapshot.stale && snapshot.status == RESET_FEED_CURRENT);

    /* A normal wake after a long sleep fetches immediately after actual SNTP. */
    reset_fixture();
    fake_wall_time = (time_t)(state.last_attempt_at + 3600);
    s_clock_ready = false;
    assert(reset_feed_import_rtc(&state));
    assert(reset_feed_start() == ESP_OK);
    assert(!admit_fetch(0, fake_wall_time));
    reset_feed_set_ready(false, true); /* User Wi-Fi OFF: still no HTTP. */
    assert(!admit_fetch(0, fake_wall_time));
    reset_feed_set_ready(true, true);
    assert(admit_fetch(0, fake_wall_time));
}

static void test_rtc_preserves_backoff(void)
{
    reset_feed_rtc_state_t state = cached_state(true);
    assert(state.failures == 1 && state.hard_backoff_until == state.last_attempt_at + 900);
    reset_fixture();
    s_clock_ready = false;
    fake_wall_time = (time_t)(state.last_attempt_at + 20);
    assert(reset_feed_import_rtc(&state));
    /* Sleeping again before SNTP must not lose or reinterpret RTC deadlines. */
    assert(reset_feed_pause_and_wait(0));
    reset_feed_rtc_state_t second;
    assert(reset_feed_export_rtc(&second));
    assert(second.hard_backoff_until == state.hard_backoff_until &&
           second.last_attempt_at == state.last_attempt_at);
    reset_fixture();
    s_clock_ready = false;
    fake_wall_time = (time_t)(state.last_attempt_at + 20);
    assert(reset_feed_import_rtc(&second));
    assert(reset_feed_start() == ESP_OK);
    reset_feed_set_ready(true, true);
    reset_feed_snapshot_t snapshot;
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.refresh_block == RESET_FEED_BLOCK_BACKOFF && snapshot.retry_in_seconds == 880);
    assert(snapshot.error == RESET_FEED_ERROR_RATE_LIMIT && snapshot.stale);
    assert(!reset_feed_request_refresh());
    assert(reset_feed_set_auto_interval(5));
    reset_feed_resume(true);
    assert(!admit_fetch(879999, fake_wall_time + 879));
    fake_monotonic_us = 880000000;
    fake_wall_time += 880;
    assert(admit_fetch(880000, fake_wall_time));
    status_code = 503;
    response_t response = fetch();
    apply_response(&response, 880000, fake_wall_time);
    assert(s_poll.failures == 2 && s_poll.hard_backoff_until_ms == 1480000);
    reset_feed_get_snapshot(&snapshot);
    assert(snapshot.refresh_block == RESET_FEED_BLOCK_BACKOFF && snapshot.retry_in_seconds == 600);
    assert(snapshot.next_auto_in_seconds == 600);
}

int main(void)
{
    test_fetch_and_cache();
    test_readiness_and_failure();
    test_limits_headers_and_status();
    test_rate_limits_and_manual_refresh();
    test_interval_and_pause();
    test_rtc_wake_and_corruption();
    test_rtc_preserves_backoff();
    puts("Reset feed HTTPS fault-injection tests: PASS");
    return 0;
}
