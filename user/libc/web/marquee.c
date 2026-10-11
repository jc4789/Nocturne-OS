/* Legacy marquee motion shares native paint/hit-test coordinates. No JS
 * timer, stylesheet mutation, DOM-wide tick walk, or CSSOM scroll rewrite. */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <nocturne.h>
#include "webi.h"

bool web_marquee_box(const box_t *b) {
    return b && b->node && b->node->box==b && !b->anon &&
        !b->node->foreign && b->node->tag==T_marquee && b->kind!=B_INLINE;
}
void web_marquee_register(web_doc *d,node_t *n) {
    if(!d || !n || n->owner!=d)return;
    pvec *p=&d->marquees;
    if(p->n==p->cap) {
        if(p->cap>INT_MAX/2)return;
        int cap=p->cap?p->cap*2:4;
        if((size_t)cap>SIZE_MAX/sizeof(void *))return;
        void **v=realloc(p->v,(size_t)cap*sizeof(void *));
        if(!v)return;
        p->v=v;p->cap=cap;
    }
    p->v[p->n++]=n;
}
void web_marquee_set(node_t *n,bool running) {
    if(!n || n->foreign || n->tag!=T_marquee)return;
    n->marquee_off=!running;
    /* Resume from the frozen position; stopped time is not accumulated. */
    n->marquee_last=uptime_ms();
    if(n->owner)n->owner->paint_dirty=true;
}
static long integer_attr(node_t *n,const char *key,long fallback) {
    const char *s=node_attr(n,key);if(!s)return fallback;
    while(*s==' ' || *s=='\t' || *s=='\n' || *s=='\f' || *s=='\r')s++;
    const char *digits=(*s=='+' || *s=='-')?s+1:s;
    if(*digits<'0' || *digits>'9')return fallback;
    char *end;long value=strtol(s,&end,10);
    return end==s?fallback:value;
}
static uint64_t interval(node_t *n) {
    long value=integer_attr(n,"scrolldelay",85);
    uint64_t delay=value<0?85:(uint64_t)value;
    if(!node_attr(n,"truespeed") && delay<60)delay=60;
    /* Zero interval is legal, but one browser turn must remain bounded. */
    return delay?delay:1;
}
static bool active(web_doc *d,node_t *n) {
    return n && n->owner==d && !n->marquee_off && doc_node_connected(n) &&
        doc_node_root(n,true)==d->root &&
        n->box && web_marquee_box(n->box) && n->box->st &&
        n->box->st->display!=D_NONE && n->box->st->content_visibility!=CV_HIDDEN;
}
static uint8_t direction(node_t *n) {
    const char *s=node_attr(n,"direction");
    return !s?0:!strcasecmp(s,"right")?1:!strcasecmp(s,"up")?2:!strcasecmp(s,"down")?3:0;
}
static uint8_t behavior(node_t *n) {
    const char *s=node_attr(n,"behavior");
    return !s?0:!strcasecmp(s,"slide")?1:!strcasecmp(s,"alternate")?2:0;
}
static float inline_extent(const box_t *b,bool vertical) {
    float extent=0;
    if(vertical) {
        for(int i=0;i<b->nlines;i++)extent=fmaxf(extent,b->lines[i].bottom);
    } else {
        for(int i=0;i<b->nruns;i++)extent=fmaxf(extent,b->runs[i].x+b->runs[i].w);
    }
    for(int i=0;i<b->ndecos;i++) {
        const struct deco *r=&b->decos[i];
        extent=fmaxf(extent,vertical?r->y+r->h:r->x+r->w);
    }
    return extent;
}
static float content_extent(const box_t *b,bool vertical) {
    /* scrollWidth/Height include the viewport minimum, and fixed-width
       containers need not compute intrinsic sizes. Read their actual native
       runs and child placement without shaping or visiting the DOM again. */
    float extent=inline_extent(b,vertical);
    float origin=vertical?box_abs_y(b):box_abs_x(b);
    for(const box_t *c=b->first;c;c=c->next) {
        if(c->clamp_hidden || !c->st || c->st->display==D_NONE || c->kind==B_TEXT || c->kind==B_BR || c->kind==B_INLINE)continue;
        float start=(vertical?box_abs_y(c):box_abs_x(c))-origin;
        float size=c->anon?inline_extent(c,vertical):vertical?c->h+c->p[2]+c->b[2]:c->w+c->p[1]+c->b[1];
        if(!c->anon)size=fmaxf(size,vertical?c->scroll_h-c->p[0]:c->scroll_w-c->p[3]);
        if(isfinite(start) && isfinite(size))extent=fmaxf(extent,start+size);
    }
    return extent;
}
static void position(node_t *n,box_t *b,float content) {
    bool vertical=n->marquee_direction>=2,positive=(n->marquee_direction&1)!=0;
    float viewport=vertical?b->h:b->w;
    if(!isfinite(viewport)||!isfinite(content)||viewport<=0 || content<0)return;
    float offset;
    if(n->marquee_behavior==2) {
        float low=fminf(0,viewport-content),high=fmaxf(0,viewport-content);
        float length=high-low;
        float p=fminf(n->marquee_progress,length);
        bool reverse=positive!=n->marquee_reverse;
        offset=reverse?low+p:high-p;
    } else {
        offset=positive?-content+n->marquee_progress:viewport-n->marquee_progress;
    }
    n->marquee_x=vertical?0:offset;n->marquee_y=vertical?offset:0;
}
void web_marquee_tick(web_doc *d,uint64_t now) {
    if(!d || !d->live || d->inert || d->native_cancelled)return;
    for(int i=0;i<d->marquees.n;i++) {
        if(!web_native_checkpoint(d))return;
        node_t *n=d->marquees.v[i];if(!active(d,n))continue;
        box_t *b=n->box;
        uint8_t dir=direction(n),mode=behavior(n);
        if(!n->marquee_initialized || n->marquee_direction!=dir || n->marquee_behavior!=mode) {
            n->marquee_initialized=true;n->marquee_direction=dir;n->marquee_behavior=mode;
            n->marquee_progress=0;n->marquee_reverse=false;n->marquee_loops=0;n->marquee_last=now;
            position(n,b,content_extent(b,dir>=2));d->paint_dirty=true;continue;
        }
        uint64_t delay=interval(n);
        if(now<n->marquee_last || now-n->marquee_last<delay)continue;
        /* Skip elapsed intervals arithmetically, rather than looping catchup. */
        uint64_t steps=(now-n->marquee_last)/delay;n->marquee_last=now;
        long amount=integer_attr(n,"scrollamount",6);if(amount<0)amount=6;
        if(!amount)continue;
        bool vertical=dir>=2;
        float viewport=vertical?b->h:b->w;
        float content=content_extent(b,vertical);
        if(!isfinite(viewport)||!isfinite(content)||viewport<=0 || content<0)continue;
        float length=mode==2?fabsf(viewport-content):mode==1?((dir&1)?content:viewport):viewport+content;
        double progress=(double)n->marquee_progress+(double)amount*steps;
        if(length<=0)continue;
        if(progress>=length) {
            n->marquee_progress=length;
            long loops=integer_attr(n,"loop",-1);
            double cycles=floor(progress/length);
            uint64_t count=cycles>=(double)UINT64_MAX?UINT64_MAX:(uint64_t)cycles;
            uint64_t old_loops=n->marquee_loops;
            if(loops>0)n->marquee_loops=count>UINT64_MAX-n->marquee_loops?UINT64_MAX:n->marquee_loops+count;
            if((loops>0 && n->marquee_loops>=(uint64_t)loops) || mode==1) {
                if(mode==2) {
                    uint64_t remaining=old_loops<(uint64_t)loops?(uint64_t)loops-old_loops:1;
                    if(!(remaining&1))n->marquee_reverse=!n->marquee_reverse;
                }
                n->marquee_off=true;
            }
            else {
                if(mode==2 && fmod(cycles,2)>=1)n->marquee_reverse=!n->marquee_reverse;
                n->marquee_progress=(float)fmod(progress,length);
            }
        } else n->marquee_progress=(float)progress;
        float oldx=n->marquee_x,oldy=n->marquee_y;position(n,b,content);
        if(oldx!=n->marquee_x || oldy!=n->marquee_y)d->paint_dirty=true;
    }
}
int64_t web_marquee_deadline(web_doc *d,uint64_t now) {
    if(!d || d->inert || d->native_cancelled)return -1;
    uint64_t best=UINT64_MAX;
    for(int i=0;i<d->marquees.n;i++) {
        node_t *n=d->marquees.v[i];if(!active(d,n))continue;
        long amount=integer_attr(n,"scrollamount",6);if(amount==0)continue;
        uint64_t delay=interval(n),due=n->marquee_last>UINT64_MAX-delay?UINT64_MAX:n->marquee_last+delay;
        if(due<best)best=due;
    }
    return best==UINT64_MAX?-1:best<=now?0:(int64_t)(best-now);
}
