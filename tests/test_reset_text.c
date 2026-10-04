#include "reset_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int pixels(void *context, uint32_t cp, uint32_t next) {
    (void)context;
    if (cp > 0x7e && cp != 0x4e2d) return -1;
    if (cp == 'W') return 14;
    if (cp == 'i') return 3;
    if (cp == 'A' && next == 'V') return 6; /* Exercise a kerned pair. */
    return cp == 0x4e2d ? 16 : cp == '.' ? 3 : 8;
}
static unsigned rows(const char *text) {
    unsigned count = 1;
    while (*text) if (*text++ == '\n') ++count;
    return count;
}
static void ascii_widths(const char *text, unsigned width) {
    unsigned x = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') { assert(x <= width); x = 0; }
        else x += (unsigned)pixels(NULL, (unsigned char)*p, p[1] == '\n' ? 0 : (unsigned char)p[1]);
    }
    assert(x <= width);
}
static void audit(const reset_text_layout_t *out, unsigned width) {
    assert(out->page_count > 0 && out->page_count <= RESET_TEXT_MAX_PAGES);
    assert(rows(out->summary) <= RESET_TEXT_SUMMARY_LINES);
    ascii_widths(out->summary, width);
    for (unsigned i = 0; i < out->page_count; ++i) {
        assert(rows(out->pages[i]) <= RESET_TEXT_READING_LINES);
        ascii_widths(out->pages[i], width);
    }
}
int main(void) {
    reset_text_layout_t out;
    reset_text_layout("Hello Codex", sizeof("Hello Codex"), false, pixels, NULL, 178, &out);
    assert(!strcmp(out.summary, "Hello Codex") && out.page_count == 1);
    assert(!out.source_truncated && !out.glyph_substituted);
    char input[400];
    memset(input, 'W', 256); input[256] = '\0';
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    assert(out.page_count == 4 && out.summary_shortened && !out.source_truncated);
    audit(&out, 178);
    char restored[257] = {0}; size_t n = 0;
    for (unsigned page = 0; page < out.page_count; ++page)
        for (const char *p = out.pages[page]; *p; ++p) if (*p != '\n') restored[n++] = *p;
    assert(n == 256 && !strcmp(input, restored)); /* Unbroken words are never lost. */
    memset(input, 'i', 256);
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    assert(out.page_count == 1); /* Same count, actual unequal pixel advances. */
    audit(&out, 178);
    memset(input, 'W', 300); input[300] = '\0';
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    assert(out.source_truncated && strstr(out.pages[out.page_count - 1], "..."));
    audit(&out, 178);
    reset_text_layout("A\r\nB\tC", sizeof("A\r\nB\tC"), false, pixels, NULL, 178, &out);
    assert(!strcmp(out.summary, "A\nB C") && !out.glyph_substituted);
    reset_text_layout("A🙂龘B", sizeof("A🙂龘B"), false, pixels, NULL, 178, &out);
    assert(out.glyph_substituted && !strcmp(out.summary, "A??B"));
    audit(&out, 178);
    reset_text_layout("中中中中", sizeof("中中中中"), false, pixels, NULL, 32, &out);
    assert(!strcmp(out.summary, "中中\n中中") && !out.glyph_substituted);
    memset(input, '\n', 255); input[255] = '\0';
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    assert(out.page_count == 8 && out.page_limit_reached);
    assert(strstr(out.pages[7], "...")); audit(&out, 178);
    reset_text_layout("", 1, false, pixels, NULL, 178, &out);
    assert(out.page_count == 1 && !out.summary[0] && !out.source_truncated);
    reset_text_layout("short", 6, true, pixels, NULL, 178, &out);
    assert(out.source_truncated && !strcmp(out.summary, "short..."));
    reset_text_layout("AVAVAV AVAVAV", sizeof("AVAVAV AVAVAV"), false, pixels, NULL, 45, &out);
    audit(&out, 45);
    memset(input, 'x', 255); input[255] = (char)0xe4; input[256] = (char)0xb8; input[257] = (char)0xad; input[258] = '\0';
    reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
    assert(out.source_truncated && out.glyph_substituted); audit(&out, 178);
    char fitted[40];
    assert(reset_text_fit("WWWWWWWWWWWWWWWW", 17, 1, pixels, NULL, 50, fitted, sizeof(fitted)));
    assert(strstr(fitted, "...")); ascii_widths(fitted, 50);
    for (unsigned length = 0; length <= 300; ++length) {
        memset(input, 'W', length); input[length] = '\0';
        reset_text_layout(input, sizeof(input), false, pixels, NULL, 178, &out);
        audit(&out, 178);
    }
    puts("Reset bounded pixel text tests: PASS");
    return 0;
}
