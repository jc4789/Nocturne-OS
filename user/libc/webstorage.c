/* Nocturne browser storage. This uses the existing native file API; no new
 * filesystem/ABI/POSIX requirement. Alternate checksum-validated snapshots
 * write only the inactive slot and keep the previous confirmed slot intact.
 * A complete write and successful close publish the inactive snapshot; checksum
 * validation happens once when opening a store, not after every write. No
 * copy-and-unlink rename is used. Shared journals in the temporary folder publish
 * per-key changes between windows; persistent snapshots remain debounced. */
#include <webstorage.h>
#include <weburl.h>
#include <nocturne.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bearssl_hash.h>

#define STORAGE_PATH 1024
const char *web_profile_folder(bool temporary) {
    static char folders[2][STORAGE_PATH];
    static int found[2];
    if (!__atomic_load_n(&found[temporary], __ATOMIC_ACQUIRE)) {
        char *folder = folders[temporary];
        n_result at = temporary ? n_path_join(n_temp_folder(), "Browser", folder, STORAGE_PATH)
                                : n_app_data_folder("Browser", folder, STORAGE_PATH);
        if (at >= 0 && temporary && (at = n_folder_create(folder)) == -N_ERR_EXISTS) at = 0;
        if (at < 0) return NULL;
        __atomic_store_n(&found[temporary], 1, __ATOMIC_RELEASE);
    }
    return folders[temporary];
}
/* The snapshots live in storage in the profile folder, the journals shared between windows in
 * storage in its temporary folder; NULL when the folder cannot be found. Only native regression
 * translation units override these constants. */
#if !defined(WEBSTORAGE_ROOT) || !defined(WEBSTORAGE_SHARED_ROOT)
static const char *storage_root(bool shared) {
    static char roots[2][STORAGE_PATH];
    static int found[2];
    if (!__atomic_load_n(&found[shared], __ATOMIC_ACQUIRE)) {
        const char *profile = web_profile_folder(shared);
        if (!profile || n_path_join(profile, "storage", roots[shared], STORAGE_PATH) < 0) return NULL;
        __atomic_store_n(&found[shared], 1, __ATOMIC_RELEASE);
    }
    return roots[shared];
}
#endif
#ifndef WEBSTORAGE_ROOT
#define WEBSTORAGE_ROOT storage_root(false)
#endif
#ifndef WEBSTORAGE_SHARED_ROOT
#define WEBSTORAGE_SHARED_ROOT storage_root(true)
#endif
/* What a page stores is bounded by its area's quota alone. A snapshot of a map within it fits
 * STORAGE_FILE_MAX: 8 bytes an entry, at most one entry without a UTF-16 unit (the empty key with
 * an empty value), at most 3 CESU-8 bytes a unit, an origin under 2048 bytes. So no snapshot size
 * refuses a write the quota allows. */
#define STORAGE_QUOTA_UNITS (WEB_STORAGE_QUOTA_BYTES / 2u)
#define STORAGE_FILE_MAX (56u + 2047u + 8u + 11u * STORAGE_QUOTA_UNITS)
/* A localStorage area no document of this window used for this long leaves RAM at the next sweep
 * (a new origin, a flush, a tab closing); its next use reads it again from the shared journal.
 * It keeps the window's RAM to what its tabs use, never bounds what a page stores. */
#define STORAGE_IDLE_MS 30000u

/* An area's map: its entries in the order key(n) counts (a doubly linked list), and an index by
 * key, so that no operation walks every entry of a map that has no count of keys. */
