/* user/libc/webprefs.c（ブラウザーの好み）のホストの試験。check_webprefs.py が製品の webprefs.c を
   名前を変えて何度も組み（p0, p1, …：一つ一つが別のプロセスで、静的な状態を共有しない）、ここが
   C: の文書・claim・見張り・待ち行列を記憶の上で真似る偽の世界と、試験の本文を持つ。範囲を限った
   裏付けで、カーネルの見張り（K1）・ゲストのブラウザー・設定の窓での受け入れではない。

   偽の世界が真似る意味（kernel/src/fs/document.c と store.c、user/include/nocturne.h の説明）：
   - READ の opening は開いた時の内容を読み続ける。EDIT（CREATE で作ったものも）は文書そのものを読む。
   - n_document_save は名前の文書の中身を丸ごと替え、文書そのもの（claim の相手）は同じまま
     （document.c の replace：ns_take で同じ id へ移す）。EDIT の opening はその後は新しい中身を読む。
   - claim は文書ごとで、wait なら持ち主が放す（release か close）まで待つ。
   - 見張り（N_WATCH_NAME）は作成・保存・削除を数え、フォルダーが除かれると CLOSED になる。
   - 待ち行列はレベルで、登録の interest に入る印だけを報告する。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <webprefs.h>
#include <webstorage.h>
#include "webprefs_processes.h"

/* Win32 の錠・条件・スレッド。windows.h は nocturne.h の名前と重なるので、使う物だけを宣言する。 */
typedef struct { void *p; } srw_lock;
typedef struct { void *p; } condition;
__declspec(dllimport) void __stdcall AcquireSRWLockExclusive(srw_lock *lock);
__declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(srw_lock *lock);
__declspec(dllimport) int __stdcall SleepConditionVariableSRW(condition *c, srw_lock *lock, unsigned long ms, unsigned long flags);
__declspec(dllimport) void __stdcall WakeAllConditionVariable(condition *c);
__declspec(dllimport) void *__stdcall CreateThread(void *security, size_t stack, unsigned long (__stdcall *start)(void *),
                                                   void *arg, unsigned long flags, unsigned long *id);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *thread, unsigned long ms);
__declspec(dllimport) int __stdcall CloseHandle(void *handle);

static unsigned checks, failed;
static void check(bool ok, const char *label) {
    checks++;
    if (!ok) {
        failed++;
        printf("失敗 %s\n", label);
    }
}
static void checkf(bool ok, const char *format, ...) {
    char text[600];
    va_list a;
    va_start(a, format);
    vsnprintf(text, sizeof text, format, a);
    va_end(a);
    check(ok, text);
}

/* 錠の写し（stub の threads.h）から：各スレッドが今持つ錠の数。 */
static _Thread_local int locks_held;
void webprefs_test_locked(int delta) { locks_held += delta; }
void webprefs_test_lock_failure(const char *what) {
    printf("失敗 錠：%s\n", what);
    fflush(stdout);
    exit(3);
}

/* ---- 偽の C: ---- */

#define FOLDER "C:\\Users\\User\\AppData\\Browser"
#define DOCUMENT FOLDER "\\preferences"
#define H "Nocturne browser preferences 1\n"

static srw_lock world;
static condition world_wake;
static long fallible, fail_at = -1; /* 失敗しうる呼出しの通し番号と、失敗させる番号 */
static int live_allocations, unexpected;
static char notes[16384] = " ";
static size_t notes_length = 1;
static bool folder_exists = true, profile_found = true, document_is_folder, watch_unsupported;
static bool doc_exists;
static char *doc_bytes;
static size_t doc_size;
static n_handle doc_owner = -1;
static unsigned saves, waiting, most_waiting;
static size_t short_reads;
static void (*before_read_open)(void);
static bool race_armed, race_inside, race_timeout;

enum { FREE, QUEUE, READING, EDITING, WATCHING };
struct fake {
    int kind;
    char *bytes; /* READING：開いた時の内容 */
    size_t size;
    uint64_t changes;
    bool gone;
};
#define HANDLES 512
static struct fake handles[HANDLES];
struct registration {
    bool used;
    n_handle queue, handle;
    uint32_t interest;
    uint64_t token;
};
static struct registration registrations[256];

static void enter(void) { AcquireSRWLockExclusive(&world); }
static void leave(void) { ReleaseSRWLockExclusive(&world); }
static bool failing(void) { return fallible++ == fail_at; }
static void note(const char *what) {
    size_t n = strlen(what);
    if (notes_length + n + 2 >= sizeof notes) return;
    memcpy(notes + notes_length, what, n);
    notes_length += n;
    notes[notes_length++] = ' ';
    notes[notes_length] = 0;
}
static bool is_document(const char *p) { return p && !_stricmp(p, DOCUMENT); }
static bool is_folder(const char *p) { return p && !_stricmp(p, FOLDER); }
static struct fake *fake_of(n_handle h) { return h >= 3 && h < HANDLES && handles[h].kind ? &handles[h] : NULL; }
static n_handle fake_new(int kind) {
    for (n_handle h = 3; h < HANDLES; h++)
        if (!handles[h].kind) {
            handles[h] = (struct fake){.kind = kind};
            return h;
        }
    return -N_ERR_NO_MEMORY;
}
/* 名前の変化を見張りに数える。 */
static void counted(void) {
    for (n_handle h = 3; h < HANDLES; h++)
        if (handles[h].kind == WATCHING && !handles[h].gone) handles[h].changes++;
}
static char *copy(const void *b, size_t n) {
    char *c = malloc(n ? n : 1);
    if (n) memcpy(c, b, n);
    return c;
}

/* webprefs.c の malloc（webprefs_fault.h）：失敗を注入でき、生きている割り当てを数える。 */
void *webprefs_test_malloc(size_t n) {
    enter();
    bool f = failing();
    if (!f) live_allocations++;
    leave();
    return f ? NULL : malloc(n);
}
void *webprefs_test_calloc(size_t n, size_t size) {
    enter();
    bool f = failing();
    if (!f) live_allocations++;
    leave();
    return f ? NULL : calloc(n, size);
}
void *webprefs_test_realloc(void *p, size_t n) {
    enter();
    bool f = failing();
    if (!f && !p) live_allocations++;
    leave();
    return f ? NULL : realloc(p, n);
}
void webprefs_test_free(void *p) {
    if (!p) return;
    enter();
    live_allocations--;
    leave();
    free(p);
}

