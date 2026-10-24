/* allapps: compile every program in C:\Nocturne\SDK\Source\Apps to object files with an in-OS C
   compiler, tcc or the one the first argument names (clang), so that compiler is known to handle
   the whole userland. Prints failures and "allapps: N failed". usage: allapps [compiler] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define APPS_FOLDER "C:\\Nocturne\\SDK\\Source\\Apps"

int main(int argc, char **argv) {
    const char *compiler = argc > 1 ? argv[1] : "tcc";
    struct n_entry *entries;
    n_result count = n_folder_entries(APPS_FOLDER, &entries);
    if (count < 0) {
        printf("allapps: no " APPS_FOLDER "\n");
        return 1;
    }
    char object[4096];
    if (n_path_join(n_temp_folder(), "allapps.o", object, sizeof object) < 0) {
        printf("allapps: no room for the object file's name\n");
        free(entries);
        return 1;
    }
    int n = 0, failed = 0;
    for (n_result i = 0; i < count; i++) {
        const char *name = entries[i].name;
        size_t l = strlen(name);
        if (l < 3 || strcmp(name + l - 2, ".c")) continue;
        char src[300];
        snprintf(src, sizeof src, APPS_FOLDER "\\%s", name);
        char *compile[] = {(char *)compiler, "-c", "-o", object, src, NULL};
        int st = run_wait(compiler, compile);
        n++;
        if (st != 0) {
            printf("FAIL %s (status %d)\n", name, st);
            failed++;
        }
    }
    free(entries);
    n_entry_remove(object);
    printf("allapps: %d compiled, %d failed\n", n, failed);
    return failed != 0 || n < 40;
}
