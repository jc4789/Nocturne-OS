/* Shared production @media / matchMedia evaluator; not site acceptance. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
extern bool css_media_evaluate(const char *,int,int,bool,char *,size_t);
static int checks,failed;
static void check(const char *q,int w,int h,bool js,bool expect,const char *serialized) {
    char text[2048];bool got=css_media_evaluate(q,w,h,js,text,sizeof text);checks++;
    if(got!=expect){printf("FAIL media matches: %s (%dx%d) got=%d\n",q,w,h,got);failed++;}
    if(serialized){checks++;if(strcmp(text,serialized)){printf("FAIL media serialization: %s -> %s != %s\n",q,text,serialized);failed++;}}
}
int main(void) {
#define Q(q,b) check(q,800,600,true,b,NULL)
    check("",800,600,true,true,"");check(" \t",800,600,true,true,"");
    check("SCREEN and ( MIN-WIDTH : 800px )",800,600,true,true,"screen and (min-width: 800px)");
    Q("all",true);Q("screen",true);Q("print",false);Q("not print",true);Q("only screen",true);
    Q("screen and (width: 800px)",true);Q("screen and (height:600px)",true);
    Q("not screen and (min-width: 900px)",true);Q("not screen and (min-width: 700px)",false);
    Q("(width >= 800px)",true);Q("(800px <= width)",true);Q("(799px < width <= 800px)",true);
    Q("(801px > width >= 800px)",true);Q("(400px <= width < 800px)",false);
    Q("((width:800px) and (height:600px))",true);Q("(width:1px) or (height:600px)",true);
    Q("not (width:1px)",true);Q("(not (width:1px))",true);
    Q("(min-width:50em)",true);Q("(max-width:50rem)",true);Q("(width:600pt)",true);
    Q("(resolution:96dpi)",true);Q("(resolution:1dppx)",true);Q("(min-resolution:2dppx)",false);
    Q("(aspect-ratio:4/3)",true);Q("(aspect-ratio:4 / 3)",true);Q("(min-aspect-ratio: 1)",true);
    Q("(orientation:landscape)",true);check("(orientation:portrait)",600,600,true,true,NULL);
    Q("(hover:hover) and (pointer:fine)",true);Q("(hover)",true);Q("(any-pointer)",true);
    Q("(color:8)",true);Q("(color)",true);Q("(monochrome)",false);Q("(color-index:0)",true);
    Q("(scripting:enabled)",true);Q("(scripting:none)",false);check("(scripting:none)",800,600,false,true,NULL);
    Q("(prefers-color-scheme:light)",true);Q("(prefers-color-scheme:dark)",false);
    Q("(prefers-reduced-motion:reduce)",false);Q("(prefers-reduced-motion)",false);
    Q("(prefers-reduced-motion:no-preference)",true);Q("(forced-colors:none)",true);
    Q("(display-mode:browser)",true);Q("(grid:0)",true);Q("(grid)",false);
    Q("(width: 800)",false);Q("(width: 1dppx)",false);Q("(resolution:96px)",false);
    Q("(min-hover:none)",false);Q("(width:800px trailing)",false);Q("(width:800px000)",false);
    Q("(aspect-ratio:4 / 3 junk)",false);Q("(aspect-ratio:4 / 0)",false);
    Q("(width == 800px)",false);Q("(width => 800px)",false);Q("(800px < width > 1px)",false);
    Q("(1px < width < 900px < 1000px)",false);Q("(color:8.5)",false);
    Q("(width:NaNpx)",false);Q("(width:infinity)",false);Q("(width:1e999px)",false);
    Q("not (unknown-feature)",false);Q("(unknown-feature) or (color)",true);
    Q("(unknown-feature) and (color)",false);Q("not ((unknown-feature) and (monochrome))",true);
    Q("not (width:bogus)",false);Q("not (prefers-color-scheme:bogus)",false);
    Q("screen trailing",false);Q("not screen trailing",false);Q("not",false);Q("only",false);
    Q("or and (color)",false);Q("only (color)",false);Q("screen or (color)",false);
    Q("(color) and (hover) or (pointer)",false);Q("not (color) and (hover)",false);
    Q("(width:800px",false);Q("(width:800px))",false);Q("((width:800px) garbage)",false);
    Q("screen and",false);Q("screen and ()",false);Q("screen and print",false);
    Q("screen and(color)",false);Q("not(monochrome)",false);Q("(color)and(hover)",false);
    Q("unknown() or (color)",true);Q("not unknown()",false);
    Q("(device-width:800px)",false);Q("not (device-height:600px)",false);
    check("screen trailing, (color)",800,600,true,true,"not all, (color)");
    check("not (unknown-feature)",800,600,true,false,"not all");
    Q("screen/**/and/**/(color)",true);Q("screen/* trailing comment",true);
    check("(min-width:800px)",799,600,true,false,NULL);
    char deep[300];memset(deep,'(',100);strcpy(deep+100,"color");memset(deep+105,')',100);deep[205]=0;Q(deep,false);
    printf("mediapolicytest: %d checks, %d failed\n",checks,failed);return failed!=0;
}
