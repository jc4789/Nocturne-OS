/* guitest: open a window, check it really reaches the screen (via screen_grab), close it and check
   it is gone, and check the screenshot program writes a PNG. Prints "guitest: ok" or the problem. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "nocturne.h"

#define MAGENTA RGB(255, 0, 255)

static uint32_t *grab(int *w, int *h) {
    if (screen_grab(NULL, 0, w, h) < 0) return NULL;
    uint32_t *px = malloc((size_t)*w * (size_t)*h * 4);
    if (px && screen_grab(px, (size_t)*w * (size_t)*h * 4, w, h) < 0) {
        free(px);
        return NULL;
    }
    return px;
}

static long count(const uint32_t *px, int n, uint32_t c) {
    long k = 0;
    for (int i = 0; i < n; i++) k += (px[i] & 0xFFFFFF) == (c & 0xFFFFFF);
    return k;
}

int main(void) {
    if (!gui_available()) {
        printf("guitest: no desktop\n");
        return 1;
    }
    int w, h;
    uint32_t *before = grab(&w, &h);
    if (!before) {
        printf("guitest: screen_grab failed\n");
        return 1;
    }
    long base = count(before, w * h, MAGENTA);

    window_t *win = win_open(200, 150, "guitest", 0);
    if (!win) {
        printf("guitest: win_open failed\n");
        return 1;
    }
    gfx_fill(&win->c, 0, 0, win->w, win->h, MAGENTA);
    win_update(win);
    msleep(300); /* let the compositor present it */
    uint32_t *during = grab(&w, &h);
    long shown = count(during, w * h, MAGENTA) - base;
    win_close(win);
    msleep(300);
    uint32_t *after = grab(&w, &h);
    long left = count(after, w * h, MAGENTA) - base;
    if (shown < 200 * 150 * 9 / 10) {
        printf("guitest: window not on screen (%ld magenta pixels)\n", shown);
        return 1;
    }
    if (left > 0) {
        printf("guitest: window still visible after close (%ld pixels)\n", left);
        return 1;
    }

    char *argv[] = {"screenshot", "/home/guitest.png", NULL};
    int st = run_wait("screenshot", argv);
    unsigned char sig[8] = {0};
    int fd = open("/home/guitest.png", O_RDONLY);
    if (fd >= 0) read(fd, sig, 8);
    close(fd);
    if (st != 0 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) {
        printf("guitest: screenshot did not write a PNG (status %d)\n", st);
        return 1;
    }
    printf("guitest: ok (window %ld px)\n", shown);
    return 0;
}
