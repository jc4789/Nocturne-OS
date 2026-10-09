/* Native diagnostic only: no author strings, URLs, tokens or state mutation. */
struct web_paint_diagnostic {
    uint64_t id, signature, reported_at;
    bool reached;
    int rect[4], clip[4];
};
static bool paint_debug_enabled;
void web_paint_debug(bool enabled) { paint_debug_enabled=enabled; }
void web_paint_debug_node_free(node_t *n) {
    if(n){free(n->paint_diagnostic);n->paint_diagnostic=NULL;}
}
static node_t *paint_debug_media_next(web_doc *d,node_t *previous) {
    web_doc *family=d->dom_family?d->dom_family:d;
    web_doc *allocation=previous?previous->allocation_doc:family;
    node_t *n=previous?previous->owned_next:allocation->owned_nodes;
    while(allocation) {
        for(;n;n=n->owned_next)
            if(n->owner==d && n->type==N_ELEM && !n->foreign && (n->tag==T_video || n->tag==T_audio))return n;
        allocation=allocation==family?family->dom_docs:allocation->dom_next;
        n=allocation?allocation->owned_nodes:NULL;
    }
    return NULL;
}
static void paint_debug_begin(web_doc *d) {
    static uint64_t serial;
    if(!paint_debug_enabled)return;
    for(node_t *n=paint_debug_media_next(d,NULL);n;n=paint_debug_media_next(d,n)) {
        if(!n->paint_diagnostic) {
            n->paint_diagnostic=calloc(1,sizeof *n->paint_diagnostic);
            if(!n->paint_diagnostic)continue; /* Diagnostic OOM cannot change rendering. */
            n->paint_diagnostic->id=serial==UINT64_MAX?0:++serial;
        }
        struct web_paint_diagnostic *p=n->paint_diagnostic;
        p->reached=false;memset(p->rect,0,sizeof p->rect);memset(p->clip,0,sizeof p->clip);
    }
}
static void paint_debug_reached(node_t *n,canvas_t *c,int x,int y,int w,int h) {
    if(!paint_debug_enabled || web_avmedia_snapshot_is_probe() || !n->paint_diagnostic)return;
    struct web_paint_diagnostic *p=n->paint_diagnostic;p->reached=true;
    p->rect[0]=x;p->rect[1]=y;p->rect[2]=w;p->rect[3]=h;
    p->clip[0]=c->cx0;p->clip[1]=c->cy0;p->clip[2]=c->cx1;p->clip[3]=c->cy1;
}
static uint64_t paint_debug_hash(uint64_t hash,const void *value,size_t bytes) {
    const unsigned char *p=value;while(bytes--)hash=(hash^*p++)*1099511628211ull;return hash;
}
static uint64_t paint_debug_box_hash(uint64_t hash,const box_t *b) {
    int data[]={b->kind,b->node?b->node->tag:0,b->st?b->st->display:0,b->st?b->st->position:0,
        b->st?b->st->overflow:0,b->st?b->st->visibility:0,b->st?b->st->z_index:0,b->st?b->st->z_auto:0};
    float dimensions[]={box_visual_x(b),box_visual_y(b),b->w,b->h,b->p[0],b->p[1],b->p[2],b->p[3],b->st?b->st->opacity:1};
    hash=paint_debug_hash(hash,data,sizeof data);return paint_debug_hash(hash,dimensions,sizeof dimensions);
}
static void paint_debug_finish(web_doc *d) {
    if(!paint_debug_enabled)return;
    uint64_t now=uptime_ms();size_t count=0;
    for(node_t *n=paint_debug_media_next(d,NULL);n;n=paint_debug_media_next(d,n))count++;
    for(node_t *n=paint_debug_media_next(d,NULL);n;n=paint_debug_media_next(d,n)) {
        struct web_paint_diagnostic *p=n->paint_diagnostic;if(!p)continue;
        bool connected=doc_node_root(n,true)==d->root;
        box_t *b=connected?n->box:NULL;
        size_t state[]={count,connected,b!=NULL,p->reached};
        uint64_t hash=paint_debug_hash(1469598103934665603ull,state,sizeof state);
        if(p->reached){hash=paint_debug_hash(hash,p->rect,sizeof p->rect);hash=paint_debug_hash(hash,p->clip,sizeof p->clip);}
        for(box_t *a=b;a;a=a->parent)hash=paint_debug_box_hash(hash,a);
        if(!b && connected)for(node_t *a=n;a;a=doc_flat_parent(a)) {
            unsigned state[]={a->tag,a->style?a->style->display:0,a->style?a->style->visibility:0};
            hash=paint_debug_hash(hash,state,sizeof state);
        }
        if(!hash)hash=1;
        /* Geometry animation cannot flood the native console. Keep only the
           latest changed state, while unchanged media frames produce no log. */
        if(hash==p->signature || (p->signature && now-p->reported_at<1000))continue;
        p->signature=hash;p->reported_at=now;
        char text[384];
        snprintf(text,sizeof text,"Native media paint geometry: id=%llu nodes=%zu tag=%u connected=%u box=%u reached=%u rect=%d,%d,%d,%d clip=%d,%d,%d,%d",
            (unsigned long long)p->id,count,n->tag,connected,b!=NULL,p->reached,
            p->rect[0],p->rect[1],p->rect[2],p->rect[3],p->clip[0],p->clip[1],p->clip[2],p->clip[3]);
        web_js_console(d,0,text);
        size_t depth=0;
        for(box_t *a=b;a;a=a->parent,depth++) {
            style_t *s=a->st;
            snprintf(text,sizeof text,"Native media paint ancestor: id=%llu depth=%zu tag=%u kind=%u xy=%.2f,%.2f wh=%.2f,%.2f padding=%.2f,%.2f,%.2f,%.2f display=%u position=%u overflow=%u visibility=%u opacity=%.3f z=%d/%u",
                (unsigned long long)p->id,depth,a->node?a->node->tag:0,a->kind,box_visual_x(a),box_visual_y(a),a->w,a->h,
                a->p[0],a->p[1],a->p[2],a->p[3],s?s->display:0,s?s->position:0,s?s->overflow:0,s?s->visibility:0,s?s->opacity:1,s?s->z_index:0,s?s->z_auto:0);
            web_js_console(d,0,text);
            if(s){
                snprintf(text,sizeof text,"Native media paint sizing: id=%llu depth=%zu height=%u/%.2f/%.2f min=%u/%.2f/%.2f max=%u/%.2f/%.2f top=%u/%.2f/%.2f bottom=%u/%.2f/%.2f boxsizing=%u",
                    (unsigned long long)p->id,depth,s->height.kind,s->height.px,s->height.pct,
                    s->min_height.kind,s->min_height.px,s->min_height.pct,s->max_height.kind,s->max_height.px,s->max_height.pct,
                    s->inset[0].kind,s->inset[0].px,s->inset[0].pct,s->inset[2].kind,s->inset[2].px,s->inset[2].pct,s->box_sizing);
                web_js_console(d,0,text);
            }
        }
        if(!b && connected) {
            depth=0;
            for(node_t *a=n;a;a=doc_flat_parent(a),depth++) {
                snprintf(text,sizeof text,"Native media paint unboxed ancestor: id=%llu depth=%zu tag=%u style=%u display=%u visibility=%u",
                    (unsigned long long)p->id,depth,a->tag,a->style!=NULL,a->style?a->style->display:0,a->style?a->style->visibility:0);
                web_js_console(d,0,text);
            }
        }
    }
}
