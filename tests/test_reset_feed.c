#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "reset_feed.h"

static const char fixture[] =
    "{\"data\":{\"latest_reset\":{\"id\":\"2106131810921136451\","
    "\"reset_type\":\"regular\",\"announced_at\":\"2026-10-02T21:18:48.000Z\","
    "\"text\":\"Reset all propagated. Enjoy.\",\"source\":{\"type\":\"x_post\","
    "\"author\":\"thsottiaux\",\"url\":\"https://x.com/thsottiaux/status/2106131810921136451\"}},"
    "\"scheduled_reset\":null,\"active_watch\":null,\"stats\":{\"total\":57,"
    "\"last_reset_at\":\"2026-10-02T21:18:48.000Z\",\"days_since_last\":1.4,"
    "\"avg_interval_days\":6.8}},\"meta\":{\"api_version\":\"v1\","
    "\"generated_at\":\"2026-10-04T06:48:33.122Z\"}}";

static const char scheduled[] =
    "{\"id\":\"scheduled-123\",\"status\":\"scheduled\",\"reset_type\":\"banked\","
    "\"announced_at\":\"2026-10-01T01:00:00Z\",\"scheduled_for\":\"2026-10-02T01:00:00Z\","
    "\"text\":\"This is still awaiting execution evidence.\",\"source\":{\"type\":\"observed\"}}";

static const char watch[] =
    "{\"level\":\"strong\",\"reset_chance_percent\":87,\"forecast_window\":\"soon\","
    "\"observed_at\":\"2026-10-01T01:00:00Z\",\"expires_at\":\"2026-10-03T01:00:00Z\","
    "\"text\":\"AI forecast only.\",\"source\":{\"type\":\"observed\",\"url\":null}}";

static cJSON *item(cJSON *object, const char *key)
{
    cJSON *result = cJSON_GetObjectItemCaseSensitive(object, key);
    assert(result);
    return result;
}

static bool parse_tree(cJSON *tree, reset_feed_data_t *out)
{
    char *text = cJSON_PrintUnformatted(tree);
    assert(text);
    bool result = reset_feed_parse(text, strlen(text), out);
    cJSON_free(text);
    return result;
}

static void reject_json(const char *json, size_t length)
{
    reset_feed_data_t original;
    memset(&original, 0xa5, sizeof(original));
    reset_feed_data_t result = original;
    assert(!reset_feed_parse(json, length, &result));
    assert(memcmp(&original, &result, sizeof(original)) == 0);
}

static void reject_tree(cJSON *tree)
{
    char *text = cJSON_PrintUnformatted(tree);
    assert(text);
    reject_json(text, strlen(text));
    cJSON_free(text);
    cJSON_Delete(tree);
}

