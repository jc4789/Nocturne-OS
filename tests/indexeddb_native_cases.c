/* Actual webidb.c and BearSSL hash; only native file/process/lease syscalls
 * are adapted to isolated host files. No browser or site success is inferred. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <sys/stat.h>
#include "../tools/msys64/ucrt64/include/fcntl.h"
#undef SYS_OPEN
#include "../common/abi.h"
struct file_entry{char path[400];int leased;};static struct file_entry files[2048];
static char **directories;static size_t directory_count;
static long long fail_write=-1;static size_t written_bytes,read_bytes;static int failures,checks;static unsigned fixture_serial;
static long long fail_head_write=-1;
static void path(const char *guest,char host[400]){snprintf(host,400,"build/idb-native-%u/",fixture_serial);size_t at=strlen(host);for(const char *p=guest;*p&&at<399;p++)host[at++]=*p=='/'?'_':*p;host[at]=0;}
static int adapter_open(const char *guest,int flags,...){
    if(flags&0x10000){if(!strcmp(guest,"/data/browser/indexeddb"))return 4095;for(size_t i=0;i<directory_count;i++)if(!strcmp(directories[i],guest))return 4096+(int)i;errno=ENOENT;return -1;}
    char host[400];path(guest,host);int mode=(flags&3)==2?_O_RDWR:(flags&3)==1?_O_WRONLY:_O_RDONLY;if(flags&0x40)mode|=_O_CREAT;if(flags&0x200)mode|=_O_TRUNC;if(flags&0x400)mode|=_O_APPEND;int fd=_open(host,mode|_O_BINARY,_S_IREAD|_S_IWRITE);if(fd>=0){if(fd>=2048){_close(fd);errno=ENOMEM;return -1;}snprintf(files[fd].path,sizeof files[fd].path,"%s",host);files[fd].leased=0;}return fd;
}
static int adapter_fcntl(int fd,int cmd,...){if(cmd==F_NUNLOCK){files[fd].leased=0;return 0;}if(files[fd].leased)return 0;for(int i=0;i<2048;i++)if(i!=fd&&files[i].leased&&!strcmp(files[i].path,files[fd].path)){errno=EAGAIN;return -1;}files[fd].leased=1;return 0;}
static ssize_t adapter_read(int fd,void *p,size_t n){int r=_read(fd,p,(unsigned)n);if(r>0)read_bytes+=(size_t)r;return r;}
static ssize_t adapter_write(int fd,const void *p,size_t n){
    size_t path_length=strlen(files[fd].path);long long *remaining=&fail_write;
    if(fail_head_write>=0&&path_length>=2&&files[fd].path[path_length-2]=='.'&&(files[fd].path[path_length-1]=='0'||files[fd].path[path_length-1]=='1'))remaining=&fail_head_write;
    if(*remaining==0){errno=EIO;return -1;}if(*remaining>=0&&(uint64_t)*remaining<n)n=(size_t)*remaining;
    int r=_write(fd,p,(unsigned)n);if(r>0){written_bytes+=(size_t)r;if(*remaining>=0)*remaining-=r;}return r;
}
static int adapter_close(int fd){if(fd>=4095)return 0;files[fd].leased=0;files[fd].path[0]=0;return _close(fd);}
static long long adapter_lseek(int fd,long long at,int whence){return _lseeki64(fd,at,whence);}
static int adapter_unlink(const char *guest){char host[400];path(guest,host);return _unlink(host);}
static int adapter_stat(const char *guest,struct n_stat *out){bool directory=!strcmp(guest,"/data/browser")||!strcmp(guest,"/data/browser/indexeddb")||!strcmp(guest,"/tmp/browser-indexeddb");for(size_t i=0;i<directory_count&&!directory;i++)directory=!strcmp(directories[i],guest);if(directory){memset(out,0,sizeof *out);out->type=2;return 0;}char host[400];path(guest,host);struct _stat64 st;if(_stat64(host,&st)<0)return -1;memset(out,0,sizeof *out);out->size=st.st_size;out->type=1;return 0;}
static int adapter_fstat(int fd,struct n_stat *out){struct _stat64 st;if(_fstat64(fd,&st)<0)return -1;memset(out,0,sizeof *out);out->size=st.st_size;out->type=1;return 0;}
static int adapter_mkdir(const char *p,unsigned mode){(void)mode;char **more=realloc(directories,(directory_count+1)*sizeof *more);if(!more)return -1;directories=more;directories[directory_count]=_strdup(p);if(!directories[directory_count])return -1;directory_count++;return 0;}
static int adapter_pid(void){return 110;}
static int adapter_processes(struct n_procinfo *p,int n){if(n){memset(p,0,sizeof *p);p->pid=110;p->start_ms=7;}return 1;}
static int adapter_readdir(int fd,int index,struct n_dirent *out){
    const char *directory=fd==4095?"/data/browser/indexeddb":directories[fd-4096];size_t prefix_length=strlen(directory);int current=0;
    for(size_t i=0;i<directory_count;i++){
        const char *name=directories[i];if(strncmp(name,directory,prefix_length)||name[prefix_length]!='/'||strchr(name+prefix_length+1,'/'))continue;
        if(current++==index){memset(out,0,sizeof *out);snprintf(out->name,sizeof out->name,"%s",name+prefix_length+1);out->type=2;return 1;}
    }
    char guest[400],query[400],prefix[400];snprintf(guest,sizeof guest,"%s/*",directory);path(guest,query);path(directory,prefix);
    const char *basename=strrchr(prefix,'/');basename=basename?basename+1:prefix;size_t disk_prefix=strlen(basename);
    struct __finddata64_t info;intptr_t search=_findfirst64(query,&info);if(search==-1)return 0;int result=0;
    do{if(strncmp(info.name,basename,disk_prefix)||info.name[disk_prefix]!='_')continue;const char *name=info.name+disk_prefix+1;if(strchr(name,'_'))continue;
        if(current++==index){memset(out,0,sizeof *out);snprintf(out->name,sizeof out->name,"%s",name);out->size=info.size;out->type=1;result=1;break;}
    }while(_findnext64(search,&info)==0);_findclose(search);return result;
}
#define open adapter_open
#define fcntl adapter_fcntl
#define read adapter_read
#define write adapter_write
#define close adapter_close
#define lseek adapter_lseek
#define unlink adapter_unlink
#define stat adapter_stat
#define fstat adapter_fstat
#define mkdir adapter_mkdir
#define getpid adapter_pid
#define proclist adapter_processes
#define readdir adapter_readdir
#include "../user/libc/webidb.c"
#define hash idb_journal_hash
#define put64 idb_journal_put64
#include "../user/libc/webidb_journal.c"
#undef put64
#undef hash
static void check(const char *label,int yes){checks++;if(!yes){failures++;printf("FAIL %s\n",label);}}
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
    uint32_t client=0;check("native-connection-register",webidb_connect(b,tb,1,&client)==0&&client);size_t clients_count=0;check("native-versionchange-enqueue",webidb_connections(b,tb,2,&clients_count)==0&&clients_count==1);int pending=0;uint64_t desired=0;check("native-versionchange-delivery",webidb_poll(b,client,&pending,&desired)==0&&pending&&desired==2);check("native-versionchange-once",webidb_poll(b,client,&pending,&desired)==0&&!pending);
    check("native-versionchange-retry-once",webidb_connections(b,tb,2,&clients_count)==0&&webidb_poll(b,client,&pending,&desired)==0&&!pending);
    check("native-close-unblocks",webidb_disconnect(b,client)==0&&webidb_connections(b,tb,2,&clients_count)==0&&clients_count==0);
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
    free(snapshot);free(large);webidb_free(a);webidb_free(b);
    for(size_t i=0;i<directory_count;i++)free(directories[i]);free(directories);
    printf("native IndexedDB: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
