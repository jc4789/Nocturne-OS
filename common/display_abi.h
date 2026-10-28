/* Display colour format and deep-colour (10 bits per channel) surfaces.
 *
 * A deep surface is the ordinary 0xAARRGGBB buffer holding the top 8 bits
 * of each channel, plus a second plane of one byte per pixel that holds the
 * 2 low bits of R, G and B. A reader that ignores the second plane still sees
 * a correct 8-bit picture (an 8 bpc display, RDP, screen_grab, screenshots).
 * The compositor only consumes deep planes when the display itself has 10 or
 * more bits per channel; on an 8 bpc display producers dither instead. */
#pragma once
#include <stdint.h>
#include "abi.h"

/* SYS_SCREEN_INFO with its second argument equal to sizeof(struct
   screen_format) fills this whole structure. A zero second argument keeps the
   original struct screen_info reply, so existing programs are unaffected. */
struct screen_format {
    uint32_t width, height;     /* the work area, as struct screen_info */
    uint32_t bits_per_pixel;    /* scanout pixel size: 16, 24 or 32 */
    uint8_t red_bits, green_bits, blue_bits;
    uint8_t bits_per_channel;   /* smallest of the three: 8 or 10 (or more) */
};

/* Desktop settings, read and changed by the Settings app: SYS_SCREEN_INFO with
   its third argument SCREEN_SETTINGS_GET or SCREEN_SETTINGS_SET and its second
   argument sizeof(struct desktop_settings). A change applies at once, and the
   desktop keeps it across restarts in the user's AppData\Nocturne\Desktop.conf,
   which is no interface: programs read and change them here, and hear a change
   by SCREEN_WATCH. */
#define SCREEN_SETTINGS_GET 1
#define SCREEN_SETTINGS_SET 2
struct desktop_settings {
    int8_t pointer_speed; /* -2 (slowest) .. 2 (fastest), 0 as delivered */
    /* The desktop drawn with 10 bits per channel and dithered onto a screen of 8 (less banding in
       gradients, photos and video, for some processor time). The user's wish: it is in effect
       while the screen shown takes it (smooth_gradients_available). */
    uint8_t smooth_gradients;
    /* read only: the screen shown is 8 bits per channel xRGB, the one the dither is made for */
    uint8_t smooth_gradients_available;
    uint8_t reserved[5];
};

/* The monitor's mode (resolution and refresh rate), read and changed by the Settings app:
   SYS_SCREEN_INFO with its third argument SCREEN_MODES_GET and its second sizeof(struct
   screen_modes), or SCREEN_MODE_SET and sizeof(struct screen_mode_set). The modes are the display
   driver's (n_pci_screen_modes, abi.h); the system chooses among them: a mode on trial, else the
   user's choice for that monitor, else the monitor's preferred mode. Without a display driver's
   modes (count 0) the screen keeps the mode it has, offered then says which (the monitor and its
   name from the EDID the firmware read, "" without one; shown the mode of the screen shown), and
   SCREEN_MODE_SET's TRY, KEEP and BACK say N_ERR_NOT_FOUND. While the firmware's framebuffer is
   shown, shown is the firmware's mode whether or not modes are offered, its refresh 0 where no
   timing of the EDID names that size. source says what decides the desktop's size, chosen the
   mode the user chose for the monitor offered names (all 0: none), whether or not it is offered
   now. A program built before chosen passes SCREEN_MODES_FIRST_BYTES and gets that much. */
#define SCREEN_MODES_GET 3
#define SCREEN_MODE_SET 4
/* SYS_SCREEN_INFO with its third argument SCREEN_WATCH (the first two 0): a watch (N_KIND_WATCH,
   abi.h), receivable once anything SCREEN_SETTINGS_GET, the struct screen_format reply or
   SCREEN_MODES_GET reports has changed, whoever changed it: the desktop's size and settings,
   whether smooth gradients can be shown, the monitor offered and its modes, the mode wanted, a
   trial beginning or ending, a refusal, the source, the user's choice. A trial's time counting
   down is no change. N_ERR_INVALID when the first two are not 0, N_ERR_UNSUPPORTED without the
   desktop (the nogui start). */
