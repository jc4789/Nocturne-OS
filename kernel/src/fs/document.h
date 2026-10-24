/* Open documents (fs/document.c), and the state of C: they share with the rest of the store
   (fs/store.c). The caller holds store.v->lock unless a function says otherwise. */
#pragma once
#include "fs/namespace.h"
#include "fs/store.h"
#include "sys/syscall.h"

struct handle;

struct store {
    struct volume *v;
    uint64_t temp;       /* C:\Nocturne\Temp: replacements not yet saved, documents removed while open */
    uint64_t temp_names; /* the last name given there */
    /* the writer: changes reach the medium a few seconds after the first of them */
    struct wait_queue kick;
    bool dirty, soon;
};
extern struct store store;

/* Where a path leads, for the kernel or (user) for an app, to which PRIVATE entries are not there:
   the folder holding its last name, and the entry when there is one (found). The root is found and
   has no name. */
struct place {
    struct path path;
    uint64_t folder;
    uint16_t folder_attributes;
    const char *name; /* into the path's text */
    size_t n;
    bool found;
    struct ns_entry entry;
    struct store_record record;
};
int place_find(const char *text, bool user, struct place *out);
void place_free(struct place *p);
/* An app changes no SYSTEM entry and nothing in a SYSTEM folder: -N_ERR_DENIED. entry: the entry
   itself changes, not only the folder's list. */
int place_may_change(const struct place *p, bool user, bool entry);
/* out: entry id, called name (n bytes), as n_entry_info gives it from its record. */
void entry_fill(struct n_entry *out, const char *name, size_t n, uint64_t id, const struct store_record *rec);

/* Something changed: the writer commits it within seconds (soon: at once). */
void store_dirty(bool soon);
/* Commits what changed so far, then tells the saves it settled (v->lock not held). */
int store_commit(void);
/* Until generation is on the medium (v->lock not held). */
int store_wait(uint64_t generation);
/* The text of name number in the temp folder (out: 17 bytes), its length. A new one is
   ++store.temp_names. */
size_t store_temp_name(uint64_t number, char *out);

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
/* Document id, the entry name in folder, is about to be removed: when it is open, the entry goes
   to the temp folder instead and the document with its last opening (1); 0 when it is not open. */
int document_hide(uint64_t folder, const char *name, size_t n, uint64_t id);
/* Document id was renamed or moved: a save of an opening that wrote nothing waits for it. */
void document_changed(uint64_t id);
/* Every document's unsaved writes into the catalog (the writer, store_sync). */
int documents_flush(void);
/* After a commit (v->lock not held): N_COMMIT_START saves it settled hear of it. */
void documents_committed(void);
