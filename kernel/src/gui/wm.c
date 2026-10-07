/*
 * Nocturne window manager + compositor.
 *
 * Runs as a kernel thread. Each window owns a client pixel buffer that is
 * shared with the owning process (mapped into its address space). Apps draw
 * into it and call win_present(); the compositor redraws damaged screen
 * regions back-to-front into a backbuffer and copies them to the framebuffer.
 * Input events are delivered through the window's file descriptor, so a
 * window can be poll()ed together with pipes.
 */
#include "kernel.h"
#include "arch/cpu.h"
#include "gui/wm.h"
#include "dev/fb.h"
#include "dev/gpu.h"
#include "dev/fbcon.h"
#include "dev/audio.h"
#include "dev/input.h"
#include "dev/timer.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sys/sched.h"
#include "sys/proc.h"
#include "sys/syscall.h"
#include "abi.h"

void power_off(void);
void power_reboot(void);

#define TITLE_H   28
#define BORDER    1
#define TASKBAR_H 40
#define SHADOW    10
#define MAX_WIN   64
#define EVQ       128
#define MIN_CW    120
#define MIN_CH    60

struct rect {
    int x, y, w, h;
};

struct window {
    int id, pid;
    uint64_t pml4;
    int x, y;   /* outer frame position */
    int cw, ch; /* client size */
    uint32_t *buf;
    size_t pages;
    uint64_t uaddr;
    int flags;
    bool shown, minimized, maximized, dead;
    struct rect restore;
    char title[64];
    struct gui_event ev[EVQ];
    int ev_head, ev_tail, ev_count;
    struct wait_queue wq;
    struct vnode *vn;
};

/* ---- state ---- */
static bool running;
static int sw, sh;
static canvas_t back, wall;
static struct wait_queue wm_wq;

static struct window *zorder[MAX_WIN]; /* bottom .. top */
static int nwin;
static int next_win_id = 1;

static int mx, my, mbuttons;

/* remote display (RDP): what changed since the remote side last looked */
#define MAX_RDAMAGE 16
static int remote_users;
static struct rect rdamage[MAX_RDAMAGE];
static int nrdamage;
static int cursor_shape = CURSOR_ARROW;

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CLIENT, DRAG_BUTTON, DRAG_VOLUME };
static int drag_mode;
static struct window *drag_win;
static int drag_dx, drag_dy, drag_btn;
static struct rect resize_rect;
static bool resize_visible;

static struct window *hover_win;
static int hover_btn; /* 0 none, 1 close, 2 max, 3 min */

static bool menu_open;
static int menu_sel = -1;
static bool vol_open;         /* the volume panel above the taskbar's speaker */
static int shown_volume = -1; /* what the speaker was last drawn showing */
static bool shown_muted;
static void set_volume_panel(bool open);
static int sel_icon = -1;
static uint64_t last_click_ms;
static int last_click_x, last_click_y;

static char toast_title[64], toast_text[128];
static uint64_t toast_until;
static int last_clock_min = -1;

static char *clipboard;
static size_t clipboard_len;
static uint32_t clipboard_seq; /* bumped on every change, so a remote viewer can follow it */

/* ---- damage tracking ---- */
#define MAX_DAMAGE 32
static struct rect damage[MAX_DAMAGE];
static int ndamage;

static bool rect_clip(struct rect *r) {
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > sw) r->w = sw - r->x;
    if (r->y + r->h > sh) r->h = sh - r->y;
    return r->w > 0 && r->h > 0;
}

static bool rect_overlap(const struct rect *a, const struct rect *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static struct rect rect_union(struct rect a, struct rect b) {
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return (struct rect){x0, y0, x1 - x0, y1 - y0};
}

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static void damage_rect(int x, int y, int w, int h) {
    struct rect r = {x, y, w, h};
    if (!rect_clip(&r)) return;
    /* merge with any overlapping rect (repeat until stable) */
    for (int i = 0; i < ndamage; i++) {
        if (rect_overlap(&r, &damage[i])) {
            r = rect_union(r, damage[i]);
            damage[i] = damage[--ndamage];
            i = -1;
        }
    }
    if (ndamage == MAX_DAMAGE) {
        for (int i = 1; i < ndamage; i++) damage[0] = rect_union(damage[0], damage[i]);
        ndamage = 1;
        damage[0] = rect_union(damage[0], r);
        return;
    }
    damage[ndamage++] = r;
}

static void damage_all(void) { damage_rect(0, 0, sw, sh); }

static void wake_compositor(void) { wq_wake_all(&wm_wq); }

/* ---- window geometry ---- */
static bool decorated(struct window *w) { return !(w->flags & WIN_NO_DECOR); }
static int frame_w(struct window *w) { return decorated(w) ? w->cw + 2 * BORDER : w->cw; }
static int frame_h(struct window *w) { return decorated(w) ? w->ch + TITLE_H + BORDER : w->ch; }
static int client_x(struct window *w) { return decorated(w) ? w->x + BORDER : w->x; }
static int client_y(struct window *w) { return decorated(w) ? w->y + TITLE_H : w->y; }

static struct rect win_bounds(struct window *w) {
    return (struct rect){w->x - SHADOW, w->y - SHADOW, frame_w(w) + 2 * SHADOW, frame_h(w) + 2 * SHADOW};
}

static void damage_window(struct window *w) {
    struct rect b = win_bounds(w);
    damage_rect(b.x, b.y, b.w, b.h);
}

static bool visible(struct window *w) { return w && !w->dead && w->shown && !w->minimized; }

static struct window *focused(void) {
    for (int i = nwin - 1; i >= 0; i--)
        if (visible(zorder[i])) return zorder[i];
    return NULL;
}

/* title bar buttons: 1 close, 2 maximize, 3 minimize; returns center x */
static int button_cx(struct window *w, int b) {
    int fx = w->x + frame_w(w);
    if (b == 1) return fx - 18;
    if (b == 2) return (w->flags & WIN_RESIZABLE) ? fx - 42 : -1000;
    return (w->flags & WIN_RESIZABLE) ? fx - 66 : fx - 42;
}

static int hit_button(struct window *w, int x, int y) {
    if (!decorated(w)) return 0;
    int cy = w->y + TITLE_H / 2;
    for (int b = 1; b <= 3; b++) {
        int cx = button_cx(w, b);
        int dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy <= 9 * 9) return b;
    }
    return 0;
}

static struct window *window_at(int x, int y) {
    for (int i = nwin - 1; i >= 0; i--) {
        struct window *w = zorder[i];
        if (!visible(w)) continue;
        if (in_rect(x, y, w->x, w->y, frame_w(w), frame_h(w))) return w;
    }
    return NULL;
}

static bool in_resize_grip(struct window *w, int x, int y) {
    if (!(w->flags & WIN_RESIZABLE) || w->maximized) return false;
    int fx1 = w->x + frame_w(w), fy1 = w->y + frame_h(w);
    return x >= fx1 - 14 && y >= fy1 - 14 && x < fx1 && y < fy1;
}

/* ---- event queues ---- */
static void push_event(struct window *w, const struct gui_event *e) {
    if (!w || w->dead) return;
    if (e->type == EV_MOUSE_MOVE && w->ev_count) {
        int last = (w->ev_head + EVQ - 1) % EVQ;
        if (w->ev[last].type == EV_MOUSE_MOVE) {
            w->ev[last] = *e;
            return;
        }
    }
    if (w->ev_count == EVQ) return;
    w->ev[w->ev_head] = *e;
    w->ev_head = (w->ev_head + 1) % EVQ;
    w->ev_count++;
    wq_wake_all(&w->wq);
    poll_notify();
}

static void send_simple(struct window *w, int type) {
    struct gui_event e = {0};
    e.type = type;
    push_event(w, &e);
}

static void send_mouse(struct window *w, int type, int buttons) {
    struct gui_event e = {0};
    e.type = type;
    e.x = mx - client_x(w);
    e.y = my - client_y(w);
    e.buttons = buttons;
    push_event(w, &e);
}

/* ---- focus / stacking ---- */
static int zindex(struct window *w) {
    for (int i = 0; i < nwin; i++)
        if (zorder[i] == w) return i;
    return -1;
}

static void damage_taskbar(void) { damage_rect(0, sh - TASKBAR_H, sw, TASKBAR_H); }

