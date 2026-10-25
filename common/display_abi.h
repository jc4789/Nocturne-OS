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
   argument sizeof(struct desktop_settings). Changes apply at once; keeping
   them across restarts is the Settings app's job (its saved file). */
#define SCREEN_SETTINGS_GET 1
#define SCREEN_SETTINGS_SET 2
enum { POINTER_Y_AUTO, POINTER_Y_NORMAL, POINTER_Y_INVERT };
struct desktop_settings {
    /* The desktop's own frame cadence while the display gives no vertical blanks (the firmware's
       framebuffer has none; a display driver's vertical blanks pace the frames instead). Not the
       monitor's refresh rate, which is the mode's (struct screen_modes). 0 draws as soon as
       anything changes. */
    uint32_t refresh_hz;
    int8_t pointer_speed;    /* -2 (slowest) .. 2 (fastest), 0 as delivered */
    uint8_t pointer_y;       /* POINTER_Y_* for a PS/2 mouse */
    uint8_t deep_simulation; /* run the 10 bpc pipeline, dithered, on an 8 bpc display */
    /* read only: what the system found */
    uint8_t pointer_y_detected;        /* POINTER_Y_AUTO inverts the Y axis (Hyper-V on Windows 11) */
    uint8_t deep_simulation_available; /* the display is 8 bpc 32-bit xRGB */
    uint8_t native_bits_per_channel;   /* the display's own depth */
    uint8_t reserved[6];
};

/* The monitor's mode (resolution and refresh rate), read and changed by the Settings app:
   SYS_SCREEN_INFO with its third argument SCREEN_MODES_GET and its second sizeof(struct
   screen_modes), or SCREEN_MODE_SET and sizeof(struct screen_mode_set). The modes are the display
   driver's (n_pci_screen_modes, abi.h); the system chooses among them: a mode on trial, else the
   user's choice for that monitor, else the monitor's preferred mode. Without a display driver
   there are none (count 0): the screen keeps the firmware's mode, and SCREEN_MODE_SET's TRY,
   KEEP and BACK say N_ERR_NOT_FOUND. */
#define SCREEN_MODES_GET 3
#define SCREEN_MODE_SET 4
#define SCREEN_MODE_TRIAL_MS 15000u /* a mode tried and not kept goes back after this */
struct screen_modes {
    struct n_screen_modes offered; /* the driver's monitor, the modes and the one it shows */
    struct n_screen_mode wanted;   /* the one the system asks it to show */
    uint32_t trial_ms;             /* the mode on trial goes back after this; 0: none is on trial */
    uint32_t refused;              /* the mode tried last could not be shown: the one before is back */
};
/* TRY: mode (one offered, as listed) is shown on trial, and goes back to the mode before it
   unless KEEP comes within SCREEN_MODE_TRIAL_MS (BACK: at once). Another TRY during a trial
   keeps the mode to go back to. KEEP makes it the user's choice for that monitor until a
   restart (keeping it longer is the Settings app's job: its saved file and SAVED at startup).
   SAVED: mode is the user's choice for the monitor named in monitor, shown whenever that monitor
   is driven and offers it. */
enum { SCREEN_MODE_TRY = 1, SCREEN_MODE_KEEP, SCREEN_MODE_BACK, SCREEN_MODE_SAVED };
struct screen_mode_set {
    uint32_t how;              /* SCREEN_MODE_* */
    uint32_t reserved;
    char monitor[32];          /* SAVED: the monitor's struct n_screen_modes monitor text */
    struct n_screen_mode mode; /* TRY, SAVED: width, height and refresh_mhz (flags ignored) */
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
