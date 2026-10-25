/* syscallfuzz: hostile-argument robustness test of the native system calls.
   syscallfuzz [seed [rounds [seconds]]]      defaults: seed 1, rounds 6, 90 seconds

   What it does (all deterministic from the seed, none of it needs a network):
     1  sweep    every call number 19..191 with hostile arguments and only invalid
                 handles: kernel-half, non-canonical, unaligned and unmapped pointers,
                 lengths that overflow at the end of the address space, negative and
                 huge indices, handles that are closed or were never open.
     2  live     the same random calls, but handle arguments come from handles that are
                 open (every kind), closed (use after close, double close) or the wrong
                 kind; waits get a zero timeout.
     3  checks   hand-written cases with a known answer: a call that must fail (bad
                 pointer, bad range, bad handle) and must not succeed or fault.
     4  races    threads using a handle while another closes and reopens it; many
                 threads closing one handle at once; waiters woken by a close.
   Each call is logged to stdout and to the system log (n_system_log_open) twice: the line
   "syscallfuzz: call sys N a b c d e" before it, so the last line names the culprit if the
   kernel hangs or faults, and "syscallfuzz: ret sys N -> R" after it. A watchdog ends the
   run with "syscallfuzz: HANG sys N" if one call does not return for 10 seconds, and the
   whole run stops after the given seconds. The last line is "syscallfuzz: RESULT PASS"
   or "syscallfuzz: RESULT FAIL violations=N".

   Left out on purpose: calls that end, power off, launch or stop programs, sleep, claim
   hardware or use the network (SYS_EXIT, SYS_POWER, SYS_SLEEP, SYS_THREAD_*, SYS_APP_LAUNCH,
   SYS_APP_OPEN, SYS_GUI_LAUNCH, SYS_WIN_CREATE, SYS_PCI_CLAIM, SYS_AUDIO_OUTPUT, 96..127).
   Pointers into memory this program owns never reach calls that change mappings. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "nocturne.h"

#define BUFSZ 8192
#define HANG_MS 10000

static char scratch[1024]; /* the scratch document, in the temporary folder (set by main) */

static uint64_t rng_state = 1;
static uint64_t rnd(void) { /* xorshift64*, same sequence for the same seed */
    uint64_t x = rng_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static uint8_t *buf;     /* BUFSZ bytes, random content that never begins a complete path */
static uint8_t *edge;    /* first byte of an unmapped page; the page before it is mapped */
static n_handle loghandle = -1;
static volatile int stop_all;
static volatile long cur_sys = -1;
static volatile uint64_t progress;
static volatile int lock_word;
static uint64_t calls, violations, started_ms, deadline_ms;
static unsigned errs[64];

static void lock(void) { while (__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE); }

static void logf_(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    lock();
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    if (loghandle >= 0) n_channel_send(loghandle, line, (size_t)n, -1);
    unlock();
}

static bool out_of_time(void) { return stop_all || n_clock_uptime_ms() >= deadline_ms; }

/* One logged call. */
static long call(long num, const long a[5]) {
    logf_("syscallfuzz: call sys %ld %lx %lx %lx %lx %lx", num, a[0], a[1], a[2], a[3], a[4]);
    cur_sys = num;
    long r = __syscall(num, a[0], a[1], a[2], a[3], a[4]);
    cur_sys = -1;
    progress++;
    calls++;
    logf_("syscallfuzz: ret sys %ld -> %ld", num, r);
    if (r < 0 && -r < 64) errs[-r]++;
    if (r < -4096L && (unsigned long)r >= 0xffff800000000000UL)
        { violations++; logf_("syscallfuzz: VIOLATION sys %ld returned kernel address %lx", num, r); }
    return r;
}

static long call5(long num, long a, long b, long c, long d, long e) {
    long v[5] = {a, b, c, d, e};
    return call(num, v);
}

#define MUST_FAIL(what, expr) do { long r_ = (expr); \
    if (r_ >= 0) { violations++; logf_("syscallfuzz: VIOLATION %s succeeded (%ld)", what, r_); } } while (0)

/* ---- hostile values ---- */
static long gen_bad(void) { /* nothing here points at memory this program owns */
    switch (rnd() % 20) {
    case 0: return 0;
    case 1: return 1;
    case 2: return -1;
    case 3: return -2147483647L - 1;
    case 4: return 2147483647L;
    case 5: return 0xffffffffL;
    case 6: return 1L << 32;
    case 7: return 0x00007fffffffffffL;
    case 8: return 0x0000800000000000L;
    case 9: return (long)0xffff800000000000UL;
    case 10: return (long)0xffffffff80000000UL;
    case 11: return 0x1001;
    case 12: return (long)(uintptr_t)edge;
    case 13: return (long)(uintptr_t)edge - 1 - (long)(rnd() % 16);
    case 14: return (long)(rnd() % 4097);
    case 15: return (long)rnd();
    case 16: return (long)(0 - (rnd() % 16));
    case 17: return (long)(0 - (rnd() % 4096));
    case 18: return 4096L << (rnd() % 20);
    default: return (long)(rnd() | 0x8000000000000000ULL);
    }
}

