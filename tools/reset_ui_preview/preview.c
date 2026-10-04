/* Renders the actual firmware presentation code using LVGL 9.5, not a mock UI. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "reset_ui.h"
#include "reset_presenter.h"
#include "bsp_display_rounding.h"

LV_FONT_DECLARE(reset_font_16);
#define WIDTH PREVIEW_WIDTH
#define HEIGHT PREVIEW_HEIGHT
static uint16_t framebuffer[WIDTH * HEIGHT];
static uint16_t draw_buffer[WIDTH * 40];
static unsigned label_count;
static unsigned missing_count;
static unsigned clipped_count;
static unsigned unexpected_deletes;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint16_t *source = (const uint16_t *)pixels;
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        int32_t left = 0, right = -1;
        bool visible = bsp_display_rounded_row_span(y, WIDTH, HEIGHT, PREVIEW_RADIUS, &left, &right);
        for (int32_t x = area->x1; x <= area->x2; ++x) {
            if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
                framebuffer[y * WIDTH + x] = visible && x >= left && x <= right ? *source : 0;
            ++source;
        }
    }
    lv_display_flush_ready(display);
}

static uint32_t next_codepoint(const unsigned char **cursor) {
    const unsigned char *p = *cursor;
    uint32_t value = *p++;
    if (value >= 0xf0) {
        value = ((value & 7U) << 18) | ((p[0] & 63U) << 12) |
                ((p[1] & 63U) << 6) | (p[2] & 63U);
        p += 3;
    } else if (value >= 0xe0) {
        value = ((value & 15U) << 12) | ((p[0] & 63U) << 6) | (p[1] & 63U);
        p += 2;
    } else if (value >= 0xc0) {
        value = ((value & 31U) << 6) | (p[0] & 63U);
        ++p;
    }
    *cursor = p;
    return value;
}

static bool has_glyph(const lv_font_t *font, uint32_t codepoint) {
    lv_font_glyph_dsc_t glyph = {0};
    return lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) && !glyph.is_placeholder;
}

static void audit_inventory(void) {
    FILE *file = fopen(RESET_FONT_INVENTORY, "rb");
    if (!file) { perror(RESET_FONT_INVENTORY); exit(2); }
    char text[32768];
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    bool io_error = ferror(file);
    bool ended = feof(file);
    fclose(file);
    if (io_error || !ended) { fprintf(stderr, "Cannot read bounded font inventory\n"); exit(2); }
    text[length] = '\0';
    const unsigned char *cursor = (const unsigned char *)text;
    unsigned checked = 0;
    while (*cursor) {
        uint32_t cp = next_codepoint(&cursor);
        if (cp < 0x20) continue;
        ++checked;
        if (!has_glyph(&reset_font_16, cp)) {
            fprintf(stderr, "Missing inventory glyph U+%04X\n", cp);
            ++missing_count;
        }
    }
    for (uint32_t cp = 0x20; cp <= 0x7e; ++cp) {
        ++checked;
        if (!has_glyph(&reset_font_16, cp)) ++missing_count;
    }
    printf("Verified %u inventory/ASCII codepoint occurrences against generated font\n", checked);
}

static void audit_labels(lv_obj_t *obj) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        ++label_count;
        const char *text = lv_label_get_text(obj);
        const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        const unsigned char *cursor = (const unsigned char *)text;
        while (*cursor) {
            uint32_t cp = next_codepoint(&cursor);
            if (cp >= 0x20 && !has_glyph(font, cp)) {
                fprintf(stderr, "Missing active-font glyph U+%04X: %s\n", cp, text);
                ++missing_count;
            }
        }
        lv_point_t natural;
        lv_text_get_size(&natural, text, font,
                        lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN),
                        lv_obj_get_style_text_line_space(obj, LV_PART_MAIN),
                        LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (natural.x > lv_obj_get_content_width(obj)) {
            fprintf(stderr, "Clipped label (%d > %d px): %s\n", (int)natural.x,
                    (int)lv_obj_get_content_width(obj), text);
            ++clipped_count;
        }
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        if (area.x1 < 0 || area.y1 < 0 || area.x2 >= WIDTH || area.y2 >= HEIGHT) {
            fprintf(stderr, "Label out of screen bounds (%d,%d)-(%d,%d): %s\n",
                    (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, text);
            ++clipped_count;
        }
        for (lv_obj_t *parent = lv_obj_get_parent(obj); parent; parent = lv_obj_get_parent(parent)) {
            lv_area_t bounds;
            lv_obj_get_coords(parent, &bounds);
            if (area.x1 < bounds.x1 || area.y1 < bounds.y1 ||
                area.x2 > bounds.x2 || area.y2 > bounds.y2) {
                fprintf(stderr, "Label exceeds parent bounds: %s\n", text);
                ++clipped_count;
                break;
            }
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) audit_labels(lv_obj_get_child(obj, i));
}

static unsigned object_count(lv_obj_t *obj) {
    unsigned count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        count += object_count(lv_obj_get_child(obj, i));
    return count;
}

static lv_obj_t *find_arc(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_arc_class)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = find_arc(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return NULL;
}

static void unexpected_delete(lv_event_t *event) {
    (void)event;
    ++unexpected_deletes;
}

static bool stress_hold(reset_ui_model_t *model) {
    model->hold_visible = true;
    model->hold_progress = 0;
    model->hold_armed = model->hold_blocked = model->hold_released = false;
    reset_ui_update(model);
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(NULL);
    lv_obj_t *arc = find_arc(lv_screen_active());
    lv_obj_t *content = lv_obj_get_child(lv_screen_active(), 0);
    lv_obj_t *first_label = lv_obj_get_child(content, 0);
    if (!arc || !first_label) return false;
    lv_obj_add_event_cb(arc, unexpected_delete, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(first_label, unexpected_delete, LV_EVENT_DELETE, NULL);
    unsigned expected_objects = object_count(lv_screen_active());
    /* Warm arc angles/drawing scratch buffers before measuring: the software
     * renderer retains largest-seen scratch buffers between frames. */
    for (unsigned i = 0; i <= 100; ++i) {
        model->hold_progress = (uint16_t)(i * 10);
        reset_ui_update(model);
        lv_obj_update_layout(lv_screen_active());
        lv_refr_now(NULL);
    }
    lv_mem_monitor_t before, after;
    lv_mem_monitor(&before);
    bool stable = true;
    for (unsigned i = 0; i < 500; ++i) {
        model->hold_progress = (uint16_t)((i * 17U) % 1001U);
        reset_ui_update(model);
        lv_obj_update_layout(lv_screen_active());
        audit_labels(lv_screen_active());
        lv_refr_now(NULL);
        stable = stable && !unexpected_deletes && find_arc(lv_screen_active()) == arc &&
                 lv_obj_get_child(content, 0) == first_label &&
                 object_count(lv_screen_active()) == expected_objects &&
                 lv_arc_get_value(arc) == model->hold_progress;
    }
    lv_mem_monitor(&after);
    stable = stable && after.free_size >= before.free_size &&
             after.used_cnt <= before.used_cnt && lv_mem_test() == LV_RESULT_OK;
    printf("Hold stress: 500 in-place progress updates; objects=%u; deletes=%u; "
           "before_free=%zu after_free=%zu; allocations=%zu/%zu; %s\n",
           expected_objects, unexpected_deletes, before.free_size, after.free_size,
           before.used_cnt, after.used_cnt, stable ? "PASS" : "FAIL");
    lv_obj_remove_event_cb(arc, unexpected_delete);
    lv_obj_remove_event_cb(first_label, unexpected_delete);
    return stable;
}

