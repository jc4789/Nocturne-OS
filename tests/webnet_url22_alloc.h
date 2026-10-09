#pragma once
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
void *count_malloc(size_t);
void *count_calloc(size_t,size_t);
void *count_realloc(void *,size_t);
void count_free(void *);
char *count_strdup(const char *);
#define malloc count_malloc
#define calloc count_calloc
#define realloc count_realloc
#define free count_free
#define strdup count_strdup
