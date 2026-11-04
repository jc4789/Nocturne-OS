#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gfx.h"
#include "display_abi.h"
#include "edid.h"
#include "sys/wait.h"

struct fb_info {
    uint8_t *addr;
    uint64_t phys;
    uint32_t width, height, pitch, bpp;
    uint8_t rshift, gshift, bshift, rsize, gsize, bsize;
    uint8_t bpc;    /* the desktop's depth: 8, or 10 for 2:10:10:10 or while simulated */
    uint8_t native_bpc; /* smallest channel depth of the firmware's framebuffer */
    bool deep;      /* bpc >= 10: the compositor keeps deep planes */
    bool simulated; /* Settings (smooth gradients): the 10 bpc pipeline, shown dithered on an 8 bpc scanout */
    bool native;    /* the firmware's framebuffer is 32bpp xRGB: direct copy */
    /* The monitor the firmware's mode is shown on, from the EDID the firmware read from it
       (monitor "" without one), and that mode's refresh where a timing of it names that size
       (0: not known). */
    char monitor[EDID_MONITOR_ID_SIZE], monitor_name[14];
    uint32_t refresh_mhz;
};
extern struct fb_info fb;
struct limine_framebuffer;
void fb_init(struct limine_framebuffer *f);
bool fb_set_simulated(bool on); /* false: screen 0's buffers are not 8 bpc xRGB (fb_dither_able) */
/* Whether screen 0's buffers are 8 bpc xRGB, the format the 10 bpc pipeline is dithered onto. */
bool fb_dither_able(void);

/* ---- the text console's screen ----
   fb_present copies a rectangle of a full-screen 32bpp xRGB buffer into the buffer screen 0 shows
   (the boot console and the panic screen draw there; the desktop draws through frames, below).
   It runs on the boot CPU alone before the scheduler, and in threads after. */
void fb_present(const uint32_t *src, int src_pitch, int x, int y, int w, int h);
/* The firmware's screen as a device of the machine (hal/hal.h). It has no way back from a sleep:
   the firmware set its mode, and nothing sets it again; it is as it was after the next boot. A driver
   that can show it again and has its own resume takes it over with fb_boot_release. */
void fb_publish(void);
void fb_boot_release(void);
/* A display driver rebuilding how the CPU reaches the boot framebuffer (NVIDIA's RM switches the
   BAR it lies in to virtual mode during its start and stop) holds it: nothing is written there
   until the last release; no time limit ends a hold. fb_hold returns once no write is under way.
   fb_release returns true when it ended the last hold, after which its caller has the screen
   drawn again in full. fb_drop_holds: the kernel stopped, the panic screen is drawn. */
void fb_hold(void);
bool fb_release(void);
void fb_drop_holds(void);
/* The kernel's last screen (the panic screen), a 32bpp xRGB buffer of the firmware's size: into
   every buffer of every screen and every image a driver may show in a buffer's place, at the top
   left, the rest of a larger one black. */
void fb_present_last(const uint32_t *src, int src_pitch, int w, int h);

/* ---- screens ----
   A screen is one display the desktop is shown on: a mode (width x height at a refresh, or at a
   variable refresh within the monitor's range) and 1 to N_SCREEN_BUFFERS_MAX buffers of that mode
   the display engine shows one of. Screen 0 is the firmware's framebuffer at boot; a display
   driver gives a screen its own buffers (dev/screen_driver.c for a driver in an app, vm/vbe.c for an
   emulator's display) and takes them away again. A screen never goes away: one whose driver left
   is off until a driver gives it buffers again. The number of screens has no limit.

   A frame (the compositor's, gui/wm.c) is drawn into a buffer the display engine does not show:
   screen_frame_begin waits until there is one (a flip asked earlier was shown), the frame writes
   its rectangles there (screen_frame_write) and screen_frame_end asks the engine to show it from
   its next vertical blank on. A screen with one buffer has no other: its frame writes into the
   buffer shown, at the frame's start, and is shown as it is written (its flip tells the driver). The flip's showing comes back from the driver
   (screen_flipped, with the moment of the vertical blank the engine measured), as do the vertical
   blanks (screen_vblank); both wake the screen's waiters (screen_wait). */
