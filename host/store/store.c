/* The build host's tool for Nocturne's volume (common/store_format.h). It keeps the volume with the
   store (common/store), the code the kernel's C: runs on, and gives the store what a program that
   keeps a volume gives it (store/volume.h): this tool's memory, a heap that guards every block; its
   medium, a disk image, a disk, or memory in the tests; and its blocks in memory, a cache of its
   own. It is a Windows program: its files, folders and arguments are Windows' own, their names in
   UTF-8 inside as names on the volume are. Over a disk image:

     store disk <image> <MiB> <ESP MiB>      a new GPT disk: EFI system and Nocturne partitions
     store esp <image>                       the EFI system partition's byte offset and its
                                             512-byte sectors
     store format <image> <label>            a new empty volume
     store folder <image> <C:\path> [--system] [--private]
     store put <image> <C:\path> <host path> [--system]
                                             a host file as a document, or a host folder and
                                             everything in it
     store fill <image> <list>               many puts, folders and removals (scripts/layout.py
                                             writes the list)
     store remove <image> <C:\path>          the entry and everything in it; nothing when it is
                                             not there
     store list <image> <C:\path>
     store get <image> <C:\path> <host file>
     store check <image>                     everything the catalog names against the space map,
                                             every content block against its check
     store test <MiB> [seed] [steps]         random changes, crashes, cut-short commits and damaged
                                             superblocks against a model of what the volume holds
     store stress <MiB> [seed] [threads] [steps]
                                             threads (as many as the CPUs unless told) changing the
                                             volume at once, each in a folder of its own, with
                                             commits between, against their models
     store tree-test <seed> [steps]          the catalog alone, grown to many leaves and branches,
                                             against a model of its keys

   <image> is a disk with a GPT (the volume is its Nocturne partition) or a bare volume: an image
   file, or on Windows a disk itself, \\.\PhysicalDriveN (as an administrator), such as the USB
   stick a machine ran from. list, get and check only read it. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "store/namespace.h"
#include "store/space.h"

/* ---- small things ---- */

static uint64_t random_state;

static uint64_t random_next(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12, x ^= x << 25, x ^= x >> 27;
    *state = x;
    return x * 0x2545f4914f6cdd1dULL;
}

static uint64_t random64(void) { return random_next(&random_state); }

static uint64_t below(uint64_t n) { return n ? random64() % n : 0; }

static void random_bytes(void *out, size_t n) {
    uint8_t *p = out;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t x = random64();
        memcpy(p + i, &x, STORE_MIN((size_t)8, n - i));
    }
}

static uint64_t be64(const uint8_t *p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) x = x << 8 | p[i];
    return x;
}

static const char *error_name(int r) {
    static _Thread_local char text[32];
    snprintf(text, sizeof text, "error %d", r);
    return text;
}

/* nanoseconds since 1970 UTC */
static int64_t host_now(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime; /* 100 ns since 1601 */
    return (int64_t)(t - 116444736000000000ULL) * 100;
}

/* What a test under way says of itself before anything that goes wrong in it ("" for none). */
static const char *test_context(void);

/* ---- memory ---- */

/* Every block has a head before it and a guard after it, and the heap knows every block: a write
   past one is found when it is freed or when the test looks at all of them (heap_check). A freed
   block waits a while, filled with a pattern, so that a write into it after its end is found too.
   heap_lock guards the lists and counts. */
struct guard {
    struct guard *prev, *next;
    size_t n;
    uint64_t magic;
};
#define GUARD_MAGIC 0x4e6f6374726e6521ULL
#define FREED_MAGIC 0x6465657266206e21ULL
#define GUARD_TAIL 16u
#define FREED_BYTES (64u << 20) /* freed blocks kept back */

static SRWLOCK heap_lock = SRWLOCK_INIT;
static struct guard heap = {&heap, &heap, 0, 0}, freed = {&freed, &freed, 0, 0};
static size_t freed_bytes;
static long alloc_fail_after = -1; /* blocks still given before memory runs out (-1: no end) */

static _Noreturn void heap_broken(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fprintf(stderr, "%sheap: ", test_context());
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
    exit(2);
}

static void guard_check(const struct guard *g) {
    if (g->magic != GUARD_MAGIC) heap_broken("the head of a block is overwritten");
    const uint8_t *tail = (const uint8_t *)(g + 1) + g->n;
    for (unsigned i = 0; i < GUARD_TAIL; i++)
        if (tail[i] != 0xa5) heap_broken("something wrote past a block of %zu bytes", g->n);
}

static void freed_check(const struct guard *g) {
    const uint8_t *p = (const uint8_t *)(g + 1);
    if (g->magic != FREED_MAGIC) heap_broken("the head of a freed block is overwritten");
    for (size_t i = 0; i < g->n + GUARD_TAIL; i++)
        if (p[i] != 0xdd) heap_broken("something wrote at %zu of a freed block of %zu bytes", i, g->n);
}

static void heap_check(void) {
    AcquireSRWLockExclusive(&heap_lock);
    for (struct guard *g = heap.next; g != &heap; g = g->next) guard_check(g);
    for (struct guard *g = freed.next; g != &freed; g = g->next) freed_check(g);
    ReleaseSRWLockExclusive(&heap_lock);
}

static void *heap_alloc(size_t n) {
    struct guard *g = NULL;
    AcquireSRWLockExclusive(&heap_lock);
    if (alloc_fail_after) {
        if (alloc_fail_after > 0) alloc_fail_after--;
        g = malloc(sizeof *g + n + GUARD_TAIL);
    }
    if (g) {
        *g = (struct guard){&heap, heap.next, n, GUARD_MAGIC};
        heap.next->prev = g;
        heap.next = g;
    }
    ReleaseSRWLockExclusive(&heap_lock);
    if (!g) return NULL;
    memset(g + 1, 0xcd, n);
    memset((uint8_t *)(g + 1) + n, 0xa5, GUARD_TAIL);
    return g + 1;
}

static void *heap_zalloc(size_t n) {
    void *p = heap_alloc(n);
    if (p) memset(p, 0, n);
    return p;
}

static void heap_free(void *p) {
    if (!p) return;
    struct guard *g = (struct guard *)p - 1, *gone = NULL;
    AcquireSRWLockExclusive(&heap_lock);
    if (g->magic == FREED_MAGIC) heap_broken("a block of %zu bytes is freed twice", g->n);
    guard_check(g);
    g->prev->next = g->next;
    g->next->prev = g->prev;
    /* to the end of the freed list; the oldest ones out */
    memset(g + 1, 0xdd, g->n + GUARD_TAIL);
    *g = (struct guard){freed.prev, &freed, g->n, FREED_MAGIC};
    freed.prev->next = g;
    freed.prev = g;
    freed_bytes += g->n;
    while (freed_bytes > FREED_BYTES) {
        struct guard *old = freed.next;
        freed_check(old);
        freed.next = old->next;
        old->next->prev = &freed;
        freed_bytes -= old->n;
        old->next = gone;
        gone = old;
    }
    ReleaseSRWLockExclusive(&heap_lock);
    for (struct guard *next; gone; gone = next) {
        next = gone->next;
        free(gone);
    }
}

static void *heap_realloc(void *p, size_t n) {
    void *q = heap_alloc(n);
    if (q && p) {
        memcpy(q, p, STORE_MIN(n, ((struct guard *)p - 1)->n));
        heap_free(p);
    }
    return q;
}

/* ---- what the store tells ---- */

static bool test_running;
static long troubles[STORE_TROUBLE_RULE + 1]; /* what the store told while a test was under way */

/* A command tells everything. A test counts what it is told, crashes and damage being what it
   makes happen, and fails at a rule the store broke. */
static void trouble(enum store_trouble t, uint64_t block, int error) {
    if (test_running && t != STORE_TROUBLE_RULE) {
        __atomic_fetch_add(&troubles[t], 1, __ATOMIC_RELAXED);
        return;
    }
    fprintf(stderr, "%sstore: %s (block %llu, %s)\n", test_context(), store_trouble_text(t), (unsigned long long)block,
            error_name(error));
    if (test_running) exit(1);
}

static const struct store_env env = {heap_alloc, heap_realloc, heap_free, trouble};

/* ---- blocks in memory ---- */

/* The volume's blocks in memory (struct store_supply), for any number of threads: one slim
   reader/writer lock, taken alone for a few steps at a time, guards the hash, the list of blocks
   nothing holds (the oldest of which go past limit) and each block's count. Reads from the medium
   are made with it let go: two threads that miss the same block may both read it, and the first to
   come back puts it in the hash, which the other takes instead of its own. Memory comes from the
   tool's heap, a lock taken after this one. */
struct cblock {
    struct store_block b; /* the store's: first */
    uint32_t refs;
    bool hashed;          /* until it is forgotten or discarded: then it goes with its last put */
    struct cblock *hash_next, *older, *newer; /* the hash chain; the list of blocks nothing holds */
};

struct cache {
    struct store_supply supply; /* first */
    struct store_device *dev;
    SRWLOCK lock;               /* everything below, and each block's refs, hashed and links */
    struct cblock **hash;
    size_t hash_size, hashed, unheld, limit;
    struct cblock *oldest, *newest;
};

#define STAGE_BLOCKS 64u  /* blocks read in one request */
#define FIRST_BUCKETS 1024u
#define CACHE_IMAGE 16384u /* blocks nothing holds that a command keeps */
#define CACHE_TEST 256u    /* that a test keeps: few, so that blocks go and are read again */

static struct cache *cache_of(struct store_supply *s) { return (struct cache *)s; }
static struct cblock *block_of(struct store_block *b) { return (struct cblock *)b; }

static size_t hash_of(size_t size, uint64_t address) {
    return (size_t)((address * 0x9E3779B97F4A7C15ULL) >> 32) & (size - 1);
}

static struct cblock *lookup(struct cache *c, uint64_t address) {
    for (struct cblock *b = c->hash[hash_of(c->hash_size, address)]; b; b = b->hash_next)
        if (b->b.address == address) return b;
    return NULL;
}

/* Twice the buckets once there are twice as many blocks; without memory, the chains grow longer. */
static void hash_in(struct cache *c, struct cblock *b) {
    struct cblock **hash = c->hashed > 2 * c->hash_size ? heap_zalloc(2 * c->hash_size * sizeof *hash) : NULL;
    if (hash) {
        for (size_t i = 0; i < c->hash_size; i++)
            for (struct cblock *k = c->hash[i], *next; k; k = next) {
                next = k->hash_next;
                struct cblock **at = &hash[hash_of(2 * c->hash_size, k->b.address)];
                k->hash_next = *at;
                *at = k;
            }
        heap_free(c->hash);
        c->hash = hash;
        c->hash_size *= 2;
    }
    struct cblock **at = &c->hash[hash_of(c->hash_size, b->b.address)];
    b->hash_next = *at;
    *at = b;
    b->hashed = true;
    c->hashed++;
}

static void hash_out(struct cache *c, struct cblock *b) {
    struct cblock **at = &c->hash[hash_of(c->hash_size, b->b.address)];
    while (*at != b) at = &(*at)->hash_next;
    *at = b->hash_next;
    b->hash_next = NULL;
    b->hashed = false;
    c->hashed--;
}

static void unheld_out(struct cache *c, struct cblock *b) {
    if (b->older) b->older->newer = b->newer;
    else c->oldest = b->newer;
    if (b->newer) b->newer->older = b->older;
    else c->newest = b->older;
    b->older = b->newer = NULL;
    c->unheld--;
}

static void block_free(struct cblock *b) {
    heap_free(b->b.data);
    heap_free(b);
}

/* data: STORE_BLOCK bytes the block takes (NULL: zeros of its own) */
static struct cblock *block_new(uint64_t address, uint8_t *data) {
    struct cblock *b = heap_zalloc(sizeof *b);
    if (!b) return NULL;
    if (!data && !(data = heap_zalloc(STORE_BLOCK))) {
        heap_free(b);
        return NULL;
    }
    b->b.address = address;
    b->b.data = data;
    b->refs = 1;
    return b;
}

static void hold(struct cache *c, struct cblock *b) {
    if (!b->refs++) unheld_out(c, b);
}

/* b out of the hash: it goes now when nothing holds it, else with its last put */
static void forget_one(struct cache *c, struct cblock *b) {
    hash_out(c, b);
    if (b->refs) return;
    unheld_out(c, b);
    block_free(b);
}

static void let_go(struct cache *c, struct cblock *b) {
    if (--b->refs) return;
    if (!b->hashed) {
        block_free(b);
        return;
    }
    b->older = c->newest;
    b->newer = NULL;
    if (c->newest) c->newest->newer = b;
    else c->oldest = b;
    c->newest = b;
    c->unheld++;
    while (c->unheld > c->limit) {
        struct cblock *old = c->oldest;
        unheld_out(c, old);
        hash_out(c, old);
        block_free(old);
    }
}

static int device_read(struct cache *c, uint64_t block, uint32_t count, void *buf) {
    if (block + count > c->dev->blocks || block + count < block) return -N_ERR_IO;
    return c->dev->read(c->dev, block, count, buf);
}

/* fresh, read whole, into the hash; or what another put there meanwhile, held, fresh freed */
static struct cblock *settle(struct cache *c, struct cblock *fresh) {
    AcquireSRWLockExclusive(&c->lock);
    struct cblock *b = lookup(c, fresh->b.address);
    if (b) hold(c, b);
    else hash_in(c, fresh);
    ReleaseSRWLockExclusive(&c->lock);
    if (!b) return fresh;
    block_free(fresh);
    return b;
}

