/* The native ABI (docs/nocturne-native-abi.md) on the real kernel: documents
 * in RAM and FAT folders, message channels, apps and event queues. Run in a
 * dedicated test VM; it says nothing about GUI or real-site acceptance. */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static unsigned checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("nativeabitest FAIL line %d\n", __LINE__); } } while (0)

static bool loaded(const char *path, const char *want, size_t n) {
    void *data;
    size_t size;
    if (n_document_load(path, &data, &size) < 0) return false;
    bool same = size == n && (!n || !memcmp(data, want, n));
    if (data) n_memory_release(data, size);
    return same;
}

static void documents(const char *root) {
    char folder[128], a[160], b[160], sub[160], c[160], moved[160], inner[160];
    snprintf(folder, sizeof folder, "%s/nativeabi", root);
    snprintf(a, sizeof a, "%s/a.txt", folder);
    snprintf(b, sizeof b, "%s/b.txt", folder);
    snprintf(sub, sizeof sub, "%s/sub", folder);
    snprintf(c, sizeof c, "%s/c.txt", sub);
    snprintf(moved, sizeof moved, "%s/moved", folder);
    snprintf(inner, sizeof inner, "%s/inner", sub);
    CHECK(n_folder_create(folder) == 0);
    CHECK(n_folder_create(folder) == -N_ERR_EXISTS);

    n_handle h = n_document_open(a, N_DOCUMENT_CREATE);
    CHECK(h >= 0);
    CHECK(n_handle_kind(h) == N_KIND_DOCUMENT);
    CHECK(n_document_open(a, N_DOCUMENT_CREATE) == -N_ERR_EXISTS);
    CHECK(n_document_write_at(h, 0, "hello", 5) == 5);
    CHECK(n_document_write_at(h, 10, "world", 5) == 5);
    CHECK(n_document_size(h) == 15);
    char buf[64];
    memset(buf, 'x', sizeof buf);
    CHECK(n_document_read_at(h, 0, buf, sizeof buf) == 15);
    CHECK(!memcmp(buf, "hello\0\0\0\0\0world", 15));
    CHECK(n_document_read_at(h, 15, buf, 4) == 0);
    n_handle reader = n_document_open(a, N_DOCUMENT_READ);
    CHECK(reader >= 0);
    CHECK(n_document_write_at(reader, 0, "x", 1) == -N_ERR_DENIED);
    CHECK(n_document_set_size(reader, 1) == -N_ERR_DENIED);
    CHECK(n_document_claim(h, false) == 0);
    CHECK(n_document_claim(reader, false) == -N_ERR_NOT_READY);
    CHECK(n_document_release(h) == 0);
    CHECK(n_document_claim(reader, false) == 0);
    CHECK(n_document_release(reader) == 0);
    CHECK(n_document_set_size(h, 5) == 0);
    CHECK(n_document_size(reader) == 5);
    CHECK(n_handle_close(reader) == 0);
    CHECK(n_handle_close(h) == 0);
    CHECK(n_handle_kind(h) == -N_ERR_NO_HANDLE);

    /* A replacement is invisible until its explicit close. */
    h = n_document_open(a, N_DOCUMENT_REPLACE);
    CHECK(h >= 0);
    CHECK(n_document_write_at(h, 0, "new content", 11) == 11);
    CHECK(loaded(a, "hello", 5));
    CHECK(n_handle_close(h) == 0);
    CHECK(loaded(a, "new content", 11));
    h = n_document_open(b, N_DOCUMENT_REPLACE); /* replacing nothing creates */
    CHECK(h >= 0);
    CHECK(n_entry_info(b, &(struct n_entry){0}) == -N_ERR_NOT_FOUND);
    CHECK(n_handle_close(h) == 0);
    CHECK(loaded(b, "", 0));

    CHECK(n_document_save(b, "saved", 5) == 0);
    CHECK(n_document_append(b, "+more", 5) == 0);
    CHECK(loaded(b, "saved+more", 10));
    struct n_entry e;
    CHECK(n_entry_info(b, &e) == 0 && e.kind == N_KIND_DOCUMENT && e.size == 10 && !strcmp(e.name, "b.txt"));
    CHECK(n_entry_info(folder, &e) == 0 && e.kind == N_KIND_FOLDER);
    CHECK(n_entry_info("relative/b.txt", &e) == -N_ERR_INVALID);
    CHECK(n_document_open("b.txt", N_DOCUMENT_READ) == -N_ERR_INVALID);

    struct n_entry list[8];
    n_result n = n_folder_list(folder, list, 8, 0);
    CHECK(n == 2);
    bool saw_a = false, saw_b = false;
    for (n_result i = 0; i < n; i++) {
        if (!strcmp(list[i].name, "a.txt")) saw_a = list[i].size == 11;
        if (!strcmp(list[i].name, "b.txt")) saw_b = list[i].size == 10;
    }
    CHECK(saw_a && saw_b);
    CHECK(n_folder_list(folder, list, 8, 1) == 1);
    CHECK(n_folder_list(folder, list, 8, 2) == 0);
    CHECK(n_folder_list(a, list, 8, 0) == -N_ERR_NOT_FOLDER);

    CHECK(n_folder_create(sub) == 0);
    CHECK(n_entry_move(b, c) == 0);
    CHECK(n_entry_info(b, &e) == -N_ERR_NOT_FOUND);
    CHECK(loaded(c, "saved+more", 10));
    CHECK(n_entry_move(a, c) == -N_ERR_EXISTS); /* never replaces */
    CHECK(loaded(a, "new content", 11));
    CHECK(n_entry_move(sub, inner) == -N_ERR_INVALID);
    CHECK(n_entry_move(sub, moved) == 0);
    snprintf(c, sizeof c, "%s/c.txt", moved);
    CHECK(loaded(c, "saved+more", 10));
    CHECK(n_entry_info(sub, &e) == -N_ERR_NOT_FOUND);
    CHECK(n_entry_remove(folder) == -N_ERR_NOT_EMPTY);
    CHECK(n_entry_remove(c) == 0);
    CHECK(n_entry_remove(moved) == 0);
    CHECK(n_entry_remove(a) == 0);
    CHECK(n_entry_remove(folder) == 0);
    CHECK(n_entry_info(folder, &e) == -N_ERR_NOT_FOUND);
}

