/* Virtual filesystem layer: path walking, open files, mounts. */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "arch/cpu.h"
#include "dev/timer.h"
#include "sys/syscall.h"

struct vnode *vfs_root;

struct poll_entry {
    struct poll_entry *wait_next, *source_next;
    struct poll_entry **source_prev;
    struct poll_wait *wait;
    struct vnode *v;
};

void vfs_poll_register(struct poll_wait *wait, struct vnode *v) {
    if (wait->error || !v) return;
    struct poll_entry *entry = kmalloc(sizeof *entry);
    if (!entry) { wait->error = -N_ERR_NO_MEMORY; return; }
    uint64_t flags = irq_save();
    vnode_ref(v);
    *entry = (struct poll_entry){.wait_next=wait->entries,
        .source_next=v->pollers, .source_prev=&v->pollers, .wait=wait, .v=v};
    if (entry->source_next) entry->source_next->source_prev = &entry->source_next;
    v->pollers = entry;
    wait->entries = entry;
    irq_restore(flags);
}

void vfs_poll_file(struct poll_wait *wait, struct file *f) {
    vfs_poll_register(wait, f->vn);
    if (f->vn->ops && f->vn->ops->poll_register)
        f->vn->ops->poll_register(f->vn, f, wait);
}

void vfs_poll_notify(struct vnode *v) {
    if (!v) return;
    uint64_t flags = irq_save();
    for (struct poll_entry *entry = v->pollers; entry; entry = entry->source_next) {
        entry->wait->generation++;
        wq_wake_all(&entry->wait->wq);
    }
    irq_restore(flags);
}

void vfs_poll_cleanup(struct poll_wait *wait) {
    while (wait->entries) {
        uint64_t flags = irq_save();
        struct poll_entry *entry = wait->entries;
        wait->entries = entry->wait_next;
        *entry->source_prev = entry->source_next;
        if (entry->source_next) entry->source_next->source_prev = entry->source_prev;
        irq_restore(flags);
        vnode_unref(entry->v);
        kfree(entry);
    }
}

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
    if (!path || !*path) return -N_ERR_NOT_FOUND;
    if (path[0] == '/') {
        if (strlcpy(tmp, path, sizeof tmp) >= sizeof tmp) return -N_ERR_NAME_TOO_LONG;
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
        if (n >= 64) return -N_ERR_NAME_TOO_LONG;
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
        if (cl >= NAME_MAX_LEN) return -N_ERR_NAME_TOO_LONG;
        if (len + cl + 2 > PATH_MAX_LEN) return -N_ERR_NAME_TOO_LONG;
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
        if (!(flags & O_CREAT)) return -N_ERR_NOT_FOUND;
        char leaf[NAME_MAX_LEN];
        struct vnode *dir = lookup_parent(path, leaf);
        if (!dir || dir->type != VT_DIR) return -N_ERR_NOT_FOUND;
        if (!leaf[0]) return -N_ERR_INVALID;
        if (!dir->ops->create) return -N_ERR_READ_ONLY;
        int r = dir->ops->create(dir, leaf, VT_FILE, &v);
        if (r < 0) return r;
    } else if ((flags & O_TRUNC) && v->type == VT_FILE) {
        if (v->ops->truncate) v->ops->truncate(v, 0);
    }
    if ((flags & O_DIRECTORY) && v->type != VT_DIR) return -N_ERR_NOT_FOLDER;
    if (v->type == VT_DIR && (flags & O_ACCMODE) != O_RDONLY) return -N_ERR_IS_FOLDER;
    struct file *f = vfs_open_vnode(v, flags);
    if (!f) return -N_ERR_NO_MEMORY;
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
        if (current_task->killed) { irq_restore(flags); return -N_ERR_STOPPED; }
        wq_wait(&f->io_wait);
    }
    if (current_task->killed) { irq_restore(flags); return -N_ERR_STOPPED; }
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

