/*
 * Nocturne's desktop: the window manager and the compositors.
 *
 * The window manager owns the windows, their stacking and focus and the pointer. The desktop
 * thread applies the input drivers' events to them, the window calls change them; both under
 * wm_lock, briefly. What the user sees of the system besides the windows' frames is the shell's, a
 * program with windows of its own in the layers of display_abi.h (N_CAP_SHELL).
 *
 * Each screen (dev/fb.h) is a part of the desktop, laid out from left to right in the screens'
 * order (screen 0 at the left), and has a compositor thread of its own with a
 * deadline at that screen's refresh (sys/sched.h). A frame takes the tiles that changed on its
 * part and a copy of the scene under wm_lock (capture), draws those tiles into the screen's
 * picture, a canvas in RAM, without it, writes them into a buffer the display does not show and
 * asks for a flip. Input, window calls and presents never wait for a frame being drawn.
 *
 * A window's pixels are surfaces: shared objects (mm/shared.h, shared_kernel_new) the app maps
 * (SYS_WIN_SURFACE) and the compositor reads through its own view of the same pages. A window
 * presented by rectangles has one surface and a kernel copy of it: each present copies its
 * rectangle into the copy under the copy's lock, which a frame takes while it reads it. A window
 * with WIN_FRAME_QUEUE presents whole surfaces in turn (or with WIN_MAILBOX the newest), and the
 * frames read them without a copy; a surface goes back to the app (EV_SURFACE) once another is
 * shown and no frame reads it.
 *
 * Input events are delivered through the window's handle, so an event queue can watch a window
 * together with channels.
 */
#include "kernel.h"
#include "gui/wm.h"
#include "gui/screenmode.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "dev/input.h"
#include "sys/object.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/process.h"
#include "sys/sched.h"
#include "sys/prof.h"
#include "mm/shared.h"
#include "sys/syscall.h"
#include "sys/settings.h"
#include "sys/thread.h"
#include "sys/wait.h"
#include "sys/watch.h"
#include "abi.h"
#include "display_abi.h"
#include "sys/authority.h"

#define TITLE_H   28
#define TITLE_RADIUS 8 /* the rounded title's corners; shared by the shadow's culling */
#define BORDER    1
#define SHADOW    10
#define POINTER_SIZE ((int)N_SCREEN_POINTER_SIZE) /* every pointer image is this square */
#define MIN_CW    120
#define MIN_CH    60
#define TILE      32   /* damage is kept in tiles of TILE x TILE pixels */
#define MAILBOX_FIRST 64
/* The events a window's mailbox keeps for an app that does not take them; beyond them input is
   lost (EV_INPUT_LOST). */
#define MAILBOX_MAX 4096
/* What a screen shows where no window is. */
#define BACKGROUND RGB(12, 10, 26)

struct rect {
    int x, y, w, h;
};

static bool rect_overlap(const struct rect *a, const struct rect *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static struct rect rect_union(struct rect a, struct rect b) {
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return (struct rect){x0, y0, x1 - x0, y1 - y0};
}

/* r cut to w x h at the origin; false when nothing is left. */
static bool rect_clip_to(struct rect *r, int w, int h) {
    int64_t x0 = MAX((int64_t)r->x, 0), y0 = MAX((int64_t)r->y, 0);
    int64_t x1 = MIN((int64_t)r->x + r->w, (int64_t)w), y1 = MIN((int64_t)r->y + r->h, (int64_t)h);
    if (x1 <= x0 || y1 <= y0) return false;
    *r = (struct rect){(int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)};
    return true;
}

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

static bool canvas_hits(const canvas_t *c, int x, int y, int w, int h) {
    return x < c->cx1 && y < c->cy1 && (int64_t)x + w > c->cx0 && (int64_t)y + h > c->cy0;
}

/* ---- tiles ----
   What changed of a picture of w x h pixels, a bit for each tile. */
struct tiles {
    uint64_t *bits;
    int cols, rows;
};

static size_t tiles_words(const struct tiles *t) { return ((size_t)t->cols * t->rows + 63) / 64; }

/* Tiles for w x h, all clear, into *t (whose old bits the caller frees); false without memory. */
static bool tiles_new(struct tiles *t, int w, int h) {
    struct tiles n = {NULL, (w + TILE - 1) / TILE, (h + TILE - 1) / TILE};
    n.bits = kzalloc(MAX(tiles_words(&n), (size_t)1) * sizeof *n.bits);
    if (!n.bits) return false;
    *t = n;
    return true;
}

/* The tiles of a rectangle within the picture (clipped to it by the caller). */
static void tiles_mark(struct tiles *t, struct rect r) {
    if (!t->bits || r.w <= 0 || r.h <= 0) return;
    int c0 = r.x / TILE, c1 = MIN((r.x + r.w - 1) / TILE, t->cols - 1);
    int r0 = r.y / TILE, r1 = MIN((r.y + r.h - 1) / TILE, t->rows - 1);
    for (int row = r0; row <= r1; row++)
        for (int col = c0; col <= c1; col++) {
            size_t i = (size_t)row * t->cols + col;
            t->bits[i / 64] |= 1ull << (i % 64);
        }
}

static void tiles_fill(struct tiles *t) {
    if (t->bits) tiles_mark(t, (struct rect){0, 0, t->cols * TILE, t->rows * TILE});
}

static bool tiles_any(const struct tiles *t) {
    for (size_t i = 0; t->bits && i < tiles_words(t); i++)
        if (t->bits[i]) return true;
    return false;
}

static void tiles_or(struct tiles *d, const struct tiles *s) {
    if (!d->bits || !s->bits || d->cols != s->cols || d->rows != s->rows) return;
    for (size_t i = 0; i < tiles_words(d); i++) d->bits[i] |= s->bits[i];
}

static void tiles_clear(struct tiles *t) {
    if (t->bits) memset(t->bits, 0, tiles_words(t) * sizeof *t->bits);
}

static bool tiles_test(const struct tiles *t, int col, int row) {
    size_t i = (size_t)row * t->cols + col;
    return t->bits[i / 64] >> (i % 64) & 1;
}

/* The rectangles the tiles cover within w x h: each row's runs, a run joined to the rectangle
   above it when that ended at the same columns on the row before. Into *list (count, room as
   kappend); false without memory (the list as far as it got). */
static bool tiles_rects(const struct tiles *t, int w, int h, struct rect **list, size_t *count, size_t *room) {
    *count = 0;
    if (!t->bits || !t->cols) return true;
    int *above = kmalloc((size_t)t->cols * 2 * sizeof *above), *now = above ? above + t->cols : NULL;
    if (!above) return false;
    for (int c = 0; c < t->cols; c++) above[c] = -1;
    bool ok = true;
    for (int row = 0; row < t->rows && ok; row++) {
        for (int c = 0; c < t->cols; c++) now[c] = -1;
        int y = row * TILE, bottom = MIN(y + TILE, h);
        for (int c = 0; c < t->cols;) {
            if (!tiles_test(t, c, row)) {
                c++;
                continue;
            }
            int c1 = c;
            while (c1 < t->cols && tiles_test(t, c1, row)) c1++;
            int x = c * TILE, right = MIN(c1 * TILE, w);
            int i = above[c];
            if (i >= 0 && (*list)[i].x + (*list)[i].w == right && (*list)[i].y + (*list)[i].h == y) {
                (*list)[i].h = bottom - (*list)[i].y;
            } else {
                struct rect *r = kappend(list, count, room, sizeof *r);
                if (!r) {
                    ok = false;
                    break;
                }
                *r = (struct rect){x, y, right - x, bottom - y};
                i = (int)(*count - 1);
            }
            now[c] = i;
            c = c1;
        }
        int *swap = above;
        above = now;
        now = swap;
    }
    kfree(MIN(above, now));
    return ok;
}

/* ---- surfaces ----
   A window's pixels: w x h of 0xAARRGGBB, with a deep plane of w x h bytes after them when deep
   (display_abi.h). object is the shared object the app maps (NULL for the kernel's copy of a
   window presented by rectangles, whose writers take lock). refs: the window's and the frames'
   that read it; the last put frees it (the pages stay while an app maps them). readers: the
   frames that read it now (a queued surface goes back to the app only without one). */
struct wsurf {
    unsigned refs;    /* atomic */
    unsigned readers; /* under wm_lock */
    struct object *object;
    uint32_t *px;
    size_t pages;
    int w, h;
    bool deep;
    struct mutex lock;
};

static size_t surf_bytes(int w, int h, bool deep) { return (size_t)w * h * (deep ? 5 : 4); }

/* Thread context, no lock that must not sleep held. */
static struct wsurf *wsurf_new(int w, int h, bool deep, bool shared) {
    struct wsurf *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    size_t bytes = surf_bytes(w, h, deep);
    void *view = NULL;
    if (shared) {
        s->object = shared_kernel_new(bytes, &view); /* zeroed: a deep plane of 0 is 8-bit content */
    } else {
        s->pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
        view = vmalloc(s->pages);
        if (view) memset(view, 0, bytes);
    }
    if (!view) {
        kfree(s);
        return NULL;
    }
    s->px = view;
    s->w = w;
    s->h = h;
    s->deep = deep;
    s->refs = 1;
    return s;
}

static struct wsurf *wsurf_get(struct wsurf *s) {
    if (s) __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
    return s;
}

/* Thread context (the last put unmaps the kernel's view), no spinlock held. */
static void wsurf_put(struct wsurf *s) {
    if (!s || __atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (s->object) shared_kernel_end(s->object, s->px);
    else vfree(s->px, s->pages);
    kfree(s);
}

/* Rows y0..y1, columns x0..x1 of src into dst at the same place (both hold them), with the deep
   plane when both have one. */
static void surf_copy(struct wsurf *dst, const struct wsurf *src, int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    for (int row = y0; row < y1; row++)
        memcpy(dst->px + (size_t)row * dst->w + x0, src->px + (size_t)row * src->w + x0, (size_t)(x1 - x0) * 4);
    if (!dst->deep || !src->deep) return;
    uint8_t *dd = (uint8_t *)(dst->px + (size_t)dst->w * dst->h);
    const uint8_t *sd = (const uint8_t *)(src->px + (size_t)src->w * src->h);
    for (int row = y0; row < y1; row++)
        memcpy(dd + (size_t)row * dst->w + x0, sd + (size_t)row * src->w + x0, (size_t)(x1 - x0));
}

/* ---- windows ---- */
enum { SURF_FREE, SURF_APP, SURF_QUEUED, SURF_SHOWN };

struct window {
    int id, pid;
    struct vmm_space *vmm_space; /* its app's (a reference): the calls that need the owner compare it */
    int x, y;   /* outer frame position on the desktop */
    int cw, ch; /* client size */
    int flags;
    bool queue, mailbox, deep;
    /* Under wm_lock. surf: the app's surfaces (WIN_FRAME_QUEUE: nsurf of them, else one); stable:
       the kernel's copy a window presented by rectangles is shown from. state: each surface's
       (SURF_*); queued: the presented ones in their order, queued_serial their presents' numbers;
       shown_surf the one shown (-1: none yet) and shown_serial its present's; owed: given back to
       the app (EV_SURFACE) once no frame reads it. */
    struct wsurf *surf[N_WIN_SURFACES_MAX];
    unsigned nsurf;
    struct wsurf *stable;
    uint8_t state[N_WIN_SURFACES_MAX];
    bool owed[N_WIN_SURFACES_MAX];
    int queued[N_WIN_SURFACES_MAX];
    uint64_t queued_serial[N_WIN_SURFACES_MAX];
    unsigned nqueued;
    int shown_surf;
    uint64_t shown_serial;
    uint64_t presents;   /* the last present's number */
    uint64_t surf_seq;   /* atomic: moves on as a surface may be acquired, before wq is woken */
    bool frame_wanted;   /* EV_FRAME owed after the next frame that shows it */
    /* The EV_FRAME a frame owes it: of screen note_screen, once that screen shows flip note_flip
       (0: the frame is being drawn). */
    bool note;
    unsigned note_screen;
    uint64_t note_flip, note_present;
    int note_surface;
    bool shown, minimized, maximized, dead, fullscreen;
    bool direct_wanted; /* win_direct: its own images shown by the display driver when it can be */
    /* SYS_WIN_POINTER: the pointer the window wants over its client area (N_POINTER_*) and the
       image it gave instead (pointer_w x pointer_h with its hot spot; NULL without one).
       pointer_serial counts the calls. */
    uint32_t pointer_shape;
    uint32_t *pointer_image;
    int pointer_w, pointer_h, pointer_hx, pointer_hy;
    uint32_t pointer_serial;
    struct rect restore;
    struct rect fullscreen_restore;
    char title[64];
    /* SYS_SHELL_PLACE: its layer, whether it stays out of the focus, whether its pixels blend with
       what is under them, and the edge of screen reserve_screen it keeps from the other windows. */
    uint32_t layer;
    bool no_focus, blend;
    uint32_t reserve_edge, reserve_screen;
    int reserve;
    /* The app's mailbox, under ev_lock (any CPU, without wm_lock: SYS_WIN_RECEIVE): the input
       events in a ring of ev_room (growing up to MAILBOX_MAX), lost when it was full, the frame's
       event (the last one only) and the surfaces given back (bits), and dead's change. It comes
       after wm_lock, and before w->wq's lock and the object's. */
    struct spinlock ev_lock;
    struct gui_event *ev;
    unsigned ev_first, ev_count, ev_room;
    bool lost;
    bool frame_ready;
    struct gui_event frame_event;
    uint32_t surfaces_back;
    struct wait_queue wq;
    struct object object; /* what the app's window handle reaches */
};

/* ---- screens ----
   A screen's part of the desktop and its compositor. Never freed; wscreens only grows. */
/* A window as a frame draws it, in the screen's coordinates. */
struct layer {
    int x, y, cw, ch, fw, fh, cx, cy, flags, hover;
    bool focus, maximized, shadow, blend;
    struct wsurf *src;  /* a reference; readers counted when it is an app's */
    bool copy;          /* src is the kernel's copy: its lock is taken while it is read */
    char title[64];
    struct rect bounds; /* clipped to the screen */
};

struct wscreen {
    unsigned id;
    struct screen *s;
    struct thread *thread;
    /* Under wm_lock: */
    bool on;                  /* its part is on the desktop */
    int x, y, w, h;           /* the part: its mode's size, or (screen 0) a remote viewer's window's */
    bool placed;              /* the display service says where the part is: place_x, place_y */
    int place_x, place_y;
    int was_x, was_y;         /* where it was before an arrangement */
    uint32_t scale_percent;   /* the display service's, for the programs to draw by */
    int remote_w, remote_h;   /* screen 0 while remote_users */
    int reserved[N_EDGE_RIGHT + 1]; /* what the shell's windows keep of each edge (enum n_screen_edge) */
    struct tiles dirty;       /* what changed since its last frame */
    bool dirty_any;
    uint64_t period_ns;       /* its refresh's period, 0: not known */
    bool vrr;                 /* a variable refresh: a frame as soon as one is ready, period the shortest */
    uint32_t refresh_mhz, vrr_min_mhz;
    bool direct_able;
    struct window *direct_win;
    uint32_t direct_pointer;  /* the pointer_serial of the image in state, 0 for none */
    struct n_screen_state state; /* what its driver reads (n_pci_screen_state) */
    struct wait_queue state_wq;
    /* The compositor thread's own; the picture under back_lock (a frame, a copy for a grab or a
       remote viewer, a change of size): */
    struct mutex back_lock;
    canvas_t back;
    uint8_t *back_deep;              /* its deep plane (display_abi.h), pitch w; NULL at 8 bpc */
    struct tiles frame;              /* the tiles of the frame being drawn */
    struct tiles stale[SCREEN_BUFFERS_MAX]; /* what each buffer lacks of the picture */
    struct tiles remote;             /* screen 0: drawn since a remote viewer last asked */
    struct tiles both;
    uint64_t generation;             /* the screen's buffers' (screen_frame_begin) */
    struct rect *rects;
    size_t nrects, rects_room;
    struct layer *layers;
    size_t nlayers, layers_room;
    bool resize_on;
    struct rect resize;
    bool pointer_on;
    int pointer_x, pointer_y;
    uint32_t pointer_serial;
    uint32_t pointer_px[POINTER_SIZE * POINTER_SIZE];
    uint32_t under[POINTER_SIZE * POINTER_SIZE];
    uint64_t frame_next;   /* the next frame's earliest start where flips show at once */
    bool immediate;        /* its flips show as they are asked (no vertical blank to wait for) */
    uint64_t policy_ns;    /* the period its thread's deadline is at, 0: fair */
    bool policy_set;
    /* Counts for prof (sys/prof.h): */
    uint64_t stats_since, render_ns, render_max, write_ns, write_max;
    unsigned frames, late;
};

/* ---- state ----
   Locks, in the order they are taken:
   - a screen's back_lock (its picture), then dev/fb.c's screen use (a frame's begin to its end);
   - wm_mutex (wm_lock): all of the window manager's state that names no other lock: the windows,
     their stacking and focus, the damage, the pointer, the desktop's own state, the screens'
     parts and states, the remote viewers, the clipboard, the hot keys. A sleeping lock, any CPU, held briefly:
     never while a frame is drawn or written, nor across a copy of a window's pixels. Reentrant: a
     window handle's last close comes from inside a WM call. gui/screenmode.c's lock,
     and dev/pciuser.c's screen lock come before it;
   - a surface's lock (the kernel's copy of a window presented by rectangles);
   - then one of a window's ev_lock and in_lock (the input ring): spinlocks, never two of them at
     once;
   - then the locks of wait queues and objects (the wakes and notifies made under them). */
static bool running; /* set once, before the threads start */
static struct mutex wm_mutex;
static void wm_lock(void) { mutex_lock(&wm_mutex); }
static void wm_unlock(void) { mutex_unlock(&wm_mutex); }

static struct wscreen **wscreens; /* by screen number */
static size_t nwscreens, wscreens_room;
static int sw, sh;          /* screen 0's part */
static int desk_w, desk_h;  /* the desktop's extent */

/* The desktop thread sleeps on desk_wq unless desk_wakes moved: input, a setting, a window that
   moved under the pointer. */
static uint64_t desk_wakes;
static struct wait_queue desk_wq;
static void desk_wake(void) {
    __atomic_add_fetch(&desk_wakes, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&desk_wq);
}

/* The user's (struct desktop_settings): pointer speed with its leftover quarter steps, and smooth
   gradients as the user wants them; the desktop thread applies them while screen 0 takes them
   (fb_dither_able). Told by sys/settings.h, and not saved here. Under wm_lock. */
static bool deep_wanted;
/* What SYS_SCREEN_INFO reports changed: its watches (SCREEN_WATCH) hear of it. From wm_init on;
   without the desktop there is none. seen_dither_able and seen_boot are screen 0 as the desktop
   thread last saw it. */
static struct watched screen_watched;
static bool screen_watchable, seen_dither_able, seen_boot;

static struct window **zorder; /* bottom .. top */
static size_t nwin, zorder_room;
static int next_win_id = 1;
static struct window *focus_owner;

static int mx, my, mbuttons;
/* The input's moment (struct key_event time_ns) while the desktop thread applies it, else 0. */
static uint64_t input_at;

/* Remote display (vm/rdp.c): remote_users changes under wm_lock and is read without it too
   (wm_remote_viewing). */
static int remote_users;

/* The pointer (SYS_WIN_POINTER, abi.h). The WM owns it: it draws it at (mx, my) less its hot spot,
   and chooses what it shows (pointer_update): one of its own shapes (pointer_art), or the image
   the window under it gave. pointer_px is that image, pointer_shape the shape it was chosen as and
   pointer_from the id of the window whose image it is (0 for the WM's art) as of that window's
   pointer_from_serial. pointer_serial changes with the image and is never 0. Under the WM lock
   (wm_pointer_image reads pointer_serial without it first). */
static uint32_t pointer_px[POINTER_SIZE * POINTER_SIZE];
static int pointer_hx, pointer_hy;
static int pointer_shape = -1, pointer_from;
static uint32_t pointer_from_serial, pointer_serial = 1;

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CLIENT, DRAG_BUTTON };
static int drag_mode;
static struct window *drag_win;
static int drag_dx, drag_dy, drag_btn;
static struct rect resize_rect;
static bool resize_visible;

static struct window *hover_win;
static int hover_btn; /* 0 none, 1 close, 2 max, 3 min */

static uint64_t last_click;      /* clock ns of the last press */
static int last_click_x, last_click_y, last_click_button;
static uint32_t press_clicks = 1; /* the press being applied: how many in a row, the EV_MOUSE_DOWN's clicks */

/* The window under the pointer, told EV_MOUSE_LEAVE when it leaves one of the shell's layers. */
static struct window *pointer_over;

/* SYS_SHELL_HOTKEY: the keys whose press goes to their window, whichever has the focus. held: the
   press went there, so its release goes nowhere. */
struct hotkey {
    struct window *w;
    uint32_t usage, mods;
    bool held;
};
static struct hotkey *hotkeys;
static size_t nhotkeys, hotkeys_room;

/* What the shell's watch reports changed (SYS_SHELL_WATCH): the windows, their order or the focus. */
static struct watched shell_watched;
static bool shell_watchable;
static void windows_changed(void) {
    if (__atomic_load_n(&shell_watchable, __ATOMIC_ACQUIRE)) watched_changed(&shell_watched);
}

static char *clipboard;
static size_t clipboard_len;
static uint32_t clipboard_seq; /* bumped on every change, so a remote viewer can follow it */
static struct n_identity clipboard_owner; /* 最後に置いたアプリの身元：触れてよい者だけが読む */
static bool clipboard_open;               /* 核の thread が置いた（遠隔の表示）：持ち主がない */

/* ---- the screens' parts ---- */
static bool part_on(const struct wscreen *ws) { return ws->on && ws->w > 0 && ws->h > 0; }

static struct wscreen *primary_screen; /* the display service's (SYS_SCREEN_ARRANGE); NULL: screen 0. Under wm_lock. */

/* The main screen: new windows and the work area of SYS_SCREEN_INFO. wm_lock held. */
static struct wscreen *screen_main(void) {
    if (primary_screen && part_on(primary_screen)) return primary_screen;
    return nwscreens ? wscreens[0] : NULL;
}

/* The part under a point of the desktop; the main screen's when none is. wm_lock held. */
static struct wscreen *screen_at(int x, int y) {
    for (size_t i = 0; i < nwscreens; i++) {
        struct wscreen *ws = wscreens[i];
        if (part_on(ws) && in_rect(x, y, ws->x, ws->y, ws->w, ws->h)) return ws;
    }
    return screen_main();
}

/* ---- damage tracking ---- */
/* wm_lock held. A part that had nothing to draw wakes its compositor. */
static void damage_rect(int x, int y, int w, int h) {
    for (size_t i = 0; i < nwscreens; i++) {
        struct wscreen *ws = wscreens[i];
        if (!part_on(ws)) continue;
        struct rect r = {x - ws->x, y - ws->y, w, h};
        if (!rect_clip_to(&r, ws->w, ws->h)) continue;
        tiles_mark(&ws->dirty, r);
        if (!ws->dirty_any) {
            ws->dirty_any = true;
            screen_poke(ws->s);
        }
    }
}

static void damage_all(void) {
    for (size_t i = 0; i < nwscreens; i++) {
        struct wscreen *ws = wscreens[i];
        if (!part_on(ws)) continue;
        tiles_fill(&ws->dirty);
        ws->dirty_any = true;
        screen_poke(ws->s);
    }
}

/* ---- window geometry ---- */
static bool decorated(struct window *w) { return !w->fullscreen && !(w->flags & WIN_NO_DECOR); }
static int frame_w(struct window *w) { return decorated(w) ? w->cw + 2 * BORDER : w->cw; }
static int frame_h(struct window *w) { return decorated(w) ? w->ch + TITLE_H + BORDER : w->ch; }
static int client_x(struct window *w) { return decorated(w) ? w->x + BORDER : w->x; }
static int client_y(struct window *w) { return decorated(w) ? w->y + TITLE_H : w->y; }

/* The desktop and the panels lie flat; every other window has a shadow. */
static int shadow_of(const struct window *w) {
    return w->layer == N_LAYER_DESKTOP || w->layer == N_LAYER_PANEL ? 0 : SHADOW;
}

static struct rect win_bounds(struct window *w) {
    int s = shadow_of(w);
    return (struct rect){w->x - s, w->y - s, frame_w(w) + 2 * s, frame_h(w) + 2 * s};
}

static void damage_window(struct window *w) {
    struct rect b = win_bounds(w);
    damage_rect(b.x, b.y, b.w, b.h);
}

static bool visible(struct window *w) { return w && !w->dead && w->shown && !w->minimized; }

/* The screen that paces a window's frames: the one under its centre. */
static struct wscreen *pacing(struct window *w) { return screen_at(w->x + frame_w(w) / 2, w->y + frame_h(w) / 2); }

/* The area a maximised or fullscreen window of screen ws covers: a maximised one leaves what the
   shell's windows keep of its edges. */
static struct rect part_area(struct wscreen *ws, bool fullscreen) {
    if (!ws) return (struct rect){0, 0, sw, sh};
    struct rect a = {ws->x, ws->y, ws->w, ws->h};
    if (fullscreen) return a;
    a.x += ws->reserved[N_EDGE_LEFT];
    a.y += ws->reserved[N_EDGE_TOP];
    a.w -= ws->reserved[N_EDGE_LEFT] + ws->reserved[N_EDGE_RIGHT];
    a.h -= ws->reserved[N_EDGE_TOP] + ws->reserved[N_EDGE_BOTTOM];
    return a;
}

static struct rect work_area(void) { return part_area(screen_main(), false); }

static struct window *focused(void) {
    if (visible(focus_owner)) return focus_owner;
    for (size_t i = nwin; i-- > 0;)
        if (visible(zorder[i]) && !zorder[i]->no_focus) return zorder[i];
    return NULL;
}

/* Whether w is shown now: a panel hides while a fullscreen window of its screen has the focus. */
static bool drawn(struct window *w) {
    if (!visible(w)) return false;
    if (w->layer != N_LAYER_PANEL) return true;
    struct window *f = focused();
    return !f || !f->fullscreen || pacing(f) != pacing(w);
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
    for (size_t i = nwin; i-- > 0;) {
        struct window *w = zorder[i];
        if (!drawn(w)) continue;
        if (in_rect(x, y, w->x, w->y, frame_w(w), frame_h(w))) return w;
    }
    return NULL;
}

static bool in_resize_grip(struct window *w, int x, int y) {
    if (!(w->flags & WIN_RESIZABLE) || w->maximized || w->fullscreen) return false;
    int fx1 = w->x + frame_w(w), fy1 = w->y + frame_h(w);
    return x >= fx1 - 14 && y >= fy1 - 14 && x < fx1 && y < fy1;
}

/* ---- the mailbox ---- */
/* Room for need events, ev_lock held (the heap does not sleep); false at MAILBOX_MAX. */
static bool mailbox_grow(struct window *w, unsigned need) {
    if (need <= w->ev_room) return true;
    if (need > MAILBOX_MAX) return false;
    unsigned room = MAX(w->ev_room * 2, (unsigned)MAILBOX_FIRST);
    while (room < need) room *= 2;
    room = MIN(room, (unsigned)MAILBOX_MAX);
    struct gui_event *ev = kmalloc((size_t)room * sizeof *ev);
    if (!ev) return false;
    for (unsigned i = 0; i < w->ev_count; i++) ev[i] = w->ev[(w->ev_first + i) % w->ev_room];
    kfree(w->ev);
    w->ev = ev;
    w->ev_room = room;
    w->ev_first = 0;
    return true;
}

static void mailbox_put(struct window *w, const struct gui_event *e) {
    w->ev[(w->ev_first + w->ev_count) % w->ev_room] = *e;
    w->ev_count++;
}

/* An event to w's app. The frame's event and the surfaces given back keep only their last (the
   bits joined); input that finds the mailbox full is lost, and EV_INPUT_LOST comes before the
   next event that finds room (or after the last one taken). time_ns 0: the input's moment while
   input is applied, else now. Any thread, any lock but ev_lock held. */
static void push_event(struct window *w, const struct gui_event *e) {
    if (!w) return;
    struct gui_event ev = *e;
    if (!ev.time_ns) ev.time_ns = input_at ? input_at : clock_now_ns();
    uint64_t flags = spin_lock(&w->ev_lock);
    if (w->dead) goto out;
    if (ev.type == EV_FRAME) {
        w->frame_event = ev;
        w->frame_ready = true;
        goto wake;
    }
    if (ev.type == EV_SURFACE) {
        w->surfaces_back |= ev.buttons;
        goto wake;
    }
    if (ev.type == EV_MOUSE_MOVE && w->ev_count && !w->lost) {
        struct gui_event *last = &w->ev[(w->ev_first + w->ev_count - 1) % w->ev_room];
        if (last->type == EV_MOUSE_MOVE) {
            *last = ev;
            goto out;
        }
    }
    if (!mailbox_grow(w, w->ev_count + 1 + w->lost)) {
        w->lost = true;
        goto out;
    }
    if (w->lost) {
        mailbox_put(w, &(struct gui_event){.type = EV_INPUT_LOST, .time_ns = ev.time_ns});
        w->lost = false;
    }
    mailbox_put(w, &ev);
wake:
    wq_wake_all(&w->wq);
    object_notify(&w->object);
out:
    spin_unlock(&w->ev_lock, flags);
}

static void send_simple(struct window *w, int type) { push_event(w, &(struct gui_event){.type = (uint32_t)type}); }

static void send_mouse(struct window *w, int type, int buttons) {
    struct gui_event e = {.type = (uint32_t)type, .x = mx - client_x(w), .y = my - client_y(w), .buttons = (uint32_t)buttons,
                          .clicks = type == EV_MOUSE_DOWN ? press_clicks : 0};
    push_event(w, &e);
}

/* ---- focus / stacking ---- */
static size_t zindex(struct window *w) {
    for (size_t i = 0; i < nwin; i++)
        if (zorder[i] == w) return i;
    return SIZE_MAX;
}

/* Where a window stacks: the layers from the bottom, the always-on-top windows above the others. */
static int band_of(const struct window *w) {
    switch (w->layer) {
    case N_LAYER_DESKTOP: return 0;
    case N_LAYER_PANEL: return 3;
    case N_LAYER_OVERLAY: return 4;
    default: return w->flags & WIN_ALWAYS_ON_TOP ? 2 : 1;
    }
}

/* The panels show or hide as the focus moves to or from a fullscreen window. */
static void damage_panels(void) {
    for (size_t i = 0; i < nwin; i++)
        if (zorder[i]->layer == N_LAYER_PANEL && visible(zorder[i])) damage_window(zorder[i]);
}

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
    damage_panels();
    windows_changed();
}

