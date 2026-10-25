/* Client side of the Nocturne window system. Results come back as they are:
   a value, or a negative n_result. Window addresses are user addresses, so a
   mapping call is either a positive address or a negative n_result. */
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"
#include "display.h"

bool gui_available(void) {
    struct screen_info si;
    return __syscall(SYS_SCREEN_INFO, (long)&si, 0, 0, 0, 0) == 0;
}

n_result screen_size(int *w, int *h) {
    struct screen_info si;
    n_result r = __syscall(SYS_SCREEN_INFO, (long)&si, 0, 0, 0, 0);
    if (r < 0) return r;
    if (w) *w = si.width;
    if (h) *h = si.height;
    return 0;
}

n_result screen_format(struct screen_format *out) {
    return __syscall(SYS_SCREEN_INFO, (long)out, sizeof *out, 0, 0, 0);
}

int display_bits_per_channel(void) {
    /* not cached: Settings can switch the desktop between 8 and 10 bpc */
    struct screen_format f;
    return screen_format(&f) == 0 && f.bits_per_channel ? f.bits_per_channel : 8;
}

uint8_t *win_deep(window_t *win) {
    if (!win || !win->c.px || !(win->flags & WIN_DEEP_COLOR) || display_bits_per_channel() < 10) return NULL;
    return (uint8_t *)(win->c.px + (size_t)win->w * win->h);
}

n_result win_open(int w, int h, const char *title, int flags, window_t **out) {
    *out = NULL;
    n_result handle = __syscall(SYS_WIN_CREATE, w, h, (long)title, flags, 0);
    if (handle < 0) return handle;
    n_result addr = __syscall(SYS_WIN_MAP, handle, 0, 0, 0, 0);
    if (addr < 0) { n_handle_close((n_handle)handle); return addr; }
    window_t *win = calloc(1, sizeof *win);
    if (!win) { n_handle_close((n_handle)handle); return -N_ERR_NO_MEMORY; }
    win->handle = (n_handle)handle;
    win->w = w;
    win->h = h;
    win->flags = flags;
    win->buffers[0] = (uint32_t *)addr;
    win->frame_acquired = true;
    if (flags & WIN_DOUBLE_BUFFER) {
        n_result other = __syscall(SYS_WIN_MAP, handle, 1, 0, 0, 0);
        if (other < 0) { n_handle_close((n_handle)handle); free(win); return other; }
        win->buffers[1] = (uint32_t *)other;
    }
    gfx_init(&win->c, (uint32_t *)addr, w, h, w);
    *out = win;
    return 0;
}

void win_close(window_t *win) {
    if (!win) return;
    n_handle_close(win->handle);
    free(win);
}

n_result win_begin_frame(window_t *win) {
    if (!win || win->closed) return -N_ERR_NO_HANDLE;
    if (!(win->flags & WIN_DOUBLE_BUFFER) || win->frame_acquired) return 0;
    n_result address = __syscall(SYS_WIN_MAP, win->handle, win->buffer_index, WIN_MAP_ACQUIRE, 0, 0);
    if (address < 0) return address;
    win->buffers[win->buffer_index] = (uint32_t *)address;
    gfx_init(&win->c, (uint32_t *)address, win->w, win->h, win->w);
    win->frame_acquired = true;
    return 0;
}

n_result win_update(window_t *win) {
    if (!win || win->closed) return -N_ERR_NO_HANDLE;
    if (!(win->flags & WIN_DOUBLE_BUFFER)) return __syscall(SYS_WIN_PRESENT, win->handle, 0, 0, 0, 0);
    if (!win->frame_acquired) return -N_ERR_BUSY;
    n_result result = __syscall(SYS_WIN_PRESENT, win->handle, 0, 0, 0, win->buffer_index + 1);
    if (result < 0) return result;
    win->buffer_index ^= 1;
    win->frame_acquired = false;
    win->c.px = NULL; /* a submitted surface is no longer a writable canvas */
    return 0;
}

n_result win_update_rect(window_t *win, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (win->flags & WIN_DOUBLE_BUFFER) return win_update(win);
    return __syscall(SYS_WIN_PRESENT, win->handle, x, y, ((long)w << 32) | (unsigned)h, 0);
}