static struct store_block *cache_find(struct store_supply *s, uint64_t address) {
    struct cache *c = cache_of(s);
    AcquireSRWLockExclusive(&c->lock);
    struct cblock *b = lookup(c, address);
    if (b) hold(c, b);
    ReleaseSRWLockExclusive(&c->lock);
    return b ? &b->b : NULL;
}

static void cache_put(struct store_supply *s, struct store_block *sb) {
    struct cache *c = cache_of(s);
    AcquireSRWLockExclusive(&c->lock);
    let_go(c, block_of(sb));
    ReleaseSRWLockExclusive(&c->lock);
}

static struct store_block *cache_read(struct store_supply *s, struct volume *v, uint64_t address, store_block_check *valid,
                                      uint64_t expect, int *error) {
    struct cache *c = cache_of(s);
    struct store_block *there = cache_find(s, address);
    if (there) return there;
    struct cblock *fresh = block_new(address, NULL);
    if (!fresh) {
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    int r = device_read(c, address, 1, fresh->b.data);
    if (r >= 0 && valid && !valid(v, fresh->b.data, address, expect)) r = -N_ERR_IO;
    if (r < 0) {
        block_free(fresh);
        *error = r;
        return NULL;
    }
    return &settle(c, fresh)->b;
}

static int cache_read_many(struct store_supply *s, struct volume *v, uint64_t start, uint32_t count,
                           store_block_check *valid, const uint32_t *expect, struct store_block **out) {
    struct cache *c = cache_of(s);
    uint8_t *stage = NULL;
    int r = 0;
    for (uint32_t k = 0; k < count; k++) out[k] = NULL;
    for (uint32_t i = 0; i < count && r >= 0;) {
        if ((out[i] = cache_find(s, start + i))) {
            i++;
            continue;
        }
        /* the blocks from i on that memory does not have, in one read; one it has ends them */
        uint32_t n = 1;
        struct store_block *after = NULL;
        while (i + n < count && n < STAGE_BLOCKS && !(after = cache_find(s, start + i + n))) n++;
        if (!stage && !(stage = heap_alloc(STAGE_BLOCKS * STORE_BLOCK))) r = -N_ERR_NO_MEMORY;
        if (r >= 0) r = device_read(c, start + i, n, stage);
        for (uint32_t k = 0; k < n && r >= 0; k++) {
            uint64_t address = start + i + k;
            struct cblock *fresh = block_new(address, NULL);
            if (!fresh) {
                r = -N_ERR_NO_MEMORY;
                break;
            }
            memcpy(fresh->b.data, stage + (size_t)k * STORE_BLOCK, STORE_BLOCK);
            if (valid && !valid(v, fresh->b.data, address, expect ? expect[i + k] : 0)) {
                block_free(fresh);
                r = -N_ERR_IO;
                break;
            }
            out[i + k] = &settle(c, fresh)->b;
        }
        if (after) out[i + n] = after;
        i += n + (after != NULL);
    }
    heap_free(stage);
    if (r < 0)
        for (uint32_t k = 0; k < count; k++) {
            if (out[k]) cache_put(s, out[k]);
            out[k] = NULL;
        }
    return r;
}

static struct store_block *cache_make(struct store_supply *s, uint64_t address, uint8_t *data, int *error) {
    struct cache *c = cache_of(s);
    struct cblock *b = block_new(address, data);
    if (!b) {
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    AcquireSRWLockExclusive(&c->lock);
    struct cblock *old = lookup(c, address);
    if (old) forget_one(c, old);
    hash_in(c, b);
    ReleaseSRWLockExclusive(&c->lock);
    return &b->b;
}

static void cache_rename(struct store_supply *s, struct store_block *sb, uint64_t address) {
    struct cache *c = cache_of(s);
    struct cblock *b = block_of(sb);
    AcquireSRWLockExclusive(&c->lock);
    struct cblock *old = lookup(c, address);
    if (old && old != b) forget_one(c, old);
    bool hashed = b->hashed;
    if (hashed) hash_out(c, b);
    b->b.address = address;
    if (hashed) hash_in(c, b);
    ReleaseSRWLockExclusive(&c->lock);
}

static void cache_discard(struct store_supply *s, struct store_block *sb) {
    struct cache *c = cache_of(s);
    struct cblock *b = block_of(sb);
    AcquireSRWLockExclusive(&c->lock);
    if (b->hashed) hash_out(c, b);
    ReleaseSRWLockExclusive(&c->lock);
}

static void cache_forget(struct store_supply *s, uint64_t start, uint64_t count) {
    struct cache *c = cache_of(s);
    AcquireSRWLockExclusive(&c->lock);
    if (count > c->hashed) { /* fewer blocks in memory than in the range: look at each of them */
        for (size_t i = 0; i < c->hash_size; i++)
            for (struct cblock *b = c->hash[i], *next; b; b = next) {
                next = b->hash_next;
                if (b->b.address >= start && b->b.address - start < count) forget_one(c, b);
            }
    } else {
        for (uint64_t i = 0; i < count; i++) {
            struct cblock *b = lookup(c, start + i);
            if (b) forget_one(c, b);
        }
    }
    ReleaseSRWLockExclusive(&c->lock);
}

/* limit: how many blocks that nothing holds stay in memory */
static struct store_supply *cache_create(struct store_device *dev, size_t limit) {
    struct cache *c = heap_zalloc(sizeof *c);
    if (!c || !(c->hash = heap_zalloc(FIRST_BUCKETS * sizeof *c->hash))) {
        heap_free(c);
        return NULL;
    }
    c->supply = (struct store_supply){
        .find = cache_find,
        .read = cache_read,
        .read_many = cache_read_many,
        .make = cache_make,
        .put = cache_put,
        .rename = cache_rename,
        .discard = cache_discard,
        .forget = cache_forget,
    };
    c->dev = dev;
    InitializeSRWLock(&c->lock);
    c->hash_size = FIRST_BUCKETS;
    c->limit = limit;
    return &c->supply;
}

/* Every block goes, once nothing holds any (after volume_free): one still held is the store's
   leak. */
static void cache_free(struct store_supply *s) {
    struct cache *c = cache_of(s);
    for (size_t i = 0; i < c->hash_size; i++)
        for (struct cblock *b = c->hash[i], *next; b; b = next) {
            next = b->hash_next;
            if (b->refs) heap_broken("the store still holds block %llu", (unsigned long long)b->b.address);
            block_free(b);
        }
    heap_free(c->hash);
    heap_free(c);
}

/* ---- files on the build host: Windows' own calls, with paths in UTF-8 ---- */

/* text as Windows takes it (UTF-16), malloc'd; NULL when it is not UTF-8 */
static wchar_t *wide(const char *text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, w, n) != n) {
        free(w);
        w = NULL;
    }
    if (!w) SetLastError(ERROR_NO_UNICODE_TRANSLATION);
    return w;
}

/* Windows' text as UTF-8, malloc'd; NULL when it is not whole UTF-16 */
static char *narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, NULL, 0, NULL, NULL);
    char *text = n > 0 ? malloc((size_t)n) : NULL;
    if (text && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, text, n, NULL, NULL) != n) {
        free(text);
        text = NULL;
    }
    return text;
}

/* what Windows said of the last call about path */
static void host_error(const char *path) {
    DWORD e = GetLastError();
    char text[256];
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, text, sizeof text,
                        NULL))
        snprintf(text, sizeof text, "Windows error %lu", (unsigned long)e);
    text[strcspn(text, "\r\n")] = 0;
    fprintf(stderr, "store: %s: %s\n", path, text);
}

enum host_open { HOST_READ, HOST_WRITE, HOST_NEW };

/* A file, or a disk itself (\\.\PhysicalDriveN), others may use as well; INVALID_HANDLE_VALUE
   (told on stderr) when it cannot be opened. HOST_NEW makes the file empty. */
static HANDLE host_open(const char *path, enum host_open how) {
    wchar_t *w = wide(path);
    HANDLE h = INVALID_HANDLE_VALUE;
    if (w)
        h = CreateFileW(w, how == HOST_READ ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, how == HOST_NEW ? CREATE_ALWAYS : OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) host_error(path);
    return h;
}

/* n bytes at offset into buf: how many there were (fewer only at the file's end), or -1 */
static int64_t host_read_at(HANDLE h, uint64_t offset, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        OVERLAPPED at = {.Offset = (DWORD)(offset + done), .OffsetHigh = (DWORD)((offset + done) >> 32)};
        DWORD want = n - done > (1u << 30) ? 1u << 30 : (DWORD)(n - done), got = 0;
        if (!ReadFile(h, (uint8_t *)buf + done, want, &got, &at)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return -1;
        }
        if (!got) break;
        done += got;
    }
    return (int64_t)done;
}

static bool host_write_at(HANDLE h, uint64_t offset, const void *buf, size_t n) {
    for (size_t done = 0; done < n;) {
        OVERLAPPED at = {.Offset = (DWORD)(offset + done), .OffsetHigh = (DWORD)((offset + done) >> 32)};
        DWORD want = n - done > (1u << 30) ? 1u << 30 : (DWORD)(n - done), put = 0;
        if (!WriteFile(h, (const uint8_t *)buf + done, want, &put, &at) || !put) return false;
        done += put;
    }
    return true;
}

/* A file's bytes, a disk's by its own request; -1 when neither tells */
static int64_t host_size(HANDLE h) {
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0) return size.QuadPart;
    GET_LENGTH_INFORMATION disk;
    DWORD k;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &disk, sizeof disk, &k, NULL))
        return disk.Length.QuadPart;
    return GetFileSizeEx(h, &size) ? size.QuadPart : -1;
}

/* ---- media ---- */

/* A disk image, or a disk: the volume from offset on. */
struct image {
    struct store_device dev; /* first */
    HANDLE h;
    uint64_t offset;
};

static struct image *image_of(struct store_device *d) { return (struct image *)d; }

static int image_read(struct store_device *d, uint64_t block, uint32_t count, void *buf) {
    struct image *im = image_of(d);
    if (block + count > d->blocks) return -N_ERR_INVALID;
    size_t n = (size_t)count * STORE_BLOCK;
    int64_t got = host_read_at(im->h, im->offset + block * STORE_BLOCK, buf, n);
    if (got < 0) return -N_ERR_IO;
    memset((uint8_t *)buf + got, 0, n - (size_t)got); /* past the end of a shorter file: zeros */
    return 0;
}

static int image_write(struct store_device *d, uint64_t block, uint32_t count, const void *buf) {
    struct image *im = image_of(d);
    if (block + count > d->blocks) return -N_ERR_INVALID;
    return host_write_at(im->h, im->offset + block * STORE_BLOCK, buf, (size_t)count * STORE_BLOCK) ? 0 : -N_ERR_IO;
}

static int image_persist(struct store_device *d) { return FlushFileBuffers(image_of(d)->h) ? 0 : -N_ERR_IO; }

/* Memory, for the tests: its power goes after fail_after more writes (-1: never). One thread writes
   at a time (the one that commits). */
struct memory {
    struct store_device dev; /* first */
    uint8_t *data;
    long fail_after;
};

static struct memory *memory_of(struct store_device *d) { return (struct memory *)d; }

static int memory_read(struct store_device *d, uint64_t block, uint32_t count, void *buf) {
    if (block + count > d->blocks) return -N_ERR_INVALID;
    memcpy(buf, memory_of(d)->data + block * STORE_BLOCK, (size_t)count * STORE_BLOCK);
    return 0;
}

static int memory_write(struct store_device *d, uint64_t block, uint32_t count, const void *buf) {
    struct memory *m = memory_of(d);
    if (block + count > d->blocks) return -N_ERR_INVALID;
    if (m->fail_after == 0) return -N_ERR_IO;
    if (m->fail_after > 0) m->fail_after--;
    memcpy(m->data + block * STORE_BLOCK, buf, (size_t)count * STORE_BLOCK);
    return 0;
}

/* size bytes of memory as a medium of blocks blocks; false without memory */
static bool memory_make(struct memory *m, uint64_t size, uint64_t blocks) {
    m->data = calloc(1, size);
    m->fail_after = -1;
    m->dev = (struct store_device){.blocks = blocks, .read = memory_read, .write = memory_write};
    return m->data != NULL;
}

/* ---- a volume on a medium ---- */

/* The volume on dev, its blocks in memory a cache of its own (limit: the blocks it keeps that
   nothing holds): a new one when label is given, else the one there; with its namespace unless
   bare (the catalog's test keeps the catalog alone). */
static int open_volume(struct store_device *dev, size_t limit, const char *label, bool bare, struct volume **v) {
    struct store_supply *blocks = cache_create(dev, limit);
    if (!blocks) return -N_ERR_NO_MEMORY;
    int r;
    if (label) {
        uint8_t id[16];
        random_bytes(id, sizeof id);
        int64_t now = host_now();
        if ((r = volume_format(&env, dev, blocks, label, id, now, v)) >= 0 && !bare && (r = ns_format(*v, now)) < 0)
            volume_free(*v);
    } else if ((r = volume_mount(&env, dev, blocks, v)) >= 0 && !bare && (r = ns_mount(*v)) < 0) {
        volume_free(*v);
    }
    if (r < 0) cache_free(blocks);
    return r;
}

/* The volume goes from memory, its blocks with it: what was not committed is gone. */
static void close_volume(struct volume *v) {
    struct store_supply *blocks = v->blocks;
    volume_free(v);
    cache_free(blocks);
}

