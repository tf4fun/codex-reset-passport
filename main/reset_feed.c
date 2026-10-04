#include "reset_feed.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* Bound complexity before cJSON allocates nodes or recursively parses input. */
#define RESET_JSON_MAX_DEPTH 12U
#define RESET_JSON_MAX_TOKENS 256U

/* cJSON accepts raw string bytes without validating UTF-8. Reject malformed,
 * overlong, surrogate and out-of-range sequences before they reach C strings. */
static bool valid_utf8(const char *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned char lead = (unsigned char)text[i++];
        if (lead < 0x80) continue;
        unsigned remaining;
        uint32_t codepoint;
        uint32_t minimum;
        if (lead >= 0xc2 && lead <= 0xdf) {
            remaining = 1; codepoint = lead & 0x1f; minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            remaining = 2; codepoint = lead & 0x0f; minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            remaining = 3; codepoint = lead & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (remaining > length - i) return false;
        while (remaining--) {
            unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    }
    return true;
}

static bool json_preflight_limit(const char *json, size_t length, unsigned max_tokens)
{
    if (!valid_utf8(json, length)) return false;
    unsigned depth = 0;
    unsigned tokens = 0;
    bool quoted = false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)json[i];
        if (c == '\0') return false;
        if (quoted) {
            if (c < 0x20) return false;
            if (c == '\\') {
                if (++i >= length) return false;
                /* cJSON exposes C strings, so embedded NUL is ambiguous. */
                if (json[i] == 'u' && i + 4 < length &&
                    memcmp(json + i + 1, "0000", 4) == 0) return false;
            } else if (c == '"') {
                quoted = false;
            }
        } else if (c == '"') {
            quoted = true;
            if (++tokens > max_tokens) return false;
        } else if (c == '{' || c == '[') {
            if (++depth > RESET_JSON_MAX_DEPTH ||
                ++tokens > max_tokens) return false;
        } else if (c == '}' || c == ']') {
            if (depth == 0) return false;
            --depth;
        } else if (c == ',' || c == ':') {
            if (++tokens > max_tokens) return false;
        }
    }
    return !quoted && depth == 0;
}

static bool json_preflight(const char *json, size_t length)
{
    return json_preflight_limit(json, length, RESET_JSON_MAX_TOKENS);
}

static bool unique_keys(const cJSON *node)
{
    const cJSON *child;
    cJSON_ArrayForEach(child, node) {
        if (cJSON_IsObject(node)) {
            if (child->string == NULL) return false;
            for (const cJSON *next = child->next; next; next = next->next) {
                if (next->string == NULL || strcmp(child->string, next->string) == 0) {
                    return false;
                }
            }
        }
        if ((cJSON_IsObject(child) || cJSON_IsArray(child)) && !unique_keys(child)) {
            return false;
        }
    }
    return true;
}

static bool digits(const char *s, size_t count, int *value)
{
    int result = 0;
    for (size_t i = 0; i < count; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        result = result * 10 + s[i] - '0';
    }
    *value = result;
    return true;
}

