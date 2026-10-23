/* ramfs: the namespace (fs/ramfs.h, docs/nosfs.md §3, §8). */
#include "fs/ramfs.h"
#include "fs/content.h"
#include "mm/heap.h"
#include "arch/cpu.h"
#include "dev/timer.h"

static struct vnode *base_root;   /* the initrd's tree */
static bool attached;             /* folders may be read in */
static struct wait_queue loading; /* folders being read in */

struct ramfs_load {
    struct vnode *dir;
    char **hidden; /* base items the volume hides from the folder */
    size_t count, room;
};

static const struct content_store *store_of(struct vnode *v) { return v->vol ? v->vol->store : NULL; }

static struct vnode *node_new(const char *name, int type, struct ramfs_volume *vol) {
    struct vnode *v = kzalloc(sizeof *v);
    if (!v) return NULL;
    strlcpy(v->name, name, sizeof v->name);
    v->type = type;
    v->ops = &ramfs_ops;
    v->mode = type == VT_DIR ? 0755 : 0644;
    v->mtime = time_now();
    v->refs = 1; /* the folder's reference */
    v->vol = vol;
    return v;
}

/* A document whose committed version c (consumed) is also its newest. */
static void set_content(struct vnode *v, struct content *c) {
    v->content = c;
    v->head = content_ref(c);
    v->size = content_size(c);
}