#define SCREEN_BUFFERS_MAX N_SCREEN_BUFFERS_MAX
struct screen;
struct scanout;
struct screen_ops {
    /* Show buffer from the next vertical blank on; its pixels are written. The driver calls
       screen_flipped once it is shown (at once where showing it is a register's write). In the
       compositor's thread, the screen's use held. */
    void (*flip)(struct screen *s, void *owner, unsigned buffer, uint64_t serial);
};
/* What a driver gives a screen: its mode, the monitor, and the buffers (fb_buffer_map). */
struct screen_mode_desc {
    uint32_t width, height;
    uint32_t refresh_mhz;  /* the mode's refresh; 0: not known (frames come only when asked for) */
    uint32_t vrr_min_mhz;  /* a variable refresh down to this; 0: a fixed refresh */
    bool direct;           /* the driver can show a window's own images (N_SCANOUT_DIRECT) */
    char monitor[EDID_MONITOR_ID_SIZE], name[14];
};
/* A buffer of format N_SCANOUT_* (fb_buffer_check passed): the given frames mapped into the kernel
   with the memory type in type (PTE bits, the type every other mapping of them has), the first
   row first_byte into the first frame. NULL without memory. fb_buffer_unmap undoes it, once
   nothing writes there (a screen's buffer: after screen_set_buffers replaced it). */
int64_t fb_buffer_check(uint32_t width, uint32_t height, uint32_t pitch, uint32_t format, uint64_t offset,
                        uint64_t *bytes);
struct scanout *fb_buffer_map(const uint64_t *frames, size_t pages, uint64_t type, size_t first_byte,
                              uint32_t width, uint32_t height, uint32_t pitch, uint32_t format);
/* A buffer the kernel can address already (memory of a VM's display, mapped write-combined). */
struct scanout *fb_buffer_at(uint8_t *addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format);
void fb_buffer_unmap(struct scanout *o);
/* What a display engine may show in a buffer's place (an app's image shown directly,
   dev/screen_driver.c): the panic screen is drawn into it too. fb_image_remove returns once nothing
   writes there any more; the caller unmaps it after. */
void fb_image_add(struct scanout *o);
void fb_image_remove(struct scanout *o);

/* A new screen, off; its number is the next. NULL without memory. */
struct screen *screen_new(void);
struct screen *screen_get(unsigned id); /* NULL: no such screen */
unsigned screen_count(void);
unsigned screen_id(const struct screen *s);

/* What shows the screens to the user. The drivers of the screens (dev/screen_driver.c) say what they do
   here and do not know who hears it; gui/ sets it at its start, before any app runs. */
struct n_screen_modes;
struct n_screen_state;
struct screen_display {
    void (*repaint)(void); /* the screens lost what was on them: draw all of them again */
    void (*direct)(unsigned screen, bool able); /* whether the driver can show a window's own images */
    void (*modes_offer)(unsigned screen, const struct n_screen_modes *m);
    void (*modes_gone)(unsigned screen);
    void (*modes_shown)(unsigned screen, struct screen_mode_desc *desc);
    int64_t (*state)(unsigned screen, uint64_t seen, struct n_screen_state *out, uint64_t deadline);
};
extern const struct screen_display *screen_display;
/* The driver owner gives s these buffers (count of them, all of desc's mode), shown by ops; the
   buffer shown now is shown (its index). count 0: the screen is off; screen 0 then shows the
   firmware's framebuffer again. Returns once no frame writes to the old buffers any more: the
   caller frees them after. The window manager hears of it (screen_changed). Thread context.
   Returns the serial of the last flip asked of the old buffers: every flip after it is of these. */
uint64_t screen_set_buffers(struct screen *s, const struct screen_ops *ops, void *owner,
                            const struct screen_mode_desc *desc, struct scanout *const *buffers, unsigned count,
                            unsigned shown);
/* Whether owner gives s its buffers now. */
bool screen_owned_by(struct screen *s, void *owner);
/* The mode of s as its driver says it (fields of struct screen_mode_desc); false when it is off. */
bool screen_mode(struct screen *s, struct screen_mode_desc *out);
/* Whether s shows the firmware's framebuffer (screen 0 without a driver's buffers). */
bool screen_boot(struct screen *s);
/* The refresh of s changed (a variable refresh range, a driver's word): the window manager
   hears of it. */
void screen_set_refresh(struct screen *s, uint32_t refresh_mhz, uint32_t vrr_min_mhz);
/* The window manager's hook: a screen came, changed its buffers or mode, or went off. Called in
   the changing thread, no lock held. */
void screen_changed_hook(void (*changed)(struct screen *s));

