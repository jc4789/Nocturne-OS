/* The store (fs/store.h): C: on the disk the system started from (or in memory, started from a CD
   or DVD), what is done to it by path, the gate between changes and commits, the folders' locks,
   and its writer. Open documents are fs/document.c's, the blocks in memory fs/cache.c's. */
#include "fs/document.h"
#include "fs/cache.h"
#include "fs/watch.h"
#include "store/space.h"
#include "dev/blk.h"
#include "sys/clock.h"
#include "dev/usb.h"
#include "lib/idtable.h"
#include "limine.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/object.h"

#define WRITE_DELAY_NS (5 * NS_PER_S) /* the first change waits this long for more to commit with it */
#define FIND_NS (10 * NS_PER_S)       /* how long the disk the system started from may take to appear */
#define FIND_MOST_NS (60 * NS_PER_S)  /* ... while USB devices are still being looked at */
#define GPT_ENTRIES_MAX (1u << 20)
#define MEMORY_KEPT 1024u   /* blocks kept of a volume in memory: reading one again is a copy */

extern volatile struct limine_executable_file_request exe_file_req;
extern volatile struct limine_module_request module_req;

struct store store;

/* ---- the medium C: is on ---- */

/* The Nocturne partition of the disk the system started from, or (no disk) the volume the boot
   loader brought into memory from a CD or DVD, whose changes last until the machine stops. */
static struct medium {
    struct store_device dev; /* first */
    struct blkdev *disk;
    uint64_t first;          /* the partition's first sector */
    uint8_t *base;           /* the volume in memory */
} medium;

static struct medium *medium_of(struct store_device *d) { return (struct medium *)d; }

static int part_read(struct store_device *d, uint64_t block, uint32_t count, void *buf) {
    struct medium *m = medium_of(d);
    return blk_read(m->disk, m->first + block * (STORE_BLOCK / 512), count * (STORE_BLOCK / 512), buf);
}

static int part_write(struct store_device *d, uint64_t block, uint32_t count, const void *buf) {
    struct medium *m = medium_of(d);
    return blk_write(m->disk, m->first + block * (STORE_BLOCK / 512), count * (STORE_BLOCK / 512), buf);
}

static int part_persist(struct store_device *d) { return blk_persist(medium_of(d)->disk); }

static int memory_read(struct store_device *d, uint64_t block, uint32_t count, void *buf) {
    memcpy(buf, medium_of(d)->base + block * STORE_BLOCK, (size_t)count * STORE_BLOCK);
    return 0;
}

static int memory_write(struct store_device *d, uint64_t block, uint32_t count, const void *buf) {
    memcpy(medium_of(d)->base + block * STORE_BLOCK, buf, (size_t)count * STORE_BLOCK);
    return 0;
}

/* where C: is, for messages */
static const char *medium_name(void) { return medium.disk ? medium.disk->name : "memory"; }

/* ---- the GPT ---- */

static const uint8_t store_type[16] = STORE_PARTITION_TYPE;

struct gpt_header {
    char signature[8];
    uint32_t revision, size, check, reserved;
    uint64_t self, other, first_usable, last_usable;
    uint8_t disk[16];
    uint64_t entries;
    uint32_t count, entry_size, entries_check;
} PACKED;

struct gpt_entry {
    uint8_t type[16], unique[16];
    uint64_t first, last, attributes;
} PACKED;

/* A disk's GPT as read. */
struct gpt {
    uint32_t unit; /* the bytes of its LBAs: 512 or 4096 */
    struct gpt_header h;
    uint8_t *entries; /* in whole LBAs */
    uint32_t sectors; /* the 512-byte sectors they take */
};

static struct gpt_entry *gpt_entry(struct gpt *g, uint32_t i) { return (void *)(g->entries + (size_t)i * g->h.entry_size); }

/* disk's GPT: 1, 0 when it has none the store reads, -1 when the disk could not be read (yet).
   buf holds 16 sectors: the first LBAs of the disk. */
static int gpt_read(struct blkdev *disk, uint8_t *buf, struct gpt *g) {
    memset(g, 0, sizeof *g);
    if (disk->sectors < 64) return 0;
    if (blk_read(disk, 0, 16, buf) < 0) return -1;
    /* the header is at the device's block 1, of 512 or 4096 bytes */
    g->unit = !memcmp(buf + 512, "EFI PART", 8) ? 512 : !memcmp(buf + 4096, "EFI PART", 8) ? 4096 : 0;
    if (!g->unit) return 0;
    memcpy(&g->h, buf + g->unit, sizeof g->h);
    uint64_t bytes = (uint64_t)g->h.count * g->h.entry_size, f = g->unit / 512;
    if (g->h.entry_size < 128 || g->h.entry_size % 8 || !bytes || bytes > GPT_ENTRIES_MAX) return 0;
    g->sectors = (uint32_t)((bytes + g->unit - 1) / g->unit * f);
    if (g->h.entries >= disk->sectors / f || g->h.entries * f + g->sectors > disk->sectors) return 0;
    if (!(g->entries = kmalloc((size_t)g->sectors * 512)) || blk_read(disk, g->h.entries * f, g->sectors, g->entries) < 0) {
        kfree(g->entries);
        g->entries = NULL;
        return -1;
    }
    return 1;
}

/* The Nocturne partition of disk (its first sector and STORE_BLOCKs), and the disk's GUID: 1 when
   it has one, 0 when not, -1 when the disk could not be read (yet). buf holds 16 sectors. */