/* To the top of its band. The list stays sorted by band. */
static void stack_top(struct window *w) {
    size_t i = zindex(w);
    if (i == SIZE_MAX) return;
    for (; i + 1 < nwin; i++) zorder[i] = zorder[i + 1];
    size_t at = nwin - 1;
    for (; at > 0 && band_of(zorder[at - 1]) > band_of(w); at--) zorder[at] = zorder[at - 1];
    zorder[at] = w;
}

static void raise_window(struct window *w) {
    if (zindex(w) == SIZE_MAX) return;
    stack_top(w);
    if (!w->no_focus) focus_owner = w;
    w->minimized = false;
    damage_window(w);
    windows_changed();
    update_focus();
}

static void lower_to_bottom(struct window *w) {
    if (w->flags & WIN_ALWAYS_ON_TOP) return;
    size_t i = zindex(w);
    if (i == SIZE_MAX) return;
    for (; i > 0 && band_of(zorder[i - 1]) >= band_of(w); i--) zorder[i] = zorder[i - 1];
    zorder[i] = w;
    if (focus_owner == w) focus_owner = NULL;
    damage_window(w);
    update_focus();
}

static void minimize_window(struct window *w) {
    w->minimized = true;
    if (focus_owner == w) focus_owner = NULL;
    damage_window(w);
    windows_changed();
    update_focus();
}

/* ---- direct scanout ----
   A display driver whose buffers have N_SCANOUT_DIRECT (direct_able) shows one app's own images
   instead of its screen's part while that app's window asked for it (win_direct), is fullscreen
   on that screen and focused, and nothing of the system is drawn over it (a window of the overlay
   layer, a resize outline) or sent to a remote viewer. It then draws the pointer
   itself. The part is still composited meanwhile, so it is current the moment the driver shows
   it again. Each screen's state is what its driver reads, with the mode the system wants it to
   show (gui/screenmode.c); its serial changes with every change of it, the pointer's moves
   included while a window is shown (a waiter reads the serial without the lock). Under wm_lock. */
static bool overlay_on(const struct wscreen *ws) {
    struct rect part = {ws->x, ws->y, ws->w, ws->h};
    for (size_t i = nwin; i-- > 0 && zorder[i]->layer == N_LAYER_OVERLAY;) {
        struct rect b = win_bounds(zorder[i]);
        if (visible(zorder[i]) && rect_overlap(&b, &part)) return true;
    }
    return false;
}

static bool direct_eligible(struct window *w, struct wscreen *ws) {
    return ws->direct_able && w && w->direct_wanted && w->fullscreen && visible(w) && pacing(w) == ws &&
           !resize_visible && !(remote_users && ws->id == 0) && !overlay_on(ws);
}

static void direct_event(struct window *w, bool shown) {
    struct gui_event e = {.type = EV_DIRECT, .key = shown};
    push_event(w, &e);
}

static void state_changed(struct wscreen *ws) {
    __atomic_store_n(&ws->state.serial, ws->state.serial + 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&ws->state_wq);
}

/* Brings each screen's state up to what it should show; the drivers and the windows concerned
   hear of a change. After input, after a window's change, and at a window's end, so direct_win
   never outlives its window. */
static void direct_update(void) {
    struct window *f = focused();
    for (size_t i = 0; i < nwscreens; i++) {
        struct wscreen *ws = wscreens[i];
        struct window *w = direct_eligible(f, ws) ? f : NULL;
        bool changed = false;
        if (w != ws->direct_win) {
            if (ws->direct_win) {
                direct_event(ws->direct_win, false);
                damage_window(ws->direct_win);
            }
            if (w) direct_event(w, true);
            ws->direct_win = w;
            ws->state.app = w ? (uint32_t)w->pid : 0;
            if (!w) ws->state.pointer_shown = 0;
            changed = true;
        }
        if (w) {
            if (ws->direct_pointer != pointer_serial) {
                memcpy(ws->state.pointer, pointer_px, sizeof ws->state.pointer);
                ws->state.hot_x = (uint32_t)pointer_hx;
                ws->state.hot_y = (uint32_t)pointer_hy;
                ws->direct_pointer = pointer_serial;
                ws->state.pointer_serial++;
                changed = true;
            }
            int px = mx - ws->x, py = my - ws->y;
            if (!ws->state.pointer_shown || ws->state.pointer_x != px || ws->state.pointer_y != py) {
                ws->state.pointer_shown = 1;
                ws->state.pointer_x = px;
                ws->state.pointer_y = py;
                changed = true;
            }
        }
        if (changed) state_changed(ws);
    }
}

/* ---- window lifetime ---- */
static void window_surfaces_drop(struct window *w) {
    for (unsigned i = 0; i < w->nsurf; i++) {
        wsurf_put(w->surf[i]);
        w->surf[i] = NULL;
    }
    wsurf_put(w->stable);
    w->stable = NULL;
    w->nsurf = 0;
    w->nqueued = 0;
    w->shown_surf = -1;
}

static void reserves_update(void);

