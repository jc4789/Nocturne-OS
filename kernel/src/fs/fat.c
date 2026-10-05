/* FAT32 filesystem (read/write, long file names) on an ATA disk.
   At boot every disk is scanned (MBR partitions, or a whole-disk "superfloppy" volume) and the
   volume labelled NOCTDATA is mounted at /data. Writes go straight to the disk, so nothing is lost
   when the VM is switched off.

   vnodes are created on lookup and cached for the life of the mount, keyed by where their short
   directory entry lives (containing directory's first cluster + byte offset), so every open of the
   same file shares one vnode and one size. Only the FAT sectors are cached (write-through). */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "dev/ata.h"
#include "dev/timer.h"

#define SECTOR 512
#define EOC    0x0FFFFFF8u
#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYS    0x04
#define ATTR_VOL    0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

struct fat_fs {
    struct ata_disk *disk;
    uint64_t lba0; /* volume start */
    uint32_t spc, reserved, nfats, fat_sectors, root_clus, nclus, cbytes;
    uint64_t data_lba;
    uint32_t free_hint;
    uint16_t fsinfo;
    struct vnode *root;
    struct vnode *nodes; /* cache (linked through vnode->sibling) */
};

struct fat_node {
    struct fat_fs *fs;
    uint32_t first;    /* first data cluster, 0 = empty file */
    uint32_t dir_clus; /* first cluster of the directory holding our entry (0 for the root) */
    uint32_t ent_off;  /* byte offset of our short entry in that directory */
};

struct fat_dirent {
    uint8_t name[11];
    uint8_t attr, ntres, ctime_tenth;
    uint16_t ctime, cdate, adate, clus_hi, mtime, mdate, clus_lo;
    uint32_t size;
} PACKED;

struct fat_lfn {
    uint8_t ord;
    uint16_t n1[5];
    uint8_t attr, type, sum;
    uint16_t n2[6];
    uint16_t zero;
    uint16_t n3[2];
} PACKED;

static struct vnode_ops fat_ops;

/* ---------------------------------------------------------------- sectors, clusters, FAT */

static int disk_read(struct fat_fs *fs, uint64_t lba, uint32_t n, void *buf) { return ata_read(fs->disk, fs->lba0 + lba, n, buf); }
static int disk_write(struct fat_fs *fs, uint64_t lba, uint32_t n, const void *buf) {
    return ata_write(fs->disk, fs->lba0 + lba, n, buf);
}

#define FCACHE 64
static struct {
    struct fat_fs *fs;
    uint64_t lba;
    uint64_t used;
    uint8_t data[SECTOR];
} fcache[FCACHE];
static uint64_t fcache_clock;

static uint8_t *fat_sector(struct fat_fs *fs, uint64_t lba) {
    int victim = 0;
    for (int i = 0; i < FCACHE; i++) {
        if (fcache[i].fs == fs && fcache[i].lba == lba) {
            fcache[i].used = ++fcache_clock;
            return fcache[i].data;
        }
        if (fcache[i].used < fcache[victim].used) victim = i;
    }
    if (disk_read(fs, lba, 1, fcache[victim].data) < 0) return NULL;
    fcache[victim].fs = fs;
    fcache[victim].lba = lba;
    fcache[victim].used = ++fcache_clock;
    return fcache[victim].data;
}

/* forget a volume's cached FAT sectors before its fat_fs is freed: a volume probed later could
   be allocated at the same address and would otherwise see this one's FAT */
static void fcache_forget(struct fat_fs *fs) {
    for (int i = 0; i < FCACHE; i++)
        if (fcache[i].fs == fs) {
            fcache[i].fs = NULL;
            fcache[i].used = 0;
        }
}

static uint32_t fat_get(struct fat_fs *fs, uint32_t c) {
    uint64_t off = (uint64_t)c * 4;
    uint8_t *s = fat_sector(fs, fs->reserved + off / SECTOR);
    if (!s) return EOC;
    return *(uint32_t *)(s + off % SECTOR) & 0x0FFFFFFF;
}

static int fat_set(struct fat_fs *fs, uint32_t c, uint32_t v) {
    uint64_t off = (uint64_t)c * 4;
    uint64_t lba = fs->reserved + off / SECTOR;
    uint8_t *s = fat_sector(fs, lba);
    if (!s) return -EIO;
    uint32_t *p = (uint32_t *)(s + off % SECTOR);
    *p = (*p & 0xF0000000) | (v & 0x0FFFFFFF);
    for (uint32_t f = 0; f < fs->nfats; f++)
        if (disk_write(fs, lba + (uint64_t)f * fs->fat_sectors, 1, s) < 0) return -EIO;
    return 0;
}

