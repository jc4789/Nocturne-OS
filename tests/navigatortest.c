/* Real web_live + actual cookie jar + isolated Worker process. Offline API
 * acceptance only; this is never evidence that an external website renders.
 * The browser's preferences are a test user's (USERFOLDER is a folder of its
 * own under TEMP), never the real user's. */
#include <nocturne.h>
#include <web.h>
#include <webcookie.h>
#include <webprefs.h>
#include <browser_identity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fixture {webcookie_jar *jar;bool enabled,ready,done;int checks,failed,errors;};
static int checks,failures;
static void log_line(void *opaque,int level,const char *text){
    struct fixture *f=opaque;
    if(!strcmp(text,"NAVIGATOR-READY")||!strcmp(text,"LANGUAGES-READY"))f->ready=true;
    else if(!strcmp(text,"NAVIGATOR-DONE")||!strcmp(text,"LANGUAGES-CHANGED"))f->done=true;
    else if(!strncmp(text,"OK ",3)){f->checks++;printf("%s\n",text);}
    else if(!strncmp(text,"FAIL ",5)){f->checks++;f->failed++;printf("%s\n",text);}
    else if(level==2){f->errors++;printf("ERROR %s\n",text);}
}
static bool cookies_enabled(void *opaque){struct fixture *f=opaque;return f->jar&&f->enabled;}
static char *cookies_get(void *opaque,const char *url){
    struct fixture *f=opaque;if(!cookies_enabled(f))return NULL;
    struct webcookie_context context={.url=url,.site_url=url,.method="GET"};
    long length=webcookie_get(f->jar,&context,NULL,0,time(NULL));if(length<0)return NULL;
    char *value=malloc((size_t)length+1);if(!value)return NULL;
    if(webcookie_get(f->jar,&context,value,(size_t)length+1,time(NULL))<0){free(value);return NULL;}return value;
}
static void cookies_set(void *opaque,const char *url,const char *value){
    struct fixture *f=opaque;if(!cookies_enabled(f))return;
    struct webcookie_context context={.url=url,.site_url=url,.method="GET"};
    webcookie_set(f->jar,&context,value,strlen(value),time(NULL));
}
static char *read_cases(void){
    FILE *file=fopen("C:\\Tests\\js_navigator_cases.js","rb");if(!file)return NULL;
    if(fseek(file,0,SEEK_END)){fclose(file);return NULL;}long length=ftell(file);
    if(length<0||length>65536||fseek(file,0,SEEK_SET)){fclose(file);return NULL;}
    char *source=malloc((size_t)length+1);if(!source){fclose(file);return NULL;}
    size_t n=fread(source,1,(size_t)length,file);fclose(file);
    if(n!=(size_t)length){free(source);return NULL;}source[n]=0;return source;
}
static void run_case(const char *cases,unsigned cpus,const char *languages,int mode){
    struct fixture f={0};if(mode){f.jar=webcookie_create();f.enabled=mode==1;}
    const char *worker=mode==1?
        "const source='const rows=[];('+runNavigatorChecks.toString()+')(WorkerNavigator,navigator,'+JSON.stringify(expected)+',true,(n,v)=>rows.push([n,v]));postMessage(rows);';"
        "const address=URL.createObjectURL(new Blob([source],{type:'text/javascript'}));const worker=new Worker(address);"
        "worker.onmessage=e=>{for(const [n,v]of e.data)check(n,v);worker.terminate();URL.revokeObjectURL(address);console.log('NAVIGATOR-DONE');};"
        "worker.onerror=e=>{check('worker-start',false);worker.terminate();URL.revokeObjectURL(address);console.log('NAVIGATOR-DONE');};":
        "console.log('NAVIGATOR-DONE');";
    size_t capacity=strlen(cases)+strlen(worker)+2048;char *page=malloc(capacity);
    if(!page||(mode&&!f.jar)){free(page);webcookie_free(f.jar);failures++;return;}
    snprintf(page,capacity,"<!doctype html><script>%s\nfunction check(n,v){console.log((v?'OK ':'FAIL ')+n);}"
        "const expected={userAgent:'%s',cpus:%u,cookies:%s,languages:'%s'.split(',')};runNavigatorChecks(Navigator,navigator,expected,false,check);"
        "document.cookie='navigator_probe=active; Path=/';check('real-cookie-jar',%s);"
        "console.log('NAVIGATOR-READY');%s</script>",cases,NOCTURNE_USER_AGENT,cpus,mode==1?"true":"false",languages,
        mode==1?"document.cookie.includes('navigator_probe=active')":"document.cookie===''",worker);
    struct web_host host={.opaque=&f,.console=log_line};
    if(mode){host.cookie_get=cookies_get;host.cookie_set=cookies_set;host.cookie_enabled=cookies_enabled;}
    web_doc *doc=web_live(page,strlen(page),"https://navigator.test/","utf-8",&host);free(page);
    if(!doc){webcookie_free(f.jar);failures++;return;}
    uint64_t deadline=uptime_ms()+15000;bool toggled=mode!=1;
    while((!f.done||!toggled)&&uptime_ms()<deadline){
        web_tick(doc,uptime_ms());
        if(f.ready&&!toggled){
            const char *disabled="check('live-cookie-disabled',navigator.cookieEnabled===false)";
            const char *enabled="check('live-cookie-enabled',navigator.cookieEnabled===true)";
            f.enabled=false;if(!web_console_eval(doc,disabled,strlen(disabled)))f.failed++;
            f.enabled=true;if(!web_console_eval(doc,enabled,strlen(enabled)))f.failed++;toggled=true;
        }
        n_sleep_ms(1);
    }
    if(!f.ready||!f.done||!toggled||f.errors||f.checks!=(mode==1?120:63)){printf("FAIL navigator completion mode=%d ready=%d done=%d checks=%d errors=%d\n",mode,f.ready,f.done,f.checks,f.errors);f.failed++;}
    checks+=f.checks;failures+=f.failed;web_free(doc);webcookie_free(f.jar);
}
static void native_check(const char *name,bool yes){checks++;if(!yes)failures++;printf("%s %s\n",yes?"OK":"FAIL",name);}
/* One turn of the browser's loop: wait on the window process's queue and hand its reports to
   web_wait_events (the preferences' watch among them), then tick the document. */
