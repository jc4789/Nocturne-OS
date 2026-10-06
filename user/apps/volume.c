/* volume: show or set the master volume (0-100); "+N" and "-N" change it by N. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

static int get(void) {
    int fd = open("/dev/volume", O_RDONLY);
    if (fd < 0) return -1;
    char buf[8] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    return n > 0 ? atoi(buf) : -1;
}

int main(int argc, char **argv) {
    int cur = get();
    if (cur < 0) {
        fprintf(stderr, "volume: /dev/volume: %s\n", strerror(errno));
        return 1;
    }
    if (argc < 2) {
        printf("volume: %d%%\n", cur);
        return 0;
    }
    const char *a = argv[1];
    char *end;
    long v = strtol(a, &end, 10);
    if (end == a || *end) {
        fprintf(stderr, "usage: volume [0-100 | +N | -N]\n");
        return 2;
    }
    if (a[0] == '+' || a[0] == '-') v += cur;
    v = MAX(0, MIN(100, v));
    char buf[8];
    int len = sprintf(buf, "%ld", v);
    int fd = open("/dev/volume", O_WRONLY);
    if (fd < 0 || write(fd, buf, (size_t)len) < 0) {
        fprintf(stderr, "volume: /dev/volume: %s\n", strerror(errno));
        return 1;
    }
    close(fd);
    printf("volume: %ld%%\n", v);
    return 0;
}
