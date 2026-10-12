/* Actual storage implementation, real isolated shared files. Only native
 * file/scheduling APIs use the existing host adapter. */
#include <stdio.h>
static int shared_reads, shared_seeks;
static size_t hot_read(void *data,size_t size,size_t n,FILE *file) {
    shared_reads++;return fread(data,size,n,file);
}
static int hot_seek(FILE *file,long at,int whence) {
    shared_seeks++;return fseek(file,at,whence);
}
#define fread hot_read
#define fseek hot_seek
#define main previous_storage_snapshot_main
#include "storage_snapshot_native_cases.c"
#undef main
#undef fread
#undef fseek
static const char *site="https://storage-hotpath-native.test";
static int hot_call(webstorage *s, uint64_t source, int op, const char *value,
                    struct web_storage_result *out) {
    struct web_storage_request r={.kind=WEB_STORAGE_LOCAL,.operation=op,.source=source,
        .key="key",.key_len=3,.value=value,.value_len=value?strlen(value):0,
        .url="https://storage-hotpath-native.test/source"};
    return webstorage_access(s,site,&r,out);
}
int main(void) {
    char path[160]; for(int i=0;i<2;i++){storage_path(site,i,path);_unlink(path);}cold_shared(site);
    reset(F_NONE);webstorage *a=webstorage_create(),*b=webstorage_create();struct web_storage_result out={0};
    verify(hot_call(a,101,WEB_STORAGE_CHECK,NULL,&out)==0,"first-getter-registers-source");
    int before_reads=shared_reads,before_seeks=shared_seeks;
    bool ok=true;for(int i=0;i<138;i++)if(hot_call(a,101,WEB_STORAGE_CHECK,NULL,&out))ok=false;
    verify(ok && shared_reads==before_reads && shared_seeks==before_seeks,"repeated-getters-do-not-read-or-seek");
    verify(hot_call(a,101,WEB_STORAGE_SET,"first",&out)==0,"first-published");
    verify(hot_call(b,202,WEB_STORAGE_GET,NULL,&out)==0 && out.text && !strcmp(out.text,"first"),"other-window-initial-read");webstorage_result_free(&out);
    before_reads=shared_reads;
    verify(hot_call(b,202,WEB_STORAGE_GET,NULL,&out)==0 && out.text && !strcmp(out.text,"first") &&
           shared_reads==before_reads+1,"unchanged-get-reads-control-once");webstorage_result_free(&out);
    verify(hot_call(a,101,WEB_STORAGE_SET,"second",&out)==0,"second-published-without-flush");
    verify(hot_call(b,202,WEB_STORAGE_GET,NULL,&out)==0 && out.text && !strcmp(out.text,"second"),"same-task-other-window-change-visible");webstorage_result_free(&out);
    verify(hot_call(b,202,WEB_STORAGE_POLL,NULL,&out)==0 && out.event && out.old_value && !strcmp(out.old_value,"first") &&
           out.new_value && !strcmp(out.new_value,"second"),"getter-shortcut-keeps-post-subscription-events");webstorage_result_free(&out);
    uint8_t saved[256],broken[256];memcpy(saved,b->areas->shared_control_bytes,256);memcpy(broken,saved,256);
    broken[8]^=1;broken[136]^=1;
    verify(storage_io_at(a->areas->shared_fd,0,broken,256,true),"corrupt-both-control-slots");
    verify(hot_call(b,202,WEB_STORAGE_GET,NULL,&out)==WEB_STORAGE_IO,"cached-header-does-not-hide-corruption");webstorage_result_free(&out);
    verify(storage_io_at(a->areas->shared_fd,0,saved,256,true),"restore-completed-header");
    verify(hot_call(b,202,WEB_STORAGE_GET,NULL,&out)==0 && out.text && !strcmp(out.text,"second"),"header-recovery-revalidates");webstorage_result_free(&out);
    verify(webstorage_flush(a,true,&out)==0 && out.save_attempts==1,"optimized-access-keeps-persistence");
    webstorage_free(b);webstorage_free(a);cold_shared(site);
    a=webstorage_create();verify(hot_call(a,303,WEB_STORAGE_GET,NULL,&out)==0 && out.text && !strcmp(out.text,"second"),"cold-reopen-retains-value");webstorage_result_free(&out);webstorage_free(a);
    cold_shared(site);for(int i=0;i<2;i++){storage_path(site,i,path);_unlink(path);}
    printf("storage-hotpath-native: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
