/* Native integration evidence, not real-site/Hyper-V acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
#include <parallel.h>
#include "webi.h"
#include "cssom.h"
#include "form_validation.h"
#include "form_value.h"

static int checks, failed;
static void check(bool ok,const char *name) {
    checks++; if(!ok){failed++;printf("FAIL styleincremental %s\n",name);}
}
static bool checkpoint(void *context) { (void)context; return true; }
static web_doc *load(void) {
    const char *html="<!doctype html><html><head></head><body></body></html>";
    struct web_host host={.script_checkpoint=checkpoint};
    web_doc *d=web_live(html,strlen(html),"https://style.test/","utf-8",&host);
    if(d) { d->profile_enabled=true; parallel_profile_enable(true); for(int i=0;i<8;i++)web_tick(d,uptime_ms()); }
    return d;
}
static node_t *element(web_doc *d,const char *tag,node_t *parent,const char *classes) {
    node_t *n=doc_node_create(d,N_ELEM,tag,NULL,0);
    if(n && classes)doc_node_attr(d,n,"class",classes);
    if(n && parent)doc_node_move(d,parent,n,NULL);
    return n;
}
static struct cssom_sheet *sheet(web_doc *d,const char *text) {
    int error=0;struct cssom_sheet *s=cssom_construct(d,&error);
    if(s && cssom_replace_sync(s,text,strlen(text))!=CSSOM_OK)return NULL;
    return s;
}
static bool near(float a,float b) { return fabsf(a-b)<0.1f; }

static void incremental(void) {
    web_doc *d=load();check(d!=NULL,"document");if(!d)return;
    const char *source=".card{display:block;width:300px}.value{display:block;height:20px;width:100px;color:red}"
        ".value.hot{width:140px;padding-left:7px;color:blue}.value + .following{display:block;height:11px}";
    struct cssom_sheet *s=sheet(d,source);check(s!=NULL,"sheet");if(!s){web_free(d);return;}
    check(cssom_adopt(d->root,&s,1)==CSSOM_OK,"adopt document sheet");
    node_t *cards[64],*values[64],*text=NULL;
    for(int i=0;i<64;i++) {
        cards[i]=element(d,"div",d->body,"card");
        values[i]=element(d,"div",cards[i],"value");
        element(d,"span",cards[i],"following");
        if(i==10) {
            text=doc_node_create(d,N_TEXT,NULL,"hello",5);
            doc_node_move(d,values[i],text,NULL);
        }
    }
    web_layout(d,800,600);
    check(values[10]->style && values[10]->box,"initial cascade/boxes");
    check(values[10]->style_mem.chunk_size==4096 && values[10]->style_mem.allocated<8192,
        "node computed arena uses small chunks");
    check(d->profile.style_parallel_nodes>0,"actual helper threads compute styles");
    box_t *root=d->root_box,*leaf=values[10]->box,*other=values[40]->box;
    uint64_t visits=d->profile.style_visits,rescans=d->profile.rescans;
    doc_node_attr(d,values[10],"class","value hot");
    web_layout(d,800,600);
    uint64_t local=d->profile.style_visits-visits;
    check(local>0 && local<16,"leaf mutation restyles local sibling set");
    check(d->profile.rescans==rescans,"class mutation does not rescan resources");
    check(d->root_box==root && values[10]->box==leaf && values[40]->box==other,"ordinary width/color changes retain boxes");
    check(near(values[10]->box->w,140) && values[10]->style->color==0xff0000ffu,"incremental values applied");
    float width=values[10]->box->w,height=cards[10]->box->h,following=cards[11]->box->y;
    uint32_t color=values[10]->style->color;
    d->style_full_dirty=d->need_style=d->need_boxes=true;
    web_layout(d,800,600);
    check(near(width,values[10]->box->w) && near(height,cards[10]->box->h) &&
        near(following,cards[11]->box->y) && color==values[10]->style->color,"incremental equals forced full layout");
    root=d->root_box;leaf=values[10]->box;
    visits=d->profile.style_visits;
    doc_node_text(d,text,"hello world, changed text",25);
    web_layout(d,800,600);
    check(d->root_box==root && values[10]->box==leaf,"CharacterData retains inline-context boxes");
    check(d->profile.style_visits==visits,"CharacterData skips selector-invariant cascade");
    check(text->box && text->box->text_mem.chunk_size==4096 && text->box->text_mem.allocated<8192,
        "changed text arena uses small chunks");
    height=values[10]->box->h;
    d->need_boxes=true;d->layout_valid=false;web_layout(d,800,600);
    check(near(height,values[10]->box->h),"text update equals full box rebuild");
    printf("styleincremental local_visits=%llu full_nodes=%llu threaded_nodes=%llu\n",
        (unsigned long long)local,(unsigned long long)d->profile.style_visits,(unsigned long long)d->profile.style_parallel_nodes);
    web_free(d);
}

static void scopes(void) {
    web_doc *d=load();check(d!=NULL,"scopes document");if(!d)return;
    struct cssom_sheet *s=sheet(d,":host{padding-left:3px}.value{display:block;width:51px;color:blue}::slotted(.light){color:lime}");
    check(s!=NULL,"shared sheet");if(!s){web_free(d);return;}
    node_t *inside[96],*hosts[96],*light[96];
    uint64_t initial_entries=d->profile.style_index_entries;
    uint64_t first_entries=0,first_generation_entries=0,first_checks=0,first_visits=0;
    for(int i=0;i<96;i++) {
        hosts[i]=element(d,"div",d->body,NULL);
        node_t *root=doc_shadow_attach(d,hosts[i],false,false,false,false,false);
        cssom_adopt(root,&s,1);
        inside[i]=element(d,"div",root,"value");
        element(d,"slot",root,NULL);
        light[i]=element(d,"span",hosts[i],"light");
        if(i==0) {
            web_layout(d,800,600);
            first_entries=d->profile.style_index_entries;
            first_generation_entries=first_entries-initial_entries;
            first_checks=d->profile.style_rule_checks;
            first_visits=d->profile.style_visits;
        }
    }
    web_layout(d,800,600);
    uint64_t entries=d->profile.style_index_entries-first_entries;
    uint64_t matched=d->profile.style_rule_checks-first_checks;
    uint64_t visited=d->profile.style_visits-first_visits;
    /* Counters are monotonic: load() has already indexed UA rules. Compare
       the two actual stylesheet generations, not a generation with a total. */
    check(entries==first_generation_entries,"96 adoptions index the AST once");
    check(visited && matched/visited<20,"matching visits own scope, not all adoption buckets");
    check(inside[95]->style && near(inside[95]->style->width.px,51),"last root own scope rule");
    check(hosts[95]->style && near(hosts[95]->style->padding[3].px,3),"host rule retained");
    check(light[95]->style && light[95]->style->color==0xff00ff00u,"slotted rule retained");
    printf("styleincremental scopes=96 first_generation_entries=%llu index_entries=%llu checks=%llu visits=%llu\n",
        (unsigned long long)first_generation_entries,(unsigned long long)entries,(unsigned long long)matched,(unsigned long long)visited);
    node_t *slot=hosts[95]->shadow_root->last;
    node_t *fallback=element(d,"div",slot,"value");
    web_layout(d,800,600);
    check(!fallback->style && !fallback->box,"assigned light suppresses fallback");
    uint64_t assignments=d->profile.shadow_reassigns;
    doc_node_attr(d,light[95],"slot","not-present");
    check(d->profile.shadow_reassigns==assignments,"live invalidation does not flush slot batch");
    web_layout(d,800,600);
    check(fallback->style && fallback->box && near(fallback->style->width.px,51) && !light[95]->box,
        "assignment topology reveals styled fallback and removes old light box");
    web_free(d);
}
static void derived_styles(void) {
    web_doc *d=load();check(d!=NULL,"derived styles document");if(!d)return;
    struct cssom_sheet *s=sheet(d,
        ".derived{display:inline-flex;font-family:monospace;--paint:red;color:var(--paint)}"
        "details{font-family:monospace;--paint:red;color:var(--paint)}");
    check(s && cssom_adopt(d->root,&s,1)==CSSOM_OK,"derived styles sheet");
    if(!s){web_free(d);return;}
    node_t *wrap=element(d,"div",d->body,"derived");
    node_t *child=element(d,"span",wrap,NULL);
    node_t *text=doc_node_create(d,N_TEXT,NULL,"wrapped",7);
    doc_node_move(d,child,text,NULL);
    node_t *details=element(d,"details",d->body,NULL);
    web_layout(d,800,600);
    box_t *outer=wrap->box,*inner=outer?outer->first:NULL,*legend=details->box?details->box->first:NULL;
    check(outer && inner && legend,"derived wrapper and default legend exist");
    doc_node_attr(d,wrap,"style","--paint:blue");
    doc_node_attr(d,details,"style","--paint:blue");
    web_layout(d,800,600);
    check(wrap->box==outer && outer && outer->first==inner && details->box && details->box->first==legend,
        "paint-only derived styles retain box topology");
    check(outer && inner && outer->st->color==0xff0000ffu && inner->st->color==0xff0000ffu &&
        outer->st->font_names==wrap->style->font_names && inner->st->vars==wrap->style->vars,
        "wrapper copies borrow current computed arena");
    check(legend && legend->st->color==0xff0000ffu && legend->st->font_names==details->style->font_names &&
        legend->st->vars==details->style->vars && legend->st->list_style==LS_STRING,
        "default legend refreshes inheritance and keeps marker overrides");
    web_free(d);
}
static void percentage_padding_cache(void) {
    web_doc *d=load();check(d!=NULL,"percentage padding document");if(!d)return;
    struct cssom_sheet *s=sheet(d,
        ".tracks{display:grid;width:400px;grid-template-columns:auto auto;align-items:start}"
        ".contribution{font-family:monospace;font-size:16px;white-space:nowrap}"
        ".stable{width:100px;height:100px;box-sizing:border-box;padding-top:10%;overflow:hidden}");
    check(s && cssom_adopt(d->root,&s,1)==CSSOM_OK,"percentage padding sheet");
    if(!s){web_free(d);return;}
    node_t *grid=element(d,"div",d->body,"tracks");
    node_t *contribution=element(d,"div",grid,"contribution");
    node_t *text=doc_node_create(d,N_TEXT,NULL,"short",5);
    doc_node_move(d,contribution,text,NULL);
    node_t *stable=element(d,"div",grid,"stable");
    web_layout(d,800,600);
    box_t *root=d->root_box,*box=stable->box;
    if(!box){check(false,"percentage padding initial box");web_free(d);return;}
    float old_padding=box->p[0],old_width=box->w;
    style_t *old_style=stable->style;
    const char *longer="a much longer unbroken contribution";
    check(doc_node_text(d,text,longer,strlen(longer)),"grid sibling CharacterData mutation");
    check(box->layout_cache_valid && !box->layout_dirty && stable->style==old_style,
        "unmodified sibling remains cache eligible");
    web_layout(d,800,600);
    check(d->root_box==root && stable->box==box && near(box->w,old_width) && !near(box->p[0],old_padding),
        "sibling text changes track percentage base but not fixed content width");
    check(near(box->h+box->p[0]+box->p[2]+box->b[0]+box->b[2],100),
        "percentage padding invalidates cached border-box content height");
    float height=box->h,padding=box->p[0];
    d->need_boxes=true;d->layout_valid=false;web_layout(d,800,600);
    check(stable->box && near(height,stable->box->h) && near(padding,stable->box->p[0]),
        "percentage padding incremental geometry equals forced full");
    printf("styleincremental percent_padding old=%.2f new=%.2f content_height=%.2f\n",
        (double)old_padding,(double)padding,(double)height);
    web_free(d);
}
static void parallel_form_selectors(void) {
    web_doc *d=load();check(d!=NULL,"form selector document");if(!d)return;
    struct cssom_sheet *s=sheet(d,
        ".checked{color:red}.checked:checked{color:lime}"
        ".text:valid{color:lime}.text:invalid{color:red}"
        ".number:in-range{color:blue}.number:out-of-range{color:orange}"
        ".empty:placeholder-shown{color:purple}"
        "form:valid,fieldset:valid{color:lime}form:invalid,fieldset:invalid{color:red}");
    check(s && cssom_adopt(d->root,&s,1)==CSSOM_OK,"form selector sheet");
    if(!s){web_free(d);return;}
    /* A huge branch hidden under one of eight tiny wrappers exercises the
       independent frontier, not just the old first-eight-siblings dispatch. */
    node_t *wrap=element(d,"div",d->body,NULL),*cards[64],*inputs[64],*texts[64],*numbers[64],*options[64];
    for(int i=0;i<7;i++)element(d,"div",wrap,NULL);
    node_t *feed=element(d,"div",wrap,NULL);
    for(int i=0;i<64;i++) {
        cards[i]=element(d,"form",feed,NULL);
        inputs[i]=element(d,"input",cards[i],"checked");
        doc_node_attr(d,inputs[i],"type","checkbox");doc_node_attr(d,inputs[i],"checked","");
        texts[i]=element(d,"input",cards[i],"text");
        doc_node_attr(d,texts[i],"required","");doc_node_attr(d,texts[i],"pattern","[a-z]+");
        doc_node_attr(d,texts[i],"value",i==0?"123":"word");
        numbers[i]=element(d,"input",cards[i],"number");
        doc_node_attr(d,numbers[i],"type","number");doc_node_attr(d,numbers[i],"min","0");
        doc_node_attr(d,numbers[i],"max","10");doc_node_attr(d,numbers[i],"value",i==0?"15":"5");
        node_t *select=element(d,"select",cards[i],NULL);
        options[i]=element(d,"option",select,"checked");
        element(d,"option",select,NULL);
    }
    doc_node_attr(d,cards[63],"id","external-owner");
    node_t *external=element(d,"input",d->body,"text");
    doc_node_attr(d,external,"required","");doc_node_attr(d,external,"form","external-owner");
    doc_node_attr(d,external,"style","display:none");
    node_t *fieldset=element(d,"fieldset",cards[0],NULL);
    node_t *empty=element(d,"textarea",fieldset,"empty");
    doc_node_attr(d,empty,"placeholder","hint");doc_node_attr(d,empty,"required","");
    uint64_t threaded=d->profile.style_parallel_nodes;
    struct parallel_stats before={0},after={0};parallel_get_stats(PARALLEL_STYLE,&before);
    web_layout(d,800,600);
    parallel_get_stats(PARALLEL_STYLE,&after);
    check(d->profile.style_parallel_nodes-threaded>256,"form rules and skewed wrappers still run substantial AP cascade");
    check(after.helper_work-before.helper_work>256 && after.work-before.work>=400 &&
        (after.sampled_helper_cpu_mask & (after.sampled_helper_cpu_mask-1))!=0,
        "substantial form CSS work executes on multiple AP CPUs, not just scheduled jobs");
    check(inputs[40]->style && inputs[40]->style->color==0xff00ff00u,"checked snapshot on helper subtree");
    check(options[40]->style && options[40]->style->color==0xff00ff00u,"default select option snapshot on helper subtree");
    check(texts[0]->style && texts[0]->style->color==0xffff0000u && texts[40]->style->color==0xff00ff00u,
        "regex validation snapshot preserves valid and invalid selectors");
    check(numbers[0]->style && numbers[0]->style->color==0xffffa500u && numbers[40]->style->color==0xff0000ffu,
        "range snapshot preserves both selector states");
    check(cards[0]->style && cards[0]->style->color==0xffff0000u && cards[40]->style->color==0xff00ff00u &&
        cards[63]->style->color==0xffff0000u,"form snapshot aggregates invalid and external hidden controls");
    check(fieldset->style && fieldset->style->color==0xffff0000u && empty->style && empty->style->color==0xff800080u,
        "fieldset and placeholder snapshot preserve ordinary DOM semantics");
    bool valid=false;
    web_select_set_index(d,options[40]->parent,1);
    check(!css_matches(options[40],":checked",&valid) && valid,"live matching does not reuse prior cascade snapshot");
    doc_node_attr(d,texts[0],"value","word");doc_node_attr(d,numbers[0],"value","5");
    doc_node_attr(d,external,"value","now valid");doc_node_attr(d,empty,"required",NULL);
    web_layout(d,800,600);
    uint32_t text_color=texts[0]->style?texts[0]->style->color:0;
    uint32_t range_color=numbers[0]->style?numbers[0]->style->color:0;
    check(text_color==0xff00ff00u && range_color==0xff0000ffu && cards[63]->style->color==0xff00ff00u,
        "form snapshot refreshes after value mutations");
    d->style_full_dirty=d->need_style=d->need_boxes=true;web_layout(d,800,600);
    check(texts[0]->style && texts[0]->style->color==text_color && numbers[0]->style && numbers[0]->style->color==range_color,
        "form snapshot incremental agrees with forced full cascade");
    printf("styleincremental form_threaded_nodes=%llu\n",(unsigned long long)(d->profile.style_parallel_nodes-threaded));
    web_free(d);
}

