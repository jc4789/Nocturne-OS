/* CSSOM View §11 GeometryUtils (getBoxQuads, convertQuadFromNode, convertRectFromNode,
 * convertPointFromNode), which MouseEvent offsetX/offsetY reuse for the padding edge of
 * their target, and Element.checkVisibility() (§6). Every coordinate comes from the layout
 * the client rectangles read: an element's principal border box or its line fragments
 * (layout_inline_rects), a Text node's line fragments (layout_text_rects), the transform
 * chain (box_transform_chain), the viewport scroll positions and the frame boxes. Nothing
 * here lays out or measures anything of its own. Included by js.c after the client
 * rectangle helpers. https://drafts.csswg.org/cssom-view/#the-geometryutils-interface */

enum geometry_box { GEOMETRY_MARGIN, GEOMETRY_BORDER, GEOMETRY_PADDING, GEOMETRY_CONTENT };
/* Returned to the bindings as a number in place of the coordinates. */
enum geometry_status { GEOMETRY_OK, GEOMETRY_NOT_FOUND, GEOMETRY_SECURITY };

/* m := t after m (affine x'=m[0]x+m[2]y+m[4], y'=m[1]x+m[3]y+m[5]) */
static void geometry_then(double m[6], const double t[6]) {
    double r[6]={t[0]*m[0]+t[2]*m[1],t[1]*m[0]+t[3]*m[1],t[0]*m[2]+t[2]*m[3],t[1]*m[2]+t[3]*m[3],
                 t[0]*m[4]+t[2]*m[5]+t[4],t[1]*m[4]+t[3]*m[5]+t[5]};
    memcpy(m,r,sizeof r);
}
static void geometry_shift(double m[6], double x, double y) { m[4]+=x; m[5]+=y; }
static void geometry_apply(const double m[6], double *x, double *y) {
    double a=*x,b=*y; *x=m[0]*a+m[2]*b+m[4]; *y=m[1]*a+m[3]*b+m[5];
}
/* The inverse map; a non-invertible transform (scale(0)) maps every point to NaN, which the
   specification leaves to the implementation. */
static void geometry_invert(const double m[6], double out[6]) {
    double det=m[0]*m[3]-m[1]*m[2];
    if(!isfinite(det) || det==0){for(int i=0;i<6;i++)out[i]=NAN;return;}
    out[0]=m[3]/det;out[1]=-m[1]/det;out[2]=-m[2]/det;out[3]=m[0]/det;
    out[4]=-(out[0]*m[4]+out[2]*m[5]);out[5]=-(out[1]*m[4]+out[3]*m[5]);
}

/* The viewport scroll position of d, which box_visual_x/y of its viewport-fixed boxes read
   too (as DOM_rect sets it before measuring). */
static void geometry_view(struct web_js_state *owner, web_doc *d, int *sx, int *sy) {
    viewport_scroll_position(owner,sx,sy);
    d->view_x=*sx;d->view_y=*sy;
}
static bool geometry_rendered(const node_t *e) {
    return e && e->type==N_ELEM && e->box && e->box->st && e->box->st->display!=D_NONE;
}
/* The rendered element whose space a Text node (or, for offsetX/offsetY, a box-less target
   such as an SVG graphic) uses: the nearest such ancestor in the flat tree. */
static node_t *geometry_rendered_ancestor(node_t *n) {
    for(node_t *p=n;p;p=doc_flat_parent(p))if(geometry_rendered(p))return p;
    return NULL;
}
struct geometry_fragments {
    double origin[2]; const box_t *space; bool found;
    double *quads; size_t count, cap; bool failed;
};
static bool geometry_first_fragment(void *opaque,const box_t *space,double x,double y,double w,double h) {
    struct geometry_fragments *f=opaque;
    if(!f->found){f->found=true;f->space=space;f->origin[0]=x;f->origin[1]=y;}
    return true;
}
/* An element's local space: the top-left corner of its first border box, in document
   coordinates before transforms, and the box whose transform chain then applies. */
