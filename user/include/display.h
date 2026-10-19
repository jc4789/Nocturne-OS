#pragma once
/* The display's colour format and deep-colour (10 bpc) window surfaces.
 * Layout and conventions: display_abi.h. */
#include "display_abi.h"
#include "nocturne.h"

/* The work area and colour format. 0, or -1 with errno set. */
int screen_format(struct screen_format *out);
/* Bits per channel the desktop delivers: 8, or 10 on a deep display. */
int display_bits_per_channel(void);
/* The deep plane of the window's current canvas (one byte per pixel, pitch
   win->w), or NULL when the window was opened without WIN_DEEP_COLOR or the
   desktop has 8 bits per channel (which Settings can change at any time).
   Like win->c.px it changes with each
   win_begin_frame and resize, so ask again after either. */
uint8_t *win_deep(window_t *win);
