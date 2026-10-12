#include "web_presentation.h"
#include "avmedia.h"
#include "nocturne.h"
#include <string.h>
static bool valid(web_doc *d,node_t *n){return d&&d->live&&!d->inert&&n&&n->owner==d&&n->type==N_ELEM&&doc_node_connected(n);}
static bool contains(node_t *root,node_t *node){for(;node;node=doc_shadow_parent(node))if(root==node)return true;return false;}
node_t *web_presentation_element(web_doc *d,int kind){
    if(!d)return NULL;node_t *n=kind==1?d->fullscreen_element:d->pip_element;return valid(d,n)?n:NULL;
}
void web_presentation_resize(web_doc *d,int w,int h){if(d&&w>0&&h>0){d->pip_width=w;d->pip_height=h;}}
void web_presentation_size(web_doc *d,int *w,int *h){if(w)*w=d?d->pip_width:0;if(h)*h=d?d->pip_height:0;}
static void changed(web_doc *d,node_t *n){d->presentation_revision++;d->dirty=d->paint_dirty=true;if(n)css_mark_dirty(d,n);}
int web_presentation_request(web_doc *d,node_t *n,int kind,const struct web_host *host){
    if(!valid(d,n))return WEB_PRESENT_INACTIVE;
    if(kind==1&&n->namespace_id!=NS_HTML&&
       !(n->namespace_id==NS_SVG&&n->name&&!strcmp(n->name,"svg"))&&
       !(n->namespace_id==NS_MATHML&&n->name&&!strcmp(n->name,"math")))return WEB_PRESENT_UNSUPPORTED;
    if((kind!=1&&kind!=2)||d->frame_parent||n->tag==T_dialog||!host||!host->presentation)return WEB_PRESENT_UNSUPPORTED;
    if(d->sandbox_activation_until<=uptime_ms())return WEB_PRESENT_ACTIVATION;
    if(kind==2&&(n->foreign||n->tag!=T_video||node_attr(n,"disablepictureinpicture")||!web_avmedia_pip_ready(d,n)))return WEB_PRESENT_MEDIA;
    if(kind==1&&d->fullscreen_element==n)return WEB_PRESENT_OK;
    if(kind==2&&d->pip_element==n)return WEB_PRESENT_OK;
    if(kind==1&&d->fullscreen_element&&!web_presentation_exit(d,1))return WEB_PRESENT_BACKEND;
    if(kind==2&&d->pip_element&&!web_presentation_exit(d,2))return WEB_PRESENT_BACKEND;
    if(!host->presentation(host->opaque,d,n,kind))return WEB_PRESENT_BACKEND;
    d->presentation_host=host->presentation;d->presentation_opaque=host->opaque;
    if(kind==1)d->fullscreen_element=n;else d->pip_element=n;
    d->sandbox_activation_until=0;changed(d,n);return WEB_PRESENT_OK;
}
bool web_presentation_exit(web_doc *d,int kind){
    if(!d)return false;node_t *n=kind==1?d->fullscreen_element:d->pip_element;
    if(!n)return true;
    if(d->presentation_host&&!d->presentation_host(d->presentation_opaque,d,n,kind==1?0:3))return false;
    if(kind==1)d->fullscreen_element=NULL;else d->pip_element=NULL;changed(d,n);return true;
}
void web_presentation_sync(web_doc *d){if(!d)return;if(d->fullscreen_element&&!valid(d,d->fullscreen_element))web_presentation_exit(d,1);if(d->pip_element&&!valid(d,d->pip_element))web_presentation_exit(d,2);}
void web_presentation_removed(web_doc *d,node_t *subtree){
    if(!d||!subtree)return;
    node_t *full=d->fullscreen_element,*pip=d->pip_element;
    if(full&&contains(subtree,full))web_presentation_exit(d,1);
    if(pip&&contains(subtree,pip))web_presentation_exit(d,2);
}
void web_presentation_free(web_doc *d){if(!d)return;web_presentation_exit(d,1);web_presentation_exit(d,2);d->presentation_host=NULL;d->presentation_opaque=NULL;}
void web_presentation_style(web_doc *d,node_t *n,style_t *s){
    if(!d||n!=d->fullscreen_element)return;
    if(s->display==D_CONTENTS||s->display==D_INLINE)s->display=D_BLOCK;
    s->position=POS_FIXED;s->float_=FL_NONE;s->box_sizing=1;
    s->width=(len_t){.kind=LK_LEN,.pct=100};s->height=(len_t){.kind=LK_LEN,.pct=100};
    s->min_width=s->min_height=(len_t){.kind=LK_LEN};s->max_width=s->max_height=(len_t){.kind=LK_NONE};
    for(unsigned i=0;i<4;i++){s->inset[i]=(len_t){.kind=LK_AUTO};s->margin[i]=(len_t){.kind=LK_LEN};}
    s->inset[0]=s->inset[3]=(len_t){.kind=LK_LEN};
}
