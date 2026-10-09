static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
int main(void){
    struct attr href={.raw="href",.value="https://example.test/real-link"},empty={.raw="href",.value=""};
    node_t root={.type=N_DOC},outer={.type=N_ELEM,.tag=T_a,.parent=&root,.attrs=&href,.nattrs=1},near={.type=N_ELEM,.tag=T_a,.parent=&outer,.attrs=&href,.nattrs=1},leaf={.type=N_ELEM,.tag=T_div,.parent=&near};
    box_t b={.kind=B_BLOCK,.node=&leaf};
    check(inline_link_ancestor(&b)==&near,"closest DOM link wins flat ancestry order");
    b.node=&near;parent_walks=0;check(inline_link_ancestor(&b)==&near&&!parent_walks,"own anchor resolves without ancestor work");
    near.nattrs=0;b.node=&leaf;check(inline_link_ancestor(&b)==&outer,"anchor without href does not shadow real outer anchor");
    near.attrs=&empty;near.nattrs=1;check(inline_link_ancestor(&b)==&near,"empty href is still a real native anchor");near.attrs=&href;
    box_t native={.kind=B_BLOCK,.node=&leaf},an2={.kind=B_BLOCK,.anon=true,.parent=&native},an1={.kind=B_BLOCK,.anon=true,.parent=&an2};
    check(inline_link_ancestor(&an1)==&near,"anonymous box resolves closest native box's flat DOM ancestry");
    /* The contents anchor has no box: its descendant's real DOM chain is
       authoritative rather than the rendered parent (outside that anchor). */
    style_t contents={.display=D_CONTENTS};near.style=&contents;
    box_t rendered_outer={.kind=B_BLOCK,.node=&outer};native.parent=&rendered_outer;
    check(inline_link_ancestor(&native)==&near,"display contents anchor omitted from box tree remains nearest link");
    check(inline_link_ancestor(&an1)==&near,"anonymous descendants keep omitted contents ancestor");
    node_t sibling={.type=N_ELEM,.tag=T_a,.attrs=&href,.nattrs=1,.parent=&root},plain={.type=N_ELEM,.tag=T_div,.parent=&root};
    box_t plain_box={.kind=B_BLOCK,.node=&plain};check(!inline_link_ancestor(&plain_box),"unrelated sibling anchor is never inherited");
    box_t paint_parent={.kind=B_BLOCK,.node=&sibling};native.parent=&paint_parent;
    check(inline_link_ancestor(&native)==&near,"top-layer paint reparenting cannot override real DOM ancestry");
    node_t host={.type=N_ELEM,.tag=T_div,.parent=&outer},shadow={.type=N_FRAGMENT,.shadow_host=&host,.shadow_manual=true},slot={.type=N_ELEM,.tag=T_slot,.parent=&shadow};
    host.shadow_root=&shadow;
    node_t light={.type=N_ELEM,.tag=T_div,.parent=&host,.manual_slot=&slot};box_t slotted={.kind=B_BLOCK,.node=&light};
    check(inline_link_ancestor(&slotted)==&outer,"assigned slot and shadow host lead to outer native link");
    node_t shadow_link={.type=N_ELEM,.tag=T_a,.attrs=&href,.nattrs=1,.parent=&shadow},shadow_child={.type=N_ELEM,.tag=T_div,.parent=&shadow_link};box_t shadow_box={.kind=B_BLOCK,.node=&shadow_child};
    check(inline_link_ancestor(&shadow_box)==&shadow_link,"shadow-local link outranks host ancestor");
    shadow_link.nattrs=0;check(inline_link_ancestor(&shadow_box)==&outer,"closed shadow host ancestry remains native not author-proxied");
    node_t orphan={.type=N_ELEM,.tag=T_div};box_t orphan_box={.kind=B_BLOCK,.node=&orphan,.parent=&paint_parent};
    check(!inline_link_ancestor(&orphan_box),"paint-only link does not fabricate an orphan's DOM ancestor");
    box_t empty_box={.kind=B_BLOCK};parent_walks=0;check(!inline_link_ancestor(&empty_box)&&!parent_walks,"native-node-free tree needs no DOM walk");
    enum{DEPTH=257};node_t *nodes=calloc(DEPTH,sizeof *nodes);box_t *boxes=calloc(DEPTH,sizeof *boxes);
    if(!nodes||!boxes){printf("native allocation failed\n");free(nodes);free(boxes);return 1;}
    for(int i=0;i<DEPTH;i++){nodes[i].type=N_ELEM;nodes[i].tag=T_div;nodes[i].parent=i?&nodes[i-1]:&root;boxes[i].kind=B_BLOCK;boxes[i].node=&nodes[i];boxes[i].parent=i?&boxes[i-1]:NULL;}
    parent_walks=0;check(!inline_link_ancestor(&boxes[DEPTH-1]),"deep native forest with no anchor resolves to none");
    check(parent_walks==DEPTH+1,"no-anchor work is exactly one flat walk, not per-box rescans");
    boxes[DEPTH-1].node=NULL;parent_walks=0;check(!inline_link_ancestor(&boxes[DEPTH-1])&&parent_walks==DEPTH,"deep anonymous leaf uses one nearest-native walk");
    nodes[100].tag=T_a;nodes[100].attrs=&href;nodes[100].nattrs=1;
    boxes[DEPTH-1].node=&nodes[DEPTH-1];parent_walks=0;check(inline_link_ancestor(&boxes[DEPTH-1])==&nodes[100]&&parent_walks==DEPTH-1-100,"deep link stops exactly at closest native ancestor");
    free(nodes);free(boxes);
    printf("inline-link19: %u checks, %u failures\n",checks,failures);return failures!=0;
}