static void win_destroy_locked(struct window *w) {
    if (w->dead) return;
    if (focus_owner == w) focus_owner = NULL;
    if (w->shown) damage_window(w);
    windows_changed(); /* it leaves the list even when the focus does not change */
    window_surfaces_drop(w);
    kfree(w->pointer_image);
    w->pointer_image = NULL;
    size_t i = zindex(w);
    if (i != SIZE_MAX) {
        for (; i + 1 < nwin; i++) zorder[i] = zorder[i + 1];
        nwin--;
    }
    /* under ev_lock: a receiver that saw it alive is in w->wq by now, for the wake below */
    uint64_t ev = spin_lock(&w->ev_lock);
    w->dead = true;
    spin_unlock(&w->ev_lock, ev);
    __atomic_add_fetch(&w->surf_seq, 1, __ATOMIC_SEQ_CST);
    if (drag_win == w) {
        drag_win = NULL;
        drag_mode = DRAG_NONE;
        if (resize_visible) damage_all();
        resize_visible = false;
    }
    if (hover_win == w) hover_win = NULL;
    if (pointer_over == w) pointer_over = NULL;
    if (last_focus == w) last_focus = NULL;
    for (size_t k = nhotkeys; k-- > 0;)
        if (hotkeys[k].w == w) hotkeys[k] = hotkeys[--nhotkeys];
    if (w->reserve_edge != N_EDGE_NONE) {
        w->reserve_edge = N_EDGE_NONE;
        reserves_update();
    }
    update_focus();
    direct_update();
    wq_wake_all(&w->wq);
    object_notify(&w->object);
    desk_wake();
}

static void win_destroy(struct window *w) {
    wm_lock();
    win_destroy_locked(w);
    wm_unlock();
}

/* Takes every queued event that fits in out[max] (the app's memory), waiting until deadline
   (CLOCK_NEVER: until one arrives). The count, 0 when the time ran out, or N_ERR_CLOSED once a
   closed window has handed out its last event. Any CPU, without wm_lock: the events leave the
   mailbox under ev_lock a batch at a time, into the kernel's memory, and are copied to out after
   it (a copy may sleep). Events taken for a page another thread took away meanwhile are lost,
   with N_ERR_BAD_ADDRESS. */
#define RECEIVE_BATCH 16
static int64_t win_receive(struct window *w, struct gui_event *out, size_t max, uint64_t deadline) {
    if (max == 0 || max > SIZE_MAX / sizeof *out) return -N_ERR_INVALID;
    if (!user_ok_w(out, MIN(max, (size_t)RECEIVE_BATCH) * sizeof *out)) return -N_ERR_BAD_ADDRESS;
    size_t got = 0;
    for (;;) {
        struct gui_event batch[RECEIVE_BATCH];
        size_t n = 0, want = MIN(max - got, (size_t)RECEIVE_BATCH);
        uint64_t fl = spin_lock(&w->ev_lock);
        for (; n < want && w->ev_count; n++) {
            batch[n] = w->ev[w->ev_first];
            w->ev_first = (w->ev_first + 1) % w->ev_room;
            w->ev_count--;
        }
        if (n < want && !w->ev_count && w->lost) {
            batch[n++] = (struct gui_event){.type = EV_INPUT_LOST, .time_ns = clock_now_ns()};
            w->lost = false;
        }
        if (n < want && w->surfaces_back) {
            batch[n++] = (struct gui_event){.type = EV_SURFACE, .buttons = w->surfaces_back, .time_ns = clock_now_ns()};
            w->surfaces_back = 0;
        }
        if (n < want && w->frame_ready) { /* after the queue */
            batch[n++] = w->frame_event;
            w->frame_ready = false;
        }
        if (n || got) {
            spin_unlock(&w->ev_lock, fl);
            if (!n) return (int64_t)got;
            int copied = copy_to_user(out + got, batch, n * sizeof *batch);
            if (copied < 0) return copied;
            got += n;
            if (n < want || got == max) return (int64_t)got;
            continue; /* a full batch: there may be more */
        }
        int64_t result = 1;
        if (w->dead) result = -N_ERR_CLOSED;
        else if (thread_current()->killed) result = -N_ERR_STOPPED;
        else if (clock_now_ns() >= deadline) result = 0;
        if (result <= 0) {
            spin_unlock(&w->ev_lock, fl);
            return result;
        }
        wq_wait_locked_until(&w->wq, &w->ev_lock, deadline);
        irq_restore(fl);
    }
}

/* Without ev_lock (an events wait asks under its queue's lock, which ev_lock comes before): every
   change notifies the object after it, and the wait asks again. */
static uint32_t win_ready(struct handle *h) {
    struct window *w = h->object->priv;
    return __atomic_load_n(&w->ev_count, __ATOMIC_RELAXED) > 0 || __atomic_load_n(&w->frame_ready, __ATOMIC_RELAXED) ||
                   __atomic_load_n(&w->surfaces_back, __ATOMIC_RELAXED) || __atomic_load_n(&w->lost, __ATOMIC_RELAXED) ||
                   __atomic_load_n(&w->dead, __ATOMIC_RELAXED)
               ? N_EVENT_RECEIVABLE
               : 0;
}

static void win_close(struct handle *h) { win_destroy(h->object->priv); }

static void win_release(struct object *o) {
    struct window *w = o->priv;
    if (w->vmm_space) vmm_space_put(w->vmm_space);
    kfree(w->ev);
    kfree(w);
}

static const struct object_ops win_ops = {
    .kind = N_KIND_WINDOW, .ready = win_ready, .close = win_close, .release = win_release};

/* What WIN_STATE answers, wm_lock held: the window's own flags, and whether it is the one
   update_focus last gave the focus to and the one shown directly. */
static int64_t win_state_of(struct window *w) {
    bool shown = !w->dead && w->shown && !w->minimized, direct = false;
    for (size_t i = 0; i < nwscreens; i++) direct |= wscreens[i]->direct_win == w;
    return (shown ? WIN_STATE_VISIBLE : 0) | (last_focus == w ? WIN_STATE_FOCUSED : 0) |
           (w->fullscreen ? WIN_STATE_FULLSCREEN : 0) | (direct ? WIN_STATE_DIRECT : 0);
}

/* A surface of a WIN_FRAME_QUEUE window may be acquired (or presented): free and read by no frame. */
static bool surface_free(struct window *w, unsigned i) {
    return w->state[i] == SURF_FREE && !w->surf[i]->readers;
}

/* Surfaces that became free and owed go back to the app, and SYS_WIN_ACQUIRE looks again.
   wm_lock held. */
static void surfaces_give_back(struct window *w) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < w->nsurf; i++)
        if (w->owed[i] && surface_free(w, i)) {
            w->owed[i] = false;
            bits |= 1u << i;
        }
    if (!bits) return;
    push_event(w, &(struct gui_event){.type = EV_SURFACE, .buttons = bits});
    __atomic_add_fetch(&w->surf_seq, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&w->wq);
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

static void text_trunc(canvas_t *c, int x, int y, const char *s, int maxw, uint32_t col) {
    if (!canvas_hits(c, x, y, maxw, gfx_font_h(FONT_SMALL))) return;
    char tmp[96];
    if (maxw <= 0) return;
    int i = 0, width = 0;
    while (s[i]) {
        uint32_t cp;
        int n = gfx_utf8_decode(s + i, &cp);
        int w = gfx_codepoint_width(cp, FONT_SMALL);
        if (i + n >= (int)sizeof tmp || width + w > maxw) break;
        i += n;
        width += w;
    }
    bool truncated = s[i] != 0;
    if (truncated) {
        int ellipsis = gfx_codepoint_width(0x2026, FONT_SMALL);
        if (ellipsis > maxw) return;
        int end = i;
        i = width = 0;
        while (i < end) {
            uint32_t cp;
            int n = gfx_utf8_decode(s + i, &cp);
            int w = gfx_codepoint_width(cp, FONT_SMALL);
            if (width + w + ellipsis > maxw || i + n + 3 >= (int)sizeof tmp) break;
            i += n;
            width += w;
        }
    }
    memcpy(tmp, s, i);
    if (truncated) {
        memcpy(tmp + i, "\xE2\x80\xA6", 3);
        i += 3;
    }
    tmp[i] = 0;
    gfx_text(c, x, y, tmp, col, TRANSPARENT, FONT_SMALL);
}

/* 8-bit art drawn over a possibly deep picture: remember the pixels of its box (inside the canvas
   clip) before drawing, then mark only the pixels it changed as 8-bit content, so the picture
   around and beneath it keeps its low bits. The box must fit the caller's buffer of stride pixels
   per row. */
struct deep_mark {
    int x, y, w, h, stride;
    uint32_t *saved;
};
static bool deep_mark_begin(struct deep_mark *m, const canvas_t *c, int x, int y, int w, int h, uint32_t *saved,
                            int stride) {
    int x0 = MAX(x, MAX(c->cx0, 0)), y0 = MAX(y, MAX(c->cy0, 0));
    int x1 = MIN(x + w, MIN(c->cx1, c->w)), y1 = MIN(y + h, MIN(c->cy1, c->h));
    *m = (struct deep_mark){x0, y0, x1 - x0, y1 - y0, stride, saved};
    if (m->w <= 0 || m->h <= 0) return false;
    for (int row = 0; row < m->h; row++)
        memcpy(saved + row * stride, c->px + (size_t)(y0 + row) * c->pitch + x0, (size_t)m->w * 4);
    return true;
}
static void deep_mark_end(const struct deep_mark *m, const canvas_t *c, uint8_t *deep) {
    for (int row = 0; row < m->h; row++) {
        const uint32_t *now = c->px + (size_t)(m->y + row) * c->pitch + m->x, *was = m->saved + row * m->stride;
        uint8_t *d = deep + (size_t)(m->y + row) * c->pitch + m->x;
        for (int col = 0; col < m->w; col++)
            if (now[col] != was[col]) d[col] = 0;
    }
}

void wm_screen_info_changed(void) {
    if (screen_watchable) watched_changed(&screen_watched);
}

/* ---- the desktop's settings (N_DOMAIN_WINDOW) ---- */


int smooth_gradients_setting_put(uint32_t key, const void *value) {
    int32_t v;
    memcpy(&v, value, sizeof v);
    bool smooth = v != 0;
    wm_lock();
    if (smooth != deep_wanted) {
        settings_changed(N_DOMAIN_WINDOW);
        deep_wanted = smooth;
        desk_wake();
        wm_screen_info_changed();
    }
    wm_unlock();
    return 0;
}

int64_t smooth_gradients_setting_get(uint32_t key, void *out, size_t cap) {
    wm_lock();
    bool v = deep_wanted;
    wm_unlock();
    return setting_int(out, cap, v);
}

/* ---- window drawing ---- */
/* The shadow, without the pixels the client area replaces after it. Rounded title corners remain
   translucent and keep every pass. */
static void draw_layer_shadow(canvas_t *c, const struct layer *w) {
    int64_t hx0 = w->x, hy0 = w->y;
    int64_t hx1 = hx0 + w->fw, hy1 = hy0 + w->fh;
    for (int i = SHADOW; i > 0; i -= 2) {
        int64_t x0 = MAX((int64_t)w->x - i + 2, (int64_t)c->cx0);
        int64_t y0 = MAX((int64_t)w->y - i + 4, (int64_t)c->cy0);
        int64_t x1 = MIN((int64_t)w->x + w->fw + i - 2, (int64_t)c->cx1);
        int64_t y1 = MIN((int64_t)w->y + w->fh + i, (int64_t)c->cy1);
        if (x1 <= x0 || y1 <= y0) continue;
        int64_t ix0 = MAX(x0, hx0), iy0 = MAX(y0, hy0);
        int64_t ix1 = MIN(x1, hx1), iy1 = MIN(y1, hy1);
        uint32_t col = ARGB(6 + (SHADOW - i) * 3, 0, 0, 10);
        if (ix1 <= ix0 || iy1 <= iy0) {
            gfx_fill_blend(c, (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0), col);
            continue;
        }
        if (iy0 > y0) gfx_fill_blend(c, (int)x0, (int)y0, (int)(x1 - x0), (int)(iy0 - y0), col);
        if (y1 > iy1) gfx_fill_blend(c, (int)x0, (int)iy1, (int)(x1 - x0), (int)(y1 - iy1), col);
        if (ix0 > x0) gfx_fill_blend(c, (int)x0, (int)iy0, (int)(ix0 - x0), (int)(iy1 - iy0), col);
        if (x1 > ix1) gfx_fill_blend(c, (int)ix1, (int)iy0, (int)(x1 - ix1), (int)(iy1 - iy0), col);
        if (!(w->flags & WIN_NO_DECOR)) {
            for (int j = 0; j < TITLE_RADIUS; j++) {
                int64_t y = hy0 + j;
                if (y < iy0 || y >= iy1) continue;
                int inset = corner_inset(j, TITLE_RADIUS);
                int64_t left = MIN(ix1, hx0 + inset), right = MAX(ix0, hx1 - inset);
                if (left > ix0) gfx_fill_blend(c, (int)ix0, (int)y, (int)(left - ix0), 1, col);
                if (right < ix1) gfx_fill_blend(c, (int)right, (int)y, (int)(ix1 - right), 1, col);
            }
        }
    }
}

