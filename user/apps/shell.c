/* シェル：デスクトップの絵と印、タスクバー、開始メニュー、音量、通知。普通のプロセスで、セッションが
   利用者の身元で起こす（user/apps/session.c）。

   窓の管理と合成はカーネルが持つ。シェルは N_CAP_SHELL で自分の窓を層に置き（display_abi.h）、窓の一覧と
   焦点、画面、音、電源の段取りを watch で聞いて描き直す。電源の変更は機械のサービスへ、音量の変更は設定の
   持ち主へ頼む。サービス "shell" のチャンネルでほかのプログラムの通知を受ける。

   スレッドは一つ：事象の待ち行列を待ち、届いたものを順に処理する。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "nocturne_settings.h"
#include "display.h"
#include "gfx.h"
#include "settings_abi.h"
#include "shell_art.h"

#define OS_NAME "Nocturne"
#define MAX_OF(a, b) ((a) > (b) ? (a) : (b))
#define MIN_OF(a, b) ((a) < (b) ? (a) : (b))

enum {
    TOKEN_WINDOWS = 1, TOKEN_SCREEN, TOKEN_SOUND, TOKEN_SOUND_SETTING, TOKEN_ZONE, TOKEN_POWER, TOKEN_CLOCK, TOKEN_TOAST_TIME, TOKEN_LOGOUT_TIME,
    TOKEN_INLET, TOKEN_BAR, TOKEN_MENU, TOKEN_VOLUME, TOKEN_TOAST,
    TOKEN_DESK = 0x1000,   /* その後ろに画面の番号 */
};
#define TOKEN_PEER (1ull << 32) /* その後ろに通知の相手の番号 */

#define TOAST_MS 4000u
#define HINT_MS 3000u           /* 窓の題と抜け方が代わる代わる出る */
#define LOGOUT_STALL_MS 15000u  /* 窓が一つも閉じないまま、これだけ経つとログアウトをやめる */
#define DOUBLE_CLICK_NS (450 * N_NS_PER_MS)

static n_handle cap, events;

struct rectangle { int x, y, w, h; };
static bool in_box(int x, int y, struct rectangle r) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* ---- 小さな描画の部品 ---- */
static void draw_crescent(canvas_t *c, int cx, int cy, int r, uint32_t col) {
    int ox = r * 5 / 10, oy = -r * 3 / 10;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r * r) continue;
            int ex = x - ox, ey = y - oy;
            if (ex * ex + ey * ey < r * r * 85 / 100) continue;
            gfx_pixel(c, cx + x, cy + y, col);
        }
}

static void text_cut(canvas_t *c, int x, int y, const char *s, int maxw, uint32_t col) {
    char tmp[96];
    if (maxw <= 0) return;
    int i = 0, width = 0;
    while (s[i]) {
        uint32_t cp;
        int n = gfx_utf8_decode(s + i, &cp), w = gfx_codepoint_width(cp, FONT_SMALL);
        if (i + n >= (int)sizeof tmp || width + w > maxw) break;
        i += n;
        width += w;
    }
    bool cut = s[i] != 0;
    if (cut) {
        int ellipsis = gfx_codepoint_width(0x2026, FONT_SMALL);
        if (ellipsis > maxw) return;
        int end = i;
        i = width = 0;
        while (i < end) {
            uint32_t cp;
            int n = gfx_utf8_decode(s + i, &cp), w = gfx_codepoint_width(cp, FONT_SMALL);
            if (width + w + ellipsis > maxw || i + n + 3 >= (int)sizeof tmp) break;
            i += n;
            width += w;
        }
    }
    memcpy(tmp, s, (size_t)i);
    if (cut) {
        memcpy(tmp + i, "\xE2\x80\xA6", 3);
        i += 3;
    }
    tmp[i] = 0;
    gfx_text(c, x, y, tmp, col, TRANSPARENT, FONT_SMALL);
}

/* 深い面の上に 8 bit で描いた所：変わった画素だけ 8 bit の内容に印を付ける。 */
static void deep_marked(const canvas_t *c, uint8_t *deep, const uint32_t *before, struct rectangle r) {
    for (int y = 0; y < r.h; y++) {
        const uint32_t *now = c->px + (size_t)(r.y + y) * c->pitch + r.x, *was = before + (size_t)y * r.w;
        uint8_t *d = deep + (size_t)(r.y + y) * c->pitch + r.x;
        for (int x = 0; x < r.w; x++)
            if (now[x] != was[x]) d[x] = 0;
    }
}

/* r を c の中へ切る。何も残らなければ false。 */
static bool box_clip(struct rectangle *r, const canvas_t *c) {
    int x0 = MAX_OF(r->x, 0), y0 = MAX_OF(r->y, 0), x1 = MIN_OF(r->x + r->w, c->w), y1 = MIN_OF(r->y + r->h, c->h);
    if (x1 <= x0 || y1 <= y0) return false;
    *r = (struct rectangle){x0, y0, x1 - x0, y1 - y0};
    return true;
}

/* ---- 何を起こせるか ---- */
enum action { RUN, SLEEP, HIBERNATE, LOGOUT, RESTART, SHUT_DOWN };
struct app {
    const char *name;
    const struct n_program *program; /* RUN だけ */
    int icon;
    enum action action;
};
static const struct app power_items[] = {
    {"Sleep", NULL, N_ICON_SLEEP, SLEEP},
    {"Hibernate", NULL, N_ICON_HIBERNATE, HIBERNATE},
    {"Log Out", NULL, N_ICON_LOGOUT, LOGOUT},
    {"Restart", NULL, N_ICON_REBOOT, RESTART},
    {"Shut Down", NULL, N_ICON_POWER, SHUT_DOWN},
};
#define POWER_ITEMS ((int)(sizeof power_items / sizeof *power_items))
/* 台帳の写し（サービス programs）。menu_apps と desktop_apps は programs_held を指す。 */
static struct n_program *programs_held;
static struct app *menu_apps, *desktop_apps;
static int menu_app_count, desktop_app_count;

static void toast(const char *title, const char *text, unsigned ms);
static void logout_begin(void);
static void desktop_changed(void);

