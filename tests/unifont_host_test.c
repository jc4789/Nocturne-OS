/* Host verification of the actual kernel bitmap renderer and console parser. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "unifont.h"
#include "../kernel/src/dev/fbcon.c"

struct fb_info fb;
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
uint64_t pmm_alloc_contig(size_t pages) { return (uint64_t)(uintptr_t)calloc(pages, 4096); }
void *kzalloc(size_t n) { return calloc(1, n); }
size_t klog_read(char *out, size_t n) { (void)out; (void)n; return 0; }
void fb_present(const uint32_t *p, int pitch, int x, int y, int w, int h) {
    (void)p; (void)pitch; (void)x; (void)y; (void)w; (void)h;
}

static void renderer(void) {
    uint32_t pixels[70 * 40];
    canvas_t canvas;
    gfx_init(&canvas, pixels, 64, 40, 70);
    unsigned glyphs = 0;
    for (uint32_t cp = 0; cp < UNIFONT_CODEPOINTS; cp++) {
        if (!unifont_widths[cp]) continue;
        glyphs++;
        CHECK(gfx_glyph_for(cp) == cp);
        for (int scale = 1; scale <= 2; scale++) {
            int font = scale == 2 ? FONT_LARGE : FONT_SMALL;
            int width = unifont_widths[cp] * scale;
            for (unsigned i = 0; i < sizeof pixels / sizeof *pixels; i++) pixels[i] = 0x12345678;
            CHECK(gfx_codepoint_width(cp, font) == width);
            CHECK(gfx_char(&canvas, 2, 3, cp, 0xFFFFFFFF, 0xFF000000, font) == width);
            for (int y = 0; y < 40; y++) for (int x = 0; x < 70; x++) {
                uint32_t expected = 0x12345678;
                if (y >= 3 && y < 3 + 16 * scale && x >= 2 && x < 2 + width) {
                    int row = ((y - 3) / scale) * 2;
                    uint16_t bits = (uint16_t)((unifont_rows[cp][row] << 8) | unifont_rows[cp][row + 1]);
                    expected = bits & (0x8000u >> ((x - 2) / scale)) ? 0xFFFFFFFF : 0xFF000000;
                }
                CHECK(pixels[y * 70 + x] == expected);
            }
        }
    }
    CHECK(glyphs == 57086);
    CHECK(gfx_glyph_for(0x10000) == 0xFFFD);
    CHECK(gfx_glyph_for(0xFFFFFFFF) == 0xFFFD);
    CHECK(gfx_glyph_for(0xD800) == 0xFFFD);
    const char *mixed = "A\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E!";
    CHECK(gfx_text_width(mixed, FONT_SMALL) == 64);
    CHECK(gfx_text(&canvas, 0, 0, mixed, 0xFFFFFFFF, 0, FONT_SMALL) == 64);
    CHECK(gfx_text_width(mixed, FONT_LARGE) == 128);
    CHECK(gfx_text(&canvas, 0, 0, mixed, 0xFFFFFFFF, 0, FONT_LARGE) == 128);
    CHECK(gfx_text_width("AA\nA", FONT_SMALL) == 16);
    uint32_t cp;
    CHECK(gfx_utf8_decode("\xE6\x97\xA5", &cp) == 3 && cp == 0x65E5);
    const char *invalid[] = {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xE6", "\xE6\x97"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++)
        CHECK(gfx_utf8_decode(invalid[i], &cp) == 1 && cp == 0xFFFD);
    for (unsigned i = 0; i < sizeof pixels / sizeof *pixels; i++) pixels[i] = 0x12345678;
    gfx_clip(&canvas, 2, 2, 5, 7);
    CHECK(gfx_char(&canvas, -3, -2, 0x65E5, 0xFFFFFFFF, TRANSPARENT, FONT_LARGE) == 32);
    for (int y = 0; y < 40; y++) for (int x = 0; x < 70; x++) {
        uint32_t expected = 0x12345678;
        if (x >= 2 && x < 7 && y >= 2 && y < 9) {
            int row = ((y + 2) / 2) * 2;
            uint16_t bits = (uint16_t)((unifont_rows[0x65E5][row] << 8) | unifont_rows[0x65E5][row + 1]);
            if (bits & (0x8000u >> ((x + 3) / 2))) expected = 0xFFFFFFFF;
        }
        CHECK(pixels[y * 70 + x] == expected);
    }
}

static void console(void) {
    fb.width = 56; fb.height = 140;
    fbcon_init();
    CHECK(ready && cell_spans && cols == 7 && rows == 4);
    fbcon_clear();
    fbcon_write("\xE6", 1); fbcon_write("\x97", 1); fbcon_write("\xA5", 1);
    CHECK(cx == 2 && cy == 0 && cell_spans[0] == 2 && cell_spans[1] == 3);
    fbcon_write("\xE6\x9C\xAC\xE8\xAA\x9E" "A", 7);
    CHECK(cx == 7 && cy == 0);
    fbcon_write("\xE4\xB8\xAD", 3);
    CHECK(cx == 2 && cy == 1);
    fbcon_write("\bA", 2);
    CHECK(cx == 1 && cell_spans[cols] == 1 && cell_spans[cols + 1] == 0);
    fbcon_clear();
    fbcon_write("\xE6" "A", 2);
    CHECK(cx == 2 && !utf8_need);
    fbcon_write("\xC0\xAF", 2);
    CHECK(cx == 4 && !utf8_need);
    fbcon_clear();
    fbcon_write("\xE6\x97\xA5", 3);
    fbcon_write("\x1B[1;2H\x1B[K", 9);
    CHECK(cell_spans[0] == 0 && cell_spans[1] == 0);
    fbcon_clear();
    fbcon_write("\t", 1);
    CHECK(cx == 1 && cy == 1); /* 7-column wrap, then the existing 8-cell tab stop. */
    fbcon_clear();
    for (int i = 0; i < 20; i++) fbcon_write("\xE6\x97\xA5\n", 4);
    CHECK(cy == rows - 1 && cx == 0);
    for (int y = 0; y < rows - 1; y++) CHECK(cell_spans[y * cols] == 2 && cell_spans[y * cols + 1] == 3);
    for (int x = 0; x < cols; x++) CHECK(cell_spans[(rows - 1) * cols + x] == 0);
}

int main(void) {
    renderer(); console();
    printf("Unifont kernel renderer/console: %u checks, 0 failures\n", checks);
    return 0;
}
