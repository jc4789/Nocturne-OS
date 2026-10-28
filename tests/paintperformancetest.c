/* Real native paint/hit paths. Local regression evidence, not site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "webi.h"
#include "frame.h"

static int checks,failed;
static uint32_t pixels[320*200],reference[320*200];
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("FAIL paintperformance %s\n",name);}}
static bool checkpoint(void *p){(void)p;return true;}
static node_t *by_id(node_t *n,const char *id){
    if(!n)return NULL;const char *value=node_attr(n,"id");if(value&&!strcmp(value,id))return n;
    for(node_t *child=n->first;child;child=child->next){node_t *found=by_id(child,id);if(found)return found;}return NULL;
}
static void compare_hits(web_doc *d){
    bool targets=true,actions=true;
    for(int y=-3;y<230;y+=11)for(int x=-3;x<350;x+=13){
        node_t *fast=web_node_at(d,x,y);
        struct web_hit a={0},b={0};bool aa=web_hit_test(d,x,y,&a);
        char href[256];strlcpy(href,a.href?a.href:"",sizeof href);
        bool valid=d->layout_valid;d->layout_valid=false;
        node_t *slow=web_node_at(d,x,y);bool bb=web_hit_test(d,x,y,&b);d->layout_valid=valid;
        targets&=fast==slow;
        actions&=aa==bb&&a.kind==b.kind&&a.node==b.node&&!strcmp(href,b.href?b.href:"");
    }
    check(targets,"BVH target equals exact stacking/clip walk");
    check(actions,"BVH action and resolved link equal exact walk");
}
/* Separate acceptance boundaries: --boundaries never replays the original
   fourteen checks above. Only the host callback asks for cancellation; real
   native preparation/sort/checkpoint/publication paths handle it. */
