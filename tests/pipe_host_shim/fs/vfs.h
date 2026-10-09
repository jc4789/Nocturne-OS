#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define O_RDONLY 0
#define O_WRONLY 1
#define O_ACCMODE 3
#define O_NONBLOCK 0x800
#define VT_PIPE 4
struct wait_queue { unsigned wakes; };
struct task { bool killed; };
extern struct task *current_task;
void wq_wait(struct wait_queue *);
void wq_wake_all(struct wait_queue *);
void poll_notify(void);
struct vnode;
struct file;
struct vnode_ops {
    int64_t (*read)(struct vnode *,struct file *,void *,uint64_t,size_t);
    int64_t (*write)(struct vnode *,struct file *,const void *,uint64_t,size_t);
    bool (*can_read)(struct vnode *,struct file *);
    bool (*can_write)(struct vnode *,struct file *);
    void (*close)(struct vnode *,struct file *);
    void (*release)(struct vnode *);
};
struct vnode { char name[256]; int type; bool unlinked; void *priv; struct vnode_ops *ops; };
struct file { struct vnode *vn; int flags; };
struct file *vfs_open_vnode(struct vnode *,int);
size_t strlcpy(char *,const char *,size_t);
