/* The store (fs/store.h): C: on the disk the system started from (or in memory, started from a CD
   or DVD), what is done to it by path, and its writer. Open documents are fs/document.c's. */
#include "fs/document.h"
#include "arch/cpu.h"
#include "fs/space.h"
#include "fs/watch.h"
#include "dev/blk.h"
#include "dev/timer.h"
#include "dev/usb.h"
#include "limine.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sys/object.h"

#define WRITE_DELAY_MS 5000 /* the first change waits this long for more to commit with it */
#define FIND_MS 10000       /* how long the disk the system started from may take to appear */
#define FIND_MOST_MS 60000  /* ... while USB devices are still being looked at */
#define GPT_ENTRIES_MAX (1u << 20)

extern volatile struct limine_executable_file_request exe_file_req;
extern volatile struct limine_module_request module_req;

struct store store;

/* ---- the medium C: is on ---- */

static struct store_device device;

/* a partition: the Nocturne partition of the disk the system started from */
static struct {
    struct blkdev *disk; /* NULL: C: is in memory */
    uint64_t first;      /* its first sector */
} part;

static int part_read(struct store_device *d, uint64_t block, uint32_t count, void *buf) {
    return blk_read(part.disk, part.first + block * (STORE_BLOCK / 512), count * (STORE_BLOCK / 512), buf);
}

static int part_write(struct store_device *d, uint64_t block, uint32_t count, const void *buf) {
    return blk_write(part.disk, part.first + block * (STORE_BLOCK / 512), count * (STORE_BLOCK / 512), buf);
}

static int part_persist(struct store_device *d) { return blk_persist(part.disk); }

/* memory: the volume the boot loader brought from a CD or DVD (priv). What changes on it lasts
   until the machine stops. */
static int memory_read(struct store_device *d, uint64_t block, uint32_t count, void *buf) {
    memcpy(buf, (const uint8_t *)d->priv + block * STORE_BLOCK, (size_t)count * STORE_BLOCK);
    return 0;
}

static int memory_write(struct store_device *d, uint64_t block, uint32_t count, const void *buf) {
    memcpy((uint8_t *)d->priv + block * STORE_BLOCK, buf, (size_t)count * STORE_BLOCK);
    return 0;
}

/* where C: is, for messages */
static const char *medium(void) { return part.disk ? part.disk->name : "memory"; }

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
    struct blkdev *disk = part.disk;
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
        if (!memcmp(e->type, store_type, 16) && e->first * f == part.first) *mine = e;
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
    int r = blk_write(part.disk, lba * f, (uint32_t)(lbas * f), data);
    return r < 0 ? r : blk_persist(part.disk);
}

