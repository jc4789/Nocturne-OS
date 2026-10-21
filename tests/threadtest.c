/* Native shared-space threads; not Web Workers and not process-copy emulation. */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <parallel.h>

#define THREADS 6
static volatile uint32_t gate, entered, failures, cpu_mask;
static int tids[THREADS], seen_tid[THREADS];
static unsigned results[THREADS];
/* Path of the document every worker appends to; NULL in the helper modes that only exit. */
static const char *shared_path;
#define RESOURCE_ROUNDS 40
#define RECORD_SIZE 16
static const char resource_path[] = "/data/threadtest-resources.tmp";
static n_handle resource_window;
static uint64_t resource_clock_page;
static void *resource_maps[THREADS][RESOURCE_ROUNDS];
static volatile uint32_t resource_gate, blocked_read_entered;
/* The handle the workers probe while worker 0 closes and replaces it every round. Probes are
   non-destructive (kind, size, positional read, peek, watch), so a number a sibling's own
   handle later reuses is never disturbed. */
static volatile n_handle racing, racing_window;
static n_handle blocked_read_handle;
static char blocked_token;
static n_result blocked_result;
enum resource_check {
    RC_HANDLE_CYCLE, RC_FOLDER_SET, RC_HANDLE_KIND, RC_DOCUMENT_SIZE,
    RC_DOCUMENT_READ, RC_FOLDER_LIST, RC_CHANNEL_PEEK, RC_EVENTS, RC_WINDOW_STATE, RC_PATH,
    RC_OPEN_CLOSE, RC_CHANNEL_CREATE, RC_CHANNEL_SEND, RC_CHANNEL_RECEIVE, RC_RECORD_APPEND,
    RC_CLOCK, RC_RESERVE, RC_PROTECT_READ, RC_PROTECT_WRITE, RC_THREAD_CREATE, RC_JOIN,
    RC_FINAL_SIZE, RC_RECORDS_INTACT, RC_MAP_CONTENT, RC_MAP_ALIAS, RC_RELEASE, RC_FOLDER_RESTORE,
    RC_BLOCK_CREATE, RC_BLOCK_REPLACE, RC_BLOCK_RESULT, RC_COUNT
};
static const char *resource_names[RC_COUNT] = {
    "handle-cycle", "folder-set", "handle-kind", "document-size",
    "document-read", "folder-list", "channel-peek", "events", "window-state", "path",
    "open-close", "channel-create", "channel-send", "channel-receive", "record-append",
    "clock", "reserve", "protect-read", "protect-write", "thread-create", "join",
    "final-size", "records-intact", "map-content", "map-alias", "release", "folder-restore",
    "blocked-receive-create", "blocked-receive-replace", "blocked-receive-result"
};
struct resource_diagnostic { unsigned count; long first_value; int first_errno; };
/* Each worker owns its row; only the supervisor prints after all joins. This
   avoids adding a shared stdio race merely to diagnose a kernel resource test. */
static struct resource_diagnostic resource_diagnostics[THREADS + 1][RC_COUNT];

static unsigned add(volatile uint32_t *p, unsigned value) {
    __asm__ volatile("lock xaddl %0, %1" : "+r"(value), "+m"(*p) : : "memory", "cc");
    return value;
}
/* Aligned x86 loads/stores plus a compiler barrier implement acquire/release;
   unlike GCC's __atomic builtins these are supported by the guest's TCC. */
