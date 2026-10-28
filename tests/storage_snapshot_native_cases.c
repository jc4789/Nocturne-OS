/* Host support test: the actual backend (user/libc/webstorage.c, with the real nocturne.h) over
 * real isolated host files, through an adapter of the native document API it calls. Only native
 * API failures are injected. This is not real-site acceptance.
 *
 * What the adapter imitates (kernel/src/fs/document.c, user/include/nocturne.h): an EDIT opening
 * reads and writes the document itself; n_document_save replaces the whole content or, failing,
 * leaves the old one; n_document_append adds a whole record; claims are per document and one
 * process here never waits on itself, so a second claim of a claimed document is reported as the
 * deadlock it would be. The clock is the test's: one ms a call, and tests move it forward. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <direct.h>
#include <io.h>
#include <nocturne.h>
#include <webstorage.h>

enum { F_NONE, F_SAVE, F_REMOVE };
static int fault, writes, reads, removes, checks, failures;
static int shared_reads; /* reads of the shared root's documents (control, delta, checkpoints) */
static size_t bytes_written, bytes_read;
static uint64_t clock_ms = 1;
static unsigned watch_changes; /* changes of the watched clears not yet taken */

#define ROOT "build/goal-20261009/storage-native-fault"
#define SHARED "build/goal-20261009/storage-native-shared"
static bool persistent(const char *path) { return !strncmp(path, ROOT, strlen(ROOT)); }

enum { H_FREE, H_DOCUMENT, H_WATCH, H_QUEUE };
static struct { int kind; FILE *f; char path[1024]; bool claimed, edit; } handles[64];
static n_handle handle_new(int kind) {
    for (int i = 0; i < 64; i++) if (handles[i].kind == H_FREE) { memset(&handles[i], 0, sizeof handles[i]); handles[i].kind = kind; return i; }
    return -N_ERR_NO_HANDLE;
}
static bool handle_is(n_handle h, int kind) { return h >= 0 && h < 64 && handles[h].kind == kind; }