/* C:'s partition to blocks (gpt_room's), its GPT's copy at the end of the disk. buf: gpt_read's. */
static int gpt_grow(struct gpt *g, struct gpt_entry *mine, uint8_t *buf, uint64_t blocks) {
    struct blkdev *disk = part.disk;
    uint64_t f = g->unit / 512, last = disk->sectors / f - 1, lbas = g->sectors / f;
    uint8_t *block = kzalloc(MAX(g->unit, STORE_BLOCK));
    if (!block) return -N_ERR_NO_MEMORY;
    /* no superblock of what the disk held before at C:'s new last block */
    int r = blk_write(disk, part.first + (blocks - 1) * (STORE_BLOCK / 512), STORE_BLOCK / 512, block);
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
   partition, then the volume (fs/volume.h says in which order). The next start tries again what
   this one could not finish. */
static void store_grow(struct volume *v) {
    if (!part.disk || volume_writable(v) < 0) return;
    uint8_t *buf = kmalloc(16 * 512);
    struct gpt g = {0};
    struct gpt_entry *mine = NULL;
    uint64_t room = buf && gpt_read(part.disk, buf, &g) > 0 ? gpt_room(&g, &mine) : 0, was = v->super.blocks;
    uint64_t to = MAX(room, device.blocks);
    struct store_layout l = store_layout(to);
    /* room for the new map's two copies and the new last superblock (space_grow) */
    if (to > was + 2 * (l.indexes + l.maps)) {
        int r = volume_even(v);
        if (r >= 0 && room > device.blocks && (r = gpt_grow(&g, mine, buf, room)) >= 0) device.blocks = room;
        if (r >= 0) r = volume_grow(v, device.blocks);
        if (r >= 0) {
            kprintf("store: C: grew from %lu MiB to %lu MiB, the rest of %s\n", (unsigned long)(was * STORE_BLOCK >> 20),
                    (unsigned long)(v->super.blocks * STORE_BLOCK >> 20), part.disk->name);
        } else {
            /* The partition may be larger than the volume now: an even commit cut short would leave
               nothing that finds the odd superblock of the size before. */
            kprintf("store: C: could not grow to the end of %s (%d): it is read only until the next start\n",
                    part.disk->name, r);
            mutex_lock(&v->lock);
            v->error = -N_ERR_READ_ONLY;
            mutex_unlock(&v->lock);
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
        device = (struct store_device){.blocks = f->size / STORE_BLOCK, .read = memory_read, .write = memory_write, .priv = f->address};
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
    uint64_t start = uptime_ms(), blocks = 0;
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
            if (boot || !part.disk) {
                part.disk = d;
                part.first = first;
                blocks = n;
            }
        }
        /* A USB stick appears once its bus has been looked at, which can take longer than FIND_MS
           on a machine with many devices. */
        uint64_t waited = uptime_ms() - start;
        if (boot || waited >= FIND_MOST_MS || (waited >= FIND_MS && !usb_enumerating())) break;
        if (waited >= FIND_MS && !said) {
            said = true;
            kprintf("store: waiting for the disk the system started from: USB devices are still being looked at\n");
        }
        sleep_ms(100);
    }
    kfree(buf);
    if (!part.disk) panic("store: no disk holds a Nocturne volume (C:)");
    if (!boot) kprintf("store: the disk the system started from did not appear: C: is on %s\n", part.disk->name);
    device = (struct store_device){.blocks = blocks, .read = part_read, .write = part_write, .persist = part_persist};
}

/* ---- mounting ---- */

void store_init(void) {
    device_find();
    struct volume *v;
    int r = volume_mount(&device, &v);
    if (r >= 0) {
        mutex_lock(&v->lock);
        r = ns_mount(v);
        mutex_unlock(&v->lock);
    }
    if (r < 0) panic("store: C: on %s cannot be mounted (%d)", medium(), r);
    /* clean blocks kept: a 16th of memory for a disk; in memory, reading one again is a copy */
    if (part.disk) v->limit = MAX(v->limit, (size_t)(pmm_total_pages() / 16));
    entry_watch_start(v);
    store.v = v;
    char label[sizeof v->super.label + 1] = {0};
    memcpy(label, v->super.label, sizeof v->super.label);
    kprintf("store: C: \"%s\" on %s, %lu MiB, %lu MiB free\n", label, medium(),
            (unsigned long)(v->super.blocks * STORE_BLOCK >> 20),
            (unsigned long)(space_available(v) * STORE_BLOCK >> 20));
}

/* The folder name of parent, made when missing, with attributes at least. */
static int folder_at(struct volume *v, uint64_t parent, const char *name, uint16_t attributes, uint64_t *id) {
    struct ns_entry *e = kmalloc(sizeof *e);
    if (!e) return -N_ERR_NO_MEMORY;
    size_t n = strlen(name);
    int r = ns_find(v, parent, name, n, e);
    if (r == -N_ERR_NOT_FOUND) {
        r = ns_create(v, parent, name, n, STORE_FOLDER, attributes, 0, id);
    } else if (r >= 0) {
        struct store_record rec;
        *id = e->id;
        if ((r = ns_record(v, e->id, &rec, NULL)) >= 0 && rec.kind != STORE_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r >= 0 && (rec.attributes & attributes) != attributes) {
            rec.attributes |= attributes;
            r = ns_record_put(v, e->id, &rec, NULL, 0);
        }
    }
    kfree(e);
    return r;
}

static char user_folder[sizeof "C:\\Users\\" + 255] = "C:\\"; /* a name: at most 255 bytes */
static char user_temp[sizeof user_folder + sizeof "\\AppData\\Temp"] = "C:\\";

const char *store_user_folder(void) { return user_folder; }
const char *store_user_temp(void) { return user_temp; }

/* The user's folder: the first in users that is there for apps, a new one when there is none. Its
   AppData\Temp is made ready, empty. */
static int user_ready(struct volume *v, uint64_t users) {
    struct ns_entry *e = kmalloc(sizeof *e);
    if (!e) return -N_ERR_NO_MEMORY;
    int r;
    struct store_record rec;
    for (const struct ns_entry *after = NULL; (r = ns_next(v, users, after, e)) >= 0; after = e)
        if ((r = ns_record(v, e->id, &rec, NULL)) < 0 || (rec.kind == STORE_FOLDER && !(rec.attributes & STORE_PRIVATE)))
            break;
    if (r == -N_ERR_NOT_FOUND) {
        e->n = strlen(strcpy(e->name, "User"));
        r = folder_at(v, users, e->name, 0, &e->id);
    }
    uint64_t data, temp;
    if (r >= 0) {
        ksnprintf(user_folder, sizeof user_folder, "C:\\Users\\%.*s", (int)e->n, e->name);
        if ((r = folder_at(v, e->id, "AppData", 0, &data)) >= 0 && (r = folder_at(v, data, "Temp", STORE_TEMPORARY, &temp)) >= 0 &&
            (r = ns_clear(v, temp)) >= 0)
            ksnprintf(user_temp, sizeof user_temp, "%s\\AppData\\Temp", user_folder);
    }
    kfree(e);
    return r;
}

static void writer(void *arg);

void store_start(void) {
    struct volume *v = store.v;
    store_grow(v); /* not before: a hibernation image goes back to the volume as it was */
    mutex_lock(&v->lock);
    uint64_t system = 0, users = 0;
    int r = folder_at(v, STORE_ID_ROOT, "Nocturne", STORE_SYSTEM, &system);
    if (r >= 0) r = folder_at(v, system, "Temp", STORE_SYSTEM | STORE_PRIVATE | STORE_TEMPORARY, &store.temp);
    if (r >= 0) r = ns_clear(v, store.temp);
    if (r < 0) kprintf("store: C:\\Nocturne\\Temp cannot be made ready (%d): nothing can be replaced\n", r);
    if ((r = folder_at(v, STORE_ID_ROOT, "Users", 0, &users)) >= 0) r = user_ready(v, users);
    if (r < 0) kprintf("store: the user's folder in C:\\Users cannot be made ready (%d)\n", r);
    mutex_unlock(&v->lock);
    if (!kthread_create("store", writer, NULL)) panic("store: no writer");
    store_dirty(false);
}

/* ---- the writer ---- */

void store_dirty(bool soon) {
    uint64_t flags = irq_save();
    bool wake = !store.dirty || (soon && !store.soon);
    store.dirty = true;
    store.soon |= soon;
    if (wake) wq_wake_all(&store.kick);
    irq_restore(flags);
}

static void writer(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t flags = irq_save();
        while (!store.dirty) wq_wait(&store.kick);
        for (uint64_t until = uptime_ms() + WRITE_DELAY_MS, now; !store.soon && (now = uptime_ms()) < until;)
            wq_wait_timeout(&store.kick, until - now);
        store.dirty = store.soon = false;
        irq_restore(flags);
        mutex_lock(&store.v->lock);
        documents_flush();
        mutex_unlock(&store.v->lock);
        store_commit();
    }
}

