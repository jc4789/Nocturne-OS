#pragma once
/* The display's colour format and deep-colour (10 bpc) window surfaces.
 * Layout and conventions: display_abi.h. */
#include "display_abi.h"
#include "nocturne.h"

/* The work area and colour format. 0, or a negative n_result. */
n_result screen_format(struct screen_format *out);
/* A watch (n_watch_take) on what SYS_SCREEN_INFO reports (SCREEN_WATCH, display_abi.h). */
n_handle n_screen_watch(void);
/* Bits per channel the desktop delivers: 8, or 10 on a deep display. */
int display_bits_per_channel(void);
/* The deep plane of the window's current canvas (one byte per pixel, pitch
   win->w), or NULL when the window was opened without WIN_DEEP_COLOR or the
   desktop has 8 bits per channel (which Settings can change at any time).
   Like win->c.px it changes with each
   win_begin_frame and resize, so ask again after either. */
uint8_t *win_deep(window_t *win);

/* ---- シェルの口（display_abi.h）。cap は N_CAP_SHELL。 ---- */
n_result n_shell_place(n_handle cap, window_t *win, const struct n_shell_place *place);
/* 一覧の数を返し、max 件まで out へ写す。 */
n_result n_shell_screens(n_handle cap, struct n_shell_screen *out, size_t max);
n_result n_shell_windows(n_handle cap, struct n_shell_window *out, size_t max);
n_result n_shell_window(n_handle cap, uint32_t id, uint32_t op);
n_handle n_shell_watch(n_handle cap);
n_result n_shell_hotkey(n_handle cap, window_t *win, uint32_t usage, uint32_t mods);
/* 電源の段取りの今と、その watch（struct n_power_state、abi.h）。 */
n_result n_power_state(n_handle cap, struct n_power_state *out);
n_handle n_power_watch(n_handle cap);

/* シェルへ通知を一通送る（サービス "shell"）。text は NULL でよい。 */
n_result n_shell_notify(const char *title, const char *text);
