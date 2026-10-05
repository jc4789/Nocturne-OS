#pragma once
#include "kernel.h"
#include "gfx.h"

void wm_init(void);           /* start the compositor thread */
bool wm_running(void);
void wm_process_exit(int pid);
int64_t wm_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
void wm_notify(const char *title, const char *text); /* desktop toast */

/* desktop.c */
void desktop_draw_wallpaper(canvas_t *c);
void desktop_draw_icon(canvas_t *c, int x, int y, int icon);
void desktop_draw_cursor(canvas_t *c, int x, int y, int shape);
enum { ICON_TERMINAL, ICON_FILES, ICON_PAINT, ICON_CLOCK, ICON_SNAKE, ICON_FRACTAL, ICON_CUBE,
       ICON_LIFE, ICON_MONITOR, ICON_EDITOR, ICON_ABOUT, ICON_CALC, ICON_POWER, ICON_REBOOT,
       ICON_TETRIS, ICON_MINES, ICON_NET, ICON_GENERIC, ICON_COUNT };
enum { CURSOR_ARROW, CURSOR_RESIZE, CURSOR_MOVE };
