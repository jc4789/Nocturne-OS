/* The existing fault adapter wraps actual native storage and real host files.
 * These expectations deliberately replace synchronous setItem publication:
 * RAM is immediate, a whole burst has one idle/forced snapshot, no readback. */
#define main previous_storage_test_main
#include "storage_snapshot_native_cases.c"
#undef main
int main(void){
    const char *origin="https://storage-batch.invalid";char path[160];
    storage_path(origin,0,path);_unlink(path);storage_path(origin,1,path);_unlink(path);
    webstorage *s=webstorage_create();struct web_storage_result out;reset(F_NONE);
    for(int i=0;i<50;i++){char value[16];snprintf(value,sizeof value,"%d",i);
        verify(storage_test_access(s,origin,WEB_STORAGE_SET,"burst",value,&out)==WEB_STORAGE_OK,"ram-set");}
    verify(writes==0&&reads==0&&webstorage_pending(s),"burst-has-no-disk-io");
    verify(value_is(s,origin,"burst","49"),"ram-read-your-last-write");
    verify(webstorage_flush(s,false,&out)==WEB_STORAGE_OK&&out.save_attempts==0&&writes==0,"quiet-period-debounce");
    verify(webstorage_flush(s,true,&out)==WEB_STORAGE_OK&&out.save_attempts==1,"one-force-publication");
    verify(writes==1&&reads==0&&bytes_written==out.snapshot_bytes&&!webstorage_pending(s),"no-snapshot-readback");
    uint64_t generation=s->areas->generation;webstorage_free(s);
    reset(F_NONE);s=webstorage_create();verify(value_is(s,origin,"burst","49")&&reads==1,"validated-load-after-reopen");
    reset(F_WRITE_CLOSE);verify(storage_test_access(s,origin,WEB_STORAGE_SET,"burst","new",&out)==WEB_STORAGE_OK,"ram-write-before-io-error");
    verify(webstorage_flush(s,true,&out)==WEB_STORAGE_IO&&webstorage_pending(s),"failed-flush-remains-dirty");
    int failed_writes=writes;
    verify(webstorage_flush(s,false,&out)==WEB_STORAGE_OK&&out.save_attempts==0&&writes==failed_writes,"failed-io-retry-backoff");
    verify(s->areas->generation==generation&&value_is(s,origin,"burst","new"),"failed-flush-does-not-roll-back-ram");
    webstorage *old=webstorage_create();reset(F_NONE);verify(value_is(old,origin,"burst","49"),"last-completed-slot-survives-failure");webstorage_free(old);
    reset(F_EINTR);verify(webstorage_flush(s,true,&out)==WEB_STORAGE_OK&&out.save_attempts==1&&reads==0,"flush-retries-eintr-without-readback");
    verify(s->areas->generation==generation+1&&!webstorage_pending(s),"retry-publishes-one-generation");
    reset(F_NONE);verify(storage_test_access(s,origin,WEB_STORAGE_SET,"closing","saved",&out)==WEB_STORAGE_OK,"shutdown-pending");webstorage_free(s);
    s=webstorage_create();verify(value_is(s,origin,"closing","saved"),"free-forces-durability-boundary");webstorage_free(s);
    /* Session areas are never serialized even at an explicit flush. */
    s=webstorage_create();struct web_storage_request request={.kind=WEB_STORAGE_SESSION,.operation=WEB_STORAGE_SET,.key="k",.key_len=1,.value="v",.value_len=1};
    reset(F_NONE);verify(webstorage_access(s,origin,&request,&out)==WEB_STORAGE_OK&&!webstorage_pending(s),"session-only-ram");
    verify(webstorage_flush(s,true,&out)==WEB_STORAGE_OK&&writes==0&&reads==0,"session-flush-no-io");webstorage_free(s);
    printf("storage-batch-native: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