/* Draws only into c within its clip; the layer and its pixels are the frame's (capture). */
static void draw_layer(canvas_t *c, const struct layer *w) {
    int fx = w->x, fy = w->y, fw = w->fw, fh = w->fh;
    bool focus = w->focus;
    if (w->shadow) draw_layer_shadow(c, w);
    if (!(w->flags & WIN_NO_DECOR)) {
        uint32_t edge = focus ? RGB(110, 96, 200) : RGB(60, 60, 84);
        uint32_t top = focus ? RGB(66, 52, 140) : RGB(44, 44, 62);
        uint32_t bot = focus ? RGB(38, 32, 88) : RGB(34, 34, 48);
        const int R = TITLE_RADIUS;
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
        int nb = (w->flags & WIN_RESIZABLE) ? 3 : 2;
        text_trunc(c, fx + 12, fy + 6, w->title, fw - 24 - nb * 24, focus ? RGB(250, 250, 255) : RGB(160, 160, 180));
        for (int b = 1; b <= 3; b++) {
            if (b == 2 && !(w->flags & WIN_RESIZABLE)) continue;
            int bx = fx + fw - (b == 1 ? 18 : b == 2 ? 42 : (w->flags & WIN_RESIZABLE) ? 66 : 42);
            int by = fy + TITLE_H / 2;
            uint32_t col = !focus ? RGB(84, 84, 108) : b == 1 ? RGB(236, 92, 108) : b == 2 ? RGB(96, 200, 130) : RGB(240, 190, 80);
            gfx_fill_circle(c, bx, by, 7, col);
            if (w->hover == b) {
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
    gfx_init(&wc, w->src->px, w->src->w, w->src->h, w->src->w);
    if (w->blend) gfx_blit_alpha(c, w->cx, w->cy, &wc, 0, 0, MIN(w->cw, w->src->w), MIN(w->ch, w->src->h));
    else gfx_blit(c, w->cx, w->cy, &wc, 0, 0, MIN(w->cw, w->src->w), MIN(w->ch, w->src->h));
    if (w->flags & WIN_RESIZABLE && !w->maximized) {
        int gx = w->x + fw - 4, gy = w->y + fh - 4;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j <= i; j++) gfx_pixel(c, gx - (i - j) * 3, gy - j * 3, RGB(150, 140, 200));
    }
}

/* Deep-plane bookkeeping inside the canvas clip. 8-bit drawing marks its area as 8-bit content; a
   deep source copies its low bits along with its pixels. Shadows and other blends over deep
   pixels are 8-bit results. */
static void deep_clear(uint8_t *deep, const canvas_t *c, int64_t x, int64_t y, int64_t w, int64_t h) {
    int64_t x0 = MAX(x, (int64_t)c->cx0), y0 = MAX(y, (int64_t)c->cy0);
    int64_t x1 = MIN(x + w, (int64_t)c->cx1), y1 = MIN(y + h, (int64_t)c->cy1);
    if (x1 <= x0) return;
    for (int64_t row = y0; row < y1; row++) memset(deep + row * c->pitch + x0, 0, (size_t)(x1 - x0));
}
static void deep_copy(uint8_t *deep, const canvas_t *c, int64_t x, int64_t y, const uint8_t *src, int src_pitch, int w,
                      int h) {
    int64_t x0 = MAX(x, (int64_t)c->cx0), y0 = MAX(y, (int64_t)c->cy0);
    int64_t x1 = MIN(x + w, (int64_t)c->cx1), y1 = MIN(y + h, (int64_t)c->cy1);
    if (x1 <= x0) return;
    for (int64_t row = y0; row < y1; row++)
        memcpy(deep + row * c->pitch + x0, src + (row - y) * src_pitch + (x0 - x), (size_t)(x1 - x0));
}

/* ---- compositing (a screen's compositor thread, back_lock held) ---- */
/* The topmost layer whose client area covers all of r: nothing under it shows there. */
static size_t first_layer(const struct wscreen *ws, struct rect r, bool *covered) {
    *covered = false;
    int64_t right = (int64_t)r.x + r.w, bottom = (int64_t)r.y + r.h;
    for (size_t at = ws->nlayers; at; at--) {
        const struct layer *l = &ws->layers[at - 1];
        if (!l->blend && r.x >= l->cx && r.y >= l->cy && right <= (int64_t)l->cx + MIN(l->cw, l->src->w) &&
            bottom <= (int64_t)l->cy + MIN(l->ch, l->src->h)) {
            *covered = true;
            return at - 1;
        }
    }
    return 0;
}

static void compose_rect(struct wscreen *ws, struct rect r) {
    canvas_t c = ws->back;
    gfx_noclip(&c);
    gfx_clip(&c, r.x, r.y, r.w, r.h);
    uint8_t *deep = ws->back_deep;
    bool covered;
    size_t first = first_layer(ws, r, &covered);
    if (!covered) {
        gfx_fill(&c, r.x, r.y, r.w, r.h, BACKGROUND);
        if (deep) deep_clear(deep, &c, r.x, r.y, r.w, r.h);
    }
    for (size_t i = first; i < ws->nlayers; i++) {
        const struct layer *l = &ws->layers[i];
        if (!rect_overlap(&l->bounds, &r)) continue;
        if (l->copy) mutex_lock(&l->src->lock);
        draw_layer(&c, l);
        if (deep) {
            deep_clear(deep, &c, l->bounds.x, l->bounds.y, l->bounds.w, l->bounds.h);
            if (l->src->deep && !l->blend)
                deep_copy(deep, &c, l->cx, l->cy, (const uint8_t *)(l->src->px + (size_t)l->src->w * l->src->h), l->src->w,
                          MIN(l->cw, l->src->w), MIN(l->ch, l->src->h));
        }
        if (l->copy) mutex_unlock(&l->src->lock);
    }
    if (ws->resize_on) {
        struct rect q = ws->resize;
        gfx_rect(&c, q.x, q.y, q.w, q.h, RGB(180, 160, 255));
        gfx_rect(&c, q.x + 1, q.y + 1, q.w - 2, q.h - 2, RGB(90, 70, 200));
        if (deep) deep_clear(deep, &c, q.x, q.y, q.w, q.h);
    }
    if (ws->pointer_on) {
        struct deep_mark mark;
        bool track = deep && deep_mark_begin(&mark, &c, ws->pointer_x, ws->pointer_y, POINTER_SIZE, POINTER_SIZE,
                                             ws->under, POINTER_SIZE);
        canvas_t image;
        gfx_init(&image, ws->pointer_px, POINTER_SIZE, POINTER_SIZE, POINTER_SIZE);
        gfx_blit_alpha(&c, ws->pointer_x, ws->pointer_y, &image, 0, 0, POINTER_SIZE, POINTER_SIZE);
        if (track) deep_mark_end(&mark, &c, deep);
    }
}

/* A WIN_FRAME_QUEUE window's next present is shown from this frame on; the one shown before goes
   back once no frame reads it. wm_lock held. */
static void queue_advance(struct window *w) {
    int next = w->queued[0];
    uint64_t serial = w->queued_serial[0];
    w->nqueued--;
    memmove(w->queued, w->queued + 1, w->nqueued * sizeof *w->queued);
    memmove(w->queued_serial, w->queued_serial + 1, w->nqueued * sizeof *w->queued_serial);
    if (w->shown_surf >= 0) {
        w->state[w->shown_surf] = SURF_FREE;
        w->owed[w->shown_surf] = true;
    }
    w->state[next] = SURF_SHOWN;
    w->shown_surf = next;
    w->shown_serial = serial;
    if (visible(w)) damage_rect(client_x(w), client_y(w), w->cw, w->ch);
    surfaces_give_back(w);
}

/* What screen ws's frame draws, taken under wm_lock (and its back_lock): the windows paced by it
   whose next present is due, its changed tiles, the layers (with references to their pixels), the
   pointer. */
static void capture(struct wscreen *ws) {
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (w->queue && w->nqueued && !w->dead && pacing(w) == ws) queue_advance(w);
    }
    struct rect part = {ws->x, ws->y, ws->w, ws->h};
    struct window *f = focused();
    ws->nlayers = 0;
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (!drawn(w)) continue;
        struct wsurf *src = w->queue ? (w->shown_surf >= 0 ? w->surf[w->shown_surf] : NULL) : w->stable;
        struct rect b = win_bounds(w);
        if (!src || !rect_overlap(&b, &part)) continue;
        struct layer *l = kappend(&ws->layers, &ws->nlayers, &ws->layers_room, sizeof *l);
        if (!l) break;
        int fw = frame_w(w), fh = frame_h(w);
        struct rect bounds = {b.x - ws->x, b.y - ws->y, b.w, b.h};
        rect_clip_to(&bounds, ws->w, ws->h);
        *l = (struct layer){.x = w->x - ws->x, .y = w->y - ws->y, .cw = w->cw, .ch = w->ch, .fw = fw, .fh = fh,
                            .cx = client_x(w) - ws->x, .cy = client_y(w) - ws->y,
                            .flags = w->flags | (w->fullscreen ? WIN_NO_DECOR : 0),
                            .hover = hover_win == w ? hover_btn : 0, .focus = w == f, .maximized = w->maximized,
                            .shadow = shadow_of(w) != 0, .blend = w->blend, .src = wsurf_get(src),
                            .copy = !w->queue, .bounds = bounds};
        if (w->queue) src->readers++;
        memcpy(l->title, w->title, sizeof l->title);
        l->title[sizeof l->title - 1] = 0;
    }
    /* the EV_FRAME owed to the windows this screen paces, once its flip shows */
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (!w->frame_wanted || !visible(w) || pacing(w) != ws) continue;
        w->frame_wanted = false;
        w->note = true;
        w->note_screen = ws->id;
        w->note_flip = 0;
        w->note_present = w->queue ? w->shown_serial : w->presents;
        w->note_surface = w->queue ? w->shown_surf : 0;
    }
    ws->resize_on = resize_visible;
    ws->resize = (struct rect){resize_rect.x - ws->x, resize_rect.y - ws->y, resize_rect.w, resize_rect.h};
    ws->pointer_on = !(remote_users && ws->id == 0);
    ws->pointer_x = mx - pointer_hx - ws->x;
    ws->pointer_y = my - pointer_hy - ws->y;
    if (ws->pointer_serial != pointer_serial) {
        memcpy(ws->pointer_px, pointer_px, sizeof ws->pointer_px);
        ws->pointer_serial = pointer_serial;
    }
    memcpy(ws->frame.bits, ws->dirty.bits, tiles_words(&ws->dirty) * sizeof *ws->dirty.bits);
    tiles_clear(&ws->dirty);
    ws->dirty_any = false;
}

/* The EV_FRAMEs whose flip ws now shows (all of them when flip is 0: the frame reached no
   buffer). wm_lock held. */
static void notes_send(struct wscreen *ws, uint64_t shown, uint64_t shown_ns, bool all) {
    int32_t period = (int32_t)MIN(ws->period_ns, (uint64_t)INT32_MAX);
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (!w->note || w->note_screen != ws->id || !w->note_flip || (!all && w->note_flip > shown)) continue;
        w->note = false;
        push_event(w, &(struct gui_event){.type = EV_FRAME, .key = (uint32_t)MAX(w->note_surface, 0), .y = period,
                                          .time_ns = shown_ns ? shown_ns : clock_now_ns(), .serial = w->note_present});
    }
}

/* After a frame of ws (flip: its serial, 0 when it reached no buffer): the layers' readers end, the
   surfaces no longer shown go back, and the notes of this frame wait for its flip. wm_lock held. */
static void finish(struct wscreen *ws, uint64_t flip) {
    for (size_t i = 0; i < ws->nlayers; i++)
        if (!ws->layers[i].copy) ws->layers[i].src->readers--;
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (w->queue) surfaces_give_back(w);
        if (w->note && w->note_screen == ws->id && !w->note_flip) w->note_flip = flip ? flip : UINT64_MAX;
    }
    if (!flip) notes_send(ws, 0, 0, true);
}

/* Whether a window this screen paces waits for a frame without damage (SYS_WIN_FRAME, a queued
   present). wm_lock held. */
static bool windows_wait(struct wscreen *ws) {
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (((visible(w) && w->frame_wanted) || (w->queue && w->nqueued)) && pacing(w) == ws) return true;
    }
    return false;
}

/* A frame of ws: capture, draw its tiles into the picture, write the picture's tiles each buffer
   lacks into the one the screen does not show, ask for the flip. Returns false when no buffer took
   it (the screen off, or held: what it lacks waits in stale). */
static bool frame(struct wscreen *ws) {
    uint64_t start = clock_now_ns();
    mutex_lock(&ws->back_lock);
    wm_lock();
    capture(ws);
    wm_unlock();
    bool ok = tiles_rects(&ws->frame, ws->w, ws->h, &ws->rects, &ws->nrects, &ws->rects_room);
    if (!ok) { /* no memory for the list: all of it, as one */
        tiles_fill(&ws->frame);
        ws->nrects = 0;
        struct rect *r = kappend(&ws->rects, &ws->nrects, &ws->rects_room, sizeof *r);
        if (r) *r = (struct rect){0, 0, ws->w, ws->h};
        else if (ws->rects_room) {
            ws->rects[0] = (struct rect){0, 0, ws->w, ws->h};
            ws->nrects = 1;
        }
    }
    for (size_t i = 0; i < ws->nrects; i++) compose_rect(ws, ws->rects[i]);
    if (ws->id == 0 && __atomic_load_n(&remote_users, __ATOMIC_RELAXED)) tiles_or(&ws->remote, &ws->frame);
    uint64_t drawn = clock_now_ns();
    uint64_t generation, flip = 0;
    int buf = screen_frame_begin(ws->s, clock_after(NS_PER_S), &generation);
    if (buf >= 0) {
        if (generation != ws->generation) {
            ws->generation = generation;
            for (unsigned b = 0; b < SCREEN_BUFFERS_MAX; b++) tiles_fill(&ws->stale[b]);
        }
        memcpy(ws->both.bits, ws->stale[buf].bits, tiles_words(&ws->both) * sizeof *ws->both.bits);
        tiles_or(&ws->both, &ws->frame);
        tiles_rects(&ws->both, ws->w, ws->h, &ws->rects, &ws->nrects, &ws->rects_room);
        for (size_t i = 0; i < ws->nrects; i++) {
            struct rect r = ws->rects[i];
            screen_frame_write(ws->s, buf, ws->back.px, ws->back.pitch, ws->back_deep, ws->back.pitch, r.x, r.y, r.w, r.h);
        }
        for (unsigned b = 0; b < SCREEN_BUFFERS_MAX; b++)
            if (b != (unsigned)buf) tiles_or(&ws->stale[b], &ws->frame);
        tiles_clear(&ws->stale[buf]);
        flip = screen_frame_end(ws->s, buf);
    } else {
        for (unsigned b = 0; b < SCREEN_BUFFERS_MAX; b++) tiles_or(&ws->stale[b], &ws->frame);
    }
    uint64_t end = clock_now_ns();
    ws->frames++;
    ws->render_ns += drawn - start;
    ws->render_max = MAX(ws->render_max, drawn - start);
    ws->write_ns += end - drawn;
    ws->write_max = MAX(ws->write_max, end - drawn);
    if (ws->period_ns && end - start > ws->period_ns) ws->late++;
    size_t nlayers = ws->nlayers;
    mutex_unlock(&ws->back_lock);
    struct screen_times t;
    screen_times(ws->s, &t);
    ws->immediate = flip && t.shown_serial >= flip;
    wm_lock();
    finish(ws, flip);
    if (flip && ws->immediate) notes_send(ws, t.shown_serial, t.shown_ns, false);
    wm_unlock();
    for (size_t i = 0; i < nlayers; i++) wsurf_put(ws->layers[i].src);
    return buf >= 0;
}

/* ---- a screen's picture ---- */
static void layout(void);
static void windows_keep_in(void);

/* The maximised and fullscreen windows ws paces take their area again: it moved or changed size.
   wm_lock held. */
static void windows_refit(struct wscreen *ws) {
    for (size_t i = 0; i < nwin; i++) {
        struct window *win = zorder[i];
        if (pacing(win) != ws || (!win->fullscreen && !win->maximized)) continue;
        struct rect a = part_area(ws, win->fullscreen);
        damage_window(win);
        win->x = a.x;
        win->y = a.y;
        damage_window(win);
        struct gui_event e = {.type = EV_RESIZE, .x = win->fullscreen ? a.w : a.w - 2 * BORDER,
                              .y = win->fullscreen ? a.h : a.h - TITLE_H - BORDER};
        if (e.x != win->cw || e.y != win->ch) push_event(win, &e);
    }
}

/* What the shell's windows keep of each screen's edges, again. wm_lock held. */
static void reserves_update(void) {
    for (size_t k = 0; k < nwscreens; k++) {
        struct wscreen *ws = wscreens[k];
        int edges[N_EDGE_RIGHT + 1] = {0};
        for (size_t i = 0; i < nwin; i++) {
            struct window *w = zorder[i];
            if (w->reserve_edge != N_EDGE_NONE && w->reserve_screen == ws->id)
                edges[w->reserve_edge] = MAX(edges[w->reserve_edge], w->reserve);
        }
        if (!memcmp(edges, ws->reserved, sizeof edges)) continue;
        memcpy(ws->reserved, edges, sizeof edges);
        windows_refit(ws);
        wm_screen_info_changed();
    }
}

/* Screen ws's part becomes w x h: a new picture, its tiles, the windows that fill it told. Any
   thread, no lock held. False without memory (it keeps what it had). */
static bool wscreen_resize(struct wscreen *ws, int w, int h) {
    size_t pages = ALIGN_UP((uint64_t)w * h * 4, PAGE_SIZE) / PAGE_SIZE;
    size_t deep_pages = ALIGN_UP((uint64_t)w * h, PAGE_SIZE) / PAGE_SIZE;
    bool deep = fb.deep;
    uint32_t *bb = vmalloc(pages);
    uint8_t *bd = deep ? vmalloc(deep_pages) : NULL;
    struct tiles dirty = {0}, frame = {0}, remote = {0}, both = {0}, stale[SCREEN_BUFFERS_MAX] = {0};
    bool ok = bb && (!deep || bd) && tiles_new(&dirty, w, h) && tiles_new(&frame, w, h) &&
              tiles_new(&remote, w, h) && tiles_new(&both, w, h);
    for (unsigned b = 0; ok && b < SCREEN_BUFFERS_MAX; b++) ok = tiles_new(&stale[b], w, h);
    if (ok) {
        canvas_t back;
        gfx_init(&back, bb, w, h, w);
        gfx_fill(&back, 0, 0, w, h, BACKGROUND);
        if (bd) memset(bd, 0, (size_t)w * h);
    }
    if (!ok) {
        if (bb) vfree(bb, pages);
        if (bd) vfree(bd, deep_pages);
        kfree(dirty.bits);
        kfree(frame.bits);
        kfree(remote.bits);
        kfree(both.bits);
        for (unsigned b = 0; b < SCREEN_BUFFERS_MAX; b++) kfree(stale[b].bits);
        return false;
    }
    mutex_lock(&ws->back_lock);
    canvas_t old_back = ws->back;
    uint8_t *old_bd = ws->back_deep;
    struct tiles old_frame = ws->frame, old_remote = ws->remote, old_both = ws->both, old_stale[SCREEN_BUFFERS_MAX];
    memcpy(old_stale, ws->stale, sizeof old_stale);
    gfx_init(&ws->back, bb, w, h, w);
    ws->back_deep = bd;
    ws->frame = frame;
    ws->remote = remote;
    ws->both = both;
    memcpy(ws->stale, stale, sizeof stale);
    for (unsigned b = 0; b < SCREEN_BUFFERS_MAX; b++) tiles_fill(&ws->stale[b]);
    tiles_fill(&ws->remote);
    wm_lock();
    struct tiles old_dirty = ws->dirty;
    ws->dirty = dirty;
    ws->w = w;
    ws->h = h;
    ws->on = true;
    layout();
    windows_keep_in();
    mx = MAX(0, MIN(mx, desk_w - 1));
    my = MAX(0, MIN(my, desk_h - 1));
    windows_refit(ws);
    damage_all();
    direct_update();
    wm_unlock();
    mutex_unlock(&ws->back_lock);
    kprintf("wm: screen %u: the desktop's part is %dx%d\n", ws->id, w, h);
    size_t old_pages = ALIGN_UP((uint64_t)old_back.w * old_back.h * 4, PAGE_SIZE) / PAGE_SIZE;
    size_t old_deep_pages = ALIGN_UP((uint64_t)old_back.w * old_back.h, PAGE_SIZE) / PAGE_SIZE;
    if (old_back.px) vfree(old_back.px, old_pages);
    if (old_bd) vfree(old_bd, old_deep_pages);
    kfree(old_dirty.bits);
    kfree(old_frame.bits);
    kfree(old_remote.bits);
    kfree(old_both.bits);
    for (unsigned b = 0; b < SCREEN_BUFFERS_MAX; b++) kfree(old_stale[b].bits);
    wm_screen_info_changed();
    return true;
}

/* The parts the display service placed where it says; the others in the screens' order on their
   right, at the top. wm_lock held. */
static void layout(void) {
    int x = 0, h = 0;
    for (size_t i = 0; i < nwscreens; i++) {
        struct wscreen *ws = wscreens[i];
        if (!ws->placed || (i && !part_on(ws))) continue;
        ws->x = ws->place_x;
        ws->y = ws->place_y;
        x = MAX(x, ws->x + ws->w);
        h = MAX(h, ws->y + ws->h);
    }
    for (size_t i = 0; i < nwscreens; i++) {
        struct wscreen *ws = wscreens[i];
        if (ws->placed || (i && !part_on(ws))) continue;
        ws->x = x;
        ws->y = 0;
        x += ws->w;
        h = MAX(h, ws->h);
    }
    desk_w = x;
    desk_h = h;
    struct wscreen *main = screen_main();
    if (main && main->w) {
        sw = main->w;
        sh = main->h;
    }
}

/* A window whose centre left every part comes back onto the main screen's. wm_lock held. */
static void windows_keep_in(void) {
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        int cx = w->x + frame_w(w) / 2, cy = w->y + frame_h(w) / 2;
        bool in = false;
        for (size_t j = 0; j < nwscreens && !in; j++)
            in = part_on(wscreens[j]) && in_rect(cx, cy, wscreens[j]->x, wscreens[j]->y, wscreens[j]->w, wscreens[j]->h);
        if (in) continue;
        struct rect a = work_area();
        if (w->x + frame_w(w) > a.x + a.w) w->x = MAX(a.x, a.x + a.w - frame_w(w));
        if (w->y + frame_h(w) > a.y + a.h) w->y = MAX(a.y, a.y + a.h - frame_h(w));
        w->x = MAX(w->x, a.x);
        w->y = MAX(w->y, a.y);
    }
}

/* What the screen's driver says of it: the size its part takes (unless remote viewers have screen
   0 at theirs), whether it is on, and its refresh, which sets the compositor's deadline. The
   compositor thread of ws. */