static unsigned load_acquire(const volatile uint32_t *p) {
    unsigned value;
    __asm__ volatile("movl %1, %0" : "=r"(value) : "m"(*p) : "memory");
    return value;
}
static void store_release(volatile uint32_t *p, unsigned value) {
    __asm__ volatile("movl %1, %0" : "=m"(*p) : "r"(value) : "memory");
}
static void mask_cpu(unsigned index) {
    unsigned bit = 1u << index;
    __asm__ volatile("lock orl %1, %0" : "+m"(cpu_mask) : "r"(bit) : "memory", "cc");
}
static void failed(void) { add(&failures, 1); }
static void resource_failed(unsigned id, enum resource_check check, long value) {
    struct resource_diagnostic *d = &resource_diagnostics[id][check];
    if (!d->count) { d->first_value = value; d->first_errno = errno; }
    d->count++;
    failed();
}
/* A handle that a sibling closed meanwhile is gone or, if its number was reused, of another kind. */
static bool gone_or_other(n_result r) { return r == -N_ERR_NO_HANDLE || r == -N_ERR_WRONG_KIND; }
static void fatal_worker(size_t index, void *context) {
    (void)index; (void)context;
    /* Represent a lock and a completion token abandoned by a dying helper.
       The owner is blocked in the real parallel pool's done wait. */
    store_release(&gate, 1);
    __asm__ volatile("ud2" : : : "memory");
}
static void exit_racer(void *arg) {
    add(&entered, 1);
    wake_address(&entered, 0);
    while (!load_acquire(&gate))
        wait_on_address(&gate, 0, UINT32_MAX);
    /* Mix already-finished, returning and still-active siblings while the
       owner enrolls in its exit wait queue. No userspace join hides the race. */
    unsigned delay = (unsigned)(uintptr_t)arg * 30000u;
    for (unsigned i = 0; i < delay; i++) __asm__ volatile("pause");
}
static void worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    seen_tid[id] = thread_id();
    uintptr_t stack_low,stack_high,stack_marker=(uintptr_t)&stack_low;
    if(thread_stack_bounds(&stack_low,&stack_high) || stack_low>=stack_high ||
       stack_marker<stack_low || stack_marker>=stack_high)failed();
    thread_local_set(7, (void *)(uintptr_t)(id + 1));
    add(&entered, 1);
    wake_address(&entered, 0);
    while (!gate) wait_on_address(&gate, 0, UINT32_MAX);
    errno = 1000 + (int)id;
    uint64_t until = uptime_ms() + 80;
    unsigned sum = id + 1;
    while (uptime_ms() < until) {
        mask_cpu(cpu_index());
        for (unsigned k = 0; k < 3000; k++) sum = sum * 1664525u + 1013904223u;
    }
    if (thread_local_get(7) != (void *)(uintptr_t)(id + 1)) failed();
    if (errno != 1000 + (int)id) failed();
    for (unsigned k = 1; k <= 96; k++) {
        size_t n = 64 + k * 37;
        unsigned char *p = malloc(n);
        if (!p) { failed(); break; }
        memset(p, id + 17, n);
        unsigned char *q = realloc(p, n + 8192);
        if (!q) { free(p); failed(); break; }
        for (size_t j = 0; j < n; j++) if (q[j] != id + 17) { failed(); break; }
        free(q);
    }
    /* Concurrent appends by path are whole: each adds exactly one byte. */
    if (shared_path && n_document_append(shared_path, "t", 1) < 0) failed();
    results[id] = sum;
}

