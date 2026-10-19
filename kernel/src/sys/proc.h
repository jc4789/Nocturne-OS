#pragma once
#include "sys/sched.h"
struct file;
struct vnode;

/* Start a program. Handle i of the new app is handles[i] (NULL: none). Its
   settings are "NAME=value" strings, passed beside its arguments. When app is
   set, that handle record reports the app's end. */
struct proc_launch {
    int argc;
    char **argv;
    int setting_count;
    char **settings;
    struct file **handles;
    size_t handle_count;
    struct task *parent;
    struct vnode *app;
};
int proc_launch(const char *path, const struct proc_launch *launch);
extern int init_pid; /* /bin/init: the only app that may claim a PCI function */
int proc_spawn_simple(const char *path, const char *arg1, struct task *parent);
uint64_t proc_user_pages(struct task *t);

/* App handles (docs/nocturne-native-abi.md §5). */
struct vnode *proc_app_new(void);
int proc_app_open(int pid, struct file **out);
bool proc_app_is(struct vnode *v);
void proc_app_ended(struct vnode *v, int status); /* BSP IRQ guard held */
int proc_app_pid(struct vnode *v);
int proc_app_result(struct vnode *v, uint64_t timeout_ms, int *status); /* UINT64_MAX: no limit */
