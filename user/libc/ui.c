/* A few shared widgets so that apps look alike. */
#include <string.h>
#include "nocturne.h"

bool ui_hit(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }

void ui_text_center(canvas_t *c, int x, int y, int w, const char *s, uint32_t col, int font) {
    int tw = gfx_text_width(s, font);
    gfx_text(c, x + (w - tw) / 2, y, s, col, TRANSPARENT, font);
}

void ui_button(canvas_t *c, int x, int y, int w, int h, const char *label, bool hot, bool pressed) {
    uint32_t top = pressed ? RGB(70, 60, 140) : hot ? RGB(96, 84, 170) : RGB(66, 60, 116);
    uint32_t bot = pressed ? RGB(90, 80, 170) : hot ? RGB(72, 62, 140) : RGB(48, 44, 88);
    gfx_fill_round(c, x, y, w, h, 6, RGB(24, 22, 40));
    for (int j = 1; j < h - 1; j++) {
        int in = (j < 3 || j > h - 4) ? 2 : 1;
        gfx_hline(c, x + in, y + j, w - 2 * in, gfx_mix(top, bot, j * 255 / h));
    }
    int fh = gfx_font_h(FONT_SMALL);
    ui_text_center(c, x, y + (h - fh) / 2 + (pressed ? 1 : 0), w, label, UI_FG, FONT_SMALL);
}

void ui_textfield(canvas_t *c, int x, int y, int w, const char *text, bool focus) {
    int h = 24;
    gfx_fill_round(c, x, y, w, h, 5, focus ? UI_ACCENT : RGB(70, 66, 110));
    gfx_fill_round(c, x + 1, y + 1, w - 2, h - 2, 4, RGB(18, 16, 32));
    int maxc = (w - 12) / 8;
    int len = (int)strlen(text);
    const char *s = len > maxc ? text + (len - maxc) : text;
    int end = x + 6 + gfx_text(c, x + 6, y + 4, s, UI_FG, TRANSPARENT, FONT_SMALL);
    if (focus) gfx_fill(c, end + 1, y + 4, 2, 16, UI_ACCENT2);
}

int ui_edit_key(char *buf, size_t cap, const struct gui_event *e) {
    if (e->type != EV_KEY || !e->pressed) return 0;
    size_t len = strlen(buf);
    if (e->key == NKEY_ENTER) return 1;
    if (e->key == NKEY_ESC) return -1;
    if (e->key == NKEY_BACKSPACE) {
        if (len) buf[len - 1] = 0;
        return 0;
    }
    if (e->key >= 32 && e->key < 127 && !(e->mods & NMOD_CTRL) && len + 1 < cap) {
        buf[len] = (char)e->key;
        buf[len + 1] = 0;
    }
    return 0;
}