static void follow(struct wscreen *ws) {
    struct screen_mode_desc d;
    bool on = screen_mode(ws->s, &d);
    int tw = on ? (int)d.width : 0, th = on ? (int)d.height : 0;
    wm_lock();
    if (ws->id == 0 && remote_users) {
        tw = ws->remote_w;
        th = ws->remote_h;
        on = true;
    }
    bool size = on && (tw != ws->w || th != ws->h);
    uint64_t period = on && d.refresh_mhz ? NS_PER_S * 1000 / d.refresh_mhz : 0;
    bool vrr = on && d.vrr_min_mhz;
    bool timing = period != ws->period_ns || vrr != ws->vrr || !ws->policy_set;
    ws->period_ns = period;
    ws->vrr = vrr;
    ws->refresh_mhz = on ? d.refresh_mhz : 0;
    ws->vrr_min_mhz = vrr ? d.vrr_min_mhz : 0;
    bool onoff = !size && on != ws->on;
    if (onoff) {
        ws->on = on;
        layout();
        windows_keep_in();
        damage_all();
        direct_update();
    }
    wm_unlock();
    if (size && !wscreen_resize(ws, tw, th))
        kprintf("wm: screen %u: no memory for a %dx%d picture; it keeps %dx%d\n", ws->id, tw, th, ws->w, ws->h);
    if (onoff) {
        kprintf("wm: screen %u is %s\n", ws->id, on ? "on" : "off");
        wm_screen_info_changed();
    }
    if (!timing) return;
    ws->policy_set = true;
    if (!period) {
        kprintf("wm: screen %u: its refresh is not known; frames come as the picture changes\n", ws->id);
        sched_set_policy(thread_current(), sched_fair(SCHED_WEIGHT_DEFAULT, SCHED_REQUEST_DEFAULT_NS));
        ws->policy_ns = 0;
        return;
    }
    if (vrr)
        kprintf("wm: screen %u: variable refresh %u.%03u to %u.%03u Hz; a frame as soon as it is drawn\n", ws->id,
                ws->vrr_min_mhz / 1000, ws->vrr_min_mhz % 1000, ws->refresh_mhz / 1000, ws->refresh_mhz % 1000);
    else
        kprintf("wm: screen %u: frames at its %u.%03u Hz\n", ws->id, ws->refresh_mhz / 1000, ws->refresh_mhz % 1000);
    ws->policy_ns = MIN(MAX(period, SCHED_PERIOD_MIN_NS), SCHED_PERIOD_MAX_NS);
    sched_set_policy(thread_current(), sched_deadline(ws->policy_ns));
}

/* The frame counts of the profiler's window (sys/prof.h), printed with its samples. */
static void stats_print(struct wscreen *ws, uint64_t now) {
    uint64_t window = prof_window_ns();
    if (!window) return;
    if (!ws->stats_since) ws->stats_since = now;
    if (now - ws->stats_since < window) return;
    if (ws->frames)
        kprintf("wm: screen %u: %u frames in %lu ms, drawing avg %lu max %lu us, writing avg %lu max %lu us, %u longer than a refresh\n",
                ws->id, ws->frames, (unsigned long)((now - ws->stats_since) / NS_PER_MS),
                (unsigned long)(ws->render_ns / ws->frames / NS_PER_US), (unsigned long)(ws->render_max / NS_PER_US),
                (unsigned long)(ws->write_ns / ws->frames / NS_PER_US), (unsigned long)(ws->write_max / NS_PER_US),
                ws->late);
    ws->stats_since = now;
    ws->frames = ws->late = 0;
    ws->render_ns = ws->render_max = ws->write_ns = ws->write_max = 0;
}

/* ---- a screen's compositor thread ----
   Each pass reads the screen's word (screen_events: a flip shown, a vertical blank, a change of
   buffers, damage poked by the window manager) before it looks, under wm_lock, at the work left;
   it sleeps only while that word still reads the same. A frame starts when there is work and no
   flip is in flight (one frame for each refresh the display shows), and where flips show at once
   (no vertical blank to wait for) not before the refresh's next due time; a variable refresh
   takes a frame as soon as it is drawn, at most at its fastest rate; an unknown refresh takes
   frames as the picture changes. */
static void compositor(void *arg) {
    struct wscreen *ws = arg;
    for (;;) {
        uint64_t seen = screen_events(ws->s);
        follow(ws);
        struct screen_times t;
        screen_times(ws->s, &t);
        wm_lock();
        notes_send(ws, t.shown_serial, t.shown_ns, false);
        bool work = part_on(ws) && ws->back.px && (ws->dirty_any || windows_wait(ws));
        wm_unlock();
        uint64_t now = clock_now_ns(), until = CLOCK_NEVER;
        bool due = work && t.asked_serial == t.shown_serial;
        if (due && ws->period_ns && (ws->immediate || ws->vrr) && now < ws->frame_next) {
            due = false;
            until = ws->frame_next;
        }
        if (!due) {
            screen_wait(ws->s, seen, until);
            continue;
        }
        if (ws->vrr) ws->frame_next = now + ws->period_ns;
        else if (ws->period_ns && now >= ws->frame_next)
            ws->frame_next += ((now - ws->frame_next) / ws->period_ns + 1) * ws->period_ns;
        if (!frame(ws)) screen_wait(ws->s, seen, CLOCK_NEVER); /* off or held: the screen's change wakes it */
        stats_print(ws, clock_now_ns());
    }
}

static void wscreen_start(struct wscreen *ws) {
    char name[32];
    ksnprintf(name, sizeof name, "compositor %u", ws->id);
    ws->thread = kthread_create(name, compositor, ws);
    if (!ws->thread) kprintf("wm: screen %u: no compositor thread\n", ws->id);
}

/* Screen id's part, made when make (NULL: none, or no memory). wm_lock held. */
static struct wscreen *wscreen_get(unsigned id, bool make) {
    while (make && nwscreens <= id) {
        struct screen *s = screen_get((unsigned)nwscreens);
        struct wscreen *ws = s ? kzalloc(sizeof *ws) : NULL;
        struct wscreen **at = ws ? kappend(&wscreens, &nwscreens, &wscreens_room, sizeof *at) : NULL;
        if (!at) {
            kfree(ws);
            return NULL;
        }
        ws->id = (unsigned)(nwscreens - 1);
        ws->s = s;
        ws->scale_percent = 100;
        ws->state.serial = 1;
        *at = ws;
        if (running) wscreen_start(ws);
    }
    return id < nwscreens ? wscreens[id] : NULL;
}

/* dev/fb.c: a screen came, changed its buffers or mode, or went off. */
static void screen_changed(struct screen *s) {
    wm_lock();
    struct wscreen *ws = wscreen_get(screen_id(s), true);
    wm_unlock();
    if (ws) screen_poke(s);
}

/* Smooth gradients (the 10 bpc simulation) on or off: the user switched them, or screen 0 took or
   stopped taking them. Every picture is made again at the new depth. The desktop thread, no lock
   held. */
static void set_deep_simulation(bool on) {
    if (!fb_set_simulated(on)) return; /* screen 0 changed meanwhile: the next pass looks again */
    wm_lock();
    size_t n = nwscreens;
    wm_unlock();
    for (size_t i = 0; i < n; i++) {
        wm_lock();
        struct wscreen *ws = wscreens[i];
        int w = ws->w, h = ws->h;
        wm_unlock();
        if (w && h && !wscreen_resize(ws, w, h)) {
            kprintf("wm: no memory for deep colour, using 8 bpc\n");
            fb_set_simulated(false);
            wm_lock();
            deep_wanted = false;
            wm_unlock();
        }
    }
    kprintf("wm: %u bpc desktop%s\n", fb.bpc, fb.simulated ? ", dithered to the 8 bpc display" : "");
    wm_screen_info_changed();
}

/* ---- input (the desktop thread) ----
   The input ring: the input drivers' sinks (key_sink, mouse_sink: any CPU and context) put events
   in at in_head, the desktop thread takes them at in_tail, both under in_lock. A full ring loses
   nothing that matters for what is held: a pointer move joins the last one, and a key's release
   is kept as a bit (released) and comes in its place before that key's next event, or after the
   ring's events. */
#define INQ 1024
struct in_ev {
    bool is_key;
    struct key_event k;
    struct mouse_event m;
};
static struct spinlock in_lock;
static struct in_ev inq[INQ];
static unsigned in_head, in_tail;
static uint64_t released[65536 / 64];
static bool any_released;
static uint32_t released_layout;
static uint64_t in_lost;

static bool key_released(uint16_t key) { return released[key / 64] >> (key % 64) & 1; }

/* in_lock held, room for one. */
static void ring_put(const struct in_ev *e) {
    inq[in_head] = *e;
    in_head = (in_head + 1) % INQ;
}
static unsigned ring_room(void) { return (in_tail + INQ - in_head - 1) % INQ; }

static void input_put(const struct in_ev *e) {
    uint64_t f = spin_lock(&in_lock);
    if (e->is_key && key_released(e->k.usage) && ring_room() >= 2) {
        struct key_event up = e->k;
        up.pressed = false;
        ring_put(&(struct in_ev){.is_key = true, .k = up});
        released[e->k.usage / 64] &= ~(1ull << (e->k.usage % 64));
    }
    if (ring_room()) {
        ring_put(e);
    } else {
        struct in_ev *last = &inq[(in_head + INQ - 1) % INQ];
        if (!e->is_key && !last->is_key && last->m.absolute == e->m.absolute) {
            last->m.dx += e->m.dx;
            last->m.dy += e->m.dy;
            last->m.wheel += e->m.wheel;
            last->m.buttons = e->m.buttons;
            last->m.x = e->m.x;
            last->m.y = e->m.y;
            last->m.time_ns = e->m.time_ns;
        } else if (e->is_key && !e->k.pressed && e->k.usage) {
            released[e->k.usage / 64] |= 1ull << (e->k.usage % 64);
            released_layout = e->k.layout;
            any_released = true;
        } else {
            in_lost++;
        }
    }
    spin_unlock(&in_lock, f);
    desk_wake();
}

static void key_sink(const struct key_event *e) { input_put(&(struct in_ev){.is_key = true, .k = *e}); }
static void mouse_sink(const struct mouse_event *e) { input_put(&(struct in_ev){.is_key = false, .m = *e}); }

/* The oldest input event, false when there is none. */
static bool input_take(struct in_ev *out) {
    uint64_t f = spin_lock(&in_lock);
    bool any = in_tail != in_head;
    if (any) {
        *out = inq[in_tail];
        in_tail = (in_tail + 1) % INQ;
    }
    spin_unlock(&in_lock, f);
    return any;
}

/* A key whose release the full ring kept as a bit, false when there is none. */
static bool input_take_release(uint16_t *key) {
    bool any = false;
    uint64_t f = spin_lock(&in_lock);
    for (size_t i = 0; any_released && i < ARRAY_SIZE(released) && !any; i++)
        if (released[i]) {
            unsigned bit = (unsigned)__builtin_ctzll(released[i]);
            released[i] &= ~(1ull << bit);
            *key = (uint16_t)(i * 64 + bit);
            any = true;
        }
    if (!any) any_released = false;
    spin_unlock(&in_lock, f);
    return any;
}

static void toggle_maximize(struct window *w) {
    if (w->fullscreen) return;
    if (!(w->flags & WIN_RESIZABLE)) return;
    damage_window(w);
    struct gui_event e = {.type = EV_RESIZE};
    if (w->maximized) {
        w->maximized = false;
        w->x = w->restore.x;
        w->y = w->restore.y;
        e.x = w->restore.w;
        e.y = w->restore.h;
    } else {
        struct rect a = part_area(pacing(w), false);
        w->maximized = true;
        w->restore = (struct rect){w->x, w->y, w->cw, w->ch};
        w->x = a.x;
        w->y = a.y;
        e.x = a.w - 2 * BORDER;
        e.y = a.h - TITLE_H - BORDER;
    }
    damage_window(w);
    push_event(w, &e);
}

static void cycle_windows(void) {
    struct window *f = focused();
    if (!f) {
        /* restore the most recent minimized window */
        for (size_t i = nwin; i-- > 0;)
            if (zorder[i]->shown && !zorder[i]->dead && zorder[i]->layer == N_LAYER_NORMAL) {
                raise_window(zorder[i]);
                return;
            }
        return;
    }
    int count = 0;
    for (size_t i = 0; i < nwin; i++)
        if (visible(zorder[i]) && zorder[i]->layer == N_LAYER_NORMAL) count++;
    if (f->layer != N_LAYER_NORMAL) {
        for (size_t i = nwin; i-- > 0;)
            if (visible(zorder[i]) && zorder[i]->layer == N_LAYER_NORMAL) {
                raise_window(zorder[i]);
                return;
            }
        return;
    }
    if (count < 2) return;
    lower_to_bottom(f);
}

/* The hot key's window takes a press that is one (and its release goes with it). */
static bool hotkey_take(const struct key_event *k) {
    uint32_t mods = k->mods & (NMOD_SHIFT | NMOD_CTRL | NMOD_ALT);
    for (size_t i = 0; i < nhotkeys; i++) {
        struct hotkey *h = &hotkeys[i];
        if (h->usage != k->usage) continue;
        if (!k->pressed && h->held) {
            h->held = false;
            return true;
        }
        if (!k->pressed || h->mods != mods) continue;
        h->held = true;
        push_event(h->w, &(struct gui_event){.type = EV_HOTKEY, .usage = k->usage, .layout = k->layout, .mods = mods, .pressed = 1});
        return true;
    }
    return false;
}

static void handle_key(const struct key_event *k) {
    if (hotkey_take(k)) return;
    struct window *f = focused();
    if (k->pressed) {
        bool alt = k->mods & NMOD_ALT;
        if (alt && k->usage == N_USAGE_TAB) {
            cycle_windows();
            return;
        }
        if (alt && k->usage == N_USAGE_F4) {
            if (f) send_simple(f, EV_CLOSE);
            return;
        }
    }
    if (f) {
        struct gui_event e = {.type = EV_KEY, .key = k->text, .usage = k->usage, .layout = k->layout, .mods = k->mods,
                              .pressed = k->pressed};
        push_event(f, &e);
    }
}

static void damage_cursor(void) { damage_rect(mx - pointer_hx, my - pointer_hy, POINTER_SIZE, POINTER_SIZE); }

/* What the pointer is over decides what it shows: the WM's own shapes over what the WM draws (a
   frame, the resize band) and during its drags, the
   window's wish (SYS_WIN_POINTER) over its client area and while a drag that started there goes on.
   *from is that window, NULL for one of the WM's shapes. */
static int pointer_choose(struct window **from) {
    *from = NULL;
    if (drag_mode == DRAG_RESIZE) return N_POINTER_NWSE_RESIZE;
    if (drag_mode == DRAG_CLIENT) {
        if (!drag_win) return N_POINTER_DEFAULT;
        *from = drag_win;
        return (int)drag_win->pointer_shape;
    }
    if (drag_mode != DRAG_NONE) return N_POINTER_DEFAULT;
    struct window *w = window_at(mx, my);
    if (!w || in_resize_grip(w, mx, my)) return w ? N_POINTER_NWSE_RESIZE : N_POINTER_DEFAULT;
    if (!in_rect(mx, my, client_x(w), client_y(w), w->cw, w->ch)) return N_POINTER_DEFAULT;
    *from = w;
    return (int)w->pointer_shape;
}

/* Brings the pointer's image up to what it should show; the old and the new place are drawn again.
   After input, after a window's wish, and on each desktop pass (windows that moved, opened or
   closed under it). */
static void pointer_update(void) {
    struct window *w;
    int shape = pointer_choose(&w);
    int from = w && w->pointer_image ? w->id : 0;
    uint32_t serial = from ? w->pointer_serial : 0;
    if (shape == pointer_shape && from == pointer_from && serial == pointer_from_serial) return;
    damage_cursor();
    if (from) {
        memset(pointer_px, 0, sizeof pointer_px);
        for (int y = 0; y < w->pointer_h; y++)
            memcpy(pointer_px + y * POINTER_SIZE, w->pointer_image + y * w->pointer_w, (size_t)w->pointer_w * 4);
        pointer_hx = w->pointer_hx;
        pointer_hy = w->pointer_hy;
    } else {
        memcpy(pointer_px, pointer_art(shape, &pointer_hx, &pointer_hy), sizeof pointer_px);
    }
    pointer_shape = shape;
    pointer_from = from;
    pointer_from_serial = serial;
    __atomic_store_n(&pointer_serial, pointer_serial + 1 ? pointer_serial + 1 : 1, __ATOMIC_RELAXED);
    damage_cursor();
}

static void update_hover(void) {
    struct window *w = window_at(mx, my);
    int b = w ? hit_button(w, mx, my) : 0;
    if (w != pointer_over) {
        if (pointer_over && pointer_over->layer != N_LAYER_NORMAL) send_simple(pointer_over, EV_MOUSE_LEAVE);
        pointer_over = w;
    }
    if (w != hover_win || b != hover_btn) {
        if (hover_win && hover_btn) damage_rect(hover_win->x + frame_w(hover_win) - 80, hover_win->y, 80, TITLE_H);
        hover_win = b ? w : NULL;
        hover_btn = b;
        if (hover_win) damage_rect(w->x + frame_w(w) - 80, w->y, 80, TITLE_H);
    }
    pointer_update();
}

/* A button went down at the clock's at: one more click in a row when the last press was of the same button,
   less than the double click's time before it, at about the same place. */
static void count_click(int button, uint64_t at) {
    bool more = button == last_click_button && at - last_click < input_double_click_ns() &&
                ABS(mx - last_click_x) < 6 && ABS(my - last_click_y) < 6;
    press_clicks = more ? press_clicks + 1 : 1;
    last_click = at;
    last_click_button = button;
    last_click_x = mx;
    last_click_y = my;
}