/* ---- GPT ---- */

#define SECTOR 512u
#define GPT_ENTRIES 128u
#define GPT_ENTRY 128u
#define GPT_SECTORS (GPT_ENTRIES * GPT_ENTRY / SECTOR)

struct gpt_header {
    char signature[8];
    uint32_t revision, size, check, reserved;
    uint64_t self, other, first_usable, last_usable;
    uint8_t disk[16];
    uint64_t entries;
    uint32_t count, entry_size, entries_check;
};

struct gpt_entry {
    uint8_t type[16], unique[16];
    uint64_t first, last, attributes;
    uint16_t name[36];
};

/* the CRC-32 GPT uses (IEEE 802.3) */
static uint32_t crc32_ieee(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

static void random_guid(uint8_t g[16]) {
    random_bytes(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0f) | 0x40); /* version 4, in GPT's byte order */
    g[8] = (uint8_t)((g[8] & 0x3f) | 0x80);
}

static void gpt_name(struct gpt_entry *e, const char *name) {
    for (size_t i = 0; name[i] && i < 36; i++) e->name[i] = (uint8_t)name[i];
}

static const uint8_t esp_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static const uint8_t store_type[16] = STORE_PARTITION_TYPE;

static int disk_make(const char *path, uint64_t mib, uint64_t esp_mib) {
    uint64_t sectors = mib * 2048, esp = 2048, esp_end = esp + esp_mib * 2048; /* the ESP at 1 MiB, after the GPT */
    uint64_t store = esp_end, last_usable = sectors - 1 - 1 - GPT_SECTORS;
    if (!esp_mib || store + 64 * 2048 > last_usable) {
        fprintf(stderr, "store: %llu MiB has no room for a %llu MiB EFI system partition and a volume\n",
                (unsigned long long)mib, (unsigned long long)esp_mib);
        return 1;
    }
    uint64_t store_end = store + (last_usable + 1 - store) / 8 * 8; /* whole blocks */
    HANDLE f = host_open(path, HOST_NEW);
    if (f == INVALID_HANDLE_VALUE) return 1;
    struct gpt_entry *entries = calloc(GPT_ENTRIES, sizeof *entries);
    uint8_t *sector = calloc(1, SECTOR);
    struct gpt_header h = {.revision = 0x10000, .size = 92, .first_usable = 2 + GPT_SECTORS, .last_usable = last_usable,
                           .count = GPT_ENTRIES, .entry_size = GPT_ENTRY};
    memcpy(h.signature, "EFI PART", 8);
    random_guid(h.disk);
    struct { const uint8_t *type; uint64_t first, last; const char *name; } part[2] = {
        {esp_type, esp, esp_end - 1, "EFI system"},
        {store_type, store, store_end - 1, "Nocturne"},
    };
    for (int i = 0; i < 2; i++) {
        memcpy(entries[i].type, part[i].type, 16);
        random_guid(entries[i].unique);
        entries[i].first = part[i].first;
        entries[i].last = part[i].last;
        gpt_name(&entries[i], part[i].name);
    }
    h.entries_check = crc32_ieee(entries, GPT_ENTRIES * GPT_ENTRY);
    int bad = 0;
    /* the protective MBR: one partition of type 0xee over the disk */
    sector[446 + 1] = 0x00, sector[446 + 2] = 0x02, sector[446 + 3] = 0x00;
    sector[446 + 4] = 0xee;
    sector[446 + 5] = sector[446 + 6] = sector[446 + 7] = 0xff;
    uint32_t start = 1, size = sectors - 1 > 0xffffffffu ? 0xffffffffu : (uint32_t)(sectors - 1);
    memcpy(sector + 446 + 8, &start, 4);
    memcpy(sector + 446 + 12, &size, 4);
    sector[510] = 0x55, sector[511] = 0xaa;
    bad |= !host_write_at(f, 0, sector, SECTOR);
    /* the header and entries at the start, their copies at the end */
    for (int copy = 0; copy < 2; copy++) {
        struct gpt_header c = h;
        c.self = copy ? sectors - 1 : 1;
        c.other = copy ? 1 : sectors - 1;
        c.entries = copy ? sectors - 1 - GPT_SECTORS : 2;
        c.check = 0;
        memset(sector, 0, SECTOR);
        memcpy(sector, &c, sizeof c);
        c.check = crc32_ieee(sector, c.size);
        memcpy(sector, &c, sizeof c);
        bad |= !host_write_at(f, c.entries * SECTOR, entries, GPT_ENTRIES * GPT_ENTRY);
        bad |= !host_write_at(f, c.self * SECTOR, sector, SECTOR);
    }
    bad |= !CloseHandle(f);
    free(entries);
    free(sector);
    if (bad) {
        fprintf(stderr, "store: %s could not be written\n", path);
        return 1;
    }
    return 0;
}

/* The first and last sector of the partition of type in f's GPT: 1 when it is there, 0 when it is
   not, -1 when f has no GPT. The entries are read a whole sector at a time, as a disk itself
   (\\.\PhysicalDriveN) reads nothing smaller. */
static int gpt_partition(HANDLE f, const uint8_t type[16], uint64_t *first, uint64_t *last) {
    struct gpt_header h;
    uint8_t sector[SECTOR];
    if (host_read_at(f, SECTOR, sector, SECTOR) != SECTOR || memcmp(sector, "EFI PART", 8)) return -1;
    memcpy(&h, sector, sizeof h);
    if (h.entry_size < sizeof(struct gpt_entry) || SECTOR % h.entry_size) return -1;
    uint32_t per = SECTOR / h.entry_size;
    struct gpt_entry e;
    for (uint32_t i = 0; i < h.count && i < 1024; i++) {
        if (!(i % per) && host_read_at(f, (h.entries + i / per) * SECTOR, sector, SECTOR) != SECTOR) break;
        memcpy(&e, sector + i % per * h.entry_size, sizeof e);
        if (!memcmp(e.type, type, 16)) {
            *first = e.first, *last = e.last;
            return 1;
        }
    }
    return 0;
}

static int esp_show(const char *path) {
    HANDLE f = host_open(path, HOST_READ);
    uint64_t first, last;
    if (f == INVALID_HANDLE_VALUE) return 1;
    int found = gpt_partition(f, esp_type, &first, &last);
    CloseHandle(f);
    if (found <= 0) {
        fprintf(stderr, "store: %s has no EFI system partition\n", path);
        return 1;
    }
    printf("%llu %llu\n", (unsigned long long)(first * SECTOR), (unsigned long long)(last + 1 - first));
    return 0;
}

/* the volume in the image: its Nocturne partition, or all of it when it has no GPT. Opened to be
   read only unless write. Each read and write is the whole blocks asked for at their own place,
   which a disk itself needs; a disk's GPT tells where the volume is. */
static int image_open(const char *path, bool write, struct image *im) {
    memset(im, 0, sizeof *im);
    if ((im->h = host_open(path, write ? HOST_WRITE : HOST_READ)) == INVALID_HANDLE_VALUE) return -1;
    int64_t end = host_size(im->h);
    uint64_t bytes = end > 0 ? (uint64_t)end : 0, first = 0, last = bytes / SECTOR - 1;
    int found = gpt_partition(im->h, store_type, &first, &last);
    if (!found || (found < 0 && bytes < STORE_BLOCK)) {
        fprintf(stderr, "store: %s has no Nocturne partition\n", path);
        CloseHandle(im->h);
        return -1;
    }
    if ((first * SECTOR) % STORE_BLOCK) {
        fprintf(stderr, "store: the Nocturne partition of %s does not start at a 4 KiB boundary\n", path);
        CloseHandle(im->h);
        return -1;
    }
    im->offset = first * SECTOR;
    im->dev = (struct store_device){.blocks = (last + 1 - first) * SECTOR / STORE_BLOCK, .read = image_read,
                                    .write = image_write, .persist = image_persist};
    return 0;
}

/* ---- the namespace by path ---- */

/* the folder that holds the path's last name: made, with its parents, when it is not there */
static int parent_of(struct volume *v, const struct path *p, uint64_t *folder) {
    uint64_t at = STORE_ID_ROOT;
    for (size_t i = 0; i + 1 < p->count; i++) {
        struct ns_entry e;
        struct store_record rec;
        int r = ns_find(v, at, p->part[i].name, p->part[i].n, &e);
        if (r == -N_ERR_NOT_FOUND)
            r = ns_create(v, at, p->part[i].name, p->part[i].n, STORE_FOLDER, 0, 0, host_now(), &e.id, NULL);
        else if (r >= 0 && (r = ns_record(v, e.id, &rec, NULL)) >= 0 && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r < 0) return r;
        at = e.id;
    }
    *folder = at;
    return 0;
}

/* ---- putting host files on the volume ---- */

#define PUT_BLOCKS 256u      /* blocks written at a time */
#define COMMIT_BLOCKS 32768u /* written between commits: memory holds them until then */

struct put {
    struct volume *v;
    uint64_t documents, folders, bytes;
};

static int put_any(struct put *p, uint64_t folder, const char *name, size_t n, const char *host, uint16_t attributes);

static int put_document(struct put *p, uint64_t folder, const char *name, size_t n, const char *host,
                        uint16_t attributes) {
    struct volume *v = p->v;
    HANDLE f = host_open(host, HOST_READ);
    if (f == INVALID_HANDLE_VALUE) return -N_ERR_NOT_FOUND;
    int64_t length = host_size(f);
    if (length < 0) {
        host_error(host);
        CloseHandle(f);
        return -N_ERR_IO;
    }
    uint64_t size = (uint64_t)length, blocks = (size + STORE_BLOCK - 1) / STORE_BLOCK;
    struct ns_entry e;
    struct store_record rec;
    int r = ns_find(v, folder, name, n, &e);
    if (r == -N_ERR_NOT_FOUND) r = ns_create(v, folder, name, n, STORE_DOCUMENT, attributes, 0, host_now(), &e.id, NULL);
    else if (r >= 0 && (r = ns_record(v, e.id, &rec, NULL)) >= 0 && rec.kind != STORE_DOCUMENT) r = -N_ERR_IS_FOLDER;
    uint64_t done = 0;
    do {
        size_t k = (size_t)STORE_MIN((uint64_t)PUT_BLOCKS, blocks - done);
        struct ns_page *pages = calloc(k ? k : 1, sizeof *pages);
        if (!pages) {
            r = -N_ERR_NO_MEMORY;
            break;
        }
        for (size_t i = 0; i < k; i++) {
            pages[i].index = done + i;
            pages[i].data = heap_zalloc(STORE_BLOCK);
            if (r >= 0 && !pages[i].data) r = -N_ERR_NO_MEMORY;
            if (r >= 0 && host_read_at(f, (done + i) * STORE_BLOCK, pages[i].data, STORE_BLOCK) < 0) r = -N_ERR_IO;
        }
        if (r >= 0) r = space_reserve(v, k);
        if (r < 0) {
            for (size_t i = 0; i < k; i++) heap_free(pages[i].data);
            free(pages);
            break;
        }
        struct ranges dropped = {0};
        r = ns_update(v, e.id, size, done, pages, k, true, host_now(), &dropped, NULL); /* blocks before done stay */
        space_release_list(v, &dropped);
        free(pages);
        done += k;
        if (r >= 0 && volume_unwritten(v) >= COMMIT_BLOCKS) r = volume_commit(v);
    } while (r >= 0 && done < blocks);
    CloseHandle(f);
    if (r < 0) fprintf(stderr, "store: %s: %s\n", host, error_name(r));
    else p->documents++, p->bytes += size;
    return r;
}

static int put_folder(struct put *p, uint64_t folder, const char *name, size_t n, const char *host,
                      uint16_t attributes) {
    struct volume *v = p->v;
    uint64_t id = folder;
    int r = 0;
    if (name) { /* NULL: the host folder's contents go into folder itself */
        struct ns_entry e;
        struct store_record rec;
        r = ns_find(v, folder, name, n, &e);
        if (r == -N_ERR_NOT_FOUND) r = ns_create(v, folder, name, n, STORE_FOLDER, attributes, 0, host_now(), &e.id, NULL);
        else if (r >= 0 && (r = ns_record(v, e.id, &rec, NULL)) >= 0 && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r < 0) {
            fprintf(stderr, "store: %s: %s\n", host, error_name(r));
            return r;
        }
        id = e.id;
        p->folders++;
    }
    size_t hn = strlen(host);
    char *pattern = malloc(hn + 3);
    if (!pattern) return -N_ERR_NO_MEMORY;
    memcpy(pattern, host, hn);
    memcpy(pattern + hn, "\\*", 3);
    wchar_t *w = wide(pattern);
    free(pattern);
    WIN32_FIND_DATAW found;
    HANDLE find = w ? FindFirstFileW(w, &found) : INVALID_HANDLE_VALUE;
    free(w);
    if (find == INVALID_HANDLE_VALUE) {
        host_error(host);
        return -N_ERR_NOT_FOUND;
    }
    bool more = true;
    for (; r >= 0 && more; more = FindNextFileW(find, &found)) {
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        char *kid = narrow(found.cFileName), *child = kid ? malloc(hn + 1 + strlen(kid) + 1) : NULL;
        if (!child) {
            fprintf(stderr, "store: %s: a name in it is not Unicode\n", host);
            free(kid);
            r = -N_ERR_INVALID;
            break;
        }
        sprintf(child, "%s/%s", host, kid);
        r = found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? put_folder(p, id, kid, strlen(kid), child, attributes)
                                                             : put_document(p, id, kid, strlen(kid), child, attributes);
        free(child);
        free(kid);
    }
    if (r >= 0 && GetLastError() != ERROR_NO_MORE_FILES) {
        host_error(host);
        r = -N_ERR_IO;
    }
    FindClose(find);
    return r;
}

