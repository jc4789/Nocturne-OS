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
    struct n_thread_block block; /* first: FS points here (abi.h) */
    int tid, error;
    void *slots[N_TLS_SLOTS];
    void **tss;      /* C11 tss values, grown on demand (threads.c) */
    size_t tss_cap;
    void *malloc_cache; /* this thread's small free blocks (malloc.c) */
    void *area;         /* where this record and the TLS block were put */
};
static unsigned char main_thread_area[sizeof(struct libc_thread) * 2] __attribute__((aligned(64)));
static unsigned tls_initialized;
/* Its address tells this copy of the libc from a driver image's in a thread's blocks (abi.h). */
static const char libc_copy;
static struct libc_thread *libc_thread_other(struct n_thread_block *first);
static struct libc_thread *libc_thread_current(void) {
    struct libc_thread *t;
    if (!tls_initialized) return NULL;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(t));
    return __builtin_expect(t->block.owner == &libc_copy, 1) ? t : libc_thread_other(&t->block);
}
int *__errno_location(void) {
    struct libc_thread *t = libc_thread_current();
    return t ? &t->error : &errno;
}
#define errno (*__errno_location())

/* The entry image (abi.h): its address, and a call of slot n. */
static uintptr_t entry_image;
uintptr_t n_entry_slot(unsigned slot) { return entry_image + (uintptr_t)slot * N_ENTRY_SLOT_BYTES; }
long __syscall(long n, long a, long b, long c, long d, long e) {
    return ((long (*)(long, long, long, long, long))(entry_image + (uintptr_t)n * N_ENTRY_SLOT_BYTES))(a, b, c, d, e);
}

/* The stamp every program carries (abi.h). */
__attribute__((section(".note.nocturne"), used, aligned(4))) static const struct n_program_note program_note = {
    .name_bytes = sizeof N_PROGRAM_NOTE_NAME, .desc_bytes = 2 * sizeof(uint32_t), .type = N_PROGRAM_NOTE_TYPE,
    .name = N_PROGRAM_NOTE_NAME, .generation = N_ABI_GENERATION, .slots = N_ENTRY_SLOTS};

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

/* The clocks (docs/nocturne-native-abi.md §8), read by the entry image's code without entering the kernel. */
#define ENTRY_CLOCK(type, slot) ((type (*)(void))(entry_image + (uintptr_t)(slot) * N_ENTRY_SLOT_BYTES))()
uint64_t n_clock_monotonic(void) { return ENTRY_CLOCK(uint64_t, N_ENTRY_CLOCK_MONOTONIC); }
uint64_t n_clock_boot(void) { return ENTRY_CLOCK(uint64_t, N_ENTRY_CLOCK_BOOT); }
int64_t n_clock_wall(void) { return ENTRY_CLOCK(int64_t, N_ENTRY_CLOCK_WALL); }
uint64_t n_clock_tsc_hz(void) { return ENTRY_CLOCK(uint64_t, N_ENTRY_CLOCK_TSC_HZ); }
uint64_t n_clock_after(uint64_t ns) {
    uint64_t now = n_clock_monotonic();
    return ns >= N_CLOCK_NEVER - now ? N_CLOCK_NEVER : now + ns;
}
void n_sleep_until(uint64_t deadline) { SC1(SYS_SLEEP, deadline); }
uint64_t n_app_cpu_ns(void) { return (uint64_t)SC0(SYS_APP_CPU_TIME); }

/* ELF TLS (x86-64 variant II): the linked image ends at the FS control
   block. The linker defines these (user/user.ld, or TinyCC's
   set_tls_layout()); a program without TLS leaves them 0. __libc_init reads
   them; a driver image has no such symbols, and its start reads its PT_TLS. */
extern char __tls_image[] __attribute__((weak)), __tls_image_end[] __attribute__((weak));
extern char __tls_memsz[] __attribute__((weak)), __tls_align[] __attribute__((weak));
static const char *tls_image, *tls_image_end;
static size_t tls_memsz, tls_align;
/* The thread pointer's alignment: the block's and the control block's. */
static size_t libc_tls_align(void) {
    return tls_align < _Alignof(struct libc_thread) ? _Alignof(struct libc_thread) : tls_align;
}
/* The block as the linker addresses it back from the thread pointer: its
   size rounded to its own alignment, not the control block's. */