static struct vnode *child(struct vnode *dir, const char *name) {
    for (struct vnode *c = dir->children; c; c = c->sibling)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

void ramfs_add_child(struct vnode *dir, struct vnode *child) {
    child->parent = dir;
    /* keep entries sorted by name */
    struct vnode **pp = &dir->children;
    while (*pp && strcmp((*pp)->name, child->name) < 0) pp = &(*pp)->sibling;
    child->sibling = *pp;
    *pp = child;
}

static void unhook(struct vnode *dir, struct vnode *v) {
    struct vnode **pp = &dir->children;
    while (*pp && *pp != v) pp = &(*pp)->sibling;
    if (*pp) *pp = v->sibling;
    v->sibling = NULL;
}

struct vnode *ramfs_new_node(const char *name, int type) { return node_new(name, type, NULL); }

struct vnode *ramfs_new_root(void) {
    struct vnode *v = node_new("", VT_DIR, NULL);
    if (v) v->loaded = 2;
    return v;
}

/* ---------------------------------------------------------------- the base */

/* The base folder holding path's last component (named in leaf; "" for the root), made with its
   parents as needed when make. */
static struct vnode *base_dir_of(const char *path, char *leaf, bool make) {
    struct vnode *dir = base_root;
    for (const char *p = path;;) {
        while (*p == '/') p++;
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t len = (size_t)(end - p);
        if (len >= NAME_MAX_LEN) return NULL;
        const char *next = end;
        while (*next == '/') next++;
        memcpy(leaf, p, len);
        leaf[len] = 0;
        if (!*next) return dir;
        struct vnode *c = child(dir, leaf);
        if (!c && make && (c = node_new(leaf, VT_DIR, NULL))) ramfs_add_child(dir, c);
        if (!c || c->type != VT_DIR) return NULL;
        dir = c;
        p = next;
    }
}

struct vnode *ramfs_base_find(const char *path) {
    char leaf[NAME_MAX_LEN];
    struct vnode *dir = base_dir_of(path, leaf, false);
    return !dir ? NULL : leaf[0] ? child(dir, leaf) : dir;
}

struct vnode *ramfs_base_folder(const char *path) {
    char leaf[NAME_MAX_LEN];
    struct vnode *dir = base_dir_of(path, leaf, true);
    if (!dir || !leaf[0]) return dir;
    struct vnode *c = child(dir, leaf);
    if (!c && (c = node_new(leaf, VT_DIR, NULL))) ramfs_add_child(dir, c);
    return c && c->type == VT_DIR ? c : NULL;
}

int ramfs_base_document(const char *path, uint32_t mode, const uint8_t *data, uint64_t size, bool borrow) {
    char leaf[NAME_MAX_LEN];
    struct vnode *dir = base_dir_of(path, leaf, true);
    if (!dir || !leaf[0]) return -N_ERR_NOT_FOUND;
    if (child(dir, leaf)) return -N_ERR_EXISTS;
    struct vnode *v = node_new(leaf, VT_FILE, NULL);
    struct content *c = content_version(NULL, NULL);
    int64_t r = v && c ? 0 : -N_ERR_NO_MEMORY;
    if (!r && size) {
        if (borrow) content_borrow(c, data, size);
        else if ((r = content_write(c, data, 0, size)) >= 0) r = (uint64_t)r == size ? 0 : -N_ERR_NO_MEMORY;
    }
    if (r) {
        kfree(v);
        content_unref(c);
        return (int)r;
    }
    content_saved(c);
    v->mode = mode;
    set_content(v, c);
    ramfs_add_child(dir, v);
    return 0;
}

static void base_free(struct vnode *v) {
    while (v->children) {
        struct vnode *c = v->children;
        v->children = c->sibling;
        base_free(c);
    }
    content_unref(v->head);
    content_unref(v->content);
    kfree(v);
}

void ramfs_base_remove(const char *path) {
    struct vnode *v = ramfs_base_find(path);
    if (!v || v == base_root || attached) return;
    unhook(v->parent, v);
    base_free(v);
}

int ramfs_base_path(struct vnode *item, char *out, size_t room) {
    if (room < 2) return -N_ERR_NAME_TOO_LONG;
    size_t at = room - 1;
    out[at] = 0;
    for (struct vnode *v = item; v && v != base_root; v = v->parent) {
        size_t len = strlen(v->name);
        if (at < len + 1) return -N_ERR_NAME_TOO_LONG;
        at -= len;
        memcpy(out + at, v->name, len);
        out[--at] = '/';
    }
    if (at == room - 1) out[--at] = '/';
    memmove(out, out + at, room - at);
    return 0;
}

/* ---------------------------------------------------------------- the namespace's folders */

struct vnode *ramfs_namespace_root(void) {
    base_root = node_new("", VT_DIR, NULL);
    struct vnode *root = node_new("", VT_DIR, NULL);
    if (!base_root || !root) panic("ramfs: no memory for the root");
    base_root->loaded = 2;
    return root;
}

void ramfs_attach(struct ramfs_volume *vol, uint64_t root_id) {
    vfs_root->vol = vol;
    vfs_root->id = vol ? root_id : 0;
    vfs_root->origin = base_root;
    attached = true;
}

int ramfs_sync(void) { return vfs_root->vol ? vfs_root->vol->sync(vfs_root->vol) : 0; }

int ramfs_found(struct ramfs_load *ld, const struct ramfs_entry *e) {
    struct vnode *dir = ld->dir;
    if (child(dir, e->name)) return 0; /* a mount point wins */
    if (e->type != VT_FILE && e->type != VT_DIR) return -N_ERR_INVALID;
    struct vnode *v = node_new(e->name, e->type, dir->vol);
    if (!v) return -N_ERR_NO_MEMORY;
    v->id = e->id;
    v->mode = e->mode;
    v->mtime = e->mtime;
    v->size = e->size;
    v->entry_dir = dir->id;
    v->entry_pos = e->pos;
    v->origin = e->origin[0] ? ramfs_base_find(e->origin) : NULL;
    if (v->origin && v->origin->type != e->type) v->origin = NULL;
    if (e->type == VT_FILE && e->base_content) {
        /* a later version of the OS without the item: the document is empty */
        struct content *c = v->origin ? content_ref(v->origin->content) : content_new(NULL);
        if (!c) {
            kfree(v);
            return -N_ERR_NO_MEMORY;
        }
        set_content(v, c);
    }
    ramfs_add_child(dir, v);
    return 0;
}

int ramfs_hidden(struct ramfs_load *ld, const char *name) {
    char *copy = strdup(name);
    char **at = copy ? kappend(&ld->hidden, &ld->count, &ld->room, sizeof *ld->hidden) : NULL;
    if (!at) {
        kfree(copy);
        return -N_ERR_NO_MEMORY;
    }
    *at = copy;
    return 0;
}

static bool is_hidden(struct ramfs_load *ld, const char *name) {
    for (size_t i = 0; i < ld->count; i++)
        if (!strcmp(ld->hidden[i], name)) return true;
    return false;
}

/* The base items of the folder's origin that nothing recorded hides or replaces. */
static int show_base(struct ramfs_load *ld) {
    struct vnode *dir = ld->dir;
    for (struct vnode *b = dir->origin->children; b; b = b->sibling) {
        if (is_hidden(ld, b->name)) continue;
        struct vnode *v = child(dir, b->name);
        if (v) {
            /* a folder of the user's where a later version of the OS has one too: it shows that
               one's items as well */
            if (v->type == VT_DIR && b->type == VT_DIR && !v->origin && !v->transient) v->origin = b;
            continue;
        }
        if (!(v = node_new(b->name, b->type, dir->vol))) return -N_ERR_NO_MEMORY;
        v->origin = b;
        v->mode = b->mode;
        v->mtime = b->mtime;
        if (b->type == VT_FILE) set_content(v, content_ref(b->content));
        ramfs_add_child(dir, v);
    }
    return 0;
}

/* Read the folder in, once: what the volume recorded, then the base. A failed read leaves it to
   be read again (what was found stays and is not found twice). */
static int dir_load(struct vnode *dir) {
    if (dir->loaded == 2 || !attached) return 0;
    uint64_t fl = irq_save();
    while (dir->loaded == 1) wq_wait(&loading);
    bool mine = !dir->loaded;
    if (mine) dir->loaded = 1;
    irq_restore(fl);
    if (!mine) return 0;
    struct ramfs_load ld = {.dir = dir};
    vnode_ref(dir);
    /* a base folder the volume never recorded can still have items of it hidden */
    int r = dir->vol && (dir->id || dir->origin) ? dir->vol->load(dir->vol, dir, &ld) : 0;
    if (!r && dir->origin) r = show_base(&ld);
    for (size_t i = 0; i < ld.count; i++) kfree(ld.hidden[i]);
    kfree(ld.hidden);
    fl = irq_save();
    dir->loaded = r ? 0 : 2;
    wq_wake_all(&loading);
    irq_restore(fl);
    vnode_unref(dir);
    return r;
}

int ramfs_mount_point(const char *path, struct vnode **out) {
    struct vnode *v = vfs_lookup(path);
    if (v) {
        *out = v;
        return 0;
    }
    char parent[PATH_MAX_LEN];
    if (strlcpy(parent, path, sizeof parent) >= sizeof parent) return -N_ERR_NAME_TOO_LONG;
    char *slash = strrchr(parent, '/');
    if (!slash || !slash[1]) return -N_ERR_INVALID;
    const char *leaf = path + (slash - parent) + 1;
    if (strlen(leaf) >= NAME_MAX_LEN) return -N_ERR_NAME_TOO_LONG;
    slash[slash == parent] = 0;
    struct vnode *dir;
    int r = ramfs_mount_point(parent, &dir);
    if (r) return r;
    if (dir->ops != &ramfs_ops || dir->type != VT_DIR) return -N_ERR_UNSUPPORTED;
    if (!(v = node_new(leaf, VT_DIR, NULL))) return -N_ERR_NO_MEMORY;
    v->transient = true;
    v->loaded = 2;
    ramfs_add_child(dir, v);
    *out = v;
    return 0;
}

static int loaded(struct vnode *v);

/* The volume records v's entry; the base folders it is in get their IDs first. */
static int record(struct vnode *v) {
    if (!v->vol || v->transient) return 0;
    if (v->parent && !v->parent->id) {
        int r = record(v->parent);
        if (r) return r;
    }
    return v->vol->changed(v->vol, v);
}

static int ram_readdir(struct vnode *v, uint64_t idx, struct dirent *out) {
    int r = dir_load(v);
    if (r) return r;
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
    return dir_load(dir) ? NULL : child(dir, name);
}

static int ram_create(struct vnode *dir, const char *name, int type, struct vnode **out) {
    int r = dir_load(dir);
    if (r) return r;
    if (dir->transient) return -N_ERR_READ_ONLY;
    if (strlen(name) >= NAME_MAX_LEN) return -N_ERR_NAME_TOO_LONG;
    if (child(dir, name)) return -N_ERR_EXISTS;
    if (type != VT_FILE && type != VT_DIR) return -N_ERR_INVALID;
    struct vnode *v = node_new(name, type, dir->vol);
    struct content *c = v && type == VT_FILE ? content_new(store_of(dir)) : NULL;
    if (!v || (type == VT_FILE && !c)) {
        kfree(v);
        return -N_ERR_NO_MEMORY;
    }
    if (c) set_content(v, c);
    else v->loaded = 2;
    ramfs_add_child(dir, v);
    if ((r = record(v))) {
        unhook(dir, v);
        v->unlinked = true;
        vnode_unref(v);
        return r;
    }
    *out = v;
    return 0;
}

static int ram_unlink(struct vnode *dir, const char *name) {
    int r = dir_load(dir);
    if (r) return r;
    struct vnode *v = child(dir, name);
    if (!v) return -N_ERR_NOT_FOUND;
    if (v->ops != &ramfs_ops || v->transient || v->mounted) return -N_ERR_DENIED; /* devices, mount points */
    if (v->type == VT_DIR && ((r = dir_load(v)) || v->children)) return r ? r : -N_ERR_NOT_EMPTY;
    /* A recorded document's blocks are its committed version's to let go of: read it in, so they
       go when the last reader of the document does. */
    if (v->type == VT_FILE && v->vol && v->id && (r = loaded(v))) return r;
    if (child(dir, name) != v) return -N_ERR_NOT_FOUND; /* gone while the folder was read in */
    /* what the base has there stays hidden; what the volume recorded goes */
    v->unlinked = true;
    r = v->vol && v->origin ? v->vol->hide(v->vol, v->origin) : 0;
    if (!r && v->vol && v->id) r = v->vol->changed(v->vol, v);
    if (r) {
        v->unlinked = false;
        return r;
    }
    unhook(dir, v);
    vnode_unref(v);
    return 0;
}

static int ram_move(struct vnode *from_dir, const char *from, struct vnode *to_dir, const char *to) {
    if (strlen(to) >= NAME_MAX_LEN) return -N_ERR_NAME_TOO_LONG;
    int r = dir_load(from_dir);
    if (!r) r = dir_load(to_dir);
    if (r) return r;
    if (to_dir->transient) return -N_ERR_READ_ONLY;
    if (child(to_dir, to)) return -N_ERR_EXISTS;
    struct vnode *v = child(from_dir, from);
    if (!v) return -N_ERR_NOT_FOUND;
    if (v->ops != &ramfs_ops || v->mounted || v->transient) return -N_ERR_DENIED; /* devices, mount points */
    if (from_dir->vol != to_dir->vol) return -N_ERR_UNSUPPORTED; /* another volume: copied */
    char was[NAME_MAX_LEN];
    strlcpy(was, v->name, sizeof was);
    unhook(from_dir, v);
    strlcpy(v->name, to, sizeof v->name);
    ramfs_add_child(to_dir, v);
    /* Recorded where it is now, then hidden where the base has it: a failure in between leaves
       the record, which shows it where it is in RAM once the commit takes the place back. */
    r = record(v);
    if (!r && v->vol && v->origin) r = v->vol->hide(v->vol, v->origin);
    if (r) {
        unhook(to_dir, v);
        strlcpy(v->name, was, sizeof v->name);
        ramfs_add_child(from_dir, v);
    }
    return r;
}

/* ---------------------------------------------------------------- documents */

static void wait_saving(struct vnode *v) {
    uint64_t fl = irq_save();
    while (v->saving) wq_wait(&v->save_wait);
    irq_restore(fl);
}

/* The newest version a save took is there (a recorded document is read in first). */
static int loaded(struct vnode *v) {
    if (v->head) return 0;
    struct content *c;
    int r = v->vol->content(v->vol, v, &c);
    if (r) return r;
    if (v->head) { /* read in meanwhile */
        content_unref(c);
        return 0;
    }
    v->content = c;
    v->head = content_ref(c);
    return 0;
}

static struct content *newest(struct vnode *v) { return v->work ? v->work : v->head; }

/* The working version (made on the newest one when nothing wrote since the last save), referenced
   for the caller: a publish may replace it while the caller sleeps in it. */
static int working(struct vnode *v, struct content **out) {
    int r = loaded(v);
    if (r) return r;
    wait_saving(v);
    if (!v->work && !(v->work = content_version(v->head, store_of(v)))) return -N_ERR_NO_MEMORY;
    *out = content_ref(v->work);
    return 0;
}

/* What handle f reads, referenced: a read-only one the version it opened, the others the newest. */
static int view(struct vnode *v, struct file *f, struct content **out) {
    if (!f || !f->priv) {
        int r = loaded(v);
        if (r) return r;
        if (f && (f->flags & O_ACCMODE) == O_RDONLY) f->priv = content_ref(v->head);
    }
    *out = content_ref(f && f->priv ? f->priv : newest(v));
    return 0;
}

static int64_t ram_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    struct content *c;
    int r = view(v, f, &c);
    if (r) return r;
    int64_t got = content_read(c, buf, off, n);
    content_unref(c);
    return got;
}

