/* Watches on C: (fs/watch.h).

   One struct entry_watch for each thing watched (a name of a folder, or a folder), shared by every
   handle opened on it: sys/watch.c counts for each handle from its opening. The table finds them by
   their folder's id under one spinlock, so a change costs a look at the watches of its own folder.
   A name's watch keeps the entry the name names now, put right by every change of that name, so
   that a document's new content finds the names that show it without a look at the catalog. A
   watch is made with its folder locked: no change to the folder's names comes between its look at
   the name and its opening. */
#include "fs/watch.h"
#include "fs/document.h"
#include "lib/idtable.h"
#include "mm/heap.h"
#include "sys/watch.h"

struct entry_watch {
    struct watched watched;
    struct id_link link;      /* by its folder's id: moved or renamed, it is the same folder */
    bool listed;              /* in the table: until its folder goes or its last handle closes */
    uint32_t what;            /* N_WATCH_NAME or N_WATCH_FOLDER */
    uint64_t names;           /* N_WATCH_NAME: the entry its name names now (0: none) */
    uint64_t step;            /* the last change it counted */
    size_t klen;
    uint8_t key[];            /* N_WATCH_NAME: the name's place in the folder (store/namespace.h) */
};

/* Every watch whose folder is there. */
static struct {
    struct spinlock lock; /* everything here and each watch's listed, names and step */
    struct id_table table;
    uint64_t step;        /* each change told is a step */
} watches;

static struct entry_watch *of_watched(struct watched *w) {
    return (struct entry_watch *)((char *)w - offsetof(struct entry_watch, watched));
}

static struct entry_watch *of_link(struct id_link *l) {
    return (struct entry_watch *)((char *)l - offsetof(struct entry_watch, link));
}

/* One more change for w's handles, once in a step; none that no app sees. */
static void count(struct entry_watch *w, bool hidden) {
    if (hidden || w->step == watches.step) return;
    w->step = watches.step;
    watched_changed(&w->watched);
}

/* The folder is no more: its watches turn N_EVENT_CLOSED and leave the table. */
static void folder_gone(uint64_t folder) {
    for (struct id_link *l = id_table_find(&watches.table, folder), *next; l; l = next) {
        next = id_table_next(l);
        struct entry_watch *w = of_link(l);
        id_table_drop(&watches.table, l);
        w->listed = false;
        watched_gone(&w->watched);
    }
}

void entry_watch_publish(const struct ns_told *t) {
    if (!t->n) return;
    uint64_t flags = spin_lock(&watches.lock);
    if (watches.table.count) {
        watches.step++;
        for (size_t i = 0; i < t->n; i++) {
            const struct ns_change *c = &t->c[i];
            for (struct id_link *l = id_table_find(&watches.table, c->folder); l; l = id_table_next(l)) {
                struct entry_watch *w = of_link(l);
                if (w->what == N_WATCH_FOLDER) {
                    count(w, c->hidden);
                } else if (c->named) {
                    if (w->klen != c->klen || memcmp(w->key, c->key, w->klen)) continue;
                    w->names = c->id;
                    count(w, c->hidden);
                } else if (w->names == c->id) {
                    count(w, c->hidden);
                }
            }
            if (c->gone) folder_gone(c->gone);
        }
    }
    spin_unlock(&watches.lock, flags);
}

/* The last handle on it closed: it leaves the table (unless its folder went first). */
static void release(struct watched *watched) {
    struct entry_watch *w = of_watched(watched);
    uint64_t flags = spin_lock(&watches.lock);
    if (w->listed) id_table_drop(&watches.table, &w->link);
    spin_unlock(&watches.lock, flags);
    kfree(w);
}

int64_t store_entry_watch(const char *path, uint32_t what, bool user) {
    if (what != N_WATCH_NAME && what != N_WATCH_FOLDER) return -N_ERR_INVALID;
    struct volume *v = store.v;
    struct place p;
    int r = place_find(path, user, &p);
    if (r < 0) return r;
    if (what == N_WATCH_FOLDER) {
        /* the folder watched is the one the path names: its own names held still */
        r = !p.found ? -N_ERR_NOT_FOUND : p.record.kind != STORE_FOLDER ? -N_ERR_NOT_FOLDER : 0;
        p.folder = p.entry.id;
        p.depth++;
        p.name = NULL;
        p.n = 0;
    } else if (!p.n) {
        r = -N_ERR_INVALID; /* the root has no name */
    }
    struct folder_lock *f = NULL;
    if (r >= 0) r = place_lock(&p, user, &f);
    uint8_t *key = r >= 0 && what == N_WATCH_NAME ? kmalloc(STORE_KEY_MAX) : NULL;
    struct ns_entry *e = key ? kmalloc(sizeof *e) : NULL;
    if (r >= 0 && what == N_WATCH_NAME && !e) r = -N_ERR_NO_MEMORY;
    size_t klen = 0;
    uint64_t names = 0;
    if (r >= 0 && what == N_WATCH_NAME && (r = ns_key(v, p.folder, p.name, p.n, key)) >= 0) {
        klen = (size_t)r;
        /* what the name names, there for the caller or not: a change to it counts only when it is */
        if ((r = ns_find(v, p.folder, p.name, p.n, e)) >= 0) names = e->id;
        if (r == -N_ERR_NOT_FOUND) r = 0;
    }
    struct entry_watch *fresh = r >= 0 ? kzalloc(sizeof *fresh + klen) : NULL;
    if (r >= 0 && !fresh) r = -N_ERR_NO_MEMORY;
    if (r >= 0) {
        watched_init(&fresh->watched, release);
        fresh->what = what;
        fresh->link.id = p.folder;
        fresh->names = names;
        fresh->klen = klen;
        if (klen) memcpy(fresh->key, key, klen);
    }
    kfree(key);
    kfree(e);
    /* The same thing watched already: one more handle on it, held from here until its opening (a
       handle that could not be made leaves it to go then). One whose last reference went on
       another CPU is on its way out: the new handle goes on a watch of its own. */
    struct entry_watch *w = NULL;
    if (r >= 0) {
        uint64_t flags;
        r = id_table_room(&watches.table, &watches.lock, &flags);
        for (struct id_link *l = r >= 0 ? id_table_find(&watches.table, p.folder) : NULL; l; l = id_table_next(l)) {
            struct entry_watch *o = of_link(l);
            if (o->what == what && o->klen == klen && (!klen || !memcmp(o->key, fresh->key, klen)) &&
                object_ref_live(&o->watched.object)) {
                w = o;
                break;
            }
        }
        if (r >= 0 && !w) {
            w = fresh;
            fresh = NULL;
            id_table_add(&watches.table, &w->link);
            w->listed = true;
            object_ref(&w->watched.object);
        }
        spin_unlock(&watches.lock, flags);
    }
    int64_t h = r;
    if (r >= 0) h = watched_open(&w->watched); /* counts from now: the folder's names stay as they were looked at */
    if (f) folder_unlock(f);
    place_free(&p);
    if (w) object_unref(&w->watched.object);
    kfree(fresh);
    return h;
}
