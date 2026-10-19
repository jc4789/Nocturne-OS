/* Host replacements for the kernel's lib/ and heap primitives, shared by all
   harnesses. Allocation goes to the host heap so AddressSanitizer sees every
   product allocation, overrun and use-after-free. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* Do not include fz.h here: this file only needs the primitive types. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kernel.h"
#include "mm/heap.h"
#include "sys/sched.h"

struct task fz_task;
volatile bool fz_resched;
volatile uint64_t timer_ticks;
uint64_t hhdm_offset;
const char *kernel_cmdline = "";

/* The kernel heap is finite: refuse absurd requests instead of reserving host memory. */
#define FZ_HEAP_LIMIT ((size_t)256 << 20)
void *kmalloc(size_t n) { return n > FZ_HEAP_LIMIT ? NULL : malloc(n ? n : 1); }
void *kzalloc(size_t n) { return n > FZ_HEAP_LIMIT ? NULL : calloc(1, n ? n : 1); }
void kfree(void *p) { free(p); }
void *krealloc(void *p, size_t n) { return n > FZ_HEAP_LIMIT ? NULL : realloc(p, n ? n : 1); }
uint64_t heap_used_bytes(void) { return 0; }
uint64_t heap_size_bytes(void) { return 0; }

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    /* the kernel printf accepts %s of NULL; keep host snprintf semantics otherwise */
    return vsnprintf(buf, size, fmt, ap);
}
int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
void klog_write(const char *s, size_t n) { (void)s; (void)n; }
size_t klog_read(char *buf, size_t max) { (void)buf; (void)max; return 0; }
bool cmdline_has(const char *word) { (void)word; return false; }

void panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) {
        size_t c = l >= n ? n - 1 : l;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
int strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}
