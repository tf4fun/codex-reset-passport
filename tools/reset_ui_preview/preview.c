/* Renders the actual firmware presentation code using LVGL 9.5, not a mock UI. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "reset_ui.h"
#include "reset_text.h"
#include "reset_hold.h"
#include "reset_presenter.h"
#include "bsp_display_rounding.h"

LV_FONT_DECLARE(reset_font_16);
LV_FONT_DECLARE(reset_font_12);
LV_FONT_DECLARE(reset_font_24);
static reset_ui_page_t audit_page;
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
        if (!has_glyph(&reset_font_16, cp) || !has_glyph(&reset_font_12, cp)) {
            fprintf(stderr, "Missing inventory glyph U+%04X\n", cp);
            ++missing_count;
        }
    }
    for (uint32_t cp = 0x20; cp <= 0x7e; ++cp) {
        ++checked;
        if (!has_glyph(&reset_font_16, cp) || !has_glyph(&reset_font_12, cp)) ++missing_count;
    }
    const unsigned char *hero = (const unsigned char *)"0123456789-天小时分钟前刚很久以前时间待核对";
    while (*hero) if (!has_glyph(&reset_font_24, next_codepoint(&hero))) ++missing_count;
    printf("Verified %u inventory/ASCII occurrences in BOTH body fonts plus 24px hero subset\n", checked);
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
        if ((audit_page == RESET_UI_READING && area.y1 == 107 && natural.y > 164)) {
            fprintf(stderr, "Body escaped reserved line/chrome budget: %s\n", text);
            ++clipped_count;
        }
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

/* This is the same presentation mapping as the firmware input owner. */
static void apply_hold(reset_ui_model_t *model, const reset_hold_t *hold) {
    model->hold_visible = hold->visible;
    model->hold_progress = hold->progress1000;
    model->hold_armed = hold->armed;
    model->hold_blocked = hold->blocked;
    model->hold_released = hold->waiting_release && !hold->pressed;
}

static void update_hold_frame(reset_ui_model_t *model, const reset_hold_t *hold) {
    apply_hold(model, hold);
    reset_ui_update(model);
    reset_ui_update(model);
    lv_obj_update_layout(lv_screen_active());
    audit_labels(lv_screen_active());
    lv_refr_now(NULL);
}

static bool same_live_memory(const lv_mem_monitor_t *a, const lv_mem_monitor_t *b) {
    return a->free_size == b->free_size && a->used_cnt == b->used_cnt;
}

