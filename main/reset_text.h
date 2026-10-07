#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "reset_text_limits.h"

/* Borrow immutable text and index page boundaries, not a matrix of pages.
 * Even newline-only input fits: each non-final seven-line page consumes at
 * least seven source bytes. No text allocation or translation service. */
#define RESET_TEXT_PAGE_CAPACITY 272U
#define RESET_TEXT_WIDTH 178U
#define RESET_TEXT_SUMMARY_LINES 3U
#define RESET_TEXT_READING_LINES 7U
#define RESET_TEXT_MAX_PAGES ((RESET_TEXT_MAX_BYTES + RESET_TEXT_READING_LINES - 1U) / RESET_TEXT_READING_LINES)
/* Auxiliary labels keep their original small stack/input budget. */
#define RESET_TEXT_FIT_MAX_BYTES 256U
/* Return an actual glyph advance in pixels, or -1 for a missing glyph.
 * Firmware binds this to LVGL's active font, including kerning for next_cp. */
typedef int (*reset_text_advance_fn)(void *context, uint32_t cp, uint32_t next_cp);
typedef struct {
    char summary[RESET_TEXT_PAGE_CAPACITY];
    const char *input;
    size_t input_capacity;
    uint16_t page_offsets[RESET_TEXT_MAX_PAGES + 1U];
    uint8_t page_count;
    bool glyph_substituted;
    bool source_truncated;
    bool page_limit_reached;
    bool summary_shortened;
} reset_text_layout_t;
void reset_text_layout(const char *input, size_t capacity, bool source_truncated,
                       reset_text_advance_fn advance, void *context,
                       unsigned width, reset_text_layout_t *out);
/* Input must remain unchanged until formatting finishes. Format only the
 * requested page, with the same metrics used during layout.
 * Out-of-range indices yield an empty string. Caller owns the output buffer. */
void reset_text_page(const reset_text_layout_t *layout, unsigned page,
                     reset_text_advance_fn advance, void *context, unsigned width,
                     char out[RESET_TEXT_PAGE_CAPACITY]);
/* Bounded one/two-line auxiliary copy with the same glyph safety/metrics.
 * Returns true if any content was omitted or unsupported glyph replaced. */
bool reset_text_fit(const char *input, size_t capacity, unsigned max_lines,
                    reset_text_advance_fn advance, void *context, unsigned width,
                    char *out, size_t out_capacity);
