/* JSON: a small DOM parser and a text builder (libc/json.c). */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct json {
    int type;
    bool b;
    double num;
    char *str;    /* J_STR: UTF-8, NUL-terminated */
    size_t len;   /* J_STR: length in bytes */
    int n, cap;   /* J_ARR / J_OBJ: number of items */
    struct json **items;
    char **keys;  /* J_OBJ only */
};

/* parse len bytes of text; returns NULL and sets *err (if given) on a syntax error */
struct json *json_parse(const char *s, size_t len, const char **err);
void json_free(struct json *j);

struct json *json_get(const struct json *obj, const char *key); /* NULL if absent / not an object */
struct json *json_at(const struct json *arr, int i);            /* NULL if out of range */
const char *json_str(const struct json *j);                     /* NULL unless a string */
const char *json_getstr(const struct json *obj, const char *key);
double json_getnum(const struct json *obj, const char *key, double def);

/* ---- building JSON text ---- */
struct jbuf {
    char *s;
    size_t len, cap;
    bool oom; /* an allocation failed: the text is incomplete */
};

void jb_init(struct jbuf *b);
void jb_free(struct jbuf *b);
void jb_raw(struct jbuf *b, const char *s, size_t n);
void jb_puts(struct jbuf *b, const char *s);
void jb_printf(struct jbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jb_vprintf(struct jbuf *b, const char *fmt, va_list ap);
/* a quoted, escaped JSON string; invalid UTF-8 bytes become U+FFFD so the result is always valid */
void jb_str(struct jbuf *b, const char *s);
void jb_strn(struct jbuf *b, const char *s, size_t n);
void json_write(struct jbuf *b, const struct json *j); /* serialize a parsed value */
char *jb_take(struct jbuf *b); /* hand over the NUL-terminated text (caller frees) */
