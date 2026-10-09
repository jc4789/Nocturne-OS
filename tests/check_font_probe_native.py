"""Only the previously unexecuted read-only glyph guard, one finite run."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parent.parent
out=root / "build/goal-20261009"
source=(root / "user/libc/font.c").read_text(encoding="utf-8")
def function(signature):
    at=source.index(signature)
    first=source[at:source.index("\n",at)]
    return first+"\n" if first.endswith("}") else source[at:source.index("\n}",at)+2]+"\n"
at=source.index("struct gent {")
structure=source[at:source.index("\n};",at)+3]
functions="".join(function(s) for s in ("void font_probe_begin(","bool font_probe_end(","static void gc_flush(","static struct gent *gc_get("))
c=out / "font-probe-native-generated.c"
c.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#define GC_SIZE 4
#define GC_MAX 3
typedef struct {int id;float unit_scale;int info;} font_t;
/* STRUCT */
static struct gent *gc;
static int gc_count;
static uint8_t gamma_tab[256];
static unsigned font_probe_depth;
static bool font_probe_failed;
static int allocations,frees,checks,failures;
static void *tracked_malloc(size_t n){allocations++;return malloc(n);}
static void *tracked_calloc(size_t n,size_t w){allocations++;return calloc(n,w);}
static void tracked_free(void *p){frees++;free(p);}
#define malloc tracked_malloc
#define calloc tracked_calloc
#define free tracked_free
static void stbtt_GetGlyphBitmapBoxSubpixel(int *info,int g,float sx,float sy,float x,float y,int *x0,int *y0,int *x1,int *y1){
    (void)info;(void)g;(void)sx;(void)sy;(void)x;(void)y;*x0=*y0=0;*x1=*y1=1;
}
static void stbtt_MakeGlyphBitmapSubpixel(int *info,uint8_t *p,int w,int h,int pitch,float sx,float sy,float x,float y,int g){
    (void)info;(void)w;(void)h;(void)pitch;(void)sx;(void)sy;(void)x;(void)y;(void)g;*p=255;
}
/* FUNCTIONS */
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
int main(void){
    font_t face={.id=1,.unit_scale=1};font_probe_begin();
    check(!gc_get(&face,1,12,0)&&!allocations&&!frees,"missing cache neither allocated nor flushed");
    check(!font_probe_end()&&!font_probe_depth,"missing cache marks failure and exits");
    struct gent *entry=gc_get(&face,1,12,0);check(entry&&entry->bmp&&allocations==2,"ordinary glyph materialization unchanged");
    int n=allocations,f=frees;font_probe_begin();font_probe_begin();
    check(gc_get(&face,1,12,0)==entry&&allocations==n&&frees==f,"nested read-only cache hit preserves identity");
    check(font_probe_end()&&font_probe_depth==1,"nested guard retains outer scope");
    gc_count=GC_MAX;check(!gc_get(&face,2,12,0)&&allocations==n&&frees==f,"read-only miss cannot flush full cache");
    check(!font_probe_end()&&!font_probe_depth,"miss propagates to outer guard");
    font_probe_begin();check(gc_get(&face,1,12,0)==entry&&font_probe_end(),"new probe resets prior failure");
    gc_flush();tracked_free(gc);
    printf("font-probe-native: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
'''.replace("/* STRUCT */",structure).replace("/* FUNCTIONS */",functions),encoding="utf-8")
cc=root / "tools/msys64/ucrt64/bin/clang.exe"
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get("PATH",""))
exe=out / "font-probe-native.exe"
subprocess.run([str(cc),"-std=gnu11","-O1",str(c),"-o",str(exe)],cwd=root,env=env,check=True)
subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=10)
