#pragma once
#include "kernel.h"
#include "gfx.h"

void wm_init(void);           /* start the compositor thread */
bool wm_running(void);
void wm_screen_size(uint32_t *w, uint32_t *h); /* the desktop's size, which a remote viewer may have changed */
/* The scanout the screen is drawn into is about to be, or now is, w x h (dev/pciuser.c: a display
   driver's mode): the desktop takes that size, its fullscreen and maximised windows are told
   (EV_RESIZE), unless remote viewers have it at theirs (it takes the scanout's when the last one
   leaves). False when the desktop's planes do not fit in memory at that size: it keeps its own. */
bool wm_screen_resized(uint32_t w, uint32_t h);

/* UTF-8 clipboard metadata or a caller-owned snapshot (release with kfree).
   Never lend mutable storage across BSP timer preemption. */
#define WM_CLIPBOARD_MAX (1 << 20)
bool wm_clipboard_set(const char *text, size_t n);
void wm_clipboard_info(size_t *len, uint32_t *seq);
char *wm_clipboard_snapshot(size_t *len);
void wm_process_exit(int pid);
int64_t wm_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
void wm_notify(const char *title, const char *text); /* desktop toast */
/* Power changes (dev/pm.c): how many app windows are open (the top one's
   title in top_title), and ask every one of them to close. */
int wm_window_count(char *top_title, size_t n);
void wm_close_windows(void);
void wm_repaint(void); /* the screen lost what was on it (after sleep): draw all of it again */
/* A vertical blank of the display driver's scanout the screen is drawn into (dev/pciuser.c): count
   is the driver's count of blanks, timestamp_ns its time of this one. While blanks come, frames
   start at them instead of after the configured period. Cheap: it wakes the compositor only when
   a frame waits for the blank. */
void wm_vblank(uint64_t count, uint64_t timestamp_ns);
/* Direct scanout (dev/pciuser.c): whether the display driver whose scanout the screen is drawn
   into can show a window's own images (N_SCANOUT_DIRECT), and what the screen should show: waits up to
   timeout_ms (-1: no limit) while the state is still numbered seen, then copies it to *out (kernel
   memory). N_ERR_TIMEOUT with *out the unchanged state; N_ERR_STOPPED when the caller is stopped. */
struct n_screen_state;
void wm_screen_direct(bool able);
int64_t wm_screen_state(uint64_t seen, struct n_screen_state *out, int64_t timeout_ms);
/* The mode the system wants the display driver to show (gui/screenmode.c), wish number serial:
   into the state the driver reads, unless a later wish is there already. */
struct n_screen_mode;
void wm_screen_mode(const struct n_screen_mode *mode, uint64_t serial);
/* The boot screen is where the screen is drawn again (dev/pciuser.c): a Ctrl+Alt+Esc request to
   the display driver for it (struct n_screen_state take_back) is over. */
void wm_screen_given_back(void);

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
void desktop_draw_wallpaper(canvas_t *c, uint8_t *deep); /* deep plane (display_abi.h) or NULL */
void desktop_draw_icon(canvas_t *c, int x, int y, int icon);
void desktop_draw_cursor(canvas_t *c, int x, int y, int shape);
enum { ICON_TERMINAL, ICON_FILES, ICON_PAINT, ICON_CLOCK, ICON_SNAKE, ICON_FRACTAL, ICON_CUBE,
       ICON_LIFE, ICON_MONITOR, ICON_EDITOR, ICON_ABOUT, ICON_CALC, ICON_POWER, ICON_REBOOT,
       ICON_TETRIS, ICON_MINES, ICON_NET, ICON_PIANO, ICON_SOUND, ICON_SETTINGS, ICON_SLEEP, ICON_HIBERNATE, ICON_GENERIC,
       ICON_COUNT };
enum { CURSOR_ARROW, CURSOR_RESIZE, CURSOR_MOVE };