static bool stress_hold(reset_ui_model_t *model) {
    reset_hold_t hold;
    lv_mem_monitor_t before = {0}, after = {0};
    unsigned expected_objects = 0;
    bool stable = true;
    /* Warm the EXACT workload before measuring it, including every arc angle.
     * Comparing a different warm-up angle set mistakes retained draw scratch
     * buffers for leaks. The measured pass uses real monotonic hold timings. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint64_t start = UINT64_C(10000) * pass;
        reset_hold_init(&hold);
        update_hold_frame(model, &hold);
        reset_hold_press(&hold, start, false);
        reset_hold_tick(&hold, start + RESET_HOLD_TAP_MS, false);
        update_hold_frame(model, &hold);
        lv_obj_t *arc = find_arc(lv_screen_active());
        lv_obj_t *content = lv_obj_get_child(lv_screen_active(), 0);
        lv_obj_t *first_label = lv_obj_get_child(content, 0);
        if (!arc || !first_label) return false;
        expected_objects = object_count(lv_screen_active());
        if (pass) {
            unexpected_deletes = 0;
            lv_obj_add_event_cb(arc, unexpected_delete, LV_EVENT_DELETE, NULL);
            lv_obj_add_event_cb(first_label, unexpected_delete, LV_EVENT_DELETE, NULL);
            lv_mem_monitor(&before);
        }
        for (unsigned i = 0; i < 500; ++i) {
            reset_hold_tick(&hold, start + RESET_HOLD_TAP_MS + i * 3U, false);
            update_hold_frame(model, &hold);
            stable = stable && find_arc(lv_screen_active()) == arc &&
                     lv_obj_get_child(content, 0) == first_label &&
                     object_count(lv_screen_active()) == expected_objects &&
                     lv_arc_get_value(arc) == hold.progress1000;
        }
        if (pass) {
            lv_mem_monitor(&after);
            stable = stable && !unexpected_deletes && same_live_memory(&before, &after) &&
                     lv_mem_test() == LV_RESULT_OK;
            lv_obj_remove_event_cb(arc, unexpected_delete);
            lv_obj_remove_event_cb(first_label, unexpected_delete);
        }
        reset_hold_cancel(&hold);
        update_hold_frame(model, &hold);
    }
    printf("Hold stress: 500 real-state-machine in-place progress updates; objects=%u; "
           "deletes=%u; before_free=%zu after_free=%zu; allocations=%zu/%zu; %s\n",
           expected_objects, unexpected_deletes, before.free_size, after.free_size,
           before.used_cnt, after.used_cnt, stable ? "PASS" : "FAIL");
    return stable;
}

static bool stress_cancel(reset_ui_model_t *model) {
    reset_hold_t hold;
    lv_mem_monitor_t baseline = {0}, current = {0};
    bool stable = true;
    unsigned expected_objects = 0;
    lv_obj_t *content = lv_obj_get_child(lv_screen_active(), 0);
    lv_obj_t *first_label = lv_obj_get_child(content, 0);
    unexpected_deletes = 0;
    lv_obj_add_event_cb(first_label, unexpected_delete, LV_EVENT_DELETE, NULL);
    for (unsigned pass = 0; pass <= 100; ++pass) {
        uint64_t start = UINT64_C(10000) * pass;
        reset_hold_init(&hold);
        reset_hold_press(&hold, start, false);
        for (unsigned elapsed = 0; elapsed <= 1250; elapsed += 10) {
            reset_hold_tick(&hold, start + elapsed, false);
            update_hold_frame(model, &hold);
            if (elapsed < RESET_HOLD_TAP_MS)
                stable = stable && find_arc(lv_screen_active()) == NULL;
        }
        stable = stable && reset_hold_release(&hold, start + 1250, start + 1250) == RESET_HOLD_CANCEL;
        uint16_t previous_progress = hold.progress1000;
        for (unsigned elapsed = 0; elapsed <= 250; elapsed += 10) {
            reset_hold_event_t event = reset_hold_tick(&hold, start + 1250 + elapsed, true);
            update_hold_frame(model, &hold);
            stable = stable && event != RESET_HOLD_TAP && event != RESET_HOLD_SLEEP &&
                     hold.progress1000 <= previous_progress;
            previous_progress = hold.progress1000;
        }
        stable = stable && !hold.active && !hold.visible && !find_arc(lv_screen_active()) &&
                 lv_obj_get_child(content, 0) == first_label && !unexpected_deletes &&
                 !lv_obj_has_flag(content, LV_OBJ_FLAG_HIDDEN) && lv_mem_test() == LV_RESULT_OK;
        lv_mem_monitor(&current);
        if (!pass) {
            baseline = current;
            expected_objects = object_count(lv_screen_active());
        } else {
            stable = stable && same_live_memory(&baseline, &current) &&
                     object_count(lv_screen_active()) == expected_objects;
        }
    }
    lv_obj_remove_event_cb(first_label, unexpected_delete);
    printf("Cancel stress: 100 real 1250 ms holds plus 250 ms unwind/open-close cycles; "
           "base page deletes=%u; warm_free=%zu final_free=%zu; allocations=%zu/%zu; %s\n",
           unexpected_deletes, baseline.free_size, current.free_size,
           baseline.used_cnt, current.used_cnt, stable ? "PASS" : "FAIL");
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
    audit_page = model->page;
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

static bool render_hold_scenario(const char *dir, FILE *trace, const char *name,
                                 unsigned held_ms, unsigned end_ms,
                                 const reset_feed_snapshot_t *feed,
                                 const reset_presenter_state_t *base) {
    const unsigned pressed_ms = 400;
    const unsigned released_ms = pressed_ms + held_ms;
    const unsigned boundaries[] = {pressed_ms, pressed_ms + 500, released_ms,
                                   released_ms + 250, end_ms};
    reset_hold_t hold;
    reset_hold_init(&hold);
    reset_presenter_state_t state = *base;
    state.page = RESET_UI_HOME;
    reset_ui_model_t model;
    bool valid = true;
    unsigned tap_count = 0, cancel_count = 0, sleep_count = 0, frame = 0;
    fprintf(trace, "{\"name\":\"%s\",\"pressed_ms\":%u,\"released_ms\":%u,\"frames\":[",
            name, pressed_ms, released_ms);
    for (unsigned now = 0; now <= end_ms; ++frame) {
        reset_hold_event_t released = RESET_HOLD_NONE;
        uint16_t displayed_before_release = hold.progress1000;
        if (now == pressed_ms) reset_hold_press(&hold, now, false);
        if (now == released_ms) released = reset_hold_release(&hold, now, now);
        reset_hold_event_t event = reset_hold_tick(&hold, now, now >= released_ms);
        bool tapped = released == RESET_HOLD_TAP || event == RESET_HOLD_TAP;
        tap_count += tapped;
        if (tapped) state.page = RESET_UI_READING; /* Ordinary HOME short-confirm action. */
        cancel_count += released == RESET_HOLD_CANCEL || event == RESET_HOLD_CANCEL;
        if (event == RESET_HOLD_SLEEP) {
            ++sleep_count;
            state.page = RESET_UI_SLEEP_WAIT;
            state.sleep_phase = 1;
        }
        if (now < pressed_ms + RESET_HOLD_TAP_MS)
            valid = valid && !hold.visible && !hold.progress1000;
        if (held_ms >= 500 && now == pressed_ms + 500)
            valid = valid && hold.visible && hold.progress1000 == 0;
        if (held_ms == 1250 && now == released_ms)
            valid = valid && hold.visible && hold.cancelling && hold.progress1000 > 0 &&
                    hold.progress1000 == displayed_before_release;
        if (held_ms >= 2000 && now == released_ms)
            valid = valid && hold.visible && hold.armed && hold.progress1000 == 1000;
        if (held_ms >= 2000 && now < released_ms + 250)
            valid = valid && state.page != RESET_UI_SLEEP_WAIT;
        if (now == released_ms + 250)
            valid = valid && !hold.visible && !hold.active &&
                    ((held_ms < 2000) || state.page == RESET_UI_SLEEP_WAIT);
        reset_presenter_build(feed, &state, &model);
        apply_hold(&model, &hold);
        char filename[96];
        snprintf(filename, sizeof(filename), "%s-frame-%03u", name, frame);
        render(dir, filename, &model);
        valid = valid && ((find_arc(lv_screen_active()) != NULL) == hold.visible);
        unsigned next = now + 20;
        for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i)
            if (boundaries[i] > now && boundaries[i] < next) next = boundaries[i];
        fprintf(trace, "%s{\"file\":\"%s.png\",\"time_ms\":%u,\"duration_ms\":%u,"
                       "\"visible\":%s,\"progress1000\":%u,\"cancelling\":%s,"
                       "\"armed\":%s,\"cleanup\":%s}", frame ? "," : "", filename, now, next - now,
                       hold.visible ? "true" : "false", hold.progress1000,
                       hold.cancelling ? "true" : "false", hold.armed ? "true" : "false",
                       state.page == RESET_UI_SLEEP_WAIT ? "true" : "false");
        lv_tick_inc(next - now);
        now = next;
    }
    valid = valid && tap_count == (held_ms < 500 ? 1U : 0U) &&
            cancel_count == (held_ms >= 500 && held_ms < 2000 ? 1U : 0U) &&
            sleep_count == (held_ms >= 2000 ? 1U : 0U) &&
            (held_ms >= 500 || state.page == RESET_UI_READING);
    fprintf(trace, "],\"tap_events\":%u,\"cancel_events\":%u,\"sleep_events\":%u,\"passed\":%s}",
            tap_count, cancel_count, sleep_count, valid ? "true" : "false");
    printf("Scenario %s: %u frames, tap=%u cancel=%u sleep=%u; %s\n",
           name, frame, tap_count, cancel_count, sleep_count, valid ? "PASS" : "FAIL");
    return valid;
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
                        .announced_at = 1790875800,
                        .text = "We have reset Codex usage limits. The update is now available to all eligible users. Thank you for building with Codex." },
            .generated_at = 1791087000
        }
    };
    reset_presenter_state_t state = {
        .page = RESET_UI_HOME, .connected = true, .has_credentials = true, .wifi_initialized = true,
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
        captured_state.page = RESET_UI_READING;
        for (unsigned page = 0; page < reset_ui_reading_page_count(); ++page) {
            captured_state.reading_page = page;
            reset_presenter_build(&captured, &captured_state, &model);
            char name[64]; snprintf(name, sizeof(name), "00-captured-reader-%u", page + 1);
            render(argv[1], name, &model);
        }
        captured_state.page = RESET_UI_OVERVIEW;
        reset_presenter_build(&captured, &captured_state, &model);
        render(argv[1], "00-captured-overview", &model);
    }
    SHOW("01-home-current");
    state.page = RESET_UI_READING;
    SHOW("01b-announcement-reading");
    state.reading_page = 1;
    SHOW("01c-announcement-reading-end");
    state.reading_page = 0;
    state.page = RESET_UI_OVERVIEW;
    SHOW("02-reset-overview");
    state.page = RESET_UI_STATS;
    SHOW("03-statistics");
    state.page = RESET_UI_SETTINGS;
    SHOW("04-settings");
    state.settings_editing = true;
    SHOW("05-settings-selected");
    state.selected_setting = 4;
    SHOW("05b-auto-sleep-default-five-minutes");
    state.settings.sleep_minutes = 0;
    SHOW("05c-auto-sleep-off");
    state.settings.sleep_minutes = 30;
    SHOW("05d-auto-sleep-thirty-minutes");
    state.settings.sleep_minutes = 5;
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
    feed.data.scheduled.announced_at = state.now - 3600;
    snprintf(feed.data.scheduled.text, sizeof(feed.data.scheduled.text), "A reset is planned. The time has passed; execution has not yet been confirmed.");
    SHOW("12-scheduled-overdue");
    feed.data.scheduled.present = false;
    feed.data.watch.present = true;
    feed.data.watch.expires_at = state.now + 3600;
    feed.data.watch.confidence_percent = 100;
    snprintf(feed.data.watch.forecast_window, sizeof(feed.data.watch.forecast_window), "within the next few hours");
    SHOW("13-ai-prediction");
    state.page = RESET_UI_SETUP;
    state.connected = false;
    state.wifi_error = true;
    SHOW("14-network-error");
    state.page = RESET_UI_OVERVIEW;
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
    /* Text safety fixtures exercise production presenter, glyph metrics and UI. */
    feed.has_data = true; feed.stale = false; feed.status = RESET_FEED_CURRENT;
    feed.last_checked_at = state.now; feed.data.generated_at = state.now;
    feed.data.latest.present = true; feed.data.latest.kind = RESET_KIND_REGULAR;
    feed.data.latest.announced_at = state.now - 118800;
    memset(feed.data.latest.text, 'W', RESET_FEED_TEXT_MAX_BYTES);
    feed.data.latest.text[RESET_FEED_TEXT_MAX_BYTES] = '\0';
    SHOW("22-long-source-time-only-home");
    state.page = RESET_UI_READING;
    unsigned wide_pages = reset_ui_reading_page_count();
    bool text_cases_ok = wide_pages > 1 && wide_pages <= RESET_TEXT_MAX_PAGES;
    for (unsigned page = 0; page < wide_pages; ++page) {
        char name[64]; snprintf(name, sizeof(name), "23-wide-word-reader-%u", page + 1);
        state.reading_page = page; SHOW(name);
    }
    const char *url = "https://example.com/abcdefghijklmnopqrstuvwxyz0123456789/abcdefghijklmnopqrstuvwxyz0123456789/abcdefghijklmnopqrstuvwxyz0123456789/abcdefghijklmnopqrstuvwxyz0123456789/abcdefghijklmnopqrstuvwxyz0123456789/abcdefghijklmnopqrstuvwxyz0123456789";
    snprintf(feed.data.latest.text, sizeof(feed.data.latest.text), "%s", url);
    state.page = RESET_UI_HOME; state.reading_page = 0;
    SHOW("24-long-url-time-only-home");
    unsigned url_pages = reset_ui_reading_page_count();
    state.page = RESET_UI_READING;
    for (unsigned page = 0; page < url_pages; ++page) {
        char name[64]; snprintf(name, sizeof(name), "25-long-url-reader-%u", page + 1);
        state.reading_page = page; SHOW(name);
    }
    snprintf(feed.data.latest.text, sizeof(feed.data.latest.text), "Original text with emoji 🙂 and unsupported glyph 龘.\r\n新公告：常规重置已执行。\tNo remote translation is used.");
    state.page = RESET_UI_HOME; state.reading_page = 0;
    SHOW("26-missing-glyph-time-only-home");
    state.page = RESET_UI_READING;
    SHOW("27-missing-glyph-reader");
    feed.data.latest.text_truncated = true;
    SHOW("28-truncated-and-substituted-reader");
    memset(feed.data.latest.text, 'W', RESET_FEED_TEXT_MAX_BYTES);
    feed.data.latest.text[RESET_FEED_TEXT_MAX_BYTES] = '\0';
    SHOW("29-source-byte-limit-reader");
    feed.data.latest.text_truncated = false;
    for (unsigned i = 0; i < 64; ++i) {
        feed.data.latest.text[4 * i] = 'A' + i % 26;
        feed.data.latest.text[4 * i + 1] = '\n';
        feed.data.latest.text[4 * i + 2] = '\n';
        feed.data.latest.text[4 * i + 3] = '\n';
    }
    state.page = RESET_UI_HOME;
    SHOW("30-page-limit-time-only-home");
    text_cases_ok = text_cases_ok && reset_ui_reading_page_count() == 8;
    state.page = RESET_UI_READING;
    for (unsigned page = 0; page < RESET_TEXT_MAX_PAGES; ++page) {
        char name[64]; snprintf(name, sizeof(name), "31-page-limit-reader-%u", page + 1);
        state.reading_page = page; SHOW(name);
    }
    state.reading_page = 100;
    SHOW("32-reader-clamped-to-last-page");
    feed.data.latest.present = false;
    feed.data.watch.present = true;
    feed.data.watch.observed_at = state.now;
    feed.data.watch.expires_at = state.now + 3600;
    snprintf(feed.data.watch.forecast_window, sizeof(feed.data.watch.forecast_window), "within the next few hours");
    state.page = RESET_UI_HOME; state.reading_page = 0;
    SHOW("33-watch-only-no-announcement");
    feed.data.watch.expires_at = state.now;
    SHOW("34-expired-watch-not-a-schedule");
    feed.data.watch.expires_at = state.now + 3600;
    memset(feed.data.watch.forecast_window, 'W', RESET_FEED_FORECAST_MAX_BYTES);
    feed.data.watch.forecast_window[RESET_FEED_FORECAST_MAX_BYTES] = '\0';
    feed.data.watch.forecast_window_truncated = true;
    SHOW("35-bounded-forecast-excerpt");
    snprintf(feed.data.watch.forecast_window, sizeof(feed.data.watch.forecast_window), "soon 🙂 龘");
    SHOW("35b-forecast-missing-glyphs");
    feed.data.watch.present = false;
    feed.data.latest.present = true;
    state.page = RESET_UI_OVERVIEW;
    const int64_t ages[] = { 0, 59, 60, 59 * 60, 3600, 23 * 3600, 118800, 99 * 86400 + 23 * 3600, 9999LL * 86400, 10000LL * 86400 };
    for (unsigned i = 0; i < sizeof(ages) / sizeof(ages[0]); ++i) {
        char name[64]; snprintf(name, sizeof(name), "36-relative-age-case-%u", i);
        feed.data.latest.announced_at = state.now - ages[i]; SHOW(name);
    }
    printf("Bounded original-text scenarios: %u wide-word pages, %u URL pages, all 8 page-limit pages, glyph fallbacks; %s\n", wide_pages, url_pages, text_cases_ok ? "PASS" : "FAIL");
    /* Recovery diagnostics: numeric codes only, no SSID or password. */
    state.page = RESET_UI_SETUP; state.connected = false; state.provisioning = false;
    state.wifi_initialized = false; state.wifi_last_error = 0;
    SHOW("37-wifi-starting");
    state.wifi_last_error = INT32_MIN;
    SHOW("38-wifi-init-failure-min-code");
    state.wifi_last_error = INT32_MAX; state.wifi_disconnect_reason = UINT16_MAX;
    SHOW("39-wifi-init-failure-max-codes");
    state.wifi_initialized = true; state.wifi_last_error = 0; state.wifi_disconnect_reason = 0;
    state.has_credentials = false;
    SHOW("40-wifi-no-saved-network");
    state.has_credentials = true; state.wifi_connecting = true; state.wifi_attempts = 5;
    SHOW("41-wifi-connecting-five-of-five");
    state.wifi_connecting = false; state.wifi_last_error = INT32_MIN; state.wifi_disconnect_reason = UINT16_MAX;
    SHOW("42-wifi-retry-max-reason-min-error");
    state.connected = true; state.wifi_last_error = 0; state.wifi_disconnect_reason = 0;
    state.wifi_persistence_error = INT32_MIN;
    SHOW("43-wifi-connected-credential-save-failed");
    state.wifi_last_error = INT32_MAX; state.wifi_disconnect_reason = UINT16_MAX;
    SHOW("43b-wifi-independent-runtime-and-save-errors");
    state.wifi_last_error = 0; state.wifi_disconnect_reason = 0;
    state.provisioning = true; state.provisioning_seconds = 180;
    SHOW("44-wifi-save-failed-countdown-preserved");
    state.page = RESET_UI_SETTINGS; state.selected_setting = 0;
    SHOW("45-settings-connected-but-not-saved");
    state.provisioning = false; state.wifi_persistence_error = 0; state.wifi_attempts = 0;
    /* Real production hold state drives every progress sample and scenario. */
    state.page = RESET_UI_HOME;
    state.connected = true;
    state.provisioning = false;
    feed.has_data = true;
    feed.stale = false;
    feed.status = RESET_FEED_CURRENT;
    feed.data.latest.present = true;
    feed.data.latest.announced_at = 1790875800;
    feed.data.latest.kind = RESET_KIND_REGULAR;
    snprintf(feed.data.latest.text, sizeof(feed.data.latest.text), "We have reset Codex usage limits. The update is now available to all eligible users. Thank you for building with Codex.");
    feed.data.generated_at = state.now;
    feed.last_checked_at = state.now;
    reset_presenter_build(&feed, &state, &model);
    reset_hold_t hold;
    reset_hold_init(&hold);
    reset_hold_press(&hold, 0, false);
    const unsigned fractions[] = {0, 250, 500, 1000};
    for (unsigned i = 0; i < sizeof(fractions) / sizeof(fractions[0]); ++i) {
        char name[64];
        reset_hold_tick(&hold, RESET_HOLD_TAP_MS + fractions[i] * 3U / 2U, false);
        apply_hold(&model, &hold);
        snprintf(name, sizeof(name), "18-hold-%03u-percent", fractions[i] / 10);
        render(argv[1], name, &model);
    }
    reset_hold_release(&hold, 2000, 2000);
    reset_hold_tick(&hold, 2000, true);
    apply_hold(&model, &hold);
    render(argv[1], "19-hold-stable-release", &model);
    reset_hold_cancel(&hold);
    apply_hold(&model, &hold);
    render(argv[1], "20-hold-cancelled-home", &model);
    state.page = RESET_UI_SETUP;
    state.provisioning = true;
    reset_presenter_build(&feed, &state, &model);
    reset_hold_press(&hold, 0, true);
    reset_hold_tick(&hold, 500, false);
    apply_hold(&model, &hold);
    render(argv[1], "21-hold-blocked-provisioning", &model);
    state.page = RESET_UI_HOME;
    state.provisioning = false;
    char trace_path[1024];
    if (snprintf(trace_path, sizeof(trace_path), "%s/hold-scenarios.json", argv[1]) >= (int)sizeof(trace_path))
        return 2;
    FILE *trace = fopen(trace_path, "wb");
    if (!trace) { perror(trace_path); return 2; }
    fputs("[", trace);
    bool scenarios_ok = render_hold_scenario(argv[1], trace, "v2.3-fast-tap-no-popup", 200, 1400, &feed, &state);
    fputs(",", trace);
    scenarios_ok = render_hold_scenario(argv[1], trace, "v2.3-release-1250ms-unwind", 1250, 2500, &feed, &state) && scenarios_ok;
    fputs(",", trace);
    scenarios_ok = render_hold_scenario(argv[1], trace, "v2.3-full-hold-release-cleanup", 2000, 3200, &feed, &state) && scenarios_ok;
    fputs("]\n", trace);
    if (fclose(trace) != 0) { perror(trace_path); return 2; }
    reset_presenter_build(&feed, &state, &model);
    audit_page = RESET_UI_HOME;
    bool hold_ok = stress_hold(&model);
    bool cancel_ok = stress_cancel(&model);
    /* Reader page navigation and truncation states use exactly the same bounded
     * objects; measure after an identical warm-up, not a different page type. */
    lv_mem_monitor_t reader_before = {0}, reader_after = {0};
    state.page = RESET_UI_READING;
    for (unsigned i = 0; i < 64; ++i) {
        feed.data.latest.text[4 * i] = 'A' + i % 26;
        feed.data.latest.text[4 * i + 1] = '\n';
        feed.data.latest.text[4 * i + 2] = '\n';
        feed.data.latest.text[4 * i + 3] = '\n';
    }
    feed.data.latest.text[256] = '\0';
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < 500; ++i) {
            state.reading_page = i % 8;
            feed.data.latest.text_truncated = i % 2;
            reset_presenter_build(&feed, &state, &model);
            audit_page = model.page;
            reset_ui_update(&model);
            lv_obj_update_layout(lv_screen_active());
            audit_labels(lv_screen_active());
            lv_refr_now(NULL);
        }
        lv_mem_monitor(pass ? &reader_after : &reader_before);
    }
    bool reader_ok = same_live_memory(&reader_before, &reader_after) && lv_mem_test() == LV_RESULT_OK;
    printf("Reader stress: 500 page/truncation changes, warmed free=%zu final=%zu blocks=%zu/%zu; %s\n", reader_before.free_size, reader_after.free_size, reader_before.used_cnt, reader_after.used_cnt, reader_ok ? "PASS" : "FAIL");
    feed.data.latest.text_truncated = false;