static bool geometry_element_origin(node_t *e, double *x, double *y, const box_t **space) {
    if(!geometry_rendered(e))return false;
    box_t *b=e->box;
    if(b->kind==B_INLINE){
        struct geometry_fragments f={0};
        if(!layout_inline_rects(e,true,geometry_first_fragment,&f) || !f.found)return false;
        *x=f.origin[0];*y=f.origin[1];*space=f.space;return true;
    }
    b=geometry_principal_box(e);
    *x=box_visual_x(b)-b->p[3]-b->b[3];*y=box_visual_y(b)-b->p[0]-b->b[0];*space=b;
    return isfinite(*x) && isfinite(*y);
}
/* "Get the complete transform of a node relative to null": n's local space to the client
   coordinates of the outermost document reached through same-origin frames (each viewport
   sits at its frame element's content edge). Text uses its rendered ancestor's space (the
   specification's next ancestor is its parent element, with no offset); a Document its
   viewport. nearest: an element without a box uses its rendered ancestor's too. */
static int geometry_transform(struct web_js_state *s, node_t *n, bool nearest, double m[6]) {
    m[0]=m[3]=1;m[1]=m[2]=m[4]=m[5]=0;
    node_t *e=NULL;web_doc *d=n->owner;
    if(n->type!=N_DOC){
        e=n->type==N_TEXT || nearest?geometry_rendered_ancestor(n):n;
        if(!geometry_rendered(e))return GEOMETRY_NOT_FOUND;
        d=e->owner;
    }
    for(;;){
        struct web_js_state *owner=geometry_owner(s,d);
        if(!owner || (e && !connected(owner,e)))return GEOMETRY_NOT_FOUND;
        int sx,sy;geometry_view(owner,d,&sx,&sy);
        if(e){
            double x,y;const box_t *space;
            if(!geometry_element_origin(e,&x,&y,&space))return GEOMETRY_NOT_FOUND;
            geometry_shift(m,x,y);
            double t[6];box_transform_chain(space,t);geometry_then(m,t);
        }
        geometry_shift(m,-sx,-sy);
        node_t *frame=d->frame_element;web_doc *parent=d->frame_parent;
        if(!frame || !parent || !geometry_owner(s,parent) || !geometry_rendered(frame))return GEOMETRY_OK;
        box_t *fb=geometry_principal_box(frame);
        geometry_shift(m,fb->b[3]+fb->p[3],fb->b[0]+fb->p[0]+fb->content_dy);
        e=frame;d=parent;
    }
}
/* Where box's top-left corner is in n's border-box space ("adjust a point to/from the border
   box"); Document and Text have only the one box. */
static void geometry_box_offset(node_t *n, int box, bool nearest, double *x, double *y) {
    *x=*y=0;
    if(n->type!=N_ELEM || box==GEOMETRY_BORDER)return;
    if(nearest && !geometry_rendered(n))return; /* the rendered ancestor's border box */
    box_t *outer=n->box,*b=geometry_principal_box(n);
    if(!outer || !b)return;
    if(box==GEOMETRY_MARGIN){*x=-outer->m[3];*y=-outer->m[0];}
    else if(box==GEOMETRY_PADDING){*x=b->b[3];*y=b->b[0];}
    else {*x=b->b[3]+b->p[3];*y=b->b[0]+b->p[0];}
}
/* "Check the documents for coordinate conversion": the documents from each node's up to
   the first one both reach (or only their own when none is shared) must all be same origin
   with node's. */
static bool geometry_documents_allowed(web_doc *a, web_doc *b) {
    web_doc *shared=NULL;
    for(web_doc *x=a;x && !shared;x=x->frame_parent)
        for(web_doc *y=b;y;y=y->frame_parent)if(x==y){shared=x;break;}
    for(web_doc *x=a;x;x=x->frame_parent){
        if(!web_frame_same_origin(a,x))return false;
        if(!shared || x==shared)break;
    }
    for(web_doc *y=b;y;y=y->frame_parent){
        if(!web_frame_same_origin(a,y))return false;
        if(!shared || y==shared)break;
    }
    return true;
}
static web_doc *geometry_document(node_t *n) { return n->owner; }
/* convertPointFromNode() over count points (x,y pairs) given in from's fromBox space,
   returned in to's toBox space. */
