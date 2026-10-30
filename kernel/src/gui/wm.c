/*
 * Nocturne window manager + compositor.
 *
 * Runs as a kernel thread. Each window owns a client pixel buffer that is
 * shared with the owning process (mapped into its address space). Apps draw
 * into it and call win_present(); the compositor redraws damaged screen
 * regions back-to-front into a backbuffer and copies them to the framebuffer.
 * Input events are delivered through the window's handle, so an event queue
 * can watch a window together with channels.
 */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "gui/wm.h"
#include "gui/screenmode.h"
#include "dev/fb.h"
#include "dev/gpu.h"
#include "dev/fbcon.h"
#include "dev/audio.h"
#include "dev/input.h"
#include "dev/pciuser.h"
#include "dev/timer.h"
#include "sys/object.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sys/sched.h"
#include "sys/proc.h"
#include "sys/syscall.h"
#include "sys/keep.h"
#include "sys/watch.h"
#include "dev/pm.h"
#include "abi.h"

#define TITLE_H   28
#define TITLE_RADIUS 8 /* existing rounded title geometry; shared by shadow culling */
#define BORDER    1
#define TASKBAR_H 40
#define SHADOW    10
#define POINTER_SIZE ((int)N_SCREEN_POINTER_SIZE) /* every pointer image is this square */
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
    uint32_t *user_buf; /* legacy producer, or second indexed full-frame surface */
    size_t pages;
    uint64_t uaddr;
    uint64_t second_uaddr;
    int front_index, pending_index; /* under wm_lock and ev_lock both (buffers_set) */
    /* The app's mailbox (ev, frame_event and frame_ready), dead's change, and the buffer indices'
       (SYS_WIN_MAP waits for them under it): any CPU, without wm_lock (SYS_WIN_RECEIVE). It comes
       after wm_lock, and before w->wq's lock and the object's. */
    struct spinlock ev_lock;
    bool frame_ready;
    struct gui_event frame_event; /* durable mailbox, not a droppable input event */
    bool frame_wanted; /* WIN_FRAME_EVENTS: presented or asked (SYS_WIN_FRAME) since the last frame that showed it */
    int flags;
    bool shown, minimized, maximized, dead,fullscreen;
    bool direct_wanted; /* win_direct: its own images shown by the display driver when it can be */
    bool deep; /* WIN_DEEP_COLOR: each surface ends with its deep plane (read while back_deep exists) */
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
    struct gui_event ev[EVQ];
    int ev_head, ev_tail, ev_count;
    struct wait_queue wq;
    struct object object; /* what the app's window handle reaches */
};

/* ---- state ----
   Locks, in the order they are taken:
   - wm_mutex (wm_lock): all of the window manager's state that names no other lock: the windows,
     their stacking and focus, the damage, the pointer, the desktop's own state, direct scanout,
     the remote viewers, the clipboard. A sleeping lock, any CPU: it stays held across
     allocations, mappings and copies, and while the compositor draws a frame (not while it
     presents one). Reentrant: notifications and a window handle's last close come from inside a
     WM call. dev/pciuser.c's screen lock and sys/keep.c's write lock come before it.
   - then one of a window's ev_lock, in_lock (the input ring) and vblank_lock: spinlocks, never
     two of them at once. dev/input.c's lock comes before in_lock (the input sinks).
   - then the locks of wait queues and objects (the wakes and notifies made under them).
   The compositor sleeps on wm_wq unless wm_wakes moved (wake_compositor). */
static bool running; /* set once, before the compositor starts */
static int sw, sh;
static canvas_t back, wall;
/* Deep planes (display_abi.h) of back and wall, pitch sw; NULL on an 8 bpc display. */
static uint8_t *back_deep, *wall_deep;
static struct wait_queue wm_wq;
/* Moves on before every wake of wm_wq, after a change of what the compositor waits for: damage,
   a present, a frame asked for, input, the vertical blank it waits for. */
static uint64_t wm_wakes;
static unsigned pending_frames;
/* Without vertical blanks, a frame with damage starts at frame_next_us (µs of uptime) at the
   earliest, and the one after it a frame period later: the refresh of the mode the screen shows
   (frame_period), so the desktop draws no frame the monitor would not show. Frames start on the
   scheduler's millisecond, the deadlines keep the period's fraction, so 59.95 Hz is 59.95 frames a
   second and not one every 16 ms. A frame later than a period starts the count anew.
   frame_mhz is the refresh paced at last. Compositor thread. */
#define FALLBACK_REFRESH_MHZ 60000u /* a mode whose refresh nobody says */
static uint64_t frame_next_us;
static uint32_t frame_mhz;
/* A display driver's vertical blanks (n_pci_screen_vblank, dev/pciuser.c) of the scanout the
   screen is drawn into: while they come, a frame with damage starts at the next blank, not after
   a frame period; VBLANK_LOST_MS without one and the period paces frames again. vblank_count is
   the driver's count of the last blank, vblank_drawn the one the last frame started at;
   vblank_waiting: the compositor sleeps until the next one. Under vblank_lock (the driver's call
   comes on any CPU); vblank_pacing is the compositor's own. */
#define VBLANK_LOST_MS 100
static struct spinlock vblank_lock;
static uint64_t vblank_count, vblank_ns, vblank_at_ms, vblank_drawn;
static bool vblank_seen, vblank_waiting, vblank_pacing;
/* Direct scanout (n_pci_screen_state, dev/pciuser.c): a display driver whose scanout has
   N_SCANOUT_DIRECT (direct_able) shows one app's own images instead of the desktop while that app's
   window asked for it (win_direct), is fullscreen and focused, and nothing of the system is drawn
   over it (the moon menu, the volume panel, a toast, a resize outline) or sent to a remote viewer.
   It then draws the pointer itself. The desktop is still composited into the driver's surface
   meanwhile, so it is current the moment the driver shows it again. direct_state is what the
   driver reads, with the mode the system wants it to show (gui/screenmode.c); its serial changes
   with every change of it, the pointer's moves included while a window is shown (direct_changed;
   a waiter reads the serial without the lock). Under the WM lock. */
static bool direct_able;
static struct window *direct_win;
static uint32_t direct_pointer; /* the pointer_serial of the image in direct_state, 0 for none */
static struct n_screen_state direct_state = {.serial = 1};
static struct wait_queue direct_wq;
/* The user's (struct desktop_settings): pointer speed with its leftover quarter steps, and smooth
   gradients as the user wants them; the compositor applies them between frames while the scanout
   shown takes them (fb_dither_able). The desktop keeps them in the user's AppData\Nocturne\
   Desktop.conf (desktop_keep, sys/keep.c). Under wm_lock. */
static int pointer_speed, pointer_rest_x, pointer_rest_y;
static bool deep_wanted;
/* What SYS_SCREEN_INFO reports changed, the desktop's or the screen mode's (gui/screenmode.c): its
   watches (SCREEN_WATCH) hear of it. From wm_init on; without the desktop there is none.
   seen_dither_able and seen_boot are the scanout shown as the compositor last saw it. */
static struct watched screen_watched;
static bool screen_watchable, seen_dither_able, seen_boot;
/* The compositor presents back without wm_lock (composite): back and its planes stay as they are
   meanwhile. back_presents moves on as each such present ends, before present_wait is woken. */
static bool presenting_back;
static uint64_t back_presents;
static struct wait_queue present_wait;

static struct mutex wm_mutex;
static void wm_lock(void) { mutex_lock(&wm_mutex); }
static void wm_unlock(void) { mutex_unlock(&wm_mutex); }

static struct window *zorder[MAX_WIN]; /* bottom .. top */
static int nwin;
static int next_win_id = 1;
static struct window *focus_owner;

static int mx, my, mbuttons;

/* remote display (RDP): what changed since the remote side last looked. remote_users changes
   under wm_lock and is read without it too (wm_remote_viewing). */
#define MAX_RDAMAGE 16
static int remote_users;
static struct rect rdamage[MAX_RDAMAGE];
static int nrdamage;

/* The pointer (SYS_WIN_POINTER, abi.h). The WM owns it: it draws it at (mx, my) less its hot spot,
   and chooses what it shows (pointer_update): one of its own shapes (desktop_pointer), or the image
   the window under it gave. pointer_px is that image, pointer_shape the shape it was chosen as and
   pointer_from the id of the window whose image it is (0 for the WM's art) as of that window's
   pointer_from_serial. pointer_serial changes with the image and is never 0. Under the WM lock
   (wm_pointer_image reads pointer_serial without it first). */
static uint32_t pointer_px[POINTER_SIZE * POINTER_SIZE];
static int pointer_hx, pointer_hy;
static int pointer_shape = -1, pointer_from;
static uint32_t pointer_from_serial, pointer_serial = 1;

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

static bool canvas_hits(const canvas_t *c, int x, int y, int w, int h) {
    return x < c->cx1 && y < c->cy1 && (int64_t)x + w > c->cx0 && (int64_t)y + h > c->cy0;
}

/* After a change of what the compositor waits for, from any CPU or context: wm_wakes first, so
   that a compositor that looked before the change does not sleep (wm_thread). */
static void wake_compositor(void) {
    __atomic_add_fetch(&wm_wakes, 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&wm_wq);
}

/* wm_lock held. Damage made outside the compositor (a window's call) wakes it, as all does. */
static void damage_rect(int x, int y, int w, int h) {
    struct rect r = {x, y, w, h};
    if (!rect_clip(&r)) return;
    wake_compositor();
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

/* ---- window geometry ---- */
static bool decorated(struct window *w) { return !w->fullscreen&&!(w->flags & WIN_NO_DECOR); }
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
    if(visible(focus_owner))return focus_owner;
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
    if (!(w->flags & WIN_RESIZABLE) || w->maximized || w->fullscreen) return false;
    int fx1 = w->x + frame_w(w), fy1 = w->y + frame_h(w);
    return x >= fx1 - 14 && y >= fy1 - 14 && x < fx1 && y < fy1;
}

/* ---- event queues ---- */
static void push_event(struct window *w, const struct gui_event *e) {
    if (!w) return;
    uint64_t flags = spin_lock(&w->ev_lock);
    if (w->dead) { spin_unlock(&w->ev_lock, flags); return; }
    if (e->type == EV_FRAME) {
        w->frame_event = *e;
        w->frame_ready = true;
        wq_wake_all(&w->wq);
        object_notify(&w->object);
        spin_unlock(&w->ev_lock, flags); return;
    }
    if (e->type == EV_MOUSE_MOVE && w->ev_count) {
        int last = (w->ev_head + EVQ - 1) % EVQ;
        if (w->ev[last].type == EV_MOUSE_MOVE) {
            w->ev[last] = *e;
            spin_unlock(&w->ev_lock, flags); return;
        }
    }
    if (w->ev_count == EVQ) { spin_unlock(&w->ev_lock, flags); return; }
    w->ev[w->ev_head] = *e;
    w->ev_head = (w->ev_head + 1) % EVQ;
    w->ev_count++;
    wq_wake_all(&w->wq);
    object_notify(&w->object);
    spin_unlock(&w->ev_lock, flags);
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
    int destination=nwin-1;
    if(!(w->flags&WIN_ALWAYS_ON_TOP))for(int j=0;j<nwin-1;j++)if(zorder[j]->flags&WIN_ALWAYS_ON_TOP){destination=j;break;}
    for(int j=nwin-1;j>destination;j--)zorder[j]=zorder[j-1];
    zorder[destination] = w;focus_owner=w;
    w->minimized = false;
    damage_window(w);
    damage_taskbar();
    update_focus();
}

static void lower_to_bottom(struct window *w) {
    if(w->flags&WIN_ALWAYS_ON_TOP)return;
    int i = zindex(w);
    if (i < 0) return;
    for (; i > 0; i--) zorder[i] = zorder[i - 1];
    zorder[0] = w;
    if(focus_owner==w)focus_owner=NULL;
    damage_window(w);
    update_focus();
}

static void minimize_window(struct window *w) {
    w->minimized = true;
    if(focus_owner==w)focus_owner=NULL;
    damage_window(w);
    damage_taskbar();
    update_focus();
}

/* ---- direct scanout ---- */
static bool direct_eligible(struct window *w) {
    return direct_able && w && w->direct_wanted && w->fullscreen && visible(w) && w == focused() && !menu_open &&
           !vol_open && !toast_until && !resize_visible && !remote_users;
}

static void direct_event(struct window *w, bool shown) {
    struct gui_event e = {.type = EV_DIRECT, .key = shown};
    push_event(w, &e);
}

/* direct_state changed (wm_lock held): its serial moves on, then its waiters are woken. */
static void direct_changed(void) {
    __atomic_store_n(&direct_state.serial, direct_state.serial + 1, __ATOMIC_SEQ_CST);
    wq_wake_all(&direct_wq);
}

/* Brings direct_state up to what the screen should show; the driver and the windows concerned
   hear of a change. The compositor does this once per pass, after its input; a window's end does
   it at once, so direct_win never outlives its window. */
static void direct_update(void) {
    struct window *w = focused();
    if (!direct_eligible(w)) w = NULL;
    bool changed = false;
    if (w != direct_win) {
        if (direct_win) {
            direct_event(direct_win, false);
            damage_window(direct_win); /* its own buffer shows again once it presents one */
        }
        if (w) direct_event(w, true);
        direct_win = w;
        direct_state.app = w ? (uint32_t)w->pid : 0;
        if (!w) direct_state.pointer_shown = 0; /* the desktop's own picture has the pointer again */
        changed = true;
    }
    if (w) {
        if (direct_pointer != pointer_serial) {
            memcpy(direct_state.pointer, pointer_px, sizeof direct_state.pointer);
            direct_state.hot_x = (uint32_t)pointer_hx;
            direct_state.hot_y = (uint32_t)pointer_hy;
            direct_pointer = pointer_serial;
            direct_state.pointer_serial++;
            changed = true;
        }
        if (!direct_state.pointer_shown || direct_state.pointer_x != mx || direct_state.pointer_y != my) {
            direct_state.pointer_shown = 1;
            direct_state.pointer_x = mx;
            direct_state.pointer_y = my;
            changed = true;
        }
    }
    if (changed) direct_changed();
}

void wm_screen_direct(bool able) {
    wm_lock();
    if (direct_able != able) {
        direct_able = able;
        kprintf("wm: %s\n", able ? "the display driver can show fullscreen windows directly"
                                  : "fullscreen windows are composited into the desktop again");
        direct_update();
    }
    wm_unlock();
}

