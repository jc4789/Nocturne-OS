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

/* The system log kept on C: in the user's AppData\Nocturne: System.log for this start, the one
   before moved to System-previous.log. A machine without a serial port loses the kernel's log
   (its rings in memory) with its power, and what a start said before it stopped is what one wants
   to read; it can be read from Files, or from another system (host/store). A thread reads the
   system log stream from the oldest record the kernel holds on and appends what comes; the
   kernel says in the text what was overwritten before it was read. A start keeps at most
   KEEP_MOST bytes: C: has its other uses. */
#define KEEP_PIECE 65536        /* text taken from the log at a time */
#define KEEP_MOST (32ull << 20)

static char kept_log[4200];

static void keep_note(const char *fmt, unsigned long long n) {
    char note[96];
    int k = snprintf(note, sizeof note, fmt, n);
    if (k > 0) n_document_append(kept_log, note, (size_t)k < sizeof note ? (size_t)k : sizeof note - 1);
}

static void keep_log(void *arg) {
    n_handle log = (n_handle)(intptr_t)arg;
    char *piece = malloc(KEEP_PIECE);
    for (unsigned long long kept = 0; piece;) {
        n_result n = n_channel_receive(log, piece, KEEP_PIECE, N_CHANNEL_WAIT, NULL);
        if (n <= 0) {
            printf("init: the system log stopped (%s)\n", n_result_text(n));
            break;
        }
        if (kept + (unsigned long long)n > KEEP_MOST) {
            keep_note("\n[init: this start said more than %llu MiB; the rest is not kept here]\n", KEEP_MOST >> 20);
            break;
        }
        n_result r = n_document_append(kept_log, piece, (size_t)n);
        if (r < 0) {
            printf("init: the system log cannot be kept on C: (%s)\n", n_result_text(r));
            break;
        }
        kept += (unsigned long long)n;
    }
    fflush(stdout);
    free(piece);
    n_handle_close(log);
}

static void keep_system_log(void) {
    char folder[4096], previous[sizeof kept_log];
    n_result r = n_app_data_folder("Nocturne", folder, sizeof folder);
    if (r >= 0) {
        snprintf(kept_log, sizeof kept_log, "%s\\System.log", folder);
        snprintf(previous, sizeof previous, "%s\\System-previous.log", folder);
        r = n_entry_replace(kept_log, previous);
        if (r == -N_ERR_NOT_FOUND) r = 0; /* the first start */
    }
    n_handle log = r >= 0 ? n_system_log_open() : r;
    n_result thread = log >= 0 ? thread_create(keep_log, (void *)(intptr_t)log) : log;
    if (thread >= 0) {
        thread_detach((int)thread);
        return;
    }
    if (log >= 0) n_handle_close(log);
    printf("init: the system log cannot be kept on C: (%s)\n", n_result_text(thread));
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
    keep_system_log();
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