#define SCREEN_WATCH 5
#define SCREEN_MODE_TRIAL_MS 15000u /* a mode tried and not kept goes back after this */
enum {
    SCREEN_SOURCE_FIRMWARE,       /* the firmware's mode: no display driver has run */
    SCREEN_SOURCE_DRIVER,         /* a display driver's mode (count 0: it finds no monitor) */
    /* The firmware's mode again while the display driver still runs: it gave the screen back
       (a mode its display was not seen to show), or has not shown its own yet.
       While its modes are offered (count), a mode tried asks it to show that one again. */
    SCREEN_SOURCE_HANDED_BACK,
    SCREEN_SOURCE_DRIVER_STOPPED, /* the firmware's mode again: the display driver ended */
    SCREEN_SOURCE_REMOTE,         /* a remote viewer's window (Remote Desktop) */
};
struct screen_modes {
    struct n_screen_modes offered; /* the driver's monitor, the modes and the one it shows */
    struct n_screen_mode wanted;   /* the one the system asks it to show */
    uint32_t trial_ms;             /* the mode on trial goes back after this; 0: none is on trial */
    uint32_t refused;              /* the mode tried last could not be shown: the one before is back */
    uint32_t source;               /* SCREEN_SOURCE_* */
    uint32_t reserved;
    struct n_screen_mode chosen;   /* the user's choice for offered.monitor (all 0: none) */
};
#define SCREEN_MODES_FIRST_BYTES 2176u /* the first struct screen_modes: offered to reserved */
/* TRY: mode (one offered, as listed) is shown on trial, and goes back to the mode before it
   unless KEEP comes within SCREEN_MODE_TRIAL_MS (BACK: at once). Another TRY during a trial
   keeps the mode to go back to. KEEP makes it the user's choice for that monitor, which the
   system keeps across restarts (in the user's AppData\Nocturne\Displays.conf, no interface) and
   shows whenever that monitor is driven and offers it. 4 stays unassigned: it put a saved choice
   back, which the system now does itself. */
enum { SCREEN_MODE_TRY = 1, SCREEN_MODE_KEEP, SCREEN_MODE_BACK };
struct screen_mode_set {
    uint32_t how;              /* SCREEN_MODE_* */
    uint32_t reserved;
    char monitor[32];          /* unused: 0 */
    struct n_screen_mode mode; /* TRY: width, height and refresh_mhz (flags ignored) */
};

/* Window flag. The window buffer carries a deep plane of w*h bytes directly
   after its w*h pixels. The compositor reads it only while the desktop has 10
   or more bits per channel, which can change at run time (Settings). */
#define WIN_DEEP_COLOR 64

/* Deep byte: bit 7 says the low bits are valid, bits 5:4 R, 3:2 G, 1:0 B.
   An invalid (zero) byte means "8-bit content": the display widens the
   8-bit value by bit replication, exactly as for a surface with no plane. */
#define DEEP_VALID 0x80u
static inline uint8_t deep_pack(unsigned r10, unsigned g10, unsigned b10) {
    return (uint8_t)(DEEP_VALID | (r10 & 3u) << 4 | (g10 & 3u) << 2 | (b10 & 3u));
}
static inline uint32_t deep_pixel(unsigned r10, unsigned g10, unsigned b10) {
    return 0xff000000u | (r10 >> 2) << 16 | (g10 >> 2) << 8 | (b10 >> 2);
}
/* 8-bit channel widened to 10 bits: 0 -> 0, 255 -> 1023. */
static inline unsigned deep_widen(unsigned v8) { return v8 << 2 | v8 >> 6; }

/* 4x4 ordered-dither (Bayer) threshold in 0..15 for reducing extra precision
   to 8 bits on an 8 bpc display: add threshold*2^(n-4) to an n-bit fraction. */
static inline unsigned dither_threshold(unsigned x, unsigned y) {
    return (unsigned)(0x5D7F91B36E4CA280ull >> (((y & 3u) * 4u + (x & 3u)) * 4u)) & 15u;
}