static bool clus_valid(struct fat_fs *fs, uint32_t c) { return c >= 2 && c < fs->nclus + 2; }
static uint64_t clus_lba(struct fat_fs *fs, uint32_t c) { return fs->data_lba + (uint64_t)(c - 2) * fs->spc; }

static int clus_read(struct fat_fs *fs, uint32_t c, void *buf) { return disk_read(fs, clus_lba(fs, c), fs->spc, buf); }
static int clus_write(struct fat_fs *fs, uint32_t c, const void *buf) { return disk_write(fs, clus_lba(fs, c), fs->spc, buf); }

/* allocate a cluster (zeroed for directories) and link it after prev (if any) */
static uint32_t clus_alloc(struct fat_fs *fs, uint32_t prev, bool zero) {
    uint32_t start = clus_valid(fs, fs->free_hint) ? fs->free_hint : 2;
    uint32_t c = start;
    do {
        if (fat_get(fs, c) == 0) {
            if (fat_set(fs, c, 0x0FFFFFFF) < 0) return 0;
            if (prev && fat_set(fs, prev, c) < 0) return 0;
            if (zero) {
                uint8_t *z = kzalloc(fs->cbytes);
                if (!z) return 0;
                int r = clus_write(fs, c, z);
                kfree(z);
                if (r < 0) return 0;
            }
            fs->free_hint = c + 1;
            return c;
        }
        if (++c >= fs->nclus + 2) c = 2;
    } while (c != start);
    return 0;
}

static void chain_free(struct fat_fs *fs, uint32_t c) {
    while (clus_valid(fs, c)) {
        uint32_t next = fat_get(fs, c);
        fat_set(fs, c, 0);
        if (c < fs->free_hint) fs->free_hint = c;
        c = next;
    }
}

/* the n-th cluster of a chain (0 if the chain is shorter) */
static uint32_t chain_nth(struct fat_fs *fs, uint32_t c, uint64_t n) {
    while (n-- && clus_valid(fs, c)) c = fat_get(fs, c);
    return clus_valid(fs, c) ? c : 0;
}

/* ---------------------------------------------------------------- directories */

static uint32_t node_first(struct vnode *v) { return ((struct fat_node *)v->priv)->first; }

/* read a whole directory into memory; *len gets its size in bytes */
static uint8_t *dir_load(struct fat_fs *fs, uint32_t first, uint32_t *len) {
    uint32_t n = 0;
    for (uint32_t c = first; clus_valid(fs, c) && n < 65536; c = fat_get(fs, c)) n++;
    uint8_t *buf = kmalloc((size_t)MAX(n, 1u) * fs->cbytes);
    if (!buf) return NULL;
    uint32_t i = 0;
    for (uint32_t c = first; clus_valid(fs, c) && i < n; c = fat_get(fs, c), i++)
        if (clus_read(fs, c, buf + (size_t)i * fs->cbytes) < 0) {
            kfree(buf);
            return NULL;
        }
    *len = n * fs->cbytes;
    return buf;
}

/* write back the sector holding directory byte offset off (from a loaded copy) */
static int dir_store(struct fat_fs *fs, uint32_t first, const uint8_t *buf, uint32_t off) {
    uint32_t c = chain_nth(fs, first, off / fs->cbytes);
    if (!c) return -EIO;
    uint32_t in = off % fs->cbytes;
    return disk_write(fs, clus_lba(fs, c) + in / SECTOR, 1, buf + (off - in % SECTOR));
}

struct entry {
    char name[NAME_MAX_LEN];
    uint32_t off;      /* short entry offset */
    uint32_t lfn_off;  /* first entry of the run (== off without LFN) */
    struct fat_dirent d;
};

static uint8_t lfn_sum(const uint8_t *s) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + s[i]);
    return sum;
}

static void short_to_str(const struct fat_dirent *d, char *out) {
    int k = 0;
    for (int i = 0; i < 8 && d->name[i] != ' '; i++) {
        char c = (char)d->name[i];
        if (d->ntres & 0x08 && c >= 'A' && c <= 'Z') c += 32;
        out[k++] = c;
    }
    if (d->name[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && d->name[i] != ' '; i++) {
            char c = (char)d->name[i];
            if (d->ntres & 0x10 && c >= 'A' && c <= 'Z') c += 32;
            out[k++] = c;
        }
    }
    out[k] = 0;
    if ((uint8_t)out[0] == 0x05) out[0] = (char)0xE5;
}