static long gen(void) { /* gen_bad plus pointers into buf: a range that ends at, or crosses, its end */
    switch (rnd() % 8) {
    case 0: return (long)(uintptr_t)buf;
    case 1: return (long)(uintptr_t)buf + 1;
    case 2: return (long)(uintptr_t)buf + BUFSZ - 1;
    case 3: return (long)(uintptr_t)buf + BUFSZ - (long)(rnd() % 64);
    case 4: return (long)(uintptr_t)buf + (long)(rnd() % BUFSZ);
    default: return gen_bad();
    }
}

/* ---- the table: which arguments are handles, which calls need care ---- */
struct sysinfo_ { uint16_t num; uint8_t hmask; };
static const struct sysinfo_ hinfo[] = {
    {SYS_HANDLE_CLOSE, 1}, {SYS_HANDLE_KIND, 1}, {SYS_DOCUMENT_READ_AT, 1}, {SYS_DOCUMENT_WRITE_AT, 1},
    {SYS_DOCUMENT_SIZE, 1}, {SYS_DOCUMENT_SET_SIZE, 1}, {SYS_DOCUMENT_CLAIM, 1}, {SYS_DOCUMENT_RELEASE, 1},
    {SYS_CHANNEL_SEND, 1 | 8}, {SYS_CHANNEL_RECEIVE, 1}, {SYS_CHANNEL_ACCEPT, 1},
    {SYS_APP_RESULT, 1}, {SYS_APP_STOP, 1}, {SYS_APP_ID, 1},
    {SYS_EVENTS_WATCH, 1 | 2}, {SYS_EVENTS_UNWATCH, 1}, {SYS_EVENTS_WATCH_ADDRESS, 1}, {SYS_EVENTS_TIMER, 1},
    {SYS_EVENTS_WAIT, 1}, {SYS_AUDIO_SUBMIT, 1}, {SYS_AUDIO_STATUS, 1}, {SYS_AUDIO_FLUSH, 1},
    {SYS_SHARED_MAP, 1}, {SYS_SHARED_VIEW, 1}, {SYS_PCI_INFO, 1}, {SYS_PCI_CONFIG_READ, 1},
    {SYS_PCI_CONFIG_WRITE, 1}, {SYS_PCI_BAR, 1}, {SYS_PCI_INTERRUPTS, 1}, {SYS_PCI_INTERRUPT_TAKE, 1},
    {SYS_DMA_CREATE, 1}, {SYS_DMA_ADDRESSES, 1},
    {SYS_WIN_MAP, 1}, {SYS_WIN_PRESENT, 1}, {SYS_WIN_SET_TITLE, 1}, {SYS_WIN_MOVE, 1}, {SYS_WIN_RESIZE, 1},
    {SYS_WIN_STATE, 1}, {SYS_WIN_PRESENTATION, 1}, {SYS_WIN_RECEIVE, 1},
};

static unsigned hmask_of(long num) {
    for (size_t i = 0; i < sizeof hinfo / sizeof *hinfo; i++) if (hinfo[i].num == num) return hinfo[i].hmask;
    return 0;
}

static bool skipped(long n) {
    switch (n) {
    case SYS_EXIT: case SYS_SLEEP: case SYS_POWER: case SYS_THREAD_CREATE: case SYS_THREAD_JOIN:
    case SYS_THREAD_EXIT: case SYS_WAIT_ADDRESS: case SYS_WAIT_ADDRESS64: case SYS_GUI_LAUNCH:
    case SYS_APP_LAUNCH: case SYS_APP_OPEN: case SYS_WIN_CREATE: case SYS_PCI_CLAIM: case SYS_AUDIO_OUTPUT:
    case SYS_THREAD_TLS: /* a valid block would redirect this thread's TLS base */
        return true;
    }
    return n >= SYS_NET_INFO && n < SYS_NET_LIMIT;
}

/* Calls that change this program's mappings or drive the screen or the GPU never get a pointer into
   its own memory: a wild range there would be the program's own doing, not a kernel fault. */
static bool badptr_only(long n) {
    switch (n) {
    case SYS_MEMORY_RELEASE: case SYS_MEMORY_PROTECT: case SYS_SHARED_MAP: case SYS_GPU_RENDER:
    case SYS_GPU_RENDER_BATCH: case SYS_GPU_BLIT: case SYS_WIN_MAP: case SYS_WIN_PRESENT:
        return true;
    }
    return false;
}

static bool creates_handle(long n) {
    switch (n) {
    case SYS_DOCUMENT_OPEN: case SYS_CHANNEL_LISTEN: case SYS_CHANNEL_CONNECT: case SYS_CHANNEL_ACCEPT:
    case SYS_SHARED_CREATE: case SYS_SHARED_RESERVE: case SYS_EVENTS_CREATE: case SYS_SYSTEM_LOG_OPEN:
    case SYS_AUDIO_OPEN: case SYS_PCI_BAR: case SYS_DMA_CREATE: case SYS_SHARED_VIEW:
        return true;
    }
    return false;
}

/* ---- handles in play ---- */
#define LIVE_MAX 24
static long live[LIVE_MAX];
static int nlive;
static long closed_[8];
static int nclosed;

static void remember_closed(long h) { closed_[nclosed++ & 7] = h; }

