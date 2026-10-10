/* Virtual filesystem layer: path walking, open files, mounts. */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "arch/cpu.h"
#include "dev/timer.h"

struct vnode *vfs_root;

void vnode_ref(struct vnode *v) {
    uint64_t flags = irq_save();
    if (v) v->refs++;
    irq_restore(flags);
}

void vnode_unref(struct vnode *v) {
    if (!v) return;
    uint64_t flags = irq_save();
    bool release = --v->refs <= 0 && v->unlinked;
    irq_restore(flags);
    if (release) {
        if (v->ops && v->ops->release) v->ops->release(v);
    }
}

void vfs_init(void) {
    vfs_root = ramfs_new_root();
    vfs_root->refs = 1;
}

int vfs_normalize(const char *cwd, const char *path, char *out) {
    char tmp[PATH_MAX_LEN * 2];
    if (!path || !*path) return -ENOENT;
    if (path[0] == '/') {
        if (strlcpy(tmp, path, sizeof tmp) >= sizeof tmp) return -ENAMETOOLONG;
    } else {
        ksnprintf(tmp, sizeof tmp, "%s/%s", cwd ? cwd : "/", path);
    }
    /* split into components, resolving . and .. */
    char *comps[64];
    int n = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (!strcmp(start, ".")) continue;
        if (!strcmp(start, "..")) {
            if (n > 0) n--;
            continue;
        }
        if (n >= 64) return -ENAMETOOLONG;
        comps[n++] = start;
    }
    size_t len = 0;
    out[0] = 0;
    if (n == 0) {
        strcpy(out, "/");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        size_t cl = strlen(comps[i]);
        if (cl >= NAME_MAX_LEN) return -ENAMETOOLONG;
        if (len + cl + 2 > PATH_MAX_LEN) return -ENAMETOOLONG;
        out[len++] = '/';
        memcpy(out + len, comps[i], cl);
        len += cl;
    }
    out[len] = 0;
    return 0;
}

static struct vnode *follow_mount(struct vnode *v) {
    while (v && v->mounted) v = v->mounted;
    return v;
}

struct vnode *vfs_lookup(const char *path) {
    struct vnode *v = follow_mount(vfs_root);
    if (path[0] != '/') return NULL;
    const char *p = path;
    char name[NAME_MAX_LEN];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t l = 0;
        while (*p && *p != '/') {
            if (l < NAME_MAX_LEN - 1) name[l++] = *p;
            p++;
        }
        name[l] = 0;
        if (v->type != VT_DIR || !v->ops->lookup) return NULL;
        v = v->ops->lookup(v, name);
        if (!v) return NULL;
        v = follow_mount(v);
    }
    return v;
}

static struct vnode *lookup_parent(const char *path, char *leaf) {
    char dir[PATH_MAX_LEN];
    strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (!slash) return NULL;
    strlcpy(leaf, slash + 1, NAME_MAX_LEN);
    if (slash == dir) dir[1] = 0;
    else *slash = 0;
    return vfs_lookup(dir);
}

struct file *vfs_open_vnode(struct vnode *v, int flags) {
    struct file *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->vn = v;
    f->flags = flags;
    f->refs = 1;
    vnode_ref(v);
    if (v->ops && v->ops->open) v->ops->open(v, f);
    return f;
}

int vfs_open(const char *path, int flags, struct file **out) {
    struct vnode *v = vfs_lookup(path);
    if (!v) {
        if (!(flags & O_CREAT)) return -ENOENT;
        char leaf[NAME_MAX_LEN];
        struct vnode *dir = lookup_parent(path, leaf);
        if (!dir || dir->type != VT_DIR) return -ENOENT;
        if (!leaf[0]) return -EINVAL;
        if (!dir->ops->create) return -EROFS;
        int r = dir->ops->create(dir, leaf, VT_FILE, &v);
        if (r < 0) return r;
    } else if ((flags & O_TRUNC) && v->type == VT_FILE) {
        if (v->ops->truncate) v->ops->truncate(v, 0);
    }
    if ((flags & O_DIRECTORY) && v->type != VT_DIR) return -ENOTDIR;
    if (v->type == VT_DIR && (flags & O_ACCMODE) != O_RDONLY) return -EISDIR;
    struct file *f = vfs_open_vnode(v, flags);
    if (!f) return -ENOMEM;
    if (flags & O_APPEND) f->off = v->size;
    *out = f;
    return 0;
}

/* A regular-file operation may sleep in its device. Do not hold IRQs off
   across that callback, but do serialize its shared offset reservation/update.
   Pipe/window/audio directions must remain independent to avoid read/write
   dependency deadlocks. Every caller holds a file reference while waiting. */
static int file_io_begin(struct file *f) {
    if (f->vn->type != VT_FILE) return 0;
    uint64_t flags = irq_save();
    while (f->io_busy) {
        if (current_task->killed) { irq_restore(flags); return -EINTR; }
        wq_wait(&f->io_wait);
    }
    if (current_task->killed) { irq_restore(flags); return -EINTR; }
    f->io_busy = true;
    irq_restore(flags);
    return 0;
}
static void file_io_end(struct file *f) {
    if (f->vn->type != VT_FILE) return;
    uint64_t flags = irq_save();
    f->io_busy = false;
    wq_wake_all(&f->io_wait);
    irq_restore(flags);
}