static void programs_read(void) {
    struct n_program *list;
    n_result count = n_programs_list(&list);
    if (count < 0) return; /* 前の写しで続ける。無ければ電源の項目だけ。 */
    struct app *menu = calloc((size_t)count + 1, sizeof *menu), *desk = calloc((size_t)count + 1, sizeof *desk);
    if (!menu || !desk) {
        free(menu);
        free(desk);
        free(list);
        return;
    }
    int menu_n = 0, desk_n = 0;
    for (n_result i = 0; i < count; i++) {
        struct app a = {list[i].name, &list[i], (int)list[i].icon, RUN};
        if (list[i].flags & N_PROGRAM_MENU) menu[menu_n++] = a;
        if (list[i].flags & N_PROGRAM_DESKTOP) desk[desk_n++] = a;
    }
    bool same = desk_n == desktop_app_count;
    for (int i = 0; same && i < desk_n; i++)
        same = desk[i].program->id == desktop_apps[i].program->id && desk[i].icon == desktop_apps[i].icon && !strcmp(desk[i].name, desktop_apps[i].name);
    free(menu_apps);
    free(desktop_apps);
    free(programs_held);
    programs_held = list;
    menu_apps = menu;
    desktop_apps = desk;
    menu_app_count = menu_n;
    desktop_app_count = desk_n;
    if (!same) desktop_changed();
}

static void perform(const struct app *a) {
    n_result r = 0;
    switch (a->action) {
    case RUN:
        r = n_program_run(a->program, NULL);
        if (r < 0) {
            char text[128];
            snprintf(text, sizeof text, "%s (%s)", a->name, n_result_text(r));
            toast("Could not start program", text, TOAST_MS);
        }
        return;
    case LOGOUT: logout_begin(); return;
    case SLEEP: r = n_machine_choose(N_MACHINE_SLEEP); break;
    case HIBERNATE: r = n_machine_choose(N_MACHINE_HIBERNATE); break;
    case RESTART: r = n_machine_choose(N_MACHINE_REBOOT); break;
    case SHUT_DOWN: r = n_machine_choose(N_MACHINE_POWER_OFF); break;
    }
    if (r < 0) toast("The power did not change", n_result_text(r), TOAST_MS);
}

/* ---- 画面ごとのデスクトップ ---- */
#define BAR_H 40
#define CELL_W 88
#define CELL_H 82
#define N_ICON_AREA_PAD 16

struct desk {
    window_t *win;
    int x, y, w, h;
    bool deep;
    /* 画面 0 の印の選択と、その下の画素 */
    int selected;
    struct rectangle under_box;
    uint32_t *under;
    uint8_t *under_deep;
    uint64_t last_click;
    int last_x, last_y;
};
static struct desk *desks;
static size_t desk_count;

static struct rectangle icon_cell(int h, int i) {
    int per_col = (h - BAR_H - 16) / CELL_H;
    if (per_col < 1) per_col = 1;
    return (struct rectangle){12 + (i / per_col) * CELL_W, 12 + (i % per_col) * CELL_H, CELL_W - 8, CELL_H - 6};
}

static void icons_draw(struct desk *d) {
    canvas_t *c = &d->win->c;
    uint8_t *deep = win_deep(d->win);
    const int stride = CELL_W + 2 * N_ICON_AREA_PAD;
    uint32_t *saved = deep ? malloc((size_t)stride * CELL_H * 4) : NULL;
    for (int i = 0; i < desktop_app_count; i++) {
        const struct app *a = &desktop_apps[i];
        struct rectangle cell = icon_cell(d->h, i), box = {cell.x - N_ICON_AREA_PAD, cell.y, stride, CELL_H};
        bool track = saved && box_clip(&box, c);
        for (int y = 0; track && y < box.h; y++)
            memcpy(saved + (size_t)y * box.w, c->px + (size_t)(box.y + y) * c->pitch + box.x, (size_t)box.w * 4);
        art_icon(c, cell.x + (cell.w - 32) / 2, cell.y + 8, a->icon);
        const char *label = a->name;
        int tx = cell.x + (cell.w - gfx_text_width(label, FONT_SMALL)) / 2;
        gfx_text(c, tx + 1, cell.y + 49, label, RGB(0, 0, 0), TRANSPARENT, FONT_SMALL);
        gfx_text(c, tx, cell.y + 48, label, RGB(235, 235, 250), TRANSPARENT, FONT_SMALL);
        if (track) deep_marked(c, deep, saved, box);
    }
    free(saved);
}

static void selection_set(struct desk *d, int icon) {
    if (icon == d->selected) return;
    canvas_t *c = &d->win->c;
    uint8_t *deep = win_deep(d->win);
    if (d->selected >= 0 && d->under) {
        struct rectangle r = d->under_box;
        for (int y = 0; y < r.h; y++) {
            memcpy(c->px + (size_t)(r.y + y) * c->pitch + r.x, d->under + (size_t)y * r.w, (size_t)r.w * 4);
            if (deep && d->under_deep)
                memcpy(deep + (size_t)(r.y + y) * c->pitch + r.x, d->under_deep + (size_t)y * r.w, (size_t)r.w);
        }
        win_update_rect(d->win, r.x, r.y, r.w, r.h);
    }
    d->selected = -1;
    if (icon < 0) return;
    struct rectangle r = icon_cell(d->h, icon);
    if (!box_clip(&r, c)) return;
    free(d->under);
    free(d->under_deep);
    d->under = malloc((size_t)r.w * r.h * 4);
    d->under_deep = deep ? malloc((size_t)r.w * r.h) : NULL;
    if (!d->under || (deep && !d->under_deep)) return;
    for (int y = 0; y < r.h; y++) {
        memcpy(d->under + (size_t)y * r.w, c->px + (size_t)(r.y + y) * c->pitch + r.x, (size_t)r.w * 4);
        if (deep) {
            memcpy(d->under_deep + (size_t)y * r.w, deep + (size_t)(r.y + y) * c->pitch + r.x, (size_t)r.w);
            memset(deep + (size_t)(r.y + y) * c->pitch + r.x, 0, (size_t)r.w);
        }
    }
    gfx_fill_blend(c, r.x, r.y, r.w, r.h, ARGB(70, 150, 140, 255));
    win_update_rect(d->win, r.x, r.y, r.w, r.h);
    d->under_box = r;
    d->selected = icon;
}

static void desk_paint(struct desk *d, size_t screen) {
    art_wallpaper(&d->win->c, win_deep(d->win));
    d->selected = -1;
    if (screen == 0) icons_draw(d);
    win_update(d->win);
}

static void desktop_changed(void) {
    if (desk_count) desk_paint(&desks[0], 0);
}

static n_result desk_place(struct desk *d, size_t screen) {
    struct n_shell_place place = {.layer = N_LAYER_DESKTOP, .screen = (uint32_t)screen};
    return n_shell_place(cap, d->win, &place);
}

