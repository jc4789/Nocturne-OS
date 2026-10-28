/* Watches on C: (n_entry_watch and n_watch_take, docs/nocturne-native-abi.md §1 and §3) on the real
 * kernel: which changes count and when (a name made, saved over, replaced, removed, moved out and in,
 * renamed in its case only; an edit not before its save), that a new reading reads what was told,
 * the folder held by what it is and the end of a removed one, what apps do not see, the errors, an
 * event queue's level, 1000 changes counted as 1000, and the time saves and renames take with
 * watches open and without. Works in a folder of its own in the temporary folder; says nothing
 * about the programs that watch. */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

static unsigned checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("entrywatchtest FAIL line %d\n", __LINE__); } } while (0)

#define PATH_ROOM 768
static char root[512]; /* the test's folder */

/* name in the test's folder (one of eight paths in turn) */
static const char *in(const char *name) {
    static char paths[8][PATH_ROOM];
    static unsigned next;
    char *p = paths[next++ % 8];
    snprintf(p, PATH_ROOM, "%s\\%s", root, name);
    return p;
}

/* a watch on name in the test's folder, or (NULL) on the folder itself */
static n_handle watch(const char *name, uint32_t what) { return n_entry_watch(name ? in(name) : root, what); }

static bool save(const char *name, const char *text) { return n_document_save(in(name), text, strlen(text)) == 0; }

static bool holds(const char *name, const char *text) {
    void *data;
    size_t size;
    if (n_document_load(in(name), &data, &size) < 0) return false;
    bool same = size == strlen(text) && !memcmp(data, text, size);
    if (data) n_memory_release(data, size);
    return same;
}

/* whether the queue reports something now (level: as often as it is asked) */
static bool ready(n_handle q) {
    struct n_event ev[8];
    return n_events_wait(q, ev, 8, 0) > 0;
}

/* path and everything in it */
static bool remove_all(const char *path) {
    struct n_entry list[16];
    n_result n;
    while ((n = n_folder_list(path, list, 16, 0)) > 0) {
        n_result removed = 0;
        for (n_result i = 0; i < n; i++) {
            char child[PATH_ROOM];
            snprintf(child, sizeof child, "%s\\%s", path, list[i].name);
            if (list[i].kind == N_KIND_FOLDER ? remove_all(child) : n_entry_remove(child) == 0) removed++;
        }
        if (!removed) break;
    }
    return n_entry_remove(path) == 0;
}

static void errors(void) {
    CHECK(n_entry_watch(root, 0) == -N_ERR_INVALID);
    CHECK(n_entry_watch(root, N_WATCH_NAME | N_WATCH_FOLDER) == -N_ERR_INVALID);
    CHECK(n_entry_watch(root, 4) == -N_ERR_INVALID);
    CHECK(__syscall(SYS_ENTRY_WATCH, (long)(uintptr_t)root, (1L << 32) | N_WATCH_FOLDER, 0, 0, 0) == -N_ERR_INVALID);
    CHECK(n_entry_watch("C:\\", N_WATCH_NAME) == -N_ERR_INVALID); /* the root has no name */
    CHECK(n_entry_watch("x.txt", N_WATCH_NAME) == -N_ERR_INVALID); /* not a complete path */
    CHECK(n_entry_watch(in("none\\x.txt"), N_WATCH_NAME) == -N_ERR_NOT_FOUND);
    CHECK(n_entry_watch(in("none"), N_WATCH_FOLDER) == -N_ERR_NOT_FOUND);
    CHECK(save("doc.txt", "d"));
    CHECK(n_entry_watch(in("doc.txt"), N_WATCH_FOLDER) == -N_ERR_NOT_FOLDER);
    CHECK(n_entry_watch(in("doc.txt\\x"), N_WATCH_NAME) == -N_ERR_NOT_FOLDER);
    CHECK(n_entry_watch((const char *)8, N_WATCH_NAME) == -N_ERR_BAD_ADDRESS);
    /* the kernel's folder of replacements is not there for apps (PRIVATE) */
    CHECK(n_entry_watch("C:\\Nocturne\\Temp", N_WATCH_FOLDER) == -N_ERR_NOT_FOUND);
    CHECK(n_entry_watch("C:\\Nocturne\\Temp\\1", N_WATCH_NAME) == -N_ERR_NOT_FOUND);
    /* the system's folders are read, so they are watched */
    n_handle sys = n_entry_watch("C:\\Nocturne\\System\\sh", N_WATCH_NAME), top = n_entry_watch("C:\\", N_WATCH_FOLDER);
    CHECK(sys >= 0 && n_handle_kind(sys) == N_KIND_WATCH && n_watch_take(sys) == 0);
    CHECK(top >= 0 && n_handle_kind(top) == N_KIND_WATCH);
    n_handle d = n_document_open(in("doc.txt"), N_DOCUMENT_READ), q = n_events_create();
    CHECK(d >= 0 && n_watch_take(d) == -N_ERR_WRONG_KIND);
    CHECK(q >= 0 && n_watch_take(q) == -N_ERR_WRONG_KIND);
    CHECK(n_handle_close(sys) == 0 && n_watch_take(sys) == -N_ERR_NO_HANDLE);
    n_handle_close(top);
    n_handle_close(d);
    n_handle_close(q);
    CHECK(n_entry_remove(in("doc.txt")) == 0);
}