static long handle_invalid(void) { /* never 0..2 (the terminal) or the log handle */
    for (;;) {
        long h;
        switch (rnd() % 10) {
        case 0: h = -1; break;
        case 1: h = -2147483647L - 1; break;
        case 2: h = 2147483647L; break;
        case 3: h = 0x80000000L; break;
        case 4: h = 0xffffffffL; break;
        case 5: h = 1L << 20; break;
        case 6: h = (1L << 32) | (long)(live[0] > 0 ? live[0] : 3); break; /* upper bits must not be ignored */
        case 7: h = 0 - (long)(rnd() % 8192); break;
        case 8: h = nclosed ? closed_[rnd() % (unsigned)(nclosed < 8 ? nclosed : 8)] : 5000; break;
        default: h = 3 + (long)(rnd() % 4000); break;
        }
        if (h >= 0 && h <= 2) continue;
        if (h == loghandle) continue;
        return h;
    }
}

static long handle_live(void) {
    if (nlive && rnd() % 4 != 0) return live[rnd() % (unsigned)nlive];
    return handle_invalid();
}

static void live_add(long h) {
    if (h < 3 || h == loghandle) return;
    for (int i = 0; i < nlive; i++) if (live[i] == h) return;
    if (nlive == LIVE_MAX) {
        long old = live[rnd() % LIVE_MAX];
        for (int i = 0; i < nlive; i++) if (live[i] == old) { live[i] = h; break; }
        __syscall(SYS_HANDLE_CLOSE, old, 0, 0, 0, 0);
        return;
    }
    live[nlive++] = h;
}

static void live_drop(long h) {
    for (int i = 0; i < nlive; i++) if (live[i] == h) { live[i] = live[--nlive]; remember_closed(h); return; }
}

/* A fresh set: one of every kind we can make without touching the outside world. */
static void live_refill(void) {
    n_handle p[2];
    long h;
    if ((h = n_document_open(scratch, N_DOCUMENT_CREATE)) < 0) h = n_document_open(scratch, N_DOCUMENT_EDIT);
    live_add(h);
    live_add(n_document_open("C:\\", N_DOCUMENT_READ));
    live_add(n_document_open(scratch, N_DOCUMENT_READ));
    live_add(n_document_open(scratch, N_DOCUMENT_REPLACE));
    if (n_channel_pair(p) == 0) { live_add(p[0]); live_add(p[1]); }
    live_add(n_events_create());
    live_add(n_shared_create(8192));
    live_add(n_shared_reserve(65536));
    live_add(n_channel_listen("syscallfuzz.listener"));
    live_add(n_audio_open());
}

/* ---- the random call engine ---- */
static void clamp_waits(long num, long a[5]) {
    switch (num) {
    case SYS_EVENTS_WAIT: a[3] = (long)(rnd() % 3); break;
    case SYS_CHANNEL_RECEIVE: a[3] &= ~(long)N_CHANNEL_WAIT; break;
    case SYS_DOCUMENT_CLAIM: a[1] = 0; break;
    case SYS_WIN_RECEIVE: a[3] = 0; break;
    case SYS_APP_RESULT: a[1] = 0; break;
    case SYS_AUDIO_SUBMIT: a[3] &= ~(long)N_AUDIO_WAIT; break;
    case SYS_PCI_INTERRUPT_TAKE: a[1] = 0; break;
    /* Sizes that make the kernel reserve real memory stay small unless they are absurd
       (those must be refused); a 2 GiB request is a test of the machine, not of the check. */
    case SYS_SHARED_CREATE: case SYS_SHARED_RESERVE: case SYS_MEMORY_RESERVE:
        if ((unsigned long)a[0] > (16UL << 20) && (unsigned long)a[0] < (1UL << 40)) a[0] %= (16L << 20);
        break;
    case SYS_DMA_CREATE: case SYS_DOCUMENT_SET_SIZE:
        if ((unsigned long)a[1] > (16UL << 20) && (unsigned long)a[1] < (1UL << 40)) a[1] %= (16L << 20);
        break;
    }
}

static void one_call(long num, bool use_live) {
    long a[5];
    unsigned hm = hmask_of(num);
    bool bad = badptr_only(num);
    for (int i = 0; i < 5; i++)
        a[i] = (hm >> i & 1) ? (use_live ? handle_live() : handle_invalid()) : (bad ? gen_bad() : gen());
    clamp_waits(num, a);
    long r = call(num, a);
    if (r >= 3) {
        if (creates_handle(num)) {
            if (num == SYS_CHANNEL_ACCEPT || use_live) live_add(r);
            else call5(SYS_HANDLE_CLOSE, r, 0, 0, 0, 0);
        } else if (num == SYS_MEMORY_RESERVE) {
            call5(SYS_MEMORY_RELEASE, r, a[0], 0, 0, 0);
        }
    }
    if (num == SYS_HANDLE_CLOSE && r == 0) live_drop(a[0]);
}

static void sweep(bool use_live, int rounds) {
    for (int round = 0; round < rounds && !out_of_time(); round++) {
        if (use_live) live_refill();
        for (long n = 19; n < SYS_MAX && !out_of_time(); n++) {
            if (skipped(n)) continue;
            one_call(n, use_live);
        }
    }
}