/* The left button went down. */
static void left_press(void) {
    bool dbl = press_clicks % 2 == 0;
    struct window *w = window_at(mx, my);
    if (w) {
        if (w != focused() && !w->no_focus) raise_window(w);
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
}

static void left_release(void) {
    struct window *w = drag_win;
    int mode = drag_mode;
    drag_mode = DRAG_NONE;
    drag_win = NULL;
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
        struct gui_event e = {.type = EV_RESIZE, .x = resize_rect.w - (frame_w(w) - w->cw),
                              .y = resize_rect.h - (frame_h(w) - w->ch)};
        if (e.x != w->cw || e.y != w->ch) push_event(w, &e);
        break;
    }
    case DRAG_CLIENT: send_mouse(w, EV_MOUSE_UP, mbuttons); break;
    default: break;
    }
}

/* Whether a point lies on a part of the desktop. */
static bool on_desktop(int x, int y) {
    for (size_t i = 0; i < nwscreens; i++) {
        struct wscreen *ws = wscreens[i];
        if (part_on(ws) && in_rect(x, y, ws->x, ws->y, ws->w, ws->h)) return true;
    }
    return false;
}

/* A pointer move or button that came at the clock's at. */
static void handle_mouse(const struct mouse_event *m, uint64_t at) {
    int dx = m->dx, dy = m->dy; /* dev/input.c has scaled a relative move */
    if (m->absolute) {
        /* a tablet-style pointer (a USB tablet's, a hypervisor's): exactly where it says, over the main screen */
        struct wscreen *main = screen_main();
        dx = (main ? main->x : 0) + (int)((int64_t)m->x * (sw - 1) / MOUSE_ABS_MAX) - mx;
        dy = (main ? main->y : 0) + (int)((int64_t)m->y * (sh - 1) / MOUSE_ABS_MAX) - my;
    } else {
    }
    int ox = mx, oy = my;
    if (dx || dy) {
        int nx = MAX(0, MIN(desk_w - 1, mx + dx)), ny = MAX(0, MIN(desk_h - 1, my + dy));
        if (!on_desktop(nx, ny)) { /* a part lower than the desktop: stay on the one it was on */
            struct wscreen *ws = screen_at(mx, my);
            if (ws) {
                nx = MAX(ws->x, MIN(ws->x + ws->w - 1, nx));
                ny = MAX(ws->y, MIN(ws->y + ws->h - 1, ny));
            }
        }
        damage_cursor();
        mx = nx;
        my = ny;
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
            if (w->y > desk_h - TITLE_H) w->y = desk_h - TITLE_H;
            if (w->x > desk_w - 60) w->x = desk_w - 60;
            if (w->x + frame_w(w) < 60) w->x = 60 - frame_w(w);
            w->maximized = false;
            damage_window(w);
        } else if (drag_mode == DRAG_RESIZE && drag_win) {
            struct window *w = drag_win;
            damage_rect(resize_rect.x, resize_rect.y, resize_rect.w, resize_rect.h);
            int nw = mx + drag_dx - w->x, nh = my + drag_dy - w->y;
            int minw = MIN_CW + (frame_w(w) - w->cw), minh = MIN_CH + (frame_h(w) - w->ch);
            resize_rect.w = MAX(minw, MIN(nw, desk_w - w->x));
            resize_rect.h = MAX(minh, MIN(nh, desk_h - w->y));
            damage_rect(resize_rect.x, resize_rect.y, resize_rect.w, resize_rect.h);
        } else if (drag_mode == DRAG_CLIENT && drag_win) {
            send_mouse(drag_win, EV_MOUSE_MOVE, mbuttons);
        } else if (drag_mode == DRAG_NONE) {
            struct window *w = window_at(mx, my);
            if (w && in_rect(mx, my, client_x(w), client_y(w), w->cw, w->ch)) send_mouse(w, EV_MOUSE_MOVE, mbuttons);
        }
        update_hover();
    }

    int changed = old_buttons ^ mbuttons;
    if (changed & 1) {
        if (mbuttons & 1) {
            count_click(1, at);
            left_press();
        } else {
            left_release();
        }
        update_hover();
    }
    if (changed & 6) {
        /* right / middle buttons go straight to the window under the cursor */
        struct window *w = drag_mode == DRAG_CLIENT ? drag_win : window_at(mx, my);
        if (mbuttons & changed & 6) count_click(mbuttons & changed & 6, at);
        if (w && in_rect(mx, my, client_x(w), client_y(w), w->cw, w->ch)) {
            if (w != focused() && !w->no_focus && (mbuttons & 6)) raise_window(w);
            send_mouse(w, (mbuttons & changed & 6) ? EV_MOUSE_DOWN : EV_MOUSE_UP, mbuttons);
        }
    }
    if (m->wheel) {
        struct window *w = window_at(mx, my);
        if (!w) w = focused();
        if (w) {
            struct gui_event e = {.type = EV_WHEEL, .x = mx - client_x(w), .y = my - client_y(w), .wheel = m->wheel};
            push_event(w, &e);
        }
    }
}

/* ---- the desktop thread ----
   Each pass applies the input waiting, the keys whose releases the ring kept, and what follows
   from them (the pointer's image, the screens' states), and then sleeps while desk_wakes still
   reads the same. Its deadline is the fastest screen's refresh: input reaches
   the windows within a frame. */
static void desktop_thread(void *arg) {
    (void)arg;
    uint64_t policy = UINT64_MAX;
    for (;;) {
        uint64_t seen = __atomic_load_n(&desk_wakes, __ATOMIC_SEQ_CST);
        wm_lock();
        uint64_t fastest = 0;
        for (size_t i = 0; i < nwscreens; i++)
            if (wscreens[i]->period_ns && (!fastest || wscreens[i]->period_ns < fastest)) fastest = wscreens[i]->period_ns;
        struct in_ev ev;
        while (input_take(&ev)) {
            input_at = ev.is_key ? ev.k.time_ns : ev.m.time_ns;
            if (!input_at) input_at = clock_now_ns();
            if (ev.is_key) handle_key(&ev.k);
            else handle_mouse(&ev.m, input_at);
        }
        uint16_t key;
        while (input_take_release(&key)) {
            input_at = clock_now_ns();
            handle_key(&(struct key_event){.usage = key, .layout = released_layout, .pressed = false, .time_ns = input_at});
        }
        input_at = 0;
        pointer_update();
        direct_update();
        bool smooth = deep_wanted && fb_dither_able();
        wm_unlock();
        if (fastest != policy) {
            policy = fastest;
            sched_set_policy(thread_current(),
                             fastest ? sched_deadline(MIN(MAX(fastest, SCHED_PERIOD_MIN_NS), SCHED_PERIOD_MAX_NS))
                                     : sched_fair(SCHED_WEIGHT_DEFAULT, SCHED_REQUEST_DEFAULT_NS));
        }
        if (smooth != fb.simulated) set_deep_simulation(smooth);
        /* Screen 0 changed (a display driver's buffers, the boot screen given back): whether smooth
           gradients can be shown and what decides its mode, as reported. */
        struct screen *s0 = screen_get(0);
        bool able = fb_dither_able(), boot = s0 && screen_boot(s0);
        if (able != seen_dither_able || boot != seen_boot) {
            seen_dither_able = able;
            seen_boot = boot;
            wm_screen_info_changed();
        }
        uint64_t lost = __atomic_exchange_n(&in_lost, 0, __ATOMIC_RELAXED);
        if (lost) kprintf("wm: the input ring was full: %lu key presses lost\n", (unsigned long)lost);
        if (__atomic_load_n(&in_head, __ATOMIC_ACQUIRE) != in_tail || __atomic_load_n(&any_released, __ATOMIC_RELAXED)) continue;
        wq_wait_unless(&desk_wq, &desk_wakes, seen, CLOCK_NEVER);
    }
}

bool wm_running(void) { return __atomic_load_n(&running, __ATOMIC_ACQUIRE); }

void wm_screen_size(uint32_t *w, uint32_t *h) {
    wm_lock();
    *w = (uint32_t)sw;
    *h = (uint32_t)sh;
    wm_unlock();
}

/* The clipboard is now nb's n bytes (kmalloc'd, with a NUL after them), which it keeps. */
static void clipboard_put(char *nb, size_t n) {
    wm_lock();
    kfree(clipboard);
    clipboard = nb;
    clipboard_len = n;
    clipboard_open = !process_current();
    if (!clipboard_open) clipboard_owner = *identity_current();
    clipboard_seq++;
    wm_unlock();
}

/* 中身は核の heap にある。一つのアプリが核のメモリを使い尽くさないよう、機械のメモリの 8 分の 1 までにする。 */
static bool clipboard_fits(size_t n) { return n <= pmm_total_pages() * PAGE_SIZE / 8; }

bool wm_clipboard_set(const char *text, size_t n) {
    if (!clipboard_fits(n)) return false;
    char *nb = kmalloc(n + 1);
    if (!nb) return false;
    memcpy(nb, text, n);
    nb[n] = 0;
    clipboard_put(nb, n);
    return true;
}

void wm_clipboard_info(size_t *len, uint32_t *seq) {
    wm_lock();
    *len = clipboard_len;
    if (seq) *seq = clipboard_seq;
    wm_unlock();
}

char *wm_clipboard_snapshot(size_t *len) {
    wm_lock();
    char *text = kmalloc(clipboard_len + 1);
    if (text) {
        if (clipboard_len) memcpy(text, clipboard, clipboard_len);
        text[clipboard_len] = 0;
        *len = clipboard_len;
    }
    wm_unlock();
    return text;
}

/* ---- the screens' drivers ---- */
void wm_screen_direct(unsigned screen, bool able) {
    wm_lock();
    struct wscreen *ws = wscreen_get(screen, true);
    if (ws && ws->direct_able != able) {
        ws->direct_able = able;
        kprintf("wm: screen %u: %s\n", screen,
                able ? "the display driver can show fullscreen windows directly"
                     : "fullscreen windows are composited into the desktop again");
        direct_update();
    }
    wm_unlock();
}

void wm_screen_mode(unsigned screen, const struct n_screen_mode *mode, uint64_t serial) {
    wm_lock();
    struct wscreen *ws = wscreen_get(screen, true);
    if (ws && serial > ws->state.mode_serial) {
        ws->state.mode = *mode;
        ws->state.mode_serial = serial;
        state_changed(ws);
    }
    wm_unlock();
}

int64_t wm_screen_state(unsigned screen, uint64_t seen, struct n_screen_state *out, uint64_t deadline) {
    int64_t result = 0;
    wm_lock();
    struct wscreen *ws = wscreen_get(screen, true);
    if (!ws) {
        wm_unlock();
        return -N_ERR_NO_MEMORY;
    }
    while (ws->state.serial == seen) {
        if (thread_current()->killed) {
            result = -N_ERR_STOPPED;
            break;
        }
        if (clock_now_ns() >= deadline) {
            result = -N_ERR_TIMEOUT;
            break;
        }
        /* A change on another CPU after this look moves the serial on before it wakes state_wq:
           the wait does not begin once it has. */
        wm_unlock();
        wq_wait_unless(&ws->state_wq, &ws->state.serial, seen, deadline);
        wm_lock();
    }
    *out = ws->state;
    wm_unlock();
    return result;
}

/* *x, *y go as far as the part that was under cx, cy moved (was_x, was_y). wm_lock held. */
static void moved_with_part(int cx, int cy, int *x, int *y) {
    for (size_t i = 0; i < nwscreens; i++) {
        struct wscreen *ws = wscreens[i];
        if (!part_on(ws) || !in_rect(cx, cy, ws->was_x, ws->was_y, ws->w, ws->h)) continue;
        *x += ws->x - ws->was_x;
        *y += ws->y - ws->was_y;
        return;
    }
}

/* SYS_SCREEN_ARRANGE (a kernel copy): where the display service puts screen id's part, whether it is
   the main screen, its scale. Windows and the pointer move with the part they are on. */
int64_t wm_screen_arrange(unsigned id, const struct n_screen_arrange *a) {
    if (!wm_running()) return -N_ERR_UNSUPPORTED;
    /* a part is at most UINT16_MAX wide and high (pci_user_screen_modes): its end must fit an int */
    if ((a->flags & ~(N_ARRANGE_PLACED | N_ARRANGE_PRIMARY)) || a->x < 0 || a->y < 0 || a->x > INT32_MAX - UINT16_MAX ||
        a->y > INT32_MAX - UINT16_MAX || !a->scale_percent)
        return -N_ERR_INVALID;
    wm_lock();
    struct wscreen *ws = wscreen_get(id, true);
    if (!ws) {
        wm_unlock();
        return screen_get(id) ? -N_ERR_NO_MEMORY : -N_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < nwscreens; i++) {
        wscreens[i]->was_x = wscreens[i]->x;
        wscreens[i]->was_y = wscreens[i]->y;
    }
    ws->placed = a->flags & N_ARRANGE_PLACED;
    ws->place_x = a->x;
    ws->place_y = a->y;
    ws->scale_percent = a->scale_percent;
    if (a->flags & N_ARRANGE_PRIMARY) primary_screen = ws;
    layout();
    for (size_t k = 0; k < nwin; k++) {
        struct window *w = zorder[k];
        moved_with_part(w->x + frame_w(w) / 2, w->y + frame_h(w) / 2, &w->x, &w->y);
    }
    moved_with_part(mx, my, &mx, &my);
    windows_keep_in();
    mx = MAX(0, MIN(mx, desk_w - 1));
    my = MAX(0, MIN(my, desk_h - 1));
    for (size_t i = 0; i < nwscreens; i++) windows_refit(wscreens[i]);
    damage_all();
    direct_update();
    wm_unlock();
    kprintf("wm: screen %u: at %d,%d%s, scale %u%%\n", id, a->x, a->y, a->flags & N_ARRANGE_PRIMARY ? ", the main screen" : "",
            a->scale_percent);
    wm_screen_info_changed();
    return 0;
}

/* What struct screen_modes says of screen id's part. */
void wm_screen_report(unsigned id, struct screen_modes *out) {
    wm_lock();
    struct wscreen *ws = wscreen_get(id, false);
    if (ws) {
        out->on = part_on(ws);
        out->x = ws->x;
        out->y = ws->y;
        out->width = (uint32_t)ws->w;
        out->height = (uint32_t)ws->h;
        out->placed = ws->placed;
        out->primary = ws == screen_main();
        out->scale_percent = ws->scale_percent;
    } else {
        out->primary = id == 0;
        out->scale_percent = 100;
    }
    wm_unlock();
}

/* ---- remote display ---- */
static struct wscreen *screen0(void) {
    wm_lock();
    struct wscreen *ws = nwscreens ? wscreens[0] : NULL;
    wm_unlock();
    return ws;
}

/* Screen 0's part at the viewers' size, or back at its mode's when none is left. */
static void remote_size(int *w, int *h) {
    struct wscreen *ws = screen0();
    if (!ws) return;
    wm_lock();
    if (*w > 0 && *h > 0) {
        ws->remote_w = *w;
        ws->remote_h = *h;
    }
    int tw = remote_users ? ws->remote_w : 0, th = remote_users ? ws->remote_h : 0;
    wm_unlock();
    if (!tw) {
        struct screen_mode_desc d;
        if (screen_mode(ws->s, &d)) {
            tw = (int)d.width;
            th = (int)d.height;
        }
    }
    wm_lock();
    bool change = tw && (tw != ws->w || th != ws->h);
    wm_unlock();
    if (change) wscreen_resize(ws, tw, th);
    wm_lock();
    *w = ws->w;
    *h = ws->h;
    damage_all(); /* drawn again without the pointer, or with it */
    wm_unlock();
}

void wm_remote_resize(int *w, int *h) { remote_size(w, h); }

bool wm_remote_attach(int *w, int *h) {
    wm_lock();
    if (!wm_running() || !nwscreens) {
        wm_unlock();
        return false;
    }
    if (!__atomic_fetch_add(&remote_users, 1, __ATOMIC_RELAXED)) {
        wscreens[0]->remote_w = wscreens[0]->w;
        wscreens[0]->remote_h = wscreens[0]->h;
        wm_screen_info_changed(); /* the source is the remote viewer's window now */
    }
    direct_update();
    wm_unlock();
    remote_size(w, h);
    return true;
}

void wm_remote_detach(void) {
    wm_lock();
    if (remote_users > 0 && __atomic_sub_fetch(&remote_users, 1, __ATOMIC_RELAXED) == 0) wm_screen_info_changed();
    direct_update();
    wm_unlock();
    int w = 0, h = 0;
    remote_size(&w, &h);
}

void wm_repaint(void) {
    if (!wm_running()) return;
    wm_lock();
    damage_all();
    wm_unlock();
}

/* Whether a remote viewer has the desktop at its window's size now. Read without wm_lock, so a
   caller holding another lock (or a spinlock) may ask. */
bool wm_remote_viewing(void) { return __atomic_load_n(&remote_users, __ATOMIC_RELAXED) > 0; }

int wm_remote_damage(struct wm_rect *out, int max) {
    struct wscreen *ws = screen0();
    if (!ws || max <= 0) return 0;
    mutex_lock(&ws->back_lock);
    struct rect *list = NULL;
    size_t n = 0, room = 0;
    if (!tiles_rects(&ws->remote, ws->w, ws->h, &list, &n, &room)) {
        n = 1;
        if (!room) list = kmalloc(sizeof *list);
        if (list) list[0] = (struct rect){0, 0, ws->w, ws->h};
        else n = 0;
    }
    tiles_clear(&ws->remote);
    mutex_unlock(&ws->back_lock);
    int count = (int)MIN(n, (size_t)max);
    for (int i = 0; i < count; i++) out[i] = (struct wm_rect){list[i].x, list[i].y, list[i].w, list[i].h};
    if (n > (size_t)max) {
        struct rect r = list[max - 1];
        for (size_t i = (size_t)max; i < n; i++) r = rect_union(r, list[i]);
        out[max - 1] = (struct wm_rect){r.x, r.y, r.w, r.h};
    }
    kfree(list);
    return count;
}