/* Frames (the compositor's thread for s). screen_frame_begin: the buffer to draw into, s's use
   held; -N_ERR_CLOSED while the screen is off or held (fb_hold), use not held. It waits for a
   flip in flight to be shown, until deadline (sys/wait.h) at the latest: -N_ERR_TIMEOUT then.
   *generation changes when the buffers do: what was written into them before is gone. */
int screen_frame_begin(struct screen *s, uint64_t deadline, uint64_t *generation);
void screen_frame_write(struct screen *s, int buffer, const uint32_t *src, int src_pitch, const uint8_t *deep,
                        int deep_pitch, int x, int y, int w, int h);
/* Asks for buffer to be shown and gives use back: the flip's serial. */
uint64_t screen_frame_end(struct screen *s, int buffer);
/* The driver: the flip of serial is shown from the vertical blank of the monotonic clock's
   time_ns on (a buffer index of s's). Any thread. */
void screen_flipped(struct screen *s, unsigned buffer, uint64_t serial, uint64_t time_ns);
/* The driver: a vertical blank of s, its count and moment on the monotonic clock. Any thread. */
void screen_vblank(struct screen *s, uint64_t count, uint64_t time_ns);
/* What the compositor waits on: a word that moves on with each flip shown, vertical blank and
   change of s. screen_wait sleeps until it differs from seen or until deadline. */
uint64_t screen_events(struct screen *s);
void screen_wait(struct screen *s, uint64_t seen, uint64_t deadline);
/* Something the frames of s wait for besides the driver changed (the compositor's damage): the
   word moves on and the waiters wake. Any thread; it does not sleep. */
void screen_poke(struct screen *s);
/* The last flip shown (its serial, 0 for none) and when; the last vertical blank (count, time;
   count 0 before the first). */
struct screen_times {
    uint64_t shown_serial, shown_ns;
    uint64_t vblank_count, vblank_ns;
    uint64_t asked_serial; /* the last flip asked (screen_frame_end) */
};
void screen_times(struct screen *s, struct screen_times *out);

/* ---- a display driver in an app (dev/screen_driver.c, docs/nocturne-native-abi.md §9) ----
   Every call takes the handle of a claim (hal/hal.h) the caller holds, in a thread that may sleep,
   holding no spinlock; b, v, m, out are kernel copies.
   screen_driver_give: the output's buffers (NULL: given back) become what its screen is drawn into;
   returns the serial of the output's last flip before them.
   screen_driver_flips: the flips the screens ask of the claim's outputs, waiting until deadline
   (sys/wait.h) for one; out has room for max. screen_driver_vblank: a vertical blank of an output,
   with the flip it shows. screen_driver_state: what the output's screen should show (gui/wm.c
   keeps it). screen_driver_modes: the monitor on an output, the modes the driver can show there
   and what it showed, for the system to choose from (gui/screenmode.c); count 0 withdraws them.
   screen_driver_image: a surface the display engine may show in the output's buffers' place,
   which the panic screen is drawn into too; returns its number. screen_driver_image_end: that one
   no longer. The output's buffers given back, or given without N_SCANOUT_DIRECT, end them all.
   screen_driver_hold: only for the claim of the device whose memory the boot screen is drawn
   through, or one whose buffers a screen is drawn into; given back when the claim ends.
   screen_driver_boot_slot: the memory resource (slot) of d the boot screen is drawn through, or -1. */
struct handle;
struct hal_claim;
struct hal_device;
struct n_scanout;
struct n_screen_buffers;
struct n_screen_flip;
struct n_screen_vblank;
int64_t screen_driver_give(struct handle *h, uint32_t output, const struct n_screen_buffers *b);
int64_t screen_driver_flips(struct handle *h, struct n_screen_flip *out, size_t max, uint64_t deadline);
int64_t screen_driver_vblank(struct handle *h, const struct n_screen_vblank *v);
int64_t screen_driver_state(struct handle *h, uint32_t output, uint64_t seen, struct n_screen_state *out,
                            uint64_t deadline);
int64_t screen_driver_modes(struct handle *h, uint32_t output, const struct n_screen_modes *m);
int64_t screen_driver_image(struct handle *h, uint32_t output, struct handle *surface, const struct n_scanout *desc);
int64_t screen_driver_image_end(struct handle *h, int64_t number);
int64_t screen_driver_hold(struct hal_claim *c, bool hold);
int screen_driver_boot_slot(const struct hal_device *d);