static void test_valid_feed(void)
{
    reset_feed_data_t data = {0};
    assert(reset_feed_parse(fixture, strlen(fixture), &data));
    assert(data.latest.present && data.latest.kind == RESET_KIND_REGULAR);
    assert(!data.latest.observed);
    assert(strcmp(data.latest.id, "2106131810921136451") == 0);
    assert(data.latest.announced_at == INT64_C(1790975928));
    assert(data.generated_at == INT64_C(1791096513));
    assert(!data.scheduled.present && !data.watch.present);
    assert(data.watch.confidence_percent == -1);
    assert(data.stats.total == 57 && data.stats.has_last_reset_at);
    assert(data.stats.last_reset_at == data.latest.announced_at);
    assert(data.stats.has_days_since_last && data.stats.days_since_last == 1.4);
    assert(data.stats.has_avg_interval_days && data.stats.avg_interval_days == 6.8);

    cJSON *root = cJSON_Parse(fixture);
    cJSON *body = item(root, "data");
    cJSON *latest = item(body, "latest_reset");
    cJSON_ReplaceItemInObjectCaseSensitive(latest, "source", cJSON_Parse("{\"type\":\"observed\"}"));
    cJSON_ReplaceItemInObjectCaseSensitive(latest, "id", cJSON_CreateString("observed-20261002"));
    cJSON_ReplaceItemInObjectCaseSensitive(latest, "reset_type", cJSON_CreateString("banked"));
    cJSON_ReplaceItemInObjectCaseSensitive(body, "scheduled_reset", cJSON_Parse(scheduled));
    cJSON_ReplaceItemInObjectCaseSensitive(body, "active_watch", cJSON_Parse(watch));
    assert(parse_tree(root, &data));
    assert(data.latest.observed && data.latest.kind == RESET_KIND_BANKED);
    assert(data.scheduled.present && data.scheduled.has_time);
    /* The passed date must not become a new executed reset. */
    assert(data.scheduled.scheduled_for < data.generated_at);
    assert(data.latest.announced_at == INT64_C(1790975928));
    assert(data.watch.present && data.watch.level == RESET_WATCH_STRONG);
    assert(data.watch.confidence_percent == 87);
    assert(data.watch.expires_at < data.generated_at); /* UI marks expiry. */

    cJSON_ReplaceItemInObjectCaseSensitive(item(body, "scheduled_reset"), "scheduled_for", cJSON_CreateNull());
    cJSON_ReplaceItemInObjectCaseSensitive(item(body, "active_watch"), "reset_chance_percent", cJSON_CreateNull());
    cJSON_ReplaceItemInObjectCaseSensitive(item(body, "active_watch"), "level", cJSON_CreateString("elevated"));
    cJSON_ReplaceItemInObjectCaseSensitive(body, "latest_reset", cJSON_CreateNull());
    assert(parse_tree(root, &data));
    assert(!data.latest.present && data.scheduled.present && !data.scheduled.has_time);
    assert(data.watch.confidence_percent == -1 && data.watch.level == RESET_WATCH_ELEVATED);
    cJSON_Delete(root);

    /* RFC-legal JSON whitespace and unknown future scalar fields are safe. */
    char spaced[sizeof(fixture) + 10];
    snprintf(spaced, sizeof(spaced), " \n%s\t\r\n", fixture);
    assert(reset_feed_parse(spaced, strlen(spaced), &data));
}

static void test_stats(void)
{
    reset_feed_data_t data;
    cJSON *root = cJSON_Parse(fixture);
    cJSON *stats = item(item(root, "data"), "stats");
    cJSON_ReplaceItemInObjectCaseSensitive(stats, "total", cJSON_CreateNumber(0));
    const char *nullable[] = {"last_reset_at", "days_since_last", "avg_interval_days"};
    for (size_t i = 0; i < sizeof(nullable) / sizeof(nullable[0]); ++i) {
        cJSON_ReplaceItemInObjectCaseSensitive(stats, nullable[i], cJSON_CreateNull());
    }
    assert(parse_tree(root, &data));
    assert(data.stats.total == 0 && !data.stats.has_last_reset_at &&
           !data.stats.has_days_since_last && !data.stats.has_avg_interval_days);
    cJSON_Delete(root);
    const char *required[] = {"total", "last_reset_at", "days_since_last", "avg_interval_days"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        root = cJSON_Parse(fixture);
        cJSON_DeleteItemFromObjectCaseSensitive(item(item(root, "data"), "stats"), required[i]);
        reject_tree(root);
        root = cJSON_Parse(fixture);
        cJSON_ReplaceItemInObjectCaseSensitive(item(item(root, "data"), "stats"),
                                             required[i], cJSON_CreateString("invalid"));
        reject_tree(root);
    }
    const double invalid_totals[] = {-1, 0.5, 4294967296.0, 1e100};
    for (size_t i = 0; i < sizeof(invalid_totals) / sizeof(invalid_totals[0]); ++i) {
        root = cJSON_Parse(fixture);
        cJSON_ReplaceItemInObjectCaseSensitive(item(item(root, "data"), "stats"),
                                             "total", cJSON_CreateNumber(invalid_totals[i]));
        reject_tree(root);
    }
    const char *numeric[] = {"days_since_last", "avg_interval_days"};
    for (size_t i = 0; i < sizeof(numeric) / sizeof(numeric[0]); ++i) {
        root = cJSON_Parse(fixture);
        stats = item(item(root, "data"), "stats");
        cJSON_ReplaceItemInObjectCaseSensitive(stats, numeric[i], cJSON_CreateNumber(-0.01));
        reject_tree(root);
        root = cJSON_Parse(fixture);
        stats = item(item(root, "data"), "stats");
        cJSON_ReplaceItemInObjectCaseSensitive(stats, numeric[i], cJSON_CreateBool(true));
        reject_tree(root);
        root = cJSON_Parse(fixture);
        stats = item(item(root, "data"), "stats");
        cJSON_ReplaceItemInObjectCaseSensitive(stats, numeric[i], cJSON_CreateRaw("1e999"));
        reject_tree(root);
    }
    root = cJSON_Parse(fixture);
    stats = item(item(root, "data"), "stats");
    cJSON_ReplaceItemInObjectCaseSensitive(stats, "last_reset_at", cJSON_CreateString("2026-10-05T00:00:00Z"));
    reject_tree(root);
    root = cJSON_Parse(fixture);
    stats = item(item(root, "data"), "stats");
    cJSON_ReplaceItemInObjectCaseSensitive(stats, "total", cJSON_CreateNumber(UINT32_MAX));
    cJSON_ReplaceItemInObjectCaseSensitive(stats, "days_since_last", cJSON_CreateNumber(0));
    cJSON_ReplaceItemInObjectCaseSensitive(stats, "avg_interval_days", cJSON_CreateNumber(0));
    assert(parse_tree(root, &data));
    assert(data.stats.total == UINT32_MAX && data.stats.has_days_since_last &&
           data.stats.days_since_last == 0 && data.stats.has_avg_interval_days);
    cJSON_Delete(root);
}