bool wm_remote_copy(uint32_t *dst, int pitch, int x, int y, int w, int h) {
    struct wscreen *ws = screen0();
    if (!ws) return false;
    mutex_lock(&ws->back_lock);
    int bw = ws->back.w, bh = ws->back.h;
    bool valid = ws->back.px && x >= 0 && y >= 0 && w >= 0 && h >= 0 && x <= bw && y <= bh && w <= bw - x && h <= bh - y;
    if (valid)
        for (int row = 0; row < h; row++)
            memcpy(dst + (size_t)row * pitch, ws->back.px + (size_t)(y + row) * ws->back.pitch + x, (size_t)w * 4);
    mutex_unlock(&ws->back_lock);
    return valid;
}

uint32_t wm_pointer_image(uint32_t seen, uint32_t *px, int *hot_x, int *hot_y) {
    if (__atomic_load_n(&pointer_serial, __ATOMIC_RELAXED) == seen) return seen;
    wm_lock();
    memcpy(px, pointer_px, sizeof pointer_px);
    *hot_x = pointer_hx;
    *hot_y = pointer_hy;
    uint32_t serial = pointer_serial;
    wm_unlock();
    return serial;
}

static const struct screen_display display = {.repaint = wm_repaint, .direct = wm_screen_direct,
                                              .modes_offer = screen_modes_offer, .modes_gone = screen_modes_gone,
                                              .modes_shown = screen_modes_shown, .state = wm_screen_state};

void wm_init(void) {
    screen_display = &display;
    watched_init(&screen_watched, NULL);
    watched_init(&shell_watched, NULL);
    __atomic_store_n(&shell_watchable, true, __ATOMIC_RELEASE);
    seen_dither_able = fb_dither_able();
    struct screen *s0 = screen_get(0);
    seen_boot = s0 && screen_boot(s0);
    screen_watchable = true;
    wm_lock();
    unsigned n = screen_count();
    for (unsigned i = 0; i < n; i++) wscreen_get(i, true);
    size_t count = nwscreens;
    wm_unlock();
    if (!count) {
        kprintf("wm: no screen\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        struct wscreen *ws = wscreens[i];
        struct screen_mode_desc d;
        if (screen_mode(ws->s, &d) && !wscreen_resize(ws, (int)d.width, (int)d.height))
            kprintf("wm: screen %u: out of memory for its picture\n", ws->id);
    }
    if (!wscreens[0]->back.px) {
        kprintf("wm: out of memory for screen 0's picture\n");
        return;
    }
    kprintf("wm: %u bpc display, %s\n", fb.bpc, fb.deep ? "10-bit deep planes" : "8-bit content, dithered gradients");
    wm_lock();
    mx = sw / 2;
    my = sh / 2;
    wm_unlock();
    __atomic_store_n(&running, true, __ATOMIC_RELEASE); /* after everything a WM call reads */
    screen_changed_hook(screen_changed);
    fbcon_stop();
    input_set_sinks(key_sink, mouse_sink);
    kthread_create("desktop", desktop_thread, NULL);
    wm_lock();
    for (size_t i = 0; i < nwscreens; i++)
        if (!wscreens[i]->thread) wscreen_start(wscreens[i]);
    wm_unlock();
    kprintf("wm: desktop started at %dx%d on %u screens\n", sw, sh, (unsigned)count);
}

int64_t wm_windows_watch(void) { return wm_running() ? watched_open(&shell_watched) : -N_ERR_UNSUPPORTED; }

int wm_window_count(char *top_title, size_t n) {
    if (!wm_running()) return 0;
    int count = 0;
    wm_lock();
    for (size_t i = nwin; i-- > 0;) {
        if (zorder[i]->dead || zorder[i]->layer != N_LAYER_NORMAL) continue;
        if (!count++ && top_title) strlcpy(top_title, zorder[i]->title, n);
    }
    wm_unlock();
    return count;
}

void wm_close_windows(void) {
    if (!wm_running()) return;
    wm_lock();
    for (size_t i = 0; i < nwin; i++)
        if (!zorder[i]->dead && zorder[i]->layer == N_LAYER_NORMAL) send_simple(zorder[i], EV_CLOSE);
    wm_unlock();
}

/* ---- syscalls ---- */
static struct window *handle_window(struct handle *h) {
    if (!h || !handle_is(h, &win_ops)) return NULL;
    struct window *w = h->object->priv;
    return w->dead ? NULL : w;
}

/* A window of the calling app, wm_lock held: NULL with *error set otherwise. */
static struct window *own_window(struct handle *h, int64_t *error) {
    struct window *w = handle_window(h);
    *error = !w ? -N_ERR_NO_HANDLE : w->vmm_space != process_current()->vmm_space ? -N_ERR_DENIED : 0;
    return *error ? NULL : w;
}

/* A title longer than the slot is cut on a UTF-8 sequence boundary and marked with an ellipsis; a
   split sequence would draw as replacement glyphs. */
static int user_title(char *dst, const char *src, size_t max) {
    int r = user_str(dst, src, max);
    if (r != -N_ERR_NAME_TOO_LONG) return r;
    size_t n = max - 4;
    while (n && ((unsigned char)dst[n] & 0xC0) == 0x80) n--;
    memcpy(dst + n, "\xE2\x80\xA6", 4);
    return (int)n + 3;
}

/* What a window call brings from the program's memory, copied before wm_lock is taken: a copy
   can sleep (a page the program has not touched yet), and another thread of the program may
   unmap that memory at any time. */
struct call_input {
    int64_t error;                  /* the copy's failure, reported after the handle's checks */
    char title[sizeof ((struct window *)0)->title];
    struct n_win_pointer pointer;
    uint32_t *image;                /* the pointer's pixels (kmalloc'd): the call keeps them or they are freed */
};

/* SYS_WIN_POINTER's description and pixels, checked. */
static int64_t pointer_read(uint64_t b, uint64_t c, struct n_win_pointer *p, uint32_t **image) {
    if (c != sizeof *p) return -N_ERR_INVALID;
    int r = copy_from_user(p, (const void *)b, sizeof *p);
    if (r < 0) return r;
    if (p->shape >= N_POINTER_SHAPES || p->reserved) return -N_ERR_INVALID;
    if (!p->width && !p->height) return p->hot_x || p->hot_y ? -N_ERR_INVALID : 0;
    if (!p->width || !p->height || p->width > N_POINTER_IMAGE_MAX || p->height > N_POINTER_IMAGE_MAX ||
        p->hot_x >= p->width || p->hot_y >= p->height)
        return -N_ERR_INVALID;
    size_t bytes = (size_t)p->width * p->height * 4;
    if (!p->pixels || !user_ok(p->pixels, bytes)) return -N_ERR_BAD_ADDRESS;
    uint32_t *pixels = kmalloc(bytes);
    if (!pixels) return -N_ERR_NO_MEMORY;
    r = copy_from_user(pixels, p->pixels, bytes);
    if (r < 0) {
        kfree(pixels);
        return r;
    }
    *image = pixels;
    return 0;
}

/* A window's surfaces of cw x ch, made without wm_lock: n of the app's and, presented by
   rectangles, the kernel's copy. False without memory (none made). */
static bool surfaces_new(struct wsurf **surf, unsigned n, struct wsurf **stable, int cw, int ch, bool deep, bool queue) {
    for (unsigned i = 0; i < n; i++)
        if (!(surf[i] = wsurf_new(cw, ch, deep, true))) {
            while (i--) wsurf_put(surf[i]);
            return false;
        }
    *stable = queue ? NULL : wsurf_new(cw, ch, deep, false);
    if (queue || *stable) return true;
    for (unsigned i = 0; i < n; i++) wsurf_put(surf[i]);
    return false;
}

static void show_if_needed(struct window *w) {
    if (w->shown) return;
    w->shown = true;
    /* the desktop and the panels come up behind what has the focus */
    if (w->layer == N_LAYER_DESKTOP) stack_top(w);
    else raise_window(w);
    damage_window(w);
    windows_changed();
    update_focus();
    desk_wake();
}

static int64_t sys_win_create(int cw, int ch, const char *title, int flags, uint64_t surfaces) {
    if (!wm_running()) return -N_ERR_UNSUPPORTED;
    bool queue = flags & WIN_FRAME_QUEUE;
    unsigned n = queue ? (surfaces ? (unsigned)MIN(surfaces, (uint64_t)UINT32_MAX) : 2) : 1;
    if ((queue && (n < 2 || n > N_WIN_SURFACES_MAX)) || (!queue && surfaces > 1)) return -N_ERR_INVALID;
    if ((flags & WIN_MAILBOX) && !queue) return -N_ERR_INVALID;
    if (cw < 1 || ch < 1 || cw > N_WIN_SIZE_MAX || ch > N_WIN_SIZE_MAX) return -N_ERR_INVALID;
    struct window *w = kzalloc(sizeof *w);
    if (!w) return -N_ERR_NO_MEMORY;
    memcpy(w->title, title, sizeof w->title);
    w->flags = flags;
    w->queue = queue;
    w->mailbox = flags & WIN_MAILBOX;
    w->deep = flags & WIN_DEEP_COLOR; /* kept on 8 bpc: the depth can change */
    w->cw = cw;
    w->ch = ch;
    w->shown_surf = -1;
    w->pid = process_current()->pid;
    w->ev = kmalloc(MAILBOX_FIRST * sizeof *w->ev);
    w->ev_room = MAILBOX_FIRST;
    if (!w->ev || !surfaces_new(w->surf, n, &w->stable, cw, ch, w->deep, queue)) {
        kfree(w->ev);
        kfree(w);
        return -N_ERR_NO_MEMORY;
    }
    w->nsurf = n;
    object_init(&w->object, &win_ops, w);
    wm_lock();
    w->id = next_win_id++;
    if (nwin == zorder_room) { /* room first: once installed the window can always be stacked */
        struct window **at = kappend(&zorder, &nwin, &zorder_room, sizeof *at);
        if (at) nwin--;
    }
    struct handle *h = nwin < zorder_room ? handle_open(&w->object, HANDLE_WRITE) : NULL;
    if (!h) {
        window_surfaces_drop(w);
        kfree(w->ev);
        kfree(w);
        wm_unlock();
        return -N_ERR_NO_MEMORY;
    }
    int index = handle_install(h);
    if (index < 0) { /* closing it destroyed the window and freed everything */
        wm_unlock();
        return index;
    }
    w->vmm_space = process_current()->vmm_space;
    vmm_space_get(w->vmm_space);
    zorder[nwin++] = w;
    stack_top(w);
    struct rect a = work_area();
    if (flags & WIN_CENTER) {
        w->x = a.x + (a.w - frame_w(w)) / 2;
        w->y = a.y + (a.h - frame_h(w)) / 2;
    } else {
        static int cascade;
        w->x = 140 + (cascade % 8) * 30;
        w->y = 40 + (cascade % 8) * 26;
        cascade++;
        if (w->x + frame_w(w) > a.x + a.w) w->x = MAX(a.x, a.x + a.w - frame_w(w));
        if (w->y + frame_h(w) > a.y + a.h) w->y = MAX(a.y, a.y + a.h - frame_h(w));
    }
    /* Use the ordinary WM transition and resize event. */
    if (flags & WIN_START_MAXIMIZED) toggle_maximize(w);
    wm_unlock();
    return index;
}

/* SYS_WIN_PRESENT of a window presented by rectangles: the rectangle is copied from the app's
   surface into the kernel's copy under the copy's lock, without wm_lock. */
static int64_t present_rect(struct handle *handle, uint64_t b, uint64_t c, uint64_t d) {
    int64_t error;
    wm_lock();
    struct window *w = own_window(handle, &error);
    if (!w) {
        wm_unlock();
        return error;
    }
    struct wsurf *app = wsurf_get(w->surf[0]), *copy = wsurf_get(w->stable);
    int x = (int)b, y = (int)c, rw = (int)(d >> 32), rh = (int)(d & 0xFFFFFFFF);
    if (rw <= 0 || rh <= 0) {
        x = y = 0;
        rw = w->cw;
        rh = w->ch;
    }
    wm_unlock();
    int x0 = MAX(0, MIN(x, copy->w)), y0 = MAX(0, MIN(y, copy->h));
    int x1 = MAX(x0, (int)MIN((int64_t)copy->w, (int64_t)x + rw));
    int y1 = MAX(y0, (int)MIN((int64_t)copy->h, (int64_t)y + rh));
    mutex_lock(&copy->lock);
    surf_copy(copy, app, x0, y0, x1, y1);
    mutex_unlock(&copy->lock);
    wm_lock();
    int64_t serial = -N_ERR_NO_HANDLE;
    if (!w->dead && w->stable == copy) { /* not resized meanwhile: then the app presents anew */
        serial = (int64_t)++w->presents;
        if (w->flags & WIN_FRAME_EVENTS) w->frame_wanted = true;
        if (!w->shown) show_if_needed(w);
        else if (visible(w)) damage_rect(client_x(w) + x0, client_y(w) + y0, x1 - x0, y1 - y0);
    } else if (!w->dead) {
        serial = (int64_t)w->presents;
    }
    wm_unlock();
    wsurf_put(app);
    wsurf_put(copy);
    return serial;
}

/* SYS_WIN_RESIZE: new surfaces, made without wm_lock, with as much of the old picture as fits. */
static int64_t win_resize(struct handle *handle, int ncw, int nch) {
    if (ncw < 1 || nch < 1 || ncw > N_WIN_SIZE_MAX || nch > N_WIN_SIZE_MAX) return -N_ERR_INVALID;
    int64_t error;
    wm_lock();
    struct window *w = own_window(handle, &error);
    if (!w) {
        wm_unlock();
        return error;
    }
    unsigned n = w->nsurf;
    bool queue = w->queue, deep = w->deep;
    struct wsurf *old_app = wsurf_get(queue ? (w->shown_surf >= 0 ? w->surf[w->shown_surf] : NULL) : w->surf[0]);
    struct wsurf *old_copy = wsurf_get(w->stable);
    wm_unlock();
    struct wsurf *surf[N_WIN_SURFACES_MAX] = {0}, *stable = NULL;
    bool ok = surfaces_new(surf, n, &stable, ncw, nch, deep, queue);
    if (ok) {
        if (old_app) surf_copy(surf[0], old_app, 0, 0, MIN(ncw, old_app->w), MIN(nch, old_app->h));
        if (old_copy) {
            mutex_lock(&old_copy->lock);
            surf_copy(stable, old_copy, 0, 0, MIN(ncw, old_copy->w), MIN(nch, old_copy->h));
            mutex_unlock(&old_copy->lock);
        }
    }
    wsurf_put(old_app);
    wsurf_put(old_copy);
    if (!ok) return -N_ERR_NO_MEMORY;
    wm_lock();
    if (w->dead) {
        wm_unlock();
        for (unsigned i = 0; i < n; i++) wsurf_put(surf[i]);
        wsurf_put(stable);
        return -N_ERR_NO_HANDLE;
    }
    damage_window(w);
    bool had_shown = w->shown_surf >= 0;
    window_surfaces_drop(w);
    memcpy(w->surf, surf, sizeof surf);
    w->stable = stable;
    w->nsurf = n;
    memset(w->state, SURF_FREE, sizeof w->state);
    memset(w->owed, 0, sizeof w->owed);
    if (queue && had_shown) {
        w->state[0] = SURF_SHOWN;
        w->shown_surf = 0;
    }
    w->cw = ncw;
    w->ch = nch;
    uint64_t f = spin_lock(&w->ev_lock);
    w->surfaces_back = 0;
    spin_unlock(&w->ev_lock, f);
    __atomic_add_fetch(&w->surf_seq, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&w->wq);
    damage_window(w);
    desk_wake();
    wm_unlock();
    return 0;
}

/* SYS_WIN_ACQUIRE: a surface of a WIN_FRAME_QUEUE window free to draw into, waiting until
   deadline for one. */
static int64_t win_acquire(struct handle *handle, uint64_t deadline) {
    for (;;) {
        int64_t error;
        wm_lock();
        struct window *w = own_window(handle, &error);
        if (!w) {
            wm_unlock();
            return error;
        }
        if (!w->queue) {
            wm_unlock();
            return -N_ERR_INVALID;
        }
        for (unsigned i = 0; i < w->nsurf; i++)
            if (surface_free(w, i)) {
                w->state[i] = SURF_APP;
                w->owed[i] = false;
                uint64_t f = spin_lock(&w->ev_lock);
                w->surfaces_back &= ~(1u << i);
                spin_unlock(&w->ev_lock, f);
                wm_unlock();
                return i;
            }
        uint64_t seen = __atomic_load_n(&w->surf_seq, __ATOMIC_SEQ_CST);
        wm_unlock();
        if (thread_current()->killed) return -N_ERR_STOPPED;
        if (clock_now_ns() >= deadline) return -N_ERR_TIMEOUT;
        /* the handle's reference keeps w */
        wq_wait_unless(&w->wq, &w->surf_seq, seen, deadline);
    }
}

/* SYS_SCREEN_GRAB: screen 0's picture, a band of rows at a time through the kernel's memory:
   the picture's lock is held only for each band's copy, never across the copy to the app. */
#define GRAB_ROWS 64
static int64_t screen_grab(int cap, uint32_t *dst, uint64_t bytes) {
    int denied = cap_check(cap, N_CAP_SCREEN_READ);
    if (denied < 0) return denied;
    if (!wm_running()) return -N_ERR_UNSUPPORTED;
    struct wscreen *ws = screen0();
    mutex_lock(&ws->back_lock);
    int w = ws->back.w, h = ws->back.h;
    mutex_unlock(&ws->back_lock);
    if (bytes == 0) return (int64_t)w << 16 | h; /* just the size */
    if (bytes < (uint64_t)w * h * 4) return -N_ERR_TOO_LARGE;
    if (!user_ok_w(dst, (size_t)w * h * 4)) return -N_ERR_BAD_ADDRESS;
    uint32_t *band = kmalloc((size_t)w * GRAB_ROWS * 4);
    if (!band) return -N_ERR_NO_MEMORY;
    int64_t r = (int64_t)w << 16 | h;
    for (int y = 0; y < h && r >= 0; y += GRAB_ROWS) {
        int rows = MIN(GRAB_ROWS, h - y);
        mutex_lock(&ws->back_lock);
        bool same = ws->back.w == w && ws->back.h == h;
        for (int row = 0; same && row < rows; row++)
            memcpy(band + (size_t)row * w, ws->back.px + (size_t)(y + row) * ws->back.pitch, (size_t)w * 4);
        mutex_unlock(&ws->back_lock);
        if (!same) r = -N_ERR_BUSY; /* the picture changed size meanwhile */
        else if (copy_to_user(dst + (size_t)y * w, band, (size_t)rows * w * 4) < 0) r = -N_ERR_BAD_ADDRESS;
    }
    kfree(band);
    return r;
}

/* The calls that change a window's state, wm_lock held. */
static int64_t wm_syscall_locked(int num, uint64_t b, uint64_t c, uint64_t e, struct handle *handle,
                                 struct call_input *in) {
    int64_t error;
    struct window *w;
    switch (num) {
    case SYS_WIN_SURFACE: {
        if (!(w = own_window(handle, &error))) return error;
        if (b >= w->nsurf) return -N_ERR_INVALID;
        return shared_kernel_handle(w->surf[b]->object);
    }
    case SYS_WIN_STATE:
        if (!(w = own_window(handle, &error))) return error;
        return win_state_of(w);
    case SYS_WIN_DIRECT:
        if (!(w = own_window(handle, &error))) return error;
        if (b > 1) return -N_ERR_INVALID;
        w->direct_wanted = b != 0;
        direct_update();
        return 0;
    case SYS_WIN_FRAME: {
        if (!(w = own_window(handle, &error))) return error;
        if (!(w->flags & WIN_FRAME_EVENTS)) return -N_ERR_INVALID;
        w->frame_wanted = true; /* the screen that paces it makes a frame for it */
        struct wscreen *ws = pacing(w);
        if (ws) screen_poke(ws->s);
        return 0;
    }
    case SYS_WIN_POINTER: {
        if (!(w = own_window(handle, &error))) return error;
        if (in->error) return in->error;
        const struct n_win_pointer *p = &in->pointer;
        kfree(w->pointer_image);
        w->pointer_image = in->image;
        in->image = NULL;
        w->pointer_shape = p->shape;
        w->pointer_w = (int)p->width;
        w->pointer_h = (int)p->height;
        w->pointer_hx = (int)p->hot_x;
        w->pointer_hy = (int)p->hot_y;
        w->pointer_serial++;
        pointer_update();
        return 0;
    }
    case SYS_WIN_PRESENTATION: {
        if (!(w = own_window(handle, &error))) return error;
        if (b > 1) return -N_ERR_INVALID;
        if (w->fullscreen == !!b) return 0;
        damage_window(w);
        struct gui_event resize = {.type = EV_RESIZE};
        if (b) {
            struct rect a = part_area(pacing(w), true);
            w->fullscreen_restore = (struct rect){w->x, w->y, w->cw, w->ch};
            w->fullscreen = true;
            w->x = a.x;
            w->y = a.y;
            resize.x = a.w;
            resize.y = a.h;
        } else {
            w->fullscreen = false;
            w->x = w->fullscreen_restore.x;
            w->y = w->fullscreen_restore.y;
            resize.x = w->fullscreen_restore.w;
            resize.y = w->fullscreen_restore.h;
        }
        push_event(w, &resize);
        damage_window(w);
        damage_panels();
        windows_changed();
        desk_wake();
        return 0;
    }
    case SYS_WIN_PRESENT: { /* WIN_FRAME_QUEUE: surface e - 1, whole */
        if (!(w = own_window(handle, &error))) return error;
        if (!e || e > w->nsurf || b || c) return -N_ERR_INVALID;
        unsigned i = (unsigned)e - 1;
        if (w->state[i] != SURF_APP && !surface_free(w, i)) return -N_ERR_BUSY;
        if (w->mailbox) /* the one still waiting goes back at once, unshown */
            for (unsigned q = 0; q < w->nqueued; q++) {
                w->state[w->queued[q]] = SURF_FREE;
                w->owed[w->queued[q]] = true;
            }
        if (w->mailbox) w->nqueued = 0;
        w->state[i] = SURF_QUEUED;
        w->owed[i] = false;
        w->queued[w->nqueued] = (int)i;
        w->queued_serial[w->nqueued++] = ++w->presents;
        w->frame_wanted = true;
        surfaces_give_back(w);
        if (!w->shown) show_if_needed(w);
        struct wscreen *ws = pacing(w);
        if (ws) screen_poke(ws->s);
        return (int64_t)w->presents;
    }
    case SYS_WIN_SET_TITLE:
        if (!(w = handle_window(handle))) return -N_ERR_NO_HANDLE;
        if (in->error) return in->error;
        memcpy(w->title, in->title, sizeof w->title);
        if (w->shown) {
            damage_rect(w->x, w->y, frame_w(w), TITLE_H);
            windows_changed();
        }
        return 0;
    case SYS_WIN_MOVE:
        if (!(w = handle_window(handle))) return -N_ERR_NO_HANDLE;
        damage_window(w);
        w->x = (int)b;
        w->y = (int)c;
        damage_window(w);
        desk_wake();
        return 0;
    default: return -N_ERR_UNSUPPORTED;
    }
}

/* SYS_SCREEN_INFO's settings and screen format: the reply is made under wm_lock and copied out
   after it. */
static int64_t sys_screen_info(uint64_t a, uint64_t b, uint64_t c) {
    if (c == SCREEN_SETTINGS_GET) {
        if (b != sizeof(struct desktop_settings)) return -N_ERR_INVALID;
        wm_lock();
        struct desktop_settings ds = {
            .smooth_gradients = deep_wanted,
            .smooth_gradients_available = fb_dither_able(),
        };
        wm_unlock();
        return copy_to_user((void *)a, &ds, sizeof ds);
    }
    /* b == 0: the original struct screen_info; b == sizeof(struct screen_format): the colour
       format as well (display_abi.h). */
    if (b && b != sizeof(struct screen_format)) return -N_ERR_INVALID;
    struct screen_format si = {0};
    wm_lock();
    bool on = wm_running();
    si.width = on ? (uint32_t)sw : fb.width;
    si.height = on ? (uint32_t)work_area().h : fb.height;
    si.bits_per_pixel = fb.bpp;
    si.red_bits = fb.simulated ? 10 : fb.rsize;
    si.green_bits = fb.simulated ? 10 : fb.gsize;
    si.blue_bits = fb.simulated ? 10 : fb.bsize;
    /* Without deep planes the desktop delivers 8 bits per channel. */
    si.bits_per_channel = fb.deep ? fb.bpc : MIN(fb.bpc, 8);
    struct wscreen *main = on ? screen_main() : NULL;
    si.scale_percent = main ? main->scale_percent : 100;
    wm_unlock();
    int r = copy_to_user((void *)a, &si, b ? sizeof si : sizeof(struct screen_info));
    if (r < 0) return r;
    return on ? 0 : -N_ERR_UNSUPPORTED;
}

static int64_t sys_clipboard_set(uint64_t a, uint64_t b) {
    size_t n = b;
    if (!clipboard_fits(n)) return -N_ERR_TOO_LARGE;
    if (!user_ok((const void *)a, n)) return -N_ERR_BAD_ADDRESS;
    char *nb = kmalloc(n + 1);
    if (!nb) return -N_ERR_NO_MEMORY;
    int r = copy_from_user(nb, (const void *)a, n);
    if (r < 0) {
        kfree(nb);
        return r;
    }
    nb[n] = 0;
    clipboard_put(nb, n);
    return 0;
}

/* Up to b bytes of the clipboard: its whole length. */
static int64_t sys_clipboard_get(uint64_t a, uint64_t b) {
    wm_lock();
    struct n_identity owner = clipboard_owner;
    bool open = clipboard_open;
    wm_unlock();
    if (clipboard && !open && !identity_may_reach(identity_current(), &owner)) return -N_ERR_DENIED;
    size_t len;
    char *text = wm_clipboard_snapshot(&len);
    if (!text) return -N_ERR_NO_MEMORY;
    size_t n = MIN(b, len);
    int r = n ? copy_to_user((void *)a, text, n) : 0;
    kfree(text);
    return r < 0 ? r : (int64_t)len;
}

/* ---- the shell (display_abi.h) ---- */

/* SYS_SHELL_PLACE. */
static int64_t shell_place(int window, const void *uplace, size_t size) {
    struct n_shell_place p;
    if (size != sizeof p) return -N_ERR_INVALID;
    int r = copy_from_user(&p, uplace, sizeof p);
    if (r < 0) return r;
    if (p.layer > N_LAYER_OVERLAY || p.reserve_edge > N_EDGE_RIGHT || p.reserve > (uint32_t)N_WIN_SIZE_MAX ||
        (p.flags & ~(N_PLACE_NO_FOCUS | N_PLACE_BLEND)) || p.reserved)
        return -N_ERR_INVALID;
    struct handle *handle = handle_get(window);
    if (!handle) return -N_ERR_NO_HANDLE;
    int64_t result = -N_ERR_WRONG_KIND;
    if (handle_is(handle, &win_ops)) {
        wm_lock();
        struct window *w = own_window(handle, &result);
        if (w && !(w->flags & WIN_NO_DECOR)) result = -N_ERR_INVALID;
        else if (w && p.screen >= nwscreens) result = -N_ERR_NOT_FOUND;
        else if (w) {
            struct wscreen *ws = wscreens[p.screen];
            damage_window(w);
            w->layer = p.layer;
            w->no_focus = p.layer == N_LAYER_PANEL || (p.flags & N_PLACE_NO_FOCUS);
            w->blend = p.flags & N_PLACE_BLEND;
            w->x = ws->x + p.x;
            w->y = ws->y + p.y;
            w->reserve_edge = p.reserve_edge;
            w->reserve_screen = p.screen;
            w->reserve = (int)p.reserve;
            if (w->no_focus && focus_owner == w) focus_owner = NULL;
            stack_top(w);
            damage_window(w);
            reserves_update();
            windows_changed();
            update_focus();
            direct_update();
            desk_wake();
        }
        wm_unlock();
    }
    handle_close(handle);
    return result;
}

/* A list of count items (the kernel's memory, freed here) to the app: the count there is. */
static int64_t list_out(void *out, size_t max, void *list, size_t count, size_t item) {
    int r = 0;
    if (MIN(max, count)) r = copy_to_user(out, list, MIN(max, count) * item);
    kfree(list);
    return r < 0 ? r : (int64_t)count;
}

/* SYS_SHELL_SCREENS. */
static int64_t shell_screens(struct n_shell_screen *out, size_t max) {
    if (max > SIZE_MAX / sizeof *out) return -N_ERR_INVALID;
    wm_lock();
    struct n_shell_screen *list = kzalloc(MAX(nwscreens, (size_t)1) * sizeof *list);
    if (!list) {
        wm_unlock();
        return -N_ERR_NO_MEMORY;
    }
    size_t count = nwscreens;
    for (size_t i = 0; i < count; i++) {
        struct wscreen *ws = wscreens[i];
        list[i] = (struct n_shell_screen){.x = ws->x, .y = ws->y, .width = ws->w, .height = ws->h, .on = part_on(ws),
                                          .scale_percent = ws->scale_percent};
    }
    wm_unlock();
    return list_out(out, max, list, count, sizeof *list);
}

/* SYS_SHELL_WINDOWS: the apps' framed windows that have been shown, oldest first. */
static int64_t shell_windows(struct n_shell_window *out, size_t max) {
    if (max > SIZE_MAX / sizeof *out) return -N_ERR_INVALID;
    wm_lock();
    struct n_shell_window *list = kzalloc(MAX(nwin, (size_t)1) * sizeof *list);
    if (!list) {
        wm_unlock();
        return -N_ERR_NO_MEMORY;
    }
    struct window *f = focused();
    size_t count = 0;
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (w->dead || !w->shown || !decorated(w) || w->layer != N_LAYER_NORMAL) continue;
        struct wscreen *ws = pacing(w);
        struct n_shell_window item = {.id = (uint32_t)w->id, .app = w->pid, .screen = ws ? ws->id : 0,
                                      .state = (w->minimized ? N_SHELL_WINDOW_MINIMIZED : 0) |
                                               (w == f ? N_SHELL_WINDOW_FOCUSED : 0)};
        memcpy(item.title, w->title, sizeof item.title);
        item.title[sizeof item.title - 1] = 0;
        size_t at = count++;
        for (; at > 0 && list[at - 1].id > item.id; at--) list[at] = list[at - 1];
        list[at] = item;
    }
    wm_unlock();
    return list_out(out, max, list, count, sizeof *list);
}