static int64_t ram_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    struct content *c;
    int r = working(v, &c);
    if (r) return r;
    int64_t done = content_write(c, buf, off, n);
    v->size = content_size(newest(v));
    content_unref(c);
    return done;
}

static int ram_truncate(struct vnode *v, uint64_t size) {
    struct content *c;
    int r = working(v, &c);
    if (r) return r;
    r = content_truncate(c, size);
    v->size = content_size(newest(v));
    content_unref(c);
    return r;
}

static uint64_t ram_size(struct vnode *v, struct file *f) { return f && f->priv ? content_size(f->priv) : v->size; }

/* A read-only handle keeps the newest version a save took when it opened (or, when that cannot be
   read in now, the one there at its first read). */
static void ram_open(struct vnode *v, struct file *f) {
    if (v->type == VT_FILE && (f->flags & O_ACCMODE) == O_RDONLY && !loaded(v)) f->priv = content_ref(v->head);
}

static void ram_close(struct vnode *v, struct file *f) {
    if (v->type == VT_FILE && f->priv) content_unref(f->priv);
    f->priv = NULL;
}

/* Does version c read its document's base item (a document of the base changed for the first
   time)? Such a version is saved whole: the volume never names the archive's bytes. */
static bool on_base(struct vnode *v, struct content *c) {
    if (!v->origin || v->origin->type != VT_FILE) return false;
    for (c = content_base(c); c; c = content_base(c))
        if (c == v->origin->content) return true;
    return false;
}

