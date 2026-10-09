/* Actual parser, CSSOM mutation and doc add_sheet_ast; no visible-site assertion. */
static unsigned checks,failed;
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL CSSOM:%d %s\n",__LINE__,#c);}}while(0)
static uint32_t count(struct cssom_sheet*s){uint32_t n;css_sheet_rules(s->ast,&n);return n;}
static struct css_rule_info *rule(struct cssom_sheet*s,unsigned i){struct css_rule_info*r=css_sheet_rules(s->ast,NULL);while(r&&i--)r=r->next;return r;}
static void clear_document(web_doc*d){css_styling_free(&d->sty);ar_free(&d->cssmem);for(int i=0;i<d->pending_css.n;i++){struct pending*p=d->pending_css.v[i];free(p->url);free(p->media);free(p);}pv_free(&d->pending_css);}
int main(void){
    node_t root={.type=N_DOC},style={.type=N_ELEM,.tag=T_style,.parent=&root},text={.type=N_TEXT,.parent=&style,.text=".first{color:red}",.textlen=17};
    web_doc d={.live=true,.root=&root};style.owner=root.owner=text.owner=&d;style.first=style.last=&text;root.first=root.last=&style;strcpy(d.base,"https://cssom.test/index.html");
    int error;struct cssom_sheet*s=cssom_style_sheet(&d,&style,&error);
    CHECK(s&&error==CSSOM_OK&&count(s)==1&&s->owner==&style&&s->document==&d);
    CHECK(cssom_style_sheet(&d,&style,&error)==s&&style.cssom_serial==1);
    uint32_t first_id=rule(s,0)->id;
    CHECK(rule(s,0)->native_rule&&((struct rule*)rule(s,0)->native_rule)->ndecls==1&&!strcmp(((struct rule*)rule(s,0)->native_rule)->decls[0].value,"red"));
    const char *insert=".insert{color:blue !important}";
    CHECK(cssom_insert(s,insert,strlen(insert),1)==CSSOM_OK&&count(s)==2);
    CHECK(rule(s,0)->id==first_id&&rule(s,1)->id!=first_id&&rule(s,1)->type==1);
    CHECK(((struct rule*)rule(s,1)->native_rule)->decls[0].important&&!strcmp(((struct rule*)rule(s,1)->native_rule)->decls[0].value,"blue"));
    CHECK(text.textlen==17&&!strcmp(text.text,".first{color:red}")&&strstr(s->source,".insert"));
    node_t shadow={.type=N_FRAGMENT};add_sheet_ast(&d,s->source,s->length,s->base,3,NULL,&shadow,s->ast);
    CHECK(d.sty.sheets.n==1&&css_sheet_rules(d.sty.sheets.v[0],NULL)==css_sheet_rules(s->ast,NULL)&&((sheet_t*)d.sty.sheets.v[0])->scope==&shadow&&((sheet_t*)d.sty.sheets.v[0])->order==3);
    const char *front=".front{width:7px}";
    CHECK(cssom_insert(s,front,strlen(front),0)==CSSOM_OK&&d.sty.sheets.n==0&&d.resources_dirty&&d.need_style);
    CHECK(rule(s,1)->id==first_id&&count(s)==3);
    CHECK(cssom_delete(s,1)==CSSOM_OK&&count(s)==2&&rule(s,1)->id!=first_id);
    CHECK(s->removed&&s->removed->id==first_id&&s->removed->type==1&&!strcmp(s->removed->text,".first{color:red}"));
    char *saved=strdup(s->source);uint32_t next=s->next_rule_id;
    CHECK(cssom_insert(s,".bad{width:1px",14,0)==CSSOM_SYNTAX&&!strcmp(saved,s->source)&&s->next_rule_id==next);
    CHECK(cssom_insert(s,".a{} .b{}",9,0)==CSSOM_SYNTAX&&count(s)==2);
    CHECK(cssom_insert(s,"@import 'https://cross.test/a.css';",34,0)==CSSOM_UNSUPPORTED&&!strcmp(saved,s->source));
    CHECK(cssom_insert(s,".b{}",4,3)==CSSOM_INDEX&&cssom_delete(s,2)==CSSOM_INDEX);
    fail_calloc=1;CHECK(cssom_insert(s,".fault{}",8,0)==CSSOM_OOM&&!strcmp(saved,s->source)&&s->next_rule_id==next);fail_calloc=0;
    fail_realloc=1;CHECK(cssom_insert(s,".fault{}",8,0)==CSSOM_OOM&&!strcmp(saved,s->source)&&s->next_rule_id==next);fail_realloc=0;
    fail_calloc=2;CHECK(cssom_delete(s,0)==CSSOM_OOM&&!strcmp(saved,s->source)&&count(s)==2);fail_calloc=0;free(saved);
    CHECK(cssom_insert(s,"/*x*/ .quoted{content:'a}b';color:green} /*y*/",45,2)==CSSOM_OK&&count(s)==3);
    uint32_t old_id=s->id;
    cssom_style_text_changed(&text);text.text=".replace{height:8px}";text.textlen=strlen(text.text);d.dom_revision++;
    struct cssom_sheet*fresh=cssom_style_sheet(&d,&style,&error);
    CHECK(fresh&&fresh!=s&&fresh->id!=old_id&&count(fresh)==1&&!s->associated);
    CHECK(cssom_style_find(&style,old_id)==s&&count(s)==3);
    clear_document(&d);d.resources_dirty=false;CHECK(cssom_insert(s,".detached{}",11,0)==CSSOM_OK&&!d.resources_dirty&&count(fresh)==1);
    /* Parser notifications can be whole-forest; exact DOM source still resets association. */
    text.text=".parser{width:9px}";text.textlen=strlen(text.text);d.dom_revision++;
    struct cssom_sheet*parser=cssom_style_sheet(&d,&style,&error);
    CHECK(parser&&parser!=fresh&&!fresh->associated&&!strcmp(parser->source,text.text));
    cssom_style_lifecycle(&style);CHECK(!parser->associated&&!style.cssom_current);
    struct cssom_sheet*reinsert=cssom_style_sheet(&d,&style,&error);CHECK(reinsert&&reinsert!=parser&&count(reinsert)==1);
    web_doc second={.live=true,.root=&root};strcpy(second.base,"https://second.test/");style.owner=&second;
    cssom_style_lifecycle(&style);struct cssom_sheet*adopted=cssom_style_sheet(&second,&style,&error);
    CHECK(adopted&&adopted->document==&second&&adopted->id!=reinsert->id);
    style.owner=&d;cssom_style_lifecycle(&style);text.text="@import 'child.css';@media screen{.inside{width:2px}}.after{width:3px}";text.textlen=strlen(text.text);d.dom_revision++;
    struct cssom_sheet*imports=cssom_style_sheet(&d,&style,&error);
    CHECK(imports&&count(imports)==3&&rule(imports,0)->type==3&&rule(imports,1)->type==4&&rule(imports,2)->type==1);
    CHECK(rule(imports,0)->import_url&&!strcmp(rule(imports,0)->import_url,"https://cssom.test/child.css"));
    add_sheet_ast(&d,imports->source,imports->length,imports->base,4,"screen",NULL,imports->ast);
    CHECK(d.pending_css.n==1&&!strcmp(((struct pending*)d.pending_css.v[0])->url,"https://cssom.test/child.css")&&d.sty.sheets.n==1);
    CHECK(((sheet_t*)d.sty.sheets.v[0])->owner_media&&!strcmp(((sheet_t*)d.sty.sheets.v[0])->owner_media,"screen"));
    CHECK(cssom_delete(imports,0)==CSSOM_OK&&count(imports)==2&&rule(imports,0)->type==4);
    style.parent=NULL;CHECK(!cssom_style_sheet(&d,&style,&error)&&error==CSSOM_INACTIVE);style.parent=&root;
    d.inert=true;CHECK(!cssom_style_sheet(&d,&style,&error)&&error==CSSOM_INACTIVE);d.inert=false;
    clear_document(&d);clear_document(&second);cssom_style_free(&style);CHECK(!style.cssom_sheets&&!style.cssom_current);
    printf("native CSSOM new boundaries: %u checks / %u failed\n",checks,failed);return failed!=0;
}
