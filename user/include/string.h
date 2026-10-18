#pragma once
#include <stddef.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strtok(char *s, const char *delim);
char *strtok_r(char *s, const char *delim, char **save);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strerror(int e);
NOCTURNE_END_DECLS

/* Programs are built freestanding, which stops the compiler from treating
   these as the standard functions: every copy of a 4-byte value or a small
   struct became a call. Routed through the builtins, copies, fills and
   comparisons of a known size are emitted inline and the rest still call
   the functions above. C++ keeps plain names for std:: and :: lookups. */
#if defined(__clang__) && !defined(__cplusplus) && !defined(NOCTURNE_STRING_IMPL)
#define memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define memmove(d, s, n) __builtin_memmove((d), (s), (n))
#define memset(d, c, n) __builtin_memset((d), (c), (n))
#define memcmp(a, b, n) __builtin_memcmp((a), (b), (n))
#define strlen(s) __builtin_strlen((s))
#endif
