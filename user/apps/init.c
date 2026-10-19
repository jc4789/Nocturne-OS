/* The first app: start the user environment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

/* Launch path with the given input, output and log, in this app's folder.
   Settings beyond FOLDER (such as TERMINAL) are passed in extra. */
static n_handle start(const char *path, const char *name, const char *arg, int32_t in, int32_t out, const char *extra) {
    const char *f = getenv("FOLDER");
    char folder[300];
    snprintf(folder, sizeof folder, "FOLDER=%s", f && f[0] == '/' ? f : "/home");
    const char *settings[2] = {folder, extra};
    const char *args[2] = {name, arg};
    int32_t handles[3] = {in, out, out};
    struct n_launch l = {.arguments = args, .settings = settings, .handles = handles, .argument_count = arg ? 2 : 1,
                         .setting_count = extra ? 2 : 1, .handle_count = 3};
    return n_app_launch(path, &l);
}

/* Test disks (scripts/test.py) carry /data/tests/autorun.sh: run it with its output in the system
   log (the serial port), then power off. Ordinary data disks don't have it. */
static void autorun(void) {
    struct n_entry e;
    if (n_entry_info("/data/tests/autorun.sh", &e) < 0) return;
    n_handle log = n_system_log_open();
    n_handle app = start("/bin/sh", "sh", "/data/tests/autorun.sh", -1, log, NULL);
    if (log >= 0) n_handle_close(log);
    if (app >= 0) n_app_result(app, -1, NULL);
    poweroff();
}

int main(void) {
    printf("\n\x1b[1;35m*\x1b[0m Welcome to \x1b[1mNocturne " "\x1b[0m- a small operating system for quiet nights.\n");
    fflush(stdout);
    if (gui_available()) {
        /* desktop session: put the saved Settings back; the desktop is the user's
           from here, opened apps come from its icons and menu */
        n_handle settings = start("/bin/settings", "settings", "--apply", -1, -1, NULL);
        if (settings >= 0) {
            n_app_result(settings, 5000, NULL);
            n_handle_close(settings);
        }
        autorun();
        for (;;) n_sleep_ms(60000);
    }
    autorun();
    /* text session: keep a shell alive on the console this app was given */
    for (;;) {
        n_handle sh = start("/bin/sh", "sh", NULL, 0, 1, n_terminal(1) ? "TERMINAL=0 1 2" : NULL);
        if (sh < 0) {
            printf("init: cannot start /bin/sh (%s)\n", n_result_text(sh));
            fflush(stdout);
            n_sleep_ms(5000);
            continue;
        }
        int st = 0;
        n_app_result(sh, -1, &st);
        n_handle_close(sh);
        printf("\ninit: shell exited (%d), restarting\n", st);
        fflush(stdout);
    }
}