static size_t libc_tls_size(void) {
    size_t align = tls_align ? tls_align : 1;
    return (tls_memsz + align - 1) & ~(align - 1);
}
/* Bytes a thread needs for its TLS block plus control block. */
static size_t libc_thread_area(void) { return libc_tls_size() + libc_tls_align() + sizeof(struct libc_thread); }
/* This copy's TLS block and record for the calling thread, in area. */
static struct libc_thread *libc_thread_block(void *area) {
    size_t align = libc_tls_align(), size = libc_tls_size(), image = (size_t)(tls_image_end - tls_image);
    uintptr_t tp = ((uintptr_t)area + size + align - 1) & ~(uintptr_t)(align - 1);
    unsigned char *block = (unsigned char *)tp - size;
    if (image) memcpy(block, tls_image, image);
    memset(block + image, 0, size - image);
    struct libc_thread *t = (struct libc_thread *)tp;
    memset(t, 0, sizeof *t);
    t->block.self = &t->block;
    t->block.entry = entry_image;
    t->block.owner = &libc_copy;
    t->area = area;
    t->tid = (int)SC0(SYS_THREAD_ID);
    return t;
}
static struct libc_thread *libc_thread_init(void *area) {
    struct libc_thread *t = libc_thread_block(area);
    __asm__ volatile("wrfsbase %0" : : "r"(t) : "memory"); /* the thread's own FS base */
    return t;
}
void __malloc_thread_exit(void);
void __libc_tss_end(void) __attribute__((weak)); /* threads.c, if the program has it */
/* The end of a thread another copy of the libc made, as far as this copy knows it. */
static void libc_thread_leave(struct n_thread_block *block) {
    struct libc_thread *t = (struct libc_thread *)block;
    if (__libc_tss_end) __libc_tss_end();
    free(t->tss);
    __malloc_thread_exit();
    struct n_thread_block *first;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(first));
    struct n_thread_block **link = &first->others;
    while (*link != block) link = &(*link)->others;
    *link = block->others;
    SC2(SYS_MEMORY_RELEASE, t->area, libc_thread_area());
}
/* This copy's record of a thread another copy made, the first time this copy runs on it: its own
   memory, since no copy can size or free another's. NULL without memory, as before __libc_init. */
static struct libc_thread *libc_thread_other(struct n_thread_block *first) {
    for (struct n_thread_block *b = first->others; b; b = b->others)
        if (b->owner == &libc_copy) return (struct libc_thread *)b;
    long r = SC2(SYS_MEMORY_RESERVE, libc_thread_area(), N_MEMORY_READ_WRITE);
    if (r < 0 && r > -4096) return NULL;
    struct libc_thread *t = libc_thread_block((void *)r);
    t->block.leave = libc_thread_leave;
    t->block.others = first->others;
    first->others = &t->block;
    return t;
}
/* Position-independent code's way to its thread-local block, which a driver image keeps; in a program
   the linker turned it into an offset from FS. A copy of the libc has one block: the module's
   number says nothing. */
