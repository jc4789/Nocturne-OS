/* Real web_live + actual cookie jar + isolated Worker process. Offline API
 * acceptance only; this is never evidence that an external website renders. */
#include <nocturne.h>
#include <web.h>
#include <webcookie.h>
#include <browser_identity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fixture {webcookie_jar *jar;bool enabled,ready,done;int checks,failed,errors;};
static int checks,failures;
static void log_line(void *opaque,int level,const char *text){
    struct fixture *f=opaque;
    if(!strcmp(text,"NAVIGATOR-READY"))f->ready=true;
    else if(!strcmp(text,"NAVIGATOR-DONE"))f->done=true;
    else if(!strncmp(text,"OK ",3)){f->checks++;printf("%s\n",text);}
    else if(!strncmp(text,"FAIL ",5)){f->checks++;f->failed++;printf("%s\n",text);}
    else if(level>=2){f->errors++;printf("ERROR %s\n",text);}
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
    FILE *file=fopen("/data/tests/js_navigator_cases.js","rb");if(!file)return NULL;
    if(fseek(file,0,SEEK_END)){fclose(file);return NULL;}long length=ftell(file);
    if(length<0||length>65536||fseek(file,0,SEEK_SET)){fclose(file);return NULL;}
    char *source=malloc((size_t)length+1);if(!source){fclose(file);return NULL;}
    size_t n=fread(source,1,(size_t)length,file);fclose(file);
    if(n!=(size_t)length){free(source);return NULL;}source[n]=0;return source;
}
static void run_case(const char *cases,unsigned cpus,int mode){
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
        "const expected={userAgent:'%s',cpus:%u,cookies:%s};runNavigatorChecks(Navigator,navigator,expected,false,check);"
        "document.cookie='navigator_probe=active; Path=/';check('real-cookie-jar',%s);"
        "console.log('NAVIGATOR-READY');%s</script>",cases,NOCTURNE_USER_AGENT,cpus,mode==1?"true":"false",
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
        msleep(1);
    }
    if(!f.ready||!f.done||!toggled||f.errors||f.checks!=(mode==1?104:55)){printf("FAIL navigator completion mode=%d ready=%d done=%d checks=%d errors=%d\n",mode,f.ready,f.done,f.checks,f.errors);f.failed++;}
    checks+=f.checks;failures+=f.failed;web_free(doc);webcookie_free(f.jar);
}
int main(void){
    struct n_cpuinfo info={0};
    if(cpu_info(&info)<0||info.version!=1||!info.scheduler_cpus||info.scheduler_cpus>info.online_cpus){puts("FAIL actual scheduler metadata");return 1;}
    printf("Navigator native identity=%s scheduler=%u online=%u detected=%u\n",NOCTURNE_USER_AGENT,info.scheduler_cpus,info.online_cpus,info.detected_cpus);
    char *cases=read_cases();if(!cases){puts("FAIL navigator fixture source");return 1;}
    run_case(cases,info.scheduler_cpus,0);run_case(cases,info.scheduler_cpus,1);run_case(cases,info.scheduler_cpus,2);free(cases);
    printf("navigatortest: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