static bool leap_year(int year)
{
    return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

bool reset_feed_parse_timestamp(const char *text, int64_t *epoch_seconds)
{
    if (!text || !epoch_seconds) return false;
    size_t n = strlen(text);
    if (n < 20 || n > 40) return false;
    if (text[4] != '-' || text[7] != '-' ||
        (text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':') {
        return false;
    }
    int year, month, day, hour, minute, second;
    if (!digits(text, 4, &year) || !digits(text + 5, 2, &month) ||
        !digits(text + 8, 2, &day) || !digits(text + 11, 2, &hour) ||
        !digits(text + 14, 2, &minute) || !digits(text + 17, 2, &second) ||
        year < 1970 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) {
        return false;
    }
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int max_day = month_days[month - 1] + (month == 2 && leap_year(year));
    if (day < 1 || day > max_day) return false;

    size_t p = 19;
    if (text[p] == '.') {
        size_t start = ++p;
        while (p < n && text[p] >= '0' && text[p] <= '9') ++p;
        if (p == start || p - start > 9) return false;
    }
    int offset = 0;
    if (p < n && (text[p] == 'Z' || text[p] == 'z')) {
        if (p + 1 != n) return false;
    } else if (p < n && (text[p] == '+' || text[p] == '-')) {
        int offset_hour, offset_minute;
        if (n - p != 6 || text[p + 3] != ':' ||
            !digits(text + p + 1, 2, &offset_hour) ||
            !digits(text + p + 4, 2, &offset_minute) ||
            offset_hour > 23 || offset_minute > 59) return false;
        offset = (offset_hour * 60 + offset_minute) * 60;
        if (text[p] == '-') offset = -offset;
    } else {
        return false;
    }
    int y = year - 1;
    int64_t days = (int64_t)(year - 1970) * 365 +
                   (y / 4 - y / 100 + y / 400) - (1969 / 4 - 1969 / 100 + 1969 / 400);
    for (int m = 1; m < month; ++m) days += month_days[m - 1] + (m == 2 && leap_year(year));
    days += day - 1;
    int64_t result = days * 86400 + hour * 3600 + minute * 60 + second - offset;
    if (result < 0) return false;
    *epoch_seconds = result;
    return true;
}

static const cJSON *field(const cJSON *object, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static const char *string_field(const cJSON *object, const char *key)
{
    const cJSON *value = field(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool timestamp_field(const cJSON *object, const char *key, int64_t *out)
{
    /* Codex did not exist before 2021. A conservative 2099 ceiling keeps
     * corrupt remote dates away from UI age/countdown calculations. */
    return reset_feed_parse_timestamp(string_field(object, key), out) &&
           *out >= INT64_C(1609459200) && *out < INT64_C(4102444800);
}

static bool kind_field(const cJSON *object, reset_kind_t *kind)
{
    const char *value = string_field(object, "reset_type");
    if (!value) return false;
    if (strcmp(value, "regular") == 0) *kind = RESET_KIND_REGULAR;
    else if (strcmp(value, "banked") == 0) *kind = RESET_KIND_BANKED;
    else return false;
    return true;
}

static bool valid_source(const cJSON *object)
{
    const cJSON *source = field(object, "source");
    const char *type = string_field(source, "type");
    if (!cJSON_IsObject(source) || !type) return false;
    /* Observations can legitimately have no author or source URL. */
    if (strcmp(type, "observed") == 0) return true;
    const char *author = string_field(source, "author");
    return strcmp(type, "x_post") == 0 && author &&
           strcmp(author, "thsottiaux") == 0;
}

bool reset_feed_source_url_valid(const char *url, size_t capacity)
{
    if (!url || !capacity) return false;
    size_t bound = capacity;
    if (bound > RESET_FEED_SOURCE_URL_MAX_BYTES + 1U) {
        bound = RESET_FEED_SOURCE_URL_MAX_BYTES + 1U;
    }
    const char *end = memchr(url, '\0', bound);
    if (!end) return false;
    size_t length = (size_t)(end - url);
    if (length <= 8 || memcmp(url, "https://", 8) != 0) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)url[i];
        /* A scanner must not reinterpret whitespace or backslashes as URL
         * separators. Only printable ASCII survives, byte-for-byte. */
        if (c <= 0x20 || c >= 0x7f || c == '\\') return false;
    }
    const char *host = url + 8;
    const char *host_end = host;
    while (host_end < end && *host_end != '/' && *host_end != '?' && *host_end != '#') ++host_end;
    size_t host_length = (size_t)(host_end - host);
    static const char *const allowed_hosts[] = {
        "x.com", "www.x.com", "twitter.com", "www.twitter.com", "codex-resets.com",
    };
    for (size_t i = 0; i < sizeof(allowed_hosts) / sizeof(allowed_hosts[0]); ++i) {
        if (host_length == strlen(allowed_hosts[i]) &&
            memcmp(host, allowed_hosts[i], host_length) == 0) return true;
    }
    /* Exact authority matching also excludes userinfo, ports, encoded host
     * names, suffix attacks, lookalikes, and unexpected subdomains. */
    return false;
}

static void copy_source_url(const cJSON *object, bool observed, char *out)
{
    out[0] = '\0';
    if (observed) return;
    const char *url = string_field(field(object, "source"), "url");
    if (url && reset_feed_source_url_valid(url, strlen(url) + 1U)) {
        memcpy(out, url, strlen(url) + 1U);
    }
}

static bool valid_id(const char *id)
{
    if (!id) return false;
    size_t n = strlen(id);
    if (n == 0 || n > 64) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

static bool bounded_text(const cJSON *object, const char *key, char *out, size_t max_bytes,
                         bool *truncated, bool *has_non_ascii)
{
    const char *text = string_field(object, key);
    if (!text) return false;
    size_t length = strlen(text); /* Bounded by the 16 KiB response limit. */
    if (!valid_utf8(text, length)) return false;
    *truncated = length > max_bytes;
    *has_non_ascii = false;
    /* Inspect even the tail beyond the retained bound. Font-specific fallback
     * belongs to the presenter; no Unicode is silently removed or translated. */
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x80) *has_non_ascii = true;
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) return false;
    }
    size_t copy_length = *truncated ? max_bytes : length;
    /* If the first omitted byte is a continuation, exclude its entire
     * codepoint. Valid UTF-8 guarantees this walk stops at its lead byte. */
    while (copy_length && ((unsigned char)text[copy_length] & 0xc0) == 0x80) --copy_length;
    memcpy(out, text, copy_length);
    out[copy_length] = '\0';
    return true;
}

static bool parse_latest(const cJSON *object, reset_announcement_t *out)
{
    if (cJSON_IsNull(object)) return true;
    if (!cJSON_IsObject(object)) return false;
    const char *id = string_field(object, "id");
    if (!valid_id(id) || !kind_field(object, &out->kind) ||
        !timestamp_field(object, "announced_at", &out->announced_at) ||
        !bounded_text(object, "text", out->text, RESET_FEED_TEXT_MAX_BYTES,
                      &out->text_truncated, &out->text_has_non_ascii) || !valid_source(object)) return false;
    memcpy(out->id, id, strlen(id) + 1);
    out->observed = strcmp(string_field(field(object, "source"), "type"), "observed") == 0;
    copy_source_url(object, out->observed, out->source_url);
    out->present = true;
    return true;
}

static bool parse_schedule(const cJSON *object, reset_schedule_t *out)
{
    if (cJSON_IsNull(object)) return true;
    if (!cJSON_IsObject(object)) return false;
    const char *status = string_field(object, "status");
    const char *id = string_field(object, "id");
    const cJSON *date = field(object, "scheduled_for");
    if (!status || strcmp(status, "scheduled") != 0 ||
        !valid_id(id) || !kind_field(object, &out->kind) ||
        !timestamp_field(object, "announced_at", &out->announced_at) ||
        !bounded_text(object, "text", out->text, RESET_FEED_TEXT_MAX_BYTES,
                      &out->text_truncated, &out->text_has_non_ascii) || !valid_source(object)) return false;
    if (!cJSON_IsNull(date)) {
        if (!timestamp_field(object, "scheduled_for", &out->scheduled_for)) return false;
        out->has_time = true;
    }
    memcpy(out->id, id, strlen(id) + 1);
    out->observed = strcmp(string_field(field(object, "source"), "type"), "observed") == 0;
    copy_source_url(object, out->observed, out->source_url);
    out->present = true;
    return true;
}

static bool parse_watch(const cJSON *object, reset_watch_t *out)
{
    out->confidence_percent = -1;
    if (cJSON_IsNull(object)) return true;
    if (!cJSON_IsObject(object)) return false;
    const char *level = string_field(object, "level");
    if (!level) return false;
    if (strcmp(level, "elevated") == 0) out->level = RESET_WATCH_ELEVATED;
    else if (strcmp(level, "strong") == 0) out->level = RESET_WATCH_STRONG;
    else return false;
    const cJSON *chance = field(object, "reset_chance_percent");
    if (!cJSON_IsNull(chance)) {
        if (!cJSON_IsNumber(chance) || !isfinite(chance->valuedouble) ||
            chance->valuedouble < 0 || chance->valuedouble > 100 ||
            chance->valuedouble != (int)chance->valuedouble) return false;
        out->confidence_percent = (int)chance->valuedouble;
    }
    if (!timestamp_field(object, "observed_at", &out->observed_at) ||
        !timestamp_field(object, "expires_at", &out->expires_at) ||
        out->expires_at <= out->observed_at ||
        !bounded_text(object, "forecast_window", out->forecast_window, RESET_FEED_FORECAST_MAX_BYTES,
                      &out->forecast_window_truncated, &out->forecast_window_has_non_ascii) ||
        !string_field(object, "text") || !valid_source(object)) return false;
    out->present = true;
    return true;
}

static bool nullable_days(const cJSON *object, const char *key,
                          bool *present, double *out)
{
    const cJSON *value = field(object, key);
    if (cJSON_IsNull(value)) return true;
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0) return false;
    *present = true;
    *out = value->valuedouble;
    return true;
}

static bool parse_stats(const cJSON *object, reset_stats_t *out)
{
    if (!cJSON_IsObject(object)) return false;
    const cJSON *total = field(object, "total");
    if (!cJSON_IsNumber(total) || !isfinite(total->valuedouble) ||
        total->valuedouble < 0 || total->valuedouble > UINT32_MAX ||
        total->valuedouble != (uint32_t)total->valuedouble) return false;
    out->total = (uint32_t)total->valuedouble;
    if (!cJSON_IsNull(field(object, "last_reset_at"))) {
        if (!timestamp_field(object, "last_reset_at", &out->last_reset_at)) return false;
        out->has_last_reset_at = true;
    }
    return nullable_days(object, "days_since_last", &out->has_days_since_last,
                         &out->days_since_last) &&
           nullable_days(object, "avg_interval_days", &out->has_avg_interval_days,
                         &out->avg_interval_days);
}

