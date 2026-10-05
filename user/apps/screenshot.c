/* screenshot: save the screen as a PNG. usage: screenshot [-s N] [-d SECONDS] [file.png] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nocturne.h"
#include "png.h"

int main(int argc, char **argv) {
    int shrink = 1, delay = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) shrink = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) delay = atoi(argv[++i]);
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: screenshot [-s N (shrink 1/N)] [-d SECONDS (wait first)] [file.png]\n");
            return 2;
        } else path = argv[i];
    }
    char name[64];
    if (!path) {
        time_t t = time(NULL);
        strftime(name, sizeof name, "/home/screenshot-%H%M%S.png", localtime(&t));
        path = name;
    }
    if (delay > 0) sleep((unsigned)delay);
    int w, h;
    uint32_t *px = NULL;
    if (screen_grab(NULL, 0, &w, &h) == 0) px = malloc((size_t)w * (size_t)h * 4);
    if (!px || screen_grab(px, (size_t)w * (size_t)h * 4, &w, &h) < 0) {
        fprintf(stderr, "screenshot: cannot read the screen (is the desktop running?)\n");
        return 1;
    }
    uint32_t *img = px;
    if (shrink > 1) {
        img = img_shrink(px, w, h, w, shrink, &w, &h);
        if (!img) {
            fprintf(stderr, "screenshot: out of memory\n");
            return 1;
        }
    }
    uint8_t *png;
    size_t n;
    if (png_encode(img, w, h, w, &png, &n) < 0) {
        fprintf(stderr, "screenshot: out of memory\n");
        return 1;
    }
    FILE *f = fopen(path, "w");
    if (!f || fwrite(png, 1, n, f) != n || fclose(f) != 0) {
        perror(path);
        return 1;
    }
    printf("%s: %dx%d, %zu bytes\n", path, w, h, n);
    return 0;
}
