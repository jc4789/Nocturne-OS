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
    void *slots[N_TLS_SLOTS];
    void **tss;      /* C11 tss values, grown on demand (threads.c) */
    size_t tss_cap;
};
static unsigned char main_thread_area[sizeof(struct libc_thread) * 2] __attribute__((aligned(64)));
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

#define SC0(n) __syscall(n, 0, 0, 0, 0, 0)
#define SC1(n, a) __syscall(n, (long)(a), 0, 0, 0, 0)
#define SC2(n, a, b) __syscall(n, (long)(a), (long)(b), 0, 0, 0)
#define SC3(n, a, b, c) __syscall(n, (long)(a), (long)(b), (long)(c), 0, 0)
#define SC4(n, a, b, c, d) __syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), 0)

/* A text file that is not an ELF program is run as a script: by the interpreter named on a
   "#!" first line, else by sh. Returns the interpreter path in buf, or NULL. */
static const char *script_interp(const char *path, char *buf, size_t n) {
    char head[256];
    n_handle d = n_document_open(path, N_DOCUMENT_READ);
    if (d < 0) return NULL;
    n_result len = n_document_read_at(d, 0, head, sizeof head - 1);
    n_handle_close(d);
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
    return N_FOLDER_SYSTEM "\\sh";
}

static struct n_clockinfo user_clock;
static unsigned clock_initialized;
static uint64_t clock_last;
static uint64_t user_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}
n_result clock_info(struct n_clockinfo *info) { return SC1(SYS_CLOCK_INFO, info); }
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

/* ELF TLS (x86-64 variant II): the linked image ends at the FS control
   block. The linker defines these (user/user.ld, or TinyCC's
   set_tls_layout()); a program without TLS leaves them 0. */
extern char __tls_image[] __attribute__((weak)), __tls_image_end[] __attribute__((weak));
extern char __tls_memsz[] __attribute__((weak)), __tls_align[] __attribute__((weak));
/* The thread pointer's alignment: the block's and the control block's. */
static size_t libc_tls_align(void) {
    size_t align = (size_t)__tls_align;
    return align < _Alignof(struct libc_thread) ? _Alignof(struct libc_thread) : align;
}
/* The block as the linker addresses it back from the thread pointer: its
   size rounded to its own alignment, not the control block's. */
static size_t libc_tls_size(void) {
    size_t align = __tls_align ? (size_t)__tls_align : 1;
    return ((size_t)__tls_memsz + align - 1) & ~(align - 1);
}
/* Bytes a thread needs for its TLS block plus control block. */
static size_t libc_thread_area(void) { return libc_tls_size() + libc_tls_align() + sizeof(struct libc_thread); }
static struct libc_thread *libc_thread_init(void *area) {
    size_t align = libc_tls_align(), size = libc_tls_size(), image = (size_t)(__tls_image_end - __tls_image);
    uintptr_t tp = ((uintptr_t)area + size + align - 1) & ~(uintptr_t)(align - 1);
    unsigned char *block = (unsigned char *)tp - size;
    if (image) memcpy(block, __tls_image, image);
    memset(block + image, 0, size - image);
    struct libc_thread *t = (struct libc_thread *)tp;
    memset(t, 0, sizeof *t);
    t->self = t;
    t->tid = (int)SC0(SYS_THREAD_ID);
    SC2(SYS_THREAD_TLS, t, sizeof *t);
    return t;
}
static _Noreturn void libc_thread_entry(void (*fn)(void *), void *arg) {
    unsigned char area[libc_thread_area()] __attribute__((aligned(16)));
    libc_thread_init(area);
    fn(arg);
    thread_exit();
}
n_result thread_create_sized(void (*fn)(void *), void *arg, size_t stack_bytes) {
    if (!fn) return -N_ERR_INVALID;
    /* The caller asks for the stack fn runs on: libc_thread_entry's frame, with
       the thread's TLS block and control block in it, comes on top. */
    size_t area = stack_bytes ? libc_thread_area() + 64 : 0;
    if (stack_bytes > SIZE_MAX - area) return -N_ERR_INVALID;
    return SC4(SYS_THREAD_CREATE, fn, arg, libc_thread_entry, stack_bytes + area);
}
n_result thread_create(void (*fn)(void *), void *arg) { return thread_create_sized(fn, arg, 0); }
n_result thread_join(int tid) { return SC1(SYS_THREAD_JOIN, tid); }
n_result thread_detach(int tid) { return SC1(SYS_THREAD_DETACH, tid); }
_Noreturn void thread_exit(void) { SC0(SYS_THREAD_EXIT); for (;;) {} }
int thread_id(void) {
    struct libc_thread *t = libc_thread_current();
    return t ? t->tid : (int)SC0(SYS_THREAD_ID);
}
unsigned cpu_index(void) { return (unsigned)SC0(SYS_CPU_INDEX); }
n_result thread_stack_bounds(uintptr_t *low, uintptr_t *high) { return SC2(SYS_THREAD_STACK_BOUNDS, low, high); }
/* The calling thread's C11 tss value array with room for `need` keys, or
   NULL when it cannot grow (need == 0 only queries the current array). */
