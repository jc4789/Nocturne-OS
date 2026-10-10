#pragma once
#include "kernel.h"
#include "sys/sched.h"
#include "sys/sched.h"

enum { VT_FILE = 1, VT_DIR = 2, VT_CHAR = 3, VT_PIPE = 4, VT_WINDOW = 5 };

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
};

struct kstat {
    uint32_t type;
    uint32_t mode;
    uint64_t size;
    int64_t mtime;
};

struct vnode;
struct file;

struct vnode_ops {
    int64_t (*read)(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n);
    int64_t (*write)(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n);
    int (*readdir)(struct vnode *v, uint64_t idx, struct dirent *out);
    struct vnode *(*lookup)(struct vnode *dir, const char *name);
    int (*create)(struct vnode *dir, const char *name, int type, struct vnode **out);
    int (*unlink)(struct vnode *dir, const char *name);
    int (*truncate)(struct vnode *v, uint64_t size);
    bool (*can_read)(struct vnode *v, struct file *f);
    bool (*can_write)(struct vnode *v, struct file *f);
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
    bool tty; /* a pipe that a terminal emulator reads/writes: reported as a character device */
    struct vnode_ops *ops;
    void *priv;
    struct vnode *mounted;  /* fs root mounted on top of this dir */
    struct vnode *parent;
    /* ramfs storage */
    struct vnode *children, *sibling;
    uint8_t *data;
    uint64_t cap;
    struct file *lease_owner;
    struct wait_queue lease_wait;
};

struct file {
    struct vnode *vn;
    uint64_t off;
    int flags;
    int refs;
    void *priv;
    bool io_busy;
    struct wait_queue io_wait; /* regular-file shared offset; device directions stay independent */
};

extern struct vnode *vfs_root;

void vfs_init(void);
int vfs_normalize(const char *cwd, const char *path, char *out); /* absolute, no . or .. */
struct vnode *vfs_lookup(const char *abspath);
int vfs_open(const char *abspath, int flags, struct file **out);
struct file *vfs_open_vnode(struct vnode *v, int flags);
int64_t vfs_read(struct file *f, void *buf, size_t n);
int64_t vfs_write(struct file *f, const void *buf, size_t n);
int64_t vfs_seek(struct file *f, int64_t off, int whence);
void vfs_close(struct file *f);
int vfs_lease(struct file *f, bool acquire);
struct file *vfs_dup(struct file *f);
int vfs_readdir(struct file *f, uint64_t idx, struct dirent *out);
int vfs_stat(const char *abspath, struct kstat *st);
int vfs_mkdir(const char *abspath);
int vfs_unlink(const char *abspath);
int vfs_mount(const char *abspath, struct vnode *root);
bool vfs_can_read(struct file *f);
bool vfs_can_write(struct file *f);
int vfs_read_whole(const char *abspath, uint8_t **data, uint64_t *size);
int vfs_write_whole(const char *abspath, const void *data, uint64_t size);
void vnode_ref(struct vnode *v);
void vnode_unref(struct vnode *v);

/* ramfs */
struct vnode *ramfs_new_root(void);
struct vnode *ramfs_new_node(const char *name, int type);
void ramfs_add_child(struct vnode *dir, struct vnode *child);
extern struct vnode_ops ramfs_ops;

/* devfs */
void devfs_init(void);
void console_input_char(char c);
void console_input_str(const char *s);

/* pipes */
int pipe_create(struct file **rd, struct file **wr);

/* initrd */
void initrd_load(void *data, uint64_t size);
