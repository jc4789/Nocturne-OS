#include "nocturne.h"
#include "webi.h"
#include <stdio.h>
#include <math.h>

/* New missing-predicate/debug-plumbing boundary only. Successful 83/35
 * clamp groups are not called by this fixture. */
static int checks, failures;
struct logs { unsigned traces, eligible, ineligible; };
static void logger(void *opaque, int level, const char *message) {
    (void)level;
    struct logs *l = opaque;
    if (!message || strncmp(message,"Native clamp trace:",19)) return;
    l->traces++;
    if (strstr(message,"eligible=1 applied=1")) l->eligible++;
    if (strstr(message,"eligible=0 applied=0")) l->ineligible++;
}
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL lineclamppredicate %s\n",name); }
}
static node_t *id(web_doc *d,const char *name) {
    for (node_t *n=d->owned_nodes;n;n=n->owned_next) {
        const char *v=node_attr(n,"id");
        if (v && !strcmp(v,name) && doc_node_connected(n)) return n;
    }
    return NULL;
}
static box_t *box(web_doc *d,const char *name) { node_t *n=id(d,name);return n?n->box:NULL; }
static bool near(float a,float b) { return fabsf(a-b)<.05f; }
static const char html[] =
    "<style>html,body{margin:0;font:16px/20px sans-serif}"
    ".sample{width:180px;-webkit-line-clamp:2;-webkit-box-orient:vertical}"
    "#ordinary{display:block}#horizontal{display:-webkit-box;-webkit-box-orient:horizontal}"
    "#active{display:-webkit-box}#overwritten{display:-webkit-box;display:flow-root}"
    "#grid{display:grid}#small{display:block}#plain{display:block;-webkit-line-clamp:none}</style>"
    "<div id=ordinary class=sample>first<br>second<br>third<br>fourth</div>"
    "<div id=horizontal class=sample>first<br>second<br>third<br>fourth</div>"
    "<div id=active class=sample>first<br>second<br>third<br>fourth</div>"
    "<div id=overwritten class=sample>first<br>second<br>third<br>fourth</div>"
    "<div id=grid class=sample>first<br>second<br>third<br>fourth</div>"
    "<div id=small class=sample>first</div>"
    "<div id=plain class=sample>first<br>second<br>third<br>fourth</div>";
static web_doc *load(struct logs *l,bool debug) {
    struct web_host host={.opaque=l,.console=logger,.debug_js=debug};
    web_doc *d=web_live(html,strlen(html),"https://line-clamp-predicate.test/","utf-8",&host);
    check("new predicate live document allocated",d!=NULL);
    if (d) {
        for (int i=0;i<8;i++) web_tick(d,uptime_ms());
        web_layout(d,480,240);
    }
    return d;
}
int main(void) {
    struct logs on={0},off={0};
    web_doc *d=load(&on,true);
    if (!d) goto done;
    check("live debug host reaches the actual native document",d->profile_enabled);
    box_t *ordinary=box(d,"ordinary"),*horizontal=box(d,"horizontal"),*active=box(d,"active"),
        *overwritten=box(d,"overwritten"),*grid=box(d,"grid"),*small=box(d,"small"),*plain=box(d,"plain");
    const struct clamp_trace *o=ordinary?ordinary->clamp_trace:NULL,*h=horizontal?horizontal->clamp_trace:NULL,
        *a=active?active->clamp_trace:NULL,*w=overwritten?overwritten->clamp_trace:NULL,
        *g=grid?grid->clamp_trace:NULL,*s=small?small->clamp_trace:NULL;
    check("nonlegacy block retains a numeric candidate",o!=NULL);
    check("nonlegacy block is explicitly ineligible and unapplied",o && !o->eligible && !o->applied);
    check("candidate diagnostic never changes full ordinary height",o && near(o->full_h,80) &&
        near(o->cut_h,80) && near(o->post_h,80) && near(ordinary->h,80));
    check("ineligible inline context did not acquire active line-depth records",ordinary &&
        ordinary->inline_ctx && ordinary->nlines==0);
    check("horizontal legacy box retains a numeric candidate",h!=NULL);
    check("horizontal legacy orientation remains ineligible",h && !h->eligible && !h->applied &&
        horizontal->st->legacy_box && horizontal->st->box_orient==BO_HORIZONTAL);
    check("horizontal candidate retains uncropped height",h && near(h->full_h,80) && near(h->post_h,80));
    check("later authored flow-root clears the legacy display predicate",w && !w->eligible &&
        !w->applied && !overwritten->st->legacy_box && overwritten->st->display==D_FLOW_ROOT);
    check("other formatting-context kind is observed rather than assumed eligible",g &&
        grid->kind==B_GRID && !g->eligible && !g->applied && near(g->post_h,80));
    check("eligible box publishes both eligibility and application",a && a->eligible && a->applied);
    check("eligible candidate separates full and cut heights",a && near(a->full_h,80) &&
        near(a->cut_h,40) && near(a->post_h,40) && near(active->h,40));
    check("active line-depth records contain only the visible budget",active && active->nlines==2 &&
        a && a->lines==2);
    check("small candidate remains numeric but does not need a large-height report",s &&
        !s->eligible && !s->applied && near(s->full_h,20));
    check("no-clamp box gets no debug arena record",plain && !plain->clamp_trace);
    check("native supervisor publishes at most four candidates per document",on.traces==4 && d->clamp_trace_reported==4);
    check("active predicate appears among supervisor reports",on.eligible==1);
    check("formerly filtered ineligible candidates appear among reports",on.ineligible==3);
    doc_node_attr(d,id(d,"ordinary"),"style","width:160px"); web_layout(d,480,240);
    check("later layout cannot exceed document report cap",on.traces==4 && d->clamp_trace_reported==4);
    web_free(d);
    d=load(&off,false);
    if (!d) goto done;
    check("ordinary live host keeps native debug gate disabled",!d->profile_enabled);
    ordinary=box(d,"ordinary"); active=box(d,"active");
    check("debug-off candidates allocate no diagnostic record",ordinary && active &&
        !ordinary->clamp_trace && !active->clamp_trace);
    check("debug-off makes no extra console publication",off.traces==0 && d->clamp_trace_reported==0);
    check("debug-off still performs the actual eligible clamp",active && active->clamp_truncated && near(active->h,40));
    web_free(d);
done:
    printf("lineclamppredicate: %d checks, %d failures\n",checks,failures);
    return failures!=0;
}
