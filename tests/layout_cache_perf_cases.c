static style_t plain(void){
    style_t s={.display=D_BLOCK,.overflow=OV_VISIBLE};
    s.height.kind=s.flex_basis.kind=s.min_height.kind=LK_AUTO;s.max_height.kind=LK_NONE;
    for(unsigned i=0;i<4;i++)s.inset[i].kind=LK_AUTO;
    return s;
}
static void test_prepare_dirty(void){
    style_t s=plain();box_t root={.kind=B_BLOCK,.st=&s},a={.kind=B_BLOCK,.st=&s},b={.kind=B_BLOCK,.st=&s},c={.kind=B_BLOCK,.st=&s};
    root.first=&a;a.next=&b;a.parent=b.parent=&root;a.first=&c;c.parent=&a;
    D->root_box=&root;check(layout_cache_prepare(&root),"initial safe subtree prepared");
    unsigned before=pct_queries;check(layout_cache_prepare(&root)&&pct_queries==before,"unchanged prepare performs no subtree scan");
    layout_invalidate(&c);check(!root.layout_prepare_valid&&!a.layout_prepare_valid&&b.layout_prepare_valid,"invalidation reaches ancestors not sibling");
    before=pct_queries;layout_cache_prepare(&root);check(pct_queries>before&&b.layout_prepare_valid,"dirty branch preparation preserves cached sibling");
    c.abspos=true;layout_invalidate(&c);
    check(!layout_cache_prepare(&root)&&b.layout_cache_safe,"absolute registration disables ancestor reuse but not sibling");
    c.abspos=false;c.floated=true;layout_invalidate(&c);layout_cache_prepare(&root);
    check(a.layout_cache_exports_floats&&root.layout_cache_exports_floats,"float registration propagates across shared BFC");
    a.is_bfc=true;layout_invalidate(&a);layout_cache_prepare(&root);
    check(a.layout_cache_exports_floats&&!root.layout_cache_exports_floats,"independent BFC contains descendant float export");
    layout_invalidate_fonts(&root);
    check(!b.layout_prepare_valid&&!c.layout_post_valid&&root.layout_dirty,"font invalidation clears subtree auxiliary caches");
}
static void test_relative_and_overflow(void){
    style_t s=plain(),r=plain();r.position=POS_RELATIVE;r.inset[0]=(len_t){.kind=LK_LEN,.pct=.5f};
    box_t root={.kind=B_BLOCK,.st=&s,.w=100,.h=100},child={.kind=B_BLOCK,.st=&r,.w=40,.h=20,.x=10,.y=5},leaf={.kind=B_BLOCK,.st=&s,.w=200,.h=30,.x=5,.y=30};
    root.first=&child;child.parent=child.cb=&root;child.first=&leaf;leaf.parent=leaf.cb=&child;
    D->root_box=&root;layout_cache_prepare(&root);relative_offsets(&root);scroll_areas(&root);
    check(near(child.rel_dy,50)&&near(root.scroll_w,215)&&near(root.scroll_h,115),"native relative and overflowing descendant bounds");
    check(near(max_bottom(&root,0),115),"document bottom shares final overflow traversal");
    root.layout_cache_valid=child.layout_cache_valid=leaf.layout_cache_valid=true;
    unsigned before=checkpoints,reads=resolutions;relative_offsets(&root);scroll_areas(&root);
    check(checkpoints==before&&resolutions==reads,"unchanged relative and scroll passes prune complete cached tree");
    root.h=200;child.x=20;relative_offsets(&root);scroll_areas(&root);
    check(near(child.rel_dy,100)&&near(root.scroll_w,225)&&near(root.scroll_h,200),"parent size and cached child placement republish against current cb");
    check(near(max_bottom(&root,0),200),"updated root bottom is not stale cached global union");
    child.x=0;child.y=0;root.h=10;r.overflow=OV_HIDDEN;layout_invalidate(&child);layout_cache_prepare(&root);relative_offsets(&root);scroll_areas(&root);
    check(near(root.scroll_w,100)&&near(root.scroll_h,25),"clipped private child overflow never leaks to ancestor");
    child.clamp_truncated=true;child.h=8;layout_invalidate(&child);scroll_areas(&root);
    check(near(child.scroll_h,8)&&near(root.scroll_h,13),"line clamp truncation owns final scrolling extent");
}
static void test_run_descent_and_cancel(void){
    style_t s=plain();struct run run={.x=4,.y=20,.w=40,.descent=7};
    box_t root={.kind=B_BLOCK,.st=&s,.w=10,.h=2,.runs=&run,.nruns=1};D->root_box=&root;
    layout_cache_prepare(&root);scroll_areas(&root);
    check(near(root.scroll_w,44)&&near(root.scroll_h,27),"overflow consumes line-owned font descent without metric dispatch");
    root.layout_cache_valid=true;root.layout_post_valid=true;root.w=11;
    cancel_at=checkpoints+1;scroll_areas(&root);
    check(D->native_cancelled&&!root.layout_post_valid,"cancelled post traversal never commits partially overwritten cache");
    D->native_cancelled=false;cancel_at=0;
}
static void test_percentage_width_and_content_shift(void){
    style_t s=plain(),r=plain();r.position=POS_RELATIVE;
    r.inset[3]=(len_t){.kind=LK_LEN,.pct=.25f};r.inset[0]=(len_t){.kind=LK_LEN,.pct=.5f};
    struct run run={.x=2,.y=15,.w=70,.descent=5};
    box_t root={.kind=B_BLOCK,.st=&s,.w=100,.h=100},child={.kind=B_CELL,.st=&r,.w=40,.h=20,.x=10,.y=5,.runs=&run,.nruns=1};
    root.first=&child;child.parent=child.cb=&root;D->root_box=&root;
    layout_cache_prepare(&root);relative_offsets(&root);scroll_areas(&root);
    root.layout_cache_valid=child.layout_cache_valid=true;
    check(near(child.rel_dx,25)&&near(child.rel_dy,50),"relative percent input uses original actual containing block");
    root.w=200;root.h=200;relative_offsets(&root);scroll_areas(&root);
    check(near(child.rel_dx,50)&&near(child.rel_dy,100),"cached child own relative width and height percentages refresh on parent cb change");
    child.content_dy=30;root.layout_post_valid=false;relative_offsets(&root);scroll_areas(&root);
    check(near(child.scroll_h,50)&&near(child.layout_post_content_dy,30),"table alignment content shift defeats cached post result while retaining inner runs");
    child.x=400;root.layout_post_valid=false;scroll_areas(&root);
    check(near(root.scroll_w,522),"cached overflowing child is published at current placement");
    child.x=0;root.layout_post_valid=false;scroll_areas(&root);
    check(near(root.scroll_w,200),"overflow union is rebuilt rather than retaining old distant placement");
}
static void test_absolute_outside_cb_and_cancel_dirty(void){
    style_t s=plain(),clip=plain(),absolute=plain(),fixed=plain();clip.overflow=OV_HIDDEN;
    absolute.position=POS_ABSOLUTE;fixed.position=POS_FIXED;
    box_t root={.kind=B_BLOCK,.st=&s,.w=100,.h=100},scroller={.kind=B_BLOCK,.st=&clip,.w=40,.h=40},a={.kind=B_BLOCK,.st=&absolute,.abspos=true,.w=50,.h=50,.x=300,.y=300},b={.kind=B_BLOCK,.st=&fixed,.abspos=true,.w=50,.h=50,.x=600,.y=600};
    root.first=&scroller;scroller.parent=scroller.cb=&root;scroller.first=&a;a.parent=&scroller;a.cb=&root;a.next=&b;b.parent=&scroller;b.cb=&root;
    D->root_box=&root;check(!layout_cache_prepare(&root)&&!scroller.layout_cache_safe,"absolute subtree cannot reuse away fresh registration");
    relative_offsets(&root);scroll_areas(&root);
    check(near(scroller.scroll_w,40)&&near(scroller.scroll_h,40),"absolute child with outside cb never inflates unrelated scroller");
    check(near(root.scroll_w,350)&&near(root.scroll_h,350),"outside cb receives absolute overflow directly while viewport-fixed box is excluded");
    check(near(max_bottom(&root,0),100),"normal-flow bottom excludes absolute child for separate absolute aggregation");
    scroller.layout_cache_valid=true;a.layout_cache_valid=true;b.layout_cache_valid=true;
    layout_invalidate(&a);check(root.layout_dirty&&scroller.layout_dirty&&a.layout_dirty&&!a.layout_post_valid,"leaf mutation preserves dirty ancestor propagation before cancellation");
    cancel_at=checkpoints+1;scroll_areas(&root);
    check(D->native_cancelled&&!root.layout_post_valid&&scroller.layout_dirty&&a.layout_dirty,"cancelled parent aggregation keeps pending child invalidations");
    D->native_cancelled=false;cancel_at=0;
}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--round2")){test_percentage_width_and_content_shift();test_absolute_outside_cb_and_cancel_dirty();}
    else {test_prepare_dirty();test_relative_and_overflow();test_run_descent_and_cancel();}
    printf("layout-cache-perf: %u checks, %u failures\n",checks,failures);return failures!=0;
}