static void cssom_container_roundtrip(void) {
    web_doc *d=load();check(d!=NULL,"CSSOM grouping document");if(!d)return;
    struct cssom_sheet *s=sheet(d,"@import url('discard.css');"
        "@container(width>1px){.kept{height:23px}}@layer{.kept{color:blue}}"
        "@unrecognized{.kept{height:999px}}");
    uint32_t count=0;struct css_rule_info *r=s?css_sheet_rules(s->ast,&count):NULL;
    check(r && count==2 && r->type==0 && r->next && r->next->type==0 &&
        strstr(r->text,"@container") && strstr(r->next->text,"@layer") && !strstr(s->source,"@import"),
        "constructed import removal preserves recognized type-zero grouping rules");
    if(!s){web_free(d);return;}
    const char *temporary=".temporary{height:1px}";
    check(cssom_insert(s,temporary,strlen(temporary),0)==CSSOM_OK,
        "CSSOM insertion preserves container group source");
    int deleted=cssom_delete(s,0);r=css_sheet_rules(s->ast,&count);
    check(deleted==CSSOM_OK && count==2 && r && strstr(r->text,"@container"),
        "CSSOM deletion preserves container group source");
    check(cssom_adopt(d->root,&s,1)==CSSOM_OK,"adopt edited container group sheet");
    node_t *parent=element(d,"div",d->body,NULL);
    doc_node_attr(d,parent,"style","container-type:inline-size;width:400px");
    node_t *kept=element(d,"div",parent,"kept");web_layout(d,1920,1080);
    check(kept->box && near(kept->box->h,23) && kept->style && kept->style->color==0xff0000ffu,
        "container group still cascades after import stripping and CSSOM edits");
    web_free(d);
}