void wm_screen_mode(const struct n_screen_mode *mode, uint64_t serial) {
    wm_lock();
    if (serial > direct_state.mode_serial) {
        direct_state.mode = *mode;
        direct_state.mode_serial = serial;
        direct_changed();
    }
    wm_unlock();
}

int64_t wm_screen_state(uint64_t seen, struct n_screen_state *out, int64_t timeout_ms) {
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    int64_t result = 0;
    wm_lock();
    while (direct_state.serial == seen) {
        uint64_t now = timeout_ms > 0 ? uptime_ms() : 0;
        if (current_task->killed) { result = -N_ERR_STOPPED; break; }
        if (timeout_ms == 0 || (timeout_ms > 0 && now >= deadline)) { result = -N_ERR_TIMEOUT; break; }
        /* A change on another CPU after this look moves the serial on before it wakes direct_wq:
           the wait does not begin once it has. */
        wm_unlock();
        wq_wait_unless(&direct_wq, &direct_state.serial, seen, timeout_ms > 0 ? deadline - now : 0);
        wm_lock();
    }
    *out = direct_state;
    wm_unlock();
    return result;
}

/* ---- window lifetime ---- */
static void unmap_user(struct window *w) {
    if (!w->pml4) return;
    if (w->uaddr)
        vmm_user_free(w->pml4, w->uaddr, w->pages * PAGE_SIZE); /* PTE_SHARED: frames stay with the window */
    if (w->second_uaddr)
        vmm_user_free(w->pml4, w->second_uaddr, w->pages * PAGE_SIZE);
    w->uaddr = 0;
    w->second_uaddr = 0;
}

/* The buffer indices change (wm_lock held). Whoever's change can end a SYS_WIN_MAP wait wakes w->wq
   after it. */
static void buffers_set(struct window *w, int front, int pending) {
    uint64_t fl = spin_lock(&w->ev_lock);
    w->front_index = front;
    w->pending_index = pending;
    spin_unlock(&w->ev_lock, fl);
}

static void win_destroy_locked(struct window *w) {
    if (w->dead) return;
    if (w->pending_index >= 0) pending_frames--;
    if(focus_owner==w)focus_owner=NULL;
    if (w->shown) damage_window(w);
    damage_taskbar(); /* its button goes away even when focus does not change */
    unmap_user(w);
    if (w->buf) vfree(w->buf, w->pages);
    if (w->user_buf) vfree(w->user_buf, w->pages);
    w->buf = NULL;
    w->user_buf = NULL;
    kfree(w->pointer_image);
    w->pointer_image = NULL;
    int i = zindex(w);
    if (i >= 0) {
        for (; i < nwin - 1; i++) zorder[i] = zorder[i + 1];
        nwin--;
    }
    /* under ev_lock: a receiver or a SYS_WIN_MAP waiter that saw it alive is in w->wq by now, for
       the wake below */
    uint64_t ev = spin_lock(&w->ev_lock);
    w->dead = true;
    w->pending_index = -1;
    spin_unlock(&w->ev_lock, ev);
    if (drag_win == w) {
        drag_win = NULL;
        drag_mode = DRAG_NONE;
        if (resize_visible) damage_all();
        resize_visible = false;
    }
    if (hover_win == w) hover_win = NULL;
    if (last_focus == w) last_focus = NULL;
    update_focus();
    if (direct_win == w) direct_update();
    wq_wake_all(&w->wq);
    object_notify(&w->object);
    wake_compositor();
}

static void win_destroy(struct window *w) {
    wm_lock();
    win_destroy_locked(w);
    wm_unlock();
}

/* Takes every queued event that fits in out[max] (the app's memory), waiting up to timeout_ms
   (negative: until one arrives). The count, 0 when the time ran out, or N_ERR_CLOSED once a
   closed window has handed out its last event. Any CPU, without wm_lock: the events leave the
   mailbox under ev_lock a batch at a time, into the kernel's memory, and are copied to out after
   it (a copy may sleep). out's pages are made present first; events taken for a page another
   thread took away meanwhile are lost, with N_ERR_BAD_ADDRESS. */
#define RECEIVE_BATCH 16
static int64_t win_receive(struct window *w, struct gui_event *out, size_t max, int64_t timeout_ms) {
    if (max == 0 || max > SIZE_MAX / sizeof *out) return -N_ERR_INVALID;
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    size_t room = MIN(max, (size_t)EVQ + 1); /* the queue, and the frame's mailbox */
    if (!user_ok_w(out, room * sizeof *out)) return -N_ERR_BAD_ADDRESS;
    size_t got = 0;
    for (;;) {
        struct gui_event batch[RECEIVE_BATCH];
        size_t n = 0, want = MIN(max - got, (size_t)RECEIVE_BATCH);
        uint64_t fl = spin_lock(&w->ev_lock);
        for (; n < want && w->ev_count; n++) {
            batch[n] = w->ev[w->ev_tail];
            w->ev_tail = (w->ev_tail + 1) % EVQ;
            w->ev_count--;
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
        uint64_t now = timeout_ms > 0 ? uptime_ms() : 0;
        if (w->dead) result = -N_ERR_CLOSED;
        else if (current_task->killed) result = -N_ERR_STOPPED;
        else if (timeout_ms == 0 || (timeout_ms > 0 && now >= deadline)) result = 0;
        if (result <= 0) { spin_unlock(&w->ev_lock, fl); return result; }
        if (timeout_ms > 0) wq_wait_locked_timeout(&w->wq, &w->ev_lock, deadline - now);
        else wq_wait_locked(&w->wq, &w->ev_lock);
        irq_restore(fl);
    }
}

/* Without ev_lock (an events wait asks under the object's lock, which ev_lock comes before): every
   change notifies the object after it, and the wait asks again. */
static uint32_t win_ready(struct handle *h) {
    struct window *w = h->object->priv;
    return __atomic_load_n(&w->ev_count, __ATOMIC_RELAXED) > 0 || __atomic_load_n(&w->frame_ready, __ATOMIC_RELAXED) ||
                   __atomic_load_n(&w->dead, __ATOMIC_RELAXED)
               ? N_EVENT_RECEIVABLE
               : 0;
}

static void win_close(struct handle *h) { win_destroy(h->object->priv); }

static void win_release(struct object *o) { kfree(o->priv); }

static const struct object_ops win_ops = {
    .kind = N_KIND_WINDOW, .ready = win_ready, .close = win_close, .release = win_release};

/* What WIN_STATE answers, wm_lock held: the window's own flags, and whether it is the one
   update_focus last gave the focus to (focused() whenever the WM is between changes) and the one
   shown directly. */
static int64_t win_state_of(struct window *w) {
    bool shown = !w->dead && w->shown && !w->minimized;
    return (shown ? WIN_STATE_VISIBLE : 0) | (last_focus == w ? WIN_STATE_FOCUSED : 0) |
           (w->fullscreen ? WIN_STATE_FULLSCREEN : 0) | (direct_win == w ? WIN_STATE_DIRECT : 0);
}

static bool alloc_buffer(struct window *w, int cw, int ch) {
    w->deep = (w->flags & WIN_DEEP_COLOR) != 0; /* kept on 8 bpc: the depth can change */
    size_t pixels = (size_t)cw * ch;
    size_t pages = ALIGN_UP((uint64_t)pixels * (w->deep ? 5 : 4), PAGE_SIZE) / PAGE_SIZE;
    uint32_t *buf = vmalloc(pages);
    if (!buf) return false;
    uint32_t *producer = vmalloc(pages);
    if (!producer) { vfree(buf, pages); return false; }
    if (w->deep) { /* nothing written yet is 8-bit content */
        memset(buf + pixels, 0, pixels);
        memset(producer + pixels, 0, pixels);
    }
    w->buf = buf;
    w->user_buf = producer;
    w->pages = pages;
    w->cw = cw;
    w->ch = ch;
    uint64_t ev = spin_lock(&w->ev_lock);
    w->front_index = w->pending_index = -1;
    w->frame_ready = false;
    spin_unlock(&w->ev_lock, ev);
    return true;
}

/* A surface of w into t's mmap region (with its guard page after it), wm_lock held. */
static uint64_t map_user(struct window *w, struct task *t, unsigned index) {
    uint64_t bytes = w->pages * PAGE_SIZE;
    uint64_t va = mmap_reserve(t, bytes);
    if (!va) return 0;
    for (size_t i = 0; i < w->pages; i++) {
        uint32_t *pixels = w->flags & WIN_DOUBLE_BUFFER ? (index ? w->user_buf : w->buf) : w->user_buf;
        uint64_t pa = vmm_translate(kernel_pml4, (uint64_t)pixels + i * PAGE_SIZE);
        if (!vmm_map_page(t->pml4, va + i * PAGE_SIZE, pa, PTE_P | PTE_W | PTE_U | PTE_SHARED | pte_nx)) {
            vmm_user_free(t->pml4, va, i * PAGE_SIZE); /* PTE_SHARED: unmap only */
            mmap_hole_add(t, va, bytes + PAGE_SIZE);
            return 0;
        }
    }
    if (index) w->second_uaddr = va;
    else w->uaddr = va;
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
    if (truncated) { memcpy(tmp + i, "\xE2\x80\xA6", 3); i += 3; }
    tmp[i] = 0;
    gfx_text(c, x, y, tmp, col, TRANSPARENT, FONT_SMALL);
}

static void composite(void);
static uint64_t frame_period(void);

/* ---- apps / desktop ---- */
struct app {
    const char *name;
    const char *path;
    int icon;
    const char *arg; /* the one argument, or NULL: apps written in TypeScript run as SYSTEM "js" FILE.js */
};

#define SYSTEM   N_FOLDER_SYSTEM "\\"
#define PROGRAMS N_FOLDER_PROGRAMS "\\"

static const struct app apps[] = {
    {"Terminal", SYSTEM "term", ICON_TERMINAL, NULL},
    {"Files", SYSTEM "files", ICON_FILES, NULL},
    {"Web Browser", PROGRAMS "Browser\\browser", ICON_NET, NULL},
    {"Text Editor", PROGRAMS "Notepad\\notepad", ICON_EDITOR, NULL},
    {"Paint", PROGRAMS "Paint\\paint", ICON_PAINT, NULL},
    {"Calculator", PROGRAMS "Calculator\\calc", ICON_CALC, NULL},
    {"Clock", PROGRAMS "Clock\\clock", ICON_CLOCK, NULL},
    {"System Monitor", SYSTEM "sysmon", ICON_MONITOR, NULL},
    {"Piano", PROGRAMS "Piano\\piano", ICON_PIANO, NULL},
    {"Sound Player", PROGRAMS "Player\\player", ICON_SOUND, NULL},
    {"Snake", PROGRAMS "Snake\\snake", ICON_SNAKE, NULL},
    {"Tetris", PROGRAMS "Tetris\\tetris", ICON_TETRIS, NULL},
    {"Minesweeper", PROGRAMS "Minesweeper\\mines", ICON_MINES, NULL},
    {"Mandelbrot", PROGRAMS "Mandelbrot\\mandel", ICON_FRACTAL, NULL},
    {"3D Cube", PROGRAMS "Cube\\cube", ICON_CUBE, NULL},
    {"Game of Life", PROGRAMS "Life\\life", ICON_LIFE, NULL},
    {"Settings", SYSTEM "js", ICON_SETTINGS, SYSTEM "settings.js"},
    {"About Nocturne", SYSTEM "js", ICON_ABOUT, SYSTEM "about.js"},
    {"Sleep", "@sleep", ICON_SLEEP, NULL},
    {"Hibernate", "@hibernate", ICON_HIBERNATE, NULL},
    {"Restart", "@reboot", ICON_REBOOT, NULL},
    {"Shut Down", "@poweroff", ICON_POWER, NULL},
};
#define NAPPS ((int)ARRAY_SIZE(apps))
#define APP_SLEEP (NAPPS - 4) /* the power items close the menu: Sleep, Hibernate, Restart, Shut Down */

/* the start menu leaves Sleep and Hibernate out on machines that cannot do them (dev/pm.c says which) */
static bool menu_has(int i) {
    if (!strcmp(apps[i].path, "@sleep")) return pm_can_sleep();
    if (!strcmp(apps[i].path, "@hibernate")) return pm_can_hibernate();
    return true;
}
static int menu_count(void) {
    int n = 0;
    for (int i = 0; i < NAPPS; i++) n += menu_has(i);
    return n;
}
static const struct app *menu_app(int i) {
    for (int k = 0; k < NAPPS; k++)
        if (menu_has(k) && i-- == 0) return &apps[k];
    return &apps[NAPPS - 1];
}

static const int desktop_apps[] = {0, 1, 2, 3, 4, 8, 9, 10, 13, 14, 16, 17};
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

/* 8-bit art drawn over a possibly deep picture: remember the pixels of its
   box (inside the canvas clip) before drawing, then mark only the pixels it
   changed as 8-bit content, so the picture around and beneath it keeps its
   low bits. The box must fit the caller's buffer of stride pixels per row. */
struct deep_mark { int x, y, w, h, stride; uint32_t *saved; };
static bool deep_mark_begin(struct deep_mark *m, const canvas_t *c, int x, int y, int w, int h,
                            uint32_t *saved, int stride) {
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
        for (int col = 0; col < m->w; col++) if (now[col] != was[col]) d[col] = 0;
    }
}

/* Icons are 8-bit art over a possibly deep wallpaper. */
#define ICON_AREA_PAD 16
static void draw_desktop_icons(canvas_t *c, uint8_t *deep) {
    static uint32_t saved[(ICON_CELL_W + 2 * ICON_AREA_PAD) * ICON_CELL_H];
    const int stride = ICON_CELL_W + 2 * ICON_AREA_PAD;
    for (int i = 0; i < NDESK; i++) {
        const struct app *a = &apps[desktop_apps[i]];
        int x, y;
        icon_cell(i, &x, &y);
        struct deep_mark mark;
        bool track = deep && deep_mark_begin(&mark, c, x - ICON_AREA_PAD, y, ICON_CELL_W + 2 * ICON_AREA_PAD,
                                             ICON_CELL_H, saved, stride);
        int cw = ICON_CELL_W - 8;
        desktop_draw_icon(c, x + (cw - 32) / 2, y + 8, a->icon);
        const char *label = a->name;
        if (!strcmp(label, "About Nocturne")) label = "About";
        int tw = gfx_text_width(label, FONT_SMALL);
        int tx = x + (cw - tw) / 2;
        gfx_text(c, tx + 1, y + 49, label, RGB(0, 0, 0), TRANSPARENT, FONT_SMALL);
        gfx_text(c, tx, y + 48, label, RGB(235, 235, 250), TRANSPARENT, FONT_SMALL);
        if (track) deep_mark_end(&mark, c, deep);
    }
}

