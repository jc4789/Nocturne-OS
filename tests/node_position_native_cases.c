/* Appended to the actual js.c helpers and dom.c root helpers by the runner. */
static int failures,checks;
static void expect_position(const char *name,node_t *a,node_t *b,unsigned expected){
    unsigned actual=compare_node_position(a,b);checks++;
    if(actual!=expected){printf("FAIL %s: %u != %u\n",name,actual,expected);failures++;}
}
int main(void){
    node_t root={.type=N_DOC},element={.type=N_ELEM},left={.type=N_ELEM},right={.type=N_ELEM};
    node_t text={.type=N_TEXT},comment={.type=N_COMMENT},alpha={.type=N_ATTR},beta={.type=N_ATTR};
    struct attr attributes[2]={{.node=&alpha},{.node=&beta}};
    root.first=&element;element.parent=&root;element.first=&left;left.parent=right.parent=&element;
    left.next=&right;right.prev=&left;left.first=&text;text.parent=&left;
    right.first=&comment;comment.parent=&right;
    element.attrs=attributes;element.nattrs=2;alpha.attr_owner=beta.attr_owner=&element;
    expect_position("same",&left,&left,0);
    expect_position("document descendant",&root,&text,20);expect_position("document ancestor",&text,&root,10);
    expect_position("sibling",&left,&right,4);expect_position("reverse sibling",&right,&left,2);
    expect_position("cousin",&text,&comment,4);expect_position("reverse cousin",&comment,&text,2);
    expect_position("same attr",&alpha,&alpha,0);
    expect_position("attr order",&alpha,&beta,36);expect_position("reverse attr order",&beta,&alpha,34);
    expect_position("element attr",&element,&alpha,20);expect_position("attr element",&alpha,&element,10);
    expect_position("attr before descendant",&alpha,&text,4);expect_position("descendant after attr",&text,&alpha,2);
    expect_position("document attr",&root,&alpha,20);expect_position("attr document",&alpha,&root,10);
    node_t gamma={.type=N_ATTR,.attr_owner=&right};
    expect_position("different owner attrs",&alpha,&gamma,4);expect_position("different owner attrs reverse",&gamma,&alpha,2);
    attributes[0].node=&beta;attributes[1].node=&alpha;
    expect_position("native attr reorder",&alpha,&beta,34);expect_position("native attr reorder reverse",&beta,&alpha,36);
    node_t shadow={.type=N_FRAGMENT,.shadow_host=&element},inside={.type=N_ELEM,.parent=&shadow};
    shadow.first=&inside;
    expect_position("shadow internal",&shadow,&inside,20);expect_position("shadow internal reverse",&inside,&shadow,10);
    unsigned disconnected=compare_node_position(&element,&inside);
    checks++;if((disconnected&33)!=33 || ((disconnected&6)!=2 && (disconnected&6)!=4))failures++;
    expect_position("shadow disconnected stable",&element,&inside,disconnected);
    expect_position("shadow disconnected reverse",&inside,&element,disconnected^6);
    node_t template_tree={.type=N_FRAGMENT,.template_host=&element},inert={.type=N_ELEM,.parent=&template_tree};
    template_tree.first=&inert;
    unsigned template_order=compare_node_position(&element,&inert);
    checks++;if((template_order&33)!=33)failures++;
    expect_position("template disconnected reverse",&inert,&element,template_order^6);
    alpha.attr_owner=NULL;
    unsigned attr_order=compare_node_position(&alpha,&beta);
    checks++;if((attr_order&33)!=33)failures++;
    expect_position("ownerless attr stable",&alpha,&beta,attr_order);
    expect_position("ownerless attr reverse",&beta,&alpha,attr_order^6);
    expect_position("ownerless attr same",&alpha,&alpha,0);
    static node_t deep[1024];
    for(unsigned i=1;i<1024;i++){deep[i].parent=&deep[i-1];deep[i-1].first=&deep[i];}
    expect_position("deep tree descendant",&deep[0],&deep[1023],20);
    expect_position("deep tree ancestor",&deep[1023],&deep[0],10);
    printf("native node position: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