static void channels(void) {
    n_handle c[2];
    char buf[64];
    CHECK(n_channel_pair(c) == 0);
    CHECK(n_handle_kind(c[0]) == N_KIND_CHANNEL && n_handle_kind(c[1]) == N_KIND_CHANNEL);
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, NULL) == -N_ERR_NOT_READY);
    CHECK(n_channel_send(c[0], "one", 3, -1) == 3);
    CHECK(n_channel_send(c[0], "second", 6, -1) == 6);
    CHECK(n_channel_send(c[0], "", 0, -1) == 0);
    CHECK(n_channel_receive(c[1], NULL, 0, N_CHANNEL_PEEK, NULL) == 3);
    CHECK(n_channel_receive(c[1], buf, 2, 0, NULL) == -N_ERR_TOO_LARGE);
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, NULL) == 3 && !memcmp(buf, "one", 3));
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, NULL) == 6 && !memcmp(buf, "second", 6));
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, NULL) == 0); /* the empty message */
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, NULL) == -N_ERR_NOT_READY);
    CHECK(n_channel_send(c[1], "back", 4, -1) == 4); /* both directions */
    CHECK(n_channel_receive(c[0], buf, sizeof buf, N_CHANNEL_WAIT, NULL) == 4 && !memcmp(buf, "back", 4));

    /* A document travels with its message and arrives as a new handle. */
    CHECK(n_document_save("/home/nativeabi-attached", "attached", 8) == 0);
    n_handle doc = n_document_open("/home/nativeabi-attached", N_DOCUMENT_READ);
    CHECK(doc >= 0);
    CHECK(n_channel_send(c[0], "doc", 3, doc) == 3);
    CHECK(n_handle_close(doc) == 0);
    n_handle got = -1;
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, &got) == 3);
    CHECK(got >= 0 && n_handle_kind(got) == N_KIND_DOCUMENT);
    CHECK(n_document_read_at(got, 0, buf, sizeof buf) == 8 && !memcmp(buf, "attached", 8));
    n_handle_close(got);
    CHECK(n_channel_send(c[0], "x", 1, c[1]) == -N_ERR_WRONG_KIND);
    CHECK(n_channel_send(c[0], "plain", 5, -1) == 5);
    got = 7;
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, &got) == 5 && got == -1);
    n_entry_remove("/home/nativeabi-attached");

    /* Event queue: level-triggered readiness, closing, timers, memory words. */
    n_handle q = n_events_create();
    CHECK(q >= 0 && n_handle_kind(q) == N_KIND_EVENTS);
    struct n_event ev[8];
    CHECK(n_events_watch(q, c[1], N_EVENT_RECEIVABLE | N_EVENT_CLOSED, 7) == 0);
    CHECK(n_events_watch(q, q, N_EVENT_RECEIVABLE, 8) == -N_ERR_WRONG_KIND);
    CHECK(n_events_watch(q, c[1], N_EVENT_TIMER, 9) == -N_ERR_INVALID);
    CHECK(n_events_wait(q, ev, 8, 0) == 0);
    CHECK(n_channel_send(c[0], "ready", 5, -1) == 5);
    CHECK(n_events_wait(q, ev, 8, 0) == 1 && ev[0].token == 7 && ev[0].events == N_EVENT_RECEIVABLE);
    CHECK(n_events_wait(q, ev, 8, 0) == 1); /* still ready */
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, NULL) == 5);
    CHECK(n_events_wait(q, ev, 8, 0) == 0);
    CHECK(n_handle_close(c[0]) == 0);
    CHECK(n_events_wait(q, ev, 8, 1000) == 1 && ev[0].token == 7 && (ev[0].events & N_EVENT_CLOSED));
    CHECK(n_channel_receive(c[1], buf, sizeof buf, 0, NULL) == -N_ERR_CLOSED);
    CHECK(n_handle_close(c[1]) == 0); /* ends the watch */
    CHECK(n_events_wait(q, ev, 8, 0) == 0);
    CHECK(n_events_unwatch(q, 7) == 0);

    uint64_t t0 = n_clock_uptime_ms();
    CHECK(n_events_timer(q, 40, false, 99) == 0);
    CHECK(n_events_wait(q, ev, 8, -1) == 1 && ev[0].token == 99 && ev[0].events == N_EVENT_TIMER);
    uint64_t waited = n_clock_uptime_ms() - t0;
    CHECK(waited >= 30 && waited < 2000);
    CHECK(n_events_wait(q, ev, 8, 0) == 0); /* one-shot */
    CHECK(n_events_timer(q, 10, true, 5) == 0);
    int ticks = 0;
    for (int i = 0; i < 3; i++) ticks += n_events_wait(q, ev, 8, 1000) == 1 && ev[0].token == 5;
    CHECK(ticks == 3);
    CHECK(n_events_unwatch(q, 5) == 1);
    CHECK(n_events_wait(q, ev, 8, 50) == 0);

    static volatile uint32_t word;
    CHECK(n_events_watch_address(q, &word, 1, 11) == -N_ERR_NOT_READY);
    CHECK(n_events_watch_address(q, &word, 0, 11) == 0);
    CHECK(n_events_wait(q, ev, 8, 0) == 0);
    CHECK(wake_address(&word, 1) == 1);
    CHECK(n_events_wait(q, ev, 8, 1000) == 1 && ev[0].token == 11 && ev[0].events == N_EVENT_ADDRESS);
    CHECK(n_events_watch_address(q, &word, 0, 12) == 0);
    CHECK(n_events_unwatch(q, 12) == 1);
    CHECK(wake_address(&word, 1) == 0);
    CHECK(n_handle_close(q) == 0);
}