static void turn(web_doc *doc,int ms){
    n_handle queue=web_wait_queue();struct n_event events[32];
    n_result ready=queue>=0?n_events_wait(queue,events,32,ms):0;
    if(ready>0)web_wait_events(events,(size_t)ready);
    if(doc)web_tick(doc,uptime_ms());
}
/* HTML NavigatorLanguage: a change of the user's languages queues, for the window, an iframe
   and a dedicated Worker, one languagechange task each; navigator.languages stays the same
   frozen array until that task begins; the Worker and the window then agree. */
static const char languages_page[]=
    "<!doctype html><body><script>"
    "function check(n,v){console.log((v?'OK ':'FAIL ')+n);}"
    "const before=navigator.languages;let expected='',windowEvents=0,frameEvents=0,workerEvents=0,frameReady=false,workerReady=false,workerLanguage=null,done=false;"
    "addEventListener('languagechange',e=>{windowEvents++;"
    "check('languages-window-trusted',e.isTrusted===true);"
    "check('languages-window-new-array',navigator.languages!==before&&Object.isFrozen(navigator.languages)&&navigator.languages===navigator.languages&&navigator.languages.join()===expected);"
    "check('languages-window-language',navigator.language===navigator.languages[0]);settle();});"
    "function settle(){if(!done&&windowEvents&&frameEvents&&workerEvents){done=true;console.log('LANGUAGES-CHANGED');}}"
    "function ready(){if(frameReady&&workerReady)console.log('LANGUAGES-READY');}"
    "addEventListener('message',e=>{const d=e.data;if(!d||typeof d!=='object')return;"
    "if(d.kind==='frame-ready'){check('languages-frame-initial',d.list===before.join());frameReady=true;ready();}"
    "else if(d.kind==='frame-change'){frameEvents++;check('languages-frame-trusted',d.trusted===true);"
    "check('languages-frame-new-array',d.fresh&&d.frozen&&d.list===expected&&d.language===d.list.split(',')[0]);settle();}});"
    "const frame=document.createElement('iframe');"
    "frame.srcdoc='<script>const before=navigator.languages;'+"
    "'onlanguagechange=e=>parent.postMessage({kind:\"frame-change\",trusted:e.isTrusted,fresh:navigator.languages!==before,frozen:Object.isFrozen(navigator.languages),list:navigator.languages.join(),language:navigator.language},\"*\");'+"
    "'parent.postMessage({kind:\"frame-ready\",list:navigator.languages.join()},\"*\");<\\/script>';"
    "document.body.appendChild(frame);"
    "const source='const before=navigator.languages;'+"
    "'onlanguagechange=e=>postMessage({kind:\"worker-change\",trusted:e.isTrusted,fresh:navigator.languages!==before,frozen:Object.isFrozen(navigator.languages),list:navigator.languages.join(),language:navigator.language});'+"
    "'postMessage({kind:\"worker-ready\",list:navigator.languages.join(),language:navigator.language});';"
    "const address=URL.createObjectURL(new Blob([source],{type:'text/javascript'}));const worker=new Worker(address);"
    "worker.onmessage=e=>{const d=e.data;"
    "if(d.kind==='worker-ready'){check('languages-worker-initial',d.list===before.join()&&d.language===navigator.language);workerReady=true;ready();}"
    "else if(d.kind==='worker-change'){workerEvents++;workerLanguage=d.language;check('languages-worker-trusted',d.trusted===true);"
    "check('languages-worker-new-array',d.fresh&&d.frozen&&d.list===expected);settle();}};"
    "worker.onerror=()=>check('languages-worker-start',false);"
    "</script></body>";