int store_commit(void) {
    int r = volume_commit(store.v);
    documents_committed();
    return r;
}

int store_wait(uint64_t generation) {
    struct volume *v = store.v;
    for (;;) {
        mutex_lock(&v->lock);
        /* an open generation that changed nothing has nothing to wait for */
        int r = v->durable >= generation ? 0
                : v->error                ? v->error
                : generation >= v->generation && !v->changed && !v->committing ? 0
                                                                                : 1;
        mutex_unlock(&v->lock);
        if (r <= 0) return r;
        if ((r = store_commit()) < 0) return r;
    }
}

int store_sync(void) {
    if (!store.v) return 0;
    mutex_lock(&store.v->lock);
    int r = documents_flush();
    mutex_unlock(&store.v->lock);
    int c = store_commit();
    return r < 0 ? r : c;
}

int64_t store_clock(void) { return time_now() * 1000000000LL; }

size_t store_temp_name(uint64_t number, char *out) {
    return (size_t)ksnprintf(out, 17, "%llx", (unsigned long long)number);
}

/* ---- paths ---- */

int place_find(const char *text, bool user, struct place *out) {
    struct volume *v = store.v;
    memset(out, 0, sizeof *out);
    int r = path_parse(text, &out->path);
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
    path_free(&out->path);
    return r;
}

void place_free(struct place *p) { path_free(&p->path); }

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
    struct volume *v = store.v;
    struct place p;
    mutex_lock(&v->lock);
    int r = place_find(path, user, &p);
    if (r >= 0) {
        if (!p.found) r = -N_ERR_NOT_FOUND;
        else entry_fill(out, p.entry.name, p.entry.n, p.entry.id, &p.record); /* as stored, not as typed */
        place_free(&p);
    }
    mutex_unlock(&v->lock);
    return r;
}

