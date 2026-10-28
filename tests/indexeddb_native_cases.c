/* Actual webidb.c and BearSSL hash; only the native document, folder, process and lease calls are
 * adapted to isolated host files. No browser or site success is inferred.
 * The databases are where the browser keeps them, in the folders of C:\Users\User\AppData\Browser
 * (and its temporary folder, C:\Users\User\AppData\Temp\Browser); the host holds those folders and
 * files under build/idb-native-<serial>.
 * Build with -idirafter user/include -Icommon -Ithird_party/bearssl/inc -Ithird_party/bearssl/src (the
 * real nocturne.h, webstorage.h and webidb.h are used, the host's own errno.h and the like) and
 * BearSSL's sha2small.c, enc32be.c and dec32be.c. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <sys/stat.h>
#include <nocturne.h>
#include <webstorage.h>
#define PROFILE_FOLDER "C:\\Users\\User\\AppData\\Browser"
#define TEMPORARY_PROFILE_FOLDER "C:\\Users\\User\\AppData\\Temp\\Browser"
struct file_entry{char path[400];int leased;};static struct file_entry files[2048];
static long long fail_write=-1;static size_t written_bytes,read_bytes;static int failures,checks;static unsigned fixture_serial;
static long long fail_head_write=-1;
/* The host path of a complete guest path: C: is dropped and '\' becomes '/'. */
static void path(const char *guest,char host[400]){snprintf(host,400,"build/idb-native-%u",fixture_serial);size_t at=strlen(host);if(guest[0]&&guest[1]==':')guest+=2;for(const char *p=guest;*p&&at<399;p++)host[at++]=*p=='\\'?'/':*p;host[at]=0;}
n_result n_path_join(const char *folder,const char *name,char *out,size_t cap){int n=snprintf(out,cap,"%s\\%s",folder,name);return n<0||(size_t)n>=cap?-N_ERR_TOO_LARGE:n;}
const char *n_path_name(const char *guest){const char *name=guest;for(const char *p=guest;*p;p++)if(*p=='\\'||*p=='/')name=p+1;return name;}
n_result n_entry_info(const char *guest,struct n_entry *out){char host[400];path(guest,host);struct _stat64 st;if(_stat64(host,&st)<0)return -N_ERR_NOT_FOUND;memset(out,0,sizeof *out);snprintf(out->name,sizeof out->name,"%s",n_path_name(guest));out->kind=(st.st_mode&_S_IFDIR)?N_KIND_FOLDER:N_KIND_DOCUMENT;out->size=(uint64_t)st.st_size;return 0;}
n_result n_entry_remove(const char *guest){char host[400];path(guest,host);struct _stat64 st;if(_stat64(host,&st)<0)return -N_ERR_NOT_FOUND;return ((st.st_mode&_S_IFDIR)?_rmdir(host):_unlink(host))==0?0:-N_ERR_IO;}
n_result n_folder_create(const char *guest){char host[400];path(guest,host);if(_mkdir(host)==0)return 0;return errno==EEXIST?-N_ERR_EXISTS:errno==ENOENT?-N_ERR_NOT_FOUND:-N_ERR_IO;}
n_result n_folder_entries(const char *guest,struct n_entry **out){
    char host[400],query[420];path(guest,host);*out=NULL;struct _stat64 st;if(_stat64(host,&st)<0)return -N_ERR_NOT_FOUND;if(!(st.st_mode&_S_IFDIR))return -N_ERR_NOT_FOLDER;
    snprintf(query,sizeof query,"%s/*",host);struct __finddata64_t info;intptr_t search=_findfirst64(query,&info);struct n_entry *list=NULL;size_t count=0;
    if(search!=-1){
        do{if(!strcmp(info.name,".")||!strcmp(info.name,".."))continue;
            struct n_entry *more=realloc(list,(count+1)*sizeof *list);if(!more){free(list);_findclose(search);return -N_ERR_NO_MEMORY;}
            list=more;memset(&list[count],0,sizeof *list);snprintf(list[count].name,sizeof list[count].name,"%s",info.name);list[count].kind=(info.attrib&_A_SUBDIR)?N_KIND_FOLDER:N_KIND_DOCUMENT;list[count].size=(uint64_t)info.size;count++;
        }while(_findnext64(search,&info)==0);
        _findclose(search);
    }
    *out=list;return (n_result)count;
}
n_handle n_document_open(const char *guest,int access){
    char host[400];path(guest,host);struct _stat64 st;int exists=_stat64(host,&st)==0;
    if(exists&&(st.st_mode&_S_IFDIR))return -N_ERR_IS_FOLDER;
    if(access==N_DOCUMENT_CREATE&&exists)return -N_ERR_EXISTS;
    if(access!=N_DOCUMENT_CREATE&&!exists)return -N_ERR_NOT_FOUND;
    if(access!=N_DOCUMENT_READ&&access!=N_DOCUMENT_EDIT&&access!=N_DOCUMENT_CREATE)return -N_ERR_UNSUPPORTED;
    int mode=access==N_DOCUMENT_READ?_O_RDONLY:_O_RDWR;if(access==N_DOCUMENT_CREATE)mode|=_O_CREAT|_O_EXCL;
    int fd=_open(host,mode|_O_BINARY,_S_IREAD|_S_IWRITE);
    if(fd<0)return errno==ENOENT?-N_ERR_NOT_FOUND:-N_ERR_IO;
    if(fd>=2048){_close(fd);return -N_ERR_NO_MEMORY;}
    snprintf(files[fd].path,sizeof files[fd].path,"%s",host);files[fd].leased=0;return fd;
}
static bool open_handle(n_handle h){return h>=0&&h<2048&&files[h].path[0];}
n_result n_document_read_at(n_handle h,uint64_t at,void *p,size_t n){
    if(!open_handle(h))return -N_ERR_NO_HANDLE;
    if(_lseeki64(h,(long long)at,SEEK_SET)<0)return -N_ERR_IO;
    int r=_read(h,p,(unsigned)n);if(r<0)return -N_ERR_IO;read_bytes+=(size_t)r;return r;
}
/* The write budget that applies to a file: the heads (".0" and ".1") have their own when it is set. */
static long long *write_budget(const char *host){size_t length=strlen(host);bool head=length>=2&&host[length-2]=='.'&&(host[length-1]=='0'||host[length-1]=='1');return fail_head_write>=0&&head?&fail_head_write:&fail_write;}
n_result n_document_write_at(n_handle h,uint64_t at,const void *p,size_t n){
    if(!open_handle(h))return -N_ERR_NO_HANDLE;
    long long *remaining=write_budget(files[h].path);
    if(*remaining==0)return -N_ERR_IO;if(*remaining>=0&&(uint64_t)*remaining<n)n=(size_t)*remaining;
    if(_lseeki64(h,(long long)at,SEEK_SET)<0)return -N_ERR_IO;
    int r=_write(h,p,(unsigned)n);if(r<0)return -N_ERR_IO;written_bytes+=(size_t)r;if(*remaining>=0)*remaining-=r;return r;
}
n_result n_document_size(n_handle h){if(!open_handle(h))return -N_ERR_NO_HANDLE;long long size=_filelengthi64(h);return size<0?-N_ERR_IO:(n_result)size;}
n_result n_document_set_size(n_handle h,uint64_t size){if(!open_handle(h))return -N_ERR_NO_HANDLE;return _chsize_s(h,(long long)size)==0?0:-N_ERR_IO;}
/* A claim belongs to one opening: another opening of the same document is not ready, the same one again is. */
n_result n_document_claim(n_handle h,bool wait){
    (void)wait;if(!open_handle(h))return -N_ERR_NO_HANDLE;if(files[h].leased)return 0;
    for(int i=0;i<2048;i++)if(i!=h&&files[i].leased&&!strcmp(files[i].path,files[h].path))return -N_ERR_NOT_READY;
    files[h].leased=1;return 0;
}
n_result n_document_release(n_handle h){if(!open_handle(h))return -N_ERR_NO_HANDLE;if(!files[h].leased)return -N_ERR_INVALID;files[h].leased=0;return 0;}
n_result n_handle_close(n_handle h){if(!open_handle(h))return -N_ERR_NO_HANDLE;files[h].leased=0;files[h].path[0]=0;return _close(h)==0?0:-N_ERR_IO;}
/* Save and append take the whole of data or nothing: a write budget that is too small fails the call and
   leaves the document as it was, as a medium that gave out before the swap would. */