bool reset_feed_parse(const char *json, size_t length, reset_feed_data_t *out)
{
    if (!json || !out || length == 0 || length > RESET_FEED_BODY_LIMIT ||
        !json_preflight(json, length)) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return false;
    while (end < json + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    bool valid = cJSON_IsObject(root) && end == json + length && unique_keys(root);
    const cJSON *data = field(root, "data");
    const cJSON *meta = field(root, "meta");
    const char *version = string_field(meta, "api_version");
    reset_feed_data_t candidate = {0};
    valid = valid && cJSON_IsObject(data) && cJSON_IsObject(meta) && version &&
            strcmp(version, "v1") == 0 && parse_stats(field(data, "stats"), &candidate.stats) &&
            timestamp_field(meta, "generated_at", &candidate.generated_at) &&
            parse_latest(field(data, "latest_reset"), &candidate.latest) &&
            parse_schedule(field(data, "scheduled_reset"), &candidate.scheduled) &&
            parse_watch(field(data, "active_watch"), &candidate.watch);
    valid = valid && (!candidate.stats.has_last_reset_at ||
                      candidate.stats.last_reset_at <= candidate.generated_at + 300) &&
            (!candidate.latest.present ||
                      candidate.latest.announced_at <= candidate.generated_at + 300) &&
            (!candidate.scheduled.present ||
             candidate.scheduled.announced_at <= candidate.generated_at + 300) &&
            (!candidate.watch.present ||
             candidate.watch.observed_at <= candidate.generated_at + 300);
    cJSON_Delete(root);
    if (valid) *out = candidate;
    return valid;
}

static uint64_t history_hash(const char *text)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (; *text; ++text) value = (value ^ (unsigned char)*text) * UINT64_C(1099511628211);
    return value;
}

