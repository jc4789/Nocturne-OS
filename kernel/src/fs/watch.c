/* Watches on C: (fs/watch.h).

   One struct entry_watch for each thing watched (a name of a folder, or a folder), shared by every
   handle opened on it: sys/watch.c counts for each handle from its opening. The table finds them by
   their folder's id, so a change of the namespace costs a look at the watches of its own folder,
   and nothing at all while no watch is open. A name's watch keeps the entry the name names now, put
   right by every change of that name, so that a document's new content finds the names that show it
   without a look at the catalog. */
#include "fs/watch.h"
#include "fs/document.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "sys/watch.h"

struct entry_watch {
    struct watched watched;
    struct entry_watch *next; /* in its bucket */
    bool listed;              /* in the table: until its folder goes or its last handle closes */
    uint32_t what;            /* N_WATCH_NAME or N_WATCH_FOLDER */
    uint64_t folder;          /* by what it is: moved or renamed, it is the same folder */
    uint64_t names;           /* N_WATCH_NAME: the entry its name names now (0: none) */
    uint64_t step;            /* the last step of changes it counted */
    size_t klen;
    uint8_t key[];            /* N_WATCH_NAME: the name's place in the folder (fs/namespace.h) */
};

/* Every watch whose folder is there, by the folder's id. */
static struct {
    struct entry_watch **bucket; /* a power of two of them */
    size_t size, count;
    uint64_t step;               /* each change the namespace tells is a step, a together one */
    unsigned together;
} table;

static struct entry_watch *of(struct watched *w) {
    return (struct entry_watch *)((char *)w - offsetof(struct entry_watch, watched));
}

static size_t slot(uint64_t folder, size_t size) { return (size_t)((folder * 0x9e3779b97f4a7c15ull) >> 32) & (size - 1); }

/* Room for one more watch: twice the buckets once there are as many watches as buckets. Without
   memory for that, the chains grow longer. */
static int room(void) {
    if (table.count < table.size) return 0;
    size_t size = table.size ? table.size * 2 : 16;
    struct entry_watch **bucket = kzalloc(size * sizeof *bucket);
    if (!bucket) return table.size ? 0 : -N_ERR_NO_MEMORY;
    for (size_t i = 0; i < table.size; i++)
        for (struct entry_watch *w = table.bucket[i], *next; w; w = next) {
            next = w->next;
            struct entry_watch **at = &bucket[slot(w->folder, size)];
            w->next = *at;
            *at = w;
        }
    kfree(table.bucket);
    table.bucket = bucket;
    table.size = size;
    return 0;
}

static void list_add(struct entry_watch *w) {
    struct entry_watch **at = &table.bucket[slot(w->folder, table.size)];
    w->next = *at;
    *at = w;
    w->listed = true;
    table.count++;
}

static void list_drop(struct entry_watch *w) {
    struct entry_watch **at = &table.bucket[slot(w->folder, table.size)];
    while (*at != w) at = &(*at)->next;
    *at = w->next;
    w->listed = false;
    table.count--;
}

/* One more change for w's handles, once in a step; none that no app sees. */
static void count(struct entry_watch *w, bool hidden) {
    if (hidden || w->step == table.step) return;
    w->step = table.step;
    watched_changed(&w->watched);
}

/* The folder is no more: its watches turn N_EVENT_CLOSED and leave the table. */
static void folder_gone(uint64_t folder) {
    for (struct entry_watch **at = &table.bucket[slot(folder, table.size)], *w; (w = *at);) {
        if (w->folder != folder) {
            at = &w->next;
            continue;
        }
        *at = w->next;
        w->listed = false;
        table.count--;
        watched_gone(&w->watched);
    }
}

