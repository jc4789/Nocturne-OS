/* Actual shared backend with isolated native-file adapter. Reader lag and
 * checkpoint epochs are exercised independently of the real OS window test. */
#define main previous_storage_snapshot_main
#include "storage_snapshot_native_cases.c"
#undef main
static const char *site="https://storage-shared-native.test";
static int call_store(webstorage *s,uint64_t source,int kind,int op,const char *key,const char *value,struct web_storage_result *out){
    struct web_storage_request r={.kind=kind,.operation=op,.source=source,.key=key,.key_len=key?strlen(key):0,
        .value=value,.value_len=value?strlen(value):0,.url="https://storage-shared-native.test/source"};
    return webstorage_access(s,site,&r,out);
}
static bool acknowledge(webstorage *s,uint64_t token,int kind,struct web_storage_result *event){
    struct web_storage_request r={.kind=kind,.operation=WEB_STORAGE_ACK,.source=token,.cursor=event->cursor};
    struct web_storage_result out={0};bool ok=webstorage_access(s,site,&r,&out)==WEB_STORAGE_OK;
    webstorage_result_free(event);return ok;
}
int main(void){
    char path[160];for(int i=0;i<2;i++){storage_path(site,i,path);_unlink(path);}cold_shared(site);
    reset(F_NONE);webstorage *a=webstorage_create(),*b=webstorage_create();struct web_storage_result out={0};
    verify(call_store(a,11,0,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0,"writer-subscribe");
    verify(call_store(b,22,0,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0,"reader-subscribe");
    bool writes_ok=true;for(int i=0;i<2048;i++){char text[32];snprintf(text,sizeof text,"%d",i);
        if(call_store(a,11,0,WEB_STORAGE_SET,"sequence",text,&out)!=0)writes_ok=false;}
    struct storage_control h={0};storage_control_read(a->areas,&h);uint64_t epoch=h.epoch;
    verify(writes_ok&&epoch==1,"lagging-reader-prohibits-compaction");
    bool events_ok=true;for(int i=0;i<2048;i++){
        char text[32],old[32];snprintf(text,sizeof text,"%d",i);snprintf(old,sizeof old,"%d",i-1);
        int status=call_store(b,22,0,WEB_STORAGE_POLL,NULL,NULL,&out);
        if(status || !out.event || !out.event_key || strcmp(out.event_key,"sequence") || !out.new_value || strcmp(out.new_value,text) ||
            (i?(!out.old_value||strcmp(out.old_value,old)):out.old_value!=NULL))events_ok=false;
        if(!acknowledge(b,22,0,&out))events_ok=false;
    }
    verify(events_ok,"lagged-event-order-old-new-values-preserved");
    verify(call_store(a,11,0,WEB_STORAGE_POLL,NULL,NULL,&out)==0&&!out.event,"writer-skips-own-events");
    verify(call_store(a,11,0,WEB_STORAGE_SET,"sequence","after-checkpoint",&out)==0,"post-drain-mutation");
    storage_control_read(a->areas,&h);verify(h.epoch==epoch+1,"consumed-log-checkpointed");
    storage_path_at(WEBSTORAGE_SHARED_ROOT,site,4,path);struct n_stat delta_stat={0},control_stat={0};
    verify(probe_stat(path,&delta_stat)==0 && delta_stat.size<1024,"checkpoint-returns-physical-delta-capacity");
    storage_path_at(WEBSTORAGE_SHARED_ROOT,site,2,path);
    verify(probe_stat(path,&control_stat)==0 && control_stat.size==STORAGE_CONTROL_BYTES,"lease-control-vnode-stays-fixed-size");
    verify(call_store(b,22,0,WEB_STORAGE_POLL,NULL,NULL,&out)==0&&out.event&&out.old_value&&!strcmp(out.old_value,"2047")&&
        out.new_value&&!strcmp(out.new_value,"after-checkpoint"),"epoch-change-keeps-next-event");acknowledge(b,22,0,&out);
    char *large=malloc(1024*1024+1);memset(large,'x',1024*1024);large[1024*1024]=0;
    int accepted=0,result=0;char last=0;
    for(int i=0;i<40;i++){large[0]=(char)('A'+i%26);result=call_store(a,11,0,WEB_STORAGE_SET,"large",large,&out);
        if(result)break;accepted++;last=large[0];}
    storage_control_read(a->areas,&h);uint64_t revision=h.revision;
    large[0]='!';int rejected=call_store(a,11,0,WEB_STORAGE_SET,"large",large,&out);
    struct storage_control unchanged={0};storage_control_read(a->areas,&unchanged);
    verify(result==WEB_STORAGE_QUOTA&&accepted>1&&rejected==WEB_STORAGE_QUOTA&&unchanged.revision==revision,"bounded-lag-backlog-rejects-before-commit");
    verify(call_store(a,11,0,WEB_STORAGE_GET,"large",NULL,&out)==0&&out.text&&out.text_len==1024*1024&&out.text[0]==last,"quota-keeps-old-map");webstorage_result_free(&out);free(large);
    verify(call_store(a,101,1,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0&&call_store(a,102,1,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0,"session-realm-subscriptions");
    verify(call_store(a,101,1,WEB_STORAGE_SET,"session","shared",&out)==0,"session-mutation");
    verify(call_store(a,102,1,WEB_STORAGE_POLL,NULL,NULL,&out)==0&&out.event&&out.new_value&&!strcmp(out.new_value,"shared"),"session-other-realm-event");acknowledge(a,102,1,&out);
    verify(call_store(a,101,1,WEB_STORAGE_POLL,NULL,NULL,&out)==0&&!out.event,"session-source-excluded");
    webstorage_free(b);webstorage_free(a);
    cold_shared(site);for(int i=0;i<2;i++){storage_path(site,i,path);_unlink(path);}
    printf("storage-shared-native: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