void **__libc_tss_values(size_t need, size_t *capacity) {
    struct libc_thread *t = libc_thread_current();
    if (!t) return NULL;
    if (need > t->tss_cap) {
        size_t cap = t->tss_cap ? t->tss_cap : 8;
        while (cap < need) cap *= 2;
        void **grown = realloc(t->tss, cap * sizeof *grown);
        if (!grown) return NULL;
        memset(grown + t->tss_cap, 0, (cap - t->tss_cap) * sizeof *grown);
        t->tss = grown;
        t->tss_cap = cap;
    }
    if (capacity) *capacity = t->tss_cap;
    return t->tss;
}
void *thread_local_get(unsigned slot) {
    struct libc_thread *t = libc_thread_current();
    return t && slot < N_TLS_SLOTS ? t->slots[slot] : NULL;
}
void thread_local_set(unsigned slot, void *value) {
    struct libc_thread *t = libc_thread_current();
    if (t && slot < N_TLS_SLOTS) t->slots[slot] = value;
}
time_t time(time_t *t) {
    time_t v = n_clock_wall();
    if (t) *t = v;
    return v;
}
n_handle n_shared_create(size_t bytes) { return (n_handle)__syscall(SYS_SHARED_CREATE, (long)bytes, 0, 0, 0, 0); }
n_handle n_shared_reserve(size_t bytes) { return (n_handle)__syscall(SYS_SHARED_RESERVE, (long)bytes, 0, 0, 0, 0); }
n_result n_shared_map(n_handle shared, size_t bytes, int access, void **mapping) {
    long r = SC3(SYS_SHARED_MAP, shared, bytes, access);
    *mapping = r < 0 ? NULL : (void *)r;
    return r < 0 ? r : 0;
}