static int put_any(struct put *p, uint64_t folder, const char *name, size_t n, const char *host, uint16_t attributes) {
    wchar_t *w = wide(host);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    if (a == INVALID_FILE_ATTRIBUTES) {
        host_error(host);
        return -N_ERR_NOT_FOUND;
    }
    return a & FILE_ATTRIBUTE_DIRECTORY ? put_folder(p, folder, name, n, host, attributes)
                                        : put_document(p, folder, name, n, host, attributes);
}

/* The folder p names, made (with its parents) when missing, with attributes at least. */
static int folder_make(struct volume *v, const struct path *p, uint16_t attributes) {
    uint64_t folder;
    struct ns_entry e;
    struct store_record rec;
    if (!p->count) return -N_ERR_EXISTS;
    int r = parent_of(v, p, &folder);
    if (r < 0) return r;
    const char *name = p->part[p->count - 1].name;
    size_t n = p->part[p->count - 1].n;
    if ((r = ns_find(v, folder, name, n, &e)) == -N_ERR_NOT_FOUND)
        return ns_create(v, folder, name, n, STORE_FOLDER, attributes, 0, host_now(), &e.id, NULL);
    if (r < 0 || (r = ns_record(v, e.id, &rec, NULL)) < 0) return r;
    if (rec.kind != STORE_FOLDER) return -N_ERR_NOT_FOLDER;
    struct ns_amend a = {.fields = NS_ATTRIBUTES, .attributes = attributes}; /* there already: it gets them */
    return ns_record_amend(v, e.id, &a);
}

/* The entry at p and everything in it; nothing when it is not there. */
static int remove_at(struct volume *v, const struct path *p) {
    if (!p->count) return -N_ERR_INVALID; /* not the root */
    struct ns_entry e, parent;
    struct store_record rec;
    struct path up = {p->count - 1, p->part};
    int r = ns_lookup(v, p, &e, &rec);
    if (r == -N_ERR_NOT_FOUND) return 0;
    if (r >= 0 && rec.kind == STORE_FOLDER) r = ns_clear(v, e.id, host_now());
    struct ranges dropped = {0};
    if (r >= 0 && (r = ns_lookup(v, &up, &parent, &rec)) >= 0)
        r = ns_remove(v, parent.id, e.name, e.n, host_now(), &dropped, NULL);
    space_release_list(v, &dropped);
    return r;
}

/* host's file or folder at p (the root takes a folder's contents). */
static int put_at(struct put *put, const struct path *p, const char *host, uint16_t attributes) {
    uint64_t folder = STORE_ID_ROOT;
    if (!p->count) return put_folder(put, folder, NULL, 0, host, attributes);
    int r = parent_of(put->v, p, &folder);
    return r < 0 ? r : put_any(put, folder, p->part[p->count - 1].name, p->part[p->count - 1].n, host, attributes);
}

/* ---- checking a volume ---- */

struct seen {
    uint64_t id;
    struct store_record rec;
    size_t inline_n;
    unsigned names;
};
struct link {
    uint64_t folder, id;
};
struct check {
    struct volume *v;
    uint8_t *named; /* a bit per block the catalog names */
    struct seen *records;
    size_t nrecords, rroom;
    struct link *links;
    size_t nlinks, lroom;
    uint64_t doc_end; /* where the current document's last extent ends */
    long problems;
};

static void problem(struct check *c, const char *fmt, ...) {
    if (c->problems++ < 20) {
        va_list a;
        va_start(a, fmt);
        fprintf(stderr, "store: check: ");
        vfprintf(stderr, fmt, a);
        fputc('\n', stderr);
        va_end(a);
    }
}

static bool name_block(struct check *c, uint64_t block, const char *what) {
    struct volume *v = c->v;
    if (!block || block >= v->super.blocks - 1 || store_layout_has(&v->space.layout, block)) {
        problem(c, "%s names block %llu, outside the room for them", what, (unsigned long long)block);
        return false;
    }
    if (c->named[block / 8] & (1u << block % 8)) problem(c, "block %llu is named twice (%s)", (unsigned long long)block, what);
    c->named[block / 8] |= (uint8_t)(1u << block % 8);
    return true;
}

static int check_node(void *ctx, uint64_t address, unsigned level) {
    name_block(ctx, address, "a catalog node");
    return 0;
}

static int check_item(void *ctx, const struct item *it) {
    struct check *c = ctx;
    struct volume *v = c->v;
    if (it->klen < 9) {
        problem(c, "a key of %zu bytes", it->klen);
        return 0;
    }
    uint64_t id = be64(it->key);
    struct seen *last = c->nrecords ? &c->records[c->nrecords - 1] : NULL;
    if (last && last->id != id) last = NULL;
    switch (it->key[8]) {
    case STORE_KEY_RECORD: {
        if (it->klen != 9 || it->vlen < sizeof(struct store_record)) {
            problem(c, "record %llu is %zu bytes", (unsigned long long)id, it->vlen);
            return 0;
        }
        struct seen *s = store_array_add(&env, &c->records, &c->nrecords, &c->rroom, sizeof *s);
        memset(s, 0, sizeof *s);
        s->id = id;
        memcpy(&s->rec, it->value, sizeof s->rec);
        s->inline_n = it->vlen - sizeof s->rec;
        c->doc_end = 0;
        if (id >= v->next_id) problem(c, "record %llu is past the next id", (unsigned long long)id);
        return 0;
    }
    case STORE_KEY_ENTRY: {
        if (!last || last->rec.kind != STORE_FOLDER) problem(c, "an entry in %llu, which is no folder", (unsigned long long)id);
        if (it->vlen <= sizeof(struct store_entry)) {
            problem(c, "an entry of folder %llu without a name", (unsigned long long)id);
            return 0;
        }
        struct store_entry e;
        memcpy(&e, it->value, sizeof e);
        const char *name = (const char *)it->value + sizeof e;
        size_t n = it->vlen - sizeof e;
        uint8_t folded[STORE_KEY_MAX];
        int k = name_check(name, n) < 0 ? -1 : name_fold(&v->fold, name, n, folded, sizeof folded);
        if (k < 0 || (size_t)k != it->klen - 9 || memcmp(folded, it->key + 9, (size_t)k))
            problem(c, "the entry \"%.*s\" of folder %llu is filed under another name", (int)n, name, (unsigned long long)id);
        struct link *l = store_array_add(&env, &c->links, &c->nlinks, &c->lroom, sizeof *l);
        *l = (struct link){id, e.id};
        return 0;
    }
    case STORE_KEY_EXTENT: {
        if (!last || last->rec.kind != STORE_DOCUMENT || (last->rec.flags & STORE_INLINE)) {
            problem(c, "an extent of %llu, which has no blocks", (unsigned long long)id);
            return 0;
        }
        bool checked = !(last->rec.flags & STORE_UNCHECKED);
        struct store_extent e;
        uint32_t check[STORE_EXTENT_CHECKS];
        if (it->klen != 17 || it->vlen < sizeof e) {
            problem(c, "an extent of %llu is malformed", (unsigned long long)id);
            return 0;
        }
        memcpy(&e, it->value, sizeof e);
        uint64_t index = be64(it->key + 9);
        if (!e.count || (checked && (e.count > STORE_EXTENT_CHECKS || it->vlen != sizeof e + 4 * (size_t)e.count)) ||
            (!checked && it->vlen != sizeof e)) {
            problem(c, "extent %llu of %llu holds %u blocks in %zu bytes", (unsigned long long)index,
                    (unsigned long long)id, e.count, it->vlen);
            return 0;
        }
        if (checked) memcpy(check, it->value + sizeof e, 4 * (size_t)e.count);
        if (index < c->doc_end) problem(c, "extent %llu of %llu overlaps the one before", (unsigned long long)index, (unsigned long long)id);
        if (index + e.count > (last->rec.size + STORE_BLOCK - 1) / STORE_BLOCK)
            problem(c, "extent %llu of %llu reaches past its end", (unsigned long long)index, (unsigned long long)id);
        c->doc_end = index + e.count;
        bool inside = true;
        for (uint32_t i = 0; i < e.count; i++) inside &= name_block(c, e.block + i, "an extent");
        for (uint32_t i = 0; checked && inside && i < e.count;) {
            uint32_t k = STORE_MIN(e.count - i, 64u);
            struct store_block *got[64];
            int r = v->blocks->read_many(v->blocks, v, e.block + i, k, content_valid, check + i, got);
            if (r < 0) problem(c, "blocks %llu.. of %llu do not read back as written (%s)",
                               (unsigned long long)(index + i), (unsigned long long)id, error_name(r));
            else
                for (uint32_t j = 0; j < k; j++) v->blocks->put(v->blocks, got[j]);
            i += k;
        }
        return 0;
    }
    }
    problem(c, "an item of type %u", it->key[8]);
    return 0;
}