const char *web_profile_folder(bool temporary) {
    enter();
    note("folder");
    bool f = failing(), found = profile_found;
    if (temporary) unexpected++;
    leave();
    return f || !found || temporary ? NULL : FOLDER;
}
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) {
    enter();
    bool f = failing();
    leave();
    if (f) return -N_ERR_TOO_LARGE;
    int n = snprintf(out, cap, "%s\\%s", folder, name);
    return n < 0 || (size_t)n >= cap ? -N_ERR_TOO_LARGE : n;
}
n_handle n_document_open(const char *path, int access) {
    if (access == N_DOCUMENT_READ && before_read_open) {
        void (*hook)(void) = before_read_open;
        before_read_open = NULL;
        hook();
    }
    enter();
    note(access == N_DOCUMENT_READ ? "open-read" : access == N_DOCUMENT_EDIT ? "open-edit"
         : access == N_DOCUMENT_CREATE ? "open-create" : "open-other");
    n_handle r;
    if (failing()) r = -N_ERR_NO_MEMORY;
    else if (!is_document(path)) { unexpected++; r = -N_ERR_NOT_FOUND; }
    else if (!folder_exists) r = -N_ERR_NOT_FOUND;
    else if (document_is_folder) r = -N_ERR_IS_FOLDER;
    else if (access == N_DOCUMENT_READ || access == N_DOCUMENT_EDIT) {
        r = !doc_exists ? -N_ERR_NOT_FOUND : fake_new(access == N_DOCUMENT_READ ? READING : EDITING);
        if (r >= 0 && access == N_DOCUMENT_READ) {
            handles[r].bytes = copy(doc_bytes, doc_size);
            handles[r].size = doc_size;
        }
    } else if (access == N_DOCUMENT_CREATE) {
        r = doc_exists ? -N_ERR_EXISTS : fake_new(EDITING);
        if (r >= 0) {
            free(doc_bytes);
            doc_bytes = NULL;
            doc_size = 0;
            doc_exists = true;
            counted();
        }
    } else { unexpected++; r = -N_ERR_INVALID; }
    leave();
    return r;
}
n_result n_document_size(n_handle h) {
    enter();
    note("size");
    struct fake *f = fake_of(h);
    n_result r;
    if (failing()) r = -N_ERR_IO;
    else if (!f || (f->kind != READING && f->kind != EDITING)) { unexpected++; r = -N_ERR_WRONG_KIND; }
    else r = (n_result)(f->kind == READING ? f->size : doc_size);
    leave();
    return r;
}
n_result n_document_read_at(n_handle h, uint64_t offset, void *buf, size_t n) {
    enter();
    note("read");
    struct fake *f = fake_of(h);
    if (race_armed && f && f->kind == EDITING) {
        /* 競りの試験：claim を持ったまま、もう一人の書き手が claim を待ち始めるまで止まる。 */
        race_armed = false;
        race_inside = true;
        WakeAllConditionVariable(&world_wake);
        for (int i = 0; i < 200 && !waiting; i++) SleepConditionVariableSRW(&world_wake, &world, 50, 0);
        if (!waiting) race_timeout = true;
    }
    n_result r;
    if (failing()) r = -N_ERR_IO;
    else if (!f || (f->kind != READING && f->kind != EDITING)) { unexpected++; r = -N_ERR_WRONG_KIND; }
    else {
        const char *b = f->kind == READING ? f->bytes : doc_bytes;
        size_t size = f->kind == READING ? f->size : doc_size;
        size_t k = offset >= size ? 0 : MIN(n, size - (size_t)offset);
        if (short_reads && k > short_reads) k = short_reads;
        if (k) memcpy(buf, b + offset, k);
        r = (n_result)k;
    }
    leave();
    return r;
}
n_result n_document_claim(n_handle h, bool wait) {
    enter();
    note("claim");
    struct fake *f = fake_of(h);
    n_result r = 0;
    if (failing()) r = -N_ERR_STOPPED;
    else if (!f || f->kind != EDITING) { unexpected++; r = -N_ERR_WRONG_KIND; }
    else {
        if (!wait) unexpected++;
        for (int i = 0; doc_owner >= 0 && doc_owner != h; i++) {
            if (i == 200) { r = -N_ERR_TIMEOUT; race_timeout = true; break; }
            waiting++;
            if (waiting > most_waiting) most_waiting = waiting;
            WakeAllConditionVariable(&world_wake);
            SleepConditionVariableSRW(&world_wake, &world, 50, 0);
            waiting--;
        }
        if (r >= 0) doc_owner = h;
    }
    leave();
    return r;
}
n_result n_document_release(n_handle h) {
    enter();
    note("release");
    n_result r = doc_owner == h ? 0 : -N_ERR_INVALID;
    if (!r) {
        doc_owner = -1;
        WakeAllConditionVariable(&world_wake);
    } else unexpected++;
    leave();
    return r;
}
n_result n_document_save(const char *path, const void *data, size_t size) {
    enter();
    note("save");
    n_result r = 0;
    if (failing()) r = -N_ERR_NO_SPACE;
    else if (!is_document(path)) { unexpected++; r = -N_ERR_NOT_FOUND; }
    else if (!folder_exists) r = -N_ERR_NOT_FOUND;
    else if (document_is_folder) r = -N_ERR_IS_FOLDER;
    else {
        free(doc_bytes);
        doc_bytes = copy(data, size);
        doc_size = size;
        doc_exists = true;
        saves++;
        counted();
    }
    leave();
    return r;
}
n_result n_handle_close(n_handle h) {
    enter();
    note("close");
    struct fake *f = fake_of(h);
    n_result r = 0;
    if (!f) { unexpected++; r = -N_ERR_NO_HANDLE; }
    else {
        if (doc_owner == h) {
            doc_owner = -1;
            WakeAllConditionVariable(&world_wake);
        }
        for (size_t i = 0; i < sizeof registrations / sizeof *registrations; i++)
            if (registrations[i].used && (registrations[i].handle == h || registrations[i].queue == h)) registrations[i].used = false;
        free(f->bytes);
        *f = (struct fake){0};
    }
    leave();
    return r;
}
n_handle n_entry_watch(const char *path, uint32_t what) {
    enter();
    note("watch");
    n_handle r;
    if (failing()) r = -N_ERR_NO_MEMORY;
    else if (watch_unsupported) r = -N_ERR_UNSUPPORTED;
    else if (what != N_WATCH_NAME || !is_document(path)) { unexpected++; r = -N_ERR_INVALID; }
    else if (!folder_exists) r = -N_ERR_NOT_FOUND;
    else r = fake_new(WATCHING);
    leave();
    return r;
}
n_result n_watch_take(n_handle h) {
    enter();
    note("take");
    struct fake *f = fake_of(h);
    n_result r;
    if (!f || f->kind != WATCHING) { unexpected++; r = f ? -N_ERR_WRONG_KIND : -N_ERR_NO_HANDLE; }
    else {
        r = f->changes ? (n_result)f->changes : f->gone ? -N_ERR_CLOSED : 0;
        f->changes = 0;
    }
    leave();
    return r;
}
n_result n_events_watch(n_handle q, n_handle h, uint32_t interest, uint64_t token) {
    enter();
    note("events-watch");
    struct fake *queue = fake_of(q);
    n_result r = 0;
    if (failing()) r = -N_ERR_NO_MEMORY;
    else if (!queue) r = -N_ERR_NO_HANDLE;
    else if (queue->kind != QUEUE) r = -N_ERR_WRONG_KIND;
    else if (!fake_of(h)) r = -N_ERR_NO_HANDLE;
    else if (interest != (N_EVENT_RECEIVABLE | N_EVENT_CLOSED) || token != WEBPREFS_TOKEN) { unexpected++; r = -N_ERR_INVALID; }
    else {
        struct registration *at = NULL;
        for (size_t i = 0; i < sizeof registrations / sizeof *registrations; i++) {
            struct registration *g = &registrations[i];
            if (g->used && g->queue == q && g->token == token) { at = g; break; }
            if (!g->used && !at) at = g;
        }
        if (!at) r = -N_ERR_NO_MEMORY;
        else *at = (struct registration){true, q, h, interest, token};
    }
    leave();
    return r;
}
n_result n_events_unwatch(n_handle q, uint64_t token) {
    enter();
    note("unwatch");
    n_result r = 0;
    for (size_t i = 0; i < sizeof registrations / sizeof *registrations; i++)
        if (registrations[i].used && registrations[i].queue == q && registrations[i].token == token) {
            registrations[i].used = false;
            r = 1;
        }
    leave();
    return r;
}
n_result n_folder_create(const char *path) {
    enter();
    note("folder-create");
    n_result r;
    if (failing()) r = -N_ERR_NO_SPACE;
    else if (!is_folder(path)) { unexpected++; r = -N_ERR_INVALID; }
    else if (folder_exists) r = -N_ERR_EXISTS;
    else { folder_exists = true; r = 0; }
    leave();
    return r;
}

/* ---- 試験から世界を動かす（別のプロセスの保存や、利用者の操作の代わり） ---- */

