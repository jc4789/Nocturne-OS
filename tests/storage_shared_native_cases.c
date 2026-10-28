/* Actual shared backend with isolated native-file adapter. Reader lag and
 * checkpoint epochs are exercised independently of the real OS window test,
 * then tabs (sessionStorage namespaces the host names), the idle localStorage
 * areas leaving RAM, the user agent's clears and the settings' usage. */
#define main previous_storage_snapshot_main
#include "storage_snapshot_native_cases.c"
#undef main
static const char *site="https://storage-shared-native.test";
static int call_at(webstorage *s,uint64_t tab,const char *origin,uint64_t source,int kind,int op,const char *key,const char *value,struct web_storage_result *out){
    struct web_storage_request r={.kind=kind,.operation=op,.source=source,.key=key,.key_len=key?strlen(key):0,
        .value=value,.value_len=value?strlen(value):0,.url="https://storage-shared-native.test/source"};
    return webstorage_access(s,tab,origin,&r,out);
}
static uint64_t tab_a; /* the tab of store a that the session cases use */
static int call_store(webstorage *s,uint64_t source,int kind,int op,const char *key,const char *value,struct web_storage_result *out){
    return call_at(s,tab_a,site,source,kind,op,key,value,out);
}
static bool acknowledge_at(webstorage *s,uint64_t tab,const char *origin,uint64_t token,int kind,struct web_storage_result *event){
    struct web_storage_request r={.kind=kind,.operation=WEB_STORAGE_ACK,.source=token,.cursor=event->cursor};
    struct web_storage_result out={0};bool ok=webstorage_access(s,tab,origin,&r,&out)==WEB_STORAGE_OK;
    webstorage_result_free(event);return ok;
}
static bool acknowledge(webstorage *s,uint64_t token,int kind,struct web_storage_result *event){return acknowledge_at(s,tab_a,site,token,kind,event);}
static bool get_is(webstorage *s,uint64_t tab,const char *origin,int kind,const char *key,const char *value){
    struct web_storage_result out={0};int status=call_at(s,tab,origin,0,kind,WEB_STORAGE_GET,key,NULL,&out);
    bool ok=status==WEB_STORAGE_OK&&(value?out.text&&!strcmp(out.text,value):!out.text);webstorage_result_free(&out);return ok;
}
static bool event_is(webstorage *s,uint64_t tab,const char *origin,uint64_t token,int kind,const char *key,const char *value){
    struct web_storage_result out={0};int status=call_at(s,tab,origin,token,kind,WEB_STORAGE_POLL,NULL,NULL,&out);
    bool ok=status==WEB_STORAGE_OK&&out.event&&(key?out.event_key&&!strcmp(out.event_key,key):!out.event_key)&&
        (value?out.new_value&&!strcmp(out.new_value,value):!out.new_value);
    if(out.event)acknowledge_at(s,tab,origin,token,kind,&out);else webstorage_result_free(&out);
    return ok;
}
static bool no_event(webstorage *s,uint64_t tab,const char *origin,uint64_t token,int kind){
    struct web_storage_result out={0};int status=call_at(s,tab,origin,token,kind,WEB_STORAGE_POLL,NULL,NULL,&out);
    bool ok=status==WEB_STORAGE_OK&&!out.event;webstorage_result_free(&out);return ok;
}
static bool present(const char *path){struct n_entry e;return n_entry_info(path,&e)>=0;}
static unsigned live_tabs(webstorage *s){unsigned n=0;for(struct storage_space *p=s->spaces;p;p=p->next)n++;return n;}
static void journal_cases(void){
    char path[STORAGE_PATH];cold_all(site);
    reset(F_NONE);webstorage *a=webstorage_create(),*b=webstorage_create();struct web_storage_result out={0};
    tab_a=webstorage_session_open(a);
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
    struct n_entry delta={0},control={0};
    storage_path_at(WEBSTORAGE_SHARED_ROOT,site,4,path);verify(n_entry_info(path,&delta)>=0 && delta.size<1024,"checkpoint-returns-physical-delta-capacity");
    storage_path_at(WEBSTORAGE_SHARED_ROOT,site,2,path);verify(n_entry_info(path,&control)>=0 && control.size==STORAGE_CONTROL_BYTES,"lease-control-vnode-stays-fixed-size");
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
    webstorage_free(b);webstorage_free(a);cold_all(site);verify(no_claims(),"journal-no-claim-outlives-its-operation");
}
/* Tabs of one window: sessionStorage per tab, named by the host; localStorage and its events
 * shared by every tab; a closed tab gives back its areas, sources and queued event bytes. */
