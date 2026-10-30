/* What the parts of the kernel's C: share: fs/store.c (paths, entries, the writer and commits),
   fs/document.c (open documents) and fs/watch.c (watches), over the store (common/store).

   Locks, each taken before the ones after it:
   - a REPLACE opening's own document, while its save takes the name it replaces;
   - store.moves, while an entry moves from one folder to another;
   - folders (folder_lock), whose names change one at a time (store/namespace.h): one that holds
     another before it, two others by their depth and then their id, the temp folder after every
     other (whoever holds it takes no other folder);
   - documents (fs/document.c), each its openings' reads and writes and its content;
   - the gate (change_begin): every change to C: is in one generation;
   - the store's latches, and the spinlocks of the tables here, held for a few steps with nothing
     taken under them.
   store.committing is a commit's: it closes the gate, and takes nothing else of the above. */
#pragma once
#include "store/namespace.h"
#include "fs/store.h"
#include "arch/spin.h"
#include "sys/sched.h"
#include "sys/syscall.h"

struct handle;

struct store {
    struct volume *v;
    struct store_supply *blocks;
    uint64_t temp;           /* C:\Nocturne\Temp: replacements not yet saved, documents removed while open */
    uint64_t temp_names;     /* the last name given there (atomic) */
    struct mutex moves;      /* moves from one folder to another, one at a time */
    struct mutex committing; /* commits, one at a time */
    /* The gate: changes under way, and a freeze that waits for them to end (closing), which a
       change that comes after it waits for in turn (open). */
    struct spinlock gate;
    unsigned changes;
    bool closing;
    struct wait_queue open, drained;
    /* The writer: changes reach the medium a few seconds after the first of them. dirty_lock
       guards dirty and soon, which any CPU sets (store_dirty) and the writer waits for in kick; it
       comes before kick's lock. */
    struct spinlock dirty_lock;
    struct wait_queue kick;
    bool dirty, soon;
};
extern struct store store;

/* A change to C:, from change_begin to change_end: in the generation change_begin gives, which no
   commit freezes meanwhile. A change never begins inside another. */
uint64_t change_begin(void);
void change_end(void);
/* A change's time: nanoseconds since 1970 UTC. */
int64_t store_now(void);

/* A folder's names held still (fs/document.h's order): NULL without memory. */
struct folder_lock;
struct folder_lock *folder_lock(uint64_t folder);
void folder_unlock(struct folder_lock *l);

/* Where a path leads, for the kernel or (user) for an app, to which PRIVATE entries are not there:
   the folder holding its last name (depth: the names before it), and the entry when there is one
   (found). The root is found and has no name. place_find looks without a lock: what it finds may
   change until place_lock locks the folder and looks again (-N_ERR_NOT_FOUND: the folder went). */
struct place {
    struct path path;
    uint64_t folder;
    size_t depth;
    uint16_t folder_attributes;
    const char *name; /* into the path's text */
    size_t n;
    bool found;
    struct ns_entry entry;
    struct store_record record;
};
int place_find(const char *text, bool user, struct place *out);
void place_free(struct place *p);
int place_lock(struct place *p, bool user, struct folder_lock **out);
/* An app changes no SYSTEM entry and nothing in a SYSTEM folder: -N_ERR_DENIED. entry: the entry
   itself changes, not only the folder's list. */
int place_may_change(const struct place *p, bool user, bool entry);
/* out: entry id, called name (n bytes), as n_entry_info gives it from its record. */
void entry_fill(struct n_entry *out, const char *name, size_t n, uint64_t id, const struct store_record *rec);

/* Something changed: the writer commits it within seconds (soon: at once). */
void store_dirty(bool soon);
/* Commits what changed so far. */
int store_commit(void);
/* Until generation is on the medium (an open one that changed nothing has nothing to wait for). */
int store_wait(uint64_t generation);
/* A new name in the temp folder (out: 17 bytes): its number, and its length in *n. */
uint64_t store_temp_name(char *out, size_t *n);
/* The text of name number there (out: 17 bytes), its length. */
size_t store_temp_text(uint64_t number, char *out);

/* Copies between the kernel and buf, the calling app's memory when user. */
static inline int copy_out(void *to, const void *from, size_t n, bool user) {
    if (user) return copy_to_user(to, from, n);
    memcpy(to, from, n);
    return 0;
}
static inline int copy_in(void *to, const void *from, size_t n, bool user) {
    if (user) return copy_from_user(to, from, n);
    memcpy(to, from, n);
    return 0;
}

/* fs/document.c */
int document_open(const char *path, int access, bool user, struct handle **out);
/* The size of document id as its writers have it now, or -N_ERR_NOT_FOUND when none is open. */
int64_t document_size_now(uint64_t id);
/* The document entry `name` (id) of folder goes, the folder locked: when it is open, it goes to the
   temp folder instead (that locked meanwhile), and from there with its last opening. */
int document_remove(uint64_t folder, const char *name, size_t n, uint64_t id, struct ns_told *told);
/* Document id went to the temp folder as name number in generation g (its name taken by a move),
   the temp folder locked: it goes from there with its last opening, or now when it is not open. */
void document_aside(uint64_t id, uint64_t number, uint64_t g);
/* Document id was renamed or moved in generation g: a save of an opening that wrote nothing waits
   for it. */
void document_changed(uint64_t id, uint64_t g);
/* Raw document id (store_raw_place) becomes size bytes long, every block of it placed: in
   generation *g. */
int document_raw(uint64_t id, uint64_t size, uint64_t *g);
/* Every document's unsaved writes into the catalog (the writer, store_sync). */
int documents_flush(void);
/* After each commit: N_COMMIT_START saves it settled hear of it. */
void documents_committed(void);