static void names(void) {
    n_handle q = n_events_create();
    n_handle a = watch("a.txt", N_WATCH_NAME), b = watch("b.txt", N_WATCH_NAME), f = watch(NULL, N_WATCH_FOLDER);
    CHECK(q >= 0 && a >= 0 && b >= 0 && f >= 0);
    if (q < 0 || a < 0 || b < 0 || f < 0) return;
    CHECK(n_handle_kind(a) == N_KIND_WATCH && n_handle_kind(f) == N_KIND_WATCH);
    CHECK(n_events_watch(q, a, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, 1) == 0);
    CHECK(!ready(q) && n_watch_take(a) == 0 && n_watch_take(f) == 0); /* nothing yet */

    /* a name not there yet, made by a save: one change (made, and its content taken in) */
    CHECK(save("a.txt", "one"));
    struct n_event ev[8];
    CHECK(n_events_wait(q, ev, 8, 0) == 1 && ev[0].token == 1 && ev[0].events == N_EVENT_RECEIVABLE);
    CHECK(ready(q)); /* level: until the count is taken */
    CHECK(n_watch_take(a) == 1 && n_watch_take(a) == 0 && !ready(q));
    CHECK(n_watch_take(f) == 1 && n_watch_take(b) == 0);

    /* a save over it */
    CHECK(save("a.txt", "two") && holds("a.txt", "two"));
    CHECK(n_watch_take(a) == 1 && n_watch_take(f) == 1);

    /* made by n_document_open(CREATE): the name at once, its content when it is saved */
    n_handle h = n_document_open(in("b.txt"), N_DOCUMENT_CREATE);
    CHECK(h >= 0 && n_watch_take(b) == 1 && n_watch_take(f) == 1 && n_watch_take(a) == 0);
    CHECK(n_document_write_at(h, 0, "bee", 3) == 3);
    CHECK(n_watch_take(b) == 0 && n_watch_take(f) == 0);
    CHECK(n_document_commit(h, N_COMMIT_WAIT) == 0 && n_watch_take(b) == 1 && n_watch_take(f) == 1);
    CHECK(n_handle_close(h) == 0 && n_watch_take(b) == 0); /* nothing written since */

    /* an edit: what write_at wrote is not seen before it is saved; the close of its last writer
       saves it */
    h = n_document_open(in("a.txt"), N_DOCUMENT_EDIT);
    CHECK(h >= 0 && n_document_write_at(h, 0, "three", 5) == 5);
    CHECK(!ready(q) && n_watch_take(a) == 0 && n_watch_take(f) == 0 && holds("a.txt", "two"));
    CHECK(n_document_commit(h, N_COMMIT_WAIT) == 0 && n_watch_take(a) == 1 && n_watch_take(f) == 1);
    CHECK(holds("a.txt", "three"));
    CHECK(n_document_write_at(h, 5, "!", 1) == 1 && n_watch_take(a) == 0);
    CHECK(n_handle_close(h) == 0 && n_watch_take(a) == 1 && holds("a.txt", "three!"));
    CHECK(n_watch_take(f) == 1);

    /* n_entry_replace: b takes a's name; both names change, the folder once */
    CHECK(n_entry_replace(in("b.txt"), in("a.txt")) == 0 && holds("a.txt", "bee"));
    CHECK(n_watch_take(a) == 1 && n_watch_take(b) == 1 && n_watch_take(f) == 1);

    /* removed; removed while open, the name goes all the same */
    CHECK(n_entry_remove(in("a.txt")) == 0 && n_watch_take(a) == 1 && n_watch_take(f) == 1);
    CHECK(save("a.txt", "four") && n_watch_take(a) == 1 && n_watch_take(f) == 1);
    n_handle reader = n_document_open(in("a.txt"), N_DOCUMENT_READ);
    CHECK(reader >= 0 && n_entry_remove(in("a.txt")) == 0 && n_watch_take(a) == 1 && n_watch_take(f) == 1);
    n_handle_close(reader); /* the document goes from the kernel's folder of replacements, no app's */
    CHECK(n_watch_take(a) == 0 && n_watch_take(f) == 0);

    /* the case of a name only; any case of the name is the name. A move to the same spelling
       changes nothing. */
    CHECK(save("c.txt", "see") && n_watch_take(f) == 1);
    n_handle c = watch("C.txt", N_WATCH_NAME);
    CHECK(c >= 0 && n_entry_move(in("c.txt"), in("C.TXT")) == 0);
    CHECK(n_watch_take(c) == 1 && n_watch_take(f) == 1);
    struct n_entry e;
    CHECK(n_entry_info(in("c.txt"), &e) == 0 && !strcmp(e.name, "C.TXT"));
    CHECK(n_entry_move(in("C.TXT"), in("C.TXT")) == 0 && n_watch_take(c) == 0 && n_watch_take(f) == 0);

    /* moved out into a folder and back in */
    CHECK(n_folder_create(in("sub")) == 0 && n_watch_take(f) == 1);
    n_handle s = watch("sub", N_WATCH_FOLDER), sc = watch("sub\\c.txt", N_WATCH_NAME);
    CHECK(s >= 0 && sc >= 0);
    CHECK(n_entry_move(in("C.TXT"), in("sub\\c.txt")) == 0);
    CHECK(n_watch_take(c) == 1 && n_watch_take(f) == 1 && n_watch_take(s) == 1 && n_watch_take(sc) == 1);
    CHECK(n_entry_move(in("sub\\c.txt"), in("c.txt")) == 0);
    CHECK(n_watch_take(c) == 1 && n_watch_take(f) == 1 && n_watch_take(s) == 1 && n_watch_take(sc) == 1);
    /* a change in a folder is none of the folder it is in: a watch is on what is directly there */
    CHECK(save("sub\\d.txt", "dee") && n_watch_take(s) == 1 && n_watch_take(f) == 0);

    /* two handles on one name count apart, each from its opening */
    n_handle a2 = watch("A.TXT", N_WATCH_NAME);
    CHECK(a2 >= 0 && n_watch_take(a2) == 0);
    CHECK(save("a.txt", "five") && n_watch_take(a) == 1);
    CHECK(save("a.txt", "six") && n_watch_take(a) == 1 && n_watch_take(a2) == 2);
    CHECK(n_handle_close(a2) == 0);
    CHECK(save("a.txt", "seven") && n_watch_take(a) == 1);

    n_handle_close(c);
    n_handle_close(s);
    n_handle_close(sc);
    n_handle_close(a);
    n_handle_close(b);
    n_handle_close(f);
    n_handle_close(q);
    remove_all(in("sub"));
    n_entry_remove(in("a.txt"));
    n_entry_remove(in("c.txt"));
}