static void test_timestamps(void)
{
    int64_t epoch;
    assert(reset_feed_parse_timestamp("1970-01-01T00:00:00Z", &epoch) && epoch == 0);
    assert(reset_feed_parse_timestamp("2026-10-02T21:18:48Z", &epoch) && epoch == INT64_C(1790975928));
    assert(reset_feed_parse_timestamp("2026-10-02T14:18:48-07:00", &epoch) && epoch == INT64_C(1790975928));
    assert(reset_feed_parse_timestamp("2026-10-02t23:18:48.123456789+02:00", &epoch) && epoch == INT64_C(1790975928));
    assert(reset_feed_parse_timestamp("2000-02-29T00:00:00z", &epoch));
    assert(reset_feed_parse_timestamp("2024-02-29T00:00:00Z", &epoch));
    const char *bad[] = {
        "", "2026", "2026-02-29T00:00:00Z", "2100-02-29T00:00:00Z",
        "2026-04-31T00:00:00Z", "2026-00-01T00:00:00Z", "2026-13-01T00:00:00Z",
        "2026-10-00T00:00:00Z", "2026-10-01T24:00:00Z", "2026-10-01T01:60:00Z",
        "2026-10-01T01:00:60Z", "2026-10-01T01:00:00", "2026-10-01T01:00:00.Z",
        "2026-10-01T01:00:00.1234567890Z", "2026-10-01T01:00:00Zgarbage",
        "2026-10-01T01:00:00+24:00", "2026-10-01T01:00:00+02:60",
        "2026-10-01 01:00:00Z", "1969-12-31T23:59:59Z", "1970-01-01T00:00:00+01:00",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        epoch = 42;
        assert(!reset_feed_parse_timestamp(bad[i], &epoch));
        assert(epoch == 42);
    }
    assert(!reset_feed_parse_timestamp(NULL, &epoch));
    assert(!reset_feed_parse_timestamp("2026-10-01T01:00:00Z", NULL));
}

