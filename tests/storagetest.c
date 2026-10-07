/* Real web_live -> native_storage -> browser-equivalent storage callback ->
 * product libc persistence. Run ONLY with an isolated QEMU scratch /data disk:
 * tcc -run /data/tests/storagetest.c --isolated-scratch
 * Never run this on a user's normal disk. It creates only unique fixture origins. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <bearssl_hash.h>
#include "nocturne.h"
#include "web.h"
#include "webstorage.h"

static webstorage *store;
static int checks, failed, js_errors, calls;
static bool done;
static char fixture_origin[160];
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("FAIL storage %s\n",name);}}
static int access_store(void *opaque,const char *origin,const struct web_storage_request *r,struct web_storage_result *out){
    (void)opaque;calls++;return webstorage_access(store,origin,r,out);
}
static bool request(void *opaque,const struct web_request *r){(void)opaque;(void)r;check(false,"unexpected network request");return false;}
static void message(void *opaque,int level,const char *text){
    (void)opaque;
    if(level==2){js_errors++;printf("ERROR storage %s\n",text);}
    if(!strncmp(text,"FAIL",4)){failed++;printf("%s\n",text);}
    if(!strncmp(text,"STORAGE-DONE",12)){done=true;printf("%s\n",text);}
}
static bool script(const char *url,const char *code){
    const char *head="<!doctype html><html><head></head><body><script>";
    const char *tail="</script></body></html>";
    size_t size=strlen(head)+strlen(code)+strlen(tail)+1;char *html=malloc(size);
    if(!html){check(false,"script allocation");return false;}
    snprintf(html,size,"%s%s%s",head,code,tail);done=false;
    struct web_host host={.request=request,.console=message,.storage=access_store};
    web_doc *d=web_live(html,strlen(html),url,"utf-8",&host);free(html);
    check(d!=NULL,"live document allocation");if(!d)return false;
    uint64_t deadline=uptime_ms()+20000;
    while(!done && uptime_ms()<deadline){web_tick(d,uptime_ms());msleep(1);}
    web_free(d);check(done,"JS completed");return done;
}
static int direct(const char *origin,int kind,int op,const char *key,const char *value,struct web_storage_result *out){
    struct web_storage_request r={.kind=kind,.operation=op,.key=key,.value=value,
        .key_len=key?strlen(key):0,.value_len=value?strlen(value):0};
    return webstorage_access(store,origin,&r,out);
}
static void expect(const char *origin,int kind,const char *key,const char *value,const char *name){
    struct web_storage_result out={0};int status=direct(origin,kind,WEB_STORAGE_GET,key,NULL,&out);
    check(status==WEB_STORAGE_OK && (value?(out.text && out.text_len==strlen(value) && !memcmp(out.text,value,out.text_len)):!out.text),name);
    free(out.text);
}
static void fixture_path(const char *origin,int slot,char path[160]){
    br_sha256_context context;uint8_t digest[32];char hex[65];const char chars[]="0123456789abcdef";
    br_sha256_init(&context);br_sha256_update(&context,origin,strlen(origin));br_sha256_out(&context,digest);
    for(int i=0;i<32;i++){hex[2*i]=chars[digest[i]>>4];hex[2*i+1]=chars[digest[i]&15];}hex[64]=0;
    snprintf(path,160,"/data/browser/storage/%s.%d",hex,slot);
}
static void corrupt(const char *origin,int slot){
    char path[160];fixture_path(origin,slot,path);int fd=open(path,O_WRONLY);
    check(fd>=0,"open OWN fixture snapshot");if(fd>=0){check(write(fd,"!",1)==1,"corrupt OWN fixture snapshot");close(fd);}
}
int main(int argc,char **argv){
    if(argc!=2 || strcmp(argv[1],"--isolated-scratch")){puts("REFUSE: isolated QEMU scratch /data is required; pass --isolated-scratch only there");return 2;}
    snprintf(fixture_origin,sizeof fixture_origin,"https://storage-fixture-%d-%llu.invalid",getpid(),(unsigned long long)uptime_ms());
    store=webstorage_create();check(store!=NULL,"backend allocation");if(!store)return 1;
    FILE *file=fopen("/data/tests/js_storage_cases.js","rb");check(file!=NULL,"JS fixture present");
    if(file){
        fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);
        check(length>0 && length<65536,"JS fixture bounded");
        if(length>0 && length<65536){
            const char *tail=";try{console.log('STORAGE-DONE '+runStorageCases());}catch(e){console.log('FAIL storage '+e+' '+e.stack);console.log('STORAGE-DONE failure');}";
            char *code=malloc((size_t)length+strlen(tail)+1);check(code!=NULL,"JS fixture allocation");
            if(code){size_t n=fread(code,1,(size_t)length,file);code[n]=0;strcat(code,tail);script(fixture_origin,code);free(code);}
        }
        fclose(file);
    }
    script(fixture_origin,"sessionStorage.setItem('navigation','kept');localStorage.setItem('persistent','yes');localStorage.setItem('nul','\\ud800\\0\\udc00');console.log('STORAGE-DONE save');");
    char url[256];snprintf(url,sizeof url,"%s/next?query#fragment",fixture_origin);
    script(url,"if(sessionStorage.getItem('navigation')!=='kept'||localStorage.getItem('persistent')!=='yes')console.log('FAIL navigation storage');document.head.innerHTML='<base href=\"https://wrong.invalid/\">';if(localStorage.getItem('persistent')!=='yes')console.log('FAIL base redirected storage');console.log('STORAGE-DONE navigation');");
    snprintf(url,sizeof url,"%s:443/canonical",fixture_origin);
    for(char *p=url;*p;p++)*p=(char)toupper((unsigned char)*p);
    script(url,"if(sessionStorage.getItem('navigation')!=='kept'||localStorage.getItem('persistent')!=='yes')console.log('FAIL canonical origin');console.log('STORAGE-DONE canonical');");
    script("https://other-storage-fixture.invalid/","if(sessionStorage.getItem('navigation')!==null||localStorage.getItem('persistent')!==null)console.log('FAIL origin isolation');console.log('STORAGE-DONE origin');");
    int oldcalls=calls;
    script("file:///home/storage-fixture.html","for(const key of ['localStorage','sessionStorage']){let ok=false;try{globalThis[key];}catch(e){ok=e.name==='SecurityError';}if(!ok)console.log('FAIL opaque storage '+key);}console.log('STORAGE-DONE opaque');");
    check(calls==oldcalls,"opaque origins never reach backend");
    expect(fixture_origin,WEB_STORAGE_SESSION,"navigation","kept","session survives document navigation");
    webstorage_free(store);store=webstorage_create();
    expect(fixture_origin,WEB_STORAGE_LOCAL,"persistent","yes","local persists across backend recreation");
    expect(fixture_origin,WEB_STORAGE_SESSION,"navigation",NULL,"fresh window session is empty");
    struct web_storage_result out={0};
    check(direct(fixture_origin,WEB_STORAGE_LOCAL,WEB_STORAGE_GET,"nul",NULL,&out)==WEB_STORAGE_OK && out.text_len==7 &&
        out.text && !memcmp(out.text,"\xed\xa0\x80\0\xed\xb0\x80",7),"persisted CESU8 includes NUL and lone surrogates");free(out.text);
    check(direct("null",WEB_STORAGE_SESSION,WEB_STORAGE_CHECK,NULL,NULL,&out)==WEB_STORAGE_SECURITY,"opaque native origin denied");
    check(direct("https://wrong.invalid/../../data",WEB_STORAGE_LOCAL,WEB_STORAGE_SET,"x","y",&out)==WEB_STORAGE_SECURITY,"no origin path injection");
    char corrupt_origin[160];snprintf(corrupt_origin,sizeof corrupt_origin,"https://corrupt-%d-%llu.invalid",getpid(),(unsigned long long)uptime_ms());
    check(direct(corrupt_origin,WEB_STORAGE_LOCAL,WEB_STORAGE_SET,"key","one",&out)==WEB_STORAGE_OK,"snapshot generation one");
    check(direct(corrupt_origin,WEB_STORAGE_LOCAL,WEB_STORAGE_SET,"key","two",&out)==WEB_STORAGE_OK,"snapshot generation two");
    corrupt(corrupt_origin,1);webstorage_free(store);store=webstorage_create();
    expect(corrupt_origin,WEB_STORAGE_LOCAL,"key","one","invalid new slot recovers previous snapshot");
    check(direct(corrupt_origin,WEB_STORAGE_LOCAL,WEB_STORAGE_SET,"key","three",&out)==WEB_STORAGE_OK,"write repairs inactive slot");
    webstorage_free(store);store=webstorage_create();expect(corrupt_origin,WEB_STORAGE_LOCAL,"key","three","repaired snapshot persists");
    corrupt(corrupt_origin,0);corrupt(corrupt_origin,1);webstorage_free(store);store=webstorage_create();
    check(direct(corrupt_origin,WEB_STORAGE_LOCAL,WEB_STORAGE_GET,"key",NULL,&out)==WEB_STORAGE_IO,"all snapshots corrupt: explicit failure, not reset");
    script(corrupt_origin,"let error;try{localStorage;}catch(e){error=e;}if(!error||error.name!=='UnknownError')console.log('FAIL corrupt storage error');console.log('STORAGE-DONE corrupt');");
    expect(fixture_origin,WEB_STORAGE_LOCAL,"persistent","yes","other origin preserved after corruption");
    webstorage_free(store);
    printf("storagetest: %d native checks, %d failed, %d JS errors, %d native host calls\n",checks,failed,js_errors,calls);
    return failed || js_errors;
}