static int geometry_convert(struct web_js_state *s, node_t *to, node_t *from, int from_box, int to_box,
                            bool nearest, double *xy, size_t count) {
    if(!geometry_documents_allowed(geometry_document(to),geometry_document(from)))return GEOMETRY_SECURITY;
    double source[6],target[6],inverse[6];
    int status=geometry_transform(s,from,nearest,source);
    if(status==GEOMETRY_OK)status=geometry_transform(s,to,nearest,target);
    if(status!=GEOMETRY_OK)return status;
    geometry_invert(target,inverse);
    double fx,fy,tx,ty;
    geometry_box_offset(from,from_box,nearest,&fx,&fy);
    geometry_box_offset(to,to_box,nearest,&tx,&ty);
    for(size_t i=0;i<count;i++){
        double x=xy[2*i]+fx,y=xy[2*i+1]+fy;
        geometry_apply(source,&x,&y);geometry_apply(inverse,&x,&y);
        xy[2*i]=x-tx;xy[2*i+1]=y-ty;
    }
    return GEOMETRY_OK;
}
static bool geometry_quad_push(struct geometry_fragments *f, double x, double y, double w, double h) {
    if(f->count==f->cap){
        size_t cap=f->cap?f->cap*2:4;
        if(cap>SIZE_MAX/(8*sizeof *f->quads)){f->failed=true;return false;}
        double *grown=realloc(f->quads,cap*8*sizeof *grown);
        if(!grown){f->failed=true;return false;}
        f->quads=grown;f->cap=cap;
    }
    double *q=f->quads+8*f->count++;
    q[0]=x;q[1]=y;q[2]=x+w;q[3]=y;q[4]=x+w;q[5]=y+h;q[6]=x;q[7]=y+h;
    return true;
}
static bool geometry_inline_fragment(void *opaque,const box_t *space,double x,double y,double w,double h) {
    struct geometry_fragments *f=opaque;
    if(!f->found){f->found=true;f->space=space;f->origin[0]=x;f->origin[1]=y;}
    return geometry_quad_push(f,x-f->origin[0],y-f->origin[1],w,h);
}
/* A Text node's quads are its client rectangles (a Range selecting it), in its document's
   viewport space. */
struct geometry_text { struct geometry_fragments *f; int sx, sy; };
static bool geometry_text_fragment(void *opaque,const box_t *space,double x,double y,double w,double h) {
    struct geometry_text *t=opaque;
    double r[4]={x,y,w,h};box_transform_rect(space,r);
    if(!isfinite(r[0]) || !isfinite(r[1]) || !isfinite(r[2]) || !isfinite(r[3]))return true;
    return geometry_quad_push(t->f,r[0]-t->sx,r[1]-t->sy,r[2],r[3]);
}
/* getBoxQuads(): node's box fragments in its own space (Element), its client rectangles
   (Text) or the layout viewport (Document), each converted into relative_to's border box.
   The flat x,y list of four points per quad is returned through *out. */