static void container_queries(void) {
    web_doc *d=load();check(d!=NULL,"container document");if(!d)return;
    const char *css=".banner{height:8rem;width:calc(50cqi + 10px);color:red}"
        ".probe{height:10cqh;width:10cqw;padding-left:0;margin-left:0}"
        ".max{height:10cqmax;width:10cqmin}"
        ".illegal{height:7px}@container InlineOnly (width>1px){.illegal{height:99px}}"
        "@container TableOnly (width>1px){.illegal{height:98px}}"
        "@container(width>=500px){.pseudo::before{content:'x';display:block;width:10cqi;height:5px}}"
        "@container \\31 23 (min-width:300px){.escaped{height:31px}}"
        "@container Slash\\/Name (min-width:300px){.escaped{width:37px}}"
        "@media(min-width:960px){@container(min-width:80rem){.banner{height:12rem}}}"
        "@container Outer (min-width:80rem){.banner{color:blue}}"
        "@container outer (min-width:1px){.banner{color:lime}}"
        "@container(width>40em){.probe{margin-left:13px}}"
        "@container(width<=40em){.probe{padding-left:7px}}"
        "@container(height>=150px){.probe{background-color:lime}}"
        "@container not style(--unsupported:yes){.probe{margin-top:999px}}"
        "@container(min-width:900px){@container Outer (width>1000px){.probe{margin-right:19px}}}"
        "@container(inline-size>900px) and (inline-size<1200px){.probe{padding-right:11px}}";
    struct cssom_sheet *s=sheet(d,css);
    check(s && cssom_adopt(d->root,&s,1)==CSSOM_OK,"container sheet");
    if(!s){web_free(d);return;}
    unsigned groups=0;
    for(struct css_rule_info *r=css_sheet_rules(s->ast,NULL);r;r=r->next)
        if(r->type==0 && !strncmp(r->text,"@container",10))groups++;
    check(groups==13,"replaceSync retains all standalone container groups in CSSOM");
    node_t *size=element(d,"div",d->body,NULL);
    doc_node_attr(d,size,"style","container-type:size;width:1500px;height:200px");
    node_t *outer=element(d,"div",size,NULL);
    doc_node_attr(d,outer,"style","container:Outer / inline-size;width:1400px");
    node_t *inner=element(d,"div",outer,NULL);
    doc_node_attr(d,inner,"id","container-inner");
    doc_node_attr(d,inner,"style","container:Inner extra / inline-size;width:1072px;font-size:32px");
    node_t *banner=element(d,"div",inner,"banner"),*probe=element(d,"div",inner,"probe");
    node_t *maximum=element(d,"div",inner,"max");
    for(int i=0;i<160;i++)element(d,"div",inner,"probe");
    web_layout(d,1920,1080);
    check(d->layout_valid && !d->need_style,"initial container measurement reaches stable cascade");
    check(inner->style && inner->style->container_type==CT_INLINE_SIZE &&
        !strcmp(inner->style->container_names,"Inner extra"),"container shorthand publishes type and identifier list");
    check(banner->box && near(banner->box->h,128),"1072px nearest container rejects 80rem despite 1920px viewport");
    check(banner->style && banner->style->color==0xff0000ffu,"named query finds outer ancestor and names are case sensitive");
    check(probe->style && near(probe->style->margin[3].px,0) && near(probe->style->padding[3].px,7),
        "query em uses nearest container font size rather than root or viewport");
    check(probe->style && probe->style->bg_color==0xff00ff00u,"height query skips inline-only container to size ancestor");
    check(probe->style && near(probe->style->margin[0].px,0),"unsupported negated style condition is unknown, not unconditional");
    check(probe->style && near(probe->style->margin[1].px,19) && near(probe->style->padding[1].px,11),
        "nested named queries and logical range conditions both evaluate");
    check(banner->box && near(banner->box->w,546) && probe->box && near(probe->box->w,107.2f) && near(probe->box->h,20),
        "cqi calc, cqw and cqh use independent eligible axes");
    check(maximum->box && near(maximum->box->w,20) && near(maximum->box->h,107.2f),"cqmin and cqmax combine independent axis containers");
    uint64_t revision=d->layout_revision,visits=d->profile.style_visits;
    web_layout(d,1920,1080);
    check(d->layout_revision==revision && d->profile.style_visits==visits,"unchanged geometry read neither scans nor recascades container rules");
    check(d->profile.style_parallel_nodes>160,"container conditions preserve substantial helper cascade");
    const char *script="var cs=getComputedStyle(document.getElementById('container-inner'));"
        "if(cs.getPropertyValue('container-type')!=='inline-size'||cs.getPropertyValue('container-name')!=='Inner extra')throw Error('container computed');";
    check(web_console_eval(d,script,strlen(script)),"computed style exposes container longhands");
    doc_node_attr(d,inner,"style","container:Inner extra / inline-size;width:1400px;font-size:32px");
    web_layout(d,1920,1080);
    check(d->layout_valid && banner->box && near(banner->box->h,192) && near(banner->box->w,710),
        "container width mutation flips condition and recomputes container units");
    float bh=banner->box?banner->box->h:0,bw=banner->box?banner->box->w:0;
    d->style_full_dirty=d->need_style=d->need_boxes=true;web_layout(d,1920,1080);
    check(banner->box && near(banner->box->h,bh) && near(banner->box->w,bw),"settled container geometry equals forced full rebuild");
    doc_node_attr(d,outer,"style","container:Renamed / inline-size;width:1400px");
    web_layout(d,1920,1080);
    check(banner->style && banner->style->color==0xffff0000u,"container name mutation invalidates named conditions");
    doc_node_attr(d,inner,"style","container-type:normal;width:1400px;font-size:32px");
    web_layout(d,1920,1080);
    check(probe->style && near(probe->style->margin[3].px,13),"container type mutation retargets nearest eligible ancestor");
    node_t *small=element(d,"div",d->body,NULL);
    doc_node_attr(d,small,"style","container-type:inline-size;width:600px");
    doc_node_move(d,small,banner,NULL);web_layout(d,1920,1080);
    check(banner->box && near(banner->box->h,128) && near(banner->box->w,310),"flat ancestor mutation retargets query and unit snapshots");
    node_t *host=element(d,"div",d->body,NULL);
    doc_node_attr(d,host,"style","container-type:inline-size;width:600px");
    node_t *shadow=doc_shadow_attach(d,host,false,false,false,false,false);
    struct cssom_sheet *ss=sheet(d,".scoped{height:9px}@container(width>=500px){.scoped{height:17px}}"
        "::slotted(.light){display:block;width:10cqb;height:10cqi}");
    cssom_adopt(shadow,&ss,1);node_t *scoped=element(d,"div",shadow,"scoped");
    element(d,"slot",shadow,NULL);node_t *light=element(d,"div",host,"light");
    web_layout(d,1920,1080);
    check(scoped->box && near(scoped->box->h,17),"shadow scoped rule queries host through flat ancestry");
    check(light->box && near(light->box->h,60) && near(light->box->w,108),"slotted units use host inline axis and viewport block fallback");
    node_t *empty=element(d,"div",d->body,NULL);
    doc_node_attr(d,empty,"style","container-type:size;width:200px");
    node_t *overflow=element(d,"div",empty,NULL);doc_node_attr(d,overflow,"style","height:300px;width:900px");
    node_t *shrink=element(d,"div",d->body,NULL);doc_node_attr(d,shrink,"style","display:inline-block;container-type:inline-size");
    node_t *wide=element(d,"div",shrink,NULL);doc_node_attr(d,wide,"style","width:900px;height:20px");
    web_layout(d,1920,1080);
    check(empty->box && near(empty->box->h,0),"size containment auto block size ignores overflowing descendant");
    check(shrink->box && near(shrink->box->w,0),"inline size containment shrink-to-fit ignores child intrinsic width");
    node_t *pseudo=element(d,"div",d->body,"pseudo");doc_node_attr(d,pseudo,"style","container-type:inline-size;width:600px");
    node_t *named=element(d,"div",d->body,NULL);
    doc_node_attr(d,named,"style","container:\\31 23 Slash\\/Name / inline-size;width:400px");
    node_t *escaped=element(d,"div",named,"escaped");
    web_layout(d,1920,1080);
    check(pseudo->style && pseudo->style->before && near(pseudo->style->before->width.px,60),
        "generated pseudo queries and units include originating container");
    check(escaped->box && near(escaped->box->h,31) && near(escaped->box->w,37),
        "escaped leading digit and punctuation names compare without identifier length cap");
    node_t *inline_only=element(d,"span",d->body,NULL);
    doc_node_attr(d,inline_only,"style","container:InlineOnly / inline-size");
    node_t *illegal=element(d,"div",inline_only,"illegal");
    node_t *table=element(d,"div",d->body,NULL);doc_node_attr(d,table,"style","display:inline-table;container:TableOnly / inline-size;width:500px");
    node_t *row=element(d,"div",table,NULL);doc_node_attr(d,row,"style","display:table-row");
    node_t *cell=element(d,"div",row,NULL);doc_node_attr(d,cell,"style","display:table-cell");
    node_t *table_illegal=element(d,"div",cell,"illegal");
    web_layout(d,1920,1080);
    check(!inline_only->container_valid && illegal->box && near(illegal->box->h,7),
        "block-in-inline repair does not make non-atomic inline a size query container");
    check(!table->container_valid && table_illegal->box && near(table_illegal->box->h,7),
        "inline-table wrapper cannot bypass size containment eligibility");
    printf("styleincremental containers banner_initial=128 banner_large=%.0f stable=%d threaded=%llu\n",
        (double)bh,d->layout_valid,(unsigned long long)d->profile.style_parallel_nodes);
    web_free(d);

    d=load();if(!d){check(false,"inline-only container document");return;}
    node_t *parent=element(d,"div",d->body,NULL);doc_node_attr(d,parent,"style","container-type:inline-size;width:400px");
    node_t *child=element(d,"div",parent,NULL);doc_node_attr(d,child,"style","width:25cqw;height:10px");
    web_layout(d,1920,1080);
    check(child->box && near(child->box->w,100),"inline declaration alone enables container-unit measurement fastpath");
    web_free(d);
}
int main(void) {
    incremental();scopes();derived_styles();percentage_padding_cache();parallel_form_selectors();cssom_container_roundtrip();container_queries();
    printf("styleincremental: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
