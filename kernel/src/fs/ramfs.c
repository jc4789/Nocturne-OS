/* ramfs: the in-memory root filesystem. A document's bytes are a content (fs/content.h) with no
   medium: pages of its own, or the initrd archive's bytes read in place until a page is changed. */
#include "fs/vfs.h"
#include "fs/content.h"
#include "mm/heap.h"
#include "arch/cpu.h"
#include "dev/timer.h"

/* A read-only handle reads the version it opened (f->priv); the others read the document's. */
static struct content *view(struct vnode *v, struct file *f) {
    return f && f->priv ? f->priv : v->content;
}

static int64_t ram_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    return content_read(view(v, f), buf, off, n);
}

static int64_t ram_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    int64_t r = content_write(v->content, buf, off, n);
    v->size = content_size(v->content);
    return r;
}

static int ram_truncate(struct vnode *v, uint64_t size) {
    int r = content_truncate(v->content, size);
    v->size = content_size(v->content);
    return r;
}

static uint64_t ram_size(struct vnode *v, struct file *f) { return content_size(view(v, f)); }

static void ram_open(struct vnode *v, struct file *f) {
    if (v->type == VT_FILE && (f->flags & O_ACCMODE) == O_RDONLY) f->priv = content_ref(v->content);
}

static void ram_close(struct vnode *v, struct file *f) {
    if (v->type == VT_FILE && f->priv) content_unref(f->priv);
    f->priv = NULL;
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

/* A node holding content c (documents only; consumed). */
static struct vnode *node_new(const char *name, int type, struct content *c) {
    struct vnode *v = kzalloc(sizeof *v);
    if (!v || (type == VT_FILE && !c)) {
        kfree(v);
        content_unref(c);
        return NULL;
    }
    strlcpy(v->name, name, sizeof v->name);
    v->type = type;
    v->ops = &ramfs_ops;
    v->mode = type == VT_DIR ? 0755 : 0644;
    v->mtime = time_now();
    v->refs = 1; /* the directory's reference */
    v->content = c;
    v->size = c ? content_size(c) : 0;
    return v;
}

struct vnode *ramfs_new_node(const char *name, int type) {
    return node_new(name, type, type == VT_FILE ? content_new(NULL, NULL) : NULL);
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
    content_unref(v->content);
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

bool ramfs_borrow(struct vnode *v, const uint8_t *data, uint64_t size) {
    if (v->ops != &ramfs_ops || v->type != VT_FILE || !size) return false;
    struct content *c = content_new(NULL, NULL);
    if (!c) return false;
    content_borrow(c, data, size);
    content_unref(v->content);
    v->content = c;
    v->size = size;
    return true;
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

/* ---- unpublished versions (N_DOCUMENT_REPLACE): an unnamed node that remembers its folder and
   name; publishing hands its content to the document there, or makes the document. */

static struct vnode_ops staged_ops;

static int ram_stage(struct vnode *dir, const char *name, struct vnode **out) {
    if (strlen(name) >= NAME_MAX_LEN) return -N_ERR_NAME_TOO_LONG;
    struct vnode *v = ramfs_new_node(name, VT_FILE);
    if (!v) return -N_ERR_NO_MEMORY;
    v->ops = &staged_ops;
    v->refs = 0; /* its handles' only */
    v->unlinked = true;
    v->staged = true;
    v->parent = dir;
    vnode_ref(dir);
    *out = v;
    return 0;
}

static int ram_publish(struct vnode *s) {
    struct vnode *dir = s->parent;
    if (dir->unlinked) return -N_ERR_NOT_FOUND;
    struct vnode *old = ram_lookup(dir, s->name);
    if (old && old->type == VT_DIR) return -N_ERR_IS_FOLDER;
    if (old && (old->type != VT_FILE || old->ops != &ramfs_ops)) return -N_ERR_WRONG_KIND;
    struct content *empty = content_new(NULL, NULL), *c = s->content;
    if (!empty) return -N_ERR_NO_MEMORY;
    if (old) {
        struct content *was = old->content;
        old->content = c;
        old->size = content_size(c);
        old->mtime = time_now();
        content_unref(was); /* read-only handles keep the version they opened */
    } else {
        struct vnode *v = node_new(s->name, VT_FILE, c);
        if (!v) {
            content_unref(empty);
            return -N_ERR_NO_MEMORY;
        }
        ramfs_add_child(dir, v);
    }
    s->content = empty;
    s->size = 0;
    s->staged = false;
    return 0;
}

static void staged_release(struct vnode *v) {
    struct vnode *dir = v->parent;
    ram_release(v);
    vnode_unref(dir);
}

static struct vnode_ops staged_ops = {
    .read = ram_read,
    .write = ram_write,
    .truncate = ram_truncate,
    .size = ram_size,
    .publish = ram_publish,
    .release = staged_release,
};

struct vnode_ops ramfs_ops = {
    .read = ram_read,
    .write = ram_write,
    .readdir = ram_readdir,
    .lookup = ram_lookup,
    .create = ram_create,
    .unlink = ram_unlink,
    .truncate = ram_truncate,
    .stage = ram_stage,
    .move = ram_move,
    .size = ram_size,
    .open = ram_open,
    .close = ram_close,
    .release = ram_release,
};

struct vnode *ramfs_new_root(void) { return ramfs_new_node("", VT_DIR); }
