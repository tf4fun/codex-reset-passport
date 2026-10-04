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
#define SET(field, value) snprintf(out->field, sizeof(out->field), "%s", (value))
void reset_presenter_build(const reset_feed_snapshot_t *feed,
                           const reset_presenter_state_t *state,
                           reset_ui_model_t *out) {
    memset(out, 0, sizeof(*out));
    out->page = state->page;
    out->connected = state->connected;
    out->settings_editing = state->settings_editing;
    out->selected_setting = state->selected_setting;
    if (state->battery >= 0 && state->battery <= 100)
        snprintf(out->battery, sizeof(out->battery), "%d%%", state->battery);
    SET(status, "距最近公告");
    SET(hero, "--"); SET(date, "-- / --  --:--");
    SET(detail_type, "暂无重置记录"); SET(detail_source, "来源：第三方汇总");
    SET(detail_generated, "--");
    SET(stats_total, "--"); SET(stats_average, "--"); SET(stats_elapsed, "--");
    int offset = state->page == RESET_UI_TIMEZONE ? state->draft_utc_offset : state->settings.utc_offset_minutes;
    reset_settings_zone_label(offset, out->zone, sizeof(out->zone));
    if (feed->has_data) {
        stamp(feed->data.generated_at, 0, out->detail_generated, sizeof(out->detail_generated), true);
        snprintf(out->stats_total, sizeof(out->stats_total), "%lu", (unsigned long)feed->data.stats.total);
        if (feed->data.stats.has_avg_interval_days)
            snprintf(out->stats_average, sizeof(out->stats_average), "%.1f", feed->data.stats.avg_interval_days);
        if (feed->data.stats.has_days_since_last)
            snprintf(out->stats_elapsed, sizeof(out->stats_elapsed), "%.1f", feed->data.stats.days_since_last);
        if (feed->data.latest.present) {
            int64_t age = state->now - feed->data.latest.announced_at;
            if (age >= 0 && (state->clock_ready || state->restored_cache))
                snprintf(out->hero, sizeof(out->hero), "%lldd %02lldh", (long long)(age / 86400), (long long)(age / 3600 % 24));
            else SET(status, "公告时间待核对");
            stamp(feed->data.latest.announced_at, offset, out->date, sizeof(out->date), false);
            SET(detail_type, feed->data.latest.kind == RESET_KIND_BANKED ? "储备重置额度" : "常规重置");
            SET(detail_source, feed->data.latest.observed ? "来源：站点观察" : "来源：X 公告");
            if (feed->data.latest.kind == RESET_KIND_BANKED) SET(status, "距储备额度公告");
        } else SET(status, "尚无重置记录");
        if (feed->data.scheduled.present) {
            out->has_notice = true;
            SET(next_title, feed->data.scheduled.kind == RESET_KIND_BANKED ? "储备额度 · 待执行" : "常规重置 · 待执行");
            if (!feed->data.scheduled.has_time) SET(next_body, "时间尚未确定");
            else if (state->clock_ready && feed->data.scheduled.scheduled_for <= state->now)
                SET(next_body, "时间已过 · 等待确认");
            else stamp(feed->data.scheduled.scheduled_for, offset, out->next_body, sizeof(out->next_body), false);
        } else if (feed->data.watch.present) {
            out->has_notice = true;
            SET(next_title, "AI 预测 · 非承诺");
            if (state->clock_ready && feed->data.watch.expires_at <= state->now) SET(next_body, "预测已过期");
            else if (feed->data.watch.confidence_percent >= 0)
                snprintf(out->next_body, sizeof(out->next_body), "预测概率 %d%%", feed->data.watch.confidence_percent);
            else SET(next_body, "关注度升高");
        }
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
    else if (feed->stale) SET(freshness, "缓存待核验 · 确认刷新");
    else if (feed->last_checked_at > 0 && state->now >= feed->last_checked_at) {
        long long mins = (long long)((state->now - feed->last_checked_at) / 60);
        if (mins == 0) SET(freshness, "刚刚同步 · 长按确认休眠");
        else snprintf(out->freshness, sizeof(out->freshness), "%lld 分钟前同步 · 长按休眠", mins);
    } else SET(freshness, "等待获取数据");
    if (state->message == 1) SET(freshness, "设置保存失败 · 请重试");
    if (state->message == 2) SET(freshness, "未能休眠 · 已保持唤醒");
    if (state->provisioning) {
        SET(setup_line, "蓝牙配网已开启");
        snprintf(out->setup_timer, sizeof(out->setup_timer), "剩余 %u 秒 · 可信近场", state->provisioning_seconds);
    } else {
        SET(setup_line, !state->settings.wifi_enabled ? "请先在设置中开启 Wi-Fi" : state->connected ? "已连接 Wi-Fi" : state->wifi_error ? "连接失败 · 上键重新配网" : "按上键开启蓝牙配网");
        SET(setup_timer, "仅支持 2.4 GHz Wi-Fi");
    }
    snprintf(out->settings_values[0], sizeof(out->settings_values[0]), "%s", state->settings.wifi_enabled ? "开启" : state->radio_control_pending ? "关闭中" : state->radio_stopped ? "关闭" : "关闭失败");
    snprintf(out->settings_values[1], sizeof(out->settings_values[1]), "%u 分钟", state->settings.interval_minutes);
    reset_settings_zone_label(state->settings.utc_offset_minutes, out->settings_values[2], sizeof(out->settings_values[2]));
    snprintf(out->settings_values[3], sizeof(out->settings_values[3]), "%u%%", state->settings.brightness);
    snprintf(out->settings_values[4], sizeof(out->settings_values[4]), "%s", state->provisioning ? "已开启" : "进入");
    snprintf(out->settings_values[5], sizeof(out->settings_values[5]), "%s", "返回");
    SET(sleep_progress, state->sleep_phase == 1 ? "正在结束数据请求" : state->sleep_phase == 2 ? "正在关闭无线连接" : "请松开全部按键");
}