uint64_t uptime_ms(void) { return clock_ms++; }
uint64_t n_clock_uptime_ms(void) { return clock_ms++; }
n_result n_app_self(void) { return 42; }
n_result proclist(struct n_procinfo *out, int max) {
    if (max < 1) return 0;
    memset(out, 0, sizeof *out); out->pid = 42; out->start_ms = 1; out->state = 1;
    return 1;
}
const char *n_temp_folder(void) { return "build/goal-20261009"; }
n_result n_app_data_folder(const char *name, char *out, size_t cap) {
    return snprintf(out, cap, "build/goal-20261009/%s", name) < (int)cap ? 0 : -N_ERR_TOO_LARGE;
}
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) {
    if (!strcmp(name, "..")) {
        const char *cut = NULL;
        for (const char *p = folder; *p; p++) if (*p == '/' || *p == '\\') cut = p;
        if (!cut || (size_t)(cut - folder) >= cap) return -N_ERR_INVALID;
        memcpy(out, folder, (size_t)(cut - folder)); out[cut - folder] = 0;
        return (n_result)(cut - folder);
    }
    int n = snprintf(out, cap, "%s\\%s", folder, name);
    return n >= 0 && (size_t)n < cap ? n : -N_ERR_TOO_LARGE;
}
static n_result host_error(void) { return errno == ENOENT ? -N_ERR_NOT_FOUND : errno == EEXIST ? -N_ERR_EXISTS : -N_ERR_IO; }
n_result n_entry_info(const char *path, struct n_entry *out) {
    struct _stat64 st;
    if (_stat64(path, &st) < 0) return host_error();
    memset(out, 0, sizeof *out);
    out->kind = (st.st_mode & _S_IFDIR) ? N_KIND_FOLDER : N_KIND_DOCUMENT; out->size = (uint64_t)st.st_size;
    return 0;
}
n_result n_folder_create(const char *path) { return _mkdir(path) < 0 ? host_error() : 0; }
n_result n_entry_remove(const char *path) {
    if (persistent(path)) { removes++; if (fault == F_REMOVE) return -N_ERR_IO; }
    return _unlink(path) < 0 ? host_error() : 0;
}
n_result n_folder_entries(const char *path, struct n_entry **out) {
    char pattern[1100]; snprintf(pattern, sizeof pattern, "%s\\*", path);
    struct _finddata64i32_t found; intptr_t find = _findfirst64i32(pattern, &found);
    *out = NULL;
    if (find == -1) { struct n_entry e; return n_entry_info(path, &e) < 0 ? -N_ERR_NOT_FOUND : 0; }
    size_t count = 0, cap = 0; struct n_entry *list = NULL;
    do {
        if (!strcmp(found.name, ".") || !strcmp(found.name, "..")) continue;
        if (count == cap) { cap = cap ? cap * 2 : 16; list = realloc(list, cap * sizeof *list); if (!list) { _findclose(find); return -N_ERR_NO_MEMORY; } }
        memset(&list[count], 0, sizeof list[count]);
        snprintf(list[count].name, sizeof list[count].name, "%s", found.name);
        list[count].kind = (found.attrib & _A_SUBDIR) ? N_KIND_FOLDER : N_KIND_DOCUMENT; list[count].size = (uint64_t)found.size;
        count++;
    } while (_findnext64i32(find, &found) == 0);
    _findclose(find); *out = list; return (n_result)count;
}
n_handle n_document_open(const char *path, int access) {
    struct _stat64 st; bool exists = _stat64(path, &st) == 0;
    if (access == N_DOCUMENT_CREATE && exists) return -N_ERR_EXISTS;
    if ((access == N_DOCUMENT_READ || access == N_DOCUMENT_EDIT) && !exists) return -N_ERR_NOT_FOUND;
    FILE *f = fopen(path, access == N_DOCUMENT_READ ? "rb" : access == N_DOCUMENT_EDIT ? "r+b" : "w+b");
    if (!f) return host_error();
    setvbuf(f, NULL, _IONBF, 0); /* a native write is in the document when it returns */
    n_handle h = handle_new(H_DOCUMENT);
    if (h < 0) { fclose(f); return h; }
    handles[h].f = f; handles[h].edit = access != N_DOCUMENT_READ; snprintf(handles[h].path, sizeof handles[h].path, "%s", path);
    return h;
}
n_result n_document_read_at(n_handle h, uint64_t offset, void *buf, size_t n) {
    if (!handle_is(h, H_DOCUMENT)) return -N_ERR_NO_HANDLE;
    if (persistent(handles[h].path)) reads++; else shared_reads++;
    if (_fseeki64(handles[h].f, (int64_t)offset, SEEK_SET)) return -N_ERR_IO;
    size_t done = fread(buf, 1, n, handles[h].f);
    if (persistent(handles[h].path)) bytes_read += done;
    return done == 0 && ferror(handles[h].f) ? -N_ERR_IO : (n_result)done;
}
n_result n_document_write_at(n_handle h, uint64_t offset, const void *buf, size_t n) {
    if (!handle_is(h, H_DOCUMENT) || !handles[h].edit) return -N_ERR_WRONG_KIND;
    if (_fseeki64(handles[h].f, (int64_t)offset, SEEK_SET)) return -N_ERR_IO;
    size_t done = fwrite(buf, 1, n, handles[h].f);
    return done == 0 && n ? -N_ERR_IO : (n_result)done;
}
n_result n_document_size(n_handle h) {
    if (!handle_is(h, H_DOCUMENT)) return -N_ERR_NO_HANDLE;
    struct _stat64 st; return _fstat64(_fileno(handles[h].f), &st) < 0 ? -N_ERR_IO : (n_result)st.st_size;
}
n_result n_document_set_size(n_handle h, uint64_t size) {
    if (!handle_is(h, H_DOCUMENT) || !handles[h].edit) return -N_ERR_WRONG_KIND;
    return _chsize_s(_fileno(handles[h].f), (int64_t)size) ? -N_ERR_IO : 0;
}
static void check_failed(const char *name);
n_result n_document_claim(n_handle h, bool wait) {
    if (!handle_is(h, H_DOCUMENT)) return -N_ERR_NO_HANDLE;
    for (int i = 0; i < 64; i++)
        if (handles[i].kind == H_DOCUMENT && handles[i].claimed && !strcmp(handles[i].path, handles[h].path)) {
            if (wait) check_failed("lease-claimed-twice-would-deadlock");
            return -N_ERR_NOT_READY;
        }
    handles[h].claimed = true; return 0;
}
n_result n_document_release(n_handle h) {
    if (!handle_is(h, H_DOCUMENT) || !handles[h].claimed) return -N_ERR_INVALID;
    handles[h].claimed = false; return 0;
}
n_result n_document_save(const char *path, const void *data, size_t size) {
    bool disk = persistent(path);
    if (disk) { writes++; if (fault == F_SAVE) return -N_ERR_IO; }
    FILE *f = fopen(path, "wb");
    if (!f) return host_error();
    size_t done = fwrite(data, 1, size, f); fclose(f);
    if (disk) bytes_written += done;
    return done == size ? 0 : -N_ERR_IO;
}
n_result n_document_append(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "ab");
    if (!f) return host_error();
    size_t done = fwrite(data, 1, size, f); fclose(f);
    if (strstr(path, "clears")) watch_changes++;
    return done == size ? (n_result)size : -N_ERR_IO;
}
n_handle n_entry_watch(const char *path, uint32_t what) { (void)path; (void)what; return handle_new(H_WATCH); }
n_result n_watch_take(n_handle h) {
    if (!handle_is(h, H_WATCH)) return -N_ERR_NO_HANDLE;
    n_result n = watch_changes; watch_changes = 0; return n;
}
n_handle n_events_create(void) { return handle_new(H_QUEUE); }
n_result n_events_watch(n_handle q, n_handle h, uint32_t interest, uint64_t token) {
    (void)interest; (void)token;
    return handle_is(q, H_QUEUE) && h >= 0 && h < 64 && handles[h].kind != H_FREE ? 0 : -N_ERR_NO_HANDLE;
}
n_result n_events_unwatch(n_handle q, uint64_t token) { (void)q; (void)token; return 1; }
n_result n_handle_close(n_handle h) {
    if (h < 0 || h >= 64 || handles[h].kind == H_FREE) return -N_ERR_NO_HANDLE;
    if (handles[h].f) fclose(handles[h].f);
    handles[h].kind = H_FREE; handles[h].f = NULL; handles[h].claimed = false;
    return 0;
}