static struct window *last_focus;

static void update_focus(void) {
    struct window *f = focused();
    if (f == last_focus) return;
    if (last_focus) {
        send_simple(last_focus, EV_UNFOCUS);
        damage_window(last_focus);
    }
    if (f) {
        send_simple(f, EV_FOCUS);
        damage_window(f);
    }
    last_focus = f;
    damage_taskbar();
}

static void raise_window(struct window *w) {
    int i = zindex(w);
    if (i < 0) return;
    for (; i < nwin - 1; i++) zorder[i] = zorder[i + 1];
    zorder[nwin - 1] = w;
    w->minimized = false;
    damage_window(w);
    damage_taskbar();
    update_focus();
}

static void lower_to_bottom(struct window *w) {
    int i = zindex(w);
    if (i < 0) return;
    for (; i > 0; i--) zorder[i] = zorder[i - 1];
    zorder[0] = w;
    damage_window(w);
    update_focus();
}

static void minimize_window(struct window *w) {
    w->minimized = true;
    damage_window(w);
    damage_taskbar();
    update_focus();
}

/* ---- window lifetime ---- */
static void unmap_user(struct window *w) {
    if (!w->uaddr || !w->pml4) return;
    for (size_t i = 0; i < w->pages; i++) vmm_unmap_page(w->pml4, w->uaddr + i * PAGE_SIZE);
    w->uaddr = 0;
}

static void win_destroy(struct window *w) {
    if (w->dead) return;
    if (w->shown) damage_window(w);
    damage_taskbar(); /* its button goes away even when focus does not change */
    unmap_user(w);
    if (w->buf) vfree(w->buf, w->pages);
    w->buf = NULL;
    int i = zindex(w);
    if (i >= 0) {
        for (; i < nwin - 1; i++) zorder[i] = zorder[i + 1];
        nwin--;
    }
    w->dead = true;
    if (drag_win == w) {
        drag_win = NULL;
        drag_mode = DRAG_NONE;
        if (resize_visible) damage_all();
        resize_visible = false;
    }
    if (hover_win == w) hover_win = NULL;
    if (last_focus == w) last_focus = NULL;
    update_focus();
    wq_wake_all(&w->wq);
    poll_notify();
    wake_compositor();
}

static int64_t win_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    struct window *w = v->priv;
    size_t max = n / sizeof(struct gui_event);
    if (max == 0) return -EINVAL;
    for (;;) {
        uint64_t fl = irq_save();
        if (w->ev_count) {
            struct gui_event *out = buf;
            size_t got = 0;
            while (got < max && w->ev_count) {
                out[got++] = w->ev[w->ev_tail];
                w->ev_tail = (w->ev_tail + 1) % EVQ;
                w->ev_count--;
            }
            irq_restore(fl);
            return got * sizeof(struct gui_event);
        }
        if (w->dead) {
            irq_restore(fl);
            return 0;
        }
        if (f->flags & O_NONBLOCK) {
            irq_restore(fl);
            return -EAGAIN;
        }
        if (current_task->killed) {
            irq_restore(fl);
            return -EINTR;
        }
        wq_wait(&w->wq);
        irq_restore(fl);
    }
}

static bool win_can_read(struct vnode *v, struct file *f) {
    struct window *w = v->priv;
    return w->ev_count > 0 || w->dead;
}

static void win_close(struct vnode *v, struct file *f) { win_destroy(v->priv); }

static void win_release(struct vnode *v) {
    kfree(v->priv);
    kfree(v);
}

static struct vnode_ops win_ops = {
    .read = win_read, .can_read = win_can_read, .close = win_close, .release = win_release};

static bool alloc_buffer(struct window *w, int cw, int ch) {
    size_t pages = ALIGN_UP((uint64_t)cw * ch * 4, PAGE_SIZE) / PAGE_SIZE;
    uint32_t *buf = vmalloc(pages);
    if (!buf) return false;
    w->buf = buf;
    w->pages = pages;
    w->cw = cw;
    w->ch = ch;
    return true;
}

static uint64_t map_user(struct window *w, struct task *t) {
    uint64_t va = t->mmap_next;
    for (size_t i = 0; i < w->pages; i++) {
        uint64_t pa = vmm_translate(kernel_pml4, (uint64_t)w->buf + i * PAGE_SIZE);
        if (!vmm_map_page(t->pml4, va + i * PAGE_SIZE, pa, PTE_P | PTE_W | PTE_U | PTE_SHARED | pte_nx)) return 0;
    }
    t->mmap_next = va + (w->pages + 1) * PAGE_SIZE;
    w->uaddr = va;
    w->pml4 = t->pml4;
    return va;
}

/* ---- drawing helpers ---- */
static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static int corner_inset(int row, int r) {
    if (row >= r) return 0;
    int d = r - row;
    return r - (int)isqrt32((uint32_t)(r * r - d * d));
}

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

static void text_trunc(canvas_t *c, int x, int y, const char *s, int maxw, uint32_t col) {
    char tmp[96];
    int maxc = maxw / gfx_font_w(FONT_SMALL);
    if (maxc <= 0) return;
    int i = 0, chars = 0;
    while (s[i] && i < (int)sizeof tmp - 4) {
        uint32_t cp;
        int n = gfx_utf8_decode(s + i, &cp);
        if (chars == maxc) break;
        i += n;
        chars++;
    }
    memcpy(tmp, s, i);
    tmp[i] = 0;
    if (s[i] && i >= 2) {
        /* replace the last char with an ellipsis-ish ".." */
        tmp[i - 1] = '.';
        tmp[i - 2] = '.';
    }
    gfx_text(c, x, y, tmp, col, TRANSPARENT, FONT_SMALL);
}

static void composite(void);

/* ---- apps / desktop ---- */
struct app {
    const char *name;
    const char *path;
    int icon;
};

static const struct app apps[] = {
    {"Terminal", "/bin/term", ICON_TERMINAL},
    {"Files", "/bin/files", ICON_FILES},
    {"Web Browser", "/bin/browser", ICON_NET},
    {"Text Editor", "/bin/notepad", ICON_EDITOR},
    {"Paint", "/bin/paint", ICON_PAINT},
    {"Calculator", "/bin/calc", ICON_CALC},
    {"Clock", "/bin/clock", ICON_CLOCK},
    {"System Monitor", "/bin/sysmon", ICON_MONITOR},
    {"Piano", "/bin/piano", ICON_PIANO},
    {"Sound Player", "/bin/player", ICON_SOUND},
    {"Snake", "/bin/snake", ICON_SNAKE},
    {"Tetris", "/bin/tetris", ICON_TETRIS},
    {"Minesweeper", "/bin/mines", ICON_MINES},
    {"Mandelbrot", "/bin/mandel", ICON_FRACTAL},
    {"3D Cube", "/bin/cube", ICON_CUBE},
    {"Game of Life", "/bin/life", ICON_LIFE},
    {"About Nocturne", "/bin/about", ICON_ABOUT},
    {"Restart", "@reboot", ICON_REBOOT},
    {"Shut Down", "@poweroff", ICON_POWER},
};
#define NAPPS ((int)ARRAY_SIZE(apps))

static const int desktop_apps[] = {0, 1, 2, 3, 4, 8, 9, 10, 13, 14, 16};
#define NDESK ((int)ARRAY_SIZE(desktop_apps))
#define ICON_CELL_W 88
#define ICON_CELL_H 82

static void icon_cell(int i, int *x, int *y) {
    int per_col = (sh - TASKBAR_H - 16) / ICON_CELL_H;
    if (per_col < 1) per_col = 1;
    *x = 12 + (i / per_col) * ICON_CELL_W;
    *y = 12 + (i % per_col) * ICON_CELL_H;
}

static int icon_at(int x, int y) {
    for (int i = 0; i < NDESK; i++) {
        int cx, cy;
        icon_cell(i, &cx, &cy);
        if (in_rect(x, y, cx, cy, ICON_CELL_W - 8, ICON_CELL_H - 6)) return i;
    }
    return -1;
}

