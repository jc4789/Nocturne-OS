static int checks, failures;
static void check(bool value,const char *name){checks++;if(!value){failures++;printf("FAIL %s\n",name);}}
static void clear_doc(web_doc *d){
    for(int i=0;i<d->css_cache.n;i++){struct cached_css*c=d->css_cache.v[i];free(c->url);free(c->base);free(c->body);free(c);}
    for(int i=0;i<d->pending_css.n;i++){struct pending*p=d->pending_css.v[i];free(p->url);free(p->media);free(p);}
    pv_free(&d->css_cache);pv_free(&d->pending_css);pv_free(&d->sty.sheets);ar_free(&d->cssmem);ar_free(&d->mem);memset(d,0,sizeof*d);
}
static struct cached_css *cache(web_doc*d,const char*url,const char*base,const char*body){
    struct cached_css*c=calloc(1,sizeof*c);c->url=strdup(url);c->base=strdup(base?base:url);c->body=strdup(body);c->n=strlen(body);c->done=true;pv_push(&d->css_cache,c);return c;
}
static void imports_cases(void){
    web_doc d={.live=true};node_t scope={0};
    enum{COUNT=1024};
    for(int i=0;i<COUNT;i++){char url[96],body[160];snprintf(url,sizeof url,"https://css.test/%d.css",i);
        if(i+1<COUNT)snprintf(body,sizeof body,"@import \"https://css.test/%d.css\";",i+1);
        else snprintf(body,sizeof body,".last{width:123px}");cache(&d,url,NULL,body);}
    struct cached_css*c=d.css_cache.v[0];
    add_sheet(&d,c->body,c->n,c->base,8,"screen",&scope);
    check(d.sty.sheets.n==COUNT,"1024 cached import sheets beyond old16");
    check(d.pending_css.n==0 && reports==0,"deep cached imports complete without queue/error");
    check(((sheet_t*)d.sty.sheets.v[COUNT-1])->scope==&scope,"deep scope preserved");
    check(((sheet_t*)d.sty.sheets.v[COUNT-1])->order==8 && ((sheet_t*)d.sty.sheets.v[0])->order==8,"deep import postorder stays within root order key");
    clear_doc(&d);d.live=true;
    c=cache(&d,"https://css.test/a.css",NULL,"@import \"https://css.test/b.css\";@import \"https://css.test/b.css\";");
    cache(&d,"https://css.test/b.css",NULL,"@import \"https://css.test/a.css#cycle\";");
    add_sheet(&d,c->body,c->n,c->base,4,NULL,NULL);
    check(d.sty.sheets.n==3 && d.pending_css.n==0,"cycle cut but repeated sibling import retained");
    clear_doc(&d);d.live=true;
    c=cache(&d,"https://css.test/a.css",NULL,"@import \"https://css.test/alias.css\";");
    cache(&d,"https://css.test/alias.css","https://css.test/a.css#redirect",".x{color:red}");
    add_sheet(&d,c->body,c->n,c->base,1,NULL,NULL);
    check(d.sty.sheets.n==1,"redirect final URL active ancestor cycle");
    clear_doc(&d);d.live=true;
    cache(&d,"https://css.test/document",NULL,".x{color:red}");
    const char*inline_css="@import \"https://css.test/document\";";
    add_sheet(&d,inline_css,strlen(inline_css),"https://css.test/document",1,NULL,NULL);
    check(d.sty.sheets.n==2,"inline resolution base is not imported ancestor");
    clear_doc(&d);d.live=true;
    sbuf many={0};for(int i=0;i<200;i++){char item[100];snprintf(item,sizeof item,"@import \"https://css.test/missing%d.css\";",i);sb_puts(&many,item);}
    add_sheet(&d,many.p,many.n,"https://css.test/root.css",1,NULL,NULL);sb_free(&many);
    check(d.pending_css.n==200,"pending import count beyond old64/128");
    clear_doc(&d);d.live=true;
    reports=0;fail_calloc=1;add_sheet(&d,"",0,"https://css.test/root.css",1,NULL,NULL);
    check(reports==1 && d.sty.sheets.n==0,"root traversal OOM reported without publication");
    clear_doc(&d);d.live=true;
    c=cache(&d,"https://css.test/a.css",NULL,"@import \"https://css.test/b.css\";");cache(&d,"https://css.test/b.css",NULL,".x{color:red}");
    reports=0;fail_calloc=2;add_sheet(&d,c->body,c->n,c->base,1,NULL,NULL);
    check(reports==1 && d.sty.sheets.n==0 && !d.cssmem.trap,"child traversal OOM cleans trap without publishing incomplete parent");
    clear_doc(&d);d.live=true;d.cssmem.limit=1;reports=0;
    add_sheet(&d,".x{color:red}",13,"https://css.test/root.css",1,NULL,NULL);
    check(reports==1 && !d.cssmem.trap && d.sty.sheets.n==0,"arena longjmp restores trap and frees traversal");
    clear_doc(&d);d.live=true;d.cssmem.limit=1;
    jmp_buf outer;d.cssmem.trap=&outer;
    int trapped=setjmp(outer);
    if(!trapped)add_sheet(&d,".x{color:red}",13,"https://css.test/root.css",1,NULL,NULL);
    check(trapped!=0 && d.cssmem.trap==&outer,"outer arena failure propagated after traversal cleanup");
    clear_doc(&d);fail_calloc=0;
}
static void elem(node_t*n,const char*name,int tag){memset(n,0,sizeof*n);n->type=N_ELEM;n->name=n->raw_name=(char*)name;n->tag=tag;n->foreign=true;}
static void attr(node_t*n,struct attr*a,const char*name,const char*value){memset(a,0,sizeof*a);a->name=a->raw=a->local=(char*)name;a->value=(char*)value;n->attrs=a;n->nattrs=1;}
static void svg_cases(void){
    enum{DEPTH=2048};node_t*nodes=calloc(DEPTH+2,sizeof*nodes);node_t doc={.type=N_DOC};
    for(int i=0;i<=DEPTH;i++){elem(&nodes[i],i?"g":"svg",i?T_UNKNOWN:T_svg);nodes[i].parent=i?&nodes[i-1]:&doc;if(i)nodes[i-1].first=&nodes[i];}
    doc.first=nodes;nodes[DEPTH+1].type=N_TEXT;nodes[DEPTH+1].text="deep&leaf";nodes[DEPTH+1].textlen=9;nodes[DEPTH+1].parent=&nodes[DEPTH];nodes[DEPTH].first=&nodes[DEPTH+1];
    sbuf b={0};check(svg_ser(NULL,&b,nodes,"#123456"),"SVG DOM2048 serialization completes");
    sb_cstr(&b);check(strstr(b.p,"deep&amp;leaf")!=NULL,"SVG deep content not old64 truncation");
    check(strstr(b.p,"xmlns=\"http://www.w3.org/2000/svg\"")!=NULL,"SVG root namespace retained");sb_free(&b);free(nodes);
    enum{USES=160};node_t svg,use;elem(&svg,"svg",T_svg);elem(&use,"use",T_UNKNOWN);doc.first=&svg;svg.parent=&doc;svg.first=&use;use.parent=&svg;
    struct attr initial;attr(&use,&initial,"href","#u0");
    node_t*symbols=calloc(USES,sizeof*symbols);node_t*refs=calloc(USES,sizeof*refs);struct attr*attrs=calloc(USES,sizeof*attrs);
    char (*ids)[16]=calloc(USES,sizeof*ids),(*hrefs)[20]=calloc(USES,sizeof*hrefs);
    svg.next=symbols;
    for(int i=0;i<USES;i++){elem(&symbols[i],"symbol",T_UNKNOWN);snprintf(ids[i],sizeof ids[i],"u%d",i);symbols[i].id=ids[i];symbols[i].parent=&doc;symbols[i].next=i+1<USES?&symbols[i+1]:NULL;
        symbols[i].first=&refs[i];refs[i].parent=&symbols[i];
        if(i+1<USES){elem(&refs[i],"use",T_UNKNOWN);refs[i].parent=&symbols[i];snprintf(hrefs[i],sizeof hrefs[i],"#u%d",i+1);attr(&refs[i],&attrs[i],"href",hrefs[i]);}
        else{refs[i].type=N_TEXT;refs[i].text="use-leaf";refs[i].textlen=8;}}
    check(svg_ser(NULL,&b,&svg,"#123456"),"SVG use160 chain completes without C recursion");sb_cstr(&b);
    check(strstr(b.p,"use-leaf")!=NULL,"SVG use160 leaf beyond old32 retained");sb_free(&b);
    refs[USES-1].type=N_ELEM;refs[USES-1].name=refs[USES-1].raw_name="use";attr(&refs[USES-1],&attrs[USES-1],"href","#u0");
    check(svg_ser(NULL,&b,&svg,"#123456"),"SVG indirect use cycle terminates");sb_cstr(&b);check(b.n<10000,"SVG cycle does not infinitely expand");sb_free(&b);
    initial.value="#u0";fail_calloc=2;check(!svg_ser(NULL,&b,&svg,"#123456"),"SVG use stack OOM refuses partial cache publication");sb_free(&b);fail_calloc=0;
    free(symbols);free(refs);free(attrs);free(ids);free(hrefs);
    svg.first=&use;svg.id="self";initial.value="#self";
    check(svg_ser(NULL,&b,&svg,"#123456"),"SVG real ancestor use is rejected");sb_cstr(&b);check(strstr(b.p,"<g")==NULL,"SVG ancestor cycle produces no cloned group");sb_free(&b);
}
static void margin_cases(void){
    enum{DEPTH=4096};box_t*boxes=calloc(DEPTH+1,sizeof*boxes);style_t*styles=calloc(DEPTH+1,sizeof*styles);
    for(int i=0;i<=DEPTH;i++){boxes[i].kind=B_BLOCK;boxes[i].st=&styles[i];styles[i].overflow=OV_VISIBLE;styles[i].display=D_BLOCK;styles[i].margin[0].kind=LK_LEN;
        if(i){boxes[i].parent=&boxes[i-1];boxes[i-1].first=&boxes[i];}}
    styles[DEPTH].margin[0].px=90;check(top_chain(&boxes[1],800)==90,"margin first-child4096 beyond old32");
    styles[10].margin[0].px=30;styles[30].margin[0].px=-20;check(top_chain(&boxes[1],800)==70,"mixed deep margins positive max plus negative min");
    styles[16].border_width[0]=1;check(top_chain(&boxes[1],800)==30,"border stops collapsing chain before deep descendant");
    styles[16].border_width[0]=0;boxes[20].floated=true;check(top_chain(&boxes[1],800)==30,"floating child excluded from inflow margin chain");
    free(boxes);free(styles);
}
int main(void){imports_cases();svg_cases();margin_cases();printf("css-depth-native: %d checks, %d failures\n",checks,failures);return failures!=0;}