static struct seen *seen_find(struct check *c, uint64_t id) {
    size_t lo = 0, hi = c->nrecords;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c->records[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < c->nrecords && c->records[lo].id == id ? &c->records[lo] : NULL;
}

/* how many problems there are; quiet: no counts printed */
static long check_volume(struct volume *v, bool quiet) {
    struct check c = {.v = v, .named = calloc(1, v->super.blocks / 8 + 1)};
    const struct catalog_visit visit = {check_node, check_item};
    int r = catalog_walk(v, &visit, &c);
    if (r < 0) problem(&c, "the catalog cannot be walked (%s)", error_name(r));
    for (size_t i = 0; i < c.nlinks; i++) {
        struct seen *s = seen_find(&c, c.links[i].id);
        if (!s) problem(&c, "folder %llu names %llu, which has no record", (unsigned long long)c.links[i].folder,
                        (unsigned long long)c.links[i].id);
        else if (s->names++, s->rec.parent != c.links[i].folder)
            problem(&c, "%llu is named in %llu, its parent is %llu", (unsigned long long)s->id,
                    (unsigned long long)c.links[i].folder, (unsigned long long)s->rec.parent);
    }
    if (!seen_find(&c, STORE_ID_ROOT)) problem(&c, "there is no root folder");
    uint64_t documents = 0, folders = 0, bytes = 0;
    for (size_t i = 0; i < c.nrecords; i++) {
        struct seen *s = &c.records[i];
        unsigned want = s->id == STORE_ID_ROOT || s->id == STORE_ID_FOLD ? 0 : 1;
        if (s->names != want) problem(&c, "%llu is named %u times", (unsigned long long)s->id, s->names);
        if (s->id == STORE_ID_ROOT && (s->rec.kind != STORE_FOLDER || s->rec.parent != STORE_ID_ROOT))
            problem(&c, "the root is not a folder of its own");
        if (s->id == STORE_ID_FOLD && s->rec.kind != STORE_DOCUMENT) problem(&c, "the case folding is no document");
        if (s->rec.kind == STORE_DOCUMENT) {
            documents++, bytes += s->rec.size;
            bool small = s->rec.flags & STORE_INLINE;
            if (small ? s->inline_n != s->rec.size : s->inline_n != 0)
                problem(&c, "document %llu holds %zu bytes in its record for %llu", (unsigned long long)s->id, s->inline_n,
                        (unsigned long long)s->rec.size);
        } else if (s->rec.kind == STORE_FOLDER) {
            folders++;
            if (s->inline_n) problem(&c, "folder %llu has content", (unsigned long long)s->id);
        } else {
            problem(&c, "%llu is of kind %u", (unsigned long long)s->id, s->rec.kind);
        }
        /* up to the root through folders */
        uint64_t at = s->id;
        size_t steps = 0;
        for (; at != STORE_ID_ROOT && s->id != STORE_ID_FOLD && steps <= c.nrecords; steps++) {
            struct seen *up = seen_find(&c, seen_find(&c, at)->rec.parent);
            if (!up || up->rec.kind != STORE_FOLDER) {
                problem(&c, "%llu is not inside the root", (unsigned long long)s->id);
                break;
            }
            at = up->id;
        }
        if (steps > c.nrecords) problem(&c, "%llu is inside itself", (unsigned long long)s->id);
    }
    /* the space map: exactly the superblocks, the map itself and what the catalog names are used */
    uint64_t used = 0, wrong = 0;
    for (uint64_t b = 0; b < v->super.blocks; b++) {
        bool want = !b || b == v->super.blocks - 1 || store_layout_has(&v->space.layout, b) || (c.named[b / 8] & (1u << b % 8));
        int got = space_used(v, b);
        used += want;
        if (got < 0) {
            problem(&c, "the space map cannot be read at %llu (%s)", (unsigned long long)b, error_name(got));
            break;
        }
        if (got != want && wrong++ < 8)
            problem(&c, "block %llu is %s in the space map", (unsigned long long)b, got ? "used" : "free");
    }
    if (wrong > 8) problem(&c, "%llu blocks in all are wrong in the space map", (unsigned long long)wrong);
    if (used != v->space.used) problem(&c, "%llu blocks are used, the space map counts %llu", (unsigned long long)used,
                                       (unsigned long long)v->space.used);
    if (!quiet)
        printf("%llu folders, %llu documents, %llu bytes; %llu of %llu blocks used; %ld problems\n",
               (unsigned long long)folders, (unsigned long long)documents, (unsigned long long)bytes,
               (unsigned long long)used, (unsigned long long)v->super.blocks, c.problems);
    free(c.named);
    heap_free(c.records);
    heap_free(c.links);
    return c.problems;
}

/* ---- the test: random changes against a model ---- */

struct blob {
    unsigned refs;
    uint64_t size;
    uint8_t data[];
};

struct mnode {
    char name[256];
    size_t n;
    uint8_t key[STORE_KEY_MAX]; /* the folded name */
    size_t klen;
    uint16_t kind, attributes;
    uint64_t id;
    struct blob *content;
    struct mnode *parent;
    struct mnode **kids;
    size_t nkids, room;
};

struct test {
    uint64_t seed;
    long step;
    const char *op;
    struct memory mem;
    uint64_t full; /* the blocks of memory: the medium grows up to it */
    struct volume *v;
    struct mnode *now, *saved, *before; /* now, at the last commit, at the commit before it */
    bool before_valid;
    long ops[16];
};

static struct test *testing; /* the volume test under way */
static char test_name[64];   /* another test under way */

static const char *test_context(void) {
    static _Thread_local char text[160];
    if (testing)
        snprintf(text, sizeof text, "store: test (seed %llu, step %ld, %s): ", (unsigned long long)testing->seed,
                 testing->step, testing->op);
    else if (test_name[0])
        snprintf(text, sizeof text, "store: %s: ", test_name);
    else
        return "";
    return text;
}

/* what the store told in the test, by kind */
static void troubles_print(void) {
    for (int i = 0; i <= STORE_TROUBLE_RULE; i++)
        if (troubles[i]) printf("  told %ld times: %s\n", troubles[i], store_trouble_text((enum store_trouble)i));
    memset(troubles, 0, sizeof troubles);
}

static _Noreturn void fail(struct test *t, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fprintf(stderr, "store: test failed (seed %llu, step %ld, %s): ", (unsigned long long)t->seed, t->step, t->op);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
    exit(1);
}

static struct blob *blob_new(uint64_t size) {
    struct blob *b = calloc(1, sizeof *b + size);
    b->refs = 1;
    b->size = size;
    return b;
}

static void blob_put(struct blob *b) {
    if (b && !--b->refs) free(b);
}

static struct mnode *mnode_copy(const struct mnode *m, struct mnode *parent) {
    struct mnode *c = malloc(sizeof *c);
    *c = *m;
    c->parent = parent;
    if (c->content) c->content->refs++;
    c->kids = NULL;
    c->nkids = c->room = 0;
    for (size_t i = 0; i < m->nkids; i++)
        *(struct mnode **)store_array_add(&env, &c->kids, &c->nkids, &c->room, sizeof *c->kids) = mnode_copy(m->kids[i], c);
    return c;
}

static void mnode_free(struct mnode *m) {
    if (!m) return;
    for (size_t i = 0; i < m->nkids; i++) mnode_free(m->kids[i]);
    heap_free(m->kids);
    blob_put(m->content);
    free(m);
}

static void mnode_unlink(struct mnode *m) {
    struct mnode *p = m->parent;
    for (size_t i = 0; i < p->nkids; i++)
        if (p->kids[i] == m) {
            p->kids[i] = p->kids[--p->nkids];
            break;
        }
}

static void mnode_link(struct mnode *m, struct mnode *parent) {
    m->parent = parent;
    *(struct mnode **)store_array_add(&env, &parent->kids, &parent->nkids, &parent->room, sizeof *parent->kids) = m;
}

static struct mnode *mnode_kid(const struct mnode *folder, const uint8_t *key, size_t klen) {
    for (size_t i = 0; i < folder->nkids; i++)
        if (folder->kids[i]->klen == klen && !memcmp(folder->kids[i]->key, key, klen)) return folder->kids[i];
    return NULL;
}

static void mnode_all(struct mnode *m, struct mnode ***list, size_t *n, size_t *room) {
    *(struct mnode **)store_array_add(&env, list, n, room, sizeof **list) = m;
    for (size_t i = 0; i < m->nkids; i++) mnode_all(m->kids[i], list, n, room);
}

/* a random node: of kind (0: any), not the root unless root_too */
static struct mnode *pick(struct test *t, uint16_t kind, bool root_too) {
    struct mnode **list = NULL;
    size_t n = 0, room = 0, k = 0;
    mnode_all(t->now, &list, &n, &room);
    for (size_t i = 0; i < n; i++)
        if ((!kind || list[i]->kind == kind) && (root_too || list[i] != t->now)) list[k++] = list[i];
    struct mnode *m = k ? list[below(k)] : NULL;
    heap_free(list);
    return m;
}

static size_t count_nodes(const struct mnode *m) {
    size_t n = 1;
    for (size_t i = 0; i < m->nkids; i++) n += count_nodes(m->kids[i]);
    return n;
}

/* a random name, mostly valid, often like another but for its case; the expected result of using it */
static int random_name(struct test *t, char *name, size_t *n, uint8_t *key, size_t *klen) {
    static const char *const parts[] = {"a", "A", "b", "B", "\xc3\xa9", "\xc3\x89", "\xcf\x89", "\xce\xa9",
                                        "\xd0\xb6", "\xd0\x96", "1", "_", "x", "X", "\xe2\x84\xa6"};
    size_t len = 0;
    uint64_t kind = below(100);
    if (kind < 2) { /* not a name */
        static const char *const bad[] = {".", "..", "a:", "a ", "b.", "a\\b", "x*", "q?"};
        strcpy(name, bad[below(sizeof bad / sizeof *bad)]);
        len = strlen(name);
    } else if (kind < 4) { /* long, and sometimes too long */
        len = 240 + (size_t)below(18);
        for (size_t i = 0; i < len; i++) name[i] = below(2) ? 'a' : 'A';
    } else {
        for (uint64_t i = 0, parts_n = 1 + below(3); i < parts_n; i++) {
            const char *p = parts[below(sizeof parts / sizeof *parts)];
            memcpy(name + len, p, strlen(p));
            len += strlen(p);
        }
    }
    name[len] = 0;
    *n = len;
    int r = name_check(name, len);
    if (r < 0) return r;
    int k = name_fold(&t->v->fold, name, len, key, STORE_KEY_MAX - 9);
    if (k < 0) return k;
    *klen = (size_t)k;
    return 0;
}

static void expect(struct test *t, int got, int want, const char *what) {
    if (got != want) fail(t, "%s gave %d, not %d", what, got, want);
}

static void check_reserved(struct test *t) {
    if (t->v->space.reserved) fail(t, "%llu blocks are left reserved", (unsigned long long)t->v->space.reserved);
}

static void remount(struct test *t) {
    close_volume(t->v);
    t->v = NULL;
    int r = open_volume(&t->mem.dev, CACHE_TEST, NULL, false, &t->v);
    if (r < 0) fail(t, "the volume does not mount (%d)", r);
}

static void model_now(struct test *t, struct mnode *from) {
    mnode_free(t->now);
    t->now = mnode_copy(from, NULL);
}

static void committed(struct test *t, uint64_t durable_before) {
    if (t->v->durable == durable_before) return; /* nothing was written */
    mnode_free(t->before);
    t->before = t->saved;
    t->saved = mnode_copy(t->now, NULL);
    t->before_valid = true;
}

/* the document's new content in the model, as ns_update makes it */
static struct blob *updated(const struct blob *old, uint64_t size, uint64_t cut, const struct ns_page *pages, size_t n) {
    struct blob *b = blob_new(size);
    uint64_t keep = STORE_MIN(STORE_MIN(old->size, cut * STORE_BLOCK), size);
    memcpy(b->data, old->data, (size_t)keep);
    for (size_t i = 0; i < n; i++) {
        uint64_t at = pages[i].index * STORE_BLOCK;
        if (at < size) memcpy(b->data + at, pages[i].data, (size_t)STORE_MIN((uint64_t)STORE_BLOCK, size - at));
    }
    return b;
}

static void op_update(struct test *t, struct mnode *doc) {
    struct volume *v = t->v;
    uint64_t old = doc->content->size, size;
    switch (below(8)) {
    case 0: size = 0; break;
    case 1: size = below(STORE_INLINE_MAX + 1); break;
    case 2: size = STORE_INLINE_MAX - 2 + below(5); break;
    case 3: size = old + below(3 * STORE_BLOCK); break;
    case 4: size = old - below(old + 1); break;
    case 5: size = (1 + below(40)) * STORE_BLOCK + below(3) - 1; break;
    case 6: size = below(300) * STORE_BLOCK + below(STORE_BLOCK); break;
    default: size = below(12 * STORE_BLOCK); break;
    }
    uint64_t blocks = (size + STORE_BLOCK - 1) / STORE_BLOCK, old_blocks = (old + STORE_BLOCK - 1) / STORE_BLOCK;
    uint64_t cut = below(3) ? old_blocks : below(old_blocks + 2);
    /* pages: a few anywhere, or a run, or every block */
    struct ns_page *pages = NULL;
    size_t n = 0, room = 0;
    uint64_t how = below(4);
    for (uint64_t i = 0; i < blocks; i++) {
        bool take = how == 0 ? below(blocks) < 3 : how == 1 ? i >= blocks / 3 && i < blocks / 3 + 8 : how == 2;
        if (!take) continue;
        struct ns_page *p = store_array_add(&env, &pages, &n, &room, sizeof *p);
        p->index = i;
        p->data = heap_alloc(STORE_BLOCK);
        random_bytes(p->data, STORE_BLOCK);
    }
    if (n && below(4) == 0) /* one beyond the end: dropped */
        pages[0].index = blocks + below(3);
    bool reserved = below(4) || space_available(v) < n + 256;
    struct blob *want = updated(doc->content, size, cut, pages, n);
    int r = reserved ? space_reserve(v, n) : 0;
    if (r == -N_ERR_NO_SPACE) {
        for (size_t i = 0; i < n; i++) heap_free(pages[i].data);
        heap_free(pages);
        blob_put(want);
        t->ops[15]++;
        return;
    }
    expect(t, r, 0, "space_reserve");
    struct ranges dropped = {0};
    r = ns_update(v, doc->id, size, cut, pages, n, reserved, host_now(), &dropped, NULL);
    heap_free(pages);
    space_release_list(v, &dropped);
    if (r == -N_ERR_NO_SPACE && reserved && !v->error) { /* the block the change itself needed */
        blob_put(want);
        t->ops[15]++;
        return;
    }
    expect(t, r, 0, "ns_update");
    blob_put(doc->content);
    doc->content = want;
}

static void op_create(struct test *t, uint16_t kind) {
    struct mnode *folder = pick(t, STORE_FOLDER, true);
    /* now and then a folder of the system's, or one the apps do not see: what goes into it has that too */
    static const uint16_t gives[] = {STORE_SYSTEM, STORE_PRIVATE, STORE_SYSTEM | STORE_PRIVATE};
    uint16_t own = kind == STORE_FOLDER && below(5) == 0 ? gives[below(3)] : 0;
    struct mnode m = {.kind = kind, .attributes = (uint16_t)(own | (folder->attributes & NS_GIVEN))};
    int want = random_name(t, m.name, &m.n, m.key, &m.klen);
    if (!want && mnode_kid(folder, m.key, m.klen)) want = -N_ERR_EXISTS;
    int r = ns_create(t->v, folder->id, m.name, m.n, kind, own, 0, host_now(), &m.id, NULL);
    expect(t, r, want, "ns_create");
    if (r < 0) return;
    struct mnode *c = malloc(sizeof *c);
    *c = m;
    if (kind == STORE_DOCUMENT) c->content = blob_new(0);
    mnode_link(c, folder);
    if (kind == STORE_DOCUMENT && below(4)) op_update(t, c);
}

static void op_remove(struct test *t) {
    struct mnode *m = pick(t, 0, false);
    if (!m) return;
    int want = m->nkids ? -N_ERR_NOT_EMPTY : 0;
    struct ranges dropped = {0};
    int r = ns_remove(t->v, m->parent->id, m->name, m->n, host_now(), &dropped, NULL);
    space_release_list(t->v, &dropped);
    expect(t, r, want, "ns_remove");
    if (r < 0) return;
    mnode_unlink(m);
    mnode_free(m);
}

/* m's attributes in folder to: what its folder gave goes, what to gives comes (store/namespace.h) */
static uint16_t moved_attributes(const struct mnode *m, const struct mnode *to) {
    return (uint16_t)((m->attributes & ~(m->parent->attributes & NS_GIVEN)) | (to->attributes & NS_GIVEN));
}

/* a move, half of them ns_replace: a document at the name goes to aside's aside_name */
static void op_move(struct test *t) {
    struct mnode *m = pick(t, 0, false);
    if (!m) return;
    struct mnode *to = below(3) ? pick(t, STORE_FOLDER, true) : m->parent;
    struct mnode *aside = below(2) ? pick(t, STORE_FOLDER, true) : NULL, *over = NULL;
    char name[300], aside_name[300];
    uint8_t key[STORE_KEY_MAX], aside_key[STORE_KEY_MAX];
    size_t n, klen, aside_n = 0, aside_klen = 0;
    int aside_want = aside ? random_name(t, aside_name, &aside_n, aside_key, &aside_klen) : 0;
    int want;
    if (below(3) == 0) { /* the same name in another case */
        memcpy(name, m->name, m->n + 1);
        n = m->n;
        for (size_t i = 0; i < n; i++)
            if ((name[i] | 0x20) >= 'a' && (name[i] | 0x20) <= 'z' && below(2)) name[i] ^= 0x20;
        want = name_check(name, n);
        klen = (size_t)name_fold(&t->v->fold, name, n, key, STORE_KEY_MAX - 9);
    } else {
        want = random_name(t, name, &n, key, &klen);
    }
    bool same = !want && to == m->parent && klen == m->klen && !memcmp(key, m->key, klen);
    if (!want && !same) {
        for (struct mnode *at = to; at; at = at->parent)
            if (at == m) want = -N_ERR_INVALID;
        if (!want && (over = mnode_kid(to, key, klen)) && !aside) want = -N_ERR_EXISTS;
        /* what is there is checked as it would move to aside */
        if (!want && over) want = aside_want;
        if (!want && over && aside == to && aside_klen == klen && !memcmp(aside_key, key, klen)) want = -N_ERR_INVALID;
        for (struct mnode *at = aside; !want && over && at; at = at->parent)
            if (at == over) want = -N_ERR_INVALID;
        if (!want && over && (over->kind != STORE_DOCUMENT || mnode_kid(aside, aside_key, aside_klen))) want = -N_ERR_EXISTS;
    }
    /* memory may run out part of the way: then the move changes nothing, and C: stays writable */
    bool starve = below(3) == 0;
    if (starve) alloc_fail_after = (long)below(16);
    int r = aside ? ns_replace(t->v, m->parent->id, m->name, m->n, to->id, name, n, aside->id, aside_name, aside_n,
                               host_now(), NULL)
                  : ns_move(t->v, m->parent->id, m->name, m->n, to->id, name, n, host_now(), NULL);
    alloc_fail_after = -1;
    if (starve && r == -N_ERR_NO_MEMORY) {
        if (t->v->error) fail(t, "a move without memory left C: read only (%d)", t->v->error);
        if (check_volume(t->v, true)) fail(t, "a move without memory left the volume with problems");
        t->ops[14]++;
        return;
    }
    expect(t, r, want, aside ? "ns_replace" : "ns_move");
    if (r < 0) return;
    if (over) {
        memcpy(over->name, aside_name, aside_n + 1);
        over->n = aside_n;
        memcpy(over->key, aside_key, aside_klen);
        over->klen = aside_klen;
        over->attributes = moved_attributes(over, aside);
        mnode_unlink(over);
        mnode_link(over, aside);
    }
    memcpy(m->name, name, n + 1);
    m->n = n;
    memcpy(m->key, key, klen);
    m->klen = klen;
    if (!same) {
        m->attributes = moved_attributes(m, to);
        mnode_unlink(m);
        mnode_link(m, to);
    }
}

static void op_read(struct test *t) {
    struct mnode *doc = pick(t, STORE_DOCUMENT, false);
    if (!doc) return;
    uint64_t size = doc->content->size, offset = below(size + STORE_BLOCK), n = below(5 * STORE_BLOCK);
    uint8_t *buf = malloc(n + 1);
    int64_t got = ns_read(t->v, doc->id, offset, buf, n);
    int64_t want = offset >= size ? 0 : (int64_t)STORE_MIN(n, size - offset);
    if (got != want) fail(t, "ns_read of %llu bytes at %llu of %llu gave %lld", (unsigned long long)n,
                          (unsigned long long)offset, (unsigned long long)size, (long long)got);
    if (want && memcmp(buf, doc->content->data + offset, (size_t)want)) fail(t, "ns_read gave other bytes");
    free(buf);
}

static void op_take(struct test *t) {
    struct mnode *doc = pick(t, below(8) ? STORE_DOCUMENT : 0, false), *from = below(8) ? pick(t, STORE_DOCUMENT, false) : doc;
    if (!doc || !from) return;
    int want = doc->kind != STORE_DOCUMENT ? -N_ERR_IS_FOLDER : doc == from ? -N_ERR_INVALID : 0;
    struct ranges dropped = {0};
    int r = ns_take(t->v, doc->id, from->id, host_now(), &dropped, NULL);
    space_release_list(t->v, &dropped);
    expect(t, r, want, "ns_take");
    if (r < 0) return;
    blob_put(doc->content);
    doc->content = from->content;
    from->content = blob_new(0);
}

/* The device grows and the volume with it, as the kernel grows C: to the end of its disk: an
   even commit first, then the growth, which the power may cut short; the next mount tries again. */
static void op_grow(struct test *t) {
    uint64_t was = t->v->super.blocks, durable = t->v->durable;
    if (was == t->full) return;
    uint64_t to = was + 1 + below(t->full - was);
    struct store_layout l = store_layout(to);
    if (to - was < 2 * (l.indexes + l.maps) + 1) return; /* no room for the map of that size */
    /* what is open first, so that the growth's commits hold no document's change */
    expect(t, volume_commit(t->v), 0, "volume_commit");
    expect(t, volume_even(t->v), 0, "volume_even");
    committed(t, durable);
    durable = t->v->durable;
    t->mem.dev.blocks = to;
    for (bool cut = below(2); t->v->super.blocks != to; cut = false) {
        if (cut) t->mem.fail_after = (long)below(60);
        int r = volume_grow(t->v, to);
        t->mem.fail_after = -1;
        if (r >= 0) break;
        expect(t, r, -N_ERR_READ_ONLY, "a growth cut short");
        /* the volume as it was or grown, the same documents either way */
        remount(t);
        model_now(t, t->saved);
        t->before_valid = false;
    }
    t->ops[13]++;
    committed(t, durable); /* the commits of the growth change no document */
    if (check_volume(t->v, true)) fail(t, "the volume has problems after growing");
}

/* the volume against the model, everything in it */
static void verify_node(struct test *t, const struct mnode *m) {
    struct volume *v = t->v;
    struct store_record rec;
    if (ns_record(v, m->id, &rec, NULL) < 0) fail(t, "%llu has no record", (unsigned long long)m->id);
    if (rec.kind != m->kind || (m->parent && rec.parent != m->parent->id) || rec.attributes != m->attributes)
        fail(t, "%llu (\"%s\") is not as it was left", (unsigned long long)m->id, m->name);
    if (m->kind == STORE_DOCUMENT) {
        if (rec.size != m->content->size)
            fail(t, "\"%s\" is %llu bytes, not %llu", m->name, (unsigned long long)rec.size, (unsigned long long)m->content->size);
        uint8_t *buf = malloc(rec.size + 1);
        int64_t got = ns_read(v, m->id, 0, buf, (size_t)rec.size);
        if (got != (int64_t)rec.size || memcmp(buf, m->content->data, (size_t)rec.size))
            fail(t, "\"%s\" does not hold what was written", m->name);
        free(buf);
        return;
    }
    size_t count = 0;
    struct ns_entry e, prev;
    for (int r = ns_next(v, m->id, NULL, &e); r != -N_ERR_NOT_FOUND; r = ns_next(v, m->id, &prev, &e)) {
        if (r < 0) fail(t, "the entries of \"%s\" cannot be listed (%d)", m->name, r);
        const struct mnode *kid = NULL;
        for (size_t i = 0; i < m->nkids && !kid; i++)
            if (m->kids[i]->id == e.id) kid = m->kids[i];
        if (!kid || kid->n != e.n || memcmp(kid->name, e.name, e.n))
            fail(t, "\"%s\" holds \"%.*s\", which it should not", m->name, (int)e.n, e.name);
        count++;
        prev = e;
    }
    if (count != m->nkids) fail(t, "\"%s\" holds %zu entries, not %zu", m->name, count, m->nkids);
    for (size_t i = 0; i < m->nkids; i++) {
        if (ns_find(v, m->id, m->kids[i]->name, m->kids[i]->n, &e) < 0 || e.id != m->kids[i]->id)
            fail(t, "\"%s\" is not found by its name", m->kids[i]->name);
        verify_node(t, m->kids[i]);
    }
}

static void verify(struct test *t) {
    verify_node(t, t->now);
    if (check_volume(t->v, true)) fail(t, "the volume has problems");
}

static int test_run(uint64_t mib, uint64_t seed, long steps) {
    struct test t = {.seed = seed, .op = "format"};
    testing = &t;
    test_running = true;
    random_state = seed * 0x9e3779b97f4a7c15ULL + 1;
    t.full = (mib << 20) / STORE_BLOCK;
    /* a volume smaller than the memory most of the time, to grow later */
    if (!memory_make(&t.mem, mib << 20, t.full / (1 + below(4)))) fail(&t, "no memory for the medium");
    int r = open_volume(&t.mem.dev, CACHE_TEST, "test", false, &t.v);
    if (r >= 0) r = volume_commit(t.v);
    if (r < 0) fail(&t, "the volume cannot be made (%d)", r);
    remount(&t); /* as the tool formats a disk, and fills it in another run */
    t.now = calloc(1, sizeof *t.now);
    t.now->kind = STORE_FOLDER;
    t.now->id = STORE_ID_ROOT;
    t.saved = mnode_copy(t.now, NULL);
    static const char *const names[] = {"create document", "create folder", "update", "remove", "move", "read",
                                        "commit", "crash", "cut-short commit", "damaged superblock", "verify", "take", "grow"};
    for (t.step = 0; t.step < steps; t.step++) {
        uint64_t roll = below(1000), durable = t.v->durable;
        size_t nodes = count_nodes(t.now);
        int op = roll < 140 ? 0 : roll < 220 ? 1 : roll < 440 ? 2 : roll < 470 ? 11 : roll < 560 ? 3 : roll < 660 ? 4
               : roll < 840 ? 5 : roll < 900 ? 6 : roll < 925 ? 7 : roll < 950 ? 8 : roll < 965 ? 9 : roll < 975 ? 12 : 10;
        if (nodes > 300 && op <= 1) op = 3;
        t.op = names[op];
        t.ops[op]++;
        switch (op) {
        case 0: op_create(&t, STORE_DOCUMENT); break;
        case 1: op_create(&t, STORE_FOLDER); break;
        case 2: {
            struct mnode *doc = pick(&t, STORE_DOCUMENT, false);
            if (doc) op_update(&t, doc);
            break;
        }
        case 3: op_remove(&t); break;
        case 4: op_move(&t); break;
        case 5: op_read(&t); break;
        case 6:
            expect(&t, volume_commit(t.v), 0, "volume_commit");
            committed(&t, durable);
            break;
        case 7: /* the power goes: what was not committed is gone */
            remount(&t);
            model_now(&t, t.saved);
            break;
        case 8: /* the power goes during a commit */
            t.mem.fail_after = (long)below(40);
            r = volume_commit(t.v);
            t.mem.fail_after = -1;
            if (r >= 0) {
                committed(&t, durable);
                break;
            }
            expect(&t, r, -N_ERR_READ_ONLY, "a commit cut short");
            remount(&t);
            model_now(&t, t.saved);
            t.before_valid = false;
            break;
        case 9: /* the last commit's superblock is found damaged: the one before it stands */
            expect(&t, volume_commit(t.v), 0, "volume_commit");
            committed(&t, durable);
            if (!t.before_valid) break;
            t.mem.data[(t.v->durable % 2 ? t.v->super.blocks - 1 : 0) * STORE_BLOCK + 200] ^= 0x5a;
            remount(&t);
            model_now(&t, t.before);
            mnode_free(t.saved);
            t.saved = mnode_copy(t.before, NULL);
            t.before_valid = false;
            break;
        case 10: verify(&t); break;
        case 11: op_take(&t); break;
        case 12: op_grow(&t); break;
        }
        check_reserved(&t);
        if (t.step % 16 == 0) heap_check();
        if ((t.step + 1) % 2000 == 0)
            printf("  step %ld: %zu entries, %llu blocks used\n", t.step + 1, count_nodes(t.now),
                   (unsigned long long)t.v->space.used);
    }
    t.op = "the end";
    expect(&t, volume_commit(t.v), 0, "volume_commit");
    remount(&t);
    verify(&t);
    printf("test passed (seed %llu, %ld steps):", (unsigned long long)seed, steps);
    for (int i = 0; i < 13; i++) printf(" %s %ld,", names[i], t.ops[i]);
    printf(" grown %ld, out of memory %ld, out of space %ld\n", t.ops[13], t.ops[14], t.ops[15]);
    troubles_print();
    close_volume(t.v);
    mnode_free(t.now);
    mnode_free(t.saved);
    mnode_free(t.before);
    free(t.mem.data);
    testing = NULL;
    test_running = false;
    return 0;
}

/* ---- the catalog's test: its tree alone against a model of its keys ---- */

/* A tree of many leaves and branches, shaped by turns of growth, of removal mostly in its low keys
   (first children leave their branches), of growth again (keys below a branch's first key) and of
   thinning everywhere with runs of keys (small branches join their neighbours): random puts of
   values of many sizes, deletes and commits. Every so often the walk checks the order of every
   node and every key is looked up. The volume test keeps its tree to a few leaves. */
static int walk_count(void *ctx, const struct item *it) {
    (void)it;
    ++*(uint64_t *)ctx;
    return 0;
}

static void tree_key(uint8_t *key, uint32_t n) {
    memset(key, 0, 17);
    key[8] = STORE_KEY_EXTENT;
    for (int i = 0; i < 4; i++) key[13 + i] = (uint8_t)(n >> (24 - 8 * i));
}

static int tree_test(uint64_t seed, long steps) {
    enum { KEYS = 20000, ROUND = 20000 };
    snprintf(test_name, sizeof test_name, "tree test (seed %llu)", (unsigned long long)seed);
    test_running = true;
    random_state = seed * 0x9e3779b97f4a7c15ULL + 1;
    struct memory mem;
    uint16_t *size = calloc(KEYS, sizeof *size); /* each key's value length; 0: not there */
    uint8_t key[17], value[STORE_VALUE_MAX], got[STORE_VALUE_MAX];
    struct volume *v;
    if (!memory_make(&mem, 512u << 20, (512u << 20) / STORE_BLOCK) || !size ||
        open_volume(&mem.dev, CACHE_TEST, "tree", true, &v) < 0) {
        fprintf(stderr, "store: the tree test cannot start\n");
        return 1;
    }
    for (long step = 0; step < steps; step++) {
        long turn = step / ROUND % 4;
        uint32_t n = (uint32_t)below(turn == 1 && below(4) ? KEYS / 4 : KEYS);
        bool remove = below(10) < (turn == 1 ? 8u : turn == 3 ? 7u : 2u);
        /* the thinning turn also takes runs of keys: whole leaves go, then branches join */
        uint32_t run = turn == 3 && remove && !below(20) ? 1 + (uint32_t)below(KEYS / 50) : 1;
        tree_key(key, n);
        int r;
        if (remove) {
            for (uint32_t k = n; k < n + run && k < KEYS; k++) {
                tree_key(key, k);
                r = catalog_delete(v, key, sizeof key);
                if ((r >= 0) != (size[k] != 0)) {
                    fprintf(stderr, "store: tree test (seed %llu, step %ld): deleting key %u gives %s\n",
                            (unsigned long long)seed, step, k, error_name(r));
                    return 1;
                }
                size[k] = 0;
            }
        } else {
            uint16_t len = (uint16_t)(1 + below(STORE_VALUE_MAX / 2));
            for (uint16_t i = 0; i < len; i++) value[i] = (uint8_t)(n + i);
            if ((r = catalog_put(v, key, sizeof key, value, len)) < 0) {
                fprintf(stderr, "store: tree test (seed %llu, step %ld): putting key %u gives %s\n",
                        (unsigned long long)seed, step, n, error_name(r));
                return 1;
            }
            size[n] = len;
        }
        if (below(500) == 0 && volume_commit(v) < 0) {
            fprintf(stderr, "store: tree test (seed %llu, step %ld): the commit fails\n", (unsigned long long)seed, step);
            return 1;
        }
        if (step % 1000 != 999 && step + 1 != steps) continue;
        uint64_t items = 0, want = 0;
        const struct catalog_visit visit = {.item = walk_count};
        if ((r = catalog_walk(v, &visit, &items)) < 0) {
            fprintf(stderr, "store: tree test (seed %llu, step %ld): the walk gives %s\n", (unsigned long long)seed,
                    step, error_name(r));
            return 1;
        }
        for (uint32_t k = 0; k < KEYS; k++) {
            if (!size[k]) continue;
            want++;
            tree_key(key, k);
            r = catalog_get(v, key, sizeof key, got, sizeof got);
            bool same = r == size[k];
            for (int i = 0; same && i < r; i++) same = got[i] == (uint8_t)(k + i);
            if (!same) {
                fprintf(stderr, "store: tree test (seed %llu, step %ld): key %u is %s\n", (unsigned long long)seed,
                        step, k, r < 0 ? "not found" : "wrong");
                return 1;
            }
        }
        if (items != want) {
            fprintf(stderr, "store: tree test (seed %llu, step %ld): the walk meets %llu keys of %llu\n",
                    (unsigned long long)seed, step, (unsigned long long)items, (unsigned long long)want);
            return 1;
        }
    }
    printf("tree test passed (seed %llu, %ld steps)\n", (unsigned long long)seed, steps);
    troubles_print();
    close_volume(v);
    free(size);
    free(mem.data);
    test_running = false;
    test_name[0] = 0;
    return 0;
}

/* ---- the stress test: threads changing the volume at once ---- */

/* Each thread keeps documents in a folder of its own, so that a folder's names and a document's
   content change one at a time as the namespace asks (store/namespace.h), checks them against its
   own model as it goes, and lists the other threads' folders while they change. Every change holds
   the gate shared; a commit, one at a time, holds it alone while it takes the open generation and
   writes it with the gate open again, as the kernel's C: does. What it tries is the store's own:
   its latches, catalog, space map and blocks in memory under many threads. Its threads are as many
   as the CPUs, as the kernel's are: the store's latches spin (store/latch.h), and a thread Windows
   takes off its CPU while it holds one leaves the others spinning until it comes back. */
#define STRESS_DOCS 48u
#define STRESS_BLOCKS 12u /* a document's most */

struct sdoc {
    char name[32];
    size_t n;
    uint64_t id;
    struct blob *content;
};

struct stress;

struct worker {
    struct stress *s;
    unsigned index;
    uint64_t random, folder;
    struct sdoc docs[STRESS_DOCS];
    size_t ndocs;
    unsigned long names; /* names given so far */
    long step, ops[9];
    HANDLE thread;
};

struct stress {
    uint64_t seed;
    struct memory mem;
    struct volume *v;
    SRWLOCK gate, committing;
    struct worker *workers;
    unsigned nworkers;
    long steps, commits;
    LONG running; /* workers not done yet */
};

static _Noreturn void stress_fail(const struct stress *s, const struct worker *w, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fprintf(stderr, "store: stress test failed (seed %llu", (unsigned long long)s->seed);
    if (w) fprintf(stderr, ", thread %u, step %ld", w->index, w->step);
    fputs("): ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
    exit(1);
}

static uint64_t worker_below(struct worker *w, uint64_t n) { return n ? random_next(&w->random) % n : 0; }

static void stress_commit(struct stress *s) {
    struct store_commit c;
    AcquireSRWLockExclusive(&s->committing);
    AcquireSRWLockExclusive(&s->gate);
    int r = volume_freeze(s->v, &c);
    ReleaseSRWLockExclusive(&s->gate);
    if (!r) {
        r = volume_write(s->v, &c);
        volume_written(s->v, &c, r);
        s->commits++;
    }
    ReleaseSRWLockExclusive(&s->committing);
    if (r < 0) stress_fail(s, NULL, "a commit fails (%s)", error_name(r));
}

static void worker_name(struct worker *w, struct sdoc *d) {
    d->n = (size_t)snprintf(d->name, sizeof d->name, "d%u-%lu", w->index, w->names++);
}

static struct sdoc *worker_pick(struct worker *w) { return w->ndocs ? &w->docs[worker_below(w, w->ndocs)] : NULL; }

static void worker_update(struct worker *w, struct sdoc *d) {
    struct stress *s = w->s;
    struct volume *v = s->v;
    uint64_t size = worker_below(w, STRESS_BLOCKS * STORE_BLOCK + 1), blocks = (size + STORE_BLOCK - 1) / STORE_BLOCK;
    uint64_t old_blocks = (d->content->size + STORE_BLOCK - 1) / STORE_BLOCK;
    uint64_t cut = worker_below(w, 2) ? old_blocks : worker_below(w, old_blocks + 1);
    struct ns_page pages[STRESS_BLOCKS];
    size_t n = 0;
    for (uint64_t i = 0; i < blocks; i++) {
        if (worker_below(w, 3) == 0) continue;
        pages[n].index = i;
        if (!(pages[n].data = heap_alloc(STORE_BLOCK))) stress_fail(s, w, "no memory");
        for (size_t k = 0; k < STORE_BLOCK; k += 8) {
            uint64_t x = random_next(&w->random);
            memcpy(pages[n].data + k, &x, 8);
        }
        n++;
    }
    struct blob *want = updated(d->content, size, cut, pages, n);
    int r = space_reserve(v, n);
    if (r == -N_ERR_NO_SPACE) {
        for (size_t i = 0; i < n; i++) heap_free(pages[i].data);
        blob_put(want);
        w->ops[8]++;
        return;
    }
    if (r < 0) stress_fail(s, w, "space_reserve gives %s", error_name(r));
    struct ranges dropped = {0};
    AcquireSRWLockShared(&s->gate);
    r = ns_update(v, d->id, size, cut, pages, n, true, host_now(), &dropped, NULL);
    space_release_list(v, &dropped);
    ReleaseSRWLockShared(&s->gate);
    if (r == -N_ERR_NO_SPACE && !v->error) { /* the block the change itself needed */
        blob_put(want);
        w->ops[8]++;
        return;
    }
    if (r < 0) stress_fail(s, w, "ns_update gives %s", error_name(r));
    blob_put(d->content);
    d->content = want;
}

static void worker_read(struct worker *w, struct sdoc *d) {
    uint64_t size = d->content->size, offset = worker_below(w, size + STORE_BLOCK), n = worker_below(w, 3 * STORE_BLOCK);
    uint8_t *buf = malloc(n + 1);
    int64_t got = ns_read(w->s->v, d->id, offset, buf, n);
    int64_t want = offset >= size ? 0 : (int64_t)STORE_MIN(n, size - offset);
    if (got != want || (want && memcmp(buf, d->content->data + offset, (size_t)want)))
        stress_fail(w->s, w, "\"%s\" does not read back as written", d->name);
    free(buf);
}

/* another thread's folder, changing meanwhile */
static void worker_list(struct worker *w) {
    struct stress *s = w->s;
    uint64_t folder = s->workers[worker_below(w, s->nworkers)].folder;
    struct ns_entry e, prev;
    for (int r = ns_next(s->v, folder, NULL, &e); r != -N_ERR_NOT_FOUND; r = ns_next(s->v, folder, &prev, &e)) {
        if (r < 0) stress_fail(s, w, "a folder cannot be listed (%s)", error_name(r));
        prev = e;
    }
}

static DWORD WINAPI worker_run(void *arg) {
    struct worker *w = arg;
    struct stress *s = w->s;
    struct volume *v = s->v;
    for (w->step = 0; w->step < s->steps; w->step++) {
        uint64_t roll = worker_below(w, 100);
        struct sdoc *d = worker_pick(w);
        int op = roll < 15 ? 0 : roll < 45 ? 1 : roll < 65 ? 2 : roll < 72 ? 3 : roll < 82 ? 4 : roll < 87 ? 5 : roll < 98 ? 6 : 7;
        if (!d || (op == 0 && w->ndocs == STRESS_DOCS)) op = w->ndocs == STRESS_DOCS ? 3 : 0;
        w->ops[op]++;
        int r = 0;
        switch (op) {
        case 0: { /* a new document */
            struct sdoc *n = &w->docs[w->ndocs];
            worker_name(w, n);
            AcquireSRWLockShared(&s->gate);
            r = ns_create(v, w->folder, n->name, n->n, STORE_DOCUMENT, 0, 0, host_now(), &n->id, NULL);
            ReleaseSRWLockShared(&s->gate);
            if (r < 0) stress_fail(s, w, "ns_create gives %s", error_name(r));
            n->content = blob_new(0);
            w->ndocs++;
            break;
        }
        case 1: worker_update(w, d); break;
        case 2: worker_read(w, d); break;
        case 3: { /* a document goes */
            struct ranges dropped = {0};
            AcquireSRWLockShared(&s->gate);
            r = ns_remove(v, w->folder, d->name, d->n, host_now(), &dropped, NULL);
            space_release_list(v, &dropped);
            ReleaseSRWLockShared(&s->gate);
            if (r < 0) stress_fail(s, w, "ns_remove gives %s", error_name(r));
            blob_put(d->content);
            *d = w->docs[--w->ndocs];
            break;
        }
        case 4: { /* a new name */
            struct sdoc to = *d;
            worker_name(w, &to);
            AcquireSRWLockShared(&s->gate);
            r = ns_move(v, w->folder, d->name, d->n, w->folder, to.name, to.n, host_now(), NULL);
            ReleaseSRWLockShared(&s->gate);
            if (r < 0) stress_fail(s, w, "ns_move gives %s", error_name(r));
            *d = to;
            break;
        }
        case 5: { /* one's content to another */
            struct sdoc *from = worker_pick(w);
            if (from == d) break;
            struct ranges dropped = {0};
            AcquireSRWLockShared(&s->gate);
            r = ns_take(v, d->id, from->id, host_now(), &dropped, NULL);
            space_release_list(v, &dropped);
            ReleaseSRWLockShared(&s->gate);
            if (r < 0) stress_fail(s, w, "ns_take gives %s", error_name(r));
            blob_put(d->content);
            d->content = from->content;
            from->content = blob_new(0);
            break;
        }
        case 6: worker_list(w); break;
        case 7: stress_commit(s); break;
        }
    }
    InterlockedDecrement(&s->running);
    return 0;
}

/* every thread's folder against its model */
static void stress_verify(struct stress *s) {
    struct volume *v = s->v;
    uint8_t *buf = malloc(STRESS_BLOCKS * STORE_BLOCK + 1);
    for (unsigned i = 0; i < s->nworkers; i++) {
        struct worker *w = &s->workers[i];
        size_t count = 0;
        struct ns_entry e, prev;
        for (int r = ns_next(v, w->folder, NULL, &e); r != -N_ERR_NOT_FOUND; r = ns_next(v, w->folder, &prev, &e)) {
            if (r < 0) stress_fail(s, w, "the folder cannot be listed (%s)", error_name(r));
            count++;
            prev = e;
        }
        if (count != w->ndocs) stress_fail(s, w, "the folder holds %zu documents, not %zu", count, w->ndocs);
        for (size_t k = 0; k < w->ndocs; k++) {
            struct sdoc *d = &w->docs[k];
            struct store_record rec;
            if (ns_find(v, w->folder, d->name, d->n, &e) < 0 || e.id != d->id) stress_fail(s, w, "\"%s\" is not there", d->name);
            if (ns_record(v, d->id, &rec, NULL) < 0 || rec.size != d->content->size)
                stress_fail(s, w, "\"%s\" is not %llu bytes", d->name, (unsigned long long)d->content->size);
            int64_t got = ns_read(v, d->id, 0, buf, (size_t)rec.size);
            if (got != (int64_t)rec.size || memcmp(buf, d->content->data, (size_t)rec.size))
                stress_fail(s, w, "\"%s\" does not hold what was written", d->name);
        }
    }
    free(buf);
    if (check_volume(v, true)) stress_fail(s, NULL, "the volume has problems");
}

static int stress_run(uint64_t mib, uint64_t seed, unsigned threads, long steps) {
    struct stress s = {.seed = seed, .nworkers = threads ? threads : 1, .steps = steps};
    snprintf(test_name, sizeof test_name, "stress test (seed %llu)", (unsigned long long)seed);
    test_running = true;
    random_state = seed * 0x9e3779b97f4a7c15ULL + 1;
    InitializeSRWLock(&s.gate);
    InitializeSRWLock(&s.committing);
    if (!memory_make(&s.mem, mib << 20, (mib << 20) / STORE_BLOCK) || !(s.workers = calloc(s.nworkers, sizeof *s.workers)))
        stress_fail(&s, NULL, "no memory for the medium");
    int r = open_volume(&s.mem.dev, CACHE_TEST, "stress", false, &s.v);
    for (unsigned i = 0; r >= 0 && i < s.nworkers; i++) {
        struct worker *w = &s.workers[i];
        char name[16];
        int n = snprintf(name, sizeof name, "w%u", i);
        *w = (struct worker){.s = &s, .index = i, .random = (seed + 1) * 0x9e3779b97f4a7c15ULL ^ (i + 1) * 0xbf58476d1ce4e5b9ULL};
        r = ns_create(s.v, STORE_ID_ROOT, name, (size_t)n, STORE_FOLDER, 0, 0, host_now(), &w->folder, NULL);
    }
    if (r < 0) stress_fail(&s, NULL, "the volume cannot be made (%s)", error_name(r));
    stress_commit(&s);
    s.running = (LONG)s.nworkers;
    for (unsigned i = 0; i < s.nworkers; i++)
        if (!(s.workers[i].thread = CreateThread(NULL, 0, worker_run, &s.workers[i], 0, NULL)))
            stress_fail(&s, NULL, "a thread cannot be made");
    /* commits between the changes, besides the workers' own */
    while (InterlockedCompareExchange(&s.running, 0, 0)) {
        stress_commit(&s);
        Sleep(1);
    }
    long ops[9] = {0};
    for (unsigned i = 0; i < s.nworkers; i++) {
        WaitForSingleObject(s.workers[i].thread, INFINITE);
        CloseHandle(s.workers[i].thread);
        for (int k = 0; k < 9; k++) ops[k] += s.workers[i].ops[k];
    }
    stress_commit(&s);
    stress_verify(&s);
    /* what the medium holds, read anew */
    close_volume(s.v);
    if ((r = open_volume(&s.mem.dev, CACHE_TEST, NULL, false, &s.v)) < 0) stress_fail(&s, NULL, "the volume does not mount (%s)", error_name(r));
    stress_verify(&s);
    if (s.v->space.reserved) stress_fail(&s, NULL, "%llu blocks are left reserved", (unsigned long long)s.v->space.reserved);
    heap_check();
    printf("stress test passed (seed %llu, %u threads, %ld steps each): create %ld, update %ld, read %ld, remove %ld, "
           "rename %ld, take %ld, list %ld, commit %ld (%ld in all), out of space %ld\n",
           (unsigned long long)seed, s.nworkers, steps, ops[0], ops[1], ops[2], ops[3], ops[4], ops[5], ops[6], ops[7],
           s.commits, ops[8]);
    troubles_print();
    close_volume(s.v);
    for (unsigned i = 0; i < s.nworkers; i++)
        for (size_t k = 0; k < s.workers[i].ndocs; k++) blob_put(s.workers[i].docs[k].content);
    free(s.workers);
    free(s.mem.data);
    test_running = false;
    test_name[0] = 0;
    return 0;
}

/* ---- commands ---- */

static int usage(void) {
    fprintf(stderr, "usage: store disk <image> <MiB> <ESP MiB>\n"
                    "       store esp <image>\n"
                    "       store format <image> <label>\n"
                    "       store folder <image> <C:\\path> [--system] [--private]\n"
                    "       store put <image> <C:\\path> <host path> [--system]\n"
                    "       store fill <image> <list>\n"
                    "       store remove <image> <C:\\path>\n"
                    "       store list <image> <C:\\path>\n"
                    "       store get <image> <C:\\path> <host file>\n"
                    "       store check <image>\n"
                    "       store test <MiB> [seed] [steps]\n"
                    "       store stress <MiB> [seed] [threads] [steps]\n"
                    "       store tree-test <seed> [steps]\n");
    return 2;
}

static uint16_t attribute_of(const char *word) {
    if (!strcmp(word, "--system")) return STORE_SYSTEM;
    if (!strcmp(word, "--private")) return STORE_PRIVATE;
    fprintf(stderr, "store: what is %s?\n", word);
    exit(2);
}

static uint16_t attributes_of(int argc, char **argv, int from) {
    uint16_t a = 0;
    for (int i = from; i < argc; i++) a |= attribute_of(argv[i]);
    return a;
}

/* Every line of list: a path on C:, a tab, the host file or folder put there ("-" for a folder
   made empty, "!" to remove the entry there and everything in it), and, after another tab, its
   attributes ("--system ..."). Lines starting with '#' are notes. */
static int fill(struct volume *v, const char *list) {
    HANDLE f = host_open(list, HOST_READ);
    if (f == INVALID_HANDLE_VALUE) return -N_ERR_NOT_FOUND;
    int64_t size = host_size(f);
    char *text = size >= 0 ? malloc((size_t)size + 1) : NULL;
    int64_t got = text ? host_read_at(f, 0, text, (size_t)size) : -1;
    CloseHandle(f);
    if (got != size) {
        fprintf(stderr, "store: %s cannot be read\n", list);
        free(text);
        return -N_ERR_IO;
    }
    text[size] = 0;
    struct put put = {.v = v};
    int r = 0;
    char *line = text, *next;
    for (unsigned at = 1; r >= 0 && *line; at++, line = next) {
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *host = strchr(line, '\t'), *words = host ? strchr(host + 1, '\t') : NULL;
        if (host) *host++ = 0;
        if (words) *words++ = 0;
        struct path p;
        if (!host || path_parse(&env, line, &p) < 0) {
            fprintf(stderr, "store: %s:%u: not a path on C: and a host path\n", list, at);
            r = -N_ERR_INVALID;
            break;
        }
        uint16_t a = 0;
        for (char *w = words ? strtok(words, " ") : NULL; w; w = strtok(NULL, " ")) a |= attribute_of(w);
        r = !strcmp(host, "-") ? folder_make(v, &p, a) : !strcmp(host, "!") ? remove_at(v, &p) : put_at(&put, &p, host, a);
        if (r < 0)
            fprintf(stderr, "store: %s:%u: %s: %s\n", list, at, line, error_name(r));
        path_free(&env, &p);
    }
    free(text);
    if (r >= 0)
        printf("%llu folders, %llu documents, %llu bytes\n", (unsigned long long)put.folders,
               (unsigned long long)put.documents, (unsigned long long)put.bytes);
    return r;
}

static int finish(struct volume *v, int r) {
    if (r >= 0) r = volume_commit(v);
    if (r < 0) fprintf(stderr, "store: %s\n", error_name(r));
    close_volume(v);
    return r < 0;
}

static int run(int argc, char **argv) {
    random_state = (uint64_t)host_now() ^ (uint64_t)(uintptr_t)&argc;
    if (argc < 3) return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "test"))
        return test_run(strtoull(argv[2], NULL, 0), argc > 3 ? strtoull(argv[3], NULL, 0) : 1,
                        argc > 4 ? strtol(argv[4], NULL, 0) : 20000);
    if (!strcmp(cmd, "stress"))
        return stress_run(strtoull(argv[2], NULL, 0), argc > 3 ? strtoull(argv[3], NULL, 0) : 1,
                          argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) : (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
                          argc > 5 ? strtol(argv[5], NULL, 0) : 3000);
    if (!strcmp(cmd, "tree-test")) return tree_test(strtoull(argv[2], NULL, 0), argc > 3 ? strtol(argv[3], NULL, 0) : 100000);
    if (!strcmp(cmd, "disk"))
        return argc == 5 ? disk_make(argv[2], strtoull(argv[3], NULL, 0), strtoull(argv[4], NULL, 0)) : usage();
    if (!strcmp(cmd, "esp")) return argc == 3 ? esp_show(argv[2]) : usage();