static void draw_desktop_icons(canvas_t *c) {
    for (int i = 0; i < NDESK; i++) {
        const struct app *a = &apps[desktop_apps[i]];
        int x, y;
        icon_cell(i, &x, &y);
        int cw = ICON_CELL_W - 8;
        desktop_draw_icon(c, x + (cw - 32) / 2, y + 8, a->icon);
        const char *label = a->name;
        if (!strcmp(label, "About Nocturne")) label = "About";
        int tw = gfx_text_width(label, FONT_SMALL);
        int tx = x + (cw - tw) / 2;
        gfx_text(c, tx + 1, y + 49, label, RGB(0, 0, 0), TRANSPARENT, FONT_SMALL);
        gfx_text(c, tx, y + 48, label, RGB(235, 235, 250), TRANSPARENT, FONT_SMALL);
    }
}

static int launch(const char *path, const char *arg) {
    if (!strcmp(path, "@reboot")) {
        wm_notify("Restarting", "See you in a moment...");
        composite();
        sleep_ms(600);
        power_reboot();
    }
    if (!strcmp(path, "@poweroff")) {
        wm_notify("Shutting down", "Good night!");
        composite();
        sleep_ms(600);
        power_off();
    }
    int pid = proc_spawn_simple(path, arg, NULL);
    if (pid < 0) {
        char msg[128];
        ksnprintf(msg, sizeof msg, "%s (error %d)", path, -pid);
        wm_notify("Could not start program", msg);
    }
    return pid;
}

/* ---- start menu ---- */
#define MENU_W      260
#define MENU_HEAD   58
#define MENU_ITEM_H 36

/* items run down a column, and on into a second one when the screen is too short for one */
static int menu_rows(void) {
    int fit = (sh - TASKBAR_H - 8 - MENU_HEAD - 10) / MENU_ITEM_H;
    int cols = fit < 1 ? NAPPS : (NAPPS + fit - 1) / fit;
    return (NAPPS + cols - 1) / cols;
}

static struct rect menu_rect(void) {
    int rows = menu_rows(), cols = (NAPPS + rows - 1) / rows;
    int h = MENU_HEAD + rows * MENU_ITEM_H + 10;
    return (struct rect){4, MAX(0, sh - TASKBAR_H - h - 4), MENU_W * cols, h};
}

static void menu_item_pos(int i, int *x, int *y) {
    struct rect m = menu_rect();
    *x = m.x + i / menu_rows() * MENU_W;
    *y = m.y + MENU_HEAD + i % menu_rows() * MENU_ITEM_H;
}

static int menu_item_at(int x, int y) {
    struct rect m = menu_rect();
    int rows = menu_rows();
    if (!in_rect(x, y, m.x, m.y + MENU_HEAD, m.w, rows * MENU_ITEM_H)) return -1;
    int i = (x - m.x) / MENU_W * rows + (y - m.y - MENU_HEAD) / MENU_ITEM_H;
    return i < NAPPS ? i : -1;
}

static void set_menu(bool open) {
    if (open) set_volume_panel(false);
    if (menu_open == open) return;
    menu_open = open;
    menu_sel = -1;
    struct rect m = menu_rect();
    damage_rect(m.x - SHADOW, m.y - SHADOW, m.w + 2 * SHADOW, m.h + 2 * SHADOW);
    damage_rect(0, sh - TASKBAR_H, 140, TASKBAR_H);
}

static void draw_shadow(canvas_t *c, int x, int y, int w, int h) {
    for (int i = SHADOW; i > 0; i -= 2) {
        int a = 6 + (SHADOW - i) * 3;
        gfx_fill_blend(c, x - i + 2, y - i + 4, w + 2 * i - 4, h + 2 * i - 4, ARGB(a, 0, 0, 10));
    }
}

static void draw_menu(canvas_t *c) {
    struct rect m = menu_rect();
    draw_shadow(c, m.x, m.y, m.w, m.h);
    gfx_fill_round(c, m.x, m.y, m.w, m.h, 10, RGB(70, 60, 120));
    gfx_fill_round(c, m.x + 1, m.y + 1, m.w - 2, m.h - 2, 9, RGB(22, 20, 44));
    /* header */
    for (int j = 0; j < MENU_HEAD - 6; j++) {
        int in = corner_inset(j, 9);
        gfx_hline(c, m.x + 1 + in, m.y + 1 + j, m.w - 2 - 2 * in,
                  gfx_mix(RGB(64, 44, 130), RGB(30, 26, 70), j * 255 / (MENU_HEAD - 6)));
    }
    draw_crescent(c, m.x + 30, m.y + 26, 13, RGB(246, 236, 196));
    gfx_text(c, m.x + 54, m.y + 10, OS_NAME, RGB(255, 255, 255), TRANSPARENT, FONT_LARGE);
    for (int i = 0; i < NAPPS; i++) {
        int ix, iy;
        menu_item_pos(i, &ix, &iy);
        if (i == NAPPS - 2 && iy > m.y + MENU_HEAD) gfx_hline(c, ix + 12, iy - 1, MENU_W - 24, RGB(60, 56, 100));
        if (i == menu_sel) gfx_fill_round(c, ix + 6, iy + 1, MENU_W - 12, MENU_ITEM_H - 2, 6, RGB(76, 64, 150));
        desktop_draw_icon(c, ix + 12, iy + 2, apps[i].icon);
        gfx_text(c, ix + 54, iy + 10, apps[i].name, RGB(230, 230, 245), TRANSPARENT, FONT_SMALL);
    }
}

/* ---- volume: a speaker on the taskbar, and a panel with a slider and mute ---- */
#define TB_START_W 128
#define TB_CLOCK_W 100
#define TB_VOL_W   40
#define VOL_W      300
#define VOL_H      96

static struct rect vol_button_rect(void) {
    return (struct rect){sw - TB_CLOCK_W - TB_VOL_W, sh - TASKBAR_H + 5, TB_VOL_W - 4, TASKBAR_H - 10};
}
static struct rect vol_rect(void) { return (struct rect){sw - VOL_W - 8, sh - TASKBAR_H - VOL_H - 6, VOL_W, VOL_H}; }
static struct rect vol_mute_rect(void) {
    struct rect v = vol_rect();
    return (struct rect){v.x + 12, v.y + 42, 40, 40};
}
static struct rect vol_slider_rect(void) { /* the track; clicks a little either side still count */
    struct rect v = vol_rect();
    return (struct rect){v.x + 70, v.y + 60, VOL_W - 70 - 56, 4};
}

static void damage_volume(void) {
    struct rect b = vol_button_rect(), v = vol_rect();
    damage_rect(b.x, b.y, b.w, b.h);
    if (vol_open) damage_rect(v.x - SHADOW, v.y - SHADOW, v.w + 2 * SHADOW, v.h + 2 * SHADOW);
}

static void set_volume_panel(bool open) {
    if (vol_open == open) return;
    if (!open) damage_volume();
    vol_open = open;
    damage_volume();
}

/* a loudspeaker centred on (cx, cy), with one to three waves for the volume, or a cross when
   muted */
