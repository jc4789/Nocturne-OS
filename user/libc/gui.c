/* Client side of the Nocturne window system. */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

bool gui_available(void) {
    struct screen_info si;
    return __syscall(SYS_SCREEN_INFO, (long)&si, 0, 0, 0, 0) == 0;
}

int screen_size(int *w, int *h) {
    struct screen_info si;
    long r = __syscall(SYS_SCREEN_INFO, (long)&si, 0, 0, 0, 0);
    if (w) *w = si.width;
    if (h) *h = si.height;
    return (int)r;
}

window_t *win_open(int w, int h, const char *title, int flags) {
    long fd = __syscall(SYS_WIN_CREATE, w, h, (long)title, flags, 0);
    if (fd < 0) {
        errno = (int)-fd;
        return NULL;
    }
    long addr = __syscall(SYS_WIN_MAP, fd, 0, 0, 0, 0);
    if (addr < 0 && addr > -4096) {
        close((int)fd);
        errno = (int)-addr;
        return NULL;
    }
    window_t *win = calloc(1, sizeof *win);
    if (!win) { close((int)fd); errno = ENOMEM; return NULL; }
    win->fd = (int)fd;
    win->w = w;
    win->h = h;
    win->flags = flags;
    win->buffers[0] = (uint32_t *)addr;
    win->frame_acquired = true;
    if (flags & WIN_DOUBLE_BUFFER) {
        long other = __syscall(SYS_WIN_MAP, fd, 1, 0, 0, 0);
        if (other < 0 && other > -4096) {
            close((int)fd); free(win); errno = (int)-other; return NULL;
        }
        win->buffers[1] = (uint32_t *)other;
    }
    gfx_init(&win->c, (uint32_t *)addr, w, h, w);
    return win;
}

void win_close(window_t *win) {
    if (!win) return;
    close(win->fd);
    free(win);
}

int win_begin_frame(window_t *win) {
    if (!win || win->closed) return -EBADF;
    if (!(win->flags & WIN_DOUBLE_BUFFER) || win->frame_acquired) return 0;
    long address = __syscall(SYS_WIN_MAP, win->fd, win->buffer_index, WIN_MAP_ACQUIRE, 0, 0);
    if (address < 0 && address > -4096) { errno = (int)-address; return (int)address; }
    win->buffers[win->buffer_index] = (uint32_t *)address;
    gfx_init(&win->c, (uint32_t *)address, win->w, win->h, win->w);
    win->frame_acquired = true;
    return 0;
}

void win_update(window_t *win) {
    if (!win || win->closed) return;
    if (!(win->flags & WIN_DOUBLE_BUFFER)) {
        __syscall(SYS_WIN_PRESENT, win->fd, 0, 0, 0, 0); return;
    }
    if (!win->frame_acquired) { errno = EBUSY; return; }
    long result = __syscall(SYS_WIN_PRESENT, win->fd, 0, 0, 0, win->buffer_index + 1);
    if (result < 0) { errno = (int)-result; return; }
    win->buffer_index ^= 1;
    win->frame_acquired = false;
    win->c.px = NULL; /* a submitted surface is no longer a writable canvas */
}

void win_update_rect(window_t *win, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (win->flags & WIN_DOUBLE_BUFFER) { win_update(win); return; }
    __syscall(SYS_WIN_PRESENT, win->fd, x, y, ((long)w << 32) | (unsigned)h, 0);
}

void win_set_title(window_t *win, const char *title) { __syscall(SYS_WIN_SET_TITLE, win->fd, (long)title, 0, 0, 0); }
void win_move(window_t *win, int x, int y) { __syscall(SYS_WIN_MOVE, win->fd, x, y, 0, 0); }
int win_state(window_t *win) { return win && !win->closed ? (int)__syscall(SYS_WIN_STATE,win->fd,0,0,0,0) : -EBADF; }
int win_fullscreen(window_t *win,bool enabled) { return win&&!win->closed?(int)__syscall(SYS_WIN_PRESENTATION,win->fd,enabled,0,0,0):-EBADF; }

static int fetch(window_t *win, int timeout) {
    if (win->closed) return -1;
    if (timeout != -1) {
        struct n_pollfd p = {win->fd, N_POLLIN, 0};
        int r = poll(&p, 1, timeout);
        if (r <= 0) return 0;
    }
    ssize_t n = read(win->fd, win->q, sizeof win->q);
    if (n <= 0) {
        win->closed = true;
        return -1;
    }
    win->qn = (int)(n / sizeof(struct gui_event));
    win->qi = 0;
    return 1;
}

int win_event(window_t *win, struct gui_event *e, int timeout_ms) {
    if (win->qi >= win->qn) {
        int r = fetch(win, timeout_ms);
        if (r <= 0) return r;
    }
    *e = win->q[win->qi++];
    if (e->type == EV_RESIZE) {
        long addr = __syscall(SYS_WIN_RESIZE, win->fd, e->x, e->y, 0, 0);
        if (addr > 0 || addr < -4096) {
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
                long other = __syscall(SYS_WIN_MAP, win->fd, 1, 0, 0, 0);
                if (other < 0 && other > -4096) { win->closed = true; return -1; }
                win->buffers[1] = (uint32_t *)other;
            }
        }
    }
    return 1;
}

int clipboard_set(const char *s, size_t n) { return (int)__syscall(SYS_CLIPBOARD_SET, (long)s, n, 0, 0, 0); }
int clipboard_get(char *buf, size_t n) { return (int)__syscall(SYS_CLIPBOARD_GET, (long)buf, n, 0, 0, 0); }
int gui_launch(const char *path, const char *arg) {
    return (int)__syscall(SYS_GUI_LAUNCH, (long)path, (long)arg, 0, 0, 0);
}

int screen_grab(uint32_t *buf, size_t bytes, int *w, int *h) {
    long r = __syscall(SYS_SCREEN_GRAB, (long)buf, (long)bytes, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    *w = (int)(r >> 16);
    *h = (int)(r & 0xFFFF);
    return 0;
}
