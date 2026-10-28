/* Actual browser binding/backend plus native cross-process and vnode leases.
 * A store is a browser window; its tabs are webstorage_session_open namespaces that the host,
 * never page code, names in each request. This focused fixture supplements the separate
 * real-site GUI acceptance. */
#include <nocturne.h>
#include <web.h>
#include <webstorage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks,failures,errors,ready,host_calls;
static char origin[128],domain[128];
static void check(bool pass,const char *name){checks++;if(!pass)failures++;printf("%s %s\n",pass?"OK":"FAIL",name);}
static int access(webstorage *s,int op,const char *key,const char *value,struct web_storage_result *out){
    struct web_storage_request r={.kind=WEB_STORAGE_LOCAL,.operation=op,.key=key,.key_len=key?strlen(key):0,
        .value=value,.value_len=value?strlen(value):0,.url="https://storage-native.test/writer"};
    return webstorage_access(s,0,origin,&r,out);
}
static int session_access(webstorage *s,uint64_t tab,const char *site,int op,const char *key,const char *value,struct web_storage_result *out){
    struct web_storage_request r={.kind=WEB_STORAGE_SESSION,.operation=op,.key=key,.key_len=key?strlen(key):0,
        .value=value,.value_len=value?strlen(value):0,.url="https://storage-native.test/writer"};
    return webstorage_access(s,tab,site,&r,out);
}
static bool value_is(webstorage *s,const char *key,const char *value){
    struct web_storage_result out={0};int status=access(s,WEB_STORAGE_GET,key,NULL,&out);
    bool ok=status==WEB_STORAGE_OK && (value?out.text && !strcmp(out.text,value):!out.text);webstorage_result_free(&out);return ok;
}
static bool session_is(webstorage *s,uint64_t tab,const char *site,const char *key,const char *value){
    struct web_storage_result out={0};int status=session_access(s,tab,site,WEB_STORAGE_GET,key,NULL,&out);
    bool ok=status==WEB_STORAGE_OK && (value?out.text && !strcmp(out.text,value):!out.text);webstorage_result_free(&out);return ok;
}
/* A tab of a window, as the browser's host callback sees it: the window's store and the tab's
 * sessionStorage. */
struct tab { webstorage *store; uint64_t session; };
static int host_storage(void *opaque,const char *site,const struct web_storage_request *r,struct web_storage_result *out){
    struct tab *t=opaque;host_calls++;
    return webstorage_access(t->store,t->session,site,r,out);
}
static void console(void *opaque,int level,const char *text){
    if(!strcmp(text,"STORAGE-READY"))ready++;
    else if(!strncmp(text,"OK ",3)){checks++;printf("%s\n",text);}
    else if(!strncmp(text,"FAIL ",5)){checks++;failures++;printf("%s\n",text);}
    else if(level==2){errors++;printf("ERROR %s\n",text);}
}
static const char page[]="<!doctype html><script>"
    "void localStorage;void sessionStorage;"
    "let received=[];function ck(p,n){console.log((p?'OK ':'FAIL ')+n);}"
    "onstorage=e=>{received.push(e);ck(e instanceof StorageEvent&&e.isTrusted&&!e.bubbles&&!e.cancelable&&e.target===window,'trusted-StorageEvent');"
    "const k=e.key;try{e.key='forged';}catch(x){}ck(e.key===k,'readonly-storage-fields');};"
    "console.log('STORAGE-READY');</script><p>storage window</p>";