static int64_t file_read(struct file *f, void *buf, size_t n, bool user) {
    struct vnode *v = f->vn;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -N_ERR_NO_HANDLE;
    if (v->type == VT_DIR) return -N_ERR_IS_FOLDER;
    /* User-only sources (address-watch queues) provide read_user alone. */
    if (!v->ops->read && !(user && v->ops->read_user)) return -N_ERR_INVALID;
    int error = file_io_begin(f);
    if (error) return error;
    int64_t r;
    if (user && v->ops->read_user) r = v->ops->read_user(v, f, buf, f->off, n);
    else {
        /* Cap the request at the current EOF under the shared-offset
           reservation; even a later size change cannot enlarge this request. */
        if (user && v->type == VT_FILE) n = f->off < v->size ? MIN(n, v->size - f->off) : 0;
        r = user && !user_ok_w(buf, n) ? -N_ERR_BAD_ADDRESS : v->ops->read(v, f, buf, f->off, n);
    }
    if (r > 0 && (v->type == VT_FILE)) f->off += r;
    file_io_end(f);
    return r;
}

int64_t vfs_read(struct file *f, void *buf, size_t n) { return file_read(f, buf, n, false); }
int64_t vfs_read_user(struct file *f, void *buf, size_t n) { return file_read(f, buf, n, true); }

static int64_t file_write(struct file *f, const void *buf, size_t n, bool user) {
    struct vnode *v = f->vn;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -N_ERR_NO_HANDLE;
    if (!v->ops->write) return -N_ERR_INVALID;
    int error = file_io_begin(f);
    if (error) return error;
    if ((f->flags & O_APPEND) && v->type == VT_FILE) f->off = v->size;
    int64_t r = user && v->ops->write_user ? v->ops->write_user(v, f, buf, f->off, n) :
        user && !user_ok(buf, n) ? -N_ERR_BAD_ADDRESS : v->ops->write(v, f, buf, f->off, n);
    if (r > 0 && v->type == VT_FILE) {
        f->off += r;
        v->mtime = time_now();
    }
    file_io_end(f);
    return r;
}

int64_t vfs_write(struct file *f, const void *buf, size_t n) { return file_write(f, buf, n, false); }
int64_t vfs_write_user(struct file *f, const void *buf, size_t n) { return file_write(f, buf, n, true); }

/* Resize an open regular file; new bytes read as zero. The shared-offset
   reservation orders it against reads and writes through the same file. */
int vfs_truncate(struct file *f, uint64_t size) {
    struct vnode *v = f->vn;
    if (v->type != VT_FILE) return -N_ERR_INVALID;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -N_ERR_NO_HANDLE;
    if (!v->ops->truncate) return -N_ERR_READ_ONLY;
    int error = file_io_begin(f);
    if (error) return error;
    error = v->ops->truncate(v, size);
    if (!error) v->mtime = time_now();
    file_io_end(f);
    return error;
}

static int vfs_lease_mode(struct file *f, bool acquire, bool blocking) {
    struct vnode *v = f->vn;
    if (v->type != VT_FILE) return -N_ERR_INVALID;
    uint64_t flags = irq_save();
    if (acquire) {
        while (v->lease_owner && v->lease_owner != f) {
            if (current_task->killed) { irq_restore(flags); return -N_ERR_STOPPED; }
            if (!blocking) { irq_restore(flags); return -N_ERR_NOT_READY; }
            wq_wait(&v->lease_wait);
        }
        if (current_task->killed) { irq_restore(flags); return -N_ERR_STOPPED; }
        v->lease_owner = f;
    } else if (v->lease_owner == f) {
        v->lease_owner = NULL;
        wq_wake_all(&v->lease_wait);
    } else if (v->lease_owner) {
        irq_restore(flags);
        return -N_ERR_DENIED;
    }
    irq_restore(flags);
    return 0;
}

int vfs_lease(struct file *f, bool acquire) {
    return vfs_lease_mode(f, acquire, true);
}

