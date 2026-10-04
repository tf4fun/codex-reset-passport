#include "reset_presenter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    reset_feed_snapshot_t f = {0};
    reset_presenter_state_t s = {.battery = -1, .now = 1791000000};
    reset_settings_defaults(&s.settings);
    reset_ui_model_t m;
    reset_presenter_build(&f, &s, &m);
    assert(!strcmp(m.hero, "--") && !strcmp(m.battery, ""));
    assert(m.warning && strstr(m.freshness, "配网"));
    s.connected = s.clock_ready = true; s.battery = 75;
    f.has_data = true; f.status = RESET_FEED_CURRENT; f.last_checked_at = s.now;
    f.data.generated_at = s.now; f.data.latest.present = true;
    f.data.latest.kind = RESET_KIND_REGULAR; f.data.latest.announced_at = s.now - 90000;
    f.data.stats.total = 57; f.data.stats.has_avg_interval_days = true; f.data.stats.avg_interval_days = 6.8;
    reset_presenter_build(&f, &s, &m);
    assert(!strcmp(m.hero, "1d 01h") && !strcmp(m.battery, "75%"));
    assert(!strcmp(m.stats_total, "57") && !strcmp(m.stats_average, "6.8"));
    assert(!strcmp(m.stats_elapsed, "--") && !m.has_notice);
    f.data.scheduled.present = f.data.scheduled.has_time = true;
    f.data.scheduled.scheduled_for = s.now - 1;
    reset_presenter_build(&f, &s, &m);
    assert(m.has_notice && strstr(m.next_body, "等待确认"));
    assert(strstr(m.next_title, "常规重置"));
    f.data.scheduled.kind = RESET_KIND_BANKED;
    reset_presenter_build(&f, &s, &m);
    assert(strstr(m.next_title, "储备额度") && strstr(m.next_title, "待执行"));
    f.data.scheduled.has_time = false;
    reset_presenter_build(&f, &s, &m); assert(strstr(m.next_body, "未确定"));
    f.data.scheduled.present = false; f.data.watch.present = true; f.data.watch.expires_at = s.now - 1;
    reset_presenter_build(&f, &s, &m); assert(strstr(m.next_body, "已过期") && strstr(m.next_title, "非承诺"));
    f.data.latest.kind = RESET_KIND_BANKED; f.data.latest.observed = true;
    reset_presenter_build(&f, &s, &m); assert(strstr(m.status, "储备") && strstr(m.detail_source, "观察"));
    s.settings.wifi_enabled = false;
    s.radio_control_pending = true;
    reset_presenter_build(&f, &s, &m); assert(strstr(m.freshness, "正在关闭"));
    s.radio_control_pending = false;
    reset_presenter_build(&f, &s, &m); assert(strstr(m.freshness, "关闭失败") && strstr(m.settings_values[0], "失败"));
    s.radio_stopped = true;
    reset_presenter_build(&f, &s, &m); assert(strstr(m.freshness, "已关闭"));
    s.settings.wifi_enabled = true; s.refresh_throttled = true; f.retry_in_seconds = 29;
    reset_presenter_build(&f, &s, &m); assert(strstr(m.freshness, "29 秒"));
    f.data.latest.announced_at = 1790982000; /* 2026-10-02 23:00 UTC */
    s.settings.utc_offset_minutes = 120;
    reset_presenter_build(&f, &s, &m); assert(!strcmp(m.date, "10/03 01:00") && !strcmp(m.zone, "UTC+02:00"));
    s.settings.utc_offset_minutes = -420;
    reset_presenter_build(&f, &s, &m); assert(!strcmp(m.date, "10/02 16:00") && !strcmp(m.zone, "UTC-07:00"));
    s.page = RESET_UI_TIMEZONE; s.draft_utc_offset = 345;
    reset_presenter_build(&f, &s, &m); assert(!strcmp(m.zone, "UTC+05:45"));
    puts("Reset presenter tests: PASS");
    return 0;
}
