#include "reset_ui.h"
#include "reset_ui_layout.h"
#include "reset_text.h"
#include "lvgl.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "src/misc/lv_text_private.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>
LV_FONT_DECLARE(reset_font_16);
LV_FONT_DECLARE(reset_font_12);
LV_FONT_DECLARE(reset_font_24);
static reset_text_layout_t announcement;
static char reading_text[RESET_TEXT_PAGE_CAPACITY];
static char next_text[272], reading_meta[96], reading_number[16];
static char latest_heading[80], future_heading[80], forecast_input[160];
static char reading_notice[72];
#define PAPER 0xFFF4DD
#define WHITE 0xFFFDF7
#define INK 0x26201A
#define SECONDARY 0x5C5347
#define MUTED 0x877B6B
#define SUN 0xFFD84D
#define ROSE 0xFFB9CC
#define SKY 0xA5DCFF
#define FUTURE_TIME 0xD6EEFF
#define FUTURE_TIME_EDGE 0x397A9F
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
TEXT_STYLE(text_hero, &reset_font_24, INK);
TEXT_STYLE(text_ascii_12, &lv_font_montserrat_12, MUTED);
TEXT_STYLE(text_ascii_14, &lv_font_montserrat_14, INK);
TEXT_STYLE(text_ascii_20, &lv_font_montserrat_20, INK);
TEXT_STYLE(text_ascii_32, &lv_font_montserrat_32, INK);
#undef TEXT_STYLE

static void apply_text_style(lv_obj_t *obj, const lv_font_t *font, uint32_t color) {
    const lv_style_t *style = &text_regular;
    if (font == &reset_font_12) style = color == SECONDARY ? &text_small_secondary :
        color == MUTED ? &text_small_muted : color == ALERT ? &text_small_alert : &text_small_ink;
    else if (font == &reset_font_24) style = &text_hero;
    else if (font == &lv_font_montserrat_12) style = &text_ascii_12;
    else if (font == &lv_font_montserrat_14) style = &text_ascii_14;
    else if (font == &lv_font_montserrat_20) style = &text_ascii_20;
    else if (font == &lv_font_montserrat_32) style = &text_ascii_32;
    lv_obj_add_style(obj, style, 0);
}

static int font_advance(void *context, uint32_t cp, uint32_t next_cp) {
    lv_font_glyph_dsc_t glyph = {0};
    return lv_font_get_glyph_dsc((const lv_font_t *)context, &glyph, cp, next_cp) && !glyph.is_placeholder ? glyph.adv_w : -1;
}
uint8_t reset_ui_reading_page_count(void) { return (announcement.page_count ? announcement.page_count : 1) + 1U; }

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
/* Centre the visible glyph union, not the font's line box. All callers pass
 * bounded, NUL-terminated presenter strings. This matches LVGL 9.5's glyph
 * baseline/box positioning, including mixed numerals and CJK. */
static void time_label(int x, int panel_y, int w, int panel_h, const char *value, const lv_font_t *font) {
    int top = INT32_MAX, bottom = INT32_MIN;
    uint32_t offset = 0;
    while (value[offset]) {
        uint32_t cp = lv_text_encoded_next(value, &offset);
        if (!cp) break;
        lv_font_glyph_dsc_t glyph = {0};
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, 0) || !glyph.box_w || !glyph.box_h) continue;
        int glyph_top = (int)font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        int glyph_bottom = glyph_top + glyph.box_h;
        if (glyph_top < top) top = glyph_top;
        if (glyph_bottom > bottom) bottom = glyph_bottom;
    }
    int y = panel_y + (panel_h - font->line_height) / 2;
    /* Put an odd spare pixel above the glyph box. Actual-pixel probes (not
     * just descriptor heights) verify the resulting optical margins. */
    if (bottom > top) y = panel_y + 1 + (panel_h - 2 - (bottom - top) + 1) / 2 - top;
    label(x, y, w, value, font, INK);
}
static void text(int x, int y, int w, const char *value, uint32_t color) {
    label(x, y, w, value, &reset_font_16, color);
}
static void small(int x, int y, int w, const char *value, uint32_t color) {
    label(x, y, w, value, &reset_font_12, color);
}
static void title(const char *value);