bool ramfs_reads_base(struct vnode *v) {
    return v->origin && v->origin->type == VT_FILE && v->content && v->content == v->origin->content;
}

void ramfs_saved(struct vnode *v, struct content *c, int error) {
    if (!error) {
        content_saved(c);
        struct content *was = v->content;
        v->content = content_ref(c);
        content_unref(was); /* merges with c once nothing else reads it */
    }
    vfs_poll_notify(v);
}

/* The working version becomes the newest a save took (frozen), and the volume commits it; edits
   meanwhile go to a new working version on top of it. A save the volume refuses changes nothing.
   Without edits (or none to take) the ticket is the one for the document as recorded so far. */
static int ram_save(struct vnode *v, bool edits, uint64_t *ticket) {
    *ticket = 0;
    if (v->type != VT_FILE) return 0;
    wait_saving(v);
    struct content *f = v->work, *was = v->head;
    if (!edits || !f) {
        if (v->vol) *ticket = v->vol->recorded(v->vol, v);
        return 0;
    }
    v->saving = true;
    content_freeze(f); /* the writes in progress land in it */
    v->work = NULL;
    v->head = f;
    int r = 0;
    if (!v->vol || v->unlinked) {
        ramfs_saved(v, f, 0);
    } else {
        if (on_base(v, f)) r = content_own_all(f);
        if (!r) r = record(v);
        if (!r) r = v->vol->save(v->vol, v, f, ticket);
        if (r) {
            content_thaw(f);
            v->work = f;
            v->head = was;
            was = NULL;
        }
    }
    content_unref(was);
    uint64_t fl = irq_save();
    v->saving = false;
    wq_wake_all(&v->save_wait);
    irq_restore(fl);
    return r;
}