/* The desktop's own art, at the display's depth. */
static void draw_wall(void) {
    desktop_draw_wallpaper(&wall, wall_deep);
    draw_desktop_icons(&wall, wall_deep);
}

/* Deep planes exist only for a display with 10 or more bits per channel. */
static bool alloc_deep(int w, int h, uint8_t **bd, uint8_t **wd) {
    *bd = *wd = NULL;
    if (!fb.deep) return true;
    size_t pages = ALIGN_UP((uint64_t)w * h, PAGE_SIZE) / PAGE_SIZE;
    *bd = vmalloc(pages);
    *wd = vmalloc(pages);
    if (*bd && *wd) {
        memset(*bd, 0, (size_t)w * h);
        memset(*wd, 0, (size_t)w * h);
        return true;
    }
    if (*bd) vfree(*bd, pages);
    if (*wd) vfree(*wd, pages);
    *bd = *wd = NULL;
    return false;
}
static void free_deep(int w, int h) {
    size_t pages = ALIGN_UP((uint64_t)w * h, PAGE_SIZE) / PAGE_SIZE;
    if (back_deep) vfree(back_deep, pages);
    if (wall_deep) vfree(wall_deep, pages);
    back_deep = wall_deep = NULL;
}

/* Smooth gradients (the 10 bpc simulation) on or off: the user switched them, or the scanout shown
   took or stopped taking them. Only from the compositor thread between frames; windows keep their
   own planes, so only the desktop's change. */
static void set_deep_simulation(bool on) {
    if (!fb_set_simulated(on)) return; /* the scanout changed meanwhile: the next frame looks again */
    free_deep(sw, sh);
    if (!alloc_deep(sw, sh, &back_deep, &wall_deep)) {
        kprintf("wm: no memory for deep colour, using 8 bpc\n");
        fb_set_simulated(false);
        deep_wanted = false;
    }
    kprintf("wm: %u bpc desktop%s\n", fb.bpc, fb.simulated ? ", dithered to the 8 bpc display" : "");
    draw_wall();
    damage_all();
    wm_screen_info_changed(); /* the colour format, and smooth gradients wanted when there was no memory */
}

void wm_screen_info_changed(void) {
    if (screen_watchable) watched_changed(&screen_watched);
}

/* ---- the desktop's settings, kept in Desktop.conf ---- */

static bool desktop_line(const char *key, const char *value, bool apply);
static size_t desktop_text(char *out, size_t cap);
static struct keep desktop_keep = {.name = "Desktop", .line = desktop_line, .text = desktop_text};

/* The pointer's speed (-2 to 2) and smooth gradients wanted, wm_lock held; whoever changes them
   (a program, the document read back), the watches hear of it and the change is kept. */
static void desktop_settings_put(int speed, bool smooth) {
    if (speed == pointer_speed && smooth == deep_wanted) return;
    if (speed != pointer_speed) pointer_rest_x = pointer_rest_y = 0;
    pointer_speed = speed;
    deep_wanted = smooth;
    wake_compositor();
    keep_changed(&desktop_keep);
    wm_screen_info_changed();
}

/* Desktop.conf: pointer_speed=<-2..2>, smooth_gradients=on|off. deep_colour is what an older
   system called smooth gradients; pointer_y and refresh it kept too, and this one has them no
   more. */
static bool desktop_line(const char *key, const char *value, bool apply) {
    int64_t v;
    bool on = !strcmp(value, "on"), off = !strcmp(value, "off");
    if (!strcmp(key, "pointer_speed")) {
        if (apply && keep_number(value, -2, 2, &v)) {
            wm_lock();
            desktop_settings_put((int)v, deep_wanted);
            wm_unlock();
        }
        return true;
    }
    if (!strcmp(key, "smooth_gradients") || !strcmp(key, "deep_colour")) {
        if (apply && (on || off)) {
            wm_lock();
            desktop_settings_put(pointer_speed, on);
            wm_unlock();
        }
        return true;
    }
    return !strcmp(key, "pointer_y") || !strcmp(key, "refresh");
}

static size_t desktop_text(char *out, size_t cap) {
    wm_lock();
    int speed = pointer_speed;
    bool smooth = deep_wanted;
    wm_unlock();
    size_t n = 0;
    keep_put(out, cap, &n, "# The desktop: the pointer's speed and smooth gradients. Nocturne keeps this: change them in Settings.\n");
    keep_put(out, cap, &n, "pointer_speed=%d\nsmooth_gradients=%s\n", speed, smooth ? "on" : "off");
    return n;
}

static int launch(const char *path, const char *arg) {
    if (!strcmp(path, "@reboot") || !strcmp(path, "@poweroff") || !strcmp(path, "@sleep") || !strcmp(path, "@hibernate")) {
        pm_request(path[1] == 'r' ? PM_REBOOT : path[1] == 's' ? PM_SLEEP : path[1] == 'h' ? PM_HIBERNATE : PM_POWER_OFF, true);
        return 0;
    }
    if (pm_stopping()) return -N_ERR_STOPPED;
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
    int cols = fit < 1 ? menu_count() : (menu_count() + fit - 1) / fit;
    return (menu_count() + cols - 1) / cols;
}

static struct rect menu_rect(void) {
    int rows = menu_rows(), cols = (menu_count() + rows - 1) / rows;
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
    return i < menu_count() ? i : -1;
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
    if (!canvas_hits(c, m.x-SHADOW, m.y-SHADOW, m.w+2*SHADOW, m.h+2*SHADOW)) return;
    draw_shadow(c, m.x, m.y, m.w, m.h);
    gfx_fill_round(c, m.x, m.y, m.w, m.h, 10, RGB(70, 60, 120));
    gfx_fill_round(c, m.x + 1, m.y + 1, m.w - 2, m.h - 2, 9, RGB(22, 20, 44));
    /* header */
    if (canvas_hits(c, m.x, m.y, m.w, MENU_HEAD)) {
        for (int j = 0; j < MENU_HEAD - 6; j++) {
            int in = corner_inset(j, 9);
            gfx_hline(c, m.x + 1 + in, m.y + 1 + j, m.w - 2 - 2 * in,
                      gfx_mix(RGB(64, 44, 130), RGB(30, 26, 70), j * 255 / (MENU_HEAD - 6)));
        }
        draw_crescent(c, m.x + 30, m.y + 26, 13, RGB(246, 236, 196));
        gfx_text(c, m.x + 54, m.y + 10, OS_NAME, RGB(255, 255, 255), TRANSPARENT, FONT_LARGE);
    }
    for (int i = 0, n = menu_count(); i < n; i++) {
        int ix, iy;
        menu_item_pos(i, &ix, &iy);
        /* Include the separator row above this item. */
        if (!canvas_hits(c, ix, iy-1, MENU_W, MENU_ITEM_H+1)) continue;
        if (i == APP_SLEEP && iy > m.y + MENU_HEAD) gfx_hline(c, ix + 12, iy - 1, MENU_W - 24, RGB(60, 56, 100));
        if (i == menu_sel) gfx_fill_round(c, ix + 6, iy + 1, MENU_W - 12, MENU_ITEM_H - 2, 6, RGB(76, 64, 150));
        desktop_draw_icon(c, ix + 12, iy + 2, menu_app(i)->icon);
        gfx_text(c, ix + 54, iy + 10, menu_app(i)->name, RGB(230, 230, 245), TRANSPARENT, FONT_SMALL);
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
    const char *dev = audio_output() ? audio_output() : "No sound device";
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
    if (canvas_hits(c, 0, y, TB_START_W, TASKBAR_H)) {
        uint32_t sb = menu_open ? RGB(84, 68, 170) : RGB(44, 38, 90);
        gfx_fill_round(c, 6, y + 5, TB_START_W - 6, TASKBAR_H - 10, 8, sb);
        draw_crescent(c, 24, y + TASKBAR_H / 2, 9, RGB(246, 236, 196));
        gfx_text(c, 42, y + 12, OS_NAME, RGB(240, 240, 255), TRANSPARENT, FONT_SMALL);
    }

    struct window *list[MAX_WIN];
    int n = taskbar_buttons(list, MAX_WIN);
    struct window *f = focused();
    for (int i = 0; i < n; i++) {
        int bx, bw;
        taskbar_button_geom(n, i, &bx, &bw);
        if (!canvas_hits(c, bx, y, bw, TASKBAR_H)) continue;
        uint32_t col = list[i] == f ? RGB(70, 62, 140) : list[i]->minimized ? RGB(26, 24, 48) : RGB(40, 36, 78);
        gfx_fill_round(c, bx, y + 5, bw, TASKBAR_H - 10, 6, col);
        if (list[i] == f) gfx_hline(c, bx + 8, y + TASKBAR_H - 6, bw - 16, RGB(170, 150, 255));
        text_trunc(c, bx + 10, y + 12, list[i]->title, bw - 20,
                   list[i]->minimized ? RGB(140, 140, 170) : RGB(235, 235, 250));
    }

    if (canvas_hits(c, sw-TB_CLOCK_W-TB_VOL_W, y, TB_VOL_W, TASKBAR_H)) draw_volume_button(c, y);

    /* clock */
    if (!canvas_hits(c, sw-TB_CLOCK_W, y, TB_CLOCK_W, TASKBAR_H)) return;
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
/* A window as a frame draws it: what the compositor captures of it under wm_lock, which it holds
   until the frame's last batch has joined (composite_ram). The pixels a layer points at are a
   window's buffers, kernel mappings that only a resize or the window's end (both under wm_lock)
   replace or free, so they stay while the shares of the batches (cpu_parallel_for, on whichever
   CPUs are idle) read them. Never the event queues or the windows themselves. */
struct draw_layer {
    int x, y, cw, ch, fw, fh, cx, cy, flags, hover;
    bool focus, maximized;
    bool client_raw_overwrite; /* the client area replaces what lies below it (gfx_blit); fixed while drawn */
    const uint32_t *pixels;
    const uint8_t *deep; /* the client's deep plane, or NULL */
    char title[64];
    struct rect bounds;
};

static bool capture_layer(struct draw_layer *l, struct window *w, bool focus) {
    int fw = frame_w(w), fh = frame_h(w);
    /* SYS_WIN_MOVE accepts arbitrary int coordinates. Cull in wide arithmetic
       before any drawing helper can add a shadow/title offset to them. */
    int64_t x0 = (int64_t)w->x - SHADOW, y0 = (int64_t)w->y - SHADOW;
    int64_t x1 = (int64_t)w->x + fw + SHADOW, y1 = (int64_t)w->y + fh + SHADOW;
    if (x1 <= 0 || y1 <= 0 || x0 >= sw || y0 >= sh || !w->buf) return false;
    *l = (struct draw_layer){.x=w->x, .y=w->y, .cw=w->cw, .ch=w->ch,
        .fw=fw, .fh=fh, .cx=client_x(w), .cy=client_y(w), .flags=w->flags|(w->fullscreen?WIN_NO_DECOR:0),
        .hover=hover_win==w ? hover_btn : 0, .focus=focus,
        .maximized=w->maximized,
        .pixels=(w->flags & WIN_DOUBLE_BUFFER) &&
                (w->pending_index >= 0 ? w->pending_index : w->front_index) == 1 ? w->user_buf : w->buf,
        .client_raw_overwrite=true,
        .bounds={MAX(0,x0),MAX(0,y0),MIN(sw,x1)-MAX(0,x0),MIN(sh,y1)-MAX(0,y0)}};
    if (w->deep && back_deep) l->deep = (const uint8_t *)(l->pixels + (size_t)w->cw * w->ch);
    memcpy(l->title, w->title, sizeof l->title);
    l->title[sizeof l->title - 1] = 0;
    return true;
}

/* Omit only pixels proved to be overwritten later in this layer. Rounded
   title corners remain translucent and must retain every original pass. */
static void draw_layer_shadow_candidate(canvas_t *c, const struct draw_layer *w) {
    if (!w->client_raw_overwrite) {
        draw_shadow(c, w->x, w->y, w->fw, w->fh);
        return;
    }
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
            gfx_fill_blend(c, (int)x0, (int)y0, (int)(x1-x0), (int)(y1-y0), col);
            continue;
        }
        if (iy0 > y0)
            gfx_fill_blend(c, (int)x0, (int)y0, (int)(x1-x0), (int)(iy0-y0), col);
        if (y1 > iy1)
            gfx_fill_blend(c, (int)x0, (int)iy1, (int)(x1-x0), (int)(y1-iy1), col);
        if (ix0 > x0)
            gfx_fill_blend(c, (int)x0, (int)iy0, (int)(ix0-x0), (int)(iy1-iy0), col);
        if (x1 > ix1)
            gfx_fill_blend(c, (int)ix1, (int)iy0, (int)(x1-ix1), (int)(iy1-iy0), col);
        if (!(w->flags & WIN_NO_DECOR)) {
            for (int j = 0; j < TITLE_RADIUS; j++) {
                int64_t y = hy0 + j;
                if (y < iy0 || y >= iy1) continue;
                int inset = corner_inset(j, TITLE_RADIUS);
                int64_t left = MIN(ix1, hx0 + inset), right = MAX(ix0, hx1 - inset);
                if (left > ix0) gfx_fill_blend(c, (int)ix0, (int)y, (int)(left-ix0), 1, col);
                if (right < ix1) gfx_fill_blend(c, (int)right, (int)y, (int)(ix1-right), 1, col);
            }
        }
    }
}