/* ---- phase 3: cases with a known answer ---- */
static void checks(void) {
    n_handle d = n_document_open(scratch, N_DOCUMENT_CREATE);
    if (d < 0) d = n_document_open(scratch, N_DOCUMENT_EDIT);
    if (d < 0) { logf_("syscallfuzz: no scratch document (%s), checks use the root folder only", n_result_text(d)); }
    static const char hello[] = "hello";
    if (d >= 0) n_document_write_at(d, 0, hello, sizeof hello);
    long kern = (long)0xffff800000000000UL, kern2 = (long)0xffffffff80000000UL;
    long nc = 0x0000800000000000L, un = 0x1001, null = 0;
    long bads[] = {kern, kern2, nc, un, null + 8, (long)(uintptr_t)edge, (long)(uintptr_t)edge - 3, -1L};
    for (size_t i = 0; i < sizeof bads / sizeof *bads && !out_of_time(); i++) {
        long p = bads[i];
        if (d >= 0) {
            MUST_FAIL("document_read_at into a bad pointer", call5(SYS_DOCUMENT_READ_AT, d, 0, p, 6, 0));
            MUST_FAIL("document_write_at from a bad pointer", call5(SYS_DOCUMENT_WRITE_AT, d, 0, p, 6, 0));
        }
        MUST_FAIL("entry_info on a bad path", call5(SYS_ENTRY_INFO, p, (long)(uintptr_t)buf, 0, 0, 0));
        MUST_FAIL("entry_info into a bad result", call5(SYS_ENTRY_INFO, (long)(uintptr_t)"C:\\", p, 0, 0, 0));
        MUST_FAIL("document_open of a bad path", call5(SYS_DOCUMENT_OPEN, p, N_DOCUMENT_READ, 0, 0, 0));
        call5(SYS_FOLDER_LIST, (long)(uintptr_t)"C:\\", p, 4, 0, 0); /* an empty folder writes nothing: no verdict */
        MUST_FAIL("folder_create of a bad path", call5(SYS_FOLDER_CREATE, p, 0, 0, 0, 0));
        MUST_FAIL("entry_remove of a bad path", call5(SYS_ENTRY_REMOVE, p, 0, 0, 0, 0));
        MUST_FAIL("entry_move of a bad path", call5(SYS_ENTRY_MOVE, p, (long)(uintptr_t)"C:\\x", 0, 0, 0));
        MUST_FAIL("document_save of bad data", call5(SYS_DOCUMENT_APPEND, (long)(uintptr_t)scratch, p, 16, 0, 0));
        MUST_FAIL("channel_listen on a bad key", call5(SYS_CHANNEL_LISTEN, p, 0, 0, 0, 0));
        call5(SYS_SYSTEM_LOG, p, 64, 0, 0, 0);
        MUST_FAIL("random into a bad buffer", call5(SYS_RANDOM, p, 16, 0, 0, 0));
        MUST_FAIL("sysinfo into a bad buffer", call5(SYS_SYSINFO, p, 0, 0, 0, 0));
        MUST_FAIL("proclist into a bad buffer", call5(SYS_PROCLIST, p, 8, 0, 0, 0));
        if (p == kern || p == kern2 || p == nc) { /* never mappable by this app */
            MUST_FAIL("memory_release of a kernel-half range", call5(SYS_MEMORY_RELEASE, p, 4096, 0, 0, 0));
            MUST_FAIL("memory_protect of a kernel-half range", call5(SYS_MEMORY_PROTECT, p, 4096, N_MEMORY_READ, 0, 0));
            MUST_FAIL("shared_map of a bad handle at a kernel-half size", call5(SYS_SHARED_MAP, -1, (long)UINT64_MAX, N_MEMORY_READ, 0, 0));
        } else {
            call5(SYS_MEMORY_RELEASE, p, 4096, 0, 0, 0);
            call5(SYS_MEMORY_PROTECT, p, 4096, N_MEMORY_READ, 0, 0);
        }
        MUST_FAIL("thread_tls at a bad block", call5(SYS_THREAD_TLS, p, 64, 0, 0, 0));
        n_handle pr[2];
        if (n_channel_pair(pr) == 0) {
            MUST_FAIL("channel_send from a bad buffer", call5(SYS_CHANNEL_SEND, pr[0], p, 16, -1, 0));
            /* A receive checks only the bytes it copies (a large buffer stays
               untouched), so the message fills all 16: it must fail and stay queued. */
            static const char sixteen[] = "0123456789abcdef";
            MUST_FAIL("channel_receive into a bad buffer", (n_channel_send(pr[0], sixteen, 16, -1), call5(SYS_CHANNEL_RECEIVE, pr[1], p, 16, 0, (long)(uintptr_t)buf)));
            if (n_channel_receive(pr[1], buf, 16, 0, NULL) != 16) {
                violations++;
                logf_("syscallfuzz: VIOLATION a refused channel_receive lost its message");
            }
            n_handle_close(pr[0]);
            n_handle_close(pr[1]);
        }
    }
    /* Ranges: offset + length wraps, lengths beyond the address space, indices at the extremes. */
    uint64_t huge[] = {UINT64_MAX, UINT64_MAX - 3, (uint64_t)INT64_MAX, (uint64_t)1 << 63, (uint64_t)1 << 47, (uint64_t)1 << 32, 0xffffffffu};
    for (size_t i = 0; i < sizeof huge / sizeof *huge && !out_of_time(); i++) {
        long h = (long)huge[i];
        bool absurd = i < 4; /* more than the address space can hold, or negative: always refused */
        if (d >= 0) {
            call5(SYS_DOCUMENT_READ_AT, d, 0, (long)(uintptr_t)buf, h, 0); /* may be clamped to the document: no verdict */
            call5(SYS_DOCUMENT_READ_AT, d, h, (long)(uintptr_t)buf, 8, 0);
            call5(SYS_DOCUMENT_READ_AT, d, (long)(UINT64_MAX - 3), (long)(uintptr_t)buf, 8, 0);
            if (absurd) {
                MUST_FAIL("document_write_at with offset+length overflow", call5(SYS_DOCUMENT_WRITE_AT, d, h, (long)(uintptr_t)buf, 8, 0));
                MUST_FAIL("document_write_at with a length past the address space", call5(SYS_DOCUMENT_WRITE_AT, d, 0, (long)(uintptr_t)buf, h, 0));
                MUST_FAIL("document_set_size past what a file can be", call5(SYS_DOCUMENT_SET_SIZE, d, h, 0, 0, 0));
            }
            n_document_set_size(d, 8);
        }
        if (absurd) {
            MUST_FAIL("document_save of a length past the address space", call5(SYS_DOCUMENT_SAVE, (long)(uintptr_t)scratch, (long)(uintptr_t)buf, h, 0, 0));
            MUST_FAIL("document_append of a length past the address space", call5(SYS_DOCUMENT_APPEND, (long)(uintptr_t)scratch, (long)(uintptr_t)buf, h, 0, 0));
            MUST_FAIL("channel_send of a length past the address space", call5(SYS_CHANNEL_SEND, nlive ? live[0] : -1, (long)(uintptr_t)buf, h, -1, 0));
            MUST_FAIL("random of a length past the address space", call5(SYS_RANDOM, (long)(uintptr_t)buf, h, 0, 0, 0));
            MUST_FAIL("memory_reserve of an impossible size", call5(SYS_MEMORY_RESERVE, h, N_MEMORY_READ_WRITE, 0, 0, 0) > 0 ? 1 : -1);
            MUST_FAIL("shared_create of an impossible size", call5(SYS_SHARED_CREATE, h, 0, 0, 0, 0));
            MUST_FAIL("shared_reserve of an impossible size", call5(SYS_SHARED_RESERVE, h, 0, 0, 0, 0));
        }
        call5(SYS_SYSTEM_LOG, (long)(uintptr_t)buf, h, 0, 0, 0);
        call5(SYS_FOLDER_LIST, (long)(uintptr_t)"C:\\", (long)(uintptr_t)buf, h, 0, 0);
        call5(SYS_FOLDER_LIST, (long)(uintptr_t)"C:\\", (long)(uintptr_t)buf, 4, h, 0);
        call5(SYS_EVENTS_TIMER, nlive ? live[0] : -1, h, 0, 1, 0);
    }
    /* Paths, in every form the parser reads: drive letters and "C:" alone, other drives, '\' and '/'
       mixed, "." and ".." (also past the root), a name and a path that are too long, characters and
       bytes a name cannot hold, a name that ends at an unmapped page. Each is refused or names a
       folder that exists (the root, C:\Nocturne), so nothing is created or removed by the calls. */
    static char longp[5000], name256[258];
    memset(longp, 'a', sizeof longp - 1);
    longp[0] = '\\';
    longp[sizeof longp - 1] = 0;
    name256[0] = '\\'; /* a name one byte past the longest */
    memset(name256 + 1, 'a', 256);
    static char dots[600];
    for (size_t i = 0; i + 3 < sizeof dots; i += 3) memcpy(dots + i, "\\..", 3);
    dots[sizeof dots - 1] = 0;
    static char slashes[600];
    memset(slashes, '\\', sizeof slashes - 1);
    const char *paths[] = {
        longp, name256, dots, slashes, longp + 4000, "", "relative", "a\\b", ".", "..",
        "C:", "c:", "C", ":", "::", ":\\", "C::\\", "C:x", "C:a\\b", "1:\\x", "D:\\x", "d:\\", "Z:\\", "\\\\server\\share",
        "C:\\", "c:\\", "C:/", "\\", "/", "//", "\\\\", "C:\\\\", "C:\\.", "C:\\..", "c:/../..", "\\.",
        "\\a\\.\\b\\..\\..\\..", "/a/./b/../../..", "C:/\\a/..\\\\..//", "c:\\NOCTURNE\\system\\..",
        "C:\\a:b", "\\a*b", "\\a?b", "\\a\"b", "\\a<b", "\\a>b", "\\a|b", "\\a\tb", "\\a\x01" "b", "\\a\x7f",
        "\\trailing ", "\\trailing.", "\\...", "\\\xc2\x85", "\\\xff\xfe", "\\\xc0\x80", "\\\xed\xa0\x80", "\\\xf4\x90\x80\x80"};
    for (size_t i = 0; i < sizeof paths / sizeof *paths && !out_of_time(); i++) {
        long p = (long)(uintptr_t)paths[i];
        struct n_entry en;
        call5(SYS_ENTRY_INFO, p, (long)(uintptr_t)&en, 0, 0, 0);
        long h = call5(SYS_DOCUMENT_OPEN, p, N_DOCUMENT_READ, 0, 0, 0);
        if (h >= 3) call5(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0);
        h = call5(SYS_DOCUMENT_OPEN, p, N_DOCUMENT_CREATE, 0, 0, 0);
        if (h >= 3) { call5(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0); }
        call5(SYS_FOLDER_LIST, p, (long)(uintptr_t)buf, 2, 0, 0);
        call5(SYS_FOLDER_CREATE, p, 0, 0, 0, 0);
        call5(SYS_ENTRY_MOVE, p, p, 0, 0, 0);
    }
    /* An unterminated string ending where the mapping ends. */
    for (long back = 1; back <= 9; back += 4) {
        long p = (long)(uintptr_t)edge - back;
        call5(SYS_ENTRY_INFO, p, (long)(uintptr_t)buf, 0, 0, 0);
        call5(SYS_DOCUMENT_OPEN, p, N_DOCUMENT_READ, 0, 0, 0);
        call5(SYS_CHANNEL_CONNECT, p, 0, 0, 0, 0);
        call5(SYS_CLIPBOARD_SET, p, 64, 0, 0, 0);
        call5(SYS_WIN_SET_TITLE, -1, p, 0, 0, 0);
        call5(SYS_DOCUMENT_SAVE, p, p, 64, 0, 0);
        call5(SYS_RANDOM, p, 64, 0, 0, 0);
        call5(SYS_SYSTEM_LOG, p, 64, 0, 0, 0);
    }
    /* Handles: closed, double closed, the wrong kind, an index in the upper half of 64 bits. */
    if (d >= 0) {
        long dd = d;
        if (call5(SYS_HANDLE_CLOSE, dd, 0, 0, 0, 0) < 0) { violations++; logf_("syscallfuzz: VIOLATION closing an open document failed"); }
        MUST_FAIL("second close", call5(SYS_HANDLE_CLOSE, dd, 0, 0, 0, 0));
        MUST_FAIL("handle_kind of a closed handle", call5(SYS_HANDLE_KIND, dd, 0, 0, 0, 0));
        MUST_FAIL("read of a closed document", call5(SYS_DOCUMENT_READ_AT, dd, 0, (long)(uintptr_t)buf, 8, 0));
        MUST_FAIL("size of a closed document", call5(SYS_DOCUMENT_SIZE, dd, 0, 0, 0, 0));
        MUST_FAIL("release of a closed document", call5(SYS_DOCUMENT_RELEASE, dd, 0, 0, 0, 0));
    }
    n_handle pr[2];
    if (n_channel_pair(pr) == 0) {
        MUST_FAIL("document_read_at on a channel", call5(SYS_DOCUMENT_READ_AT, pr[0], 0, (long)(uintptr_t)buf, 8, 0));
        MUST_FAIL("events_wait on a channel", call5(SYS_EVENTS_WAIT, pr[0], (long)(uintptr_t)buf, 4, 0, 0));
        MUST_FAIL("channel_accept on a plain channel", call5(SYS_CHANNEL_ACCEPT, pr[0], 0, 0, 0, 0));
        MUST_FAIL("shared_map of a channel", call5(SYS_SHARED_MAP, pr[0], 4096, N_MEMORY_READ, 0, 0));
        call5(SYS_CHANNEL_SEND, pr[0], (long)(uintptr_t)buf, 1, pr[0], 0); /* a channel end attached to itself: no verdict, must not hang */
        n_handle_close(pr[0]);
        MUST_FAIL("send to a closed peer", call5(SYS_CHANNEL_SEND, pr[1], (long)(uintptr_t)buf, 1, -1, 0));
        n_handle_close(pr[1]);
    }
    for (int i = 0; i < 2000 && !out_of_time(); i++) {
        long h = (long)(((uint64_t)rnd() << 32) | 3); /* only the low 32 bits name a handle in this app */
        call5(SYS_HANDLE_KIND, h, 0, 0, 0, 0);
    }
    /* Memory: a protect/release that names more than was reserved; a mapping released twice. */
    void *m;
    if (n_memory_reserve(3 * 4096, N_MEMORY_READ_WRITE, &m) >= 0) {
        call5(SYS_MEMORY_PROTECT, (long)(uintptr_t)m + 1, 4096, N_MEMORY_READ, 0, 0);
        call5(SYS_MEMORY_PROTECT, (long)(uintptr_t)m, 4096, 7, 0, 0);
        call5(SYS_MEMORY_PROTECT, (long)(uintptr_t)m, 4096, N_MEMORY_READ_WRITE | N_MEMORY_READ_EXECUTE, 0, 0);
        call5(SYS_MEMORY_PROTECT, (long)(uintptr_t)m, (long)UINT64_MAX - 4095, N_MEMORY_READ, 0, 0);
        n_memory_release(m, 3 * 4096);
        /* no second release: the range may belong to a new mapping of this program by now */
    }
    /* Address waits with a short timeout: bad and misaligned words, and a word that differs. */
    static volatile uint32_t word;
    long wp[] = {kern, kern2, nc, un, (long)(uintptr_t)edge, (long)(uintptr_t)&word + 1, (long)(uintptr_t)&word + 2};
    for (size_t i = 0; i < sizeof wp / sizeof *wp; i++) {
        call5(SYS_WAIT_ADDRESS, wp[i], 0, 1, 0, 0);
        call5(SYS_WAIT_ADDRESS64, wp[i], 0, 1, 0, 0);
        call5(SYS_WAKE_ADDRESS, wp[i], 0, 0, 0, 0);
        call5(SYS_WAKE_ADDRESS, wp[i], 1, 0, 0, 0);
    }
    call5(SYS_WAIT_ADDRESS, (long)(uintptr_t)&word, 7, 1, 0, 0);
    call5(SYS_WAIT_ADDRESS, (long)(uintptr_t)&word, 0, 1, 0, 0);
    call5(SYS_WAKE_ADDRESS, (long)(uintptr_t)&word, (long)UINT64_MAX, 0, 0, 0);
    call5(SYS_THREAD_JOIN, -1, 0, 0, 0, 0);
    call5(SYS_THREAD_JOIN, 0x7fffffff, 0, 0, 0, 0);
    call5(SYS_THREAD_JOIN, 1L << 32, 0, 0, 0, 0);
    call5(SYS_EVENTS_UNWATCH, -1, (long)UINT64_MAX, 0, 0, 0);
    if (d >= 0) {
        call5(SYS_HANDLE_CLOSE, d, 0, 0, 0, 0);
    }
    call5(SYS_ENTRY_REMOVE, (long)(uintptr_t)scratch, 0, 0, 0, 0);
}

