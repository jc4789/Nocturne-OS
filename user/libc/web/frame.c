#include "frame.h"
#include <nocturne.h>

const char *web_effective_url(web_doc *d) {
    return d && d->inherited_url ? d->inherited_url : d ? d->url : "about:blank";
}
bool web_frame_element(const node_t *n) {
    return n && n->type==N_ELEM && !n->foreign && (n->tag==T_iframe || n->tag==T_frame);
}
struct web_frame *web_frame_find(web_doc *d,node_t *n) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)if(f->element==n)return f;
    return NULL;
}
struct web_frame *web_frame_ensure(web_doc *d,node_t *n) {
    struct web_frame *f=web_frame_find(d,n);
    if(f)return f;
    web_doc *top=d;while(top && top->frame_parent)top=top->frame_parent;
    if(!d || !top || !web_frame_element(n) || d->frame_depth>=WEB_FRAME_DEPTH_MAX || top->frame_count>=WEB_FRAMES_MAX)return NULL;
    f=calloc(1,sizeof *f);if(!f)return NULL;
    f->element=n;f->dom_order=UINT32_MAX;f->next=d->frames;d->frames=f;top->frame_count++;
    return f;
}
void web_frame_retire(struct web_frame *f) {
    if(!f || !f->document)return;
    web_doc *old=f->document;
    /* A same-origin caller can retain Document/Node values after navigation.
       Keep their native arenas until the enclosing page dies, under a finite
       navigation bound. Never free native nodes behind escaped JS wrappers. */
    old->frame_retired_next=f->retired;f->retired=old;web_js_retire(old);old->live=false;
    f->document=NULL;f->notified=false;
}
void web_frames_detach_tree(web_doc *d,node_t *root) {
    if(!d || !root)return;
    if(web_frame_element(root)) {
        struct web_frame *f=web_frame_find(d,root);
        if(f){f->detached=true;f->initial_failed=false;f->window_token=NULL;web_frame_retire(f);}
    }
    if(root->shadow_root)web_frames_detach_tree(d,root->shadow_root);
    for(node_t *child=root->first;child;child=child->next)web_frames_detach_tree(d,child);
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
    web_doc *parent=f?f->element->owner:NULL,*top=parent;
    while(top && top->frame_parent)top=top->frame_parent;
    if(!parent || !top || top->frame_navigation_count>=WEB_FRAME_NAVIGATIONS_MAX)return false;
    if(!f->window_token) {
        f->window_token=doc_node_create(parent,N_FRAGMENT,NULL,"",0);
        if(!f->window_token)return false;
    }
    web_doc *made=web_live_child(html,n,url,charset,host,parent,f->element,
        inherited?(f->navigation_owner?f->navigation_owner:parent):NULL);
    if(!made)return false;
    top->frame_navigation_count++;
    web_frame_retire(f);f->document=made;f->failed=false;f->initial_failed=false;f->detached=false;f->notified=false;
    parent->dirty=true;return true;
}
bool web_frame_same_origin(web_doc *a,web_doc *b) {
    if(a==b)return true;
    if(!a || !b)return false;
    if((a->origin_owner?a->origin_owner:a)==(b->origin_owner?b->origin_owner:b))return true;
    struct url *x=malloc(sizeof *x),*y=malloc(sizeof *y);
    bool same=x && y && url_parse(web_effective_url(a),x) && url_parse(web_effective_url(b),y) &&
        x->tls==y->tls && x->port==y->port && !strcasecmp(x->host,y->host);
    free(x);free(y);return same;
}
void web_frames_tick(web_doc *d,uint64_t now) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=f->next) {
        web_doc *child=f->document;
        if(!child || f->detached)continue;
        web_tick(child,now);
        if(web_dirty(child))d->dirty=true;
        if(web_paint_dirty(child))d->paint_dirty=true;
        if(!f->request && !f->notified && web_js_complete(child)) {
            f->notified=true;
            struct web_event event={.type="load"};web_dispatch(d,f->element,&event);
        }
    }
}
int64_t web_frames_deadline(web_doc *d) {
    int64_t result=-1;
    for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)if(f->document && !f->detached) {
        int64_t deadline=web_deadline(f->document);
        if(deadline>=0 && (result<0 || deadline<result))result=deadline;
    }
    return result;
}
bool web_frames_busy(web_doc *d) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)
        if(!f->detached && !f->failed && (f->request || (f->document && !web_js_complete(f->document))))return true;
    return false;
}
void web_frames_loaded(web_doc *d,uint64_t id,const struct web_response *r) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)if(f->document && !f->detached)
        web_resource_loaded(f->document,id,r);
}
void web_frames_free(web_doc *d) {
    while(d && d->frames) {
        struct web_frame *f=d->frames;d->frames=f->next;
        web_free(f->document);
        while(f->retired){web_doc *old=f->retired;f->retired=old->frame_retired_next;web_free(old);}
        free(f->source);free(f->srcdoc);free(f->navigation);free(f->navigation_origin);free(f->sandbox);free(f);
    }
}
bool web_frame_paint(node_t *n,canvas_t *c,int x,int y,int w,int h) {
    struct web_frame *f=web_frame_find(n?n->owner:NULL,n);
    if(!web_frame_element(n))return false;
    if(w<=0 || h<=0)return true;
    if(!f || !f->document || f->detached) {gfx_fill(c,x,y,w,h,RGB(255,255,255));return true;}
    web_layout(f->document,w,h);
    web_paint(f->document,c,x,y,w,h,f->scroll_x,f->scroll_y);
    return true;
}
static web_doc *named_frame(web_doc *d,const char *name) {
    for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)if(f->document && !f->detached){
        const char *own=node_attr(f->element,"name");if(own && !strcmp(own,name))return f->document;
        web_doc *nested=named_frame(f->document,name);if(nested)return nested;
    }
    return NULL;
}
bool web_frame_navigate(web_doc *top,web_node *anchor,const char *url) {
    web_doc *source=anchor?anchor->owner:NULL;
    if(!top || !source || !url || !source->live)return false;
    web_doc *root=source;while(root->frame_parent)root=root->frame_parent;
    if(root!=top)return false;
    const char *target=node_attr(anchor,"target");web_doc *destination=source;
    if(target && *target && strcasecmp(target,"_self")){
        if(!strcasecmp(target,"_top"))destination=top;
        else if(!strcasecmp(target,"_parent"))destination=source->frame_parent?source->frame_parent:source;
        else if(!strcasecmp(target,"_blank")){web_js_console(source,1,"New auxiliary browsing contexts are not implemented");return true;}
        else {destination=named_frame(top,target);if(!destination){web_js_console(source,1,"Named link target has no browsing context");return true;}}
    }
    if(destination==top)return false;
    struct web_frame *frame=web_frame_find(destination->frame_parent,destination->frame_element);
    if(!web_frame_set_navigation(frame,url,source)){web_js_console(source,2,"Child link navigation could not retain its URL and initiator");return true;}
    top->dirty=true;return true;
}
