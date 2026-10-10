/* Real native DOM/layout/path parser. The private debug parse log verifies that
   1024 samples do not reparse d; this is not real-site rendering acceptance. */
#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks,failures,errors,parses;static bool done;
static void log_line(void *opaque,int level,const char *text){
    if(!strcmp(text,"SVG geometry path parsed")){parses++;return;}
    if(!strcmp(text,"SVG-CACHE-BEGIN")){parses=0;return;}
    if(!strncmp(text,"SVG-CACHE-CHECK ",16)){
        int expected=atoi(text+16);checks++;if(parses!=expected)failures++;
        printf("%s svg-native-cache parsed=%d expected=%d\n",parses==expected?"OK":"FAIL",parses,expected);return;
    }
    if(!strncmp(text,"OK ",3)){checks++;puts(text);}
    else if(!strncmp(text,"FAIL ",5)){checks++;failures++;puts(text);}
    else if(!strcmp(text,"SVG-GEOMETRY-DONE"))done=true;
    else if(level>=2){errors++;printf("ERROR %s\n",text);}
    else if(!strncmp(text,"SVG-GEOMETRY-JS-CHECKS ",23))puts(text);
}
static char *read_source(void){
    FILE *file=fopen("/data/tests/js_svg_geometry_cases.js","rb");if(!file)return NULL;
    if(fseek(file,0,SEEK_END)){fclose(file);return NULL;}long length=ftell(file);
    if(length<0||length>65536||fseek(file,0,SEEK_SET)){fclose(file);return NULL;}
    char *text=malloc((size_t)length+128);if(!text){fclose(file);return NULL;}
    if(fread(text,1,(size_t)length,file)!=(size_t)length){fclose(file);free(text);return NULL;}
    fclose(file);text[length]=0;strcat(text,"\nrunSVGGeometryCases();console.log('SVG-GEOMETRY-DONE');");return text;
}
int main(void){
    const char page[]="<!doctype html><style>html,body{margin:0}#geometry-root{position:absolute;left:10px;top:20px;width:200px;height:100px}</style>"
        "<svg id='geometry-root' width='200' height='100' viewBox='0 0 100 100'><g transform='translate(10 20) scale(2 3)'><path d='M0 0 L3 4'/></g></svg>";
    struct web_host host={.console=log_line,.debug_js=true,.js_task_budget_ms=5000};
    web_doc *doc=web_live(page,sizeof page-1,"https://svg-geometry.test/","utf-8",&host);if(!doc)return 1;
    web_layout(doc,800,600);web_tick(doc,uptime_ms());char *source=read_source();
    if(!source||!web_console_eval(doc,source,source?strlen(source):0)){puts("FAIL SVG geometry execution");failures++;}
    free(source);if(!done||errors){printf("FAIL SVG geometry completion done=%d errors=%d\n",done,errors);failures++;}
    web_free(doc);printf("svggeometrytest: %d checks, %d failed\n",checks,failures);return failures?1:0;
}