static int gpt_find(struct blkdev *disk, uint8_t *buf, uint8_t guid[16], uint64_t *first, uint64_t *blocks) {
    struct gpt g;
    int found = gpt_read(disk, buf, &g);
    if (found <= 0) return found;
    memcpy(guid, g.h.disk, 16);
    uint64_t f = g.unit / 512;
    found = 0;
    for (uint32_t i = 0; i < g.h.count && !found; i++) {
        const struct gpt_entry *e = gpt_entry(&g, i);
        if (memcmp(e->type, store_type, 16)) continue;
        uint64_t lo = e->first * f, hi = (e->last + 1) * f;
        if (lo % (STORE_BLOCK / 512) || hi <= lo || hi > disk->sectors) {
            kprintf("store: %s has a Nocturne partition the store cannot use (not on 4 KiB)\n", disk->name);
            continue;
        }
        *first = lo;
        *blocks = (hi - lo) / (STORE_BLOCK / 512);
        found = 1;
    }
    kfree(g.entries);
    return found;
}

/* the CRC-32 of GPT (IEEE 802.3): a few blocks, once */
static uint32_t gpt_crc(const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

static uint32_t gpt_header_crc(struct gpt_header h) {
    h.check = 0;
    return gpt_crc(&h, sizeof h);
}

/* The STORE_BLOCKs C:'s partition can have when it reaches the end of its disk (an image written
   to a larger disk), or 0: the GPT reaches the end already, does not check out, or another
   partition follows. *mine: its entry. */
static uint64_t gpt_room(struct gpt *g, struct gpt_entry **mine) {
    struct blkdev *disk = medium.disk;
    uint64_t f = g->unit / 512, last = disk->sectors / f - 1, usable = last - 1 - g->sectors / f;
    if (usable <= g->h.last_usable) return 0;
    if (g->h.size != sizeof g->h || g->h.self != 1 || gpt_header_crc(g->h) != g->h.check ||
        gpt_crc(g->entries, (size_t)g->h.count * g->h.entry_size) != g->h.entries_check) {
        kprintf("store: the GPT of %s does not check out: C: keeps its size\n", disk->name);
        return 0;
    }
    static const uint8_t none[16];
    *mine = NULL;
    for (uint32_t i = 0; i < g->h.count; i++) {
        struct gpt_entry *e = gpt_entry(g, i);
        if (!memcmp(e->type, store_type, 16) && e->first * f == medium.first) *mine = e;
    }
    if (!*mine) return 0;
    for (uint32_t i = 0; i < g->h.count; i++) {
        struct gpt_entry *e = gpt_entry(g, i);
        if (e != *mine && memcmp(e->type, none, 16) && e->last > (*mine)->last) {
            kprintf("store: a partition follows C: on %s: C: keeps its size\n", disk->name);
            return 0;
        }
    }
    return (usable + 1 - (*mine)->first) * f / (STORE_BLOCK / 512);
}

static int write_lbas(struct gpt *g, uint64_t lba, const void *data, uint64_t lbas) {
    uint64_t f = g->unit / 512;
    int r = blk_write(medium.disk, lba * f, (uint32_t)(lbas * f), data);
    return r < 0 ? r : blk_persist(medium.disk);
}

/* C:'s partition to blocks (gpt_room's), its GPT's copy at the end of the disk. buf: gpt_read's. */
static int gpt_grow(struct gpt *g, struct gpt_entry *mine, uint8_t *buf, uint64_t blocks) {
    struct blkdev *disk = medium.disk;
    uint64_t f = g->unit / 512, last = disk->sectors / f - 1, lbas = g->sectors / f;
    uint8_t *block = kzalloc(MAX(g->unit, STORE_BLOCK));
    if (!block) return -N_ERR_NO_MEMORY;
    /* no superblock of what the disk held before at C:'s new last block */
    int r = blk_write(disk, medium.first + (blocks - 1) * (STORE_BLOCK / 512), STORE_BLOCK / 512, block);
    if (r >= 0) r = blk_persist(disk);
    mine->last = mine->first + blocks * (STORE_BLOCK / 512) / f - 1;
    struct gpt_header h = g->h;
    h.last_usable = last - 1 - lbas;
    h.other = last;
    h.entries_check = gpt_crc(g->entries, (size_t)h.count * h.entry_size);
    struct gpt_header copy = h;
    copy.self = last;
    copy.other = 1;
    copy.entries = last - lbas;
    copy.check = gpt_header_crc(copy);
    h.check = gpt_header_crc(h);
    /* the copy first: until the header at block 1 changes, it and the old copy are the GPT */
    if (r >= 0) r = write_lbas(g, copy.entries, g->entries, lbas);
    memcpy(block, &copy, sizeof copy);
    if (r >= 0) r = write_lbas(g, last, block, 1);
    if (r >= 0) r = write_lbas(g, h.entries, g->entries, lbas);
    memset(block, 0, g->unit);
    memcpy(block, &h, sizeof h);
    if (r >= 0) r = write_lbas(g, 1, block, 1);
    /* the protective MBR over the whole disk */
    if (r >= 0 && buf[446 + 4] == 0xee) {
        uint32_t size = last > 0xffffffffu ? 0xffffffffu : (uint32_t)last;
        memcpy(buf + 446 + 12, &size, 4);
        r = blk_write(disk, 0, 1, buf);
        if (r >= 0) r = blk_persist(disk);
    }
    kfree(block);
    return r;
}

/* C: to the end of its disk, on the first start from an image written to a larger one: its
   partition, then the volume (store/volume.h says in which order), alone on C: (store_start, with
   the gate closed and no other commit). The next start tries again what this one could not finish. */
static void store_grow(struct volume *v) {
    if (!medium.disk || volume_writable(v) < 0) return;
    uint8_t *buf = kmalloc(16 * 512);
    struct gpt g = {0};
    struct gpt_entry *mine = NULL;
    uint64_t room = buf && gpt_read(medium.disk, buf, &g) > 0 ? gpt_room(&g, &mine) : 0, was = v->super.blocks;
    uint64_t to = MAX(room, medium.dev.blocks);
    struct store_layout l = store_layout(to);
    /* room for the new map's two copies and the new last superblock (space_grow) */
    if (to > was + 2 * (l.indexes + l.maps)) {
        int r = volume_even(v);
        if (r >= 0 && room > medium.dev.blocks && (r = gpt_grow(&g, mine, buf, room)) >= 0) medium.dev.blocks = room;
        if (r >= 0) r = volume_grow(v, medium.dev.blocks);
        if (r >= 0) {
            kprintf("store: C: grew from %lu MiB to %lu MiB, the rest of %s\n", (unsigned long)(was * STORE_BLOCK >> 20),
                    (unsigned long)(v->super.blocks * STORE_BLOCK >> 20), medium.disk->name);
        } else {
            /* The partition may be larger than the volume now: an even commit cut short would leave
               nothing that finds the odd superblock of the size before. */
            kprintf("store: C: could not grow to the end of %s (%d): it is read only until the next start\n",
                    medium.disk->name, r);
            volume_broken(v, STORE_TROUBLE_UNFINISHED, 0, r);
        }
    }
    kfree(g.entries);
    kfree(buf);
}

/* The disk the system started from, by its GPT identity; any disk with a Nocturne partition when
   that is unknown or never appears. Started from a CD or DVD, C: is in memory: the volume the boot
   loader brought as the module "C:". */
static void device_find(void) {
    struct limine_module_response *mods = module_req.response;
    for (uint64_t i = 0; mods && i < mods->module_count; i++) {
        struct limine_file *f = mods->modules[i];
        if (!f->string || strcmp(f->string, "C:")) continue;
        medium.base = f->address;
        medium.dev = (struct store_device){.blocks = f->size / STORE_BLOCK, .read = memory_read, .write = memory_write};
        return;
    }
    static const uint8_t none[16];
    uint8_t want[16] = {0};
    struct limine_executable_file_response *ef = exe_file_req.response;
    if (ef && ef->executable_file && ef->executable_file->media_type == LIMINE_MEDIA_TYPE_GENERIC)
        memcpy(want, (const void *)&ef->executable_file->gpt_disk_uuid, 16);
    bool known = memcmp(want, none, 16) != 0;
    uint8_t *buf = kmalloc(16 * 512);
    if (!buf) panic("store: no memory to find C:");
    uint64_t start = clock_now_ns(), blocks = 0;
    uint32_t looked = 0; /* the disks whose GPT was read; one that could not be is tried again */
    bool boot = false, said = false;
    for (;;) {
        for (int i = 0; i < blk_count() && !boot; i++) {
            struct blkdev *d = blk_get(i);
            uint8_t guid[16];
            uint64_t first, n;
            bool kept = i < 32; /* the disks past the 32nd are read every time */
            int r = kept && (looked >> i & 1) ? 0 : gpt_find(d, buf, guid, &first, &n);
            if (r >= 0 && kept) looked |= 1u << i;
            if (r <= 0) continue;
            boot = !known || !memcmp(guid, want, 16);
            if (boot || !medium.disk) {
                medium.disk = d;
                medium.first = first;
                blocks = n;
            }
        }
        /* A USB stick appears once its bus has been looked at, which can take longer than FIND_NS
           on a machine with many devices. */
        uint64_t waited = clock_now_ns() - start;
        if (boot || waited >= FIND_MOST_NS || (waited >= FIND_NS && !usb_enumerating())) break;
        if (waited >= FIND_NS && !said) {
            said = true;
            kprintf("store: waiting for the disk the system started from: USB devices are still being looked at\n");
        }
        sleep_until(clock_after(100 * NS_PER_MS));
    }
    kfree(buf);
    if (!medium.disk) panic("store: no disk holds a Nocturne volume (C:)");
    if (!boot) kprintf("store: the disk the system started from did not appear: C: is on %s\n", medium.disk->name);
    medium.dev = (struct store_device){.blocks = blocks, .read = part_read, .write = part_write, .persist = part_persist};
}

/* ---- the store's program: memory, and whom it tells of trouble ---- */

static void trouble(enum store_trouble t, uint64_t block, int error) {
    kprintf("store: C: %s (block %lu, %d)\n", store_trouble_text(t), (unsigned long)block, error);
}

static const struct store_env env = {.alloc = kmalloc, .realloc = krealloc, .free = kfree, .trouble = trouble};

int64_t store_now(void) { return clock_wall_ns(); }

/* ---- mounting ---- */

static void cursors_init(void);

void store_init(void) {
    device_find();
    /* clean blocks kept: a 16th of memory for a disk */
    size_t kept = medium.disk ? MAX((size_t)MEMORY_KEPT, (size_t)(pmm_total_pages() / 16)) : MEMORY_KEPT;
    if (!(store.blocks = cache_create(&medium.dev, kept))) panic("store: no memory for C:'s blocks");
    struct volume *v;
    int r = volume_mount(&env, &medium.dev, store.blocks, &v);
    if (r >= 0) r = ns_mount(v);
    if (r < 0) panic("store: C: on %s cannot be mounted (%d)", medium_name(), r);
    store.v = v;
    cursors_init();
    char label[sizeof v->super.label + 1] = {0};
    memcpy(label, v->super.label, sizeof v->super.label);
    kprintf("store: C: \"%s\" on %s, %lu MiB, %lu MiB free\n", label, medium_name(),
            (unsigned long)(v->super.blocks * STORE_BLOCK >> 20),
            (unsigned long)(space_available(v) * STORE_BLOCK >> 20));
}

/* The folder name of parent, made when missing, with attributes at least. */
static int folder_at(uint64_t parent, const char *name, uint16_t attributes, uint64_t *id) {
    struct volume *v = store.v;
    struct folder_lock *f = folder_lock(parent);
    struct ns_entry *e = kmalloc(sizeof *e);
    int r = f && e ? 0 : -N_ERR_NO_MEMORY;
    size_t n = strlen(name);
    if (r >= 0) r = ns_find(v, parent, name, n, e);
    if (r == -N_ERR_NOT_FOUND) {
        struct ns_told told = {0};
        change_begin();
        r = ns_create(v, parent, name, n, STORE_FOLDER, attributes, 0, store_now(), id, &told);
        change_end();
        entry_watch_publish(&told);
    } else if (r >= 0) {
        struct store_record rec;
        *id = e->id;
        if ((r = ns_record(v, e->id, &rec, NULL)) >= 0 && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r >= 0 && (rec.attributes & attributes) != attributes) {
            change_begin();
            r = ns_record_amend(v, e->id, &(struct ns_amend){.fields = NS_ATTRIBUTES, .attributes = attributes});
            change_end();
        }
    }
    if (f) folder_unlock(f);
    kfree(e);
    return r;
}

/* Everything in folder goes: nothing has any of it open yet (store_start). */
static int folder_clear(uint64_t folder) {
    struct folder_lock *f = folder_lock(folder);
    if (!f) return -N_ERR_NO_MEMORY;
    change_begin();
    int r = ns_clear(store.v, folder, store_now());
    change_end();
    folder_unlock(f);
    return r;
}

static char user_folder[sizeof "C:\\Users\\" + 255] = "C:\\"; /* a name: at most 255 bytes */
static char user_temp[sizeof user_folder + sizeof "\\AppData\\Temp"] = "C:\\";

const char *store_user_folder(void) { return user_folder; }
const char *store_user_temp(void) { return user_temp; }

/* The user's folder: the first in users that is there for apps, a new one when there is none. Its
   AppData\Temp is made ready, empty. */
static int user_ready(uint64_t users) {
    struct volume *v = store.v;
    struct ns_entry *e = kmalloc(sizeof *e);
    if (!e) return -N_ERR_NO_MEMORY;
    int r;
    struct store_record rec;
    for (const struct ns_entry *after = NULL; (r = ns_next(v, users, after, e)) >= 0; after = e)
        if ((r = ns_record(v, e->id, &rec, NULL)) < 0 || (rec.kind == STORE_FOLDER && !(rec.attributes & STORE_PRIVATE)))
            break;
    if (r == -N_ERR_NOT_FOUND) {
        e->n = strlen(strcpy(e->name, "User"));
        r = folder_at(users, e->name, 0, &e->id);
    }
    uint64_t data, temp;
    if (r >= 0) {
        ksnprintf(user_folder, sizeof user_folder, "C:\\Users\\%.*s", (int)e->n, e->name);
        if ((r = folder_at(e->id, "AppData", 0, &data)) >= 0 && (r = folder_at(data, "Temp", STORE_TEMPORARY, &temp)) >= 0 &&
            (r = folder_clear(temp)) >= 0)
            ksnprintf(user_temp, sizeof user_temp, "%s\\AppData\\Temp", user_folder);
    }
    kfree(e);
    return r;
}

static void writer(void *arg);
static void gate_close(void);
static void gate_open(void);

void store_start(void) {
    struct volume *v = store.v;
    /* not before: a hibernation image goes back to the volume as it was */
    mutex_lock(&store.committing);
    gate_close();
    store_grow(v);
    gate_open();
    mutex_unlock(&store.committing);
    uint64_t system = 0, users = 0;
    int r = folder_at(STORE_ID_ROOT, "Nocturne", STORE_SYSTEM, &system);
    if (r >= 0) r = folder_at(system, "Temp", STORE_SYSTEM | STORE_PRIVATE | STORE_TEMPORARY, &store.temp);
    if (r >= 0) r = folder_clear(store.temp);
    if (r < 0) kprintf("store: C:\\Nocturne\\Temp cannot be made ready (%d): nothing can be replaced\n", r);
    if ((r = folder_at(STORE_ID_ROOT, "Users", 0, &users)) >= 0) r = user_ready(users);
    if (r < 0) kprintf("store: the user's folder in C:\\Users cannot be made ready (%d)\n", r);
    if (!kthread_create("store", writer, NULL)) panic("store: no writer");
    store_dirty(false);
}

/* ---- the gate ---- */

uint64_t change_begin(void) {
    uint64_t flags = spin_lock(&store.gate);
    while (store.closing) {
        wq_wait_locked(&store.open, &store.gate);
        spin_take(&store.gate);
    }
    store.changes++;
    uint64_t g = __atomic_load_n(&store.v->generation, __ATOMIC_ACQUIRE);
    spin_unlock(&store.gate, flags);
    return g;
}

void change_end(void) {
    uint64_t flags = spin_lock(&store.gate);
    bool last = !--store.changes && store.closing;
    spin_unlock(&store.gate, flags);
    if (last) wq_wake_all(&store.drained);
}

/* No change under way from gate_close to gate_open, and none begins: the changes under way end
   first, and the ones that come after wait. store.committing held. */
static void gate_close(void) {
    uint64_t flags = spin_lock(&store.gate);
    store.closing = true;
    while (store.changes) {
        wq_wait_locked(&store.drained, &store.gate);
        spin_take(&store.gate);
    }
    spin_unlock(&store.gate, flags);
}

static void gate_open(void) {
    uint64_t flags = spin_lock(&store.gate);
    store.closing = false;
    spin_unlock(&store.gate, flags);
    wq_wake_all(&store.open);
}

/* The open generation's commit (1: nothing changed in it). store.committing held. */
static int commit_locked(void) {
    struct volume *v = store.v;
    struct store_commit c;
    gate_close();
    int r = volume_freeze(v, &c);
    gate_open();
    if (!r) {
        r = volume_write(v, &c);
        volume_written(v, &c, r);
        r = r < 0 ? r : 0;
    }
    documents_committed();
    return r;
}

/* ---- the writer ---- */

void store_dirty(bool soon) {
    uint64_t flags = spin_lock(&store.dirty_lock);
    bool wake = !store.dirty || (soon && !store.soon);
    store.dirty = true;
    store.soon |= soon;
    spin_unlock(&store.dirty_lock, flags);
    if (wake) wq_wake_all(&store.kick);
}

/* It looks at dirty and soon under dirty_lock and is in kick before the lock is given back: a
   change on another CPU then finds it there. */
static void writer(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t flags = spin_lock(&store.dirty_lock);
        while (!store.dirty) {
            wq_wait_locked(&store.kick, &store.dirty_lock);
            spin_take(&store.dirty_lock);
        }
        for (uint64_t until = clock_after(WRITE_DELAY_NS); !store.soon && clock_now_ns() < until;) {
            wq_wait_locked_until(&store.kick, &store.dirty_lock, until);
            spin_take(&store.dirty_lock);
        }
        store.dirty = store.soon = false;
        spin_unlock(&store.dirty_lock, flags);
        documents_flush();
        store_commit();
    }
}