static void draw_speaker(canvas_t *c, int cx, int cy, int volume, bool muted, uint32_t col) {
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
    int waves = volume == 0 ? 0 : volume < 34 ? 1 : volume < 67 ? 2 : 3;
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

static void draw_volume_button(canvas_t *c, int y) {
    struct rect b = vol_button_rect();
    shown_volume = audio_volume();
    shown_muted = audio_muted();
    if (vol_open || (in_rect(mx, my, b.x, b.y, b.w, b.h) && drag_mode == DRAG_NONE))
        gfx_fill_round(c, b.x, b.y, b.w, b.h, 6, vol_open ? RGB(70, 62, 140) : RGB(40, 36, 78));
    draw_speaker(c, b.x + b.w / 2 - 1, y + TASKBAR_H / 2, shown_volume, shown_muted, RGB(235, 235, 250));
}

static void draw_volume_panel(canvas_t *c) {
    struct rect v = vol_rect(), m = vol_mute_rect(), s = vol_slider_rect();
    int vol = audio_volume();
    bool mute = audio_muted();
    draw_shadow(c, v.x, v.y, v.w, v.h);
    gfx_fill_round(c, v.x, v.y, v.w, v.h, 10, RGB(70, 60, 120));
    gfx_fill_round(c, v.x + 1, v.y + 1, v.w - 2, v.h - 2, 9, RGB(22, 20, 44));
    gfx_text(c, v.x + 16, v.y + 12, "Volume", RGB(245, 245, 255), TRANSPARENT, FONT_SMALL);
    const char *dev = audio_remote() ? "Remote desktop" : audio_card() ? audio_card() : "No sound device";
    gfx_text(c, v.x + v.w - 16 - gfx_text_width(dev, FONT_SMALL), v.y + 12, dev, RGB(150, 150, 190), TRANSPARENT,
             FONT_SMALL);
    gfx_fill_round(c, m.x, m.y, m.w, m.h, 8, mute ? RGB(90, 40, 60) : RGB(44, 38, 90));
    draw_speaker(c, m.x + m.w / 2 - 1, m.y + m.h / 2, vol, mute, RGB(235, 235, 250));
    int fillw = s.w * vol / 100, kx = s.x + fillw;
    gfx_fill_round(c, s.x, s.y, s.w, s.h, 2, RGB(60, 56, 100));
    gfx_fill_round(c, s.x, s.y, MAX(fillw, 4), s.h, 2, mute ? RGB(110, 100, 140) : RGB(150, 130, 255));
    gfx_fill_circle(c, kx, s.y + 2, 8, mute ? RGB(150, 146, 170) : RGB(235, 230, 255));
    char num[8];
    ksnprintf(num, sizeof num, "%d", vol);
    gfx_text(c, v.x + v.w - 20 - gfx_text_width(num, FONT_SMALL), s.y - 6, num, RGB(235, 235, 250), TRANSPARENT,
             FONT_SMALL);
}

static void volume_tick(void) { audio_system_tone(880, 70); }

static void volume_from_mouse(void) {
    struct rect s = vol_slider_rect();
    int v = (mx - s.x) * 100 / s.w;
    if (v != audio_volume() || audio_muted()) audio_set_volume(v);
    damage_volume();
}

static bool over_volume(int x, int y) {
    struct rect b = vol_button_rect(), v = vol_rect();
    return in_rect(x, y, b.x, b.y, b.w, b.h) || (vol_open && in_rect(x, y, v.x, v.y, v.w, v.h));
}

/* ---- taskbar ---- */
static int taskbar_buttons(struct window **list, int max) {
    int n = 0;
    for (int i = 0; i < nwin && n < max; i++) {
        /* stable order: by id */
        struct window *w = zorder[i];
        if (w->dead || !w->shown || !decorated(w)) continue;
        list[n++] = w;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && list[j - 1]->id > list[j]->id; j--) {
            struct window *t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }
    return n;
}

static void taskbar_button_geom(int n, int i, int *x, int *w) {
    int avail = sw - TB_START_W - TB_CLOCK_W - TB_VOL_W - 16;
    int bw = n ? avail / n : 0;
    if (bw > 190) bw = 190;
    *x = TB_START_W + 8 + i * bw;
    *w = bw - 4;
}

static void draw_taskbar(canvas_t *c) {
    int y = sh - TASKBAR_H;
    gfx_fill_blend(c, 0, y, sw, TASKBAR_H, ARGB(220, 14, 12, 32));
    gfx_hline(c, 0, y, sw, RGB(90, 76, 160));
    /* start button */
    uint32_t sb = menu_open ? RGB(84, 68, 170) : RGB(44, 38, 90);
    gfx_fill_round(c, 6, y + 5, TB_START_W - 6, TASKBAR_H - 10, 8, sb);
    draw_crescent(c, 24, y + TASKBAR_H / 2, 9, RGB(246, 236, 196));
    gfx_text(c, 42, y + 12, OS_NAME, RGB(240, 240, 255), TRANSPARENT, FONT_SMALL);

    struct window *list[MAX_WIN];
    int n = taskbar_buttons(list, MAX_WIN);
    struct window *f = focused();
    for (int i = 0; i < n; i++) {
        int bx, bw;
        taskbar_button_geom(n, i, &bx, &bw);
        uint32_t col = list[i] == f ? RGB(70, 62, 140) : list[i]->minimized ? RGB(26, 24, 48) : RGB(40, 36, 78);
        gfx_fill_round(c, bx, y + 5, bw, TASKBAR_H - 10, 6, col);
        if (list[i] == f) gfx_hline(c, bx + 8, y + TASKBAR_H - 6, bw - 16, RGB(170, 150, 255));
        text_trunc(c, bx + 10, y + 12, list[i]->title, bw - 20,
                   list[i]->minimized ? RGB(140, 140, 170) : RGB(235, 235, 250));
    }

    draw_volume_button(c, y);

    /* clock */
    struct tm_parts tp;
    time_to_parts(time_now(), &tp);
    static const char *wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char t1[16], t2[24];
    ksnprintf(t1, sizeof t1, "%02d:%02d", tp.hour, tp.min);
    ksnprintf(t2, sizeof t2, "%s %d %s", wd[tp.wday % 7], tp.mday, mo[(tp.mon - 1) % 12]);
    int cx = sw - TB_CLOCK_W;
    gfx_text(c, cx + (TB_CLOCK_W - gfx_text_width(t1, FONT_SMALL)) / 2, y + 3, t1, RGB(245, 245, 255), TRANSPARENT,
             FONT_SMALL);
    gfx_text(c, cx + (TB_CLOCK_W - gfx_text_width(t2, FONT_SMALL)) / 2, y + 20, t2, RGB(160, 160, 200), TRANSPARENT,
             FONT_SMALL);
}

static int taskbar_window_at(int x, int y, struct window **out) {
    struct window *list[MAX_WIN];
    int n = taskbar_buttons(list, MAX_WIN);
    for (int i = 0; i < n; i++) {
        int bx, bw;
        taskbar_button_geom(n, i, &bx, &bw);
        if (x >= bx && x < bx + bw) {
            *out = list[i];
            return 1;
        }
    }
    return 0;
}

/* ---- window drawing ---- */
static void draw_window(canvas_t *c, struct window *w, bool focus) {
    int fx = w->x, fy = w->y, fw = frame_w(w), fh = frame_h(w);
    draw_shadow(c, fx, fy, fw, fh);
    if (decorated(w)) {
        uint32_t edge = focus ? RGB(110, 96, 200) : RGB(60, 60, 84);
        uint32_t top = focus ? RGB(66, 52, 140) : RGB(44, 44, 62);
        uint32_t bot = focus ? RGB(38, 32, 88) : RGB(34, 34, 48);
        const int R = 8;
        for (int j = 0; j < TITLE_H; j++) {
            int in = corner_inset(j, R);
            if (j == 0) {
                gfx_hline(c, fx + in, fy, fw - 2 * in, edge);
                continue;
            }
            gfx_pixel(c, fx + in, fy + j, edge);
            gfx_pixel(c, fx + fw - 1 - in, fy + j, edge);
            gfx_hline(c, fx + in + 1, fy + j, fw - 2 * in - 2, gfx_mix(top, bot, j * 255 / TITLE_H));
        }
        gfx_vline(c, fx, fy + TITLE_H, fh - TITLE_H, edge);
        gfx_vline(c, fx + fw - 1, fy + TITLE_H, fh - TITLE_H, edge);
        gfx_hline(c, fx, fy + fh - 1, fw, edge);
        /* title */
        int nb = (w->flags & WIN_RESIZABLE) ? 3 : 2;
        text_trunc(c, fx + 12, fy + 6, w->title, fw - 24 - nb * 24, focus ? RGB(250, 250, 255) : RGB(160, 160, 180));
        /* buttons */
        for (int b = 1; b <= 3; b++) {
            int bx = button_cx(w, b);
            if (bx < -500) continue;
            int by = fy + TITLE_H / 2;
            uint32_t col = !focus ? RGB(84, 84, 108)
                         : b == 1 ? RGB(236, 92, 108)
                         : b == 2 ? RGB(96, 200, 130)
                                  : RGB(240, 190, 80);
            gfx_fill_circle(c, bx, by, 7, col);
            if (hover_win == w && hover_btn == b) {
                uint32_t g = RGB(40, 20, 30);
                if (b == 1) {
                    gfx_line(c, bx - 3, by - 3, bx + 3, by + 3, g);
                    gfx_line(c, bx - 3, by + 3, bx + 3, by - 3, g);
                } else if (b == 2) {
                    gfx_rect(c, bx - 3, by - 3, 7, 7, g);
                } else {
                    gfx_hline(c, bx - 3, by, 7, g);
                }
            }
        }
    }
    canvas_t wc;
    gfx_init(&wc, w->buf, w->cw, w->ch, w->cw);
    gfx_blit(c, client_x(w), client_y(w), &wc, 0, 0, w->cw, w->ch);
    if (w->flags & WIN_RESIZABLE && !w->maximized) {
        /* little grip dots */
        int gx = w->x + fw - 4, gy = w->y + fh - 4;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j <= i; j++) gfx_pixel(c, gx - (i - j) * 3, gy - j * 3, RGB(150, 140, 200));
    }
}

