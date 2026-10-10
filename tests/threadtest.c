/* Native shared-space threads; not Web Workers and not process-copy emulation. */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <parallel.h>

#define THREADS 6
static volatile uint32_t gate, entered, failures, cpu_mask;
static int tids[THREADS], seen_tid[THREADS];
static unsigned results[THREADS];
static int shared_fd;
#define RESOURCE_ROUNDS 40
#define RACE_FD 31
#define RACE_WINDOW_FD 30
static int resource_null, resource_dir, resource_window;
static uint64_t resource_clock_page;
static void *resource_maps[THREADS][RESOURCE_ROUNDS];
static volatile uint32_t resource_gate, blocked_read_entered;
static int blocked_read_fd;
static char blocked_token;
static ssize_t blocked_result;
enum resource_check {
    RC_REPLACE, RC_WINDOW_REPLACE, RC_CHDIR, RC_FSTAT, RC_READ, RC_READDIR,
    RC_SEEK, RC_FCNTL, RC_POLL, RC_WINDOW_STATE, RC_CWD, RC_OPEN_DUP,
    RC_PIPE_CREATE, RC_PIPE_WRITE, RC_PIPE_READ, RC_RECORD_WRITE, RC_CLOCK,
    RC_MMAP, RC_PROTECT_READ, RC_PROTECT_WRITE, RC_THREAD_CREATE, RC_JOIN,
    RC_FINAL_OFFSET, RC_MAP_CONTENT, RC_MAP_ALIAS, RC_UNMAP, RC_CWD_RESTORE,
    RC_BLOCK_CREATE, RC_BLOCK_REPLACE, RC_BLOCK_RESULT, RC_COUNT
};
static const char *resource_names[RC_COUNT] = {
    "dup2", "window-dup2", "chdir", "fstat", "read", "readdir",
    "lseek", "fcntl", "poll", "window-state", "cwd", "open-dup",
    "pipe-create", "pipe-write", "pipe-read", "record-write", "clock",
    "mmap", "mprotect-read", "mprotect-write", "thread-create", "join",
    "final-offset", "map-content", "map-alias", "munmap", "cwd-restore",
    "blocked-read-create", "blocked-read-replace", "blocked-read-result"
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
    if (write(shared_fd, "t", 1) != 1) failed();
    results[id] = sum;
}