static void write_ppm(const char *dir, const char *name) {
    char path[1024];
    int result = snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    if (result < 0 || (size_t)result >= sizeof(path)) exit(2);
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); exit(2); }
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (size_t i = 0; i < WIDTH * HEIGHT; ++i) {
        uint16_t pixel = framebuffer[i];
        uint8_t rgb[3] = {
            (uint8_t)(((pixel >> 11) & 31U) * 255U / 31U),
            (uint8_t)(((pixel >> 5) & 63U) * 255U / 63U),
            (uint8_t)((pixel & 31U) * 255U / 31U)
        };
        if (fwrite(rgb, sizeof(rgb), 1, file) != 1) { perror(path); exit(2); }
    }
    if (fclose(file) != 0) { perror(path); exit(2); }
    printf("Rendered %s\n", path);
}

static void render(const char *dir, const char *name, const reset_ui_model_t *model) {
    reset_ui_update(model);
    /* Exercise duplicate state updates as the device's timer does. */
    reset_ui_update(model);
    lv_obj_update_layout(lv_screen_active());
    audit_labels(lv_screen_active());
    lv_mem_monitor_t current_memory;
    lv_mem_monitor(&current_memory);
    printf("Render %s: pool_free=%zu largest=%zu objects=%u\n", name,
           current_memory.free_size, current_memory.free_biggest_size,
           object_count(lv_screen_active()));
    lv_refr_now(NULL);
    write_ppm(dir, name);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) { fprintf(stderr, "Usage: %s OUTPUT_DIRECTORY [FEED_JSON]\n", argv[0]); return 2; }
    lv_init();
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    if (!display) return 2;
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    if (has_glyph(&reset_font_16, 0x9f98U)) {
        fprintf(stderr, "Known-missing U+9F98 unexpectedly passed the glyph check\n");
        return 1;
    }
    printf("Board geometry: %d x %d RGB565, actual BSP rounded mask radius %d\n",
           WIDTH, HEIGHT, PREVIEW_RADIUS);
    audit_inventory();
    reset_ui_create();
    /* Deterministic synthetic scenarios, never claimed as a live feed capture. */
    reset_feed_snapshot_t feed = {
        .has_data = true,
        .status = RESET_FEED_CURRENT,
        .last_checked_at = 1791087000,
        .data = {
            .latest = { .present = true, .kind = RESET_KIND_REGULAR,
                        .announced_at = 1790875800 },
            .generated_at = 1791087000
        }
    };
    reset_presenter_state_t state = {
        .page = RESET_UI_HOME, .connected = true, .has_credentials = true,
        .clock_ready = true, .battery = 86, .now = 1791087120
    };
    reset_settings_defaults(&state.settings);
    feed.data.stats.total = 57;
    feed.data.stats.has_avg_interval_days = true;
    feed.data.stats.avg_interval_days = 6.8;
    feed.data.stats.has_days_since_last = true;
    feed.data.stats.days_since_last = 2.4;
    reset_ui_model_t model;