static void draw_toast(canvas_t *c) {
    int w = 320, h = 64, x = sw - w - 16, y = 16;
    draw_shadow(c, x, y, w, h);
    gfx_fill_round(c, x, y, w, h, 10, RGB(90, 76, 170));
    gfx_fill_round(c, x + 1, y + 1, w - 2, h - 2, 9, RGB(28, 24, 56));
    draw_crescent(c, x + 26, y + 32, 11, RGB(246, 236, 196));
    text_trunc(c, x + 48, y + 12, toast_title, w - 60, RGB(255, 255, 255));
    text_trunc(c, x + 48, y + 34, toast_text, w - 60, RGB(180, 180, 210));
}

static struct rect toast_rect(void) { return (struct rect){sw - 336 - SHADOW, 0, 336 + 2 * SHADOW, 96}; }

/* ---- compositing ---- */
static void composite_rect(struct rect r) {
    canvas_t *c = &back;
    gfx_noclip(c);
    gfx_clip(c, r.x, r.y, r.w, r.h);
    gfx_blit(c, r.x, r.y, &wall, r.x, r.y, r.w, r.h);
    if (sel_icon >= 0) {
        int x, y;
        icon_cell(sel_icon, &x, &y);
        gfx_fill_blend(c, x, y, ICON_CELL_W - 8, ICON_CELL_H - 6, ARGB(70, 150, 140, 255));
    }
    struct window *f = focused();
    for (int i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (!visible(w)) continue;
        struct rect b = win_bounds(w);
        if (!rect_overlap(&b, &r)) continue;
        draw_window(c, w, w == f);
    }
    if (resize_visible) {
        struct rect o = resize_rect;
        gfx_rect(c, o.x, o.y, o.w, o.h, RGB(180, 160, 255));
        gfx_rect(c, o.x + 1, o.y + 1, o.w - 2, o.h - 2, RGB(90, 70, 200));
    }
    if (r.y + r.h > sh - TASKBAR_H) draw_taskbar(c);
    if (menu_open) draw_menu(c);
    if (vol_open) draw_volume_panel(c);
    if (toast_until) draw_toast(c);
    /* a remote viewer draws the pointer itself; keep it out of the picture it is sent */
    if (!remote_users) desktop_draw_cursor(c, mx, my, cursor_shape);
    fb_present(back.px, back.pitch, r.x, r.y, r.w, r.h);
    if (remote_users) {
        if (nrdamage == MAX_RDAMAGE) {
            for (int i = 1; i < nrdamage; i++) rdamage[0] = rect_union(rdamage[0], rdamage[i]);
            nrdamage = 1;
            rdamage[0] = rect_union(rdamage[0], r);
        } else {
            rdamage[nrdamage++] = r;
        }
    }
}

static void composite(void) {
    /* snapshot so that drawing can't race with new damage */
    struct rect list[MAX_DAMAGE];
    int n = ndamage;
    memcpy(list, damage, n * sizeof(struct rect));
    ndamage = 0;
    for (int i = 0; i < n; i++) composite_rect(list[i]);
}

static void damage_cursor(void) { damage_rect(mx - 8, my - 8, 26, 30); }

/* ---- input handling (compositor thread) ---- */
#define INQ 256
struct in_ev {
    bool is_key;
    struct key_event k;
    struct mouse_event m;
};
static struct in_ev inq[INQ];
static volatile int in_head, in_tail;

static void key_sink(const struct key_event *e) {
    int nh = (in_head + 1) % INQ;
    if (nh == in_tail) return;
    inq[in_head].is_key = true;
    inq[in_head].k = *e;
    in_head = nh;
    wake_compositor();
}

static void mouse_sink(const struct mouse_event *e) {
    int nh = (in_head + 1) % INQ;
    if (nh == in_tail) return;
    inq[in_head].is_key = false;
    inq[in_head].m = *e;
    in_head = nh;
    wake_compositor();
}

static void toggle_maximize(struct window *w) {
    if (!(w->flags & WIN_RESIZABLE)) return;
    damage_window(w);
    struct gui_event e = {0};
    e.type = EV_RESIZE;
    if (w->maximized) {
        w->maximized = false;
        w->x = w->restore.x;
        w->y = w->restore.y;
        e.x = w->restore.w;
        e.y = w->restore.h;
    } else {
        w->maximized = true;
        w->restore = (struct rect){w->x, w->y, w->cw, w->ch};
        w->x = 0;
        w->y = 0;
        e.x = sw - 2 * BORDER;
        e.y = sh - TASKBAR_H - TITLE_H - BORDER;
    }
    damage_window(w);
    push_event(w, &e);
}

static void cycle_windows(void) {
    struct window *f = focused();
    if (!f) {
        /* restore the most recent minimized window */
        for (int i = nwin - 1; i >= 0; i--)
            if (zorder[i]->shown && !zorder[i]->dead) {
                raise_window(zorder[i]);
                return;
            }
        return;
    }
    int count = 0;
    for (int i = 0; i < nwin; i++)
        if (visible(zorder[i])) count++;
    if (count < 2) return;
    lower_to_bottom(f);
}

static void handle_key(const struct key_event *k) {
    struct window *f = focused();
    if (k->pressed) {
        bool ctrl = k->mods & MOD_CTRL, alt = k->mods & MOD_ALT;
        if (k->key == KEY_SUPER || (ctrl && k->key == KEY_ESC)) {
            set_menu(!menu_open);
            return;
        }
        if (alt && k->key == KEY_TAB) {
            cycle_windows();
            return;
        }
        if (alt && k->key == KEY_F4) {
            if (f) send_simple(f, EV_CLOSE);
            return;
        }
        if (ctrl && alt && (k->key == 't' || k->key == 'T')) {
            launch("/bin/term", NULL);
            return;
        }
        if (vol_open && k->key == KEY_ESC) {
            set_volume_panel(false);
            return;
        }
        if (menu_open) {
            if (k->key == KEY_ESC) set_menu(false);
            else if (k->key == KEY_UP || k->key == KEY_DOWN) {
                int d = k->key == KEY_UP ? -1 : 1;
                menu_sel = menu_sel < 0 ? (d > 0 ? 0 : NAPPS - 1) : (menu_sel + d + NAPPS) % NAPPS;
                struct rect m = menu_rect();
                damage_rect(m.x, m.y, m.w, m.h);
            } else if (k->key == KEY_ENTER && menu_sel >= 0) {
                int s = menu_sel;
                set_menu(false);
                launch(apps[s].path, NULL);
            }
            return;
        }
    }
    if (k->key == KEY_SUPER) return;
    if (f) {
        struct gui_event e = {0};
        e.type = EV_KEY;
        e.key = k->key;
        e.mods = k->mods;
        e.pressed = k->pressed;
        push_event(f, &e);
    } else if (k->pressed && sel_icon >= 0 && k->key == KEY_ENTER) {
        launch(apps[desktop_apps[sel_icon]].path, NULL);
    }
}

static void update_hover(void) {
    struct window *w = window_at(mx, my);
    int b = w ? hit_button(w, mx, my) : 0;
    if (w != hover_win || b != hover_btn) {
        if (hover_win && hover_btn) damage_rect(hover_win->x + frame_w(hover_win) - 80, hover_win->y, 80, TITLE_H);
        hover_win = b ? w : NULL;
        hover_btn = b;
        if (hover_win) damage_rect(w->x + frame_w(w) - 80, w->y, 80, TITLE_H);
    }
    int shape = (drag_mode == DRAG_RESIZE || (w && drag_mode == DRAG_NONE && in_resize_grip(w, mx, my)))
                    ? CURSOR_RESIZE
                    : CURSOR_ARROW;
    if (shape != cursor_shape) {
        cursor_shape = shape;
        damage_cursor();
    }
}

