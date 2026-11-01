/* A WIN_FRAME_QUEUE window's surfaces: each present is shown by a frame (EV_FRAME names it), a
   surface comes back (EV_SURFACE) only once another is shown, what is drawn into a surface not
   presented never reaches the screen, and a resize to fullscreen and back gives new surfaces.
   Arguments: the frames to draw and how long to keep the window open after (ms). */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>

/* The EV_FRAME that shows present number presented. */
static bool wait_frame(window_t *win, uint64_t presented) {
    struct gui_event event;
    while (win_event(win, &event, N_CLOCK_NEVER) > 0) {
        if (event.type == EV_CLOSE) return false;
        if (event.type == EV_FRAME && event.serial >= presented) return event.serial == presented;
    }
    return false;
}

static bool grab_at(uint32_t *screen, int gw, int gh, int x, int y, uint32_t want, const char *what) {
    int w = gw, h = gh;
    if (screen_grab(screen, (size_t)gw * gh * sizeof *screen, &w, &h) < 0 || w != gw || h != gh) return false;
    if (screen[(size_t)y * gw + x] == want) return true;
    printf("wmframetest: FAIL %s got=%x expected=%x\n", what, screen[(size_t)y * gw + x], want);
    return false;
}

int main(int argc, char **argv) {
    if (argc != 3) return 1;
    unsigned long frames = strtoul(argv[1], NULL, 10), hold = strtoul(argv[2], NULL, 10);
    if (!frames) return 1;
    int sw, sh, gw, gh;
    if (screen_size(&sw, &sh) < 0 || screen_grab(NULL, 0, &gw, &gh) < 0) return 1;
    window_t *win;
    if (win_open(sw / 2, sh / 2, "フレームの列: 返却・画素確認", WIN_FRAME_QUEUE, &win) < 0) return 1;
    win_move(win, sw / 4, sh / 4);
    uint32_t *screen = malloc((size_t)gw * gh * sizeof *screen);
    int result = 1;
    if (!screen) goto done;
    uint64_t start = (n_clock_monotonic() / N_NS_PER_MS);
    for (unsigned long frame = 0; frame < frames; frame++) {
        if (win_begin_frame(win) < 0) goto done;
        int drawn = win->current;
        uint32_t color = RGB(frame & 255, (frame >> 8) & 255, 255 - (frame & 255));
        gfx_fill(&win->c, 0, 0, win->w, win->h, color);
        if (win_update(win) < 0 || win->current >= 0 || win->c.px || !wait_frame(win, win->presented)) goto done;
        /* the surface shown is the system's: presenting it again is refused */
        if (__syscall(SYS_WIN_PRESENT, win->handle, 0, 0, 0, drawn + 1) != -N_ERR_BUSY ||
            __syscall(SYS_WIN_PRESENT, win->handle, 0, 0, 0, N_WIN_SURFACES_MAX + 1) != -N_ERR_INVALID)
            goto done;
        /* what is drawn into a surface not presented is not shown */
        if (win_begin_frame(win) < 0 || win->current == drawn) goto done;
        gfx_fill(&win->c, 0, 0, win->w, win->h, RGB(0, 255, 0));
        if (!grab_at(screen, gw, gh, sw / 4 + 1 + win->w / 3, sh / 4 + 28 + win->h / 3, color, "frame")) goto done;
    }
    /* new surfaces on EV_RESIZE, fullscreen and back */
    for (unsigned restored = 0; restored < 2; restored++) {
        if (win_fullscreen(win, !restored) < 0) goto done;
        struct gui_event event;
        do {
            if (win_event(win, &event, N_CLOCK_NEVER) <= 0 || event.type == EV_CLOSE) goto done;
        } while (event.type != EV_RESIZE);
        if (win_begin_frame(win) < 0) goto done;
        uint32_t color = restored ? RGB(24, 96, 180) : RGB(180, 96, 24);
        gfx_fill(&win->c, 0, 0, win->w, win->h, color);
        if (win_update(win) < 0 || !wait_frame(win, win->presented)) goto done;
        int x = restored ? sw / 4 + 1 + win->w / 3 : gw / 3;
        int y = restored ? sh / 4 + 28 + win->h / 3 : gh / 3;
        if (!grab_at(screen, gw, gh, x, y, color, "resize")) goto done;
    }
    printf("wmframetest: PASS fullscreen-resize-restore\n");
    printf("wmframetest: PASS frames=%lu elapsed_ms=%lu surfaces=returned unpresented=hidden screen-grab=correct\n",
           frames, (unsigned long)((n_clock_monotonic() / N_NS_PER_MS) - start));
    printf("wmframetest: READY visual hold_ms=%lu\n", hold);
    n_sleep_until(n_clock_after((uint64_t)(hold) * N_NS_PER_MS));
    result = 0;
done:
    free(screen);
    win_close(win);
    printf(result ? "wmframetest: FAIL\n" : "wmframetest: PASS destroy\n");
    return result;
}