static int child(const char *mode) {
    char buf[64];
    if (!strcmp(mode, "wait")) {
        n_channel_receive(0, buf, sizeof buf, N_CHANNEL_WAIT, NULL); /* until stopped */
        return 1;
    }
    if (n_channel_receive(0, buf, sizeof buf, N_CHANNEL_WAIT, NULL) != 4 || memcmp(buf, "ping", 4)) return 2;
    const char *greeting = getenv("NATIVE_GREETING");
    int n = snprintf(buf, sizeof buf, "pong:%s:%d", greeting ? greeting : "-", (int)n_app_parent());
    if (n_channel_send(0, buf, (size_t)n, -1) != n) return 3;
    return 7;
}

static void apps(const char *self) {
    n_handle c[2];
    char buf[64], want[64];
    CHECK(n_channel_pair(c) == 0);
    const char *args[] = {self, "child", "talk"};
    const char *settings[] = {"NATIVE_GREETING=hi"};
    int32_t handles[] = {c[1], 1, 2};
    struct n_launch l = {.arguments = args, .settings = settings, .handles = handles,
                         .argument_count = 3, .setting_count = 1, .handle_count = 3};
    n_handle app = n_app_launch(self, &l);
    CHECK(app >= 0 && n_handle_kind(app) == N_KIND_APP);
    n_handle_close(c[1]);
    int status = -1;
    CHECK(n_app_result(app, 0, &status) == -N_ERR_NOT_READY);
    n_handle q = n_events_create();
    CHECK(n_events_watch(q, app, N_EVENT_APP_ENDED, 3) == 0);
    struct n_event ev[4];
    CHECK(n_events_wait(q, ev, 4, 0) == 0);
    CHECK(n_channel_send(c[0], "ping", 4, -1) == 4);
    n_result n = n_channel_receive(c[0], buf, sizeof buf, N_CHANNEL_WAIT, NULL);
    int wn = snprintf(want, sizeof want, "pong:hi:%d", (int)n_app_self());
    CHECK(n == wn && !memcmp(buf, want, (size_t)wn));
    CHECK(n_events_wait(q, ev, 4, 5000) == 1 && ev[0].token == 3 && ev[0].events == N_EVENT_APP_ENDED);
    CHECK(n_app_result(app, -1, &status) == 0 && status == 7);
    CHECK(n_app_result(app, 0, &status) == 0 && status == 7); /* the record stays until closed */
    CHECK(n_channel_receive(c[0], buf, sizeof buf, 0, NULL) == -N_ERR_CLOSED);
    CHECK(n_handle_close(app) == 0);
    CHECK(n_handle_close(c[0]) == 0);
    CHECK(n_events_wait(q, ev, 4, 0) == 0);

    /* Stopping: a timeout first, then the end report. */
    CHECK(n_channel_pair(c) == 0);
    const char *wait_args[] = {self, "child", "wait"};
    handles[0] = c[1];
    l.arguments = wait_args;
    app = n_app_launch(self, &l);
    CHECK(app >= 0);
    CHECK(n_app_result(app, 50, &status) == -N_ERR_TIMEOUT);
    CHECK(n_app_stop(app, 0) == 0);
    CHECK(n_app_result(app, 5000, &status) == 0 && status != 0);
    CHECK(n_app_stop(app, 0) == 0); /* already ended */
    CHECK(n_app_stop(app, 2) == -N_ERR_INVALID);
    n_handle_close(app);
    n_handle_close(c[0]);
    n_handle_close(c[1]);
    CHECK(n_app_launch(self, &l) == -N_ERR_NO_HANDLE); /* the table names closed handles */
    l.handle_count = 0;

    CHECK(n_app_launch("nativeabitest", &l) == -N_ERR_INVALID);
    CHECK(n_app_launch("/home/no-such-program", &l) == -N_ERR_NOT_FOUND);
    const char *bad[] = {"NO_EQUALS"};
    l.settings = bad;
    CHECK(n_app_launch(self, &l) == -N_ERR_INVALID);
    CHECK(n_app_result(q, 0, &status) == -N_ERR_WRONG_KIND);
    n_handle_close(q);
}