static void left_press(void) {
    uint64_t now = uptime_ms();
    bool dbl = now - last_click_ms < 450 && ABS(mx - last_click_x) < 6 && ABS(my - last_click_y) < 6;
    last_click_ms = dbl ? 0 : now;
    last_click_x = mx;
    last_click_y = my;

    if (vol_open) {
        struct rect v = vol_rect(), mr = vol_mute_rect(), s = vol_slider_rect(), b = vol_button_rect();
        if (in_rect(mx, my, v.x, v.y, v.w, v.h)) {
            if (in_rect(mx, my, mr.x, mr.y, mr.w, mr.h)) {
                audio_set_muted(!audio_muted());
                damage_volume();
                volume_tick();
            } else if (in_rect(mx, my, s.x - 10, s.y - 14, s.w + 20, s.h + 28)) {
                drag_mode = DRAG_VOLUME;
                volume_from_mouse();
            }
            return;
        }
        set_volume_panel(false);
        if (in_rect(mx, my, b.x, b.y, b.w, b.h)) return; /* clicking the speaker again closes */
    }
    if (menu_open) {
        struct rect m = menu_rect();
        if (in_rect(mx, my, m.x, m.y, m.w, m.h)) {
            int i = menu_item_at(mx, my);
            if (i >= 0) {
                set_menu(false);
                launch(apps[i].path, NULL);
            }
            return;
        }
        set_menu(false);
        if (my >= sh - TASKBAR_H && mx < TB_START_W) return; /* clicking start again closes */
    }
    if (my >= sh - TASKBAR_H) {
        if (mx < TB_START_W) {
            set_menu(true);
            return;
        }
        struct rect vb = vol_button_rect();
        if (in_rect(mx, my, vb.x, vb.y, vb.w, vb.h)) {
            set_volume_panel(true);
            return;
        }
        struct window *w;
        if (taskbar_window_at(mx, my, &w)) {
            if (w == focused()) minimize_window(w);
            else raise_window(w);
        }
        return;
    }
    struct window *w = window_at(mx, my);
    if (w) {
        if (sel_icon >= 0) {
            int x, y;
            icon_cell(sel_icon, &x, &y);
            damage_rect(x, y, ICON_CELL_W, ICON_CELL_H);
            sel_icon = -1;
        }
        if (w != focused()) raise_window(w);
        int b = hit_button(w, mx, my);
        if (b) {
            drag_mode = DRAG_BUTTON;
            drag_win = w;
            drag_btn = b;
            return;
        }
        if (in_resize_grip(w, mx, my)) {
            drag_mode = DRAG_RESIZE;
            drag_win = w;
            resize_rect = (struct rect){w->x, w->y, frame_w(w), frame_h(w)};
            drag_dx = w->x + frame_w(w) - mx;
            drag_dy = w->y + frame_h(w) - my;
            resize_visible = true;
            damage_rect(resize_rect.x, resize_rect.y, resize_rect.w, resize_rect.h);
            return;
        }
        if (decorated(w) && my < w->y + TITLE_H) {
            if (dbl) {
                toggle_maximize(w);
                return;
            }
            drag_mode = DRAG_MOVE;
            drag_win = w;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
            return;
        }
        if (in_rect(mx, my, client_x(w), client_y(w), w->cw, w->ch)) {
            drag_mode = DRAG_CLIENT;
            drag_win = w;
            send_mouse(w, EV_MOUSE_DOWN, mbuttons);
        }
        return;
    }
    /* desktop */
    int ic = icon_at(mx, my);
    if (ic != sel_icon) {
        int x, y;
        if (sel_icon >= 0) {
            icon_cell(sel_icon, &x, &y);
            damage_rect(x, y, ICON_CELL_W, ICON_CELL_H);
        }
        sel_icon = ic;
        if (ic >= 0) {
            icon_cell(ic, &x, &y);
            damage_rect(x, y, ICON_CELL_W, ICON_CELL_H);
        }
    }
    if (ic >= 0 && dbl) launch(apps[desktop_apps[ic]].path, NULL);
}

static void left_release(void) {
    struct window *w = drag_win;
    int mode = drag_mode;
    drag_mode = DRAG_NONE;
    drag_win = NULL;
    if (mode == DRAG_VOLUME) volume_tick(); /* let the new level be heard */
    if (!w) return;
    switch (mode) {
    case DRAG_BUTTON:
        if (hit_button(w, mx, my) == drag_btn) {
            if (drag_btn == 1) send_simple(w, EV_CLOSE);
            else if (drag_btn == 2) toggle_maximize(w);
            else minimize_window(w);
        }
        break;
    case DRAG_RESIZE: {
        resize_visible = false;
        damage_rect(resize_rect.x, resize_rect.y, resize_rect.w, resize_rect.h);
        struct gui_event e = {0};
        e.type = EV_RESIZE;
        e.x = resize_rect.w - (frame_w(w) - w->cw);
        e.y = resize_rect.h - (frame_h(w) - w->ch);
        if (e.x != w->cw || e.y != w->ch) push_event(w, &e);
        break;
    }
    case DRAG_CLIENT: send_mouse(w, EV_MOUSE_UP, mbuttons); break;
    default: break;
    }
}

static void handle_mouse(const struct mouse_event *m) {
    int dx = m->dx, dy = m->dy;
    if (m->absolute) {
        /* a tablet-style pointer (Hyper-V's synthetic mouse): go exactly where the host cursor is */
        dx = (int)((int64_t)m->x * (sw - 1) / MOUSE_ABS_MAX) - mx;
        dy = (int)((int64_t)m->y * (sh - 1) / MOUSE_ABS_MAX) - my;
    } else {
        /* mild acceleration */
        if (ABS(dx) > 5) dx *= 2;
        if (ABS(dy) > 5) dy *= 2;
    }
    int ox = mx, oy = my;
    if (dx || dy) {
        damage_cursor();
        mx = MAX(0, MIN(sw - 1, mx + dx));
        my = MAX(0, MIN(sh - 1, my + dy));
        damage_cursor();
    }
    int old_buttons = mbuttons;
    mbuttons = m->buttons;
    bool moved = mx != ox || my != oy;

    if (moved) {
        if (drag_mode == DRAG_MOVE && drag_win) {
            struct window *w = drag_win;
            damage_window(w);
            w->x = mx - drag_dx;
            w->y = my - drag_dy;
            if (w->y < 0) w->y = 0;
            if (w->y > sh - TASKBAR_H - TITLE_H) w->y = sh - TASKBAR_H - TITLE_H;
            if (w->x > sw - 60) w->x = sw - 60;
            if (w->x + frame_w(w) < 60) w->x = 60 - frame_w(w);
            w->maximized = false;
            damage_window(w);
        } else if (drag_mode == DRAG_RESIZE && drag_win) {
            struct window *w = drag_win;
            damage_rect(resize_rect.x, resize_rect.y, resize_rect.w, resize_rect.h);
            int nw = mx + drag_dx - w->x, nh = my + drag_dy - w->y;
            int minw = MIN_CW + (frame_w(w) - w->cw), minh = MIN_CH + (frame_h(w) - w->ch);
            resize_rect.w = MAX(minw, MIN(nw, sw - w->x));
            resize_rect.h = MAX(minh, MIN(nh, sh - TASKBAR_H - w->y));
            damage_rect(resize_rect.x, resize_rect.y, resize_rect.w, resize_rect.h);
        } else if (drag_mode == DRAG_CLIENT && drag_win) {
            send_mouse(drag_win, EV_MOUSE_MOVE, mbuttons);
        } else if (drag_mode == DRAG_VOLUME) {
            volume_from_mouse();
        } else if (drag_mode == DRAG_NONE) {
            struct rect vb = vol_button_rect();
            if (in_rect(mx, my, vb.x, vb.y, vb.w, vb.h) != in_rect(ox, oy, vb.x, vb.y, vb.w, vb.h))
                damage_rect(vb.x, vb.y, vb.w, vb.h); /* hover highlight */
            struct window *w = window_at(mx, my);
            if (w && in_rect(mx, my, client_x(w), client_y(w), w->cw, w->ch)) send_mouse(w, EV_MOUSE_MOVE, mbuttons);
            if (menu_open) {
                int s = menu_item_at(mx, my);
                if (s != menu_sel) {
                    menu_sel = s;
                    struct rect r = menu_rect();
                    damage_rect(r.x, r.y, r.w, r.h);
                }
            }
        }
        update_hover();
    }

    int changed = old_buttons ^ mbuttons;
    if (changed & 1) {
        if (mbuttons & 1) left_press();
        else left_release();
        update_hover();
    }
    if (changed & 6) {
        /* right / middle buttons go straight to the window under the cursor */
        struct window *w = drag_mode == DRAG_CLIENT ? drag_win : window_at(mx, my);
        if (w && in_rect(mx, my, client_x(w), client_y(w), w->cw, w->ch)) {
            if (w != focused() && (mbuttons & 6)) raise_window(w);
            send_mouse(w, (mbuttons & changed & 6) ? EV_MOUSE_DOWN : EV_MOUSE_UP, mbuttons);
        }
    }
    if (m->wheel && over_volume(mx, my)) { /* scrolling over the speaker or its panel */
        audio_set_volume(audio_volume() - m->wheel * 5);
        damage_volume();
        volume_tick();
    } else if (m->wheel) {
        struct window *w = window_at(mx, my);
        if (!w) w = focused();
        if (w) {
            struct gui_event e = {0};
            e.type = EV_WHEEL;
            e.x = mx - client_x(w);
            e.y = my - client_y(w);
            e.wheel = m->wheel;
            push_event(w, &e);
        }
    }
}

