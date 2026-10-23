/* The namespace (fs/ramfs.c, docs/nosfs.md §1, §3, §8): one tree of folders and documents in RAM.
   A folder is read in when it is first used: the entries its volume recorded, over the base item
   at the same place (the initrd's tree, which is never in the namespace itself). A change is
   handed to the volume, which records it; a tree without a volume (/tmp, /dev, or all of them when
   there is none) lives in RAM only. */
#pragma once
#include "fs/vfs.h"

struct content;
struct content_store;
struct ramfs_load;

/* An entry as its volume recorded it. */
struct ramfs_entry {
    const char *name;
    int type; /* VT_FILE, VT_DIR */
    uint32_t mode;
    uint64_t id, size;
    int64_t mtime;
    const char *origin; /* the base item it shows, by its path there ("": none) */
    bool base_content;  /* a document whose content is still its base item's */
    uint64_t pos;       /* where in its folder the volume keeps it (vnode entry_pos) */
};

/* The persistent volume behind the namespace (fs/nos.c). Every hook runs in task context on the
   BSP; only load, content and saved sleep. */
struct ramfs_volume {
    const struct content_store *store;
    /* Read in folder dir: ramfs_found for each entry recorded in it (when it has an ID),
       ramfs_hidden for each base item hidden from dir->origin (when it shows one). */
    int (*load)(struct ramfs_volume *vol, struct vnode *dir, struct ramfs_load *ld);
    /* The committed content of document v (recorded, not read in yet). */
    int (*content)(struct ramfs_volume *vol, struct vnode *v, struct content **out);
    /* v's entry changed: made, renamed, moved, removed (v->unlinked), or its document saved. The
       volume records it with the next commit and gives a new one its ID here; -N_ERR_NO_SPACE
       when there is no room left to record it (a removal always has room). */
    int (*changed)(struct ramfs_volume *vol, struct vnode *v);
    /* The base item (a node of the base tree) is not shown where the base has it any more. */
    int (*hide)(struct ramfs_volume *vol, struct vnode *item);
    /* Commit version c of document v (frozen, on the volume's store): *ticket says what saved
       waits for. When it is on the medium the volume calls ramfs_saved. */
    int (*save)(struct ramfs_volume *vol, struct vnode *v, struct content *c, uint64_t *ticket);
    /* 0: the commit with ticket is on the medium; an error; -N_ERR_NOT_READY while it is on its
       way (wait: until it is known). */
    int (*saved)(struct ramfs_volume *vol, uint64_t ticket, bool wait);
    /* The ticket for v as recorded so far: its entry and its saves (0: nothing was). */
    uint64_t (*recorded)(struct ramfs_volume *vol, struct vnode *v);
    /* Everything recorded so far is committed (waits). */
    int (*sync)(struct ramfs_volume *vol);
    /* Document v (recorded, empty, committed) gets blocks for size bytes: *out is the version
       naming them, frozen, which the commit with *ticket records as committed. */
    int (*reserve)(struct ramfs_volume *vol, struct vnode *v, uint64_t size, struct content **out,
                   uint64_t *ticket);
    /* Where version c's blocks are on the disk, in page order. */
    int (*extents)(struct ramfs_volume *vol, struct content *c,
                   int (*run)(void *ctx, struct blkdev *d, uint64_t lba, uint64_t sectors), void *ctx);
};

/* The namespace's root (vfs_init); folders are read in once ramfs_attach says where from. */
struct vnode *ramfs_namespace_root(void);
/* The namespace is on vol (NULL: RAM only), its root recorded under root_id. */
void ramfs_attach(struct ramfs_volume *vol, uint64_t root_id);
int ramfs_sync(void); /* vfs_sync */
/* A folder of its own (/tmp, /dev): RAM only, mounted on the namespace. */
struct vnode *ramfs_new_root(void);
/* A node outside any folder; device nodes set their ops. */
struct vnode *ramfs_new_node(const char *name, int type);
void ramfs_add_child(struct vnode *dir, struct vnode *child);
/* A mount point at path, made with its missing folders: they are never recorded and nothing but
   mount points can be made in them. */
int ramfs_mount_point(const char *abspath, struct vnode **out);
extern struct vnode_ops ramfs_ops;

/* Called by volumes. */
int ramfs_found(struct ramfs_load *ld, const struct ramfs_entry *e);
int ramfs_hidden(struct ramfs_load *ld, const char *name);
/* The commit of document v's version c ended: error 0, c is v's committed version. */
void ramfs_saved(struct vnode *v, struct content *c, int error);
/* v's committed version is its base item's content (an entry records that, not the content). */
bool ramfs_reads_base(struct vnode *v);

/* The base (fs/initrd.c builds it before the namespace is attached). */
struct vnode *ramfs_base_folder(const char *abspath); /* made with its parents as needed */
/* A document of the base: data read in place (borrow) or copied. -N_ERR_EXISTS: there is one. */
int ramfs_base_document(const char *abspath, uint32_t mode, const uint8_t *data, uint64_t size, bool borrow);
void ramfs_base_remove(const char *abspath); /* before ramfs_attach only */
struct vnode *ramfs_base_find(const char *abspath);
int ramfs_base_path(struct vnode *item, char *out, size_t room);
