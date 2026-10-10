/* Host support test: the actual backend writes real isolated files, while only
 * native file API failures are injected. This is not real-site acceptance. */
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
typedef intptr_t ssize_t;
struct n_stat { uint64_t size; int type; };
enum { F_NONE, F_SHORT, F_EINTR, F_ZERO, F_WRITE, F_WRITE_CLOSE,
       F_READ_ZERO, F_READ_CLOSE, F_CORRUPT, F_OPEN_TRUNCATE, F_UNLINK };
static FILE *handles[32];
static bool writing[32];
static int fault, writes, reads, unlinks, renames, checks, failures;
static size_t bytes_written, bytes_read;
static bool injected;
static int probe_stat(const char *path, struct n_stat *out) {
    struct _stat64 st;
    if (_stat64(path, &st) < 0) return -1;
    out->size=(uint64_t)st.st_size;out->type=(st.st_mode&_S_IFDIR)?2:1;return 0;
}
static int probe_mkdir(const char *path, ...) { return _mkdir(path); }
static int probe_open(const char *path, int flags, ...) {
    bool wr=(flags&3)!=0;FILE *f=fopen(path,wr?"wb":"rb");
    if(!f)return -1;
    if(wr&&fault==F_OPEN_TRUNCATE&&!injected){injected=true;fclose(f);errno=ENOMEM;return -1;}
    for(int i=0;i<32;i++)if(!handles[i]){handles[i]=f;writing[i]=wr;return i;}
    fclose(f);errno=EMFILE;return -1;
}
static ssize_t probe_write(int fd,const void *data,size_t n) {
    writes++;
    if(fault==F_EINTR&&!injected){injected=true;errno=EINTR;return -1;}
    if((fault==F_ZERO||fault==F_WRITE||fault==F_UNLINK)&&injected){errno=EIO;return fault==F_ZERO?0:-1;}
    if((fault==F_SHORT||fault==F_ZERO||fault==F_WRITE||fault==F_UNLINK)&&!injected){if(n>7)n=7;injected=true;}
    size_t done=fwrite(data,1,n,handles[fd]);bytes_written+=done;
    return done==0&&ferror(handles[fd])?-1:(ssize_t)done;
}
static ssize_t probe_read(int fd,void *data,size_t n) {
    reads++;
    if(fault==F_READ_ZERO){if(injected)return 0;if(n>7)n=7;injected=true;}
    size_t done=fread(data,1,n,handles[fd]);bytes_read+=done;
    if(fault==F_CORRUPT&&!injected&&done>8){((unsigned char *)data)[8]^=1;injected=true;}
    return done==0&&ferror(handles[fd])?-1:(ssize_t)done;
}
static int probe_close(int fd) {
    bool wr=writing[fd];int result=fclose(handles[fd]);handles[fd]=NULL;
    if((wr&&fault==F_WRITE_CLOSE)||(!wr&&fault==F_READ_CLOSE)){errno=EIO;return -1;}
    return result;
}
static int probe_unlink(const char *path) {unlinks++;if(fault==F_UNLINK){errno=EIO;return -1;}return _unlink(path);}
static int probe_rename(const char *from,const char *to){renames++;return rename(from,to);}
static uint64_t uptime_ms(void){static uint64_t clock;return ++clock;}
#define WEBSTORAGE_ROOT "build/goal-20261009/storage-native-fault"
#define stat probe_stat
#define mkdir probe_mkdir
#define open probe_open
#define write probe_write
#define read probe_read
#define close probe_close
#define unlink probe_unlink
#define rename probe_rename
#include "../user/libc/webstorage.c"
#undef stat
#undef mkdir
#undef open
#undef write
#undef read
#undef close
#undef unlink
#undef rename
static void verify(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static void reset(int value){fault=value;injected=false;writes=reads=unlinks=renames=0;bytes_written=bytes_read=0;}
static int storage_test_access(webstorage *s,const char *origin,int operation,const char *key,const char *value,struct web_storage_result *out){
    struct web_storage_request request={.kind=WEB_STORAGE_LOCAL,.operation=operation,.key=key,.key_len=key?strlen(key):0,.value=value,.value_len=value?strlen(value):0};
    return webstorage_access(s,origin,&request,out);
}
static bool value_is(webstorage *s,const char *origin,const char *key,const char *value){
    struct web_storage_result out;int r=storage_test_access(s,origin,WEB_STORAGE_GET,key,NULL,&out);
    bool ok=r==WEB_STORAGE_OK&&out.text&&out.text_len==strlen(value)&&!memcmp(out.text,value,out.text_len);free(out.text);return ok;
}
static unsigned char *file_bytes(const char *path,size_t *len){
    FILE *f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
    unsigned char *p=n>=0?malloc((size_t)n+1):NULL;if(p&&fread(p,1,(size_t)n,f)==(size_t)n)*len=(size_t)n;else{free(p);p=NULL;}fclose(f);return p;
}
static void exercise(int mode){
    char origin[96],confirmed[160],candidate[160];snprintf(origin,sizeof origin,"https://storage-fault-%d.invalid",mode);
    storage_path(origin,0,confirmed);storage_path(origin,1,candidate);_unlink(confirmed);_unlink(candidate);
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
    bool success=mode==F_NONE||mode==F_SHORT||mode==F_EINTR||mode==F_READ_ZERO||mode==F_READ_CLOSE||mode==F_CORRUPT;
    verify(r==(success?WEB_STORAGE_OK:WEB_STORAGE_IO),"publication-result");
    verify(renames==0,"no-copy-rename");
    if(success){
        verify(s->areas->generation==generation+1&&s->areas->slot==1-slot,"generation-adopted-once");
        verify(bytes_written==out.snapshot_bytes&&bytes_read==0&&unlinks==0,"one-write-no-readback-no-copy");
    }else{
        verify(s->areas->generation==generation&&s->areas->slot==slot,"failed-generation-unpublished");
        struct _stat64 st;verify(mode==F_UNLINK?s->areas->unavailable:_stat64(candidate,&st)<0&&errno==ENOENT,"candidate-invalidated-or-failclosed");
        verify(unlinks==1,"every-failure-invalidates-candidate");
    }
    size_t currentn=0;unsigned char *current=file_bytes(confirmed,&currentn);
    verify(old&&current&&oldn==currentn&&!memcmp(old,current,oldn),"last-confirmed-slot-untouched");free(old);free(current);
    reset(F_NONE);
    verify(value_is(s,origin,"first","new")&&value_is(s,origin,"second","kept"),"ram-map-retained-for-retry");
    struct web_storage_request key={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_KEY,.index=0};
    verify(webstorage_access(s,origin,&key,&out)==WEB_STORAGE_OK&&out.text&&!strcmp(out.text,"first"),"key-order-preserved");free(out.text);
    webstorage *disk=webstorage_create();verify(value_is(disk,origin,"first",success?"new":"old"),"disk-keeps-last-completed-slot");webstorage_free(disk);
    if(!success)verify(webstorage_flush(s,true,&out)==WEB_STORAGE_OK,"failed-commit-can-retry");
    webstorage_free(s);s=webstorage_create();
    verify(value_is(s,origin,"first","new")&&value_is(s,origin,"second","kept"),"reopen-recovers-completed-retry");
    webstorage_free(s);storage_path(origin,0,confirmed);storage_path(origin,1,candidate);_unlink(confirmed);_unlink(candidate);
}
int main(void){
    for(int mode=F_NONE;mode<=F_UNLINK;mode++)exercise(mode);
    printf("storage snapshot native: %d checks, %d failed\n",checks,failures);return failures!=0;
}
