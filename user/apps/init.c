/* PID 1: start the user environment. */
#include <stdio.h>
#include <string.h>
#include "nocturne.h"

/* Test disks (scripts/test.py) carry /data/tests/autorun.sh: run it with its output on the serial
   port, then power off. Ordinary data disks don't have it. */
static void autorun(void) {
    struct n_stat st;
    if (stat("/data/tests/autorun.sh", &st) < 0) return;
    int nul = open("/dev/null", O_RDONLY), log = open("/dev/kmsg", O_WRONLY);
    int fdmap[3] = {nul, log, log};
    char *argv[] = {"sh", "/data/tests/autorun.sh", NULL};
    int pid = spawn("/bin/sh", argv, fdmap, 0);
    close(nul);
    close(log);
    if (pid >= 0) waitpid(pid, NULL, 0);
    poweroff();
}

int main(int argc, char **argv) {
    printf("\n\x1b[1;35m*\x1b[0m Welcome to \x1b[1mNocturne " "\x1b[0m- a small operating system for quiet nights.\n");
    if (gui_available()) {
        /* desktop session: open a terminal and idle */
        int nul = open("/dev/null", O_RDWR);
        int fdmap[3] = {nul, nul, nul};
        char *targv[] = {"term", NULL};
        spawn("/bin/term", targv, fdmap, SPAWN_DETACH);
        close(nul);
        autorun();
        for (;;) msleep(60000);
    }
    autorun();
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