int store_commit(void) {
    mutex_lock(&store.committing);
    int r = commit_locked();
    mutex_unlock(&store.committing);
    return r < 0 ? r : 0;
}

int store_wait(uint64_t generation) {
    struct volume *v = store.v;
    mutex_lock(&store.committing);
    /* with the commits held off, every closed generation is on the medium (or the volume is read
       only): the one waited for is the open one when it is not there yet */
    int r = 0;
    if (__atomic_load_n(&v->durable, __ATOMIC_ACQUIRE) < generation && (r = volume_writable(v)) >= 0) {
        r = commit_locked();
        if (r == 1) r = 0; /* the open generation changed nothing: nothing to wait for */
    }
    mutex_unlock(&store.committing);
    return r;
}

int store_sync(void) {
    if (!store.v) return 0;
    int r = documents_flush();
    int c = store_wait(__atomic_load_n(&store.v->generation, __ATOMIC_ACQUIRE));
    return r < 0 ? r : c;
}

uint64_t store_temp_name(char *out, size_t *n) {
    uint64_t number = __atomic_add_fetch(&store.temp_names, 1, __ATOMIC_RELAXED);
    *n = store_temp_text(number, out);
    return number;
}

size_t store_temp_text(uint64_t number, char *out) {
    return (size_t)ksnprintf(out, 17, "%llx", (unsigned long long)number);
}

