/* allapps: compile every program in /usr/src/apps with tcc (to object files), so the in-OS
   toolchain is known to handle the whole userland. Prints failures and "allapps: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

int main(void) {
    struct n_entry *entries;
    n_result count = n_folder_entries("/usr/src/apps", &entries);
    if (count < 0) {
        printf("allapps: no /usr/src/apps\n");
        return 1;
    }
    int n = 0, failed = 0;
    for (n_result i = 0; i < count; i++) {
        const char *name = entries[i].name;
        size_t l = strlen(name);
        if (l < 3 || strcmp(name + l - 2, ".c")) continue;
        char src[300];
        snprintf(src, sizeof src, "/usr/src/apps/%s", name);
        char *argv[] = {"tcc", "-c", "-o", "/home/allapps.o", src, NULL};
        int st = run_wait("tcc", argv);
        n++;
        if (st != 0) {
            printf("FAIL %s (status %d)\n", name, st);
            failed++;
        }
    }
    free(entries);
    printf("allapps: %d compiled, %d failed\n", n, failed);
    return failed != 0 || n < 40;
}
