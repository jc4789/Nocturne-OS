static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static void set_attr(node_t *n,const char *name,const char *value){
    n->attrs=calloc(1,sizeof *n->attrs);n->nattrs=1;n->attrs[0].raw=name;n->attrs[0].value=value;
}
int main(void){
    web_doc top={.live=true},child={.live=true},nested={.live=true};D=&top;
    node_t tr={.type=N_DOC,.owner=&top},cr={.type=N_DOC,.owner=&child},nr={.type=N_DOC,.owner=&nested};
    top.root=&tr;child.root=&cr;nested.root=&nr;
    style_t plain={.display=D_INLINE,.font_size=14},block={.display=D_BLOCK};
    box_t cb={.kind=B_BLOCK,.x=8,.y=20,.st=&block};
    node_t anchor={.type=N_ELEM,.tag=T_a,.owner=&child,.parent=&cr,.anchor_block=&cb};
    box_t ib={.kind=B_INLINE,.st=&plain,.node=&anchor};anchor.box=&ib;
    struct iline L={0};add_deco(&L,&ib,0,80,12,true,false);add_deco(&L,&ib,0,55,30,false,true);
    check(L.decos.n==2*sizeof(struct deco),"plain inline has real fragments without background/border");
    cb.decos=(struct deco*)L.decos.p;cb.ndecos=2;
    int x=0,y=0,w=0,h=0;
    check(web_node_rect(&child,&anchor,&x,&y,&w,&h),"child inline native rectangle exists");
    check(x==8&&y==22&&w==80&&h==32,"multiline fragment union, not unpositioned zero-width inline box");
    check(ib.w==0&&ib.h==0,"geometry does not fabricate mutable inline box dimensions");
    node_t frame_node={.type=N_ELEM,.tag=T_frame,.owner=&top,.parent=&tr};
    box_t fb={.kind=B_ATOMIC,.node=&frame_node,.st=&block,.x=128,.y=10,.w=120,.h=600};frame_node.box=&fb;
    struct web_frame f={.owner=&top,.element=&frame_node,.document=&child,.scroll_x=3,.scroll_y=7};
    top.frames=&f;child.frame_parent=&top;child.frame_element=&frame_node;
    check(web_node_rect(&top,&anchor,&x,&y,&w,&h)&&x==133&&y==25&&w==80&&h==32,"native top hit-space includes frame offset and child scroll");
    check(web_node_rect(&child,&anchor,&x,&y,&w,&h)&&x==8&&y==22,"child-local rectangle excludes parent frame offset");
    plain.display=D_NONE;check(!web_node_rect(&child,&anchor,&x,&y,&w,&h),"nonrendered inline rejected");plain.display=D_INLINE;
    anchor.parent=NULL;check(!web_node_rect(&child,&anchor,&x,&y,&w,&h),"detached inline rejected");anchor.parent=&cr;
    f.detached=true;check(!web_node_rect(&top,&anchor,&x,&y,&w,&h),"retired frame generation rejects top geometry");f.detached=false;
    query_hit=&anchor;check(doc_element_at(&child,10,24)==&anchor,"child document hit remains local");
    check(doc_element_at(&top,138,34)==&frame_node,"parent document hit exposes frame not child DOM");
    node_t nested_frame={.type=N_ELEM,.tag=T_iframe,.owner=&child,.parent=&cr};
    struct web_frame nf={.owner=&child,.element=&nested_frame,.document=&nested};child.frames=&nf;
    nested.frame_parent=&child;nested.frame_element=&nested_frame;
    node_t target={.type=N_ELEM,.owner=&nested,.parent=&nr};query_hit=&target;
    check(doc_element_at(&top,140,36)==&frame_node&&doc_element_at(&child,12,10)==&nested_frame,"nested frame hit retargets each receiver boundary");
    nf.document=NULL;check(doc_element_at(&top,140,36)==NULL,"stale child-document association is not exposed");nf.document=&nested;
    nested_frame.parent=NULL;check(doc_element_at(&child,12,10)==NULL,"detached container is not exposed");nested_frame.parent=&cr;
    /* More than 32 rows/columns and deep nested framesets are new boundaries. */
    char tracks[256];for(int i=0;i<64;i++){tracks[i*2]='*';tracks[i*2+1]=i==63?0:',';}
    size_t n=0;struct frameset_track *parsed=frameset_tracks(tracks,640,&n);
    bool equal=n==64;for(size_t i=0;i<n;i++)if(parsed[i].value!=10)equal=false;
    check(equal,"all 64 weighted tracks preserved");
    parsed=frameset_tracks("100,25%,*",400,&n);check(n==3&&parsed[0].value==100&&parsed[1].value==100&&parsed[2].value==200,"pixel/percent/star layout preserved");
    parsed=frameset_tracks("300,300",400,&n);check(n==2&&parsed[0].value==200&&parsed[1].value==200,"oversubscribed fixed tracks shrink proportionally");
    parsed=frameset_tracks("1e999,*,nan",400,&n);check(n==3&&parsed[0].value==0&&parsed[1].value==400&&parsed[2].value==0,"invalid nonfinite input cannot poison viewport dimensions");
    node_t fsn={.type=N_ELEM,.tag=T_frameset};set_attr(&fsn,"cols",tracks);
    box_t fs={.node=&fsn,.w=640};box_t children[64]={0};
    for(int i=0;i<64;i++){if(i<63)children[i].next=&children[i+1];}fs.first=children;
    layout_frameset(&fs,100);check(children[63].x==630&&children[63].w==10&&children[63].h==100,"64th frame has its actual viewport geometry");free(fsn.attrs);
    set_attr(&fsn,"rows",tracks);fs.w=100;layout_frameset(&fs,640);
    check(children[63].y==630&&children[63].w==100&&children[63].h==10,"64th row has its actual viewport geometry");free(fsn.attrs);fsn.attrs=NULL;fsn.nattrs=0;
    size_t depth=2048;box_t *deep=calloc(depth,sizeof *deep);node_t *nodes=calloc(depth,sizeof *nodes);
    for(size_t i=0;i<depth;i++){nodes[i].tag=T_frameset;deep[i].node=&nodes[i];if(i+1<depth)deep[i].first=&deep[i+1];}
    deep[0].w=640;layout_frameset(deep,480);
    check(deep[depth-1].w==640&&deep[depth-1].h==480,"nested frameset traversal does not consume native recursion depth");
    free(nodes);free(deep);free(L.decos.p);ar_free(&top.lmem);
    jmp_buf trap;top.lmem.trap=&trap;fail_malloc=true;bool failed=false;
    if(!setjmp(trap))frameset_tracks("*",640,&n);else failed=true;
    fail_malloc=false;top.lmem.trap=NULL;check(failed,"actual allocator failure uses existing arena trap, not an artificial track cap");ar_free(&top.lmem);
    printf("frame-inline-geometry-native: %u checks, %u failures\n",checks,failures);return failures!=0;
}
