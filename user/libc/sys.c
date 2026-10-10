/* System call wrappers and process startup. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "nocturne.h"

/* Keep the old data symbol for legacy dynamically compiled callers; rebuilt
   code uses the FS-based TCB and does not share another thread's errno. */
#undef errno
int errno;
struct libc_thread {
    struct libc_thread *self;
    int tid, error;
    void *slots[8];
};
static struct libc_thread main_thread;
static unsigned tls_initialized;
static struct libc_thread *libc_thread_current(void) {
    struct libc_thread *t;
    if (!tls_initialized) return NULL;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(t));
    return t;
}
int *__errno_location(void) {
    struct libc_thread *t = libc_thread_current();
    return t ? &t->error : &errno;
}
#define errno (*__errno_location())

long __syscall(long n, long a, long b, long c, long d, long e) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "memory");
    return r;
}

static long ret(long r) {
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define SC0(n) __syscall(n, 0, 0, 0, 0, 0)
#define SC1(n, a) __syscall(n, (long)(a), 0, 0, 0, 0)
#define SC2(n, a, b) __syscall(n, (long)(a), (long)(b), 0, 0, 0)
#define SC3(n, a, b, c) __syscall(n, (long)(a), (long)(b), (long)(c), 0, 0)
#define SC4(n, a, b, c, d) __syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), 0)

ssize_t read(int fd, void *buf, size_t n) { return ret(SC3(SYS_READ, fd, buf, n)); }
ssize_t write(int fd, const void *buf, size_t n) { return ret(SC3(SYS_WRITE, fd, buf, n)); }
int open(const char *path, int flags, ...) { return (int)ret(SC2(SYS_OPEN, path, flags)); }
int close(int fd) { return (int)ret(SC1(SYS_CLOSE, fd)); }
int audio_flush(int fd) { return (int)ret(SC1(SYS_AUDIO_FLUSH, fd)); }
long lseek(int fd, long off, int whence) { return ret(SC3(SYS_LSEEK, fd, off, whence)); }
int stat(const char *path, struct n_stat *st) { return (int)ret(SC2(SYS_STAT, path, st)); }
int fstat(int fd, struct n_stat *st) { return (int)ret(SC2(SYS_FSTAT, fd, st)); }
int readdir(int fd, int idx, struct n_dirent *d) { return (int)ret(SC3(SYS_READDIR, fd, idx, d)); }
int mkdir(const char *path, ...) { return (int)ret(SC1(SYS_MKDIR, path)); }
int unlink(const char *path) { return (int)ret(SC1(SYS_UNLINK, path)); }
int rmdir(const char *path) { return unlink(path); }
int remove(const char *path) { return unlink(path); }
int rename(const char *from, const char *to) { return (int)ret(SC2(SYS_RENAME, from, to)); }
int chdir(const char *path) { return (int)ret(SC1(SYS_CHDIR, path)); }
char *getcwd(char *buf, size_t n) { return ret(SC2(SYS_GETCWD, buf, n)) < 0 ? NULL : buf; }
/* A text file that is not an ELF program is run as a script: by the interpreter named on a
   "#!" first line, else by /bin/sh. Returns the interpreter path in buf, or NULL. */
static const char *script_interp(const char *path, char *buf, size_t n) {
    char head[256];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t len = read(fd, head, sizeof head - 1);
    close(fd);
    if (len <= 0) return NULL;
    head[len] = 0;
    if ((size_t)len != strlen(head)) return NULL; /* binary data */
    if (head[0] == '#' && head[1] == '!') {
        char *p = head + 2;
        while (*p == ' ') p++;
        size_t k = strcspn(p, " \t\r\n");
        if (k == 0 || k >= n) return NULL;
        memcpy(buf, p, k);
        buf[k] = 0;
        return buf;
    }
    return "/bin/sh";
}

int spawn(const char *path, char *const argv[], const int fdmap[3], int flags) {
    long r = SC4(SYS_SPAWN, path, argv, fdmap, flags);
    if (r == -ENOEXEC) {
        char ibuf[128];
        const char *interp = script_interp(path, ibuf, sizeof ibuf);
        if (interp && strcmp(interp, path) != 0) {
            int argc = 0;
            while (argv && argv[argc]) argc++;
            char **nargv = malloc(sizeof(char *) * (size_t)(argc + 2));
            if (!nargv) return (int)ret(-ENOMEM);
            nargv[0] = (char *)interp;
            nargv[1] = (char *)path;
            for (int i = 1; i < argc; i++) nargv[i + 1] = argv[i];
            nargv[argc < 1 ? 2 : argc + 1] = NULL;
            r = SC4(SYS_SPAWN, interp, nargv, fdmap, flags);
            free(nargv);
        }
    }
    return (int)ret(r);
}
int waitpid(int pid, int *status, int flags) { return (int)ret(SC3(SYS_WAITPID, pid, status, flags)); }
int getpid(void) { return (int)SC0(SYS_GETPID); }
int getppid(void) { return (int)SC0(SYS_GETPPID); }
int kill(int pid) { return (int)ret(SC1(SYS_KILL, pid)); }
int killtree(int pid, int include_self) { return (int)ret(SC2(SYS_KILLTREE, pid, include_self)); }
void *sbrk(long inc) {
    long r = SC1(SYS_SBRK, inc);
    if (r < 0 && r > -4096) {
        errno = ENOMEM;
        return (void *)-1;
    }
    return (void *)r;
}
int msleep(unsigned ms) { return (int)SC1(SYS_SLEEP, ms); }
unsigned sleep(unsigned s) {
    msleep(s * 1000);
    return 0;
}
static struct n_clockinfo user_clock;
static unsigned clock_initialized;
static uint64_t clock_last;
static uint64_t user_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}
int clock_info(struct n_clockinfo *info) { return (int)ret(SC1(SYS_CLOCK_INFO, info)); }
static uint64_t clock_publish(uint64_t ms) {
    uint64_t previous = __atomic_load_n(&clock_last, __ATOMIC_RELAXED);
    while (ms > previous && !__atomic_compare_exchange_n(&clock_last, &previous, ms, false,
                                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return ms < previous ? previous : ms;
}
uint64_t uptime_ms(void) {
    if (__atomic_load_n(&clock_initialized, __ATOMIC_ACQUIRE) != 2) {
        unsigned expected = 0;
        if (__atomic_compare_exchange_n(&clock_initialized, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (clock_info(&user_clock) < 0) memset(&user_clock, 0, sizeof user_clock);
            __atomic_store_n(&clock_initialized, 2, __ATOMIC_RELEASE);
        } else return clock_publish((uint64_t)SC0(SYS_UPTIME));
    }
    if ((user_clock.flags & N_CLOCK_HV_REFERENCE) && user_clock.reference_page) {
        const struct n_reference_tsc *p = (const struct n_reference_tsc *)(uintptr_t)user_clock.reference_page;
        for (unsigned attempt = 0; attempt < 4; attempt++) {
            uint32_t sequence = __atomic_load_n(&p->sequence, __ATOMIC_ACQUIRE);
            if (!sequence) break;
            uint64_t tsc = user_rdtsc();
            uint64_t scale = p->scale;
            int64_t offset = p->offset;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (sequence != __atomic_load_n(&p->sequence, __ATOMIC_ACQUIRE)) continue;
            uint64_t reference = (uint64_t)(((__uint128_t)tsc * scale) >> 64) + offset;
            if (reference < user_clock.reference_sample_100ns) break;
            return clock_publish(user_clock.uptime_ms + (reference - user_clock.reference_sample_100ns) / 10000);
        }
        return clock_publish((uint64_t)SC0(SYS_UPTIME));
    }
    if (!(user_clock.flags & N_CLOCK_USER_TSC) || user_clock.tsc_hz < 1000000)
        return clock_publish((uint64_t)SC0(SYS_UPTIME));
    uint64_t now = user_rdtsc();
    if (now < user_clock.tsc_sample) return clock_publish((uint64_t)SC0(SYS_UPTIME));
    uint64_t ms = user_clock.uptime_ms + (now - user_clock.tsc_sample) / (user_clock.tsc_hz / 1000);
    return clock_publish(ms);
}

static void libc_thread_init(struct libc_thread *t) {
    memset(t, 0, sizeof *t);
    t->self = t;
    t->tid = (int)SC0(SYS_THREAD_ID);
    SC2(SYS_THREAD_TLS, t, sizeof *t);
}
static _Noreturn void libc_thread_entry(void (*fn)(void *), void *arg) {
    struct libc_thread local;
    libc_thread_init(&local);
    fn(arg);
    thread_exit();
}
int thread_create(void (*fn)(void *), void *arg) {
    if (!fn) { errno = EINVAL; return -1; }
    return (int)ret(SC3(SYS_THREAD_CREATE, fn, arg, libc_thread_entry));
}
int thread_join(int tid) { return (int)ret(SC1(SYS_THREAD_JOIN, tid)); }
_Noreturn void thread_exit(void) { SC0(SYS_THREAD_EXIT); for (;;) {} }
int thread_id(void) {
    struct libc_thread *t = libc_thread_current();
    return t ? t->tid : (int)SC0(SYS_THREAD_ID);
}
unsigned cpu_index(void) { return (unsigned)SC0(SYS_CPU_INDEX); }
void *thread_local_get(unsigned slot) {
    struct libc_thread *t = libc_thread_current();
    return t && slot < 8 ? t->slots[slot] : NULL;
}
void thread_local_set(unsigned slot, void *value) {
    struct libc_thread *t = libc_thread_current();
    if (t && slot < 8) t->slots[slot] = value;
}
int wait_on_address(volatile uint32_t *address, uint32_t expected, unsigned timeout_ms) {
    return (int)ret(SC3(SYS_WAIT_ADDRESS, address, expected, timeout_ms));
}
int wake_address(volatile uint32_t *address, unsigned count) {
    return (int)ret(SC2(SYS_WAKE_ADDRESS, address, count));
}
time_t time(time_t *t) {
    time_t v = SC0(SYS_TIME);
    if (t) *t = v;
    return v;
}
int pipe(int fds[2]) { return (int)ret(SC1(SYS_PIPE, fds)); }
int dup(int fd) { return (int)ret(SC1(SYS_DUP, fd)); }
int dup2(int fd, int newfd) { return (int)ret(SC2(SYS_DUP2, fd, newfd)); }
int poll(struct n_pollfd *fds, int n, int timeout) { return (int)ret(SC3(SYS_POLL, fds, n, timeout)); }
int proclist(struct n_procinfo *out, int max) { return (int)ret(SC2(SYS_PROCLIST, out, max)); }
int sysinfo(struct n_sysinfo *si) { return (int)ret(SC1(SYS_SYSINFO, si)); }
int cpu_info(struct n_cpuinfo *info) { return (int)ret(SC1(SYS_CPU_INFO, info)); }
void yield(void) { SC0(SYS_YIELD); }
int poweroff(void) { return (int)ret(SC1(SYS_POWER, POWER_OFF)); }
int reboot(void) { return (int)ret(SC1(SYS_POWER, POWER_REBOOT)); }
int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    return (int)ret(SC3(SYS_FCNTL, fd, cmd, arg));
}
int dmesg(char *buf, size_t n) { return (int)ret(SC2(SYS_DMESG, buf, n)); }
int pcilist(struct n_pciinfo *out, int max) { return (int)ret(SC2(SYS_PCILIST, out, max)); }
int isatty(int fd) {
    struct n_stat st;
    if (fstat(fd, &st) < 0) return 0;
    return st.type == N_FT_CHAR;
}

bool find_program(const char *name, char *out, size_t n) {
    struct n_stat st;
    if (strchr(name, '/')) {
        strlcpy(out, name, n);
        return stat(out, &st) == 0 && st.type == N_FT_FILE;
    }
    /* built-in programs first, then the ones built or installed on the persistent data disk */
    static const char *const dirs[] = {"/bin", "/data/bin"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(out, n, "%s/%s", dirs[i], name);
        if (stat(out, &st) == 0 && st.type == N_FT_FILE) return true;
    }
    return false;
}

int run_wait(const char *path, char *const argv[]) {
    char full[256];
    if (!find_program(path, full, sizeof full)) return -ENOENT;
    int pid = spawn(full, argv, NULL, 0);
    if (pid < 0) return -errno;
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

char **environ = (char *[]){NULL};

/* there is no exec: run the program as a child, wait, and exit with its status */
int execvp(const char *file, char *const argv[]) {
    char full[256];
    if (!find_program(file, full, sizeof full)) {
        errno = ENOENT;
        return -1;
    }
    int pid = spawn(full, argv, NULL, 0);
    if (pid < 0) return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    exit(st);
}
int execv(const char *path, char *const argv[]) { return execvp(path, argv); }

int system(const char *cmd) {
    char *argv[] = {"sh", "-c", (char *)cmd, NULL};
    return run_wait("/bin/sh", argv);
}

/* Environment: there is no inherited environment, just these defaults plus whatever the program
   sets for itself. */
static char **env_names, **env_vals;
static int env_n;

static int env_find(const char *name) {
    for (int i = 0; i < env_n; i++)
        if (!strcmp(env_names[i], name)) return i;
    return -1;
}

char *getenv(const char *name) {
    int i = env_find(name);
    if (i >= 0) return env_vals[i];
    if (!strcmp(name, "HOME")) return "/home";
    if (!strcmp(name, "PATH")) return "/bin:/data/bin";
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    int i = env_find(name);
    if (i >= 0 && !overwrite) return 0;
    char *v = strdup(value);
    if (!v) return -1;
    if (i >= 0) {
        free(env_vals[i]);
        env_vals[i] = v;
        return 0;
    }
    char **nn = realloc(env_names, sizeof(char *) * (size_t)(env_n + 1));
    if (nn) env_names = nn;
    char **nv = realloc(env_vals, sizeof(char *) * (size_t)(env_n + 1));
    if (nv) env_vals = nv;
    char *n = strdup(name);
    if (!nn || !nv || !n) {
        free(v);
        free(n);
        return -1;
    }
    env_names[env_n] = n;
    env_vals[env_n++] = v;
    return 0;
}

int unsetenv(const char *name) {
    int i = env_find(name);
    if (i >= 0) {
        free(env_names[i]);
        free(env_vals[i]);
        env_n--;
        env_names[i] = env_names[env_n];
        env_vals[i] = env_vals[env_n];
    }
    return 0;
}

/* make an absolute path with "." and ".." resolved (there are no symbolic links) */
char *realpath(const char *path, char *resolved) {
    char tmp[1024], *out = resolved ? resolved : malloc(1024);
    if (!out) return NULL;
    if (path[0] == '/') {
        strlcpy(tmp, path, sizeof tmp);
    } else {
        if (!getcwd(tmp, sizeof tmp)) goto fail;
        strlcat(tmp, "/", sizeof tmp);
        strlcat(tmp, path, sizeof tmp);
    }
    size_t len = 0;
    out[0] = 0;
    for (char *save, *part = strtok_r(tmp, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;
            out[len] = 0;
            continue;
        }
        if (len + strlen(part) + 2 > 1024) {
            errno = ENAMETOOLONG;
            goto fail;
        }
        out[len++] = '/';
        strcpy(out + len, part);
        len += strlen(part);
    }
    if (!len) strcpy(out, "/");
    struct n_stat st;
    if (stat(out, &st) < 0) goto fail;
    return out;
fail:
    if (!resolved) free(out);
    return NULL;
}

int mkstemp(char *tmpl) {
    size_t n = strlen(tmpl);
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) {
        errno = EINVAL;
        return -1;
    }
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned long seed = uptime_ms() * 2654435761u + (unsigned long)getpid();
    for (int tries = 0; tries < 100; tries++) {
        for (int i = 0; i < 6; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            tmpl[n - 6 + i] = chars[(seed >> 33) % 36];
        }
        struct n_stat st;
        if (stat(tmpl, &st) == 0) continue;
        return open(tmpl, O_RDWR | O_CREAT | O_TRUNC);
    }
    errno = EEXIST;
    return -1;
}

/* ---- startup / exit ---- */
void __stdio_init(void);
void __stdio_flush_all(void);
int main(int argc, char **argv);

#define MAX_ATEXIT 16
static void (*atexit_fns[MAX_ATEXIT])(void);
static int natexit;

int atexit(void (*fn)(void)) {
    if (natexit == MAX_ATEXIT) return -1;
    atexit_fns[natexit++] = fn;
    return 0;
}

_Noreturn void exit(int code) {
    while (natexit) atexit_fns[--natexit]();
    __stdio_flush_all();
    SC1(SYS_EXIT, code);
    for (;;) {}
}

_Noreturn void abort(void) {
    fputs("abort()\n", stderr);
    exit(134);
}

void __libc_init(void) {
    libc_thread_init(&main_thread);
    tls_initialized = 1;
    __stdio_init();
}
void __libc_start(int argc, char **argv) {
    __libc_init();
    exit(main(argc, argv));
}

/* ---- sys/mman.h: anonymous memory only ---- */
#include <sys/mman.h>
static int mprot(int prot) { return (prot & PROT_WRITE ? N_PROT_WRITE : 0) | (prot & PROT_EXEC ? N_PROT_EXEC : 0); }
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    (void)addr, (void)off;
    if (!(flags & MAP_ANONYMOUS) || fd != -1 || len == 0) {
        errno = ENOSYS;
        return MAP_FAILED;
    }
    long r = ret(__syscall(SYS_MMAP, (long)len, mprot(prot), 0, 0, 0));
    return r < 0 ? MAP_FAILED : (void *)r;
}
int munmap(void *addr, size_t len) { return (int)ret(__syscall(SYS_MUNMAP, (long)addr, (long)len, 0, 0, 0)); }
int mprotect(void *addr, size_t len, int prot) {
    return (int)ret(__syscall(SYS_MPROTECT, (long)addr, (long)len, mprot(prot), 0, 0));
}
