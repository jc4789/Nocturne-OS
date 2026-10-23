/* The part of kernel/src/kernel.h the store's shared code (kernel/src/fs) uses, on the build host:
   the C library's strings and memory, messages to stderr. */
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "abi.h"

#define ALIGN_UP(x, a) ((((uint64_t)(x)) + ((uint64_t)(a) - 1)) & ~((uint64_t)(a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PAGE_SIZE 4096ULL

#define kprintf(...) fprintf(stderr, __VA_ARGS__)
/* the C library's printf, whose formats (%zu) MinGW's GCC would judge by the old Windows ones */
#if defined(__GNUC__) && !defined(__clang__)
_Noreturn void panic(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
#else
_Noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif
size_t strlcpy(char *d, const char *s, size_t n);