/* iterate live entries: returns the next one at or after *pos, false at the end */
static bool dir_next(const uint8_t *buf, uint32_t len, uint32_t *pos, struct entry *e) {
    char lfn[NAME_MAX_LEN * 2];
    int lfn_parts = 0;
    uint8_t lfn_csum = 0;
    uint32_t run_start = *pos;
    bool have_lfn = false;
    for (uint32_t o = *pos; o + 32 <= len; o += 32) {
        const uint8_t *p = buf + o;
        if (p[0] == 0x00) return false;
        if (p[0] == 0xE5) {
            have_lfn = false;
            continue;
        }
        if (p[11] == ATTR_LFN) {
            const struct fat_lfn *l = (const struct fat_lfn *)p;
            int ord = l->ord & 0x3F;
            if (l->ord & 0x40) {
                memset(lfn, 0, sizeof lfn);
                have_lfn = true;
                lfn_parts = ord;
                lfn_csum = l->sum;
                run_start = o;
            }
            if (!have_lfn || ord < 1 || ord > 10 || l->sum != lfn_csum) {
                have_lfn = false;
                continue;
            }
            uint16_t ch[13];
            memcpy(ch, l->n1, 10);
            memcpy(ch + 5, l->n2, 12);
            memcpy(ch + 11, l->n3, 4);
            for (int i = 0; i < 13; i++) {
                int idx = (ord - 1) * 13 + i;
                if (idx >= (int)sizeof lfn - 1) break;
                uint16_t c = ch[i];
                if (c == 0 || c == 0xFFFF) continue;
                lfn[idx] = c < 128 ? (char)c : '?';
            }
            continue;
        }
        const struct fat_dirent *d = (const struct fat_dirent *)p;
        if (d->attr & ATTR_VOL) {
            have_lfn = false;
            continue;
        }
        memcpy(&e->d, d, sizeof *d);
        e->off = o;
        if (have_lfn && lfn_sum(d->name) == lfn_csum && lfn_parts) {
            /* entries are filled sparsely by ord; squeeze out any holes */
            int k = 0;
            for (int i = 0; i < (int)sizeof lfn - 1 && k < NAME_MAX_LEN - 1; i++)
                if (lfn[i]) e->name[k++] = lfn[i];
            e->name[k] = 0;
            e->lfn_off = run_start;
        } else {
            short_to_str(d, e->name);
            e->lfn_off = o;
        }
        *pos = o + 32;
        return true;
    }
    return false;
}

static bool dir_find(const uint8_t *buf, uint32_t len, const char *name, struct entry *e) {
    uint32_t pos = 0;
    while (dir_next(buf, len, &pos, e)) {
        if (!strcasecmp(e->name, name)) return true;
        char sn[16];
        short_to_str(&e->d, sn);
        if (!strcasecmp(sn, name)) return true;
    }
    return false;
}

/* ---------------------------------------------------------------- time */

/* stamp an entry's modification time */
static void fat_time(int64_t t, struct fat_dirent *d) {
    struct tm_parts p;
    time_to_parts(t, &p);
    if (p.year < 1980) p.year = 1980;
    d->mdate = (uint16_t)(((p.year - 1980) << 9) | (p.mon << 5) | p.mday);
    d->mtime = (uint16_t)((p.hour << 11) | (p.min << 5) | (p.sec / 2));
}

static int64_t unix_time(uint16_t date, uint16_t tm) {
    struct tm_parts p = {.year = 1980 + (date >> 9), .mon = (date >> 5) & 15, .mday = date & 31,
                         .hour = tm >> 11, .min = (tm >> 5) & 63, .sec = (tm & 31) * 2};
    if (!p.mon || !p.mday) return 0;
    return time_from_parts(&p);
}

/* ---------------------------------------------------------------- vnodes */

