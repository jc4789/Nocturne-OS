/* PID 1: start the user environment. */
#include <stdio.h>
#include <string.h>
#include "nocturne.h"

int main(int argc, char **argv) {
    printf("\n\x1b[1;35m*\x1b[0m Welcome to \x1b[1mNocturne " "\x1b[0m- a small operating system for quiet nights.\n");
    if (gui_available()) {
        /* desktop session: open a terminal and idle */
        int nul = open("/dev/null", O_RDWR);
        int fdmap[3] = {nul, nul, nul};
        char *targv[] = {"term", NULL};
        spawn("/bin/term", targv, fdmap, SPAWN_DETACH);
        close(nul);
        for (;;) msleep(60000);
    }
    /* text session: keep a shell alive on the console */
    for (;;) {
        char *sargv[] = {"sh", NULL};
        int pid = spawn("/bin/sh", sargv, NULL, 0);
        if (pid < 0) {
            printf("init: cannot start /bin/sh\n");
            msleep(5000);
            continue;
        }
        int st;
        waitpid(pid, &st, 0);
        printf("\ninit: shell exited (%d), restarting\n", st);
    }
}
