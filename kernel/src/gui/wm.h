#pragma once
#include "kernel.h"
#include "gfx.h"

/* Start the desktop: its input thread and a compositor thread for each screen (dev/fb.h). The
   desktop's settings (the pointer's speed, smooth gradients) are the user's: they come through
   sys/settings.h, and the kernel does not save them. */
void wm_init(void);
bool wm_running(void);
/* Something SYS_SCREEN_INFO reports changed (a screen's mode, gui/screenmode.c): its watches
   (SCREEN_WATCH, display_abi.h) hear of it. Any CPU; it does not sleep, so the caller may hold a
   lock (watched_changed, sys/watch.c). Nothing without the desktop. */
void wm_screen_info_changed(void);
/* The size of screen 0's part of the desktop (the one with the taskbar), which a remote viewer may
   have changed. */
void wm_screen_size(uint32_t *w, uint32_t *h);

/* The UTF-8 clipboard, any CPU, under the WM lock: a call on another CPU may replace it at any
   time, so it is read as a caller-owned snapshot (release with kfree). wm_clipboard_set copies
   text (kernel memory). */
#define WM_CLIPBOARD_MAX (1 << 20)
bool wm_clipboard_set(const char *text, size_t n);
void wm_clipboard_info(size_t *len, uint32_t *seq);
char *wm_clipboard_snapshot(size_t *len);
/* The window and screen calls, any CPU. */
int64_t wm_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
void wm_notify(const char *title, const char *text); /* desktop toast */
/* Power changes (dev/pm.c): how many app windows are open (the top one's
   title in top_title), and ask every one of them to close. */
int wm_window_count(char *top_title, size_t n);
void wm_close_windows(void);
void wm_repaint(void); /* the screens lost what was on them (after sleep, a hold): draw all of them again */
/* What the taskbar shows of state the desktop does not own changed by another's hand: the volume or
   the muting (dev/audio.c), the wall clock set (sys/clock.c). The desktop looks at them again;
   without this call it would see them only at the next minute. Any CPU, any context, without
   sleeping; nothing without the desktop. */
void wm_taskbar_changed(void);
/* A display driver's screen (dev/pciuser.c), by its number (dev/fb.h): whether its buffers can
   show a window's own images (N_SCANOUT_DIRECT), and what the screen should show: wm_screen_state
   waits until deadline (sys/wait.h) while the state is still numbered seen, then copies it to *out
   (kernel memory). N_ERR_TIMEOUT with *out the unchanged state; N_ERR_STOPPED when the caller is
   stopped. They work without the desktop too (the nogui start). */
struct n_screen_state;
void wm_screen_direct(unsigned screen, bool able);
int64_t wm_screen_state(unsigned screen, uint64_t seen, struct n_screen_state *out, uint64_t deadline);
/* The mode the system wants the screen's driver to show (gui/screenmode.c), wish number serial:
   into the state the driver reads, unless a later wish is there already. */
struct n_screen_mode;
void wm_screen_mode(unsigned screen, const struct n_screen_mode *mode, uint64_t serial);

/* Remote display (vm/rdp.c): screen 0's part of the desktop, at the viewer's window's size while a
   viewer is attached. The copies take the screen's picture lock only for the copy, never while
   encoding or sending a network packet. */
struct wm_rect {
    int x, y, w, h;
};
/* *w, *h: the size the viewer would like (0: keep the current one); returns the size it gets.
   Screen 0's part goes back to the screen's size when the last viewer detaches. */
bool wm_remote_attach(int *w, int *h);
void wm_remote_resize(int *w, int *h); /* the viewer's window changed size: same rules */
void wm_remote_detach(void);
bool wm_remote_viewing(void); /* a viewer has the desktop at its size; takes no lock */
/* Takes the areas redrawn since the last call, at most max (the rest joined into the last). */
int wm_remote_damage(struct wm_rect *out, int max);
bool wm_remote_copy(uint32_t *dst, int pitch, int x, int y, int w, int h);
/* The pointer the desktop shows now (a remote viewer draws it itself). Returns its serial, which
   changes with the image and is never 0; when that is not seen, its image (a square of
   N_SCREEN_POINTER_SIZE pixels of 0xAARRGGBB not premultiplied) goes into px and its hot spot into
   *hot_x, *hot_y, under the WM lock. */
uint32_t wm_pointer_image(uint32_t seen, uint32_t *px, int *hot_x, int *hot_y);

/* desktop.c */
void desktop_draw_wallpaper(canvas_t *c, uint8_t *deep); /* deep plane (display_abi.h) or NULL */
void desktop_draw_icon(canvas_t *c, int x, int y, int icon);
/* The art of a pointer shape (N_POINTER_*, abi.h; another number is N_POINTER_DEFAULT): a square
   of N_SCREEN_POINTER_SIZE pixels of 0xAARRGGBB not premultiplied, and its hot spot. Drawn the
   first time it is asked for and kept. Under the WM lock. */
const uint32_t *desktop_pointer(int shape, int *hot_x, int *hot_y);
enum { ICON_TERMINAL, ICON_FILES, ICON_PAINT, ICON_CLOCK, ICON_SNAKE, ICON_FRACTAL, ICON_CUBE,
       ICON_LIFE, ICON_MONITOR, ICON_EDITOR, ICON_ABOUT, ICON_CALC, ICON_POWER, ICON_REBOOT,
       ICON_TETRIS, ICON_MINES, ICON_NET, ICON_PIANO, ICON_SOUND, ICON_SETTINGS, ICON_SLEEP, ICON_HIBERNATE, ICON_GENERIC,
       ICON_COUNT };