static void test_invalid_feed(void)
{
    const char *invalid[] = {"", "{}", "[]", "null", "true", "{", "{\"data\":null}",
        "{\"data\":{},\"data\":{}}", "{\"key\":\"embedded\\u0000zero\"}",
        "{\"key\":\"bad\nline\"}", "{\"key\":\"bad\\qescape\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        reject_json(invalid[i], strlen(invalid[i]));
    }
    reject_json(NULL, 1);
    assert(!reset_feed_parse(fixture, strlen(fixture), NULL));
    for (size_t i = 0; i < strlen(fixture); ++i) reject_json(fixture, i);
    char trailing[sizeof(fixture) + 5];
    snprintf(trailing, sizeof(trailing), "%s{}", fixture);
    reject_json(trailing, strlen(trailing));
    memcpy(trailing, fixture, sizeof(fixture));
    trailing[20] = '\0';
    reject_json(trailing, sizeof(fixture) - 1);

    const char *required[] = {"latest_reset", "scheduled_reset", "active_watch", "stats"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        cJSON *root = cJSON_Parse(fixture);
        cJSON_DeleteItemFromObjectCaseSensitive(item(root, "data"), required[i]);
        reject_tree(root);
    }
    const char *bad_kinds[] = {"weekly", "REGULAR", "regular ", "", "banked\n"};
    for (size_t i = 0; i < sizeof(bad_kinds) / sizeof(bad_kinds[0]); ++i) {
        cJSON *root = cJSON_Parse(fixture);
        cJSON_ReplaceItemInObjectCaseSensitive(item(item(root, "data"), "latest_reset"),
                                             "reset_type", cJSON_CreateString(bad_kinds[i]));
        reject_tree(root);
    }
    cJSON *root = cJSON_Parse(fixture);
    cJSON_AddStringToObject(item(root, "meta"), "api_version", "v1");
    reject_tree(root);
    root = cJSON_Parse(fixture);
    cJSON_ReplaceItemInObjectCaseSensitive(item(root, "meta"), "api_version", cJSON_CreateString("v2"));
    reject_tree(root);
    root = cJSON_Parse(fixture);
    cJSON_ReplaceItemInObjectCaseSensitive(item(root, "meta"), "generated_at", cJSON_CreateString("2100-01-01T00:00:00Z"));
    reject_tree(root);
    root = cJSON_Parse(fixture);
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(root, "data"), "latest_reset"),
                                         "announced_at", cJSON_CreateString("2026-10-05T00:00:00Z"));
    reject_tree(root);
    root = cJSON_Parse(fixture);
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(root, "data"), "latest_reset"),
                                         "announced_at", cJSON_CreateString("2020-01-01T00:00:00Z"));
    reject_tree(root);
    root = cJSON_Parse(fixture);
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(item(root, "data"), "latest_reset"), "source"),
                                         "author", cJSON_CreateString("someone_else"));
    reject_tree(root);
    root = cJSON_Parse(fixture);
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(item(root, "data"), "latest_reset"), "source"),
                                         "type", cJSON_CreateString("rumor"));
    reject_tree(root);
    char big_id[66];
    memset(big_id, 'a', sizeof(big_id) - 1);
    big_id[sizeof(big_id) - 1] = 0;
    root = cJSON_Parse(fixture);
    cJSON_ReplaceItemInObjectCaseSensitive(item(item(root, "data"), "latest_reset"), "id", cJSON_CreateString(big_id));
    reject_tree(root);

    const double bad_chances[] = {-1, 101, 50.5, 1e100};
    for (size_t i = 0; i < sizeof(bad_chances) / sizeof(bad_chances[0]); ++i) {
        root = cJSON_Parse(fixture);
        cJSON *w = cJSON_Parse(watch);
        cJSON_ReplaceItemInObjectCaseSensitive(w, "reset_chance_percent", cJSON_CreateNumber(bad_chances[i]));
        cJSON_ReplaceItemInObjectCaseSensitive(item(root, "data"), "active_watch", w);
        reject_tree(root);
    }
    root = cJSON_Parse(fixture);
    cJSON *w = cJSON_Parse(watch);
    cJSON_ReplaceItemInObjectCaseSensitive(w, "expires_at", cJSON_CreateString("2026-09-01T01:00:00Z"));
    cJSON_ReplaceItemInObjectCaseSensitive(item(root, "data"), "active_watch", w);
    reject_tree(root);
    root = cJSON_Parse(fixture);
    cJSON *s = cJSON_Parse(scheduled);
    cJSON_ReplaceItemInObjectCaseSensitive(s, "status", cJSON_CreateString("executed"));
    cJSON_ReplaceItemInObjectCaseSensitive(item(root, "data"), "scheduled_reset", s);
    reject_tree(root);
}

static void test_resource_limits(void)
{
    char *large = malloc(RESET_FEED_BODY_LIMIT + 2);
    assert(large);
    memset(large, ' ', RESET_FEED_BODY_LIMIT + 1);
    memcpy(large, fixture, strlen(fixture));
    reset_feed_data_t data;
    assert(reset_feed_parse(large, RESET_FEED_BODY_LIMIT, &data));
    reject_json(large, RESET_FEED_BODY_LIMIT + 1);
    free(large);

    cJSON *root = cJSON_Parse(fixture);
    cJSON *nested = cJSON_AddObjectToObject(root, "extra");
    for (int i = 0; i < 14; ++i) nested = cJSON_AddObjectToObject(nested, "extra");
    reject_tree(root);
    root = cJSON_Parse(fixture);
    cJSON *array = cJSON_AddArrayToObject(root, "extra");
    for (int i = 0; i < 256; ++i) cJSON_AddItemToArray(array, cJSON_CreateNumber(i));
    reject_tree(root);
}