void *__tls_get_addr(const size_t *module_and_offset) {
    struct libc_thread *t = libc_thread_current();
    if (!t) _Exit(127);
    return (char *)t - libc_tls_size() + module_and_offset[1];
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
_Noreturn void thread_exit(void) {
    if (tls_initialized) {
        struct n_thread_block *first;
        __asm__ volatile("movq %%fs:0, %0" : "=r"(first));
        while (first->others) first->others->leave(first->others);
    }
    __malloc_thread_exit();
    SC0(SYS_THREAD_EXIT);
    for (;;) {}
}
/* The allocator's per-thread slot; NULL before this thread has TLS. */
void **__libc_malloc_cache(void) {
    struct libc_thread *t = libc_thread_current();
    return t ? &t->malloc_cache : NULL;
}
int thread_id(void) {
    struct libc_thread *t = libc_thread_current();
    return t ? t->tid : (int)SC0(SYS_THREAD_ID);
}
/* The CPU's index, which the kernel keeps in its TSC_AUX. */
unsigned cpu_index(void) {
    uint32_t index;
    __asm__ volatile("rdtscp" : "=c"(index) : : "rax", "rdx");
    return index;
}
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
    int64_t ns = n_clock_wall();
    time_t v = ns >= 0 ? ns / (int64_t)N_NS_PER_S : -((-ns + (int64_t)N_NS_PER_S - 1) / (int64_t)N_NS_PER_S);
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
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, uint64_t deadline) {
    return NC5(SYS_WAIT_ADDRESS, address, expected, deadline, 0, 0);
}
n_result wait_on_address64(volatile uint64_t *address, uint64_t expected, uint64_t deadline) {
    return NC5(SYS_WAIT_ADDRESS64, address, expected, deadline, 0, 0);
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
n_result n_document_info(n_handle h, struct n_entry *out) { return NC5(SYS_DOCUMENT_INFO, h, out, sizeof *out, 0, 0); }
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
/* Entries are asked for with the size of struct n_entry (common/abi.h); SYS_ENTRY_INFO and
   SYS_FOLDER_LIST stay for programs built before its id. */
n_result n_entry_info(const char *path, struct n_entry *out) { return NC5(SYS_ENTRY_INFO_SIZED, path, out, sizeof *out, 0, 0); }
n_result n_entry_remove(const char *path) { return NC5(SYS_ENTRY_REMOVE, path, 0, 0, 0, 0); }
n_result n_entry_move(const char *from, const char *to) { return NC5(SYS_ENTRY_MOVE, from, to, 0, 0, 0); }
n_result n_entry_replace(const char *from, const char *to) { return NC5(SYS_ENTRY_REPLACE, from, to, 0, 0, 0); }
n_handle n_entry_watch(const char *path, uint32_t what) { return (n_handle)NC5(SYS_ENTRY_WATCH, path, what, 0, 0, 0); }
n_result n_folder_create(const char *path) { return NC5(SYS_FOLDER_CREATE, path, 0, 0, 0, 0); }
n_result n_folder_list(const char *path, struct n_entry *out, size_t max, uint64_t start) {
    return NC5(SYS_FOLDER_LIST_SIZED, path, out, max, start, sizeof *out);
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
n_result n_identity_self(struct n_identity *out) { return NC5(SYS_IDENTITY_SELF, out, 0, 0, 0, 0); }
n_result n_channel_peer(n_handle channel, struct n_identity *out) { return NC5(SYS_CHANNEL_PEER, channel, out, 0, 0, 0); }
n_handle n_cap_derive(n_handle cap, enum n_cap kind) { return (n_handle)NC5(SYS_CAP_DERIVE, cap, kind, 0, 0, 0); }
n_result n_cap_kind(n_handle cap) { return NC5(SYS_CAP_KIND, cap, 0, 0, 0, 0); }
n_handle n_cap_find(enum n_cap kind) { return (n_handle)NC5(SYS_CAP_FIND, kind, 0, 0, 0, 0); }
size_t n_caps_list(int32_t *out) {
    size_t n = 0;
    for (int kind = 0; kind < N_CAP_COUNT; kind++) {
        n_handle cap = n_cap_find((enum n_cap)kind);
        if (cap >= 0) out[n++] = cap;
    }
    return n;
}
n_handle n_inlet_connect(n_handle inlet) {
    n_handle pair[2];
    n_result r = n_channel_pair(pair);
    if (r < 0) return (n_handle)r;
    r = n_channel_send(inlet, "", 0, pair[1]);
    n_handle_close(pair[1]);
    if (r < 0) {
        n_handle_close(pair[0]);
        return (n_handle)r;
    }
    return pair[0];
}

n_handle n_inlet_accept(n_handle inlet, int flags) {
    char small[512], *buf = small;
    size_t cap = sizeof small;
    n_handle end = -1;
    n_result r;
    /* 本文（サービスの要求）は読み捨てる。依頼者の身元は n_channel_peer で読む。 */
    while ((r = n_channel_receive(inlet, buf, cap, flags, &end)) == -N_ERR_TOO_LARGE) {
        n_result size = n_channel_receive(inlet, NULL, 0, N_CHANNEL_PEEK, NULL);
        if (size < 0) {
            r = size;
            break;
        }
        if (buf != small) free(buf);
        buf = malloc((size_t)size);
        cap = (size_t)size;
        if (!buf) return -N_ERR_NO_MEMORY;
    }
    if (buf != small) free(buf);
    if (r < 0) return (n_handle)r;
    if (end < 0) return -N_ERR_WRONG_KIND;
    if (n_handle_kind(end) != N_KIND_CHANNEL) {
        n_handle_close(end);
        return -N_ERR_WRONG_KIND;
    }
    return end;
}

n_handle n_service_connect(const char *name) {
    if (!name) return -N_ERR_BAD_ADDRESS;
    return (n_handle)NC5(SYS_SERVICE_CONNECT, name, strlen(name), 0, 0, 0);
}

n_handle n_service_listen(const char *name) {
    if (!name) return -N_ERR_BAD_ADDRESS;
    n_handle manager = n_service_connect(N_SERVICE_BIND);
    if (manager < 0) return manager;
    n_handle pair[2];
    n_result r = n_channel_pair(pair);
    if (r < 0) {
        n_handle_close(manager);
        return (n_handle)r;
    }
    r = n_channel_send(manager, name, strlen(name), pair[1]);
    n_handle_close(pair[1]);
    int32_t reply = -N_ERR_CLOSED;
    if (r >= 0) r = n_channel_receive(manager, &reply, sizeof reply, N_CHANNEL_WAIT, NULL);
    if (r >= 0) r = reply;
    n_handle_close(manager);
    if (r < 0) {
        n_handle_close(pair[0]);
        return (n_handle)r;
    }
    return pair[0];
}

n_handle n_service_register(n_handle service_root) { return (n_handle)NC5(SYS_SERVICE_REGISTER, service_root, 0, 0, 0, 0); }
n_result n_channel_send(n_handle h, const void *data, size_t n, n_handle attach) {
    return NC5(SYS_CHANNEL_SEND, h, data, n, attach, 0);
}
n_result n_channel_receive(n_handle h, void *buf, size_t cap, int flags, n_handle *attached) {
    return NC5(SYS_CHANNEL_RECEIVE, h, buf, cap, flags, attached);
}
n_result n_channel_receive_from(n_handle h, void *buf, size_t cap, int flags, struct n_channel_message *message) {
    return NC5(SYS_CHANNEL_RECEIVE_FROM, h, buf, cap, flags, message);
}
static n_handle launch_program(const char *path, const struct n_launch *launch) {
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

/* 起動の属性（struct n_start）。今のフォルダーはこのアプリが n_folder_set で変える。一度渡したフォルダーの文字列は
   アプリの寿命の間残るので、別のスレッドが持つ指は消えない。 */
static const struct n_start no_start = {.environment = (char *const[]){NULL}};
static const struct n_start *start_info = &no_start;
static const char *own_folder(void);

/* 子は起動元の今のフォルダーを受ける。起動元が持たなければ子も持たない：身元の利用者のフォルダーを
   引くのは accounts への接続で、サービスの管理者は accounts を起こす前にここを通る。 */
n_handle n_app_launch(const char *path, const struct n_launch *launch) {
    if (!launch) return launch_program(path, launch);
    struct n_launch l = *launch;
    if (!l.folder) l.folder = own_folder();
    return launch_program(path, &l);
}

n_result n_app_result(n_handle app, uint64_t deadline, int *status) {
    return NC5(SYS_APP_RESULT, app, deadline, status, 0, 0);
}
n_result n_app_stop(n_handle app, int flags) { return NC5(SYS_APP_STOP, app, flags, 0, 0, 0); }
n_result n_random(void *buf, size_t n) { return NC5(SYS_RANDOM, buf, n, 0, 0, 0); }
n_result n_app_self(void) { return NC5(SYS_APP_SELF, 0, 0, 0, 0, 0); }
n_result n_app_parent(void) { return NC5(SYS_APP_PARENT, 0, 0, 0, 0, 0); }
n_result n_system_log(char *buf, size_t cap) { return NC5(SYS_SYSTEM_LOG, buf, cap, 0, 0, 0); }
n_handle n_system_log_open(void) { return (n_handle)NC5(SYS_SYSTEM_LOG_OPEN, 0, 0, 0, 0, 0); }
n_handle n_app_open(int64_t id) { return (n_handle)NC5(SYS_APP_OPEN, id, 0, 0, 0, 0); }
n_result n_app_id(n_handle app) { return NC5(SYS_APP_ID, app, 0, 0, 0, 0); }
n_handle n_pci_claim(n_handle cap, uint8_t bus, uint8_t dev, uint8_t func) { return (n_handle)NC5(SYS_PCI_CLAIM, cap, bus, dev, func, 0); }
n_result n_pci_info(n_handle pci, struct n_pci_function *out) { return NC5(SYS_PCI_INFO, pci, out, 0, 0, 0); }
n_result n_pci_config_read(n_handle pci, uint32_t target, uint32_t offset, uint32_t width) {
    return NC5(SYS_PCI_CONFIG_READ, pci, target, offset, width, 0);
}
n_result n_pci_config_write(n_handle pci, uint32_t offset, uint32_t width, uint32_t value) {
    return NC5(SYS_PCI_CONFIG_WRITE, pci, offset, width, value, 0);
}
n_handle n_pci_bar(n_handle pci, uint32_t bar) { return (n_handle)NC5(SYS_PCI_BAR, pci, bar, 0, 0, 0); }
n_result n_pci_interrupts(n_handle pci, uint32_t count) { return NC5(SYS_PCI_INTERRUPTS, pci, count, 0, 0, 0); }
n_result n_pci_interrupt_take(n_handle pci, uint64_t *bits, uint32_t words, uint64_t deadline) {
    return NC5(SYS_PCI_INTERRUPT_TAKE, pci, deadline, bits, words, 0);
}
n_result n_pci_hold_screen(n_handle pci, bool hold) { return NC5(SYS_PCI_HOLD_SCREEN, pci, hold ? 1 : 0, 0, 0, 0); }
n_result n_pci_give_screen(n_handle pci, uint32_t output, const struct n_screen_buffers *buffers) {
    return NC5(SYS_PCI_GIVE_SCREEN, pci, output, buffers, 0, 0);
}
n_result n_pci_screen_flips(n_handle pci, struct n_screen_flip *out, size_t max, uint64_t deadline) {
    return NC5(SYS_PCI_SCREEN_FLIPS, pci, out, max, deadline, 0);
}
n_result n_pci_screen_vblank(n_handle pci, const struct n_screen_vblank *vblank) {
    return NC5(SYS_PCI_SCREEN_VBLANK, pci, vblank, 0, 0, 0);
}
n_result n_pci_screen_modes(n_handle pci, uint32_t output, const struct n_screen_modes *modes) {
    return NC5(SYS_PCI_SCREEN_MODES, pci, output, modes, 0, 0);
}
n_result n_pci_screen_state(n_handle pci, uint32_t output, uint64_t seen, struct n_screen_state *out,
                            uint64_t deadline) {
    return NC5(SYS_PCI_SCREEN_STATE, pci, output, seen, out, deadline);
}
n_result n_pci_screen_image(n_handle pci, uint32_t output, n_handle surface, const struct n_scanout *desc) {
    return NC5(SYS_PCI_SCREEN_IMAGE, pci, output, surface, desc, 0);
}
n_result n_pci_screen_image_end(n_handle pci, n_result image) {
    return NC5(SYS_PCI_SCREEN_IMAGE_END, pci, image, 0, 0, 0);
}
n_result n_pci_acpi_eval(n_handle pci, uint32_t node, const char *name, const struct n_acpi_value *args,
                         uint32_t nargs, const void *data, size_t data_size, void *out, size_t out_size) {
    struct n_acpi_eval e = {node, nargs, name, args, data, data_size};
    return NC5(SYS_PCI_ACPI_EVAL, pci, &e, out, out_size, 0);
}
n_result n_pci_acpi_notify_take(n_handle pci, uint64_t deadline) {
    return NC5(SYS_PCI_ACPI_NOTIFY_TAKE, pci, deadline, 0, 0, 0);
}
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
n_handle n_pager_create(size_t bytes, n_handle *memory) {
    return (n_handle)NC5(SYS_PAGER_CREATE, bytes, memory, 0, 0, 0);
}
n_result n_pager_take(n_handle pager, struct n_pager_fault *out, size_t max, uint64_t deadline) {
    return NC5(SYS_PAGER_TAKE, pager, out, max, deadline, 0);
}
n_result n_pager_supply(n_handle pager, const struct n_pager_supply *runs, size_t count) {
    return NC5(SYS_PAGER_SUPPLY, pager, runs, count, 0, 0);
}
n_result n_pager_revoke(n_handle pager, uint64_t offset, uint64_t bytes, uint32_t access) {
    return NC5(SYS_PAGER_REVOKE, pager, offset, bytes, access, 0);
}
n_result n_pager_fail(n_handle pager, uint64_t offset, uint64_t bytes) {
    return NC5(SYS_PAGER_FAIL, pager, offset, bytes, 0, 0);
}
n_result n_audio_device_list(n_handle cap, void *out, size_t bytes) { return NC5(SYS_AUDIO_DEVICE_LIST, cap, out, bytes, 0, 0); }
n_handle n_audio_device_open(n_handle cap, uint32_t id, const struct n_audio_device_format *format, struct n_audio_device_ring *ring) {
    return (n_handle)NC5(SYS_AUDIO_DEVICE_OPEN, cap, id, format, ring, 0);
}
n_result n_processes(struct n_procinfo **out) {
    for (size_t capacity = 256;; capacity *= 2) {
        struct n_procinfo *list = malloc(capacity * sizeof *list);
        if (!list) return -N_ERR_NO_MEMORY;
        n_result n = proclist(list, (int)capacity);
        if (n >= 0 && (size_t)n == capacity) {
            free(list);
            continue;
        }
        if (n < 0) free(list);
        else *out = list;
        return n;
    }
}
n_result n_audio_device_run(n_handle device, int run) { return NC5(SYS_AUDIO_DEVICE_RUN, device, run, 0, 0, 0); }
n_result n_audio_device_status(n_handle device, struct n_audio_device_status *out) { return NC5(SYS_AUDIO_DEVICE_STATUS, device, out, 0, 0, 0); }
n_handle n_audio_device_watch(n_handle cap) { return (n_handle)NC5(SYS_AUDIO_DEVICE_WATCH, cap, 0, 0, 0, 0); }
n_result n_settings_apply(n_handle cap, uint32_t domain, uint32_t key, const void *value, size_t size) {
    return NC5(SYS_SETTINGS_APPLY, cap, domain, key, value, size);
}
n_result n_settings_effective(uint32_t domain, uint32_t key, void *out, size_t cap) {
    return NC5(SYS_SETTINGS_EFFECTIVE, domain, key, out, cap, 0);
}
n_result n_settings_describe(uint32_t domain, uint32_t key, struct n_setting_desc *out) {
    return NC5(SYS_SETTINGS_DESCRIBE, domain, key, out, 0, 0);
}
n_handle n_settings_watch(uint32_t domain) { return (n_handle)NC5(SYS_SETTINGS_WATCH, domain, 0, 0, 0, 0); }
n_result n_entry_set_owner(const char *path, uint32_t user) { return NC5(SYS_ENTRY_SET_OWNER, path, user, 0, 0, 0); }
n_result n_watch_take(n_handle watch) { return NC5(SYS_WATCH_TAKE, watch, 0, 0, 0, 0); }
n_handle n_events_create(void) { return (n_handle)NC5(SYS_EVENTS_CREATE, 0, 0, 0, 0, 0); }
n_result n_events_watch(n_handle q, n_handle h, uint32_t interest, uint64_t token) {
    return NC5(SYS_EVENTS_WATCH, q, h, interest, token, 0);
}
n_result n_events_watch_address(n_handle q, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    return NC5(SYS_EVENTS_WATCH_ADDRESS, q, address, expected, token, 0);
}
n_result n_events_timer(n_handle q, uint64_t due, uint64_t period, uint64_t token) {
    return NC5(SYS_EVENTS_TIMER, q, due, period, token, 0);
}
n_result n_events_unwatch(n_handle q, uint64_t token) { return NC5(SYS_EVENTS_UNWATCH, q, token, 0, 0, 0); }
n_result n_events_wait(n_handle q, struct n_event *out, size_t max, uint64_t deadline) {
    return NC5(SYS_EVENTS_WAIT, q, out, max, deadline, 0);
}
n_result n_thread_schedule(int tid, uint32_t schedule, uint64_t value) {
    return NC5(SYS_THREAD_SCHEDULE, tid, schedule, value, 0, 0);
}
n_result proclist(struct n_procinfo *out, int max) { return SC2(SYS_PROCLIST, out, max); }
n_result sysinfo(struct n_sysinfo *si) { return SC1(SYS_SYSINFO, si); }
n_result cpu_info(struct n_cpuinfo *info) { return SC1(SYS_CPU_INFO, info); }
void yield(void) { SC0(SYS_YIELD); }
n_result poweroff(n_handle cap) { return NC5(SYS_POWER, cap, POWER_OFF, 0, 0, 0); }
n_result reboot(n_handle cap) { return NC5(SYS_POWER, cap, POWER_REBOOT, 0, 0, 0); }
n_result n_power_request(n_handle cap, uint32_t action) { return NC5(SYS_POWER, cap, action, 0, 0, 0); }
n_result n_power_events(n_handle cap, struct n_power_events *out) { return NC5(SYS_POWER_EVENTS, cap, out, sizeof *out, 0, 0); }
n_handle n_power_events_watch(n_handle cap) { return (n_handle)NC5(SYS_POWER_EVENTS_WATCH, cap, 0, 0, 0, 0); }
n_result n_power_windows(n_handle cap, bool ask_to_close, char *top_title, size_t size) {
    return NC5(SYS_POWER_WINDOWS, cap, ask_to_close, top_title, size, 0);
}
n_result n_power_say(n_handle cap, const struct n_power_state *state) { return NC5(SYS_POWER_SAY, cap, state, sizeof *state, 0, 0); }
n_handle n_power_windows_watch(n_handle cap) { return (n_handle)NC5(SYS_POWER_WINDOWS_WATCH, cap, 0, 0, 0, 0); }
n_result n_hal_list(struct n_hal_device *out, size_t max) { return SC2(SYS_HAL_LIST, out, max); }
n_handle n_hal_watch(void) { return (n_handle)SC0(SYS_HAL_WATCH); }

n_result n_hal_snapshot(struct n_hal_device **out) {
    *out = NULL;
    size_t capacity = 32;
    for (;;) {
        struct n_hal_device *grown = realloc(*out, capacity * sizeof **out);
        if (!grown) {
            free(*out);
            *out = NULL;
            return -N_ERR_NO_MEMORY;
        }
        *out = grown;
        n_result total = n_hal_list(*out, capacity);
        if (total < 0) {
            free(*out);
            *out = NULL;
        }
        if (total < 0 || (size_t)total <= capacity) return total;
        capacity = (size_t)total;
    }
}

n_result pcilist(struct n_pciinfo *out, int max) {
    struct n_hal_device *list;
    n_result total = n_hal_snapshot(&list);
    if (total < 0) return total;
    int n = 0;
    for (n_result i = 0; i < total && n < max; i++)
        if (list[i].bus == N_HAL_BUS_PCI) out[n++] = list[i].at.pci;
    free(list);
    return n;
}

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
    struct n_launch l = {.arguments = (const char *const *)argv, .handles = handles, .argument_count = argc,
                         .handle_count = 3, .terminals = start_info->terminals, .terminal_count = start_info->terminal_count};
    n_handle app = n_app_launch(full, &l);
    if (app < 0) return (int)app;
    int st = 0;
    n_result r = n_app_result(app, N_CLOCK_NEVER, &st);
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

struct kept_folder {
    struct kept_folder *next;
    char path[];
};
static struct kept_folder *kept_folders;
static const char *current_folder;
static uint32_t folder_lock;

static const char *own_folder(void) {
    const char *f = __atomic_load_n(&current_folder, __ATOMIC_ACQUIRE);
    return f ? f : start_info->folder;
}
const char *n_folder(void) {
    const char *f = own_folder();
    return f ? f : n_user_folder();
}

static int folder_keep(const char *path) {
    while (__atomic_exchange_n(&folder_lock, 1, __ATOMIC_ACQUIRE))
        if (wait_on_address(&folder_lock, 1, UINT32_MAX) < 0) yield();
    int r = -1;
    struct kept_folder *k = kept_folders;
    while (k && strcmp(k->path, path)) k = k->next;
    if (!k && (k = malloc(sizeof *k + strlen(path) + 1))) {
        strcpy(k->path, path);
        k->next = kept_folders;
        kept_folders = k;
    }
    if (k) {
        __atomic_store_n(&current_folder, k->path, __ATOMIC_RELEASE);
        r = 0;
    }
    __atomic_store_n(&folder_lock, 0, __ATOMIC_RELEASE);
    wake_address(&folder_lock, 1);
    return r;
}

/* 移植のプログラムの環境。libc の getenv と、移植の標準ライブラリーだけが読む。 */
char *const *__libc_environment = (char *const[]){NULL};

char *getenv(const char *name) {
    size_t len = strlen(name);
    for (char *const *s = __libc_environment; *s; s++)
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
    const char *folder = own_folder();
    /* 持たなければ利用者のフォルダーで始まる。完全なパスには引かない：引くのは接続だから。 */
    if (!folder) folder = n_path_is_complete(name) ? "C:\\" : n_user_folder();
    return n_path_join(folder, name, out, cap);
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
    return folder_keep(full) < 0 ? -N_ERR_NO_MEMORY : 0;
}

bool n_terminal(n_handle h) {
    for (uint32_t i = 0; i < start_info->terminal_count; i++)
        if (start_info->terminals[i] == h) return true;
    return false;
}

n_handle n_interrupts(void) { return start_info->interrupts ? start_info->interrupts : -N_ERR_NOT_FOUND; }

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
        if (wait_on_address(&exit_callbacks_lock, 1, N_CLOCK_NEVER) < 0) yield();
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

/* A driver image's libc in a process that runs (driver_image.c): its first thread goes on with the block
   of the libc that made it, and FS stays. */
void __libc_image_init(const struct n_start *start, const char *image, const char *image_end, size_t memsz, size_t align) {
    __asm__ volatile("movq %%fs:%c1, %0" : "=r"(entry_image) : "i"(__builtin_offsetof(struct n_thread_block, entry)));
    start_info = start;
    if (start->environment) __libc_environment = start->environment;
    tls_image = image;
    tls_image_end = image_end;
    tls_memsz = memsz;
    tls_align = align;
    tls_initialized = 1;
    __stdio_init();
}
const struct n_start *__libc_start_info(void) { return start_info; }

void __libc_init(void) {
    /* `tcc -run` links a libc of its own into tcc's process and starts here: tcc's block has the image. */
    if (!entry_image) __asm__ volatile("movq %%fs:%c1, %0" : "=r"(entry_image) : "i"(__builtin_offsetof(struct n_thread_block, entry)));
    tls_image = __tls_image;
    tls_image_end = __tls_image_end;
    tls_memsz = (size_t)__tls_memsz;
    tls_align = (size_t)__tls_align;
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
void __libc_start(int argc, char **argv, const struct n_start *start) {
    if (start) {
        entry_image = start->entry;
        start_info = start;
        if (start->environment) __libc_environment = start->environment;
    }
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
n_result n_image_map(n_handle document, struct n_image *out) { return SC2(SYS_IMAGE_MAP, document, out); }
n_result n_memory_reserve(size_t bytes, int access, void **memory) {
    long r = SC2(SYS_MEMORY_RESERVE, bytes, access);
    *memory = r < 0 ? NULL : (void *)r;
    return r < 0 ? r : 0;
}
n_result n_memory_release(void *addr, size_t bytes) { return SC2(SYS_MEMORY_RELEASE, addr, bytes); }
n_result n_memory_protect(void *addr, size_t bytes, int access) {
    return SC3(SYS_MEMORY_PROTECT, addr, bytes, access);
}
