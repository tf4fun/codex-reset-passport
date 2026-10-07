#pragma once

/* Storage budget for 280 Unicode codepoints, each at most four UTF-8 bytes.
 * X's weighted count is different (notably joined emoji and expanded URLs);
 * this reader has a byte budget, not a post-composition validator. */
#define RESET_TEXT_MAX_CODEPOINTS 280U
#define RESET_TEXT_MAX_BYTES (RESET_TEXT_MAX_CODEPOINTS * 4U)