/* ---- native ABI: results come back as they are, errno is left alone ---- */
#define NC5(n, a, b, c, d, e) __syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e))
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, unsigned timeout_ms) {
    return NC5(SYS_WAIT_ADDRESS, address, expected, timeout_ms, 0, 0);
}
n_result wait_on_address64(volatile uint64_t *address, uint64_t expected, unsigned timeout_ms) {
    return NC5(SYS_WAIT_ADDRESS64, address, expected, timeout_ms, 0, 0);
}
n_result wake_address(volatile uint32_t *address, unsigned count) {
    return NC5(SYS_WAKE_ADDRESS, address, count, 0, 0, 0);
}
const char *n_result_text(n_result r) { return strerror((int)(r < 0 ? -r : r)); }
n_result n_handle_close(n_handle h) { return NC5(SYS_HANDLE_CLOSE, h, 0, 0, 0, 0); }
n_result n_handle_kind(n_handle h) { return NC5(SYS_HANDLE_KIND, h, 0, 0, 0, 0); }
n_handle n_document_open(const char *path, int access) { return (n_handle)NC5(SYS_DOCUMENT_OPEN, path, access, 0, 0, 0); }
n_result n_document_read_at(n_handle h, uint64_t offset, void *buf, size_t n) {
    return NC5(SYS_DOCUMENT_READ_AT, h, offset, buf, n, 0);
}
n_result n_document_write_at(n_handle h, uint64_t offset, const void *buf, size_t n) {
    return NC5(SYS_DOCUMENT_WRITE_AT, h, offset, buf, n, 0);
}
n_result n_document_size(n_handle h) { return NC5(SYS_DOCUMENT_SIZE, h, 0, 0, 0, 0); }
n_result n_document_info(n_handle h, struct n_entry *out) { return NC5(SYS_DOCUMENT_INFO, h, out, 0, 0, 0); }
n_result n_document_set_size(n_handle h, uint64_t size) { return NC5(SYS_DOCUMENT_SET_SIZE, h, size, 0, 0, 0); }
n_result n_document_claim(n_handle h, bool wait) { return NC5(SYS_DOCUMENT_CLAIM, h, wait, 0, 0, 0); }
n_result n_document_release(n_handle h) { return NC5(SYS_DOCUMENT_RELEASE, h, 0, 0, 0, 0); }
n_result n_document_commit(n_handle h, int how) { return NC5(SYS_DOCUMENT_COMMIT, h, how, 0, 0, 0); }
n_result n_document_save(const char *path, const void *data, size_t size) {
    return NC5(SYS_DOCUMENT_SAVE, path, data, size, 0, 0);
}
n_result n_document_append(const char *path, const void *data, size_t size) {
    return NC5(SYS_DOCUMENT_APPEND, path, data, size, 0, 0);
}
n_result n_document_load(const char *path, void **data, size_t *size) {
    *data = NULL;
    *size = 0;
    n_handle h = n_document_open(path, N_DOCUMENT_READ);
    if (h < 0) return h;
    n_result r = n_document_size(h);
    size_t reserved = r > 0 ? (size_t)r : 0, bytes = 0;
    void *pages = NULL;
    if (reserved) r = n_memory_reserve(reserved, N_MEMORY_READ_WRITE, &pages);
    uint8_t *buf = pages;
    while (r >= 0 && bytes < reserved) {
        r = n_document_read_at(h, bytes, buf + bytes, reserved - bytes);
        if (r <= 0) break; /* 0: it shrank meanwhile */
        bytes += (size_t)r;
    }
    n_handle_close(h);
    if (r < 0) {
        if (buf) n_memory_release(buf, reserved);
        return r;
    }
    /* it shrank meanwhile: give back the pages past the end, so the caller's
       release of *size bytes covers everything */
    size_t kept = (bytes + N_PAGE_SIZE - 1) & ~(size_t)(N_PAGE_SIZE - 1);
    size_t whole = (reserved + N_PAGE_SIZE - 1) & ~(size_t)(N_PAGE_SIZE - 1);
    if (buf && kept < whole) n_memory_release(buf + kept, whole - kept);
    *data = bytes ? buf : NULL;
    *size = bytes;
    return 0;
}
n_result n_entry_info(const char *path, struct n_entry *out) { return NC5(SYS_ENTRY_INFO, path, out, 0, 0, 0); }
n_result n_entry_remove(const char *path) { return NC5(SYS_ENTRY_REMOVE, path, 0, 0, 0, 0); }
n_result n_entry_move(const char *from, const char *to) { return NC5(SYS_ENTRY_MOVE, from, to, 0, 0, 0); }
n_result n_entry_replace(const char *from, const char *to) { return NC5(SYS_ENTRY_REPLACE, from, to, 0, 0, 0); }
n_result n_folder_create(const char *path) { return NC5(SYS_FOLDER_CREATE, path, 0, 0, 0, 0); }
n_result n_folder_list(const char *path, struct n_entry *out, size_t max, uint64_t start) {
    return NC5(SYS_FOLDER_LIST, path, out, max, start, 0);
}
n_result n_folder_entries(const char *path, struct n_entry **out) {
    size_t n = 0, cap = 64;
    struct n_entry *all = malloc(cap * sizeof *all);
    if (!all) return -N_ERR_NO_MEMORY;
    for (;;) {
        n_result r = n_folder_list(path, all + n, cap - n, n);
        if (r < 0) {
            free(all);
            return r;
        }
        n += (size_t)r;
        if (n < cap) break;
        struct n_entry *grown = realloc(all, cap * 2 * sizeof *all);
        if (!grown) {
            free(all);
            return -N_ERR_NO_MEMORY;
        }
        all = grown;
        cap *= 2;
    }
    *out = all;
    return (n_result)n;
}
n_result n_channel_pair(n_handle out[2]) { return NC5(SYS_CHANNEL_PAIR, out, 0, 0, 0, 0); }
n_handle n_channel_listen(const char *key) {
    if (!key || !*key) return -N_ERR_INVALID;
    return (n_handle)NC5(SYS_CHANNEL_LISTEN, key, strlen(key), 0, 0, 0);
}
n_handle n_channel_connect(const char *key) {
    if (!key || !*key) return -N_ERR_INVALID;
    return (n_handle)NC5(SYS_CHANNEL_CONNECT, key, strlen(key), 0, 0, 0);
}
n_handle n_channel_accept(n_handle listener) { return (n_handle)NC5(SYS_CHANNEL_ACCEPT, listener, 0, 0, 0, 0); }
n_result n_channel_send(n_handle h, const void *data, size_t n, n_handle attach) {
    return NC5(SYS_CHANNEL_SEND, h, data, n, attach, 0);
}
n_result n_channel_receive(n_handle h, void *buf, size_t cap, int flags, n_handle *attached) {
    return NC5(SYS_CHANNEL_RECEIVE, h, buf, cap, flags, attached);
}
n_result n_channel_receive_from(n_handle h, void *buf, size_t cap, int flags, struct n_channel_message *message) {
    return NC5(SYS_CHANNEL_RECEIVE_FROM, h, buf, cap, flags, message);
}
n_handle n_app_launch(const char *path, const struct n_launch *launch) {
    n_handle app = (n_handle)NC5(SYS_APP_LAUNCH, path, launch, 0, 0, 0);
    if (app != -N_ERR_NOT_PROGRAM || !launch) return app;
    /* a script: its interpreter runs it, with the script's name before the rest */
    char ibuf[128];
    const char *interp = script_interp(path, ibuf, sizeof ibuf);
    if (!interp || !strcmp(interp, path)) return app;
    uint32_t argc = launch->argument_count;
    const char **args = malloc(sizeof(char *) * (argc + 2));
    if (!args) return -N_ERR_NO_MEMORY;
    args[0] = interp;
    args[1] = path;
    for (uint32_t i = 1; i < argc; i++) args[i + 1] = launch->arguments[i];
    struct n_launch l = *launch;
    l.arguments = args;
    l.argument_count = argc ? argc + 1 : 2;
    app = (n_handle)NC5(SYS_APP_LAUNCH, interp, &l, 0, 0, 0);
    free(args);
    return app;
}
n_result n_app_result(n_handle app, int64_t timeout_ms, int *status) {
    return NC5(SYS_APP_RESULT, app, timeout_ms, status, 0, 0);
}
n_result n_app_stop(n_handle app, int flags) { return NC5(SYS_APP_STOP, app, flags, 0, 0, 0); }
n_result n_random(void *buf, size_t n) { return NC5(SYS_RANDOM, buf, n, 0, 0, 0); }
n_result n_app_self(void) { return NC5(SYS_APP_SELF, 0, 0, 0, 0, 0); }
n_result n_app_parent(void) { return NC5(SYS_APP_PARENT, 0, 0, 0, 0, 0); }
n_result n_system_log(char *buf, size_t cap) { return NC5(SYS_SYSTEM_LOG, buf, cap, 0, 0, 0); }
n_handle n_system_log_open(void) { return (n_handle)NC5(SYS_SYSTEM_LOG_OPEN, 0, 0, 0, 0, 0); }
n_handle n_app_open(int64_t id) { return (n_handle)NC5(SYS_APP_OPEN, id, 0, 0, 0, 0); }
n_result n_app_id(n_handle app) { return NC5(SYS_APP_ID, app, 0, 0, 0, 0); }
n_handle n_pci_claim(uint8_t bus, uint8_t dev, uint8_t func) { return (n_handle)NC5(SYS_PCI_CLAIM, bus, dev, func, 0, 0); }
n_result n_pci_info(n_handle pci, struct n_pci_function *out) { return NC5(SYS_PCI_INFO, pci, out, 0, 0, 0); }
n_result n_pci_config_read(n_handle pci, uint32_t target, uint32_t offset, uint32_t width) {
    return NC5(SYS_PCI_CONFIG_READ, pci, target, offset, width, 0);
}
n_result n_pci_config_write(n_handle pci, uint32_t offset, uint32_t width, uint32_t value) {
    return NC5(SYS_PCI_CONFIG_WRITE, pci, offset, width, value, 0);
}
n_handle n_pci_bar(n_handle pci, uint32_t bar) { return (n_handle)NC5(SYS_PCI_BAR, pci, bar, 0, 0, 0); }
n_result n_pci_interrupts(n_handle pci, uint32_t count) { return NC5(SYS_PCI_INTERRUPTS, pci, count, 0, 0, 0); }
n_result n_pci_interrupt_take(n_handle pci, int64_t timeout_ms) { return NC5(SYS_PCI_INTERRUPT_TAKE, pci, timeout_ms, 0, 0, 0); }
n_result n_pci_hold_screen(n_handle pci, bool hold) { return NC5(SYS_PCI_HOLD_SCREEN, pci, hold ? 1 : 0, 0, 0, 0); }
n_handle n_dma_create(n_handle pci, size_t bytes, uint32_t flags) { return (n_handle)NC5(SYS_DMA_CREATE, pci, bytes, flags, 0, 0); }
n_result n_dma_addresses(n_handle shared, size_t first_page, uint64_t *out, size_t count) {
    return NC5(SYS_DMA_ADDRESSES, shared, first_page, out, count, 0);
}
n_handle n_shared_view(n_handle shared, size_t offset, size_t bytes, uint32_t cache) {
    return (n_handle)NC5(SYS_SHARED_VIEW, shared, offset, bytes, cache, 0);
}
n_handle n_shared_view_access(n_handle shared, size_t offset, size_t bytes, uint32_t cache, uint32_t access) {
    return (n_handle)NC5(SYS_SHARED_VIEW, shared, offset, bytes, cache, access);
}
n_result n_shared_revoke(n_handle view) { return NC5(SYS_SHARED_REVOKE, view, 0, 0, 0, 0); }
n_handle n_shared_compose(const struct n_shared_range *ranges, size_t count, uint32_t access) {
    return NC5(SYS_SHARED_COMPOSE, ranges, count, access, 0, 0);
}
n_handle n_audio_open(void) { return (n_handle)NC5(SYS_AUDIO_OPEN, 0, 0, 0, 0, 0); }
n_result n_audio_submit(n_handle a, const int16_t *frames, size_t count, uint32_t flags) {
    return NC5(SYS_AUDIO_SUBMIT, a, frames, count, flags, 0);
}
n_result n_audio_status(n_handle a, struct n_audio_status *out) { return NC5(SYS_AUDIO_STATUS, a, out, 0, 0, 0); }
n_result n_audio_flush(n_handle a) { return NC5(SYS_AUDIO_FLUSH, a, 0, 0, 0, 0); }
n_result n_audio_volume(void) { return NC5(SYS_AUDIO_VOLUME, -1, 0, 0, 0, 0); }
n_result n_audio_set_volume(int percent) {
    return percent < 0 ? -N_ERR_INVALID : NC5(SYS_AUDIO_VOLUME, percent, 0, 0, 0, 0);
}
n_handle n_events_create(void) { return (n_handle)NC5(SYS_EVENTS_CREATE, 0, 0, 0, 0, 0); }
n_result n_events_watch(n_handle q, n_handle h, uint32_t interest, uint64_t token) {
    return NC5(SYS_EVENTS_WATCH, q, h, interest, token, 0);
}
n_result n_events_watch_address(n_handle q, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    return NC5(SYS_EVENTS_WATCH_ADDRESS, q, address, expected, token, 0);
}
n_result n_events_timer(n_handle q, uint64_t ms, bool repeat, uint64_t token) {
    return NC5(SYS_EVENTS_TIMER, q, ms, repeat, token, 0);
}
n_result n_events_unwatch(n_handle q, uint64_t token) { return NC5(SYS_EVENTS_UNWATCH, q, token, 0, 0, 0); }
n_result n_events_wait(n_handle q, struct n_event *out, size_t max, int64_t timeout_ms) {
    return NC5(SYS_EVENTS_WAIT, q, out, max, timeout_ms, 0);
}
uint64_t n_clock_uptime_ms(void) { return uptime_ms(); }
int64_t n_clock_wall(void) { return SC0(SYS_TIME); }
void n_sleep_ms(uint64_t ms) { SC1(SYS_SLEEP, ms); }
n_result proclist(struct n_procinfo *out, int max) { return SC2(SYS_PROCLIST, out, max); }
n_result sysinfo(struct n_sysinfo *si) { return SC1(SYS_SYSINFO, si); }
n_result cpu_info(struct n_cpuinfo *info) { return SC1(SYS_CPU_INFO, info); }
void yield(void) { SC0(SYS_YIELD); }
n_result poweroff(void) { return SC1(SYS_POWER, POWER_OFF); }
n_result reboot(void) { return SC1(SYS_POWER, POWER_REBOOT); }
n_result pcilist(struct n_pciinfo *out, int max) { return SC2(SYS_PCILIST, out, max); }

