#pragma once
#include "kernel.h"
#include "sys/sched.h"

enum { VT_FILE = 1, VT_DIR = 2, VT_CHAR = 3, VT_WINDOW = 5, VT_CHANNEL = 6, VT_SHARED = 7, VT_ADDRESS_WATCH = 8,
       VT_APP = 9, VT_EVENTS = 10, VT_PCI = 11 };

#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_RDWR      0x2
#define O_ACCMODE   0x3
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_DIRECTORY 0x10000

#define NAME_MAX_LEN 256 /* FAT32 long names go up to 255 */
#define PATH_MAX_LEN 512

struct dirent {
    char name[NAME_MAX_LEN];
    uint32_t type;
    uint32_t reserved;
    uint64_t size;
    int64_t mtime;
};

struct vnode;
struct file;
struct poll_entry;
struct poll_wait {
    struct wait_queue wq;
    uint64_t generation;
    struct poll_entry *entries;
    int error;
};

struct vnode_ops {
    int64_t (*read)(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n);
    int64_t (*write)(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n);
    /* User variants validate the actual transfer after readiness/length is
       known. Kernel callers continue to use read/write without user checks. */
    int64_t (*read_user)(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n);
    int64_t (*write_user)(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n);
    int (*readdir)(struct vnode *v, uint64_t idx, struct dirent *out);
    struct vnode *(*lookup)(struct vnode *dir, const char *name);
    int (*create)(struct vnode *dir, const char *name, int type, struct vnode **out);
    int (*unlink)(struct vnode *dir, const char *name);
    int (*truncate)(struct vnode *v, uint64_t size);
    /* Swap in new content whole: readers see the old or the new bytes, never
       a mix. Consumes data (kmalloc'd) whether or not it succeeds. */
    int (*replace)(struct vnode *v, uint8_t *data, uint64_t size);
    /* Rename/move an entry within one filesystem; the target name must be free. */
    int (*move)(struct vnode *from_dir, const char *from, struct vnode *to_dir, const char *to);
    /* What was written to v is on the medium when this returns (a written document's handle is
       closed). NULL: nothing to wait for, as in memory. */
    int (*sync)(struct vnode *v);
    bool (*can_read)(struct vnode *v, struct file *f);
    bool (*can_write)(struct vnode *v, struct file *f);
    bool (*hangup)(struct vnode *v, struct file *f);
    void (*poll_register)(struct vnode *v, struct file *f, struct poll_wait *wait);
    void (*open)(struct vnode *v, struct file *f);
    void (*close)(struct vnode *v, struct file *f);
    void (*release)(struct vnode *v); /* last reference dropped after unlink */
};

struct vnode {
    char name[NAME_MAX_LEN];
    int type;
    uint32_t mode;
    uint64_t size;
    int64_t mtime;
    int refs;
    bool unlinked;
    struct vnode_ops *ops;
    void *priv;
    struct vnode *mounted;  /* fs root mounted on top of this dir */
    struct vnode *parent;
    /* ramfs storage. cap 0 with data set: the bytes are the initrd archive's, read in place; the
       first change copies them out (fs/initrd.c gets the pages back once nothing reads them). */
    struct vnode *children, *sibling;
    uint8_t *data;
    uint64_t cap;
    struct file *lease_owner;
    struct wait_queue lease_wait;
    struct poll_entry *pollers;
};

struct file {
    struct vnode *vn;
    uint64_t off;
    int flags;
    int refs;
    void *priv;
    bool io_busy;
    bool written;              /* a document changed through it since its last sync */
    struct wait_queue io_wait; /* regular-file shared offset; device directions stay independent */
    struct event_watch *watchers; /* event-queue watches on this handle, dropped at the last close */
};

extern struct vnode *vfs_root;

void vfs_init(void);
int vfs_normalize(const char *cwd, const char *path, char *out); /* absolute, no . or .. */
struct vnode *vfs_lookup(const char *abspath);
int vfs_open(const char *abspath, int flags, struct file **out);
struct file *vfs_open_vnode(struct vnode *v, int flags);
int64_t vfs_read(struct file *f, void *buf, size_t n);
int64_t vfs_write(struct file *f, const void *buf, size_t n);
int64_t vfs_read_user(struct file *f, void *buf, size_t n);
int64_t vfs_write_user(struct file *f, const void *buf, size_t n);
int vfs_truncate(struct file *f, uint64_t size);
void vfs_close(struct file *f);
int vfs_lease(struct file *f, bool acquire);
int vfs_try_lease(struct file *f);
struct file *vfs_dup(struct file *f);
int vfs_mkdir(const char *abspath);
int vfs_unlink(const char *abspath);
int vfs_mount(const char *abspath, struct vnode *root);
bool vfs_can_read(struct file *f);
bool vfs_can_write(struct file *f);
/* Registration and notification are BSP-owned. Each registration pins only
   the vnode, not an open file: a sibling close must still produce EOF/HUP. */