bool reset_history_parse_page(reset_history_accumulator_t *state,
                              const char *json, size_t length)
{
    if (!state) return false;
    if (state->failed || state->complete || state->pages >= RESET_HISTORY_MAX_PAGES ||
        state->item_count > RESET_HISTORY_PAGE_LIMIT * RESET_HISTORY_MAX_PAGES ||
        !state->has_more || !state->data.start_day ||
        state->data.start_day != reset_history_window_start(state->data.through_at) ||
        !json || !length || length > RESET_FEED_BODY_LIMIT ||
        !json_preflight_limit(json, length, 1536U)) {
        state->failed = true;
        state->complete = false;
        return false;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) { state->failed = true; return false; }
    while (end < json + length && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    const cJSON *data = field(root, "data");
    const cJSON *pagination = field(root, "pagination");
    const cJSON *more = field(pagination, "has_more");
    const cJSON *cursor = field(pagination, "next_cursor");
    const cJSON *meta = field(root, "meta");
    const char *version = string_field(meta, "api_version");
    int64_t generated_at = 0;
    bool valid = cJSON_IsObject(root) && end == json + length && unique_keys(root) &&
                 cJSON_IsArray(data) && cJSON_IsObject(pagination) && cJSON_IsBool(more) &&
                 cJSON_IsObject(meta) && version && strcmp(version, "v1") == 0 &&
                 timestamp_field(meta, "generated_at", &generated_at) &&
                 generated_at >= state->data.through_at - 300 &&
                 generated_at <= state->data.through_at + 600;
    bool has_more = cJSON_IsTrue(more);
    int count = cJSON_GetArraySize(data);
    valid = valid && count <= (int)RESET_HISTORY_PAGE_LIMIT &&
            state->item_count + (unsigned)count <= RESET_HISTORY_PAGE_LIMIT * RESET_HISTORY_MAX_PAGES &&
            (has_more ? (count > 0 && cJSON_IsString(cursor) &&
                         reset_history_cursor_valid(cursor->valuestring)) : cJSON_IsNull(cursor));
    uint64_t cursor_hash = 0;
    if (valid && has_more) {
        cursor_hash = history_hash(cursor->valuestring);
        for (unsigned i = 0; i < state->pages; ++i) {
            if (state->cursors[i] == cursor_hash) valid = false;
        }
        /* A page limit is an incomplete result, never an empty calendar. */
        if (state->pages + 1 >= RESET_HISTORY_MAX_PAGES) valid = false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, data) {
        if (!valid) break;
        reset_announcement_t announcement = {0};
        valid = parse_latest(item, &announcement) && announcement.present &&
                announcement.announced_at >= state->data.start_day &&
                announcement.announced_at <= state->data.through_at &&
                announcement.announced_at <= generated_at + 300 &&
                announcement.announced_at >= state->last_announced_at;
        if (!valid) break;
        uint64_t hash = history_hash(announcement.id);
        for (unsigned i = 0; i < state->item_count; ++i) {
            if (state->ids[i] == hash) valid = false;
        }
        if (!valid) break;
        state->ids[state->item_count++] = hash;
        state->last_announced_at = announcement.announced_at;
        unsigned day = (unsigned)((announcement.announced_at - state->data.start_day) / INT64_C(86400));
        state->data.cells[day] |= announcement.kind == RESET_KIND_REGULAR ?
                                 RESET_HISTORY_REGULAR : RESET_HISTORY_BANKED;
    }
    if (valid) {
        state->cursors[state->pages++] = cursor_hash;
        state->has_more = has_more;
        if (has_more) memcpy(state->next_cursor, cursor->valuestring, strlen(cursor->valuestring) + 1U);
        else {
            state->next_cursor[0] = '\0';
            for (unsigned i = 0; i < RESET_HISTORY_DAYS; ++i) {
                if (state->data.start_day + (int64_t)i * INT64_C(86400) <= state->data.through_at) {
                    state->data.cells[i] |= RESET_HISTORY_KNOWN;
                }
            }
            state->complete = reset_history_data_valid(&state->data);
            valid = state->complete;
        }
    }
    cJSON_Delete(root);
    if (!valid) { state->failed = true; state->complete = false; }
    return valid;
}

bool reset_feed_parse_retry_after(const char *text, uint32_t *seconds)
{
    if (!text || !*text || !seconds) return false;
    uint32_t value = 0;
    for (size_t i = 0; text[i]; ++i) {
        if (i >= 10 || text[i] < '0' || text[i] > '9') return false;
        uint32_t digit = (uint32_t)(text[i] - '0');
        if (value > (UINT32_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *seconds = value;
    return true;
}

static uint64_t add_ms(uint64_t now, uint64_t delay)
{
    return UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
}

static uint64_t seconds_to_ms(uint64_t seconds)
{
    return seconds > UINT64_MAX / 1000 ? UINT64_MAX : seconds * 1000;
}

bool reset_feed_parse_retry_after_at(const char *text, int64_t wall_now,
                                     uint64_t *seconds)
{
    if (!text || !seconds || wall_now < 0) return false;
    while (*text == ' ' || *text == '\t') ++text;
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) --length;
    if (!length) return false;
    uint64_t value = 0;
    size_t i = 0;
    for (; i < length && text[i] >= '0' && text[i] <= '9'; ++i) {
        unsigned digit = (unsigned)(text[i] - '0');
        value = value > (UINT64_MAX - digit) / 10 ? UINT64_MAX : value * 10 + digit;
    }
    if (i == length) {
        *seconds = value;
        return true;
    }
    if (length >= 128) return false;
    char input[128];
    memcpy(input, text, length);
    input[length] = 0;
    char weekday[10] = {0}, month[4] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0, consumed = 0;
    bool short_year = false;
    if (sscanf(input, "%3[A-Za-z], %2d %3[A-Za-z] %4d %2d:%2d:%2d GMT%n",
               weekday, &day, month, &year, &hour, &minute, &second, &consumed) != 7 ||
        consumed != (int)length) {
        consumed = 0;
        if (sscanf(input, "%9[A-Za-z], %2d-%3[A-Za-z]-%2d %2d:%2d:%2d GMT%n",
                   weekday, &day, month, &year, &hour, &minute, &second, &consumed) == 7 &&
            consumed == (int)length) {
            short_year = true;
            year += 2000;
        } else {
            consumed = 0;
            if (sscanf(input, "%3[A-Za-z] %3[A-Za-z] %2d %2d:%2d:%2d %4d%n",
                       weekday, month, &day, &hour, &minute, &second, &year, &consumed) != 7 ||
                consumed != (int)length) return false;
        }
    }
    static const char *weekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun",
        "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    bool weekday_valid = false;
    for (size_t w = 0; w < sizeof(weekdays) / sizeof(weekdays[0]); ++w) {
        if (strcmp(weekday, weekdays[w]) == 0) weekday_valid = true;
    }
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month_number = 0;
    for (int m = 0; m < 12; ++m) if (strcmp(month, months[m]) == 0) month_number = m + 1;
    if (!weekday_valid || !month_number || year < 1970 || year > 9999) return false;
    char iso[40];
    snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             year, month_number, day, hour, minute, second);
    int64_t target;
    if (!reset_feed_parse_timestamp(iso, &target)) return false;
    /* RFC 9110: an obsolete two-digit year >50 years ahead means the most
     * recent past year with the same final two digits. */
    if (short_year && target > wall_now && target - wall_now > INT64_C(1577880000)) {
        year -= 100;
        if (year < 1970) { *seconds = 0; return true; }
        snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 year, month_number, day, hour, minute, second);
        if (!reset_feed_parse_timestamp(iso, &target)) return false;
    }
    *seconds = target > wall_now ? (uint64_t)(target - wall_now) : 0;
    return true;
}

bool reset_feed_interval_valid(uint32_t minutes)
{
    return minutes == 5 || minutes == 15 || minutes == 30 || minutes == 60;
}

static uint64_t automatic_interval_ms(const reset_feed_poll_t *poll)
{
    uint32_t minutes = reset_feed_interval_valid(poll->auto_interval_minutes) ?
                       poll->auto_interval_minutes : RESET_FEED_DEFAULT_INTERVAL_MINUTES;
    return (uint64_t)minutes * 60000;
}

bool reset_feed_poll_manual_due(const reset_feed_poll_t *poll, uint64_t now_ms)
{
    return poll && now_ms >= poll->hard_backoff_until_ms && now_ms >= poll->debounce_until_ms;
}

bool reset_feed_poll_due(const reset_feed_poll_t *poll, uint64_t now_ms)
{
    return reset_feed_poll_manual_due(poll, now_ms) && now_ms >= poll->next_attempt_ms;
}

void reset_feed_poll_begin(reset_feed_poll_t *poll, uint64_t now_ms)
{
    if (poll) poll->debounce_until_ms = add_ms(now_ms, RESET_FEED_MANUAL_DEBOUNCE_MS);
}

void reset_feed_poll_complete(reset_feed_poll_t *poll, uint64_t now_ms,
                              bool success, uint64_t retry_after_seconds)
{
    if (!poll) return;
    uint64_t delay = automatic_interval_ms(poll);
    if (success) {
        poll->failures = 0;
        poll->hard_backoff_until_ms = 0;
    } else {
        if (poll->failures < 5) ++poll->failures;
        uint64_t backoff = RESET_FEED_FAILURE_BACKOFF_MS << (poll->failures - 1);
        if (backoff > UINT64_C(3600000)) backoff = UINT64_C(3600000);
        uint64_t server_delay = seconds_to_ms(retry_after_seconds);
        if (server_delay > backoff) backoff = server_delay;
        poll->hard_backoff_until_ms = add_ms(now_ms, backoff);
        if (backoff > delay) delay = backoff;
    }
    poll->next_attempt_ms = add_ms(now_ms, delay);
}

#ifdef ESP_PLATFORM
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RESET_HTTP_TIMEOUT_MS 8000
#define RESET_HTTP_TOTAL_MS UINT64_C(30000)
#define RESET_ETAG_CAPACITY 128

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#error "The reset feed requires TLS certificate bundle verification"
#endif

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_worker;
static bool s_starting;
static bool s_wifi_ready;
static bool s_clock_ready;
static bool s_fetching;
static bool s_paused;
static bool s_refresh_pending;
static bool s_history_pending;
static bool s_history_fetching;
static bool s_history_needs_verification;
static reset_history_snapshot_t s_history;
/* One worker owns these bounded work buffers; never on its TLS call stack. */
static reset_history_accumulator_t s_history_staging;
static char s_history_url[RESET_HISTORY_URL_CAPACITY];
static bool s_cache_needs_verification;
static bool s_restore_pending;
static int64_t s_restore_hard_backoff_at;
static int64_t s_restore_next_auto_at;
static reset_feed_snapshot_t s_snapshot;
static reset_feed_poll_t s_poll;
static uint64_t s_last_checked_ms;
/* Only the worker uses the ETag, except pre-start RTC import clearing it. */
static char s_etag[RESET_ETAG_CAPACITY];

typedef struct {
    char etag[RESET_ETAG_CAPACITY];
    uint64_t retry_after;
    bool json_content;
    bool unsupported_encoding;
} response_headers_t;

typedef struct {
    reset_feed_error_t error;
    int http_status;
    bool unchanged;
    reset_feed_data_t data;
    response_headers_t headers;
} response_t;

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static bool header_etag(char *out, const char *value)
{
    size_t i = 0;
    for (; value[i]; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (i + 1 >= RESET_ETAG_CAPACITY || c < 0x21 || c > 0x7e) return false;
    }
    if (i == 0) return false;
    memcpy(out, value, i + 1);
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_headers_t *headers = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_HEADER || !headers ||
        !event->header_key || !event->header_value) return ESP_OK;
    const char *value = event->header_value;
    if (strcasecmp(event->header_key, "ETag") == 0) {
        headers->etag[0] = '\0';
        (void)header_etag(headers->etag, value);
    } else if (strcasecmp(event->header_key, "Retry-After") == 0) {
        uint64_t retry = 0;
        if (reset_feed_parse_retry_after_at(value, (int64_t)time(NULL), &retry) &&
            retry > headers->retry_after) headers->retry_after = retry;
    } else if (strcasecmp(event->header_key, "Content-Type") == 0) {
        size_t n = strlen("application/json");
        headers->json_content = strncasecmp(value, "application/json", n) == 0 &&
                                (value[n] == '\0' || value[n] == ';' || value[n] == ' ');
    } else if (strcasecmp(event->header_key, "Content-Encoding") == 0) {
        headers->unsupported_encoding = strcasecmp(value, "identity") != 0;
    }
    return ESP_OK;
}

static bool transport_ready(void)
{
    bool ready;
    taskENTER_CRITICAL(&s_lock);
    ready = s_wifi_ready && s_clock_ready && !s_paused;
    taskEXIT_CRITICAL(&s_lock);
    /* A sanity check complements, but never replaces, an actual SNTP sync. */
    return ready && (int64_t)time(NULL) >= INT64_C(1704067200);
}

static int request_timeout(uint64_t started)
{
    uint64_t now = monotonic_ms();
    if (now < started || now - started >= RESET_HTTP_TOTAL_MS || !transport_ready()) return 0;
    uint64_t remaining = RESET_HTTP_TOTAL_MS - (now - started);
    return remaining < RESET_HTTP_TIMEOUT_MS ? (int)remaining : RESET_HTTP_TIMEOUT_MS;
}

/* One client/TLS allocation at a time, shared by status and history. Every
 * history page receives the SAME cycle start, bounding the whole cycle to30s
 * rather than granting a fresh30s budget for each cursor. */
