/* Stubs for the vfs/ramfs/initrd harness. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kernel.h"
#include "fs/vfs.h"
#include "sys/sched.h"
#include "sys/syscall.h"

int64_t time_now(void) { return 1; }
void events_file_closed(struct file *f) { (void)f; }
void wq_wake_all(struct wait_queue *wq) { (void)wq; }
void wq_wait(struct wait_queue *wq) { (void)wq; }
bool user_ok(const void *p, size_t n) { return p || !n; }
bool user_ok_w(void *p, size_t n) { return p || !n; }

#include "dev/timer.h"
void time_to_parts(int64_t t, struct tm_parts *p) {
    /* any monotone mapping will do; values stay in the ranges the FAT code encodes */
    p->year = 2000 + (int)((t / 31536000) % 100);
    p->mon = 1 + (int)((t / 2592000) % 12);
    p->mday = 1 + (int)((t / 86400) % 28);
    p->hour = (int)((t / 3600) % 24);
    p->min = (int)((t / 60) % 60);
    p->sec = (int)(t % 60);
    p->wday = 0;
}
int64_t time_from_parts(const struct tm_parts *p) { return (int64_t)(p->year - 1970) * 31536000 + p->mon * 2592000 + p->mday * 86400; }