static bool is_document(const char *path) {
    struct n_entry e;
    return n_entry_info(path, &e) >= 0 && e.kind == N_KIND_DOCUMENT;
}

bool find_program(const char *name, char *out, size_t n) {
    if (!*name || strpbrk(name, "\\/:")) return n_path_complete(name, out, n) >= 0 && is_document(out);
    if ((size_t)snprintf(out, n, "%s\\%s", N_FOLDER_SYSTEM, name) < n && is_document(out)) return true;
    struct n_entry *apps;
    n_result count = n_folder_entries(N_FOLDER_PROGRAMS, &apps);
    bool found = false;
    for (n_result i = 0; i < count && !found; i++)
        found = apps[i].kind == N_KIND_FOLDER &&
                (size_t)snprintf(out, n, "%s\\%s\\%s", N_FOLDER_PROGRAMS, apps[i].name, name) < n && is_document(out);
    if (count >= 0) free(apps);
    return found;
}

bool program_folder(const char *folder) {
    char parent[4096];
    return !strcasecmp(folder, N_FOLDER_SYSTEM) ||
           (n_path_join(folder, "..", parent, sizeof parent) >= 0 && !strcasecmp(parent, N_FOLDER_PROGRAMS));
}

/* Run a program in place of this app: it gets this app's input, output and log,
   folder and terminal, and its result is returned (or a negative n_result). */