/* ---- compositor thread ---- */
static void wm_thread(void *arg) {
    damage_all();
    for (;;) {
        uint64_t fl = irq_save();
        if (in_head == in_tail && ndamage == 0) wq_wait_timeout(&wm_wq, 250);
        irq_restore(fl);

        while (in_tail != in_head) {
            struct in_ev ev = inq[in_tail];
            in_tail = (in_tail + 1) % INQ;
            if (ev.is_key) handle_key(&ev.k);
            else handle_mouse(&ev.m);
        }

        struct tm_parts tp;
        time_to_parts(time_now(), &tp);
        if (tp.min != last_clock_min) {
            last_clock_min = tp.min;
            damage_rect(sw - TB_CLOCK_W, sh - TASKBAR_H, TB_CLOCK_W, TASKBAR_H);
        }
        if (audio_volume() != shown_volume || audio_muted() != shown_muted) damage_volume();
        if (toast_until && uptime_ms() > toast_until) {
            toast_until = 0;
            struct rect t = toast_rect();
            damage_rect(t.x, t.y, t.w, t.h);
        }
        composite();
    }
}

bool wm_running(void) { return running; }

void wm_screen_size(uint32_t *w, uint32_t *h) {
    *w = (uint32_t)sw;
    *h = (uint32_t)sh;
}

bool wm_clipboard_set(const char *text, size_t n) {
    if (n > WM_CLIPBOARD_MAX) return false;
    char *nb = kmalloc(n + 1);
    if (!nb) return false;
    memcpy(nb, text, n);
    nb[n] = 0;
    if (clipboard) kfree(clipboard);
    clipboard = nb;
    clipboard_len = n;
    clipboard_seq++;
    return true;
}

const char *wm_clipboard(size_t *len, uint32_t *seq) {
    *len = clipboard_len;
    if (seq) *seq = clipboard_seq;
    return clipboard ? clipboard : "";
}

/* ---- remote display ---- */

/* Change the size of the desktop. The framebuffer keeps its own size (fb_present clips), so a
   remote viewer can have a desktop of the size its window has. Only call this from a kernel
   thread: the compositor must not be in the middle of a frame. */
static bool set_screen_size(int w, int h) {
    if (w == sw && h == sh) return true;
    size_t pages = ALIGN_UP((uint64_t)w * h * 4, PAGE_SIZE) / PAGE_SIZE;
    size_t old = ALIGN_UP((uint64_t)sw * sh * 4, PAGE_SIZE) / PAGE_SIZE;
    uint32_t *bb = vmalloc(pages), *wp = vmalloc(pages);
    if (!bb || !wp) {
        if (bb) vfree(bb, pages);
        if (wp) vfree(wp, pages);
        return false;
    }
    vfree(back.px, old);
    vfree(wall.px, old);
    sw = w;
    sh = h;
    gfx_init(&back, bb, sw, sh, sw);
    gfx_init(&wall, wp, sw, sh, sw);
    desktop_draw_wallpaper(&wall);
    draw_desktop_icons(&wall);
    mx = MIN(mx, sw - 1);
    my = MIN(my, sh - 1);
    drag_mode = DRAG_NONE;
    resize_visible = false;
    for (int i = 0; i < nwin; i++) {
        struct window *win = zorder[i];
        if (win->maximized) {
            struct gui_event e = {0};
            e.type = EV_RESIZE;
            e.x = sw - 2 * BORDER;
            e.y = sh - TASKBAR_H - TITLE_H - BORDER;
            push_event(win, &e);
            continue;
        }
        if (win->x + frame_w(win) > sw) win->x = MAX(0, sw - frame_w(win));
        if (win->y + frame_h(win) > sh - TASKBAR_H) win->y = MAX(0, sh - TASKBAR_H - frame_h(win));
    }
    ndamage = 0;
    nrdamage = 0;
    damage_all();
    kprintf("wm: desktop is now %dx%d\n", sw, sh);
    return true;
}

void wm_remote_resize(int *w, int *h) {
    if (*w > 0 && *h > 0) set_screen_size(MAX(640, MIN(*w, 3840)), MAX(480, MIN(*h, 2160)));
    *w = sw;
    *h = sh;
    damage_all(); /* repaint without the pointer */
    wake_compositor();
}

bool wm_remote_attach(int *w, int *h) {
    if (!running) return false;
    remote_users++;
    wm_remote_resize(w, h);
    return true;
}

void wm_remote_detach(void) {
    if (remote_users > 0) remote_users--;
    if (!remote_users) set_screen_size((int)fb.width, (int)fb.height);
    damage_all();
    wake_compositor();
}

int wm_remote_damage(struct wm_rect *out, int max) {
    uint64_t f = irq_save();
    int n = MIN(nrdamage, max);
    for (int i = 0; i < n; i++) out[i] = (struct wm_rect){rdamage[i].x, rdamage[i].y, rdamage[i].w, rdamage[i].h};
    nrdamage = 0;
    irq_restore(f);
    return n;
}

const uint32_t *wm_remote_frame(int *pitch) {
    *pitch = back.pitch;
    return back.px;
}

int wm_cursor_shape(void) { return cursor_shape; }

void wm_init(void) {
    /* PCI has been enumerated. Optional offscreen GPU never changes the display
       or the RAM frame used by Enhanced Session/RDP. */
    gpu_init();
    sw = fb.width;
    sh = fb.height;
    size_t pages = ALIGN_UP((uint64_t)sw * sh * 4, PAGE_SIZE) / PAGE_SIZE;
    uint32_t *bb = vmalloc(pages), *wp = vmalloc(pages);
    if (!bb || !wp) {
        kprintf("wm: out of memory for buffers\n");
        return;
    }
    gfx_init(&back, bb, sw, sh, sw);
    gfx_init(&wall, wp, sw, sh, sw);
    desktop_draw_wallpaper(&wall);
    draw_desktop_icons(&wall);
    fb_benchmark(wall.px,wall.pitch);
    mx = sw / 2;
    my = sh / 2;
    running = true;
    fbcon_set_enabled(false);
    input_set_sinks(key_sink, mouse_sink);
    kthread_create("compositor", wm_thread, NULL);
    kprintf("wm: compositor started at %dx%d\n", sw, sh);
    wm_notify("Welcome to " OS_NAME, "Double-click an icon or open the moon menu.");
}

void wm_notify(const char *title, const char *text) {
    if (!running) {
        kprintf("[%s] %s\n", title, text);
        return;
    }
    strlcpy(toast_title, title, sizeof toast_title);
    strlcpy(toast_text, text, sizeof toast_text);
    toast_until = uptime_ms() + 4000;
    struct rect t = toast_rect();
    damage_rect(t.x, t.y, t.w, t.h);
    wake_compositor();
}