static struct vnode *node_get(struct fat_fs *fs, uint32_t dir_clus, const struct entry *e) {
    for (struct vnode *v = fs->nodes; v; v = v->sibling) {
        struct fat_node *n = v->priv;
        if (n->dir_clus == dir_clus && n->ent_off == e->off) return v;
    }
    struct vnode *v = kzalloc(sizeof *v);
    struct fat_node *n = kzalloc(sizeof *n);
    if (!v || !n) {
        kfree(v);
        kfree(n);
        return NULL;
    }
    strlcpy(v->name, e->name, sizeof v->name);
    v->type = (e->d.attr & ATTR_DIR) ? VT_DIR : VT_FILE;
    v->mode = (e->d.attr & ATTR_RO) ? 0444 : 0644;
    v->size = v->type == VT_FILE ? e->d.size : 0;
    v->mtime = unix_time(e->d.mdate, e->d.mtime);
    v->ops = &fat_ops;
    v->priv = n;
    n->fs = fs;
    n->first = (uint32_t)e->d.clus_hi << 16 | e->d.clus_lo;
    n->dir_clus = dir_clus;
    n->ent_off = e->off;
    v->sibling = fs->nodes;
    fs->nodes = v;
    return v;
}

static void node_forget(struct fat_fs *fs, struct vnode *v) {
    for (struct vnode **pp = &fs->nodes; *pp; pp = &(*pp)->sibling)
        if (*pp == v) {
            *pp = v->sibling;
            v->sibling = NULL;
            return;
        }
}

/* rewrite our directory entry (size, first cluster, mtime) */
static int node_sync(struct vnode *v) {
    struct fat_node *n = v->priv;
    struct fat_fs *fs = n->fs;
    if (v == fs->root || v->unlinked) return 0; /* an unlinked file has no entry any more */
    uint32_t dirc = n->dir_clus ? n->dir_clus : fs->root_clus;
    uint32_t c = chain_nth(fs, dirc, n->ent_off / fs->cbytes);
    if (!c) return -EIO;
    uint32_t in = n->ent_off % fs->cbytes;
    uint64_t lba = clus_lba(fs, c) + in / SECTOR;
    uint8_t sec[SECTOR];
    if (disk_read(fs, lba, 1, sec) < 0) return -EIO;
    struct fat_dirent *d = (struct fat_dirent *)(sec + in % SECTOR);
    d->clus_hi = (uint16_t)(n->first >> 16);
    d->clus_lo = (uint16_t)n->first;
    if (v->type == VT_FILE) d->size = (uint32_t)v->size;
    fat_time(v->mtime ? v->mtime : time_now(), d);
    d->adate = d->mdate;
    d->attr |= ATTR_ARCH;
    return disk_write(fs, lba, 1, sec);
}

/* ---------------------------------------------------------------- vnode ops */

static int64_t fat_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t len) {
    struct fat_node *n = v->priv;
    struct fat_fs *fs = n->fs;
    if (off >= v->size) return 0;
    if (off + len > v->size) len = v->size - off;
    uint8_t *tmp = kmalloc(fs->cbytes);
    if (!tmp) return -ENOMEM;
    uint32_t c = chain_nth(fs, n->first, off / fs->cbytes);
    size_t done = 0;
    while (done < len && c) {
        uint32_t in = (uint32_t)((off + done) % fs->cbytes);
        size_t take = MIN(len - done, (size_t)(fs->cbytes - in));
        if (in == 0 && take == fs->cbytes) {
            if (clus_read(fs, c, (uint8_t *)buf + done) < 0) break;
        } else {
            if (clus_read(fs, c, tmp) < 0) break;
            memcpy((uint8_t *)buf + done, tmp + in, take);
        }
        done += take;
        if (done < len) c = chain_nth(fs, c, 1);
    }
    kfree(tmp);
    return done ? (int64_t)done : -EIO;
}

static int64_t fat_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t len) {
    struct fat_node *n = v->priv;
    struct fat_fs *fs = n->fs;
    if (!len) return 0;
    if (off + len > 0xFFFFFFFFull) return -ENOSPC; /* FAT32 file size limit */
    uint64_t need = (off + len + fs->cbytes - 1) / fs->cbytes;
    /* grow the chain to cover the write */
    uint64_t have = 0;
    uint32_t last = 0;
    for (uint32_t c = n->first; clus_valid(fs, c); c = fat_get(fs, c)) {
        have++;
        last = c;
    }
    while (have < need) {
        uint32_t c = clus_alloc(fs, last, false);
        if (!c) return -ENOSPC;
        if (!n->first) n->first = c;
        last = c;
        have++;
    }
    uint8_t *tmp = kmalloc(fs->cbytes);
    if (!tmp) return -ENOMEM;
    uint32_t c = chain_nth(fs, n->first, off / fs->cbytes);
    size_t done = 0;
    int err = 0;
    while (done < len && c) {
        uint32_t in = (uint32_t)((off + done) % fs->cbytes);
        size_t take = MIN(len - done, (size_t)(fs->cbytes - in));
        if (in == 0 && take == fs->cbytes) {
            err = clus_write(fs, c, (const uint8_t *)buf + done);
        } else {
            err = clus_read(fs, c, tmp);
            if (!err) {
                memcpy(tmp + in, (const uint8_t *)buf + done, take);
                err = clus_write(fs, c, tmp);
            }
        }
        if (err) break;
        done += take;
        if (done < len) c = chain_nth(fs, c, 1);
    }
    kfree(tmp);
    if (off + done > v->size) v->size = off + done;
    v->mtime = time_now();
    node_sync(v);
    return done ? (int64_t)done : -EIO;
}