struct storage_entry {
    struct storage_entry *next, *prev;
    struct storage_entry *chain; /* the next entry of its index bucket */
    char *key, *value;
    size_t kn, vn, units;
    uint32_t hash;
};
struct storage_control {
    uint64_t serial, epoch, revision, end, checkpoint_revision, persisted;
    uint64_t generation, dirty_since, changed_at;
    int checkpoint_slot, slot;
};
struct storage_space;
struct storage_area {
    struct storage_area *next;
    struct storage_entry *entries, *tail;
    struct storage_entry **index, *walk; /* walk: the entry key(walk_at) last gave */
    size_t buckets;                      /* of index, a power of two, at least count */
    uint32_t walk_at;
    char *origin;
    struct storage_space *space; /* the tab of a sessionStorage area; NULL for localStorage */
    size_t units;
    unsigned count, sources; /* sources: the realms reading its events */
    bool touched;            /* used since the last sweep */
    uint64_t used_at;        /* when a sweep last saw it used */
    int kind, slot;
    uint64_t generation;
    bool unavailable; /* inactive snapshot invalidation failed; retry, RAM wins */
    bool dirty;
    uint64_t dirty_since, changed_at, retry_at;
    n_handle shared, shared_log; /* leased control and delta documents, or -1 */
    uint64_t shared_epoch, shared_cursor;
    bool shared_holder;
    /* Compared with freshly read shared-root bytes under the native lease.
     * Reusing validated headers never replaces observing other windows. */
    bool shared_control_cached;
    uint8_t shared_control_bytes[256];
    struct storage_control shared_control;
};
/* A tab's sessionStorage (one top-level traversable's session storage shed): its areas, and the
 * bytes of the storage events queued for its documents. Both go when the tab closes. */
struct storage_space {
    struct storage_space *next;
    struct storage_area *areas;
    uint64_t id;
    size_t event_bytes;
};
struct storage_source;
struct webstorage {
    struct storage_area *areas;   /* the localStorage areas in RAM */
    struct storage_space *spaces; /* the live tabs */
    struct storage_source *sources;
    uint64_t identity;
    n_handle clears, clears_queue; /* webstorage_watch's watch on the shared clears (or -1), its queue */
    uint64_t clears_seen;          /* bytes of the clears already applied */
};
static int storage_shared_flush(webstorage *, struct storage_area *, struct web_storage_result *);
static void storage_sources_free(webstorage *);

static char *storage_copy(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) { if (n) memcpy(p, s, n); p[n] = 0; }
    return p;
}
/* Strict CESU-8, not UTF-8 replacement: four-byte scalar sequences are not
 * canonical here because QuickJS emits their two UTF-16 surrogate sequences. */
static bool storage_units(const char *text, size_t len, size_t *out) {
    if (len && !text) return false;
    size_t units = 0;
    for (size_t i = 0; i < len; units++) {
        unsigned c = (unsigned char)text[i++];
        if (c < 128) continue;
        unsigned trailing = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : 0;
        if (!trailing || trailing > len - i) return false;
        unsigned second = (unsigned char)text[i];
        if (c == 0xE0 && second < 0xA0) return false;
        while (trailing--) { unsigned b = (unsigned char)text[i++]; if (b < 0x80 || b > 0xBF) return false; }
    }
    *out = units; return true;
}
/* An area's key: the serialization of an http(s) tuple origin (weburl.h's web_origin_http, the
 * URL record's owner), under 2048 bytes as an area's file holds it. */
static bool storage_origin(const char *s) {
    return s && strlen(s) < 2048 && web_origin_http(s);
}
static void storage_entry_free(struct storage_entry *e) {
    if (e) { free(e->key); free(e->value); free(e); }
}
static uint32_t storage_key_hash(const char *key, size_t kn) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < kn; i++) { h ^= (unsigned char)key[i]; h *= 16777619u; }
    return h;
}
static struct storage_entry *storage_find(struct storage_area *a, const char *key, size_t kn) {
    if (!a->buckets) return NULL;
    uint32_t h = storage_key_hash(key, kn);
    for (struct storage_entry *e = a->index[h & (a->buckets - 1)]; e; e = e->chain)
        if (e->hash == h && e->kn == kn && (!kn || !memcmp(e->key, key, kn))) return e;
    return NULL;
}
/* Room in the index of a for one more entry: it doubles once the entries fill it. */
static bool storage_index_room(struct storage_area *a) {
    if (a->count < a->buckets) return true;
    size_t buckets = a->buckets ? a->buckets * 2 : 16;
    struct storage_entry **index = buckets > a->buckets ? calloc(buckets, sizeof *index) : NULL;
    if (!index) return false;
    for (struct storage_entry *e = a->entries; e; e = e->next) { size_t at = e->hash & (buckets - 1); e->chain = index[at]; index[at] = e; }
    free(a->index); a->index = index; a->buckets = buckets;
    return true;
}
/* e at the end of the order of a (after storage_index_room), or in the place of old, the entry of
 * its key, which leaves the map. */
