/* The build host's tool for Nocturne's volume (common/store_format.h). It runs the kernel's own
   store code (kernel/src/fs: the volume, space map, catalog and namespace), with host/store/shim
   standing in for the kernel's headers, over a disk image:

     store disk <image> <MiB> <ESP MiB>      a new GPT disk: BIOS boot, EFI system and Nocturne
                                             partitions
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

   <image> is a disk with a GPT (the volume is its Nocturne partition) or a bare volume. */
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#ifdef _WIN32
#include <io.h>
#define seek64 _fseeki64
#define tell64 _ftelli64
#else
#include <unistd.h>
#define seek64 fseeko
#define tell64 ftello
#endif
#include "fs/namespace.h"
#include "fs/space.h"
#include "lib/crc32c.h"

/* ---- what the kernel gives the store ---- */

static const char *panic_context(void);

_Noreturn void panic(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fputs(panic_context(), stderr);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
    exit(2);
}

size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return len;
}

/* Every block has a head before it and a guard after it, and the heap knows every block: a write
   past one is found when it is freed or when the test looks at all of them (heap_check). A freed
   block waits a while, filled with a pattern, so that a write into it after its end is found too. */
struct guard {
    struct guard *prev, *next;
    size_t n;
    uint64_t magic;
};
#define GUARD_MAGIC 0x4e6f6374726e6521ULL
#define FREED_MAGIC 0x6465657266206e21ULL
#define GUARD_TAIL 16u
#define FREED_BYTES (64u << 20) /* freed blocks kept back */

static struct guard heap = {&heap, &heap, 0, 0}, freed = {&freed, &freed, 0, 0};
static size_t freed_bytes;
static long alloc_fail_after = -1; /* blocks still given before memory runs out (-1: no end) */

static void guard_check(const struct guard *g) {
    if (g->magic != GUARD_MAGIC) panic("heap: the head of a block is overwritten");
    const uint8_t *tail = (const uint8_t *)(g + 1) + g->n;
    for (unsigned i = 0; i < GUARD_TAIL; i++)
        if (tail[i] != 0xa5) panic("heap: something wrote past a block of %zu bytes", g->n);
}

static void freed_check(const struct guard *g) {
    const uint8_t *p = (const uint8_t *)(g + 1);
    if (g->magic != FREED_MAGIC) panic("heap: the head of a freed block is overwritten");
    for (size_t i = 0; i < g->n + GUARD_TAIL; i++)
        if (p[i] != 0xdd) panic("heap: something wrote at %zu of a freed block of %zu bytes", i, g->n);
}

static void heap_check(void) {
    for (struct guard *g = heap.next; g != &heap; g = g->next) guard_check(g);
    for (struct guard *g = freed.next; g != &freed; g = g->next) freed_check(g);
}

void *kmalloc(size_t n) {
    if (alloc_fail_after == 0) return NULL;
    if (alloc_fail_after > 0) alloc_fail_after--;
    struct guard *g = malloc(sizeof *g + n + GUARD_TAIL);
    if (!g) return NULL;
    *g = (struct guard){&heap, heap.next, n, GUARD_MAGIC};
    heap.next->prev = g;
    heap.next = g;
    memset(g + 1, 0xcd, n);
    memset((uint8_t *)(g + 1) + n, 0xa5, GUARD_TAIL);
    return g + 1;
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    struct guard *g = (struct guard *)p - 1;
    if (g->magic == FREED_MAGIC) panic("heap: a block of %zu bytes is freed twice", g->n);
    guard_check(g);
    g->prev->next = g->next;
    g->next->prev = g->prev;
    /* to the end of the freed list; the oldest one out */
    g->magic = FREED_MAGIC;
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
        free(old);
    }
}

void *krealloc(void *p, size_t n) {
    void *q = kmalloc(n);
    if (q && p) {
        memcpy(q, p, MIN(n, ((struct guard *)p - 1)->n));
        kfree(p);
    }
    return q;
}

