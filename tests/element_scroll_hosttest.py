"""Bounded checks of exact element-scroll coordinate/clamp/area helpers.
Host fonts/viewport eligibility are shims, not real website acceptance.
"""
from pathlib import Path
import subprocess

source = Path('user/libc/web/layout.c').read_text(encoding='utf-8')
def part(begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]
code = part('float box_abs_x(', 'static void list_abs(')
code += part('static float scroll_extent(', 'static float max_bottom(')
prefix = r'''
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
enum { N_ELEM=1,B_BLOCK=0,B_INLINE=1,B_TEXT=2,B_BR=9,OV_VISIBLE,OV_HIDDEN,OV_SCROLL,OV_AUTO,OV_CLIP,POS_STATIC=0,POS_ABSOLUTE=2,POS_FIXED=3 };
typedef struct style { int overflow,position; } style_t;
typedef struct web_doc { int dummy; } web_doc;
struct box;
typedef struct node { int type; struct box *box;web_doc *owner;double scroll_x,scroll_y; } node_t;
struct run {float x,y,w;struct box *atomic;style_t *st;};
struct deco {float x,y,w,h;};
typedef struct box {int kind;bool anon;node_t *node;style_t *st;struct box *cb,*first,*next;float x,y,w,h,rel_dx,rel_dy,content_dy,p[4],b[4],m[4],scroll_w,scroll_h;struct run *runs;struct deco *decos;int nruns,ndecos;} box_t;
typedef struct wfont {int dummy;} wfont;
static web_doc owner,*D=&owner;
static bool doc_viewport_overflow_box(const web_doc *d,const box_t *b){(void)d;(void)b;return false;}
static bool web_dialog_layer_box(web_doc *d,box_t *b){(void)d;(void)b;return false;}
static wfont style_font(style_t *s){(void)s;return (wfont){0};}
static void wf_metrics(wfont *f,float *asc,float *desc){(void)f;*asc=8;*desc=2;}
static float fmaxf_(float a,float b){return a>b?a:b;}
'''
suffix = r'''
static unsigned total,failed;
static void check(bool ok,const char *name){total++;if(!ok){failed++;fprintf(stderr,"FAIL %s\n",name);}}
static void attach(box_t *b,node_t *n,style_t *st){b->node=n;b->st=st;n->type=N_ELEM;n->box=b;n->owner=&owner;}
int main(void){
    style_t scrolling={.overflow=OV_AUTO},visible={.overflow=OV_VISIBLE},clip={.overflow=OV_CLIP};
    box_t root={.w=800,.h=600,.st=&visible};
    box_t a={.x=20,.y=30,.w=100,.h=50,.cb=&root};node_t an={0};attach(&a,&an,&scrolling);
    box_t b={.x=200,.y=120,.w=300,.h=80,.cb=&a};node_t bn={0};attach(&b,&bn,&visible);root.first=&a;a.first=&b;
    a.p[0]=a.p[1]=a.p[2]=a.p[3]=5;
    scroll_areas(&root);
    check(a.scroll_w==510&&a.scroll_h==210,"child extent and padding");
    check(box_abs_x(&b)==220&&box_abs_y(&b)==150,"raw layout position");
    an.scroll_x=50.25;an.scroll_y=20.5;box_scroll_clamp(&a);
    check(box_visual_x(&a)==20&&box_visual_y(&a)==30,"container border not scrolled");
    check(box_visual_x(&b)==169.75&&box_visual_y(&b)==129.5,"fractional child visual offsets");
    check(box_abs_x(&b)==220&&box_abs_y(&b)==150,"offset geometry remains unscrolled");
    an.scroll_x=1e300;an.scroll_y=1e300;box_scroll_clamp(&a);
    check(an.scroll_x==400&&an.scroll_y==150,"finite huge clamp no int wrap");
    double values[]={-1,-1e300,NAN,INFINITY,-INFINITY,0,1,0.125,399.5,400,401,1e300};
    for(unsigned i=0;i<12;i++)for(unsigned j=0;j<12;j++){
        an.scroll_x=values[i];an.scroll_y=values[j];box_scroll_clamp(&a);
        check(isfinite(an.scroll_x)&&an.scroll_x>=0&&an.scroll_x<=400,"x bounded");
        check(isfinite(an.scroll_y)&&an.scroll_y>=0&&an.scroll_y<=150,"y bounded");
    }
    for(int ov=OV_VISIBLE;ov<=OV_CLIP;ov++){
        scrolling.overflow=ov;an.scroll_x=3;an.scroll_y=4;box_scroll_clamp(&a);
        bool yes=ov==OV_HIDDEN||ov==OV_AUTO||ov==OV_SCROLL;
        check(box_element_scrollable(&a)==yes,"hidden scroll auto only");check((an.scroll_x==3)==yes,"unsupported overflow resets offset");
    }
    scrolling.overflow=OV_AUTO;
    style_t inner={.overflow=OV_AUTO};attach(&b,&bn,&inner);b.w=80;b.h=40;
    box_t c={.x=400,.y=200,.w=600,.h=200,.cb=&b,.st=&visible};b.first=&c;
    scroll_areas(&root);check(a.scroll_w==290&&a.scroll_h==170,"nested private overflow stays clipped");
    check(b.scroll_w==1000&&b.scroll_h==400,"nested scroller owns its full area");
    an.scroll_x=10;an.scroll_y=20;bn.scroll_x=25;bn.scroll_y=30;box_scroll_clamp(&a);box_scroll_clamp(&b);
    check(box_visual_x(&c)==585&&box_visual_y(&c)==300,"nested scroll sums through CB chain");
    c.cb=&root;c.st=&(style_t){.overflow=OV_VISIBLE,.position=POS_FIXED};
    scroll_areas(&root);check(box_visual_x(&c)==400&&box_visual_y(&c)==200,"fixed escapes ancestor offsets");
    check(b.scroll_w==80&&b.scroll_h==40,"fixed cannot inflate intermediate area");
    c.st=&(style_t){.overflow=OV_VISIBLE,.position=POS_ABSOLUTE};c.w=1200;
    scroll_areas(&root);check(box_visual_x(&c)==400&&a.scroll_w==290,"outside absolute CB escapes intermediate scroll");
    check(root.scroll_w==1600,"outside absolute contributes to real CB");
    a.first=NULL;a.nruns=1;a.runs=&(struct run){.x=0,.y=80,.w=650,.st=&visible};
    scroll_areas(&root);check(a.scroll_w==660&&a.scroll_h==92,"nowrap text and vertical run overflow");
    a.nruns=0;a.ndecos=1;a.decos=&(struct deco){.x=500,.y=90,.w=60,.h=20};
    scroll_areas(&root);check(a.scroll_w==570&&a.scroll_h==120,"inline decoration overflow");
    a.ndecos=0;an.scroll_x=100;an.scroll_y=100;scroll_areas(&root);
    check(an.scroll_x==0&&an.scroll_y==0,"layout shrink clamps stale positions");
    a.anon=true;check(!box_element_scrollable(&a),"anonymous principal clone cannot double scroll");a.anon=false;
    a.kind=B_INLINE;check(!box_element_scrollable(&a),"nonreplaced inline is not scrollable");
    printf("element scroll host %u checks / %u failed\n",total,failed);return failed?1:0;
}
'''
out = Path('build/goal-20261009/element-scroll-host.c')
out.write_text(prefix+code+suffix, encoding='utf-8', newline='\n')
exe = 'build/goal-20261009/element-scroll-host.exe'
subprocess.run(['tools/msys64/ucrt64/bin/clang.exe','-O2',str(out),'-o',exe],check=True)
subprocess.run([exe],check=True)