struct boundary_state {web_doc *doc;unsigned layers;bool armed,sort_cancelled;};
static bool boundary_checkpoint(void *opaque){
    struct boundary_state *state=opaque;
    if(state->armed&&state->doc&&state->doc->root_box&&
       !state->doc->root_box->paint_layers_valid&&
       state->doc->root_box->paint_layers.n==(int)state->layers){
        state->sort_cancelled=true;return false;
    }
    return true;
}
static void boundary_hit(web_doc *d,int x,int y,const char *target_name,const char *action_name){
    node_t *fast=web_node_at(d,x,y);
    struct web_hit fast_action={0},slow_action={0};bool fast_ok=web_hit_test(d,x,y,&fast_action);
    char href[256];strlcpy(href,fast_action.href?fast_action.href:"",sizeof href);
    bool valid=d->layout_valid;d->layout_valid=false;
    node_t *slow=web_node_at(d,x,y);bool slow_ok=web_hit_test(d,x,y,&slow_action);d->layout_valid=valid;
    check(fast==slow,target_name);
    check(fast_ok==slow_ok&&fast_action.kind==slow_action.kind&&fast_action.node==slow_action.node&&
        !strcmp(href,slow_action.href?slow_action.href:""),action_name);
}
static int boundaries(void){
    const char *html="<!doctype html><style>html,body{margin:0}"
        "ul{margin:0;padding-left:60px}li{list-style-type:disc;list-style-position:outside;height:28px}"
        "details{margin-left:60px}summary{list-style-position:outside}#generated::before{content:'generated '}"
        "</style><ul><li id=ordinary>plain item</li></ul>"
        "<details open><summary id=legend>legend</summary>body</details>"
        "<a id=generated href=/generated>real text</a>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://paint.test/boundaries","utf-8",&host);
    check(d!=NULL,"marker boundary document");if(!d)return 1;
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());web_layout(d,320,200);
    node_t *li=by_id(d->root,"ordinary"),*summary=by_id(d->root,"legend"),*generated=by_id(d->root,"generated");
    check(li&&li->box&&(li->box->marker||li->box->marker_shape),"ordinary outside marker exists");
    if(li&&li->box){box_t *b=li->box;
        int x=(int)(box_visual_x(b)-b->p[3]-b->b[3]-b->st->font_size);
        int y=(int)(box_visual_y(b)-b->p[0]-b->b[0]+b->h*.5f);
        boundary_hit(d,x,y,"ordinary outside marker BVH target equals walk",
            "ordinary outside marker BVH action equals walk");
    }
    check(summary&&summary->box&&(summary->box->marker||summary->box->marker_shape),"summary outside marker exists");
    if(summary&&summary->box){box_t *b=summary->box;
        int x=(int)(box_visual_x(b)-b->p[3]-b->b[3]-b->st->font_size);
        int y=(int)(box_visual_y(b)-b->p[0]-b->b[0]+b->h*.5f);
        boundary_hit(d,x,y,"summary outside marker BVH target equals walk",
            "summary outside marker BVH action equals walk");
    }
    check(generated&&generated->anchor_block,"generated inline text has a native formatting root");
    bool generated_checked=false;
    if(generated&&generated->anchor_block){box_t *b=generated->anchor_block;
        for(int i=0;i<b->nruns;i++)if(b->runs[i].node==generated){struct run *r=&b->runs[i];
            boundary_hit(d,(int)(box_visual_x(b)+r->x+1),(int)(box_visual_y(b)+b->content_dy+r->y-2),
                "generated run BVH target equals walk","generated run BVH link action equals walk");generated_checked=true;break;
        }
    }
    check(generated_checked,"generated pseudo run was actually tested, not skipped");
    web_free(d);

    struct boundary_state state={.layers=4096};
    const char *large="<!doctype html><style>html,body{margin:0}.layer{position:absolute;left:0;top:0;"
        "width:1px;height:1px;z-index:0;background:red}</style><div id=layers></div>";
    struct web_host cancellable={.script_checkpoint=boundary_checkpoint,.opaque=&state};
    d=web_live(large,strlen(large),"https://paint.test/cancel","utf-8",&cancellable);state.doc=d;
    check(d!=NULL,"sorting cancellation document");if(!d)goto done;
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());
    node_t *parent=by_id(d->root,"layers");check(parent!=NULL,"sorting layer parent");
    if(!parent){web_free(d);goto done;}
    for(unsigned i=0;i<state.layers;i++){
        node_t *layer=doc_node_create(d,N_ELEM,"div",NULL,0);
        if(!layer){check(false,"sorting layer allocation");web_free(d);goto done;}
        doc_node_attr(d,layer,"class","layer");doc_node_move(d,parent,layer,NULL);
    }
    web_layout(d,320,200);
    check(d->layout_valid&&!d->native_cancelled,"large layer tree has a committed layout");
    /* Set the pixels to the previous coherent window, not a white canvas. */
    for(size_t i=0;i<sizeof pixels/sizeof *pixels;i++)pixels[i]=0xff3c5a7bu;
    memcpy(reference,pixels,sizeof pixels);
    canvas_t canvas;gfx_init(&canvas,pixels,320,200,320);
    state.armed=true;web_paint(d,&canvas,0,0,320,200,0,0);state.armed=false;
    check(state.sort_cancelled&&d->native_cancelled,"host cancellation reaches layer sorting after collection");
    check(!d->paint_order_valid&&d->root_box&&!d->root_box->paint_layers_valid,
        "cancelled layer order is not published as valid");
    check(!memcmp(pixels,reference,sizeof pixels),"sorting cancellation leaves previous committed window pixels intact");
    web_free(d);