#define WEBSTORAGE_ROOT ROOT
#define WEBSTORAGE_SHARED_ROOT SHARED
#include "../user/libc/webstorage.c"

static void check_failed(const char *name) { checks++; failures++; printf("FAIL %s\n", name); }
static void verify(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static void reset(int value){fault=value;writes=reads=removes=0;bytes_written=bytes_read=0;}
static void storage_path(const char *origin,int slot,char path[STORAGE_PATH]){storage_path_at(WEBSTORAGE_ROOT,origin,slot,path);}
static void cold_shared(const char *origin){char path[STORAGE_PATH];for(int i=0;i<5;i++){storage_path_at(WEBSTORAGE_SHARED_ROOT,origin,i,path);_unlink(path);}}
static void cold_all(const char *origin){char path[STORAGE_PATH];cold_shared(origin);for(int i=0;i<2;i++){storage_path(origin,i,path);_unlink(path);}}
/* No claim may outlive the operation that took it. */
static bool no_claims(void){for(int i=0;i<64;i++)if(handles[i].kind==H_DOCUMENT&&handles[i].claimed)return false;return true;}
static bool disk_value_is(const char *origin,int slot,const char *key,const char *value){
    struct storage_area *a=NULL;if(storage_read(origin,slot,&a)!=1)return false;
    struct storage_entry *e=storage_find(a,key,strlen(key));bool ok=e&&e->vn==strlen(value)&&!memcmp(e->value,value,e->vn);storage_area_free(a);return ok;
}
static int storage_test_access(webstorage *s,const char *origin,int operation,const char *key,const char *value,struct web_storage_result *out){
    struct web_storage_request request={.kind=WEB_STORAGE_LOCAL,.operation=operation,.key=key,.key_len=key?strlen(key):0,.value=value,.value_len=value?strlen(value):0};
    return webstorage_access(s,0,origin,&request,out);
}
static bool value_is(webstorage *s,const char *origin,const char *key,const char *value){
    struct web_storage_result out;int r=storage_test_access(s,origin,WEB_STORAGE_GET,key,NULL,&out);
    bool ok=r==WEB_STORAGE_OK&&(value?out.text&&out.text_len==strlen(value)&&!memcmp(out.text,value,out.text_len):!out.text);free(out.text);return ok;
}
static unsigned char *file_bytes(const char *path,size_t *len){
    FILE *f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    unsigned char *p=n>=0?malloc((size_t)n+1):NULL;if(p&&fread(p,1,(size_t)n,f)==(size_t)n)*len=(size_t)n;else{free(p);p=NULL;}fclose(f);return p;
}
static void flip_byte(const char *path,long at){
    FILE *f=fopen(path,"r+b");if(!f)return;fseek(f,at,SEEK_SET);int c=fgetc(f);fseek(f,at,SEEK_SET);fputc(c^1,f);fclose(f);
}
static void folders(void){_mkdir("build");_mkdir("build/goal-20261009");_mkdir(ROOT);_mkdir(SHARED);}
/* A save that fails leaves the last completed slot and the RAM map; a retry publishes once. */
static void exercise(int mode){
    char origin[96],confirmed[STORAGE_PATH],candidate[STORAGE_PATH];snprintf(origin,sizeof origin,"https://storage-fault-%d.invalid",mode);
    cold_all(origin);
    webstorage *s=webstorage_create();struct web_storage_result out;reset(F_NONE);
    verify(storage_test_access(s,origin,WEB_STORAGE_SET,"first","old",&out)==WEB_STORAGE_OK,"initial-publication");
    verify(webstorage_flush(s,true,&out)==WEB_STORAGE_OK,"initial-flush");
    verify(storage_test_access(s,origin,WEB_STORAGE_SET,"second","kept",&out)==WEB_STORAGE_OK,"second-publication");
    verify(webstorage_flush(s,true,&out)==WEB_STORAGE_OK,"second-flush");
    int slot=s->areas->slot;uint64_t generation=s->areas->generation;
    storage_path(origin,slot,confirmed);storage_path(origin,1-slot,candidate);
    size_t oldn=0;unsigned char *old=file_bytes(confirmed,&oldn);verify(old!=NULL,"confirmed-readable");
    reset(mode);verify(storage_test_access(s,origin,WEB_STORAGE_SET,"first","new",&out)==WEB_STORAGE_OK,"ram-publication");
    int r=webstorage_flush(s,true,&out);
    bool success=mode==F_NONE;
    verify(r==(success?WEB_STORAGE_OK:WEB_STORAGE_IO),"publication-result");
    if(success){
        verify(s->areas->generation==generation+1&&s->areas->slot==1-slot,"generation-adopted-once");
        verify(bytes_written==out.snapshot_bytes&&bytes_read==0&&!removes,"one-write-no-readback-no-remove");
    }else{
        verify(s->areas->generation==generation&&s->areas->slot==slot&&webstorage_pending(s),"failed-generation-unpublished");
    }
    size_t currentn=0;unsigned char *current=file_bytes(confirmed,&currentn);
    verify(old&&current&&oldn==currentn&&!memcmp(old,current,oldn),"last-confirmed-slot-untouched");free(old);free(current);
    reset(F_NONE);
    verify(value_is(s,origin,"first","new")&&value_is(s,origin,"second","kept"),"ram-map-retained-for-retry");
    struct web_storage_request key={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_KEY,.index=0};
    verify(webstorage_access(s,0,origin,&key,&out)==WEB_STORAGE_OK&&out.text&&!strcmp(out.text,"first"),"key-order-preserved");free(out.text);
    verify(disk_value_is(origin,s->areas->slot,"first",success?"new":"old"),"disk-keeps-last-completed-slot");
    if(!success)verify(webstorage_flush(s,true,&out)==WEB_STORAGE_OK&&!webstorage_pending(s),"failed-commit-can-retry");
    webstorage_free(s);cold_shared(origin);s=webstorage_create();
    verify(value_is(s,origin,"first","new")&&value_is(s,origin,"second","kept"),"reopen-recovers-completed-retry");
    webstorage_free(s);cold_all(origin);verify(no_claims(),"no-claim-outlives-its-operation");
}
/* An invalid newer slot gives way to the older valid one; all invalid is a failure, never an
 * empty map. */
static void corrupt_cases(void){
    const char *origin="https://storage-corrupt.invalid";char path[STORAGE_PATH];cold_all(origin);
    webstorage *s=webstorage_create();struct web_storage_result out;reset(F_NONE);
    verify(storage_test_access(s,origin,WEB_STORAGE_SET,"key","one",&out)==0&&webstorage_flush(s,true,&out)==0,"generation-one");
    verify(storage_test_access(s,origin,WEB_STORAGE_SET,"key","two",&out)==0&&webstorage_flush(s,true,&out)==0,"generation-two");
    int newest=s->areas->slot;webstorage_free(s);cold_shared(origin);
    storage_path(origin,newest,path);flip_byte(path,60);
    s=webstorage_create();verify(value_is(s,origin,"key","one"),"invalid-new-slot-recovers-previous");webstorage_free(s);cold_shared(origin);
    storage_path(origin,1-newest,path);flip_byte(path,60);
    s=webstorage_create();verify(storage_test_access(s,origin,WEB_STORAGE_GET,"key",NULL,&out)==WEB_STORAGE_IO,"all-invalid-fails-not-reset");
    webstorage_free(s);cold_all(origin);
}
/* Only the quota bounds a map: more keys than any former count, a snapshot past the former 8 MiB
 * of document, and a refusal exactly at WEB_STORAGE_QUOTA_BYTES. */
static void quota_cases(void){
    const char *origin="https://storage-quota.invalid";cold_all(origin);
    webstorage *s=webstorage_create();struct web_storage_result out;reset(F_NONE);
    bool ok=true;char key[32];
    for(int i=0;i<20000 && ok;i++){snprintf(key,sizeof key,"k%d",i);ok=storage_test_access(s,origin,WEB_STORAGE_SET,key,"v",&out)==0;}
    verify(ok&&s->areas->count==20000,"more-keys-than-a-fixed-count");
    struct web_storage_request length={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_LENGTH};
    verify(webstorage_access(s,0,origin,&length,&out)==0&&out.length==20000,"length-of-a-large-map");
    bool order=true;for(uint32_t i=0;i<20000 && order;i++){
        struct web_storage_request at={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_KEY,.index=i};snprintf(key,sizeof key,"k%u",i);
        order=webstorage_access(s,0,origin,&at,&out)==0&&out.text&&!strcmp(out.text,key);free(out.text);}
    verify(order,"key-enumeration-in-insertion-order");
    verify(storage_test_access(s,origin,WEB_STORAGE_REMOVE,"k5",NULL,&out)==0&&value_is(s,origin,"k5",NULL)&&value_is(s,origin,"k6","v"),"remove-from-a-large-map");
    struct web_storage_request at5={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_KEY,.index=5};
    verify(webstorage_access(s,0,origin,&at5,&out)==0&&out.text&&!strcmp(out.text,"k6"),"order-closes-after-remove");free(out.text);
    verify(storage_test_access(s,origin,WEB_STORAGE_CLEAR,NULL,NULL,&out)==0,"large-map-clear");
    webstorage_free(s);cold_all(origin);
    /* Three-byte characters, an entry per two units: the map fills the quota and its snapshot
     * passes 8 MiB. Built in RAM (one origin's map, the same apply that journal records use). */
    struct storage_area *big=calloc(1,sizeof *big);big->origin=storage_copy(origin,strlen(origin));
    big->kind=WEB_STORAGE_LOCAL;big->slot=-1;big->shared=big->shared_log=-1;
    char k[7]={0};ok=true;unsigned n=0;
    for(unsigned i=0;ok && n*2u+2u<=STORAGE_QUOTA_UNITS;i++){
        unsigned c1=0x4e00+(i>>9),c2=0x4e00+(i&511);
        k[0]=(char)(0xE0|c1>>12);k[1]=(char)(0x80|(c1>>6&63));k[2]=(char)(0x80|(c1&63));
        k[3]=(char)(0xE0|c2>>12);k[4]=(char)(0x80|(c2>>6&63));k[5]=(char)(0x80|(c2&63));
        struct web_storage_request set={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_SET,.key=k,.key_len=6,.value="",.value_len=0};
        ok=storage_area_apply(big,&set,&out)==0;if(ok)n++;
    }
    verify(ok&&big->units==n*2u&&big->count==n,"fills-the-quota-with-small-entries");
    struct web_storage_request over={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_SET,.key="xyz",.key_len=3,.value="",.value_len=0};
    verify(storage_area_apply(big,&over,&out)==WEB_STORAGE_QUOTA,"refused-only-past-the-quota");
    struct web_storage_result saved={0};
    verify(storage_save(big,&saved)==WEB_STORAGE_OK&&saved.snapshot_bytes>8u*1024u*1024u,"snapshot-past-the-former-document-limit");
    struct storage_area *back=NULL;
    verify(storage_read(origin,big->slot,&back)==1&&back->count==n&&back->units==big->units,"large-snapshot-reads-back");
    storage_area_free(back);storage_area_free(big);cold_all(origin);
}
int main(void){
    folders();
    for(int mode=F_NONE;mode<=F_SAVE;mode++)exercise(mode);
    corrupt_cases();
    quota_cases();
    printf("storage snapshot native: %d checks, %d failed\n",checks,failures);return failures!=0;
}