static void storage_link(struct storage_area *a, struct storage_entry *e, struct storage_entry *old) {
    e->hash = storage_key_hash(e->key, e->kn);
    struct storage_entry **bucket = &a->index[e->hash & (a->buckets - 1)];
    if (old) {
        e->prev = old->prev; e->next = old->next;
        if (e->prev) e->prev->next = e; else a->entries = e;
        if (e->next) e->next->prev = e; else a->tail = e;
        while (*bucket != old) bucket = &(*bucket)->chain;
        *bucket = e; e->chain = old->chain;
        if (a->walk == old) a->walk = e;
        old->next = old->prev = old->chain = NULL;
        return;
    }
    e->prev = a->tail; e->next = NULL;
    if (a->tail) a->tail->next = e; else a->entries = e;
    a->tail = e; e->chain = *bucket; *bucket = e; a->count++;
}
static void storage_unlink(struct storage_area *a, struct storage_entry *e) {
    if (e->prev) e->prev->next = e->next; else a->entries = e->next;
    if (e->next) e->next->prev = e->prev; else a->tail = e->prev;
    struct storage_entry **bucket = &a->index[e->hash & (a->buckets - 1)];
    while (*bucket != e) bucket = &(*bucket)->chain;
    *bucket = e->chain; e->next = e->prev = e->chain = NULL; a->count--; a->walk = NULL;
}
/* key(index): from the entry key last gave when that is not past index, as an enumeration asks. */
static struct storage_entry *storage_at(struct storage_area *a, uint32_t index) {
    if (index >= a->count) return NULL;
    struct storage_entry *e = a->entries; uint32_t at = 0;
    if (a->walk && a->walk_at <= index) { e = a->walk; at = a->walk_at; }
    while (at < index) { e = e->next; at++; }
    a->walk = e; a->walk_at = index;
    return e;
}
static void storage_map_clear(struct storage_area *a) {
    for (struct storage_entry *e = a->entries, *next; e; e = next) { next = e->next; storage_entry_free(e); }
    free(a->index);
    a->entries = a->tail = a->walk = NULL; a->index = NULL; a->buckets = 0; a->count = 0; a->units = 0;
}
/* The map of from becomes the map of a; from is left empty. */
static void storage_map_take(struct storage_area *a, struct storage_area *from) {
    storage_map_clear(a);
    a->entries = from->entries; a->tail = from->tail; a->index = from->index; a->buckets = from->buckets;
    a->count = from->count; a->units = from->units;
    from->entries = from->tail = from->walk = NULL; from->index = NULL; from->buckets = 0; from->count = 0; from->units = 0;
}
static void storage_area_free(struct storage_area *a) {
    if (a) { if(a->shared>=0)n_handle_close(a->shared);if(a->shared_log>=0)n_handle_close(a->shared_log); storage_map_clear(a); free(a->origin); free(a); }
}
static void storage_space_free(struct storage_space *p) {
    while (p->areas) { struct storage_area *a = p->areas; p->areas = a->next; storage_area_free(a); }
    free(p);
}
static uint32_t storage_serial;
webstorage *webstorage_create(void) {
    webstorage *s=calloc(1,sizeof *s);
    if(s){s->identity=(uint64_t)(uint32_t)n_app_self()<<32 | __atomic_add_fetch(&storage_serial,1,__ATOMIC_RELAXED);s->clears=-1;}
    return s;
}
void webstorage_free(webstorage *s) {
    if (!s) return;
    struct web_storage_result result;
    webstorage_flush(s, true, &result);
    storage_sources_free(s);
    while (s->areas) { struct storage_area *a = s->areas; s->areas = a->next; storage_area_free(a); }
    while (s->spaces) { struct storage_space *p = s->spaces; s->spaces = p->next; storage_space_free(p); }
    if (s->clears >= 0) n_handle_close(s->clears);
    free(s);
}
static struct storage_space *storage_space_find(webstorage *s, uint64_t id) {
    for (struct storage_space *p = id ? s->spaces : NULL; p; p = p->next) if (p->id == id) return p;
    return NULL;
}
/* Tab numbers ever given in this process, not the live tabs: unique across its stores, so a
 * number of another window's store names no tab of this one. */