static void world_reset(void) {
    enter();
    folder_exists = profile_found = true;
    document_is_folder = watch_unsupported = false;
    free(doc_bytes);
    doc_bytes = NULL;
    doc_size = 0;
    doc_exists = false;
    short_reads = 0;
    fail_at = -1;
    leave();
}
static void put_bytes(const char *b, size_t n) {
    enter();
    free(doc_bytes);
    doc_bytes = copy(b, n);
    doc_size = n;
    doc_exists = true;
    counted();
    leave();
}
static void put(const char *text) { put_bytes(text, strlen(text)); }
static void drop_document(void) {
    enter();
    if (doc_exists) {
        free(doc_bytes);
        doc_bytes = NULL;
        doc_size = 0;
        doc_exists = false;
        counted();
    }
    leave();
}
static void drop_folder(void) {
    enter();
    if (doc_exists) counted();
    free(doc_bytes);
    doc_bytes = NULL;
    doc_size = 0;
    doc_exists = false;
    folder_exists = false;
    for (n_handle h = 3; h < HANDLES; h++)
        if (handles[h].kind == WATCHING) handles[h].gone = true;
    leave();
}
static char *view;
static const char *document(void) {
    enter();
    free(view);
    view = NULL;
    if (doc_exists) {
        view = malloc(doc_size + 1);
        memcpy(view, doc_bytes, doc_size);
        view[doc_size] = 0;
    }
    leave();
    return view ? view : "<文書なし>";
}
static unsigned saved(void) {
    enter();
    unsigned n = saves;
    leave();
    return n;
}
static void notes_clear(void) {
    enter();
    notes[0] = ' ';
    notes[1] = 0;
    notes_length = 1;
    leave();
}
/* 記録の中の最初の what の位置（無ければ -1）。 */
static long first_note(const char *what) {
    char pattern[64];
    snprintf(pattern, sizeof pattern, " %s ", what);
    enter();
    const char *at = strstr(notes, pattern);
    long r = at ? (long)(at - notes) : -1;
    leave();
    return r;
}
static bool noted(const char *what) { return first_note(what) >= 0; }
static size_t queue_wait(n_handle q, struct n_event *out, size_t max) {
    enter();
    size_t n = 0;
    for (size_t i = 0; i < sizeof registrations / sizeof *registrations; i++) {
        struct registration *g = &registrations[i];
        if (!g->used || g->queue != q) continue;
        struct fake *f = fake_of(g->handle);
        uint32_t ready = f && f->kind == WATCHING ? (f->changes ? N_EVENT_RECEIVABLE : 0) | (f->gone ? N_EVENT_CLOSED : 0) : 0;
        ready &= g->interest;
        if (ready && n < max) out[n++] = (struct n_event){.token = g->token, .events = ready};
    }
    leave();
    return n;
}
static size_t registered(n_handle q) {
    enter();
    size_t n = 0;
    for (size_t i = 0; i < sizeof registrations / sizeof *registrations; i++)
        if (registrations[i].used && registrations[i].queue == q) n++;
    leave();
    return n;
}
static size_t open_of(int kind) {
    enter();
    size_t n = 0;
    for (n_handle h = 3; h < HANDLES; h++)
        if (handles[h].kind == kind) n++;
    leave();
    return n;
}
static void faults(long at) {
    enter();
    fallible = 0;
    fail_at = at;
    leave();
}
static long faults_end(void) {
    enter();
    long n = fallible;
    fail_at = -1;
    leave();
    return n;
}

/* ---- プロセス ---- */

struct process {
    const char *name;
    const struct webprefs_language *(*catalog)(size_t *count);
    n_result (*watch)(n_handle queue);
    void (*events)(const struct n_event *events, size_t count);
    void (*sync)(void);
    uint64_t (*generation)(void);
    n_result (*languages)(char *out, size_t cap);
    bool (*languages_default)(void);
    n_result (*accept_language)(char *out, size_t cap);
    bool (*fit)(const char *const *tags, size_t count);
    bool (*search_history)(void);
    n_result (*set_languages)(const char *const *tags, size_t count);
    n_result (*reset_languages)(void);
    n_result (*set_search_history)(bool on);
    n_handle queue;
};
#define PROCESS_API(p)                                                                     \
    const struct webprefs_language *p##_webprefs_catalog(size_t *count);                   \
    n_result p##_webprefs_watch(n_handle queue);                                           \
    void p##_webprefs_events(const struct n_event *events, size_t count);                  \
    void p##_webprefs_sync(void);                                                          \
    uint64_t p##_webprefs_languages_generation(void);                                      \
    n_result p##_webprefs_languages(char *out, size_t cap);                                \
    bool p##_webprefs_languages_default(void);                                             \
    n_result p##_webprefs_accept_language(char *out, size_t cap);                          \
    bool p##_webprefs_languages_fit(const char *const *tags, size_t count);                \
    bool p##_webprefs_search_history(void);                                                \
    n_result p##_webprefs_set_languages(const char *const *tags, size_t count);            \
    n_result p##_webprefs_reset_languages(void);                                           \
    n_result p##_webprefs_set_search_history(bool on);