static int ram_saved(struct vnode *v, uint64_t ticket, bool wait) {
    return v->vol ? v->vol->saved(v->vol, ticket, wait) : 0;
}

/* The version the volume makes is the newest a save took, like a save's; nothing wrote the
   document, so nothing is lost. */
static int ram_reserve(struct vnode *v, uint64_t size) {
    if (v->type != VT_FILE || !v->vol || v->unlinked) return -N_ERR_UNSUPPORTED;
    int r = loaded(v);
    if (r) return r;
    wait_saving(v);
    if (v->work || content_size(v->head)) return -N_ERR_BUSY;
    if ((r = record(v))) return r;
    v->saving = true;
    struct content *c;
    uint64_t ticket;
    if (!(r = v->vol->reserve(v->vol, v, size, &c, &ticket))) {
        content_unref(v->head);
        v->head = c;
        v->size = size;
    }
    uint64_t fl = irq_save();
    v->saving = false;
    wq_wake_all(&v->save_wait);
    irq_restore(fl);
    return r ? r : v->vol->saved(v->vol, ticket, true);
}

static int ram_extents(struct vnode *v, int (*run)(void *ctx, struct blkdev *d, uint64_t lba, uint64_t sectors),
                       void *ctx) {
    if (v->type != VT_FILE || !v->vol) return -N_ERR_UNSUPPORTED;
    int r = loaded(v);
    if (r) return r;
    if (v->work || v->head != v->content) return -N_ERR_BUSY; /* changed since its blocks were committed */
    return v->vol->extents(v->vol, v->content, run, ctx);
}