static void fetch_http(response_t *response, const char *url, bool conditional,
                       reset_history_accumulator_t *history, uint64_t started)
{
    memset(response, 0, sizeof(*response));
    response->error = RESET_FEED_ERROR_NETWORK;
    int timeout = request_timeout(started);
    if (!timeout) return;
    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = timeout,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .event_handler = http_event,
        .user_data = &response->headers,
        .user_agent = "AI-Passport-Reset-Observer/2",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        response->error = RESET_FEED_ERROR_MEMORY;
        return;
    }
    char *body = NULL;
    if (esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK ||
        esp_http_client_set_header(client, "Accept-Encoding", "identity") != ESP_OK ||
        (conditional && s_etag[0] && esp_http_client_set_header(client, "If-None-Match", s_etag) != ESP_OK)) goto cleanup;
    timeout = request_timeout(started);
    if (!timeout || esp_http_client_set_timeout_ms(client, timeout) != ESP_OK ||
        esp_http_client_open(client, 0) != ESP_OK) goto cleanup;
    timeout = request_timeout(started);
    if (!timeout || esp_http_client_set_timeout_ms(client, timeout) != ESP_OK) goto cleanup;
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) goto cleanup;
    response->http_status = esp_http_client_get_status_code(client);
    if (response->http_status == 304) {
        if (conditional && s_etag[0]) {
            response->unchanged = true;
            response->error = RESET_FEED_ERROR_NONE;
        } else {
            response->error = RESET_FEED_ERROR_HTTP;
        }
        goto cleanup;
    }
    if (response->http_status != 200) {
        response->error = response->http_status == 429 ?
                          RESET_FEED_ERROR_RATE_LIMIT : RESET_FEED_ERROR_HTTP;
        goto cleanup;
    }
    if (content_length > RESET_FEED_BODY_LIMIT) {
        response->error = RESET_FEED_ERROR_TOO_LARGE;
        goto cleanup;
    }
    if (!response->headers.json_content || response->headers.unsupported_encoding) {
        response->error = RESET_FEED_ERROR_FORMAT;
        goto cleanup;
    }
    body = malloc(RESET_FEED_BODY_LIMIT + 1U);
    if (!body) {
        response->error = RESET_FEED_ERROR_MEMORY;
        goto cleanup;
    }
    size_t used = 0;
    /* fetch_headers may already have cached the whole response. Always drain
     * read(), even when the parser reports that all wire bytes have arrived. */
    for (;;) {
        timeout = request_timeout(started);
        if (!timeout || esp_http_client_set_timeout_ms(client, timeout) != ESP_OK) goto cleanup;
        /* The extra byte detects oversize bodies even with no Content-Length. */
        size_t room = RESET_FEED_BODY_LIMIT + 1U - used;
        int count = esp_http_client_read(client, body + used, room < 1024 ? (int)room : 1024);
        if (count < 0) goto cleanup;
        if (count == 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            goto cleanup;
        }
        used += (size_t)count;
        if (used > RESET_FEED_BODY_LIMIT) {
            response->error = RESET_FEED_ERROR_TOO_LARGE;
            goto cleanup;
        }
    }
    body[used] = '\0';
    /* Release TLS and transport buffers before cJSON allocates nodes. Retain
     * only the bounded response body and already-copied header values. */
    esp_http_client_cleanup(client);
    client = NULL;
    if (!request_timeout(started)) goto cleanup;
    bool parsed = history ? reset_history_parse_page(history, body, used) :
                            reset_feed_parse(body, used, &response->data);
    response->error = parsed ? RESET_FEED_ERROR_NONE : RESET_FEED_ERROR_FORMAT;
    if (!history && response->error == RESET_FEED_ERROR_NONE &&
        response->data.generated_at > (int64_t)time(NULL) + 600) {
        response->error = RESET_FEED_ERROR_FORMAT;
    }
cleanup:
    free(body);
    if (client) esp_http_client_cleanup(client);
}

static void fetch_status(response_t *response)
{
    fetch_http(response, RESET_FEED_URL, true, NULL, monotonic_ms());
}

static void fetch_history(response_t *response)
{
    uint64_t started = monotonic_ms();
    memset(response, 0, sizeof(*response));
    response->error = RESET_FEED_ERROR_FORMAT;
    if (!reset_history_begin(&s_history_staging, (int64_t)time(NULL))) return;
    do {
        const char *cursor = s_history_staging.pages ? s_history_staging.next_cursor : NULL;
        if (!reset_history_build_url(&s_history_staging.data, cursor,
                                     s_history_url, sizeof(s_history_url))) {
            response->error = RESET_FEED_ERROR_FORMAT;
            return;
        }
        fetch_http(response, s_history_url, false, &s_history_staging, started);
        if (response->error != RESET_FEED_ERROR_NONE) return;
    } while (s_history_staging.has_more);
    if (!s_history_staging.complete || !request_timeout(started)) {
        response->error = RESET_FEED_ERROR_NETWORK;
    }
}

static void apply_response(const response_t *response, uint64_t now_ms, int64_t wall_now)
{
    taskENTER_CRITICAL(&s_lock);
    bool success = response->error == RESET_FEED_ERROR_NONE &&
                   (!response->unchanged || s_snapshot.has_data);
    if (success) {
        /* Never associate an invalid payload's ETag with good cache. */
        if (!response->unchanged || response->headers.etag[0]) {
            memcpy(s_etag, response->headers.etag, sizeof(s_etag));
        }
        if (!response->unchanged) s_snapshot.data = response->data;
        s_snapshot.has_data = true;
        s_snapshot.last_checked_at = wall_now;
        s_last_checked_ms = now_ms;
        s_cache_needs_verification = false;
    }
    s_snapshot.error = success ? RESET_FEED_ERROR_NONE :
                       response->error == RESET_FEED_ERROR_NONE ? RESET_FEED_ERROR_HTTP : response->error;
    s_snapshot.http_status = response->http_status;
    s_fetching = false;
    s_refresh_pending = false;
    reset_feed_poll_complete(&s_poll, now_ms, success, response->headers.retry_after);
    taskEXIT_CRITICAL(&s_lock);
}

static void apply_history_response(const response_t *response, uint64_t now_ms, int64_t wall_now)
{
    taskENTER_CRITICAL(&s_lock);
    bool success = response->error == RESET_FEED_ERROR_NONE &&
                   s_history_staging.complete && !s_history_staging.failed &&
                   s_wifi_ready && s_clock_ready && !s_paused;
    if (success) {
        s_history.data = s_history_staging.data;
        s_history.has_data = true;
        s_history.last_checked_at = wall_now;
        s_history_needs_verification = false;
    } else {
        s_history_needs_verification = true;
    }
    s_history.error = success ? RESET_FEED_ERROR_NONE :
                      response->error == RESET_FEED_ERROR_NONE ? RESET_FEED_ERROR_NETWORK : response->error;
    s_history.http_status = response->http_status;
    s_history_fetching = false;
    s_fetching = false;
    /* A history success must not postpone an already-due status check. Errors
     * and 429 share the same global backoff across both endpoints. */
    uint64_t status_deadline = s_poll.next_attempt_ms;
    reset_feed_poll_complete(&s_poll, now_ms, success, response->headers.retry_after);
    if (success) s_poll.next_attempt_ms = status_deadline;
    taskEXIT_CRITICAL(&s_lock);
}