static void desk_close(struct desk *d) {
    if (!d->win) return;
    win_close(d->win);
    d->win = NULL;
    free(d->under);
    free(d->under_deep);
    d->under = NULL;
    d->under_deep = NULL;
}

/* 画面 i の部分に合わせる。深さが変わった窓は作り直し、新しい絵が出てから古い窓を閉じる。 */
static void desk_fit(size_t i, const struct n_shell_screen *s, bool deep) {
    struct desk *d = &desks[i];
    bool same_kind = d->win && d->deep == deep;
    if (same_kind && d->w == s->width && d->h == s->height) {
        if (d->x != s->x || d->y != s->y) {
            d->x = s->x;
            d->y = s->y;
            desk_place(d, i);
        }
        return;
    }
    d->x = s->x;
    d->y = s->y;
    d->w = s->width;
    d->h = s->height;
    if (same_kind) {
        if (win_resize(d->win, s->width, s->height) >= 0) desk_paint(d, i);
        return;
    }
    window_t *old = d->win, *win;
    if (win_open(s->width, s->height, OS_NAME, WIN_NO_DECOR | (deep ? WIN_DEEP_COLOR : 0), &win) < 0) return;
    d->win = win;
    d->deep = deep;
    desk_place(d, i);
    desk_paint(d, i);
    n_events_unwatch(events, TOKEN_DESK + i);
    if (old) win_close(old);
    n_events_watch(events, win->handle, N_EVENT_RECEIVABLE, TOKEN_DESK + i);
}

/* ---- タスクバー ---- */
#define START_W 128
#define CLOCK_W 100
#define SPEAKER_W 40

static window_t *bar;
static int bar_w;
static bool speaker_hover;
static struct n_shell_window *windows;
static size_t windows_count, windows_room;
static struct {
    int32_t volume;
    bool muted;
    char *device; /* 音を出す装置の名。無ければ NULL。 */
} sound;

static window_t *menu, *volume;

static void windows_read(void) {
    for (;;) {
        n_result n = n_shell_windows(cap, windows, windows_room);
        if (n < 0) {
            windows_count = 0;
            return;
        }
        if ((size_t)n <= windows_room) {
            windows_count = (size_t)n;
            return;
        }
        struct n_shell_window *grown = realloc(windows, (size_t)n * sizeof *grown);
        if (!grown) {
            windows_count = windows_room;
            return;
        }
        windows = grown;
        windows_room = (size_t)n;
    }
}

static struct rectangle speaker_box(int w) { return (struct rectangle){w - CLOCK_W - SPEAKER_W, 5, SPEAKER_W - 4, BAR_H - 10}; }

static void button_box(int w, size_t n, size_t i, int *x, int *width) {
    int avail = w - START_W - CLOCK_W - SPEAKER_W - 16;
    int bw = n ? avail / (int)n : 0;
    if (bw > 190) bw = 190;
    *x = START_W + 8 + (int)i * bw;
    *width = bw - 4;
}

/* (cx, cy) を中心にスピーカー。音量に応じて波が一つから三つ、消音なら × 印。 */
static void draw_speaker(canvas_t *c, int cx, int cy, int level, bool muted, uint32_t col) {
    gfx_fill(c, cx - 9, cy - 3, 4, 7, col);
    for (int i = 0; i < 6; i++) gfx_vline(c, cx - 5 + i, cy - 3 - i, 7 + 2 * i, col);
    if (muted) {
        uint32_t x = RGB(240, 110, 120);
        for (int d = 0; d < 2; d++) {
            gfx_line(c, cx + 3 + d, cy - 4, cx + 10 + d, cy + 3, x);
            gfx_line(c, cx + 3 + d, cy + 3, cx + 10 + d, cy - 4, x);
        }
        return;
    }
    int waves = level == 0 ? 0 : level < 34 ? 1 : level < 67 ? 2 : 3;
    for (int w = 0; w < waves; w++) {
        int r = 4 + w * 4;
        for (int dy = -r * 7 / 10; dy <= r * 7 / 10; dy++) {
            int dx = 0;
            while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
            gfx_pixel(c, cx + 1 + dx, cy + dy, col);
            gfx_pixel(c, cx + 2 + dx, cy + dy, col);
        }
    }
}

static void bar_draw(void) {
    if (!bar) return;
    canvas_t *c = &bar->c;
    int w = bar->w;
    gfx_fill(c, 0, 0, w, BAR_H, ARGB(220, 14, 12, 32));
    gfx_hline(c, 0, 0, w, RGB(90, 76, 160));
    gfx_fill_round(c, 6, 5, START_W - 6, BAR_H - 10, 8, menu ? RGB(84, 68, 170) : RGB(44, 38, 90));
    draw_crescent(c, 24, BAR_H / 2, 9, RGB(246, 236, 196));
    gfx_text(c, 42, 12, OS_NAME, RGB(240, 240, 255), TRANSPARENT, FONT_SMALL);
    for (size_t i = 0; i < windows_count; i++) {
        const struct n_shell_window *b = &windows[i];
        bool focused = b->state & N_SHELL_WINDOW_FOCUSED, minimized = b->state & N_SHELL_WINDOW_MINIMIZED;
        int bx, bw;
        button_box(w, windows_count, i, &bx, &bw);
        gfx_fill_round(c, bx, 5, bw, BAR_H - 10, 6, focused ? RGB(70, 62, 140) : minimized ? RGB(26, 24, 48) : RGB(40, 36, 78));
        if (focused) gfx_hline(c, bx + 8, BAR_H - 6, bw - 16, RGB(170, 150, 255));
        text_cut(c, bx + 10, 12, b->title, bw - 20, minimized ? RGB(140, 140, 170) : RGB(235, 235, 250));
    }
    struct rectangle s = speaker_box(w);
    if (volume || speaker_hover) gfx_fill_round(c, s.x, s.y, s.w, s.h, 6, volume ? RGB(70, 62, 140) : RGB(40, 36, 78));
    draw_speaker(c, s.x + s.w / 2 - 1, BAR_H / 2, sound.volume, sound.muted, RGB(235, 235, 250));
    static const char *const wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char t1[16], t2[24];
    snprintf(t1, sizeof t1, "%02d:%02d", tm->tm_hour, tm->tm_min);
    snprintf(t2, sizeof t2, "%s %d %s", wd[tm->tm_wday % 7], tm->tm_mday, mo[tm->tm_mon % 12]);
    int cx = w - CLOCK_W;
    gfx_text(c, cx + (CLOCK_W - gfx_text_width(t1, FONT_SMALL)) / 2, 3, t1, RGB(245, 245, 255), TRANSPARENT, FONT_SMALL);
    gfx_text(c, cx + (CLOCK_W - gfx_text_width(t2, FONT_SMALL)) / 2, 20, t2, RGB(160, 160, 200), TRANSPARENT, FONT_SMALL);
    win_update(bar);
}