/* struct volume's watch: what the namespace changed in one step. */
static void heard(struct volume *v, const struct ns_change *c, size_t n) {
    (void)v;
    if (!table.count) return;
    if (!table.together) table.step++;
    for (size_t i = 0; i < n; i++) {
        for (struct entry_watch *w = table.bucket[slot(c[i].folder, table.size)]; w; w = w->next) {
            if (w->folder != c[i].folder) continue;
            if (w->what == N_WATCH_FOLDER) {
                count(w, c[i].hidden);
            } else if (c[i].key) {
                if (w->klen != c[i].klen || memcmp(w->key, c[i].key, w->klen)) continue;
                w->names = c[i].id;
                count(w, c[i].hidden);
            } else if (w->names == c[i].id) {
                count(w, c[i].hidden);
            }
        }
        if (c[i].gone) folder_gone(c[i].gone);
    }
}

void entry_watch_start(struct volume *v) { v->watch = heard; }

void entry_watch_together(void) {
    if (!table.together++) table.step++;
}

void entry_watch_apart(void) { table.together--; }

/* The last handle on it closed: it leaves the table (unless its folder went first). */
static void release(struct watched *watched) {
    struct entry_watch *w = of(watched);
    struct volume *v = store.v;
    mutex_lock(&v->lock);
    if (w->listed) list_drop(w);
    mutex_unlock(&v->lock);
    kfree(w);
}

/* A watch whose last handle has closed is on its way out (release waits for v->lock): a new handle
   goes on a watch of its own. */
static bool alive(struct entry_watch *w) {
    uint64_t f = irq_save();
    bool a = w->watched.object.refs > 0;
    irq_restore(f);
    return a;
}

int64_t store_entry_watch(const char *path, uint32_t what, bool user) {
    if (what != N_WATCH_NAME && what != N_WATCH_FOLDER) return -N_ERR_INVALID;
    struct volume *v = store.v;
    uint8_t *key = NULL;
    struct ns_entry *e = NULL;
    if (what == N_WATCH_NAME && (!(key = kmalloc(STORE_KEY_MAX)) || !(e = kmalloc(sizeof *e)))) {
        kfree(key);
        return -N_ERR_NO_MEMORY;
    }
    struct place p;
    uint64_t folder = 0, names = 0;
    size_t klen = 0;
    mutex_lock(&v->lock);
    int r = place_find(path, user, &p);
    if (r >= 0) {
        if (what == N_WATCH_FOLDER) {
            r = !p.found ? -N_ERR_NOT_FOUND : p.record.kind != STORE_FOLDER ? -N_ERR_NOT_FOLDER : 0;
            folder = p.entry.id;
        } else if (!p.n) {
            r = -N_ERR_INVALID; /* the root has no name */
        } else if ((r = ns_key(v, p.folder, p.name, p.n, key)) >= 0) {
            folder = p.folder;
            klen = (size_t)r;
            /* what the name names, there for the caller or not: a change to it counts only when it is */
            if ((r = ns_find(v, folder, p.name, p.n, e)) >= 0) names = e->id;
            if (r == -N_ERR_NOT_FOUND) r = 0;
        }
        place_free(&p);
    }
    /* the same thing watched already: one more handle on it */
    struct entry_watch *w = NULL;
    if (r >= 0 && table.size)
        for (w = table.bucket[slot(folder, table.size)]; w; w = w->next)
            if (w->folder == folder && w->what == what && w->klen == klen && (!klen || !memcmp(w->key, key, klen)) &&
                alive(w))
                break;
    if (r >= 0 && !w && (r = room()) >= 0) {
        if (!(w = kzalloc(sizeof *w + klen))) {
            r = -N_ERR_NO_MEMORY;
        } else {
            watched_init(&w->watched, release);
            w->what = what;
            w->folder = folder;
            w->names = names;
            w->klen = klen;
            if (klen) memcpy(w->key, key, klen);
            list_add(w);
        }
    }
    int64_t h = r;
    if (r >= 0) {
        /* held while v->lock is let go: a handle that could not be made leaves it to go then */
        object_ref(&w->watched.object);
        h = watched_open(&w->watched); /* counts from now: no change can come between */
    }
    mutex_unlock(&v->lock);
    if (r >= 0) object_unref(&w->watched.object);
    kfree(key);
    kfree(e);
    return h;
}