static int geometry_box_quads(struct web_js_state *s, node_t *n, int box, node_t *relative_to,
                              double **out, size_t *quads) {
    *out=NULL;*quads=0;
    struct geometry_fragments f={0};
    node_t *from=n;
    web_doc *d=n->owner;
    struct web_js_state *owner=geometry_owner(s,d);
    if(!owner)return GEOMETRY_OK;
    int sx,sy;geometry_view(owner,d,&sx,&sy);
    if(n->type==N_DOC){
        geometry_quad_push(&f,0,0,d->width,d->height);
    }else if(n->type==N_TEXT){
        if(connected(owner,n)){
            struct geometry_text t={&f,sx,sy};
            if(!layout_text_rects(n,0,UINT32_MAX,true,geometry_text_fragment,&t) && !f.failed)f.failed=true;
        }
        from=d->root; /* the rectangles are in the document's space */
    }else if(geometry_rendered(n) && connected(owner,n)){
        box_t *b=n->box;
        if(b->kind==B_INLINE){
            if(!layout_inline_rects(n,true,geometry_inline_fragment,&f) && !f.failed)f.failed=true;
            /* The inline's start and end edges belong to its first and last fragments only. */
            for(size_t i=0;i<f.count && box!=GEOMETRY_BORDER;i++){
                double *q=f.quads+8*i,l=0,t=0,r=0,bt=0;
                if(box==GEOMETRY_MARGIN){
                    t=-b->m[0];bt=b->m[2];
                    if(!i)l=-b->m[3];
                    if(i+1==f.count)r=b->m[1];
                }else{
                    t=b->b[0];bt=-b->b[2];
                    if(!i)l=b->b[3];
                    if(i+1==f.count)r=-b->b[1];
                    if(box==GEOMETRY_CONTENT){
                        t+=b->p[0];bt-=b->p[2];
                        if(!i)l+=b->p[3];
                        if(i+1==f.count)r-=b->p[1];
                    }
                }
                q[0]+=l;q[6]+=l;q[2]+=r;q[4]+=r;q[1]+=t;q[3]+=t;q[5]+=bt;q[7]+=bt;
            }
            box=GEOMETRY_BORDER; /* the edges are applied; the space is the first border box's */
        }else{
            box_t *p=geometry_principal_box(n);
            double w=p->w+p->p[1]+p->p[3]+p->b[1]+p->b[3],h=p->h+p->p[0]+p->p[2]+p->b[0]+p->b[2];
            double x=0,y=0;
            if(box==GEOMETRY_MARGIN){x=-b->m[3];y=-b->m[0];w+=b->m[1]+b->m[3];h+=b->m[0]+b->m[2];}
            else if(box==GEOMETRY_PADDING){x=p->b[3];y=p->b[0];w-=p->b[1]+p->b[3];h-=p->b[0]+p->b[2];}
            else if(box==GEOMETRY_CONTENT){x=p->b[3]+p->p[3];y=p->b[0]+p->p[0];w=p->w;h=p->h;}
            geometry_quad_push(&f,x,y,w,h);
            box=GEOMETRY_BORDER;
        }
    }
    if(f.failed){free(f.quads);return -1;}
    if(!f.count){free(f.quads);return GEOMETRY_OK;}
    /* relativeTo.convertQuadFromNode(quad, node): into relativeTo's border box. */
    int status=geometry_convert(s,relative_to,from,GEOMETRY_BORDER,GEOMETRY_BORDER,false,f.quads,f.count*4);
    if(status!=GEOMETRY_OK){free(f.quads);return status;}
    *out=f.quads;*quads=f.count;
    return GEOMETRY_OK;
}
static JSValue geometry_numbers(JSContext *ctx, const double *values, size_t count) {
    JSValue array=JS_NewArray(ctx);
    if(JS_IsException(array))return array;
    if(count>UINT32_MAX){JS_FreeValue(ctx,array);return JS_ThrowRangeError(ctx,"Too many geometry points");}
    for(size_t i=0;i<count;i++)if(JS_SetPropertyUint32(ctx,array,(uint32_t)i,JS_NewFloat64(ctx,values[i]))<0){
        JS_FreeValue(ctx,array);return JS_EXCEPTION;
    }
    return array;
}
static bool geometry_node(const node_t *n) {
    return n && (n->type==N_ELEM || n->type==N_TEXT || n->type==N_DOC);
}
static void geometry_flush(struct web_js_state *s, node_t *n) {
    struct web_js_state *owner=n?geometry_owner(s,n->owner):NULL;
    if(owner)flush_layout(owner);
}
/* rawDom.boxQuads(node, box, relativeTo): a flat list of x,y numbers, eight per quad, or a
   geometry_status number. */
static JSValue geometry_box_quads_value(struct web_js_state *s, node_t *n, int argc, JSValueConst *argv) {
    JSContext *ctx=s->ctx;int32_t box=GEOMETRY_BORDER;
    node_t *relative=argc>3?unwrap(ctx,argv[3]):NULL;
    if(!geometry_node(n) || !geometry_node(relative))return JS_ThrowTypeError(ctx,"GeometryNode required");
    if(argc>2 && JS_ToInt32(ctx,&box,argv[2])<0)return JS_EXCEPTION;
    if(box<GEOMETRY_MARGIN || box>GEOMETRY_CONTENT)return JS_ThrowTypeError(ctx,"Invalid CSSBoxType");
    geometry_flush(s,n);geometry_flush(s,relative);
    double *quads=NULL;size_t count=0;
    int status=geometry_box_quads(s,n,box,relative,&quads,&count);
    if(status<0)return JS_ThrowOutOfMemory(ctx);
    if(status!=GEOMETRY_OK)return JS_NewInt32(ctx,status);
    JSValue result=geometry_numbers(ctx,quads,count*8);
    free(quads);return result;
}
/* rawDom.convertPoints(node, from, fromBox, toBox, [x, y, ...], nearest): the points in
   node's space, or a geometry_status number. */
