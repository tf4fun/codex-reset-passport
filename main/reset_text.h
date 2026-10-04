#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Deliberately bounded: the feed stores 256 UTF-8 bytes, the reader at most
 * eight seven-line pages. No allocation, full CJK font, or translation service. */
#define RESET_TEXT_MAX_BYTES 256U
#define RESET_TEXT_MAX_PAGES 8U
#define RESET_TEXT_PAGE_CAPACITY 272U
#define RESET_TEXT_WIDTH 178U
#define RESET_TEXT_SUMMARY_LINES 3U
#define RESET_TEXT_READING_LINES 7U
/* Return an actual glyph advance in pixels, or -1 for a missing glyph.
 * Firmware binds this to LVGL's active font, including kerning for next_cp. */
typedef int (*reset_text_advance_fn)(void *context, uint32_t cp, uint32_t next_cp);
typedef struct {
    char summary[RESET_TEXT_PAGE_CAPACITY];
    char pages[RESET_TEXT_MAX_PAGES][RESET_TEXT_PAGE_CAPACITY];
    uint8_t page_count;
    bool glyph_substituted;
    bool source_truncated;
    bool page_limit_reached;
    bool summary_shortened;
} reset_text_layout_t;
void reset_text_layout(const char *input, size_t capacity, bool source_truncated,
                       reset_text_advance_fn advance, void *context,
                       unsigned width, reset_text_layout_t *out);
/* Bounded one/two-line auxiliary copy with the same glyph safety/metrics.
 * Returns true if any content was omitted or unsupported glyph replaced. */
bool reset_text_fit(const char *input, size_t capacity, unsigned max_lines,
                    reset_text_advance_fn advance, void *context, unsigned width,
                    char *out, size_t out_capacity);
