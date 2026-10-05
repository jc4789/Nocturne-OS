#pragma once
#include "kernel.h"
struct file;
void vfs_close(struct file *f);
int64_t vfs_write(struct file *f, const void *buf, size_t n);
