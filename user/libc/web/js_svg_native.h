/* Geometry is read from native nodes and the same path parser used by paint. */
#include "svg_geometry.h"
#include "../../../third_party/img/nanosvg.h"

static JSValue svg_numbers(JSContext *ctx,const float *values,size_t count){
    JSValue array=JS_NewArray(ctx);if(JS_IsException(array))return array;
    for(size_t i=0;i<count;i++)if(JS_SetPropertyUint32(ctx,array,(uint32_t)i,JS_NewFloat64(ctx,values[i]))<0){JS_FreeValue(ctx,array);return JS_EXCEPTION;}
    return array;
}
static JSValue native_svg_geometry(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv){
    struct web_js_state *s=state(ctx);node_t *n=argc?unwrap(ctx,argv[0]):NULL;int32_t mode=0;
    if(!n)return JS_ThrowTypeError(ctx,"SVGElement receiver required");
    if(n->type!=N_ELEM||n->namespace_id!=NS_SVG)return JS_ThrowTypeError(ctx,"SVGElement receiver required");
    if(argc<2)return JS_ThrowTypeError(ctx,"SVG geometry operation required");
    if(JS_ToInt32(ctx,&mode,argv[1]))return JS_EXCEPTION;
    if(mode==3){
        if(!strcmp(n->raw_name,"svg")||!strcmp(n->raw_name,"path")||!strcmp(n->raw_name,"g")||
           !strcmp(n->raw_name,"rect")||!strcmp(n->raw_name,"circle")||!strcmp(n->raw_name,"ellipse")||
           !strcmp(n->raw_name,"line")||!strcmp(n->raw_name,"polyline")||!strcmp(n->raw_name,"polygon")||
           !strcmp(n->raw_name,"text")||!strcmp(n->raw_name,"image")||!strcmp(n->raw_name,"use")||
           !strcmp(n->raw_name,"foreignObject"))return JS_TRUE;
        return JS_ThrowTypeError(ctx,"SVGGraphicsElement receiver required");
    }
    if(mode==4){if(!strcmp(n->raw_name,"path"))return JS_TRUE;
        return JS_ThrowTypeError(ctx,"SVGPathElement receiver required");}
    if(mode==0){
        if(strcmp(n->raw_name,"svg"))return JS_ThrowTypeError(ctx,"SVGSVGElement receiver required");
        float v[4]={0};if(!svg_viewbox(node_attr(n,"viewBox"),v))memset(v,0,sizeof v);
        return svg_numbers(ctx,v,4);
    }
    if(mode==2){
        if(strcmp(n->raw_name,"svg"))return JS_ThrowTypeError(ctx,"SVGSVGElement receiver required");
        struct web_js_state *owner=n->owner?n->owner->js:NULL;
        if(!owner||owner->disabled||!connected(owner,n))return JS_NULL;
        flush_layout(owner);box_t *b=n->box;if(!b)return JS_NULL;
        int sx=0,sy=0;viewport_scroll_position(owner,&sx,&sy);
        float v[]={box_visual_x(b)-sx,box_visual_y(b)+b->content_dy-sy,b->w,b->h};
        return svg_numbers(ctx,v,4);
    }
    if(mode!=1||strcmp(n->raw_name,"path"))return JS_ThrowTypeError(ctx,"SVGPathElement receiver required");
    const char *data=node_attr(n,"d");if(!data)data="";
    if(s->host.debug_js)log_text(s,0,"SVG geometry path parsed");
    sbuf xml={0};sb_puts(&xml,"<svg width='1' height='1' viewBox='0 0 1 1'><path d=\"");
    for(const char *p=data;*p;p++){
        if(*p=='"')sb_puts(&xml,"&quot;");else if(*p=='&')sb_puts(&xml,"&amp;");
        else if(*p=='<')sb_puts(&xml,"&lt;");else sb_putc(&xml,*p);
    }
    sb_puts(&xml,"\"/></svg>");NSVGimage *image=nsvgParse(sb_cstr(&xml),"px",96);sb_free(&xml);
    if(!image)return oom(ctx);
    JSValue paths=JS_NewArray(ctx);size_t count=0;
    for(NSVGshape *shape=image->shapes;shape;shape=shape->next)for(NSVGpath *p=shape->paths;p;p=p->next)count++;
    if(count>UINT32_MAX){nsvgDelete(image);JS_FreeValue(ctx,paths);return JS_ThrowRangeError(ctx,"SVG subpath count overflow");}
    /* NanoSVG prepends subpaths. Reverse only that order, never the point data. */
    for(NSVGshape *shape=image->shapes;shape&&!JS_IsException(paths);shape=shape->next)for(NSVGpath *p=shape->paths;p;p=p->next){
        if(!web_native_checkpoint(s->doc)){JS_FreeValue(ctx,paths);paths=JS_ThrowInternalError(ctx,"SVG geometry cancelled");break;}
        if(p->npts<0||(size_t)p->npts>UINT32_MAX/2){JS_FreeValue(ctx,paths);paths=JS_ThrowRangeError(ctx,"SVG path point count overflow");break;}
        JSValue points=svg_numbers(ctx,p->pts,(size_t)p->npts*2);
        if(JS_IsException(points)||JS_SetPropertyUint32(ctx,paths,(uint32_t)--count,points)<0){JS_FreeValue(ctx,paths);paths=JS_EXCEPTION;break;}
    }
    if(!image->shapes&&!JS_IsException(paths)){
        /* A move-only path has no painted curve, but has a well-defined point. */
        const char *p=data;while(svg_space(*p))p++;
        if(*p=='M'||*p=='m'){p++;while(svg_space(*p))p++;float v[2];
            if(svg_number(&p,v)){while(svg_space(*p))p++;if(*p==',')p++;while(svg_space(*p))p++;
                if(svg_number(&p,v+1))JS_SetPropertyUint32(ctx,paths,0,svg_numbers(ctx,v,2));}}
    }
    nsvgDelete(image);return paths;
}