static void resource_worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    while (!load_acquire(&resource_gate))
        wait_on_address(&resource_gate, 0, UINT32_MAX);
    for (unsigned round = 0; round < RESOURCE_ROUNDS; round++) {
        if (!id) {
            /* Replace the probed handle: publish the new one, then close the old one. */
            n_handle fresh = -1;
            if (round & 1) {
                n_handle pair[2];
                if (n_channel_pair(pair) == 0) { n_handle_close(pair[1]); fresh = pair[0]; }
            } else fresh = n_document_open(resource_path, N_DOCUMENT_READ);
            if (fresh < 0) resource_failed(id, RC_HANDLE_CYCLE, fresh);
            else {
                n_handle old = racing;
                racing = fresh;
                if (old >= 0) n_handle_close(old);
            }
            if (resource_window >= 0) racing_window = round & 1 ? resource_window : racing;
            if (n_folder_set(round & 1 ? "/data" : "/") < 0) resource_failed(id, RC_FOLDER_SET, -1);
        }
        /* Retain/replace, independent slot allocation and nonblocking lookups must not
           dereference a sibling's freed object. */
        n_handle h = racing;
        n_result r = n_handle_kind(h);
        if (r < 0 && r != -N_ERR_NO_HANDLE) resource_failed(id, RC_HANDLE_KIND, r);
        r = n_document_size(h);
        if (r < 0 && !gone_or_other(r)) resource_failed(id, RC_DOCUMENT_SIZE, r);
        char byte = (char)id;
        r = n_document_read_at(h, 0, &byte, 1);
        if (r < 0 && !gone_or_other(r)) resource_failed(id, RC_DOCUMENT_READ, r);
        struct n_entry list[4];
        r = n_folder_list("/data", list, 4, 0);
        if (r < 0) resource_failed(id, RC_FOLDER_LIST, r);
        r = n_channel_receive(h, NULL, 0, N_CHANNEL_PEEK, NULL);
        if (r < 0 && !gone_or_other(r) && r != -N_ERR_NOT_READY && r != -N_ERR_CLOSED)
            resource_failed(id, RC_CHANNEL_PEEK, r);
        n_handle events = n_events_create();
        if (events < 0) resource_failed(id, RC_EVENTS, events);
        else {
            /* A watch on a stale or wrong-kind handle is refused; it never holds the object. */
            r = n_events_watch(events, h, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, id);
            struct n_event ready[2];
            if (r < 0 && !gone_or_other(r) && r != -N_ERR_INVALID && r != -N_ERR_UNSUPPORTED)
                resource_failed(id, RC_EVENTS, r);
            else if (n_events_wait(events, ready, 2, 0) < 0) resource_failed(id, RC_EVENTS, -1);
            n_handle_close(events);
        }
        window_t probe = {.handle = racing_window};
        /* GUI state reports a raw negative result, not -1 plus TLS errno. */
        int state = resource_window >= 0 ? win_state(&probe) : 0;
        if (state < 0 && !gone_or_other(state)) resource_failed(id, RC_WINDOW_STATE, state);
        char complete[64];
        if (n_path_complete("probe", complete, sizeof complete) < 0 ||
            (strcmp(complete, "/probe") && strcmp(complete, "/data/probe")))
            resource_failed(id, RC_PATH, -1);
        n_handle first = n_document_open(resource_path, N_DOCUMENT_READ);
        n_handle second = first >= 0 ? n_document_open(resource_path, N_DOCUMENT_READ) : -1;
        if (first < 0 || second < 0 || n_document_size(second) < 0)
            resource_failed(id, RC_OPEN_CLOSE, first < 0 ? first : second);
        if (second >= 0) n_handle_close(second);
        if (first >= 0) n_handle_close(first);
        n_handle ends[2];
        if (n_channel_pair(ends) < 0) resource_failed(id, RC_CHANNEL_CREATE, -1);
        else {
            byte = (char)(id + 1);
            if (n_channel_send(ends[1], &byte, 1, -1) != 1) resource_failed(id, RC_CHANNEL_SEND, -1);
            byte = 0;
            if (n_channel_receive(ends[0], &byte, 1, N_CHANNEL_WAIT, NULL) != 1 || byte != (char)(id + 1))
                resource_failed(id, RC_CHANNEL_RECEIVE, byte);
            n_handle_close(ends[0]); n_handle_close(ends[1]);
        }
        char record[RECORD_SIZE]; memset(record, (int)id + 1, sizeof record);
        if (n_document_append(resource_path, record, sizeof record) < 0) resource_failed(id, RC_RECORD_APPEND, -1);
        struct n_clockinfo clock = {0};
        if (clock_info(&clock) || clock.reference_page != resource_clock_page)
            resource_failed(id, RC_CLOCK, (long)clock.reference_page);
        void *page;
        n_memory_reserve(4096, N_MEMORY_READ_WRITE, &page);
        unsigned char *map = page;
        resource_maps[id][round] = map;
        if (!map) { resource_failed(id, RC_RESERVE, 0); continue; }
        memset(map, (int)id + 19, 4096);
        if (n_memory_protect(map, 4096, N_MEMORY_READ) || map[4095] != (unsigned char)(id + 19))
            resource_failed(id, RC_PROTECT_READ, map[4095]);
        if (n_memory_protect(map, 4096, N_MEMORY_READ_WRITE)) resource_failed(id, RC_PROTECT_WRITE, -1);
    }
}

static void blocked_reader(void *unused) {
    (void)unused;
    store_release(&blocked_read_entered, 1);
    wake_address(&blocked_read_entered, 0);
    blocked_result = n_channel_receive(blocked_read_handle, &blocked_token, 1, N_CHANNEL_WAIT, NULL);
}

