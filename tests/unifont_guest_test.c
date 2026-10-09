/* Real guest bitmap table smoke check, Unicode kernel title and console display. */
#include <stdio.h>
#include <string.h>
#include "nocturne.h"
#include "unifont.h"

static void bitmap_text(canvas_t *c, int x, int y, const char *s, int scale) {
    while (*s) {
        uint32_t cp;
        s += gfx_utf8_decode(s, &cp);
        cp = gfx_glyph_for(cp);
        for (int j = 0; j < 16; j++) {
            unsigned bits = (unifont_rows[cp][j * 2] << 8) | unifont_rows[cp][j * 2 + 1];
            for (int i = 0; i < unifont_widths[cp]; i++)
                if (bits & (0x8000u >> i)) gfx_fill(c, x + i * scale, y + j * scale, scale, scale, RGB(244, 239, 220));
        }
        x += unifont_widths[cp] * scale;
    }
}

int main(int argc, char **argv) {
    unsigned total = 0, narrow = 0, wide = 0;
    for (unsigned cp = 0; cp < UNIFONT_CODEPOINTS; cp++) {
        total += unifont_widths[cp] != 0;
        narrow += unifont_widths[cp] == 8;
        wide += unifont_widths[cp] == 16;
    }
    if (total != 57086 || narrow != 7309 || wide != 49777 || gfx_glyph_for(0x65E5) != 0x65E5) {
        printf("unifont_guest: FAIL table %u/%u/%u\n", total, narrow, wide);
        return 1;
    }
    const char *lines[] = {
        "Unifont Japanese 18.0.01 / 57086 glyphs",
        "日本語：ひらがな カタカナ 漢字",
        "中文：简体中文 繁體中文",
        "한국어: 한글 / Ελληνικά / Кириллица",
        "Symbols: ← ↑ → ↓ ┌─┐ █ ★ ☆ ☾ ✓ ✗",
    };
    if (argc > 1 && !strcmp(argv[1], "console")) {
        int fd = open("/dev/console", O_WRONLY);
        if (fd < 0) return 1;
        write(fd, "\x1B[2J\x1B[1;1H", 10);
        for (unsigned i = 0; i < sizeof lines / sizeof *lines; i++) {
            /* Deliberately split each UTF-8 sequence across system calls. */
            for (const char *p = lines[i]; *p; p++) write(fd, p, 1);
            write(fd, "\n", 1);
        }
        write(fd, "ABC日本語D\n", strlen("ABC日本語D\n"));
        close(fd);
    } else {
        window_t *w = win_open(740, 340, "日本語 中文 한글 — Unifont 全字形", WIN_RESIZABLE);
        if (!w) return 1;
        win_move(w, 160, 180);
        gfx_fill(&w->c, 0, 0, w->w, w->h, RGB(24, 22, 42));
        for (unsigned i = 0; i < sizeof lines / sizeof *lines; i++) bitmap_text(&w->c, 20, 16 + i * 26, lines[i], 1);
        bitmap_text(&w->c, 20, 165, "日本語 中文 한글", 2);
        bitmap_text(&w->c, 20, 215, "Aa 0123456789 ←→ ★☆", 2);
        /* Body uses the exact embedded bitmap; window decorations are drawn by the kernel. */
        win_update(w);
        window_t *short_title = win_open(180, 60, "日本語の長い題名の省略を確認", WIN_RESIZABLE);
        if (!short_title) return 1;
        win_move(short_title, 940, 180);
        gfx_fill(&short_title->c, 0, 0, short_title->w, short_title->h, RGB(24, 22, 42));
        win_update(short_title);
    }
    printf("unifont_guest: glyphs=%u narrow=%u wide=%u, 0 failures\n", total, narrow, wide);
    msleep(60000);
    return 0;
}
