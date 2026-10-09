static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static node_t *walk_nodes[3];static unsigned walk_count;static bool legend;
static void walk(struct pctx *P){
    /* Isolate the paint traversal: feed its hit candidates in actual paint
       order. Selection/reset and final activation are unchanged product C. */
    for(unsigned i=0;i<walk_count;i++){
        node_t *n=walk_nodes[i];if(!hit_style(n->style))continue;
        hit_target(P,n);
        if(node_attr(n,"href"))set_hit(P,WEB_HIT_NONE,NULL,n);
        if(legend && n->tag==T_details)set_disclosure_hit(P,n);
    }
}
int main(void){
    bool wired=false;for(size_t i=0;i<sizeof simple_pcs/sizeof *simple_pcs;i++)if(!strcmp(simple_pcs[i].name,"hover"))wired=simple_pcs[i].pc==PC_HOVER;
    check(wired,"CSS hover is no longer PC_NEVER");
    web_doc top={.live=true},child={.live=true},other={.live=true};
    node_t root={.type=N_DOC,.owner=&top},cr={.type=N_DOC,.owner=&child},orr={.type=N_DOC,.owner=&other};top.root=&root;child.root=&cr;other.root=&orr;
    node_t parent={.type=N_ELEM,.tag=T_div,.parent=&root,.owner=&top},a={.type=N_ELEM,.tag=T_a,.parent=&parent,.owner=&top},sibling={.type=N_ELEM,.tag=T_span,.parent=&parent,.owner=&top};
    doc_hover_update(&top,&a);
    check(css_hovered(&a)&&css_hovered(&parent)&&!css_hovered(&sibling),"native target and flat ancestors, not siblings");
    check(top.hover_leaf==&top&&top.hover_target==&a&&top.need_style&&top.dirty,"native hover invalidates styles");
    check(!top.resources_dirty,"pointer state does not request resource rescans");
    top.dirty=top.need_style=false;doc_hover_update(&top,&a);
    check(!top.dirty&&!top.need_style,"same target does not restyle again");
    doc_hover_update(&top,&sibling);check(css_hovered(&sibling)&&!css_hovered(&a)&&top.need_style,"target transition updates selectors");
    doc_hover_update(&top,NULL);check(!css_hovered(&parent)&&!top.hover_target&&!top.hover_leaf,"pointer exit clears hover");
    node_t frame_node={.type=N_ELEM,.tag=T_iframe,.parent=&root,.owner=&top};
    node_t inside={.type=N_ELEM,.tag=T_div,.parent=&cr,.owner=&child},inside2={.type=N_ELEM,.tag=T_span,.parent=&inside,.owner=&child};
    struct web_frame f={.owner=&top,.element=&frame_node,.document=&child};top.frames=&f;child.frame_parent=&top;child.frame_element=&frame_node;
    doc_hover_update(&top,&inside);
    check(top.hover_leaf==&child&&top.hover_target==&frame_node&&child.hover_target==&inside,"child/native parent-frame CSS hover chain");
    check(css_hovered(&frame_node)&&css_hovered(&inside)&&!css_hovered(&a),"both owning contexts match their own native targets");
    top.dirty=top.need_style=false;child.dirty=child.need_style=false;doc_hover_update(&top,&inside2);
    check(!top.need_style&&child.need_style&&css_hovered(&inside)&&css_hovered(&inside2),"movement within child preserves parent hover without restyle");
    doc_hover_update(&top,&a);check(!child.hover_target&&css_hovered(&a)&&!css_hovered(&frame_node),"leaving child clears its state");
    f.detached=true;doc_hover_update(&top,&inside);check(!top.hover_target&&!child.hover_target,"detached child context is rejected");f.detached=false;
    f.document=NULL;doc_hover_update(&top,&inside);check(!top.hover_target&&!child.hover_target,"stale frame generation is rejected");f.document=&child;
    frame_node.parent=NULL;doc_hover_update(&top,&inside);check(!top.hover_target&&!child.hover_target,"disconnected frame container rejected");frame_node.parent=&root;
    doc_hover_update(&top,&inside);child.live=false;
    check(!css_hovered(&inside),"retired context cannot continue matching CSS hover");doc_hover_update(&top,&inside);
    check(!top.hover_target&&!child.hover_target,"retired chain cleared without reactivating realm");child.live=true;
    node_t foreign={.type=N_ELEM,.owner=&other,.parent=&orr};doc_hover_update(&top,&foreign);
    check(!top.hover_target&&!other.hover_target,"unrelated document cannot supply hover target");
    inert_node=&a;doc_hover_update(&top,&a);check(!top.hover_target,"inert hit target rejected");inert_node=NULL;
    node_t host={.type=N_ELEM,.tag=T_div,.owner=&top,.parent=&root},shadow={.type=N_FRAGMENT,.owner=&top,.shadow_host=&host,.shadow_manual=true},slot={.type=N_ELEM,.tag=T_slot,.owner=&top,.parent=&shadow};
    host.shadow_root=&shadow;node_t light={.type=N_ELEM,.tag=T_span,.owner=&top,.parent=&host,.manual_slot=&slot};
    doc_hover_update(&top,&light);check(css_hovered(&light)&&css_hovered(&slot)&&css_hovered(&host),"assigned slot and shadow host use real flat-tree ancestry");
    light.parent=NULL;check(!css_hovered(&light)&&!css_hovered(&host),"detached retained target does not leak hover match");doc_hover_update(&top,NULL);
    /* The GitHub-like occlusion failure: action from an older layer survives
       while the current target is an unrelated ordinary overlay. */
    style_t auto_style={0},none_style={.pointer_events=1},hidden={.visibility=1};
    struct attr href={.raw="href",.value="https://example.test/packages"};a.attrs=&href;a.nattrs=1;a.style=&auto_style;sibling.style=&auto_style;
    walk_nodes[0]=&a;walk_nodes[1]=&sibling;walk_count=2;
    struct web_hit hit={0};check(!web_hit_test(&top,1,1,&hit)&&!hit.href&&hit.kind==WEB_HIT_NONE,"noninteractive overlay cancels underlying link activation");
    sibling.style=&none_style;check(web_hit_test(&top,1,1,&hit)&&hit.node==&a&&hit.href,"pointer-events none overlay correctly passes through");
    sibling.style=&hidden;check(web_hit_test(&top,1,1,&hit)&&hit.node==&a,"hidden overlay correctly passes through");
    sibling.style=&auto_style;auto_style.opacity=0;
    check(!web_hit_test(&top,1,1,&hit)&&!hit.href,"transparent auto overlay still hit-tests");
    sibling.parent=&a;check(web_hit_test(&top,1,1,&hit)&&hit.node==&a,"ordinary descendant resolves its true ancestor link");sibling.parent=&parent;
    struct web_hit pending={.kind=WEB_HIT_LINK,.node=&a,.href=href.value};struct pctx P={.hit=&pending,.target=&a,.hit_any=true};hit_target(&P,&sibling);
    check(P.target==&sibling&&!P.hit_any&&!pending.href&&pending.kind==WEB_HIT_NONE,"every native target update clears stale action state");
    node_t details={.type=N_ELEM,.tag=T_details,.owner=&top,.parent=&root,.style=&auto_style};walk_nodes[0]=&details;walk_count=1;legend=false;
    check(!web_hit_test(&top,1,1,&hit)&&hit.kind==WEB_HIT_NONE,"default details body is not an invented legend hit");
    legend=true;check(web_hit_test(&top,1,1,&hit)&&hit.kind==WEB_HIT_DETAILS&&hit.node==&details,"actual renderer legend action retained");
    walk_nodes[1]=&sibling;walk_count=2;check(!web_hit_test(&top,1,1,&hit)&&hit.kind==WEB_HIT_NONE,"ordinary overlay also cancels stale disclosure action");
    printf("native-hover-overlay17: %u checks, %u failures\n",checks,failures);return failures!=0;
}