/* 次に分が変わる時に起きる。 */
static void clock_arm(void) {
    time_t now = time(NULL);
    n_events_unwatch(events, TOKEN_CLOCK);
    n_events_timer(events, n_clock_after((uint64_t)(60 - now % 60) * N_NS_PER_S), 0, TOKEN_CLOCK);
}

static void hotkey_keys(void) {
    n_shell_hotkey(cap, bar, NKEY_SUPER, 0);
    n_shell_hotkey(cap, bar, NKEY_ESC, NMOD_CTRL);
    n_shell_hotkey(cap, bar, 't', NMOD_CTRL | NMOD_ALT);
    n_shell_hotkey(cap, bar, 'T', NMOD_CTRL | NMOD_ALT);
}

static void bar_fit(const struct n_shell_screen *s) {
    if (!bar) {
        if (win_open(s->width, BAR_H, "Taskbar", WIN_NO_DECOR, &bar) < 0) return;
        hotkey_keys();
        n_events_watch(events, bar->handle, N_EVENT_RECEIVABLE, TOKEN_BAR);
    } else if (bar_w != s->width && win_resize(bar, s->width, BAR_H) < 0) {
        return;
    }
    bar_w = s->width;
    struct n_shell_place place = {.layer = N_LAYER_PANEL, .y = s->height - BAR_H, .reserve_edge = N_EDGE_BOTTOM,
                                  .reserve = BAR_H, .flags = N_PLACE_BLEND};
    n_shell_place(cap, bar, &place);
    bar_draw();
}

/* ---- 開始メニュー ---- */
#define MENU_W      260
#define MENU_HEAD   58
#define MENU_ITEM_H 36

static const struct app **menu_items;
static int menu_length, menu_first_power, menu_sel, menu_rows, menu_columns, menu_scroll, screen_w, screen_h;

static void menu_close(void) {
    if (!menu) return;
    n_events_unwatch(events, TOKEN_MENU);
    win_close(menu);
    menu = NULL;
    bar_draw();
}

static struct rectangle menu_item_box(int i) {
    return (struct rectangle){(i / menu_rows - menu_scroll) * MENU_W, MENU_HEAD + i % menu_rows * MENU_ITEM_H, MENU_W, MENU_ITEM_H};
}

static void menu_draw(void) {
    canvas_t *c = &menu->c;
    int w = menu->w, h = menu->h;
    gfx_fill(c, 0, 0, w, h, TRANSPARENT);
    gfx_fill_round(c, 0, 0, w, h, 10, RGB(70, 60, 120));
    gfx_fill_round(c, 1, 1, w - 2, h - 2, 9, RGB(22, 20, 44));
    for (int j = 0; j < MENU_HEAD - 6; j++) {
        int in = 0;
        if (j < 9) { /* 丸い角の中へ入れる */
            int d = 9 - j;
            while (in < 9 && (9 - in) * (9 - in) + d * d > 81) in++;
        }
        gfx_hline(c, 1 + in, 1 + j, w - 2 - 2 * in, gfx_mix(RGB(64, 44, 130), RGB(30, 26, 70), j * 255 / (MENU_HEAD - 6)));
    }
    draw_crescent(c, 30, 26, 13, RGB(246, 236, 196));
    gfx_text(c, 54, 10, OS_NAME, RGB(255, 255, 255), TRANSPARENT, FONT_LARGE);
    for (int i = 0; i < menu_length; i++) {
        struct rectangle b = menu_item_box(i);
        if (b.x < 0 || b.x >= w) continue;
        if (i == menu_first_power && b.y > MENU_HEAD) gfx_hline(c, b.x + 12, b.y - 1, MENU_W - 24, RGB(60, 56, 100));
        if (i == menu_sel) gfx_fill_round(c, b.x + 6, b.y + 1, MENU_W - 12, MENU_ITEM_H - 2, 6, RGB(76, 64, 150));
        art_icon(c, b.x + 12, b.y + 2, menu_items[i]->icon);
        gfx_text(c, b.x + 54, b.y + 10, menu_items[i]->name, RGB(230, 230, 245), TRANSPARENT, FONT_SMALL);
    }
    win_update(menu);
}

static void volume_close(void);

static void menu_open(void) {
    volume_close();
    struct n_power_state power = {0};
    n_power_state(cap, &power);
    programs_read();
    const struct app **items = realloc(menu_items, (size_t)(menu_app_count + POWER_ITEMS) * sizeof *items);
    if (!items) return;
    menu_items = items;
    /* 機械ができて、設定が許すものだけを出す。読めなければ機械の事実に従う。 */
    int32_t allow_sleep = 1, allow_hibernate = 1;
    n_setting_get_int(n_setting_at(N_DOMAIN_POWER, N_POWER_ALLOW_SLEEP), &allow_sleep);
    n_setting_get_int(n_setting_at(N_DOMAIN_POWER, N_POWER_ALLOW_HIBERNATE), &allow_hibernate);
    menu_length = 0;
    for (int i = 0; i < menu_app_count; i++) menu_items[menu_length++] = &menu_apps[i];
    menu_first_power = menu_length;
    for (int i = 0; i < POWER_ITEMS; i++) {
        if (power_items[i].action == SLEEP && (!power.can_sleep || !allow_sleep)) continue;
        if (power_items[i].action == HIBERNATE && (!power.can_hibernate || !allow_hibernate)) continue;
        menu_items[menu_length++] = &power_items[i];
    }
    /* 画面が短ければ二列目へ続ける */
    int fit = (screen_h - BAR_H - 8 - MENU_HEAD - 10) / MENU_ITEM_H;
    menu_columns = fit < 1 ? menu_length : (menu_length + fit - 1) / fit;
    menu_rows = (menu_length + menu_columns - 1) / menu_columns;
    /* 画面に入らない列はホイールと矢印で出す */
    menu_columns = (menu_length + menu_rows - 1) / menu_rows;
    menu_scroll = 0;
    int shown = MAX_OF(1, MIN_OF(menu_columns, (screen_w - 8) / MENU_W));
    int w = MENU_W * shown, h = MENU_HEAD + menu_rows * MENU_ITEM_H + 10;
    if (win_open(w, h, "Menu", WIN_NO_DECOR, &menu) < 0) {
        menu = NULL;
        return;
    }
    struct n_shell_place place = {.layer = N_LAYER_OVERLAY, .x = 4, .y = MAX_OF(0, screen_h - BAR_H - h - 4), .flags = N_PLACE_BLEND};
    n_shell_place(cap, menu, &place);
    menu_sel = -1;
    menu_draw();
    n_events_watch(events, menu->handle, N_EVENT_RECEIVABLE, TOKEN_MENU);
    bar_draw();
}

