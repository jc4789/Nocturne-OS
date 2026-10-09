#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <limits.h>
#include "gfx.h"
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define HTTP_URL_MAX 16384u
enum {N_DOC,N_ELEM,N_FRAGMENT,T_iframe,T_frame,T_template,T_frameset,D_BLOCK,D_NONE};
typedef struct web_doc web_doc;
typedef struct node node_t;
typedef node_t web_node;
typedef struct style {int display,visibility;} style_t;
typedef struct box {float w,h;style_t *st;} box_t;
struct node {int type,tag;bool foreign,connected;node_t *first,*next,*parent,*shadow_root,*shadow_host;web_doc *owner;box_t *box;style_t *style;};
struct web_doc {struct web_frame *frames,*frame_container;web_doc *frame_parent,*frame_retired_next,*frame_free_next,*origin_owner;node_t *frame_element,*address_frame,*body;bool live,inert,dirty,paint_dirty;const char *url,*inherited_url;};
struct web_host {int unused;};
struct web_response {int unused;};
struct web_event {const char *type;};
struct url {bool tls;char host[128];uint16_t port;char path[HTTP_URL_MAX];};
/* PRODUCT_STRUCT */
static int checks,failed,ticks,resources,frees,layouts,paints,publish_depth,max_depth;
static int live_surfaces,max_surfaces;
static bool oom;
static node_t *tokens;
static void check(bool yes,const char *name){checks++;if(!yes){failed++;printf("FAIL %s\n",name);}}
void gfx_init(canvas_t *c,uint32_t *p,int w,int h,int pitch){*c=(canvas_t){.px=p,.w=w,.h=h,.pitch=pitch,.cx1=w,.cy1=h};}
void gfx_fill(canvas_t *c,int x,int y,int w,int h,uint32_t color){
    for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)if(xx>=c->cx0&&yy>=c->cy0&&xx<c->cx1&&yy<c->cy1)c->px[yy*c->pitch+xx]=color;
}
void gfx_blit(canvas_t *d,int x,int y,const canvas_t *s,int sx,int sy,int w,int h){
    for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++)if(x+xx>=d->cx0&&y+yy>=d->cy0&&x+xx<d->cx1&&y+yy<d->cy1)d->px[(y+yy)*d->pitch+x+xx]=s->px[(sy+yy)*s->pitch+sx+xx];
}
static bool doc_node_connected(node_t *n){return n&&n->connected;}
static const char *web_url(web_doc *d){return d?d->url:NULL;}
static node_t *doc_node_create(web_doc *d,int type,const char *a,const char *b,size_t n){
    (void)a;(void)b;(void)n;node_t *made=calloc(1,sizeof *made);if(!made)return NULL;made->owner=d;made->type=type;made->next=tokens;tokens=made;return made;
}
static web_doc *web_live_child(const char *a,size_t n,const char *url,const char *charset,const struct web_host *h,web_doc *p,node_t *element,web_doc *origin){
    (void)a;(void)n;(void)charset;(void)h;(void)origin;if(oom)return NULL;
    web_doc *made=calloc(1,sizeof *made);if(made){made->url=url;made->live=true;made->frame_parent=p;made->frame_element=element;}return made;
}
static void web_js_retire(web_doc *d){d->live=false;}
void web_frames_tick(web_doc *d,uint64_t now);
int64_t web_frames_deadline(web_doc *d);
void web_frames_loaded(web_doc *d,uint64_t id,const struct web_response *r);
void web_frames_free(web_doc *d);
bool web_frame_paint(node_t *n,canvas_t *c,int x,int y,int w,int h);
static void web_tick(web_doc *d,uint64_t now){ticks++;web_frames_tick(d,now);}
static bool web_dirty(web_doc *d){bool dirty=d->dirty;d->dirty=false;return dirty;}
static bool web_paint_dirty(web_doc *d){bool dirty=d->paint_dirty;d->paint_dirty=false;return dirty;}
static bool web_js_complete(web_doc *d){(void)d;return true;}
static bool web_dispatch(web_doc *d,node_t *n,const struct web_event *event){(void)d;(void)n;(void)event;return true;}
static int64_t web_deadline(web_doc *d){int64_t n=web_frames_deadline(d);return n<0?17:n;}
static void web_resource_loaded(web_doc *d,uint64_t id,const struct web_response *r){resources++;web_frames_loaded(d,id,r);}
static void web_free(web_doc *d){if(d){web_frames_free(d);frees++;free(d);}}
static int web_layout(web_doc *d,int w,int h){(void)d;(void)w;(void)h;layouts++;return 0;}
static void web_paint(web_doc *d,canvas_t *c,int x,int y,int w,int h,int sx,int sy){
    (void)sx;(void)sy;paints++;publish_depth++;if(publish_depth>max_depth)max_depth=publish_depth;
    gfx_fill(c,x,y,w,h,0xff123456);if(d->frames)web_frame_paint(d->frames->element,c,x,y,w,h);publish_depth--;
}
static void web_js_console(web_doc *d,int l,const char *s){(void)d;(void)l;(void)s;}
/* PRODUCT_FUNCTIONS */
int main(void){
    web_doc root={.live=true,.url="https://example.test/top"};
    node_t sibling[96]={0};for(int i=0;i<96;i++){sibling[i]=(node_t){.owner=&root,.type=N_ELEM,.tag=T_iframe};check(web_frame_ensure(&root,&sibling[i])!=NULL,"sibling frame beyond former count cap");}
    check(web_frame_ensure(&root,&sibling[0])==web_frame_find(&root,&sibling[0]),"same metadata identity");
    struct web_frame *first=web_frame_find(&root,&sibling[0]);
    for(int i=0;i<180;i++)check(web_frame_commit(first,"",0,"about:blank",NULL,NULL,true),"navigation beyond former cumulative cap");
    int count=0;for(web_doc *r=first->retired;r;r=r->frame_retired_next)count++;
    check(count==179,"retained native arenas preserved after navigation");
    oom=true;web_doc *old=first->document;check(!web_frame_commit(first,"",0,"about:blank",NULL,NULL,true)&&first->document==old,"real context allocation failure keeps prior context");oom=false;
    check(web_frame_ancestor_url(&root,"https://EXAMPLE.test:443/top#next")==1,"same ancestor URL excludes fragment and canonical origin");
    check(web_frame_ancestor_url(&root,"https://example.test/other")==0,"same domain different resource allowed");
    check(web_frame_ancestor_url(&root,"https://example.test/top?x=1")==0,"different query allowed");
    web_frames_free(&root);check(!root.frames&&frees==180,"all uncapped retired documents freed");
    enum {DEPTH=2048};node_t *nodes=calloc(DEPTH,sizeof *nodes);box_t *boxes=calloc(DEPTH,sizeof *boxes);style_t style={.display=D_BLOCK};
    web_doc *parent=&root;struct web_frame *leaf=NULL;
    for(int i=0;i<DEPTH;i++){
        nodes[i]=(node_t){.owner=parent,.type=N_ELEM,.tag=T_iframe,.connected=true,.box=&boxes[i],.style=&style};boxes[i]=(box_t){.w=2,.h=2,.st=&style};
        leaf=web_frame_ensure(parent,&nodes[i]);if(!leaf||!web_frame_commit(leaf,"",0,"about:srcdoc",NULL,NULL,true)){check(false,"deep forest materialization");return 1;}parent=leaf->document;
    }
    check(parent->frame_parent!=NULL&&leaf->document->frame_container==leaf,"deep native container links");
    count=0;for(struct web_frame *f=root.frames;f;f=web_frame_walk_next(&root,f,true))count++;
    check(count==DEPTH,"stackless document walk exceeds former depth");
    ticks=0;web_frames_tick(&root,0);check(ticks==DEPTH,"deep tick once per live child, no recursive descent");
    check(web_frames_deadline(&root)==17,"deep deadline native minimum");resources=0;web_frames_loaded(&root,9,NULL);check(resources==DEPTH,"deep resource completion reaches every child once");
    root.address_frame=leaf->element;check(web_frame_address(&root)==leaf->document->url&&!strcmp(root.url,"https://example.test/top"),"deep child address keeps root URL independent");
    web_frames_prepare_paint(&root);check(layouts==DEPTH&&paints==DEPTH&&max_depth==1,"postorder paint removes child C recursion");
    check(root.frames->paint.px&&root.frames->paint.px[0]==0xff123456,"deep final composite preserved");
    check(!leaf->paint.px,"consumed child surfaces released before top publication");
    web_frames_finish_paint(&root);check(!root.frames->paint.px,"paint completion releases native surfaces");
    leaf->detached=true;check(web_frame_address(&root)==NULL,"stale deep address selection rejected");
    int before=frees;web_frames_free(&root);check(frees-before==DEPTH&&!root.frames,"deep postorder teardown without native stack growth");
    node_t *tree=calloc(2049,sizeof *tree);for(int i=0;i<2048;i++){tree[i].first=&tree[i+1];tree[i+1].parent=&tree[i];}
    count=0;for(node_t *n=tree;n;n=web_frame_dom_next(tree,n))count++;
    check(count==2049,"DOM traversal exceeds removed depth400 gate");free(tree);
    while(tokens){node_t *n=tokens;tokens=n->next;free(n);}free(nodes);free(boxes);
    printf("frame-unbounded-native: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
