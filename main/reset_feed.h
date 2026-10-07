#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "reset_history.h"
#include "reset_text_limits.h"

#define RESET_FEED_URL "https://codex-resets.com/api/v1/status"
#define RESET_FEED_BODY_LIMIT (16U * 1024U)
#define RESET_FEED_TEXT_MAX_BYTES RESET_TEXT_MAX_BYTES
#define RESET_FEED_FORECAST_MAX_BYTES 96U
#define RESET_FEED_SOURCE_URL_MAX_BYTES 120U
/* Client policy, not a published upstream rate limit. */
#define RESET_FEED_FAILURE_BACKOFF_MS UINT64_C(300000)
#define RESET_FEED_MANUAL_DEBOUNCE_MS UINT64_C(30000)
#define RESET_FEED_DEFAULT_INTERVAL_MINUTES 15U
#define RESET_FEED_STALE_AFTER_MS UINT64_C(900000)

/* This third-party feed describes global announcements, never personal quota. */
typedef enum {
    RESET_KIND_NONE = 0,
    RESET_KIND_REGULAR,
    RESET_KIND_BANKED,
} reset_kind_t;

typedef enum {
    RESET_WATCH_NONE = 0,
    RESET_WATCH_ELEVATED,
    RESET_WATCH_STRONG,
} reset_watch_level_t;

typedef struct {
    bool present;
    bool observed;              /* True: no official announcement was posted. */
    reset_kind_t kind;
    int64_t announced_at;       /* UTC epoch seconds; announcement, not quota. */
    char id[65];
    /* Exact, bounded HTTPS source URL on the source-host allowlist. Empty for
     * observations or missing/unsafe/overlong URLs; never truncate or invent
     * a destination. QR capacity may impose a smaller limit in the UI. */
    char source_url[RESET_FEED_SOURCE_URL_MAX_BYTES + 1U];
    /* Original valid UTF-8, never a translation or executable instruction.
     * Always NUL-terminated at a complete codepoint boundary. At most
     * RESET_FEED_TEXT_MAX_BYTES survive; text_truncated requires an explicit
     * excerpt indicator in the UI.
     * ASCII controls other than LF/CR/TAB reject the response, including DEL.
     * text_has_non_ascii is advisory, NOT a guarantee of font glyph coverage:
     * the UI must check every codepoint, safely substitute missing glyphs and
     * label any fallback. Empty text is valid, distinct from a missing record. */
    char text[RESET_FEED_TEXT_MAX_BYTES + 1U];
    bool text_truncated;
    bool text_has_non_ascii;
} reset_announcement_t;

typedef struct {
    bool present;               /* Still pending even after scheduled_for. */
    reset_kind_t kind;
    int64_t announced_at;
    bool has_time;
    int64_t scheduled_for;
    bool observed;
    char id[65];
    /* Same safe source-URL contract as reset_announcement_t. */
    char source_url[RESET_FEED_SOURCE_URL_MAX_BYTES + 1U];
    /* Same bounded original-text contract as reset_announcement_t. */
    char text[RESET_FEED_TEXT_MAX_BYTES + 1U];
    bool text_truncated;
    bool text_has_non_ascii;
} reset_schedule_t;

typedef struct {
    bool present;               /* AI-classified forecast, not official. */
    reset_watch_level_t level;
    int confidence_percent;    /* -1 means not supplied. */
    int64_t observed_at;
    int64_t expires_at;
    /* Bounded original wording, not a timestamp or confirmed reset time.
     * Same UTF-8 / control / font-fallback contract as announcement text. */
    char forecast_window[RESET_FEED_FORECAST_MAX_BYTES + 1U];
    bool forecast_window_truncated;
    bool forecast_window_has_non_ascii;
} reset_watch_t;

typedef struct {
    uint32_t total;
    bool has_last_reset_at;
    bool has_days_since_last;
    bool has_avg_interval_days;
    int64_t last_reset_at;
    double days_since_last;
    double avg_interval_days;
} reset_stats_t;

typedef struct {
    reset_announcement_t latest;
    reset_schedule_t scheduled;
    reset_watch_t watch;
    reset_stats_t stats;
    int64_t generated_at;
} reset_feed_data_t;

typedef enum {
    RESET_FEED_WAITING_WIFI = 0,
    RESET_FEED_WAITING_CLOCK,
    RESET_FEED_FETCHING,
    RESET_FEED_CURRENT,
    RESET_FEED_ERROR,
    RESET_FEED_PAUSED,
} reset_feed_status_t;