void *kappend(void *array_at, size_t *count, size_t *room, size_t size) {
    void **array = array_at;
    if (*count == *room) {
        size_t more = *room ? *room * 2 : 8;
        void *grown = krealloc(*array, more * size);
        if (!grown) return NULL;
        *array = grown;
        *room = more;
    }
    return (uint8_t *)*array + (*count)++ * size;
}

int64_t store_clock(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ---- small things ---- */

static uint64_t random_state;

static uint64_t random64(void) {
    uint64_t x = random_state;
    x ^= x >> 12, x ^= x << 25, x ^= x >> 27;
    random_state = x;
    return x * 0x2545f4914f6cdd1dULL;
}

static uint64_t below(uint64_t n) { return n ? random64() % n : 0; }

static void random_bytes(void *out, size_t n) {
    uint8_t *p = out;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t x = random64();
        memcpy(p + i, &x, MIN((size_t)8, n - i));
    }
}

static uint64_t be64(const uint8_t *p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) x = x << 8 | p[i];
    return x;
}

static const char *error_name(int r) {
    static char text[32];
    snprintf(text, sizeof text, "error %d", r);
    return text;
}

/* ---- disk images ---- */

struct image {
    FILE *f;
    uint64_t offset; /* the volume's first byte */
};

static int image_read(struct store_device *d, uint64_t block, uint32_t count, void *buf) {
    struct image *im = d->priv;
    if (block + count > d->blocks) return -N_ERR_INVALID;
    if (seek64(im->f, (int64_t)(im->offset + block * STORE_BLOCK), SEEK_SET)) return -N_ERR_IO;
    size_t got = fread(buf, STORE_BLOCK, count, im->f);
    if (got < count) { /* past the end of a shorter file: zeros */
        if (ferror(im->f)) return -N_ERR_IO;
        memset((uint8_t *)buf + got * STORE_BLOCK, 0, (size_t)(count - got) * STORE_BLOCK);
    }
    return 0;
}

static int image_write(struct store_device *d, uint64_t block, uint32_t count, const void *buf) {
    struct image *im = d->priv;
    if (block + count > d->blocks) return -N_ERR_INVALID;
    if (seek64(im->f, (int64_t)(im->offset + block * STORE_BLOCK), SEEK_SET) ||
        fwrite(buf, STORE_BLOCK, count, im->f) != count)
        return -N_ERR_IO;
    return 0;
}

