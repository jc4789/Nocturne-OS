/* ramfs: the in-memory root filesystem. */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "dev/timer.h"

static int64_t ram_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    if (off >= v->size) return 0;
    if (off + n > v->size) n = v->size - off;
    memcpy(buf, v->data + off, n);
    return n;
}

static int ensure_cap(struct vnode *v, uint64_t need) {
    if (need <= v->cap) return 0;
    uint64_t nc = v->cap ? v->cap : 256;
    while (nc < need) nc *= 2;
    uint8_t *nd = krealloc(v->data, nc);
    if (!nd) return -ENOMEM;
    v->data = nd;
    v->cap = nc;
    return 0;
}

static int64_t ram_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    if (ensure_cap(v, off + n) < 0) return -ENOSPC;
    if (off > v->size) memset(v->data + v->size, 0, off - v->size);
    memcpy(v->data + off, buf, n);
    if (off + n > v->size) v->size = off + n;
    return n;
}

static int ram_truncate(struct vnode *v, uint64_t size) {
    if (size > v->size) {
        if (ensure_cap(v, size) < 0) return -ENOSPC;
        memset(v->data + v->size, 0, size - v->size);
    }
    v->size = size;
    return 0;
}

static int ram_readdir(struct vnode *v, uint64_t idx, struct dirent *out) {
    struct vnode *c = v->children;
    while (c && idx--) c = c->sibling;
    if (!c) return 0;
    strlcpy(out->name, c->name, sizeof out->name);
    out->type = c->mounted ? VT_DIR : c->type;
    out->size = c->size;
    return 1;
}

static struct vnode *ram_lookup(struct vnode *dir, const char *name) {
    for (struct vnode *c = dir->children; c; c = c->sibling)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

struct vnode *ramfs_new_node(const char *name, int type) {
    struct vnode *v = kzalloc(sizeof *v);
    strlcpy(v->name, name, sizeof v->name);
    v->type = type;
    v->ops = &ramfs_ops;
    v->mode = type == VT_DIR ? 0755 : 0644;
    v->mtime = time_now();
    v->refs = 1; /* the directory's reference */
    return v;
}

void ramfs_add_child(struct vnode *dir, struct vnode *child) {
    child->parent = dir;
    /* keep entries sorted by name */
    struct vnode **pp = &dir->children;
    while (*pp && strcmp((*pp)->name, child->name) < 0) pp = &(*pp)->sibling;
    child->sibling = *pp;
    *pp = child;
}

static int ram_create(struct vnode *dir, const char *name, int type, struct vnode **out) {
    if (ram_lookup(dir, name)) return -EEXIST;
    struct vnode *v = ramfs_new_node(name, type);
    if (!v) return -ENOMEM;
    ramfs_add_child(dir, v);
    *out = v;
    return 0;
}

static void ram_release(struct vnode *v) {
    kfree(v->data);
    kfree(v);
}

static int ram_unlink(struct vnode *dir, const char *name) {
    struct vnode **pp = &dir->children;
    while (*pp && strcmp((*pp)->name, name)) pp = &(*pp)->sibling;
    struct vnode *v = *pp;
    if (!v) return -ENOENT;
    if (v->type == VT_DIR && v->children) return -ENOTEMPTY;
    if (v->ops != &ramfs_ops) return -EPERM; /* device nodes */
    *pp = v->sibling;
    v->unlinked = true;
    vnode_unref(v);
    return 0;
}

struct vnode_ops ramfs_ops = {
    .read = ram_read,
    .write = ram_write,
    .readdir = ram_readdir,
    .lookup = ram_lookup,
    .create = ram_create,
    .unlink = ram_unlink,
    .truncate = ram_truncate,
    .release = ram_release,
};

struct vnode *ramfs_new_root(void) { return ramfs_new_node("", VT_DIR); }