/* ---- folders' locks ---- */

struct folder_lock {
    struct id_link link; /* by the folder's id */
    unsigned users;      /* who holds it or waits for it (the table's lock) */
    struct mutex mutex;
};

static struct {
    struct spinlock lock;
    struct id_table table;
} folders;

static struct folder_lock *folder_of(struct id_link *l) {
    return (struct folder_lock *)((char *)l - offsetof(struct folder_lock, link));
}

struct folder_lock *folder_lock(uint64_t folder) {
    struct folder_lock *fresh = NULL, *l = NULL;
    for (;;) {
        uint64_t flags = 0;
        int r = fresh ? id_table_room(&folders.table, &folders.lock, &flags) : 0;
        if (!fresh) flags = spin_lock(&folders.lock);
        struct id_link *found = r >= 0 ? id_table_find(&folders.table, folder) : NULL;
        if (found) {
            l = folder_of(found);
            l->users++;
        } else if (fresh && r >= 0) {
            l = fresh;
            fresh = NULL;
            id_table_add(&folders.table, &l->link);
        }
        spin_unlock(&folders.lock, flags);
        if (l || r < 0) break;
        if (!(fresh = kzalloc(sizeof *fresh))) return NULL;
        fresh->link.id = folder;
        fresh->users = 1;
    }
    kfree(fresh);
    if (l) mutex_lock(&l->mutex);
    return l;
}