static void ram_release(struct vnode *v) {
    content_unref(v->work);
    content_unref(v->head);
    content_unref(v->content);
    kfree(v);
}

/* ---------------------------------------------------------------- unpublished versions

   N_DOCUMENT_REPLACE writes an unnamed node: its content is a working version with no base, its
   parent and name say where it goes, priv is the document it replaces (the one there when it was
   made, wherever that document went since; or, published, the one it made). Publishing makes it
   that document's working version and saves it. */

static struct vnode_ops staged_ops;

static int ram_stage(struct vnode *dir, const char *name, struct vnode **out) {
    if (strlen(name) >= NAME_MAX_LEN) return -N_ERR_NAME_TOO_LONG;
    int r = dir_load(dir);
    if (r) return r;
    if (dir->transient) return -N_ERR_READ_ONLY;
    struct vnode *t = child(dir, name);
    if (t && t->type == VT_DIR) return -N_ERR_IS_FOLDER;
    if (t && (t->type != VT_FILE || t->ops != &ramfs_ops)) return -N_ERR_WRONG_KIND;
    struct vnode *s = node_new(name, VT_FILE, dir->vol);
    struct content *c = s ? content_version(NULL, store_of(dir)) : NULL;
    if (!c) {
        kfree(s);
        return -N_ERR_NO_MEMORY;
    }
    s->ops = &staged_ops;
    s->refs = 0; /* its handles' only */
    s->unlinked = true;
    s->staged = true;
    s->content = c;
    s->parent = dir;
    vnode_ref(dir);
    if (t) vnode_ref(t);
    s->priv = t;
    *out = s;
    return 0;
}

static int64_t staged_read(struct vnode *s, struct file *f, void *buf, uint64_t off, size_t n) {
    struct content *c = content_ref(s->content);
    int64_t got = content_read(c, buf, off, n);
    content_unref(c);
    return got;
}

static int64_t staged_write(struct vnode *s, struct file *f, const void *buf, uint64_t off, size_t n) {
    if (!s->staged) return -N_ERR_DENIED; /* published: the document's now */
    int64_t done = content_write(s->content, buf, off, n);
    s->size = content_size(s->content);
    return done;
}

static int staged_truncate(struct vnode *s, uint64_t size) {
    if (!s->staged) return -N_ERR_DENIED;
    int r = content_truncate(s->content, size);
    s->size = content_size(s->content);
    return r;
}

static int staged_save(struct vnode *s, bool edits, uint64_t *ticket) {
    struct vnode *t = s->priv;
    if (!s->staged) return ram_save(t, edits, ticket); /* published before; its save failed */
    if (t ? t->unlinked : s->parent->unlinked) return -N_ERR_NOT_FOUND;
    if (!t) {
        int r = ram_create(s->parent, s->name, VT_FILE, &t);
        if (r) return r;
        vnode_ref(t);
        s->priv = t;
        vfs_poll_notify(s); /* its watchers watch the document from now on (staged_poll_register) */
    }
    int r = loaded(t); /* the version replaced lets go of its blocks */
    if (r) return r;
    wait_saving(t);
    content_unref(t->work); /* what was being edited is replaced */
    t->work = content_ref(s->content);
    t->size = content_size(s->content);
    t->mtime = time_now();
    s->staged = false;
    return ram_save(t, true, ticket);
}

static int staged_saved(struct vnode *s, uint64_t ticket, bool wait) { return ram_saved(s, ticket, wait); }

/* the document's save ends are what its watchers wait for */
static void staged_poll_register(struct vnode *s, struct file *f, struct poll_wait *wait) {
    if (s->priv) vfs_poll_register(wait, s->priv);
}

static void staged_release(struct vnode *s) {
    content_unref(s->content);
    vnode_unref(s->priv);
    vnode_unref(s->parent);
    kfree(s);
}

static struct vnode_ops staged_ops = {
    .read = staged_read,
    .write = staged_write,
    .truncate = staged_truncate,
    .save = staged_save,
    .saved = staged_saved,
    .poll_register = staged_poll_register,
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
    .save = ram_save,
    .saved = ram_saved,
    .size = ram_size,
    .open = ram_open,
    .close = ram_close,
    .release = ram_release,
    .reserve = ram_reserve,
    .extents = ram_extents,
};