/* Told by a watch, a new reading reads the new content: another thread saves while this one waits. */
static int saver(void *path) {
    n_sleep_ms(100);
    return n_document_save(path, "fresh", 5) == 0 ? 0 : 1;
}

static void told(void) {
    char path[PATH_ROOM];
    snprintf(path, sizeof path, "%s\\told.txt", root);
    CHECK(save("told.txt", "stale"));
    n_handle w = n_entry_watch(path, N_WATCH_NAME), q = n_events_create();
    CHECK(w >= 0 && q >= 0 && n_events_watch(q, w, N_EVENT_RECEIVABLE, 7) == 0);
    thrd_t t;
    int done = 1;
    bool started = thrd_create(&t, saver, path) == thrd_success;
    CHECK(started);
    struct n_event ev[4];
    CHECK(n_events_wait(q, ev, 4, 5000) == 1 && ev[0].token == 7);
    CHECK(holds("told.txt", "fresh"));
    if (started) thrd_join(t, &done);
    CHECK(done == 0 && n_watch_take(w) == 1);
    n_handle_close(w);
    n_handle_close(q);
    n_entry_remove(path);
}

/* A folder is watched by what it is: moved and renamed, the same; removed, its watches end. */
static void gone(void) {
    n_handle q = n_events_create(), up = watch("gone", N_WATCH_NAME);
    CHECK(q >= 0 && up >= 0 && n_folder_create(in("gone")) == 0 && n_watch_take(up) == 1);
    n_handle g = watch("gone", N_WATCH_FOLDER), x = watch("gone\\x.txt", N_WATCH_NAME);
    CHECK(g >= 0 && x >= 0);
    if (q < 0 || g < 0 || x < 0) return;
    CHECK(n_events_watch(q, g, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, 1) == 0);
    CHECK(n_events_watch(q, x, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, 2) == 0);
    CHECK(n_entry_move(in("gone"), in("went")) == 0 && n_watch_take(up) == 1);
    CHECK(n_watch_take(g) == 0 && n_watch_take(x) == 0); /* its name changed in its folder's folder */
    CHECK(save("went\\x.txt", "x") && n_watch_take(g) == 1 && n_watch_take(x) == 1);
    /* a change not taken yet, then the folder goes */
    CHECK(n_entry_remove(in("went\\x.txt")) == 0 && n_entry_remove(in("went")) == 0);
    struct n_event ev[4];
    n_result n = n_events_wait(q, ev, 4, 0);
    CHECK(n == 2);
    for (n_result i = 0; i < n && i < 4; i++) CHECK(ev[i].events == (N_EVENT_RECEIVABLE | N_EVENT_CLOSED));
    CHECK(n_watch_take(g) == 1 && n_watch_take(g) == -N_ERR_CLOSED && n_watch_take(g) == -N_ERR_CLOSED);
    CHECK(n_watch_take(x) == 1 && n_watch_take(x) == -N_ERR_CLOSED);
    n = n_events_wait(q, ev, 4, 0); /* level: closed for as long as they are open */
    CHECK(n == 2 && ev[0].events == N_EVENT_CLOSED && ev[1].events == N_EVENT_CLOSED);
    /* a new folder of that name is another folder */
    CHECK(n_folder_create(in("went")) == 0 && save("went\\x.txt", "x") && n_watch_take(x) == -N_ERR_CLOSED);
    CHECK(n_watch_take(up) == 0); /* "went" is not its name */
    n_handle_close(g);
    n_handle_close(x);
    CHECK(n_events_wait(q, ev, 4, 0) == 0); /* closed watches leave the queue */
    n_handle_close(up);
    n_handle_close(q);
    remove_all(in("went"));
}

