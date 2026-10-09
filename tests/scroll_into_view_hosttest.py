"""Only new scrollIntoView alignment/ancestor boundaries, exact C helpers."""
from pathlib import Path
import subprocess
layout=Path('user/libc/web/layout.c').read_text(encoding='utf-8')
doc=Path('user/libc/web/doc.c').read_text(encoding='utf-8')
def part(text,begin,end):
    p=text.index(begin)
    return text[p:text.index(end,p)]
pieces=part(layout,'float box_abs_x(','static void list_abs(')
pieces+=part(doc,'bool doc_element_scroll_metrics(','const char *doc_link_href(')
prefix=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
enum {N_DOC,N_ELEM};
enum {B_BLOCK,B_INLINE,B_TEXT,B_BR};
enum {D_BLOCK,D_NONE};
enum {OV_VISIBLE,OV_HIDDEN,OV_SCROLL,OV_AUTO,OV_CLIP};
enum {POS_STATIC,POS_ABSOLUTE,POS_FIXED};
enum doc_scroll_alignment {DOC_SCROLL_START,DOC_SCROLL_END,DOC_SCROLL_CENTER,DOC_SCROLL_NEAREST};
struct node;struct box;
typedef struct pvec {void **v;int n,cap;} pvec;
typedef struct web_doc {bool live,inert,paint_dirty;int width,height;struct node *html,*root;} web_doc;
typedef struct style {int display,overflow,position;} style_t;
typedef struct node {int type;bool connected;web_doc *owner;struct box *box,*anchor_block;double scroll_x,scroll_y;} node_t;
struct deco {float x,y,w,h;node_t *node;};
typedef struct box {int kind;bool anon;node_t *node;style_t *st;struct box *cb;float x,y,w,h,rel_dx,rel_dy,content_dy,p[4],b[4],scroll_w,scroll_h;struct deco *decos;int ndecos;} box_t;
static bool allocation_fail;
static void *bounded_malloc(size_t n){return allocation_fail?NULL:malloc(n);}
#define malloc bounded_malloc
static bool doc_viewport_overflow_box(const web_doc *d,const box_t *b){return b->node==d->html;}
static bool doc_node_connected(node_t *n){return n->connected;}
static node_t *doc_node_root(node_t *n,bool composed){(void)composed;return n->owner->root;}
static int web_layout(web_doc *d,int w,int h){(void)d;(void)w;(void)h;return 0;}
'''
suffix=r'''
#undef malloc
static unsigned total,failed;
static void check(bool ok,const char *name){total++;if(!ok){failed++;fprintf(stderr,"FAIL %s\n",name);}}
static void attach(web_doc *d,box_t *b,node_t *n,style_t *st){b->node=n;b->st=st;n->box=b;n->owner=d;n->type=N_ELEM;n->connected=true;}
static void release(pvec *v){free(v->v);memset(v,0,sizeof *v);}
int main(void){
    double target[][2]={{20,40},{-20,10},{90,120},{-30,120},{-150,-30},{110,250},{0,100},{20,120}};
    double nearest[]={0,-20,20,0,-130,110,0,20};
    for(unsigned i=0;i<8;i++){
        check(scroll_align_delta(target[i][0],target[i][1],0,100,DOC_SCROLL_START)==target[i][0],"start");
        check(scroll_align_delta(target[i][0],target[i][1],0,100,DOC_SCROLL_END)==target[i][1]-100,"end");
        check(scroll_align_delta(target[i][0],target[i][1],0,100,DOC_SCROLL_CENTER)==(target[i][0]+target[i][1]-100)/2,"center");
        check(scroll_align_delta(target[i][0],target[i][1],0,100,DOC_SCROLL_NEAREST)==nearest[i],"nearest visible/partial/oversized/equal");
    }
    web_doc d={.live=true,.width=800,.height=600};node_t root_node={.type=N_DOC,.owner=&d,.connected=true};d.root=&root_node;
    style_t visible={.display=D_BLOCK,.overflow=OV_VISIBLE},scroll={.display=D_BLOCK,.overflow=OV_AUTO};
    box_t root={.w=800,.h=600,.scroll_w=2000,.scroll_h=2000,.st=&visible};
    box_t outer={.x=50,.y=80,.w=200,.h=160,.scroll_w=1200,.scroll_h=1000,.cb=&root};node_t on={0};attach(&d,&outer,&on,&scroll);
    box_t inner={.x=400,.y=300,.w=100,.h=80,.scroll_w=600,.scroll_h=500,.cb=&outer};node_t in={0};attach(&d,&inner,&in,&scroll);
    box_t leaf={.x=250,.y=200,.w=20,.h=10,.cb=&inner};node_t n={0};attach(&d,&leaf,&n,&visible);
    pvec changed={0};double x=0,y=0;
    check(doc_element_scroll_into_view(&d,&n,DOC_SCROLL_START,DOC_SCROLL_START,false,&x,&y,&changed)==1,"rendered native path");
    check(in.scroll_x==250&&in.scroll_y==200&&on.scroll_x==400&&on.scroll_y==300,"inner then outer actual offsets");
    check(changed.n==2&&changed.v[0]==&in&&changed.v[1]==&on,"actual changed order");
    check(box_visual_x(&leaf)==50&&box_visual_y(&leaf)==80&&x==50&&y==80,"actual rect changed then viewport proposal");release(&changed);
    in.scroll_x=in.scroll_y=on.scroll_x=on.scroll_y=0;x=y=0;
    check(doc_element_scroll_into_view(&d,&n,0,0,true,&x,&y,&changed)==1&&changed.n==1,"nearest container only");
    check(in.scroll_x==250&&on.scroll_x==0&&x==0&&y==0,"nearest leaves outer and viewport alone");release(&changed);
    in.scroll_x=in.scroll_y=on.scroll_x=on.scroll_y=0;x=y=0;
    check(doc_element_scroll_into_view(&d,&n,3,3,false,&x,&y,&changed)==1,"nearest alignment all");
    check(in.scroll_x==170&&in.scroll_y==130&&on.scroll_x==300&&on.scroll_y==220&&x==0&&y==0,"nearest moves each minimum and visible viewport unchanged");release(&changed);
    in.scroll_x=in.scroll_y=on.scroll_x=on.scroll_y=0;x=y=0;allocation_fail=true;
    check(doc_element_scroll_into_view(&d,&n,0,0,false,&x,&y,&changed)==-1&&in.scroll_x==0&&on.scroll_x==0&&!changed.v,"OOM before any scroll");allocation_fail=false;
    root.cb=&root;check(doc_element_scroll_into_view(&d,&n,0,0,true,&x,&y,&changed)==-2&&in.scroll_x==0,"bounded CB chain refusal before effect");root.cb=NULL;
    n.connected=false;check(doc_element_scroll_into_view(&d,&n,0,0,false,&x,&y,&changed)==0&&!changed.v,"detached early return");n.connected=true;
    visible.display=D_NONE;check(doc_element_scroll_into_view(&d,&n,0,0,false,&x,&y,&changed)==0,"nonrendered early return");visible.display=D_BLOCK;
    check(doc_element_scroll_into_view(&d,&n,4,0,false,&x,&y,&changed)==0,"invalid native enum refusal");
    inner.st=&(style_t){.display=D_BLOCK,.overflow=OV_AUTO,.position=POS_FIXED};inner.cb=&root;x=11;y=12;
    check(doc_element_scroll_into_view(&d,&n,0,0,false,&x,&y,&changed)==1&&x==11&&y==12&&on.scroll_x==0,"fixed containing block leaves viewport unchanged");release(&changed);
    leaf.kind=B_INLINE;leaf.cb=NULL;n.anchor_block=&inner;
    struct deco deco[]={ {.x=150,.y=100,.w=10,.h=12,.node=&n},{.x=165,.y=110,.w=20,.h=12,.node=&n} };inner.decos=deco;inner.ndecos=2;
    in.scroll_x=in.scroll_y=0;
    check(doc_element_scroll_into_view(&d,&n,0,0,true,&x,&y,&changed)==1&&in.scroll_x==150&&in.scroll_y==100,"inline native span union scrolls its actual block");release(&changed);
    printf("scrollIntoView host %u checks / %u failed\n",total,failed);return failed?1:0;
}
'''
out=Path('build/goal-20261009/scroll-into-view-host.c')
out.write_text(prefix+pieces+suffix,encoding='utf-8',newline='\n')
exe='build/goal-20261009/scroll-into-view-host.exe'
subprocess.run(['tools/msys64/ucrt64/bin/clang.exe','-O2',str(out),'-o',exe],check=True)
subprocess.run([exe],check=True)
