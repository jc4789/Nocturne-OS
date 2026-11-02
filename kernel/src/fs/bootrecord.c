/* The boot record (fs/bootrecord.h). Two slots: a new generation goes to the slot the last one is
   not in, so a write cut short leaves the last one whole. The document's clusters are found through
   the partition's FAT32 and never change: this only writes over what is there. */
#include "fs/bootrecord.h"
#include "fs/store.h"
#include "dev/blk.h"
#include "limine.h"
#include "mm/heap.h"
#include "store/crc32c.h"
#include "sys/settings.h"
#include "sys/wait.h"

extern volatile struct limine_module_request module_req;

static const int32_t defaults[N_BOOT_KEY_COUNT] = {
    [N_BOOT_IOMMU] = 1, [N_BOOT_ACPI_AML] = 1, [N_BOOT_SMP] = 1, [N_BOOT_RESUME] = 1, [N_BOOT_UNISOLATED_DRIVERS] = 0,
};

static int32_t now[N_BOOT_KEY_COUNT]; /* what this start goes by: set once in kmain */
static struct mutex write_lock;       /* the rest, one write at a time */
static struct n_boot_record wanted;   /* what the document says or is about to */
static unsigned slot;                 /* the slot wanted's generation is in */
static uint8_t part_id[16];           /* the module's partition */
static bool writable;                 /* there is a document to write to */

static uint32_t record_check(struct n_boot_record r) {
    r.check = 0;
    return crc32c(0, &r, sizeof r);
}

static bool record_valid(const struct n_boot_record *r) {
    if (r->magic != N_BOOT_RECORD_MAGIC || record_check(*r) != r->check) return false;
    for (unsigned k = 0; k < N_BOOT_KEY_COUNT; k++)
        if (r->value[k] != 0 && r->value[k] != 1) return false;
    return true;
}

void boot_record_read(void) {
    memcpy(now, defaults, sizeof now);
    memcpy(wanted.value, defaults, sizeof wanted.value);
    struct limine_module_response *mods = module_req.response;
    const struct limine_file *f = NULL;
    for (uint64_t i = 0; mods && i < mods->module_count; i++)
        if (mods->modules[i]->string && !strcmp(mods->modules[i]->string, "settings")) f = mods->modules[i];
    if (f && f->size >= N_BOOT_RECORD_BYTES) {
        const uint8_t *base = f->address;
        memcpy(part_id, &f->gpt_part_uuid, sizeof part_id);
        writable = f->media_type == LIMINE_MEDIA_TYPE_GENERIC;
        for (unsigned s = 0; s < 2; s++) {
            struct n_boot_record r;
            memcpy(&r, base + s * N_BOOT_RECORD_SLOT, sizeof r);
            if (!record_valid(&r) || (wanted.magic && r.generation <= wanted.generation)) continue;
            wanted = r;
            slot = s;
        }
        if (wanted.magic) memcpy(now, wanted.value, sizeof now);
    }
#ifdef NOCTURNE_TEST_FLAGS
    static const struct {
        const char *word;
        enum n_boot_key key;
        int32_t value;
    } flags[] = {{"noiommu", N_BOOT_IOMMU, 0}, {"noaml", N_BOOT_ACPI_AML, 0}, {"nosmp", N_BOOT_SMP, 0},
                 {"noresume", N_BOOT_RESUME, 0}, {"appdrivers-unisolated", N_BOOT_UNISOLATED_DRIVERS, 1}};
    for (size_t i = 0; i < ARRAY_SIZE(flags); i++)
        if (test_flag(flags[i].word)) now[flags[i].key] = flags[i].value;
#endif
}

int32_t boot_setting(enum n_boot_key key) { return now[key]; }

/* ---- the document on the disk ---- */

struct fat {
    struct blkdev *disk;
    uint64_t first;     /* the partition's first 512-byte sector */
    uint32_t unit;      /* the bytes of its sectors */
    uint32_t per_cluster, reserved, fat_sectors, fats, root;
    uint8_t *sector;
};

static uint64_t fat_lba(const struct fat *f, uint64_t sector) { return f->first + sector * (f->unit / 512); }

static int fat_read(const struct fat *f, uint64_t sector, void *buf) {
    return blk_read(f->disk, fat_lba(f, sector), f->unit / 512, buf);
}

static uint64_t cluster_sector(const struct fat *f, uint32_t cluster) {
    return f->reserved + (uint64_t)f->fats * f->fat_sectors + (uint64_t)(cluster - 2) * f->per_cluster;
}

/* The cluster after cluster: 0 at the end of its chain. */
static int fat_next(const struct fat *f, uint32_t cluster, uint32_t *next) {
    uint64_t at = (uint64_t)cluster * 4;
    int r = fat_read(f, f->reserved + at / f->unit, f->sector);
    if (r < 0) return r;
    uint32_t v;
    memcpy(&v, f->sector + at % f->unit, 4);
    v &= 0x0fffffffu;
    *next = v < 2 || v >= 0x0ffffff7u ? 0 : v;
    return 0;
}