static void resource_worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    while (!load_acquire(&resource_gate))
        wait_on_address(&resource_gate, 0, UINT32_MAX);
    for (unsigned round = 0; round < RESOURCE_ROUNDS; round++) {
        if (!id) {
            int replaced = dup2(round & 1 ? resource_dir : resource_null, RACE_FD);
            if (replaced != RACE_FD) resource_failed(id, RC_REPLACE, replaced);
            if (resource_window >= 0 &&
                dup2(round & 1 ? resource_window : resource_null, RACE_WINDOW_FD) != RACE_WINDOW_FD)
                resource_failed(id, RC_WINDOW_REPLACE, -1);
            if (chdir(round & 1 ? "/data" : "/")) resource_failed(id, RC_CHDIR, -1);
        }
        /* Atomic retain/replace, independent slot allocation and nonblocking
           metadata lookups must not dereference a sibling's freed file. */
        struct n_stat st;
        struct n_dirent entry;
        char byte = (char)id, cwd[64];
        if (fstat(RACE_FD, &st)) resource_failed(id, RC_FSTAT, -1);
        ssize_t nr = read(RACE_FD, &byte, 1);
        if (nr != 0 && !(nr == -1 && errno == EISDIR)) resource_failed(id, RC_READ, nr);
        int rd = readdir(RACE_FD, 0, &entry);
        if (rd < 0 && errno != ENOTDIR) resource_failed(id, RC_READDIR, rd);
        long offset = lseek(RACE_FD, 0, SEEK_CUR);
        if (offset != -1 || errno != ESPIPE) resource_failed(id, RC_SEEK, offset);
        if (fcntl(RACE_FD, F_GETFL) < 0) resource_failed(id, RC_FCNTL, -1);
        struct n_pollfd pfd = {RACE_FD, N_POLLIN | N_POLLOUT, 0};
        if (poll(&pfd, 1, 0) < 0) resource_failed(id, RC_POLL, -1);
        window_t probe = {.fd = RACE_WINDOW_FD};
        /* GUI state reports a raw negative errno, not -1 plus TLS errno. */
        int state = resource_window >= 0 ? win_state(&probe) : 0;
        if (state < 0 && state != -EBADF) resource_failed(id, RC_WINDOW_STATE, state);
        if (!getcwd(cwd, sizeof cwd) || (strcmp(cwd, "/") && strcmp(cwd, "/data")))
            resource_failed(id, RC_CWD, -1);
        int fd = open("/dev/null", O_RDWR), other = fd >= 0 ? dup(fd) : -1;
        if (fd < 0 || other < 0 || (other >= 0 && fstat(other, &st)))
            resource_failed(id, RC_OPEN_DUP, fd < 0 ? fd : other);
        if (other >= 0) close(other);
        if (fd >= 0) close(fd);
        int ends[2];
        if (pipe(ends)) resource_failed(id, RC_PIPE_CREATE, -1);
        else {
            byte = (char)(id + 1);
            if (write(ends[1], &byte, 1) != 1) resource_failed(id, RC_PIPE_WRITE, -1);
            byte = 0;
            if (read(ends[0], &byte, 1) != 1 || byte != (char)(id + 1))
                resource_failed(id, RC_PIPE_READ, byte);
            close(ends[0]); close(ends[1]);
        }
        char record[16]; memset(record, (int)id + 1, sizeof record);
        ssize_t written = write(shared_fd, record, sizeof record);
        if (written != sizeof record) resource_failed(id, RC_RECORD_WRITE, written);
        struct n_clockinfo clock = {0};
        if (clock_info(&clock) || clock.reference_page != resource_clock_page)
            resource_failed(id, RC_CLOCK, (long)clock.reference_page);
        unsigned char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        resource_maps[id][round] = map;
        if (map == MAP_FAILED) { resource_failed(id, RC_MMAP, (long)map); continue; }
        memset(map, (int)id + 19, 4096);
        if (mprotect(map, 4096, PROT_READ) || map[4095] != (unsigned char)(id + 19))
            resource_failed(id, RC_PROTECT_READ, map[4095]);
        if (mprotect(map, 4096, PROT_READ | PROT_WRITE)) resource_failed(id, RC_PROTECT_WRITE, -1);
    }
}

static void blocked_reader(void *unused) {
    (void)unused;
    store_release(&blocked_read_entered, 1);
    wake_address(&blocked_read_entered, 0);
    blocked_result = read(blocked_read_fd, &blocked_token, 1);
}

