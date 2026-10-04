#include "reset_text.h"
#include <string.h>

/* Strict bounded decoding is also a second defense against malformed callers.
 * Network parsing rejects invalid UTF-8 before this module is reached. */
static size_t decode(const char *s, size_t available, uint32_t *cp) {
    if (!available || !(unsigned char)s[0]) return 0;
    const unsigned char *p = (const unsigned char *)s;
    unsigned n = p[0] < 0x80 ? 1 : p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
                 p[0] >= 0xe0 && p[0] <= 0xef ? 3 : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > available) { *cp = '?'; return 1; }
    uint32_t value = p[0] & (n == 1 ? 0x7fU : n == 2 ? 0x1fU : n == 3 ? 0xfU : 7U);
    for (unsigned i = 1; i < n; ++i) {
        if ((p[i] & 0xc0U) != 0x80U) { *cp = '?'; return 1; }
        value = (value << 6) | (p[i] & 0x3fU);
    }
    if ((n == 2 && value < 0x80) || (n == 3 && value < 0x800) ||
        (n == 4 && value < 0x10000) || (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
        *cp = '?'; return 1;
    }
    *cp = value;
    return n;
}
static size_t sanitize(const char *input, size_t capacity, reset_text_advance_fn advance,
                       void *context, char *out, bool *replaced, bool *truncated) {
    size_t i = 0, used = 0;
    if (!input) { out[0] = '\0'; return 0; }
    const size_t limit = capacity < RESET_TEXT_MAX_BYTES ? capacity : RESET_TEXT_MAX_BYTES;
    while (i < limit && input[i]) {
        uint32_t cp;
        size_t n = decode(input + i, limit - i, &cp);
        bool invalid = (unsigned char)input[i] >= 0x80 && n == 1;
        if (cp == '\r') {
            cp = '\n';
            if (i + 1 < limit && input[i + 1] == '\n') ++n;
        }
        if (cp == '\t') cp = ' ';
        bool bad = invalid || (cp != '\n' && (cp < 0x20 || cp == 0x7f || !advance || advance(context, cp, 0) < 0));
        if (bad) { out[used++] = '?'; *replaced = true; }
        else if (cp == '\n' || cp == ' ') out[used++] = (char)cp;
        else { memcpy(out + used, input + i, n); used += n; }
        i += n;
    }
    if ((i < capacity && input[i]) || i == capacity) *truncated = true;
    out[used] = '\0';
    return used;
}
static unsigned measure(const char *s, size_t size, reset_text_advance_fn advance, void *context) {
    unsigned width = 0;
    size_t i = 0;
    while (i < size) {
        uint32_t cp, next = 0;
        size_t n = decode(s + i, size - i, &cp);
        if (!n) break;
        if (i + n < size) decode(s + i + n, size - i - n, &next);
        int pixels = advance(context, cp, next);
        width += pixels < 0 ? 0U : (unsigned)pixels;
        i += n;
    }
    return width;
}
/* A line ends at whitespace if possible, otherwise at a complete codepoint.
 * Even one unbroken URL cannot escape its pixel/line budget. */
static size_t line(const char *s, size_t length, reset_text_advance_fn advance,
                   void *context, unsigned width, size_t *visible) {
    size_t i = 0, fitted = 0, break_at = 0;
    while (i < length && s[i] != '\n') {
        uint32_t cp;
        size_t n = decode(s + i, length - i, &cp);
        if (!n) break;
        if (measure(s, i + n, advance, context) > width) break;
        i += n;
        fitted = i;
        if (cp == ' ') break_at = i;
    }
    if (i < length && s[i] != '\n' && break_at) fitted = break_at;
    if (!fitted && i < length && s[i] != '\n') {
        uint32_t cp;
        fitted = decode(s, length, &cp); /* Must progress even with impossible width. */
    }
    size_t consumed = fitted;
    *visible = fitted;
    while (*visible && s[*visible - 1] == ' ') --*visible;
    while (consumed < length && s[consumed] == ' ') ++consumed;
    if (consumed < length && s[consumed] == '\n') ++consumed;
    return consumed;
}
static size_t block(const char *input, size_t length, unsigned max_lines,
                    reset_text_advance_fn advance, void *context, unsigned width,
                    char *out, size_t capacity) {
    size_t consumed = 0, used = 0;
    for (unsigned row = 0; row < max_lines && consumed < length; ++row) {
        size_t visible = 0;
        size_t take = line(input + consumed, length - consumed, advance, context, width, &visible);
        if (!take || used + visible + (row ? 1U : 0U) >= capacity) break;
        if (row) out[used++] = '\n';
        memcpy(out + used, input + consumed, visible);
        used += visible;
        consumed += take;
    }
    out[used] = '\0';
    return consumed;
}
static void ellipsis(char *out, size_t capacity, reset_text_advance_fn advance,
                     void *context, unsigned width) {
    /* ASCII dots are always in the fixed font; reserve their actual width. */
    size_t end = strlen(out), begin = end;
    while (begin && out[begin - 1] != '\n') --begin;
    unsigned dots = measure("...", 3, advance, context);
    while (end > begin && (end + 3 >= capacity || measure(out + begin, end - begin, advance, context) + dots > width)) {
        do { --end; } while (end > begin && ((unsigned char)out[end] & 0xc0U) == 0x80U);
    }
    if (dots <= width && end + 3 < capacity) { memcpy(out + end, "...", 4); }
    else out[end] = '\0';
}
void reset_text_layout(const char *input, size_t capacity, bool source_truncated,
                       reset_text_advance_fn advance, void *context,
                       unsigned width, reset_text_layout_t *out) {
    memset(out, 0, sizeof(*out));
    out->source_truncated = source_truncated;
    char clean[RESET_TEXT_MAX_BYTES + 1];
    size_t length = sanitize(input, capacity, advance, context, clean,
                             &out->glyph_substituted, &out->source_truncated);
    if (!advance || !width) { out->page_count = 1; out->page_limit_reached = length > 0; return; }
    size_t summary = block(clean, length, RESET_TEXT_SUMMARY_LINES, advance, context,
                           width, out->summary, sizeof(out->summary));
    out->summary_shortened = summary < length || out->source_truncated;
    if (out->summary_shortened) ellipsis(out->summary, sizeof(out->summary), advance, context, width);
    size_t consumed = 0;
    do {
        consumed += block(clean + consumed, length - consumed, RESET_TEXT_READING_LINES,
                          advance, context, width, out->pages[out->page_count],
                          sizeof(out->pages[0]));
        ++out->page_count;
    } while (consumed < length && out->page_count < RESET_TEXT_MAX_PAGES);
    out->page_limit_reached = consumed < length;
    if (out->page_limit_reached || out->source_truncated)
        ellipsis(out->pages[out->page_count - 1], sizeof(out->pages[0]), advance, context, width);
}
bool reset_text_fit(const char *input, size_t capacity, unsigned max_lines,
                    reset_text_advance_fn advance, void *context, unsigned width,
                    char *out, size_t out_capacity) {
    if (!out_capacity) return true;
    char clean[RESET_TEXT_MAX_BYTES + 1];
    bool replaced = false, truncated = false;
    size_t length = sanitize(input, capacity, advance, context, clean, &replaced, &truncated);
    if (!advance || !width) { out[0] = '\0'; return true; }
    size_t consumed = block(clean, length, max_lines, advance, context, width, out, out_capacity);
    if (consumed < length || truncated) ellipsis(out, out_capacity, advance, context, width);
    return consumed < length || truncated || replaced;
}