static int run_here(const char *full, char *const argv[]) {
    uint32_t argc = 0;
    while (argv[argc]) argc++;
    int32_t handles[3];
    for (int i = 0; i < 3; i++) handles[i] = n_handle_kind(i) >= 0 ? i : -1;
    char folder[4104], terminal[64];
    const char *settings[2];
    uint32_t ns = 0;
    const char *f = getenv("FOLDER"), *t = getenv("TERMINAL");
    if (!f || !n_path_is_complete(f)) f = "C:\\";
    snprintf(folder, sizeof folder, "FOLDER=%s", f);
    settings[ns++] = folder;
    if (t) {
        snprintf(terminal, sizeof terminal, "TERMINAL=%s", t);
        settings[ns++] = terminal;
    }
    struct n_launch l = {.arguments = (const char *const *)argv, .settings = settings, .handles = handles,
                         .argument_count = argc, .setting_count = ns, .handle_count = 3};
    n_handle app = n_app_launch(full, &l);
    if (app < 0) return (int)app;
    int st = 0;
    n_result r = n_app_result(app, -1, &st);
    n_handle_close(app);
    return r < 0 ? (int)r : st;
}

int run_wait(const char *path, char *const argv[]) {
    char full[4096];
    if (!find_program(path, full, sizeof full)) return -N_ERR_NOT_FOUND;
    return run_here(full, argv);
}