#define SHOW(name) do { reset_presenter_build(&feed, &state, &model); \
                       render(argv[1], (name), &model); } while (0)
    if (argc == 3) {
        FILE *file = fopen(argv[2], "rb");
        if (!file) { perror(argv[2]); return 2; }
        char json[RESET_FEED_BODY_LIMIT + 1];
        size_t length = fread(json, 1, sizeof(json), file);
        bool io_error = ferror(file);
        fclose(file);
        reset_feed_snapshot_t captured = {0};
        if (io_error || length > RESET_FEED_BODY_LIMIT ||
            !reset_feed_parse(json, length, &captured.data)) {
            fprintf(stderr, "Feed capture failed bounded firmware parser\n");
            return 2;
        }
        captured.has_data = true;
        captured.status = RESET_FEED_CURRENT;
        captured.last_checked_at = captured.data.generated_at;
        reset_presenter_state_t captured_state = state;
        captured_state.now = captured.data.generated_at;
        /* Battery is simulated even in the captured-feed presentation. */
        captured_state.battery = -1;
        reset_presenter_build(&captured, &captured_state, &model);
        render(argv[1], "00-captured-feed", &model);
    }
    SHOW("01-home-current");
    state.page = RESET_UI_STATS;
    SHOW("02-statistics");
    state.page = RESET_UI_DETAILS;
    SHOW("03-announcement-details");
    state.page = RESET_UI_SETTINGS;
    SHOW("04-settings");
    state.settings_editing = true;
    SHOW("05-settings-selected");
    state.page = RESET_UI_TIMEZONE;
    state.draft_utc_offset = -420;
    SHOW("06-timezone-example");
    state.page = RESET_UI_SLEEP_WAIT;
    state.sleep_phase = 1;
    SHOW("07-sleep-network-cleanup");
    state.page = RESET_UI_SETUP;
    state.provisioning = true;
    state.provisioning_seconds = 180;
    SHOW("08-network-setup");
    state.page = RESET_UI_CLEAR;
    SHOW("09-clear-confirmation");
    state.page = RESET_UI_HOME;
    state.provisioning = false;
    state.connected = false;
    feed.stale = true;
    SHOW("10-home-stale");
    feed.has_data = false;
    state.battery = -1;
    SHOW("11-home-offline");
    state.connected = true;
    state.battery = 100;
    feed.has_data = true;
    feed.stale = false;
    feed.data.scheduled.present = true;
    feed.data.scheduled.kind = RESET_KIND_REGULAR;
    feed.data.scheduled.has_time = true;
    feed.data.scheduled.scheduled_for = state.now - 60;
    SHOW("12-scheduled-overdue");
    feed.data.scheduled.present = false;
    feed.data.watch.present = true;
    feed.data.watch.expires_at = state.now + 3600;
    feed.data.watch.confidence_percent = 100;
    SHOW("13-ai-prediction");
    state.page = RESET_UI_SETUP;
    state.connected = false;
    state.wifi_error = true;
    SHOW("14-network-error");
    state.page = RESET_UI_DETAILS;
    feed.data.latest.kind = RESET_KIND_BANKED;
    feed.data.latest.observed = true;
    SHOW("15-banked-observation");
    state.page = RESET_UI_HOME;
    state.connected = true;
    state.wifi_error = false;
    feed.data.scheduled.present = true;
    feed.data.scheduled.kind = RESET_KIND_BANKED;
    feed.data.scheduled.has_time = false;
    SHOW("16-banked-pending");
    memset(&feed, 0, sizeof(feed));
    feed.status = RESET_FEED_ERROR;
    feed.error = RESET_FEED_ERROR_RATE_LIMIT;
    feed.stale = true;
    SHOW("17-first-rate-limit");
    /* Capture the actual persistent arc at required fractions and every 40 ms. */
    state.page = RESET_UI_HOME;
    state.connected = true;
    state.provisioning = false;
    feed.has_data = true;
    feed.stale = false;
    feed.status = RESET_FEED_CURRENT;
    feed.data.latest.present = true;
    feed.data.latest.announced_at = 1790875800;
    feed.data.generated_at = state.now;
    feed.last_checked_at = state.now;
    reset_presenter_build(&feed, &state, &model);
    model.hold_visible = true;
    const unsigned fractions[] = {0, 250, 500, 1000};
    for (unsigned i = 0; i < sizeof(fractions) / sizeof(fractions[0]); ++i) {
        char name[64];
        model.hold_progress = (uint16_t)fractions[i];
        model.hold_armed = fractions[i] == 1000;
        snprintf(name, sizeof(name), "18-hold-%03u-percent", fractions[i] / 10);
        render(argv[1], name, &model);
    }
    model.hold_released = true;
    render(argv[1], "19-hold-stable-release", &model);
    model.hold_visible = false;
    model.hold_armed = model.hold_released = false;
    render(argv[1], "20-hold-cancelled-home", &model);
    state.page = RESET_UI_SETUP;
    state.provisioning = true;
    reset_presenter_build(&feed, &state, &model);
    model.hold_visible = model.hold_blocked = true;
    render(argv[1], "21-hold-blocked-provisioning", &model);
    state.page = RESET_UI_HOME;
    state.provisioning = false;
    reset_presenter_build(&feed, &state, &model);
    for (unsigned frame = 0; frame <= 86; ++frame) {
        unsigned elapsed = frame * 40U;
        char name[64];
        model.hold_visible = elapsed < 2880;
        model.hold_progress = elapsed >= 2000 ? 1000 : (uint16_t)(elapsed / 2U);
        model.hold_armed = elapsed >= 2000;
        model.hold_released = elapsed >= 2600;
        if (elapsed >= 2880) {
            state.page = RESET_UI_SLEEP_WAIT;
            state.sleep_phase = 1;
            reset_presenter_build(&feed, &state, &model);
        }
        snprintf(name, sizeof(name), "hold-frame-%03u", frame);
        render(argv[1], name, &model);
        lv_tick_inc(40);
    }
    state.page = RESET_UI_HOME;
    reset_presenter_build(&feed, &state, &model);
    bool hold_ok = stress_hold(&model);
