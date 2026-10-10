/* Nocturne browser storage. This uses the existing native file API; no new
 * filesystem/ABI/POSIX requirement. Alternate checksum-validated snapshots
 * write only the inactive slot and keep the previous confirmed slot intact.
 * A complete write and successful close publish the inactive snapshot; checksum
 * validation happens once when opening a store, not after every write. No
 * copy-and-unlink rename is used. fsync/power-loss durability and cross-process
 * cache coherence are not provided by this backend. */
#include <webstorage.h>
#include <nocturne.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bearssl_hash.h>

/* Only native regression translation units override this constant. */
#ifndef WEBSTORAGE_ROOT
#define WEBSTORAGE_ROOT "/data/browser/storage"
#endif
#define STORAGE_MAX_ORIGINS 64u
#define STORAGE_MAX_KEYS 4096u
#define STORAGE_WINDOW_BYTES (20u * 1024u * 1024u)
#define STORAGE_FILE_MAX (8u * 1024u * 1024u)

struct storage_entry {
    struct storage_entry *next;
    char *key, *value;
    size_t kn, vn, units;
};
struct storage_area {
    struct storage_area *next;
    struct storage_entry *entries;
    char *origin;
    size_t units;
    unsigned count;
    int kind, slot;
    uint64_t generation;
    bool unavailable; /* inactive snapshot invalidation failed; retry, RAM wins */
    bool dirty;
    uint64_t dirty_since, changed_at, retry_at;
};
struct webstorage { struct storage_area *areas; unsigned count; size_t units; };

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
static bool storage_origin(const char *s) {
    /* Trust the private WHATWG parser for canonicalization; independently reject
     * opaque origins, path/userinfo/query fragments and all filesystem syntax. */
    if (!s || strlen(s) > 2047) return false;
    const char *h = !strncmp(s, "https://", 8) ? s + 8 : !strncmp(s, "http://", 7) ? s + 7 : NULL;
    if (!h || !*h) return false;
    for (const unsigned char *p = (const unsigned char *)h; *p; p++)
        if (*p <= 32 || *p >= 127 || strchr("/\\@?#%", *p)) return false;
    return true;
}
static void storage_entries_free(struct storage_entry *e) {
    while (e) { struct storage_entry *next = e->next; free(e->key); free(e->value); free(e); e = next; }
}
static void storage_area_free(struct storage_area *a) {
    if (a) { storage_entries_free(a->entries); free(a->origin); free(a); }
}
webstorage *webstorage_create(void) { return calloc(1, sizeof(webstorage)); }
void webstorage_free(webstorage *s) {
    if (!s) return;
    struct web_storage_result result;
    webstorage_flush(s, true, &result);
    while (s->areas) { struct storage_area *a = s->areas; s->areas = a->next; storage_area_free(a); }
    free(s);
}
static struct storage_entry *storage_find(struct storage_area *a, const char *key, size_t kn) {
    for (struct storage_entry *e = a->entries; e; e = e->next)
        if (e->kn == kn && (!kn || !memcmp(e->key, key, kn))) return e;
    return NULL;
}
static void storage_hash(const void *data, size_t n, uint8_t out[32]) {
    br_sha256_context context;
    br_sha256_init(&context); br_sha256_update(&context, data, n); br_sha256_out(&context, out);
}
static void storage_path(const char *origin, int slot, char path[160]) {
    uint8_t digest[32]; char name[65]; const char hex[] = "0123456789abcdef";
    storage_hash(origin, strlen(origin), digest);
    for (int i = 0; i < 32; i++) { name[2*i] = hex[digest[i] >> 4]; name[2*i+1] = hex[digest[i] & 15]; }
    name[64] = 0;
    snprintf(path, 160, "%s/%s.%d", WEBSTORAGE_ROOT, name, slot);
}
static uint32_t storage_u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t storage_u64(const uint8_t *p) { return storage_u32(p) | (uint64_t)storage_u32(p + 4) << 32; }
static void storage_put32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v >> (i*8)); }
static void storage_put64(uint8_t *p, uint64_t v) { storage_put32(p,(uint32_t)v); storage_put32(p+4,(uint32_t)(v>>32)); }

/* 0 absent, 1 valid, -1 invalid/I/O. An invalid existing store is never silently
 * reset to an empty map. A valid other slot can recover an interrupted write. */