static uint64_t storage_spaces_made;
uint64_t webstorage_session_open(webstorage *s) {
    if (!s) return 0;
    uint64_t id = __atomic_add_fetch(&storage_spaces_made, 1, __ATOMIC_RELAXED);
    if (!id) return 0; /* the count went round: no number is left that names no other tab */
    struct storage_space *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->id = id; p->next = s->spaces; s->spaces = p;
    return p->id;
}
/* Leaves out the localStorage areas no document of this window used for STORAGE_IDLE_MS: clean
 * ones (what they hold is in the shared journal and its checkpoint) that no realm reads events
 * from. Used now is what was touched since the last sweep. */
static void storage_sweep(webstorage *s) {
    uint64_t now = uptime_ms();
    for (struct storage_area **link = &s->areas; *link;) {
        struct storage_area *a = *link;
        if (a->touched) { a->touched = false; a->used_at = now; }
        if (!a->dirty && !a->sources && now - a->used_at >= STORAGE_IDLE_MS) { *link = a->next; storage_area_free(a); }
        else link = &a->next;
    }
}
/* Takes a, which no realm reads, out of RAM at once (it was read only for a measure or a clear). */
static void storage_area_drop(webstorage *s, struct storage_area *a) {
    if (a->sources) return;
    for (struct storage_area **link = &s->areas; *link; link = &(*link)->next)
        if (*link == a) { *link = a->next; storage_area_free(a); return; }
}
static void storage_hash(const void *data, size_t n, uint8_t out[32]) {
    br_sha256_context context;
    br_sha256_init(&context); br_sha256_update(&context, data, n); br_sha256_out(&context, out);
}
/* An empty path, which no operation accepts, when there is no root or the path does not fit. */
static void storage_path_at(const char *root,const char *origin, int slot, char path[STORAGE_PATH]) {
    uint8_t digest[32]; char name[65]; const char hex[] = "0123456789abcdef";
    storage_hash(origin, strlen(origin), digest);
    for (int i = 0; i < 32; i++) { name[2*i] = hex[digest[i] >> 4]; name[2*i+1] = hex[digest[i] & 15]; }
    name[64] = 0;
    if (!root || snprintf(path, STORAGE_PATH, "%s\\%s.%d", root, name, slot) >= STORAGE_PATH) path[0] = 0;
}
static uint32_t storage_u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t storage_u64(const uint8_t *p) { return storage_u32(p) | (uint64_t)storage_u32(p + 4) << 32; }
static void storage_put32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v >> (i*8)); }
static void storage_put64(uint8_t *p, uint64_t v) { storage_put32(p,(uint32_t)v); storage_put32(p+4,(uint32_t)(v>>32)); }

/* 0 absent, 1 valid, -1 invalid/I/O. An invalid existing store is never silently
 * reset to an empty map. A valid other slot can recover an interrupted write. */