/* ---- phase 4: races ---- */
#define RACERS 4
static volatile long race_h;      /* the handle every racer uses */
static volatile int race_kind;    /* what the racers do with it */
static volatile int race_go, race_alive;
static volatile uint32_t race_gate;

static void racer(void *arg) {
    long id = (long)(uintptr_t)arg;
    uint64_t x = 0x9E3779B97F4A7C15ULL * (uint64_t)(id + 1) ^ rng_state; /* private sequence per thread */
    void *page;
    n_memory_reserve(4096, N_MEMORY_READ_WRITE, &page);
    uint8_t *local = page;
    struct n_event ev[4];
    while (!__atomic_load_n(&race_go, __ATOMIC_ACQUIRE) && !stop_all) {}
    while (__atomic_load_n(&race_go, __ATOMIC_ACQUIRE) && !stop_all) {
        long h = race_h;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint64_t t = x;
        switch (race_kind) {
        case 0: /* document */
            switch (t % 6) {
            case 0: __syscall(SYS_DOCUMENT_READ_AT, h, 0, (long)(uintptr_t)local, 64, 0); break;
            case 1: __syscall(SYS_DOCUMENT_WRITE_AT, h, t % 128, (long)(uintptr_t)local, 32, 0); break;
            case 2: __syscall(SYS_DOCUMENT_SIZE, h, 0, 0, 0, 0); break;
            case 3: __syscall(SYS_DOCUMENT_SET_SIZE, h, t % 4096, 0, 0, 0); break;
            case 4: __syscall(SYS_DOCUMENT_CLAIM, h, 0, 0, 0, 0); break;
            default: __syscall(SYS_DOCUMENT_RELEASE, h, 0, 0, 0, 0); break;
            }
            break;
        case 1: /* channel: a pair; racers send into one end and receive from it */
            if (t & 1) __syscall(SYS_CHANNEL_SEND, h, (long)(uintptr_t)local, 1 + t % 64, -1, 0);
            else __syscall(SYS_CHANNEL_RECEIVE, h, (long)(uintptr_t)local, 4096, (t & 2) ? N_CHANNEL_PEEK : 0, 0);
            break;
        case 2: /* event queue */
            switch (t % 5) {
            case 0: __syscall(SYS_EVENTS_TIMER, h, 1 + t % 3, t & 4, t, 0); break;
            case 1: __syscall(SYS_EVENTS_WAIT, h, (long)(uintptr_t)ev, 4, t % 3, 0); break;
            case 2: __syscall(SYS_EVENTS_UNWATCH, h, t, 0, 0, 0); break;
            case 3: __syscall(SYS_EVENTS_WATCH_ADDRESS, h, (long)(uintptr_t)&race_gate, 0, t, 0); break;
            default: __syscall(SYS_EVENTS_WATCH, h, h, N_EVENT_RECEIVABLE, t, 0); break;
            }
            break;
        case 3: /* shared object */
            if (t & 1) {
                long m = __syscall(SYS_SHARED_MAP, h, 8192, N_MEMORY_READ_WRITE, 0, 0);
                if (m > 4096) { *(volatile uint8_t *)m = 1; __syscall(SYS_MEMORY_RELEASE, m, 8192, 0, 0, 0); }
            } else __syscall(SYS_SHARED_VIEW, h, 0, 4096, 0, 0);
            break;
        default: /* nothing but close */
            __syscall(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0);
            break;
        }
        progress++;
        calls++;
        __atomic_fetch_add(&race_gate, 1, __ATOMIC_RELAXED);
    }
    if (local) n_memory_release(local, 4096);
    __atomic_fetch_sub(&race_alive, 1, __ATOMIC_RELEASE);
}

