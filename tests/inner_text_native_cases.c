static int checks, failures;
static void verify(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static void content(struct inner_text_buffer *b,const char *want,const char *name){
    verify(!b->failed&&b->n==strlen(want)&&(!b->n||!memcmp(b->p,want,b->n)),name);
    free(b->p);*b=(struct inner_text_buffer){0};
}
int main(void){
    struct inner_text_buffer b={0};
    style_t normal={.display=D_INLINE,.white_space=WS_NORMAL},pre={.white_space=WS_PRE},hidden={.visibility=1};
    box_t text={.kind=B_TEXT,.st=&normal,.text=" a  b ",.len=6};
    inner_text_box(&b,&text);content(&b,"a b","collapse-trim");
    text.text="a ";text.len=2;inner_text_box(&b,&text);inner_text_required(&b,0);
    text.text=" b";inner_text_box(&b,&text);content(&b,"a b","inline-pending-space");
    text.st=&pre;text.text=" a  b\n c ";text.len=9;inner_text_box(&b,&text);content(&b," a  b\n c ","pre-preserve");
    text.st=&hidden;inner_text_box(&b,&text);content(&b,"","hidden-box");
    inner_text_required(&b,2);inner_text_string(&b,"a",1);inner_text_required(&b,2);content(&b,"a","outer-required-trim");
    inner_text_string(&b,"a",1);inner_text_required(&b,1);inner_text_required(&b,2);inner_text_required(&b,1);
    inner_text_string(&b,"b",1);content(&b,"a\n\nb","required-max");
    inner_text_string(&b,"\n",1);inner_text_string(&b,"a",1);inner_text_string(&b,"\n",1);content(&b,"\na\n","literal-lf-not-trimmed");
    style_t block={.display=D_BLOCK},paragraph={.display=D_BLOCK};
    box_t divbox={.kind=B_BLOCK,.st=&block},pbox={.kind=B_BLOCK,.st=&paragraph};
    node_t div={.type=N_ELEM,.tag=T_div,.style=&block,.box=&divbox};
    node_t p={.type=N_ELEM,.tag=T_p,.style=&paragraph,.box=&pbox};
    verify(inner_text_boundary(&div)==1,"block-boundary");verify(inner_text_boundary(&p)==2,"p-boundary");
    p.box=NULL;verify(!inner_text_boundary(&p),"nonrendered-p-no-boundary");
    node_t parent={.type=N_ELEM},a={.type=N_TEXT,.text="a",.textlen=1},br={.type=N_ELEM,.tag=T_br},c={.type=N_TEXT,.text="b",.textlen=1};
    box_t ab={.kind=B_TEXT,.st=&normal,.text="a",.len=1},bb={.kind=B_BR,.st=&normal},cb={.kind=B_TEXT,.st=&normal,.text="b",.len=1};
    parent.first=&a;parent.last=&c;a.parent=br.parent=c.parent=&parent;a.next=&br;br.next=&c;a.box=&ab;br.box=&bb;br.style=&normal;c.box=&cb;
    inner_text_collect(&b,&parent,true);content(&b,"a\nb","native-tree-br");
    inner_text_collect(&b,&parent,false);content(&b,"ab","nonrendered-descendant-fallback");
    a.box=NULL;inner_text_collect(&b,&parent,true);content(&b,"\nb","nonrendered-descendant-exclusion");
    a.box=&ab;ab.st=&hidden;inner_text_collect(&b,&parent,true);content(&b,"\nb","visibility-text-exclusion");ab.st=&normal;
    box_t table={.kind=B_TABLE},group1={.kind=B_ROW_GROUP},group2={.kind=B_ROW_GROUP},row1={.kind=B_ROW},row2={.kind=B_ROW},cell1={.kind=B_CELL},cell2={.kind=B_CELL};
    group1.parent=group2.parent=&table;group1.next=&group2;group1.first=&row1;group2.first=&row2;row1.parent=&group1;row2.parent=&group2;
    cell1.parent=cell2.parent=&row1;cell1.next=&cell2;
    verify(inner_text_following_table_box(&cell1,B_CELL),"cell-next");verify(!inner_text_following_table_box(&cell2,B_CELL),"cell-last");
    verify(inner_text_following_table_box(&row1,B_ROW),"row-next-group");verify(!inner_text_following_table_box(&row2,B_ROW),"row-last");
    node_t *deep=malloc(sizeof(node_t)*1025);memset(deep,0,sizeof(node_t)*1025);
    for(int i=0;i<1024;i++){deep[i].type=N_ELEM;deep[i].first=&deep[i+1];deep[i+1].parent=&deep[i];}
    deep[1024].type=N_TEXT;deep[1024].text="deep";deep[1024].textlen=4;
    inner_text_collect(&b,deep,false);content(&b,"deep","depth1024-no-recursion");free(deep);
    char *large=malloc(INNER_TEXT_MAX);memset(large,'x',INNER_TEXT_MAX);
    verify(inner_text_put(&b,large,INNER_TEXT_MAX),"buffer-exact-limit");verify(!inner_text_put(&b,"x",1)&&b.failed&&range_errors==1,"buffer-overflow-explicit-error");
    verify(b.n==INNER_TEXT_MAX,"buffer-overflow-no-truncation-success");free(b.p);free(large);b=(struct inner_text_buffer){0};
    b.visits=INNER_TEXT_VISITS-1;verify(inner_text_visit(&b),"visit-exact-limit");verify(!inner_text_visit(&b)&&b.failed&&range_errors==2,"visit-overflow-explicit-error");
    printf("inner text native: %d checks, %d failed\n",checks,failures);return failures!=0;
}