static int fat_truncate(struct vnode *v, uint64_t size) {
    struct fat_node *n = v->priv;
    struct fat_fs *fs = n->fs;
    if (size >= v->size) {
        v->size = size; /* growing: the new bytes are allocated lazily by the next write */
        return node_sync(v);
    }
    uint64_t keep = (size + fs->cbytes - 1) / fs->cbytes;
    if (keep == 0) {
        chain_free(fs, n->first);
        n->first = 0;
    } else {
        uint32_t c = chain_nth(fs, n->first, keep - 1);
        if (c) {
            uint32_t rest = fat_get(fs, c);
            fat_set(fs, c, 0x0FFFFFFF);
            chain_free(fs, rest);
        }
    }
    v->size = size;
    v->mtime = time_now();
    return node_sync(v);
}

static uint32_t dir_first(struct vnode *dir) {
    struct fat_node *n = dir->priv;
    return dir == n->fs->root ? n->fs->root_clus : n->first;
}

static int fat_readdir(struct vnode *dir, uint64_t idx, struct dirent *out) {
    struct fat_fs *fs = ((struct fat_node *)dir->priv)->fs;
    uint32_t len;
    uint8_t *buf = dir_load(fs, dir_first(dir), &len);
    if (!buf) return -EIO;
    uint32_t pos = 0;
    struct entry e;
    uint64_t i = 0;
    int ret = 0;
    while (dir_next(buf, len, &pos, &e)) {
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        if (i++ == idx) {
            strlcpy(out->name, e.name, sizeof out->name);
            out->type = (e.d.attr & ATTR_DIR) ? VT_DIR : VT_FILE;
            out->size = (e.d.attr & ATTR_DIR) ? 0 : e.d.size;
            /* the cached vnode knows about writes not yet visible here (it is always synced, but be safe) */
            ret = 1;
            break;
        }
    }
    kfree(buf);
    return ret;
}

static struct vnode *fat_lookup(struct vnode *dir, const char *name) {
    struct fat_node *dn = dir->priv;
    struct fat_fs *fs = dn->fs;
    if (!strcmp(name, ".")) return dir;
    uint32_t first = dir_first(dir);
    uint32_t len;
    uint8_t *buf = dir_load(fs, first, &len);
    if (!buf) return NULL;
    struct entry e;
    struct vnode *v = NULL;
    if (dir_find(buf, len, name, &e)) {
        v = node_get(fs, dir == fs->root ? 0 : first, &e);
        if (v) v->parent = dir;
    }
    kfree(buf);
    return v;
}

/* 8.3 name for a long name: BASENA~N.EXT, unique in the directory */
static void make_short(const uint8_t *buf, uint32_t len, const char *name, uint8_t out[11]) {
    char base[9] = {0}, ext[4] = {0};
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    int bl = 0, el = 0;
    for (const char *p = name; *p && p != dot && bl < 6; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == ' ' || c == '.') continue;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'()-@^_`{}~", c))) c = '_';
        base[bl++] = c;
    }
    if (!bl) base[bl++] = '_';
    for (const char *p = dot ? dot + 1 : ""; *p && el < 3; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '_';
        ext[el++] = c;
    }
    for (int num = 1; num < 1000000; num++) {
        char tail[8];
        int tl = ksnprintf(tail, sizeof tail, "~%d", num);
        memset(out, ' ', 11);
        int keep = MIN(bl, 8 - tl);
        memcpy(out, base, keep);
        memcpy(out + keep, tail, tl);
        memcpy(out + 8, ext, el);
        bool clash = false;
        for (uint32_t o = 0; o + 32 <= len; o += 32) {
            if (buf[o] == 0) break;
            if (buf[o] != 0xE5 && buf[o + 11] != ATTR_LFN && !memcmp(buf + o, out, 11)) {
                clash = true;
                break;
            }
        }
        if (!clash) return;
    }
}