done:
    printf("paintperformance boundaries: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
/* New scroll/extent conditions only; neither old fixture branch is replayed. */
struct scroll_input_state {web_doc *doc;node_t *inner;unsigned events,bad_events,frame_events,shadow_events;
    bool inject,active_native,consumed;};
static void scroll_input_console(void *opaque,int level,const char *message){
    (void)level;struct scroll_input_state *s=opaque;
    if(!strcmp(message,"scroll-input-frame-event"))s->frame_events++;
    if(!strcmp(message,"scroll-input-shadow-event"))s->shadow_events++;
    if(!strcmp(message,"scroll-input-bad-event"))s->bad_events++;
    if(strcmp(message,"scroll-input-event"))return;
    s->events++;
    if(s->inject){s->inject=false;int x=0,y=7;
        s->active_native=web_script_running(s->doc);
        s->consumed=web_scroll_input(s->doc,s->inner,&x,&y,0)&&!x&&!y;
    }
}
static web_doc *scroll_input_document(const char *html,const struct web_host *host){
    web_doc *d=web_live(html,strlen(html),"https://scroll.test/","utf-8",host);
    if(d){for(int i=0;i<32;i++)web_tick(d,uptime_ms());web_layout(d,320,200);}
    return d;
}
static double scroll_input_limit(node_t *n,int axis){
    box_t *b=n->box;return axis?b->scroll_h-b->h-b->p[0]-b->p[2]:
        b->scroll_w-b->w-b->p[1]-b->p[3];
}
static int scroll_inputs(void){
    struct scroll_input_state state={0};
    struct web_host host={.script_checkpoint=checkpoint,.console=scroll_input_console,.opaque=&state};
    const char *html="<!doctype html><style>html,body{margin:0}"
        "#outer{width:120px;height:60px;overflow:auto}"
        "#inner{width:80px;height:30px;overflow:auto}#leaf{width:160px;height:90px}"
        "#hidden{width:90px;height:30px;overflow:hidden}#hiddenleaf{height:80px}"
        "</style><div id=outer><div id=inner><div id=leaf></div></div>"
        "<div style='height:100px'></div></div><div id=hidden><div id=hiddenleaf></div></div>";
    web_doc *d=scroll_input_document(html,&host);state.doc=d;
    check(d&&d->layout_valid,"scroll input committed document");if(!d)goto done;
    node_t *inner=by_id(d->root,"inner"),*outer=by_id(d->root,"outer"),*leaf=by_id(d->root,"leaf"),
        *hidden=by_id(d->root,"hidden"),*hiddenleaf=by_id(d->root,"hiddenleaf");state.inner=inner;
    bool boxes=inner&&outer&&leaf&&hidden&&hiddenleaf&&inner->box&&outer->box&&leaf->box&&hidden->box;
    check(boxes,"scroll input uses real principal boxes");if(!boxes){web_free(d);goto done;}
    double ix=scroll_input_limit(inner,0),iy=scroll_input_limit(inner,1),oy=scroll_input_limit(outer,1);
    check(ix==80&&iy==60&&oy==70,"nested real CSS scroll extents");
    uint64_t revision=d->layout_revision,visual=d->visual_revision;
    int x=95,y=100;
    check(web_scroll_input(d,leaf,&x,&y,0)&&x==15&&!y&&inner->scroll_x==80&&
        inner->scroll_y==60&&outer->scroll_y==40,"delta consumes inner then outer and preserves root residual");
    check(d->layout_revision==revision&&d->visual_revision==visual+2&&d->paint_dirty,
        "native input commits presentation without layout");
    int damage_x,damage_y,damage_w,damage_h;
    d->paint_damage_valid=true;d->paint_damage_full=false;
    d->paint_damage[0]=d->paint_damage[1]=0;d->paint_damage[2]=d->paint_damage[3]=2;
    x=0;y=200;check(web_scroll_input(d,leaf,&x,&y,0)&&y==170&&outer->scroll_y==oy,
        "bounded ancestors forward excess downward delta");
    check(!web_take_paint_damage(d,&damage_x,&damage_y,&damage_w,&damage_h),
        "native scrolling cannot reuse an older small video damage rectangle");
    y=-300;check(web_scroll_input(d,leaf,&x,&y,0)&&y==-170&&!inner->scroll_y&&!outer->scroll_y,
        "upward delta follows the same bounded chain");
    y=0;check(web_scroll_input(d,leaf,&x,&y,1)&&!y&&inner->scroll_y==iy&&!outer->scroll_y,
        "End consumes nearest vertical scrollport, not root or outer");
    check(web_scroll_input(d,leaf,&x,&y,1)&&!y&&!outer->scroll_y,"End at inner edge remains consumed");
    check(web_scroll_input(d,leaf,&x,&y,-1)&&!inner->scroll_y,"Home returns inner to top");
    check(doc_element_scroll(d,hidden,0,20),"hidden CSSOM scrolling remains available");
    d->paint_damage_valid=true;d->paint_damage_full=false;
    doc_element_scroll(d,hidden,0,21);
    check(!web_take_paint_damage(d,&damage_x,&damage_y,&damage_w,&damage_h),
        "CSSOM scrolling also promotes pending damage to full geometry repaint");
    doc_element_scroll(d,hidden,0,20);
    y=11;check(!web_scroll_input(d,hiddenleaf,&x,&y,0)&&y==11&&hidden->scroll_y==20,
        "hidden overflow excludes native input without disabling CSSOM");
    doc_element_scroll(d,inner,0,iy-.5);y=2;
    check(web_scroll_input(d,leaf,&x,&y,0)&&!y&&inner->scroll_y==iy&&outer->scroll_y==1,
        "fractional inner edge forwards bounded integer residual");
    doc_element_scroll(d,inner,0,0);doc_element_scroll(d,outer,0,0);
    x=3;y=9;d->need_style=true;
    check(!web_scroll_input(d,leaf,&x,&y,0)&&x==3&&y==9&&d->layout_revision==revision,
        "dirty style input neither flushes layout nor consumes delta");d->need_style=false;
    node_t *detached=doc_node_create(d,N_ELEM,"div",NULL,0);
    check(!web_scroll_input(d,detached,&x,&y,0)&&x==3&&y==9,"detached target cannot use stale native boxes");
    web_doc *other=scroll_input_document("<!doctype html><div id=other></div>",&host);
    check(other&&!web_scroll_input(d,by_id(other->root,"other"),&x,&y,0)&&x==3&&y==9,
        "unrelated document cannot borrow the input root");web_free(other);
    /* Drain only this new condition's earlier input notifications before
       registering listeners. No runtime source or old fixture is bypassed. */
    for(int i=0;i<32;i++)web_tick(d,uptime_ms());
    const char *listen="document.getElementById('inner').addEventListener('scroll',e=>{"
        "if(!e.isTrusted||e.bubbles)console.log('scroll-input-bad-event');console.log('scroll-input-event');});";
    check(web_console_eval(d,listen,strlen(listen)),"real event listener registration");
    state.events=state.bad_events=0;state.inject=true;x=0;y=4;
    web_scroll_input(d,leaf,&x,&y,0);y=5;web_scroll_input(d,leaf,&x,&y,0);
    check(!state.events&&d->scroll_input_first==inner&&d->scroll_input_last==inner&&
        inner->scroll_input_owner==d,"native input notification is asynchronous and intrusive-deduplicated");
    check(web_deadline(d)>=0,"pending native notification wakes an idle document");
    for(int i=0;i<48&&!state.events;i++)web_tick(d,uptime_ms());
    check(state.events==1&&state.active_native&&state.consumed&&inner->scroll_y==16,
        "active scroll callback can commit new native input without recursive author delivery");
    check(inner->scroll_input_owner==d,"callback-time input remains queued for a later tick");
    for(int i=0;i<48;i++)web_tick(d,uptime_ms());
    check(state.events==2&&!state.bad_events&&!d->scroll_input_first,
        "later tick dispatches trusted nonbubbling event and drains ownership");
    const char *shadow="const inputHost=document.createElement('div');document.body.appendChild(inputHost);"
        "const inputShadow=inputHost.attachShadow({mode:'open'});"
        "inputShadow.innerHTML='<style>#slotport{overflow:auto;height:20px;width:80px}</style>"
        "<div id=slotport><slot></slot></div>';"
        "const inputAssigned=document.createElement('div');inputAssigned.id='assignedInput';"
        "inputAssigned.style.height='75px';inputHost.appendChild(inputAssigned);"
        "inputShadow.getElementById('slotport').addEventListener('scroll',()=>console.log('scroll-input-shadow-event'));";
    check(web_console_eval(d,shadow,strlen(shadow)),"composed input fixture uses actual Shadow DOM and slot assignment");
    web_layout(d,320,200);
    node_t *assigned=by_id(d->root,"assignedInput"),*slotport=assigned&&assigned->parent&&assigned->parent->shadow_root?
        by_id(assigned->parent->shadow_root,"slotport"):NULL;
    check(assigned&&assigned->assigned_slot&&slotport&&slotport->box,
        "composed input has committed slot and native scrollport ancestry");
    if(assigned&&assigned->assigned_slot&&slotport&&slotport->box){
        x=0;y=25;check(web_scroll_input(d,assigned,&x,&y,0)&&!y&&slotport->scroll_y==25,
            "assigned light child consumes composed shadow scrollport input");
        doc_node_remove(d,assigned->parent);
        for(int i=0;i<32;i++)web_tick(d,uptime_ms());
        check(!slotport->scroll_input_owner&&!state.shadow_events,
            "detached pending shadow scrollport releases borrower without stale event");
    }
    web_layout(d,320,200);x=0;y=1;web_scroll_input(d,leaf,&x,&y,0);
    const char *inert_html="<!doctype html><div id=destination></div>";
    web_doc *inert=doc_inert(d,inert_html,strlen(inert_html),"about:blank");
    check(inert&&doc_node_move(inert,by_id(inert->root,"destination"),inner,NULL),
        "pending native scrollport adopts through existing family-owned DOM path");
    if(inert){
        for(int i=0;i<32;i++)web_tick(d,uptime_ms());
        check(inner->owner==inert&&!inner->scroll_input_owner&&state.events==2,
            "adopted pending node neither dispatches in old realm nor retains old queue owner");
    }
    web_free(d);

    const char *clipped="<!doctype html><style>html,body{margin:0}#clip{position:relative;"
        "overflow:auto;height:40px}#abs{position:absolute;top:300px;height:50px;width:10px}</style>"
        "<div id=clip><div id=abs></div></div>";
    /* Root auto height is the 40px flow, not the 200px viewport. The isolated
       --scroll-extent guest control records root=40 and private overflow=350. */
    d=scroll_input_document(clipped,&host);check(d&&d->doc_h==40,
        "private absolute overflow does not inflate root document extent");
    if(d){node_t *clip=by_id(d->root,"clip");check(clip&&clip->box&&clip->box->scroll_h>=350,
        "private absolute overflow remains available to its scrollport");web_free(d);}
    const char *visible="<!doctype html><style>html,body{margin:0}#visible{position:relative;"
        "height:40px;border-bottom:3px solid red;margin-bottom:7px}#abs{position:absolute;top:300px;"
        "height:50px;width:10px;margin-bottom:9px;border-bottom:2px solid red}</style>"
        "<div id=visible><div id=abs></div></div>";
    d=scroll_input_document(visible,&host);check(d&&d->doc_h>=361,
        "unclipped absolute border and margin still extend root overflow");web_free(d);
    const char *escape="<!doctype html><style>html,body{margin:0}#cb{position:relative;height:40px}"
        "#clip{overflow:hidden;height:20px}#abs{position:absolute;top:300px;height:50px;width:10px}"
        "</style><div id=cb><div id=clip><div id=abs></div></div></div>";
    d=scroll_input_document(escape,&host);check(d&&d->doc_h>=350,
        "absolute containing block outside clip preserves escaping root overflow");web_free(d);

    const char *framed="<!doctype html><style>html,body{margin:0}#frameouter{height:60px;"
        "width:160px;overflow:auto}iframe{display:block;width:120px;height:40px;border:0}</style>"
        "<div id=frameouter><iframe id=frame srcdoc=\"<!doctype html><style>html,body{margin:0}"
        "#fc{height:25px;width:60px;overflow:auto}#fl{height:65px;width:100px}</style>"
        "<div id=fc><div id=fl></div></div><div style='height:150px'></div>"
        "<script>document.addEventListener('scroll',()=>console.log('scroll-input-frame-event'));</script>\">"
        "</iframe><div style='height:100px'></div></div>";
    d=scroll_input_document(framed,&host);
    check(d!=NULL,"native child input document");
    if(d){
        /* Use the real frame viewport preparation, not fabricated dimensions
           or a standalone child without its browsing-context ownership. */
        web_frames_prepare_paint(d,NULL);web_frames_finish_paint(d);
        node_t *frame=by_id(d->root,"frame"),*frameouter=by_id(d->root,"frameouter");
        struct web_frame *f=web_frame_find(d,frame);web_doc *child=f?f->document:NULL;
        node_t *fc=child?by_id(child->root,"fc"):NULL,*fl=child?by_id(child->root,"fl"):NULL;
        bool ready=child&&child->layout_valid&&fc&&fl&&fc->box&&frameouter&&frameouter->box;
        check(ready,"attached frame has real committed viewport and scrollport");
        if(ready){
            int my=MAX(0,child->doc_h-f->viewport_h),outer_my=(int)scroll_input_limit(frameouter,1);
            x=45;y=60;revision=child->layout_revision;visual=child->visual_revision;
            d->paint_damage_valid=child->paint_damage_valid=true;
            d->paint_damage_full=child->paint_damage_full=false;
            state.frame_events=0;
            check(web_scroll_input(d,fl,&x,&y,0)&&x==5&&!y&&fc->scroll_y==40&&
                f->scroll_y==20&&!frameouter->scroll_y,
                "input crosses child element then its real viewport, not parent root");
            check(child->layout_revision==revision&&child->visual_revision>visual&&child->view_y==20&&
                child->scroll_viewport_pending&&!state.frame_events&&d->paint_dirty,
                "child viewport update queues event and composite paint without author reentry");
            check(!web_take_paint_damage(child,&damage_x,&damage_y,&damage_w,&damage_h)&&
                !web_take_paint_damage(d,&damage_x,&damage_y,&damage_w,&damage_h),
                "child native scroll invalidates old damage in both child and parent composite");
            x=0;y=my+outer_my+20;
            check(web_scroll_input(d,fl,&x,&y,0)&&f->scroll_y==my&&frameouter->scroll_y==outer_my&&y==40,
                "child viewport saturation forwards remainder through frame element ancestors");
            y=-1000;web_scroll_input(d,fl,&x,&y,0);
            check(!fc->scroll_y&&!f->scroll_y&&!frameouter->scroll_y&&y<0,
                "reverse input traverses child viewport and parent scrollport");
            y=0;check(web_scroll_input(d,fl,&x,&y,1)&&fc->scroll_y==40&&!f->scroll_y,
                "End in child consumes its nearest inner scrollport");
            const char *remove_scrollport="document.getElementById('fc').style.overflow='visible'";
            check(web_console_eval(child,remove_scrollport,strlen(remove_scrollport)),
                "child scrollport style changes through real CSSOM");
            web_layout(child,f->viewport_w,f->viewport_h);
            y=0;check(web_scroll_input(d,fl,&x,&y,1)&&f->scroll_y==MAX(0,child->doc_h-f->viewport_h)&&
                !frameouter->scroll_y,"End without child element overflow consumes child viewport");
            for(int i=0;i<48;i++)web_tick(d,uptime_ms());
            check(state.frame_events>0&&!child->scroll_viewport_pending,
                "normal child tick delivers document scroll event and clears pending source");
        }
        web_free(d);
    }
    const char *borrowed="<!doctype html><style>html,body{margin:0}iframe{width:120px;height:80px;border:0}</style>"
        "<iframe srcdoc=\"<!doctype html><style>html,body{margin:0}</style>"
        "<div id=borrow style='height:20px;width:30px;overflow:auto'><div id=borrowleaf style='height:80px'></div></div>\"></iframe>"
        "<iframe srcdoc=\"<!doctype html><style>html,body{margin:0}</style>"
        "<div id=borrow style='height:20px;width:30px;overflow:auto'><div id=borrowleaf style='height:80px'></div></div>\"></iframe>";
    d=scroll_input_document(borrowed,&host);
    check(d!=NULL,"sibling allocation-owner pending lifetime document");
    if(d){
        web_frames_prepare_paint(d,NULL);web_frames_finish_paint(d);
        /* web_frames_detach reverses discovery into frame-list order. Allocate
           in its first sibling and queue in the later sibling, exercising the
           unsafe allocation-owner-first direction without fabricating nodes. */
        struct web_frame *allocation=d->frames,*destination=allocation?allocation->next:NULL;
        web_doc *a=allocation?allocation->document:NULL,*b=destination?destination->document:NULL;
        node_t *borrow=a?by_id(a->root,"borrow"):NULL,*borrowleaf=a?by_id(a->root,"borrowleaf"):NULL;
        bool adopted=a&&b&&borrow&&borrowleaf&&doc_node_move(b,b->body,borrow,NULL);
        check(adopted,"real sibling adoption keeps allocation owner distinct from DOM owner");
        if(adopted){
            web_layout(b,destination->viewport_w,destination->viewport_h);x=0;y=5;
            check(web_scroll_input(d,borrowleaf,&x,&y,0)&&!y&&borrow->scroll_input_owner==b&&
                b->scroll_input_first==borrow,
                "later sibling queues native notification borrowing earlier sibling allocation");
        }
        web_free(d);
    }
done:
    printf("paintperformance scroll-input: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
/* Only the failed extent boundary and an abs-free normal-flow control. Root
   auto height follows content; viewport height is a containing-block input,
   not a minimum used document height (layout_inner_used). Print actual values
   before choosing between a product overflow leak and a fixture assumption. */
static int scroll_extents(void){
    struct web_host host={.script_checkpoint=checkpoint};
    const char *control="<!doctype html><style>html,body{margin:0}#clip{position:relative;"
        "overflow:auto;height:40px}</style><div id=clip></div>";
    web_doc *d=scroll_input_document(control,&host);
    check(d&&d->layout_valid,"extent control committed layout");
    int control_h=d?d->doc_h:-1;
    if(d){printf("scroll-extent control doc=%d root_h=%g root_scroll=%g viewport=%d\n",
        d->doc_h,d->root_box->h,d->root_box->scroll_h,d->height);
        check(d->doc_h==40&&d->root_box->h==40&&d->root_box->scroll_h==40,
            "short root used auto height follows forty-pixel normal flow, not viewport");web_free(d);}
    const char *clipped="<!doctype html><style>html,body{margin:0}#clip{position:relative;"
        "overflow:auto;height:40px}#abs{position:absolute;top:300px;height:50px;width:10px}</style>"
        "<div id=clip><div id=abs></div></div>";
    d=scroll_input_document(clipped,&host);
    node_t *clip=d?by_id(d->root,"clip"):NULL,*absolute=d?by_id(d->root,"abs"):NULL;
    bool ready=d&&d->layout_valid&&clip&&clip->box&&absolute&&absolute->box;
    check(ready,"extent private absolute has committed real boxes");
    if(ready){printf("scroll-extent private doc=%d root_h=%g root_scroll=%g clip_h=%g private=%g abs_y=%g abs_h=%g cb_is_clip=%d\n",
        d->doc_h,d->root_box->h,d->root_box->scroll_h,clip->box->h,clip->box->scroll_h,
        absolute->box->y,absolute->box->h,absolute->box->cb==clip->box);
        check(d->doc_h==control_h&&d->root_box->scroll_h==40&&clip->box->scroll_h>=350&&
            absolute->box->cb==clip->box,"private absolute extent remains private and root matches abs-free flow");}
    web_free(d);
    printf("paintperformance scroll-extent: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
static void placeholder_paint(web_doc *d, uint32_t *target) {
    canvas_t canvas;gfx_init(&canvas,target,320,200,320);
    web_layout(d,320,200);web_paint(d,&canvas,0,0,320,200,0,0);
}
static int placeholders(void) {
    const char *html="<!doctype html><style>html,body{margin:0;background:white}"
        "input,textarea{position:absolute;left:4px;top:4px;width:180px;height:26px;"
        "border:0;padding:0;margin:0;background:white;color:black;font:16px monospace}"
        "textarea{top:40px;height:50px}.hide::placeholder{color:transparent}"
        ".zero::placeholder{opacity:0}.red::placeholder{color:red;font-size:20px}"
        "</style><input id=single type=search placeholder=Hint><textarea id=multi placeholder=Hint></textarea>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://paint.test/placeholders","utf-8",&host);
    check(d!=NULL,"placeholder native document");if(!d)return 1;
    for(unsigned i=0;i<8;i++)web_tick(d,uptime_ms());web_layout(d,320,200);
    node_t *single=by_id(d->root,"single"),*multi=by_id(d->root,"multi");
    check(single&&multi&&single->style&&multi->style,"both input and textarea have styles");
    if(!single||!multi||!single->style||!multi->style){web_free(d);return 1;}
    check(!single->style->placeholder&&!multi->style->placeholder,"UA-only controls allocate no extra pseudo style");
    placeholder_paint(d,pixels);
    doc_node_attr(d,single,"placeholder","");doc_node_attr(d,multi,"placeholder","");
    placeholder_paint(d,reference);
    check(memcmp(pixels,reference,sizeof pixels)!=0,"UA hint text is actually painted");
    doc_node_attr(d,single,"placeholder","Hint");doc_node_attr(d,multi,"placeholder","Hint");
    doc_node_attr(d,single,"class","hide");doc_node_attr(d,multi,"class","hide");
    placeholder_paint(d,pixels);
    check(single->style->placeholder&&multi->style->placeholder,"authored input and textarea pseudo bodies cached");
    check(single->style->placeholder&&single->style->placeholder->color==0,"transparent declaration wins real cascade");
    check(!memcmp(pixels,reference,sizeof pixels),"transparent hints paint identical to no hint without hiding controls");
    doc_node_attr(d,single,"class","zero");doc_node_attr(d,multi,"class","zero");
    placeholder_paint(d,pixels);
    check(single->style->placeholder&&single->style->placeholder->opacity==0,"placeholder opacity reaches cached body");
    check(!memcmp(pixels,reference,sizeof pixels),"opacity zero also hides input and textarea hints");
    doc_node_attr(d,single,"class","red");doc_node_attr(d,multi,"class","red");
    placeholder_paint(d,pixels);
    check(single->style->placeholder&&single->style->placeholder->color==RGB(255,0,0)&&
        single->style->placeholder->font_size==20,"author colour and font reach pseudo body");
    bool red=false;for(unsigned i=0;i<sizeof pixels/sizeof *pixels;i++)
        if(((pixels[i]>>16)&255)>(pixels[i]&255)+30)red=true;
    check(red,"red authored placeholder glyph pixels present");
    doc_node_value(d,single,"value",5);doc_node_value(d,multi,"value",5);
    placeholder_paint(d,reference);
    doc_node_attr(d,single,"class","hide");doc_node_attr(d,multi,"class","hide");
    placeholder_paint(d,pixels);
    check(!memcmp(pixels,reference,sizeof pixels),"nonempty input and textarea values ignore placeholder paint rules");
    web_free(d);
    printf("paintperformance placeholder: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--placeholder"))return placeholders();
    if(argc>1&&!strcmp(argv[1],"--scroll-extent"))return scroll_extents();
    if(argc>1&&!strcmp(argv[1],"--scroll-input"))return scroll_inputs();
    if(argc>1&&!strcmp(argv[1],"--boundaries"))return boundaries();
    const char *html="<!doctype html><style>html,body{margin:0}"
        "a{display:block;height:23px}#clip{position:relative;overflow:hidden;width:150px;height:60px}"
        "#fixed{position:fixed;left:280px;top:10px;width:30px;height:30px}"
        "#cover{position:absolute;left:35px;top:8px;width:65px;height:40px;opacity:0;z-index:9}"
        "#restore{pointer-events:none}#restore a{pointer-events:auto}"
        "#move{position:relative;transform:translate(12px,4px)}"
        "</style><div id=clip><a href=/first>first</a><div id=cover></div>"
        "<a id=fixed href=/fixed>fixed</a><a href=/hidden>hidden</a></div>"
        "<div id=restore><a href=/restored>restored</a></div>"
        "<div id=move><a href=/translated>translated</a></div>"
        "<details open><summary>legend</summary><a href=/inside>inside</a></details>"
        "<div style='opacity:.5'><input id=control type=checkbox></div>"
        "<div style='height:100px'></div><a href=/bottom>bottom</a>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://paint.test/","utf-8",&host);
    check(d!=NULL,"document creation");if(!d)return 1;
    for(int i=0;i<8;i++)web_tick(d,uptime_ms());
    web_layout(d,320,200);compare_hits(d);
    check(d->hit_index!=NULL,"spatial index is native document owned");
    void *identity=d->hit_index;compare_hits(d);check(identity==d->hit_index,"unchanged mousemove reuses index storage");
    node_t *cover=by_id(d->root,"cover");
    doc_node_attr(d,cover,"style","position:absolute;left:100px;top:8px;width:65px;height:40px;z-index:-1");
    web_layout(d,320,200);compare_hits(d);
    web_viewport_position(d,0,75);compare_hits(d);web_viewport_position(d,0,0);
    node_t *control=by_id(d->root,"control");
    canvas_t canvas;gfx_init(&canvas,pixels,320,200,320);
    web_paint(d,&canvas,0,0,320,200,0,0);memcpy(reference,pixels,sizeof pixels);
    d->paint_dirty=d->paint_damage_valid=d->paint_damage_full=false;
    if(control)control->checked=!control->checked;
    doc_paint_damage(d,control);
    int x,y,w,h;bool local=web_take_paint_damage(d,&x,&y,&w,&h);
    check(local&&w>0&&h>0&&w<320&&h<200,"atomic damage is a bounded viewport rectangle");
    if(local){gfx_clip(&canvas,x,y,w,h);web_paint(d,&canvas,0,0,320,200,0,0);gfx_noclip(&canvas);}
    canvas_t full;gfx_init(&full,reference,320,200,320);web_paint(d,&full,0,0,320,200,0,0);
    check(!memcmp(pixels,reference,sizeof pixels),"clipped damage equals full alpha/stable-order repaint");
    d->paint_dirty=false;doc_paint_damage(d,control);doc_paint_damage(d,NULL);
    check(!web_take_paint_damage(d,&x,&y,&w,&h),"unknown damage cannot be reduced by a later local notification");
    web_free(d);
    printf("paintperformance: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
