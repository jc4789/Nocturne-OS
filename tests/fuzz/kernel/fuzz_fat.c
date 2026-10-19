/* libFuzzer harness: the FAT32 driver (kernel/src/fs/fat.c) on a memory disk.
   The data disk may be damaged or hostile, so the harness
     1. builds a small FAT32 volume labelled NOCTDATA and fills it with folders,
        long-named files and a multi-cluster file through the product code itself
        (done once; the result is the golden image),
     2. for each input copies the golden image, applies the input's byte patches
        (BPB, FAT, directory clusters), mounts it with fat_mount_data(), and
     3. runs an operation script (lookup, list, read, write, create, unlink,
        move, truncate, replace) through the VFS.
   Input: [npatch:u8] npatch * [target:u8][offset:u16][len:u8][bytes...], then the script. */
#include <stdio.h>
#include "../../../kernel/src/fs/vfs.c"
#include "../../../kernel/src/fs/ramfs.c"
#include "../../../kernel/src/fs/fat.c"

#define IMG_SECTORS 4096u
static uint8_t golden[IMG_SECTORS * 512], image[IMG_SECTORS * 512];
static uint8_t *cur = image;

static int mem_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) {
    if (lba >= IMG_SECTORS || n > IMG_SECTORS - lba) return -N_ERR_IO;
    memcpy(buf, cur + lba * 512, (size_t)n * 512);
    return 0;
}
static int mem_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) {
    if (lba >= IMG_SECTORS || n > IMG_SECTORS - lba) return -N_ERR_IO;
    memcpy(cur + lba * 512, buf, (size_t)n * 512);
    return 0;
}
static struct blkdev disk = {.name = "mem0", .sectors = IMG_SECTORS, .read = mem_read, .write = mem_write};
int blk_count(void) { return 1; }
struct blkdev *blk_get(int i) { return i == 0 ? &disk : NULL; }

static void make_fat32(uint8_t *img) {
    memset(img, 0, IMG_SECTORS * 512);
    uint8_t *b = img;
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "NOCTURNE", 8);
    *(uint16_t *)(b + 11) = 512;
    b[13] = 1; /* sectors per cluster */
    *(uint16_t *)(b + 14) = 32; /* reserved */
    b[16] = 2; /* FATs */
    *(uint32_t *)(b + 32) = IMG_SECTORS;
    *(uint32_t *)(b + 36) = 32; /* sectors per FAT: 4096 entries */
    *(uint32_t *)(b + 44) = 2;
    *(uint16_t *)(b + 48) = 1;
    memcpy(b + 71, "NOCTDATA   ", 11);
    b[510] = 0x55; b[511] = 0xAA;
    for (int f = 0; f < 2; f++) {
        uint32_t *fat = (uint32_t *)(img + (32 + f * 32) * 512);
        fat[0] = 0x0FFFFFF8; fat[1] = 0x0FFFFFFF; fat[2] = 0x0FFFFFFF; /* root dir: cluster 2 */
    }
}

static void reset_world(void) {
    /* drop the previous mount: vnodes, node cache, FAT sector cache */
    for (int i = 0; i < FCACHE; i++) { fcache[i].fs = NULL; fcache[i].used = 0; }
    fcache_clock = 0;
}

