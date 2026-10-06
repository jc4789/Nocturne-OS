/* TrueType text (libc/font.c, using stb_truetype): antialiased, any size, with a glyph cache.
   Sizes are in pixels per em, as CSS font-size. Inter (regular, bold, italic, bold italic) is in
   /usr/share/fonts and is what font_ui() returns. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

typedef struct font font_t;

enum { FONT_REGULAR = 0, FONT_BOLD = 1, FONT_ITALIC = 2, FONT_BOLD_ITALIC = 3 };

font_t *font_open(const char *path); /* NULL if the file is missing or not a TrueType font */
font_t *font_ui(int style);          /* Inter; loaded once. NULL if the font files are missing */

/* line metrics in pixels for size px: ascent above the baseline, descent below it (positive),
   and the recommended gap between lines */
void font_metrics(font_t *f, float px, float *ascent, float *descent, float *line_gap);
bool font_has(font_t *f, uint32_t codepoint);
float font_advance(font_t *f, float px, uint32_t codepoint);
float font_width(font_t *f, float px, const char *utf8, size_t n);
/* draw n bytes of UTF-8 with the baseline at y; returns the x after the text */
float font_draw(canvas_t *c, font_t *f, float px, float x, int y, const char *utf8, size_t n, uint32_t color);