/* Pure RAM-only helper: canvas clip is caller-private; layer is immutable. */
static void draw_layer(canvas_t *c, const struct draw_layer *w, bool shadow_strip) {
    int fx = w->x, fy = w->y, fw = w->fw, fh = w->fh;
    bool focus = w->focus;
    if (shadow_strip) draw_layer_shadow_candidate(c, w);
    else draw_shadow(c, fx, fy, fw, fh);
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
        /* title */
        int nb = (w->flags & WIN_RESIZABLE) ? 3 : 2;
        text_trunc(c, fx + 12, fy + 6, w->title, fw - 24 - nb * 24, focus ? RGB(250, 250, 255) : RGB(160, 160, 180));
        /* buttons */
        for (int b = 1; b <= 3; b++) {
            int bx = fx + fw - (b == 1 ? 18 : b == 2 ? 42 :
                     (w->flags & WIN_RESIZABLE) ? 66 : 42);
            if (b == 2 && !(w->flags & WIN_RESIZABLE)) continue;
            if (bx < -500) continue;
            int by = fy + TITLE_H / 2;
            uint32_t col = !focus ? RGB(84, 84, 108)
                         : b == 1 ? RGB(236, 92, 108)
                         : b == 2 ? RGB(96, 200, 130)
                                  : RGB(240, 190, 80);
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
    gfx_init(&wc, (uint32_t *)w->pixels, w->cw, w->ch, w->cw);
    gfx_blit(c, w->cx, w->cy, &wc, 0, 0, w->cw, w->ch);
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
struct draw_scene {
    canvas_t dst, wallpaper;
    uint8_t *dst_deep;            /* deep planes of dst/wallpaper (pitch = dst pitch), or NULL */
    const uint8_t *wallpaper_deep;
    struct draw_layer layers[MAX_WIN];
    unsigned count;
    struct rect selection;
    bool selected, shadow_strip, occlusion;
};

/* Deep-plane bookkeeping inside the canvas clip. 8-bit drawing marks its
   area as 8-bit content; a deep source copies its low bits along with its
   pixels. Shadows and other blends over deep pixels are 8-bit results. */
static void deep_clear(uint8_t *deep, const canvas_t *c, int64_t x, int64_t y, int64_t w, int64_t h) {
    int64_t x0 = MAX(x, (int64_t)c->cx0), y0 = MAX(y, (int64_t)c->cy0);
    int64_t x1 = MIN(x + w, (int64_t)c->cx1), y1 = MIN(y + h, (int64_t)c->cy1);
    if (x1 <= x0) return;
    for (int64_t row = y0; row < y1; row++) memset(deep + row * c->pitch + x0, 0, (size_t)(x1 - x0));
}
static void deep_copy(uint8_t *deep, const canvas_t *c, int64_t x, int64_t y,
                      const uint8_t *src, int src_pitch, int w, int h) {
    int64_t x0 = MAX(x, (int64_t)c->cx0), y0 = MAX(y, (int64_t)c->cy0);
    int64_t x1 = MIN(x + w, (int64_t)c->cx1), y1 = MIN(y + h, (int64_t)c->cy1);
    if (x1 <= x0) return;
    for (int64_t row = y0; row < y1; row++)
        memcpy(deep + row * c->pitch + x0, src + (row - y) * src_pitch + (x0 - x), (size_t)(x1 - x0));
}

struct scene_job {
    const struct draw_scene *scene;
    struct rect batch;
    unsigned cell_count;
    unsigned cpu[CPU_MAX_COUNT]; /* one independent cell per tile */
};

static bool wm_parallel, wm_parallel_force, wm_verify, wm_yield, wm_shadow_strip, wm_occlusion;
static bool wm_trace, trace_done, trace_chord;
static bool trace_active; /* the compositor changes it under in_lock; the sinks read it under it */
static canvas_t verify_back;
/* Keep width*layers*rows work budgets, not stale row ceilings. This preserves
   useful tiles even after a 64-layer scene returns to 17 or a single layer. */
static uint64_t wm_work_budget[4] = {512ULL*1280,512ULL*1280,512ULL*1280,512ULL*1280};
static uint64_t scene_frames, scene_jobs, scene_max_ticks;

static uint64_t scene_tick(void) {
    __asm__ volatile("lfence" : : : "memory");
    return rdtsc();
}

static uint64_t scene_us(uint64_t ticks) {
    return tsc_hz >= 1000000 ? ticks / (tsc_hz / 1000000) : 0;
}

static unsigned scene_density(unsigned layers) {
    return layers<=1 ? 0 : layers<=4 ? 1 : layers<=16 ? 2 : 3;
}

/* Every CPU may take shares of a batch while it is idle (cpu_parallel_for). */
static unsigned scene_cpu_count(void) { return cpu_online_count(); }

static unsigned scene_rows(unsigned layers, unsigned width) {
    unsigned cpus=scene_cpu_count();
    uint64_t row_cost=(uint64_t)width*MAX(layers,4u); /* clipped positive screen width */
    unsigned cost_rows=wm_work_budget[scene_density(layers)]/row_cost;
    return MAX(cpus,MIN(128u,MAX(cpus,cost_rows)));
}

static void scene_shrink(unsigned layers,unsigned width,unsigned rows,uint64_t maximum) {
    if (scene_us(maximum)>1000 && rows>scene_cpu_count())
        wm_work_budget[scene_density(layers)]=MIN(wm_work_budget[scene_density(layers)],
            (uint64_t)MAX(scene_cpu_count(),rows/2)*width*MAX(layers,4u));
}

/* A client is raw-replaced by gfx_blit, even when its stored alpha is zero.
 * Only complete coverage of this exact clip can hide all lower drawing.
 * A partial rectangle, frame, rounded title or shadow is never an occluder.
 * This reads the captured scene, not the windows themselves. */
static unsigned scene_first_layer(const struct draw_scene *s, struct rect r, bool *covered) {
    *covered=false;
    if (!s->occlusion) return 0;
    int64_t right=(int64_t)r.x+r.w, bottom=(int64_t)r.y+r.h;
    for (unsigned at=s->count; at; at--) {
        const struct draw_layer *l=&s->layers[at-1];
        if (l->client_raw_overwrite && r.x>=l->cx && r.y>=l->cy &&
            right<=(int64_t)l->cx+l->cw && bottom<=(int64_t)l->cy+l->ch) {
            *covered=true;
            return at-1;
        }
    }
    return 0;
}

/* wm_lock held (or wm_init, before the compositor starts). */
static void capture_scene(struct draw_scene *s, canvas_t dst) {
    s->dst = dst;
    s->wallpaper = wall;
    s->dst_deep = back_deep;
    s->wallpaper_deep = wall_deep;
    s->count = 0;
    s->selected = sel_icon >= 0;
    s->shadow_strip = wm_shadow_strip; /* chosen before the batches, fixed until they join */
    s->occlusion = wm_occlusion;
    if (s->selected) {
        int x, y;
        icon_cell(sel_icon, &x, &y);
        s->selection = (struct rect){x,y,ICON_CELL_W-8,ICON_CELL_H-6};
    }
    struct window *f = focused();
    for (int i = 0; i < nwin; i++)
        if (visible(zorder[i]) && capture_layer(&s->layers[s->count], zorder[i], zorder[i]==f)) s->count++;
}

/* One share of a batch (cpu_parallel_for): it reads only the scene and writes only its own rows. */
static void scene_tile(size_t index, void *arg) {
    struct scene_job *job = arg;
    const struct draw_scene *s = job->scene;
    struct rect r = job->batch;
    /* Proportional boundaries cover every row once, including 5/6/9-row
       batches which ceil-sized cells previously reduced below online CPUs. */
    unsigned begin=(uint64_t)index*(unsigned)r.h/job->cell_count;
    unsigned end=(uint64_t)(index+1)*(unsigned)r.h/job->cell_count;
    r.y += (int)begin;
    r.h = (int)(end-begin);
    if (r.h <= 0) return;
    canvas_t c = s->dst; /* a share never changes the scene's clip */
    gfx_noclip(&c);
    gfx_clip(&c, r.x, r.y, r.w, r.h);
    bool covered=false;
    unsigned first=scene_first_layer(s,r,&covered);
    uint8_t *deep = s->dst_deep;
    if (!covered) {
        gfx_blit(&c, r.x, r.y, &s->wallpaper, r.x, r.y, r.w, r.h);
        if (deep) deep_copy(deep, &c, r.x, r.y, s->wallpaper_deep + (size_t)r.y * c.pitch + r.x, c.pitch, r.w, r.h);
    }
    if (s->selected && !covered) {
        struct rect a = s->selection;
        gfx_fill_blend(&c, a.x, a.y, a.w, a.h, ARGB(70,150,140,255));
        if (deep) deep_clear(deep, &c, a.x, a.y, a.w, a.h);
    }
    for (unsigned i = first; i < s->count; i++) {
        const struct draw_layer *l = &s->layers[i];
        if (!rect_overlap(&l->bounds, &r)) continue;
        draw_layer(&c, l, s->shadow_strip);
        if (!deep) continue;
        deep_clear(deep, &c, l->bounds.x, l->bounds.y, l->bounds.w, l->bounds.h);
        if (l->deep) deep_copy(deep, &c, l->cx, l->cy, l->deep, l->cw, l->cw, l->ch);
    }
    job->cpu[index] = cpu_current_index();
}

/* Both modes run the same callback over the same tiles, scene and row limits. Interrupts come in
   between batches; what the scene points at stays, since its caller holds wm_lock throughout. */
static uint64_t render_scene(const struct draw_scene *s, struct rect r, bool parallel,
                             unsigned batch_rows, uint64_t *cpu_mask, uint64_t *maximum) {
    uint64_t ticks = 0;
    unsigned cpus = scene_cpu_count();
    for (int y = r.y; y < r.y+r.h; y += (int)batch_rows) {
        struct scene_job job = {.scene=s, .batch={r.x,y,r.w,MIN((int)batch_rows,r.y+r.h-y)}};
        size_t count = job.cell_count = MIN(cpus,(unsigned)job.batch.h);
        uint64_t start = scene_tick();
        if (parallel) cpu_parallel_for(count, scene_tile, &job);
        else for (size_t i = 0; i < count; i++) scene_tile(i, &job);
        uint64_t elapsed = scene_tick()-start;
        ticks += elapsed;
        if (maximum) *maximum = MAX(*maximum, elapsed);
        if (cpu_mask) for (size_t i=0;i<count;i++) *cpu_mask |= 1ULL << job.cpu[i];
    }
    return ticks;
}

static void render_scene_direct(const struct draw_scene *s, struct rect r) {
    struct scene_job job={.scene=s,.batch=r,.cell_count=1};
    scene_tile(0,&job);
}

/* The reference picture: the scene in one tile on this CPU, with the original shadows and no
   occlusion; the scene's own choices are put back after it. */
static void render_scene_original(struct draw_scene *s, struct rect r) {
    bool saved=s->shadow_strip, saved_occlusion=s->occlusion;
    s->shadow_strip=false;
    s->occlusion=false;
    render_scene_direct(s,r);
    s->shadow_strip=saved;
    s->occlusion=saved_occlusion;
}

static void measure_reference(struct draw_scene *s,struct rect r,unsigned rows,
                              uint64_t *direct,uint64_t *serial) {
    canvas_t original=s->dst;
    uint8_t *original_deep=s->dst_deep;
    s->dst_deep=NULL; /* the oracle compares pixels only */
    bool saved=s->shadow_strip, saved_occlusion=s->occlusion;
    s->shadow_strip=false;
    s->occlusion=false;
    s->dst=verify_back;
    uint64_t start=scene_tick();
    render_scene_direct(s,r);
    *direct=scene_tick()-start;
    *serial=render_scene(s,r,false,rows,NULL,NULL);
    s->dst=original;
    s->dst_deep=original_deep;
    s->shadow_strip=saved;
    s->occlusion=saved_occlusion;
}

static void scene_benchmark(void) {
    if (!cmdline_has("wmbench")) return;
    size_t pixels=(size_t)sw*sh, pages=ALIGN_UP((pixels+32)*4,PAGE_SIZE)/PAGE_SIZE;
    uint32_t *ref=vmalloc(pages), *out=vmalloc(pages);
    if (!ref || !out) {
        if (ref) vfree(ref,pages);
        if (out) vfree(out,pages);
        panic("wmbench: cannot allocate comparison buffers");
    }
    struct draw_scene s;
    capture_scene(&s,back);
    s.dst_deep=NULL; /* pixel comparison buffers only */
    struct window fixture={.cw=16,.ch=16,.buf=wall.px};
    int extrema[]={INT32_MIN,INT32_MAX};
    for (unsigned i=0;i<ARRAY_SIZE(extrema);i++) {
        fixture.x=extrema[i];fixture.y=0;
        if (capture_layer(&s.layers[0],&fixture,false)) panic("wmbench: extreme x was not culled");
        fixture.x=0;fixture.y=extrema[i];
        if (capture_layer(&s.layers[0],&fixture,false)) panic("wmbench: extreme y was not culled");
    }
    s.selected=true;
    s.selection=(struct rect){17,19,73,91};
    static const unsigned layers[]={0,1,4,16,64,17,1};
    for (unsigned test=0;test<ARRAY_SIZE(layers);test++) {
        s.count=0;
        for (unsigned i=0;i<layers[test];i++) {
            fixture=(struct window){.x=(int)(i%5)*101-73,.y=(int)(i%7)*59-29,
                .cw=MIN(sw,820),.ch=MIN(sh,530),.buf=wall.px,
                .flags=(i%3==0 ? WIN_NO_DECOR : WIN_RESIZABLE),.maximized=i%4==0};
            strcpy(fixture.title,"RAM scene / alpha / title / clipped tile");
            if (capture_layer(&s.layers[s.count],&fixture,i==layers[test]-1)) s.count++;
        }
        unsigned rows=scene_rows(s.count,(unsigned)sw);
        struct rect r={0,0,sw,sh};
        uint64_t serial=0,parallel=0,direct=0,mask=0,maximum=0,begin_jobs=cpu_parallel_jobs();
        for (unsigned round=0;round<6;round++) {
            for (size_t p=0;p<pixels+32;p++) ref[p]=out[p]=0x5a5aa5a5;
            s.dst=(canvas_t){.px=ref+16,.w=sw,.h=sh,.pitch=sw};
            uint64_t start=scene_tick();
            render_scene_original(&s,r);
            direct+=scene_tick()-start;
            s.dst.px=out+16;
            render_scene(&s,r,false,rows,NULL,NULL);
            if (memcmp(ref,out,(pixels+32)*4)) panic("wmbench: direct/tiled mismatch layers=%u",s.count);
            /* Alternate destination/order to avoid always timing a cold serial buffer. */
            if (round&1) {
                s.dst.px=ref+16;
                parallel+=render_scene(&s,r,true,rows,&mask,&maximum);
                s.dst.px=out+16;
                serial+=render_scene(&s,r,false,rows,NULL,NULL);
            } else {
                s.dst.px=ref+16;
                serial+=render_scene(&s,r,false,rows,NULL,NULL);
                s.dst.px=out+16;
                parallel+=render_scene(&s,r,true,rows,&mask,&maximum);
            }
            if (memcmp(ref,out,(pixels+32)*4)) panic("wmbench: parallel pixels/guards differ layers=%u",s.count);
            /* Untimed: the buffer the parallel pass did not draw is drawn again with the original
               shadows, so an error both shortcut passes share still shows. */
            s.dst.px=(round&1)?out+16:ref+16;
            render_scene_original(&s,r);
            if (memcmp(ref,out,(pixels+32)*4)) panic("wmbench: original/parallel pixels differ layers=%u",s.count);
            /* Off-origin, odd width/height, last row, and tiny jobs must not write
               outside their damage region or disagree at a tile boundary. */
            struct rect clips[]={{1,3,MIN(sw-1,97),MIN(sh-3,131)},
                                 {sw-33,sh-19,33,19},{7,9,1,1}};
            for (unsigned k=0;k<ARRAY_SIZE(clips);k++) {
                for (size_t p=0;p<pixels+32;p++) ref[p]=out[p]=0x5a5aa5a5;
                s.dst.px=ref+16;render_scene_original(&s,clips[k]);
                s.dst.px=out+16;render_scene(&s,clips[k],true,rows,&mask,&maximum);
                if (memcmp(ref,out,(pixels+32)*4)) panic("wmbench: clipped pixels/guards differ case=%u",k);
            }
        }
        if (s.count==17) {
            const unsigned uneven[]={4,5,6,9};
            struct rect clip={1,3,MIN(sw-1,97),MIN(sh-3,35)};
            uint64_t jobs=cpu_parallel_jobs(),uneven_mask=0;
            for (unsigned k=0;k<ARRAY_SIZE(uneven);k++) {
                for (size_t p=0;p<pixels+32;p++) ref[p]=out[p]=0x5a5aa5a5;
                s.dst.px=ref+16;render_scene_original(&s,clip);
                uint64_t before=cpu_parallel_jobs();
                s.dst.px=out+16;render_scene(&s,clip,true,uneven[k],&uneven_mask,NULL);
                if (memcmp(ref,out,(pixels+32)*4) ||
                    (scene_cpu_count()>1 && uneven[k]>=2 &&
                     cpu_parallel_jobs()==before)) panic("wmbench: uneven batch failed rows=%u",uneven[k]);
            }
            kprintf("wmbench: uneven-batches PASS rows=4,5,6,9 mask=%lx jobs=%lu pixels=PASS guards=PASS\n",
                    uneven_mask,cpu_parallel_jobs()-jobs);
        }
        /* RDP can choose an odd pitch: adjacent non-overlapping rows may share
           a cache line, but neither clip nor guard pixels may be changed. */
        for (size_t p=0;p<pixels+32;p++) ref[p]=out[p]=0x5a5aa5a5;
        s.dst=(canvas_t){.px=ref+16,.w=sw-1,.h=sh,.pitch=sw-1};
        struct rect odd={1,1,sw-2,sh-2};
        render_scene_original(&s,odd);
        s.dst.px=out+16;
        render_scene(&s,odd,true,rows,&mask,&maximum);
        if (memcmp(ref,out,(pixels+32)*4)) panic("wmbench: odd-pitch pixels/guards differ");
        kprintf("wmbench: %dx%d layers=%u rounds=6 CPUs=%u mask=%lx direct_ticks=%lu serial_ticks=%lu parallel_ticks=%lu jobs=%lu batch_rows=%u max_join_us=%lu pixels=PASS guards=PASS direct_shadow=original serial_shadow=%s parallel_shadow=%s oracle_shadow=original\n",
                sw,sh,s.count,scene_cpu_count(),mask,direct,serial,parallel,
                cpu_parallel_jobs()-begin_jobs,rows,scene_us(maximum),
                s.shadow_strip?"strip":"original",s.shadow_strip?"strip":"original");
        scene_shrink(s.count,(unsigned)sw,rows,maximum);
    }
    vfree(ref,pages);vfree(out,pages);
    kprintf("wmbench: PASS full/clipped/tiny/odd-pitch/extrema/guards\n");
}

static void composite_ram(struct rect r) {
    struct draw_scene scene;
    capture_scene(&scene, back);
    unsigned relevant=0;
    for (unsigned i=0;i<scene.count;i++) if (rect_overlap(&scene.layers[i].bounds,&r))
        scene.layers[relevant++]=scene.layers[i];
    scene.count=relevant;
    bool covered=false;
    unsigned active_layers=scene.count-scene_first_layer(&scene,r,&covered);
    bool verify=wm_verify && verify_back.w==back.w && verify_back.h==back.h;
    /* In shares by default only for many layers over a large damage; forced (wmparallel), still
       only where the same conditions hold. */
    bool parallel = wm_parallel && scene_cpu_count()>1 && r.h>=2 &&
                    (uint64_t)r.w*r.h>=65536 && scene.count>0 &&
                    (wm_parallel_force || (active_layers>=16 && (uint64_t)r.w*r.h>=262144));
    if (!parallel) {
        render_scene_direct(&scene,r);
        if (verify && scene.count) {
            scene.dst=verify_back;
            scene.dst_deep=NULL;
            render_scene_original(&scene,r);
            for (int y=r.y;y<r.y+r.h;y++)
                if (memcmp(back.px+(size_t)y*back.pitch+r.x,
                           verify_back.px+(size_t)y*verify_back.pitch+r.x,(size_t)r.w*4))
                    panic("wmverify: serial fallback pixels differ row=%d",y);
            kprintf("wmram: fallback CPUs=%u layers=%u rect=%dx%d pixels=PASS shadow=%s oracle_shadow=original\n",
                    scene_cpu_count(),scene.count,r.w,r.h,scene.shadow_strip?"strip":"original");
        }
        return;
    }
    /* Many overlapping alpha shadows need shorter joins, not a fixed giant job. */
    unsigned rows=scene_rows(active_layers,(unsigned)r.w);
    uint64_t start_jobs=cpu_parallel_jobs(), maximum=0, mask=0;
    uint64_t direct=0,serial=0;
    bool parallel_first=verify && (scene_frames&1);
    if (verify && !parallel_first) measure_reference(&scene,r,rows,&direct,&serial);
    uint64_t ticks=render_scene(&scene,r,true,rows,&mask,&maximum);
    if (parallel_first) measure_reference(&scene,r,rows,&direct,&serial);
    if (verify) {
        for (int y=r.y;y<r.y+r.h;y++)
            if (memcmp(back.px+(size_t)y*back.pitch+r.x,
                       verify_back.px+(size_t)y*verify_back.pitch+r.x,(size_t)r.w*4))
                panic("wmverify: live snapshot pixels differ row=%d",y);
    }
    scene_frames++;
    scene_jobs+=cpu_parallel_jobs()-start_jobs;
    scene_max_ticks=MAX(scene_max_ticks,maximum);
    scene_shrink(active_layers,(unsigned)r.w,rows,maximum);
    if (!trace_active && (verify || scene_frames==1 || !(scene_frames%64)))
        kprintf("wmram: frame=%lu rect=%dx%d layers=%u CPUs=%u mask=%lx direct_ticks=%lu serial_ticks=%lu parallel_ticks=%lu jobs=%lu density=%u batch_rows=%u join_us=%lu max_join_us=%lu pixels=%s reference_timed=%u direct_shadow=original serial_shadow=original parallel_shadow=%s\n",
                scene_frames,r.w,r.h,scene.count,scene_cpu_count(),mask,direct,serial,ticks,
                scene_jobs,scene_density(scene.count),rows,scene_us(maximum),scene_us(scene_max_ticks),verify?"PASS":"unchecked",(unsigned)verify,scene.shadow_strip?"strip":"original");
}

/* The compositor, wm_lock held. */
static void composite_rect(struct rect r) {
    composite_ram(r);
    canvas_t *c = &back;
    gfx_noclip(c);
    gfx_clip(c, r.x, r.y, r.w, r.h);
    if (resize_visible) {
        struct rect o = resize_rect;
        gfx_rect(c, o.x, o.y, o.w, o.h, RGB(180, 160, 255));
        gfx_rect(c, o.x + 1, o.y + 1, o.w - 2, o.h - 2, RGB(90, 70, 200));
    }
    bool taskbar = r.y + r.h > sh - TASKBAR_H&&(!focused()||!focused()->fullscreen||menu_open||vol_open);
    if (taskbar) draw_taskbar(c);
    if (menu_open) draw_menu(c);
    if (vol_open) draw_volume_panel(c);
    if (toast_until) draw_toast(c);
    /* a remote viewer draws the pointer itself; keep it out of the picture it is sent */
    if (!remote_users) {
        static uint32_t under[POINTER_SIZE * POINTER_SIZE];
        struct deep_mark mark;
        int px = mx - pointer_hx, py = my - pointer_hy;
        bool track = back_deep && deep_mark_begin(&mark, c, px, py, POINTER_SIZE, POINTER_SIZE, under, POINTER_SIZE);
        canvas_t image;
        gfx_init(&image, pointer_px, POINTER_SIZE, POINTER_SIZE, POINTER_SIZE);
        gfx_blit_alpha(c, px, py, &image, 0, 0, POINTER_SIZE, POINTER_SIZE);
        if (track) deep_mark_end(&mark, c, back_deep);
    }
    if (back_deep) { /* the shell's own drawing above is 8-bit content */
        if (resize_visible) deep_clear(back_deep, c, resize_rect.x, resize_rect.y, resize_rect.w, resize_rect.h);
        if (taskbar) deep_clear(back_deep, c, 0, sh - TASKBAR_H, sw, TASKBAR_H);
        if (menu_open) {
            struct rect m = menu_rect();
            deep_clear(back_deep, c, m.x - SHADOW, m.y - SHADOW, m.w + 2 * SHADOW, m.h + 2 * SHADOW);
        }
        if (vol_open) {
            struct rect v = vol_rect();
            deep_clear(back_deep, c, v.x - SHADOW, v.y - SHADOW, v.w + 2 * SHADOW, v.h + 2 * SHADOW);
        }
        if (toast_until) {
            struct rect t = toast_rect();
            deep_clear(back_deep, c, t.x, t.y, t.w, t.h);
        }
    }
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
    int32_t period = (int32_t)frame_period(); /* EV_FRAME's y: when the next frame comes */
    /* Includes hidden/offscreen windows: no outstanding reads exist for those
       either. The new front stays immutable until the NEXT frame completes,
       since expose/move damage can reread it between client submissions. */
    for (int i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (w->frame_wanted && visible(w)) { /* WIN_FRAME_EVENTS: this frame took its presents */
            w->frame_wanted = false;
            push_event(w, &(struct gui_event){.type = EV_FRAME, .y = period});
        }
        if (w->pending_index < 0) continue;
        int front = w->pending_index;
        buffers_set(w, front, -1);
        pending_frames--;
        /* its wake ends a SYS_WIN_MAP wait for the buffer that was in front */
        struct gui_event event = {.type=EV_FRAME, .key=front, .x=front ^ 1, .y=period};
        push_event(w, &event);
    }
    if (n) {
        /* Raster jobs and all window reads have already joined. Only the
           compositor writes this RAM canvas; a resize of the desktop waits for this borrow
           (set_screen_size). Window calls on other CPUs may now destroy, resize or present
           windows without sleeping behind the write-combined VRAM transfer. No window pixel pin is
           needed because fb_present reads back, never a window surface. */
        canvas_t frame = back;
        uint8_t *deep = back_deep;
        presenting_back = true;
        wm_unlock();
        for (int i = 0; i < n; i++)
            fb_present(frame.px, frame.pitch, deep, frame.pitch, list[i].x, list[i].y, list[i].w, list[i].h);
        wm_lock();
        presenting_back = false;
        __atomic_store_n(&back_presents, back_presents + 1, __ATOMIC_SEQ_CST);
        wq_wake_all(&present_wait);
    }
}

static void damage_cursor(void) { damage_rect(mx - pointer_hx, my - pointer_hy, POINTER_SIZE, POINTER_SIZE); }

/* ---- input handling (compositor thread) ----
   The input ring: the input drivers' sinks (key_sink, mouse_sink: any CPU, interrupt handlers
   included, dev/input.c's lock held) put events in at in_head, the compositor takes them at
   in_tail, both under in_lock. in_head is read without it to see whether input waits. */
#define INQ 256
struct in_ev {
    bool is_key;
    struct key_event k;
    struct mouse_event m;
    uint64_t stamp, seq; /* wmtrace's alone: never in an app's event */
};
static struct spinlock in_lock;
static struct in_ev inq[INQ];
static int in_head, in_tail;

#define TRACE_FRAMES 256
struct trace_frame {
    uint64_t begin, end, oldest, seq, jobs, area;
    uint32_t rects, inputs, waited, reserved;
};
static struct trace_frame trace_frames[TRACE_FRAMES];
static struct {
    uint64_t start, deadline, frames, empty, waits, wait_ticks, max_wait;
    uint64_t nowait, streak, max_streak, streak_start, max_run, max_frame, max_proxy, max_dequeue;
    uint64_t dequeue_hist[16], frame_hist[16], raw_lost, yield_calls;
    uint64_t received, dropped, skipped; /* the sinks', under in_lock */
    unsigned depth;                      /* the sinks', under in_lock */
    unsigned records;
} wt;
_Static_assert(sizeof(trace_frames)+sizeof(wt)+INQ*16 <= 24*1024,"wmtrace's memory");

static void trace_hist(uint64_t *bins, uint64_t ticks) {
    uint64_t us=scene_us(ticks); unsigned b=0;
    while (us>1 && b<15) { us>>=1; b++; }
    bins[b]++;
}
/* in_lock held (a sink). */
static void trace_stamp(struct in_ev *ev) {
    ev->stamp=ev->seq=0;
    if (!trace_active) return;
    if (ev->is_key && (ev->k.key==KEY_F10 || ev->k.key==KEY_LCTRL || ev->k.key==KEY_RCTRL ||
                      ev->k.key==KEY_LALT || ev->k.key==KEY_RALT)) { wt.skipped++; return; }
    ev->stamp=rdtsc(); ev->seq=++wt.received;
    wt.depth=MAX(wt.depth,(unsigned)((in_head-in_tail+INQ)%INQ+1));
}
static void trace_begin(void) {
    if (trace_done || trace_active) return;
    if (wm_verify || !tsc_hz || tsc_hz>UINT64_MAX/20) {
        trace_done=true; kprintf("wmtrace: REJECT wmverify or unavailable TSC\n"); return;
    }
    memset(&wt,0,sizeof wt);
    kprintf("wmtrace: START seconds=20 CPUs=%u parallel=%u yield=%u proxy=WM-loop-not-app-paint\n",scene_cpu_count(),(unsigned)wm_parallel,(unsigned)wm_yield);
    audio_trace_begin();
    uint64_t f=spin_lock(&in_lock); wt.start=rdtsc(); wt.deadline=wt.start+20*tsc_hz;
    trace_active=true; spin_unlock(&in_lock,f);
}
static void trace_finish(uint64_t end) {
    /* no sink counts in wt once this is done */
    uint64_t f=spin_lock(&in_lock); trace_active=false; trace_done=true; spin_unlock(&in_lock,f);
    audio_trace_end();
    kprintf("wmtrace: DONE elapsed_us=%lu overshoot_us=%lu received=%lu dropped=%lu excluded=%lu depth=%u frames=%lu empty=%lu waits=%lu wait_us=%lu max_wait_us=%lu nowait=%lu max_streak=%lu max_run_us=%lu max_frame_us=%lu max_dequeue_us=%lu max_proxy_us=%lu records=%u raw_lost=%lu yield_calls=%lu run_proxy=loop-no-wait-including-yield\n",
            scene_us(end-wt.start),scene_us(end-wt.deadline),wt.received,wt.dropped,wt.skipped,wt.depth,
            wt.frames,wt.empty,wt.waits,scene_us(wt.wait_ticks),scene_us(wt.max_wait),wt.nowait,wt.max_streak,
            scene_us(wt.max_run),scene_us(wt.max_frame),scene_us(wt.max_dequeue),scene_us(wt.max_proxy),wt.records,wt.raw_lost,wt.yield_calls);
    for (unsigned b=0;b<16;b++) kprintf("wmtrace: hist bin=%u dequeue=%lu frame=%lu unit=log2-us-last-saturated\n",b,wt.dequeue_hist[b],wt.frame_hist[b]);
    for (unsigned i=0;i<wt.records;i++) {
        struct trace_frame *r=&trace_frames[i];
        kprintf("wmtrace: raw frame=%u begin_ticks=%lu end_ticks=%lu oldest_ticks=%lu seq=%lu inputs=%u rects=%u area=%lu waited=%u jobs=%lu\n",
                i,r->begin-wt.start,r->end-wt.start,r->oldest?r->oldest-wt.start:0,r->seq,r->inputs,r->rects,r->area,r->waited,r->jobs);
    }
}
static void trace_frame_end(struct trace_frame *r) {
    if (!trace_active) return;
    if (!r->begin) r->begin=wt.start; /* the pass that took the starting chord */
    r->end=rdtsc(); r->jobs=cpu_parallel_jobs()-r->jobs;
    if (r->waited) { wt.streak=0; wt.streak_start=0; }
    else { wt.nowait++; if (!wt.streak) wt.streak_start=r->begin; wt.streak++;
        wt.max_streak=MAX(wt.max_streak,wt.streak); wt.max_run=MAX(wt.max_run,r->end-wt.streak_start); }
    if (r->rects) {
        wt.frames++; wt.max_frame=MAX(wt.max_frame,r->end-r->begin); trace_hist(wt.frame_hist,r->end-r->begin);
        if (r->oldest) wt.max_proxy=MAX(wt.max_proxy,r->end-r->oldest);
        if (wt.records<TRACE_FRAMES) trace_frames[wt.records++]=*r; else wt.raw_lost++;
    } else wt.empty++;
    if (r->end>=wt.deadline) trace_finish(r->end);
}

/* An input event into the ring (dropped when it is full); the compositor is woken after it. */
static void input_put(bool is_key, const struct key_event *k, const struct mouse_event *m) {
    uint64_t f = spin_lock(&in_lock);
    int nh = (in_head + 1) % INQ;
    bool full = nh == in_tail;
    if (full) {
        if (trace_active) wt.dropped++;
    } else {
        struct in_ev *ev = &inq[in_head];
        ev->is_key = is_key;
        if (is_key) ev->k = *k;
        else ev->m = *m;
        trace_stamp(ev);
        __atomic_store_n(&in_head, nh, __ATOMIC_RELEASE);
    }
    spin_unlock(&in_lock, f);
    if (!full) wake_compositor();
}

static void key_sink(const struct key_event *e) { input_put(true, e, NULL); }
static void mouse_sink(const struct mouse_event *e) { input_put(false, NULL, e); }

/* The compositor: the oldest input event, false when there is none. */
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

/* Whether input waits for the compositor (in_tail is its own). */
static bool input_waiting(void) { return __atomic_load_n(&in_head, __ATOMIC_ACQUIRE) != in_tail; }

static void toggle_maximize(struct window *w) {
    if(w->fullscreen)return;
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
    if (wm_trace && k->key==KEY_F10 && ((k->pressed && (k->mods&(MOD_CTRL|MOD_ALT))==(MOD_CTRL|MOD_ALT)) || trace_chord)) {
        trace_chord=k->pressed;
        if (trace_chord) trace_begin();
        return;
    }
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
            launch(SYSTEM "term", NULL);
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
                int n = menu_count();
                menu_sel = menu_sel < 0 ? (d > 0 ? 0 : n - 1) : (menu_sel + d + n) % n;
                struct rect m = menu_rect();
                damage_rect(m.x, m.y, m.w, m.h);
            } else if (k->key == KEY_ENTER && menu_sel >= 0) {
                int s = menu_sel;
                set_menu(false);
                launch(menu_app(s)->path, menu_app(s)->arg);
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
        launch(apps[desktop_apps[sel_icon]].path, apps[desktop_apps[sel_icon]].arg);
    }
}

/* What the pointer is over decides what it shows: the WM's own shapes over what the WM draws (a
   menu, the volume panel, a toast, the taskbar, a frame, the resize band) and during its drags, the
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
    struct rect r;
    if (menu_open && (r = menu_rect(), in_rect(mx, my, r.x, r.y, r.w, r.h))) return N_POINTER_DEFAULT;
    if (vol_open && (r = vol_rect(), in_rect(mx, my, r.x, r.y, r.w, r.h))) return N_POINTER_DEFAULT;
    if (toast_until && (r = toast_rect(), in_rect(mx, my, r.x, r.y, r.w, r.h))) return N_POINTER_DEFAULT;
    struct window *f = focused();
    if (my >= sh - TASKBAR_H && (!f || !f->fullscreen || menu_open || vol_open)) return N_POINTER_DEFAULT;
    struct window *w = window_at(mx, my);
    if (!w || in_resize_grip(w, mx, my)) return w ? N_POINTER_NWSE_RESIZE : N_POINTER_DEFAULT;
    if (!in_rect(mx, my, client_x(w), client_y(w), w->cw, w->ch)) return N_POINTER_DEFAULT;
    *from = w;
    return (int)w->pointer_shape;
}

/* Brings the pointer's image up to what it should show; the old and the new place are drawn again.
   After input, after a window's wish, and once a pass (windows that moved, opened or closed under
   it). */
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
        memcpy(pointer_px, desktop_pointer(shape, &pointer_hx, &pointer_hy), sizeof pointer_px);
    }
    pointer_shape = shape;
    pointer_from = from;
    pointer_from_serial = serial;
    __atomic_store_n(&pointer_serial, pointer_serial + 1 ? pointer_serial + 1 : 1, __ATOMIC_RELAXED);
    damage_cursor();
    wake_compositor();
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
    pointer_update();
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
                launch(menu_app(i)->path, menu_app(i)->arg);
            }
            return;
        }
        set_menu(false);
        if (my >= sh - TASKBAR_H && mx < TB_START_W) return; /* clicking start again closes */
    }
    if (my >= sh - TASKBAR_H&&(!focused()||!focused()->fullscreen)) {
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
    if (ic >= 0 && dbl) launch(apps[desktop_apps[ic]].path, apps[desktop_apps[ic]].arg);
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
        /* pointer speed in quarters: x0.5, x0.75, x1, x1.5, x2; slow moves keep their remainder */
        static const int quarters[5] = {2, 3, 4, 6, 8};
        int q = quarters[pointer_speed + 2];
        if (q != 4) {
            pointer_rest_x += dx * q;
            pointer_rest_y += dy * q;
            dx = pointer_rest_x / 4;
            dy = pointer_rest_y / 4;
            pointer_rest_x -= dx * 4;
            pointer_rest_y -= dy * 4;
        }
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

void wm_vblank(uint64_t count, uint64_t timestamp_ns) {
    uint64_t fl = spin_lock(&vblank_lock);
    vblank_count = count;
    vblank_ns = timestamp_ns;
    vblank_at_ms = uptime_ms();
    vblank_seen = true;
    bool wake = vblank_waiting;
    vblank_waiting = false;
    spin_unlock(&vblank_lock, fl);
    if (wake) wake_compositor();
}

/* Whether vertical blanks pace frames at now (read under vblank_lock, after the last blank's
   vblank_at_ms). vblank_lock held. */
static bool vblank_paced(uint64_t now) { return vblank_seen && now - vblank_at_ms < VBLANK_LOST_MS; }
/* Whether a frame with damage may start at now. vblank_lock held. */
static bool frame_due(uint64_t now) {
    return vblank_paced(now) ? vblank_count != vblank_drawn : now * 1000 >= frame_next_us;
}
/* The frame period in µs: the refresh of the mode the screen shows, the display driver's while its
   scanout is shown, else the firmware's (from the monitor's EDID), else FALLBACK_REFRESH_MHZ. */
static uint64_t frame_period(void) {
    uint32_t mhz = fb_boot_shown() ? fb.refresh_mhz : screen_shown_refresh_mhz();
    if (!mhz) mhz = FALLBACK_REFRESH_MHZ;
    if (mhz != frame_mhz) {
        frame_mhz = mhz;
        kprintf("wm: without vertical blanks, frames are paced at the mode's %u.%03u Hz\n", mhz / 1000, mhz % 1000);
    }
    return 1000000000ull / mhz;
}
/* Whether a shown window waits for a frame without damage to make it (SYS_WIN_FRAME). wm_lock
   held; whoever asks for one wakes the compositor after it. */
static bool frames_asked(void) {
    for (int i = 0; i < nwin; i++)
        if (zorder[i]->frame_wanted && visible(zorder[i])) return true;
    return false;
}
/* The next frame's earliest start, after one started at now (ms). */
static void frame_started(uint64_t now) {
    uint64_t us = now * 1000, period = frame_period();
    frame_next_us = us - frame_next_us < period ? frame_next_us + period : us + period;
}

/* ---- compositor thread ----
   Each pass ends reading wm_wakes and then, under wm_lock, whether work is left (damage, a
   present, a frame asked for). The next sleeps only while wm_wakes still reads the same
   (wq_wait_unless): whoever makes work on another CPU after that look moved it on before its
   wake. A vertical blank that came before the sleep makes the frame due; one after it wakes the
   thread (vblank_waiting). */
static void wm_thread(void *arg) {
    wm_lock();
    damage_all();
    uint64_t seen = __atomic_load_n(&wm_wakes, __ATOMIC_SEQ_CST);
    bool work = true;
    wm_unlock();
    for (;;) {
        bool waited=false; uint64_t waiting=0;
        uint64_t wait_ms = 0;
        if (!input_waiting()) {
            uint64_t fl = spin_lock(&vblank_lock);
            uint64_t now = uptime_ms();
            if (!work) wait_ms = 250; /* the clock and a toast's end */
            else if (!frame_due(now)) {
                vblank_waiting = vblank_paced(now);
                wait_ms = vblank_waiting ? VBLANK_LOST_MS - (now - vblank_at_ms) : (frame_next_us - now * 1000 + 999) / 1000;
            }
            spin_unlock(&vblank_lock, fl);
        }
        if (wait_ms) {
            if (trace_active) { waited=true; waiting=rdtsc(); wt.waits++; }
            wq_wait_unless(&wm_wq, &wm_wakes, seen, wait_ms);
            if (waiting) { uint64_t ticks=rdtsc()-waiting; wt.wait_ticks+=ticks; wt.max_wait=MAX(wt.max_wait,ticks); }
        }
        uint64_t fl = spin_lock(&vblank_lock);
        vblank_waiting = false;
        bool pacing = vblank_paced(uptime_ms());
        uint64_t first_count = vblank_count, first_ns = vblank_ns;
        spin_unlock(&vblank_lock, fl);
        if (pacing != vblank_pacing) {
            vblank_pacing = pacing;
            if (pacing)
                kprintf("wm: frames paced by the display driver's vertical blank (count %lu at %lu ns)\n",
                        (unsigned long)first_count, (unsigned long)first_ns);
            else
                kprintf("wm: no vertical blank for %u ms; frames are paced at the mode's refresh again\n",
                        VBLANK_LOST_MS);
        }
        wm_lock();
        struct trace_frame frame={.begin=trace_active?rdtsc():0,.waited=waited};

        struct in_ev ev;
        while (input_take(&ev)) {
            if (trace_active && ev.stamp>=wt.start && ev.seq) {
                uint64_t delay=rdtsc()-ev.stamp; wt.max_dequeue=MAX(wt.max_dequeue,delay); trace_hist(wt.dequeue_hist,delay);
                if (!frame.oldest) frame.oldest=ev.stamp;
                frame.seq=ev.seq; frame.inputs++;
            }
            if (ev.is_key) handle_key(&ev.k);
            else handle_mouse(&ev.m);
        }

        /* Nothing is presenting between frames: the planes can be replaced. */
        bool smooth = deep_wanted && fb_dither_able();
        if (smooth != fb.simulated) set_deep_simulation(smooth);
        /* Another scanout shown (a display driver's taken, the boot screen given back): whether
           smooth gradients can be shown and where the screen's mode comes from, as reported. */
        bool able = fb_dither_able(), boot = fb_boot_shown();
        if (able != seen_dither_able || boot != seen_boot) {
            seen_dither_able = able;
            seen_boot = boot;
            wm_screen_info_changed();
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
        pointer_update();
        direct_update();
        bool want = ndamage || pending_frames || frames_asked();
        fl = spin_lock(&vblank_lock);
        uint64_t now = uptime_ms();
        bool render = want && frame_due(now);
        if (render) vblank_drawn = vblank_count;
        spin_unlock(&vblank_lock, fl);
        if (trace_active && render) {
            frame.rects=ndamage; frame.jobs=cpu_parallel_jobs();
            for (int i=0;i<ndamage;i++) frame.area+=(uint64_t)damage[i].w*damage[i].h;
        }
        if (render) {
            frame_started(now);
            composite();
        }
        trace_frame_end(&frame);
        seen = __atomic_load_n(&wm_wakes, __ATOMIC_SEQ_CST);
        work = ndamage || pending_frames || frames_asked();
        wm_unlock();
        /* only once every batch and present has ended and wm_lock is given back */
        if (wm_yield) {
            if (trace_active) wt.yield_calls++; /* calls, not switches */
            sched_yield();
        }
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
    clipboard_seq++;
    wm_unlock();
}

bool wm_clipboard_set(const char *text, size_t n) {
    if (n > WM_CLIPBOARD_MAX) return false;
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

/* ---- remote display ---- */

/* Change the size of the desktop: to the scanout's when a display driver shows another mode
   (wm_screen_resized), or to a remote viewer's window's (the scanout keeps its own size;
   fb_present clips to it). Not from the compositor: it must not be in the middle of a frame.
   wm_lock held once (not nested): it is given back while the compositor presents back. */
static bool set_screen_size(int w, int h) {
    if (w == sw && h == sh) return true;
    while (presenting_back) {
        /* the present's end moves back_presents on under wm_lock, then wakes present_wait */
        uint64_t seen = back_presents;
        wm_unlock();
        wq_wait_unless(&present_wait, &back_presents, seen, 0);
        wm_lock();
        if (w == sw && h == sh) return true;
    }
    size_t pages = ALIGN_UP((uint64_t)w * h * 4, PAGE_SIZE) / PAGE_SIZE;
    size_t old = ALIGN_UP((uint64_t)sw * sh * 4, PAGE_SIZE) / PAGE_SIZE;
    uint32_t *bb = vmalloc(pages), *wp = vmalloc(pages);
    uint32_t *reference = wm_verify ? vmalloc(pages) : NULL;
    uint8_t *bd, *wd;
    bool deep = alloc_deep(w, h, &bd, &wd);
    if (!bb || !wp || (wm_verify && !reference) || !deep) {
        if (bb) vfree(bb, pages);
        if (wp) vfree(wp, pages);
        if (reference) vfree(reference,pages);
        if (deep && bd) {
            size_t deep_pages = ALIGN_UP((uint64_t)w * h, PAGE_SIZE) / PAGE_SIZE;
            vfree(bd, deep_pages);
            vfree(wd, deep_pages);
        }
        return false;
    }
    vfree(back.px, old);
    vfree(wall.px, old);
    free_deep(sw, sh);
    if (wm_verify) vfree(verify_back.px,old);
    sw = w;
    sh = h;
    gfx_init(&back, bb, sw, sh, sw);
    gfx_init(&wall, wp, sw, sh, sw);
    back_deep = bd;
    wall_deep = wd;
    if (wm_verify) gfx_init(&verify_back,reference,sw,sh,sw);
    draw_wall();
    mx = MIN(mx, sw - 1);
    my = MIN(my, sh - 1);
    drag_mode = DRAG_NONE;
    resize_visible = false;
    for (int i = 0; i < nwin; i++) {
        struct window *win = zorder[i];
        if (win->fullscreen) {
            struct gui_event e={.type=EV_RESIZE,.x=sw,.y=sh};win->x=win->y=0;push_event(win,&e);continue;
        }
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
    wm_screen_info_changed();
    return true;
}

void wm_remote_resize(int *w, int *h) {
    wm_lock();
    if (*w > 0 && *h > 0) set_screen_size(MAX(640, MIN(*w, 3840)), MAX(480, MIN(*h, 2160)));
    *w = sw;
    *h = sh;
    damage_all(); /* repaint without the pointer */
    wake_compositor();
    wm_unlock();
}

bool wm_remote_attach(int *w, int *h) {
    wm_lock();
    if (!wm_running()) { wm_unlock(); return false; }
    if (!__atomic_fetch_add(&remote_users, 1, __ATOMIC_RELAXED))
        wm_screen_info_changed(); /* the source is the remote viewer's window now */
    /* Do not nest WM ownership: set_screen_size may release it while waiting
       for the compositor's VRAM borrow of the previous backbuffer. */
    if (*w > 0 && *h > 0) set_screen_size(MAX(640, MIN(*w, 3840)), MAX(480, MIN(*h, 2160)));
    *w = sw;
    *h = sh;
    damage_all();
    wake_compositor();
    wm_unlock();
    return true;
}

void wm_repaint(void) {
    if (!wm_running()) return;
    wm_lock();
    damage_all();
    wake_compositor();
    wm_unlock();
}

void wm_remote_detach(void) {
    uint32_t w, h;
    fb_screen_size(&w, &h);
    wm_lock();
    if (remote_users > 0 && __atomic_sub_fetch(&remote_users, 1, __ATOMIC_RELAXED) == 0) wm_screen_info_changed();
    if (!remote_users) set_screen_size((int)w, (int)h);
    damage_all();
    wake_compositor();
    wm_unlock();
}

/* Whether a remote viewer has the desktop at its window's size now. Read without wm_lock, so a
   caller holding another lock (or a spinlock) may ask. */
bool wm_remote_viewing(void) { return __atomic_load_n(&remote_users, __ATOMIC_RELAXED) > 0; }

bool wm_screen_resized(uint32_t w, uint32_t h) {
    if (!wm_running()) return true;
    wm_lock();
    bool ok = remote_users || set_screen_size((int)w, (int)h);
    if (!ok) kprintf("wm: no memory for a %ux%u desktop; it keeps its size\n", w, h);
    damage_all();
    wake_compositor();
    wm_unlock();
    return ok;
}

int wm_remote_damage(struct wm_rect *out, int max) {
    wm_lock();
    int n = MIN(nrdamage, max);
    for (int i = 0; i < n; i++) out[i] = (struct wm_rect){rdamage[i].x, rdamage[i].y, rdamage[i].w, rdamage[i].h};
    nrdamage = 0;
    wm_unlock();
    return n;
}

bool wm_remote_copy(uint32_t *dst, int pitch, int x, int y, int w, int h) {
    wm_lock();
    bool valid = x >= 0 && y >= 0 && w >= 0 && h >= 0 &&
                 x <= sw && y <= sh && w <= sw - x && h <= sh - y;
    if (valid)
        for (int row = 0; row < h; row++)
            memcpy(dst + (size_t)row * pitch,
                   back.px + (size_t)(y + row) * back.pitch + x, (size_t)w * 4);
    wm_unlock();
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

void wm_init(void) {
    /* PCI has been enumerated. Optional offscreen GPU never changes the display
       or the RAM frame used by Enhanced Session/RDP. */
    gpu_init();
    watched_init(&screen_watched, NULL);
    seen_dither_able = fb_dither_able();
    seen_boot = fb_boot_shown();
    screen_watchable = true;
    sw = fb.width;
    sh = fb.height;
    deep_wanted = fb.simulated;
    keep_register(&desktop_keep); /* read back by keep_start (init.c), before the first program */
    size_t pages = ALIGN_UP((uint64_t)sw * sh * 4, PAGE_SIZE) / PAGE_SIZE;
    uint32_t *bb = vmalloc(pages), *wp = vmalloc(pages);
    if (!bb || !wp) {
        kprintf("wm: out of memory for buffers\n");
        return;
    }
    gfx_init(&back, bb, sw, sh, sw);
    gfx_init(&wall, wp, sw, sh, sw);
    if (!alloc_deep(sw, sh, &back_deep, &wall_deep)) kprintf("wm: no memory for deep colour, using 8 bpc\n");
    kprintf("wm: %u bpc display, %s\n", fb.bpc,
            back_deep ? "10-bit deep planes" : "8-bit content, dithered gradients");
    draw_wall();
    fb_benchmark(wall.px,wall.pitch);
    /* Experimental RAM work is independent of the opt-in VRAM copy path. */
    wm_verify=cmdline_has("wmverify");
    wm_trace=cmdline_has("wmtrace");
    wm_yield=cmdline_has("wmyield");
    wm_shadow_strip=!cmdline_has("wmshadowstrip=off"); /* default on; legacy shadow path is opt-out */
    wm_occlusion=!cmdline_has("wmocclusion=off"); /* diagnostic original layer traversal */
    wm_parallel_force=cmdline_has("wmparallel") || wm_verify;
    wm_parallel=!cmdline_has("wmparallel=off"); /* off wins over the diagnostics that force it */
    if (wm_verify) {
        uint32_t *reference=vmalloc(pages);
        if (!reference) panic("wmverify: cannot allocate reference RAM");
        gfx_init(&verify_back,reference,sw,sh,sw);
    }
    scene_benchmark();
    mx = sw / 2;
    my = sh / 2;
    __atomic_store_n(&running, true, __ATOMIC_RELEASE); /* after everything a WM call reads */
    fbcon_set_enabled(false);
    input_set_sinks(key_sink, mouse_sink);
    kthread_create("compositor", wm_thread, NULL);
    kprintf("wm: compositor started at %dx%d\n", sw, sh);
    wm_notify("Welcome to " OS_NAME, "Double-click an icon or open the moon menu.");
}

void wm_notify(const char *title, const char *text) {
    if (!wm_running()) {
        kprintf("[%s] %s\n", title, text);
        return;
    }
    wm_lock();
    strlcpy(toast_title, title, sizeof toast_title);
    strlcpy(toast_text, text, sizeof toast_text);
    toast_until = uptime_ms() + 4000;
    struct rect t = toast_rect();
    damage_rect(t.x, t.y, t.w, t.h);
    wake_compositor();
    wm_unlock();
}

void wm_process_exit(int pid) {
    if (!wm_running()) return;
    wm_lock();
    for (int i = nwin - 1; i >= 0; i--) {
        if (i >= nwin) continue;
        if (zorder[i]->pid == pid) win_destroy(zorder[i]);
    }
    wm_unlock();
}

int wm_window_count(char *top_title, size_t n) {
    if (!wm_running()) return 0;
    int count = 0;
    wm_lock();
    for (int i = nwin - 1; i >= 0; i--) {
        if (zorder[i]->dead) continue;
        if (!count++ && top_title) strlcpy(top_title, zorder[i]->title, n);
    }
    wm_unlock();
    return count;
}

void wm_close_windows(void) {
    if (!wm_running()) return;
    wm_lock();
    for (int i = 0; i < nwin; i++)
        if (!zorder[i]->dead) send_simple(zorder[i], EV_CLOSE);
    wm_unlock();
}

/* ---- syscalls ---- */
static struct window *handle_window(struct handle *h) {
    if (!handle_is(h, &win_ops)) return NULL;
    struct window *w = h->object->priv;
    return w->dead ? NULL : w;
}

/* A title longer than the slot is cut on a UTF-8 sequence boundary and marked
   with an ellipsis; a split sequence would draw as replacement glyphs. */
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

static int64_t sys_win_create(int cw, int ch, const char *title, int flags) {
    if (!wm_running()) return -N_ERR_UNSUPPORTED;
    if (nwin >= MAX_WIN) return -N_ERR_NO_MEMORY;
    if (cw < 16) cw = 16;
    if (ch < 16) ch = 16;
    if (cw > sw) cw = sw;
    if (ch > sh) ch = sh;
    struct window *w = kzalloc(sizeof *w);
    if (!w) return -N_ERR_NO_MEMORY;
    memcpy(w->title, title, sizeof w->title);
    w->flags = flags;
    w->pid = task_process()->pid;
    w->id = next_win_id++;
    if (!alloc_buffer(w, cw, ch)) {
        kfree(w);
        return -N_ERR_NO_MEMORY;
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
    object_init(&w->object, &win_ops, w);
    struct handle *h = handle_open(&w->object, true);
    if (!h) {
        vfree(w->buf, w->pages);
        vfree(w->user_buf, w->pages);
        kfree(w);
        return -N_ERR_NO_MEMORY;
    }
    int index = handle_install(h);
    if (index < 0) return index; /* closing it destroyed the window and freed everything */
    w->pml4 = current_task->pml4;
    zorder[nwin++] = w;
    /* Use the ordinary WM transition and resize event. The client still owns
       its real framebuffer allocation; startup must not invent a viewport. */
    if (flags & WIN_START_MAXIMIZED) toggle_maximize(w);
    return index;
}

static void show_if_needed(struct window *w) {
    if (w->shown) return;
    w->shown = true;
    raise_window(w);
    damage_window(w);
    damage_rect(0, sh - TASKBAR_H, sw, TASKBAR_H);
}

/* wm_lock held once (wm_syscall): a SYS_WIN_MAP wait gives it back while it sleeps. */
static int64_t wm_syscall_locked(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                                 struct handle *handle, struct call_input *in) {
    switch (num) {
    case SYS_WIN_CREATE: return sys_win_create((int)a, (int)b, in->title, (int)d);
    case SYS_WIN_MAP: {
        struct window *w = handle_window(handle);
        if (!w) return -N_ERR_NO_HANDLE;
        if (w->pml4 != current_task->pml4) return -N_ERR_DENIED;
        if (b >= ((w->flags & WIN_DOUBLE_BUFFER) ? WIN_BUFFER_COUNT : 1) || c > WIN_MAP_ACQUIRE) return -N_ERR_INVALID;
        if (c && !(w->flags & WIN_DOUBLE_BUFFER)) return -N_ERR_INVALID;
        while (c && (w->pending_index >= 0 || w->front_index == (int)b)) {
            /* The indices change under ev_lock too (buffers_set), and whoever changes them wakes
               w->wq after; the handle's reference keeps w while wm_lock is given back. */
            wm_unlock();
            uint64_t fl = spin_lock(&w->ev_lock);
            if (!w->dead && !current_task->killed && (w->pending_index >= 0 || w->front_index == (int)b))
                wq_wait_locked(&w->wq, &w->ev_lock);
            else
                spin_give(&w->ev_lock);
            irq_restore(fl);
            wm_lock();
            if (w->dead) return -N_ERR_NO_HANDLE;
            if (current_task->killed) return -N_ERR_STOPPED;
        }
        uint64_t va = b ? w->second_uaddr : w->uaddr;
        if (va) return (int64_t)va;
        va = map_user(w, task_process(), (unsigned)b);
        return va ? (int64_t)va : -N_ERR_NO_MEMORY;
    }
    case SYS_WIN_STATE: {
        struct window *w=handle_window(handle);
        if(!w)return -N_ERR_NO_HANDLE;
        if(w->pml4!=current_task->pml4)return -N_ERR_DENIED;
        return win_state_of(w);
    }
    case SYS_WIN_DIRECT: {
        struct window *w = handle_window(handle);
        if (!w) return -N_ERR_NO_HANDLE;
        if (w->pml4 != current_task->pml4) return -N_ERR_DENIED;
        if (b > 1) return -N_ERR_INVALID;
        w->direct_wanted = b != 0;
        direct_update();
        return 0;
    }
    case SYS_WIN_FRAME: {
        struct window *w = handle_window(handle);
        if (!w) return -N_ERR_NO_HANDLE;
        if (w->pml4 != current_task->pml4) return -N_ERR_DENIED;
        if (!(w->flags & WIN_FRAME_EVENTS)) return -N_ERR_INVALID;
        w->frame_wanted = true; /* the compositor makes a frame for it (frames_asked) */
        wake_compositor();
        return 0;
    }
    case SYS_WIN_POINTER: {
        struct window *w = handle_window(handle);
        if (!w) return -N_ERR_NO_HANDLE;
        if (w->pml4 != current_task->pml4) return -N_ERR_DENIED;
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
        struct window *w=handle_window(handle);if(!w)return -N_ERR_NO_HANDLE;
        if(w->pml4!=current_task->pml4)return -N_ERR_DENIED;if(b>1)return -N_ERR_INVALID;
        if(w->fullscreen==!!b)return 0;
        damage_window(w);struct gui_event resize={.type=EV_RESIZE};
        if(b){w->fullscreen_restore=(struct rect){w->x,w->y,w->cw,w->ch};w->fullscreen=true;w->x=w->y=0;resize.x=sw;resize.y=sh;}
        else{w->fullscreen=false;w->x=w->fullscreen_restore.x;w->y=w->fullscreen_restore.y;resize.x=w->fullscreen_restore.w;resize.y=w->fullscreen_restore.h;}
        push_event(w,&resize);damage_window(w);damage_taskbar();wake_compositor();return 0;
    }
    case SYS_WIN_PRESENT: {
        struct window *w = handle_window(handle);
        if (!w) return -N_ERR_NO_HANDLE;
        if (w->pml4 != current_task->pml4) return -N_ERR_DENIED;
        if (w->flags & WIN_DOUBLE_BUFFER) {
            if (!e || e > WIN_BUFFER_COUNT || b || c || d) return -N_ERR_INVALID;
            int index = (int)e - 1;
            if (w->pending_index >= 0 || w->front_index == index) return -N_ERR_BUSY;
            if (!(index ? w->second_uaddr : w->uaddr)) return -N_ERR_INVALID;
            buffers_set(w, w->front_index, index);
            pending_frames++;
            if (!w->shown) show_if_needed(w);
            else if (visible(w)) damage_rect(client_x(w), client_y(w), w->cw, w->ch);
            wake_compositor();
            return 0;
        }
        if (e) return -N_ERR_INVALID;
        int x = (int)b, y = (int)c, rw = (int)(d >> 32), rh = (int)(d & 0xFFFFFFFF);
        if (rw <= 0 || rh <= 0) {
            x = y = 0;
            rw = w->cw;
            rh = w->ch;
        }
        /* A present message publishes only the requested rectangle. Mutable
           user pixels are never referenced by asynchronous compositor jobs. */
        int x0 = MAX(0, MIN(x, w->cw)), y0 = MAX(0, MIN(y, w->ch));
        int x1 = MAX(x0, (int)MIN((int64_t)w->cw, (int64_t)x + rw));
        int y1 = MAX(y0, (int)MIN((int64_t)w->ch, (int64_t)y + rh));
        for (int row = y0; row < y1; row++)
            memcpy(w->buf + (size_t)row * w->cw + x0,
                   w->user_buf + (size_t)row * w->cw + x0, (size_t)(x1 - x0) * 4);
        if (w->flags & WIN_FRAME_EVENTS) w->frame_wanted = true;
        if (!w->shown) show_if_needed(w);
        else if (visible(w)) damage_rect(client_x(w) + x, client_y(w) + y, MIN(rw, w->cw - x), MIN(rh, w->ch - y));
        wake_compositor();
        return 0;
    }
    case SYS_WIN_SET_TITLE: {
        struct window *w = handle_window(handle);
        if (!w) return -N_ERR_NO_HANDLE;
        if (in->error) return in->error;
        memcpy(w->title, in->title, sizeof w->title);
        if (w->shown) {
            damage_rect(w->x, w->y, frame_w(w), TITLE_H);
            damage_rect(0, sh - TASKBAR_H, sw, TASKBAR_H);
            wake_compositor();
        }
        return 0;
    }
    case SYS_WIN_MOVE: {
        struct window *w = handle_window(handle);
        if (!w) return -N_ERR_NO_HANDLE;
        damage_window(w);
        w->x = (int)b;
        w->y = (int)c;
        damage_window(w);
        wake_compositor();
        return 0;
    }
    case SYS_WIN_RESIZE: {
        struct window *w = handle_window(handle);
        if (!w) return -N_ERR_NO_HANDLE;
        if (w->pml4 != current_task->pml4) return -N_ERR_DENIED;
        int ncw = MAX(16, MIN((int)b, sw)), nch = MAX(16, MIN((int)c, sh));
        damage_window(w);
        uint32_t *obuf = w->buf;
        uint32_t *ouser = w->user_buf;
        size_t opages = w->pages;
        int ocw = w->cw, och = w->ch;
        bool was_pending = w->pending_index >= 0;
        if (!alloc_buffer(w, ncw, nch)) {
            w->buf = obuf;
            w->user_buf = ouser;
            w->pages = opages;
            return -N_ERR_NO_MEMORY;
        }
        for (int y = 0; y < MIN(och, nch); y++) {
            memcpy(&w->buf[y * ncw], &obuf[y * ocw], MIN(ocw, ncw) * 4);
            memcpy(&w->user_buf[y * ncw], &ouser[y * ocw], MIN(ocw, ncw) * 4);
        }
        /* unmap the old buffer from the owner */
        if (w->uaddr)
            vmm_user_free(w->pml4, w->uaddr, opages * PAGE_SIZE); /* PTE_SHARED: unmap only */
        if (w->second_uaddr)
            vmm_user_free(w->pml4, w->second_uaddr, opages * PAGE_SIZE);
        w->uaddr = w->second_uaddr = 0;
        if (was_pending) pending_frames--;
        wq_wake_all(&w->wq);
        vfree(obuf, opages);
        vfree(ouser, opages);
        damage_window(w);
        wake_compositor();
        uint64_t va = map_user(w, task_process(), 0);
        return va ? (int64_t)va : -N_ERR_NO_MEMORY;
    }
    case SYS_SCREEN_GRAB: {
        /* copy the composed screen (desktop, windows, taskbar) as 0xAARRGGBB pixels; returns
           width << 16 | height. The buffer must hold width * height pixels (bytes = 0: size only). */
        if (!wm_running()) return -N_ERR_UNSUPPORTED;
        uint32_t *dst = (uint32_t *)a;
        size_t need = (size_t)sw * (size_t)sh * 4;
        if (b == 0) return (int64_t)sw << 16 | sh; /* just the size */
        if (b < need) return -N_ERR_TOO_LARGE;
        if (!user_ok_w(dst, need)) return -N_ERR_BAD_ADDRESS;
        /* The compositor writes the canvas only under wm_lock, which this sleeping copy holds. */
        for (int y = 0; y < sh; y++) {
            int r = copy_to_user(dst + (size_t)y * (size_t)sw, back.px + (size_t)y * (size_t)back.pitch, (size_t)sw * 4);
            if (r < 0) return r;
        }
        return (int64_t)sw << 16 | sh;
    }
    default: return -N_ERR_UNSUPPORTED;
    }
}

/* Starts the program at path a (with the one argument b, or none), without wm_lock. */
static int64_t sys_gui_launch(uint64_t a, uint64_t b) {
    char *path, *arg = NULL;
    int r = user_string((const char *)a, &path);
    if (r < 0) return r;
    if (b && (r = user_string((const char *)b, &arg)) < 0) {
        kfree(path);
        return r;
    }
    r = proc_spawn_simple(path, arg, NULL);
    kfree(path);
    kfree(arg);
    return r;
}

/* SYS_SCREEN_INFO's settings and screen format: the reply is made under wm_lock and copied out
   after it. */
static int64_t sys_screen_info(uint64_t a, uint64_t b, uint64_t c) {
    if (c == SCREEN_SETTINGS_GET || c == SCREEN_SETTINGS_SET) {
        if (b != sizeof(struct desktop_settings)) return -N_ERR_INVALID;
        struct desktop_settings ds;
        if (c == SCREEN_SETTINGS_SET) {
            int r = copy_from_user(&ds, (const void *)a, sizeof ds);
            if (r < 0) return r;
            if (ds.pointer_speed < -2 || ds.pointer_speed > 2 || ds.smooth_gradients > 1) return -N_ERR_INVALID;
        }
        wm_lock();
        if (c == SCREEN_SETTINGS_SET) desktop_settings_put(ds.pointer_speed, ds.smooth_gradients != 0);
        ds = (struct desktop_settings){
            .pointer_speed = (int8_t)pointer_speed,
            .smooth_gradients = deep_wanted,
            .smooth_gradients_available = fb_dither_able(),
        };
        wm_unlock();
        return copy_to_user((void *)a, &ds, sizeof ds);
    }
    /* b == 0: the original struct screen_info; b == sizeof(struct
       screen_format): the colour format as well (display_abi.h). */
    if (b && b != sizeof(struct screen_format)) return -N_ERR_INVALID;
    struct screen_format si = {0};
    wm_lock();
    bool on = wm_running();
    si.width = on ? sw : fb.width;
    si.height = on ? sh - TASKBAR_H : fb.height;
    si.bits_per_pixel = fb.bpp;
    si.red_bits = fb.simulated ? 10 : fb.rsize;
    si.green_bits = fb.simulated ? 10 : fb.gsize;
    si.blue_bits = fb.simulated ? 10 : fb.bsize;
    /* Without deep planes the desktop delivers 8 bits per channel. */
    si.bits_per_channel = back_deep ? fb.bpc : MIN(fb.bpc, 8);
    wm_unlock();
    int r = copy_to_user((void *)a, &si, b ? sizeof si : sizeof(struct screen_info));
    if (r < 0) return r;
    return on ? 0 : -N_ERR_UNSUPPORTED;
}

static int64_t sys_clipboard_set(uint64_t a, uint64_t b) {
    size_t n = b;
    if (n > WM_CLIPBOARD_MAX) return -N_ERR_TOO_LARGE;
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
    size_t len;
    char *text = wm_clipboard_snapshot(&len);
    if (!text) return -N_ERR_NO_MEMORY;
    size_t n = MIN(b, len);
    int r = n ? copy_to_user((void *)a, text, n) : 0;
    kfree(text);
    return r < 0 ? r : (int64_t)len;
}

int64_t wm_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    if (num == SYS_GUI_LAUNCH) return sys_gui_launch(a, b);
    if (num == SYS_SCREEN_INFO) {
        /* The monitor's mode: its own lock, which comes before the WM's (it tells the driver
           through wm_screen_mode). */
        if (c == SCREEN_MODES_GET || c == SCREEN_MODE_SET) return screen_modes_syscall((void *)a, b, c);
        /* A watch on what all of SYS_SCREEN_INFO reports (else 5 would be read as the reply's size). */
        if (c == SCREEN_WATCH)
            return a || b ? -N_ERR_INVALID : !screen_watchable ? -N_ERR_UNSUPPORTED : watched_open(&screen_watched);
        return sys_screen_info(a, b, c);
    }
    if (num == SYS_CLIPBOARD_SET) return sys_clipboard_set(a, b);
    if (num == SYS_CLIPBOARD_GET) return sys_clipboard_get(a, b);
    struct handle *handle = NULL;
    if (num == SYS_WIN_RECEIVE) {
        /* Without the WM lock (ev_lock); the handle's reference keeps the window's memory. */
        handle = handle_get((int)a);
        int64_t result = !handle ? -N_ERR_NO_HANDLE : !handle_is(handle, &win_ops) ? -N_ERR_WRONG_KIND
                       : win_receive(handle->object->priv, (struct gui_event *)b, (size_t)c, (int64_t)d);
        handle_close(handle);
        return result;
    }
    struct call_input in = {0};
    if (num == SYS_WIN_CREATE) {
        if (!c) strcpy(in.title, current_task->name);
        else if (user_title(in.title, (const char *)c, sizeof in.title) < 0) strcpy(in.title, "window");
    } else if (num == SYS_WIN_SET_TITLE) {
        if (user_title(in.title, (const char *)b, sizeof in.title) < 0) in.error = -N_ERR_BAD_ADDRESS;
    } else if (num == SYS_WIN_POINTER) {
        in.error = pointer_read(b, c, &in.pointer, &in.image);
    }
    if (num == SYS_WIN_MAP || num == SYS_WIN_STATE || num == SYS_WIN_PRESENTATION || num == SYS_WIN_PRESENT ||
        num == SYS_WIN_SET_TITLE || num == SYS_WIN_MOVE || num == SYS_WIN_RESIZE || num == SYS_WIN_DIRECT ||
        num == SYS_WIN_FRAME || num == SYS_WIN_POINTER)
        handle = handle_get((int)a);
    wm_lock();
    int64_t result = wm_syscall_locked(num, a, b, c, d, e, handle, &in);
    wm_unlock();
    kfree(in.image); /* a pointer image the call did not keep */
    handle_close(handle);
    return result;
}