static void test_retry_policy(void)
{
    uint32_t seconds = 42;
    assert(reset_feed_parse_retry_after("0", &seconds) && seconds == 0);
    assert(reset_feed_parse_retry_after("900", &seconds) && seconds == 900);
    assert(reset_feed_parse_retry_after("4294967295", &seconds) && seconds == UINT32_MAX);
    const char *bad[] = {"", "-1", "1.5", "+20", "4294967296", "99999999999", "30\r\n", "Sunday"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        seconds = 42;
        assert(!reset_feed_parse_retry_after(bad[i], &seconds));
        assert(seconds == 42);
    }
    assert(!reset_feed_parse_retry_after(NULL, &seconds));

    reset_feed_poll_t poll = {0};
    assert(reset_feed_poll_due(&poll, 0));
    reset_feed_poll_complete(&poll, 1000, true, 0);
    assert(poll.next_attempt_ms == 901000); /* Default 15 minutes. */
    poll.auto_interval_minutes = 5;
    reset_feed_poll_complete(&poll, 1000, true, 0);
    assert(poll.next_attempt_ms == 301000 && poll.failures == 0);
    assert(!reset_feed_poll_due(&poll, 300999));
    assert(reset_feed_poll_due(&poll, 301000));
    const uint64_t backoffs[] = {300000, 600000, 1200000, 2400000, 3600000, 3600000};
    for (size_t i = 0; i < sizeof(backoffs) / sizeof(backoffs[0]); ++i) {
        reset_feed_poll_complete(&poll, 1000, false, 0);
        assert(poll.next_attempt_ms == 1000 + backoffs[i]);
    }
    reset_feed_poll_complete(&poll, 1000, false, 9000);
    assert(poll.next_attempt_ms == 9001000);
    reset_feed_poll_complete(&poll, 1000, true, 9000);
    assert(poll.next_attempt_ms == 301000 && poll.failures == 0);
    reset_feed_poll_complete(&poll, 1000, false, 1);
    assert(poll.next_attempt_ms == 301000); /* Failure backoff is a hard floor. */
    reset_feed_poll_complete(&poll, UINT64_MAX - 1, true, 0);
    assert(poll.next_attempt_ms == UINT64_MAX);
    assert(!reset_feed_poll_due(NULL, 0));
    assert(!reset_feed_poll_manual_due(NULL, 0));
    reset_feed_poll_begin(NULL, 0);
    reset_feed_poll_complete(NULL, 0, false, 0);
}

static void test_manual_policy(void)
{
    const uint32_t intervals[] = {5, 15, 30, 60};
    for (size_t i = 0; i < sizeof(intervals) / sizeof(intervals[0]); ++i) {
        assert(reset_feed_interval_valid(intervals[i]));
        reset_feed_poll_t poll = {.auto_interval_minutes = intervals[i]};
        reset_feed_poll_begin(&poll, 1000);
        reset_feed_poll_complete(&poll, 2000, true, 0);
        assert(poll.next_attempt_ms == 2000 + (uint64_t)intervals[i] * 60000);
        assert(!reset_feed_poll_manual_due(&poll, 30999));
        assert(reset_feed_poll_manual_due(&poll, 31000));
        assert(!reset_feed_poll_due(&poll, 31000));
        reset_feed_poll_begin(&poll, 31000);
        reset_feed_poll_complete(&poll, 32000, false, 9000);
        assert(poll.hard_backoff_until_ms == 9032000);
        assert(!reset_feed_poll_manual_due(&poll, 9031999));
        assert(reset_feed_poll_manual_due(&poll, 9032000));
        reset_feed_poll_complete(&poll, 1000, false, UINT64_MAX);
        assert(poll.hard_backoff_until_ms == UINT64_MAX && poll.next_attempt_ms == UINT64_MAX);
    }
    assert(!reset_feed_interval_valid(0) && !reset_feed_interval_valid(1) &&
           !reset_feed_interval_valid(10) && !reset_feed_interval_valid(61));
}