    struct image im;
    struct volume *v = NULL;
    bool write = strcmp(cmd, "list") && strcmp(cmd, "get") && strcmp(cmd, "check"), format = !strcmp(cmd, "format");
    if (format && argc != 4) return usage();
    if (image_open(argv[2], write, &im) < 0) return 1;
    int r = open_volume(&im.dev, CACHE_IMAGE, format ? argv[3] : NULL, false, &v);
    if (r < 0) {
        fprintf(stderr, "store: %s: no volume (%s)\n", argv[2], error_name(r));
        return 1;
    }
    if (format) return finish(v, 0);
    if (!strcmp(cmd, "fill")) return argc == 4 ? finish(v, fill(v, argv[3])) : usage();
    if (!strcmp(cmd, "check")) {
        long problems = check_volume(v, false);
        close_volume(v);
        return problems != 0;
    }
    if (argc < 4) return usage();
    struct path p;
    if ((r = path_parse(&env, argv[3], &p)) < 0) {
        fprintf(stderr, "store: %s is not a path on C:\n", argv[3]);
        return 2;
    }
    if (!strcmp(cmd, "folder")) {
        r = folder_make(v, &p, attributes_of(argc, argv, 4));
    } else if (!strcmp(cmd, "remove")) {
        if (argc != 4) return usage();
        r = remove_at(v, &p);
    } else if (!strcmp(cmd, "put")) {
        if (argc < 5) return usage();
        struct put put = {.v = v};
        r = put_at(&put, &p, argv[4], attributes_of(argc, argv, 5));
        if (r >= 0)
            printf("%llu folders, %llu documents, %llu bytes\n", (unsigned long long)put.folders,
                   (unsigned long long)put.documents, (unsigned long long)put.bytes);
    } else if (!strcmp(cmd, "list") || !strcmp(cmd, "get")) {
        struct ns_entry e;
        struct store_record rec;
        bool list = !strcmp(cmd, "list");
        if (argc != (list ? 4 : 5)) return usage();
        if ((r = ns_lookup(v, &p, &e, &rec)) >= 0 && list && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r >= 0 && !list && rec.kind != STORE_DOCUMENT) r = -N_ERR_IS_FOLDER;
        if (r >= 0 && list) {
            struct ns_entry kid, prev;
            for (r = ns_next(v, e.id, NULL, &kid); r >= 0; r = ns_next(v, e.id, &prev, &kid)) {
                struct store_record kr;
                if ((r = ns_record(v, kid.id, &kr, NULL)) < 0) break;
                printf("%s %12llu %c%c%c %.*s\n", kr.kind == STORE_FOLDER ? "folder  " : "document",
                       (unsigned long long)kr.size, kr.attributes & STORE_SYSTEM ? 's' : '-',
                       kr.attributes & STORE_TEMPORARY ? 't' : '-', kr.attributes & STORE_PRIVATE ? 'p' : '-',
                       (int)kid.n, kid.name);
                prev = kid;
            }
            if (r == -N_ERR_NOT_FOUND) r = 0;
        } else if (r >= 0) {
            HANDLE f = host_open(argv[4], HOST_NEW);
            uint8_t *buf = malloc(1 << 20);
            if (f == INVALID_HANDLE_VALUE || !buf) r = -N_ERR_IO;
            for (uint64_t at = 0; r >= 0 && at < rec.size;) {
                int64_t got = ns_read(v, e.id, at, buf, 1 << 20);
                if (got <= 0) r = got < 0 ? (int)got : -N_ERR_IO;
                else if (!host_write_at(f, at, buf, (size_t)got)) r = -N_ERR_IO;
                else at += (uint64_t)got;
            }
            if (f != INVALID_HANDLE_VALUE && !CloseHandle(f)) r = -N_ERR_IO;
            free(buf);
        }
    } else {
        return usage();
    }
    path_free(&env, &p);
    return finish(v, r);
}

/* Windows gives the arguments as UTF-16: the tool takes them in UTF-8, as names on the volume are. */
int wmain(int argc, wchar_t **wargv) {
    char **argv = malloc(((size_t)argc + 1) * sizeof *argv);
    if (!argv) return 2;
    for (int i = 0; i < argc; i++)
        if (!(argv[i] = narrow(wargv[i]))) {
            fprintf(stderr, "store: argument %d is not Unicode\n", i);
            return 2;
        }
    argv[argc] = NULL;
    return run(argc, argv);
}
