/* wxtest: checks that memory protection (W^X) works. Built and run inside Nocturne by the test suite:
     tcc -o /home/wxtest wxtest.c && /home/wxtest
   Each probe runs as a child process (wxtest PROBE) because a working probe dies with a page fault.
   Prints one "PASS name" / "FAIL name" line per check and "wxtest: N failed" at the end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "nocturne.h"

static int failed;

static void check(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failed++;
}

static int ret42(void) { return 42; }
static const unsigned char code42[] = {0xB8, 42, 0, 0, 0, 0xC3}; /* mov eax, 42; ret */

/* probes: each should crash (exit status 128 + 14 = page fault) */
static int probe(const char *what) {
    if (!strcmp(what, "write-code")) {
        *(volatile unsigned char *)(void *)ret42 = 0xC3;
    } else if (!strcmp(what, "write-rodata")) {
        *(volatile unsigned char *)(void *)code42 = 0;
    } else if (!strcmp(what, "exec-stack")) {
        unsigned char buf[16];
        memcpy(buf, code42, sizeof code42);
        return ((int (*)(void))(void *)buf)();
    } else if (!strcmp(what, "exec-heap")) {
        unsigned char *p = malloc(64);
        memcpy(p, code42, sizeof code42);
        return ((int (*)(void))(void *)p)();
    } else if (!strcmp(what, "exec-after-mprotect-rw")) {
        unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memcpy(p, code42, sizeof code42);
        mprotect(p, 4096, PROT_READ | PROT_WRITE);
        return ((int (*)(void))(void *)p)();
    }
    return 0; /* survived: the protection is missing */
}

static int run_probe(const char *self, const char *what) {
    char *argv[] = {(char *)self, (char *)what, NULL};
    int pid = spawn(self, argv, NULL, 0);
    if (pid < 0) return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

int main(int argc, char **argv) {
    if (argc > 1) return probe(argv[1]);
    const char *self = argv[0];

    check("code runs", ret42() == 42);
    const char *crash[] = {"write-code", "write-rodata", "exec-stack", "exec-heap", "exec-after-mprotect-rw"};
    for (size_t i = 0; i < sizeof crash / sizeof *crash; i++) {
        int st = run_probe(self, crash[i]);
        char name[64];
        snprintf(name, sizeof name, "%s faults (status %d)", crash[i], st);
        check(name, st == 128 + 14);
    }

    /* JIT-style: write code into RW memory, flip it to RX, call it */
    unsigned char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap", p != MAP_FAILED && ((uintptr_t)p & 4095) == 0);
    if (p != MAP_FAILED) {
        memset(p, 0x5A, 8192);
        check("mmap memory is zeroed then writable", p[8191] == 0x5A);
        memcpy(p, code42, sizeof code42);
        check("mprotect rx", mprotect(p, 4096, PROT_READ | PROT_EXEC) == 0);
        check("call mprotected code", ((int (*)(void))(void *)p)() == 42);
        check("munmap", munmap(p, 8192) == 0);
    }
    check("mprotect unmapped fails", mprotect((void *)0x500000000000, 4096, PROT_READ) < 0);

    /* the kernel must refuse to write into read-only memory for us, not crash */
    int fd = open("/etc/motd", O_RDONLY);
    long r = read(fd, (void *)code42, 4);
    check("read() into rodata is EFAULT", r < 0 && errno == EFAULT);
    close(fd);

    printf("wxtest: %d failed\n", failed);
    return failed != 0;
}
