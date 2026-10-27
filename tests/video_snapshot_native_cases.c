/* The product's own types; the host stands in only for the pool, the media heap,
   the document's paint damage and the browser's present. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "webi.h"
#include "avmedia.h"
#include "video_snapshot.h"
#include "media.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "media_worker_private.h"
#include "media_mse_private.h"
#include "media_mse_worker_private.h"
#include "media_rate_private.h"
#include "web_presentation.h"
#include "media_http_private.h"
#include <parallel.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
/* The pool: bands run in order on this thread, as a pool job's own nested draw does. */
bool parallel_for_stage(enum parallel_stage stage,size_t count,parallel_fn fn,void *context,parallel_service_fn service,void *opaque){
    (void)stage;(void)service;(void)opaque;for(size_t i=0;i<count;i++)fn(i,context);return false;}
static bool draw_in_bands(const canvas_t *c,uint8_t *cd,const uint32_t *pixels,const uint8_t *deep,int sw,int sh,int x,int y,int w,int h,
                          int64_t x0,int64_t y0,int64_t x1,int64_t y1){
    (void)c;(void)cd;(void)pixels;(void)deep;(void)sw;(void)sh;(void)x;(void)y;(void)w;(void)h;(void)x0;(void)y0;(void)x1;(void)y1;return false;}
