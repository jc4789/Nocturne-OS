/* Client side of the Nocturne window system. Results come back as they are: a value, or a
   negative n_result. A window's surfaces are shared objects the app maps (SYS_WIN_SURFACE). */
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

n_handle n_screen_watch(void) { return (n_handle)__syscall(SYS_SCREEN_INFO, 0, 0, SCREEN_WATCH, 0, 0); }

int display_bits_per_channel(void) {
    /* not cached: Settings can switch the desktop between 8 and 10 bpc */
    struct screen_format f;
    return screen_format(&f) == 0 && f.bits_per_channel ? f.bits_per_channel : 8;
}

uint8_t *win_deep(window_t *win) {
    if (!win || !win->c.px || !(win->flags & WIN_DEEP_COLOR) || display_bits_per_channel() < 10) return NULL;
    return (uint8_t *)(win->c.px + (size_t)win->w * win->h);
}

/* The window's surfaces of w x h mapped into this app (SYS_WIN_SURFACE); each handle is closed
   after its mapping, which keeps the pages. */
static size_t surface_bytes(int w, int h, int flags) {
    size_t bytes = (size_t)w * h * ((flags & WIN_DEEP_COLOR) ? 5 : 4);
    return (bytes + 4095) & ~(size_t)4095;
}

static void surfaces_unmap(window_t *win) {
    for (unsigned i = 0; i < win->nsurf; i++)
        if (win->surface[i]) n_memory_release(win->surface[i], win->surface_bytes);
    memset(win->surface, 0, sizeof win->surface);
    win->c.px = NULL;
}

static n_result surfaces_map(window_t *win, int w, int h) {
    size_t bytes = surface_bytes(w, h, win->flags);
    win->surface_bytes = bytes;
    for (unsigned i = 0; i < win->nsurf; i++) {
        n_result handle = __syscall(SYS_WIN_SURFACE, win->handle, i, 0, 0, 0);
        if (handle < 0) return handle;
        void *p = NULL;
        n_result r = n_shared_map((n_handle)handle, bytes, N_MEMORY_READ_WRITE, &p);
        n_handle_close((n_handle)handle);
        if (r < 0) return r;
        win->surface[i] = p;
    }
    win->w = w;
    win->h = h;
    win->current = -1;
    if (win->flags & WIN_FRAME_QUEUE) win->c = (canvas_t){0};
    else gfx_init(&win->c, win->surface[0], w, h, w);
    return 0;
}

n_result win_open(int w, int h, const char *title, int flags, window_t **out) {
    *out = NULL;
    /* a mailbox keeps one shown, one waiting and one being drawn */
    unsigned count = (flags & WIN_FRAME_QUEUE) ? ((flags & WIN_MAILBOX) ? 3 : 2) : 1;
    n_result handle = __syscall(SYS_WIN_CREATE, w, h, (long)title, flags, (flags & WIN_FRAME_QUEUE) ? count : 0);
    if (handle < 0) return handle;
    window_t *win = calloc(1, sizeof *win);
    if (!win) { n_handle_close((n_handle)handle); return -N_ERR_NO_MEMORY; }
    win->handle = (n_handle)handle;
    win->flags = flags;
    win->nsurf = count;
    n_result r = surfaces_map(win, w, h);
    if (r < 0) { win_close(win); return r; }
    *out = win;
    return 0;
}

void win_close(window_t *win) {
    if (!win) return;
    n_handle_close(win->handle);
    surfaces_unmap(win);
    free(win);
}

n_result win_begin_frame(window_t *win) {
    if (!win || win->closed) return -N_ERR_NO_HANDLE;
    if (!(win->flags & WIN_FRAME_QUEUE) || win->current >= 0) return 0;
    n_result index = __syscall(SYS_WIN_ACQUIRE, win->handle, (long)N_CLOCK_NEVER, 0, 0, 0);
    if (index < 0) return index;
    win->current = (int)index;
    gfx_init(&win->c, win->surface[index], win->w, win->h, win->w);
    return 0;
}

static n_result presented(window_t *win, n_result number) {
    if (number < 0) return number;
    win->presented = (uint64_t)number;
    return 0;
}

n_result win_update(window_t *win) {
    if (!win || win->closed) return -N_ERR_NO_HANDLE;
    if (!(win->flags & WIN_FRAME_QUEUE)) return presented(win, __syscall(SYS_WIN_PRESENT, win->handle, 0, 0, 0, 0));
    if (win->current < 0) return -N_ERR_BUSY;
    n_result r = presented(win, __syscall(SYS_WIN_PRESENT, win->handle, 0, 0, 0, win->current + 1));
    if (r < 0) return r;
    win->current = -1;
    win->c.px = NULL; /* a presented surface is the system's until EV_SURFACE gives it back */
    return 0;
}

n_result win_update_rect(window_t *win, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    if (win->flags & WIN_FRAME_QUEUE) return win_update(win);
    return presented(win, __syscall(SYS_WIN_PRESENT, win->handle, x, y, ((long)w << 32) | (unsigned)h, 0));
}