#undef SHOW
    /* Replay both phases of the allocator's observed two-batch placement
     * cycle. Each 500-change endpoint must match its own warmed endpoint
     * exactly, in both live blocks and free bytes: no growth tolerance. */
    lv_mem_monitor_t warm_memory[2] = {{0}}, memory = {0};
    bool memory_ok = true;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned phase = 0; phase < 2; ++phase) {
            for (unsigned i = 0; i < 500; ++i) {
                state.page = (reset_ui_page_t)(i % 4);
                reset_presenter_build(&feed, &state, &model);
                reset_ui_update(&model);
                reset_ui_update(&model);
                lv_obj_update_layout(lv_screen_active());
                lv_refr_now(NULL);
            }
            lv_mem_monitor(&memory);
            printf("Top navigation %s phase %u: free=%zu allocations=%zu\n",
                   pass ? "measured" : "warm", phase + 1, memory.free_size, memory.used_cnt);
            if (!pass) warm_memory[phase] = memory;
            else memory_ok = memory_ok && same_live_memory(&warm_memory[phase], &memory);
        }
    }
    memory_ok = memory_ok && lv_mem_test() == LV_RESULT_OK;
    if (!memory_ok) fprintf(stderr, "LVGL allocation did not stabilize across page changes\n");
    printf("LVGL 24 KiB pool after 1000 measured page changes: max_used=%zu, "
           "free=%zu, largest_free=%zu, fragmentation=%u%%\n",
           memory.max_used, memory.free_size, memory.free_biggest_size, memory.frag_pct);
    printf("Audited %u actual LVGL labels: %u missing glyphs, %u clipped labels\n",
           label_count, missing_count, clipped_count);
    lv_display_delete(display);
    lv_deinit();
    return missing_count || clipped_count || !memory_ok || !hold_ok || !cancel_ok || !scenarios_ok || !text_cases_ok || !reader_ok ? 1 : 0;
}