static JSValue geometry_convert_value(struct web_js_state *s, node_t *n, int argc, JSValueConst *argv) {
    JSContext *ctx=s->ctx;int32_t from_box=GEOMETRY_BORDER,to_box=GEOMETRY_BORDER;
    node_t *from=argc>2?unwrap(ctx,argv[2]):NULL;
    if(!geometry_node(n) || !geometry_node(from))return JS_ThrowTypeError(ctx,"GeometryNode required");
    if(argc<6)return JS_ThrowTypeError(ctx,"Coordinate conversion requires its points");
    if(JS_ToInt32(ctx,&from_box,argv[3])<0 || JS_ToInt32(ctx,&to_box,argv[4])<0)return JS_EXCEPTION;
    if(from_box<GEOMETRY_MARGIN || from_box>GEOMETRY_CONTENT || to_box<GEOMETRY_MARGIN || to_box>GEOMETRY_CONTENT)
        return JS_ThrowTypeError(ctx,"Invalid CSSBoxType");
    uint32_t length=0;
    JSValue size=JS_GetPropertyStr(ctx,argv[5],"length");
    int failed=JS_IsException(size) || JS_ToUint32(ctx,&length,size)<0;
    JS_FreeValue(ctx,size);
    if(failed)return JS_EXCEPTION;
    if(length%2)return JS_ThrowRangeError(ctx,"Invalid point list");
    int nearest=argc>6?JS_ToBool(ctx,argv[6]):0;
    if(nearest<0)return JS_EXCEPTION;
    double local[8],*xy=length<=8?local:malloc((size_t)length*sizeof *xy);
    if(!xy)return JS_ThrowOutOfMemory(ctx);
    for(uint32_t i=0;i<length;i++){
        JSValue v=JS_GetPropertyUint32(ctx,argv[5],i);
        failed=JS_IsException(v) || JS_ToFloat64(ctx,&xy[i],v)<0;
        JS_FreeValue(ctx,v);
        if(failed){if(xy!=local)free(xy);return JS_EXCEPTION;}
    }
    /* Conversion runs author getters above; read the layout only now. */
    geometry_flush(s,n);geometry_flush(s,from);
    int status=geometry_convert(s,n,from,from_box,to_box,nearest,xy,(size_t)length/2);
    JSValue result=status==GEOMETRY_OK?geometry_numbers(ctx,xy,(size_t)length):JS_NewInt32(ctx,status);
    if(xy!=local)free(xy);
    return result;
}
/* Element.checkVisibility(options): flags 1 checkOpacity/opacityProperty, 2
   checkVisibilityCSS/visibilityProperty, 4 contentVisibilityAuto. */
static bool geometry_check_visibility(struct web_js_state *s, node_t *n, int flags) {
    struct web_js_state *owner=geometry_owner(s,n->owner);
    if(!owner || !connected(owner,n))return false;
    flush_layout(owner);
    if(!connected(owner,n) || !geometry_rendered(n))return false;
    for(node_t *a=doc_flat_parent(n);a;a=doc_flat_parent(a))
        if(a->type==N_ELEM && a->style && a->style->content_visibility==CV_HIDDEN)return false;
    if(flags&1)for(node_t *a=n;a;a=doc_flat_parent(a))
        if(a->type==N_ELEM && a->style && a->style->opacity==0)return false;
    if((flags&2) && n->style && n->style->visibility)return false;
    /* flags&4: the layout lays out and paints every content-visibility:auto subtree (it never
       skips one for being off screen), so no ancestor skips its contents. */
    return true;
}