n_result win_resize(window_t *win, int w, int h) {
    if (!win || win->closed) return -N_ERR_NO_HANDLE;
    n_result r = __syscall(SYS_WIN_RESIZE, win->handle, w, h, 0, 0);
    if (r < 0) return r;
    surfaces_unmap(win);
    r = surfaces_map(win, w, h);
    if (r < 0) win->closed = true;
    return r;
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
n_result win_pointer(window_t *win, const struct n_win_pointer *pointer) {
    if (!win || win->closed) return -N_ERR_NO_HANDLE;
    return __syscall(SYS_WIN_POINTER, win->handle, (uint64_t)(uintptr_t)pointer, sizeof *pointer, 0, 0);
}
n_result win_pointer_shape(window_t *win, uint32_t shape) {
    struct n_win_pointer pointer = {.shape = shape};
    return win_pointer(win, &pointer);
}

/* One system call takes every queued event that fits, waiting until deadline. */
static n_result fetch(window_t *win, uint64_t deadline) {
    if (win->closed) return -N_ERR_CLOSED;
    n_result n = __syscall(SYS_WIN_RECEIVE, win->handle, (long)win->q,
                           sizeof win->q / sizeof win->q[0], (long)deadline, 0);
    if (n == 0 || n == -N_ERR_TIMEOUT || n == -N_ERR_STOPPED) return 0;
    if (n < 0) {
        win->closed = true;
        return n;
    }
    win->qn = (int)n;
    win->qi = 0;
    return 1;
}

n_result win_event(window_t *win, struct gui_event *e, uint64_t deadline) {
    if (win->qi >= win->qn) {
        n_result r = fetch(win, deadline);
        if (r <= 0) return r;
    }
    *e = win->q[win->qi++];
    if (e->type == EV_RESIZE) {
        n_result r = win_resize(win, e->x, e->y);
        if (r < 0 && win->closed) return r;
        e->x = win->w; /* without memory for the new size it keeps the old one */
        e->y = win->h;
        /* surfaces given back before the resize went with the old ones */
        int kept = win->qi;
        for (int i = win->qi; i < win->qn; i++)
            if (win->q[i].type != EV_SURFACE) win->q[kept++] = win->q[i];
        win->qn = kept;
    }
    return 1;
}

n_result clipboard_set(const char *s, size_t n) { return __syscall(SYS_CLIPBOARD_SET, (long)s, n, 0, 0, 0); }
n_result clipboard_get(char *buf, size_t n) { return __syscall(SYS_CLIPBOARD_GET, (long)buf, n, 0, 0, 0); }
n_result gui_launch(const char *path, const char *arg) {
    char full[4096];
    if (!find_program(path, full, sizeof full)) return -N_ERR_NOT_FOUND;
    const char *arguments[2] = {full, arg};
    struct n_launch launch = {.arguments = arguments, .argument_count = arg ? 2 : 1};
    n_handle app = n_app_launch(full, &launch);
    if (app < 0) return app;
    n_result id = n_app_id(app);
    n_handle_close(app);
    return id;
}

n_result n_shell_place(n_handle cap, window_t *win, const struct n_shell_place *place) {
    return __syscall(SYS_SHELL_PLACE, cap, win->handle, (long)place, sizeof *place, 0);
}
n_result n_shell_screens(n_handle cap, struct n_shell_screen *out, size_t max) {
    return __syscall(SYS_SHELL_SCREENS, cap, (long)out, (long)max, 0, 0);
}
n_result n_shell_windows(n_handle cap, struct n_shell_window *out, size_t max) {
    return __syscall(SYS_SHELL_WINDOWS, cap, (long)out, (long)max, 0, 0);
}
n_result n_shell_window(n_handle cap, uint32_t id, uint32_t op) { return __syscall(SYS_SHELL_WINDOW, cap, id, op, 0, 0); }
n_handle n_shell_watch(n_handle cap) { return (n_handle)__syscall(SYS_SHELL_WATCH, cap, 0, 0, 0, 0); }
n_result n_shell_hotkey(n_handle cap, window_t *win, uint32_t key, uint32_t mods) {
    return __syscall(SYS_SHELL_HOTKEY, cap, win->handle, key, mods, 0);
}
n_result n_power_state(n_handle cap, struct n_power_state *out) {
    return __syscall(SYS_POWER_STATE, cap, (long)out, sizeof *out, 0, 0);
}
n_handle n_power_watch(n_handle cap) { return (n_handle)__syscall(SYS_POWER_WATCH, cap, 0, 0, 0, 0); }

n_result n_shell_notify(const char *title, const char *text) {
    struct n_shell_notice notice = {0};
    strlcpy(notice.title, title, sizeof notice.title);
    if (text) strlcpy(notice.text, text, sizeof notice.text);
    n_handle channel = n_service_connect(N_SHELL_SERVICE);
    if (channel < 0) return channel;
    n_result r = n_channel_send(channel, &notice, sizeof notice, -1);
    n_handle_close(channel);
    return r;
}

n_result screen_grab(n_handle cap, uint32_t *buf, size_t bytes, int *w, int *h) {
    n_result r = __syscall(SYS_SCREEN_GRAB, cap, (long)buf, (long)bytes, 0, 0);
    if (r < 0) return r;
    *w = (int)(r >> 16);
    *h = (int)(r & 0xFFFF);
    return 0;
}