typedef enum {
    RESET_FEED_ERROR_NONE = 0,
    RESET_FEED_ERROR_MEMORY,
    RESET_FEED_ERROR_NETWORK,
    RESET_FEED_ERROR_HTTP,
    RESET_FEED_ERROR_RATE_LIMIT,
    RESET_FEED_ERROR_TOO_LARGE,
    RESET_FEED_ERROR_FORMAT,
} reset_feed_error_t;

typedef enum {
    RESET_FEED_BLOCK_NONE = 0,
    RESET_FEED_BLOCK_NOT_STARTED,
    RESET_FEED_BLOCK_WIFI,
    RESET_FEED_BLOCK_CLOCK,
    RESET_FEED_BLOCK_PAUSED,
    RESET_FEED_BLOCK_BUSY,
    RESET_FEED_BLOCK_BACKOFF,
    RESET_FEED_BLOCK_DEBOUNCE,
} reset_feed_block_t;

typedef struct {
    /* Borrowed current cache. Firmware readers must hold lock_snapshot until
     * they finish accessing it; pin_reader keeps its revision while reading. */
    const reset_feed_data_t *data;
    uint32_t revision;
    bool has_data;
    bool stale;                 /* Last verification older than 15 minutes. */
    reset_feed_status_t status;
    reset_feed_error_t error;
    int http_status;
    int64_t last_checked_at;    /* Last successful 200 or 304 verification. */
    int64_t last_attempt_at;    /* Includes failures. */
    bool can_refresh;
    reset_feed_block_t refresh_block;
    int64_t retry_at;           /* UTC hard-backoff/debounce deadline, or 0. */
    uint32_t retry_in_seconds;  /* Manual/wake wait, excludes auto interval. */
    int64_t next_auto_at;       /* UTC deadline, 0 until a synchronized clock. */
    uint32_t next_auto_in_seconds;
    uint32_t auto_interval_minutes;
} reset_feed_snapshot_t;

typedef struct {
    /* Getter projects the last complete cache into the current UTC window.
     * Missing days remain UNKNOWN, even when has_data is true. Future days
     * use reset_history_day_state(), never infer empty from a zero mask. */
    reset_history_data_t data;
    bool has_data;
    bool stale;
    bool loading;
    reset_feed_status_t status;
    reset_feed_error_t error;
    int http_status;
    int64_t last_checked_at;
    uint32_t retry_in_seconds;
} reset_history_snapshot_t;

/* Pure bounded parser: failure leaves *out unchanged. Requires valid UTF-8
 * JSON; required text must be a string (empty is allowed, null is not).
 * Only validated values and bounded source text survive. Check font coverage;
 * render as plain data with markup/recolor disabled, never as a format string. */
bool reset_feed_parse(const char *json, size_t length, reset_feed_data_t *out);
/* Validate within caller-provided storage, including its terminating NUL.
 * Empty, unterminated, non-ASCII, non-HTTPS, and non-allowlisted URLs fail. */
bool reset_feed_source_url_valid(const char *url, size_t capacity);
bool reset_feed_parse_timestamp(const char *text, int64_t *epoch_seconds);
bool reset_feed_parse_retry_after(const char *text, uint32_t *seconds);
/* Accept delta-seconds and all three HTTP-date forms; overflow is clamped to
 * UINT64_MAX (effectively never retry), never converted into a short delay. */
bool reset_feed_parse_retry_after_at(const char *text, int64_t wall_now,
                                     uint64_t *seconds);

/* Pure monotonic policy: automatic interval is separate from non-bypassable
 * failure/Retry-After backoff and the manual/wake 30-second anti-storm floor. */
typedef struct {
    uint64_t next_attempt_ms;
    uint64_t hard_backoff_until_ms;
    uint64_t debounce_until_ms;
    uint32_t auto_interval_minutes; /* Zero initializes to the safe 15m default. */
    unsigned failures;
} reset_feed_poll_t;
bool reset_feed_poll_due(const reset_feed_poll_t *poll, uint64_t now_ms);
bool reset_feed_poll_manual_due(const reset_feed_poll_t *poll, uint64_t now_ms);
void reset_feed_poll_begin(reset_feed_poll_t *poll, uint64_t now_ms);
bool reset_feed_interval_valid(uint32_t minutes);
void reset_feed_poll_complete(reset_feed_poll_t *poll, uint64_t now_ms,
                              bool success, uint64_t retry_after_seconds);

