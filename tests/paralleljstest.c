/* Real freestanding browser bindings and native records; no mock DOM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "webi.h"
static int checks,failures,done;
static void console(void *opaque,int level,const char *message){
    (void)opaque;
    if(!strncmp(message,"OK paralleljs ",14))checks++;
    if(!strncmp(message,"DONE paralleljs ",16))done=atoi(message+16);
    if(!strncmp(message,"ORDER paralleljs ",17))printf("%s\n",message);
    if(level==2||!strncmp(message,"FAIL paralleljs ",16)){failures++;printf("%s\n",message);}
}
int main(void){
    FILE *file=fopen("/data/tests/js_native_mutation_cases.js","rb");if(!file)return 2;
    fseek(file,0,SEEK_END);long length=ftell(file);rewind(file);char *code=malloc((size_t)length+128);
    if(!code){fclose(file);return 2;}size_t bytes=fread(code,1,(size_t)length,file);fclose(file);code[bytes]=0;
    strcat(code,";runNativeMutationCases().catch(e=>console.error(e.stack||e));");
    struct web_host host={.console=console};const char *html="<!doctype html><html><head></head><body></body></html>";
    web_doc *d=web_live(html,strlen(html),"https://parallel.test/","utf-8",&host);if(!d){free(code);return 2;}
    for(int i=0;i<10;i++)web_tick(d,uptime_ms());
    bool ok=web_console_eval(d,code,strlen(code));free(code);
    for(int i=0;i<10&&!done;i++)web_tick(d,uptime_ms());
    if(!ok||done!=23||checks!=23){failures++;printf("FAIL paralleljs completion %d/%d\n",checks,done);}
    if(d->profile.native_calls||d->profile.native_ms){failures++;printf("FAIL paralleljs disabled profiling\n");}
    web_free(d);printf("paralleljstest: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
