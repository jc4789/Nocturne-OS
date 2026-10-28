/* wptrunner: the guest half of scripts/wpt.py. The host serves web-platform-tests, picks the
   tests and reads their results; this program only keeps one browser per slot running as the
   host says, over one TCP connection to 10.0.2.2 (the port is in C:\Tests\wptport).
   Host -> guest, one line each:
     open <slot> [<browser option>...] <url>   (re)start the slot's browser on url
     close <slot>                              stop the slot's browser
     quit                                      stop every browser and end (init powers off)
   Guest -> host:
     hello <slots>          once connected
     opened <slot> <id>     the browser started (its app identifier)
     failed <slot> <error>  it could not start
     ended <slot> <status>  it ended without being told to (a crash or its own exit)
   Each browser runs in C:\Tests\profiles\slot<slot> as its user folder, made new at each open,
   and with --wpt-automation, a test tool of the browser's: WPT's testdriver
   (tests/wpt/testdriver-vendor.js) may then drive its own input. This is the only program that
   gives the browser that option. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define HOST 0x0A000202 /* 10.0.2.2 */
#define MAX_SLOTS 64
#define MAX_WORDS 16

static n_handle apps[MAX_SLOTS];
static int link_handle = -1;

static void say(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;
    for (int done = 0; done < n;) {
        long r = tcp_send(link_handle, line + done, (size_t)(n - done));
        if (r <= 0) return;
        done += (int)r;
    }
}

static void stop_slot(int slot) {
    if (apps[slot] < 0) return;
    n_app_stop(apps[slot], N_APP_STOP_TREE);
    n_app_result(apps[slot], -1, NULL);
    n_handle_close(apps[slot]);
    apps[slot] = -1;
}

/* folder and everything in it */
static void remove_tree(const char *path) {
    struct n_entry *e = NULL;
    n_result count = n_folder_entries(path, &e);
    for (n_result i = 0; i < count; i++) {
        char child[4096];
        if (n_path_join(path, e[i].name, child, sizeof child) < 0) continue;
        if (e[i].kind == N_KIND_FOLDER) remove_tree(child);
        else n_entry_remove(child);
    }
    free(e);
    n_entry_remove(path);
}

/* Each browser gets a new user folder of its own, as wptrunner gives each browser a new profile:
   cookies, storage and preferences do not pass between slots or from one browser to the next. */
static void open_slot(int slot, char **words, int count) {
    stop_slot(slot);
    char user[64], temp[80], user_setting[96], temp_setting[96];
    snprintf(user, sizeof user, "C:\\Tests\\profiles\\slot%d", slot);
    snprintf(temp, sizeof temp, "%s\\Temp", user);
    remove_tree(user);
    n_folder_create("C:\\Tests\\profiles");
    n_folder_create(user);
    n_folder_create(temp);
    snprintf(user_setting, sizeof user_setting, "USERFOLDER=%s", user);
    snprintf(temp_setting, sizeof temp_setting, "TEMP=%s", temp);
    const char *settings[] = {user_setting, temp_setting};
    const char *args[MAX_WORDS + 2];
    args[0] = "browser";
    args[1] = "--wpt-automation";
    for (int i = 0; i < count; i++) args[i + 2] = words[i];
    int32_t handles[] = {-1, -1, -1};
    struct n_launch launch = {.arguments = args, .settings = settings, .handles = handles,
                              .argument_count = (uint32_t)count + 2, .setting_count = 2, .handle_count = 3};
    n_handle app = n_app_launch(N_FOLDER_PROGRAMS "\\Browser\\browser", &launch);
    if (app < 0) {
        say("failed %d %s\n", slot, n_result_text(app));
        return;
    }
    apps[slot] = app;
    say("opened %d %ld\n", slot, (long)n_app_id(app));
}

/* one command line from the host; false on quit */
static bool command(char *line) {
    char *words[MAX_WORDS + 2];
    int count = 0;
    for (char *at = strtok(line, " \t\r"); at && count < MAX_WORDS + 2; at = strtok(NULL, " \t\r"))
        words[count++] = at;
    if (!count) return true;
    if (!strcmp(words[0], "quit")) return false;
    int slot = count > 1 ? atoi(words[1]) : -1;
    if (slot < 0 || slot >= MAX_SLOTS) {
        say("error bad slot\n");
        return true;
    }
    if (!strcmp(words[0], "open") && count > 2) open_slot(slot, words + 2, count - 2);
    else if (!strcmp(words[0], "close")) stop_slot(slot);
    else say("error unknown command %s\n", words[0]);
    return true;
}

int main(void) {
    for (int i = 0; i < MAX_SLOTS; i++) apps[i] = -1;
    int port = 0;
    FILE *f = fopen("C:\\Tests\\wptport", "r");
    if (!f || fscanf(f, "%d", &port) != 1 || port <= 0 || port > 65535) {
        printf("wptrunner: no port in C:\\Tests\\wptport\n");
        return 1;
    }
    fclose(f);
    for (int attempt = 0; attempt < 60 && link_handle < 0; attempt++) {
        int s = tcp_connect(HOST, (uint16_t)port, 2000);
        if (s >= 0) link_handle = s;
        else n_sleep_ms(1000);
    }
    if (link_handle < 0) {
        printf("wptrunner: cannot reach the host at 10.0.2.2:%d\n", port);
        return 1;
    }
    printf("wptrunner: connected to 10.0.2.2:%d\n", port);
    fflush(stdout);
    say("hello %d\n", MAX_SLOTS);
    char buffer[8192];
    size_t used = 0;
    for (;;) {
        long got = tcp_recv(link_handle, buffer + used, sizeof buffer - 1 - used, 250);
        if (got == 0) break; /* the host went away */
        if (got > 0) {
            used += (size_t)got;
            buffer[used] = 0;
            char *start = buffer, *nl;
            bool going = true;
            while (going && (nl = strchr(start, '\n'))) {
                *nl = 0;
                going = command(start);
                start = nl + 1;
            }
            if (!going) break;
            used -= (size_t)(start - buffer);
            memmove(buffer, start, used);
            if (used == sizeof buffer - 1) used = 0; /* a line longer than any command */
        } else if (got != -N_ERR_TIMEOUT && got != -N_ERR_NOT_READY) {
            printf("wptrunner: connection error %s\n", n_result_text((n_result)got));
            break;
        }
        for (int slot = 0; slot < MAX_SLOTS; slot++) {
            int status = 0;
            if (apps[slot] >= 0 && n_app_result(apps[slot], 0, &status) == 0) {
                n_handle_close(apps[slot]);
                apps[slot] = -1;
                say("ended %d %d\n", slot, status);
            }
        }
    }
    for (int slot = 0; slot < MAX_SLOTS; slot++) stop_slot(slot);
    tcp_close(link_handle);
    printf("wptrunner: done\n");
    return 0;
}