static int image_persist(struct store_device *d) {
    struct image *im = d->priv;
    if (fflush(im->f)) return -N_ERR_IO;
#ifdef _WIN32
    return _commit(_fileno(im->f)) ? -N_ERR_IO : 0;
#else
    return fsync(fileno(im->f)) ? -N_ERR_IO : 0;
#endif
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

static const uint8_t bios_boot_type[16] = {'H', 'a', 'h', '!', 'I', 'd', 'o', 'n', 't', 'N', 'e', 'e', 'd', 'E', 'F', 'I'};
static const uint8_t esp_type[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
static const uint8_t store_type[16] = STORE_PARTITION_TYPE;

static int disk_make(const char *path, uint64_t mib, uint64_t esp_mib) {
    uint64_t sectors = mib * 2048, bios = 2048, esp = 4096, esp_end = esp + esp_mib * 2048;
    uint64_t store = esp_end, last_usable = sectors - 1 - 1 - GPT_SECTORS;
    if (!esp_mib || store + 64 * 2048 > last_usable) {
        fprintf(stderr, "store: %llu MiB has no room for a %llu MiB EFI system partition and a volume\n",
                (unsigned long long)mib, (unsigned long long)esp_mib);
        return 1;
    }
    uint64_t store_end = store + (last_usable + 1 - store) / 8 * 8; /* whole blocks */
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    struct gpt_entry *entries = calloc(GPT_ENTRIES, sizeof *entries);
    uint8_t *sector = calloc(1, SECTOR);
    struct gpt_header h = {.revision = 0x10000, .size = 92, .first_usable = 2 + GPT_SECTORS, .last_usable = last_usable,
                           .count = GPT_ENTRIES, .entry_size = GPT_ENTRY};
    memcpy(h.signature, "EFI PART", 8);
    random_guid(h.disk);
    struct { const uint8_t *type; uint64_t first, last; const char *name; } part[3] = {
        {bios_boot_type, bios, esp - 1, "BIOS boot"},
        {esp_type, esp, esp_end - 1, "EFI system"},
        {store_type, store, store_end - 1, "Nocturne"},
    };
    for (int i = 0; i < 3; i++) {
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
    bad |= seek64(f, 0, SEEK_SET) || fwrite(sector, SECTOR, 1, f) != 1;
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
        bad |= seek64(f, (int64_t)(c.entries * SECTOR), SEEK_SET) ||
               fwrite(entries, GPT_ENTRY, GPT_ENTRIES, f) != GPT_ENTRIES;
        bad |= seek64(f, (int64_t)(c.self * SECTOR), SEEK_SET) || fwrite(sector, SECTOR, 1, f) != 1;
    }
    bad |= fclose(f) != 0;
    free(entries);
    free(sector);
    if (bad) {
        fprintf(stderr, "store: %s could not be written\n", path);
        return 1;
    }
    return 0;
}

/* The first and last sector of the partition of type in f's GPT: 1 when it is there, 0 when it is
   not, -1 when f has no GPT. */
static int gpt_partition(FILE *f, const uint8_t type[16], uint64_t *first, uint64_t *last) {
    struct gpt_header h;
    uint8_t sector[SECTOR];
    if (seek64(f, SECTOR, SEEK_SET) || fread(sector, SECTOR, 1, f) != 1 || memcmp(sector, "EFI PART", 8)) return -1;
    memcpy(&h, sector, sizeof h);
    struct gpt_entry e;
    for (uint32_t i = 0; i < h.count && i < 1024; i++) {
        if (seek64(f, (int64_t)(h.entries * SECTOR + (uint64_t)i * h.entry_size), SEEK_SET) ||
            fread(&e, sizeof e, 1, f) != 1)
            break;
        if (!memcmp(e.type, type, 16)) {
            *first = e.first, *last = e.last;
            return 1;
        }
    }
    return 0;
}

static int esp_show(const char *path) {
    FILE *f = fopen(path, "rb");
    uint64_t first, last;
    if (!f) {
        perror(path);
        return 1;
    }
    int found = gpt_partition(f, esp_type, &first, &last);
    fclose(f);
    if (found <= 0) {
        fprintf(stderr, "store: %s has no EFI system partition\n", path);
        return 1;
    }
    printf("%llu %llu\n", (unsigned long long)(first * SECTOR), (unsigned long long)(last + 1 - first));
    return 0;
}

/* the volume in the image: its Nocturne partition, or all of it when it has no GPT */
static int image_open(const char *path, struct image *im, struct store_device *dev) {
    memset(im, 0, sizeof *im);
    if (!(im->f = fopen(path, "r+b"))) {
        perror(path);
        return -1;
    }
    seek64(im->f, 0, SEEK_END);
    uint64_t bytes = (uint64_t)tell64(im->f), first = 0, last = bytes / SECTOR - 1;
    if (!gpt_partition(im->f, store_type, &first, &last)) {
        fprintf(stderr, "store: %s has no Nocturne partition\n", path);
        fclose(im->f);
        return -1;
    }
    if ((first * SECTOR) % STORE_BLOCK) {
        fprintf(stderr, "store: the Nocturne partition of %s does not start at a 4 KiB boundary\n", path);
        fclose(im->f);
        return -1;
    }
    im->offset = first * SECTOR;
    *dev = (struct store_device){.blocks = (last + 1 - first) * SECTOR / STORE_BLOCK, .read = image_read,
                                 .write = image_write, .persist = image_persist, .priv = im};
    return 0;
}

static int mount(struct store_device *dev, struct volume **v) {
    int r = volume_mount(dev, v);
    if (r >= 0 && (r = ns_mount(*v)) < 0) volume_free(*v);
    return r;
}

/* ---- the namespace by path ---- */

/* the folder that holds the path's last name: made, with its parents, when it is not there */
static int parent_of(struct volume *v, const struct path *p, uint64_t *folder) {
    uint64_t at = STORE_ID_ROOT;
    for (size_t i = 0; i + 1 < p->count; i++) {
        struct ns_entry e;
        struct store_record rec;
        int r = ns_find(v, at, p->part[i].name, p->part[i].n, &e);
        if (r == -N_ERR_NOT_FOUND) r = ns_create(v, at, p->part[i].name, p->part[i].n, STORE_FOLDER, 0, 0, &e.id);
        else if (r >= 0 && (r = ns_record(v, e.id, &rec, NULL)) >= 0 && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r < 0) return r;
        at = e.id;
    }
    *folder = at;
    return 0;
}

/* ---- putting host files on the volume ---- */

#define PUT_BLOCKS 256u      /* blocks written at a time */
#define COMMIT_BLOCKS 32768u /* written between commits: the cache holds them until then */

struct put {
    struct volume *v;
    uint64_t documents, folders, bytes;
};

static int put_any(struct put *p, uint64_t folder, const char *name, size_t n, const char *host, uint16_t attributes);

static int put_document(struct put *p, uint64_t folder, const char *name, size_t n, const char *host,
                        uint16_t attributes) {
    struct volume *v = p->v;
    FILE *f = fopen(host, "rb");
    if (!f) {
        perror(host);
        return -N_ERR_NOT_FOUND;
    }
    seek64(f, 0, SEEK_END);
    uint64_t size = (uint64_t)tell64(f), blocks = (size + STORE_BLOCK - 1) / STORE_BLOCK;
    seek64(f, 0, SEEK_SET);
    struct ns_entry e;
    struct store_record rec;
    int r = ns_find(v, folder, name, n, &e);
    if (r == -N_ERR_NOT_FOUND) r = ns_create(v, folder, name, n, STORE_DOCUMENT, attributes, 0, &e.id);
    else if (r >= 0 && (r = ns_record(v, e.id, &rec, NULL)) >= 0 && rec.kind != STORE_DOCUMENT) r = -N_ERR_IS_FOLDER;
    uint64_t done = 0;
    do {
        size_t k = (size_t)MIN((uint64_t)PUT_BLOCKS, blocks - done);
        struct ns_page *pages = calloc(k ? k : 1, sizeof *pages);
        for (size_t i = 0; i < k; i++) {
            pages[i].index = done + i;
            pages[i].data = kzalloc(STORE_BLOCK);
            if (r >= 0 && fread(pages[i].data, 1, STORE_BLOCK, f) == 0 && ferror(f)) r = -N_ERR_IO;
        }
        if (r >= 0) r = space_reserve(v, k);
        if (r < 0) {
            for (size_t i = 0; i < k; i++) kfree(pages[i].data);
            free(pages);
            break;
        }
        struct ranges dropped = {0};
        r = ns_update(v, e.id, size, done, pages, k, true, &dropped); /* blocks before done stay */
        space_release_list(v, &dropped);
        free(pages);
        done += k;
        if (r >= 0 && v->unwritten_count >= COMMIT_BLOCKS) r = volume_commit(v);
    } while (r >= 0 && done < blocks);
    fclose(f);
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
        if (r == -N_ERR_NOT_FOUND) r = ns_create(v, folder, name, n, STORE_FOLDER, attributes, 0, &e.id);
        else if (r >= 0 && (r = ns_record(v, e.id, &rec, NULL)) >= 0 && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r < 0) {
            fprintf(stderr, "store: %s: %s\n", host, error_name(r));
            return r;
        }
        id = e.id;
        p->folders++;
    }
    DIR *d = opendir(host);
    if (!d) {
        perror(host);
        return -N_ERR_NOT_FOUND;
    }
    for (struct dirent *de; r >= 0 && (de = readdir(d));) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        size_t hn = strlen(host) + 1 + strlen(de->d_name) + 1;
        char *child = malloc(hn);
        snprintf(child, hn, "%s/%s", host, de->d_name);
        r = put_any(p, id, de->d_name, strlen(de->d_name), child, attributes);
        free(child);
    }
    closedir(d);
    return r;
}