n_result win_set_title(window_t *win, const char *title) { return __syscall(SYS_WIN_SET_TITLE, win->handle, (long)title, 0, 0, 0); }
n_result win_move(window_t *win, int x, int y) { return __syscall(SYS_WIN_MOVE, win->handle, x, y, 0, 0); }
n_result win_state(window_t *win) { return win && !win->closed ? __syscall(SYS_WIN_STATE,win->handle,0,0,0,0) : -N_ERR_NO_HANDLE; }
n_result win_fullscreen(window_t *win,bool enabled) { return win&&!win->closed?__syscall(SYS_WIN_PRESENTATION,win->handle,enabled,0,0,0):-N_ERR_NO_HANDLE; }
n_result win_direct(window_t *win, bool wanted) {
    return win && !win->closed ? __syscall(SYS_WIN_DIRECT, win->handle, wanted, 0, 0, 0) : -N_ERR_NO_HANDLE;
}
n_result win_frame(window_t *win) {
    return win && !win->closed ? __syscall(SYS_WIN_FRAME, win->handle, 0, 0, 0, 0) : -N_ERR_NO_HANDLE;
}

/* One system call takes every queued event that fits, waiting up to timeout. */
static n_result fetch(window_t *win, int timeout) {
    if (win->closed) return -N_ERR_CLOSED;
    n_result n = __syscall(SYS_WIN_RECEIVE, win->handle, (long)win->q,
                           sizeof win->q / sizeof win->q[0], timeout, 0);
    if (n == 0 || n == -N_ERR_TIMEOUT || n == -N_ERR_STOPPED) return 0;
    if (n < 0) {
        win->closed = true;
        return n;
    }
    win->qn = (int)n;
    win->qi = 0;
    return 1;
}

n_result win_event(window_t *win, struct gui_event *e, int timeout_ms) {
    if (win->qi >= win->qn) {
        n_result r = fetch(win, timeout_ms);
        if (r <= 0) return r;
    }
    *e = win->q[win->qi++];
    if (e->type == EV_RESIZE) {
        n_result addr = __syscall(SYS_WIN_RESIZE, win->handle, e->x, e->y, 0, 0);
        if (addr > 0) {
            win->w = e->x;
            win->h = e->y;
            gfx_init(&win->c, (uint32_t *)addr, e->x, e->y, e->x);
            win->buffers[0] = (uint32_t *)addr;
            win->buffer_index = 0;
            win->frame_acquired = true;
            if (win->flags & WIN_DOUBLE_BUFFER) {
                /* fetch may have batched a completion for the OLD surfaces
                   behind this resize. Preserve input, discard stale releases. */
                int kept = win->qi;
                for (int i = win->qi; i < win->qn; i++)
                    if (win->q[i].type != EV_FRAME) win->q[kept++] = win->q[i];
                win->qn = kept;
                n_result other = __syscall(SYS_WIN_MAP, win->handle, 1, 0, 0, 0);
                if (other < 0) { win->closed = true; return other; }
                win->buffers[1] = (uint32_t *)other;
            }
        }
    }
    return 1;
}

n_result clipboard_set(const char *s, size_t n) { return __syscall(SYS_CLIPBOARD_SET, (long)s, n, 0, 0, 0); }
n_result clipboard_get(char *buf, size_t n) { return __syscall(SYS_CLIPBOARD_GET, (long)buf, n, 0, 0, 0); }
n_result gui_launch(const char *path, const char *arg) {
    char full[4096];
    if (!find_program(path, full, sizeof full)) return -N_ERR_NOT_FOUND;
    return __syscall(SYS_GUI_LAUNCH, (long)full, (long)arg, 0, 0, 0);
}

n_result screen_grab(uint32_t *buf, size_t bytes, int *w, int *h) {
    n_result r = __syscall(SYS_SCREEN_GRAB, (long)buf, (long)bytes, 0, 0, 0);
    if (r < 0) return r;
    *w = (int)(r >> 16);
    *h = (int)(r & 0xFFFF);
    return 0;
}