#undef SHOW
    /* Navigation/repeated-refresh endurance: allocation must stabilize. */
    lv_mem_monitor_t warm_memory = {0};
    for (unsigned i = 0; i < 500; ++i) {
        state.page = (reset_ui_page_t)(i % 4);
        reset_presenter_build(&feed, &state, &model);
        reset_ui_update(&model);
        reset_ui_update(&model);
        lv_obj_update_layout(lv_screen_active());
        lv_refr_now(NULL);
        if (i == 99) lv_mem_monitor(&warm_memory);
    }
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    printf("Warm memory free=%zu used=%zu; final free=%zu used=%zu\n", warm_memory.free_size, warm_memory.used_cnt, memory.free_size, memory.used_cnt);
    bool memory_ok = memory.free_size >= warm_memory.free_size &&
                     memory.used_cnt <= warm_memory.used_cnt &&
                     lv_mem_test() == LV_RESULT_OK;
    if (!memory_ok) fprintf(stderr, "LVGL allocation did not stabilize across page changes\n");
    printf("LVGL 24 KiB pool after 500 repeated page changes: max_used=%zu, "
           "free=%zu, largest_free=%zu, fragmentation=%u%%\n",
           memory.max_used, memory.free_size, memory.free_biggest_size, memory.frag_pct);
    printf("Audited %u actual LVGL labels: %u missing glyphs, %u clipped labels\n",
           label_count, missing_count, clipped_count);
    lv_display_delete(display);
    lv_deinit();
    return missing_count || clipped_count || !memory_ok || !hold_ok ? 1 : 0;
}