/* Retain this pointer-free, UTC-based value in RTC_DATA_ATTR storage. Import
 * only after an actual deep-sleep reset; cold boot must ignore RTC contents.
 * Version 6 expands original-text storage; version/size/checksum reject old,
 * corrupt or incompatible snapshots. No secrets,
 * ETag, heap pointer, RTOS handle, or boot-relative clock is retained. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t checksum;
    reset_feed_data_t data;
    uint32_t has_data;
    uint32_t failures;
    uint32_t auto_interval_minutes;
    uint32_t error;
    int32_t http_status;
    int64_t last_checked_at;
    int64_t last_attempt_at;
    int64_t hard_backoff_until;
    int64_t next_auto_at;
    reset_history_data_t history;
    uint32_t has_history;
    uint32_t history_error;
    int32_t history_http_status;
    int64_t history_last_checked_at;
} reset_feed_rtc_state_t;

#ifdef ESP_PLATFORM
#include "esp_err.h"

/* Call start once in application startup. The persistent task never touches
 * LVGL; views share the sole RTC cache under a mutex. No credentials or feed persist in
 * flash. HTTPS uses the IDF CA bundle, hostname verification, and no redirects.
 * Requires json, esp_http_client, mbedtls and esp_timer components. */
esp_err_t reset_feed_start(void);

/* Nonblocking; time_synchronized must reflect an actual SNTP sync, not just
 * plausible wall time. Call on connectivity/sync changes, including failures. */
void reset_feed_set_ready(bool wifi_connected, bool time_synchronized);

/* Nonblocking. Manual refresh bypasses only the automatic interval, never
 * readiness, the 30s attempt debounce, or failure/Retry-After backoff. Snapshot
 * refresh_block explains a rejection. Accepted requests are coalesced. */
bool reset_feed_request_refresh(void);
/* Copies small status metadata only. Access data through lock_snapshot, or
 * while its revision is pinned; the borrowed cache can otherwise change. */
void reset_feed_get_snapshot(reset_feed_snapshot_t *out);
/* Worker and UI share one cache, including its RTC storage. Do not retain a
 * data view across unlock unless pin_reader succeeded for that revision.
 * Acquire before the LVGL lock; the feed worker never acquires LVGL. */
bool reset_feed_lock_snapshot(reset_feed_snapshot_t *out, uint32_t timeout_ms);
void reset_feed_unlock_snapshot(void);
/* Pin the last rendered revision atomically. A mismatched revision returns
 * false so HOME refreshes before opening; no old body copy is kept. While
 * pinned, status fetches wait and an already-in-flight response cannot replace
 * the cache or its ETag. Unpin retries any deferred response with normal limits. */
bool reset_feed_pin_reader(uint32_t revision);
void reset_feed_unpin_reader(void);

/* Nonblocking, lazy history request on page entry or OK; coalesced and
 * throttled to six hours after success. Shares the existing worker, transport
 * readiness, pause, debounce and global failure/Retry-After policy. A true
 * result means fresh data already exists or a request is pending; it does
 * not mean network access or successful completion. Pending requests may wait
 * for Wi-Fi/SNTP/backoff; pause cancels them. */
bool reset_feed_request_history(void);
void reset_feed_get_history_snapshot(reset_history_snapshot_t *out);

/* Configuration is applied immediately; flash persistence belongs to caller. */
bool reset_feed_set_auto_interval(uint32_t minutes);

/* Call from a worker, never a button/LVGL callback. Pauses admission first,
 * then waits up to timeout_ms (plus scheduler tick granularity) for HTTP cleanup. A false result means do not
 * sleep; the service stays paused until resume(). Successful return guarantees
 * no HTTP request is in flight and none can start while paused. */
bool reset_feed_pause_and_wait(uint32_t timeout_ms);
void reset_feed_resume(bool refresh_now);

/* Export only after successful pause_and_wait; import only before start.
 * Restored cache is marked stale until a successful online verification.
 * Wake refresh waits for Wi-Fi + SNTP and preserves UTC backoff/debounce. */
bool reset_feed_export_rtc(reset_feed_rtc_state_t *out);
bool reset_feed_import_rtc(const reset_feed_rtc_state_t *state);
/* Application uses the service-owned cache in place, without a second RTC or
 * task-stack copy. Restore only after a real deep-sleep reset, before start. */
bool reset_feed_restore_retained(void);
bool reset_feed_retain(void);
#endif