static void test_retry_http_dates(void)
{
    uint64_t seconds = 42;
    const int64_t now = INT64_C(1791100000);
    const char *dates[] = {"Sun, 04 Oct 2026 08:01:40 GMT",
                           "Sunday, 04-Oct-26 08:01:40 GMT",
                           "Sun Oct  4 08:01:40 2026"};
    for (size_t i = 0; i < sizeof(dates) / sizeof(dates[0]); ++i) {
        assert(reset_feed_parse_retry_after_at(dates[i], now, &seconds));
        assert(seconds == 900);
    }
    assert(reset_feed_parse_retry_after_at("Sun, 06 Nov 1994 08:49:37 GMT", now, &seconds) && seconds == 0);
    assert(reset_feed_parse_retry_after_at("Sunday, 06-Nov-94 08:49:37 GMT", now, &seconds) && seconds == 0);
    assert(reset_feed_parse_retry_after_at(" \t900 \t", now, &seconds) && seconds == 900);
    assert(reset_feed_parse_retry_after_at("4294967296", now, &seconds) && seconds == UINT64_C(4294967296));
    assert(reset_feed_parse_retry_after_at("18446744073709551615", now, &seconds) && seconds == UINT64_MAX);
    assert(reset_feed_parse_retry_after_at("99999999999999999999999999999999", now, &seconds) && seconds == UINT64_MAX);
    const char *bad[] = {"", "-1", "1.5", "1x", "Sun, 04 Foo 2026 08:01:40 GMT",
                         "Sun, 31 Sep 2026 08:01:40 GMT", "Sun, 04 Oct 2026 08:01:40 GMTgarbage",
                         "Sun, 04 Oct 2026 08:01:40 UTC", "Huh, 04 Oct 2026 08:01:40 GMT"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        seconds = 42;
        assert(!reset_feed_parse_retry_after_at(bad[i], now, &seconds));
        assert(seconds == 42);
    }
    assert(!reset_feed_parse_retry_after_at(NULL, now, &seconds));
    assert(!reset_feed_parse_retry_after_at("1", now, NULL));
}

static int allocation_limit;
static int allocations;
static int outstanding;

static void *limited_malloc(size_t size)
{
    if (allocations++ >= allocation_limit) return NULL;
    void *result = malloc(size);
    if (result) ++outstanding;
    return result;
}

static void limited_free(void *ptr)
{
    if (ptr) --outstanding;
    free(ptr);
}

static void test_allocation_failures(void)
{
    cJSON_Hooks hooks = {.malloc_fn = limited_malloc, .free_fn = limited_free};
    for (allocation_limit = 0; allocation_limit < 128; ++allocation_limit) {
        allocations = 0;
        outstanding = 0;
        cJSON_InitHooks(&hooks);
        reset_feed_data_t original;
        memset(&original, 0xa5, sizeof(original));
        reset_feed_data_t result = original;
        bool success = reset_feed_parse(fixture, strlen(fixture), &result);
        if (!success) assert(memcmp(&original, &result, sizeof(result)) == 0);
        assert(outstanding == 0);
    }
    cJSON_InitHooks(NULL);
}

static void test_mutated_input(void)
{
    /* Deterministic malformed input pass, also run under ASan/UBSan. */
    uint32_t random = 0x12345678;
    for (int i = 0; i < 10000; ++i) {
        char buffer[sizeof(fixture)];
        memcpy(buffer, fixture, sizeof(buffer));
        random = random * 1664525U + 1013904223U;
        size_t position = random % (sizeof(buffer) - 1);
        random = random * 1664525U + 1013904223U;
        buffer[position] = (char)(random >> 24);
        reset_feed_data_t data;
        (void)reset_feed_parse(buffer, sizeof(buffer) - 1, &data);
    }
}

int main(void)
{
    test_valid_feed();
    test_timestamps();
    test_stats();
    test_invalid_feed();
    test_resource_limits();
    test_retry_policy();
    test_manual_policy();
    test_retry_http_dates();
    test_allocation_failures();
    test_mutated_input();
    puts("Reset feed parser and polling tests: PASS");
    return 0;
}