static long race_make(int kind) {
    n_handle p[2];
    switch (kind) {
    case 0: { long h = n_document_open(scratch, N_DOCUMENT_CREATE); return h < 0 ? n_document_open(scratch, N_DOCUMENT_EDIT) : h; }
    case 1: if (n_channel_pair(p) == 0) { n_handle_close(p[1]); return p[0]; } return -1; /* keep one end; sends then fail or queue */
    case 2: return n_events_create();
    case 3: return n_shared_create(8192);
    default: return n_events_create();
    }
}

static void races(int rounds) {
    static const char *names[] = {"document", "channel", "events", "shared", "double-close"};
    for (int kind = 0; kind < 5 && !out_of_time(); kind++) {
        logf_("syscallfuzz: race %s", names[kind]);
        race_kind = kind;
        race_h = race_make(kind);
        race_alive = RACERS;
        race_go = 0;
        int tids[RACERS], made = 0;
        for (long i = 0; i < RACERS; i++) {
            int t = thread_create(racer, (void *)(uintptr_t)i);
            if (t > 0) tids[made++] = t; else __atomic_fetch_sub(&race_alive, 1, __ATOMIC_RELAXED);
        }
        __atomic_store_n(&race_go, 1, __ATOMIC_RELEASE);
        for (int r = 0; r < rounds * 20 && !out_of_time(); r++) {
            n_sleep_ms(2);
            long old = race_h;
            /* close, then immediately make a new one: the number is likely reused */
            call5(SYS_HANDLE_CLOSE, old, 0, 0, 0, 0);
            if (kind == 4) call5(SYS_HANDLE_CLOSE, old, 0, 0, 0, 0);
            race_h = race_make(kind);
            n_handle sink[2];
            if (r % 7 == 0 && n_channel_pair(sink) == 0) { n_handle_close(sink[0]); n_handle_close(sink[1]); }
            progress++;
        }
        __atomic_store_n(&race_go, 0, __ATOMIC_RELEASE);
        for (int i = 0; i < made; i++) thread_join(tids[i]);
        call5(SYS_HANDLE_CLOSE, race_h, 0, 0, 0, 0);
    }
    n_entry_remove(scratch);
}

