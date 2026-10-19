/* libFuzzer harness: path normalisation / lookup / document operations of
   kernel/src/fs/vfs.c on top of ramfs.c, and the USTAR unpacker initrd.c.

   Input byte 0 selects the mode:
     even: the rest is an archive handed to initrd_load() (the archive buffer is
           an exact-size heap copy so any overread is an ASan report), then a
           few path operations are run on the result;
     odd:  the rest is an operation script (see below). */
#include "../../../kernel/src/fs/vfs.c"
#include "../../../kernel/src/fs/ramfs.c"
#include "../../../kernel/src/fs/initrd.c"

/* initrd_load wants a flat root; a fresh ramfs root per input keeps runs independent. */
static const char *pool[] = {"a", "b", "c", ".", "..", "", "aa", "x.y", "d"};

static size_t build_path(const uint8_t *d, size_t n, char *out, size_t cap) {
    size_t at = 0;
    for (size_t i = 0; i < n && at + 8 < cap; i++) {
        if (d[i] & 0x80) { out[at++] = '/'; }
        const char *c = pool[d[i] % (sizeof pool / sizeof *pool)];
        size_t l = strlen(c);
        memcpy(out + at, c, l);
        at += l;
        if (d[i] & 0x40) { out[at++] = '/'; }
    }
    out[at] = 0;
    return at;
}

static void run_script(const uint8_t *d, size_t n) {
    char p1[PATH_MAX_LEN * 3], p2[PATH_MAX_LEN * 3], abs1[PATH_MAX_LEN], abs2[PATH_MAX_LEN];
    struct file *open_files[4] = {0};
    size_t at = 0;
    int steps = 0;
    while (at + 4 <= n && steps++ < 64) {
        uint8_t op = d[at], l1 = d[at + 1] % 40, l2 = d[at + 2] % 40, arg = d[at + 3];
        at += 4;
        if (l1 > n - at) l1 = (uint8_t)(n - at);
        build_path(d + at, l1, p1, sizeof p1);
        at += l1;
        if (l2 > n - at) l2 = (uint8_t)(n - at);
        build_path(d + at, l2, p2, sizeof p2);
        at += l2;
        const char *cwd = (arg & 1) ? "/a/b" : NULL;
        if (vfs_normalize(cwd, p1, abs1) < 0) strcpy(abs1, "/");
        if (vfs_normalize("/", p2, abs2) < 0) strcpy(abs2, "/");
        switch (op % 16) {
        case 0: vfs_mkdir(abs1); break;
        case 1: vfs_unlink(abs1); break;
        case 2: vfs_move(abs1, abs2); break;
        case 3: { struct file *f; if (!vfs_document_open(abs1, N_DOCUMENT_CREATE, &f)) { if (open_files[arg & 3]) vfs_close(open_files[arg & 3]); open_files[arg & 3] = f; } break; }
        case 4: { struct file *f; if (!vfs_document_open(abs1, N_DOCUMENT_EDIT, &f)) { if (open_files[arg & 3]) vfs_close(open_files[arg & 3]); open_files[arg & 3] = f; } break; }
        case 5: { struct file *f; if (!vfs_document_open(abs1, N_DOCUMENT_REPLACE, &f)) { if (open_files[arg & 3]) vfs_close(open_files[arg & 3]); open_files[arg & 3] = f; } break; }
        case 6: { struct file *f = open_files[arg & 3]; if (f) { uint8_t b[64]; memset(b, arg, sizeof b);
                  uint64_t off = (arg & 0x80) ? ~0ull - (arg & 31) : (uint64_t)arg * 3; vfs_document_write_user(f, off, b, arg & 63); } break; }
        case 7: { struct file *f = open_files[arg & 3]; if (f) { uint8_t b[64]; vfs_document_read_user(f, (uint64_t)arg * 5, b, arg & 63); } break; }
        case 8: { struct file *f = open_files[arg & 3]; if (f) { vfs_document_commit(f); vfs_close(f); open_files[arg & 3] = NULL; } break; }
        case 9: { struct dirent e[4]; vfs_folder_list(abs1, arg, e, 4); vfs_entry_info(abs1, e); break; }
        case 10: { struct file *f = open_files[arg & 3]; if (f) vfs_truncate(f, (arg & 0x80) ? ((uint64_t)1 << 40) : arg); break; }
        case 11: vfs_append_user(abs1, "hello", 5); break;
        case 12: { uint8_t *buf = kmalloc(16); memset(buf, 1, 16); vfs_replace(abs1, buf, 16); break; }
        case 13: { uint8_t *data; uint64_t sz; if (!vfs_read_whole(abs1, &data, &sz)) kfree(data); break; }
        case 14: { struct file *f = open_files[arg & 3]; if (f) { uint8_t b[64]; vfs_write(f, b, arg & 63); vfs_read(f, b, arg & 63); } break; }
        case 15: vfs_mount(abs1, ramfs_new_root()); break;
        }
    }
    for (int i = 0; i < 4; i++) if (open_files[i]) vfs_close(open_files[i]);
}

/* Release the whole tree so repeated inputs do not accumulate (harness-only). */
static void free_tree(struct vnode *v) {
    if (!v) return;
    free_tree(v->mounted);
    struct vnode *c = v->children;
    while (c) {
        struct vnode *next = c->sibling;
        free_tree(c);
        c = next;
    }
    kfree(v->data);
    kfree(v);
}

static int fuzz_one(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    vfs_init();
    int rc = fuzz_one(data, size);
    free_tree(vfs_root);
    vfs_root = NULL;
    return rc;
}

static int fuzz_one(const uint8_t *data, size_t size) {
    if (data[0] & 1) {
        run_script(data + 1, size - 1);
    } else {
        /* exact-size heap copy; 512-byte tail not rounded so overreads trap */
        uint8_t *tar = malloc(size - 1 ? size - 1 : 1);
        memcpy(tar, data + 1, size - 1);
        initrd_load(tar, size - 1);
        free(tar);
        char abs1[PATH_MAX_LEN];
        vfs_normalize("/", "/a/../b//./c", abs1);
        struct dirent e[8];
        vfs_folder_list("/", 0, e, 8);
    }
    return 0;
}
