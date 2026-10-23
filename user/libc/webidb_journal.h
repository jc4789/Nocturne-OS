#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Persistent-engine private journal codec; no page task calls these APIs.
 * read returns an owned legacy-format manifest, keeping schema/catalog and
 * reference validation in webidb.c. Missing=0, valid=1, IO=-1, memory=-2. */
/* base is the complete path a database's files hang from, shorter than WEBIDB_BASE_MAX; the
 * names added to it (".journal3", ".pending", ".values" and the files in it, ...) fit in WEBIDB_FILE_MAX. */
#define WEBIDB_BASE_MAX 1024
#define WEBIDB_FILE_MAX (WEBIDB_BASE_MAX + 96)
int webidb_journal_read(const char *base,int slot,uint8_t **bytes,size_t *length);
int webidb_journal_write(const char *base,int slot,const uint8_t *bytes,size_t length);
void webidb_journal_forget(void);
