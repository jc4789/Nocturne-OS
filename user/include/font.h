/* TrueType text (libc/font.c, using stb_truetype): antialiased, any size, with a glyph cache.
   Sizes are in pixels per em, as CSS font-size. font_ui() uses bundled Maple Mono NF
   (regular, bold, italic, bold italic). Bundled families share lazy missing-glyph fallbacks. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

typedef struct font font_t;

enum { FONT_REGULAR = 0, FONT_BOLD = 1, FONT_ITALIC = 2, FONT_BOLD_ITALIC = 3 };
enum { FONT_FAMILY_SANS = 0, FONT_FAMILY_SERIF = 1, FONT_FAMILY_MONO = 2 };

font_t *font_open(const char *path); /* NULL if the file is missing or not a TrueType font */
/* Copies a byte-bounded, validated SFNT TrueType face. CFF/WOFF/WOFF2 and
   malformed outlines fail, not an empty successful face. Caller owns it. */
font_t *font_open_memory(const void *data, size_t length);
void font_close(font_t *font); /* only caller-owned faces, never bundled caches */
font_t *font_ui(int style);          /* Maple Mono; loaded once. NULL if the files are missing */
/* Inter / Noto Serif Living Regular / Maple Mono。統合 Noto は四ファイルだけで
   各 Regular face を共有し、実 bold/italic は持たない。Sans は Inter の指定
   style/Regular、欠落時は Sans Living。未知 family は Sans、全欠落は NULL。
   同梱統合 Noto の行箱は Inter Regular の metrics（欠落時は既定値）を使う。
   欠落 CJK は本文 family 用の日本語 Regular OTF、CN、Plangothic の順。
   UI/Mono はその OTF を使わず、Maple の主書体/CN 優先順を保つ。
   font_open は単一 face のまま。この API は Web font / shaping を追加しない。 */
font_t *font_family(int family, int style);

/* line metrics in pixels for size px: ascent above the baseline, descent below it (positive),
   and the recommended gap between lines */
void font_metrics(font_t *f, float px, float *ascent, float *descent, float *line_gap);
bool font_has(font_t *f, uint32_t codepoint);
float font_advance(font_t *f, float px, uint32_t codepoint);
float font_width(font_t *f, float px, const char *utf8, size_t n);
/* Read-only identity for the currently opened native faces; 0 disables cache
 * reuse after uint64_t generation exhaustion. */
uint64_t font_metrics_generation(void);
/* draw n bytes of UTF-8 with the baseline at y; returns the x after the text */
float font_draw(canvas_t *c, font_t *f, float px, float x, int y, const char *utf8, size_t n, uint32_t color);
/* Fixed-cell application chrome; -1 requests the bitmap emergency fallback. */
int font_cell_width(uint32_t codepoint, int size);
int font_cell_draw(canvas_t *c, int x, int y, uint32_t cp, uint32_t fg, uint32_t bg, int size);
/* Native renderer snapshot probes: cache hits only; no allocation or flush.
 * end returns false if any unavailable font/glyph was requested. */
void font_probe_begin(void);
bool font_probe_end(void);