static web_doc *document(struct tab *tab,const char *suffix){
    char url[192];snprintf(url,sizeof url,"%s/%s",origin,suffix);
    struct web_host host={.opaque=tab,.storage=host_storage,.storage_events=true,.console=console,.js_task_budget_ms=5000};
    return web_live(page,sizeof page-1,url,"utf-8",&host);
}
static void pump(web_doc *a,web_doc *b,web_doc *c,web_doc *d,unsigned ms){
    uint64_t end=uptime_ms()+ms;
    while(uptime_ms()<end){uint64_t now=uptime_ms();if(a)web_tick(a,now);if(b)web_tick(b,now);if(c)web_tick(c,now);if(d)web_tick(d,now);n_sleep_ms(1);}
}
static void eval(web_doc *d,const char *code){web_console_eval(d,code,strlen(code));}
static volatile uint32_t lease_stage;
static char lease_path[1024];
static void lease_waiter(void *unused){
    /* This x86-64/TCC fixture exchanges only an aligned volatile stage word;
     * no payload publication or unsupported compiler atomic builtins. */
    n_handle fd=n_document_open(lease_path,N_DOCUMENT_EDIT);lease_stage=1;
    if(fd>=0 && n_document_claim(fd,true)==0){lease_stage=2;n_document_release(fd);n_handle_close(fd);}
    else lease_stage=3;
}
static bool run_child(char **argv,const char *mode,const char *argument){
    const char *child_argv[]={argv[0],mode,argument};
    struct n_launch launch={.arguments=child_argv,.argument_count=3};
    n_handle child=n_app_launch(argv[0],&launch);int status=-1;
    bool ok=child>=0&&n_app_result(child,-1,&status)>=0&&status==0;
    if(child>=0)n_handle_close(child);
    return ok;
}
/* The usage the settings would show for site: its local and session bytes. */
static bool usage_of(webstorage *s,const char *site,uint64_t *local,uint64_t *session){
    struct web_storage_usage *usage=NULL;long n=webstorage_usage(s,&usage);*local=*session=0;
    for(long i=0;i<n;i++)if(!strcmp(usage[i].origin,site)){*local=usage[i].local;*session=usage[i].session;}
    webstorage_usage_free(usage,n<0?0:(size_t)n);return n>=0;
}
int main(int argc,char **argv){
    char program[4096]; /* argv[0] is as it was typed; an app is launched by its complete path */
    if(n_path_complete(argv[0],program,sizeof program)>=0)argv[0]=program;
    if(argc==3 && !strcmp(argv[1],"--writer")){
        strcpy(origin,argv[2]);webstorage *s=webstorage_create();struct web_storage_result out={0};
        int result=access(s,WEB_STORAGE_SET,"process-key","child",&out);webstorage_free(s);return result!=WEB_STORAGE_OK;
    }
    if(argc==3 && !strcmp(argv[1],"--clear")){ /* the Settings window, another program */
        webstorage *s=webstorage_create();int result=webstorage_clear_site(s,argv[2]);webstorage_free(s);return result!=WEB_STORAGE_OK;
    }
    snprintf(origin,sizeof origin,"https://storage-window-%d.test",(int)n_app_self());
    snprintf(domain,sizeof domain,"storage-window-%d.test",(int)n_app_self());
    char kept[128];snprintf(kept,sizeof kept,"https://storage-kept-%d.test",(int)n_app_self());
    char lease_leaf[48];snprintf(lease_leaf,sizeof lease_leaf,"storage-lease-%d",(int)n_app_self());
    check(n_path_join(n_temp_folder(),lease_leaf,lease_path,sizeof lease_path)>0,"lease-path-in-temporary-folder");
    n_entry_remove(lease_path);
    n_handle lease=n_document_open(lease_path,N_DOCUMENT_CREATE);check(lease>=0&&n_document_claim(lease,false)==0,"lease-owner-lock");
    n_handle other=n_document_open(lease_path,N_DOCUMENT_EDIT);check(other>=0&&n_document_release(other)<0,"lease-other-handle-cannot-release");if(other>=0)n_handle_close(other);
    int tid=thread_create(lease_waiter,NULL);check(tid>0,"lease-waiter-created");
    uint64_t deadline=uptime_ms()+1000;while(!lease_stage&&uptime_ms()<deadline)n_sleep_ms(1);
    n_sleep_ms(20);check(lease_stage==1,"lease-separate-open-waits");n_handle_close(lease);
    deadline=uptime_ms()+1000;while(lease_stage==1&&uptime_ms()<deadline)n_sleep_ms(1);
    check(lease_stage==2,"lease-last-close-releases");if(lease_stage!=1&&tid>0)thread_join(tid);n_entry_remove(lease_path);
    webstorage *a=webstorage_create(),*b=webstorage_create();struct web_storage_result out={0};
    n_handle queue=n_events_create();
    check(queue>=0&&webstorage_watch(a,queue)>=0,"window-watches-other-programs-clears");
    struct tab a1={a,webstorage_session_open(a)},a2={a,webstorage_session_open(a)},b1={b,webstorage_session_open(b)};
    check(a1.session&&a2.session&&a1.session!=a2.session&&b1.session,"host-made-tab-namespaces");
    check(access(a,WEB_STORAGE_CLEAR,NULL,NULL,&out)==WEB_STORAGE_OK,"native-initial-clear");
    check(access(a,WEB_STORAGE_SET,"parent-key","kept",&out)==WEB_STORAGE_OK,"parent-unflushed-write");
    check(run_child(argv,"--writer",origin),"cross-process-writer-completed");
    check(value_is(a,"parent-key","kept")&&value_is(a,"process-key","child"),"cross-process-rebase-keeps-both-keys");
    check(access(a,WEB_STORAGE_SET,"flush-key","pending",&out)==WEB_STORAGE_OK,"stale-window-pending-save");
    check(access(b,WEB_STORAGE_SET,"other-key","kept",&out)==WEB_STORAGE_OK,"second-window-distinct-key");
    check(webstorage_flush(a,true,&out)==WEB_STORAGE_OK&&value_is(b,"other-key","kept")&&value_is(b,"process-key","child"),"flush-rebases-latest-shared-map");
    check(access(a,WEB_STORAGE_CLEAR,NULL,NULL,&out)==WEB_STORAGE_OK,"fixture-clear");
    /* sessionStorage is the tab's: the host's number selects it, nothing in the page does. */
    check(session_access(a,a1.session,origin,WEB_STORAGE_SET,"tab","one",&out)==WEB_STORAGE_OK&&
          session_is(a,a1.session,origin,"tab","one")&&session_is(a,a2.session,origin,"tab",NULL),"session-per-tab-in-one-window");
    check(session_access(a,0,origin,WEB_STORAGE_CHECK,NULL,NULL,&out)==WEB_STORAGE_SECURITY&&
          session_access(a,b1.session,origin,WEB_STORAGE_CHECK,NULL,NULL,&out)==WEB_STORAGE_SECURITY,"session-needs-a-tab-of-this-window");
    check(session_access(a,a1.session,origin,WEB_STORAGE_CLEAR,NULL,NULL,&out)==WEB_STORAGE_OK,"session-fixture-clear");
    struct tab unused_tab={a,a1.session};
    const char *unused_page="<!doctype html><p>Storage is not accessed</p>";
    int before=host_calls;web_doc *unused=web_live(unused_page,strlen(unused_page),"https://storage-unused.test/","utf-8",
        &(struct web_host){.opaque=&unused_tab,.storage=host_storage,.storage_events=true,.console=console});
    pump(unused,NULL,NULL,NULL,120);check(unused&&host_calls==before,"unused-storage-has-no-backend-io-or-poll");web_free(unused);
    const char *quiet_page="<!doctype html><script>localStorage.quiet='1';const f=()=>{};addEventListener('storage',f);removeEventListener('storage',f);</script>"
        "<p>Storage without a storage listener</p>";
    before=host_calls;web_doc *quiet=web_live(quiet_page,strlen(quiet_page),"https://storage-quiet.test/","utf-8",
        &(struct web_host){.opaque=&unused_tab,.storage=host_storage,.storage_events=true,.console=console});
    pump(quiet,NULL,NULL,NULL,250);int loaded=host_calls;pump(quiet,NULL,NULL,NULL,350);
    check(quiet&&loaded>before&&host_calls==loaded,"storage-without-listener-has-no-poll");web_free(quiet);
    web_doc *da=document(&a1,"a"),*db=document(&b1,"b");check(da&&db,"live-window-documents");pump(da,db,NULL,NULL,250);check(ready==2,"both-author-listeners-ready");
    const char *first="localStorage.k='old';localStorage.k='old';localStorage.removeItem('absent');";
    check(web_console_eval(da,first,strlen(first)),"first-author-update");
    pump(da,db,NULL,NULL,150);
    const char *bwrite="ck(localStorage.k==='old','other-window-immediate-map');localStorage.other='kept';";
    check(web_console_eval(db,bwrite,strlen(bwrite)),"second-author-update");
    const char *awrite="localStorage.k='new';";check(web_console_eval(da,awrite,strlen(awrite)),"following-author-update");
    pump(da,db,NULL,NULL,250);
    eval(db,"ck(received.length===2&&received[0].key==='k'&&received[0].oldValue===null&&received[0].newValue==='old'&&received[1].oldValue==='old'&&received[1].newValue==='new'&&received[0].url===location.origin+'/a'&&received[0].storageArea===localStorage,'remote-event-order-values-source-area');");
    eval(da,"ck(received.length===1&&received[0].key==='other','source-excluded-from-own-events');");
    eval(da,"localStorage.removeItem('k');localStorage.clear();localStorage.clear();");pump(da,db,NULL,NULL,200);
    eval(db,"ck(received.length===4&&received[2].key==='k'&&received[2].oldValue==='new'&&received[2].newValue===null&&received[3].key===null&&received[3].oldValue===null&&received[3].newValue===null,'remove-clear-null-values-empty-clear-no-event');");
    /* dc is another realm of da's tab (a frame); dt a document of another tab of the same window. */
    web_doc *dc=document(&a1,"same-tab-frame"),*dt=document(&a2,"other-tab");pump(da,db,dc,dt,200);
    eval(da,"sessionStorage.s='session';");pump(da,db,dc,dt,200);
    eval(dc,"ck(received.length===1&&received[0].storageArea===sessionStorage&&sessionStorage.s==='session','session-same-tab-realm-event');");
    eval(db,"ck(sessionStorage.s===undefined&&received.length===4,'session-excludes-other-window');");
    eval(dt,"ck(sessionStorage.s===undefined&&received.length===0,'session-excludes-other-tab-of-the-window');");
    eval(dt,"sessionStorage.s='second tab';");pump(da,db,dc,dt,200);
    eval(da,"ck(sessionStorage.s==='session'&&received.length===1,'other-tab-session-write-no-event-no-change');");
    eval(da,"localStorage.retired='current';");pump(da,db,dc,dt,200);
    eval(dt,"ck(received.length===1&&received[0].key==='retired'&&received[0].newValue==='current'&&received[0].storageArea===localStorage&&localStorage.retired==='current','local-event-reaches-other-tab-of-the-window');");
    eval(dc,"ck(received.length===2&&received[1].key==='retired'&&received[1].storageArea===localStorage,'local-event-reaches-same-tab-realm');");
    web_free(db);db=document(&b1,"replacement");pump(da,db,dc,dt,200);
    eval(db,"ck(received.length===0&&localStorage.retired==='current','replacement-document-has-no-old-events');");
    /* Closing a tab takes its sessionStorage; a new tab starts empty. */
    web_free(dt);dt=NULL;uint64_t closed=a2.session;webstorage_session_close(a,closed);
    check(session_access(a,closed,origin,WEB_STORAGE_GET,"s",NULL,&out)==WEB_STORAGE_SECURITY,"closed-tab-session-is-gone");
    a2.session=webstorage_session_open(a);
    check(a2.session&&a2.session!=closed&&session_is(a,a2.session,origin,"s",NULL)&&session_is(a,a1.session,origin,"s","session"),"new-tab-number-and-empty-session");
    /* The Settings window (another program) clears the site: this window's tabs and documents read
     * empty maps, with no storage event; another site keeps its data. */
    check(session_access(a,a1.session,kept,WEB_STORAGE_SET,"keep","yes",&out)==WEB_STORAGE_OK,"other-site-session");
    struct web_storage_request keep_local={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_SET,.key="keep",.key_len=4,.value="yes",.value_len=3};
    check(webstorage_access(a,0,kept,&keep_local,&out)==WEB_STORAGE_OK,"other-site-local");
    uint64_t local=0,session=0;
    check(usage_of(a,origin,&local,&session)&&local>0&&session==2*(1+7),"usage-counts-local-and-tab-session");
    eval(da,"window.beforeClear=received.length;");
    check(run_child(argv,"--clear",domain),"other-program-clears-site");
    struct n_event events[8];n_result n=n_events_wait(queue,events,8,2000);
    check(n>0,"clear-reaches-window-queue");if(n>0)webstorage_events(a,events,(size_t)n);
    pump(da,db,dc,NULL,250);
    eval(da,"ck(sessionStorage.length===0&&localStorage.length===0&&received.length===window.beforeClear,'user-agent-clear-empty-maps-no-event');");
    eval(dc,"ck(sessionStorage.s===undefined&&localStorage.retired===undefined,'user-agent-clear-every-realm');");
    check(session_is(a,a1.session,kept,"keep","yes")&&value_is(b,"retired",NULL),"clear-keeps-other-site-reaches-other-window");
    struct web_storage_result kept_value={0};struct web_storage_request get_kept={.kind=WEB_STORAGE_LOCAL,.operation=WEB_STORAGE_GET,.key="keep",.key_len=4};
    check(webstorage_access(b,0,kept,&get_kept,&kept_value)==WEB_STORAGE_OK&&kept_value.text&&!strcmp(kept_value.text,"yes"),"clear-keeps-other-site-local");webstorage_result_free(&kept_value);
    check(usage_of(a,origin,&local,&session)&&!local&&!session,"usage-after-clear-is-empty");
    check(webstorage_clear_site(a,kept)==WEB_STORAGE_SECURITY,"clear-takes-a-host-not-an-origin");
    check(errors==0,"no-browser-JS-exceptions");web_free(dc);web_free(db);web_free(da);
    check(webstorage_clear_site(a,domain)==WEB_STORAGE_OK,"fixture-cleanup");
    char kept_domain[128];snprintf(kept_domain,sizeof kept_domain,"storage-kept-%d.test",(int)n_app_self());
    check(webstorage_clear_site(a,kept_domain)==WEB_STORAGE_OK,"fixture-cleanup-kept");
    webstorage_free(b);webstorage_free(a);n_handle_close(queue);
    printf("STORAGE-WINDOW-DONE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
