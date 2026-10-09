/* Production native initial-commit latch; no guest/site acceptance claim. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/libc/web/frame.h"
#define ssize_t nocturne_frame_test_ssize_t
#include "../user/libc/web/frame.c"
static int checks,failures,attempts,tokens,retirements;
static bool reject_child=true,reject_token=false;
static node_t token;
static web_doc child;
static void check(const char *label,bool value){checks++;if(!value){failures++;printf("FAIL %s\n",label);}}
node_t *doc_node_create(web_doc *d,int type,const char *name,const char *text,size_t length){
    (void)name;(void)text;(void)length;tokens++;if(reject_token)return NULL;token.owner=d;token.type=type;return &token;
}
web_doc *web_live_child(const char *html,size_t length,const char *url,const char *charset,const struct web_host *host,
                       web_doc *parent,node_t *frame,web_doc *origin){
    (void)html;(void)length;(void)url;(void)charset;(void)host;(void)origin;
    attempts++;if(reject_child)return NULL;memset(&child,0,sizeof child);child.frame_parent=parent;child.frame_element=frame;return &child;
}
void web_js_retire(web_doc *d){(void)d;retirements++;}
/* The COFF linker retains external product functions. These fail if an
   unrelated renderer/network path is accidentally used by this latch test. */
bool url_parse(const char *u,struct url *v){(void)u;(void)v;check("unexpected URL parse",false);return false;}
void web_tick(web_doc *d,uint64_t now){(void)d;(void)now;check("unexpected tick",false);}
bool web_dirty(web_doc *d){(void)d;check("unexpected layout dirty",false);return false;}
bool web_paint_dirty(web_doc *d){(void)d;check("unexpected paint dirty",false);return false;}
bool web_js_complete(web_doc *d){(void)d;check("unexpected complete check",false);return false;}
bool web_dispatch(web_doc *d,web_node *n,const struct web_event *e){(void)d;(void)n;(void)e;check("unexpected dispatch",false);return false;}
int64_t web_deadline(web_doc *d){(void)d;check("unexpected deadline",false);return -1;}
void web_resource_loaded(web_doc *d,uint64_t id,const struct web_response *r){(void)d;(void)id;(void)r;check("unexpected response",false);}
void web_free(web_doc *d){(void)d;check("unexpected doc free",false);}
void gfx_fill(canvas_t *c,int x,int y,int w,int h,uint32_t color){(void)c;(void)x;(void)y;(void)w;(void)h;(void)color;check("unexpected fill",false);}
int web_layout(web_doc *d,int w,int h){(void)d;(void)w;(void)h;check("unexpected layout",false);return 0;}
void web_paint(web_doc *d,canvas_t *c,int x,int y,int w,int h,int sx,int sy){(void)d;(void)c;(void)x;(void)y;(void)w;(void)h;(void)sx;(void)sy;check("unexpected paint",false);}
const char *node_attr(const node_t *n,const char *name){(void)n;(void)name;check("unexpected attribute",false);return NULL;}
void web_js_console(web_doc *d,int level,const char *s){(void)d;(void)level;(void)s;check("unexpected console",false);}
int main(void){
    web_doc parent={.live=true,.url="https://fixture.test/"};node_t element={.owner=&parent,.type=N_ELEM,.tag=T_iframe};
    struct web_frame frame={.element=&element,.source="/one",.srcdoc=NULL};parent.frames=&frame;
    struct web_host host={0};
    check("initial failure",!web_frame_initial_create(&frame,&host));
    check("latched failure",frame.initial_failed && frame.failed && !frame.detached);
    check("not load blocker",!web_frames_busy(&parent));
    for(int i=0;i<100;i++){check("same generation blocked",web_frame_initial_blocked(&frame,"/one",NULL));check("no allocation retry",!web_frame_initial_create(&frame,&host));}
    check("one child attempt",attempts==1);check("one token attempt",tokens==1);
    check("changed src retry allowed",!web_frame_initial_blocked(&frame,"/two",NULL));
    check("changed srcdoc retry allowed",!web_frame_initial_blocked(&frame,"/one",""));
    frame.srcdoc="one";check("same srcdoc blocked",web_frame_initial_blocked(&frame,"/one","one"));
    check("removed srcdoc retry allowed",!web_frame_initial_blocked(&frame,"/one",NULL));
    frame.srcdoc=NULL;frame.source=NULL;check("no source identity blocked",!web_frame_initial_blocked(&frame,"/one",NULL));frame.source="/one";
    check("new explicit navigation",web_frame_set_navigation(&frame,"https://fixture.test/next",&parent));
    check("navigation clears latch",!frame.initial_failed);check("navigation permits attempt",!web_frame_initial_create(&frame,&host));
    check("second attempt only",attempts==2);check("failed retained navigation blocked",web_frame_initial_blocked(&frame,"/one",NULL));
    web_frames_detach_tree(&parent,&element);check("removal clears generation",frame.detached && !frame.initial_failed && !frame.window_token);
    reject_child=false;check("reinsert creates native child",web_frame_initial_create(&frame,&host));
    check("success resets failed state",frame.document==&child && !frame.failed && !frame.initial_failed && !frame.detached);
    check("success container identity",child.frame_container==&frame);check("success not blocked",!web_frame_initial_blocked(&frame,"/one",NULL));
    frame.document=NULL;frame.window_token=NULL;reject_token=true;
    check("token allocation failure",!web_frame_initial_create(&frame,&host));int before=tokens;
    for(int i=0;i<20;i++)check("token failure no retry",!web_frame_initial_create(&frame,&host));
    check("one failed token allocation",tokens==before);check("token failure blocked",web_frame_initial_blocked(&frame,"/one",NULL));
    reject_token=false;reject_child=true;frame.detached=true;
    check("actual context allocation failure",!web_frame_initial_create(&frame,&host));check("allocation failure latched",frame.initial_failed && frame.failed && !frame.detached);
    free(frame.navigation);free(frame.navigation_origin);
    printf("frame initial failure: %d checks, %d failed\n",checks,failures);return failures!=0;
}