/* SYS_SHELL_WINDOW. */
static int64_t shell_window(uint64_t id, uint64_t op) {
    if (op > N_SHELL_CLOSE) return -N_ERR_INVALID;
    int64_t result = -N_ERR_NOT_FOUND;
    wm_lock();
    for (size_t i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if ((uint64_t)w->id != id || w->dead || w->layer != N_LAYER_NORMAL) continue;
        if (op == N_SHELL_RAISE) raise_window(w);
        else if (op == N_SHELL_MINIMIZE) minimize_window(w);
        else send_simple(w, EV_CLOSE);
        direct_update();
        desk_wake();
        result = 0;
        break;
    }
    wm_unlock();
    return result;
}

/* SYS_SHELL_HOTKEY. */
static int64_t shell_hotkey(int window, uint64_t usage, uint64_t mods) {
    if (usage < N_USAGE_FIRST || usage > 0xFFFF || mods & ~(uint64_t)(NMOD_SHIFT | NMOD_CTRL | NMOD_ALT)) return -N_ERR_INVALID;
    struct handle *handle = handle_get(window);
    if (!handle) return -N_ERR_NO_HANDLE;
    int64_t result = -N_ERR_WRONG_KIND;
    if (handle_is(handle, &win_ops)) {
        wm_lock();
        struct window *w = own_window(handle, &result);
        if (w) {
            bool taken = false;
            for (size_t i = 0; i < nhotkeys; i++)
                taken |= hotkeys[i].usage == usage && hotkeys[i].mods == mods;
            struct hotkey *h = taken ? NULL : kappend(&hotkeys, &nhotkeys, &hotkeys_room, sizeof *h);
            if (h) *h = (struct hotkey){.w = w, .usage = (uint32_t)usage, .mods = (uint32_t)mods};
            result = taken ? -N_ERR_EXISTS : h ? 0 : -N_ERR_NO_MEMORY;
        }
        wm_unlock();
    }
    handle_close(handle);
    return result;
}

static int64_t shell_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    int denied = cap_check((int)a, N_CAP_SHELL);
    if (denied < 0) return denied;
    if (!wm_running()) return -N_ERR_UNSUPPORTED;
    switch (num) {
    case SYS_SHELL_PLACE: return shell_place((int)b, (const void *)c, d);
    case SYS_SHELL_SCREENS: return shell_screens((struct n_shell_screen *)b, c);
    case SYS_SHELL_WINDOWS: return shell_windows((struct n_shell_window *)b, c);
    case SYS_SHELL_WINDOW: return shell_window(b, c);
    case SYS_SHELL_WATCH: return watched_open(&shell_watched);
    default: return shell_hotkey((int)b, c, d);
    }
}

int64_t wm_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    if (num >= SYS_SHELL_PLACE && num <= SYS_SHELL_HOTKEY) return shell_syscall(num, a, b, c, d);
    if (num == SYS_SCREEN_INFO) {
        /* A screen's mode: its own lock, which comes before the WM's (it tells the driver through
           wm_screen_mode). The screen's number is in the op's bits from 8 on. */
        if ((c & 0xff) == SCREEN_MODES_GET) return screen_modes_syscall((void *)a, b, c);
        /* A watch on what all of SYS_SCREEN_INFO reports (else 5 would be read as the reply's size). */
        if (c == SCREEN_WATCH)
            return a || b ? -N_ERR_INVALID : !screen_watchable ? -N_ERR_UNSUPPORTED : watched_open(&screen_watched);
        return sys_screen_info(a, b, c);
    }
    if (num == SYS_CLIPBOARD_SET) return sys_clipboard_set(a, b);
    if (num == SYS_CLIPBOARD_GET) return sys_clipboard_get(a, b);
    if (num == SYS_SCREEN_GRAB) return screen_grab((int)a, (uint32_t *)b, c);
    if (num == SYS_WIN_CREATE) {
        char title[sizeof ((struct window *)0)->title];
        if (!c) strlcpy(title, thread_current()->name, sizeof title);
        else if (user_title(title, (const char *)c, sizeof title) < 0) title[0] = 0;
        return sys_win_create((int)a, (int)b, title, (int)d, e);
    }
    struct handle *handle = handle_get((int)a);
    if (!handle) return -N_ERR_NO_HANDLE;
    if (!handle_is(handle, &win_ops)) {
        handle_close(handle);
        return -N_ERR_WRONG_KIND;
    }
    int64_t result;
    if (num == SYS_WIN_RECEIVE) {
        /* Without the WM lock (ev_lock); the handle's reference keeps the window's memory. */
        result = win_receive(handle->object->priv, (struct gui_event *)b, (size_t)c, d);
    } else if (num == SYS_WIN_PRESENT && !((struct window *)handle->object->priv)->queue) {
        result = e ? -N_ERR_INVALID : present_rect(handle, b, c, d);
    } else if (num == SYS_WIN_RESIZE) {
        result = win_resize(handle, (int)b, (int)c);
    } else if (num == SYS_WIN_ACQUIRE) {
        result = win_acquire(handle, b);
    } else {
        struct call_input in = {0};
        if (num == SYS_WIN_SET_TITLE) {
            if (user_title(in.title, (const char *)b, sizeof in.title) < 0) in.error = -N_ERR_BAD_ADDRESS;
        } else if (num == SYS_WIN_POINTER) {
            in.error = pointer_read(b, c, &in.pointer, &in.image);
        }
        wm_lock();
        result = wm_syscall_locked(num, b, c, e, handle, &in);
        wm_unlock();
        kfree(in.image); /* a pointer image the call did not keep */
    }
    handle_close(handle);
    return result;
}
