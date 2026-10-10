#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks,failures,errors,js_checks;
static void check(const char *name,bool pass){checks++;if(!pass){failures++;printf("FAIL canvasgradient %s\n",name);}}
static void console(void *unused,int level,const char *message){
    if(!strncmp(message,"OK gradient ",12)){checks++;js_checks++;puts(message);}
    else if(!strncmp(message,"FAIL gradient ",14)){checks++;js_checks++;failures++;puts(message);}
    else if(level>=2){errors++;printf("ERROR canvasgradient %s\n",message);}
    else if(!strncmp(message,"GRADIENT-",9))puts(message);
}
int main(void){
    FILE *file=fopen("/data/tests/js_canvas_gradient_cases.js","rb");
    if(!file){puts("canvasgradient: fixture missing");return 1;}
    fseek(file,0,SEEK_END);long length=ftell(file);fseek(file,0,SEEK_SET);
    char *source=length>0?malloc((size_t)length+1):NULL;
    if(!source||fread(source,1,(size_t)length,file)!=(size_t)length){fclose(file);free(source);return 1;}
    fclose(file);source[length]=0;
    const char page[]="<!doctype html><canvas id=gradient width=64 height=64></canvas>";
    struct web_host host={.console=console,.js_task_budget_ms=5000};
    web_doc *d=web_live(page,sizeof page-1,"https://canvas-gradient.test/","utf-8",&host);
    check("document allocated",d!=NULL);
    if(d){
        for(unsigned i=0;i<8;i++){web_tick(d,uptime_ms());msleep(1);}
        web_layout(d,128,128);
        check("native JS evaluated",web_console_eval(d,source,(size_t)length));
        check("no runtime exception",errors==0);
        check("complete JS boundary",js_checks==85);
        web_free(d);
    }
    free(source);printf("canvasgradient: %d checks, %d failed\n",checks,failures);
    return failures!=0;
}
