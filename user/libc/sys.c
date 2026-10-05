/* System call wrappers and process startup. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "nocturne.h"

int errno;

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
uint64_t uptime_ms(void) { return (uint64_t)SC0(SYS_UPTIME); }
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

int system(const char *cmd) {
    char *argv[] = {"sh", "-c", (char *)cmd, NULL};
    return run_wait("/bin/sh", argv);
}

char *getenv(const char *name) {
    if (!strcmp(name, "HOME")) return "/home";
    if (!strcmp(name, "PATH")) return "/bin";
    return NULL;
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

void __libc_start(int argc, char **argv) {
    __stdio_init();
    exit(main(argc, argv));
}
