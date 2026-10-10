/* Actual browser binding/backend plus native cross-process and vnode leases.
 * This focused fixture supplements the separate real-site GUI acceptance. */
#include <nocturne.h>
#include <web.h>
#include <webstorage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks,failures,errors,ready,host_calls;
static char origin[128];
static void check(bool pass,const char *name){checks++;if(!pass)failures++;printf("%s %s\n",pass?"OK":"FAIL",name);}
static int access(webstorage *s,int op,const char *key,const char *value,struct web_storage_result *out){
    struct web_storage_request r={.kind=WEB_STORAGE_LOCAL,.operation=op,.key=key,.key_len=key?strlen(key):0,
        .value=value,.value_len=value?strlen(value):0,.url="https://storage-native.test/writer"};
    return webstorage_access(s,origin,&r,out);
}
static bool value_is(webstorage *s,const char *key,const char *value){
    struct web_storage_result out={0};int status=access(s,WEB_STORAGE_GET,key,NULL,&out);
    bool ok=status==WEB_STORAGE_OK && out.text && !strcmp(out.text,value);webstorage_result_free(&out);return ok;
}
static int host_storage(void *opaque,const char *site,const struct web_storage_request *r,struct web_storage_result *out){
    host_calls++;
    return webstorage_access(opaque,site,r,out);
}
static void console(void *opaque,int level,const char *text){
    if(!strcmp(text,"STORAGE-READY"))ready++;
    else if(!strncmp(text,"OK ",3)){checks++;printf("%s\n",text);}
    else if(!strncmp(text,"FAIL ",5)){checks++;failures++;printf("%s\n",text);}
    else if(level>=2){errors++;printf("ERROR %s\n",text);}
}
static const char page[]="<!doctype html><script>"
    "void localStorage;void sessionStorage;"
    "let received=[];function ck(p,n){console.log((p?'OK ':'FAIL ')+n);}"
    "onstorage=e=>{received.push(e);ck(e instanceof StorageEvent&&e.isTrusted&&!e.bubbles&&!e.cancelable&&e.target===window,'trusted-StorageEvent');"
    "const k=e.key;try{e.key='forged';}catch(x){}ck(e.key===k,'readonly-storage-fields');};"
    "console.log('STORAGE-READY');</script><p>storage window</p>";