static bool history_fresh_locked(int64_t wall_now)
{
    return s_history.has_data && !s_history_needs_verification &&
           s_history.error == RESET_FEED_ERROR_NONE &&
           wall_now >= s_history.last_checked_at &&
           wall_now - s_history.last_checked_at < RESET_HISTORY_FRESH_SECONDS &&
           s_history.data.start_day == reset_history_window_start(wall_now) &&
           s_history.data.through_at / INT64_C(86400) == wall_now / INT64_C(86400) &&
           (!s_snapshot.has_data || !s_snapshot.data.latest.present ||
            s_snapshot.data.latest.announced_at <= s_history.data.through_at);
}

static uint64_t remaining_ms(uint64_t deadline, uint64_t now)
{
    return deadline > now ? deadline - now : 0;
}

static uint64_t rounded_seconds(uint64_t milliseconds)
{
    return milliseconds / 1000 + (milliseconds % 1000 != 0);
}

static int64_t epoch_after_ms(int64_t now, uint64_t milliseconds)
{
    uint64_t seconds = rounded_seconds(milliseconds);
    if (now < 0 || seconds > (uint64_t)(INT64_MAX - now)) return INT64_MAX;
    return now + (int64_t)seconds;
}

static uint64_t epoch_deadline_ms(int64_t deadline, int64_t wall_now, uint64_t now)
{
    return deadline > wall_now ? add_ms(now, seconds_to_ms((uint64_t)(deadline - wall_now))) : now;
}

/* Called with s_lock held. Never compare RTC policy with an unsynchronized
 * clock. A fresh boot's monotonic timer cannot stand in for elapsed sleep. */
static void restore_policy_locked(uint64_t now, int64_t wall_now)
{
    if (!s_restore_pending || !s_clock_ready || wall_now < INT64_C(1704067200)) return;
    s_poll.hard_backoff_until_ms = epoch_deadline_ms(s_restore_hard_backoff_at, wall_now, now);
    int64_t debounce_at = s_snapshot.last_attempt_at ?
                         epoch_after_ms(s_snapshot.last_attempt_at, RESET_FEED_MANUAL_DEBOUNCE_MS) : 0;
    s_poll.debounce_until_ms = epoch_deadline_ms(debounce_at, wall_now, now);
    s_poll.next_attempt_ms = epoch_deadline_ms(s_restore_next_auto_at, wall_now, now);
    s_restore_pending = false;
}

static reset_feed_block_t refresh_block_locked(uint64_t now, int64_t wall_now)
{
    if (s_paused) return RESET_FEED_BLOCK_PAUSED;
    if (!s_worker) return RESET_FEED_BLOCK_NOT_STARTED;
    if (!s_wifi_ready) return RESET_FEED_BLOCK_WIFI;
    if (!s_clock_ready || wall_now < INT64_C(1704067200) || s_restore_pending) {
        return RESET_FEED_BLOCK_CLOCK;
    }
    if (s_fetching) return RESET_FEED_BLOCK_BUSY;
    if (now < s_poll.hard_backoff_until_ms) return RESET_FEED_BLOCK_BACKOFF;
    if (now < s_poll.debounce_until_ms) return RESET_FEED_BLOCK_DEBOUNCE;
    if (s_refresh_pending) return RESET_FEED_BLOCK_BUSY;
    return RESET_FEED_BLOCK_NONE;
}

/* Admission and pause share the lock: once pause returns success there cannot
 * be a late request entering HTTP. Tests exercise this exact admission path. */
