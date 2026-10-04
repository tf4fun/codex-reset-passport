#include "reset_ui.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>
LV_FONT_DECLARE(reset_font_16);
LV_FONT_DECLARE(reset_font_12);
#define PAPER 0xFFF4DD
#define WHITE 0xFFFDF7
#define INK 0x26201A
#define SECONDARY 0x5C5347
#define MUTED 0x877B6B
#define SUN 0xFFD84D
#define ROSE 0xFFB9CC
#define SKY 0xA5DCFF
#define ALERT 0x96512C
static lv_obj_t *screen, *content;
static reset_ui_model_t previous;
static bool have_previous;
static lv_obj_t *hold_overlay, *hold_arc, *hold_title;
static lv_obj_t *hold_hint;
/* Read-only shared styles live in Flash instead of consuming the LVGL pool. */
#define TEXT_STYLE(name, font_value, color_value) \
    static const lv_style_const_prop_t name##_props[] = { \
        { .prop = LV_STYLE_TEXT_FONT, .value = { .ptr = (font_value) } }, \
        { .prop = LV_STYLE_TEXT_COLOR, .value = { .color = { \
            .red = ((color_value) >> 16) & 255, .green = ((color_value) >> 8) & 255, \
            .blue = (color_value) & 255 } } }, \
        { .prop = LV_STYLE_TEXT_LINE_SPACE, .value = { .num = 4 } }, \
        LV_STYLE_CONST_PROPS_END }; \
    static LV_STYLE_CONST_INIT(name, name##_props)
TEXT_STYLE(text_small_ink, &reset_font_12, INK);
TEXT_STYLE(text_small_secondary, &reset_font_12, SECONDARY);
TEXT_STYLE(text_small_muted, &reset_font_12, MUTED);
TEXT_STYLE(text_small_alert, &reset_font_12, ALERT);
TEXT_STYLE(text_regular, &reset_font_16, INK);
TEXT_STYLE(text_ascii_12, &lv_font_montserrat_12, MUTED);
TEXT_STYLE(text_ascii_14, &lv_font_montserrat_14, INK);
TEXT_STYLE(text_ascii_20, &lv_font_montserrat_20, INK);
TEXT_STYLE(text_ascii_32, &lv_font_montserrat_32, INK);
#undef TEXT_STYLE

static void apply_text_style(lv_obj_t *obj, const lv_font_t *font, uint32_t color) {
    const lv_style_t *style = &text_regular;
    if (font == &reset_font_12) style = color == SECONDARY ? &text_small_secondary :
        color == MUTED ? &text_small_muted : color == ALERT ? &text_small_alert : &text_small_ink;
    else if (font == &lv_font_montserrat_12) style = &text_ascii_12;
    else if (font == &lv_font_montserrat_14) style = &text_ascii_14;
    else if (font == &lv_font_montserrat_20) style = &text_ascii_20;
    else if (font == &lv_font_montserrat_32) style = &text_ascii_32;
    lv_obj_add_style(obj, style, 0);
}

static lv_obj_t *label(int x, int y, int w, const char *value, const lv_font_t *font, uint32_t color) {
    lv_obj_t *obj = lv_label_create(content);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_label_set_text_static(obj, value);
    apply_text_style(obj, font, color);
    return obj;
}
static void text(int x, int y, int w, const char *value, uint32_t color) {
    label(x, y, w, value, &reset_font_16, color);
}
static void small(int x, int y, int w, const char *value, uint32_t color) {
    label(x, y, w, value, &reset_font_12, color);
}
static void draw_card_shadow(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(event, 3);
        return;
    }
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    area.x1 += 2; area.x2 += 2;
    area.y1 += 3; area.y2 += 3;
    lv_draw_rect_dsc_t draw;
    lv_draw_rect_dsc_init(&draw);
    draw.bg_color = lv_color_hex(INK);
    draw.bg_opa = LV_OPA_COVER;
    draw.radius = lv_obj_get_style_radius(obj, LV_PART_MAIN);
    lv_draw_rect(lv_event_get_layer(event), &draw, &area);
}
static void rect(int x, int y, int w, int h, uint32_t color, int radius, bool shadow) {
    lv_obj_t *obj = lv_obj_create(content);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(INK), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    if (shadow) {
        lv_obj_add_event_cb(obj, draw_card_shadow, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        lv_obj_add_event_cb(obj, draw_card_shadow, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        lv_obj_refresh_ext_draw_size(obj);
    }
}
static void title(const char *value) { text(22, 52, 196, value, INK); }
static void footer(const char *value, bool warning) {
    small(22, 291, 196, value, warning ? ALERT : SECONDARY);
}
static void full_stat(int y, uint32_t color, const char *name, const char *number) {
    rect(18, y, 202, 56, color, 11, true);
    small(30, y + 8, 178, name, SECONDARY);
    label(30, y + 25, 178, number, &lv_font_montserrat_20, INK);
}
static lv_obj_t *hold_label(int y, const char *value, const lv_font_t *font, uint32_t color) {
    lv_obj_t *obj = lv_label_create(hold_overlay);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 30, y);
    lv_obj_set_width(obj, 180);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_label_set_text_static(obj, value);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    apply_text_style(obj, font, color);
    return obj;
}
static void draw_hold_moon(lv_event_t *event) {
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
    int32_t cx = (bounds.x1 + bounds.x2) / 2;
    int32_t cy = (bounds.y1 + bounds.y2) / 2;
    lv_draw_rect_dsc_t draw;
    lv_draw_rect_dsc_init(&draw);
    draw.radius = LV_RADIUS_CIRCLE;
    draw.bg_opa = LV_OPA_COVER;
    draw.bg_color = lv_color_hex(INK);
    lv_area_t moon = {cx - 12, cy - 12, cx + 12, cy + 12};
    lv_draw_rect(lv_event_get_layer(event), &draw, &moon);
    /* Two small geometric discs form a crescent; no image or extra widget. */
    draw.bg_color = lv_color_hex(PAPER);
    lv_area_t cutout = {cx - 4, cy - 16, cx + 18, cy + 6};
    lv_draw_rect(lv_event_get_layer(event), &draw, &cutout);
}
static void create_hold_overlay(void) {
    /* A persistent screen sibling, not a child of the rebuildable page. */
    hold_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(hold_overlay);
    lv_obj_set_size(hold_overlay, 240, 320);
    lv_obj_remove_flag(hold_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(hold_overlay, lv_color_hex(INK), 0);
    lv_obj_set_style_bg_opa(hold_overlay, LV_OPA_40, 0);
    lv_obj_t *panel = lv_obj_create(hold_overlay);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 20, 61);
    lv_obj_set_size(panel, 200, 214);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(INK), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    hold_title = hold_label(83, "准备休息", &reset_font_16, INK);
    hold_arc = lv_arc_create(hold_overlay);
    lv_obj_remove_style_all(hold_arc);
    lv_obj_set_pos(hold_arc, 75, 127);
    lv_obj_set_size(hold_arc, 90, 90);
    lv_obj_remove_flag(hold_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(hold_arc, 270);
    lv_arc_set_bg_angles(hold_arc, 0, 360);
    lv_arc_set_range(hold_arc, 0, 1000);
    lv_arc_set_value(hold_arc, 0);
    lv_obj_set_style_arc_width(hold_arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_color(hold_arc, lv_color_hex(0xE4D8C2), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(hold_arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(hold_arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(hold_arc, lv_color_hex(SUN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(hold_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(hold_arc, true, LV_PART_INDICATOR);
    lv_obj_add_event_cb(hold_arc, draw_hold_moon, LV_EVENT_DRAW_MAIN_END, NULL);
    hold_hint = hold_label(227, "松手就返回", &reset_font_16, INK);
    lv_obj_add_flag(hold_overlay, LV_OBJ_FLAG_HIDDEN);
}
static void update_hold_overlay(const reset_ui_model_t *m) {
    if (!m->hold_visible) {
        if (hold_overlay) {
            lv_obj_delete(hold_overlay);
            hold_overlay = NULL;
        }
        lv_obj_remove_flag(content, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!hold_overlay) create_hold_overlay();
    /* Keep the page objects intact but suppress their draw tasks under the
     * modal. This preserves the 24 KiB pool without a framebuffer snapshot. */
    lv_obj_add_flag(content, LV_OBJ_FLAG_HIDDEN);
    unsigned progress = m->hold_progress > 1000 ? 1000 : m->hold_progress;
    if (m->hold_blocked) progress = 0;
    lv_obj_remove_flag(hold_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_arc_set_value(hold_arc, (int32_t)progress);
    /* Static-lifetime literals avoid label-text allocation every frame. */
    lv_obj_set_style_arc_color(hold_arc, lv_color_hex(m->hold_armed ? SKY : SUN), LV_PART_INDICATOR);
    lv_label_set_text_static(hold_title, m->hold_blocked ? "配网期间无法休眠" : "准备休息");
    lv_label_set_text_static(hold_hint, m->hold_blocked ? "松手返回" :
                             m->hold_armed ? (m->hold_released ? "正在确认松键" : "松手，晚安") : "松手就返回");
}
void reset_ui_create(void) {
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 240, 320);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    hold_overlay = NULL;
    lv_screen_load(screen);
    have_previous = false;
}
void reset_ui_update(const reset_ui_model_t *m) {
    if (!screen || !m) return;
    if (have_previous && memcmp(&previous, m, sizeof(*m)) == 0) return;
    bool page_changed = !have_previous ||
                        memcmp(&previous, m, offsetof(reset_ui_model_t, hold_visible)) != 0;
    previous = *m;
    m = &previous; /* Static-lifetime storage for page label text. */
    have_previous = true;
    update_hold_overlay(m);
    if (!page_changed) return;
    lv_obj_clean(content);
    label(22, 23, 150, "Codex Resets", &lv_font_montserrat_14, INK);
    label(178, 23, 39, m->battery, &lv_font_montserrat_12, MUTED);
    if (m->page <= RESET_UI_SETTINGS) {
        static char number[8];
        snprintf(number, sizeof(number), "%u/4", (unsigned)m->page + 1);
        label(192, 52, 30, number, &lv_font_montserrat_12, MUTED);
    }
    if (m->page == RESET_UI_HOME) {
        small(22, 47, 196, "全站公告观察", SECONDARY);
        rect(18, 73, 202, 142, WHITE, 12, true);
        small(30, 84, 178, m->status, m->warning ? ALERT : SECONDARY);
        rect(29, 105, 181, 45, SUN, 8, false);
        label(38, 110, 166, m->hero, &lv_font_montserrat_32, INK);
        label(30, 162, 178, m->date, &lv_font_montserrat_20, INK);
        small(30, 189, 178, m->zone, MUTED);
        if (m->has_notice) {
            rect(18, 228, 202, 52, SKY, 10, true);
            small(29, 236, 181, m->next_title, SECONDARY);
            text(29, 254, 181, m->next_body, INK);
        } else {
            rect(18, 228, 96, 52, ROSE, 10, true);
            rect(124, 228, 96, 52, SKY, 10, true);
            small(28, 235, 76, "重置次数", SECONDARY);
            small(134, 235, 76, "平均间隔(天)", SECONDARY);
            label(28, 253, 78, m->stats_total, &lv_font_montserrat_20, INK);
            label(134, 253, 78, m->stats_average, &lv_font_montserrat_20, INK);
        }
        footer(m->freshness, m->warning);
    } else if (m->page == RESET_UI_STATS) {
        title("站点统计");
        full_stat(86, SUN, "累计已执行重置", m->stats_total);
        full_stat(151, ROSE, "平均间隔 · 天", m->stats_average);
        full_stat(216, SKY, "距上次 · 天", m->stats_elapsed);
        footer("上下翻页 · 长按确认休眠", false);
    } else if (m->page == RESET_UI_DETAILS) {
        title("最新公告");
        rect(18, 84, 202, 189, WHITE, 12, true);
        text(30, 98, 178, m->detail_type, INK);
        label(30, 128, 178, m->date, &lv_font_montserrat_20, INK);
        small(30, 155, 178, m->zone, MUTED);
        small(30, 181, 178, m->detail_source, SECONDARY);
        small(30, 207, 178, "站点数据生成时间 UTC", MUTED);
        label(30, 228, 178, m->detail_generated, &lv_font_montserrat_14, INK);
        small(30, 251, 178, "codex-resets.com", SECONDARY);
        footer("第三方汇总 · 非个人额度", false);
    } else if (m->page == RESET_UI_SETTINGS) {
        title("设备设置");
        static const char *names[] = {"Wi-Fi", "自动同步", "本地时差", "屏幕亮度", "蓝牙配网", "返回概览"};
        for (int i = 0; i < 6; ++i) {
            int y = 85 + 32 * i;
            if (m->settings_editing && m->selected_setting == i) rect(18, y - 4, 202, 30, SUN, 7, false);
            small(28, y + 2, 79, names[i], SECONDARY);
            small(112, y + 2, 98, m->settings_values[i], INK);
        }
        footer(m->settings_editing ? "上下选择 · 确认操作 · 长按休眠" : "上下翻页 · 确认进入设置", false);
    } else if (m->page == RESET_UI_TIMEZONE) {
        title("本地时间偏移");
        rect(18, 95, 202, 74, SUN, 12, true);
        label(32, 120, 179, m->zone, &lv_font_montserrat_20, INK);
        text(24, 194, 192, "上加时差 · 下减时差", INK);
        small(24, 225, 192, "每次十五分钟，确认保存", SECONDARY);
        small(24, 248, 192, "固定偏移，不自动切换夏令时", MUTED);
        footer("确认保存返回 · 长按进入休眠", false);
    } else if (m->page == RESET_UI_SETUP) {
        title("连接家中网络");
        small(24, 85, 192, m->setup_line, ALERT);
        rect(18, 117, 202, 135, WHITE, 12, true);
        small(29, 130, 182, "微信小程序", SECONDARY);
        text(29, 152, 182, "蓝牙配网-FoloToy", INK);
        label(29, 177, 182, "AI PASSPORT", &lv_font_montserrat_14, INK);
        small(29, 205, 182, "BLUFI_FoloPassport", MUTED);
        small(29, 229, 182, m->setup_timer, MUTED);
        footer("上开启 · 下清除 · 确认返回", false);
    } else if (m->page == RESET_UI_CLEAR) {
        title("清除保存的网络？");
        rect(18, 93, 202, 155, ROSE, 12, true);
        text(30, 110, 179, "清除后需要重新配网", INK);
        small(30, 151, 179, "不会删除其他设备设置", SECONDARY);
        text(30, 200, 179, "短按确认清除", INK);
        footer("上下取消 · 长按进入休眠", false);
    } else {
        title("准备深度休眠");
        rect(18, 102, 202, 113, SUN, 12, true);
        text(30, 124, 179, m->sleep_progress, INK);
        small(30, 172, 179, "正在安全结束无线连接", SECONDARY);
        footer("任意短按取消", false);
    }
}
