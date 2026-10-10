/* Native integration regressions; real-site rendering is checked separately. */
#include <nocturne.h>
#include <parallel.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webi.h"

static int checks, failed;
static volatile uint32_t mask;
static void check(bool ok, const char *name) {
    checks++; if (!ok) { failed++; printf("FAIL parallel %s\n", name); }
}
static uint64_t values[65];
static void work(size_t i, void *context) {
    uint64_t *out = context, value = i + 1;
    unsigned bit = 1u << cpu_index();
    __asm__ volatile("lock orl %1, %0" : "+m"(mask) : "r"(bit) : "memory", "cc");
    for (unsigned k = 0; k < 200000; k++) value = value * UINT64_C(6364136223846793005) + 1;
    out[i] = value;
}
static void leaf(size_t i, void *context) { ((unsigned *)context)[i]++; }
static void nested(size_t i, void *context) { parallel_for(2, leaf, (unsigned *)context + i * 2); }
static void profiled_skew(size_t i, void *context) {
    if (!i) msleep(20); /* uneven duration, not a performance benchmark */
    work(i,context);
    parallel_record_work(PARALLEL_GENERIC,i+1);
}
static void profile_coverage(void) {
    struct parallel_stats before, after, disabled;
    parallel_get_stats(PARALLEL_GENERIC,&before);
    parallel_profile_enable(true);
    parallel_for_stage(PARALLEL_GENERIC,65,profiled_skew,values,NULL,NULL);
    parallel_get_stats(PARALLEL_GENERIC,&after);
    check(after.batches==before.batches+1 && after.items-before.items==65 && after.work-before.work==2145,
          "dynamic skewed jobs record exact coverage and units");
    struct n_cpuinfo info;cpu_info(&info);
    check(info.scheduler_cpus==1 || (after.helper_items>before.helper_items && after.helper_work>before.helper_work),
          "AP executes actual work, not merely an empty descriptor");
    check(after.sampled_cpu_mask && (info.scheduler_cpus==1 || after.sampled_helper_cpu_mask),
          "execution CPU samples are recorded");
    parallel_profile_enable(false);
    parallel_for(65,work,values);
    parallel_get_stats(PARALLEL_GENERIC,&disabled);
    check(!memcmp(&after,&disabled,sizeof after),"profiling disabled has no statistics update");
}
static node_t *find(node_t *n, const char *id) {
    if (!n) return NULL;
    const char *s = node_attr(n,"id");
    if (s && !strcmp(s,id)) return n;
    for (node_t *c=n->first;c;c=c->next) { node_t *v=find(c,id); if(v)return v; }
    return NULL;
}
static void pool(void) {
    uint64_t reference[65];
    parallel_enable(false); parallel_for(65,work,reference);
    parallel_enable(true); mask=0;
    check(parallel_for(65,work,values),"parallel dispatch");
    check(!memcmp(values,reference,sizeof values),"exact coverage and shared outputs");
    struct n_cpuinfo info; cpu_info(&info);
    unsigned active=0;
    for(unsigned i=0;i<32;i++)active+=(mask>>i)&1;
    printf("paralleltest: CPU mask=%x active=%u scheduler=%u\n",mask,active,info.scheduler_cpus);
    check(active>1 || info.scheduler_cpus==1,"multiple user CPUs");
    unsigned counts[64]={0};
    parallel_for(32,nested,counts);
    bool exact=true;for(int i=0;i<64;i++)if(counts[i]!=1)exact=false;
    check(exact,"nested parallel serial fallback");
    for(size_t n=0;n<9;n++) {
        memset(counts,0,sizeof counts);parallel_for(n,leaf,counts);
        for(size_t i=0;i<64;i++)if(counts[i]!=(i<n))exact=false;
    }
    check(exact,"zero and small partial jobs");
}
static void images(void) {
    uint32_t *pixels=malloc(351*293*sizeof *pixels);
    if(!pixels){check(false,"image allocation");return;}
    for(int i=0;i<351*293;i++)pixels[i]=0x80000000u+(uint32_t)i*7919;
    image_t source={351,293,pixels};
    parallel_enable(false);image_t *reference=image_scale(&source,701,403);
    parallel_enable(true);image_t *actual=image_scale(&source,701,403);
    check(reference&&actual&&!memcmp(reference->px,actual->px,701*403*4),"bilinear tiles exact serial equivalence");
    image_free(reference);image_free(actual);
    parallel_enable(false);reference=image_scale(&source,256,288);
    parallel_enable(true);actual=image_scale(&source,256,288);
    check(reference&&actual&&!memcmp(reference->px,actual->px,256*288*4),"reduction tiles exact serial equivalence");
    image_free(reference);image_free(actual);free(pixels);
}
static void browser(void) {
    const char *html="<!doctype html><style>body{margin:0}#a,#b{display:flow-root;width:380px}"
        "#a{height:250px;background:linear-gradient(25deg,red,blue);border:9px solid green;border-radius:40px}"
        "#b{height:250px;background:radial-gradient(circle,white,black)}.f{display:flex}.f div{padding:5px}</style>"
        "<div id=a>Hello one</div><div id=b>Hello two</div><div class=f>"
        "<div>Aa</div><div>Bb</div><div>Cc</div><div>Dd</div><div>Ee</div><div>Ff</div><div>Gg</div><div>Hh</div></div>";
    web_doc *d=web_parse(html,strlen(html),"https://parallel.test/","utf-8");
    check(d!=NULL,"document creation");if(!d)return;
    parallel_enable(false);web_layout(d,800,600);
    uint32_t *left=calloc(800*600,4),*right=calloc(800*600,4);
    if(!left||!right){check(false,"paint canvases");free(left);free(right);web_free(d);return;}
    canvas_t c;gfx_init(&c,left,800,600,800);web_paint(d,&c,0,0,800,600,0,0);
    parallel_enable(true);d->layout_valid=false;layout_invalidate(d->root_box);
    uint64_t layout_jobs=parallel_jobs();web_layout(d,800,600);
    check(parallel_jobs()>=layout_jobs+2,"intrinsic and full child layout execute helper jobs");
    gfx_init(&c,right,800,600,800);web_paint(d,&c,0,0,800,600,0,0);
    check(!memcmp(left,right,800*600*4),"parallel full layout and tiled raster match serial pixels");
    node_t *a=find(d->root,"a"),*b=find(d->root,"b");
    check(a&&b&&a->box&&b->box,"local layout boxes");
    if(a&&b&&a->box&&b->box) {
        box_t *root=d->root_box,*old_a=a->box,*old_b=b->box;
        struct run *runs=old_b->runs;
        check(doc_node_attr(d,a,"style","width:210px"),"local geometry change");
        web_layout(d,800,600);
        check(d->root_box==root&&a->box==old_a&&b->box==old_b,"compatible mutation preserves box topology");
        check(a->box->w==210&&b->box->runs==runs,"only dirty BFC reflows");
        if(a->first&&a->first->type==N_TEXT) {
            check(doc_node_text(d,a->first,"Updated text",12),"local CharacterData update");
            web_layout(d,800,600);
            check(d->root_box==root&&b->box->runs==runs,"text mutation reuses unrelated layout");
        }
    }
    d->layout_valid=false;layout_invalidate(d->root_box);
    uint64_t before=parallel_jobs();web_layout(d,800,600);
    check(parallel_jobs()>before,"flex subtree intrinsic parallel jobs");
    free(left);free(right);web_free(d);
}
struct stop_state { int owner; unsigned caller_checks, wrong_thread; };
static bool stop_in_caller(void *opaque) {
    struct stop_state *s=opaque;
    if(thread_id()!=s->owner){
        unsigned one=1;
        __asm__ volatile("lock xaddl %0, %1" : "+r"(one), "+m"(s->wrong_thread) : : "memory", "cc");
        return false;
    }
    if(parallel_active() && !parallel_worker_index()) {
        s->caller_checks++;
        return false;
    }
    return true;
}
static void cancellation(void) {
    char *html=calloc(1,64000);if(!html){check(false,"cancellation fixture");return;}
    char *p=html;p+=sprintf(p,"<!doctype html><style>");
    /* Enough independent rule matching that helpers cannot finish every tiny
       leaf during publication before the supervisor gets a subtree job. */
    for(int i=0;i<96;i++)p+=sprintf(p,".leaf:not(.absent%d){color:red}",i);
    p+=sprintf(p,"</style><body>");
    for(int i=0;i<12;i++) {
        p+=sprintf(p,"<div>");
        for(int j=0;j<100;j++)p+=sprintf(p,"<span class=leaf>text</span>");
        p+=sprintf(p,"</div>");
    }
    struct stop_state state={.owner=thread_id()};
    struct web_host host={.opaque=&state,.script_checkpoint=stop_in_caller};
    web_doc *d=web_live(html,(size_t)(p-html),"https://parallel.test/stop","utf-8",&host);
    check(d!=NULL,"cancellation live document");
    if(d) {
        /* Let the product parser finish before forcing a fresh cascade. */
        web_tick(d,uptime_ms());
        d->need_style=true;d->style_full_dirty=true;d->layout_valid=false;
        web_layout(d,800,600);
        printf("paralleltest: native-stop caller_checks=%u cancelled=%d wrong_thread=%u\n",
               state.caller_checks,(int)d->native_cancelled,state.wrong_thread);
        check(state.caller_checks && d->native_cancelled,"native stop during caller parallel subtree");
        check(!state.wrong_thread,"only supervisor executes native UI checkpoint");
        web_free(d); /* every helper must have joined before any arena is freed */
    }
    free(html);
    unsigned counts[64]={0};parallel_for(64,leaf,counts);
    bool exact=true;for(int i=0;i<64;i++)if(counts[i]!=1)exact=false;
    check(exact,"pool remains usable after native stop and document free");
}
static void stretch_definiteness(void) {
    const char *html="<!doctype html><style>body{margin:0}.row{display:flex;width:300px}"
        ".row>div{width:100px}.sibling{height:80px}.percent{height:100%}</style>"
        "<div class=row><div id=item style=min-height:80px><div id=percent class=percent></div></div>"
        "<div class=sibling></div></div>"
        "<div class=row><div id=plain><div id=plainpercent class=percent></div></div>"
        "<div class=sibling></div></div>";
    web_doc *d=web_parse(html,strlen(html),"https://parallel.test/stretch","utf-8");
    check(d!=NULL,"stretch document");if(!d)return;
    web_layout(d,800,600);
    node_t *item=find(d->root,"item"),*percent=find(d->root,"percent"),
        *plain=find(d->root,"plain"),*plainpercent=find(d->root,"plainpercent");
    check(item&&percent&&item->box&&percent->box&&item->box->h==80&&percent->box->h==80,
        "same measured height still propagates definite height to percentage child");
    check(plain&&plainpercent&&plain->box&&plainpercent->box&&plain->box->h==80&&plainpercent->box->h==80,
        "sibling stretch propagates used height through percentage descendants");
    web_free(d);
}
int main(void) {
    pool();profile_coverage();images();browser();stretch_definiteness();cancellation();
    uint64_t jobs=parallel_jobs();parallel_shutdown();
    unsigned counts[2]={0};parallel_for(2,leaf,counts);
    check(counts[0]==1&&counts[1]==1,"pool shutdown and restart");
    parallel_shutdown();
    bool restarts=true;
    for (int i=0;i<32;i++) {
        counts[0]=counts[1]=0;
        parallel_for(2,leaf,counts);
        if(counts[0]!=1 || counts[1]!=1)restarts=false;
        parallel_shutdown();
    }
    check(restarts,"bounded idle-wait shutdown publication stress");
    printf("paralleltest: %d checks, %d failed, native jobs=%lu\n",checks,failed,(unsigned long)jobs);
    return failed?1:0;
}