void folder_unlock(struct folder_lock *l) {
    mutex_unlock(&l->mutex);
    uint64_t flags = spin_lock(&folders.lock);
    bool last = !--l->users;
    if (last) id_table_drop(&folders.table, &l->link);
    spin_unlock(&folders.lock, flags);
    if (last) kfree(l);
}

/* ---- paths ---- */

int place_find(const char *text, bool user, struct place *out) {
    struct volume *v = store.v;
    memset(out, 0, sizeof *out);
    int r = path_parse(&env, text, &out->path);
    if (r < 0) return r;
    out->folder = out->entry.id = STORE_ID_ROOT;
    if ((r = ns_record(v, STORE_ID_ROOT, &out->record, NULL)) < 0) goto fail;
    out->folder_attributes = out->record.attributes;
    out->found = true;
    for (size_t i = 0; i < out->path.count; i++) {
        if (out->record.kind != STORE_FOLDER) {
            r = -N_ERR_NOT_FOLDER;
            goto fail;
        }
        out->folder = out->entry.id;
        out->depth = i;
        out->folder_attributes = out->record.attributes;
        out->name = out->path.part[i].name;
        out->n = out->path.part[i].n;
        bool last = i + 1 == out->path.count;
        r = ns_find(v, out->folder, out->name, out->n, &out->entry);
        if (r >= 0) r = ns_record(v, out->entry.id, &out->record, NULL);
        if (r >= 0 && user && (out->record.attributes & STORE_PRIVATE)) r = -N_ERR_NOT_FOUND;
        if (r == -N_ERR_NOT_FOUND && last) {
            out->found = false;
            out->entry.id = 0;
            return 0;
        }
        if (r < 0) goto fail;
    }
    return 0;
fail:
    path_free(&env, &out->path);
    return r;
}