static bool admit_fetch(uint64_t now, int64_t wall_now)
{
    bool due = false;
    taskENTER_CRITICAL(&s_lock);
    restore_policy_locked(now, wall_now);
    if (!s_paused && !s_fetching && s_wifi_ready && s_clock_ready &&
        wall_now >= INT64_C(1704067200) && !s_restore_pending &&
        (s_refresh_pending ? reset_feed_poll_manual_due(&s_poll, now) :
                             reset_feed_poll_due(&s_poll, now))) {
        s_fetching = true;
        s_refresh_pending = false;
        s_snapshot.last_attempt_at = wall_now;
        reset_feed_poll_begin(&s_poll, now);
        due = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return due;
}

static bool admit_history(uint64_t now, int64_t wall_now)
{
    bool due = false;
    taskENTER_CRITICAL(&s_lock);
    restore_policy_locked(now, wall_now);
    if (s_history_pending && !s_paused && !s_fetching && s_wifi_ready && s_clock_ready &&
        wall_now >= INT64_C(1704067200) && !s_restore_pending &&
        reset_feed_poll_manual_due(&s_poll, now)) {
        s_history_pending = false;
        if (!history_fresh_locked(wall_now)) {
            s_fetching = true;
            s_history_fetching = true;
            /* This global attempt timestamp also preserves debounce in RTC. */
            s_snapshot.last_attempt_at = wall_now;
            reset_feed_poll_begin(&s_poll, now);
            due = true;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return due;
}

static void feed_worker(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t now = monotonic_ms();
        int64_t wall_now = (int64_t)time(NULL);
        if (admit_history(now, wall_now)) {
            response_t response;
            fetch_history(&response);
            apply_history_response(&response, monotonic_ms(), (int64_t)time(NULL));
        } else if (admit_fetch(now, wall_now)) {
            response_t response;
            fetch_status(&response);
            now = monotonic_ms();
            wall_now = (int64_t)time(NULL);
            apply_response(&response, now, wall_now);
        }
        now = monotonic_ms();
        taskENTER_CRITICAL(&s_lock);
        bool ready = s_wifi_ready && s_clock_ready && !s_paused;
        uint64_t deadline = s_refresh_pending || s_history_pending ? 0 : s_poll.next_attempt_ms;
        if (s_poll.hard_backoff_until_ms > deadline) deadline = s_poll.hard_backoff_until_ms;
        if (s_poll.debounce_until_ms > deadline) deadline = s_poll.debounce_until_ms;
        uint64_t delay = deadline > now ? deadline - now : 1000;
        taskEXIT_CRITICAL(&s_lock);
        /* Paused/disconnected means no periodic wakeups. Wake/manual requests
         * may bypass only the automatic interval, never the two hard floors. */
        if (delay > UINT64_C(3600000)) delay = UINT64_C(3600000);
        ulTaskNotifyTake(pdTRUE, ready ? pdMS_TO_TICKS((uint32_t)delay) : portMAX_DELAY);
    }
}

esp_err_t reset_feed_start(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_worker || s_starting) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_starting = true;
    taskEXIT_CRITICAL(&s_lock);
    TaskHandle_t worker = NULL;
    /* Match IDF's HTTPS-client example budget for TLS certificate validation. */
    BaseType_t result = xTaskCreate(feed_worker, "reset_feed", 8192, NULL, 4, &worker);
    taskENTER_CRITICAL(&s_lock);
    s_starting = false;
    if (result == pdPASS) s_worker = worker;
    else s_snapshot.error = RESET_FEED_ERROR_MEMORY;
    taskEXIT_CRITICAL(&s_lock);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void reset_feed_set_ready(bool wifi_connected, bool time_synchronized)
{
    TaskHandle_t worker;
    bool changed;
    uint64_t now = monotonic_ms();
    int64_t wall_now = (int64_t)time(NULL);
    taskENTER_CRITICAL(&s_lock);
    changed = s_wifi_ready != wifi_connected || s_clock_ready != time_synchronized;
    s_wifi_ready = wifi_connected;
    s_clock_ready = time_synchronized;
    restore_policy_locked(now, wall_now);
    worker = s_worker;
    taskEXIT_CRITICAL(&s_lock);
    if (worker && changed) xTaskNotifyGive(worker);
}

bool reset_feed_request_refresh(void)
{
    uint64_t now = monotonic_ms();
    int64_t wall_now = (int64_t)time(NULL);
    TaskHandle_t worker;
    bool allowed;
    taskENTER_CRITICAL(&s_lock);
    restore_policy_locked(now, wall_now);
    worker = s_worker;
    allowed = refresh_block_locked(now, wall_now) == RESET_FEED_BLOCK_NONE;
    if (allowed) s_refresh_pending = true;
    taskEXIT_CRITICAL(&s_lock);
    if (allowed) xTaskNotifyGive(worker);
    return allowed;
}

bool reset_feed_request_history(void)
{
    TaskHandle_t worker;
    bool allowed;
    int64_t wall_now = (int64_t)time(NULL);
    taskENTER_CRITICAL(&s_lock);
    worker = s_worker;
    allowed = worker && !s_paused;
    if (allowed && !s_history_fetching && !history_fresh_locked(wall_now)) s_history_pending = true;
    taskEXIT_CRITICAL(&s_lock);
    if (allowed) xTaskNotifyGive(worker);
    return allowed;
}

static uint32_t bounded_seconds(uint64_t milliseconds)
{
    uint64_t seconds = rounded_seconds(milliseconds);
    return seconds > UINT32_MAX ? UINT32_MAX : (uint32_t)seconds;
}

void reset_feed_get_history_snapshot(reset_history_snapshot_t *out)
{
    if (!out) return;
    uint64_t now = monotonic_ms();
    int64_t wall_now = (int64_t)time(NULL);
    taskENTER_CRITICAL(&s_lock);
    restore_policy_locked(now, wall_now);
    *out = s_history;
    reset_history_project(s_history.has_data ? &s_history.data : NULL, wall_now, &out->data);
    out->stale = !history_fresh_locked(wall_now);
    out->loading = s_history_pending || s_history_fetching;
    if (s_paused) out->status = RESET_FEED_PAUSED;
    else if (!s_wifi_ready) out->status = RESET_FEED_WAITING_WIFI;
    else if (!s_clock_ready || wall_now < INT64_C(1704067200)) out->status = RESET_FEED_WAITING_CLOCK;
    else if (out->loading) out->status = RESET_FEED_FETCHING;
    else if (s_history.error != RESET_FEED_ERROR_NONE) out->status = RESET_FEED_ERROR;
    else out->status = RESET_FEED_CURRENT;
    uint64_t deadline = s_poll.hard_backoff_until_ms > s_poll.debounce_until_ms ?
                        s_poll.hard_backoff_until_ms : s_poll.debounce_until_ms;
    out->retry_in_seconds = bounded_seconds(remaining_ms(deadline, now));
    taskEXIT_CRITICAL(&s_lock);
}

void reset_feed_get_snapshot(reset_feed_snapshot_t *out)
{
    if (!out) return;
    uint64_t now = monotonic_ms();
    int64_t wall_now = (int64_t)time(NULL);
    taskENTER_CRITICAL(&s_lock);
    restore_policy_locked(now, wall_now);
    *out = s_snapshot;
    if (s_paused) out->status = RESET_FEED_PAUSED;
    else if (!s_wifi_ready) out->status = RESET_FEED_WAITING_WIFI;
    else if (!s_clock_ready || wall_now < INT64_C(1704067200)) out->status = RESET_FEED_WAITING_CLOCK;
    else if (s_fetching || (!s_snapshot.has_data && s_snapshot.error == RESET_FEED_ERROR_NONE)) {
        out->status = RESET_FEED_FETCHING;
    } else if (s_snapshot.error != RESET_FEED_ERROR_NONE) out->status = RESET_FEED_ERROR;
    else out->status = RESET_FEED_CURRENT;
    out->stale = !out->has_data || s_cache_needs_verification || now < s_last_checked_ms ||
                 now - s_last_checked_ms > RESET_FEED_STALE_AFTER_MS;
    out->refresh_block = refresh_block_locked(now, wall_now);
    out->can_refresh = out->refresh_block == RESET_FEED_BLOCK_NONE;
    uint64_t retry_deadline = s_poll.hard_backoff_until_ms > s_poll.debounce_until_ms ?
                              s_poll.hard_backoff_until_ms : s_poll.debounce_until_ms;
    uint64_t auto_deadline = s_poll.next_attempt_ms > retry_deadline ?
                             s_poll.next_attempt_ms : retry_deadline;
    uint64_t retry_wait = remaining_ms(retry_deadline, now);
    uint64_t auto_wait = remaining_ms(auto_deadline, now);
    out->retry_in_seconds = bounded_seconds(retry_wait);
    out->next_auto_in_seconds = bounded_seconds(auto_wait);
    out->retry_at = retry_wait && s_clock_ready ? epoch_after_ms(wall_now, retry_wait) : 0;
    out->next_auto_at = s_clock_ready ? epoch_after_ms(wall_now, auto_wait) : 0;
    if (s_restore_pending) {
        out->retry_at = s_restore_hard_backoff_at;
        out->next_auto_at = s_restore_next_auto_at;
    }
    out->auto_interval_minutes = (uint32_t)(automatic_interval_ms(&s_poll) / 60000);
    taskEXIT_CRITICAL(&s_lock);
}

bool reset_feed_set_auto_interval(uint32_t minutes)
{
    if (!reset_feed_interval_valid(minutes)) return false;
    uint64_t now = monotonic_ms();
    TaskHandle_t worker;
    taskENTER_CRITICAL(&s_lock);
    if (s_poll.auto_interval_minutes != minutes) {
        s_poll.auto_interval_minutes = minutes;
        /* A setting change is not a fetch. Retain first-start immediacy, but
         * otherwise count the new interval from the change. Hard floors stay. */
        if (s_snapshot.last_attempt_at) s_poll.next_attempt_ms = add_ms(now, automatic_interval_ms(&s_poll));
        if (s_restore_pending && s_snapshot.last_attempt_at) {
            s_restore_next_auto_at = epoch_after_ms(s_snapshot.last_attempt_at, automatic_interval_ms(&s_poll));
        }
    }
    worker = s_worker;
    taskEXIT_CRITICAL(&s_lock);
    if (worker) xTaskNotifyGive(worker);
    return true;
}

bool reset_feed_pause_and_wait(uint32_t timeout_ms)
{
    uint64_t started = monotonic_ms();
    TaskHandle_t worker;
    taskENTER_CRITICAL(&s_lock);
    s_paused = true;
    s_refresh_pending = false;
    s_history_pending = false;
    worker = s_worker;
    taskEXIT_CRITICAL(&s_lock);
    if (worker) xTaskNotifyGive(worker);
    for (;;) {
        taskENTER_CRITICAL(&s_lock);
        bool idle = !s_fetching && !s_starting;
        taskEXIT_CRITICAL(&s_lock);
        if (idle) return true;
        uint64_t elapsed = monotonic_ms() - started;
        if (elapsed >= timeout_ms) return false;
        uint32_t delay = (uint32_t)(timeout_ms - elapsed);
        if (delay > 10) delay = 10;
        TickType_t ticks = pdMS_TO_TICKS(delay);
        vTaskDelay(ticks ? ticks : 1);
    }
}

void reset_feed_resume(bool refresh_now)
{
    TaskHandle_t worker;
    taskENTER_CRITICAL(&s_lock);
    s_paused = false;
    if (refresh_now) s_refresh_pending = true;
    worker = s_worker;
    taskEXIT_CRITICAL(&s_lock);
    if (worker) xTaskNotifyGive(worker);
}

#define RESET_RTC_MAGIC UINT32_C(0x52534632)
/* Version 5 retains the independently validated complete history cache.
 * Both this version and sizeof check reject older cache layouts on wake. */
#define RESET_RTC_VERSION 5U

static uint32_t rtc_checksum(const reset_feed_rtc_state_t *state)
{
    const unsigned char *bytes = (const unsigned char *)state;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < sizeof(*state); ++i) {
        if (i >= offsetof(reset_feed_rtc_state_t, checksum) &&
            i < offsetof(reset_feed_rtc_state_t, checksum) + sizeof(state->checksum)) continue;
        hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    }
    return hash;
}

static bool valid_rtc_text(const char *text, size_t capacity)
{
    const char *end = memchr(text, '\0', capacity);
    if (!end || !valid_utf8(text, (size_t)(end - text))) return false;
    for (const char *p = text; p < end; ++p) {
        unsigned char c = (unsigned char)*p;
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f) return false;
    }
    return true;
}

