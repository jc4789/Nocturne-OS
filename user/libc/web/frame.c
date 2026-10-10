#include "frame.h"
#include <nocturne.h>
#include <math.h>
#include <limits.h>

const char *web_effective_url(web_doc *d) {
    return d && d->inherited_url ? d->inherited_url : d ? d->url : "about:blank";
}
bool web_frame_element(const node_t *n) {
    return n && n->type==N_ELEM && !n->foreign && (n->tag==T_iframe || n->tag==T_frame);
}
/* Native trees are acyclic by DOM insertion validation. These walks need no
 * depth array, allocator or C recursion, including shadow roots. */
node_t *web_frame_dom_next(node_t *root,node_t *n) {
    if(!root || !n)return NULL;
    if(!(n->type==N_ELEM && !n->foreign && n->tag==T_template)) {
        if(n->shadow_root)return n->shadow_root;
        if(n->first)return n->first;
    }
    while(n && n!=root) {
        if(n->shadow_host){n=n->shadow_host;if(n->first)return n->first;}
        else {if(n->next)return n->next;n=n->parent;}
    }
    return NULL;
}
struct web_frame *web_frame_walk_next(web_doc *root,struct web_frame *f,bool descend) {
    if(!f)return NULL;
    if(descend && f->document && !f->detached && f->document->frames)return f->document->frames;
    while(f) {
        if(f->next)return f->next;
        if(!f->owner || f->owner==root)return NULL;
        f=f->owner->frame_container;
    }
    return NULL;
}
int web_frame_ancestor_url(web_doc *parent,const char *text) {
    if(!parent || !text)return false;
    /* srcdoc takes a different HTML processing branch. An initial blank
     * context is created independently and never reaches this URL check. */
    if(!strncasecmp(text,"about:",6)) {
        size_t n=strcspn(text,"#");
        for(web_doc *p=parent;p;p=p->frame_parent) {
            size_t m=strcspn(p->url?p->url:"","#");
            if(m==n && !strncmp(text,p->url?p->url:"",n))return true;
        }
        return false;
    }
    char *a=NULL,*origin=NULL;
    enum http_url_result status=http_canonical_owned_n(text,strlen(text),&a,&origin);
    free(origin);
    /* An actual allocation failure never bypasses recursive-resource checks. */
    if(status==HTTP_URL_OOM)return -1;
    bool matches=false;
    if(status==HTTP_URL_TUPLE)for(web_doc *p=parent;p;p=p->frame_parent) {
        char *b=NULL;origin=NULL;
        enum http_url_result own_status=http_canonical_owned_n(p->url,p->url?strlen(p->url):0,&b,&origin);
        free(origin);
        if(own_status==HTTP_URL_OOM){free(a);free(b);return -1;}
        if(own_status==HTTP_URL_TUPLE && strcspn(a,"#")==strcspn(b,"#") &&
           !strncmp(a,b,strcspn(a,"#")))matches=true;
        free(b);if(matches)break;
    }
    if(!matches)for(web_doc *p=parent;p;p=p->frame_parent) {
        const char *own=p->url?p->url:"";size_t n=strcspn(text,"#");
        if(strcspn(own,"#")==n && !strncmp(text,own,n)){matches=true;break;}
    }
    free(a);return matches;
}
struct web_frame *web_frame_find(web_doc *d,node_t *n) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)if(f->element==n)return f;
    return NULL;
}
struct web_frame *web_frame_ensure(web_doc *d,node_t *n) {
    struct web_frame *f=web_frame_find(d,n);
    if(f)return f;
    if(!d || !web_frame_element(n))return NULL;
    f=calloc(1,sizeof *f);if(!f)return NULL;
    f->owner=d;f->element=n;f->dom_order=UINT32_MAX;f->next=d->frames;d->frames=f;
    return f;
}
void web_frame_retire(struct web_frame *f) {
    if(!f || !f->document)return;
    web_doc *old=f->document;
    doc_active_cancel(old);
    free(f->paint.px);memset(&f->paint,0,sizeof f->paint);
    /* A same-origin caller can retain Document/Node values after navigation.
       Keep their native arenas until the enclosing page dies. Actual allocator
       failure, not an arbitrary navigation counter, limits retained arenas. */
    old->frame_retired_next=f->retired;f->retired=old;web_js_retire(old);old->live=false;
    f->document=NULL;f->notified=false;
}
void web_frames_detach_tree(web_doc *d,node_t *root) {
    if(!d || !root)return;
    /* This hook precedes all native subtree detach/move operations, including
       ordinary nodes. A removed pressed target must not revive on reinsertion. */
    for(node_t *p=d->active_target;p;p=doc_shadow_parent(p))
        if(p==root){web_active_release(d);break;}
    if(web_frame_element(root)) {
        struct web_frame *f=web_frame_find(d,root);
        if(f){f->detached=true;f->initial_failed=false;f->window_token=NULL;web_frame_retire(f);}
    }
    for(node_t *node=web_frame_dom_next(root,root);node;node=web_frame_dom_next(root,node))
        if(web_frame_element(node)) {
            struct web_frame *f=web_frame_find(d,node);
            if(f){f->detached=true;f->initial_failed=false;f->window_token=NULL;web_frame_retire(f);}
        }
}
bool web_frame_set_navigation(struct web_frame *f,const char *url,web_doc *origin) {
    if(!f || !url || !origin)return false;
    char *copy=strdup(url),*initiator=strdup(web_effective_url(origin));
    if(!copy || !initiator){free(copy);free(initiator);return false;}
    free(f->navigation);free(f->navigation_origin);f->navigation=copy;f->navigation_origin=initiator;f->navigation_owner=origin;f->initial_failed=false;return true;
}
bool web_frame_initial_blocked(const struct web_frame *f,const char *src,const char *srcdoc) {
    return f && f->initial_failed && !f->detached && f->source && !strcmp(f->source,src?src:"") &&
        ((!f->srcdoc && !srcdoc)||(f->srcdoc && srcdoc && !strcmp(f->srcdoc,srcdoc)));
}
bool web_frame_initial_create(struct web_frame *f,const struct web_host *host) {
    if(!f || (f->initial_failed && !f->detached))return false;
    if(web_frame_commit(f,"",0,"about:blank",NULL,host,true))return true;
    // A failed generation is quiescent, not an invitation to allocate and log
    // again on each unrelated DOM revision/contentWindow read.
    f->initial_failed=true;f->failed=true;f->detached=false;return false;
}
bool web_frame_commit(struct web_frame *f,const char *html,size_t n,const char *url,const char *charset,
                      const struct web_host *host,bool inherited) {
    web_doc *parent=f?f->element->owner:NULL;
    if(!parent)return false;
    if(!f->window_token) {
        f->window_token=doc_node_create(parent,N_FRAGMENT,NULL,"",0);
        if(!f->window_token)return false;
    }
    web_doc *made=web_live_child(html,n,url,charset,host,parent,f->element,
        inherited?(f->navigation_owner?f->navigation_owner:parent):NULL);
    if(!made)return false;
    made->frame_container=f;
    web_frame_retire(f);f->document=made;f->failed=false;f->initial_failed=false;f->detached=false;f->notified=false;f->paint_failed=false;
    parent->dirty=true;return true;
}
bool web_frame_same_origin(web_doc *a,web_doc *b) {
    if(a==b)return true;
    if(!a || !b)return false;
    if((a->origin_owner?a->origin_owner:a)==(b->origin_owner?b->origin_owner:b))return true;
    char *x=NULL,*y=NULL;
    enum http_url_result xr=http_origin_owned(web_effective_url(a),&x),
        yr=http_origin_owned(web_effective_url(b),&y);
    bool same=xr==HTTP_URL_TUPLE && yr==HTTP_URL_TUPLE && !strcmp(x,y);
    free(x);free(y);return same;
}
void web_frames_tick(web_doc *d,uint64_t now) {
    static bool running;if(running || !d)return;running=true;
    struct web_frame *f=d->frames;bool entering=true;
    while(f) {
        web_doc *child=f->document;
        if(entering && child && !f->detached && child->live) {
            web_tick(child,now);
            if(f->document==child && !f->detached && child->frames){f=child->frames;continue;}
        }
        child=f->document;
        if(child && !f->detached) {
            if(web_dirty(child))f->owner->dirty=true;
            if(web_paint_dirty(child))f->owner->paint_dirty=true;
        }
        if(child && !f->detached && !f->request && !f->notified && web_js_complete(child)) {
            f->notified=true;
            struct web_event event={.type="load"};web_dispatch(f->owner,f->element,&event);
        }
        if(f->next){f=f->next;entering=true;}
        else if(f->owner==d)break;
        else {f=f->owner->frame_container;entering=false;}
    }
    running=false;
}
int64_t web_frames_deadline(web_doc *d) {
    static bool running;if(running)return -1;running=true;
    int64_t result=-1;
    for(struct web_frame *f=d?d->frames:NULL;f;f=web_frame_walk_next(d,f,true))if(f->document && !f->detached) {
        int64_t deadline=web_deadline(f->document);
        if(deadline>=0 && (result<0 || deadline<result))result=deadline;
    }
    running=false;return result;
}
bool web_frames_busy(web_doc *d) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)
        if(!f->detached && !f->failed && (f->request || (f->document && !web_js_complete(f->document))))return true;
    return false;
}
void web_frames_loaded(web_doc *d,uint64_t id,const struct web_response *r) {
    static bool running;if(running)return;running=true;
    for(struct web_frame *f=d?d->frames:NULL;f;) {
        struct web_frame *next=web_frame_walk_next(d,f,true);
        if(f->document && !f->detached)web_resource_loaded(f->document,id,r);
        f=next;
    }
    running=false;
}
void web_frames_free(web_doc *d) {
    if(!d)return;
    web_doc *pending=d,*finished=NULL;
    while(pending) {
        web_doc *current=pending;pending=current->frame_free_next;current->frame_free_next=NULL;
        while(current->frames) {
            struct web_frame *f=current->frames;current->frames=f->next;
            if(f->document){f->document->frame_free_next=pending;pending=f->document;}
            while(f->retired){web_doc *old=f->retired;f->retired=old->frame_retired_next;old->frame_free_next=pending;pending=old;}
            free(f->paint.px);free(f->source);free(f->srcdoc);free(f->navigation);free(f->navigation_origin);free(f->sandbox);free(f);
        }
        if(current!=d){current->frame_free_next=finished;finished=current;}
    }
    /* Reverse discovery keeps every descendant's native owner alive until
     * its realm/cache finalizers have completed. All frame lists are empty. */
    while(finished){web_doc *old=finished;finished=old->frame_free_next;old->frame_free_next=NULL;web_free(old);}
}
static bool frame_surface_prepare(struct web_frame *f) {
    box_t *b=f->element?f->element->box:NULL;
    /* An old publication must not be overwritten by gfx_init (or reused after
       the element becomes unrendered). Ownership is independent of the crop. */
    free(f->paint.px);memset(&f->paint,0,sizeof f->paint);f->paint_visible=false;
    if(!f->document || f->detached || !b || !b->st || b->st->display==D_NONE ||
       b->st->visibility || b->st->content_visibility==CV_HIDDEN ||
       !isfinite(b->w) || !isfinite(b->h) || b->w<0.5f || b->h<0.5f ||
       (double)b->w>(double)INT_MAX-1 || (double)b->h>(double)INT_MAX-1)return false;
    int w=(int)roundf(b->w),h=(int)roundf(b->h);
    f->viewport_w=w;f->viewport_h=h;
    f->document->view_x=f->scroll_x;f->document->view_y=f->scroll_y;
    web_layout(f->document,w,h);
    web_doc *owner=f->owner;struct web_frame *container=owner->frame_container;
    double left=owner->view_x,top=owner->view_y;
    double right=left+owner->width,bottom=top+owner->height;
    if(container) {
        left+=container->paint_x;top+=container->paint_y;
        right=left+container->paint.w;bottom=top+container->paint.h;
    }
    double x=box_visual_x(b),y=(double)box_visual_y(b)+b->content_dy;
    bool fixed=false;
    for(box_t *a=b;a;a=a->parent)if(a->st && a->st->position==POS_FIXED){fixed=true;break;}
    if(fixed){x+=owner->view_x;y+=owner->view_y;}
    /* Nested overflow clips are real paint bounds, not a document-size quota.
       Viewport-fixed descendants follow the same escape as native paint. */
    if(!fixed)for(box_t *a=b->parent;a;a=a->parent)if(a->st && a->kind!=B_INLINE &&
        a->st->overflow!=OV_VISIBLE && !doc_viewport_overflow_box(owner,a)) {
        double ax=(double)box_visual_x(a)-a->p[3];
        double ay=(double)box_visual_y(a)+a->content_dy-a->p[0];
        left=fmax(left,ax);top=fmax(top,ay);
        right=fmin(right,ax+a->w+a->p[3]+a->p[1]);
        bottom=fmin(bottom,ay+a->h+a->p[0]+a->p[2]);
    }
    left=fmax(left,x);top=fmax(top,y);right=fmin(right,x+w);bottom=fmin(bottom,y+h);
    if(!isfinite(left) || !isfinite(top) || !isfinite(right) || !isfinite(bottom) || right<=left || bottom<=top)return false;
    int ox=(int)fmax(0,floor(left-x)),oy=(int)fmax(0,floor(top-y));
    int ex=(int)fmin(w,ceil(right-x)),ey=(int)fmin(h,ceil(bottom-y));
    int cw=ex-ox,ch=ey-oy;
    if(cw<=0 || ch<=0 || (size_t)cw>SIZE_MAX/4/(size_t)ch)return false;
    f->paint_x=ox;f->paint_y=oy;f->paint.w=cw;f->paint.h=ch;f->paint.pitch=cw;
    f->paint_visible=true;return true;
}
void web_frames_prepare_paint(web_doc *d) {
    /* Layout flows top-down, then published pixels bottom-up. Each child
     * paint blits already completed children; browsing-context depth consumes
     * heap surfaces, never another C paint/layout call stack. */
    struct web_frame *f=d?d->frames:NULL;bool entering=true;
    while(f) {
        if(entering && frame_surface_prepare(f) && f->document->frames){f=f->document->frames;continue;}
        if(f->paint_visible && f->paint.w && f->paint.h && f->document && !f->detached) {
            int w=f->paint.w,h=f->paint.h;uint32_t *pixels=malloc((size_t)w*h*4);
            if(pixels){gfx_init(&f->paint,pixels,w,h,w);f->paint_failed=false;
                /* Negative crop origin leaves layout/media queries/fixed
                   positioning and the author's actual scroll unchanged. */
                web_paint(f->document,&f->paint,-f->paint_x,-f->paint_y,f->viewport_w,f->viewport_h,f->scroll_x,f->scroll_y);}
            else if(!f->paint_failed){f->paint_failed=true;web_js_console(f->owner,2,"Frame paint surface allocation failed");}
            /* A completed parent owns the composite. Child surfaces no longer
             * need to survive, so a deep single chain peaks at two surfaces. */
            for(struct web_frame *c=f->document->frames;c;c=c->next){free(c->paint.px);memset(&c->paint,0,sizeof c->paint);}
        }
        if(f->next){f=f->next;entering=true;}
        else if(f->owner==d)break;
        else {f=f->owner->frame_container;entering=false;}
    }
}
void web_frames_finish_paint(web_doc *d) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=web_frame_walk_next(d,f,true)) {
        free(f->paint.px);memset(&f->paint,0,sizeof f->paint);
    }
}
bool web_frame_paint(node_t *n,canvas_t *c,int x,int y,int w,int h) {
    struct web_frame *f=web_frame_find(n?n->owner:NULL,n);
    if(!web_frame_element(n))return false;
    if(w<=0 || h<=0)return true;
    if(!f || !f->document || f->detached) {gfx_fill(c,x,y,w,h,RGB(255,255,255));return true;}
    if(!f->paint_visible)return true; /* no pixel intersects the parent viewport */
    if(!f->paint.px || f->viewport_w!=w || f->viewport_h!=h) {
        /* Actual OOM must not replace a usable document with a white box.
           The parent's existing canvas is a valid native paint target. This
           emergency path allocates no additional full-frame pixel buffer. */
        web_paint(f->document,c,x,y,w,h,f->scroll_x,f->scroll_y);return true;
    }
    int64_t px=(int64_t)x+f->paint_x,py=(int64_t)y+f->paint_y;
    if(px>=INT_MIN && px<=INT_MAX && py>=INT_MIN && py<=INT_MAX)
        gfx_blit(c,(int)px,(int)py,&f->paint,0,0,f->paint.w,f->paint.h);
    return true;
}
static web_doc *named_frame(web_doc *d,const char *name) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=web_frame_walk_next(d,f,true))if(f->document && !f->detached){
        const char *own=node_attr(f->element,"name");if(own && !strcmp(own,name))return f->document;
    }
    return NULL;
}
static bool frame_link_space(unsigned char c) {
    return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f';
}
static bool frame_link_token(const char *value,const char *token) {
    if(!value)return false;
    size_t length=strlen(token);
    while(*value){
        while(frame_link_space((unsigned char)*value))value++;
        const char *start=value;while(*value && !frame_link_space((unsigned char)*value))value++;
        if((size_t)(value-start)==length && !strncasecmp(start,token,length))return true;
    }
    return false;
}
static const char *frame_link_target(web_doc *source,node_t *anchor) {
    const char *target=node_attr(anchor,"target");
    if(target)return target; /* An explicitly empty target overrides base. */
    node_t *root=source->root,*n=root;
    while(n){
        if(n->type==N_ELEM && n->namespace_id==NS_HTML && n->name && !strcmp(n->name,"base")){
            target=node_attr(n,"target");if(target)return target;
        }
        /* Only the document tree participates, not shadow/template contents. */
        if(n->first && !(n->type==N_ELEM && n->namespace_id==NS_HTML && n->name && !strcmp(n->name,"template")))n=n->first;
        else {while(n!=root && !n->next)n=n->parent;if(n==root)break;n=n->next;}
    }
    return NULL;
}
static bool frame_auxiliary_allowed(web_doc *source) {
    for(web_doc *d=source;d;d=d->frame_parent){
        if(!d->live || d->inert){web_js_console(source,1,"Inactive document cannot open a link window");return false;}
        if(!d->frame_parent)break;
        struct web_frame *f=web_frame_find(d->frame_parent,d->frame_element);
        if(!f || f->detached || f->document!=d){web_js_console(source,1,"Detached frame cannot open a link window");return false;}
        /* The saved sandbox belongs to this active document. A later DOM
           attribute mutation must not invent different active sandbox flags. */
        if(!f->sandbox)continue;
        if(!frame_link_token(f->sandbox,"allow-popups")){
            web_js_console(source,1,"Sandbox disallows auxiliary link navigation");return false;
        }
        if(!frame_link_token(f->sandbox,"allow-popups-to-escape-sandbox")){
            web_js_console(source,1,"Auxiliary window sandbox inheritance is not implemented");return false;
        }
    }
    return true;
}
bool web_frame_navigate(web_doc *top,web_node *anchor,const char *url) {
    web_doc *source=anchor?anchor->owner:NULL;
    if(!top || !source || !url)return false;
    if(!source->live){web_js_console(source,1,"Inactive document link navigation discarded");return true;}
    web_doc *root=source;while(root->frame_parent)root=root->frame_parent;
    if(root!=top){web_js_console(source,1,"Link source belongs to another browsing context");return true;}
    const char *target=frame_link_target(source,anchor);web_doc *destination=source;
    if(target && *target && strcasecmp(target,"_self")){
        if(!strcasecmp(target,"_top"))destination=top;
        else if(!strcasecmp(target,"_parent"))destination=source->frame_parent?source->frame_parent:source;
        else if(!strcasecmp(target,"_blank")){
            if(!frame_auxiliary_allowed(source))return true;
            const char *rel=node_attr(anchor,"rel");
            if(frame_link_token(rel,"opener") && !frame_link_token(rel,"noopener") && !frame_link_token(rel,"noreferrer")){
                web_js_console(source,1,"Auxiliary link opener relationships are not implemented");return true;
            }
            if(!web_js_auxiliary_link(source,url))web_js_console(source,2,"Auxiliary link requires an HTTP(S) destination and a native window host");
            return true;
        }
        else {destination=named_frame(top,target);if(!destination){web_js_console(source,1,"Named link target has no browsing context");return true;}}
    }
    if(destination==top)return false;
    struct web_frame *frame=web_frame_find(destination->frame_parent,destination->frame_element);
    if(!web_frame_set_navigation(frame,url,source)){web_js_console(source,2,"Child link navigation could not retain its URL and initiator");return true;}
    top->address_frame=destination->frame_element;
    top->dirty=true;return true;
}
