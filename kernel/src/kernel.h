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

/* Results: the negated N_ERR_* codes of common/abi.h. */
#include "abi.h"

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

/* lib/sort.c: count items of size bytes, in place by cmp's order (negative: a before b). */
void sort(void *items, size_t count, size_t size, int (*cmp)(const void *a, const void *b));

/* lib/printf.c: the formatter. kvsnprintf(NULL, 0, ...) measures. */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* sys/log.c: the kernel's log. A line is a record (the clock's ns, the CPU, the task, its level,
   its text) in the writing CPU's own ring: any CPU, any context (an NMI too), without a lock and
   without waiting for a reader (the serial port, the boot screen, the system log's readers; sys/
   log.h). A line longer than LOG_TEXT_MAX keeps its first LOG_TEXT_MAX bytes. kprintf is klog at
   LOG_INFO and returns the text's length. */
enum log_level { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };
void klog(enum log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int kvlog(enum log_level level, const char *fmt, va_list ap);
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* sys/panic.c: the kernel stops. Without a lock or an allocation, every other CPU stopped first,
   the report goes straight to the screen shown and to the serial port. A CPU that faults while
   another reports halts. */
struct regs;
NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
NORETURN void panic_regs(const char *what, struct regs *r); /* a fault: the registers it found */

/* true if the kernel command line contains this exact word (init.c) */
bool cmdline_has(const char *word);
/* key=<decimal> on the kernel command line: the last well-formed one (init.c) */
bool cmdline_number(const char *key, uint64_t *out);

#define ASSERT(x) do { if (!(x)) panic("assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__); } while (0)

/* HHDM helpers */
extern uint64_t hhdm_offset;
static inline void *phys_to_virt(uint64_t p) { return (void *)(p + hhdm_offset); }
static inline uint64_t hhdm_virt_to_phys(const void *v) { return (uint64_t)v - hhdm_offset; }