static int storage_read(const char *origin, int slot, struct storage_area **out) {
    char path[160]; storage_path(origin, slot, path);
    struct n_stat st;
    if (stat(path, &st) < 0) return errno == ENOENT ? 0 : -1;
    if (st.type != 1 || st.size < 56 || st.size > STORAGE_FILE_MAX) return -1;
    size_t n = (size_t)st.size, done = 0;
    uint8_t *bytes = malloc(n);
    if (!bytes) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { free(bytes); return -1; }
    while (done < n) {
        ssize_t nr = read(fd, bytes + done, n - done);
        if (nr < 0 && errno == EINTR) continue;
        if (nr <= 0 || (size_t)nr > n - done) break;
        done += (size_t)nr;
    }
    int closed = close(fd);
    if (done != n || closed < 0) { free(bytes); return -1; }
    uint8_t digest[32]; storage_hash(bytes + 40, n - 40, digest);
    if (memcmp(bytes,"NWEBST1",8) || memcmp(bytes+8,digest,32)) { free(bytes); return -1; }
    uint64_t generation = storage_u64(bytes + 40);
    uint32_t on = storage_u32(bytes + 48), count = storage_u32(bytes + 52);
    if (!generation || on != strlen(origin) || on > n - 56 || count > STORAGE_MAX_KEYS || memcmp(bytes+56,origin,on)) { free(bytes); return -1; }
    struct storage_area *a = calloc(1, sizeof *a);
    if (!a) { free(bytes); return -1; }
    a->origin = storage_copy(origin,on); a->kind=WEB_STORAGE_LOCAL; a->slot=slot; a->generation=generation;
    if (!a->origin) { storage_area_free(a); free(bytes); return -1; }
    size_t at=56+on; struct storage_entry **tail=&a->entries; bool ok=true;
    for (uint32_t i=0;i<count;i++) {
        if (n-at<8) { ok=false; break; }
        size_t kn=storage_u32(bytes+at), vn=storage_u32(bytes+at+4), ku, vu; at+=8;
        if (kn>n-at || vn>n-at-kn || !storage_units((char *)bytes+at,kn,&ku) ||
            !storage_units((char *)bytes+at+kn,vn,&vu) || storage_find(a,(char *)bytes+at,kn) ||
            ku+vu>WEB_STORAGE_QUOTA_BYTES/2-a->units) { ok=false; break; }
        struct storage_entry *e=calloc(1,sizeof *e);
        if (!e) { ok=false; break; }
        e->kn=kn; e->vn=vn; e->units=ku+vu;
        e->key=storage_copy((char *)bytes+at,kn); e->value=storage_copy((char *)bytes+at+kn,vn);
        if (!e->key || !e->value) { storage_entries_free(e); ok=false; break; }
        *tail=e; tail=&e->next; a->units+=e->units; a->count++; at+=kn+vn;
    }
    free(bytes);
    if (!ok || at!=n) { storage_area_free(a); return -1; }
    *out=a; return 1;
}
static bool storage_directory(const char *path) {
    struct n_stat st;
    if (stat(path,&st)==0) return st.type==2;
    if (errno!=ENOENT) return false;
    if (mkdir(path)<0 && errno!=EEXIST) return false;
    return stat(path,&st)==0 && st.type==2;
}
static bool storage_dirs(void) {
    /* Paths are native constants only. Do not create /data itself when absent. */
    char parent[160]; snprintf(parent,sizeof parent,"%s",WEBSTORAGE_ROOT);
    char *slash=strrchr(parent,'/'); if (!slash || slash==parent) return false; *slash=0;
    return storage_directory(parent) && storage_directory(WEBSTORAGE_ROOT);
}
static int storage_save(struct storage_area *a, struct web_storage_result *out) {
    if (a->generation==UINT64_MAX || !storage_dirs()) return WEB_STORAGE_IO;
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
    char path[160]; storage_path(a->origin,slot,path);
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC), result=WEB_STORAGE_IO;
    if (fd>=0) {
        size_t done=0;
        while (done<n) {
            ssize_t nw=write(fd,bytes+done,n-done);
            if (nw<0 && errno==EINTR) continue;
            if (nw<=0 || (size_t)nw>n-done) break;
            done+=(size_t)nw;
        }
        int closed=close(fd);
        if (done==n && closed==0) {
            /* The native API reports short writes and close failure. Reopening
             * and rehashing this exact snapshot adds synchronous storvsc I/O,
             * but not durability. Keep the other completed slot for recovery. */
            result=WEB_STORAGE_OK; a->generation++; a->slot=slot;
        }
    }
    if (result!=WEB_STORAGE_OK) {
        /* Even a failed open may have truncated the candidate before failing
         * to allocate a file handle. Remove only this inactive slot on every
         * unsuccessful write/close. Uncertain invalidation makes the
         * area unavailable; never report success or silently serve stale RAM. */
        if (unlink(path)<0 && errno!=ENOENT) a->unavailable=true;
    }
    free(bytes); return result;
}
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
        int result=storage_save(a,out);out->save_ms+=uptime_ms()-start;
        if(result==WEB_STORAGE_OK){a->dirty=false;a->unavailable=false;a->retry_at=0;}
        else {status=result;a->retry_at=now+1000;}
    }
    return status;
}
static int storage_get_area(webstorage *s, const char *origin, int kind, struct storage_area **out) {
    for (struct storage_area *a=s->areas;a;a=a->next)
        if (a->kind==kind && !strcmp(a->origin,origin)) {
            *out=a; return WEB_STORAGE_OK;
        }
    if (s->count>=STORAGE_MAX_ORIGINS) return WEB_STORAGE_QUOTA;
    struct storage_area *a=NULL;
    if (kind==WEB_STORAGE_LOCAL) {
        struct storage_area *b=NULL; int ar=storage_read(origin,0,&a), br=storage_read(origin,1,&b);
        if (b && (!a || b->generation>a->generation)) { storage_area_free(a); a=b; b=NULL; }
        storage_area_free(b);
        if (!a && (ar<0 || br<0)) return WEB_STORAGE_IO;
    }
    if (!a) {
        a=calloc(1,sizeof *a); if (!a) return WEB_STORAGE_QUOTA;
        a->origin=storage_copy(origin,strlen(origin)); a->kind=kind; a->slot=-1;
        if (!a->origin) { storage_area_free(a); return WEB_STORAGE_QUOTA; }
    }
    if (a->units>STORAGE_WINDOW_BYTES/2-s->units) { storage_area_free(a); return WEB_STORAGE_QUOTA; }
    a->next=s->areas; s->areas=a; s->count++; s->units+=a->units; *out=a; return WEB_STORAGE_OK;
}
int webstorage_access(webstorage *s, const char *origin, const struct web_storage_request *r, struct web_storage_result *out) {
    if (!out) return WEB_STORAGE_IO;
    memset(out,0,sizeof *out);
    if (!s || !r || !storage_origin(origin) || (r->kind!=WEB_STORAGE_LOCAL && r->kind!=WEB_STORAGE_SESSION)) return WEB_STORAGE_SECURITY;
    if (r->operation<WEB_STORAGE_LENGTH || r->operation>WEB_STORAGE_CHECK) return WEB_STORAGE_IO;
    size_t ku=0,vu=0;
    if (r->operation==WEB_STORAGE_GET || r->operation==WEB_STORAGE_SET || r->operation==WEB_STORAGE_REMOVE) {
        if (r->key_len>STORAGE_FILE_MAX || !storage_units(r->key,r->key_len,&ku)) return WEB_STORAGE_QUOTA;
    }
    if (r->operation==WEB_STORAGE_SET && (r->value_len>STORAGE_FILE_MAX || !storage_units(r->value,r->value_len,&vu))) return WEB_STORAGE_QUOTA;
    struct storage_area *a=NULL; int result=storage_get_area(s,origin,r->kind,&a);
    if (result!=WEB_STORAGE_OK) return result;
    if (r->operation==WEB_STORAGE_CHECK) return WEB_STORAGE_OK;
    if (r->operation==WEB_STORAGE_LENGTH) { out->length=a->count; return WEB_STORAGE_OK; }
    struct storage_entry *e=r->operation==WEB_STORAGE_KEY ? a->entries : storage_find(a,r->key,r->key_len);
    if (r->operation==WEB_STORAGE_KEY || r->operation==WEB_STORAGE_GET) {
        if (r->operation==WEB_STORAGE_KEY) for (uint32_t i=0;e && i<r->index;i++) e=e->next;
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
    if (newunits>WEB_STORAGE_QUOTA_BYTES/2 || newunits>STORAGE_WINDOW_BYTES/2-(s->units-a->units) ||
        (r->operation==WEB_STORAGE_SET && !e && a->count>=STORAGE_MAX_KEYS)) return WEB_STORAGE_QUOTA;
    struct storage_entry **link=&a->entries;
    while (*link && !((*link)->kn==r->key_len && (!r->key_len || !memcmp((*link)->key,r->key,r->key_len)))) link=&(*link)->next;
    if (r->operation==WEB_STORAGE_CLEAR) { storage_entries_free(a->entries); a->entries=NULL; a->count=0; }
    else if (r->operation==WEB_STORAGE_REMOVE) { struct storage_entry *old=*link; *link=old->next; old->next=NULL; storage_entries_free(old); a->count--; }
    else {
        char *value=storage_copy(r->value,r->value_len);
        if (!value) return WEB_STORAGE_QUOTA;
        if (!*link) {
            struct storage_entry *f=calloc(1,sizeof *f);
            if (!f || !(f->key=storage_copy(r->key,r->key_len))) { free(f); free(value); return WEB_STORAGE_QUOTA; }
            f->kn=r->key_len; *link=f; a->count++;
        }
        free((*link)->value); (*link)->value=value; (*link)->vn=r->value_len; (*link)->units=ku+vu;
    }
    s->units=s->units-a->units+newunits;
    a->units=newunits;
    if(a->kind==WEB_STORAGE_LOCAL){uint64_t now=uptime_ms();if(!a->dirty)a->dirty_since=now;a->changed_at=now;a->dirty=true;}
    return WEB_STORAGE_OK;
}
