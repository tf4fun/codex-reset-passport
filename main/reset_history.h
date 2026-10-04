#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One compile-time choice shared by query, cache and drawing. */
#ifndef RESET_HISTORY_WEEKS
#define RESET_HISTORY_WEEKS 8U
#endif
#if RESET_HISTORY_WEEKS != 6 && RESET_HISTORY_WEEKS != 8
#error "Reset history supports six or eight UTC weeks"
#endif
#define RESET_HISTORY_DAYS (7U * RESET_HISTORY_WEEKS)
#define RESET_HISTORY_REGULAR 0x01U
#define RESET_HISTORY_BANKED 0x02U
#define RESET_HISTORY_KNOWN 0x80U
#define RESET_HISTORY_URL "https://codex-resets.com/api/v1/resets"
#define RESET_HISTORY_PAGE_LIMIT 25U
#define RESET_HISTORY_MAX_PAGES 4U
#define RESET_HISTORY_CURSOR_CAPACITY 1025U
#define RESET_HISTORY_URL_CAPACITY (RESET_HISTORY_CURSOR_CAPACITY + 192U)
/* Lazy on entry/OK, at most one successful verification per six hours. */
#define RESET_HISTORY_FRESH_SECONDS INT64_C(21600)

typedef struct {
    int64_t start_day;  /* Monday UTC midnight: current week minus WEEKS-1. */
    int64_t through_at; /* Exact inclusive UTC query cutoff, never a forecast. */
    /* Sequential days: column=index/7, Monday-first row=index%7.
     * Zero is UNKNOWN. KNOWN alone means no announcement through through_at.
     * REGULAR|BANKED means both kinds occurred; flags are never counts.
     * Only a complete, validated pagination cycle sets KNOWN. */
    uint8_t cells[RESET_HISTORY_DAYS];
} reset_history_data_t;

typedef enum {
    RESET_HISTORY_DAY_UNKNOWN = 0,
    RESET_HISTORY_DAY_EMPTY,
    RESET_HISTORY_DAY_REGULAR,
    RESET_HISTORY_DAY_BANKED,
    RESET_HISTORY_DAY_BOTH,
    RESET_HISTORY_DAY_FUTURE,
} reset_history_day_state_t;

/* Pure UTC helpers, independent of timezone, libc static tm storage and LVGL. */
int64_t reset_history_window_start(int64_t utc_now);
reset_history_day_state_t reset_history_day_state(const reset_history_data_t *data,
                                                 unsigned day, int64_t utc_now);
void reset_history_project(const reset_history_data_t *cached, int64_t utc_now,
                           reset_history_data_t *out);
bool reset_history_data_valid(const reset_history_data_t *data);
bool reset_history_cursor_valid(const char *cursor);
bool reset_history_build_url(const reset_history_data_t *window, const char *cursor,
                             char *out, size_t capacity);

/* Worker-owned staging only, never copied onto a UI stack or published early.
 * Hash matches conservatively reject duplicate IDs/cursors (including loops).
 * Four pages, 100 IDs, and <=16 KiB per page bound all parsing and storage. */
typedef struct {
    reset_history_data_t data;
    uint64_t ids[RESET_HISTORY_PAGE_LIMIT * RESET_HISTORY_MAX_PAGES];
    uint64_t cursors[RESET_HISTORY_MAX_PAGES];
    char next_cursor[RESET_HISTORY_CURSOR_CAPACITY];
    unsigned item_count;
    unsigned pages;
    int64_t last_announced_at;
    bool has_more;
    bool failed;
    bool complete;
} reset_history_accumulator_t;
bool reset_history_begin(reset_history_accumulator_t *state, int64_t utc_now);
/* Implemented beside feed parsing to share bounded JSON/UTF-8 validation.
 * Any failure poisons staging; callers must retain their previous good cache.
 * Do not publish data until complete is true. */
bool reset_history_parse_page(reset_history_accumulator_t *state,
                              const char *json, size_t length);
