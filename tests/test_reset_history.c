#include "reset_feed.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t epoch(const char *text)
{
    int64_t result;
    assert(reset_feed_parse_timestamp(text, &result));
    return result;
}

static cJSON *page(const char *generated, bool more, const char *cursor)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddArrayToObject(root, "data");
    cJSON *pagination = cJSON_AddObjectToObject(root, "pagination");
    cJSON_AddBoolToObject(pagination, "has_more", more);
    if (cursor) cJSON_AddStringToObject(pagination, "next_cursor", cursor);
    else cJSON_AddNullToObject(pagination, "next_cursor");
    cJSON *meta = cJSON_AddObjectToObject(root, "meta");
    cJSON_AddStringToObject(meta, "api_version", "v1");
    cJSON_AddStringToObject(meta, "generated_at", generated);
    return root;
}

static void event(cJSON *root, const char *id, const char *kind, const char *at)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", id);
    cJSON_AddStringToObject(item, "reset_type", kind);
    cJSON_AddStringToObject(item, "announced_at", at);
    cJSON_AddStringToObject(item, "text", "");
    cJSON *source = cJSON_AddObjectToObject(item, "source");
    cJSON_AddStringToObject(source, "type", "observed");
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root, "data"), item);
}

static bool consume(reset_history_accumulator_t *state, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    bool result = reset_history_parse_page(state, json, strlen(json));
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

static void test_utc_calendar(void)
{
    int64_t now = epoch("2026-10-04T12:00:00Z");
    int64_t monday = epoch("2026-09-28T00:00:00Z");
    assert(reset_history_window_start(now) == monday - (RESET_HISTORY_WEEKS - 1) * 7 * INT64_C(86400));
    assert(reset_history_window_start(epoch("2026-10-05T00:00:00Z")) ==
           reset_history_window_start(now) + 7 * INT64_C(86400));
    assert(reset_history_window_start(epoch("2024-03-01T03:00:00+03:00")) ==
           epoch("2024-02-26T00:00:00Z") - (RESET_HISTORY_WEEKS - 1) * 7 * INT64_C(86400));
    assert(reset_history_window_start(epoch("2024-01-01T00:00:00Z")) ==
           epoch("2024-01-01T00:00:00Z") - (RESET_HISTORY_WEEKS - 1) * 7 * INT64_C(86400));
    assert(!reset_history_window_start(0));
    assert(!reset_history_window_start(INT64_MAX));

    reset_history_accumulator_t state;
    now = epoch("2024-03-01T12:00:00Z");
    assert(reset_history_begin(&state, now));
    cJSON *root = page("2024-03-01T12:00:00Z", false, NULL);
    event(root, "leap-regular", "regular", "2024-02-29T00:05:00Z");
    event(root, "leap-banked", "banked", "2024-03-01T00:00:00+01:00");
    assert(consume(&state, root) && state.complete && !state.failed);
    unsigned leap_day = (unsigned)((epoch("2024-02-29T00:00:00Z") - state.data.start_day) / 86400);
    assert(state.data.cells[leap_day] == (RESET_HISTORY_KNOWN | RESET_HISTORY_REGULAR | RESET_HISTORY_BANKED));
    assert(reset_history_day_state(&state.data, leap_day, now) == RESET_HISTORY_DAY_BOTH);
    assert(reset_history_day_state(&state.data, leap_day - 1, now) == RESET_HISTORY_DAY_EMPTY);
    assert(reset_history_day_state(&state.data, leap_day + 1, now) == RESET_HISTORY_DAY_EMPTY);
    assert(reset_history_day_state(&state.data, leap_day + 2, now) == RESET_HISTORY_DAY_FUTURE);
    reset_history_data_t kinds = state.data;
    kinds.cells[leap_day] = RESET_HISTORY_KNOWN | RESET_HISTORY_REGULAR;
    assert(reset_history_day_state(&kinds, leap_day, now) == RESET_HISTORY_DAY_REGULAR);
    kinds.cells[leap_day] = RESET_HISTORY_KNOWN | RESET_HISTORY_BANKED;
    assert(reset_history_day_state(&kinds, leap_day, now) == RESET_HISTORY_DAY_BANKED);
    kinds.cells[leap_day] = 0;
    assert(reset_history_day_state(&kinds, leap_day, now) == RESET_HISTORY_DAY_UNKNOWN);
    assert(reset_history_data_valid(&state.data));
    reset_history_data_t projected;
    reset_history_project(&state.data, epoch("2024-03-04T10:00:00Z"), &projected);
    assert(projected.start_day == state.data.start_day + 7 * INT64_C(86400));
    assert(projected.cells[leap_day - 7] == state.data.cells[leap_day]);
    assert(reset_history_day_state(&projected, RESET_HISTORY_DAYS - 7, epoch("2024-03-04T10:00:00Z")) == RESET_HISTORY_DAY_UNKNOWN);
    assert(reset_history_day_state(&projected, RESET_HISTORY_DAYS - 6, epoch("2024-03-04T10:00:00Z")) == RESET_HISTORY_DAY_FUTURE);
}

static void test_urls(void)
{
    reset_history_accumulator_t state;
    assert(reset_history_begin(&state, epoch("2026-09-30T12:34:56Z")));
    char url[RESET_HISTORY_URL_CAPACITY];
    assert(reset_history_build_url(&state.data, NULL, url, sizeof(url)));
    assert(strstr(url, "https://codex-resets.com/api/v1/resets?from="));
    assert(strstr(url, "&to=2026-09-30T12%3A34%3A56Z&limit=25&order=asc"));
    assert(reset_history_build_url(&state.data, "Abc_DEF-012", url, sizeof(url)));
    assert(strstr(url, "&cursor=Abc_DEF-012"));
    char max_cursor[RESET_HISTORY_CURSOR_CAPACITY + 1];
    memset(max_cursor, 'a', sizeof(max_cursor));
    max_cursor[RESET_HISTORY_CURSOR_CAPACITY - 1] = 0;
    assert(reset_history_build_url(&state.data, max_cursor, url, sizeof(url)));
    max_cursor[RESET_HISTORY_CURSOR_CAPACITY - 1] = 'a';
    max_cursor[RESET_HISTORY_CURSOR_CAPACITY] = 0;
    assert(!reset_history_build_url(&state.data, max_cursor, url, sizeof(url)));
    const char *invalid[] = {"", "a+b", "a=b", "a&to=0", "a/b", "a%20", "a b", "a\n"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        assert(!reset_history_cursor_valid(invalid[i]));
        assert(!reset_history_build_url(&state.data, invalid[i], url, sizeof(url)));
    }
    assert(!reset_history_build_url(&state.data, NULL, url, 16));
}

static void test_atomic_pages(void)
{
    const char *now = "2026-09-30T12:00:00Z";
    reset_history_accumulator_t state;
    assert(reset_history_begin(&state, epoch(now)));
    cJSON *root = page(now, true, "opaque_first");
    event(root, "first", "regular", "2026-09-28T10:00:00Z");
    assert(consume(&state, root) && !state.complete && state.has_more && state.pages == 1);
    for (unsigned i = 0; i < RESET_HISTORY_DAYS; ++i) assert(!(state.data.cells[i] & RESET_HISTORY_KNOWN));
    root = page(now, false, NULL);
    event(root, "second", "banked", "2026-09-28T10:00:01Z");
    assert(consume(&state, root) && state.complete && state.pages == 2);
    assert(state.data.cells[RESET_HISTORY_DAYS - 7] == 0x83);
    assert(state.data.cells[RESET_HISTORY_DAYS - 4] == 0); /* Thursday still future. */

    /* Repeated IDs or cursors poison staging even when earlier pages are valid. */
    for (unsigned repeated = 0; repeated < 2; ++repeated) {
        assert(reset_history_begin(&state, epoch(now)));
        root = page(now, true, "same_cursor");
        event(root, "same", "regular", "2026-09-28T10:00:00Z");
        assert(consume(&state, root));
        root = page(now, repeated == 1, repeated ? "same_cursor" : NULL);
        event(root, repeated ? "different" : "same", "regular", "2026-09-28T10:00:00Z");
        assert(!consume(&state, root) && state.failed && !state.complete);
    }
    assert(reset_history_begin(&state, epoch(now)));
    for (unsigned i = 0; i < RESET_HISTORY_MAX_PAGES; ++i) {
        char id[20]; snprintf(id, sizeof(id), "id-%u", i);
        root = page(now, true, id);
        event(root, id, "regular", "2026-09-28T10:00:00Z");
        bool result = consume(&state, root);
        assert(result == (i + 1 < RESET_HISTORY_MAX_PAGES));
    }
    assert(state.failed && !state.complete);

    /* The last allowed page may complete normally, including an empty tail. */
    assert(reset_history_begin(&state, epoch(now)));
    for (unsigned i = 0; i < RESET_HISTORY_MAX_PAGES; ++i) {
        char id[20]; snprintf(id, sizeof(id), "end-%u", i);
        bool more = i + 1 < RESET_HISTORY_MAX_PAGES;
        root = page(now, more, more ? id : NULL);
        if (more) event(root, id, "regular", "2026-09-28T10:00:00Z");
        assert(consume(&state, root));
    }
    assert(state.complete && state.pages == RESET_HISTORY_MAX_PAGES);
}

static void test_rejections_and_bounds(void)
{
    const char *now = "2026-09-30T12:00:00Z";
    const char *bad_times[] = {"2026-07-01T00:00:00Z", "2026-10-01T00:00:00Z", "2026-02-29T00:00:00Z"};
    reset_history_accumulator_t state;
    for (unsigned i = 0; i < sizeof(bad_times)/sizeof(*bad_times); ++i) {
        assert(reset_history_begin(&state, epoch(now)));
        cJSON *root = page(now, false, NULL);
        event(root, "bad", "regular", bad_times[i]);
        assert(!consume(&state, root) && state.failed);
    }
    const char *bad_envelopes[] = {
        "{}", "{\"data\":[],\"pagination\":{\"has_more\":false},\"meta\":{\"api_version\":\"v1\",\"generated_at\":\"2026-09-30T12:00:00Z\"}}",
        "{\"data\":[],\"pagination\":{\"has_more\":true,\"next_cursor\":\"empty\"},\"meta\":{\"api_version\":\"v1\",\"generated_at\":\"2026-09-30T12:00:00Z\"}}",
        "{\"data\":[],\"pagination\":{\"has_more\":false,\"next_cursor\":null},\"meta\":{\"api_version\":\"v1\",\"generated_at\":\"2026-09-29T12:00:00Z\"}}",
        "{\"data\":[],\"data\":[],\"pagination\":{\"has_more\":false,\"next_cursor\":null},\"meta\":{\"api_version\":\"v1\",\"generated_at\":\"2026-09-30T12:00:00Z\"}}"
    };
    for (unsigned i = 0; i < sizeof(bad_envelopes)/sizeof(*bad_envelopes); ++i) {
        assert(reset_history_begin(&state, epoch(now)));
        assert(!reset_history_parse_page(&state, bad_envelopes[i], strlen(bad_envelopes[i])));
        assert(!state.complete && state.failed);
    }
    assert(reset_history_begin(&state, epoch(now)));
    cJSON *root = page(now, false, NULL);
    for (unsigned i = 0; i < RESET_HISTORY_PAGE_LIMIT; ++i) {
        char id[20]; snprintf(id, sizeof(id), "realistic-%u", i);
        event(root, id, i % 2 ? "regular" : "banked", "2026-09-28T10:00:00Z");
    }
    char *json = cJSON_PrintUnformatted(root);
    assert(strlen(json) < RESET_FEED_BODY_LIMIT);
    assert(reset_history_parse_page(&state, json, strlen(json)) && state.item_count == RESET_HISTORY_PAGE_LIMIT);
    cJSON_free(json); cJSON_Delete(root);
    assert(state.complete && state.data.cells[RESET_HISTORY_DAYS - 7] == 0x83);
    assert(reset_history_begin(&state, epoch(now)));
    root = page(now, false, NULL);
    for (unsigned i = 0; i <= RESET_HISTORY_PAGE_LIMIT; ++i) {
        char id[20]; snprintf(id, sizeof(id), "excess-%u", i);
        event(root, id, "regular", "2026-09-28T10:00:00Z");
    }
    assert(!consume(&state, root) && state.failed);
    root = page(now, false, NULL);
    json = cJSON_PrintUnformatted(root);
    for (size_t n = 1; n < strlen(json); ++n) {
        assert(reset_history_begin(&state, epoch(now)));
        assert(!reset_history_parse_page(&state, json, n) && state.failed);
    }
    cJSON_free(json); cJSON_Delete(root);
}

int main(void)
{
    test_utc_calendar();
    test_urls();
    test_atomic_pages();
    test_rejections_and_bounds();
    puts("Reset history UTC calendar, bounded parser and pagination tests: PASS");
    return 0;
}
