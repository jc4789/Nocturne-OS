#pragma once
#include "sys/sched.h"
struct file;
int proc_spawn(const char *path, int argc, char **argv, struct file *fds[3], const char *cwd, struct task *parent);
int proc_spawn_simple(const char *path, const char *arg1, struct task *parent);
uint64_t proc_user_pages(struct task *t);
