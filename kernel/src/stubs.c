#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
void vfs_close(struct file *f) {}
int64_t vfs_write(struct file *f, const void *buf, size_t n) { return 0; }
void wm_process_exit(int pid) {}
void syscall_dispatch(struct regs *r) {}
static void spinner(void *arg) {
    for (int i = 0;; i++) { kprintf("[%s] tick %d at %lu ms\n", (char*)arg, i, uptime_ms()); sleep_ms(500); if (i == 5) break; }
}
void kmain_late(void) {
    kthread_create("a", spinner, "thread A");
    kthread_create("b", spinner, "thread B");
}