static int resource_regression(const struct n_clockinfo *clock) {
    unsigned before = failures;
    const char *folder = getenv("FOLDER");
    char saved_folder[256];
    snprintf(saved_folder, sizeof saved_folder, "%s", folder && folder[0] == '/' ? folder : "/");
    n_entry_remove(resource_path);
    if (n_document_save(resource_path, "", 0) < 0) return 1;
    racing = n_document_open(resource_path, N_DOCUMENT_READ);
    resource_window = (n_handle)__syscall(SYS_WIN_CREATE, 32, 32, (long)"threadtest resources", 0, 0);
    racing_window = resource_window;
    resource_clock_page = clock->reference_page;
    if (racing < 0) return 1;
    for (unsigned i = 0; i < THREADS; i++) {
        tids[i] = thread_create(resource_worker, (void *)(uintptr_t)i);
        if (tids[i] < 0) resource_failed(THREADS, RC_THREAD_CREATE, tids[i]);
    }
    store_release(&resource_gate, 1);
    wake_address(&resource_gate, 0);
    for (unsigned i = 0; i < THREADS; i++)
        if (tids[i] >= 0 && thread_join(tids[i])) resource_failed(THREADS, RC_JOIN, tids[i]);
    /* Every append landed whole: the right total, and each record is one worker's bytes. */
    struct n_entry entry;
    n_result final_size = n_entry_info(resource_path, &entry) < 0 ? -1 : (n_result)entry.size;
    if (final_size != THREADS * RESOURCE_ROUNDS * RECORD_SIZE)
        resource_failed(THREADS, RC_FINAL_SIZE, (long)final_size);
    void *records = NULL;
    size_t record_bytes = 0;
    if (n_document_load(resource_path, &records, &record_bytes) < 0) resource_failed(THREADS, RC_RECORDS_INTACT, -1);
    else {
        unsigned per_worker[THREADS + 1] = {0};
        const unsigned char *bytes = records;
        for (size_t at = 0; at + RECORD_SIZE <= record_bytes; at += RECORD_SIZE) {
            unsigned owner = bytes[at];
            bool whole = owner >= 1 && owner <= THREADS;
            for (size_t k = 1; k < RECORD_SIZE && whole; k++) whole = bytes[at + k] == owner;
            if (!whole) resource_failed(THREADS, RC_RECORDS_INTACT, (long)at);
            else per_worker[owner]++;
        }
        for (unsigned i = 1; i <= THREADS; i++)
            if (per_worker[i] != RESOURCE_ROUNDS) resource_failed(THREADS, RC_RECORDS_INTACT, (long)per_worker[i]);
        if (records) n_memory_release(records, record_bytes);
    }
    for (unsigned i = 0; i < THREADS * RESOURCE_ROUNDS; i++) {
        unsigned char *map = resource_maps[i / RESOURCE_ROUNDS][i % RESOURCE_ROUNDS];
        if (!map) continue;
        if (map[4095] != i / RESOURCE_ROUNDS + 19) resource_failed(THREADS, RC_MAP_CONTENT, map[4095]);
        for (unsigned j = 0; j < i; j++)
            if (map == resource_maps[j / RESOURCE_ROUNDS][j % RESOURCE_ROUNDS])
                resource_failed(THREADS, RC_MAP_ALIAS, (long)map);
    }
    for (unsigned i = 0; i < THREADS; i++)
        for (unsigned j = 0; j < RESOURCE_ROUNDS; j++)
            if (resource_maps[i][j] && n_memory_release(resource_maps[i][j], 4096))
                resource_failed(THREADS, RC_RELEASE, (long)resource_maps[i][j]);
    if (racing >= 0) n_handle_close(racing);
    if (resource_window >= 0) n_handle_close(resource_window);
    n_entry_remove(resource_path);
    if (n_folder_set(saved_folder) < 0) resource_failed(THREADS, RC_FOLDER_RESTORE, -1);
    /* Closing a handle must not free an in-flight blocking receive. The receiver's retained
       channel still delivers the original sender's message after the handle is gone and
       its number is taken by another object. */
    n_handle ends[2];
    if (n_channel_pair(ends) < 0) resource_failed(THREADS, RC_BLOCK_CREATE, -1);
    else {
        blocked_read_handle = ends[0];
        int reader = thread_create(blocked_reader, NULL);
        if (reader < 0) { resource_failed(THREADS, RC_BLOCK_CREATE, reader); n_handle_close(ends[0]); }
        else {
            while (!load_acquire(&blocked_read_entered))
                wait_on_address(&blocked_read_entered, 0, 2000);
            n_sleep_ms(20);
            n_handle reused = -1;
            if (n_handle_close(ends[0]) < 0 || (reused = n_events_create()) < 0)
                resource_failed(THREADS, RC_BLOCK_REPLACE, -1);
            if (n_channel_send(ends[1], "p", 1, -1) != 1 || thread_join(reader) || blocked_result != 1 || blocked_token != 'p')
                resource_failed(THREADS, RC_BLOCK_RESULT, (long)blocked_result);
            if (reused >= 0) n_handle_close(reused);
        }
        n_handle_close(ends[1]);
    }
    for (unsigned id = 0; id <= THREADS; id++)
        for (unsigned check = 0; check < RC_COUNT; check++) {
            struct resource_diagnostic *d = &resource_diagnostics[id][check];
            if (d->count)
                printf("threadtest: resource-detail worker=%u check=%s count=%u first=%ld errno=%d\n",
                       id, resource_names[check], d->count, d->first_value, d->first_errno);
        }
    printf("threadtest: resources handle-retain/replace/append/folder/VM/clock/WM %s errors=%u\n",
           failures == before ? "PASS" : "FAIL", failures - before);
    return 0; /* detailed failures are already counted in the shared total */
}