static int fat_create(struct vnode *dir, const char *name, int type, struct vnode **outv) {
    struct fat_node *dn = dir->priv;
    struct fat_fs *fs = dn->fs;
    if (type != VT_FILE && type != VT_DIR) return -EINVAL;
    size_t nl = strlen(name);
    if (!nl || nl >= NAME_MAX_LEN) return -ENAMETOOLONG;
    for (const char *p = name; *p; p++)
        if (strchr("\\/:*?\"<>|", *p) || (uint8_t)*p < 32) return -EINVAL;
    uint32_t first = dir_first(dir);
    uint32_t len;
    uint8_t *buf = dir_load(fs, first, &len);
    if (!buf) return -EIO;
    struct entry e;
    if (dir_find(buf, len, name, &e)) {
        kfree(buf);
        return -EEXIST;
    }
    uint8_t sn[11];
    make_short(buf, len, name, sn);
    int nlfn = (int)((nl + 12) / 13);
    int need = nlfn + 1;
    /* find a run of free slots */
    uint32_t slot = 0xFFFFFFFF;
    int run = 0;
    for (uint32_t o = 0; o + 32 <= len; o += 32) {
        if (buf[o] == 0x00 || buf[o] == 0xE5) {
            if (++run == need) {
                slot = o - (uint32_t)(need - 1) * 32;
                break;
            }
            if (buf[o] == 0x00) {
                /* everything after the end marker is free too */
                if ((len - o) / 32 >= (uint32_t)(need - run + 1)) {
                    slot = o - (uint32_t)(run - 1) * 32;
                    break;
                }
            }
        } else {
            run = 0;
        }
    }
    if (slot == 0xFFFFFFFF) {
        /* extend the directory by enough clusters */
        uint32_t last = chain_nth(fs, first, len / fs->cbytes - 1);
        uint32_t add = (uint32_t)(need * 32 + fs->cbytes - 1) / fs->cbytes;
        /* the run may start in the free tail of the current last cluster */
        uint32_t tail_free = 0;
        for (uint32_t o = len; o >= 32 && (buf[o - 32] == 0 || buf[o - 32] == 0xE5); o -= 32) tail_free++;
        uint8_t *nb = kmalloc(len + (size_t)add * fs->cbytes);
        if (!nb) {
            kfree(buf);
            return -ENOMEM;
        }
        memcpy(nb, buf, len);
        memset(nb + len, 0, (size_t)add * fs->cbytes);
        kfree(buf);
        buf = nb;
        for (uint32_t i = 0; i < add; i++) {
            last = clus_alloc(fs, last, true);
            if (!last) {
                kfree(buf);
                return -ENOSPC;
            }
        }
        slot = len - tail_free * 32;
        len += add * fs->cbytes;
    }
    /* the new object's first cluster (directories always get one) */
    uint32_t clus = 0;
    if (type == VT_DIR) {
        clus = clus_alloc(fs, 0, true);
        if (!clus) {
            kfree(buf);
            return -ENOSPC;
        }
        uint8_t *z = kzalloc(fs->cbytes);
        struct fat_dirent *d = (struct fat_dirent *)z;
        memset(d[0].name, ' ', 11);
        d[0].name[0] = '.';
        d[0].attr = ATTR_DIR;
        d[0].clus_hi = (uint16_t)(clus >> 16);
        d[0].clus_lo = (uint16_t)clus;
        memset(d[1].name, ' ', 11);
        d[1].name[0] = d[1].name[1] = '.';
        d[1].attr = ATTR_DIR;
        uint32_t parent = dir == fs->root ? 0 : first;
        d[1].clus_hi = (uint16_t)(parent >> 16);
        d[1].clus_lo = (uint16_t)parent;
        fat_time(time_now(), &d[0]);
        d[1].mdate = d[0].mdate;
        d[1].mtime = d[0].mtime;
        clus_write(fs, clus, z);
        kfree(z);
    }
    uint8_t sum = lfn_sum(sn);
    for (int i = 0; i < nlfn; i++) {
        int ord = nlfn - i;
        struct fat_lfn *l = (struct fat_lfn *)(buf + slot + (uint32_t)i * 32);
        memset(l, 0, 32);
        l->ord = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        l->attr = ATTR_LFN;
        l->sum = sum;
        uint16_t ch[13];
        for (int k = 0; k < 13; k++) {
            size_t idx = (size_t)(ord - 1) * 13 + k;
            ch[k] = idx < nl ? (uint8_t)name[idx] : idx == nl ? 0 : 0xFFFF;
        }
        memcpy(l->n1, ch, 10);
        memcpy(l->n2, ch + 5, 12);
        memcpy(l->n3, ch + 11, 4);
    }
    uint32_t off = slot + (uint32_t)nlfn * 32;
    struct fat_dirent *d = (struct fat_dirent *)(buf + off);
    memset(d, 0, 32);
    memcpy(d->name, sn, 11);
    d->attr = type == VT_DIR ? ATTR_DIR : ATTR_ARCH;
    d->clus_hi = (uint16_t)(clus >> 16);
    d->clus_lo = (uint16_t)clus;
    fat_time(time_now(), d);
    d->cdate = d->adate = d->mdate;
    d->ctime = d->mtime;
    int err = 0;
    for (uint32_t o = slot; o <= off && !err; o += 32)
        if (o == slot || o % SECTOR == 0) err = dir_store(fs, first, buf, o);
    struct entry ne;
    memcpy(&ne.d, d, sizeof *d);
    strlcpy(ne.name, name, sizeof ne.name);
    ne.off = off;
    ne.lfn_off = slot;
    kfree(buf);
    if (err) return err;
    struct vnode *v = node_get(fs, dir == fs->root ? 0 : first, &ne);
    if (!v) return -ENOMEM;
    v->parent = dir;
    *outv = v;
    return 0;
}