static int menu_item_at(int x, int y) {
    if (y < MENU_HEAD || y >= MENU_HEAD + menu_rows * MENU_ITEM_H || x < 0 || x >= menu->w) return -1;
    int i = (x / MENU_W + menu_scroll) * menu_rows + (y - MENU_HEAD) / MENU_ITEM_H;
    return i < menu_length ? i : -1;
}

static void menu_choose(int i) {
    const struct app *a = menu_items[i];
    menu_close();
    perform(a);
}

/* 選んだ項目の列を窓の中へ出す。 */
static void menu_reveal(int i) {
    int shown = menu->w / MENU_W, column = i / menu_rows;
    if (column < menu_scroll) menu_scroll = column;
    else if (column >= menu_scroll + shown) menu_scroll = column - shown + 1;
}

static void menu_event(const struct gui_event *e) {
    switch (e->type) {
    case EV_WHEEL: {
        int last = MAX_OF(0, menu_columns - menu->w / MENU_W);
        menu_scroll = MAX_OF(0, MIN_OF(last, menu_scroll + (e->wheel > 0 ? -1 : 1)));
        menu_sel = -1;
        menu_draw();
        break;
    }
    case EV_UNFOCUS:
    case EV_CLOSE: menu_close(); break;
    case EV_MOUSE_MOVE: {
        int i = menu_item_at(e->x, e->y);
        if (i != menu_sel) {
            menu_sel = i;
            menu_draw();
        }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (menu_sel >= 0) {
            menu_sel = -1;
            menu_draw();
        }
        break;
    case EV_MOUSE_DOWN: {
        int i = menu_item_at(e->x, e->y);
        if ((e->buttons & 1) && i >= 0) menu_choose(i);
        break;
    }
    case EV_KEY:
        if (!e->pressed) break;
        if (e->key == NKEY_ESC) menu_close();
        else if (e->key == NKEY_UP || e->key == NKEY_DOWN) {
            int d = e->key == NKEY_UP ? -1 : 1;
            menu_sel = menu_sel < 0 ? (d > 0 ? 0 : menu_length - 1) : (menu_sel + d + menu_length) % menu_length;
            menu_reveal(menu_sel);
            menu_draw();
        } else if (e->key == NKEY_ENTER && menu_sel >= 0) {
            menu_choose(menu_sel);
        }
        break;
    default: break;
    }
}

/* ---- 音量 ---- */
#define VOLUME_W 300
#define VOLUME_H 96

static bool slider_held;

static struct rectangle mute_box(void) { return (struct rectangle){12, 42, 40, 40}; }
static struct rectangle slider_box(void) { return (struct rectangle){70, 60, VOLUME_W - 70 - 56, 4}; }

/* 音量の値の変更はここだけ。 */
static void sound_put(uint32_t key, int32_t value) {
    n_result r = n_setting_set_int(n_setting_at(N_DOMAIN_AUDIO, key), value, NULL);
    if (r < 0) toast("The volume did not change", n_result_text(r), TOAST_MS);
}

static void volume_draw(void) {
    canvas_t *c = &volume->c;
    struct rectangle m = mute_box(), s = slider_box();
    gfx_fill(c, 0, 0, VOLUME_W, VOLUME_H, TRANSPARENT);
    gfx_fill_round(c, 0, 0, VOLUME_W, VOLUME_H, 10, RGB(70, 60, 120));
    gfx_fill_round(c, 1, 1, VOLUME_W - 2, VOLUME_H - 2, 9, RGB(22, 20, 44));
    gfx_text(c, 16, 12, "Volume", RGB(245, 245, 255), TRANSPARENT, FONT_SMALL);
    const char *device = sound.device ? sound.device : "No sound device";
    gfx_text(c, VOLUME_W - 16 - gfx_text_width(device, FONT_SMALL), 12, device, RGB(150, 150, 190), TRANSPARENT, FONT_SMALL);
    gfx_fill_round(c, m.x, m.y, m.w, m.h, 8, sound.muted ? RGB(90, 40, 60) : RGB(44, 38, 90));
    draw_speaker(c, m.x + m.w / 2 - 1, m.y + m.h / 2, sound.volume, sound.muted, RGB(235, 235, 250));
    int fill = s.w * sound.volume / 100, knob = s.x + fill;
    gfx_fill_round(c, s.x, s.y, s.w, s.h, 2, RGB(60, 56, 100));
    gfx_fill_round(c, s.x, s.y, MAX_OF(fill, 4), s.h, 2, sound.muted ? RGB(110, 100, 140) : RGB(150, 130, 255));
    gfx_fill_circle(c, knob, s.y + 2, 8, sound.muted ? RGB(150, 146, 170) : RGB(235, 230, 255));
    char number[8];
    snprintf(number, sizeof number, "%d", (int)sound.volume);
    gfx_text(c, VOLUME_W - 20 - gfx_text_width(number, FONT_SMALL), s.y - 6, number, RGB(235, 235, 250), TRANSPARENT, FONT_SMALL);
    win_update(volume);
}

static void volume_close(void) {
    if (!volume) return;
    n_events_unwatch(events, TOKEN_VOLUME);
    win_close(volume);
    volume = NULL;
    slider_held = false;
    bar_draw();
}

static void volume_open(void) {
    menu_close();
    if (win_open(VOLUME_W, VOLUME_H, "Volume", WIN_NO_DECOR, &volume) < 0) {
        volume = NULL;
        return;
    }
    struct n_shell_place place = {.layer = N_LAYER_OVERLAY, .x = screen_w - VOLUME_W - 8,
                                  .y = screen_h - BAR_H - VOLUME_H - 6, .flags = N_PLACE_BLEND};
    n_shell_place(cap, volume, &place);
    volume_draw();
    n_events_watch(events, volume->handle, N_EVENT_RECEIVABLE, TOKEN_VOLUME);
    bar_draw();
}

static void volume_tick(void) { sound_effect(880, 880, 70, SND_TRIANGLE); }

static void slider_follow(int x) {
    struct rectangle s = slider_box();
    int level = MAX_OF(0, MIN_OF(100, (x - s.x) * 100 / s.w));
    if (level != sound.volume) sound_put(N_AUDIO_VOLUME, level);
    if (sound.muted) sound_put(N_AUDIO_MUTED, 0);
}

static void volume_wheel(int wheel) {
    sound_put(N_AUDIO_VOLUME, MAX_OF(0, MIN_OF(100, sound.volume - wheel * 5)));
    volume_tick();
}

static void volume_event(const struct gui_event *e) {
    switch (e->type) {
    case EV_UNFOCUS:
    case EV_CLOSE: volume_close(); break;
    case EV_KEY:
        if (e->pressed && e->key == NKEY_ESC) volume_close();
        break;
    case EV_MOUSE_DOWN: {
        if (!(e->buttons & 1)) break;
        struct rectangle m = mute_box(), s = slider_box();
        if (in_box(e->x, e->y, m)) {
            sound_put(N_AUDIO_MUTED, !sound.muted);
            volume_tick();
        } else if (in_box(e->x, e->y, (struct rectangle){s.x - 10, s.y - 14, s.w + 20, s.h + 28})) {
            slider_held = true;
            slider_follow(e->x);
        }
        break;
    }
    case EV_MOUSE_MOVE:
        if (slider_held) slider_follow(e->x);
        break;
    case EV_MOUSE_UP:
        if (slider_held) volume_tick(); /* 新しい大きさを聞かせる */
        slider_held = false;
        break;
    case EV_WHEEL: volume_wheel(e->wheel); break;
    default: break;
    }
}

static void sound_read(void) {
    free(sound.device);
    sound.device = n_audio_output_name();
    int32_t value;
    if (n_setting_get_int(n_setting_at(N_DOMAIN_AUDIO, N_AUDIO_VOLUME), &value) >= 0) sound.volume = value;
    if (n_setting_get_int(n_setting_at(N_DOMAIN_AUDIO, N_AUDIO_MUTED), &value) >= 0) sound.muted = value != 0;
}

static void sound_changed(void) {
    sound_read();
    bar_draw();
    if (volume) volume_draw();
}

/* ---- タスクバーの入力 ---- */
static void bar_press(int x, int y) {
    if (x < START_W) {
        if (menu) menu_close();
        else menu_open();
        return;
    }
    if (in_box(x, y, speaker_box(bar->w))) {
        if (volume) volume_close();
        else volume_open();
        return;
    }
    menu_close();
    volume_close();
    windows_read(); /* 閉じたメニューの焦点は戻った */
    for (size_t i = 0; i < windows_count; i++) {
        int bx, bw;
        button_box(bar->w, windows_count, i, &bx, &bw);
        if (x < bx || x >= bx + bw) continue;
        n_shell_window(cap, windows[i].id, windows[i].state & N_SHELL_WINDOW_FOCUSED ? N_SHELL_MINIMIZE : N_SHELL_RAISE);
        break;
    }
}

static void bar_event(const struct gui_event *e) {
    switch (e->type) {
    case EV_MOUSE_DOWN:
        if (e->buttons & 1) bar_press(e->x, e->y);
        break;
    case EV_MOUSE_MOVE:
    case EV_MOUSE_LEAVE: {
        bool over = e->type == EV_MOUSE_MOVE && in_box(e->x, e->y, speaker_box(bar->w));
        if (over != speaker_hover) {
            speaker_hover = over;
            bar_draw();
        }
        break;
    }
    case EV_WHEEL:
        if (in_box(e->x, e->y, speaker_box(bar->w))) volume_wheel(e->wheel);
        break;
    case EV_HOTKEY:
        if (e->key == 't' || e->key == 'T') {
            n_result r = gui_launch(N_FOLDER_SYSTEM "\\term", NULL);
            if (r < 0) toast("Could not start program", n_result_text(r), TOAST_MS);
        } else if (menu) {
            menu_close();
        } else {
            menu_open();
        }
        break;
    default: break;
    }
}

/* ---- デスクトップの入力 ---- */
static int icon_at(const struct desk *d, int x, int y) {
    for (int i = 0; i < desktop_app_count; i++)
        if (in_box(x, y, icon_cell(d->h, i))) return i;
    return -1;
}

static void desk_event(struct desk *d, size_t screen, const struct gui_event *e) {
    if (screen != 0) return;
    switch (e->type) {
    case EV_MOUSE_DOWN: {
        if (!(e->buttons & 1)) break;
        bool twice = e->time_ns - d->last_click < DOUBLE_CLICK_NS && abs(e->x - d->last_x) < 6 && abs(e->y - d->last_y) < 6;
        d->last_click = twice ? 0 : e->time_ns;
        d->last_x = e->x;
        d->last_y = e->y;
        int icon = icon_at(d, e->x, e->y);
        selection_set(d, icon);
        if (icon >= 0 && twice) perform(&desktop_apps[icon]);
        break;
    }
    case EV_UNFOCUS: selection_set(d, -1); break;
    case EV_KEY:
        if (e->pressed && e->key == NKEY_ENTER && d->selected >= 0) perform(&desktop_apps[d->selected]);
        break;
    default: break;
    }
}

/* ---- 画面が変わった ---- */
static void screens_changed(void) {
    n_result n = n_shell_screens(cap, NULL, 0);
    if (n <= 0) return;
    struct n_shell_screen *list = calloc((size_t)n, sizeof *list);
    if (!list) return;
    n = n_shell_screens(cap, list, (size_t)n);
    if ((size_t)n > desk_count) {
        struct desk *grown = realloc(desks, (size_t)n * sizeof *grown);
        if (grown) {
            memset(grown + desk_count, 0, ((size_t)n - desk_count) * sizeof *grown);
            desks = grown;
            desk_count = (size_t)n;
        }
    }
    bool deep = display_bits_per_channel() >= 10;
    for (size_t i = 0; i < desk_count && i < (size_t)n; i++) {
        if (list[i].on) {
            desk_fit(i, &list[i], deep);
        } else {
            n_events_unwatch(events, TOKEN_DESK + i);
            desk_close(&desks[i]);
        }
    }
    if (list[0].on) {
        screen_w = list[0].width;
        screen_h = list[0].height;
        bar_fit(&list[0]);
    }
    free(list);
}

/* ---- 通知 ---- */
#define TOAST_W 320
#define TOAST_H 64

static window_t *toast_win;
static char toast_title[64], toast_text[128], toast_other[128];
static bool toast_turning; /* toast_text と toast_other を代わる代わる出す */

static void toast_draw(void) {
    canvas_t *c = &toast_win->c;
    gfx_fill(c, 0, 0, TOAST_W, TOAST_H, TRANSPARENT);
    gfx_fill_round(c, 0, 0, TOAST_W, TOAST_H, 10, RGB(90, 76, 170));
    gfx_fill_round(c, 1, 1, TOAST_W - 2, TOAST_H - 2, 9, RGB(28, 24, 56));
    draw_crescent(c, 26, 32, 11, RGB(246, 236, 196));
    text_cut(c, 48, 12, toast_title, TOAST_W - 60, RGB(255, 255, 255));
    text_cut(c, 48, 34, toast_text, TOAST_W - 60, RGB(180, 180, 210));
    win_update(toast_win);
}

static void toast_close(void) {
    n_events_unwatch(events, TOKEN_TOAST_TIME);
    toast_turning = false;
    if (!toast_win) return;
    n_events_unwatch(events, TOKEN_TOAST);
    win_close(toast_win);
    toast_win = NULL;
}

static void toast_arm(unsigned ms) {
    n_events_unwatch(events, TOKEN_TOAST_TIME);
    if (ms) n_events_timer(events, n_clock_after(ms * N_NS_PER_MS), 0, TOKEN_TOAST_TIME);
}

/* ms 0：次の通知まで残る。 */
static void toast(const char *title, const char *text, unsigned ms) {
    strlcpy(toast_title, title, sizeof toast_title);
    strlcpy(toast_text, text ? text : "", sizeof toast_text);
    toast_turning = false;
    if (!toast_win) {
        if (win_open(TOAST_W, TOAST_H, "Notice", WIN_NO_DECOR, &toast_win) < 0) {
            toast_win = NULL;
            return;
        }
        struct n_shell_place place = {.layer = N_LAYER_OVERLAY, .x = screen_w - TOAST_W - 16, .y = 16,
                                      .flags = N_PLACE_NO_FOCUS | N_PLACE_BLEND};
        n_shell_place(cap, toast_win, &place);
        n_events_watch(events, toast_win->handle, N_EVENT_RECEIVABLE, TOKEN_TOAST);
    }
    toast_draw();
    toast_arm(ms);
}

static void toast_time(void) {
    if (!toast_turning) {
        toast_close();
        return;
    }
    char was[sizeof toast_text];
    strlcpy(was, toast_text, sizeof was);
    strlcpy(toast_text, toast_other, sizeof toast_text);
    strlcpy(toast_other, was, sizeof toast_other);
    toast_draw();
    toast_arm(HINT_MS);
}

/* ---- 電源の段取り ---- */
static uint64_t power_seen;

static const char *reason_text(uint32_t reason, const char *detail) {
    switch (reason) {
    case N_POWER_REASON_IOMMU: return "The IOMMU cannot be trusted to come back";
    case N_POWER_REASON_NO_S3: return "The firmware has no sleep state";
    case N_POWER_REASON_NO_WAKE_PAGE: return "There is no wake page below 1 MiB";
    case N_POWER_REASON_DEVICE: return detail;
    case N_POWER_REASON_DEVICE_BUSY: return detail;
    case N_POWER_REASON_DEVICE_STUCK: return detail;
    case N_POWER_REASON_APP_DRIVER: return "An app drives one of the PCI devices";
    case N_POWER_REASON_DISK_FLUSH: return "A disk did not write its cache back";
    case N_POWER_REASON_CPU_STOP: return "A processor did not stop";
    case N_POWER_REASON_FIRMWARE: return "The firmware did not sleep";
    case N_POWER_REASON_LOW_ROOM: return "There is no room below 1 MiB";
    case N_POWER_REASON_NOT_ON_DISK: return "C: is not on a disk";
    case N_POWER_REASON_NO_MEMORY: return "Not enough memory";
    case N_POWER_REASON_NO_SPACE: return "Not enough free space on C:";
    case N_POWER_REASON_IMAGE_TOO_LARGE: return "The image did not fit in memory or on C:";
    case N_POWER_REASON_STORE_WRITE: return "C: could not be written";
    default: return "";
    }
}

static void power_changed(void) {
    struct n_power_state p;
    if (n_power_state(cap, &p) < 0 || p.serial == power_seen) return;
    power_seen = p.serial;
    bool restart = p.action == POWER_REBOOT, hibernate = p.action == POWER_HIBERNATE;
    switch (p.stage) {
    case N_POWER_CLOSING:
        toast(restart ? "Waiting to restart" : "Waiting to shut down", p.detail, 0);
        if (p.forceable) {
            strlcpy(toast_other, "Choose again to force it", sizeof toast_other);
            toast_turning = true;
            toast_arm(HINT_MS);
        }
        break;
    case N_POWER_CANCELLED: toast(restart ? "Restart cancelled" : "Shutdown cancelled", p.detail, TOAST_MS); break;
    case N_POWER_STOPPING:
        toast(restart ? "Restarting" : "Shutting down", restart ? "See you in a moment..." : "Good night!", 0);
        break;
    case N_POWER_SAVING: toast("Hibernating", "Saving the memory to C:", 0); break;
    case N_POWER_REFUSED:
        toast(hibernate ? "Cannot hibernate" : "Cannot sleep", reason_text(p.reason, p.detail), TOAST_MS);
        break;
    case N_POWER_FAILED:
        toast(hibernate ? "Hibernation failed" : "Sleep failed", reason_text(p.reason, p.detail), TOAST_MS);
        break;
    case N_POWER_DEVICE_LOST:
        toast(p.reason == N_POWER_REASON_IOMMU ? "The IOMMU did not come back from sleep" : "A device did not wake up", p.detail, TOAST_MS);
        break;
    default: break;
    }
    clock_arm();
    bar_draw();
}

/* ---- ログアウト ----
   窓に閉じるよう頼み、全部閉じたらセッションを終える。保存を聞くアプリの窓が残ればやめる。 */
static bool logging_out;
static size_t logout_left;

static void logout_arm(void) {
    n_events_unwatch(events, TOKEN_LOGOUT_TIME);
    n_events_timer(events, n_clock_after(LOGOUT_STALL_MS * N_NS_PER_MS), 0, TOKEN_LOGOUT_TIME);
}

static void logout_end(void) {
    logging_out = false;
    n_events_unwatch(events, TOKEN_LOGOUT_TIME);
    n_result r = n_session_end();
    if (r < 0) toast("Could not log out", n_result_text(r), TOAST_MS);
}

static void logout_begin(void) {
    windows_read();
    if (!windows_count) {
        logout_end();
        return;
    }
    logging_out = true;
    logout_left = windows_count;
    for (size_t i = 0; i < windows_count; i++) n_shell_window(cap, windows[i].id, N_SHELL_CLOSE);
    logout_arm();
}

static void logout_follow(void) {
    if (!logging_out) return;
    if (!windows_count) {
        logout_end();
    } else if (windows_count < logout_left) {
        logout_left = windows_count;
        logout_arm();
    }
}

static void logout_stalled(void) {
    if (!logging_out) return;
    logging_out = false;
    toast("Log out cancelled", windows_count ? windows[windows_count - 1].title : "", TOAST_MS);
}

/* ---- ほかのプログラムからの通知（サービス "shell"） ---- */
static n_handle inlet = -1;
static n_handle *peers;
static size_t peer_count;

static void peer_add(n_handle channel) {
    size_t at = 0;
    while (at < peer_count && peers[at] >= 0) at++;
    if (at == peer_count) {
        n_handle *grown = realloc(peers, (peer_count + 1) * sizeof *grown);
        if (!grown) {
            n_handle_close(channel);
            return;
        }
        peers = grown;
        peer_count++;
    }
    peers[at] = channel;
    n_events_watch(events, channel, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_PEER + at);
}

static void peer_event(size_t at) {
    if (at >= peer_count || peers[at] < 0) return; /* もう閉じた相手の知らせ */
    for (;;) {
        struct n_shell_notice notice;
        n_result got = n_channel_receive(peers[at], &notice, sizeof notice, 0, NULL);
        if (got == -N_ERR_NOT_READY) return;
        if (got != (n_result)sizeof notice) break;
        notice.title[sizeof notice.title - 1] = 0;
        notice.text[sizeof notice.text - 1] = 0;
        toast(notice.title, notice.text, TOAST_MS);
    }
    n_events_unwatch(events, TOKEN_PEER + at);
    n_handle_close(peers[at]);
    peers[at] = -1;
}

static void inlet_event(void) {
    for (;;) {
        n_handle channel = n_inlet_accept(inlet, 0);
        if (channel < 0) return;
        peer_add(channel);
    }
}

/* ---- 事象 ---- */
static void watch_add(n_handle watch, uint64_t token) {
    if (watch >= 0) n_events_watch(events, watch, N_EVENT_RECEIVABLE, token);
}

static n_handle windows_watch, screen_watch, sound_watch, sound_setting_watch, zone_watch, power_watch;

static void setting_watch_drain(n_handle watch) {
    struct n_settings_changed heard;
    while (n_setting_watch_take(watch, &heard) >= 0) {}
}

static void dispatch(const struct n_event *ev) {
    struct gui_event e;
    uint64_t token = ev->token;
    if (token >= TOKEN_PEER) {
        peer_event((size_t)(token - TOKEN_PEER));
        return;
    }
    if (token >= TOKEN_DESK) {
        size_t i = (size_t)(token - TOKEN_DESK);
        while (i < desk_count && desks[i].win && win_event(desks[i].win, &e, 0) > 0) desk_event(&desks[i], i, &e);
        return;
    }
    switch (token) {
    case TOKEN_WINDOWS:
        n_watch_take(windows_watch);
        windows_read();
        bar_draw();
        logout_follow();
        break;
    case TOKEN_SCREEN:
        n_watch_take(screen_watch);
        screens_changed();
        break;
    case TOKEN_SOUND:
        n_audio_output_watch_take(sound_watch);
        sound_changed();
        break;
    case TOKEN_SOUND_SETTING:
        setting_watch_drain(sound_setting_watch);
        sound_changed();
        break;
    case TOKEN_ZONE:
        setting_watch_drain(zone_watch);
        clock_arm();
        bar_draw();
        break;
    case TOKEN_POWER:
        n_watch_take(power_watch);
        power_changed();
        break;
    case TOKEN_CLOCK:
        clock_arm();
        bar_draw();
        break;
    case TOKEN_TOAST_TIME: toast_time(); break;
    case TOKEN_LOGOUT_TIME: logout_stalled(); break;
    case TOKEN_INLET: inlet_event(); break;
    case TOKEN_BAR:
        while (bar && win_event(bar, &e, 0) > 0) bar_event(&e);
        break;
    case TOKEN_MENU:
        while (menu && win_event(menu, &e, 0) > 0) menu_event(&e);
        break;
    case TOKEN_VOLUME:
        while (volume && win_event(volume, &e, 0) > 0) volume_event(&e);
        break;
    case TOKEN_TOAST:
        while (toast_win && win_event(toast_win, &e, 0) > 0) {} /* 読み捨てる */
        break;
    default: break;
    }
}

int main(void) {
    cap = n_cap_find(N_CAP_SHELL);
    if (cap < 0 || !gui_available()) return 1;
    events = n_events_create();
    if (events < 0) return 1;
    sound_read();
    windows_watch = n_shell_watch(cap);
    screen_watch = n_screen_watch();
    sound_watch = n_audio_output_watch();
    sound_setting_watch = n_setting_watch(N_DOMAIN_AUDIO, N_SETTING_ANY);
    zone_watch = n_setting_watch(N_DOMAIN_TIME, N_TIME_ZONE_MINUTES);
    power_watch = n_power_watch(cap);
    watch_add(windows_watch, TOKEN_WINDOWS);
    watch_add(screen_watch, TOKEN_SCREEN);
    watch_add(sound_watch, TOKEN_SOUND);
    watch_add(sound_setting_watch, TOKEN_SOUND_SETTING);
    watch_add(zone_watch, TOKEN_ZONE);
    watch_add(power_watch, TOKEN_POWER);
    programs_read();
    screens_changed();
    windows_read();
    bar_draw();
    clock_arm();
    struct n_power_state power;
    if (n_power_state(cap, &power) >= 0) power_seen = power.serial;
    inlet = n_service_listen(N_SHELL_SERVICE);
    if (inlet >= 0) n_events_watch(events, inlet, N_EVENT_RECEIVABLE, TOKEN_INLET);
    toast("Welcome to " OS_NAME, "Double-click an icon or open the moon menu.", TOAST_MS);
    for (;;) {
        struct n_event ev[16];
        n_result n = n_events_wait(events, ev, sizeof ev / sizeof *ev, N_CLOCK_NEVER);
        if (n < 0) return 1;
        for (n_result i = 0; i < n; i++) dispatch(&ev[i]);
    }
}
