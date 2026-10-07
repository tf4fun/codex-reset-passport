#include "reset_presenter.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static void stamp(int64_t epoch, int offset, char *out, size_t size, bool year) {
    time_t t = (time_t)(epoch + (int64_t)offset * 60);
    struct tm *tm = gmtime(&t);
    if (!tm || epoch <= 0) { snprintf(out, size, "-- / --  --:--"); return; }
    strftime(out, size, year ? "%Y-%m-%d %H:%M" : "%m/%d %H:%M", tm);
}
static void relative(int64_t epoch, const reset_presenter_state_t *state, char *out, size_t size) {
    if (epoch <= 0 || state->now < epoch || (!state->clock_ready && !state->restored_cache)) {
        snprintf(out, size, "时间待核对"); return;
    }
    int64_t age = state->now - epoch;
    long long days = (long long)(age / 86400), hours = (long long)(age / 3600 % 24);
    if (days > 9999) snprintf(out, size, "很久以前");
    else if (days >= 100 || (days && !hours)) snprintf(out, size, "%lld天前", days);
    else if (days) snprintf(out, size, "%lld天%lld小时前", days, hours);
    else if (hours) snprintf(out, size, "%lld小时前", hours);
    else if (age >= 60) snprintf(out, size, "%lld分钟前", (long long)(age / 60));
    else snprintf(out, size, "刚刚");
}
bool reset_presenter_capture_reader(const reset_ui_model_t *model,
                                    reset_reader_snapshot_t *snapshot) {
    if (!snapshot) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!model || !model->reading_available) return false;
    snapshot->text = model->announcement_text;
    snapshot->revision = model->announcement_revision;
    snprintf(snapshot->type, sizeof(snapshot->type), "%s", model->announcement_type);
    snprintf(snapshot->date, sizeof(snapshot->date), "%s", model->announcement_date);
    snprintf(snapshot->zone, sizeof(snapshot->zone), "%s", model->zone);
    snprintf(snapshot->source_url, sizeof(snapshot->source_url), "%s", model->announcement_source_url);
    snapshot->source_original = model->announcement_source_original;
    snapshot->truncated = model->announcement_truncated;
    snapshot->valid = true;
    return true;
}
/* Select exactly one primary record. Prediction observations never replace a
 * historical reset or become a future timestamp. The feed already validated
 * bounded source URLs; the QR renderer also checks its destination/capacity. */