static bool g_ready;
static void init_golden(void) {
    make_fat32(golden);
    memcpy(image, golden, sizeof image);
    cur = image;
    vfs_init();
    fat_mount_data();
    if (!vfs_lookup("/data")) abort();
    vfs_mkdir("/data/d1");
    vfs_mkdir("/data/d1/sub");
    static uint8_t big[5000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;
    vfs_write_whole("/data/hello.txt", "hello", 5);
    vfs_write_whole("/data/A Rather Long File Name.dat", big, 700);
    vfs_write_whole("/data/d1/big.bin", big, sizeof big);
    vfs_write_whole("/data/d1/sub/x", "x", 1);
    memcpy(golden, image, sizeof golden);
    g_ready = true;
}

static const char *pool[] = {"data", "d1", "sub", "hello.txt", "big.bin", "x", "A Rather Long File Name.dat", "n", "m", "q.t"};
static size_t build_path(const uint8_t *d, size_t n, char *out, size_t cap) {
    size_t at = 0;
    strcpy(out, "/data");
    at = 5;
    for (size_t i = 0; i < n && at + 40 < cap; i++) {
        out[at++] = '/';
        const char *c = pool[d[i] % (sizeof pool / sizeof *pool)];
        size_t l = strlen(c);
        memcpy(out + at, c, l);
        at += l;
    }
    out[at] = 0;
    return at;
}

static void free_world(void) {
    /* mounted fs: free the node cache and the fat_fs (harness-only cleanup) */
    struct vnode *data = vfs_lookup("/data");
    (void)data;
}

static void script(const uint8_t *d, size_t n) {
    char p1[PATH_MAX_LEN * 2], p2[PATH_MAX_LEN * 2];
    struct file *files[4] = {0};
    size_t at = 0;
    int steps = 0;
    static uint8_t buf[9000];
    while (at + 4 <= n && steps++ < 48) {
        uint8_t op = d[at], l1 = d[at + 1] % 6, l2 = d[at + 2] % 6, arg = d[at + 3];
        at += 4;
        if (l1 > n - at) l1 = (uint8_t)(n - at);
        build_path(d + at, l1, p1, sizeof p1);
        at += l1;
        if (l2 > n - at) l2 = (uint8_t)(n - at);
        build_path(d + at, l2, p2, sizeof p2);
        at += l2;
        static int trace = -1;
        if (trace < 0) trace = getenv("FZ_TRACE") != NULL;
        if (trace) fprintf(stderr, "op %d arg %d p1=%s p2=%s\n", op % 14, arg, p1, p2);
        switch (op % 14) {
        case 0: vfs_mkdir(p1); break;
        case 1: vfs_unlink(p1); break;
        case 2: vfs_move(p1, p2); break;
        case 3: { struct file *f; if (!vfs_document_open(p1, N_DOCUMENT_CREATE, &f)) { if (files[arg & 3]) vfs_close(files[arg & 3]); files[arg & 3] = f; } break; }
        case 4: { struct file *f; if (!vfs_document_open(p1, N_DOCUMENT_EDIT, &f)) { if (files[arg & 3]) vfs_close(files[arg & 3]); files[arg & 3] = f; } break; }
        case 5: { struct file *f; if (!vfs_document_open(p1, N_DOCUMENT_REPLACE, &f)) { if (files[arg & 3]) vfs_close(files[arg & 3]); files[arg & 3] = f; } break; }
        case 6: if (files[arg & 3]) { memset(buf, arg, sizeof buf); vfs_document_write_user(files[arg & 3], (uint64_t)(arg >> 2) * 300, buf, (size_t)(arg % 17) * 400); } break;
        case 7: if (files[arg & 3]) vfs_document_read_user(files[arg & 3], (uint64_t)(arg >> 2) * 100, buf, (size_t)(arg % 23) * 400); break;
        case 8: if (files[arg & 3]) { vfs_document_commit(files[arg & 3]); vfs_close(files[arg & 3]); files[arg & 3] = NULL; } break;
        case 9: { static struct dirent e[8]; vfs_folder_list(p1, arg, e, 8); vfs_entry_info(p1, e); break; }
        case 10: if (files[arg & 3]) vfs_truncate(files[arg & 3], (uint64_t)arg * 97); break;
        case 11: vfs_append_user(p1, buf, arg * 20); break;
        case 12: { uint8_t *data; uint64_t sz; if (!vfs_read_whole(p1, &data, &sz)) kfree(data); break; }
        case 13: { uint8_t *z = kmalloc(arg * 30 + 1); memset(z, 7, arg * 30 + 1); vfs_replace(p1, z, arg * 30); break; }
        }
    }
    for (int i = 0; i < 4; i++) if (files[i]) vfs_close(files[i]);
}

static void free_tree(struct vnode *v, struct fat_fs **fs_out) {
    if (!v) return;
    free_tree(v->mounted, fs_out);
    if (v->ops == &fat_ops && v->priv && !*fs_out) *fs_out = ((struct fat_node *)v->priv)->fs;
    if (v->ops == &ramfs_ops) {
        for (struct vnode *c = v->children, *next; c; c = next) { next = c->sibling; free_tree(c, fs_out); }
        kfree(v->data);
        kfree(v);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_ready) init_golden();
    if (size < 1) return 0;
    memcpy(image, golden, sizeof image);
    cur = image;
    size_t at = 1;
    unsigned np = data[0] % 12;
    for (unsigned i = 0; i < np && at + 4 <= size; i++) {
        uint8_t target = data[at], len = data[at + 3] % 33;
        uint32_t off = data[at + 1] | (uint32_t)data[at + 2] << 8;
        at += 4;
        if (len > size - at) len = (uint8_t)(size - at);
        uint32_t base;
        switch (target % 6) {
        case 0: base = 0; off %= 512; break;                    /* BPB */
        case 1: base = 32 * 512; off %= 32 * 512; break;        /* FAT 1 */
        case 2: base = 64 * 512; off %= 32 * 512; break;        /* FAT 2 */
        case 3: base = 96 * 512; off %= 8 * 512; break;         /* root + first dir clusters */
        case 4: base = 96 * 512; off %= 40 * 512; break;
        default: base = 0; off %= IMG_SECTORS * 512u - 64; break;
        }
        memcpy(image + base + off, data + at, len);
        at += len;
    }
    reset_world();
    vfs_init();
    fat_mount_data();
    script(data + at, size - at);
    struct fat_fs *fs = NULL;
    struct vnode *root = vfs_root;
    free_tree(root, &fs);
    if (fs) {
        for (struct vnode *v = fs->nodes, *next; v; v = next) { next = v->sibling; kfree(v->priv); kfree(v); }
        if (fs->root) { kfree(fs->root->priv); kfree(fs->root); }
        fcache_forget(fs);
        kfree(fs);
    }
    vfs_root = NULL;
    return 0;
}