/* Pages below 2 GiB for code with 32-bit absolute addresses, beside the usual ones. */
static void memory(void) {
    const uintptr_t low_top = (uintptr_t)1 << 31;
    void *pm, *qm, *againm, *highm, *none;
    CHECK(n_memory_reserve(3 * 4096, N_MEMORY_READ_WRITE | N_MEMORY_LOW, &pm) == 0);
    CHECK(n_memory_reserve(4096, N_MEMORY_READ_WRITE | N_MEMORY_LOW, &qm) == 0);
    char *p = pm, *q = qm;
    CHECK(p && q && (uintptr_t)p + 3 * 4096 <= low_top && (uintptr_t)q + 4096 <= low_top && !((uintptr_t)p & 4095));
    if (!p || !q) return;
    CHECK(!p[0] && !p[3 * 4096 - 1]);
    p[0] = 1, p[3 * 4096 - 1] = 2, q[0] = 3;
    CHECK(n_memory_protect(p, 4096, N_MEMORY_READ_EXECUTE) == 0);
    CHECK(n_memory_protect(p, 4096, N_MEMORY_READ_WRITE) == 0 && p[0] == 1);
    CHECK(n_memory_release(p + 4096, 2 * 4096) == 0 && n_memory_release(p, 4096) == 0);
    n_memory_reserve(3 * 4096, N_MEMORY_READ_WRITE | N_MEMORY_LOW, &againm);
    char *again = againm;
    CHECK(again == p && !again[0]); /* the freed range is reused, zero-filled */
    CHECK(q[0] == 3);
    CHECK(n_memory_release(again, 3 * 4096) == 0 && n_memory_release(q, 4096) == 0);
    n_memory_reserve(4096, N_MEMORY_READ_WRITE, &highm);
    char *high = highm;
    CHECK(high && (uintptr_t)high >= low_top);
    if (high) n_memory_release(high, 4096);
    errno = 0;
    CHECK(n_memory_reserve(4096, N_MEMORY_LOW, &none) == -N_ERR_INVALID && !none);
    CHECK(n_memory_reserve((size_t)3 << 29, N_MEMORY_READ_WRITE | N_MEMORY_LOW, &none) == -N_ERR_NO_MEMORY && !none);
    CHECK(n_memory_release((void *)(uintptr_t)0x10000000, 4096) == -N_ERR_INVALID);
    CHECK(errno == 0); /* the native calls leave errno alone */
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "child")) return child(argv[2]);
    memory();
    documents("/home");
    documents("/data");
    /* Across folders on different devices the document is copied whole. */
    CHECK(n_document_save("/home/nativeabi-cross", "across", 6) == 0);
    CHECK(n_entry_move("/home/nativeabi-cross", "/data/nativeabi-cross") == 0);
    CHECK(loaded("/data/nativeabi-cross", "across", 6));
    CHECK(n_entry_info("/home/nativeabi-cross", &(struct n_entry){0}) == -N_ERR_NOT_FOUND);
    n_entry_remove("/data/nativeabi-cross");
    channels();
    apps(argv[0]);
    CHECK(!strcmp(n_result_text(-N_ERR_NOT_FOUND), "not found"));
    printf("nativeabitest: %u checks, %u failed\n", checks, failed);
    return failed != 0;
}
