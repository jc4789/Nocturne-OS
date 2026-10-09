/* Unifont Japanese 18.0.01 bitmap data: OFL-1.1.
 * Copyright and license: third_party/unifont/NOTICE.txt and OFL-1.1.txt.
 * All source BDF glyphs are retained, including both 8px and 16px widths.
 * Missing BMP entries have width 0. Rows are 16-bit, MSB-first, left-aligned.
 */
#pragma once
#include <stdint.h>
#define UNIFONT_CODEPOINTS 0x10000u
#define UNIFONT_HEIGHT 16
extern const uint8_t unifont_widths[UNIFONT_CODEPOINTS];
extern const uint8_t unifont_rows[UNIFONT_CODEPOINTS][UNIFONT_HEIGHT * 2];
