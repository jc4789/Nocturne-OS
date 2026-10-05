/* Nocturne OS - common kernel definitions */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define OS_NAME    "Nocturne"
#define OS_VERSION "0.9 \"Moonrise\""

#define ALIGN_UP(x, a)   ((((uint64_t)(x)) + ((uint64_t)(a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) (((uint64_t)(x)) & ~((uint64_t)(a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS(a) ((a) < 0 ? -(a) : (a))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PACKED   __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define UNUSED   __attribute__((unused))
#define PAGE_SIZE 4096ULL

/* errno values (negated in syscall returns) */
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EFAULT  14
#define EBUSY   16
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EPIPE   32
#define ERANGE  34
#define ENAMETOOLONG 36
#define ENOSYS  38
#define ENOTEMPTY 39
#define ECONNRESET 104
#define EADDRINUSE 98
#define ENETDOWN 100
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113

/* lib/string.c */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strdup(const char *s);
int toupper(int c);
int tolower(int c);

/* lib/printf.c */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void klog_write(const char *s, size_t n);
/* true if the kernel command line contains this exact word (init.c) */
bool cmdline_has(const char *word);
size_t klog_read(char *buf, size_t max);

#define ASSERT(x) do { if (!(x)) panic("assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__); } while (0)

/* HHDM helpers */
extern uint64_t hhdm_offset;
static inline void *phys_to_virt(uint64_t p) { return (void *)(p + hhdm_offset); }
static inline uint64_t hhdm_virt_to_phys(const void *v) { return (uint64_t)v - hhdm_offset; }

/* timekeeping */
extern volatile uint64_t timer_ticks; /* milliseconds since boot */
static inline uint64_t uptime_ms(void) { return timer_ticks; }