/* The bundled LVGL encoder uses byte mode at medium ECC. Compute its exact
 * minimum version ourselves: LVGL's optional quiet-zone helper can provide
 * less than four modules, so reserve our own ISO-sized border instead. */
static void source_page(const reset_ui_model_t *m) {
    const char *url = m->announcement_source_url;
    bool original = m->announcement_source_original;
    size_t length = strnlen(url, sizeof(m->announcement_source_url));
    int version = length < sizeof(m->announcement_source_url) ?
                  qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, length) : 0;
    int modules = version > 0 ? qrcodegen_version2size(version) : 0;
    int scale = modules ? 192 / (modules + 8) : 0;
    /* Never truncate an encoded address or shrink below three screen pixels
     * per module. Bad/oversized input uses the explicitly labelled data site. */
    if (!reset_feed_source_url_valid(url, sizeof(m->announcement_source_url)) || scale < 3) {
        url = "https://codex-resets.com/zh-CN";
        original = false;
        length = strlen(url);
        version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, length);
        modules = qrcodegen_version2size(version);
        scale = 192 / (modules + 8);
    }
    title(original ? "扫码查看原文" : "查看数据来源");
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    /* lv_qrcode_create first allocates an LV_DPI_DEF I1 canvas. set_size then
     * allocates the requested I1 canvas BEFORE releasing that default buffer.
     * Budget both exact stride/height/palette/alignment sizes, two draw-buffer
     * descriptors, and the encoder's two version-bounded work buffers. The
     * separate 1536-byte reserve conservatively covers the paper/QR widgets,
     * styles, child arrays and allocator headers on the 64-bit host (and C3).
     * Contiguous capacity is required per allocation, not for the whole sum:
     * a fixed 6000-byte largest-block test rejected usable fragmented pools. */
    size_t pixels = (size_t)(modules * scale);
    size_t canvas_bytes = lv_draw_buf_width_to_stride((uint32_t)pixels, LV_COLOR_FORMAT_I1) * pixels +
                          LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * sizeof(lv_color32_t) + LV_DRAW_BUF_ALIGN;
    size_t default_bytes = lv_draw_buf_width_to_stride(LV_DPI_DEF, LV_COLOR_FORMAT_I1) * (size_t)LV_DPI_DEF +
                           LV_COLOR_INDEXED_PALETTE_SIZE(LV_COLOR_FORMAT_I1) * sizeof(lv_color32_t) + LV_DRAW_BUF_ALIGN;
    size_t scratch_bytes = 2U * qrcodegen_BUFFER_LEN_FOR_VERSION(version) + 128U;
    const size_t widget_reserve = 1536U;
    size_t largest_canvas = canvas_bytes > default_bytes ? canvas_bytes : default_bytes;
    if (memory.free_size < canvas_bytes + default_bytes + 2U * sizeof(lv_draw_buf_t) + widget_reserve + scratch_bytes ||
        memory.free_biggest_size < largest_canvas + sizeof(lv_draw_buf_t) + widget_reserve) {
        text(30, 143, 180, "二维码暂不可用", INK);
        small(30, 175, 180, "请返回后重试", SECONDARY);
        return;
    }
    lv_obj_t *paper = lv_obj_create(content);
    lv_obj_remove_style_all(paper);
    lv_obj_set_pos(paper, 24, 82);
    lv_obj_set_size(paper, 192, 192);
    lv_obj_remove_flag(paper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(paper, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(paper, LV_OPA_COVER, 0);
    lv_obj_t *qr = lv_qrcode_create(paper);
    lv_qrcode_set_size(qr, modules * scale);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
    lv_qrcode_set_quiet_zone(qr, false);
    lv_obj_center(qr);
    lv_draw_buf_t *buffer = lv_canvas_get_draw_buf(qr);
    /* The upstream encoder allocates two version-bounded scratch buffers.
     * Check their joint contiguous budget after the canvas allocation, while
     * the LVGL lock excludes other callers, rather than relying on assertions. */
    lv_mem_monitor(&memory);
    if (memory.free_biggest_size < scratch_bytes ||
        !buffer || buffer->header.w != (unsigned)(modules * scale) ||
        buffer->header.h != (unsigned)(modules * scale) ||
        lv_qrcode_update(qr, url, length) != LV_RESULT_OK) {
        lv_obj_delete(paper);
        text(30, 143, 180, "二维码暂不可用", INK);
        small(30, 175, 180, "请返回后重试", SECONDARY);
    }
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
static lv_obj_t *rect(int x, int y, int w, int h, uint32_t color, int radius, bool shadow) {
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
    return obj;
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
#define HISTORY_REGULAR_COLOR 0xFF5C29
#define HISTORY_BANKED_COLOR 0xFFAD7C
#define HISTORY_EMPTY_COLOR 0xDDD3C1
#define HISTORY_FUTURE_COLOR 0xF5EEDC
static void history_cell(lv_layer_t *layer, int x, int y, int w, int h, uint8_t state) {
    lv_draw_rect_dsc_t draw;
    lv_draw_rect_dsc_init(&draw);
    draw.radius = 3;
    draw.bg_opa = LV_OPA_COVER;
    /* Solid dates do not need heavy outlines. Unknown remains outlined so
     * verified empty, uncovered and future days retain distinct treatments. */
    draw.border_color = lv_color_hex(MUTED);
    draw.border_width = state == RESET_HISTORY_DAY_UNKNOWN ? 1 : 0;
    draw.bg_color = lv_color_hex(state == RESET_HISTORY_DAY_REGULAR || state == RESET_HISTORY_DAY_BOTH ? HISTORY_REGULAR_COLOR :
                                state == RESET_HISTORY_DAY_BANKED ? HISTORY_BANKED_COLOR :
                                state == RESET_HISTORY_DAY_EMPTY ? HISTORY_EMPTY_COLOR :
                                state == RESET_HISTORY_DAY_FUTURE ? HISTORY_FUTURE_COLOR : WHITE);
    lv_area_t area = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(layer, &draw, &area);
    if (state == RESET_HISTORY_DAY_BOTH) {
        /* Two colours in one UTC-day cell preserve both kinds without inventing a count. */
        draw.radius = 1; draw.border_width = 0;
        draw.bg_color = lv_color_hex(HISTORY_BANKED_COLOR);
        area.x1 = x + w / 2; area.x2 = x + w - 2;
        area.y1 = y + 1; area.y2 = y + h - 2;
        lv_draw_rect(layer, &draw, &area);
    }
}
static void draw_history_grid(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    const reset_ui_model_t *m = &previous;
    lv_layer_t *layer = lv_event_get_layer(event);
    /* One bounded draw object owns the quiet frame, cells and legend keys. */
    lv_draw_rect_dsc_t panel;
    lv_draw_rect_dsc_init(&panel);
    panel.radius = 12;
    panel.bg_opa = LV_OPA_COVER;
    panel.bg_color = lv_color_hex(WHITE);
    panel.border_width = 1;
    panel.border_color = lv_color_hex(0xC8BEAD);
    lv_area_t area = {box.x1 + RESET_UI_HISTORY_PANEL_X, box.y1 + RESET_UI_HISTORY_PANEL_Y,
                     box.x1 + RESET_UI_HISTORY_PANEL_X + RESET_UI_HISTORY_PANEL_W - 1,
                     box.y1 + RESET_UI_HISTORY_PANEL_Y + RESET_UI_HISTORY_PANEL_H - 1};
    lv_draw_rect(layer, &panel, &area);
    for (unsigned i = 0; i < RESET_HISTORY_DAYS; ++i)
        history_cell(layer, box.x1 + RESET_UI_HISTORY_GRID_X + (i / 7) * RESET_UI_HISTORY_COLUMN_PITCH,
                     box.y1 + RESET_UI_HISTORY_GRID_Y + (i % 7) * RESET_UI_HISTORY_ROW_PITCH,
                     RESET_UI_HISTORY_CELL_SIZE, RESET_UI_HISTORY_CELL_SIZE, m->history_cells[i]);
    history_cell(layer, box.x1 + 35, box.y1 + RESET_UI_HISTORY_LEGEND_Y + 3, 8, 8, RESET_HISTORY_DAY_REGULAR);
    history_cell(layer, box.x1 + 95, box.y1 + RESET_UI_HISTORY_LEGEND_Y + 3, 8, 8, RESET_HISTORY_DAY_BANKED);
    history_cell(layer, box.x1 + 155, box.y1 + RESET_UI_HISTORY_LEGEND_Y + 3, 8, 8, RESET_HISTORY_DAY_EMPTY);
    history_cell(layer, box.x1 + 67, box.y1 + RESET_UI_HISTORY_SECOND_LEGEND_Y + 3, 8, 8, RESET_HISTORY_DAY_UNKNOWN);
    history_cell(layer, box.x1 + 137, box.y1 + RESET_UI_HISTORY_SECOND_LEGEND_Y + 3, 8, 8, RESET_HISTORY_DAY_FUTURE);
}
static void history_page(const reset_ui_model_t *m) {
    title("重置历史");
    small(24, 79, 134, m->history_range, MUTED);
    small(159, 79, 60, m->history_note, MUTED);
    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 240, 320);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(grid, draw_history_grid, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t *weekdays = label(RESET_UI_HISTORY_WEEKDAY_X, RESET_UI_HISTORY_GRID_Y, 16, "一\n二\n三\n四\n五\n六\n日", &reset_font_12, MUTED);
    /* Keep the seven row labels in one object and align unchanged-size text
     * with the more widely spaced cell centres. */
    lv_obj_set_style_text_line_space(weekdays, RESET_UI_HISTORY_ROW_PITCH - reset_font_12.line_height, 0);
    for (unsigned col = 0; col < RESET_HISTORY_WEEKS; ++col) {
        int x = RESET_UI_HISTORY_GRID_X + col * RESET_UI_HISTORY_COLUMN_PITCH;
        if (x > RESET_UI_HISTORY_MONTH_MAX_X) x = RESET_UI_HISTORY_MONTH_MAX_X;
        if (m->history_months[col][0]) small(x, RESET_UI_HISTORY_MONTH_Y, 30, m->history_months[col], MUTED);
    }
    small(48, RESET_UI_HISTORY_LEGEND_Y, 40, "常规", SECONDARY);
    small(108, RESET_UI_HISTORY_LEGEND_Y, 40, "备用", SECONDARY);
    small(168, RESET_UI_HISTORY_LEGEND_Y, 48, "无重置", SECONDARY);
    small(80, RESET_UI_HISTORY_SECOND_LEGEND_Y, 40, "未知", MUTED);
    small(150, RESET_UI_HISTORY_SECOND_LEGEND_Y, 40, "未来", MUTED);
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
    reset_text_layout(m->announcement_text, RESET_FEED_TEXT_MAX_BYTES + 1U, m->announcement_truncated,
                      font_advance, (void *)&reset_font_16, RESET_TEXT_WIDTH, &announcement);
    if (m->page == RESET_UI_HOME) {
        title("重置概览");
        snprintf(latest_heading, sizeof(latest_heading), "最近一次 · %.47s", m->detail_type);
        if (m->primary_scheduled) {
            /* Future always stays above history, and the large card owns the reader. */
            /* Colours identify the event, not whichever card is currently big. */
            rect(18, 78, 202, 138, SKY, 12, true);
            snprintf(future_heading, sizeof(future_heading), "下一次 · %.47s", m->next_status);
            small(30, 87, 178, future_heading, SECONDARY);
            lv_obj_t *future_time = rect(29, 113, 180, 49, FUTURE_TIME, 8, false);
            lv_obj_set_style_border_color(future_time, lv_color_hex(FUTURE_TIME_EDGE), 0);
            time_label(37, 113, 168, 49, m->next_time,
                       m->next_has_time ? &lv_font_montserrat_32 : &reset_font_16);
            label(30, 174, 178, m->next_date, &lv_font_montserrat_14, INK);
            small(30, 196, 178, m->zone, MUTED);
            rect(18, 226, 202, 57, WHITE, 10, true);
            small(30, 232, 178, latest_heading, SECONDARY);
            rect(29, 251, 180, 25, SUN, 6, false);
            time_label(37, 251, 168, 25, m->latest_available ? m->hero : "暂无记录", &reset_font_16);
        } else {
            rect(18, 78, 202, 57, SKY, 10, true);
            small(30, 84, 178, "下一次重置", SECONDARY);
            if (m->timing_is_forecast) {
                if (m->forecast_confidence >= 0 && m->forecast_confidence <= 100)
                    snprintf(forecast_input, sizeof(forecast_input), "预测 %d%%：%.127s", m->forecast_confidence, m->next_body);
                else snprintf(forecast_input, sizeof(forecast_input), "预测：%.127s", m->next_body);
                bool shortened = reset_text_fit(forecast_input, sizeof(forecast_input), 2, font_advance,
                                                 (void *)&reset_font_12, 178, next_text, sizeof(next_text));
                if (shortened || m->next_truncated) {
                    snprintf(forecast_input, sizeof(forecast_input), "预测(省略/替换)：%.127s", m->next_body);
                    reset_text_fit(forecast_input, sizeof(forecast_input), 2, font_advance,
                                   (void *)&reset_font_12, 178, next_text, sizeof(next_text));
                }
                small(30, 102, 178, next_text, INK);
            } else text(30, 106, 178, "暂未公布", INK);
            rect(18, 145, 202, 138, WHITE, 12, true);
            small(30, 154, 178, latest_heading, SECONDARY);
            rect(29, 180, 180, 49, SUN, 8, false);
            time_label(37, 180, 168, 49, m->latest_available ? m->hero : "暂无记录",
                       m->latest_available ? &reset_font_24 : &reset_font_16);
            label(30, 241, 178, m->date, &lv_font_montserrat_14, INK);
            small(30, 263, 178, m->zone, MUTED);
        }
        footer(m->freshness, m->warning);
    } else if (m->page == RESET_UI_HISTORY) {
        history_page(m);
    } else if (m->page == RESET_UI_READING) {
        unsigned page = m->reading_page < reset_ui_reading_page_count() ? m->reading_page : announcement.page_count;
        snprintf(reading_number, sizeof(reading_number), "%u/%u", page + 1U, reset_ui_reading_page_count());
        label(169, 54, 53, reading_number, &lv_font_montserrat_12, MUTED);
        if (page == announcement.page_count) {
            source_page(m);
            footer("上下翻页 · 确认返回", false);
            return;
        }
        title("公告原文");
        snprintf(reading_meta, sizeof(reading_meta), "%.47s  %.31s", m->announcement_date, m->zone);
        small(24, 83, 192, reading_meta, MUTED);
        reset_text_page(&announcement, page, font_advance, (void *)&reset_font_16,
                        RESET_TEXT_WIDTH, reading_text);
        text(30, 107, 178, reading_text, INK);
        snprintf(reading_notice, sizeof(reading_notice), "%s", announcement.glyph_substituted &&
                 (announcement.source_truncated || announcement.page_limit_reached) ? "原文有截断 · 部分字形已替换" :
                 (announcement.page_limit_reached || announcement.source_truncated) ? "原文有截断 · 请查看站点" :
                 announcement.glyph_substituted ? "部分字形已替换为 ?" : "");
        if (reading_notice[0]) small(24, 275, 192, reading_notice, ALERT);
        small(24, 295, 192, "Codex Resets", MUTED);
    } else if (m->page == RESET_UI_STATS) {
        title("站点统计");
        full_stat(86, SUN, "累计已执行重置", m->stats_total);
        full_stat(151, ROSE, "历史平均间隔 · 天", m->stats_average);
        full_stat(216, SKY, "距上次 · 天", m->stats_elapsed);
        footer("上下翻页 · 长按确认休眠", false);
    } else if (m->page == RESET_UI_SETTINGS) {
        title("设备设置");
        static const char *names[] = {"Wi-Fi", "自动同步", "固定时差", "屏幕亮度", "自动熄屏", "蓝牙配网", "返回概览"};
        for (int i = 0; i < 7; ++i) {
            int y = 84 + 28 * i;
            if (m->settings_editing && m->selected_setting == i) rect(18, y - 4, 202, 26, SUN, 7, false);
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
        if (m->setup_diagnostic[0]) small(24, 266, 192, m->setup_diagnostic, ALERT);
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
