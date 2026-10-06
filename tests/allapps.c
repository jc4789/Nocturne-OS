/* allapps: compile every program in /usr/src/apps with tcc (to object files), so the in-OS
   toolchain is known to handle the whole userland. Prints failures and "allapps: N failed". */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include "nocturne.h"

int main(void) {
    int fd = open("/usr/src/apps", O_RDONLY);
    if (fd < 0) {
        printf("allapps: no /usr/src/apps\n");
        return 1;
    }
    struct n_dirent d;
    int n = 0, failed = 0;
    for (int i = 0; readdir(fd, i, &d) > 0; i++) {
        size_t l = strlen(d.name);
        if (l < 3 || strcmp(d.name + l - 2, ".c")) continue;
        char src[128];
        snprintf(src, sizeof src, "/usr/src/apps/%s", d.name);
        char *argv[] = {"tcc", "-c", "-o", "/home/allapps.o", src, NULL};
        int st = run_wait("tcc", argv);
        n++;
        if (st != 0) {
            printf("FAIL %s (status %d)\n", d.name, st);
            failed++;
        }
    }
    close(fd);
    printf("allapps: %d compiled, %d failed\n", n, failed);
    return failed != 0 || n < 40;
}
