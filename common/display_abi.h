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

/* Desktop settings as they stand: SYS_SCREEN_INFO with its third argument
   SCREEN_SETTINGS_GET and its second argument sizeof(struct desktop_settings). A
   program hears a change by SCREEN_WATCH. They change by SYS_SETTINGS_APPLY
   (N_WINDOW_SMOOTH_GRADIENTS, settings_abi.h). */
#define SCREEN_SETTINGS_GET 1
struct desktop_settings {
    /* The desktop drawn with 10 bits per channel and dithered onto a screen of 8 (less banding in
       gradients, photos and video, for some processor time). The user's wish: it is in effect
       while the screen shown takes it (smooth_gradients_available). */
    uint8_t smooth_gradients;
    /* read only: the screen shown is 8 bits per channel xRGB, the one the dither is made for */
    uint8_t smooth_gradients_available;
    uint8_t reserved[6];
};

/* The monitor's mode (resolution and refresh rate), which the Settings window reads:
   SYS_SCREEN_INFO with its third argument SCREEN_MODES_GET and its second sizeof(struct
   screen_modes). The modes are the display driver's (n_pci_screen_modes, abi.h); the system
   chooses among them: the user's choice for that monitor (N_DISPLAY_MODE, settings_abi.h), else the
   monitor's preferred mode. Without a display driver's modes (count 0) the screen keeps the mode it
   has, offered then says which (the monitor and its name from the EDID the firmware read, "" without
   one; shown the mode of the screen shown). While the firmware's framebuffer is shown, shown is the
   firmware's mode whether or not modes are offered, its refresh 0 where no timing of the EDID names
   that size. source says what decides the desktop's size, chosen the mode the user chose for the
   monitor offered names (all 0: none), whether or not it is offered now.
   Each screen has its own: the third argument is the op with the screen's number in its bits
   from 8 on (op | screen << 8), and screens says how many there are
   (an unknown screen: N_ERR_NOT_FOUND). */
#define SCREEN_MODES_GET 3
/* SYS_SCREEN_INFO with its third argument SCREEN_WATCH (the first two 0): a watch (N_KIND_WATCH,
   abi.h), receivable once anything SCREEN_SETTINGS_GET, the struct screen_format reply or
   SCREEN_MODES_GET reports has changed, whoever changed it: the desktop's size and settings,
   whether smooth gradients can be shown, the monitor offered and its modes, the mode wanted, a
   a refusal, the source, the user's choice. N_ERR_INVALID when the first two are not 0, N_ERR_UNSUPPORTED without the
   desktop. */
#define SCREEN_WATCH 5
#define SCREEN_MODE_TRIAL_MS 15000u /* the Settings window's: a mode tried and not kept goes back after this */
enum {
    SCREEN_SOURCE_FIRMWARE,       /* the firmware's mode: no display driver has run */
    SCREEN_SOURCE_DRIVER,         /* a display driver's mode (count 0: it offers no list; a monitor in offered, or none found) */
    /* The firmware's mode again while the display driver still runs: it gave the screen back
       (a mode its display was not seen to show), or has not shown its own yet.
       While its modes are offered (count), a mode applied asks it to show that one again. */
    SCREEN_SOURCE_HANDED_BACK,
    SCREEN_SOURCE_DRIVER_STOPPED, /* the firmware's mode again: the display driver ended */
    SCREEN_SOURCE_REMOTE,         /* a remote viewer's window (Remote Desktop) */
};
struct screen_modes {
    struct n_screen_modes offered; /* the driver's monitor, the modes and the one it shows */
    struct n_screen_mode wanted;   /* the one the system asks it to show */
    uint32_t refused;              /* the mode applied last could not be shown: the next best is shown */
    uint32_t source;               /* SCREEN_SOURCE_* */
    uint32_t screens;              /* the number of screens */
    struct n_screen_mode chosen;   /* the user's choice for offered.monitor (all 0: none) */
};
/* Window flag. The window buffer carries a deep plane of w*h bytes directly
   after its w*h pixels. The compositor reads it only while the desktop has 10
   or more bits per channel, which can change at run time (Settings). */
#define WIN_DEEP_COLOR 64

/* ---- シェルの口 ----
   カーネルは窓の管理者で、シェルは普通のプロセス。シェルは N_CAP_SHELL の能力で、自分の窓を層に置き、
   画面の端を予約し、窓の一覧を読み、窓を前に出す。タスクバーもメニューも壁紙もカーネルは知らない。 */

/* 窓の層。下から順に重なる。層の中では窓が前後する。 */
enum n_window_layer {
    N_LAYER_NORMAL,  /* アプリの窓 */
    N_LAYER_DESKTOP, /* 全部の窓より下。焦点は押されたときだけ受ける */
    N_LAYER_PANEL,   /* アプリの窓より上。焦点を受けない。同じ画面の全画面の窓が焦点を持つ間は隠れる */
    N_LAYER_OVERLAY, /* 一番上。ここに窓があれば直接の表示（EV_DIRECT）をやめる */
};

/* 画面の端（n_shell_place の reserve_edge）。 */
enum n_screen_edge { N_EDGE_NONE, N_EDGE_TOP, N_EDGE_BOTTOM, N_EDGE_LEFT, N_EDGE_RIGHT };

#define N_PLACE_NO_FOCUS 1u /* 焦点を受けない（N_LAYER_OVERLAY のとき） */
#define N_PLACE_BLEND 2u    /* 画素の alpha で下と混ぜる。隠さない所を持つ窓 */

/* SYS_SHELL_PLACE：飾りの無い自分の窓（WIN_NO_DECOR）を一度に決める。予約は窓が持ち、窓の終わりで
   消える。最大化の窓と SYS_SCREEN_INFO の作業域は予約の残りに従う。 */
struct n_shell_place {
    uint32_t layer;        /* enum n_window_layer */
    uint32_t screen;       /* 画面の番号 */
    int32_t x, y;          /* その画面の中の位置 */
    uint32_t reserve_edge; /* enum n_screen_edge */
    uint32_t reserve;      /* その端から予約する画素の数 */
    uint32_t flags;        /* N_PLACE_* */
    uint32_t reserved;
};

/* SYS_SHELL_SCREENS の一件。x、y、width、height は画面の部分。on が 0 の画面は部分を持たない。 */
struct n_shell_screen {
    int32_t x, y;
    int32_t width, height;
    uint32_t on;
    uint32_t reserved;
};

#define N_SHELL_WINDOW_MINIMIZED 1u
#define N_SHELL_WINDOW_FOCUSED 2u
#define N_SHELL_WINDOW_FULLSCREEN 4u
/* SYS_SHELL_WINDOWS の一件：アプリの飾りのある窓を、作られた順に。 */
struct n_shell_window {
    uint32_t id;     /* 窓の番号：使い回さない */
    int32_t app;     /* 窓を作ったアプリの番号（SYS_APP_OPEN） */
    uint32_t state;  /* N_SHELL_WINDOW_* */
    uint32_t screen; /* 窓の中心がある画面の番号 */
    char title[64];
};

/* SYS_SHELL_WINDOW の操作。 */
enum n_shell_window_op { N_SHELL_RAISE, N_SHELL_MINIMIZE, N_SHELL_CLOSE };

/* サービス "shell" のチャンネルへ送る通知。一通に一つ。text は無くてよい。 */
#define N_SHELL_SERVICE "shell"
struct n_shell_notice {
    uint32_t reserved[2];
    char title[64];
    char text[128];
};

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
