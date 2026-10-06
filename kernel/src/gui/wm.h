#pragma once
#include "kernel.h"
#include "gfx.h"

void wm_init(void);           /* start the compositor thread */
bool wm_running(void);
void wm_screen_size(uint32_t *w, uint32_t *h); /* the desktop's size, which a remote viewer may have changed */
void wm_process_exit(int pid);
int64_t wm_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
void wm_notify(const char *title, const char *text); /* desktop toast */

/* Remote display (the RDP server). While attached, the screen is composited without the
   pointer (the remote side draws its own) and every redrawn area is recorded. The frame is
   only consistent while the compositor thread is not running, which in this kernel means:
   read it from another kernel thread between blocking calls. */
struct wm_rect {
    int x, y, w, h;
};
/* *w, *h: the size the viewer would like (0: keep the current one); returns the size it gets.
   The desktop goes back to the framebuffer's size when the last viewer detaches. */
bool wm_remote_attach(int *w, int *h);
void wm_remote_detach(void);
int wm_remote_damage(struct wm_rect *out, int max); /* takes the areas redrawn since the last call */
const uint32_t *wm_remote_frame(int *pitch);
int wm_cursor_shape(void);

/* desktop.c */
void desktop_draw_wallpaper(canvas_t *c);
void desktop_draw_icon(canvas_t *c, int x, int y, int icon);
void desktop_draw_cursor(canvas_t *c, int x, int y, int shape);
enum { ICON_TERMINAL, ICON_FILES, ICON_PAINT, ICON_CLOCK, ICON_SNAKE, ICON_FRACTAL, ICON_CUBE,
       ICON_LIFE, ICON_MONITOR, ICON_EDITOR, ICON_ABOUT, ICON_CALC, ICON_POWER, ICON_REBOOT,
       ICON_TETRIS, ICON_MINES, ICON_NET, ICON_GENERIC, ICON_COUNT };
enum { CURSOR_ARROW, CURSOR_RESIZE, CURSOR_MOVE };