/* ---- driver ---- */
static uint64_t seed_arg = 1;
static int rounds_arg = 6;

static void worker(void *unused) {
    (void)unused;
    logf_("syscallfuzz: phase 1 sweep (invalid handles)");
    sweep(false, rounds_arg);
    logf_("syscallfuzz: phase 2 live handles");
    sweep(true, rounds_arg);
    logf_("syscallfuzz: phase 3 known-answer checks");
    checks();
    logf_("syscallfuzz: phase 4 races");
    races(rounds_arg);
    __atomic_store_n(&stop_all, 2, __ATOMIC_RELEASE);
}

static void finish(const char *how) {
    logf_("syscallfuzz: %s seed=%llu calls=%llu elapsed_ms=%llu", how, (unsigned long long)seed_arg,
          (unsigned long long)calls, (unsigned long long)(n_clock_uptime_ms() - started_ms));
    for (int i = 1; i < 64; i++) if (errs[i]) logf_("syscallfuzz: error %d (%s) x%u", i, n_result_text(-i), errs[i]);
    if (violations) logf_("syscallfuzz: RESULT FAIL violations=%llu", (unsigned long long)violations);
    else logf_("syscallfuzz: RESULT PASS");
}

int main(int argc, char **argv) {
    if (argc > 1) seed_arg = strtoull(argv[1], NULL, 0);
    if (argc > 2) rounds_arg = atoi(argv[2]);
    unsigned seconds = argc > 3 ? (unsigned)atoi(argv[3]) : 90;
    if (rounds_arg < 1) rounds_arg = 1;
    rng_state = seed_arg * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
    if (!rng_state) rng_state = 1;
    for (int i = 0; i < 8; i++) rnd();
    loghandle = n_system_log_open();
    started_ms = n_clock_uptime_ms();
    deadline_ms = started_ms + (uint64_t)seconds * 1000;

    void *pages, *pair = NULL;
    n_result reserved = n_memory_reserve(BUFSZ, N_MEMORY_READ_WRITE, &pages);
    if (reserved >= 0) reserved = n_memory_reserve(8192, N_MEMORY_READ_WRITE, &pair);
    buf = pages;
    uint8_t *two = pair;
    if (reserved < 0) { logf_("syscallfuzz: cannot reserve memory"); return 1; }
    n_memory_release(two + 4096, 4096); /* the second page is now a hole: edge points into it */
    edge = two + 4096;
    for (int i = 0; i < BUFSZ; i++) buf[i] = (uint8_t)(rnd() >> 24);
    buf[0] = 'x'; /* any path made from buf is relative (or on another drive), and so refused */
    memset(two, 'a', 4096);
    two[0] = '\\';

    logf_("syscallfuzz: start seed=%llu rounds=%d seconds=%u (buf %p, hole %p)", (unsigned long long)seed_arg,
          rounds_arg, seconds, (void *)buf, (void *)edge);
    n_path_join(n_temp_folder(), "syscallfuzz.tmp", scratch, sizeof scratch);

    int tid = thread_create(worker, NULL);
    if (tid <= 0) { logf_("syscallfuzz: cannot start the worker thread"); return 1; }
    uint64_t last = progress, last_change = n_clock_uptime_ms();
    while (!__atomic_load_n(&stop_all, __ATOMIC_ACQUIRE)) {
        n_sleep_ms(100);
        uint64_t now = n_clock_uptime_ms();
        if (progress != last) { last = progress; last_change = now; }
        if (now >= deadline_ms) { logf_("syscallfuzz: time is up"); stop_all = 1; break; }
        if (now - last_change > HANG_MS) {
            violations++;
            logf_("syscallfuzz: HANG sys %ld (no call returned for %d ms)", cur_sys, HANG_MS);
            finish("aborted");
            return 3;
        }
    }
    /* Give the worker a moment to leave its call before the process ends. */
    for (int i = 0; i < 20 && progress != last; i++) { last = progress; n_sleep_ms(50); }
    finish(stop_all == 2 ? "done" : "stopped");
    return violations ? 2 : 0;
}