/* name (the 11 bytes of a short name) in the folder at cluster: its first cluster and size. */
static int fat_find(const struct fat *f, uint32_t folder, const char *name, uint32_t *cluster, uint32_t *size) {
    for (uint32_t c = folder; c;) {
        for (uint32_t s = 0; s < f->per_cluster; s++) {
            int r = fat_read(f, cluster_sector(f, c) + s, f->sector);
            if (r < 0) return r;
            for (uint32_t at = 0; at < f->unit; at += 32) {
                const uint8_t *e = f->sector + at;
                if (!e[0]) return -N_ERR_NOT_FOUND;
                if (e[0] == 0xe5 || e[11] == 0x0f || memcmp(e, name, 11)) continue;
                uint16_t high, low;
                memcpy(&high, e + 20, 2);
                memcpy(&low, e + 26, 2);
                memcpy(size, e + 28, 4);
                *cluster = (uint32_t)high << 16 | low;
                return 0;
            }
        }
        int r = fat_next(f, c, &c);
        if (r < 0) return r;
    }
    return -N_ERR_NOT_FOUND;
}

static int fat_open(struct fat *f) {
    int r = store_boot_partition(part_id, &f->disk, &f->first, &(uint64_t){0});
    if (r < 0) return r;
    /* the boot sector's fields are where they are for any sector size */
    uint8_t *head = kmalloc(512);
    if (!head) return -N_ERR_NO_MEMORY;
    r = blk_read(f->disk, f->first, 1, head);
    if (r >= 0) {
        uint16_t unit, reserved, root_entries, fat16;
        memcpy(&unit, head + 11, 2);
        memcpy(&reserved, head + 14, 2);
        memcpy(&root_entries, head + 17, 2);
        memcpy(&fat16, head + 22, 2);
        f->unit = unit;
        f->per_cluster = head[13];
        f->reserved = reserved;
        f->fats = head[16];
        memcpy(&f->fat_sectors, head + 36, 4);
        memcpy(&f->root, head + 44, 4);
        if ((unit != 512 && unit != 4096) || !f->per_cluster || !f->fats || root_entries || fat16 || !f->fat_sectors)
            r = -N_ERR_UNSUPPORTED; /* not the FAT32 scripts/mkimage.sh makes */
    }
    kfree(head);
    if (r < 0) return r;
    f->sector = kmalloc(f->unit);
    return f->sector ? 0 : -N_ERR_NO_MEMORY;
}

/* slot of the document at cluster, a whole number of sectors, over what is there. */
static int fat_write_slot(const struct fat *f, uint32_t cluster, unsigned s, const uint8_t *data) {
    uint32_t bytes = f->per_cluster * f->unit, skip = s * N_BOOT_RECORD_SLOT, done = 0;
    while (cluster && done < N_BOOT_RECORD_SLOT) {
        if (skip < bytes) {
            uint32_t n = MIN(bytes - skip, N_BOOT_RECORD_SLOT - done);
            int r = blk_write(f->disk, fat_lba(f, cluster_sector(f, cluster) + skip / f->unit), n / 512, data + done);
            if (r < 0) return r;
            done += n;
            skip = 0;
        } else {
            skip -= bytes;
        }
        int r = fat_next(f, cluster, &cluster);
        if (r < 0) return r;
    }
    return done == N_BOOT_RECORD_SLOT ? 0 : -N_ERR_IO;
}

/* r as the next generation, in s. */
static int document_write(unsigned s, const struct n_boot_record *r) {
    struct fat f = {0};
    int e = fat_open(&f);
    uint8_t *data = e >= 0 ? kzalloc(N_BOOT_RECORD_SLOT) : NULL;
    if (e >= 0 && !data) e = -N_ERR_NO_MEMORY;
    uint32_t cluster = 0, size = 0;
    if (e >= 0) e = fat_find(&f, f.root, "BOOT       ", &cluster, &size);
    if (e >= 0) e = fat_find(&f, cluster, "SETTINGSREC", &cluster, &size);
    if (e >= 0 && size < N_BOOT_RECORD_BYTES) e = -N_ERR_IO;
    if (e >= 0) {
        memcpy(data, r, sizeof *r);
        e = fat_write_slot(&f, cluster, s, data);
    }
    if (e >= 0) e = blk_persist(f.disk);
    kfree(data);
    kfree(f.sector);
    return e;
}

int boot_setting_put(uint32_t key, const void *value) {
    if (!writable) return -N_ERR_UNSUPPORTED; /* started from a CD or DVD, or by no record */
    int32_t v;
    memcpy(&v, value, sizeof v);
    mutex_lock(&write_lock);
    int r = 0;
    if (!wanted.magic || wanted.value[key] != v) {
        struct n_boot_record next = wanted;
        next.magic = N_BOOT_RECORD_MAGIC;
        next.generation = wanted.generation + 1;
        next.value[key] = v;
        next.check = record_check(next);
        unsigned to = wanted.magic ? !slot : 0;
        if ((r = document_write(to, &next)) >= 0) {
            wanted = next;
            slot = to;
        }
    }
    mutex_unlock(&write_lock);
    return r < 0 ? r : 0;
}

int64_t boot_setting_get(uint32_t key, void *out, size_t cap) { return setting_int(out, cap, now[key]); }