static int resource_regression(const struct n_clockinfo *clock) {
    unsigned before = failures;
    char saved_cwd[256];
    if (!getcwd(saved_cwd, sizeof saved_cwd) || chdir("/")) return 1;
    shared_fd = open("/data/threadtest-resources.tmp", O_RDWR | O_CREAT | O_TRUNC);
    resource_null = open("/dev/null", O_RDWR);
    resource_dir = open("/data", O_RDONLY | O_DIRECTORY);
    resource_window = (int)__syscall(SYS_WIN_CREATE, 32, 32, (long)"threadtest resources", 0, 0);
    resource_clock_page = clock->reference_page;
    if (shared_fd < 0 || resource_null < 0 || resource_dir < 0 ||
        dup2(resource_null, RACE_FD) != RACE_FD) return 1;
    if (resource_window >= 0 && dup2(resource_window, RACE_WINDOW_FD) != RACE_WINDOW_FD)
        resource_failed(THREADS, RC_WINDOW_REPLACE, -1);
    for (unsigned i = 0; i < THREADS; i++) {
        tids[i] = thread_create(resource_worker, (void *)(uintptr_t)i);
        if (tids[i] < 0) resource_failed(THREADS, RC_THREAD_CREATE, tids[i]);
    }
    store_release(&resource_gate, 1);
    wake_address(&resource_gate, 0);
    for (unsigned i = 0; i < THREADS; i++)
        if (tids[i] >= 0 && thread_join(tids[i])) resource_failed(THREADS, RC_JOIN, tids[i]);
    long final_offset = lseek(shared_fd, 0, SEEK_END);
    if (final_offset != THREADS * RESOURCE_ROUNDS * 16)
        resource_failed(THREADS, RC_FINAL_OFFSET, final_offset);
    for (unsigned i = 0; i < THREADS * RESOURCE_ROUNDS; i++) {
        unsigned char *map = resource_maps[i / RESOURCE_ROUNDS][i % RESOURCE_ROUNDS];
        if (!map || map == MAP_FAILED) continue;
        if (map[4095] != i / RESOURCE_ROUNDS + 19) resource_failed(THREADS, RC_MAP_CONTENT, map[4095]);
        for (unsigned j = 0; j < i; j++)
            if (map == resource_maps[j / RESOURCE_ROUNDS][j % RESOURCE_ROUNDS])
                resource_failed(THREADS, RC_MAP_ALIAS, (long)map);
    }
    for (unsigned i = 0; i < THREADS; i++)
        for (unsigned j = 0; j < RESOURCE_ROUNDS; j++)
            if (resource_maps[i][j] && resource_maps[i][j] != MAP_FAILED && munmap(resource_maps[i][j], 4096))
                resource_failed(THREADS, RC_UNMAP, (long)resource_maps[i][j]);
    close(RACE_FD);
    if (resource_window >= 0) { close(RACE_WINDOW_FD); close(resource_window); }
    close(resource_dir); close(shared_fd);
    unlink("/data/threadtest-resources.tmp");
    if (chdir(saved_cwd)) resource_failed(THREADS, RC_CWD_RESTORE, -1);
    /* Closing/reusing a descriptor must not free an in-flight blocking read.
       The reader's retained pipe still receives the original writer's token. */
    int ends[2];
    if (pipe(ends)) resource_failed(THREADS, RC_BLOCK_CREATE, -1);
    else {
        blocked_read_fd = ends[0];
        int reader = thread_create(blocked_reader, NULL);
        if (reader < 0) { resource_failed(THREADS, RC_BLOCK_CREATE, reader); close(ends[0]); }
        else {
            while (!load_acquire(&blocked_read_entered))
                wait_on_address(&blocked_read_entered, 0, 2000);
            msleep(20);
            if (close(ends[0]) || dup2(resource_null, ends[0]) != ends[0])
                resource_failed(THREADS, RC_BLOCK_REPLACE, -1);
            if (write(ends[1], "p", 1) != 1 || thread_join(reader) || blocked_result != 1 || blocked_token != 'p')
                resource_failed(THREADS, RC_BLOCK_RESULT, blocked_result);
            close(ends[0]);
        }
        close(ends[1]);
    }
    close(resource_null);
    for (unsigned id = 0; id <= THREADS; id++)
        for (unsigned check = 0; check < RC_COUNT; check++) {
            struct resource_diagnostic *d = &resource_diagnostics[id][check];
            if (d->count)
                printf("threadtest: resource-detail worker=%u check=%s count=%u first=%ld errno=%d\n",
                       id, resource_names[check], d->count, d->first_value, d->first_errno);
        }
    printf("threadtest: resources FD-retain/replace/offset/cwd/VM/clock/WM %s errors=%u\n",
           failures == before ? "PASS" : "FAIL", failures - before);
    return 0; /* detailed failures are already counted in the shared total */
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
    if (cpu_info(&info) || clock_info(&clock)) return 1;
    printf("threadtest: scheduler=%u online=%u clock_flags=%u\n", info.scheduler_cpus, info.online_cpus, clock.flags);
    shared_fd = open("/data/threadtest-shared.tmp", O_RDWR | O_CREAT | O_TRUNC);
    if (shared_fd < 0) return 1;
    errno = 701;
    thread_local_set(7, (void *)(uintptr_t)9001);
    for (unsigned i = 0; i < THREADS; i++) {
        tids[i] = thread_create(worker, (void *)(uintptr_t)i);
        if (tids[i] < 0) { printf("threadtest: create failed errno=%d\n", errno); return 1; }
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
    if (lseek(shared_fd, 0, SEEK_END) != THREADS) bad++;
    close(shared_fd);
    unlink("/data/threadtest-shared.tmp");
    unsigned active = 0;
    for (unsigned i = 0; i < 16; i++) if (cpu_mask & (1u << i)) active++;
    if (info.scheduler_cpus > 1 && active < 2) bad++;
    volatile uint32_t timeout_word = 0;
    if (wait_on_address(&timeout_word, 0, 2) != -1 || errno != ETIMEDOUT) bad++;
    timeout_word = 1;
    if (wait_on_address(&timeout_word, 0, 1000) != -1 || errno != EAGAIN) bad++;
    uint64_t previous = uptime_ms();
    for (unsigned i = 0; i < 20000; i++) {
        uint64_t now = uptime_ms();
        if (now < previous) bad++;
        previous = now;
    }
    bad += resource_regression(&clock);
    char *args[] = {argv[0], "--exit", NULL};
    int child = spawn(argv[0], args, NULL, 0), status = -1;
    if (child < 0 || waitpid(child, &status, 0) != child || status) bad++;
    args[1] = "--exit-active";
    for (unsigned round = 0; round < 16; round++) {
        child = spawn(argv[0], args, NULL, 0); status = -1;
        if (child < 0) { bad++; break; }
        uint64_t deadline = uptime_ms() + 5000;
        int result;
        while ((result = waitpid(child, &status, WNOHANG)) == 0 && uptime_ms() < deadline)
            msleep(1);
        if (result != child || status) {
            killtree(child, 1);
            bad++;
            break;
        }
    }
    args[1] = "--fault-worker";
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0) {
        printf("threadtest: fatal-setup nullfd=%d errno=%d\n", nullfd, errno);
        bad++;
    }
    else {
        /* Pipes are growable, not bounded FIFOs. Seed an inherited stderr
           pipe and leave it unread until the fatal child is reclaimed; never
           assume a fixed byte count can make this OS's pipe return EAGAIN. */
        int map[3] = {nullfd, nullfd, nullfd};
        int diag[2] = {-1, -1};
        if (pipe(diag)) {
            printf("threadtest: fatal-setup pipe errno=%d\n", errno);
            bad++;
        }
        else {
            char fill[1024]; memset(fill, 'd', sizeof fill);
            ssize_t seeded = write(diag[1], fill, sizeof fill);
            if (seeded != sizeof fill) {
                printf("threadtest: fatal-setup seed=%ld errno=%d\n", seeded, errno);
                bad++;
            }
            map[2] = diag[1];
        }
        child = spawn(argv[0], args, map, 0); status = -1;
        int spawn_errno = errno;
        close(nullfd);
        if (diag[1] >= 0) close(diag[1]);
        if (child < 0) {
            printf("threadtest: fatal-spawn child=%d errno=%d\n", child, spawn_errno);
            bad++;
        }
        else {
            uint64_t started = uptime_ms(), deadline = started + 5000;
            int result, wait_errno;
            do {
                errno = 0;
                result = waitpid(child, &status, WNOHANG);
                wait_errno = errno;
                if (result || uptime_ms() >= deadline) break;
                msleep(1);
            } while (true);
            if (result != child || status != 134) {
                uint64_t elapsed = uptime_ms() - started;
                int killed = 0, kill_errno = 0;
                if (result != child) { killed = killtree(child, 1); kill_errno = errno; }
                printf("threadtest: fatal-wait child=%d result=%d status=%d errno=%d elapsed=%lu kill=%d kill_errno=%d\n",
                       child, result, status, wait_errno, (unsigned long)elapsed, killed, kill_errno);
                bad++;
            } else printf("threadtest: expected helper fault status=134\n");
        }
        if (diag[0] >= 0) {
            char recorded[2048];
            ssize_t nr = fcntl(diag[0], F_SETFL, O_NONBLOCK) ? -1 : read(diag[0], recorded, sizeof recorded);
            if (nr != 1024) {
                printf("threadtest: fatal-stderr expected seeded bytes=1024 got=%ld errno=%d\n", nr, errno);
                bad++;
            } else
                for (unsigned i = 0; i < 1024; i++) if (recorded[i] != 'd') { bad++; break; }
            close(diag[0]);
        }
    }
    if (clock.flags & N_CLOCK_HV_REFERENCE) {
        if (!clock.reference_page ||
            mprotect((void *)(uintptr_t)clock.reference_page, 4096, PROT_READ | PROT_WRITE) != -1 ||
            errno != EPERM) bad++;
    }
    printf("threadtest: CPU mask=%x active=%u shared malloc/TLS/FD/wait %s errors=%u\n",
           cpu_mask, active, !bad && !failures ? "PASS" : "FAIL", failures + bad);
    return bad || failures ? 1 : 0;
}