static void select_record(reset_ui_model_t *out, const char *raw, const char *type,
                          int64_t announced_at, const char *source_url, bool observed,
                          bool truncated, int offset, const reset_presenter_state_t *state) {
    out->announcement_text = raw[0] ? raw : "此公告未提供原文";
    snprintf(out->announcement_type, sizeof(out->announcement_type), "%s", type);
    stamp(announced_at, offset, out->announcement_date, sizeof(out->announcement_date), false);
    relative(announced_at, state, out->announcement_age, sizeof(out->announcement_age));
    out->announcement_truncated = truncated;
    if (!observed && source_url[0]) {
        snprintf(out->announcement_source_url, sizeof(out->announcement_source_url), "%s", source_url);
        out->announcement_source_original = true;
    }
    out->reading_available = true;
}
#define SET(field, value) snprintf(out->field, sizeof(out->field), "%s", (value))
void reset_presenter_build(const reset_feed_snapshot_t *feed,
                           const reset_presenter_state_t *state,
                           reset_ui_model_t *out) {
    memset(out, 0, sizeof(*out));
    out->page = state->page;
    out->connected = state->connected;
    out->settings_editing = state->settings_editing;
    out->selected_setting = state->selected_setting;
    out->reading_page = state->reading_page;
    out->announcement_revision = feed->revision;
    if (state->battery >= 0 && state->battery <= 100)
        snprintf(out->battery, sizeof(out->battery), "%d%%", state->battery);
    SET(status, "最近一次 CODEX 额度重置");
    SET(hero, "--"); SET(date, "-- / --  --:--");
    SET(announcement_type, "暂无公告"); SET(announcement_age, "时间暂未公布");
    SET(announcement_date, "-- / --  --:--"); out->announcement_text = "暂无可显示的公告原文";
    SET(announcement_source_url, "https://codex-resets.com/zh-CN");
    SET(detail_type, "暂无重置记录"); SET(detail_source, "第三方汇总 · 非个人额度");
    SET(detail_generated, "--");
    SET(stats_total, "--"); SET(stats_average, "--"); SET(stats_elapsed, "--");
    SET(next_title, "重置概览"); SET(next_time, "暂未公布");
    SET(next_status, "暂无重置记录");
    out->forecast_confidence = -1;
    int offset = state->page == RESET_UI_TIMEZONE ? state->draft_utc_offset : state->settings.utc_offset_minutes;
    reset_settings_zone_label(offset, out->zone, sizeof(out->zone));
    if (feed->has_data && feed->data) {
        stamp(feed->data->generated_at, 0, out->detail_generated, sizeof(out->detail_generated), true);
        snprintf(out->stats_total, sizeof(out->stats_total), "%lu", (unsigned long)feed->data->stats.total);
        if (feed->data->stats.has_avg_interval_days)
            snprintf(out->stats_average, sizeof(out->stats_average), "%.1f", feed->data->stats.avg_interval_days);
        if (feed->data->stats.has_days_since_last)
            snprintf(out->stats_elapsed, sizeof(out->stats_elapsed), "%.1f", feed->data->stats.days_since_last);
        if (feed->data->latest.present) {
            out->latest_available = true;
            relative(feed->data->latest.announced_at, state, out->hero, sizeof(out->hero));
            stamp(feed->data->latest.announced_at, offset, out->date, sizeof(out->date), true);
            SET(detail_type, feed->data->latest.kind == RESET_KIND_BANKED ? "备用额度 · 已发放" : "常规重置 · 已执行");
            SET(detail_source, feed->data->latest.observed ? "来源：站点观察" : "来源：站点收录公告");
            if (feed->data->latest.kind == RESET_KIND_BANKED) SET(status, "最近一次备用额度发放");
        }
        /* Presence determines priority, never relative announcement freshness. */
        if (feed->data->scheduled.present) {
            out->primary_scheduled = out->has_notice = true;
            SET(next_title, "下一次重置");
            SET(next_status, feed->data->scheduled.kind == RESET_KIND_BANKED ? "备用额度 · 已计划" : "常规重置 · 已计划");
            if (feed->data->scheduled.has_time) {
                out->next_has_time = true;
                char when[32];
                stamp(feed->data->scheduled.scheduled_for, offset, when, sizeof(when), true);
                /* stamp has the fixed YYYY-MM-DD HH:MM format. */
                snprintf(out->next_date, sizeof(out->next_date), "%.10s", when);
                snprintf(out->next_time, sizeof(out->next_time), "%.5s", when + 11);
                if (state->clock_ready && feed->data->scheduled.scheduled_for <= state->now)
                    SET(next_status, feed->data->scheduled.kind == RESET_KIND_BANKED ? "备用额度 · 等待确认" : "常规重置 · 等待确认");
            }
            if (state->page == RESET_UI_HOME)
                select_record(out, feed->data->scheduled.text,
                              feed->data->scheduled.kind == RESET_KIND_BANKED ? "备用额度 · 已计划" : "常规重置 · 已计划",
                              feed->data->scheduled.announced_at, feed->data->scheduled.source_url,
                              feed->data->scheduled.observed, feed->data->scheduled.text_truncated, offset, state);
        } else if (feed->data->latest.present) {
            SET(next_title, feed->data->latest.kind == RESET_KIND_BANKED ? "最近一次备用额度" : "最近一次重置");
            SET(next_status, out->detail_type);
            if (state->page == RESET_UI_HOME)
                select_record(out, feed->data->latest.text, out->detail_type,
                              feed->data->latest.announced_at, feed->data->latest.source_url,
                              feed->data->latest.observed, feed->data->latest.text_truncated, offset, state);
        }
        /* A watch remains a separate secondary prediction, even with a plan.
         * Expired/unverified-clock watches never become the primary record. */
        if (feed->data->watch.present && state->clock_ready && feed->data->watch.expires_at > state->now) {
            out->timing_is_forecast = true;
            SET(next_body, feed->data->watch.forecast_window[0] ? feed->data->watch.forecast_window : "时间暂未公布");
            out->next_truncated = feed->data->watch.forecast_window_truncated;
            out->forecast_confidence = feed->data->watch.confidence_percent;
        }
    }
    /* The reader must never silently fall back to another live record. */
    if (state->page == RESET_UI_READING && state->reader_snapshot && state->reader_snapshot->valid) {
        const reset_reader_snapshot_t *saved = state->reader_snapshot;
        out->announcement_text = saved->text;
        out->announcement_revision = saved->revision;
        SET(announcement_type, saved->type);
        SET(announcement_date, saved->date);
        SET(announcement_source_url, saved->source_url);
        SET(zone, saved->zone);
        out->announcement_source_original = saved->source_original;
        out->announcement_truncated = saved->truncated;
        out->reading_available = true;
    }
    out->warning = !state->connected || feed->stale || feed->status == RESET_FEED_ERROR;
    if (!state->settings.wifi_enabled) {
        if (state->radio_control_pending) SET(freshness, "Wi-Fi 正在关闭");
        else if (!state->radio_stopped) SET(freshness, "Wi-Fi 关闭失败 · 请重启");
        else SET(freshness, "Wi-Fi 已关闭 · 设置中开启");
    }
    else if (!state->connected) SET(freshness, feed->has_data ? "离线 · 上次数据" : "未联网 · 设置中配网");
    else if (!state->clock_ready) SET(freshness, "正在校时 · 缓存待核验");
    else if (feed->status == RESET_FEED_FETCHING) SET(freshness, "正在同步");
    else if (state->refresh_throttled && feed->retry_in_seconds)
        snprintf(out->freshness, sizeof(out->freshness), "刷新需等待 %lu 秒", (unsigned long)feed->retry_in_seconds);
    else if (feed->status == RESET_FEED_ERROR && feed->retry_in_seconds)
        snprintf(out->freshness, sizeof(out->freshness), "更新失败 · %lu 秒后重试", (unsigned long)feed->retry_in_seconds);
    else if (feed->stale) SET(freshness, "缓存待核验 · 等待同步");
    else if (feed->last_checked_at > 0 && state->now >= feed->last_checked_at) {
        long long mins = (long long)((state->now - feed->last_checked_at) / 60);
        if (mins == 0) SET(freshness, "刚刚同步 · 长按确认休眠");
        else snprintf(out->freshness, sizeof(out->freshness), "%lld 分钟前同步 · 长按休眠", mins);
    } else SET(freshness, "等待获取数据");
    if (state->message == 1) SET(freshness, "设置保存失败 · 请重试");
    if (state->message == 2) SET(freshness, "未能休眠 · 已保持唤醒");
    unsigned attempts = state->wifi_attempts > 5 ? 5 : state->wifi_attempts;
    if (state->provisioning) {
        SET(setup_line, state->connected && state->wifi_persistence_error ? "已联网 · 凭证保存失败" : "蓝牙配网已开启");
        snprintf(out->setup_timer, sizeof(out->setup_timer), "剩余 %u 秒 · 可信近场", state->provisioning_seconds);
        /* Keep the original provisioning deadline visible even after GOT_IP
         * when saving credentials failed. These are independent outcomes. */
        if (state->wifi_persistence_error)
            snprintf(out->setup_diagnostic, sizeof(out->setup_diagnostic), "保存错误 E:%ld", (long)state->wifi_persistence_error);
    } else {
        SET(setup_timer, "仅支持 2.4 GHz Wi-Fi");
        if (!state->settings.wifi_enabled) SET(setup_line, "请先在设置中开启 Wi-Fi");
        else if (!state->wifi_initialized) SET(setup_line, state->wifi_last_error ? "Wi-Fi 启动失败" : "Wi-Fi 正在启动");
        else if (state->connected) SET(setup_line, state->wifi_persistence_error ? "已联网 · 凭证保存失败" : "已连接 Wi-Fi");
        else if (!state->has_credentials) SET(setup_line, "未保存网络 · 上键配网");
        else if (state->wifi_connecting)
            snprintf(out->setup_line, sizeof(out->setup_line), "正在连接 %u/5", attempts);
        else SET(setup_line, state->wifi_error || attempts >= 5 ? "连接失败 · 等待重试" : "等待自动重连");
        if (state->wifi_last_error || state->wifi_disconnect_reason) {
            snprintf(out->setup_timer, sizeof(out->setup_timer), "E:%ld  D:%u", (long)state->wifi_last_error, state->wifi_disconnect_reason);
            if (state->wifi_persistence_error)
                snprintf(out->setup_diagnostic, sizeof(out->setup_diagnostic), "保存错误 E:%ld", (long)state->wifi_persistence_error);
        } else if (state->wifi_persistence_error)
            snprintf(out->setup_timer, sizeof(out->setup_timer), "保存错误 E:%ld", (long)state->wifi_persistence_error);
        else if (state->has_credentials && !state->connected && !state->wifi_connecting && state->wifi_initialized && state->settings.wifi_enabled)
            snprintf(out->setup_timer, sizeof(out->setup_timer), "已尝试 %u/5 · 等待重试", attempts);
    }
    const char *wifi_value = !state->settings.wifi_enabled ?
        (state->radio_control_pending ? "关闭中" : state->radio_stopped ? "关闭" : "关闭失败") :
        !state->wifi_initialized ? (state->wifi_last_error ? "启动失败" : "启动中") :
        state->connected ? (state->wifi_persistence_error ? "已连 · 未保存" : "已连接") :
        state->wifi_connecting ? "连接中" : !state->has_credentials ? "未配网" : "等待重试";
    snprintf(out->settings_values[0], sizeof(out->settings_values[0]), "%s", wifi_value);
    snprintf(out->settings_values[1], sizeof(out->settings_values[1]), "%u 分钟", state->settings.interval_minutes);
    reset_settings_zone_label(state->settings.utc_offset_minutes, out->settings_values[2], sizeof(out->settings_values[2]));
    snprintf(out->settings_values[3], sizeof(out->settings_values[3]), "%u%%", state->settings.brightness);
    if (state->settings.sleep_minutes)
        snprintf(out->settings_values[4], sizeof(out->settings_values[4]), "%u 分钟", state->settings.sleep_minutes);
    else snprintf(out->settings_values[4], sizeof(out->settings_values[4]), "%s", "关闭");
    snprintf(out->settings_values[5], sizeof(out->settings_values[5]), "%s", state->provisioning ? "已开启" : "进入");
    snprintf(out->settings_values[6], sizeof(out->settings_values[6]), "%s", "返回");
    /* Calendar columns and labels always use UTC, independently of the local
     * offset used by announcement cards. Unknown coverage never means empty. */
    const reset_history_snapshot_t *history = state->history;
    bool clock_usable = (state->clock_ready || state->restored_cache) && state->now > 0;
    int64_t start_day = history && history->data.start_day > 0 ? history->data.start_day :
                        clock_usable ? reset_history_window_start(state->now) : 0;
    bool unknown = false;
    for (unsigned i = 0; i < RESET_HISTORY_DAYS; ++i) {
        int64_t day = start_day + (int64_t)i * 86400;
        reset_history_day_state_t value = RESET_HISTORY_DAY_UNKNOWN;
        if (clock_usable && day > state->now / 86400 * 86400)
            value = RESET_HISTORY_DAY_FUTURE;
        else if (clock_usable && history && history->has_data && history->data.start_day == start_day)
            value = reset_history_day_state(&history->data, i, state->now);
        out->history_cells[i] = (uint8_t)value;
        if (value == RESET_HISTORY_DAY_UNKNOWN) unknown = true;
        if (clock_usable && start_day > 0) {
            time_t t = (time_t)day;
            struct tm *tm = gmtime(&t);
            if (tm && (!i || tm->tm_mday == 1))
                snprintf(out->history_months[i / 7], sizeof(out->history_months[0]), "%u月", (unsigned)tm->tm_mon + 1U);
        }
    }
    if (clock_usable && start_day > 0) {
        char first[32], last[32];
        stamp(start_day, 0, first, sizeof(first), false);
        stamp(start_day + (RESET_HISTORY_DAYS - 1) * INT64_C(86400), 0, last, sizeof(last), false);
        snprintf(out->history_range, sizeof(out->history_range), "%.5s - %.5s UTC", first, last);
    } else SET(history_range, "UTC · 等待校时");
    SET(history_note, !clock_usable ? "等待校时" : !history ? "尚未读取" :
        history->status == RESET_FEED_WAITING_WIFI ? "等待联网" :
        history->status == RESET_FEED_WAITING_CLOCK ? "等待校时" :
        history->loading ? "读取中" : history->status == RESET_FEED_ERROR ?
        (history->has_data ? "缓存 · 失败" : "读取失败") : !history->has_data ? "尚未读取" :
        history->stale ? "缓存待核验" : unknown ? "部分未知" : "已同步");
    SET(sleep_progress, state->sleep_phase == 1 ? "正在结束数据请求" : state->sleep_phase == 2 ? "正在关闭无线连接" : "请松开全部按键");
}
