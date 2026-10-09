#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "gfx.h"
#include "video_snapshot.h"
#define NMEDIA_WORKER_URL 64
#define NMEDIA_MAX_PIXELS (1920*1080)
typedef struct web_doc {bool live;struct web_doc *frame_parent;} web_doc;
typedef void node_t;
typedef void nmedia;
typedef void nmedia_worker;
typedef void nmedia_mse_worker;
struct nmedia_info {int unused;};
struct nmedia_output {int unused;};
struct web_video_patch {canvas_t canvas;int scroll_x,scroll_y,x,y,w,h,pitch;const uint32_t *pixels;};
/* PRODUCT_STRUCT */
static struct web_avmedia *streams,*snapshot_candidate;
static web_doc *snapshot_document;
static unsigned snapshot_phase,native_depth;
static bool snapshot_rejected,oom,seeking,accept=true;
static int published,checks,failed;
static uint32_t delivered[64];
static void web_avmedia_enter(void){native_depth++;}
static void web_avmedia_leave(void){if(native_depth)native_depth--;}
static void *nmedia_ff_realloc(void *p,size_t n){return oom?NULL:realloc(p,n);}
static void nmedia_ff_free(void *p){free(p);}
static bool nmedia_worker_seeking(nmedia_worker *w){(void)w;return seeking;}
static int64_t maximum(int64_t a,int64_t b){return a>b?a:b;}
static int64_t minimum(int64_t a,int64_t b){return a<b?a:b;}
static int clip_rect(const canvas_t *c,int *x,int *y,int *w,int *h){
    if(*x<c->cx0){*w-=c->cx0-*x;*x=c->cx0;}
    if(*y<c->cy0){*h-=c->cy0-*y;*y=c->cy0;}
    if(*x+*w>c->cx1)*w=c->cx1-*x;
    if(*y+*h>c->cy1)*h=c->cy1-*y;
    return *w>0&&*h>0;
}
static bool web_js_video_present(web_doc *d,const struct web_video_patch *p){
    (void)d;if(!accept)return false;published++;
    for(int y=0;y<p->h;y++)memcpy(delivered+y*p->w,p->pixels+y*p->pitch,(size_t)p->w*4);
    return true;
}
void gfx_noclip(canvas_t *c);
/* PRODUCT_FUNCTIONS */
static void check(bool yes,const char *name){checks++;if(!yes){failed++;printf("FAIL %s\n",name);}}
static struct web_avmedia s;
static web_doc doc={.live=true};
static uint32_t ui[64],video[4]={0xff112233,0xff334455,0xff556677,0xff778899};
static canvas_t target;
static void setup(void){
    if(s.snapshot.storage)snapshot_clear(&s);
    memset(&s,0,sizeof s);streams=&s;s.doc=&doc;s.playing=true;s.pixels=video;
    s.width=s.height=2;s.generation=9;s.presented_frames=10;
    gfx_init(&target,ui,8,8,8);for(int i=0;i<64;i++)ui[i]=0xffaabbcc;
    s.snapshot.x=2;s.snapshot.y=3;s.snapshot.w=s.snapshot.h=2;
    s.snapshot.video_x=2;s.snapshot.video_y=3;s.snapshot.video_w=s.snapshot.video_h=2;
    doc.live=true;doc.frame_parent=NULL;seeking=oom=false;accept=true;
}
static bool prepare(canvas_t *probe){
    web_avmedia_snapshot_begin(&doc);snapshot_candidate=&s;
    int x=-1,y=-1;bool r=web_avmedia_snapshot_prepare(&doc,&target,probe,&x,&y,5,7);
    if(r)check(x==2&&y==3&&probe->w==2&&probe->h==2,"resolved clip coordinates");
    return r;
}
static void commit_identity(void){
    canvas_t probe;check(prepare(&probe),"snapshot prepare");
    web_avmedia_snapshot_probe(1);gfx_fill(&probe,0,0,2,2,RGB(0,0,0));
    web_avmedia_snapshot_probe(2);gfx_fill(&probe,0,0,2,2,RGB(255,255,255));
    web_avmedia_snapshot_finish();
}
int main(void){
    uint32_t b[3]={RGB(0,0,0),RGB(5,7,9),RGB(0,0,0)},w[3]={RGB(255,255,255),RGB(5,7,9),RGB(255,255,255)};
    uint8_t mask[3];check(web_video_snapshot_mask(b,w,mask,3)&&mask[0]==0&&mask[1]==1,"identity and opaque suffix");
    uint32_t out[3]={1,2,3},committed[3]={4,5,6};web_video_snapshot_overlay(out,committed,mask,3);
    check(out[0]==1&&out[1]==5&&out[2]==3,"opaque overlay retained, video untouched");
    b[0]=gfx_mix(RGB(0,0,0),RGB(120,60,20),128);w[0]=gfx_mix(RGB(255,255,255),RGB(120,60,20),128);
    check(!web_video_snapshot_mask(b,w,mask,1),"integer translucent suffix rejected");
    b[0]=w[0]=gfx_mix(RGB(4,8,12),RGB(120,60,20),128);
    check(web_video_snapshot_mask(b,w,mask,1)&&mask[0],"alpha over opaque suffix is constant");
    setup();commit_identity();check(s.snapshot.valid&&native_depth==0,"committed identity snapshot armed");
    check(s.snapshot.target.px==ui&&s.snapshot.scroll_x==5&&s.snapshot.scroll_y==7,"canvas and both scroll axes fixed");
    int n=published;snapshot_publish(&s);check(published==n,"same decoded frame not republished");
    s.presented_frames++;snapshot_publish(&s);check(published==n+1&&s.window_frames==1,"host publication counted separately");
    check(!memcmp(delivered,video,sizeof video),"native frame draw uses committed geometry");
    bool unchanged=true;for(int i=0;i<64;i++)if(ui[i]!=0xffaabbcc)unchanged=false;
    check(unchanged,"service does not touch UI before host guard");
    s.snapshot.opaque[1]=1;s.snapshot.committed[1]=0xff010203;s.presented_frames++;
    snapshot_publish(&s);check(delivered[1]==0xff010203&&delivered[0]==video[0],"opaque caption/control patch preserved");
    s.playing=false;s.presented_frames++;n=published;snapshot_publish(&s);check(published==n,"paused stream not published");s.playing=true;
    seeking=true;snapshot_publish(&s);check(published==n,"seeking stream not published");seeking=false;
    s.generation++;snapshot_publish(&s);check(published==n,"source generation mismatch rejected");s.generation--;
    s.width++;snapshot_publish(&s);check(published==n,"source size mismatch rejected");s.width--;
    doc.live=false;snapshot_publish(&s);check(published==n,"retired document rejected");doc.live=true;
    accept=false;snapshot_publish(&s);check(published==n&&!s.snapshot.valid,"host rejection disarms without publication");
    setup();canvas_t probe;web_avmedia_snapshot_begin(&doc);snapshot_candidate=&s;web_avmedia_snapshot_reject();int x,y;
    check(!web_avmedia_snapshot_prepare(&doc,&target,&probe,&x,&y,0,0),"unsafe group/frame structure rejected");web_avmedia_snapshot_finish();
    setup();web_avmedia_snapshot_begin(&doc);snapshot_candidate=&s;s.snapshot.w=262145;
    check(!web_avmedia_snapshot_prepare(&doc,&target,&probe,&x,&y,0,0),"snapshot pixel cap fallback");web_avmedia_snapshot_finish();
    setup();web_avmedia_snapshot_begin(&doc);snapshot_candidate=&s;oom=true;
    check(!web_avmedia_snapshot_prepare(&doc,&target,&probe,&x,&y,0,0)&&!s.snapshot.valid,"allocation failure stays ordinary paint");web_avmedia_snapshot_finish();
    setup();check(prepare(&probe),"alpha snapshot prepare");
    web_avmedia_snapshot_probe(1);gfx_fill(&probe,0,0,2,2,RGB(40,40,40));
    web_avmedia_snapshot_probe(2);gfx_fill(&probe,0,0,2,2,RGB(180,180,180));web_avmedia_snapshot_finish();
    check(!s.snapshot.valid&&native_depth==0,"translucent capture cannot arm snapshot");
    snapshot_clear(&s);check(!s.snapshot.storage&&!s.snapshot.valid,"unload releases snapshot ownership");
    printf("video-snapshot-native: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