/* Where the last listing stopped: a listing that goes on from there needs no walk from the start. */
static struct {
    uint64_t folder, index; /* the entries listed before it */
    bool user, valid;
    struct ns_entry last;
} cursor;

int64_t store_folder_list(const char *path, uint64_t start, void *out, size_t max, size_t bytes, bool user) {
    struct volume *v = store.v;
    struct place p;
    mutex_lock(&v->lock);
    int r = place_find(path, user, &p);
    uint64_t folder = 0;
    if (r >= 0) {
        r = !p.found ? -N_ERR_NOT_FOUND : p.record.kind != STORE_FOLDER ? -N_ERR_NOT_FOLDER : 0;
        folder = p.entry.id;
        place_free(&p);
    }
    struct ns_entry *e = r >= 0 ? kmalloc(sizeof *e) : NULL;
    if (r >= 0 && !e) r = -N_ERR_NO_MEMORY;
    bool resume = cursor.valid && cursor.folder == folder && cursor.user == user && cursor.index == start && start;
    uint64_t index = resume ? start : 0;
    bool started = resume;
    size_t got = 0;
    cursor.valid = false;
    while (r >= 0 && got < max) {
        if ((r = ns_next(v, folder, started ? &cursor.last : NULL, e)) == -N_ERR_NOT_FOUND) {
            r = 0; /* the end of the folder */
            break;
        }
        if (r < 0) break;
        struct store_record rec;
        if ((r = ns_record(v, e->id, &rec, NULL)) < 0) break;
        cursor.last = *e;
        started = true;
        if ((user && (rec.attributes & STORE_PRIVATE)) || index++ < start) continue;
        struct n_entry ne;
        entry_fill(&ne, e->name, e->n, e->id, &rec);
        if ((r = copy_out((uint8_t *)out + got * bytes, &ne, bytes, user)) < 0) break;
        got++;
    }
    if (r >= 0 && got) {
        cursor.folder = folder;
        cursor.index = index;
        cursor.user = user;
        cursor.valid = true;
    }
    kfree(e);
    mutex_unlock(&v->lock);
    return r < 0 ? r : (int64_t)got;
}

int store_folder_create(const char *path, bool user) {
    struct volume *v = store.v;
    struct place p;
    mutex_lock(&v->lock);
    int r = place_find(path, user, &p);
    if (r >= 0) {
        uint64_t id;
        r = p.found ? -N_ERR_EXISTS : place_may_change(&p, user, false);
        if (r >= 0) r = ns_create(v, p.folder, p.name, p.n, STORE_FOLDER, 0, 0, &id);
        place_free(&p);
    }
    mutex_unlock(&v->lock);
    if (r >= 0) store_dirty(false);
    return r;
}

int store_entry_remove(const char *path, bool user) {
    struct volume *v = store.v;
    struct place p;
    mutex_lock(&v->lock);
    int r = place_find(path, user, &p);
    if (r >= 0) {
        r = !p.found ? -N_ERR_NOT_FOUND : !p.n ? -N_ERR_DENIED : place_may_change(&p, user, true);
        if (r >= 0) r = volume_writable(v);
        /* an open document goes with its last opening */
        if (r >= 0 && p.record.kind == STORE_DOCUMENT) r = document_hide(p.folder, p.name, p.n, p.entry.id);
        if (r == 0) {
            struct ranges dropped = {0};
            r = ns_remove(v, p.folder, p.name, p.n, &dropped);
            space_release_list(v, &dropped);
        }
        place_free(&p);
    }
    mutex_unlock(&v->lock);
    if (r >= 0) store_dirty(false);
    return r < 0 ? r : 0;
}

