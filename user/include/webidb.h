#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
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
/* commit with the lease kept: the token stays valid at the generation just written (its staged
 * payloads that are not referenced dropped), for the next transaction its caller schedules on
 * the database; webidb_commit or webidb_abort ends it. */
int webidb_save(webidb *db,uint32_t token,const uint8_t *bytes,size_t length,const char *const *references,size_t count,uint64_t version);
struct webidb_database {char *name;size_t name_length;uint64_t version;};
int webidb_databases(webidb *db,const char *origin,struct webidb_database **items,size_t *count);
void webidb_databases_free(struct webidb_database *items,size_t count);
int webidb_abort(webidb *db,uint32_t token);
/* Connections share notifications between native browser tasks, of every process and realm
 * (Window or Worker) of the origin. A caller retries BUSY asynchronously; desired_version=0
 * means database deletion. connections marks a versionchange to desired_version on each open
 * connection (once per desired version) and counts them: count those still open, unfired those
 * whose versionchange has not been fired yet. count also has, once (in the call whose unfired is
 * 0), each connection that closed after its versionchange to desired_version was fired while it
 * was open: its opener must fire blocked however soon the close came. poll delivers a connection's marked versionchange
 * once (*pending 1, *desired_version), or that webidb_clear removed its database (*pending 2:
 * close it forcibly); fired acknowledges that its versionchange has been fired (or skipped: its
 * close is pending), so IndexedDB's open fires blocked only after every one was fired. */
int webidb_connections(webidb *db,uint32_t token,uint64_t desired_version,size_t *count,size_t *unfired);
int webidb_connect(webidb *db,uint32_t token,uint64_t version,uint32_t *connection);
int webidb_poll(webidb *db,uint32_t connection,int *pending,uint64_t *desired_version);
int webidb_fired(webidb *db,uint32_t connection);
int webidb_disconnect(webidb *db,uint32_t connection);
int webidb_forget_connection(webidb *db,uint32_t connection);
/* The Settings window's site data, from any browser process. usage: each database in the
 * profile, whatever its origin, to each: the origin its catalog names ("" when neither slot can
 * be read) and the bytes of all its files. clear: removes every database whose origin match
 * accepts (match NULL: all) whole, as the Storage Standard clears a bucket. One in a transaction
 * now is left (its lease is held): WEB_IDB_BUSY once the rest is removed; the connections open to
 * a removed one, in any process, are closed forcibly at their next poll (webidb_poll). */
int webidb_usage(void (*each)(void *opaque,const char *origin,uint64_t bytes),void *opaque);
int webidb_clear(bool (*match)(void *opaque,const char *origin),void *opaque);