/* Launches this program in a helper mode; the handles it gets are 0 input, 1 output, 2 log (or -1). */
static n_handle start_helper(const char *self, const char *mode, const int32_t *handles) {
    const char *args[] = {self, mode};
    struct n_launch launch = {.arguments = args, .argument_count = 2, .handles = handles, .handle_count = handles ? 3 : 0};
    return n_app_launch(self, &launch);
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--fault-worker")) {
        parallel_call(fatal_worker, NULL, NULL, NULL);
        return 0; /* unreachable: fatal helper must abort this whole process */
    }
    if (argc == 2 && !strcmp(argv[1], "--exit-active")) {
        for (unsigned i = 0; i < THREADS; i++)
            if (thread_create(exit_racer, (void *)(uintptr_t)i) < 0) return 1;
        while (load_acquire(&entered) != THREADS) {
            unsigned before = load_acquire(&entered);
            if (before != THREADS) wait_on_address(&entered, before, 2000);
        }
        store_release(&gate, 1);
        wake_address(&gate, 0);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--exit")) {
        for (unsigned i = 0; i < THREADS; i++)
            if (thread_create(worker, (void *)(uintptr_t)i) < 0) return 1;
        while (entered != THREADS) {
            unsigned before = entered;
            if (entered != THREADS) wait_on_address(&entered, before, 2000);
        }
        /* All siblings are asleep in address wait when root destroys process. */
        return 0;
    }
    struct n_cpuinfo info;
    struct n_clockinfo clock;
    int bad = 0;
    uintptr_t stack_low,stack_high,stack_marker=(uintptr_t)&stack_low;
    if(thread_stack_bounds(&stack_low,&stack_high) || stack_low>=stack_high ||
       stack_marker<stack_low || stack_marker>=stack_high)bad++;
    uintptr_t unchanged=stack_marker;
    errno=0;
    if(thread_stack_bounds(&unchanged,NULL)!=-N_ERR_BAD_ADDRESS || errno!=0 || unchanged!=stack_marker)bad++;
    if (cpu_info(&info) || clock_info(&clock)) return 1;
    printf("threadtest: scheduler=%u online=%u clock_flags=%u\n", info.scheduler_cpus, info.online_cpus, clock.flags);
    static const char shared_document[] = "/data/threadtest-shared.tmp";
    n_entry_remove(shared_document);
    if (n_document_save(shared_document, "", 0) < 0) return 1;
    shared_path = shared_document;
    errno = 701;
    thread_local_set(7, (void *)(uintptr_t)9001);
    for (unsigned i = 0; i < THREADS; i++) {
        n_result tid = thread_create(worker, (void *)(uintptr_t)i);
        if (tid < 0) { printf("threadtest: create failed: %s\n", n_result_text(tid)); return 1; }
        tids[i] = (int)tid;
    }
    while (entered != THREADS) {
        unsigned before = entered;
        if (entered != THREADS) wait_on_address(&entered, before, 2000);
    }
    gate = 1;
    __asm__ volatile("mfence" : : : "memory");
    wake_address(&gate, 0);
    errno = 701;
    for (unsigned i = 0; i < THREADS; i++) {
        if (thread_join(tids[i])) bad++;
        if (seen_tid[i] != tids[i] || !results[i]) bad++;
    }
    if (thread_local_get(7) != (void *)(uintptr_t)9001 || errno != 701) bad++;
    struct n_entry shared_entry;
    if (n_entry_info(shared_document, &shared_entry) < 0 || shared_entry.size != THREADS) bad++;
    n_entry_remove(shared_document);
    shared_path = NULL;
    unsigned active = 0;
    for (unsigned i = 0; i < 16; i++) if (cpu_mask & (1u << i)) active++;
    if (info.scheduler_cpus > 1 && active < 2) bad++;
    volatile uint32_t timeout_word = 0;
    if (wait_on_address(&timeout_word, 0, 2) != -N_ERR_TIMEOUT) bad++;
    timeout_word = 1;
    if (wait_on_address(&timeout_word, 0, 1000) != -N_ERR_NOT_READY) bad++;
    uint64_t previous = uptime_ms();
    for (unsigned i = 0; i < 20000; i++) {
        uint64_t now = uptime_ms();
        if (now < previous) bad++;
        previous = now;
    }
    bad += resource_regression(&clock);
    int status = -1;
    n_handle child = start_helper(argv[0], "--exit", NULL);
    if (child < 0 || n_app_result(child, -1, &status) < 0 || status) bad++;
    if (child >= 0) n_handle_close(child);
    for (unsigned round = 0; round < 16; round++) {
        child = start_helper(argv[0], "--exit-active", NULL); status = -1;
        if (child < 0) { bad++; break; }
        n_result result = n_app_result(child, 5000, &status);
        if (result < 0 || status) {
            n_app_stop(child, N_APP_STOP_TREE);
            n_handle_close(child);
            bad++;
            break;
        }
        n_handle_close(child);
    }
    /* A helper dies of a fatal fault while its owner waits in the parallel pool. Its log is a
       channel whose queue already holds a seeded message that nobody reads until the helper is
       reclaimed; queues grow, so no fixed byte count can make the fatal path wait on it. */
    n_handle diag[2] = {-1, -1};
    if (n_channel_pair(diag) < 0) {
        printf("threadtest: fatal-setup channel failed\n");
        bad++;
    }
    else {
        char fill[1024]; memset(fill, 'd', sizeof fill);
        n_result seeded = n_channel_send(diag[1], fill, sizeof fill, -1);
        if (seeded != (n_result)sizeof fill) {
            printf("threadtest: fatal-setup seed=%ld\n", (long)seeded);
            bad++;
        }
        const int32_t log_only[3] = {-1, -1, diag[1]};
        child = start_helper(argv[0], "--fault-worker", log_only); status = -1;
        n_handle_close(diag[1]);
        if (child < 0) {
            printf("threadtest: fatal-spawn child=%d\n", child);
            bad++;
        }
        else {
            uint64_t started = uptime_ms();
            n_result result = n_app_result(child, 5000, &status);
            if (result < 0 || status != 134) {
                uint64_t elapsed = uptime_ms() - started;
                n_result stopped = 0;
                if (result < 0) stopped = n_app_stop(child, N_APP_STOP_TREE);
                printf("threadtest: fatal-wait child=%d result=%ld status=%d elapsed=%lu stop=%ld\n",
                       child, (long)result, status, (unsigned long)elapsed, (long)stopped);
                bad++;
            } else printf("threadtest: expected helper fault status=134\n");
            n_handle_close(child);
        }
        char recorded[2048];
        n_result nr = n_channel_receive(diag[0], recorded, sizeof recorded, 0, NULL);
        if (nr != 1024) {
            printf("threadtest: fatal-log expected seeded bytes=1024 got=%ld\n", (long)nr);
            bad++;
        } else
            for (unsigned i = 0; i < 1024; i++) if (recorded[i] != 'd') { bad++; break; }
        n_handle_close(diag[0]);
    }
    if (clock.flags & N_CLOCK_HV_REFERENCE) {
        if (!clock.reference_page ||
            n_memory_protect((void *)(uintptr_t)clock.reference_page, 4096, N_MEMORY_READ_WRITE) != -N_ERR_DENIED) bad++;
    }
    printf("threadtest: CPU mask=%x active=%u shared malloc/TLS/handle/wait %s errors=%u\n",
           cpu_mask, active, !bad && !failures ? "PASS" : "FAIL", failures + bad);
    return bad || failures ? 1 : 0;
}