void place_free(struct place *p) { path_free(&env, &p->path); }

int place_lock(struct place *p, bool user, struct folder_lock **out) {
    struct volume *v = store.v;
    if (!(*out = folder_lock(p->folder))) return -N_ERR_NO_MEMORY;
    struct store_record rec;
    int r = ns_record(v, p->folder, &rec, NULL);
    if (r >= 0 && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOUND;
    if (r >= 0) p->folder_attributes = rec.attributes;
    if (r >= 0 && p->n) {
        r = ns_find(v, p->folder, p->name, p->n, &p->entry);
        if (r >= 0) r = ns_record(v, p->entry.id, &p->record, NULL);
        if (r >= 0 && user && (p->record.attributes & STORE_PRIVATE)) r = -N_ERR_NOT_FOUND;
        p->found = r >= 0;
        if (r == -N_ERR_NOT_FOUND) {
            p->entry.id = 0;
            r = 0;
        }
    }
    if (r < 0) {
        folder_unlock(*out);
        *out = NULL;
    }
    return r;
}

int place_may_change(const struct place *p, bool user, bool entry) {
    if (!user) return 0;
    if (p->folder_attributes & STORE_SYSTEM) return -N_ERR_DENIED;
    return entry && p->found && (p->record.attributes & STORE_SYSTEM) ? -N_ERR_DENIED : 0;
}

void entry_fill(struct n_entry *out, const char *name, size_t n, uint64_t id, const struct store_record *rec) {
    memset(out, 0, sizeof *out);
    if (n) memcpy(out->name, name, MIN(n, sizeof out->name - 1));
    out->kind = rec->kind == STORE_FOLDER ? N_KIND_FOLDER : N_KIND_DOCUMENT;
    if (rec->kind == STORE_DOCUMENT) {
        int64_t now = document_size_now(id);
        out->size = now >= 0 ? (uint64_t)now : rec->size;
    }
    int64_t seconds = rec->modified / 1000000000, ns = rec->modified % 1000000000;
    if (ns < 0) { ns += 1000000000; seconds--; } /* before 1970: the second below */
    out->modified = seconds;
    out->modified_ns = (uint32_t)ns;
    out->id = id;
}

int store_entry_info(const char *path, bool user, struct n_entry *out) {
    struct place p;
    int r = place_find(path, user, &p);
    if (r >= 0) {
        if (!p.found) r = -N_ERR_NOT_FOUND;
        else entry_fill(out, p.entry.name, p.entry.n, p.entry.id, &p.record); /* as stored, not as typed */
        place_free(&p);
    }
    return r;
}

/* Where listings stopped: one that goes on from where another stopped needs no walk from the
   start. About two for each CPU are kept (no more list at once), the least recently used first. */
struct cursor {
    uint64_t folder, index; /* the entries listed before it */
    bool user;
    struct ns_entry last;
};

static struct {
    struct spinlock lock;
    struct cursor *c; /* oldest first */
    size_t n, room;
} cursors;

static void cursors_init(void) {
    cursors.room = 2 * (size_t)cpu_online_count();
    if (!(cursors.c = kmalloc(cursors.room * sizeof *cursors.c))) cursors.room = 0;
}

/* The cursor where a listing of folder stopped after index entries, taken out (false: none). */
static bool cursor_take(uint64_t folder, uint64_t index, bool user, struct ns_entry *last) {
    uint64_t flags = spin_lock(&cursors.lock);
    size_t i = 0;
    while (i < cursors.n && !(cursors.c[i].folder == folder && cursors.c[i].index == index && cursors.c[i].user == user)) i++;
    bool found = i < cursors.n;
    if (found) {
        *last = cursors.c[i].last;
        memmove(&cursors.c[i], &cursors.c[i + 1], (cursors.n - i - 1) * sizeof *cursors.c);
        cursors.n--;
    }
    spin_unlock(&cursors.lock, flags);
    return found;
}

static void cursor_keep(uint64_t folder, uint64_t index, bool user, const struct ns_entry *last) {
    uint64_t flags = spin_lock(&cursors.lock);
    if (cursors.room) {
        if (cursors.n == cursors.room) memmove(&cursors.c[0], &cursors.c[1], --cursors.n * sizeof *cursors.c);
        cursors.c[cursors.n++] = (struct cursor){.folder = folder, .index = index, .user = user, .last = *last};
    }
    spin_unlock(&cursors.lock, flags);
}

int64_t store_folder_list(const char *path, uint64_t start, void *out, size_t max, size_t bytes, bool user) {
    struct volume *v = store.v;
    struct place p;
    int r = place_find(path, user, &p);
    uint64_t folder = 0;
    if (r >= 0) {
        r = !p.found ? -N_ERR_NOT_FOUND : p.record.kind != STORE_FOLDER ? -N_ERR_NOT_FOLDER : 0;
        folder = p.entry.id;
        place_free(&p);
    }
    struct ns_entry *e = r >= 0 ? kmalloc(2 * sizeof *e) : NULL, *last = e ? e + 1 : NULL;
    if (r >= 0 && !e) r = -N_ERR_NO_MEMORY;
    bool started = r >= 0 && start && cursor_take(folder, start, user, last);
    uint64_t index = started ? start : 0;
    size_t got = 0;
    while (r >= 0 && got < max) {
        if ((r = ns_next(v, folder, started ? last : NULL, e)) == -N_ERR_NOT_FOUND) {
            r = 0; /* the end of the folder */
            break;
        }
        if (r < 0) break;
        *last = *e;
        started = true;
        struct store_record rec;
        if ((r = ns_record(v, e->id, &rec, NULL)) == -N_ERR_NOT_FOUND) { /* being removed: its name goes first */
            r = 0;
            continue;
        }
        if (r < 0) break;
        if ((user && (rec.attributes & STORE_PRIVATE)) || index++ < start) continue;
        struct n_entry ne;
        entry_fill(&ne, e->name, e->n, e->id, &rec);
        if ((r = copy_out((uint8_t *)out + got * bytes, &ne, bytes, user)) < 0) break;
        got++;
    }
    if (r >= 0 && got) cursor_keep(folder, index, user, last);
    kfree(e);
    return r < 0 ? r : (int64_t)got;
}

int store_folder_create(const char *path, bool user) {
    struct place p;
    int r = place_find(path, user, &p);
    if (r < 0) return r;
    struct folder_lock *f = NULL;
    if ((r = place_lock(&p, user, &f)) >= 0) {
        r = p.found ? -N_ERR_EXISTS : place_may_change(&p, user, false);
        if (r >= 0) {
            uint64_t id;
            struct ns_told told = {0};
            change_begin();
            r = ns_create(store.v, p.folder, p.name, p.n, STORE_FOLDER, 0, 0, store_now(), &id, &told);
            change_end();
            entry_watch_publish(&told);
        }
        folder_unlock(f);
    }
    place_free(&p);
    if (r >= 0) store_dirty(false);
    return r;
}

int store_entry_remove(const char *path, bool user) {
    struct volume *v = store.v;
    struct place p;
    int r = place_find(path, user, &p);
    if (r < 0) return r;
    struct folder_lock *f = NULL;
    if (!p.n) r = -N_ERR_DENIED;
    else if ((r = place_lock(&p, user, &f)) >= 0)
        r = !p.found ? -N_ERR_NOT_FOUND : place_may_change(&p, user, true);
    if (r >= 0) r = volume_writable(v);
    struct ns_told told = {0};
    if (r >= 0 && p.record.kind == STORE_FOLDER) {
        /* its own names held still as well: nothing goes into it meanwhile */
        struct folder_lock *in = folder_lock(p.entry.id);
        if (!in) r = -N_ERR_NO_MEMORY;
        if (r >= 0) {
            struct ranges dropped = {0};
            change_begin();
            r = ns_remove(v, p.folder, p.name, p.n, store_now(), &dropped, &told);
            change_end();
            space_release_list(v, &dropped);
            folder_unlock(in);
        }
    } else if (r >= 0) {
        r = document_remove(p.folder, p.name, p.n, p.entry.id, &told); /* an open document goes with its last opening */
    }
    entry_watch_publish(&told);
    if (f) folder_unlock(f);
    place_free(&p);
    if (r >= 0) store_dirty(false);
    return r < 0 ? r : 0;
}

/* a and b's folders locked: one that holds the other first, two others by depth and id */
static int lock_pair(struct place *a, struct place *b, bool user, struct folder_lock **la, struct folder_lock **lb) {
    *la = *lb = NULL;
    if (a->folder == b->folder) {
        int r = place_lock(a, user, la);
        if (r >= 0 && (r = place_lock(b, user, lb)) < 0) { /* the same mutex, taken again */
            folder_unlock(*la);
            *la = NULL;
        }
        return r;
    }
    bool a_first = a->depth != b->depth ? a->depth < b->depth : a->folder < b->folder;
    struct place *first = a_first ? a : b, *second = a_first ? b : a;
    struct folder_lock **l1 = a_first ? la : lb, **l2 = a_first ? lb : la;
    int r = place_lock(first, user, l1);
    if (r >= 0 && (r = place_lock(second, user, l2)) < 0) {
        folder_unlock(*l1);
        *l1 = NULL;
    }
    return r;
}

int store_entry_move(const char *from, const char *to, bool replace, bool user) {
    struct volume *v = store.v;
    struct place a, b;
    bool across = false;
    int r;
    for (;;) {
        if ((r = place_find(from, user, &a)) < 0) break;
        if ((r = place_find(to, user, &b)) < 0) {
            place_free(&a);
            break;
        }
        /* From one folder to another, one move at a time: no folder can then go into one that goes
           into it, and the folders' depths stay as found. */
        if (across || a.folder == b.folder) break;
        place_free(&a);
        place_free(&b);
        mutex_lock(&store.moves);
        across = true;
    }
    if (r < 0) {
        if (across) mutex_unlock(&store.moves);
        return r;
    }
    struct folder_lock *la = NULL, *lb = NULL, *lt = NULL;
    if (!a.n || !b.n) r = !a.n ? -N_ERR_DENIED : -N_ERR_EXISTS;
    else r = lock_pair(&a, &b, user, &la, &lb);
    if (r >= 0) r = !a.found ? -N_ERR_NOT_FOUND : place_may_change(&a, user, true);
    /* the same entry may take another case of its name */
    bool over = r >= 0 && b.found && b.entry.id != a.entry.id;
    if (r >= 0 && over && !(replace && a.record.kind == STORE_DOCUMENT && b.record.kind == STORE_DOCUMENT)) r = -N_ERR_EXISTS;
    if (r >= 0) r = place_may_change(&b, user, over);
    if (r >= 0) r = volume_writable(v);
    struct ns_told told = {0};
    if (r >= 0 && over) {
        /* the document replaced goes to the temp folder in the same change, or nothing changes;
           from there as store_entry_remove takes it */
        if (!(lt = folder_lock(store.temp))) r = -N_ERR_NO_MEMORY;
        char temp[17];
        size_t k;
        uint64_t number = store_temp_name(temp, &k), g = 0;
        if (r >= 0) {
            g = change_begin();
            r = ns_replace(v, a.folder, a.name, a.n, b.folder, b.name, b.n, store.temp, temp, k, store_now(), &told);
            change_end();
        }
        if (r >= 0) {
            document_aside(b.entry.id, number, g);
            document_changed(a.entry.id, g);
        }
    } else if (r >= 0) {
        uint64_t g = change_begin();
        r = ns_move(v, a.folder, a.name, a.n, b.folder, b.name, b.n, store_now(), &told);
        change_end();
        if (r >= 0) document_changed(a.entry.id, g);
    }
    entry_watch_publish(&told);
    if (lt) folder_unlock(lt);
    if (lb) folder_unlock(lb);
    if (la) folder_unlock(la);
    if (across) mutex_unlock(&store.moves);
    place_free(&a);
    place_free(&b);
    if (r >= 0) store_dirty(false);
    return r;
}

/* ---- whole documents ---- */

static int write_all(struct handle *h, uint64_t offset, const void *data, size_t n, bool user) {
    int64_t w = n ? store_document_write(h, offset, data, n, user) : 0;
    return w < 0 ? (int)w : (size_t)w != n ? -N_ERR_IO : 0;
}

int store_save(const char *path, const void *data, size_t n, bool user) {
    struct handle *h;
    int r = document_open(path, N_DOCUMENT_REPLACE, user, &h);
    if (r < 0) return r;
    r = write_all(h, 0, data, n, user);
    if (r >= 0) r = store_document_commit(h, N_COMMIT_WAIT);
    handle_close(h);
    return r;
}

int store_append(const char *path, const void *data, size_t n, bool user) {
    struct handle *h;
    int r = document_open(path, N_DOCUMENT_EDIT, user, &h);
    if (r == -N_ERR_NOT_FOUND) r = document_open(path, N_DOCUMENT_CREATE, user, &h);
    if (r == -N_ERR_EXISTS) r = document_open(path, N_DOCUMENT_EDIT, user, &h); /* made meanwhile */
    if (r < 0) return r;
    if ((r = store_document_claim(h, true)) >= 0) {
        int64_t size = store_document_size(h);
        r = size < 0 ? (int)size : write_all(h, (uint64_t)size, data, n, user);
        store_document_release(h);
    }
    handle_close(h);
    return r;
}

/* ---- raw documents ---- */

static int raw_extents(struct volume *v, uint64_t id, uint64_t size, struct store_raw *out) {
    struct ns_extent *list;
    size_t n;
    int r = ns_extents(v, id, &list, &n);
    if (r < 0) return r;
    out->extents = kmalloc((n ? n : 1) * sizeof *out->extents);
    out->n = 0;
    uint64_t next = 0;
    r = out->extents ? 0 : -N_ERR_NO_MEMORY;
    for (size_t i = 0; i < n && r >= 0; i++) {
        if (!list[i].block || list[i].index != next) { /* a raw document has no holes */
            r = -N_ERR_IO;
            break;
        }
        uint64_t lba = medium.first + list[i].block * (STORE_BLOCK / 512), sectors = list[i].count * (STORE_BLOCK / 512);
        struct store_raw_extent *last = out->n ? &out->extents[out->n - 1] : NULL;
        if (last && last->lba + last->sectors == lba) last->sectors += sectors;
        else out->extents[out->n++] = (struct store_raw_extent){lba, sectors};
        next += list[i].count;
    }
    if (r >= 0 && next * STORE_BLOCK < size) r = -N_ERR_IO;
    kfree(list);
    if (r < 0) {
        kfree(out->extents);
        out->extents = NULL;
        return r;
    }
    out->disk = medium.disk;
    out->size = size;
    return 0;
}

/* the raw document at path, or (found false) where it would go */
static bool raw_is(const struct place *p) {
    return !p->found || (p->record.kind == STORE_DOCUMENT && (p->record.flags & STORE_UNCHECKED));
}

bool store_raw_possible(void) { return medium.disk != NULL; }

int store_raw_find(const char *path, struct store_raw *out) {
    memset(out, 0, sizeof *out);
    if (!medium.disk) return -N_ERR_UNSUPPORTED;
    struct place p;
    int r = place_find(path, false, &p);
    if (r >= 0) {
        r = !raw_is(&p) ? -N_ERR_EXISTS : p.found ? raw_extents(store.v, p.entry.id, p.record.size, out) : -N_ERR_NOT_FOUND;
        place_free(&p);
    }
    return r;
}

int store_raw_place(const char *path, uint64_t size, struct store_raw *out) {
    memset(out, 0, sizeof *out);
    if (!medium.disk) return -N_ERR_UNSUPPORTED;
    struct volume *v = store.v;
    struct place p;
    int r = volume_writable(v);
    if (r >= 0) r = place_find(path, false, &p);
    if (r < 0) return r;
    struct folder_lock *f = NULL;
    if ((r = place_lock(&p, false, &f)) >= 0 && !raw_is(&p)) r = -N_ERR_EXISTS; /* something else has the name */
    uint64_t id = p.entry.id;
    if (r >= 0 && !p.found) {
        struct ns_told told = {0};
        change_begin();
        r = ns_create(v, p.folder, p.name, p.n, STORE_DOCUMENT, STORE_PRIVATE, STORE_UNCHECKED, store_now(), &id, &told);
        change_end();
        entry_watch_publish(&told);
    }
    if (f) folder_unlock(f);
    place_free(&p);
    uint64_t g = 0;
    if (r >= 0) r = document_raw(id, size, &g);
    if (r >= 0) r = raw_extents(v, id, size, out);
    /* where the blocks are is on the medium before anything is written to them */
    if (r >= 0 && (r = store_wait(g)) < 0) store_raw_free(out);
    return r;
}

void store_raw_free(struct store_raw *r) {
    kfree(r->extents);
    r->extents = NULL;
    r->n = 0;
}