static int put_any(struct put *p, uint64_t folder, const char *name, size_t n, const char *host, uint16_t attributes) {
    struct stat st;
    if (stat(host, &st)) {
        perror(host);
        return -N_ERR_NOT_FOUND;
    }
    return S_ISDIR(st.st_mode) ? put_folder(p, folder, name, n, host, attributes)
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
    if ((r = ns_find(v, folder, name, n, &e)) == -N_ERR_NOT_FOUND) return ns_create(v, folder, name, n, STORE_FOLDER, attributes, 0, &e.id);
    if (r < 0 || (r = ns_record(v, e.id, &rec, NULL)) < 0) return r;
    if (rec.kind != STORE_FOLDER) return -N_ERR_NOT_FOLDER;
    rec.attributes |= attributes; /* there already: it gets the attributes */
    return ns_record_put(v, e.id, &rec, NULL, 0);
}

/* The entry at p and everything in it; nothing when it is not there. */
static int remove_at(struct volume *v, const struct path *p) {
    if (!p->count) return -N_ERR_INVALID; /* not the root */
    struct ns_entry e, parent;
    struct store_record rec;
    struct path up = {p->count - 1, p->part};
    int r = ns_lookup(v, p, &e, &rec);
    if (r == -N_ERR_NOT_FOUND) return 0;
    if (r >= 0 && rec.kind == STORE_FOLDER) r = ns_clear(v, e.id);
    struct ranges dropped = {0};
    if (r >= 0 && (r = ns_lookup(v, &up, &parent, &rec)) >= 0) r = ns_remove(v, parent.id, e.name, e.n, &dropped);
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
    if (block < v->space.layout.first || block >= v->super.blocks - 1) {
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
        struct seen *s = kappend(&c->records, &c->nrecords, &c->rroom, sizeof *s);
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
        struct link *l = kappend(&c->links, &c->nlinks, &c->lroom, sizeof *l);
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
            uint32_t k = MIN(e.count - i, 64u);
            struct cblock *got[64];
            int r = cache_read_many(v, e.block + i, k, content_valid, check + i, got);
            if (r < 0) problem(c, "blocks %llu.. of %llu do not read back as written (%s)",
                               (unsigned long long)(index + i), (unsigned long long)id, error_name(r));
            else
                for (uint32_t j = 0; j < k; j++) cache_put(v, got[j]);
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
        bool want = b < v->space.layout.first || b == v->super.blocks - 1 || (c.named[b / 8] & (1u << b % 8));
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
    kfree(c.records);
    kfree(c.links);
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
    uint16_t kind;
    uint64_t id;
    struct blob *content;
    struct mnode *parent;
    struct mnode **kids;
    size_t nkids, room;
};

struct memory {
    uint8_t *data;
    long fail_after; /* writes that still reach the medium before the power goes (-1: no end) */
};

struct test {
    uint64_t seed;
    long step;
    const char *op;
    struct memory mem;
    struct store_device dev;
    struct volume *v;
    struct mnode *now, *saved, *before; /* now, at the last commit, at the commit before it */
    bool before_valid;
    long ops[16];
};

static struct test *testing; /* the test under way, for a panic to tell where it is */

static const char *panic_context(void) {
    static char text[128];
    if (!testing) return "";
    snprintf(text, sizeof text, "store: test (seed %llu, step %ld, %s): ", (unsigned long long)testing->seed,
             testing->step, testing->op);
    return text;
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

static int memory_read(struct store_device *d, uint64_t block, uint32_t count, void *buf) {
    struct memory *m = d->priv;
    if (block + count > d->blocks) return -N_ERR_INVALID;
    memcpy(buf, m->data + block * STORE_BLOCK, (size_t)count * STORE_BLOCK);
    return 0;
}

static int memory_write(struct store_device *d, uint64_t block, uint32_t count, const void *buf) {
    struct memory *m = d->priv;
    if (block + count > d->blocks) return -N_ERR_INVALID;
    if (m->fail_after == 0) return -N_ERR_IO;
    if (m->fail_after > 0) m->fail_after--;
    memcpy(m->data + block * STORE_BLOCK, buf, (size_t)count * STORE_BLOCK);
    return 0;
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
    for (size_t i = 0; i < m->nkids; i++) *(struct mnode **)kappend(&c->kids, &c->nkids, &c->room, sizeof *c->kids) = mnode_copy(m->kids[i], c);
    return c;
}

static void mnode_free(struct mnode *m) {
    if (!m) return;
    for (size_t i = 0; i < m->nkids; i++) mnode_free(m->kids[i]);
    kfree(m->kids);
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
    *(struct mnode **)kappend(&parent->kids, &parent->nkids, &parent->room, sizeof *parent->kids) = m;
}

static struct mnode *mnode_kid(const struct mnode *folder, const uint8_t *key, size_t klen) {
    for (size_t i = 0; i < folder->nkids; i++)
        if (folder->kids[i]->klen == klen && !memcmp(folder->kids[i]->key, key, klen)) return folder->kids[i];
    return NULL;
}

static void mnode_all(struct mnode *m, struct mnode ***list, size_t *n, size_t *room) {
    *(struct mnode **)kappend(list, n, room, sizeof **list) = m;
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
    kfree(list);
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
    volume_free(t->v);
    t->v = NULL;
    int r = mount(&t->dev, &t->v);
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
    uint64_t keep = MIN(MIN(old->size, cut * STORE_BLOCK), size);
    memcpy(b->data, old->data, (size_t)keep);
    for (size_t i = 0; i < n; i++) {
        uint64_t at = pages[i].index * STORE_BLOCK;
        if (at < size) memcpy(b->data + at, pages[i].data, (size_t)MIN((uint64_t)STORE_BLOCK, size - at));
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
        struct ns_page *p = kappend(&pages, &n, &room, sizeof *p);
        p->index = i;
        p->data = kmalloc(STORE_BLOCK);
        random_bytes(p->data, STORE_BLOCK);
    }
    if (n && below(4) == 0) /* one beyond the end: dropped */
        pages[0].index = blocks + below(3);
    bool reserved = below(4) || space_available(v) < n + 256;
    struct blob *want = updated(doc->content, size, cut, pages, n);
    int r = reserved ? space_reserve(v, n) : 0;
    if (r == -N_ERR_NO_SPACE) {
        for (size_t i = 0; i < n; i++) kfree(pages[i].data);
        kfree(pages);
        blob_put(want);
        t->ops[15]++;
        return;
    }
    expect(t, r, 0, "space_reserve");
    struct ranges dropped = {0};
    r = ns_update(v, doc->id, size, cut, pages, n, reserved, &dropped);
    kfree(pages);
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
    struct mnode m = {.kind = kind};
    int want = random_name(t, m.name, &m.n, m.key, &m.klen);
    if (!want && mnode_kid(folder, m.key, m.klen)) want = -N_ERR_EXISTS;
    int r = ns_create(t->v, folder->id, m.name, m.n, kind, 0, 0, &m.id);
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
    int r = ns_remove(t->v, m->parent->id, m->name, m->n, &dropped);
    space_release_list(t->v, &dropped);
    expect(t, r, want, "ns_remove");
    if (r < 0) return;
    mnode_unlink(m);
    mnode_free(m);
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
    int r = aside ? ns_replace(t->v, m->parent->id, m->name, m->n, to->id, name, n, aside->id, aside_name, aside_n)
                  : ns_move(t->v, m->parent->id, m->name, m->n, to->id, name, n);
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
        mnode_unlink(over);
        mnode_link(over, aside);
    }
    memcpy(m->name, name, n + 1);
    m->n = n;
    memcpy(m->key, key, klen);
    m->klen = klen;
    if (!same) {
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
    int64_t want = offset >= size ? 0 : (int64_t)MIN(n, size - offset);
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
    int r = ns_take(t->v, doc->id, from->id, &dropped);
    space_release_list(t->v, &dropped);
    expect(t, r, want, "ns_take");
    if (r < 0) return;
    blob_put(doc->content);
    doc->content = from->content;
    from->content = blob_new(0);
}

/* the volume against the model, everything in it */
static void verify_node(struct test *t, const struct mnode *m) {
    struct volume *v = t->v;
    struct store_record rec;
    if (ns_record(v, m->id, &rec, NULL) < 0) fail(t, "%llu has no record", (unsigned long long)m->id);
    if (rec.kind != m->kind || (m->parent && rec.parent != m->parent->id))
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
    random_state = seed * 0x9e3779b97f4a7c15ULL + 1;
    t.mem.data = calloc(1, mib << 20);
    t.mem.fail_after = -1;
    t.dev = (struct store_device){.blocks = (mib << 20) / STORE_BLOCK, .read = memory_read, .write = memory_write, .priv = &t.mem};
    uint8_t id[16];
    random_bytes(id, sizeof id);
    int r = volume_format(&t.dev, "test", id, &t.v);
    if (r >= 0) r = ns_format(t.v);
    if (r >= 0) r = volume_commit(t.v);
    if (r < 0) fail(&t, "the volume cannot be made (%d)", r);
    t.now = calloc(1, sizeof *t.now);
    t.now->kind = STORE_FOLDER;
    t.now->id = STORE_ID_ROOT;
    t.saved = mnode_copy(t.now, NULL);
    static const char *const names[] = {"create document", "create folder", "update", "remove", "move", "read",
                                        "commit", "crash", "cut-short commit", "damaged superblock", "verify", "take"};
    for (t.step = 0; t.step < steps; t.step++) {
        uint64_t roll = below(1000), durable = t.v->durable;
        size_t nodes = count_nodes(t.now);
        int op = roll < 140 ? 0 : roll < 220 ? 1 : roll < 440 ? 2 : roll < 470 ? 11 : roll < 560 ? 3 : roll < 660 ? 4
               : roll < 840 ? 5 : roll < 900 ? 6 : roll < 925 ? 7 : roll < 950 ? 8 : roll < 965 ? 9 : 10;
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
            t.mem.data[(t.v->durable % 2 ? t.dev.blocks - 1 : 0) * STORE_BLOCK + 200] ^= 0x5a;
            remount(&t);
            model_now(&t, t.before);
            mnode_free(t.saved);
            t.saved = mnode_copy(t.before, NULL);
            t.before_valid = false;
            break;
        case 10: verify(&t); break;
        case 11: op_take(&t); break;
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
    for (int i = 0; i < 12; i++) printf(" %s %ld,", names[i], t.ops[i]);
    printf(" out of memory %ld, out of space %ld\n", t.ops[14], t.ops[15]);
    volume_free(t.v);
    mnode_free(t.now);
    mnode_free(t.saved);
    mnode_free(t.before);
    free(t.mem.data);
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
                    "       store test <MiB> [seed] [steps]\n");
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
    FILE *f = fopen(list, "r");
    if (!f) {
        perror(list);
        return -N_ERR_NOT_FOUND;
    }
    struct put put = {.v = v};
    char line[8192];
    int r = 0;
    for (unsigned at = 1; r >= 0 && fgets(line, sizeof line, f); at++) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *host = strchr(line, '\t'), *words = host ? strchr(host + 1, '\t') : NULL;
        if (host) *host++ = 0;
        if (words) *words++ = 0;
        struct path p;
        if (!host || path_parse(line, &p) < 0) {
            fprintf(stderr, "store: %s:%u: not a path on C: and a host path\n", list, at);
            r = -N_ERR_INVALID;
            break;
        }
        uint16_t a = 0;
        for (char *w = words ? strtok(words, " ") : NULL; w; w = strtok(NULL, " ")) a |= attribute_of(w);
        r = !strcmp(host, "-") ? folder_make(v, &p, a) : !strcmp(host, "!") ? remove_at(v, &p) : put_at(&put, &p, host, a);
        if (r < 0)
            fprintf(stderr, "store: %s:%u: %s: %s\n", list, at, line, error_name(r));
        path_free(&p);
    }
    fclose(f);
    if (r >= 0)
        printf("%llu folders, %llu documents, %llu bytes\n", (unsigned long long)put.folders,
               (unsigned long long)put.documents, (unsigned long long)put.bytes);
    return r;
}

static int finish(struct volume *v, int r) {
    if (r >= 0) r = volume_commit(v);
    if (r < 0) fprintf(stderr, "store: %s\n", error_name(r));
    volume_free(v);
    return r < 0;
}

int main(int argc, char **argv) {
    random_state = (uint64_t)store_clock() ^ (uint64_t)(uintptr_t)&argc;
    if (argc < 3) return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "test"))
        return test_run(strtoull(argv[2], NULL, 0), argc > 3 ? strtoull(argv[3], NULL, 0) : 1,
                        argc > 4 ? strtol(argv[4], NULL, 0) : 20000);
    if (!strcmp(cmd, "disk"))
        return argc == 5 ? disk_make(argv[2], strtoull(argv[3], NULL, 0), strtoull(argv[4], NULL, 0)) : usage();
    if (!strcmp(cmd, "esp")) return argc == 3 ? esp_show(argv[2]) : usage();

    struct image im;
    struct store_device dev;
    struct volume *v = NULL;
    if (image_open(argv[2], &im, &dev) < 0) return 1;
    int r;
    if (!strcmp(cmd, "format")) {
        if (argc != 4) return usage();
        uint8_t id[16];
        random_bytes(id, sizeof id);
        r = volume_format(&dev, argv[3], id, &v);
        if (r >= 0 && (r = ns_format(v)) < 0) volume_free(v);
    } else {
        r = mount(&dev, &v);
    }
    if (r < 0) {
        fprintf(stderr, "store: %s: no volume (%s)\n", argv[2], error_name(r));
        return 1;
    }
    if (!strcmp(cmd, "format")) return finish(v, 0);
    if (!strcmp(cmd, "fill")) return argc == 4 ? finish(v, fill(v, argv[3])) : usage();
    if (!strcmp(cmd, "check")) {
        long problems = check_volume(v, false);
        volume_free(v);
        return problems != 0;
    }
    if (argc < 4) return usage();
    struct path p;
    if ((r = path_parse(argv[3], &p)) < 0) {
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
            FILE *f = fopen(argv[4], "wb");
            uint8_t *buf = malloc(1 << 20);
            for (uint64_t at = 0; f && at < rec.size && r >= 0;) {
                int64_t got = ns_read(v, e.id, at, buf, 1 << 20);
                if (got <= 0) r = got < 0 ? (int)got : -N_ERR_IO;
                else if (fwrite(buf, 1, (size_t)got, f) != (size_t)got) r = -N_ERR_IO;
                else at += (uint64_t)got;
            }
            if (!f || fclose(f)) r = -N_ERR_IO;
            free(buf);
        }
    } else {
        return usage();
    }
    path_free(&p);
    return finish(v, r);
}