static void tab_cases(void){
    const char *origin="https://storage-tabs.test";cold_all(origin);
    webstorage *w=webstorage_create();struct web_storage_result out={0};reset(F_NONE);
    uint64_t t1=webstorage_session_open(w),t2=webstorage_session_open(w);
    verify(t1&&t2&&t1!=t2&&live_tabs(w)==2,"two-tab-namespaces");
    verify(call_at(w,0,origin,0,1,WEB_STORAGE_CHECK,NULL,NULL,&out)==WEB_STORAGE_SECURITY&&
           call_at(w,99,origin,0,1,WEB_STORAGE_CHECK,NULL,NULL,&out)==WEB_STORAGE_SECURITY,"session-needs-a-live-tab");
    verify(call_at(w,0,origin,0,0,WEB_STORAGE_CHECK,NULL,NULL,&out)==0,"local-needs-no-tab");
    verify(call_at(w,t1,origin,301,1,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0&&call_at(w,t1,origin,302,1,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0&&
           call_at(w,t2,origin,303,1,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0,"session-realms-in-two-tabs");
    verify(call_at(w,t1,origin,301,1,WEB_STORAGE_SET,"k","one",&out)==0,"tab-one-session-write");
    verify(get_is(w,t1,origin,1,"k","one")&&get_is(w,t2,origin,1,"k",NULL),"session-isolated-between-tabs");
    verify(event_is(w,t1,origin,302,1,"k","one")&&no_event(w,t2,origin,303,1)&&no_event(w,t1,origin,301,1),"session-event-only-in-its-tab");
    verify(call_at(w,t2,origin,303,1,WEB_STORAGE_SET,"k","two",&out)==0&&get_is(w,t1,origin,1,"k","one")&&no_event(w,t1,origin,302,1),"other-tab-write-invisible");
    /* localStorage: one area, every tab's realms hear each other. */
    verify(call_at(w,t1,origin,401,0,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0&&call_at(w,t2,origin,402,0,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0,"local-realms-in-two-tabs");
    verify(call_at(w,t1,origin,401,0,WEB_STORAGE_SET,"shared","yes",&out)==0&&get_is(w,t2,origin,0,"shared","yes"),"local-shared-between-tabs");
    verify(event_is(w,t2,origin,402,0,"shared","yes")&&no_event(w,t1,origin,401,0),"local-event-reaches-other-tab");
    /* Events queued and not taken in tab 1 go with it. */
    verify(call_at(w,t1,origin,301,1,WEB_STORAGE_SET,"queued","1",&out)==0&&call_at(w,t1,origin,301,1,WEB_STORAGE_SET,"queued","2",&out)==0,"events-queued-in-tab-one");
    struct storage_space *one=storage_space_find(w,t1),*two=storage_space_find(w,t2);
    verify(one&&one->event_bytes>0&&two&&two->event_bytes==0,"event-bytes-counted-per-tab");
    webstorage_session_close(w,t1);
    uint64_t reopened=webstorage_session_open(w);
    verify(live_tabs(w)==2&&reopened>t2&&reopened!=t1&&!storage_space_find(w,t1),"closed-tab-leaves-live-count-not-made-count");
    webstorage_session_close(w,reopened);
    unsigned sources=0;for(struct storage_source *p=w->sources;p;p=p->next){sources++;if(p->token==301||p->token==302)sources+=100;}
    verify(sources==3,"closed-tab-sources-dropped"); /* 303 (tab 2's session), 401 and 402 (local) */
    verify(call_at(w,t1,origin,0,1,WEB_STORAGE_GET,"k",NULL,&out)==WEB_STORAGE_SECURITY,"closed-tab-session-gone");
    verify(get_is(w,t2,origin,1,"k","two")&&get_is(w,t2,origin,0,"shared","yes"),"other-tab-keeps-its-session-and-local");
    uint64_t t3=webstorage_session_open(w);
    verify(t3>reopened&&get_is(w,t3,origin,1,"k",NULL),"new-tab-new-number-empty");
    struct web_storage_request release={.operation=WEB_STORAGE_RELEASE,.source=401};
    verify(webstorage_access(w,0,NULL,&release,&out)==0,"realm-release-without-tab");
    webstorage_free(w);cold_all(origin);
}
/* A localStorage area no document used for STORAGE_IDLE_MS leaves RAM; its value stays. One with a
 * reader of its events, or with unsaved changes, stays. */
static void idle_cases(void){
    const char *idle="https://storage-idle.test",*held="https://storage-held.test";cold_all(idle);cold_all(held);
    webstorage *w=webstorage_create();struct web_storage_result out={0};reset(F_NONE);
    verify(call_at(w,0,idle,0,0,WEB_STORAGE_SET,"k","v",&out)==0&&call_at(w,0,held,501,0,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0,"idle-fixture");
    verify(webstorage_flush(w,true,&out)==0,"idle-flushed");
    webstorage_flush(w,false,&out); /* a sweep marks what was used */
    verify(storage_area_find(w,NULL,idle)&&storage_area_find(w,NULL,held),"recent-areas-stay");
    clock_ms+=STORAGE_IDLE_MS+1;webstorage_flush(w,false,&out);
    verify(!storage_area_find(w,NULL,idle)&&storage_area_find(w,NULL,held),"idle-area-leaves-reader-stays");
    verify(get_is(w,0,idle,0,"k","v")&&storage_area_find(w,NULL,idle),"idle-area-read-again");
    verify(call_at(w,0,idle,0,0,WEB_STORAGE_SET,"k","w",&out)==0,"unsaved-change");
    clock_ms+=STORAGE_IDLE_MS+1;
    { struct storage_area *a=storage_area_find(w,NULL,idle);bool dirty=a&&a->dirty;storage_sweep(w);
      verify(dirty&&storage_area_find(w,NULL,idle),"unsaved-area-stays"); }
    webstorage_free(w);
    w=webstorage_create();verify(get_is(w,0,idle,0,"k","w"),"unsaved-change-saved-at-close");webstorage_free(w);
    cold_all(idle);cold_all(held);
}
/* The user agent clears a site: its origins of every scheme and port and the names inside it,
 * saved or not, read in RAM or not, in this window, another window and their tabs; no event. */
static void clear_cases(void){
    const char *cleared[]={"https://clear.test","https://a.clear.test","http://b.clear.test:8080","https://disk.clear.test"};
    const char *kept[]={"https://notclear.test","https://keep.test","https://10.0.0.1","https://test"};
    for(int i=0;i<4;i++){cold_all(cleared[i]);cold_all(kept[i]);}
    char clears[STORAGE_PATH];storage_clears_path(clears);_unlink(clears);
    webstorage *a=webstorage_create(),*b=webstorage_create();struct web_storage_result out={0};reset(F_NONE);
    n_handle queue=n_events_create();
    verify(webstorage_watch(b,queue)==0&&b->clears>=0&&b->clears_seen==0,"other-window-watches-clears");
    uint64_t ta=webstorage_session_open(a),tb=webstorage_session_open(b);
    for(int i=0;i<4;i++){
        verify(call_at(a,ta,cleared[i],0,0,WEB_STORAGE_SET,"k","v",&out)==0&&call_at(a,ta,kept[i],0,0,WEB_STORAGE_SET,"k","v",&out)==0,"clear-local-fixture");
        verify(call_at(a,ta,cleared[i],0,1,WEB_STORAGE_SET,"s","v",&out)==0&&call_at(b,tb,cleared[i],0,1,WEB_STORAGE_SET,"s","v",&out)==0&&
               call_at(a,ta,kept[i],0,1,WEB_STORAGE_SET,"s","v",&out)==0,"clear-session-fixture");
    }
    verify(webstorage_flush(a,true,&out)==0,"clear-fixture-saved");
    /* disk.clear.test: saved, and in no window's RAM, nor in the shared root (a later start). */
    for(struct storage_area **link=&a->areas;*link;)if(!strcmp((*link)->origin,cleared[3])){struct storage_area *x=*link;*link=x->next;storage_area_free(x);}else link=&(*link)->next;
    cold_shared(cleared[3]);
    verify(call_at(a,ta,cleared[1],601,0,WEB_STORAGE_SUBSCRIBE,NULL,NULL,&out)==0,"clear-reader-fixture");
    struct web_storage_usage *usage=NULL;long n=webstorage_usage(a,&usage);bool seen=false,disk_only=false;
    for(long i=0;i<n;i++){
        if(!strcmp(usage[i].origin,cleared[0]))seen=usage[i].local==4&&usage[i].session==4&&usage[i].disk>0;
        if(!strcmp(usage[i].origin,cleared[3]))disk_only=usage[i].local==4&&usage[i].session==4&&usage[i].disk>0;
    }
    bool sorted=true;for(long i=1;i<n;i++)if(strcmp(usage[i-1].origin,usage[i].origin)>=0)sorted=false;
    verify(n>=8&&seen&&disk_only&&sorted,"usage-local-session-disk-sorted");
    verify(!storage_area_find(a,NULL,cleared[3]),"usage-reads-without-keeping");
    webstorage_usage_free(usage,(size_t)n);
    verify(webstorage_clear_site(a,"https://clear.test")==WEB_STORAGE_SECURITY&&webstorage_clear_site(a,"")==WEB_STORAGE_SECURITY,"clear-takes-a-host");
    verify(webstorage_clear_site(a,"clear.test")==WEB_STORAGE_OK,"clear-site");
    bool gone=true,kept_ok=true;char path[STORAGE_PATH];
    for(int i=0;i<4;i++){
        if(!get_is(a,ta,cleared[i],0,"k",NULL)||!get_is(b,tb,cleared[i],0,"k",NULL)||!get_is(a,ta,cleared[i],1,"s",NULL))gone=false;
        for(int slot=0;slot<2;slot++){storage_path(cleared[i],slot,path);if(present(path))gone=false;}
        if(!get_is(a,ta,kept[i],0,"k","v")||!get_is(b,tb,kept[i],0,"k","v")||!get_is(a,ta,kept[i],1,"s","v"))kept_ok=false;
    }
    verify(gone,"site-local-every-window-and-disk-session-this-window");
    verify(kept_ok,"other-sites-names-ip-kept");
    verify(no_event(a,ta,cleared[1],601,0),"user-agent-clear-fires-no-event");
    verify(get_is(b,tb,cleared[0],1,"s","v"),"other-window-session-before-its-event");
    struct n_event event={.token=WEBSTORAGE_TOKEN,.events=N_EVENT_RECEIVABLE};
    webstorage_events(b,&event,1);
    bool b_gone=true;for(int i=0;i<4;i++)if(!get_is(b,tb,cleared[i],1,"s",NULL))b_gone=false;
    verify(b_gone&&get_is(b,tb,kept[0],1,"s",NULL)&&b->clears_seen>0,"other-window-session-cleared-by-event");
    verify(call_at(b,tb,cleared[0],0,1,WEB_STORAGE_SET,"s","after",&out)==0,"write-after-clear");
    watch_changes=1;webstorage_events(b,&event,1);
    verify(get_is(b,tb,cleared[0],1,"s","after"),"seen-clear-not-applied-twice");
    verify(call_at(a,ta,cleared[0],0,0,WEB_STORAGE_SET,"k","again",&out)==0&&webstorage_flush(a,true,&out)==0&&get_is(b,tb,cleared[0],0,"k","again"),"cleared-site-usable-again");
    verify(webstorage_clear_site(a,"10.0.0.1")==0&&get_is(a,ta,kept[2],0,"k",NULL)&&get_is(a,ta,kept[1],0,"k","v"),"clear-an-address-exactly");
    verify(webstorage_clear_all(a)==0,"clear-all");
    webstorage_events(b,&event,1);
    bool all=true;for(int i=0;i<4;i++)if(!get_is(a,ta,kept[i],0,"k",NULL)||!get_is(b,tb,kept[i],0,"k",NULL)||!get_is(a,ta,kept[i],1,"s",NULL)||
        !get_is(b,tb,cleared[i],1,"s",NULL))all=false;
    verify(all,"clear-all-every-origin-and-tab");
    usage=NULL;n=webstorage_usage(a,&usage);bool none=n>=0;
    for(long i=0;i<n;i++)for(int j=0;j<4;j++)if(!strcmp(usage[i].origin,cleared[j])||!strcmp(usage[i].origin,kept[j]))none=false;
    webstorage_usage_free(usage,n<0?0:(size_t)n);verify(none,"usage-after-clear-all-is-empty");
    verify(no_claims(),"clear-no-claim-outlives-its-operation");
    webstorage_free(b);webstorage_free(a);n_handle_close(queue);
    for(int i=0;i<4;i++){cold_all(cleared[i]);cold_all(kept[i]);}
    _unlink(clears);
}
int main(void){
    folders();
    journal_cases();
    tab_cases();
    idle_cases();
    clear_cases();
    printf("storage-shared-native: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
