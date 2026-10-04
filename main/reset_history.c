#include "reset_history.h"

#include <stdio.h>
#include <string.h>

#define DAY_SECONDS INT64_C(86400)
#define HISTORY_MIN_TIME INT64_C(1609459200)
#define HISTORY_MAX_TIME INT64_C(4102444800)

int64_t reset_history_window_start(int64_t utc_now)
{
    if (utc_now < HISTORY_MIN_TIME || utc_now >= HISTORY_MAX_TIME) return 0;
    int64_t day = utc_now / DAY_SECONDS;
    /* Unix day zero was Thursday; Monday is weekday zero. */
    return (day - (day + 3) % 7 - 7 * (RESET_HISTORY_WEEKS - 1)) * DAY_SECONDS;
}

reset_history_day_state_t reset_history_day_state(const reset_history_data_t *data,
                                                 unsigned day, int64_t utc_now)
{
    if (!data || day >= RESET_HISTORY_DAYS || !reset_history_window_start(utc_now) ||
        data->start_day < 0 || data->start_day >= HISTORY_MAX_TIME) {
        return RESET_HISTORY_DAY_UNKNOWN;
    }
    int64_t at = data->start_day + (int64_t)day * DAY_SECONDS;
    if (at / DAY_SECONDS > utc_now / DAY_SECONDS) return RESET_HISTORY_DAY_FUTURE;
    uint8_t cell = data->cells[day];
    if (!(cell & RESET_HISTORY_KNOWN)) return RESET_HISTORY_DAY_UNKNOWN;
    switch (cell & (RESET_HISTORY_REGULAR | RESET_HISTORY_BANKED)) {
    case RESET_HISTORY_REGULAR: return RESET_HISTORY_DAY_REGULAR;
    case RESET_HISTORY_BANKED: return RESET_HISTORY_DAY_BANKED;
    case RESET_HISTORY_REGULAR | RESET_HISTORY_BANKED: return RESET_HISTORY_DAY_BOTH;
    default: return RESET_HISTORY_DAY_EMPTY;
    }
}

void reset_history_project(const reset_history_data_t *cached, int64_t utc_now,
                           reset_history_data_t *out)
{
    if (!out) return;
    reset_history_data_t projected = {.start_day = reset_history_window_start(utc_now)};
    if (cached && projected.start_day && reset_history_data_valid(cached)) {
        projected.through_at = cached->through_at;
        for (unsigned i = 0; i < RESET_HISTORY_DAYS; ++i) {
            int64_t delta = projected.start_day + (int64_t)i * DAY_SECONDS - cached->start_day;
            if (delta >= 0 && delta < (int64_t)RESET_HISTORY_DAYS * DAY_SECONDS) {
                projected.cells[i] = cached->cells[delta / DAY_SECONDS];
            }
        }
    }
    *out = projected;
}

bool reset_history_data_valid(const reset_history_data_t *data)
{
    if (!data || !data->start_day ||
        data->start_day != reset_history_window_start(data->through_at)) return false;
    for (unsigned i = 0; i < RESET_HISTORY_DAYS; ++i) {
        uint8_t flags = data->cells[i];
        bool known = data->start_day + (int64_t)i * DAY_SECONDS <= data->through_at;
        if (flags & ~(RESET_HISTORY_KNOWN | RESET_HISTORY_REGULAR | RESET_HISTORY_BANKED)) return false;
        if (known != !!(flags & RESET_HISTORY_KNOWN) || (!known && flags)) return false;
    }
    return true;
}

bool reset_history_cursor_valid(const char *cursor)
{
    if (!cursor || !cursor[0]) return false;
    for (size_t i = 0; i < RESET_HISTORY_CURSOR_CAPACITY; ++i) {
        unsigned char c = (unsigned char)cursor[i];
        if (!c) return true;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return false;
}

/* Bounded Gregorian conversion avoids libc's process-global tm entirely. */
static bool encoded_timestamp(int64_t at, char *out, size_t capacity)
{
    if (at < 0 || at >= HISTORY_MAX_TIME) return false;
    int64_t days = at / DAY_SECONDS;
    int year = 1970;
    for (;;) {
        bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        int year_days = leap ? 366 : 365;
        if (days < year_days) break;
        days -= year_days;
        ++year;
    }
    static const unsigned months[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned month = 0;
    for (; month < 11; ++month) {
        unsigned length = months[month] + (month == 1 && year % 4 == 0 &&
                                           (year % 100 != 0 || year % 400 == 0));
        if (days < (int64_t)length) break;
        days -= length;
    }
    unsigned hour = (unsigned)(at % DAY_SECONDS / 3600);
    unsigned minute = (unsigned)(at % 3600 / 60);
    unsigned second = (unsigned)(at % 60);
    int n = snprintf(out, capacity, "%04d-%02u-%02uT%02u%%3A%02u%%3A%02uZ",
                     year, month + 1, (unsigned)days + 1, hour, minute, second);
    return n > 0 && (size_t)n < capacity;
}

bool reset_history_build_url(const reset_history_data_t *window, const char *cursor,
                             char *out, size_t capacity)
{
    if (!window || !out || !capacity || !window->start_day ||
        window->start_day != reset_history_window_start(window->through_at) ||
        (cursor && !reset_history_cursor_valid(cursor))) return false;
    char from[32], to[32];
    if (!encoded_timestamp(window->start_day, from, sizeof(from)) ||
        !encoded_timestamp(window->through_at, to, sizeof(to))) return false;
    /* Validated base64url consists only of URL-unreserved characters. Copy it
     * exactly; no decoding, guessed token, query injection, or truncation. */
    int n = snprintf(out, capacity, RESET_HISTORY_URL "?from=%s&to=%s&limit=%u&order=asc%s%s",
                     from, to, RESET_HISTORY_PAGE_LIMIT, cursor ? "&cursor=" : "", cursor ? cursor : "");
    if (n <= 0 || (size_t)n >= capacity) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool reset_history_begin(reset_history_accumulator_t *state, int64_t utc_now)
{
    if (!state) return false;
    memset(state, 0, sizeof(*state));
    state->data.start_day = reset_history_window_start(utc_now);
    state->data.through_at = utc_now;
    state->has_more = true;
    state->failed = state->data.start_day == 0;
    return !state->failed;
}
