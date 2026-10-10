#pragma once
#include "kernel.h"
#include "gfx.h"

void wm_init(void);           /* start the compositor thread */
bool wm_running(void);
void wm_screen_size(uint32_t *w, uint32_t *h); /* the desktop's size, which a remote viewer may have changed */

/* UTF-8 clipboard metadata or a caller-owned snapshot (release with kfree).
   Never lend mutable storage across BSP timer preemption. */
#define WM_CLIPBOARD_MAX (1 << 20)
bool wm_clipboard_set(const char *text, size_t n);
void wm_clipboard_info(size_t *len, uint32_t *seq);
char *wm_clipboard_snapshot(size_t *len);
void wm_process_exit(int pid);
int64_t wm_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
void wm_notify(const char *title, const char *text); /* desktop toast */

/* Remote display: rectangle copies hold WM ownership only for the copy, never
   while encoding or sending a network packet. */
struct wm_rect {
    int x, y, w, h;
};
/* *w, *h: the size the viewer would like (0: keep the current one); returns the size it gets.
   The desktop goes back to the framebuffer's size when the last viewer detaches. */
bool wm_remote_attach(int *w, int *h);
void wm_remote_resize(int *w, int *h); /* the viewer's window changed size: same rules */
void wm_remote_detach(void);
int wm_remote_damage(struct wm_rect *out, int max); /* takes the areas redrawn since the last call */
bool wm_remote_copy(uint32_t *dst, int pitch, int x, int y, int w, int h);
int wm_cursor_shape(void);

/* desktop.c */
void desktop_draw_wallpaper(canvas_t *c);
void desktop_draw_icon(canvas_t *c, int x, int y, int icon);
void desktop_draw_cursor(canvas_t *c, int x, int y, int shape);
enum { ICON_TERMINAL, ICON_FILES, ICON_PAINT, ICON_CLOCK, ICON_SNAKE, ICON_FRACTAL, ICON_CUBE,
       ICON_LIFE, ICON_MONITOR, ICON_EDITOR, ICON_ABOUT, ICON_CALC, ICON_POWER, ICON_REBOOT,
       ICON_TETRIS, ICON_MINES, ICON_NET, ICON_PIANO, ICON_SOUND, ICON_GENERIC, ICON_COUNT };
enum { CURSOR_ARROW, CURSOR_RESIZE, CURSOR_MOVE };