void vfs_poll_register(struct poll_wait *wait, struct vnode *v);
void vfs_poll_file(struct poll_wait *wait, struct file *f);
void vfs_poll_notify(struct vnode *v);
void vfs_poll_cleanup(struct poll_wait *wait);
int vfs_read_whole(const char *abspath, uint8_t **data, uint64_t *size);
int vfs_write_whole(const char *abspath, const void *data, uint64_t size);

/* Native documents and folders (docs/nocturne-native-abi.md §3). Paths are
   absolute and already normalized; there is no working directory. */
int vfs_document_open(const char *abspath, int access, struct file **out);
int64_t vfs_document_read_user(struct file *f, uint64_t off, void *buf, size_t n);
int64_t vfs_document_write_user(struct file *f, uint64_t off, const void *buf, size_t n);
int vfs_document_commit(struct file *f); /* REPLACE handles: swap the staged content in */
int vfs_replace(const char *abspath, uint8_t *data, uint64_t size); /* consumes data */
int vfs_append_user(const char *abspath, const void *udata, uint64_t size);
int vfs_entry_info(const char *abspath, struct dirent *out);
int vfs_folder_list(const char *abspath, uint64_t start, struct dirent *out, size_t max);
int vfs_move(const char *from, const char *to);
int vfs_kind(struct vnode *v);
void vnode_ref(struct vnode *v);
void vnode_unref(struct vnode *v);

/* ramfs */
struct vnode *ramfs_new_root(void);
struct vnode *ramfs_new_node(const char *name, int type);
void ramfs_add_child(struct vnode *dir, struct vnode *child);
extern struct vnode_ops ramfs_ops;
struct vnode *ramfs_replace_new(const char *abspath); /* staging for N_DOCUMENT_REPLACE */
bool ramfs_is_replace(struct vnode *v);
int ramfs_replace_commit(struct vnode *v);
void ramfs_borrow(struct vnode *v, const uint8_t *data, uint64_t size); /* initrd bytes, in place */

/* event queues (sys/events.c) */
struct event_watch;
void events_file_closed(struct file *f); /* BSP IRQ guard held */

/* devfs */
void devfs_init(void);
void console_input_char(char c);
void console_input_str(const char *s);

/* pipes */
int channel_listen(const char *key,size_t length,struct file **listener);
int channel_connect(const char *key,size_t length,struct file **client);
int channel_accept(struct file *listener,struct file **server);
/* Native message channels: each send is one message, received whole. */
int message_channel_pair(struct file **a, struct file **b);
bool message_channel_is(struct file *f);
int64_t message_channel_send(struct file *f, const void *udata, size_t n, struct file *attach);
/* A message from the kernel's own buffer that never waits: a full queue is
   N_ERR_NOT_READY and the message is dropped. sender_app: the app it speaks for (a crash
   report names the app that crashed, not whichever task happens to be running). */
int64_t message_channel_post(struct file *f, const void *data, size_t n, uint64_t sender_app);
int64_t message_channel_receive(struct file *f, void *ubuf, size_t cap, bool wait, bool peek, bool discard,
                                struct file **attached, uint64_t *sender_app);

/* FAT (fs/fat.c) */
struct blkdev;
void fat_mount_data(void);
bool fat_is(struct vnode *v);
int fat_reserve(struct vnode *v, uint64_t size); /* empty file -> size bytes, nothing written */
int fat_extents(struct vnode *v, int (*run)(void *ctx, struct blkdev *d, uint64_t lba, uint64_t sectors),
                void *ctx);

/* initrd: the archive stays where the bootloader put it and its files are read from it */
void initrd_load(void *data, uint64_t size);
void initrd_unborrow(const uint8_t *data, uint64_t size); /* nothing reads these bytes any more */
/* Hibernation: the archive's pages that still hold what the bootloader loaded (the next boot
   loads them again, so an image need not carry them). */
bool initrd_range(uint64_t *phys, uint64_t *pages);
bool initrd_page_pristine(uint64_t pfn);