/* PRODUCT_STRUCT */
static struct web_avmedia *streams;
static unsigned native_depth;
static bool oom,seeking,accept=true;
static int published,damaged,checks,failed;
static uint32_t delivered[64];
void *nmedia_ff_realloc(void *p,size_t n){return oom?NULL:realloc(p,n);}
void nmedia_ff_free(void *p){free(p);}
bool nmedia_worker_seeking(nmedia_worker *w){(void)w;return seeking;}
bool doc_node_connected(node_t *n){return n!=NULL;}
void doc_paint_damage(web_doc *d,node_t *n){(void)d;(void)n;damaged++;}
static int64_t maximum(int64_t a,int64_t b){return a>b?a:b;}
static int64_t minimum(int64_t a,int64_t b){return a<b?a:b;}
static int clip_rect(const canvas_t *c,int *x,int *y,int *w,int *h){
    if(*x<c->cx0){*w-=c->cx0-*x;*x=c->cx0;}
    if(*y<c->cy0){*h-=c->cy0-*y;*y=c->cy0;}
    if(*x+*w>c->cx1)*w=c->cx1-*x;
    if(*y+*h>c->cy1)*h=c->cy1-*y;
    return *w>0&&*h>0;
}
bool web_js_video_present(web_doc *d,const struct web_video_patch *p){
    (void)d;if(!accept)return false;published++;
    p->draw(p->context,delivered,p->w);
    return true;
}
void gfx_noclip(canvas_t *c);
/* PRODUCT_FUNCTIONS */
static void check(bool yes,const char *name){checks++;if(!yes){failed++;printf("FAIL %s\n",name);}}
static struct web_avmedia s;
static web_doc doc={.live=true};
static node_t element={.owner=&doc};
static uint32_t ui[64],video[4]={0xff112233,0xff334455,0xff556677,0xff778899};
static canvas_t target;
static void setup(void){
    if(s.snapshot.storage)snapshot_clear(&s);
    memset(&s,0,sizeof s);streams=&s;s.doc=&doc;s.node=&element;s.playing=true;s.pixels=video;
    s.width=s.height=2;s.generation=9;s.presented_frames=10;
    gfx_init(&target,ui,8,8,8);for(int i=0;i<64;i++)ui[i]=0xffaabbcc;
    s.snapshot.x=2;s.snapshot.y=3;s.snapshot.w=s.snapshot.h=2;
    s.snapshot.video_x=2;s.snapshot.video_y=3;s.snapshot.video_w=s.snapshot.video_h=2;
    s.snapshot.band_x=2;s.snapshot.band_y=3;s.snapshot.band_w=s.snapshot.band_h=2;
    doc.live=true;doc.frame_parent=NULL;seeking=oom=false;accept=true;
}
static bool prepare(canvas_t *probe){
    web_avmedia_snapshot_begin(&doc,&target,0,0,8,8);snapshot_candidate=&s;
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
    uint8_t mask[3];uint32_t spans[3];check(web_video_snapshot_mask(b,w,spans,mask,3,false)&&mask[0]==0&&mask[1]==1,"identity and opaque suffix");
    uint32_t out[3]={1,2,3},committed[3]={4,5,6};web_video_snapshot_overlay(out,committed,b,mask,3);
    check(out[0]==1&&out[1]==5&&out[2]==3,"opaque overlay retained, video untouched");
    b[0]=gfx_mix(RGB(0,0,0),RGB(120,60,20),128);w[0]=gfx_mix(RGB(255,255,255),RGB(120,60,20),128);
    check(!web_video_snapshot_mask(b,w,spans,mask,1,false),"undeclared translucent suffix rejected");
    check(web_video_snapshot_mask(b,w,spans,mask,1,true)&&mask[0]==2,"declared linear compositing accepted");
    {uint32_t video=RGB(200,100,40),expect=gfx_mix(video,RGB(120,60,20),128),got=video;
     web_video_snapshot_overlay(&got,spans,b,mask,1);
     bool near=true;for(int sh=0;sh<24;sh+=8){int d=(int)((got>>sh)&255)-(int)((expect>>sh)&255);if(d<-2||d>2)near=false;}
     check(near,"recomposed translucent pixel within integer rounding");}
    b[0]=w[0]=gfx_mix(RGB(4,8,12),RGB(120,60,20),128);
    check(web_video_snapshot_mask(b,w,spans,mask,1,false)&&mask[0],"alpha over opaque suffix is constant");
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
    setup();canvas_t probe;web_avmedia_snapshot_begin(&doc,&target,0,0,8,8);snapshot_candidate=&s;web_avmedia_snapshot_reject();int x,y;
    check(!web_avmedia_snapshot_prepare(&doc,&target,&probe,&x,&y,0,0),"unsafe group/frame structure rejected");web_avmedia_snapshot_finish();
    setup();web_avmedia_snapshot_begin(&doc,&target,0,0,8,8);snapshot_candidate=&s;s.snapshot.w=262145;
    check(!web_avmedia_snapshot_prepare(&doc,&target,&probe,&x,&y,0,0),"snapshot pixel cap fallback");web_avmedia_snapshot_finish();
    setup();web_avmedia_snapshot_begin(&doc,&target,0,0,8,8);snapshot_candidate=&s;oom=true;
    check(!web_avmedia_snapshot_prepare(&doc,&target,&probe,&x,&y,0,0)&&!s.snapshot.valid,"allocation failure stays ordinary paint");web_avmedia_snapshot_finish();
    setup();check(prepare(&probe),"alpha snapshot prepare");
    web_avmedia_snapshot_probe(1);gfx_fill(&probe,0,0,2,2,RGB(40,40,40));
    web_avmedia_snapshot_probe(2);gfx_fill(&probe,0,0,2,2,RGB(180,180,180));web_avmedia_snapshot_finish();
    check(!s.snapshot.valid&&native_depth==0,"translucent capture cannot arm snapshot");
    snapshot_clear(&s);check(!s.snapshot.storage&&!s.snapshot.valid,"unload releases snapshot ownership");
    {   /* A later paint whose damage crosses the top row of the armed surface. */
        setup();commit_identity();ui[4*8+2]=ui[4*8+3]=0xff445566;
        s.presented_frames++;int before=published;
        s.snapshot.band_x=2;s.snapshot.band_y=3;s.snapshot.band_w=2;s.snapshot.band_h=1;
        canvas_t probe;web_avmedia_snapshot_begin(&doc,&target,0,0,8,8);snapshot_candidate=&s;
        int x=-1,y=-1;check(web_avmedia_snapshot_prepare(&doc,&target,&probe,&x,&y,5,7),"kept surface prepares its band");
        check(x==2&&y==3&&probe.w==2&&probe.h==1&&probe.pitch==2,"probe canvas is the band within the kept surface");
        check(s.snapshot.committed[2]!=0xff445566,"rows outside the band keep their committed overlay");
        web_avmedia_snapshot_probe(1);gfx_fill(&probe,0,0,2,1,RGB(0,0,0));
        web_avmedia_snapshot_probe(2);gfx_fill(&probe,0,0,2,1,RGB(255,255,255));web_avmedia_snapshot_finish();
        check(s.snapshot.valid&&s.snapshot.w==2&&s.snapshot.h==2,"band repaint keeps the whole surface armed");
        check(s.snapshot.delivered!=s.presented_frames,"the rest of the surface still waits for the new frame");
        check(snapshot_publishable(&s),"kept surface publishable");
        snapshot_publish(&s);check(published==before+1&&s.snapshot.delivered==s.presented_frames,"next publication brings the whole surface to one frame");
    }
    {   /* A new surface the damage cut short of the video's box. */
        setup();s.snapshot.partial=true;int before=damaged;commit_identity();
        check(!s.snapshot.valid&&damaged==before+1&&!s.snapshot.partial,"partial surface is not armed and asks for the whole box");
        s.presented_frames++;int n=published;snapshot_publish(&s);check(published==n,"partial surface never published");
    }
    {   /* Three bands of a letterboxed frame, into a destination wider than the patch. */
        enum {W=3,H=70,PITCH=5};static uint32_t committed[W*H],black[W*H],dst[PITCH*H];static uint8_t opaque[W*H];
        setup();s.snapshot.committed=committed;s.snapshot.black=black;s.snapshot.opaque=opaque;
        s.snapshot.x=s.snapshot.y=0;s.snapshot.w=W;s.snapshot.h=H;
        s.snapshot.video_x=s.snapshot.video_y=0;s.snapshot.video_w=W;s.snapshot.video_h=H;
        for(int i=0;i<PITCH*H;i++)dst[i]=0xff123456;
        opaque[W*69+1]=1;committed[W*69+1]=0xff010203;
        struct snapshot_frame frame={.s=&s};snapshot_draw(&frame,dst,PITCH);
        bool bars=true,picture=true,beside=true;
        for(int y=0;y<H;y++)for(int x=0;x<PITCH;x++){
            uint32_t v=dst[y*PITCH+x];
            if(x>=W)beside&=v==0xff123456;
            else if(y>=33&&y<36)picture&=v!=RGB(0,0,0);
            else if(!(y==69&&x==1))bars&=v==RGB(0,0,0);
        }
        check(bars&&picture,"every band clears its rows and draws its part of the picture");
        check(beside,"a band writes only the patch's columns of the destination");
        check(dst[69*PITCH+1]==0xff010203,"the last band keeps its committed overlay");
        s.snapshot.committed=s.snapshot.black=NULL;s.snapshot.opaque=NULL;
    }
    printf("video-snapshot-native: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