PROCESSES(PROCESS_API)
#define PROCESS_ENTRY(p)                                                                                            \
    {#p, p##_webprefs_catalog, p##_webprefs_watch, p##_webprefs_events, p##_webprefs_sync,                         \
     p##_webprefs_languages_generation, p##_webprefs_languages, p##_webprefs_languages_default,                    \
     p##_webprefs_accept_language, p##_webprefs_languages_fit, p##_webprefs_search_history,                        \
     p##_webprefs_set_languages, p##_webprefs_reset_languages, p##_webprefs_set_search_history, -1},
static struct process processes[] = {PROCESSES(PROCESS_ENTRY)};
#define PROCESS_COUNT (sizeof processes / sizeof *processes)
static size_t used_processes;

/* 新しいプロセス（まだ何も読んでいない）と、その待ち行列。 */
static struct process *fresh(void) {
    if (used_processes == PROCESS_COUNT) {
        printf("失敗 プロセスの写しが足りない（check_webprefs.py の PROCESSES を増やす）\n");
        exit(2);
    }
    struct process *p = &processes[used_processes++];
    enter();
    p->queue = fake_new(QUEUE);
    leave();
    return p;
}
static struct process *watching(void) {
    struct process *p = fresh();
    n_result r = p->watch(p->queue);
    checkf(r == 0, "%s が見張りを置ける（%lld）", p->name, (long long)r);
    return p;
}
/* 待ち行列の今の報告を主ループのように渡す：渡した数。 */
static size_t deliver(struct process *p) {
    struct n_event events[8];
    size_t n = queue_wait(p->queue, events, 8);
    if (n) p->events(events, n);
    return n;
}
static char ring[8][300];
static unsigned ring_next;
static const char *langs(struct process *p) {
    char *b = ring[ring_next++ % 8];
    n_result r = p->languages(b, 300);
    if (r < 0) snprintf(b, 300, "<失敗 %lld>", (long long)r);
    else if ((size_t)r != strlen(b)) snprintf(b, 300, "<長さが違う>");
    return b;
}
static const char *accept(struct process *p) {
    char *b = ring[ring_next++ % 8];
    n_result r = p->accept_language(b, 300);
    if (r < 0) snprintf(b, 300, "<失敗 %lld>", (long long)r);
    else if ((size_t)r != strlen(b)) snprintf(b, 300, "<長さが違う>");
    return b;
}
/* 呼出しの後：文書の opening・claim・割り当て・錠が残らず、偽の C: を想定どおりに使った。 */
static void settled(const char *label) {
    size_t documents = open_of(READING) + open_of(EDITING);
    enter();
    n_handle owner = doc_owner;
    int live = live_allocations, odd = unexpected;
    unexpected = 0;
    leave();
    checkf(!documents && owner < 0 && !live && !locks_held && !odd,
           "%s：opening %zu・claim %d・割り当て %d・錠 %d・想定外の使い方 %d が残らない", label, documents, (int)owner, live,
           locks_held, odd);
}

/* Accept-Language の規則を試験の側で書いたもの：一番目は q なし、二番目から 0.9 で 0.1 ずつ下がり、
   0.1 で止まる（webprefs.h）。 */
static void spec_accept(const char *const *tags, size_t count, char *out, size_t cap) {
    size_t at = 0;
    out[0] = 0;
    for (size_t i = 0; i < count && at < cap; i++)
        at += (size_t)(i ? snprintf(out + at, cap - at, ",%s;q=0.%d", tags[i], i < 9 ? 10 - (int)i : 1)
                         : snprintf(out + at, cap - at, "%s", tags[i]));
}
static void join(const char *const *tags, size_t count, char *out, size_t cap) {
    size_t at = 0;
    out[0] = 0;
    for (size_t i = 0; i < count && at < cap; i++) at += (size_t)snprintf(out + at, cap - at, "%s%s", i ? "," : "", tags[i]);
}

/* RFC 5646 §2.1 の langtag のうち目録が使う形（language 2〜3 字、script 4 字、region 2 字か 3 桁）で、
   §2.1.1 の大小（language は小文字、script は頭だけ大文字、region は大文字）か。 */
static bool tag_form(const char *t) {
    size_t n = strcspn(t, "-");
    if (n < 2 || n > 3) return false;
    for (size_t i = 0; i < n; i++)
        if (t[i] < 'a' || t[i] > 'z') return false;
    t += n;
    if (*t == '-' && strcspn(t + 1, "-") == 4) {
        if (t[1] < 'A' || t[1] > 'Z') return false;
        for (int i = 2; i <= 4; i++)
            if (t[i] < 'a' || t[i] > 'z') return false;
        t += 5;
    }
    if (*t == '-') {
        size_t k = strcspn(t + 1, "-");
        bool letters = k == 2 && t[1] >= 'A' && t[1] <= 'Z' && t[2] >= 'A' && t[2] <= 'Z';
        bool digits = k == 3 && t[1] >= '0' && t[1] <= '9' && t[2] >= '0' && t[2] <= '9' && t[3] >= '0' && t[3] <= '9';
        if (!letters && !digits) return false;
        t += 1 + k;
    }
    return !*t;
}
static bool utf8_text(const char *s) {
    if (!s || !*s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c = *p;
        size_t n = c < 0x80 ? 1 : c >= 0xC2 && c < 0xE0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : c >= 0xF0 && c < 0xF5 ? 4 : 0;
        if (!n || (n == 1 && c < 0x20)) return false;
        for (size_t i = 1; i < n; i++)
            if ((p[i] & 0xC0) != 0x80) return false;
        p += n;
    }
    return true;
}

/* ---- スレッドの試験 ---- */

struct writer {
    struct process *p;
    n_result result;
};
static unsigned long __stdcall write_languages(void *arg) {
    struct writer *w = arg;
    const char *tags[] = {"de"};
    w->result = w->p->set_languages(tags, 1);
    return 0;
}
static unsigned long __stdcall write_history(void *arg) {
    struct writer *w = arg;
    w->result = w->p->set_search_history(true);
    return 0;
}
static const char *const stress_lists[2] = {"ja,en-US", "fr"};
static const char *const stress_accepts[2] = {"ja,en-US;q=0.9", "fr"};
static int stress_stop;
struct stress {
    struct process *p;
    int mode;
    unsigned calls, bad, held;
};
static unsigned long __stdcall stress_run(void *arg) {
    struct stress *s = arg;
    char b[300];
    uint64_t last = 0;
    while (!__atomic_load_n(&stress_stop, __ATOMIC_ACQUIRE)) {
        uint64_t g = s->p->generation();
        if (g < last) s->bad++;
        last = g;
        if (s->mode == 0) {
            if (s->p->languages(b, sizeof b) < 0 || (strcmp(b, stress_lists[0]) && strcmp(b, stress_lists[1]))) s->bad++;
        } else if (s->mode == 1) {
            if (s->p->accept_language(b, sizeof b) < 0 || (strcmp(b, stress_accepts[0]) && strcmp(b, stress_accepts[1]))) s->bad++;
        } else {
            s->p->sync();
            (void)s->p->search_history();
            (void)s->p->languages_default();
        }
        s->calls++;
    }
    s->held = (unsigned)locks_held;
    return 0;
}

static void hook_put_sv(void) { put(H "languages=sv\n"); }

int main(void) {
    char text[70000], expected[400];

    /* 目録：tag は §2.1 の形と §2.1.1 の大小、ASCII の大小を無視して一つずつ、名前は UTF-8 の文字列。
       既定の en-US がある。一覧の長さの上限（LIST_TAGS）は二文字より短い tag が無いことに頼る。 */
    {
        struct process *p = fresh();
        size_t count = 0;
        const struct webprefs_language *catalog = p->catalog(&count);
        bool forms = true, unique = true, names = true, en_us = false, short_tag = false;
        for (size_t i = 0; i < count; i++) {
            const char *t = catalog[i].tag;
            if (!tag_form(t)) { forms = false; printf("失敗 目録の tag \"%s\" が §2.1 の形か §2.1.1 の大小でない\n", t); }
            if (strlen(t) < 2) short_tag = true;
            for (size_t k = 0; k < i; k++)
                if (!_stricmp(catalog[k].tag, t)) unique = false;
            if (!utf8_text(catalog[i].own) || !utf8_text(catalog[i].english)) names = false;
            if (!strcmp(t, "en-US")) en_us = true;
            const char *one[] = {t};
            if (!p->fit(one, 1)) forms = false;
        }
        printf("目録 %zu 件\n", count);
        check(count > 0 && catalog, "目録がある");
        check(forms, "目録の tag はどれも RFC 5646 §2.1 の形で §2.1.1 の大小、一つだけでも一覧になる");
        check(unique, "目録の tag は大小を無視して一つずつ");
        check(names, "目録の自分の名前と英語の名前は空でない UTF-8 の文字列");
        check(en_us, "目録に既定の en-US がある");
        check(!short_tag, "目録に二文字より短い tag が無い");
        settled("目録");
    }

    /* 文書が無い：全部が既定で、読むだけでは書かない。見張りが先で、読むのが後。 */
    world_reset();
    {
        struct process *p = fresh();
        notes_clear();
        n_result r = p->watch(p->queue);
        long w = first_note("watch"), e = first_note("events-watch"), o = first_note("open-read");
        check(r == 0, "文書が無くても見張りを置ける");
        check(w >= 0 && e > w && o > e, "見張りを置いて待ち行列に登録してから文書を読む");
        check(!strcmp(langs(p), "en-US") && !strcmp(accept(p), "en-US") && !p->search_history() && p->languages_default(),
              "文書が無ければ言語は en-US、検索の履歴は off、言語は既定");
        p->sync();
        check(!noted("open-edit") && !noted("open-create") && !noted("save") && !strcmp(document(), "<文書なし>"),
              "読むだけでは文書を作らず書かない");
        check(registered(p->queue) == 1, "待ち行列に登録は一つ");
        check(p->watch(p->queue) == 0 && registered(p->queue) == 1 && open_of(WATCHING) == 1, "同じ待ち行列でもう一度呼んでも見張りは一つ");
        settled("文書が無い");
    }

    /* 見張りと読みの間の変化：読みに入り、見張りにも数えられ、世代は進まない。 */
    world_reset();
    put(H "languages=ja\n");
    {
        struct process *p = fresh();
        before_read_open = hook_put_sv;
        check(p->watch(p->queue) == 0 && !strcmp(langs(p), "sv"), "見張りを置いた後の変化は最初の読みに入る");
        check(deliver(p) == 1 && !strcmp(langs(p), "sv") && p->generation() == 0, "その変化は見張りにも届き、一覧は同じなので世代は進まない");
        settled("見張りの順");
    }

    /* 最初の行が違う：全部が既定。 */
    world_reset();
    {
        struct process *p = watching();
        static const char *const wrong[] = {
            "Nocturne browser preferences 2\nlanguages=ja\nsearch-history=on\n",
            "nocturne browser preferences 1\nlanguages=ja\nsearch-history=on\n",
            "Nocturne browser preferences 1\r\nlanguages=ja\nsearch-history=on\n",
            "\xEF\xBB\xBFNocturne browser preferences 1\nlanguages=ja\nsearch-history=on\n",
            "Nocturne browser preferences 1 \nlanguages=ja\nsearch-history=on\n",
            "Nocturne browser preferences 10\nlanguages=ja\nsearch-history=on\n",
            "languages=ja\nsearch-history=on\n",
            "",
        };
        for (size_t i = 0; i < sizeof wrong / sizeof *wrong; i++) {
            put(H "languages=ja\nsearch-history=on\n");
            deliver(p);
            bool before = !strcmp(langs(p), "ja") && p->search_history();
            put(wrong[i]);
            deliver(p);
            checkf(before && !strcmp(langs(p), "en-US") && !p->search_history(), "最初の行が違う文書（%zu 番）は全部が既定", i);
        }
        put("Nocturne browser preferences 1");
        deliver(p);
        check(!strcmp(langs(p), "en-US"), "最初の行だけで改行の無い文書は正しく、全部が既定");
        put(H "languages=ja");
        deliver(p);
        check(!strcmp(langs(p), "ja"), "最後の行に改行が無くても読む");
        settled("最初の行");
    }

    /* 知っている項目の値が不正：その項目だけ既定。同じ名前の行は最初のものだけ数える。 */
    world_reset();
    {
        struct process *p = watching();
        static const struct { const char *document, *languages; bool history; const char *label; } bad[] = {
            {H "languages=\nsearch-history=on\n", "en-US", true, "空の言語の一覧は既定、検索の履歴はそのまま"},
            {H "languages=ja\nsearch-history=yes\n", "ja", false, "検索の履歴の不正な値は既定（off）、言語はそのまま"},
            {H "languages=ja\nsearch-history=On\n", "ja", false, "検索の履歴の値は on・off の小文字だけ"},
            {H "languages=ja\r\nsearch-history=on\r\n", "en-US", false, "CR の付いた値は不正"},
            {H "languages=xx,qq-ZZ\nsearch-history=on\n", "en-US", true, "目録の tag が一つも無ければ既定"},
            {H "languages=fr\nlanguages=de\nsearch-history=on\nsearch-history=off\n", "fr", true, "同じ名前の行は最初のものが効く"},
            {H "languages=ja\nsearch-history\n", "ja", false, "= の無い行は知らない行"},
            {H "Languages=fr\nlanguages=it\n", "it", false, "名前は小文字だけで、大文字の名前は知らない行"},
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            put(bad[i].document);
            deliver(p);
            checkf(!strcmp(langs(p), bad[i].languages) && p->search_history() == bad[i].history, "値：%s（言語 %s）", bad[i].label, langs(p));
        }
        settled("不正な値");
    }

    /* 大小のそろえ・重複・目録に無い tag。 */
    world_reset();
    {
        struct process *p = watching();
        put(H "languages=JA, en-us ,ZH-tw,\tEs-419\t\n");
        deliver(p);
        check(!strcmp(langs(p), "ja,en-US,zh-TW,es-419"), "読む時は大小を無視して目録の形にそろえ、前後の空白とタブを除く");
        check(!strcmp(accept(p), "ja,en-US;q=0.9,zh-TW;q=0.8,es-419;q=0.7"), "そろえた一覧の Accept-Language");
        put(H "languages=ja,JA,en-US,ja,EN-US\n");
        deliver(p);
        check(!strcmp(langs(p), "ja,en-US"), "重複は最初のものだけ残す");
        put(H "languages=tlh,ja,x-klingon,en-GB-oed,i-klingon,ja-JP,,zh-Hant\n");
        deliver(p);
        check(!strcmp(langs(p), "ja"), "目録に無い tag（正しい形でも）は捨て、空の項目は数えない");
        put(H "languages=tlh,x-klingon\n");
        deliver(p);
        check(!strcmp(langs(p), "en-US") && p->languages_default(), "目録の tag が残らなければ既定");
        const char *upper[] = {"JA", "Es-419"};
        check(p->set_languages(upper, 2) == 0 && strstr(document(), "\nlanguages=ja,es-419\n") && !strcmp(langs(p), "ja,es-419"),
              "大文字で渡しても目録の形で書く");
        const char *twice[] = {"ja", "JA"};
        const char *unknown[] = {"tlh"}, *region[] = {"ja-JP"}, *empty[] = {""}, *nothing[] = {NULL};
        unsigned before = saved();
        notes_clear();
        check(p->set_languages(twice, 2) == -N_ERR_INVALID && !p->fit(twice, 2), "大小だけ違う同じ tag は二度目とみなす");
        check(p->set_languages(unknown, 1) == -N_ERR_INVALID && !p->fit(unknown, 1), "目録に無い tag は受けない");
        check(p->set_languages(region, 1) == -N_ERR_INVALID && !p->fit(region, 1), "目録に無い地域の組合せは受けない");
        check(p->set_languages(empty, 1) == -N_ERR_INVALID && p->set_languages(nothing, 1) == -N_ERR_INVALID, "空の tag と NULL は受けない");
        check(p->set_languages(upper, 0) == -N_ERR_INVALID && p->set_languages(NULL, 1) == -N_ERR_INVALID && !p->fit(upper, 0) &&
              !p->fit(NULL, 2), "空の一覧は受けない");
        check(saved() == before && !noted("open-edit") && !noted("open-create"), "受けない一覧では文書を開かない");
        settled("大小・重複・目録");
    }

    /* 128 バイトの限りと q の規則。 */
    world_reset();
    {
        struct process *p = watching();
        static const char *const two[] = {"af", "am", "ar", "az", "be", "bg", "bn", "br", "bs", "ca", "cs", "cy", "da", "de", "el", "et"};
        char list[400];
        check(p->fit(two, 15), "二文字の tag 15 個（Accept-Language はちょうど 128 バイト）は収まる");
        check(!p->fit(two, 16), "16 個（137 バイト）は収まらない");
        const char *with_long[15];
        memcpy(with_long, two, sizeof with_long);
        with_long[13] = "es-419";
        check(!p->fit(with_long, 15), "長い tag で 128 バイトを越える一覧は収まらない");
        check(p->fit(with_long, 14), "その一つ前までは収まる");
        unsigned before = saved();
        check(p->set_languages(two, 16) == -N_ERR_INVALID && saved() == before, "収まらない一覧は -N_ERR_INVALID で、文書は変わらない");
        check(p->set_languages(two, 15) == 0, "ちょうど 128 バイトの一覧を書ける");
        join(two, 15, list, sizeof list);
        spec_accept(two, 15, expected, sizeof expected);
        check(!strcmp(langs(p), list) && !strcmp(accept(p), expected) && strlen(accept(p)) == 128, "その一覧と 128 バイトの Accept-Language");
        char small[200];
        check(p->accept_language(small, 129) == 128 && p->accept_language(small, 128) == -N_ERR_TOO_LARGE, "Accept-Language は零を含めて 129 バイトの緩衝に入り、128 では -N_ERR_TOO_LARGE");
        size_t n = strlen(list);
        check(p->languages(small, n + 1) == (n_result)n && p->languages(small, n) == -N_ERR_TOO_LARGE && p->languages(NULL, 0) == -N_ERR_TOO_LARGE,
              "一覧の緩衝の大きさ");
        /* 手で書かれた長すぎる一覧は、収まる所までの頭を使う。 */
        join(two, 16, list, sizeof list);
        snprintf(text, sizeof text, H "languages=%s\n", list);
        put(text);
        deliver(p);
        join(two, 15, list, sizeof list);
        check(!strcmp(langs(p), list), "長すぎる一覧は収まる所まで（15 個）");
        snprintf(text, sizeof text, H "languages=es-419,%s\n", list);
        put(text);
        deliver(p);
        const char *es_first[14] = {"es-419"};
        memcpy(es_first + 1, two, 13 * sizeof *two);
        join(es_first, 14, list, sizeof list);
        check(!strcmp(langs(p), list), "長い tag が先なら、収まる 14 個まで");
        /* q の規則：一番目は q なし、0.9 から 0.1 ずつ下がり、10 番目から後は 0.1。 */
        static const char *const twelve[] = {"ja", "en-US", "en", "fr", "de", "es", "it", "pt-BR", "ko", "zh-CN", "ru", "nl"};
        check(p->set_languages(twelve, 12) == 0 &&
              !strcmp(accept(p), "ja,en-US;q=0.9,en;q=0.8,fr;q=0.7,de;q=0.6,es;q=0.5,it;q=0.4,pt-BR;q=0.3,ko;q=0.2,zh-CN;q=0.1,ru;q=0.1,nl;q=0.1"),
              "q の規則（今の webfetch.c の試験と同じ値）");
        bool rule = true;
        for (size_t k = 1; k <= 15; k++) {
            check(p->set_languages(two, k) == 0, "q の規則の一覧を書ける");
            spec_accept(two, k, expected, sizeof expected);
            if (strcmp(accept(p), expected)) { rule = false; printf("失敗 %zu 個：%s と %s\n", k, accept(p), expected); }
        }
        check(rule, "1〜15 個のどの長さでも Accept-Language は q の規則どおり");
        const char *bytes = accept(p);
        bool allowed = true;
        for (const char *c = bytes; *c; c++)
            if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || strchr(",;=.-", *c))) allowed = false;
        check(allowed, "Accept-Language は [0-9A-Za-z,;=.-] だけ（CORS-safelisted）");
        settled("128 バイトと q");
    }

    /* 知らない行を残す。一回の変更はその一行だけ。最初の行が違う文書・64 KiB の限り。 */
    world_reset();
    {
        struct process *p = watching();
        put(H "# a comment\nzoom=150\n\nLanguages=fr\nlanguages=de\nfuture-thing=1\nlanguages=it\nlast-no-lf");
        deliver(p);
        const char *ja[] = {"ja"};
        check(p->set_languages(ja, 1) == 0 &&
              !strcmp(document(), H "# a comment\nzoom=150\n\nLanguages=fr\nlanguages=ja\nfuture-thing=1\nlast-no-lf\n"),
              "知らない行（空の行・大文字の名前を含む）を順に残し、言語の行はその場で替え、後の同じ名前の行は除く");
        check(p->set_search_history(true) == 0 &&
              !strcmp(document(), H "# a comment\nzoom=150\n\nLanguages=fr\nlanguages=ja\nfuture-thing=1\nlast-no-lf\nsearch-history=on\n"),
              "無い項目の行は末尾に足す");
        check(p->reset_languages() == 0 &&
              !strcmp(document(), H "# a comment\nzoom=150\n\nLanguages=fr\nfuture-thing=1\nlast-no-lf\nsearch-history=on\n") &&
              !strcmp(langs(p), "en-US") && p->languages_default(),
              "既定に戻すと言語の行だけを除き、既定が効く");
        unsigned before = saved();
        check(p->reset_languages() == 0 && saved() == before, "変わらない変更は保存しない");
        put("Nocturne browser preferences 2\nlanguages=ja\nnew=1\n");
        deliver(p);
        check(p->set_search_history(true) == 0 && !strcmp(document(), H "search-history=on\n"),
              "最初の行が違う文書へ書くときは、この版の文書として始め直す");
        /* 64 KiB：ちょうどなら読み、越えれば全部が既定。 */
        size_t at = (size_t)snprintf(text, sizeof text, H "languages=ja\nfiller=");
        memset(text + at, 'x', 65536 - at - 1);
        text[65535] = '\n';
        put_bytes(text, 65536);
        deliver(p);
        check(!strcmp(langs(p), "ja"), "64 KiB ちょうどの文書は読む");
        before = saved();
        check(p->set_search_history(false) == -N_ERR_TOO_LARGE && saved() == before && !strcmp(langs(p), "ja"),
              "書いた結果が 64 KiB を越えるなら -N_ERR_TOO_LARGE で、文書も状態も変わらない");
        memset(text + 65535, 'x', 1);
        text[65536] = '\n';
        put_bytes(text, 65537);
        deliver(p);
        check(!strcmp(langs(p), "en-US"), "64 KiB を越える文書はこの版の文書でなく、全部が既定");
        check(p->set_search_history(true) == 0 && !strcmp(document(), H "search-history=on\n"), "それへ書くときは始め直す");
        /* 短い読みでも全部を読む。 */
        put(H "languages=ko,ja\nsearch-history=on\n");
        enter();
        short_reads = 3;
        leave();
        deliver(p);
        check(!strcmp(langs(p), "ko,ja") && p->search_history(), "一度に少しずつしか読めなくても全部を読む");
        enter();
        short_reads = 0;
        leave();
        settled("知らない行");
    }

    /* 検索の履歴：既定は off、on・off を書く、別のプロセスに届く。既定に戻す言語の問い。 */
    world_reset();
    {
        struct process *a = watching(), *b = watching();
        check(!a->search_history() && !b->search_history(), "検索の履歴の既定は off");
        check(a->set_search_history(true) == 0 && !strcmp(document(), H "search-history=on\n") && a->search_history(), "on を書き、書いたプロセスにはすぐ効く");
        check(deliver(b) == 1 && b->search_history(), "別のプロセスには見張りで届く");
        check(a->set_search_history(false) == 0 && !strcmp(document(), H "search-history=off\n") && !a->search_history(), "off を書く");
        check(deliver(b) == 1 && !b->search_history(), "off も届く");
        check(b->languages_default(), "言語の行が無ければ既定");
        const char *en_us[] = {"en-US"}, *ja[] = {"ja"};
        check(b->set_languages(en_us, 1) == 0 && b->languages_default() && strstr(document(), "languages=en-US\n"), "既定と同じ tag の行も既定");
        check(b->set_languages(ja, 1) == 0 && !b->languages_default(), "違う一覧は既定でない");
        deliver(a);
        check(!a->languages_default() && a->reset_languages() == 0 && a->languages_default(), "既定に戻すと既定");
        settled("検索の履歴");
    }

    /* 既定に戻す：文書が無ければ何も作らない。 */
    world_reset();
    {
        struct process *p = watching();
        notes_clear();
        check(p->reset_languages() == 0 && !strcmp(document(), "<文書なし>") && !noted("open-create") && !noted("save"),
              "文書が無ければ既定に戻しても文書を作らない");
        settled("文書の無い既定");
    }

    /* 二人の書き手が別の項目を変えても両方残る（古い写しのまま書いても、claim の下で読み直す）。 */
    world_reset();
    {
        struct process *a = watching(), *b = watching();
        put(H "languages=fr\nsearch-history=off\nzoom=2\n");
        deliver(a);
        deliver(b);
        const char *ja[] = {"ja"};
        check(!strcmp(langs(a), "fr") && !strcmp(langs(b), "fr"), "二つのプロセスが同じ文書を読む");
        check(a->set_languages(ja, 1) == 0, "A が言語を変える");
        check(!strcmp(langs(b), "fr"), "B はまだ見張りの知らせを受けていない");
        check(b->set_search_history(true) == 0, "B が古い写しのまま検索の履歴を変える");
        check(!strcmp(document(), H "languages=ja\nsearch-history=on\nzoom=2\n"), "両方の変更と知らない行が残る");
        check(!strcmp(langs(b), "ja") && b->search_history(), "B は書いた時に読んだ文書を持つ");
        deliver(a);
        deliver(b);
        check(!strcmp(langs(a), "ja") && a->search_history() && !strcmp(langs(b), "ja"), "知らせの後は二人とも同じ");
        settled("二人の書き手");
    }
    /* 同じことを本当に競らせる：A が claim を持って読んでいる間に B が claim を待ち、A の保存の後に
       B が読み直す。 */
    world_reset();
    put(H "languages=fr\nsearch-history=off\n");
    {
        struct process *a = fresh(), *b = fresh();
        enter();
        race_armed = true;
        race_inside = race_timeout = false;
        most_waiting = 0;
        leave();
        struct writer wa = {a, 1}, wb = {b, 1}; /* result は書き手が上書きする */
        void *ta = CreateThread(NULL, 0, write_languages, &wa, 0, NULL);
        enter();
        for (int i = 0; i < 200 && !race_inside; i++) SleepConditionVariableSRW(&world_wake, &world, 50, 0);
        bool inside = race_inside;
        leave();
        void *tb = CreateThread(NULL, 0, write_history, &wb, 0, NULL);
        WaitForSingleObject(ta, 30000);
        WaitForSingleObject(tb, 30000);
        CloseHandle(ta);
        CloseHandle(tb);
        enter();
        bool contended = inside && !race_timeout && most_waiting >= 1;
        race_armed = false;
        leave();
        check(contended, "B は A が claim を持つ間 claim を待った");
        check(wa.result == 0 && wb.result == 0, "二人とも書けた");
        check(!strcmp(document(), H "languages=de\nsearch-history=on\n"), "競っても両方の変更が残る");
        check(!strcmp(langs(b), "de") && b->search_history(), "後の書き手は前の書き手の変更を読んでから書いた");
        settled("競る二人の書き手");
    }

    /* 世代：一覧が変わった時だけ進む。 */
    world_reset();
    put(H "languages=ja\n");
    {
        struct process *a = watching(), *b = watching(), *n = NULL;
        check(a->generation() == 0 && !strcmp(langs(a), "ja"), "最初の読みでは世代は進まない（読む前に誰も一覧を受け取っていない）");
        const char *ko[] = {"ko"}, *ko_upper[] = {"KO"}, *en_us[] = {"en-US"}, *de[] = {"de"}, *it[] = {"it"};
        check(b->set_search_history(true) == 0 && deliver(a) == 1 && a->generation() == 0, "検索の履歴だけの変化では進まない");
        check(b->set_languages(ko, 1) == 0 && deliver(a) == 1 && a->generation() == 1 && !strcmp(langs(a), "ko"), "一覧が変われば一つ進む");
        unsigned before = saved();
        check(b->set_languages(ko_upper, 1) == 0 && saved() == before && deliver(a) == 0 && a->generation() == 1, "同じ一覧の変更は保存せず、知らせも無く、進まない");
        check(b->reset_languages() == 0 && deliver(a) == 1 && a->generation() == 2 && !strcmp(langs(a), "en-US"), "既定に戻れば進む");
        check(b->set_languages(en_us, 1) == 0 && deliver(a) == 1 && a->generation() == 2, "既定と同じ一覧を書いても一覧は同じなので進まない");
        put(H "languages=en-US\nsearch-history=on\n");
        check(deliver(a) == 1 && a->generation() == 2, "同じ中身の保存でも進まない");
        check(a->set_languages(de, 1) == 0 && a->generation() == 3, "自分の変更はすぐに一つ進む");
        check(deliver(a) == 1 && a->generation() == 3, "自分の保存の知らせでは進まない");
        n = watching();
        check(b->set_languages(it, 1) == 0 && deliver(n) == 1 && n->generation() == 0 && !strcmp(langs(n), "it"),
              "一覧をまだ誰にも渡していないプロセスでは進まない");
        settled("世代");
    }

    /* 待ち行列の他の事象は触らない。webprefs_sync は待たずに今ある変化を取る。 */
    world_reset();
    put(H "languages=ja\n");
    {
        struct process *a = watching(), *b = watching();
        (void)langs(a);
        struct n_event other = {.token = 7, .events = N_EVENT_RECEIVABLE};
        notes_clear();
        a->events(&other, 1);
        a->events(NULL, 0);
        check(!noted("take") && !noted("open-read"), "WEBPREFS_TOKEN でない事象では何もしない");
        const char *ko[] = {"ko"};
        check(b->set_languages(ko, 1) == 0, "B が変える");
        a->sync();
        check(!strcmp(langs(a), "ko") && a->generation() == 1, "主ループを通らずに webprefs_sync で変化を取る");
        struct n_event events[4];
        check(queue_wait(a->queue, events, 4) == 0, "sync が取った変化はもう待ち行列に報告されない");
        notes_clear();
        a->sync();
        check(noted("take") && !noted("open-read"), "変化が無ければ sync は読まない");
        settled("sync");
    }

    /* 見張りの無いプロセス：最初の使用で一度だけ読む。sync は読む。 */
    world_reset();
    put(H "languages=ja\n");
    {
        struct process *p = fresh();
        check(!strcmp(langs(p), "ja"), "見張りが無くても最初の使用で読む");
        put(H "languages=ko\n");
        notes_clear();
        check(!strcmp(langs(p), "ja") && !p->search_history() && !noted("open-read") && !noted("watch"), "その後の使用では読み直さない");
        p->sync();
        check(!strcmp(langs(p), "ko") && p->generation() == 1, "sync は見張りが無ければ読む");
        settled("見張りなし");
    }
    /* C: に見張りの無いシステム：一度読む形に落ち、使用のたびに試し直さない。 */
    world_reset();
    put(H "languages=ja\n");
    {
        enter();
        watch_unsupported = true;
        leave();
        struct process *p = fresh();
        check(p->watch(p->queue) == -N_ERR_UNSUPPORTED && !strcmp(langs(p), "ja"), "見張りが無くても読む");
        put(H "languages=ko\n");
        notes_clear();
        check(!strcmp(langs(p), "ja") && !noted("watch") && !noted("open-read"), "使用のたびに見張りを試し直さない");
        p->sync();
        check(!strcmp(langs(p), "ko"), "sync は読む");
        enter();
        watch_unsupported = false;
        leave();
        settled("見張りの無いシステム");
    }
    /* 文書の名前がフォルダー：読めば既定、書けば失敗。 */
    world_reset();
    {
        enter();
        document_is_folder = true;
        leave();
        struct process *p = watching();
        const char *ja[] = {"ja"};
        check(!strcmp(langs(p), "en-US") && p->set_languages(ja, 1) == -N_ERR_IS_FOLDER, "preferences がフォルダーなら既定で、書けない");
        enter();
        document_is_folder = false;
        leave();
        settled("フォルダーの名前");
    }

    /* プロファイルのフォルダーが除かれる：見張りは CLOSED、既定へ。作り直されたら見張りを戻す。 */
    world_reset();
    put(H "languages=ja\n");
    {
        struct process *a = watching(), *b = fresh();
        check(!strcmp(langs(a), "ja"), "A が読む");
        drop_folder();
        check(deliver(a) == 1 && !strcmp(langs(a), "en-US") && a->generation() == 1, "フォルダーが除かれると見張りが閉じ、全部が既定");
        check(registered(a->queue) == 0, "閉じた見張りは待ち行列から外す");
        const char *fr[] = {"fr"}, *it[] = {"it"};
        notes_clear();
        check(b->set_languages(fr, 1) == 0 && noted("folder-create") && !strcmp(document(), H "languages=fr\n"), "書く時はフォルダーだけを作り直す");
        check(!strcmp(langs(a), "fr") && registered(a->queue) == 1 && a->generation() == 2, "A は次の使用で見張りを戻して読む");
        check(b->set_languages(it, 1) == 0 && deliver(a) == 1 && !strcmp(langs(a), "it"), "戻した見張りに次の変化が届く");
        settled("フォルダーが除かれる");
    }

    /* 割り当てと呼出しの失敗の注入。 */
    long points_total = 0;
    /* 見張りを置く（新しいプロセスごと）：失敗しても次の使用で読み、見張りも戻る。 */
    world_reset();
    put(H "languages=ja\nsearch-history=on\n");
    {
        struct process *p = fresh();
        faults(-2);
        n_result r = p->watch(p->queue);
        long points = faults_end();
        check(r == 0 && !strcmp(langs(p), "ja"), "見張りを置く基準の回");
        for (long i = 0; i < points; i++) {
            p = fresh();
            faults(i);
            r = p->watch(p->queue);
            faults_end();
            settled("見張りを置く失敗");
            bool read = !strcmp(langs(p), "ja") && p->search_history();
            put(H "languages=ko\n");
            deliver(p);
            bool heard = !strcmp(langs(p), "ko") && registered(p->queue) == 1;
            put(H "languages=ja\nsearch-history=on\n");
            checkf(read && heard, "見張りを置く %ld 番目の失敗（%lld）：次の使用で読み、見張りも戻る", i, (long long)r);
        }
        points_total += points;
        printf("見張りを置く：失敗の注入 %ld 点\n", points);
    }
    /* 知らせで読み直す：失敗しても次の使用で読み直す。 */
    world_reset();
    put(H "languages=fr\n");
    {
        struct process *p = watching();
        (void)langs(p);
        put(H "languages=de\n");
        faults(-2);
        deliver(p);
        long points = faults_end();
        check(!strcmp(langs(p), "de"), "読み直しの基準の回");
        put(H "languages=fr\n");
        deliver(p);
        for (long i = 0; i < points; i++) {
            put(H "languages=de\n");
            faults(i);
            deliver(p);
            faults_end();
            settled("読み直しの失敗");
            checkf(!strcmp(langs(p), "de"), "読み直しの %ld 番目の失敗：次の使用で読み直す", i);
            put(H "languages=fr\n");
            deliver(p);
        }
        points_total += points;
        printf("読み直し：失敗の注入 %ld 点\n", points);
    }
    /* 言語を書く・既定に戻す：失敗なら文書も状態も前のまま、成功なら新しい中身。 */
    static const struct { const char *base, *goal, *before, *after; int what; const char *label; } edits[] = {
        {H "languages=fr\nsearch-history=on\nkeep=1\n", H "languages=ja,en\nsearch-history=on\nkeep=1\n", "fr", "ja,en", 0, "言語を書く"},
        {H "languages=fr\nsearch-history=on\nkeep=1\n", H "search-history=on\nkeep=1\n", "fr", "en-US", 1, "既定に戻す"},
    };
    for (size_t e = 0; e < sizeof edits / sizeof *edits; e++) {
        world_reset();
        struct process *p = watching();
        const char *tags[] = {"ja", "en"};
        put(edits[e].base);
        deliver(p);
        faults(-2);
        n_result r = edits[e].what ? p->reset_languages() : p->set_languages(tags, 2);
        long points = faults_end();
        checkf(r == 0 && !strcmp(document(), edits[e].goal), "%s：基準の回", edits[e].label);
        for (long i = 0; i < points; i++) {
            put(edits[e].base);
            deliver(p);
            faults(i);
            r = edits[e].what ? p->reset_languages() : p->set_languages(tags, 2);
            faults_end();
            settled(edits[e].label);
            const char *d = document();
            bool ok = r == 0 ? !strcmp(d, edits[e].goal) && !strcmp(langs(p), edits[e].after)
                             : !strcmp(d, edits[e].base) && !strcmp(langs(p), edits[e].before);
            deliver(p);
            ok = ok && !strcmp(langs(p), r == 0 ? edits[e].after : edits[e].before);
            checkf(r < 0 && ok, "%s の %ld 番目の失敗（%lld）：失敗を返し、文書も状態も前のまま", edits[e].label, i, (long long)r);
        }
        points_total += points;
        printf("%s：失敗の注入 %ld 点\n", edits[e].label, points);
    }
    /* 文書の無い所へ書く（作る道）。 */
    world_reset();
    {
        struct process *p = watching();
        faults(-2);
        n_result r = p->set_search_history(true);
        long points = faults_end();
        check(r == 0 && !strcmp(document(), H "search-history=on\n"), "文書を作って書く基準の回");
        for (long i = 0; i < points; i++) {
            drop_document();
            deliver(p);
            faults(i);
            r = p->set_search_history(true);
            faults_end();
            settled("文書を作る失敗");
            const char *d = document();
            bool ok = r < 0 && (!strcmp(d, "<文書なし>") || !*d) && !p->search_history();
            deliver(p);
            ok = ok && !p->search_history();
            checkf(ok, "文書を作って書く %ld 番目の失敗（%lld）：失敗を返し、文書は無いか空で、off のまま", i, (long long)r);
        }
        points_total += points;
        printf("文書を作る：失敗の注入 %ld 点\n", points);
    }
    /* フォルダーごと無い所へ書く（フォルダーを作り直す道）。 */
    world_reset();
    {
        struct process *p = fresh();
        const char *it[] = {"it"};
        check(p->set_languages(it, 1) == 0, "文書の道を先に知る（失敗の番号を回ごとにそろえる）");
        drop_folder();
        faults(-2);
        n_result r = p->set_languages(it, 1);
        long points = faults_end();
        check(r == 0 && !strcmp(document(), H "languages=it\n"), "フォルダーを作り直して書く基準の回");
        for (long i = 0; i < points; i++) {
            drop_folder();
            faults(i);
            r = p->set_languages(it, 1);
            faults_end();
            settled("フォルダーを作り直す失敗");
            const char *d = document();
            checkf(r < 0 && (!strcmp(d, "<文書なし>") || !*d), "フォルダーを作り直して書く %ld 番目の失敗（%lld）：失敗を返し、文書は無いか空", i, (long long)r);
            check(p->set_languages(it, 1) == 0 && !strcmp(document(), H "languages=it\n") && !strcmp(langs(p), "it"), "失敗の後にもう一度書ける");
        }
        points_total += points;
        printf("フォルダーを作り直す：失敗の注入 %ld 点\n", points);
    }

    /* スレッド：読み手がどのスレッドから呼んでも、壊れた値や戻る世代を見ない。 */
    world_reset();
    put(H "languages=ja,en-US\nsearch-history=on\n");
    {
        struct process *p = watching();
        (void)langs(p);
        struct stress s[4] = {{.p = p, .mode = 0}, {.p = p, .mode = 1}, {.p = p, .mode = 0}, {.p = p, .mode = 2}};
        void *threads[4];
        __atomic_store_n(&stress_stop, 0, __ATOMIC_RELEASE);
        for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, stress_run, &s[i], 0, NULL);
        for (int i = 0; i < 400; i++) {
            put(i % 2 ? H "languages=ja,en-US\nsearch-history=on\n" : H "languages=fr\n");
            deliver(p);
        }
        __atomic_store_n(&stress_stop, 1, __ATOMIC_RELEASE);
        unsigned bad = 0, calls = 0, held = 0;
        for (int i = 0; i < 4; i++) {
            WaitForSingleObject(threads[i], 30000);
            CloseHandle(threads[i]);
            bad += s[i].bad;
            calls += s[i].calls;
            held += s[i].held;
        }
        printf("スレッド：読み手の呼出し %u 回、世代 %llu\n", calls, (unsigned long long)p->generation());
        check(!bad && calls > 0 && !held, "四つのスレッドの読み手は壊れた値も戻る世代も見ず、錠を残さない");
        check(p->generation() > 0, "読み手のいる間に一覧が変わり世代が進んだ");
        settled("スレッド");
    }

    printf("失敗の注入は計 %ld 点、使ったプロセスの写し %zu／%zu\n", points_total, used_processes, (size_t)PROCESS_COUNT);
    printf("webprefs のホストの試験 %u 件、失敗 %u 件\n", checks, failed);
    return failed ? 1 : 0;
}