static bool valid_rtc_source_url(const char *url, size_t capacity, bool observed)
{
    if (!memchr(url, '\0', capacity)) return false;
    return url[0] == '\0' || (!observed && reset_feed_source_url_valid(url, capacity));
}

bool reset_feed_export_rtc(reset_feed_rtc_state_t *out)
{
    if (!out) return false;
    uint64_t now = monotonic_ms();
    int64_t wall_now = (int64_t)time(NULL);
    reset_feed_rtc_state_t state = {0};
    taskENTER_CRITICAL(&s_lock);
    if (!s_paused || s_fetching || s_starting) {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    state.magic = RESET_RTC_MAGIC;
    state.version = RESET_RTC_VERSION;
    state.size = sizeof(state);
    state.data = s_snapshot.data;
    state.has_data = s_snapshot.has_data;
    state.failures = s_poll.failures;
    state.auto_interval_minutes = (uint32_t)(automatic_interval_ms(&s_poll) / 60000);
    state.error = s_snapshot.error;
    state.http_status = s_snapshot.http_status;
    state.last_checked_at = s_snapshot.last_checked_at;
    state.last_attempt_at = s_snapshot.last_attempt_at;
    state.history = s_history.data;
    state.has_history = s_history.has_data;
    state.history_error = s_history.error;
    state.history_http_status = s_history.http_status;
    state.history_last_checked_at = s_history.last_checked_at;
    if (s_restore_pending) {
        state.hard_backoff_until = s_restore_hard_backoff_at;
        state.next_auto_at = s_restore_next_auto_at;
    } else if (wall_now >= INT64_C(1704067200)) {
        uint64_t remaining = remaining_ms(s_poll.hard_backoff_until_ms, now);
        state.hard_backoff_until = remaining ? epoch_after_ms(wall_now, remaining) : 0;
        state.next_auto_at = epoch_after_ms(wall_now, remaining_ms(s_poll.next_attempt_ms, now));
        /* Preserve the fractional-second debounce conservatively and also
         * survive a wall-clock correction since the attempt began. */
        remaining = remaining_ms(s_poll.debounce_until_ms, now);
        if (remaining) {
            int64_t conservative_attempt = epoch_after_ms(wall_now, remaining) -
                                           (int64_t)(RESET_FEED_MANUAL_DEBOUNCE_MS / 1000);
            if (conservative_attempt > state.last_attempt_at) state.last_attempt_at = conservative_attempt;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    state.checksum = rtc_checksum(&state);
    memcpy(out, &state, sizeof(state));
    return true;
}

bool reset_feed_import_rtc(const reset_feed_rtc_state_t *state)
{
    if (!state || state->magic != RESET_RTC_MAGIC || state->version != RESET_RTC_VERSION ||
        state->size != sizeof(*state) || state->checksum != rtc_checksum(state) ||
        state->has_data > 1 || state->has_history > 1 || state->failures > 5 ||
        !reset_feed_interval_valid(state->auto_interval_minutes) ||
        state->error > RESET_FEED_ERROR_FORMAT || state->http_status < 0 || state->http_status > 599 ||
        state->last_checked_at < 0 || state->last_attempt_at < 0 ||
        state->hard_backoff_until < 0 || state->next_auto_at < 0 ||
        state->history_error > RESET_FEED_ERROR_FORMAT ||
        state->history_http_status < 0 || state->history_http_status > 599 ||
        state->history_last_checked_at < 0 ||
        (state->has_history && (!reset_history_data_valid(&state->history) ||
             state->history_last_checked_at < state->history.through_at ||
             state->history_last_checked_at >= INT64_C(4102444800))) ||
        !valid_rtc_text(state->data.latest.text, sizeof(state->data.latest.text)) ||
        !valid_rtc_text(state->data.scheduled.text, sizeof(state->data.scheduled.text)) ||
        !valid_rtc_source_url(state->data.latest.source_url, sizeof(state->data.latest.source_url),
                              state->data.latest.observed) ||
        !valid_rtc_source_url(state->data.scheduled.source_url, sizeof(state->data.scheduled.source_url),
                              state->data.scheduled.observed) ||
        !valid_rtc_text(state->data.watch.forecast_window, sizeof(state->data.watch.forecast_window)) ||
        (state->has_data && (state->last_checked_at == 0 ||
                            state->data.generated_at < INT64_C(1609459200) ||
                            state->data.generated_at >= INT64_C(4102444800)))) return false;
    taskENTER_CRITICAL(&s_lock);
    if (s_worker || s_starting || s_fetching) {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.data = state->data;
    s_snapshot.has_data = state->has_data != 0;
    s_snapshot.error = (reset_feed_error_t)state->error;
    s_snapshot.http_status = state->http_status;
    s_snapshot.last_checked_at = state->last_checked_at;
    s_snapshot.last_attempt_at = state->last_attempt_at;
    memset(&s_history, 0, sizeof(s_history));
    if (state->has_history) s_history.data = state->history;
    s_history.has_data = state->has_history != 0;
    s_history.error = (reset_feed_error_t)state->history_error;
    s_history.http_status = state->history_http_status;
    s_history.last_checked_at = state->history_last_checked_at;
    s_history_needs_verification = state->has_history != 0;
    s_history_pending = false;
    s_history_fetching = false;
    memset(&s_poll, 0, sizeof(s_poll));
    s_poll.failures = state->failures;
    s_poll.auto_interval_minutes = state->auto_interval_minutes;
    s_restore_hard_backoff_at = state->hard_backoff_until;
    s_restore_next_auto_at = state->next_auto_at;
    s_restore_pending = true;
    s_cache_needs_verification = state->has_data != 0;
    s_refresh_pending = true;
    s_etag[0] = 0; /* Cache is semantic only: force full revalidation on wake. */
    taskEXIT_CRITICAL(&s_lock);
    return true;
}
#endif
