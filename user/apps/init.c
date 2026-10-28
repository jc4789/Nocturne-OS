/* The first app: start the user environment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "nocturne.h"
#include "nvrm.h"

#define SH      N_FOLDER_SYSTEM "\\sh"
#define NVRM    N_FOLDER_SYSTEM "\\nvrm"
#define AUTORUN "C:\\Tests\\autorun.sh"

/* Launch path with up to two arguments and the given input, output and log, in
   this app's folder. Settings beyond FOLDER (such as TERMINAL) are passed in extra. */
static n_handle start2(const char *path, const char *name, const char *arg, const char *arg2, int32_t in, int32_t out,
                       const char *extra) {
    const char *f = getenv("FOLDER");
    char folder[4104];
    snprintf(folder, sizeof folder, "FOLDER=%s", f && n_path_is_complete(f) ? f : n_user_folder());
    const char *settings[2] = {folder, extra};
    const char *args[3] = {name, arg, arg2};
    int32_t handles[3] = {in, out, out};
    struct n_launch l = {.arguments = args, .settings = settings, .handles = handles,
                         .argument_count = arg ? (arg2 ? 3 : 2) : 1, .setting_count = extra ? 2 : 1, .handle_count = 3};
    return n_app_launch(path, &l);
}

static n_handle start(const char *path, const char *name, const char *arg, int32_t in, int32_t out, const char *extra) {
    return start2(path, name, arg, NULL, in, out, extra);
}

/* Test disks (scripts/test.py) carry C:\Tests\autorun.sh: run it with its output in the system
   log (the serial port), then power off. Other disks don't have it. */
static void autorun(void) {
    struct n_entry e;
    if (n_entry_info(AUTORUN, &e) < 0) return;
    n_handle log = n_system_log_open();
    n_handle app = start(SH, "sh", AUTORUN, -1, log, NULL);
    if (log >= 0) n_handle_close(log);
    if (app >= 0) n_app_result(app, -1, NULL);
    poweroff();
}

/* Drivers in apps (docs/nocturne-native-abi.md §9): an NVIDIA display function goes to nvrm,
   which gets the claim as its handle 3 and logs to the system log. init keeps no copy, so the
   claim (and the device's DMA) ends with the driver. Media without the driver skip this. */
static void start_drivers(void) {
    struct n_entry e;
    if (n_entry_info(NVRM, &e) < 0) return;
    struct n_pciinfo *list = NULL;
    size_t capacity = 1;
    int n;
    for (;;) {
        if (capacity > INT_MAX || capacity > SIZE_MAX / sizeof *list) {
            free(list);
            return;
        }
        struct n_pciinfo *grown = realloc(list, capacity * sizeof *list);
        if (!grown) {
            free(list);
            return;
        }
        list = grown;
        n = pcilist(list, (int)capacity);
        if (n < 0) { free(list); return; }
        if ((size_t)n < capacity) break;
        if (capacity > INT_MAX / 2) { free(list); return; }
        capacity *= 2;
    }
    for (int i = 0; i < n; i++) {
        if (!nvrm_pci_is_gpu(list[i].vendor, list[i].cls, list[i].subcls, list[i].progif)) continue;
        char where[16];
        snprintf(where, sizeof where, "%02x:%02x.%u", list[i].bus, list[i].dev, list[i].func);
        n_handle pci = n_pci_claim(list[i].bus, list[i].dev, list[i].func);
        if (pci < 0) {
            printf("init: cannot claim %s for nvrm (%s)\n", where, n_result_text(pci));
            continue;
        }
        n_handle log = n_system_log_open();
        const char *args[2] = {"nvrm", where};
        int32_t handles[4] = {-1, log, log, pci};
        struct n_launch l = {.arguments = args, .handles = handles, .argument_count = 2, .handle_count = 4};
        n_handle app = n_app_launch(NVRM, &l);
        if (app < 0) printf("init: cannot start " NVRM " (%s)\n", n_result_text(app));
        else n_handle_close(app);
        if (log >= 0) n_handle_close(log);
        n_handle_close(pci);
    }
    free(list);
    fflush(stdout);
}

int main(void) {
    printf("\n\x1b[1;35m*\x1b[0m Welcome to \x1b[1mNocturne " "\x1b[0m- a small operating system for quiet nights.\n");
    fflush(stdout);
    start_drivers();
    if (gui_available()) {
        /* desktop session: the desktop is the user's from here (the system has put the user's
           settings back before this app started), opened apps come from its icons and menu */
        autorun();
        for (;;) n_sleep_ms(60000);
    }
    autorun();
    /* text session: keep a shell alive on the console this app was given */
    for (;;) {
        n_handle sh = start(SH, "sh", NULL, 0, 1, n_terminal(1) ? "TERMINAL=0 1 2" : NULL);
        if (sh < 0) {
            printf("init: cannot start " SH " (%s)\n", n_result_text(sh));
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