/* What apps do not see counts nothing: a save makes its replacement in C:\Nocturne\Temp (PRIVATE),
   and a document removed while open waits there. */
static void unseen(void) {
    n_handle sys = n_entry_watch("C:\\Nocturne", N_WATCH_FOLDER);
    CHECK(sys >= 0);
    CHECK(save("p.txt", "1") && save("p.txt", "2"));
    n_handle r = n_document_open(in("p.txt"), N_DOCUMENT_READ);
    CHECK(r >= 0 && n_entry_remove(in("p.txt")) == 0);
    n_handle_close(r);
    CHECK(n_watch_take(sys) == 0);
    n_handle_close(sys);
}

/* 1000 changes are 1000. */
static void many(void) {
    CHECK(save("n0", "n"));
    n_handle f = watch(NULL, N_WATCH_FOLDER), n0 = watch("n0", N_WATCH_NAME);
    CHECK(f >= 0 && n0 >= 0);
    bool moved = true;
    for (int i = 0; i < 1000; i++) moved &= n_entry_move(in(i % 2 ? "n1" : "n0"), in(i % 2 ? "n0" : "n1")) == 0;
    CHECK(moved);
    n_result nf = n_watch_take(f), nn = n_watch_take(n0);
    printf("entrywatchtest: 1000 renames counted %lld by the folder's watch, %lld by the name's\n", (long long)nf,
           (long long)nn);
    CHECK(nf == 1000 && nn == 1000);
    bool saved = true;
    for (int i = 0; i < 50; i++) saved &= save("n0", i % 2 ? "odd" : "even");
    CHECK(saved && n_watch_take(n0) == 50 && n_watch_take(f) == 50);
    n_handle_close(f);
    n_handle_close(n0);
    n_entry_remove(in("n0"));
}