static web_doc *document(webstorage *store,const char *suffix){
    char url[192];snprintf(url,sizeof url,"%s/%s",origin,suffix);
    struct web_host host={.opaque=store,.storage=host_storage,.storage_events=true,.console=console,.js_task_budget_ms=5000};
    return web_live(page,sizeof page-1,url,"utf-8",&host);
}
static void pump(web_doc *a,web_doc *b,web_doc *c,unsigned ms){
    uint64_t end=uptime_ms()+ms;
    while(uptime_ms()<end){uint64_t now=uptime_ms();if(a)web_tick(a,now);if(b)web_tick(b,now);if(c)web_tick(c,now);msleep(1);}
}
static volatile uint32_t lease_stage;
static char lease_path[128];
static void lease_waiter(void *unused){
    /* This x86-64/TCC fixture exchanges only an aligned volatile stage word;
     * no payload publication or unsupported compiler atomic builtins. */
    int fd=open(lease_path,O_RDWR|O_CREAT);lease_stage=1;
    if(fd>=0 && fcntl(fd,F_NLOCK)==0){lease_stage=2;fcntl(fd,F_NUNLOCK);close(fd);}
    else lease_stage=3;
}
int main(int argc,char **argv){
    if(argc==3 && !strcmp(argv[1],"--writer")){
        strcpy(origin,argv[2]);webstorage *s=webstorage_create();struct web_storage_result out={0};
        int result=access(s,WEB_STORAGE_SET,"process-key","child",&out);webstorage_free(s);return result!=WEB_STORAGE_OK;
    }
    snprintf(origin,sizeof origin,"https://storage-window-%d.test",getpid());
    snprintf(lease_path,sizeof lease_path,"/tmp/storage-lease-%d",getpid());
    int lease=open(lease_path,O_RDWR|O_CREAT);check(lease>=0&&fcntl(lease,F_NLOCK)==0,"lease-owner-lock");
    int other=open(lease_path,O_RDWR);check(other>=0&&fcntl(other,F_NUNLOCK)<0,"lease-other-description-cannot-unlock");if(other>=0)close(other);
    int tid=thread_create(lease_waiter,NULL);check(tid>0,"lease-waiter-created");
    uint64_t deadline=uptime_ms()+1000;while(!lease_stage&&uptime_ms()<deadline)msleep(1);
    msleep(20);check(lease_stage==1,"lease-separate-open-waits");close(lease);
    deadline=uptime_ms()+1000;while(lease_stage==1&&uptime_ms()<deadline)msleep(1);
    check(lease_stage==2,"lease-last-close-releases");if(lease_stage!=1&&tid>0)thread_join(tid);unlink(lease_path);
    webstorage *a=webstorage_create(),*b=webstorage_create();struct web_storage_result out={0};
    check(access(a,WEB_STORAGE_CLEAR,NULL,NULL,&out)==WEB_STORAGE_OK,"native-initial-clear");
    check(access(a,WEB_STORAGE_SET,"parent-key","kept",&out)==WEB_STORAGE_OK,"parent-unflushed-write");
    char *child_argv[]={argv[0],"--writer",origin,NULL};int child=spawn(argv[0],child_argv,NULL,0),status=-1;
    check(child>0&&waitpid(child,&status,0)>=0&&status==0,"cross-process-writer-completed");
    check(value_is(a,"parent-key","kept")&&value_is(a,"process-key","child"),"cross-process-rebase-keeps-both-keys");
    check(access(a,WEB_STORAGE_SET,"flush-key","pending",&out)==WEB_STORAGE_OK,"stale-window-pending-save");
    check(access(b,WEB_STORAGE_SET,"other-key","kept",&out)==WEB_STORAGE_OK,"second-window-distinct-key");
    check(webstorage_flush(a,true,&out)==WEB_STORAGE_OK&&value_is(b,"other-key","kept")&&value_is(b,"process-key","child"),"flush-rebases-latest-shared-map");
    check(access(a,WEB_STORAGE_CLEAR,NULL,NULL,&out)==WEB_STORAGE_OK,"fixture-clear");
    struct web_host unused_host={.opaque=a,.storage=host_storage,.storage_events=true,.console=console};
    const char *unused_page="<!doctype html><p>Storage is not accessed</p>";
    int before=host_calls;web_doc *unused=web_live(unused_page,strlen(unused_page),"https://storage-unused.test/","utf-8",&unused_host);
    pump(unused,NULL,NULL,120);check(unused&&host_calls==before,"unused-storage-has-no-backend-io-or-poll");web_free(unused);
    web_doc *da=document(a,"a"),*db=document(b,"b");check(da&&db,"live-window-documents");pump(da,db,NULL,250);check(ready==2,"both-author-listeners-ready");
    const char *first="localStorage.k='old';localStorage.k='old';localStorage.removeItem('absent');";
    check(web_console_eval(da,first,strlen(first)),"first-author-update");
    pump(da,db,NULL,150);
    const char *bwrite="ck(localStorage.k==='old','other-window-immediate-map');localStorage.other='kept';";
    check(web_console_eval(db,bwrite,strlen(bwrite)),"second-author-update");
    const char *awrite="localStorage.k='new';";check(web_console_eval(da,awrite,strlen(awrite)),"following-author-update");
    pump(da,db,NULL,250);
    const char *bcheck="ck(received.length===2&&received[0].key==='k'&&received[0].oldValue===null&&received[0].newValue==='old'&&received[1].oldValue==='old'&&received[1].newValue==='new'&&received[0].url===location.origin+'/a'&&received[0].storageArea===localStorage,'remote-event-order-values-source-area');";
    web_console_eval(db,bcheck,strlen(bcheck));
    const char *acheck="ck(received.length===1&&received[0].key==='other','source-excluded-from-own-events');";web_console_eval(da,acheck,strlen(acheck));
    const char *remove="localStorage.removeItem('k');localStorage.clear();localStorage.clear();";web_console_eval(da,remove,strlen(remove));pump(da,db,NULL,200);
    const char *removed="ck(received.length===4&&received[2].key==='k'&&received[2].oldValue==='new'&&received[2].newValue===null&&received[3].key===null&&received[3].oldValue===null&&received[3].newValue===null,'remove-clear-null-values-empty-clear-no-event');";web_console_eval(db,removed,strlen(removed));
    web_doc *dc=document(a,"same-window-frame");pump(da,db,dc,200);
    const char *session="sessionStorage.s='session';";web_console_eval(da,session,strlen(session));pump(da,db,dc,200);
    const char *ccheck="ck(received.length===1&&received[0].storageArea===sessionStorage&&sessionStorage.s==='session','session-same-window-realm-event');";web_console_eval(dc,ccheck,strlen(ccheck));
    const char *isolated="ck(sessionStorage.s===undefined&&received.length===4,'session-excludes-other-window');";web_console_eval(db,isolated,strlen(isolated));
    const char *retire="localStorage.retired='current';";web_console_eval(da,retire,strlen(retire));web_free(db);db=document(b,"replacement");pump(da,db,dc,200);
    const char *generation="ck(received.length===0&&localStorage.retired==='current','replacement-document-has-no-old-events');";web_console_eval(db,generation,strlen(generation));
    check(errors==0,"no-browser-JS-exceptions");web_free(dc);web_free(db);web_free(da);webstorage_free(b);webstorage_free(a);
    printf("STORAGE-WINDOW-DONE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