static void run_languages(void){
    /* The page's checks: 2 when the iframe and the Worker are ready, then 1 before the task,
       3 in the window's task, 2 for the iframe's, 2 for the Worker's, 2 at the end. */
    struct fixture f={0};int expected_checks=2;
    struct web_host host={.opaque=&f,.console=log_line};
    web_doc *doc=web_live(languages_page,sizeof languages_page-1,"https://navigator.test/languages","utf-8",&host);
    if(!doc){native_check("languages-page",false);return;}
    bool ready=false,changed=false;
    uint64_t deadline=uptime_ms()+15000;
    while(!ready&&uptime_ms()<deadline){turn(doc,5);ready=f.ready;}
    native_check("languages-ready",ready);
    /* Two catalog languages the list does not start with: a list the realms have not seen. */
    char now[WEBPREFS_ACCEPT_LANGUAGE_MAX+1]="";webprefs_languages(now,sizeof now);
    size_t first=strcspn(now,","),count=0,chosen=0;const struct webprefs_language *catalog=webprefs_catalog(&count);
    const char *tags[2]={0};
    for(size_t i=0;catalog&&i<count&&chosen<2;i++)
        if(strlen(catalog[i].tag)!=first||strncmp(catalog[i].tag,now,first))tags[chosen++]=catalog[i].tag;
    char expected[WEBPREFS_ACCEPT_LANGUAGE_MAX+1]="";
    if(chosen==2)snprintf(expected,sizeof expected,"%s,%s",tags[0],tags[1]);
    char script[256];snprintf(script,sizeof script,"expected='%s'",expected);
    native_check("languages-expected",chosen==2&&web_console_eval(doc,script,strlen(script)));
    if(ready&&chosen==2){
        uint64_t generation=webprefs_languages_generation();
        native_check("languages-set",webprefs_set_languages(tags,2)==0);
        /* The process's copy changes when the watch reports (no tick: no realm runs a task). */
        uint64_t until=uptime_ms()+5000;
        while(webprefs_languages_generation()==generation&&uptime_ms()<until)turn(NULL,20);
        char list[WEBPREFS_ACCEPT_LANGUAGE_MAX+1]="";webprefs_languages(list,sizeof list);
        native_check("languages-copy-changed",webprefs_languages_generation()!=generation&&!strcmp(list,expected));
        /* Before the window's languagechange task begins, the same frozen array. */
        const char *kept="check('languages-kept-until-task',navigator.languages===before&&navigator.language===before[0]&&windowEvents===0)";
        if(!web_console_eval(doc,kept,strlen(kept)))f.failed++;
        expected_checks+=1+3+2+2;
        while(!f.done&&uptime_ms()<deadline)turn(doc,5);
        changed=f.done;
        /* No second task: keep the loop going a while longer. */
        uint64_t settle=uptime_ms()+500;
        while(uptime_ms()<settle)turn(doc,5);
        const char *once="check('languages-once',windowEvents===1&&frameEvents===1&&workerEvents===1);"
                         "check('languages-worker-matches-window',workerLanguage===navigator.language)";
        if(!web_console_eval(doc,once,strlen(once)))f.failed++;
        expected_checks+=2;
        native_check("languages-reset",webprefs_reset_languages()==0);
    }
    native_check("languages-changed",changed);
    if(f.errors||f.checks!=expected_checks){printf("FAIL languagechange completion checks=%d expected=%d errors=%d\n",f.checks,expected_checks,f.errors);f.failed++;}
    checks+=f.checks;failures+=f.failed;web_free(doc);
}
int main(void){
    struct n_cpuinfo info={0};
    if(cpu_info(&info)<0||info.version!=1||!info.scheduler_cpus||info.scheduler_cpus>info.online_cpus){puts("FAIL actual scheduler metadata");return 1;}
    printf("Navigator native identity=%s scheduler=%u online=%u detected=%u\n",NOCTURNE_USER_AGENT,info.scheduler_cpus,info.online_cpus,info.detected_cpus);
    /* A test user's folder, before anything reads the browser's profile folder (it is kept for
       the process): the preferences this test writes are never the real user's. */
    /* TEMP is emptied at every start, and the language test puts the default back at its end. */
    char user[1024];n_result made=n_path_join(n_temp_folder(),"NavigatorTestUser",user,sizeof user);
    if(made>=0&&(made=n_folder_create(user))==-N_ERR_EXISTS)made=0;
    if(made<0||setenv("USERFOLDER",user,1)!=0||strcmp(n_user_folder(),user)){puts("FAIL test user folder");return 1;}
    /* navigator.languages is the list the browser's preferences hold (tags and commas only). */
    char languages[WEBPREFS_ACCEPT_LANGUAGE_MAX+1];if(webprefs_languages(languages,sizeof languages)<=0){puts("FAIL user languages");return 1;}
    printf("Navigator languages=%s\n",languages);
    char *cases=read_cases();if(!cases){puts("FAIL navigator fixture source");return 1;}
    run_case(cases,info.scheduler_cpus,languages,0);run_case(cases,info.scheduler_cpus,languages,1);run_case(cases,info.scheduler_cpus,languages,2);free(cases);
    run_languages();
    printf("navigatortest: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