static n_result put_document(const char *guest,const void *data,size_t n,bool append){
    char host[400];path(guest,host);struct _stat64 st;
    if(_stat64(host,&st)==0&&(st.st_mode&_S_IFDIR))return -N_ERR_IS_FOLDER;
    long long *remaining=write_budget(host);
    if(*remaining>=0&&(uint64_t)*remaining<n){*remaining=0;return -N_ERR_IO;}
    int fd=_open(host,_O_WRONLY|_O_CREAT|(append?_O_APPEND:_O_TRUNC)|_O_BINARY,_S_IREAD|_S_IWRITE);
    if(fd<0)return errno==ENOENT?-N_ERR_NOT_FOUND:-N_ERR_IO;
    int r=n?_write(fd,data,(unsigned)n):0;if(_close(fd)!=0||r<0||(size_t)r!=n)return -N_ERR_IO;
    written_bytes+=n;if(*remaining>=0)*remaining-=(long long)n;return 0;
}
n_result n_document_save(const char *guest,const void *data,size_t n){return put_document(guest,data,n,false);}
n_result n_document_append(const char *guest,const void *data,size_t n){return put_document(guest,data,n,true);}
n_result n_app_self(void){return 110;}
n_result proclist(struct n_procinfo *p,int n){if(n){memset(p,0,sizeof *p);p->pid=110;p->start_ms=7;}return 1;}
/* The browser's profile folders, made on the host when they are first asked for. */
static void make_folders(const char *guest){char partial[400];size_t length=strlen(guest);for(size_t at=3;at<=length;at++)if(at==length||guest[at]=='\\'){memcpy(partial,guest,at);partial[at]=0;n_folder_create(partial);}}
const char *web_profile_folder(bool temporary){const char *folder=temporary?TEMPORARY_PROFILE_FOLDER:PROFILE_FOLDER;make_folders(folder);return folder;}
#include "../user/libc/webidb.c"
#define hash idb_journal_hash
#define put64 idb_journal_put64
#define document idb_journal_document
#include "../user/libc/webidb_journal.c"
#undef document
#undef put64
#undef hash
static void check(const char *label,int yes){checks++;if(!yes){failures++;printf("FAIL %s\n",label);}}
/* The Settings window's site data (webidb_usage, webidb_clear). */
struct usage_seen {uint64_t idb,other,unknown;size_t count;};
static void usage_each(void *opaque,const char *origin,uint64_t bytes){
    struct usage_seen *u=opaque;u->count++;
    if(!strcmp(origin,"https://idb.test"))u->idb+=bytes;else if(!strcmp(origin,"https://other.test"))u->other+=bytes;else u->unknown+=bytes;
}
static struct usage_seen usage_now(void){struct usage_seen u={0};int r=webidb_usage(usage_each,&u);if(r)u.count=(size_t)-1;return u;}
static bool origin_is(void *opaque,const char *origin){return !strcmp(origin,(const char *)opaque);}
int main(void){fixture_serial=(unsigned)_getpid();char dir[100];snprintf(dir,sizeof dir,"build/idb-native-%u",fixture_serial);_mkdir(dir);
    webidb *a=webidb_create(),*b=webidb_create();uint32_t ta=0,tb=0;uint8_t *meta=NULL,*bytes=NULL;size_t n=0,bn=0;char ref[65],unused[65];const char *refs[1]={ref};
    check("native-origin-security",webidb_begin(a,"file://","db",2,&ta,&meta,&n)==WEB_IDB_SECURITY);
    check("new-database",webidb_begin(a,"https://idb.test","db",2,&ta,&meta,&n)==0&&!meta&&n==0);
    check("cross-realm-lease-busy",webidb_begin(b,"https://idb.test","db",2,&tb,&meta,&n)==WEB_IDB_BUSY);
    uint8_t *large=malloc(1024*1024);memset(large,0x72,1024*1024);check("immutable-payload-stage",webidb_stage(a,ta,large,1024*1024,ref)==0);
    check("metadata-atomic-commit",webidb_commit(a,ta,(const uint8_t *)"one",3,refs,1,1)==0);
    struct webidb_database *catalog_items=NULL;size_t catalog_count=0;read_bytes=0;
    check("catalog-current-origin",webidb_databases(a,"https://idb.test",&catalog_items,&catalog_count)==0&&catalog_count==1&&catalog_items[0].version==1&&catalog_items[0].name_length==2&&!memcmp(catalog_items[0].name,"db",2));
    check("catalog-does-not-read-video-payload",read_bytes<2048);webidb_databases_free(catalog_items,catalog_count);
    check("catalog-origin-isolation",webidb_databases(a,"https://other.test",&catalog_items,&catalog_count)==0&&catalog_count==0);webidb_databases_free(catalog_items,catalog_count);
    check("catalog-opaque-origin-rejected",webidb_databases(a,"null",&catalog_items,&catalog_count)==WEB_IDB_SECURITY);
    written_bytes=read_bytes=0;check("reopen-reads-metadata-only",webidb_begin(b,"https://idb.test","db",2,&tb,&meta,&n)==0&&n==3&&!memcmp(meta,"one",3)&&read_bytes<1024);free(meta);
    check("lazy-value-round-trip",webidb_value(b,tb,ref,&bytes,&bn)==0&&bn==1024*1024&&bytes[1024*1024-1]==0x72);free(bytes);
    uint32_t client=0;check("native-connection-register",webidb_connect(b,tb,1,&client)==0&&client);size_t clients_count=0,unfired=0;check("native-versionchange-enqueue",webidb_connections(b,tb,2,&clients_count,&unfired)==0&&clients_count==1&&unfired==1);int pending=0;uint64_t desired=0;check("native-versionchange-delivery",webidb_poll(b,client,&pending,&desired)==0&&pending&&desired==2);check("native-versionchange-once",webidb_poll(b,client,&pending,&desired)==0&&!pending);
    check("native-versionchange-unfired-until-fired",webidb_connections(b,tb,2,&clients_count,&unfired)==0&&clients_count==1&&unfired==1);
    check("native-versionchange-fired",webidb_fired(b,client)==0&&webidb_connections(b,tb,2,&clients_count,&unfired)==0&&clients_count==1&&unfired==0);
    check("native-versionchange-retry-once",webidb_connections(b,tb,2,&clients_count,&unfired)==0&&webidb_poll(b,client,&pending,&desired)==0&&!pending);
    check("native-versionchange-new-version-marks-again",webidb_connections(b,tb,3,&clients_count,&unfired)==0&&unfired==1&&webidb_poll(b,client,&pending,&desired)==0&&pending&&desired==3&&webidb_fired(b,client)==0);
    check("native-close-unblocks",webidb_disconnect(b,client)==0&&webidb_connections(b,tb,2,&clients_count,&unfired)==0&&clients_count==0&&unfired==0);
    size_t before=written_bytes;check("unchanged-value-no-rewrite",webidb_stage(b,tb,large,1024*1024,unused)==0&&written_bytes==before&&!strcmp(ref,unused));
    fail_write=12;check("partial-manifest-fails",webidb_commit(b,tb,(const uint8_t *)"two",3,refs,1,2)==WEB_IDB_IO);fail_write=-1;webidb_abort(b,tb);
    check("catalog-partial-write-retains-old-version",webidb_databases(a,"https://idb.test",&catalog_items,&catalog_count)==0&&catalog_count==1&&catalog_items[0].version==1);webidb_databases_free(catalog_items,catalog_count);
    check("partial-write-retains-previous-generation",webidb_begin(a,"https://idb.test","db",2,&ta,&meta,&n)==0&&n==3&&!memcmp(meta,"one",3));free(meta);
    check("value-reference-capability",webidb_value(a,ta,"0000000000000000000000000000000000000000000000000000000000000000",&bytes,&bn)==WEB_IDB_SECURITY);
    check("abort-staged-value",webidb_stage(a,ta,(const uint8_t *)"uncommitted",11,unused)==0&&webidb_abort(a,ta)==0);
    check("origin-isolation",webidb_begin(b,"https://other.test","db",2,&tb,&meta,&n)==0&&!meta&&webidb_value(b,tb,ref,&bytes,&bn)==WEB_IDB_SECURITY);webidb_abort(b,tb);
    check("embedded-NUL-database-name",webidb_begin(b,"https://idb.test","db\0more",7,&tb,&meta,&n)==0&&!meta);webidb_abort(b,tb);
    check("catalog-delete-manifest",webidb_begin(a,"https://idb.test","db",2,&ta,&meta,&n)==0&&webidb_commit(a,ta,(const uint8_t *)"deleted",7,NULL,0,0)==0);free(meta);
    check("catalog-deletion-not-old-backup",webidb_databases(a,"https://idb.test",&catalog_items,&catalog_count)==0&&catalog_count==0);webidb_databases_free(catalog_items,catalog_count);
    uint8_t *snapshot=malloc(256*1024);for(size_t i=0;i<256*1024;i++)snapshot[i]=(uint8_t)(i*13+i/79);
    check("journal-first-checkpoint",webidb_begin(a,"https://idb.test","journal",7,&ta,&meta,&n)==0&&webidb_commit(a,ta,snapshot,256*1024,NULL,0,1)==0);free(meta);
    for(unsigned revision=0;revision<8;revision++){
        read_bytes=0;int opened=webidb_begin(a,"https://idb.test","journal",7,&ta,&meta,&n)==0;
        check("warm-metadata-cache-no-full-disk-read",opened&&n==256*1024&&!memcmp(meta,snapshot,n)&&read_bytes<1024);free(meta);
        snapshot[4096+revision*8192]^=0x55;written_bytes=0;
        check("span-delta-does-not-rewrite-full-manifest",webidb_commit(a,ta,snapshot,256*1024,NULL,0,1)==0&&written_bytes<8192);
    }
    webidb_free(a);a=webidb_create();
    check("cold-journal-chain-reconstruction",webidb_begin(a,"https://idb.test","journal",7,&ta,&meta,&n)==0&&n==256*1024&&!memcmp(meta,snapshot,n));free(meta);webidb_abort(a,ta);
    check("partial-head-transaction-open",webidb_begin(a,"https://idb.test","journal",7,&ta,&meta,&n)==0);free(meta);
    snapshot[90000]^=0x11;fail_head_write=16;
    check("partial-small-commit-head-fails",webidb_commit(a,ta,snapshot,256*1024,NULL,0,1)==WEB_IDB_IO);
    fail_head_write=-1;snapshot[90000]^=0x11;webidb_abort(a,ta);webidb_free(a);a=webidb_create();
    check("partial-head-cold-recovery-retains-current",webidb_begin(a,"https://idb.test","journal",7,&ta,&meta,&n)==0&&n==256*1024&&!memcmp(meta,snapshot,n));free(meta);webidb_abort(a,ta);
    uint8_t compact[4096];memset(compact,0x41,sizeof compact);
    int compact_ok=webidb_begin(a,"https://idb.test","compact",7,&ta,&meta,&n)==0&&webidb_commit(a,ta,compact,sizeof compact,NULL,0,1)==0;free(meta);
    for(unsigned i=0;i<350&&compact_ok;i++){
        compact_ok=webidb_begin(a,"https://idb.test","compact",7,&ta,&meta,&n)==0&&n==sizeof compact&&!memcmp(meta,compact,n);free(meta);
        compact[i%sizeof compact]^=1;
        if(compact_ok)compact_ok=webidb_commit(a,ta,compact,sizeof compact,NULL,0,1)==0;
    }
    check("journal-compaction-publishes-all-generations",compact_ok);webidb_free(a);a=webidb_create();
    check("compacted-cold-snapshot-reconstructs",webidb_begin(a,"https://idb.test","compact",7,&ta,&meta,&n)==0&&n==sizeof compact&&!memcmp(meta,compact,n));free(meta);webidb_abort(a,ta);
    /* Site data: each database's origin and bytes; clearing one origin's whole, a database in a
       transaction left (BUSY), the other origin untouched. */
    check("site-data-keep-other-origin",webidb_begin(b,"https://other.test","keep",4,&tb,&meta,&n)==0&&webidb_commit(b,tb,(const uint8_t *)"kept",4,NULL,0,1)==0);free(meta);
    struct usage_seen u=usage_now();
    check("site-data-usage-per-origin",u.count!=(size_t)-1&&u.idb>=1024*1024&&u.other>0&&u.unknown==0);
    check("site-data-busy-database-left",webidb_begin(a,"https://idb.test","db",2,&ta,&meta,&n)==0&&webidb_clear(origin_is,"https://idb.test")==WEB_IDB_BUSY);free(meta);webidb_abort(a,ta);
    u=usage_now();check("site-data-busy-database-kept",u.idb>=1024*1024&&u.other>0);
    check("site-data-clear-origin",webidb_clear(origin_is,"https://idb.test")==WEB_IDB_OK);
    u=usage_now();check("site-data-cleared-whole",u.count!=(size_t)-1&&u.idb==0&&u.other>0&&u.unknown==0);
    check("site-data-cleared-catalog",webidb_databases(a,"https://idb.test",&catalog_items,&catalog_count)==0&&catalog_count==0);webidb_databases_free(catalog_items,catalog_count);
    check("site-data-cleared-database-empty",webidb_begin(a,"https://idb.test","journal",7,&ta,&meta,&n)==0&&!meta&&n==0);webidb_abort(a,ta);
    check("site-data-other-origin-intact",webidb_databases(b,"https://other.test",&catalog_items,&catalog_count)==0&&catalog_count==1&&catalog_items[0].version==1);webidb_databases_free(catalog_items,catalog_count);
    check("site-data-clear-all",webidb_clear(NULL,NULL)==WEB_IDB_OK&&(u=usage_now(),u.count==0));
    free(snapshot);free(large);webidb_free(a);webidb_free(b);
    printf("native IndexedDB: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
