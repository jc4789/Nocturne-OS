#pragma once
#include <stddef.h>
#include <stdint.h>
/* Browser-private IndexedDB snapshots. Native vnode leases are released on
 * abort, retirement, and task death. No caller-selected filesystem paths. */
typedef struct webidb webidb;
enum { WEB_IDB_OK, WEB_IDB_BUSY, WEB_IDB_SECURITY, WEB_IDB_IO, WEB_IDB_MEMORY };
webidb *webidb_create(void);
void webidb_free(webidb *db);
int webidb_begin(webidb *db,const char *origin,const char *name,size_t name_length,uint32_t *token,
                uint8_t **bytes,size_t *length);
int webidb_stage(webidb *db,uint32_t token,const uint8_t *bytes,size_t length,char reference[65]);
int webidb_value(webidb *db,uint32_t token,const char *reference,uint8_t **bytes,size_t *length);
int webidb_commit(webidb *db,uint32_t token,const uint8_t *bytes,size_t length,const char *const *references,size_t count,uint64_t version);
struct webidb_database {char *name;size_t name_length;uint64_t version;};
int webidb_databases(webidb *db,const char *origin,struct webidb_database **items,size_t *count);
void webidb_databases_free(struct webidb_database *items,size_t count);
int webidb_abort(webidb *db,uint32_t token);
/* Connections share notifications between native browser tasks. A caller
 * retries BUSY asynchronously; desired_version=0 means database deletion. */
int webidb_connections(webidb *db,uint32_t token,uint64_t desired_version,size_t *count);
int webidb_connect(webidb *db,uint32_t token,uint64_t version,uint32_t *connection);
int webidb_poll(webidb *db,uint32_t connection,int *pending,uint64_t *desired_version);
int webidb_disconnect(webidb *db,uint32_t connection);
int webidb_forget_connection(webidb *db,uint32_t connection);
