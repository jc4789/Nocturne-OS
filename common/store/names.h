/* Names and paths on the store (common/store_format.h).

   A name is 1 to 255 bytes of UTF-8 without control characters, \ / : * ? " < > |, or a space or
   period at its end, and is not "." or "..". Names are kept as given and compare by their simple
   case folding. A path is "C:\a\b" or "\a\b" ('/' works as '\'); "." and ".." are resolved by the
   path's text, before any lookup. */
#pragma once
#include "abi.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct store_env;

/* The C library's string functions, which every program the store runs in has. */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char *strchr(const char *s, int c);

/* A volume's case folding (its document STORE_ID_FOLD). */
struct fold {
    uint32_t unicode, count;
    uint32_t (*pairs)[2];
};
int fold_load(const struct store_env *env, const void *data, size_t n, struct fold *out);
void fold_free(const struct store_env *env, struct fold *f);
/* The table a new volume gets: from the env's alloc, *n bytes. */
int fold_document(const struct store_env *env, uint8_t **out, size_t *n);

/* 0 for a valid name, else -N_ERR_INVALID or -N_ERR_NAME_TOO_LONG. */
int name_check(const char *name, size_t n);
/* name's folding into out (cap bytes); its length, or -N_ERR_NAME_TOO_LONG. */
int name_fold(const struct fold *f, const char *name, size_t n, uint8_t *out, size_t cap);

/* A path as its names, from the root. */
struct path {
    size_t count;
    struct { const char *name; size_t n; } *part; /* into the path's text */
};
/* -N_ERR_INVALID for a relative path or a bad name, -N_ERR_NOT_FOUND for another drive. */
int path_parse(const struct store_env *env, const char *text, struct path *out);
void path_free(const struct store_env *env, struct path *p);
