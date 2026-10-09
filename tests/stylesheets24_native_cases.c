static bool evaluate(JSContext*ctx,const char*text,const char*name){
    JSValue v=JS_Eval(ctx,text,strlen(text),name,JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(v)){JSValue e=JS_GetException(ctx);const char*m=JS_ToCString(ctx,e);printf("EXCEPTION %s: %s\n",name,m?m:"unknown");JS_FreeCString(ctx,m);JS_FreeValue(ctx,e);failures++;JS_FreeValue(ctx,v);return false;}
    JS_FreeValue(ctx,v);return true;
}
static char*readfile(const char*path){FILE*f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char*s=malloc((size_t)n+1);if(!s){fclose(f);return NULL;}fread(s,1,(size_t)n,f);s[n]=0;fclose(f);return s;}
static void child(int parent,int id){node_t*p=&nodes[parent],*n=&nodes[id];n->parent=p;n->prev=p->last;if(p->last)p->last->next=n;else p->first=n;p->last=n;}
int main(void){
    JSRuntime*rt=JS_NewRuntime();if(!rt)return 2;JS_NewClassID(&node_class);JSClassDef cls={.class_name="NativeFixtureNode"};JS_NewClass(rt,node_class,&cls);
    const int tags[NODE_COUNT]={0,T_style,0,T_link,T_link,T_link,T_div,0,T_style,0,T_template,T_style,0,0,T_style,0,T_link,T_a};
    for(int i=0;i<NODE_COUNT;i++){nodes[i].type=N_ELEM;nodes[i].tag=tags[i];nodes[i].owner=&docs[0];}
    nodes[0].type=nodes[13].type=N_DOC;nodes[13].owner=&docs[1];nodes[2].type=nodes[9].type=nodes[12].type=nodes[15].type=N_TEXT;
    nodes[7].type=N_FRAGMENT;nodes[7].shadow_host=&nodes[6];nodes[6].shadow_root=&nodes[7];
    nodes[2].text=".inline {width:1px}";nodes[9].text=".shadow{}";nodes[12].text=".template{}";nodes[15].text=".second{}";
    for(int i=0;i<NODE_COUNT;i++)if(nodes[i].text)nodes[i].textlen=strlen(nodes[i].text);
    docs[0].root=&nodes[0];docs[1].root=&nodes[13];docs[0].url="https://parent.test/page";docs[1].url="about:blank";docs[1].inherited_url="https://parent.test/page";
    for(int i=0;i<2;i++){docs[i].live=true;docs[i].dom_revision=1;strcpy(docs[i].base,"https://parent.test/page");}
    child(0,1);child(1,2);child(0,3);child(0,4);child(0,5);child(0,6);child(7,8);child(8,9);child(0,10);child(10,11);child(11,12);child(0,14);child(14,15);child(0,16);child(0,17);
    attr(3,"rel","stylesheet");attr(3,"href","/a.css");attr(4,"rel","stylesheet");attr(4,"href","/pending.css");
    attr(5,"rel","stylesheet");attr(5,"href","https://cross.test/x.css");attr(16,"rel","stylesheet");attr(16,"href","data:text/css,.data%7Bcolor:blue%7D");
    struct cached_css cache[]={
        {.url="https://parent.test/a.css",.base="https://parent.test/a.css",.body=".a {color:red}",.n=14,.done=true},
        {.url="https://parent.test/pending.css",.base="https://parent.test/pending.css"},
        {.url="https://cross.test/x.css",.base="https://cross.test/x.css",.body=".secret{color:red}",.n=18,.done=true},
        {.url="https://parent.test/b.css",.base="https://parent.test/b.css",.body=".b{width:4px}",.n=13,.done=true}
    };
    /* Actual spans, not the initializer's hand-counted byte lengths. */
    for(int i=0;i<4;i++){if(cache[i].body)cache[i].n=strlen(cache[i].body);pv_push(&docs[0].css_cache,&cache[i]);}
    char*boot=readfile("build/goal-20261009/stylesheets24/bindings.js"),*cases=readfile("build/goal-20261009/stylesheets24/cases.js");if(!boot||!cases)return 2;
    for(int i=0;i<2;i++){
        struct fixture_state*f=&contexts[i];f->s.ctx=JS_NewContext(rt);f->s.doc=&docs[i];docs[i].js=&f->s;
        for(int j=0;j<NODE_COUNT;j++)f->wrappers[j]=JS_UNDEFINED;
        JS_SetContextOpaque(f->s.ctx,f);JSValue g=JS_GetGlobalObject(f->s.ctx);
        JS_SetPropertyStr(f->s.ctx,g,"fixtureDom",JS_NewCFunction(f->s.ctx,fixture,"fixtureDom",3));
        JS_SetPropertyStr(f->s.ctx,g,"fixtureRaw",JS_NewCFunction(f->s.ctx,raw,"fixtureRaw",6));
        JS_SetPropertyStr(f->s.ctx,g,"check",JS_NewCFunction(f->s.ctx,jscheck,"check",2));JS_FreeValue(f->s.ctx,g);
        evaluate(f->s.ctx,boot,"stylesheet-native-bindings24");
    }
    evaluate(contexts[0].s.ctx,cases,"stylesheet-list-link24-new");
    JSValue g0=JS_GetGlobalObject(contexts[0].s.ctx),old=JS_GetPropertyStr(contexts[0].s.ctx,g0,"savedSheet");
    JSValue g1=JS_GetGlobalObject(contexts[1].s.ctx);JS_SetPropertyStr(contexts[1].s.ctx,g1,"parentSheet",old);JS_FreeValue(contexts[1].s.ctx,g1);JS_FreeValue(contexts[0].s.ctx,g0);
    evaluate(contexts[1].s.ctx,"const parentDoc=fixtureDom('parentDoc');const ownSheet=Document.prototype.__lookupGetter__('styleSheets').call(parentDoc)[0];check(ownSheet instanceof CSSStyleSheet&&ownSheet!==parentSheet,'cross-realm sheet wrapper uses caller CSSStyleSheet prototype');check(parentDoc.styleSheets===parentDoc.styleSheets,'cross-realm Document list identity is stable');check(ownSheet.ownerNode===parentDoc.styleSheets[0].ownerNode,'cross-realm node identity reused');fixtureDom('retire');let denied=false;try{parentDoc.styleSheets.length}catch(e){denied=e.name==='InvalidStateError'}check(denied,'retired document cannot expose native sheet list');fixtureDom('restore');","stylesheet-realm24-new");
    /* New owned link getter boundary, independently of stylesheet list cases. */
    size_t n=20000;char*relative=malloc(n+1);relative[0]='/';memset(relative+1,'x',n-1);relative[n]=0;attr(17,"href",relative);
    const char*resolved=doc_link_href(&docs[1],&nodes[17]);check(resolved&&strlen(resolved)==strlen("https://parent.test")+n,"native link getter retains a 20000-byte href without the old16KiB prefix");
    char*absolute=doc_absolute_url(docs[0].base,relative);check(absolute&&resolved&&!strcmp(absolute,resolved),"owned stylesheet absolute resolver agrees with the real link getter");free(absolute);free(relative);
    attr(17,"href","javascript:alert(1)");check(doc_link_href(&docs[0],&nodes[17])==NULL,"owned link getter keeps javascript navigation rejection");
    for(int i=0;i<2;i++){
        for(int j=0;j<NODE_COUNT;j++)JS_FreeValue(contexts[i].s.ctx,contexts[i].wrappers[j]);
        JS_FreeContext(contexts[i].s.ctx);
    }
    JS_FreeRuntime(rt);for(int i=0;i<NODE_COUNT;i++)cssom_style_free(&nodes[i]);
    pv_free(&docs[0].css_cache);free(docs[0].resolved_link);free(boot);free(cases);
    printf("stylesheets24-native: %d checks, %d failures; native sync calls %d\n",checks,failures,sync_calls);return failures!=0;
}
