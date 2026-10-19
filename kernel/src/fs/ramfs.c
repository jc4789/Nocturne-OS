/* ramfs: the in-memory root filesystem. */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "arch/cpu.h"
#include "dev/timer.h"

static int64_t ram_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    if (off >= v->size) return 0;
    if (n > v->size - off) n = v->size - off;
    memcpy(buf, v->data + off, n);
    return n;
}

static int ensure_cap(struct vnode *v, uint64_t need) {
    if (need <= v->cap) return 0;
    /* The first write sizes the file exactly (the initrd unpacks each file in
       one write, as do most whole-file saves); later growth doubles. */
    uint64_t nc = v->cap ? v->cap * 2 : need;
    if (nc < need) nc = need;
    uint8_t *nd = krealloc(v->data, nc);
    if (!nd) return -N_ERR_NO_MEMORY;
    v->data = nd;
    v->cap = nc;
    return 0;
}

static int64_t ram_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    if (!n) return 0; /* an empty file has no backing storage to offset into */
    if (off + n < off) return -N_ERR_NO_SPACE;
    if (ensure_cap(v, off + n) < 0) return -N_ERR_NO_SPACE;
    if (off > v->size) memset(v->data + v->size, 0, off - v->size);
    memcpy(v->data + off, buf, n);
    if (off + n > v->size) v->size = off + n;
    return n;
}

static int ram_truncate(struct vnode *v, uint64_t size) {
    if (size > v->size) {
        if (ensure_cap(v, size) < 0) return -N_ERR_NO_SPACE;
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
    out->mtime = c->mtime;
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
    if (ram_lookup(dir, name)) return -N_ERR_EXISTS;
    struct vnode *v = ramfs_new_node(name, type);
    if (!v) return -N_ERR_NO_MEMORY;
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
    if (!v) return -N_ERR_NOT_FOUND;
    if (v->type == VT_DIR && v->children) return -N_ERR_NOT_EMPTY;
    if (v->ops != &ramfs_ops) return -N_ERR_DENIED; /* device nodes */
    *pp = v->sibling;
    v->unlinked = true;
    vnode_unref(v);
    return 0;
}

static int ram_replace(struct vnode *v, uint8_t *data, uint64_t size) {
    uint64_t flags = irq_save();
    uint8_t *old = v->data;
    v->data = data;
    v->size = v->cap = data ? size : 0;
    v->mtime = time_now();
    irq_restore(flags);
    kfree(old);
    return 0;
}

static int ram_move(struct vnode *from_dir, const char *from, struct vnode *to_dir, const char *to) {
    if (strlen(to) >= NAME_MAX_LEN) return -N_ERR_NAME_TOO_LONG;
    if (ram_lookup(to_dir, to)) return -N_ERR_EXISTS;
    struct vnode **pp = &from_dir->children;
    while (*pp && strcmp((*pp)->name, from)) pp = &(*pp)->sibling;
    struct vnode *v = *pp;
    if (!v) return -N_ERR_NOT_FOUND;
    if (v->ops != &ramfs_ops || v->mounted) return -N_ERR_DENIED; /* device nodes, mount points */
    *pp = v->sibling;
    strlcpy(v->name, to, sizeof v->name);
    ramfs_add_child(to_dir, v);
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
    .replace = ram_replace,
    .move = ram_move,
    .release = ram_release,
};

/* N_DOCUMENT_REPLACE staging: an unnamed in-memory document that remembers
   its destination. Committing hands its bytes to the destination's replace. */
static void replace_release(struct vnode *v) {
    kfree(v->priv);
    kfree(v->data);
    kfree(v);
}

static struct vnode_ops replace_ops = {
    .read = ram_read,
    .write = ram_write,
    .truncate = ram_truncate,
    .release = replace_release,
};
/* A staging document after its commit: a duplicated handle closing later
   must not commit again, which would replace the document with nothing. */
static struct vnode_ops replaced_ops = {
    .read = ram_read,
    .release = replace_release,
};

struct vnode *ramfs_replace_new(const char *path) {
    struct vnode *v = kzalloc(sizeof *v);
    char *target = strdup(path);
    if (!v || !target) { kfree(v); kfree(target); return NULL; }
    strlcpy(v->name, "replacement", sizeof v->name);
    v->type = VT_FILE;
    v->ops = &replace_ops;
    v->mode = 0644;
    v->mtime = time_now();
    v->priv = target;
    v->unlinked = true; /* freed with its last handle */
    return v;
}

bool ramfs_is_replace(struct vnode *v) { return v->ops == &replace_ops; }

int ramfs_replace_commit(struct vnode *v) {
    uint64_t flags = irq_save();
    if (v->ops != &replace_ops) { irq_restore(flags); return 0; }
    uint8_t *data = v->data;
    uint64_t size = v->size;
    v->data = NULL;
    v->size = v->cap = 0;
    v->ops = &replaced_ops;
    irq_restore(flags);
    return vfs_replace(v->priv, data, size);
}

struct vnode *ramfs_new_root(void) { return ramfs_new_node("", VT_DIR); }