int system(const char *cmd) {
    char *argv[] = {"sh", "-c", (char *)cmd, NULL};
    return run_wait("sh", argv);
}

/* Environment: there is no inherited environment, just the launch settings plus whatever the program
   sets for itself. */
static char **env_names, **env_vals;
static int env_n;

static int env_find(const char *name) {
    for (int i = 0; i < env_n; i++)
        if (!strcmp(env_names[i], name)) return i;
    return -1;
}

char *const *n_settings = (char *const[]){NULL};

char *getenv(const char *name) {
    int i = env_find(name);
    if (i >= 0) return env_vals[i];
    size_t len = strlen(name);
    for (char *const *s = n_settings; *s; s++)
        if (!strncmp(*s, name, len) && (*s)[len] == '=') return *s + len + 1;
    return NULL;
}

static bool is_separator(char c) { return c == '\\' || c == '/'; }

/* the length of path's drive ("C:"), or 0 */
static size_t drive_length(const char *path) {
    return ((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z' && path[1] == ':') ? 2 : 0;
}

bool n_path_is_complete(const char *path) { return is_separator(path[drive_length(path)]); }

/* The drive comes from the complete part, upper case; the names after it are joined with '\'. A
   name with a drive but no separator after it ("C:a") is in folder, as there is one drive. */
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) {
    bool alone = n_path_is_complete(name) || !folder || !n_path_is_complete(folder);
    const char *base = alone ? name : folder;
    size_t d = drive_length(base);
    if (cap < 4) return -N_ERR_TOO_LARGE;
    out[0] = d ? (char)(base[0] & ~0x20) : 'C';
    out[1] = ':';
    size_t root = 2, used = root;
    const char *parts[2] = {alone ? name + d : folder + d, alone ? "" : name + drive_length(name)};
    for (int i = 0; i < 2; i++) {
        for (const char *s = parts[i]; *s;) {
            while (is_separator(*s)) s++;
            const char *e = s;
            while (*e && !is_separator(*e)) e++;
            size_t len = (size_t)(e - s);
            if (len == 2 && s[0] == '.' && s[1] == '.') {
                while (used > root && out[used - 1] != '\\') used--;
                if (used > root) used--;
            } else if (len && !(len == 1 && s[0] == '.')) {
                if (used + 1 + len >= cap) return -N_ERR_TOO_LARGE;
                out[used++] = '\\';
                memmove(out + used, s, len);
                used += len;
            }
            s = e;
        }
    }
    if (used == root) out[used++] = '\\';
    out[used] = 0;
    return (n_result)used;
}

n_result n_path_complete(const char *name, char *out, size_t cap) {
    const char *folder = getenv("FOLDER");
    return n_path_join(folder && n_path_is_complete(folder) ? folder : "C:\\", name, out, cap);
}

const char *n_path_name(const char *path) {
    const char *name = path + drive_length(path);
    for (const char *p = name; *p; p++)
        if (is_separator(*p)) name = p + 1;
    return name;
}

static int hex_value(char c) {
    return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
}

n_result n_path_from_url(const char *url, char *out, size_t cap) {
    if (!url || strncasecmp(url, "file://", 7)) return -N_ERR_INVALID;
    const char *p = url + 7;
    if (!strncasecmp(p, "localhost/", 10)) p += 9;
    if (*p != '/') return -N_ERR_INVALID;
    size_t length = strcspn(p, "?#"), n = 0;
    char *decoded = malloc(length + 1);
    if (!decoded) return -N_ERR_NO_MEMORY;
    bool ok = true;
    for (size_t i = 0; i < length && ok; i++) {
        int c = (unsigned char)p[i];
        if (c == '%') {
            int high = i + 2 < length ? hex_value(p[i + 1]) : -1, low = high >= 0 ? hex_value(p[i + 2]) : -1;
            c = low < 0 ? -1 : high << 4 | low;
            i += 2;
        }
        /* a bad escape, a control byte or '\' is no part of a name in a URL */
        ok = c >= 32 && c != 127 && c != '\\';
        decoded[n++] = (char)c;
    }
    decoded[n] = 0;
    /* "/C:/..." names the drive */
    n_result r = ok ? n_path_join("C:\\", decoded + (drive_length(decoded + 1) ? 1 : 0), out, cap) : -N_ERR_INVALID;
    free(decoded);
    return r;
}

n_result n_path_to_url(const char *path, char *out, size_t cap) {
    static const char digits[] = "0123456789ABCDEF";
    char full[4096];
    n_result r = n_path_join("C:\\", path, full, sizeof full);
    if (r < 0) return r;
    size_t used = 8;
    if (cap <= used) return -N_ERR_TOO_LARGE;
    memcpy(out, "file:///", used);
    for (const char *s = full; *s; s++) {
        unsigned char c = *s == '\\' ? '/' : (unsigned char)*s;
        bool plain = (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || strchr("-_.~/:", c);
        if (used + (plain ? 1 : 3) >= cap) return -N_ERR_TOO_LARGE;
        if (plain) out[used++] = (char)c;
        else {
            out[used++] = '%';
            out[used++] = digits[c >> 4];
            out[used++] = digits[c & 15];
        }
    }
    out[used] = 0;
    return (n_result)used;
}

static const char *complete_setting(const char *name) {
    const char *s = getenv(name);
    return s && n_path_is_complete(s) ? s : "C:\\";
}

const char *n_user_folder(void) { return complete_setting("USERFOLDER"); }
const char *n_temp_folder(void) { return complete_setting("TEMP"); }

static n_result folder_ready(const char *path) {
    n_result r = n_folder_create(path);
    return r == -N_ERR_EXISTS ? 0 : r;
}

n_result n_app_data_folder(const char *name, char *out, size_t cap) {
    char data[4096];
    n_result r = n_path_join(n_user_folder(), "AppData", data, sizeof data), made;
    if (r >= 0) r = n_path_join(data, name, out, cap);
    if (r >= 0 && (made = folder_ready(data)) >= 0) made = folder_ready(out);
    return r < 0 ? r : made < 0 ? made : r;
}

n_result n_folder_set(const char *name) {
    char full[4096];
    n_result r = n_path_complete(name, full, sizeof full);
    if (r < 0) return r;
    struct n_entry e;
    r = n_entry_info(full, &e);
    if (r < 0) return r;
    if (e.kind != N_KIND_FOLDER) return -N_ERR_NOT_FOLDER;
    return setenv("FOLDER", full, 1) < 0 ? -N_ERR_NO_MEMORY : 0;
}

bool n_terminal(n_handle h) {
    const char *s = getenv("TERMINAL");
    while (s && *s) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s) {
            s++;
            continue;
        }
        if (v == h) return true;
        s = end;
    }
    return false;
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

/* ---- startup / exit ---- */
void __stdio_init(void);
void __stdio_flush_all(void);
int main(int argc, char **argv);

/* C and C++ callbacks share a LIFO so mixed-language registration order is
   preserved. Remove a callback before invoking it: finalization may recurse or
   register another callback, and a completed destructor must never run twice. */
struct exit_callback {
    struct exit_callback *next;
    union { void (*c)(void); void (*cxx)(void *); } fn;
    void *argument, *dso;
    bool is_cxx;
};
/* A program is one static image: its C++ destructors all register with this handle. */
void *__dso_handle = &__dso_handle;
static struct exit_callback *exit_callbacks;
static uint32_t exit_callbacks_lock;
static void exit_lock(void) {
    while (__atomic_exchange_n(&exit_callbacks_lock, 1, __ATOMIC_ACQUIRE)) {
        if (wait_on_address(&exit_callbacks_lock, 1, UINT32_MAX) < 0) yield();
    }
}
static void exit_unlock(void) {
    __atomic_store_n(&exit_callbacks_lock, 0, __ATOMIC_RELEASE);
    wake_address(&exit_callbacks_lock, UINT32_MAX);
}
static int register_exit_callback(struct exit_callback *entry) {
    if (!entry) return -1;
    exit_lock();
    entry->next = exit_callbacks;
    exit_callbacks = entry;
    exit_unlock();
    return 0;
}
int atexit(void (*fn)(void)) {
    struct exit_callback *entry = malloc(sizeof *entry);
    if (!entry) return -1;
    *entry = (struct exit_callback){.fn.c = fn};
    return register_exit_callback(entry);
}
int __cxa_atexit(void (*fn)(void *), void *argument, void *dso) {
    struct exit_callback *entry = malloc(sizeof *entry);
    if (!entry) return -1;
    *entry = (struct exit_callback){.fn.cxx = fn, .argument = argument, .dso = dso, .is_cxx = true};
    return register_exit_callback(entry);
}
void __cxa_finalize(void *dso) {
    for (;;) {
        exit_lock();
        struct exit_callback **slot = &exit_callbacks;
        while (*slot && dso && (!(*slot)->is_cxx || (*slot)->dso != dso)) slot = &(*slot)->next;
        struct exit_callback *entry = *slot;
        if (entry) *slot = entry->next;
        exit_unlock();
        if (!entry) return;
        /* Do not hold the registry lock across application code or free(). */
        if (entry->is_cxx) entry->fn.cxx(entry->argument);
        else entry->fn.c();
        free(entry);
    }
}

_Noreturn void _Exit(int code) {
    SC1(SYS_EXIT, code);
    for (;;) {}
}

_Noreturn void exit(int code) {
    __cxa_finalize(NULL);
    __stdio_flush_all();
    _Exit(code);
}

_Noreturn void abort(void) {
    fputs("abort()\n", stderr);
    exit(134);
}

void __libc_init(void) {
    void *area = main_thread_area;
    if (libc_thread_area() > sizeof main_thread_area) {
        long r = __syscall(SYS_MEMORY_RESERVE, (long)libc_thread_area(), N_MEMORY_READ_WRITE, 0, 0, 0);
        if (r < 0 && r > -4096) _Exit(127);
        area = (void *)r;
    }
    libc_thread_init(area);
    tls_initialized = 1;
    __stdio_init();
}
void __libc_start(int argc, char **argv, char **settings) {
    if (settings) n_settings = settings;
    __libc_init();
    /* Weak symbols also keep libc usable by the in-OS C compiler, whose link
       path need not supply these sections. Empty arrays are a no-op for C. */
    extern void (*__preinit_array_start[])(void) __attribute__((weak));
    extern void (*__preinit_array_end[])(void) __attribute__((weak));
    extern void (*__init_array_start[])(void) __attribute__((weak));
    extern void (*__init_array_end[])(void) __attribute__((weak));
    void __libc_fini_array(void);
    extern void (*__fini_array_start[])(void) __attribute__((weak));
    extern void (*__fini_array_end[])(void) __attribute__((weak));
    if (__fini_array_start && __fini_array_end != __fini_array_start && atexit(__libc_fini_array)) abort();
    if (__preinit_array_start)
        for (void (**fn)(void) = __preinit_array_start; fn != __preinit_array_end; ++fn) (*fn)();
    if (__init_array_start)
        for (void (**fn)(void) = __init_array_start; fn != __init_array_end; ++fn) (*fn)();
    exit(main(argc, argv));
}
void __libc_fini_array(void) {
    extern void (*__fini_array_start[])(void) __attribute__((weak));
    extern void (*__fini_array_end[])(void) __attribute__((weak));
    if (__fini_array_start)
        for (void (**fn)(void) = __fini_array_end; fn != __fini_array_start;) (*--fn)();
}

/* ---- native memory: zero-filled pages, never writable and executable at once ---- */
n_result n_memory_reserve(size_t bytes, int access, void **memory) {
    long r = SC2(SYS_MEMORY_RESERVE, bytes, access);
    *memory = r < 0 ? NULL : (void *)r;
    return r < 0 ? r : 0;
}
n_result n_memory_release(void *addr, size_t bytes) { return SC2(SYS_MEMORY_RELEASE, addr, bytes); }
n_result n_memory_protect(void *addr, size_t bytes, int access) {
    return SC3(SYS_MEMORY_PROTECT, addr, bytes, access);
}