static int fat_unlink(struct vnode *dir, const char *name) {
    struct fat_node *dn = dir->priv;
    struct fat_fs *fs = dn->fs;
    uint32_t first = dir_first(dir);
    uint32_t len;
    uint8_t *buf = dir_load(fs, first, &len);
    if (!buf) return -EIO;
    struct entry e;
    if (!dir_find(buf, len, name, &e)) {
        kfree(buf);
        return -ENOENT;
    }
    uint32_t clus = (uint32_t)e.d.clus_hi << 16 | e.d.clus_lo;
    if (e.d.attr & ATTR_DIR) {
        uint32_t sl;
        uint8_t *sub = dir_load(fs, clus, &sl);
        if (sub) {
            uint32_t pos = 0;
            struct entry se;
            while (dir_next(sub, sl, &pos, &se)) {
                if (strcmp(se.name, ".") && strcmp(se.name, "..")) {
                    kfree(sub);
                    kfree(buf);
                    return -ENOTEMPTY;
                }
            }
            kfree(sub);
        }
    }
    for (uint32_t o = e.lfn_off; o <= e.off; o += 32) buf[o] = 0xE5;
    int err = 0;
    for (uint32_t o = e.lfn_off; o <= e.off && !err; o += 32)
        if (o == e.lfn_off || o % SECTOR == 0) err = dir_store(fs, first, buf, o);
    kfree(buf);
    if (err) return err;
    /* the open vnode (if any) keeps its clusters until released */
    uint32_t dkey = dir == fs->root ? 0 : first;
    struct vnode *v = NULL;
    for (struct vnode *x = fs->nodes; x; x = x->sibling) {
        struct fat_node *n = x->priv;
        if (n->dir_clus == dkey && n->ent_off == e.off) v = x;
    }
    if (v) {
        node_forget(fs, v);
        v->unlinked = true;
        if (v->refs <= 0) {
            chain_free(fs, node_first(v));
            kfree(v->priv);
            kfree(v);
        }
    } else {
        chain_free(fs, clus);
    }
    return 0;
}

static void fat_release(struct vnode *v) {
    struct fat_node *n = v->priv;
    chain_free(n->fs, n->first);
    kfree(n);
    kfree(v);
}

static struct vnode_ops fat_ops = {
    .read = fat_read,
    .write = fat_write,
    .readdir = fat_readdir,
    .lookup = fat_lookup,
    .create = fat_create,
    .unlink = fat_unlink,
    .truncate = fat_truncate,
    .release = fat_release,
};

/* ---------------------------------------------------------------- mounting */

static bool volume_label(struct fat_fs *fs, const uint8_t *bpb, char *out) {
    /* the label lives in the root directory (Windows) and/or the boot sector (mformat) */
    uint32_t len;
    uint8_t *buf = dir_load(fs, fs->root_clus, &len);
    if (buf) {
        for (uint32_t o = 0; o + 32 <= len && buf[o]; o += 32) {
            if (buf[o] != 0xE5 && buf[o + 11] != ATTR_LFN && (buf[o + 11] & ATTR_VOL)) {
                memcpy(out, buf + o, 11);
                out[11] = 0;
                kfree(buf);
                return true;
            }
        }
        kfree(buf);
    }
    memcpy(out, bpb + 71, 11);
    out[11] = 0;
    return true;
}