static int storage_read_at(const char *root,const char *origin, int slot, struct storage_area **out) {
    char path[STORAGE_PATH]; storage_path_at(root,origin, slot, path);
    if (!path[0]) return -1;
    struct n_entry entry;
    n_result found = n_entry_info(path, &entry);
    if (found < 0) return found == -N_ERR_NOT_FOUND ? 0 : -1;
    if (entry.kind != N_KIND_DOCUMENT || entry.size < 56 || entry.size > STORAGE_FILE_MAX) return -1;
    size_t n = (size_t)entry.size;
    uint8_t *bytes = malloc(n);
    if (!bytes) return -1;
    n_handle h = n_document_open(path, N_DOCUMENT_READ);
    if (h < 0) { free(bytes); return -1; }
    n_result got = n_document_read_at(h, 0, bytes, n);
    n_handle_close(h);
    if (got != (n_result)n) { free(bytes); return -1; }
    uint8_t digest[32]; storage_hash(bytes + 40, n - 40, digest);
    if (memcmp(bytes,"NWEBST1",8) || memcmp(bytes+8,digest,32)) { free(bytes); return -1; }
    uint64_t generation = storage_u64(bytes + 40);
    uint32_t on = storage_u32(bytes + 48), count = storage_u32(bytes + 52);
    if (!generation || on != strlen(origin) || on > n - 56 || memcmp(bytes+56,origin,on)) { free(bytes); return -1; }
    struct storage_area *a = calloc(1, sizeof *a);
    if (!a) { free(bytes); return -1; }
    a->shared=a->shared_log=-1;
    a->origin = storage_copy(origin,on); a->kind=WEB_STORAGE_LOCAL; a->slot=slot; a->generation=generation;
    if (!a->origin) { storage_area_free(a); free(bytes); return -1; }
    size_t at=56+on; bool ok=true;
    for (uint32_t i=0;i<count;i++) {
        if (n-at<8) { ok=false; break; }
        size_t kn=storage_u32(bytes+at), vn=storage_u32(bytes+at+4), ku, vu; at+=8;
        if (kn>n-at || vn>n-at-kn || !storage_units((char *)bytes+at,kn,&ku) ||
            !storage_units((char *)bytes+at+kn,vn,&vu) || storage_find(a,(char *)bytes+at,kn) ||
            ku+vu>STORAGE_QUOTA_UNITS-a->units) { ok=false; break; }
        struct storage_entry *e=calloc(1,sizeof *e);
        if (!e) { ok=false; break; }
        e->kn=kn; e->vn=vn; e->units=ku+vu;
        e->key=storage_copy((char *)bytes+at,kn); e->value=storage_copy((char *)bytes+at+kn,vn);
        if (!e->key || !e->value || !storage_index_room(a)) { storage_entry_free(e); ok=false; break; }
        storage_link(a,e,NULL); a->units+=e->units; at+=kn+vn;
    }
    free(bytes);
    if (!ok || at!=n) { storage_area_free(a); return -1; }
    *out=a; return 1;
}
static int storage_read(const char *origin,int slot,struct storage_area **out){return storage_read_at(WEBSTORAGE_ROOT,origin,slot,out);}
static bool storage_directory(const char *path) {
    struct n_entry entry;
    n_result found=n_entry_info(path,&entry);
    if (found>=0) return entry.kind==N_KIND_FOLDER;
    if (found!=-N_ERR_NOT_FOUND) return false;
    n_result made=n_folder_create(path);
    if (made<0 && made!=-N_ERR_EXISTS) return false;
    return n_entry_info(path,&entry)>=0 && entry.kind==N_KIND_FOLDER;
}
static bool storage_dirs_at(const char *root) {
    /* The folder holding root is made when absent, never the ones above it. */
    char parent[STORAGE_PATH];
    return root && n_path_join(root, "..", parent, sizeof parent) >= 0 && storage_directory(parent) && storage_directory(root);
}
static int storage_save_at(const char *root,struct storage_area *a, struct web_storage_result *out) {
    if (a->generation==UINT64_MAX || !storage_dirs_at(root)) return WEB_STORAGE_IO;
    size_t on=strlen(a->origin), n=56+on;
    for (struct storage_entry *e=a->entries;e;e=e->next) n+=8+e->kn+e->vn;
    if (n>STORAGE_FILE_MAX) return WEB_STORAGE_QUOTA;
    uint8_t *bytes=malloc(n); if (!bytes) return WEB_STORAGE_QUOTA;
    out->snapshot_bytes+=n;
    memcpy(bytes,"NWEBST1",8); storage_put64(bytes+40,a->generation+1);
    storage_put32(bytes+48,(uint32_t)on); storage_put32(bytes+52,a->count); memcpy(bytes+56,a->origin,on);
    size_t at=56+on;
    for (struct storage_entry *e=a->entries;e;e=e->next) {
        storage_put32(bytes+at,(uint32_t)e->kn); storage_put32(bytes+at+4,(uint32_t)e->vn); at+=8;
        memcpy(bytes+at,e->key,e->kn); at+=e->kn; memcpy(bytes+at,e->value,e->vn); at+=e->vn;
    }
    storage_hash(bytes+40,n-40,bytes+8);
    int slot=a->slot==0?1:0;
    char path[STORAGE_PATH]; storage_path_at(root,a->origin,slot,path);
    /* The inactive slot is swapped in whole: a failed save leaves its older,
     * lower-generation snapshot, never a torn one, and the other completed
     * slot stays the recovery copy. Reopening and rehashing would add
     * synchronous disk I/O, but not durability. */
    int result=WEB_STORAGE_IO;
    if (n_document_save(path,bytes,n)>=0) { result=WEB_STORAGE_OK; a->generation++; a->slot=slot; }
    free(bytes); return result;
}
static int storage_save(struct storage_area *a,struct web_storage_result *out){return storage_save_at(WEBSTORAGE_ROOT,a,out);}
bool webstorage_pending(webstorage *s) {
    for (struct storage_area *a=s?s->areas:NULL;a;a=a->next) if(a->dirty)return true;
    return false;
}
int webstorage_flush(webstorage *s, bool force, struct web_storage_result *out) {
    struct web_storage_result ignored;
    if(!out)out=&ignored;
    memset(out,0,sizeof *out);
    if(!s)return WEB_STORAGE_IO;
    uint64_t now=uptime_ms(); int status=WEB_STORAGE_OK;
    for(struct storage_area *a=s->areas;a;a=a->next) {
        if(!a->dirty || (!force && (now<a->retry_at || (now-a->changed_at<100 && now-a->dirty_since<1000))))continue;
        out->save_attempts++;
        uint64_t start=uptime_ms();
        int result=storage_shared_flush(s,a,out);out->save_ms+=uptime_ms()-start;
        if(result==WEB_STORAGE_OK){a->dirty=false;a->unavailable=false;a->retry_at=0;}
        else {status=result;a->retry_at=now+1000;}
    }
    storage_sweep(s);
    return status;
}
/* The origin's localStorage area (space NULL), or its sessionStorage area in the tab space. */
static struct storage_area *storage_area_find(webstorage *s, struct storage_space *space, const char *origin) {
    for (struct storage_area *a=space?space->areas:s->areas;a;a=a->next) if (!strcmp(a->origin,origin)) return a;
    return NULL;
}
static int storage_get_area(webstorage *s, struct storage_space *space, const char *origin, struct storage_area **out) {
    struct storage_area *a=storage_area_find(s,space,origin);
    if (a) { *out=a; return WEB_STORAGE_OK; }
    if (!space) storage_sweep(s);
    a=calloc(1,sizeof *a); if (!a) return WEB_STORAGE_QUOTA;
    a->shared=a->shared_log=-1;
    a->origin=storage_copy(origin,strlen(origin)); a->kind=space?WEB_STORAGE_SESSION:WEB_STORAGE_LOCAL; a->slot=-1; a->space=space;
    if (!a->origin) { storage_area_free(a); return WEB_STORAGE_QUOTA; }
    struct storage_area **list=space?&space->areas:&s->areas;
    a->next=*list; *list=a; *out=a; return WEB_STORAGE_OK;
}
/* A read, or a mutation that a shared journal record repeats, on a's RAM map. */
static int storage_area_apply(struct storage_area *a, const struct web_storage_request *r, struct web_storage_result *out) {
    memset(out,0,sizeof *out);
    if (r->operation<WEB_STORAGE_LENGTH || r->operation>WEB_STORAGE_CHECK) return WEB_STORAGE_IO;
    size_t ku=0,vu=0;
    if (r->operation==WEB_STORAGE_GET || r->operation==WEB_STORAGE_SET || r->operation==WEB_STORAGE_REMOVE) {
        if (r->key_len>STORAGE_FILE_MAX || !storage_units(r->key,r->key_len,&ku)) return WEB_STORAGE_QUOTA;
    }
    if (r->operation==WEB_STORAGE_SET && (r->value_len>STORAGE_FILE_MAX || !storage_units(r->value,r->value_len,&vu))) return WEB_STORAGE_QUOTA;
    if (r->operation==WEB_STORAGE_CHECK) return WEB_STORAGE_OK;
    if (r->operation==WEB_STORAGE_LENGTH) { out->length=a->count; return WEB_STORAGE_OK; }
    struct storage_entry *e=r->operation==WEB_STORAGE_KEY ? storage_at(a,r->index) : storage_find(a,r->key,r->key_len);
    if (r->operation==WEB_STORAGE_KEY || r->operation==WEB_STORAGE_GET) {
        if (e) {
            out->text_len=r->operation==WEB_STORAGE_KEY?e->kn:e->vn;
            out->text=storage_copy(r->operation==WEB_STORAGE_KEY?e->key:e->value,out->text_len);
            if (!out->text) return WEB_STORAGE_QUOTA;
        }
        return WEB_STORAGE_OK;
    }
    if ((r->operation==WEB_STORAGE_REMOVE && !e) || (r->operation==WEB_STORAGE_CLEAR && !a->count) ||
        (r->operation==WEB_STORAGE_SET && e && e->vn==r->value_len && (!e->vn || !memcmp(e->value,r->value,e->vn)))) return WEB_STORAGE_OK;
    size_t newunits=r->operation==WEB_STORAGE_CLEAR?0:a->units-(e?e->units:0)+(r->operation==WEB_STORAGE_SET?ku+vu:0);
    if (newunits>STORAGE_QUOTA_UNITS) return WEB_STORAGE_QUOTA;
    if (r->operation==WEB_STORAGE_CLEAR) storage_map_clear(a);
    else if (r->operation==WEB_STORAGE_REMOVE) { storage_unlink(a,e); storage_entry_free(e); }
    else {
        char *value=storage_copy(r->value,r->value_len);
        if (!value) return WEB_STORAGE_QUOTA;
        if (!e) {
            e=calloc(1,sizeof *e);
            if (!e || !(e->key=storage_copy(r->key,r->key_len)) || !storage_index_room(a)) { storage_entry_free(e); free(value); return WEB_STORAGE_QUOTA; }
            e->kn=r->key_len; storage_link(a,e,NULL);
        }
        free(e->value); e->value=value; e->vn=r->value_len; e->units=ku+vu;
    }
    a->units=newunits;
    if(a->kind==WEB_STORAGE_LOCAL){uint64_t now=uptime_ms();if(!a->dirty)a->dirty_since=now;a->changed_at=now;a->dirty=true;}
    return WEB_STORAGE_OK;
}
#include "webstorage_shared.h"
