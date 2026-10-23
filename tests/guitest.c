/* guitest: open a window, check it really reaches the screen (via screen_grab), close it and check
   it is gone, and check the screenshot program writes a PNG. Prints "guitest: ok" or the problem. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    window_t *win;
    n_result opened = win_open(200, 150, "guitest", 0, &win);
    if (opened < 0) {
        printf("guitest: win_open failed: %s\n", n_result_text(opened));
        return 1;
    }
    gfx_fill(&win->c, 0, 0, win->w, win->h, MAGENTA);
    win_update(win);
    n_sleep_ms(300); /* let the compositor present it */
    uint32_t *during = grab(&w, &h);
    long shown = count(during, w * h, MAGENTA) - base;
    win_close(win);
    n_sleep_ms(300);
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

    char png[4096];
    if (n_path_join(n_temp_folder(), "guitest.png", png, sizeof png) < 0) {
        printf("guitest: the temporary folder's path is too long\n");
        return 1;
    }
    char *argv[] = {"screenshot", png, NULL};
    int st = run_wait("screenshot", argv);
    unsigned char sig[8] = {0};
    n_handle doc = n_document_open(png, N_DOCUMENT_READ);
    if (doc >= 0) {
        n_document_read_at(doc, 0, sig, 8);
        n_handle_close(doc);
    }
    if (st != 0 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) {
        printf("guitest: screenshot did not write a PNG (status %d)\n", st);
        return 1;
    }
    printf("guitest: ok (window %ld px)\n", shown);
    return 0;
}
