#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
/* Persistent-engine private journal codec; no page task calls these APIs.
 * read returns an owned legacy-format manifest, keeping schema/catalog and
 * reference validation in webidb.c. Missing=0, valid=1, IO=-1, memory=-2. */
int webidb_journal_read(const char *base,int slot,uint8_t **bytes,size_t *length);
int webidb_journal_write(const char *base,int slot,const uint8_t *bytes,size_t length);
void webidb_journal_forget(void);
