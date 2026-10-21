/* wxtest: checks that memory protection (W^X) works. Built and run inside Nocturne by the test suite:
     tcc -o /home/wxtest wxtest.c && /home/wxtest
   Each probe runs as a child process (wxtest PROBE) because a working probe dies with a page fault.
   Prints one "PASS name" / "FAIL name" line per check and "wxtest: N failed" at the end. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    } else if (!strcmp(what, "exec-after-protect-rw")) {
        /* Generated code is made executable, then made writable again: it must lose execute. */
        void *page;
        if (n_memory_reserve(4096, N_MEMORY_READ_WRITE, &page) < 0) return 0;
        unsigned char *p = page;
        memcpy(p, code42, sizeof code42);
        if (n_memory_protect(p, 4096, N_MEMORY_READ_EXECUTE) ||
            n_memory_protect(p, 4096, N_MEMORY_READ_WRITE)) return 0;
        return ((int (*)(void))(void *)p)();
    } else if (!strcmp(what, "write-after-protect-rx")) {
        void *page;
        if (n_memory_reserve(4096, N_MEMORY_READ_WRITE, &page) < 0) return 0;
        unsigned char *p = page;
        memcpy(p, code42, sizeof code42);
        if (n_memory_protect(p, 4096, N_MEMORY_READ_EXECUTE)) return 0;
        *(volatile unsigned char *)p = 0xC3;
    }
    return 0; /* survived: the protection is missing */
}

static int run_probe(const char *self, const char *what) {
    const char *args[] = {self, what};
    struct n_launch l = {.arguments = args, .argument_count = 2};
    n_handle app = n_app_launch(self, &l);
    if (app < 0) return -1;
    int st = 0;
    n_result r = n_app_result(app, -1, &st);
    n_handle_close(app);
    return r < 0 ? -1 : st;
}

int main(int argc, char **argv) {
    if (argc > 1) return probe(argv[1]);
    const char *self = argv[0];

    check("code runs", ret42() == 42);
    const char *crash[] = {"write-code", "write-rodata", "exec-stack", "exec-heap", "exec-after-protect-rw",
                           "write-after-protect-rx"};
    for (size_t i = 0; i < sizeof crash / sizeof *crash; i++) {
        int st = run_probe(self, crash[i]);
        char name[64];
        snprintf(name, sizeof name, "%s faults (status %d)", crash[i], st);
        check(name, st == 128 + 14);
    }

    /* JIT-style: write code into RW memory, flip it to RX, call it */
    void *pages = NULL, *none;
    n_memory_reserve(8192, N_MEMORY_READ_WRITE, &pages);
    unsigned char *p = pages;
    check("n_memory_reserve", p && ((uintptr_t)p & 4095) == 0);
    if (p) {
        check("reserved memory is zeroed", !p[0] && !p[8191]);
        memset(p, 0x5A, 8192);
        check("reserved memory is writable", p[8191] == 0x5A);
        memcpy(p, code42, sizeof code42);
        check("protect rx", n_memory_protect(p, 4096, N_MEMORY_READ_EXECUTE) == 0);
        check("call protected code", ((int (*)(void))(void *)p)() == 42);
        check("second page stays writable", (p[4096] = 1, p[4096] == 1));
        check("release", n_memory_release(p, 8192) == 0);
    }
    check("reserve without an access fails", n_memory_reserve(4096, N_MEMORY_LOW, &none) == -N_ERR_INVALID && !none);
    check("protect unmapped fails", n_memory_protect((void *)0x500000000000, 4096, N_MEMORY_READ) < 0);

    /* the kernel must refuse to write into read-only memory for us, not crash */
    n_handle motd = n_document_open("/etc/motd", N_DOCUMENT_READ);
    n_result r = motd >= 0 ? n_document_read_at(motd, 0, (void *)code42, 4) : motd;
    check("document read into rodata is a bad address", r == -N_ERR_BAD_ADDRESS);
    if (motd >= 0) n_handle_close(motd);

    printf("wxtest: %d failed\n", failed);
    return failed != 0;
}