int store_entry_move(const char *from, const char *to, bool replace, bool user) {
    struct volume *v = store.v;
    struct place a, b;
    mutex_lock(&v->lock);
    int r = place_find(from, user, &a);
    if (r >= 0) {
        r = !a.found ? -N_ERR_NOT_FOUND : !a.n ? -N_ERR_DENIED : place_may_change(&a, user, true);
        if (r >= 0 && (r = place_find(to, user, &b)) >= 0) {
            /* the same entry may take another case of its name */
            bool over = b.found && b.entry.id != a.entry.id;
            if (!b.n) r = -N_ERR_EXISTS;
            else if (over && !(replace && a.record.kind == STORE_DOCUMENT && b.record.kind == STORE_DOCUMENT))
                r = -N_ERR_EXISTS;
            else r = place_may_change(&b, user, over);
            if (r >= 0 && over) {
                /* the document replaced goes to the temp folder in the same change, or nothing
                   changes; from there as store_entry_remove takes it, in the commit of the move */
                char temp[17];
                uint64_t number = ++store.temp_names;
                size_t k = store_temp_name(number, temp);
                if ((r = ns_replace(v, a.folder, a.name, a.n, b.folder, b.name, b.n, store.temp, temp, k)) >= 0)
                    document_aside(b.entry.id, number);
            } else if (r >= 0) {
                r = ns_move(v, a.folder, a.name, a.n, b.folder, b.name, b.n);
            }
            if (r >= 0) document_changed(a.entry.id);
            place_free(&b);
        }
        place_free(&a);
    }
    mutex_unlock(&v->lock);
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

static int raw_extents(struct volume *v, uint64_t id, const struct store_record *rec, struct store_raw *out) {
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
        uint64_t lba = part.first + list[i].block * (STORE_BLOCK / 512), sectors = list[i].count * (STORE_BLOCK / 512);
        struct store_raw_extent *last = out->n ? &out->extents[out->n - 1] : NULL;
        if (last && last->lba + last->sectors == lba) last->sectors += sectors;
        else out->extents[out->n++] = (struct store_raw_extent){lba, sectors};
        next += list[i].count;
    }
    if (r >= 0 && next * STORE_BLOCK < rec->size) r = -N_ERR_IO;
    kfree(list);
    if (r < 0) {
        kfree(out->extents);
        out->extents = NULL;
        return r;
    }
    out->disk = part.disk;
    out->size = rec->size;
    return 0;
}

/* the raw document at path, or (found false) where it would go */
static int raw_find(struct volume *v, const char *path, struct place *p) {
    int r = place_find(path, false, p);
    if (r >= 0 && p->found && (p->record.kind != STORE_DOCUMENT || !(p->record.flags & STORE_UNCHECKED))) {
        place_free(p);
        r = -N_ERR_EXISTS; /* something else has the name */
    }
    return r;
}

bool store_raw_possible(void) { return part.disk != NULL; }

int store_raw_find(const char *path, struct store_raw *out) {
    memset(out, 0, sizeof *out);
    if (!part.disk) return -N_ERR_UNSUPPORTED;
    struct volume *v = store.v;
    struct place p;
    mutex_lock(&v->lock);
    int r = raw_find(v, path, &p);
    if (r >= 0) {
        r = p.found ? raw_extents(v, p.entry.id, &p.record, out) : -N_ERR_NOT_FOUND;
        place_free(&p);
    }
    mutex_unlock(&v->lock);
    return r;
}

int store_raw_place(const char *path, uint64_t size, struct store_raw *out) {
    memset(out, 0, sizeof *out);
    if (!part.disk) return -N_ERR_UNSUPPORTED;
    struct volume *v = store.v;
    struct place p;
    mutex_lock(&v->lock);
    int r = volume_writable(v);
    if (r >= 0) r = raw_find(v, path, &p);
    if (r < 0) {
        mutex_unlock(&v->lock);
        return r;
    }
    uint64_t id = p.entry.id, have = p.found ? (p.record.size + STORE_BLOCK - 1) / STORE_BLOCK : 0;
    uint64_t want = (size + STORE_BLOCK - 1) / STORE_BLOCK;
    if (!p.found) r = ns_create(v, p.folder, p.name, p.n, STORE_DOCUMENT, STORE_PRIVATE, STORE_UNCHECKED, &id);
    place_free(&p);
    struct ranges dropped = {0};
    if (r >= 0 && want < have) r = ns_cut(v, id, want, &dropped);
    if (r >= 0 && want > have && (r = space_reserve(v, want - have)) >= 0)
        r = ns_write(v, id, false, have, (size_t)(want - have), NULL, true, &dropped);
    space_release_list(v, &dropped);
    struct store_record rec;
    if (r >= 0 && (r = ns_record(v, id, &rec, NULL)) >= 0) {
        rec.size = size;
        rec.modified = store_clock();
        rec.flags = (uint16_t)((rec.flags & ~STORE_INLINE) | STORE_UNCHECKED);
        r = ns_record_put(v, id, &rec, NULL, 0);
    }
    if (r >= 0) r = raw_extents(v, id, &rec, out);
    mutex_unlock(&v->lock);
    /* where the blocks are is on the medium before anything is written to them */
    if (r >= 0 && (r = store_commit()) < 0) store_raw_free(out);
    return r;
}

void store_raw_free(struct store_raw *r) {
    kfree(r->extents);
    r->extents = NULL;
    r->n = 0;
}
