#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks,failures,errors,js_checks;
static bool done;
static void check(const char *name,bool pass){checks++;printf("%s bitmap-promise-native %s\n",pass?"OK":"FAIL",name);if(!pass)failures++;}
static void console(void *unused,int level,const char *message){
    if(!strncmp(message,"OK bitmap-promise ",18)){checks++;js_checks++;puts(message);}
    else if(!strncmp(message,"FAIL bitmap-promise ",20)){checks++;js_checks++;failures++;puts(message);}
    else if(!strcmp(message,"BITMAP-PROMISE-DONE 10")){done=true;puts(message);}
    else if(level>=2){errors++;printf("ERROR bitmap-promise %s\n",message);}
    else if(!strncmp(message,"BITMAP-PROMISE-",15))puts(message);
    fflush(stdout);
}
int main(void){
    FILE *file=fopen("/data/tests/js_image_bitmap_promise_cases.js","rb");
    if(!file){puts("imagebitmappromise: fixture missing");return 1;}
    fseek(file,0,SEEK_END);long length=ftell(file);fseek(file,0,SEEK_SET);
    char *source=length>0?malloc((size_t)length+1):NULL;
    if(!source||fread(source,1,(size_t)length,file)!=(size_t)length){fclose(file);free(source);return 1;}
    fclose(file);source[length]=0;
    const char page[]="<!doctype html><body></body>";
    struct web_host host={.console=console,.js_task_budget_ms=5000};
    web_doc *d=web_live(page,sizeof page-1,"https://bitmap-promise.test/","utf-8",&host);
    check("real document allocated",d!=NULL);
    if(d){
        for(unsigned i=0;i<8;i++){web_tick(d,uptime_ms());msleep(1);}
        web_layout(d,128,128);
        check("new scheduling boundary evaluated",web_console_eval(d,source,(size_t)length));
        uint64_t deadline=uptime_ms()+5000;
        while(!done&&uptime_ms()<deadline){web_tick(d,uptime_ms());msleep(1);}
        check("bounded new Promise checks complete",done&&js_checks==10);
        check("no runtime exception",errors==0);
        web_free(d);
    }
    free(source);printf("imagebitmappromise: %d checks, %d failed (JS %d)\n",checks,failures,js_checks);
    return failures!=0;
}