static struct fat_fs *try_volume(struct ata_disk *disk, uint64_t lba, char *label) {
    uint8_t bs[SECTOR];
    if (ata_read(disk, lba, 1, bs) < 0) {
        kprintf("fat: disk %d: read error at sector %lu\n", disk->index, lba);
        return NULL;
    }
    if (bs[510] != 0x55 || bs[511] != 0xAA) return NULL;
    uint16_t bps = *(uint16_t *)(bs + 11);
    uint8_t spc = bs[13];
    uint16_t reserved = *(uint16_t *)(bs + 14);
    uint8_t nfats = bs[16];
    uint16_t root_entries = *(uint16_t *)(bs + 17);
    uint32_t total = *(uint16_t *)(bs + 19) ? *(uint16_t *)(bs + 19) : *(uint32_t *)(bs + 32);
    uint32_t fat_sectors = *(uint32_t *)(bs + 36);
    if (bps != SECTOR || !spc || (spc & (spc - 1)) || !nfats || root_entries || !fat_sectors) return NULL;
    struct fat_fs *fs = kzalloc(sizeof *fs);
    fs->disk = disk;
    fs->lba0 = lba;
    fs->spc = spc;
    fs->cbytes = spc * SECTOR;
    fs->reserved = reserved;
    fs->nfats = nfats;
    fs->fat_sectors = fat_sectors;
    fs->root_clus = *(uint32_t *)(bs + 44);
    fs->data_lba = reserved + (uint64_t)nfats * fat_sectors;
    fs->nclus = (uint32_t)((total - fs->data_lba) / spc);
    fs->free_hint = 2;
    fs->fsinfo = *(uint16_t *)(bs + 48);
    volume_label(fs, bs, label);
    for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = 0;
    return fs;
}

static void mount_fs(struct fat_fs *fs, const char *path, const char *label) {
    /* we do not maintain FSInfo's free-cluster count, so tell other systems it is unknown */
    uint8_t fi[SECTOR];
    if (fs->fsinfo && fs->fsinfo < fs->reserved && disk_read(fs, fs->fsinfo, 1, fi) == 0 && *(uint32_t *)fi == 0x41615252 &&
        *(uint32_t *)(fi + 488) != 0xFFFFFFFF) {
        *(uint32_t *)(fi + 488) = 0xFFFFFFFF;
        *(uint32_t *)(fi + 492) = 0xFFFFFFFF;
        disk_write(fs, fs->fsinfo, 1, fi);
    }
    struct vnode *root = kzalloc(sizeof *root);
    struct fat_node *n = kzalloc(sizeof *n);
    n->fs = fs;
    n->first = fs->root_clus;
    root->type = VT_DIR;
    root->ops = &fat_ops;
    root->priv = n;
    root->refs = 1;
    fs->root = root;
    vfs_mount(path, root);
    kprintf("fat: mounted \"%s\" (%lu MiB, %u KiB clusters) at %s\n", label,
            (uint64_t)fs->nclus * fs->spc / 2048, fs->cbytes / 1024, path);
}

void fat_mount_data(void) {
    for (int i = 0; i < ata_count(); i++) {
        struct ata_disk *d = ata_get(i);
        uint8_t mbr[SECTOR];
        if (ata_read(d, 0, 1, mbr) < 0) {
            kprintf("fat: disk %d: cannot read the partition table\n", i);
            continue;
        }
        uint64_t starts[5];
        int ns = 0;
        starts[ns++] = 0; /* superfloppy */
        if (mbr[510] == 0x55 && mbr[511] == 0xAA)
            for (int p = 0; p < 4; p++) {
                uint8_t type = mbr[446 + p * 16 + 4];
                uint32_t start = *(uint32_t *)(mbr + 446 + p * 16 + 8);
                if ((type == 0x0B || type == 0x0C || type == 0x07) && start) starts[ns++] = start;
            }
        for (int s = 0; s < ns; s++) {
            char label[12];
            struct fat_fs *fs = try_volume(d, starts[s], label);
            if (!fs) continue;
            if (!strcasecmp(label, "NOCTDATA")) {
                mount_fs(fs, "/data", label);
                return;
            }
            fcache_forget(fs);
            kfree(fs);
        }
    }
    vfs_mkdir("/data");
    kprintf("fat: no NOCTDATA volume found; /data is not persistent\n");
}