int vfs_try_lease(struct file *f) {
    return vfs_lease_mode(f, true, false);
}

void vfs_close(struct file *f) {
    if (!f) return;
    uint64_t flags = irq_save();
    bool last = --f->refs == 0;
    if (last) events_file_closed(f);
    irq_restore(flags);
    if (!last) return;
    /* The last close callback can drain audio and sleep. Reference ownership,
       not an IRQ-disabled driver invocation, keeps this detached file alive. */
    struct vnode *v = f->vn;
    vfs_lease(f, false);
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

int vfs_mkdir(const char *path) {
    if (vfs_lookup(path)) return -N_ERR_EXISTS;
    char leaf[NAME_MAX_LEN];
    struct vnode *dir = lookup_parent(path, leaf);
    if (!dir || dir->type != VT_DIR) return -N_ERR_NOT_FOUND;
    if (!dir->ops->create) return -N_ERR_READ_ONLY;
    struct vnode *out;
    return dir->ops->create(dir, leaf, VT_DIR, &out);
}

int vfs_unlink(const char *path) {
    if (!strcmp(path, "/")) return -N_ERR_BUSY;
    char leaf[NAME_MAX_LEN];
    struct vnode *dir = lookup_parent(path, leaf);
    if (!dir) return -N_ERR_NOT_FOUND;
    struct vnode *v = vfs_lookup(path);
    if (!v) return -N_ERR_NOT_FOUND;
    if (v->mounted || v == vfs_root) return -N_ERR_BUSY;
    if (!dir->ops->unlink) return -N_ERR_READ_ONLY;
    return dir->ops->unlink(dir, leaf);
}

int vfs_mount(const char *path, struct vnode *root) {
    struct vnode *v = vfs_lookup(path);
    if (!v) {
        int r = vfs_mkdir(path);
        if (r < 0) return r;
        v = vfs_lookup(path);
    }
    if (!v || v->type != VT_DIR) return -N_ERR_NOT_FOLDER;
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
        return -N_ERR_IS_FOLDER;
    }
    uint64_t sz = f->vn->size;
    uint8_t *buf = kmalloc(sz + 1);
    if (!buf) {
        vfs_close(f);
        return -N_ERR_NO_MEMORY;
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

/* ---- native documents and folders ---- */

int vfs_kind(struct vnode *v) {
    switch (v->type) {
    case VT_FILE: return N_KIND_DOCUMENT;
    case VT_DIR: return N_KIND_FOLDER;
    case VT_CHANNEL: return N_KIND_CHANNEL;
    case VT_WINDOW: return N_KIND_WINDOW;
    case VT_SHARED: return N_KIND_SHARED;
    case VT_APP: return N_KIND_APP;
    case VT_PCI: return N_KIND_PCI;
    case VT_EVENTS: case VT_ADDRESS_WATCH: return N_KIND_EVENTS;
    default: return N_KIND_DEVICE;
    }
}

/* A new, empty document; the name must be free. */
static int create_document(const char *path, struct vnode **out) {
    char leaf[NAME_MAX_LEN];
    struct vnode *dir = lookup_parent(path, leaf);
    if (!dir) return -N_ERR_NOT_FOUND;
    if (dir->type != VT_DIR) return -N_ERR_NOT_FOLDER;
    if (!leaf[0]) return -N_ERR_INVALID;
    if (vfs_lookup(path)) return -N_ERR_EXISTS;
    if (!dir->ops->create) return -N_ERR_READ_ONLY;
    return dir->ops->create(dir, leaf, VT_FILE, out);
}

static int existing_document(const char *path, struct vnode **out) {
    struct vnode *v = vfs_lookup(path);
    if (!v) return -N_ERR_NOT_FOUND;
    if (v->type == VT_DIR) return -N_ERR_IS_FOLDER;
    if (v->type != VT_FILE) return -N_ERR_WRONG_KIND;
    *out = v;
    return 0;
}

int vfs_document_open(const char *path, int access, struct file **out) {
    struct vnode *v;
    int r, flags = O_RDWR;
    switch (access) {
    case N_DOCUMENT_READ: flags = O_RDONLY; /* fall through */
    case N_DOCUMENT_EDIT: r = existing_document(path, &v); break;
    case N_DOCUMENT_CREATE: r = create_document(path, &v); break;
    case N_DOCUMENT_REPLACE: {
        /* The staged content lives in memory until the handle is closed with
           n_handle_close; an app that ends first leaves the old document. */
        struct vnode *old = vfs_lookup(path);
        char leaf[NAME_MAX_LEN];
        struct vnode *dir = lookup_parent(path, leaf);
        if (old && old->type == VT_DIR) return -N_ERR_IS_FOLDER;
        if (old && old->type != VT_FILE) return -N_ERR_WRONG_KIND;
        if (!old && (!dir || !leaf[0])) return -N_ERR_NOT_FOUND;
        if (!old && dir->type != VT_DIR) return -N_ERR_NOT_FOLDER;
        if (!(old ? old->ops : dir->ops)->replace) return -N_ERR_UNSUPPORTED;
        v = ramfs_replace_new(path);
        r = v ? 0 : -N_ERR_NO_MEMORY;
        break;
    }
    default: return -N_ERR_INVALID;
    }
    if (r < 0) return r;
    struct file *f = vfs_open_vnode(v, flags);
    if (!f) {
        if (ramfs_is_replace(v)) v->ops->release(v);
        return -N_ERR_NO_MEMORY;
    }
    *out = f;
    return 0;
}

/* Positional access: no shared cursor. The per-handle reservation still
   orders requests issued through the same handle by sibling threads. */
int64_t vfs_document_read_user(struct file *f, uint64_t off, void *buf, size_t n) {
    struct vnode *v = f->vn;
    if (v->type != VT_FILE) return -N_ERR_WRONG_KIND;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -N_ERR_DENIED;
    if (!v->ops->read) return -N_ERR_UNSUPPORTED;
    int error = file_io_begin(f);
    if (error) return error;
    n = off < v->size ? MIN(n, v->size - off) : 0;
    int64_t r = !n ? 0 : !user_ok_w(buf, n) ? -N_ERR_BAD_ADDRESS : v->ops->read(v, f, buf, off, n);
    file_io_end(f);
    return r;
}

int64_t vfs_document_write_user(struct file *f, uint64_t off, const void *buf, size_t n) {
    struct vnode *v = f->vn;
    if (v->type != VT_FILE) return -N_ERR_WRONG_KIND;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -N_ERR_DENIED;
    if (!v->ops->write) return -N_ERR_READ_ONLY;
    if (off + n < off) return -N_ERR_TOO_LARGE;
    if (!n) return 0;
    int error = file_io_begin(f);
    if (error) return error;
    int64_t r = !user_ok(buf, n) ? -N_ERR_BAD_ADDRESS : v->ops->write(v, f, buf, off, n);
    if (r > 0) v->mtime = time_now();
    file_io_end(f);
    return r;
}

int vfs_document_commit(struct file *f) {
    if (!ramfs_is_replace(f->vn)) return 0;
    int error = file_io_begin(f);
    if (error) return error;
    error = ramfs_replace_commit(f->vn);
    file_io_end(f);
    return error;
}

int vfs_replace(const char *path, uint8_t *data, uint64_t size) {
    struct vnode *v = vfs_lookup(path);
    int r = 0;
    if (!v) r = create_document(path, &v);
    else if (v->type == VT_DIR) r = -N_ERR_IS_FOLDER;
    else if (v->type != VT_FILE) r = -N_ERR_WRONG_KIND;
    else if (!v->ops->replace) r = -N_ERR_UNSUPPORTED;
    if (r < 0) { kfree(data); return r; }
    if (!v->ops->replace) { kfree(data); return -N_ERR_UNSUPPORTED; }
    return v->ops->replace(v, data, size);
}

/* Log-style append of user bytes. The document's claim is held from taking
   the end to the last byte, so concurrent appenders never overwrite each other. */
int vfs_append_user(const char *path, const void *data, uint64_t size) {
    struct vnode *v;
    int r = existing_document(path, &v);
    if (r == -N_ERR_NOT_FOUND) r = create_document(path, &v);
    if (r == -N_ERR_EXISTS) r = existing_document(path, &v);
    if (r < 0) return r;
    if (!v->ops->write) return -N_ERR_READ_ONLY;
    struct file *f = vfs_open_vnode(v, O_WRONLY | O_APPEND);
    if (!f) return -N_ERR_NO_MEMORY;
    int64_t n = vfs_lease(f, true);
    if (!n) n = vfs_write_user(f, data, size);
    vfs_close(f); /* releases the claim */
    return n < 0 ? (int)n : (uint64_t)n < size ? -N_ERR_NO_SPACE : 0;
}

int vfs_entry_info(const char *path, struct dirent *out) {
    struct vnode *v = vfs_lookup(path);
    if (!v) return -N_ERR_NOT_FOUND;
    const char *slash = strrchr(path, '/');
    strlcpy(out->name, slash && slash[1] ? slash + 1 : "/", sizeof out->name);
    out->type = v->type;
    out->reserved = 0;
    out->size = v->type == VT_DIR ? 0 : v->size;
    out->mtime = v->mtime;
    return 0;
}

int vfs_folder_list(const char *path, uint64_t start, struct dirent *out, size_t max) {
    struct vnode *v = vfs_lookup(path);
    if (!v) return -N_ERR_NOT_FOUND;
    if (v->type != VT_DIR) return -N_ERR_NOT_FOLDER;
    if (!v->ops->readdir) return 0;
    size_t n = 0;
    while (n < max) {
        int r = v->ops->readdir(v, start + n, &out[n]);
        if (r < 0) return n ? (int)n : r;
        if (!r) break;
        n++;
    }
    return (int)n;
}

int vfs_move(const char *from, const char *to) {
    if (!strcmp(from, "/") || !strcmp(to, "/")) return -N_ERR_BUSY;
    size_t fl = strlen(from);
    /* A folder cannot move into itself. */
    if (!strncmp(to, from, fl) && (to[fl] == '/' || !to[fl])) return to[fl] ? -N_ERR_INVALID : 0;
    struct vnode *v = vfs_lookup(from);
    if (!v) return -N_ERR_NOT_FOUND;
    if (v->mounted || v == follow_mount(vfs_root)) return -N_ERR_BUSY;
    if (vfs_lookup(to)) return -N_ERR_EXISTS;
    char from_leaf[NAME_MAX_LEN], to_leaf[NAME_MAX_LEN];
    struct vnode *from_dir = lookup_parent(from, from_leaf), *to_dir = lookup_parent(to, to_leaf);
    if (!from_dir || !to_dir) return -N_ERR_NOT_FOUND;
    if (to_dir->type != VT_DIR) return -N_ERR_NOT_FOLDER;
    if (!to_leaf[0]) return -N_ERR_INVALID;
    if (from_dir->ops == to_dir->ops && from_dir->ops->move) {
        int moved = from_dir->ops->move(from_dir, from_leaf, to_dir, to_leaf);
        if (moved != -N_ERR_UNSUPPORTED) return moved;
    }
    /* Another filesystem: a document is copied whole, then the original is
       removed. A folder would need a recursive copy that could fail halfway. */
    if (v->type != VT_FILE) return -N_ERR_UNSUPPORTED;
    uint8_t *data;
    uint64_t size;
    int r = vfs_read_whole(from, &data, &size);
    if (r < 0) return r;
    r = vfs_replace(to, data, size);
    return r < 0 ? r : vfs_unlink(from);
}
