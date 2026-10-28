#pragma once
#include <webidb.h>
#include <nocturne.h>
#include <stdbool.h>

/* Browser-private, pointer-free native storage broker protocol. Only the
 * browser's origin-policy gate supplies origin; scripts never supply paths. */
#define WEB_IDB_WIRE_MAGIC 0x49444231u
enum webidb_operation {
    WEB_IDB_BEGIN=1, WEB_IDB_STAGE, WEB_IDB_VALUE, WEB_IDB_COMMIT,
    WEB_IDB_ABORT, WEB_IDB_DATABASES, WEB_IDB_CONNECTIONS, WEB_IDB_CONNECT,
    WEB_IDB_POLL, WEB_IDB_DISCONNECT, WEB_IDB_FORGET, WEB_IDB_VALUES,
    /* The connection's versionchange event (the one POLL delivered) has been fired: the
       opener's CONNECTIONS no longer counts it as waiting to be fired (webidb_fired). */
    WEB_IDB_FIRED
};
struct webidb_wire_command {
    uint32_t magic, operation, sequence, token;
    uint64_t version, origin_length, name_length, payload_length, reference_count;
};
struct webidb_wire_result {
    uint32_t magic, operation, sequence, status, token, pending;
    uint64_t version, count, payload_length;
};
_Static_assert(sizeof(struct webidb_wire_command)==56,"IndexedDB broker command v1");
_Static_assert(sizeof(struct webidb_wire_result)==48,"IndexedDB broker result v1");

typedef struct webidb_async webidb_async;
struct webidb_async_request {
    unsigned operation;
    uint32_t token;
    uint64_t version;
    const char *origin, *name;
    size_t name_length;
    const uint8_t *bytes;
    size_t length;
    const char *reference;
    const char *const *references;
    size_t reference_count;
};
struct webidb_async_result {
    unsigned operation;
    uint32_t sequence, token;
    int status, pending;
    uint64_t version;
    size_t count;
    uint8_t *bytes;
    size_t length;
    char reference[65];
    struct webidb_database *items;
    size_t item_count;
};
/* queue: the event queue whose wait this transport's results must end (the pumping thread's).
 * While a sent command waits for its result, the channel is watched there (N_EVENT_RECEIVABLE,
 * N_EVENT_CLOSED); the watch goes when the result is in, so an idle helper never wakes it.
 * -1: none, and webidb_async_watched is false. */
webidb_async *webidb_async_create(n_handle queue);
/* Copies private input into owned queue storage; never opens the browser's data or waits
 * for helper/disk. Zero means allocation/representation/spawn failure. */
uint32_t webidb_async_submit(webidb_async *,const struct webidb_async_request *);
/* At most 64 KiB per lane; nonblocking pipe I/O, no yield/wait or author JS. */
void webidb_async_pump(webidb_async *);
bool webidb_async_take(webidb_async *,struct webidb_async_result *);
bool webidb_async_pending(const webidb_async *);
bool webidb_async_ready(const webidb_async *);
bool webidb_async_failed(const webidb_async *);
/* Whether the result of the command sent will end a wait on the queue given at create. */
bool webidb_async_watched(const webidb_async *);
void webidb_async_result_free(struct webidb_async_result *);
/* Closes lanes and retires helper without a blocking wait. */
void webidb_async_free(webidb_async *);
void webidb_async_background(void);
