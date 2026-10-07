#include "reset_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int pixels(void *context, uint32_t cp, uint32_t next) {
    (void)context;
    if (cp > 0x7e && cp != 0x4e2d && cp != 0x1f600) return -1;
    if (cp == 'W') return 14;
    if (cp == 'i') return 3;
    if (cp == 'A' && next == 'V') return 6;
    return cp > 0x7e ? 16 : cp == '.' ? 3 : 8;
}
static int zero_pixels(void *context, uint32_t cp, uint32_t next) {
    return cp == 'x' ? 0 : pixels(context, cp, next);
}
static uint32_t codepoint(const char **text) {
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t cp = *s++;
    if (cp >= 0xc0) {
        unsigned continuation = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
        cp &= continuation == 1 ? 0x1fU : continuation == 2 ? 0xfU : 7U;
        while (continuation--) { assert((*s & 0xc0U) == 0x80U); cp = (cp << 6) | (*s++ & 0x3fU); }
    }
    *text = (const char *)s;
    return cp;
}
static unsigned rows(const char *text) {
    unsigned count = 1;
    while (*text) if (*text++ == '\n') ++count;
    return count;
}
static void glyph_widths(const char *text, unsigned width) {
    unsigned x = 0;
    while (*text) {
        uint32_t cp = codepoint(&text);
        if (cp == '\n') { assert(x <= width); x = 0; }
        else {
            const char *peek = text;
            uint32_t next = *peek && *peek != '\n' ? codepoint(&peek) : 0;
            int advance = pixels(NULL, cp, next);
            assert(advance >= 0); x += (unsigned)advance;
        }
    }
    assert(x <= width);
}
static void page_text(const reset_text_layout_t *out, unsigned page, unsigned width,
                      char text[RESET_TEXT_PAGE_CAPACITY]) {
    reset_text_page(out, page, pixels, NULL, width, text);
}
static void audit(const reset_text_layout_t *out, unsigned width) {
    assert(out->page_count > 0 && out->page_count <= RESET_TEXT_MAX_PAGES);
    assert(rows(out->summary) <= RESET_TEXT_SUMMARY_LINES);
    glyph_widths(out->summary, width);
    char page[RESET_TEXT_PAGE_CAPACITY];
    for (unsigned i = 0; i < out->page_count; ++i) {
        page_text(out, i, width, page);
        assert(rows(page) <= RESET_TEXT_READING_LINES);
        glyph_widths(page, width);
        assert(out->page_offsets[i] <= out->page_offsets[i + 1]);
    }
}
static void roundtrip(const char *input, reset_text_layout_t *out) {
    reset_text_layout(input, strlen(input) + 1, false, pixels, NULL, 178, out);
    assert(out->input == input && !out->source_truncated && !out->page_limit_reached);
    audit(out, 178);
    char restored[RESET_TEXT_MAX_BYTES + 1] = {0}, page[RESET_TEXT_PAGE_CAPACITY];
    size_t n = 0;
    for (unsigned i = 0; i < out->page_count; ++i) {
        page_text(out, i, 178, page);
        for (const char *p = page; *p; ++p) if (*p != '\n') restored[n++] = *p;
    }
    assert(!strcmp(input, restored));
}
int main(void) {
    _Static_assert(sizeof(reset_text_layout_t) + RESET_TEXT_PAGE_CAPACITY < 1024,
                   "Reader metadata and current page must stay below 1 KiB");
    reset_text_layout_t out;
    reset_text_layout("Hello Codex", sizeof("Hello Codex"), false, pixels, NULL, 178, &out);
    assert(!strcmp(out.summary, "Hello Codex") && out.page_count == 1);
    assert(!out.source_truncated && !out.glyph_substituted);
    char input[RESET_TEXT_MAX_BYTES + 5], page[RESET_TEXT_PAGE_CAPACITY];
    memset(input, 'W', 256); input[256] = '\0';
    roundtrip(input, &out); assert(out.page_count == 4 && out.summary_shortened);
    memset(input, 'i', 256); input[256] = '\0';
    roundtrip(input, &out); assert(out.page_count == 1);
    const char *characters[] = {"W", "中", "😀"};
    for (unsigned i = 0; i < 3; ++i) {
        size_t n = strlen(characters[i]);
        for (unsigned cp = 0; cp < 280; ++cp) memcpy(input + cp * n, characters[i], n);
        input[280 * n] = '\0';
        roundtrip(input, &out); assert(!out.glyph_substituted);
    }
    /* Everything retained fits, including long words and many empty lines. */
    memset(input, 'W', RESET_TEXT_MAX_BYTES); input[RESET_TEXT_MAX_BYTES] = '\0';
    roundtrip(input, &out); assert(out.page_count > 8);
    /* Supported zero-advance glyphs cannot stall on the page byte capacity. */
    memset(input, 'x', RESET_TEXT_MAX_BYTES); input[RESET_TEXT_MAX_BYTES] = '\0';
    reset_text_layout(input, sizeof(input), false, zero_pixels, NULL, 178, &out);
    assert(!out.page_limit_reached && !out.source_truncated);
    size_t retained = 0;
    for (unsigned i = 0; i < out.page_count; ++i) {
        reset_text_page(&out, i, zero_pixels, NULL, 178, page);
        for (const char *p = page; *p; ++p) if (*p != '\n') { assert(*p == 'x'); ++retained; }
    }
    assert(retained == RESET_TEXT_MAX_BYTES);
    memset(input, '\n', 280); input[280] = '\0';
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    assert(out.page_count == 40 && !out.source_truncated && !out.page_limit_reached); audit(&out, 178);
    memset(input, '\n', RESET_TEXT_MAX_BYTES); input[RESET_TEXT_MAX_BYTES] = '\0';
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    assert(out.page_count == RESET_TEXT_MAX_PAGES && !out.page_limit_reached); audit(&out, 178);
    memset(input, 'W', RESET_TEXT_MAX_BYTES + 1); input[RESET_TEXT_MAX_BYTES + 1] = '\0';
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    page_text(&out, out.page_count - 1, 178, page);
    assert(out.source_truncated && strstr(page, "...")); audit(&out, 178);
    reset_text_layout("A\r\nB\tC", sizeof("A\r\nB\tC"), false, pixels, NULL, 178, &out);
    assert(!strcmp(out.summary, "A\nB C") && !out.glyph_substituted);
    reset_text_layout("A🙂龘B", sizeof("A🙂龘B"), false, pixels, NULL, 178, &out);
    assert(out.glyph_substituted && !strcmp(out.summary, "A??B")); audit(&out, 178);
    reset_text_layout("中中中中", sizeof("中中中中"), false, pixels, NULL, 32, &out);
    assert(!strcmp(out.summary, "中中\n中中") && !out.glyph_substituted); audit(&out, 32);
    reset_text_layout("", 1, false, pixels, NULL, 178, &out);
    assert(out.page_count == 1 && !out.summary[0] && !out.source_truncated);
    reset_text_layout("short", 6, true, pixels, NULL, 178, &out);
    assert(out.source_truncated && !strcmp(out.summary, "short..."));
    reset_text_layout("AVAVAV AVAVAV", sizeof("AVAVAV AVAVAV"), false, pixels, NULL, 45, &out); audit(&out, 45);
    memset(input, 'x', RESET_TEXT_MAX_BYTES - 1);
    memcpy(input + RESET_TEXT_MAX_BYTES - 1, "中", sizeof("中"));
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    assert(out.source_truncated && out.glyph_substituted); audit(&out, 178);
    char fitted[40];
    assert(reset_text_fit("WWWWWWWWWWWWWWWW", 17, 1, pixels, NULL, 50, fitted, sizeof(fitted)));
    assert(strstr(fitted, "...")); glyph_widths(fitted, 50);
    for (unsigned length = 0; length <= RESET_TEXT_MAX_BYTES + 1; ++length) {
        memset(input, 'W', length); input[length] = '\0';
        reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out); audit(&out, 178);
    }
    page_text(&out, RESET_TEXT_MAX_PAGES, 178, page); assert(!page[0]);
    puts("Reset bounded pixel text tests: PASS");
    return 0;
}