static uint64_t saves(int k) {
    char text[32];
    uint64_t t0 = n_clock_uptime_ms();
    for (int i = 0; i < k; i++) {
        snprintf(text, sizeof text, "save %d", i);
        n_document_save(in("speed.txt"), text, strlen(text));
    }
    return n_clock_uptime_ms() - t0;
}

static uint64_t renames(int k) {
    uint64_t t0 = n_clock_uptime_ms();
    for (int i = 0; i < k; i++) n_entry_move(in(i % 2 ? "r1" : "r0"), in(i % 2 ? "r0" : "r1"));
    return n_clock_uptime_ms() - t0;
}

/* A save and a rename cost what they did with watches open in their folder: without, with 102 (the
   folder's, the two names that change and 99 others), without again. */
static void speed(void) {
    CHECK(save("speed.txt", "s") && save("r0", "r"));
    uint64_t s0 = saves(100), r0 = renames(2000);
    enum { NAMES = 99 };
    n_handle w[NAMES + 3];
    int open = 0;
    w[open++] = watch(NULL, N_WATCH_FOLDER);
    w[open++] = watch("speed.txt", N_WATCH_NAME);
    w[open++] = watch("r0", N_WATCH_NAME);
    for (int i = 0; i < NAMES; i++) {
        char name[16];
        snprintf(name, sizeof name, "w%d", i);
        w[open++] = watch(name, N_WATCH_NAME);
    }
    bool all = true;
    for (int i = 0; i < open; i++) all &= w[i] >= 0;
    CHECK(all);
    uint64_t s1 = saves(100), r1 = renames(2000);
    CHECK(n_watch_take(w[0]) == 2100 && n_watch_take(w[1]) == 100 && n_watch_take(w[2]) == 2000 && n_watch_take(w[3]) == 0);
    for (int i = 0; i < open; i++) n_handle_close(w[i]);
    uint64_t s2 = saves(100), r2 = renames(2000);
    printf("entrywatchtest: 100 saves %llu ms without watches, %llu ms with %d, %llu ms without again\n",
           (unsigned long long)s0, (unsigned long long)s1, open, (unsigned long long)s2);
    printf("entrywatchtest: 2000 renames %llu ms without watches, %llu ms with %d, %llu ms without again\n",
           (unsigned long long)r0, (unsigned long long)r1, open, (unsigned long long)r2);
    /* within half again the mean of the two without (and a little for the clock's steps) */
    CHECK(s1 <= (s0 + s2) * 3 / 4 + 100);
    CHECK(r1 <= (r0 + r2) * 3 / 4 + 50);
    n_entry_remove(in("speed.txt"));
    n_entry_remove(in("r0"));
}

int main(void) {
    char name[64];
    snprintf(name, sizeof name, "entrywatchtest-%llu", (unsigned long long)n_clock_uptime_ms());
    if (n_path_join(n_temp_folder(), name, root, sizeof root) < 0 || n_folder_create(root) != 0) {
        printf("entrywatchtest: no folder of its own in %s\n", n_temp_folder());
        printf("entrywatchtest FAIL\n");
        return 1;
    }
    errors();
    names();
    told();
    gone();
    unseen();
    many();
    speed();
    CHECK(remove_all(root));
    printf("entrywatchtest: %u checks, %u failed\n", checks, failed);
    return failed != 0;
}