void wm_process_exit(int pid) {
    if (!running) return;
    for (int i = nwin - 1; i >= 0; i--) {
        if (i >= nwin) continue;
        if (zorder[i]->pid == pid) win_destroy(zorder[i]);
    }
}

/* ---- syscalls ---- */
static struct window *fd_window(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    struct file *f = current_task->fds[fd];
    if (!f || f->vn->type != VT_WINDOW) return NULL;
    struct window *w = f->vn->priv;
    return w->dead ? NULL : w;
}

static int64_t sys_win_create(int cw, int ch, const char *utitle, int flags) {
    if (!running) return -ENOSYS;
    if (nwin >= MAX_WIN) return -ENOMEM;
    if (cw < 16) cw = 16;
    if (ch < 16) ch = 16;
    if (cw > sw) cw = sw;
    if (ch > sh) ch = sh;
    struct window *w = kzalloc(sizeof *w);
    if (!w) return -ENOMEM;
    if (utitle && user_str(w->title, utitle, sizeof w->title) < 0) strcpy(w->title, "window");
    if (!utitle) strcpy(w->title, current_task->name);
    w->flags = flags;
    w->pid = current_task->pid;
    w->id = next_win_id++;
    if (!alloc_buffer(w, cw, ch)) {
        kfree(w);
        return -ENOMEM;
    }
    if (flags & WIN_CENTER) {
        w->x = (sw - frame_w(w)) / 2;
        w->y = (sh - TASKBAR_H - frame_h(w)) / 2;
    } else {
        static int cascade;
        w->x = 140 + (cascade % 8) * 30;
        w->y = 40 + (cascade % 8) * 26;
        cascade++;
        if (w->x + frame_w(w) > sw) w->x = MAX(0, sw - frame_w(w));
        if (w->y + frame_h(w) > sh - TASKBAR_H) w->y = MAX(0, sh - TASKBAR_H - frame_h(w));
    }
    struct vnode *v = kzalloc(sizeof *v);
    if (!v) {
        vfree(w->buf, w->pages);
        kfree(w);
        return -ENOMEM;
    }
    strlcpy(v->name, "window", sizeof v->name);
    v->type = VT_WINDOW;
    v->ops = &win_ops;
    v->priv = w;
    v->unlinked = true;
    w->vn = v;
    struct file *f = vfs_open_vnode(v, O_RDWR);
    int fd = -EMFILE;
    for (int i = 0; i < MAX_FDS; i++)
        if (!current_task->fds[i]) {
            current_task->fds[i] = f;
            fd = i;
            break;
        }
    if (fd < 0) {
        vfs_close(f); /* destroys the window and frees everything */
        return fd;
    }
    w->pml4 = current_task->pml4;
    zorder[nwin++] = w;
    return fd;
}

static void show_if_needed(struct window *w) {
    if (w->shown) return;
    w->shown = true;
    raise_window(w);
    damage_window(w);
    damage_rect(0, sh - TASKBAR_H, sw, TASKBAR_H);
}

int64_t wm_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    switch (num) {
    case SYS_WIN_CREATE: return sys_win_create((int)a, (int)b, (const char *)c, (int)d);
    case SYS_WIN_MAP: {
        struct window *w = fd_window((int)a);
        if (!w) return -EBADF;
        if (w->pml4 != current_task->pml4) return -EPERM;
        if (w->uaddr) return (int64_t)w->uaddr;
        uint64_t va = map_user(w, current_task);
        return va ? (int64_t)va : -ENOMEM;
    }
    case SYS_WIN_PRESENT: {
        struct window *w = fd_window((int)a);
        if (!w) return -EBADF;
        int x = (int)b, y = (int)c, rw = (int)(d >> 32), rh = (int)(d & 0xFFFFFFFF);
        if (rw <= 0 || rh <= 0) {
            x = y = 0;
            rw = w->cw;
            rh = w->ch;
        }
        if (!w->shown) show_if_needed(w);
        else if (visible(w)) damage_rect(client_x(w) + x, client_y(w) + y, MIN(rw, w->cw - x), MIN(rh, w->ch - y));
        wake_compositor();
        return 0;
    }
    case SYS_WIN_SET_TITLE: {
        struct window *w = fd_window((int)a);
        if (!w) return -EBADF;
        if (user_str(w->title, (const char *)b, sizeof w->title) < 0) return -EFAULT;
        if (w->shown) {
            damage_rect(w->x, w->y, frame_w(w), TITLE_H);
            damage_rect(0, sh - TASKBAR_H, sw, TASKBAR_H);
            wake_compositor();
        }
        return 0;
    }
    case SYS_SCREEN_INFO: {
        struct screen_info *si = (struct screen_info *)a;
        if (!user_ok_w(si, sizeof *si)) return -EFAULT;
        si->width = running ? sw : fb.width;
        si->height = running ? sh - TASKBAR_H : fb.height;
        return running ? 0 : -ENOSYS;
    }
    case SYS_WIN_MOVE: {
        struct window *w = fd_window((int)a);
        if (!w) return -EBADF;
        damage_window(w);
        w->x = (int)b;
        w->y = (int)c;
        damage_window(w);
        wake_compositor();
        return 0;
    }
    case SYS_CLIPBOARD_SET: {
        size_t n = b;
        if (n > WM_CLIPBOARD_MAX) return -E2BIG;
        if (!user_ok((void *)a, n)) return -EFAULT;
        return wm_clipboard_set((const char *)a, n) ? 0 : -ENOMEM;
    }
    case SYS_CLIPBOARD_GET: {
        size_t n = MIN(b, clipboard_len);
        if (!user_ok_w((void *)a, n)) return -EFAULT;
        if (n) memcpy((void *)a, clipboard, n);
        return (int64_t)clipboard_len;
    }
    case SYS_WIN_RESIZE: {
        struct window *w = fd_window((int)a);
        if (!w) return -EBADF;
        if (w->pml4 != current_task->pml4) return -EPERM;
        int ncw = MAX(16, MIN((int)b, sw)), nch = MAX(16, MIN((int)c, sh));
        damage_window(w);
        uint32_t *obuf = w->buf;
        size_t opages = w->pages;
        int ocw = w->cw, och = w->ch;
        bool was_mapped = w->uaddr != 0;
        if (!alloc_buffer(w, ncw, nch)) {
            w->buf = obuf;
            w->pages = opages;
            return -ENOMEM;
        }
        for (int y = 0; y < MIN(och, nch); y++) memcpy(&w->buf[y * ncw], &obuf[y * ocw], MIN(ocw, ncw) * 4);
        /* unmap the old buffer from the owner */
        if (was_mapped) {
            for (size_t i = 0; i < opages; i++) vmm_unmap_page(w->pml4, w->uaddr + i * PAGE_SIZE);
            w->uaddr = 0;
        }
        vfree(obuf, opages);
        damage_window(w);
        wake_compositor();
        uint64_t va = map_user(w, current_task);
        return va ? (int64_t)va : -ENOMEM;
    }
    case SYS_SCREEN_GRAB: {
        /* copy the composed screen (desktop, windows, taskbar) as 0xAARRGGBB pixels; returns
           width << 16 | height. The buffer must hold width * height pixels (bytes = 0: size only). */
        if (!running) return -ENOSYS;
        uint32_t *dst = (uint32_t *)a;
        size_t need = (size_t)sw * (size_t)sh * 4;
        if (b == 0) return (int64_t)sw << 16 | sh; /* just the size */
        if (b < need) return -E2BIG;
        if (!user_ok_w(dst, need)) return -EFAULT;
        for (int y = 0; y < sh; y++) memcpy(dst + (size_t)y * (size_t)sw, back.px + (size_t)y * (size_t)back.pitch, (size_t)sw * 4);
        return (int64_t)sw << 16 | sh;
    }
    case SYS_GUI_LAUNCH: {
        char path[PATH_MAX_LEN], arg[PATH_MAX_LEN];
        if (user_str(path, (const char *)a, sizeof path) < 0) return -EFAULT;
        bool has_arg = b != 0;
        if (has_arg && user_str(arg, (const char *)b, sizeof arg) < 0) return -EFAULT;
        return proc_spawn_simple(path, has_arg ? arg : NULL, NULL);
    }
    default: return -ENOSYS;
    }
}