int64_t vfs_read(struct file *f, void *buf, size_t n) {
    struct vnode *v = f->vn;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (v->type == VT_DIR) return -EISDIR;
    if (!v->ops->read) return -EINVAL;
    int error = file_io_begin(f);
    if (error) return error;
    int64_t r = v->ops->read(v, f, buf, f->off, n);
    if (r > 0 && (v->type == VT_FILE)) f->off += r;
    file_io_end(f);
    return r;
}

int64_t vfs_write(struct file *f, const void *buf, size_t n) {
    struct vnode *v = f->vn;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!v->ops->write) return -EINVAL;
    int error = file_io_begin(f);
    if (error) return error;
    if ((f->flags & O_APPEND) && v->type == VT_FILE) f->off = v->size;
    int64_t r = v->ops->write(v, f, buf, f->off, n);
    if (r > 0 && v->type == VT_FILE) {
        f->off += r;
        v->mtime = time_now();
    }
    file_io_end(f);
    return r;
}

int64_t vfs_seek(struct file *f, int64_t off, int whence) {
    if (f->vn->type != VT_FILE) return -ESPIPE;
    int error = file_io_begin(f);
    if (error) return error;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off : (int64_t)f->vn->size;
    int64_t n = base + off;
    if (n >= 0) f->off = n;
    file_io_end(f);
    return n < 0 ? -EINVAL : n;
}

void vfs_close(struct file *f) {
    if (!f) return;
    uint64_t flags = irq_save();
    bool last = --f->refs == 0;
    irq_restore(flags);
    if (!last) return;
    /* The last close callback can drain audio and sleep. Reference ownership,
       not an IRQ-disabled driver invocation, keeps this detached file alive. */
    struct vnode *v = f->vn;
    if (v->ops && v->ops->close) v->ops->close(v, f);
    vnode_unref(v);
    kfree(f);
}

struct file *vfs_dup(struct file *f) {
    uint64_t flags = irq_save();
    f->refs++;
    irq_restore(flags);
    return f;
}

int vfs_readdir(struct file *f, uint64_t idx, struct dirent *out) {
    struct vnode *v = f->vn;
    if (v->type != VT_DIR) return -ENOTDIR;
    if (!v->ops->readdir) return 0;
    return v->ops->readdir(v, idx, out);
}

int vfs_stat(const char *path, struct kstat *st) {
    struct vnode *v = vfs_lookup(path);
    if (!v) return -ENOENT;
    st->type = v->type;
    st->size = v->size;
    st->mtime = v->mtime;
    st->mode = v->mode;
    return 0;
}

int vfs_mkdir(const char *path) {
    if (vfs_lookup(path)) return -EEXIST;
    char leaf[NAME_MAX_LEN];
    struct vnode *dir = lookup_parent(path, leaf);
    if (!dir || dir->type != VT_DIR) return -ENOENT;
    if (!dir->ops->create) return -EROFS;
    struct vnode *out;
    return dir->ops->create(dir, leaf, VT_DIR, &out);
}

int vfs_unlink(const char *path) {
    if (!strcmp(path, "/")) return -EBUSY;
    char leaf[NAME_MAX_LEN];
    struct vnode *dir = lookup_parent(path, leaf);
    if (!dir) return -ENOENT;
    struct vnode *v = vfs_lookup(path);
    if (!v) return -ENOENT;
    if (v->mounted || v == vfs_root) return -EBUSY;
    if (!dir->ops->unlink) return -EROFS;
    return dir->ops->unlink(dir, leaf);
}

int vfs_mount(const char *path, struct vnode *root) {
    struct vnode *v = vfs_lookup(path);
    if (!v) {
        int r = vfs_mkdir(path);
        if (r < 0) return r;
        v = vfs_lookup(path);
    }
    if (!v || v->type != VT_DIR) return -ENOTDIR;
    v->mounted = root;
    root->parent = v->parent;
    return 0;
}

bool vfs_can_read(struct file *f) {
    struct vnode *v = f->vn;
    if (v->ops && v->ops->can_read) return v->ops->can_read(v, f);
    return true;
}

bool vfs_can_write(struct file *f) {
    struct vnode *v = f->vn;
    if (v->ops && v->ops->can_write) return v->ops->can_write(v, f);
    return true;
}

int vfs_read_whole(const char *path, uint8_t **data, uint64_t *size) {
    struct file *f;
    int r = vfs_open(path, O_RDONLY, &f);
    if (r < 0) return r;
    if (f->vn->type != VT_FILE) {
        vfs_close(f);
        return -EISDIR;
    }
    uint64_t sz = f->vn->size;
    uint8_t *buf = kmalloc(sz + 1);
    if (!buf) {
        vfs_close(f);
        return -ENOMEM;
    }
    uint64_t got = 0;
    while (got < sz) {
        int64_t n = vfs_read(f, buf + got, sz - got);
        if (n <= 0) break;
        got += n;
    }
    buf[got] = 0;
    vfs_close(f);
    *data = buf;
    *size = got;
    return 0;
}

int vfs_write_whole(const char *path, const void *data, uint64_t size) {
    struct file *f;
    int r = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if (r < 0) return r;
    int64_t n = vfs_write(f, data, size);
    vfs_close(f);
    return n < 0 ? (int)n : 0;
}
