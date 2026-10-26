/* CSS pool/cache host cases, included after the exact production ownership
   and parser functions by check_cssperformance.py; not real-site acceptance. */

static int checks, failed;
static void check(bool ok, const char *name) {
    checks++; if (!ok) { failed++; printf("FAIL cssperformance %s\n", name); }
}
static struct css_style_pool *new_pool(web_doc *d) {
    d->style_pool = calloc(1, sizeof *d->style_pool);
    if (d->style_pool) d->style_pool->owned = true;
    return d->style_pool;
}
static size_t slab_count(struct css_style_pool *p) {
    size_t count = 0;
    for (struct css_style_slab *s = p->slabs; s; s = s->next) count++;
    return count;
}
static void fixed_styles(void) {
    web_doc d = {0}; node_t *nodes = calloc(4096, sizeof *nodes);
    check(nodes && new_pool(&d), "pool setup"); if (!nodes || !d.style_pool) return;
    for (size_t i = 0; i < 4096; i++) {
        nodes[i].style = style_slot_alloc(&d, &nodes[i], &nodes[i].style_mem);
        css_style_init(nodes[i].style, NULL);
    }
    check(slab_count(d.style_pool) == 64 && d.style_pool->live == 4096, "64 shared slabs hold 4096 fixed styles");
    bool no_tail = true;
    for (size_t i = 0; i < 4096; i++) if (nodes[i].style_mem.allocated) no_tail = false;
    check(no_tail, "plain bodies allocate no node arena");
    struct cascade c = {.d=&d};
    style_t *previous = nodes[17].style;
    retire_style(&c, &nodes[17]);
    nodes[17].style = style_slot_alloc(&d, &nodes[17], c.a);
    css_style_init(nodes[17].style, previous);
    check(nodes[17].style != previous && nodes[17].style_previous_slots && d.style_pool->live == 4097,
        "previous body survives replacement until publication join");
    css_styles_release(&d);
    check(!nodes[17].style_previous_slots && !nodes[17].style_previous && d.style_pool->live == 4096,
        "joined generation returns previous body");
    size_t peak_slabs = slab_count(d.style_pool);
    for (unsigned cycle = 0; cycle < 200; cycle++) {
        retire_style(&c, &nodes[17]);
        nodes[17].style = style_slot_alloc(&d, &nodes[17], c.a);
        css_style_init(nodes[17].style, NULL); css_styles_release(&d);
    }
    check(slab_count(d.style_pool) == peak_slabs, "200 generations reuse returned slots rather than grow");
    for (size_t i = 0; i < 4096; i++) css_node_style_free(&nodes[i]);
    check(d.style_pool->live == 0, "node cleanup returns all current and pseudo/base bodies");
    printf("cssperformance sizeof(style_t)=%zu peak_slabs=%zu shared_body_bytes=%zu nodes=4096\n",
        sizeof(style_t), peak_slabs, peak_slabs * sizeof(struct css_style_slab));
    css_style_doc_free(&d); free(nodes);
}
static void adopted_style(void) {
    web_doc source = {0}, target = {0}; node_t adopted = {0};
    check(new_pool(&source) && new_pool(&target), "adoption pools");
    adopted.style = style_slot_alloc(&source, &adopted, &adopted.style_mem);
    css_style_init(adopted.style, NULL); adopted.style->color = 0xff123456;
    struct css_style_pool *old = source.style_pool;
    css_style_doc_free(&source);
    check(!source.style_pool && !old->owned && old->live == 1 && adopted.style->color == 0xff123456,
        "closing old render owner preserves allocation-owned adopted slot");
    struct cascade c = {.d=&target}; retire_style(&c, &adopted);
    adopted.style = style_slot_alloc(&target, &adopted, c.a); css_style_init(adopted.style, NULL);
    css_styles_release(&target); /* last returned orphan destroys its old pool */
    check(target.style_pool->live == 1 && !adopted.style_previous_slots, "adopted restyle returns its old pool safely");
    css_style_doc_free(&target);
    check(adopted.style != NULL, "closing new render owner also preserves borrowed slot");
    css_node_style_free(&adopted);
}
static const char *declaration_value(struct css_declarations *cache, const char *name) {
    const struct propdef *property = css_prop_lookup(name, strlen(name));
    for (int i = 0; cache && i < cache->count; i++) if (cache->values[i].p == property) return cache->values[i].value;
    return NULL;
}
static void declaration_cache(void) {
    arena_t failure = {0}; struct css_declarations *cache = NULL;
    char text[] = "width:10px;--shade:red;background-image:url(foo.png)";
    declarations_get(&cache, text, "https://cache.test/one/", &failure);
    check(cache && cache->count == 3, "inline source parses into immutable native declarations");
    struct css_declarations *first = cache; struct decl *ast = cache->values;
    declarations_get(&cache, text, "https://cache.test/one/", &failure);
    check(cache == first && cache->values == ast, "unchanged cascade reuses AST");
    char *same_content = strdup(text);
    declarations_get(&cache, same_content, "https://cache.test/one/", &failure);
    check(cache == first, "equal bytes at a new attribute address reuse AST"); free(same_content);
    text[6] = '2'; text[7] = '2';
    declarations_get(&cache, text, "https://cache.test/one/", &failure);
    check(!strcmp(declaration_value(cache, "width"), "22px"), "same address and length with changed contents invalidates");
    declarations_get(&cache, text, "https://cache.test/two/", &failure);
    check(strstr(declaration_value(cache, "background-image"), "https://cache.test/two/foo.png") != NULL,
        "changed document base re-resolves cached relative URL");
    declarations_get(&cache, "", "https://cache.test/two/", &failure);
    check(!cache, "attribute removal reclaims cache"); ar_free(&failure);
    node_t n = {0};
    check(css_animation_set(&n, PE_NONE, 9, "opacity:0.3") == 1, "WAAPI sample publication");
    struct css_animation *a = n.animations;
    declarations_get(&a->declarations, a->text, "about:blank", &failure); first = a->declarations;
    check(css_animation_set(&n, PE_NONE, 9, "opacity:0.3") == 0 && a->declarations == first,
        "same animation sample does not invalidate parsed block");
    check(css_animation_set(&n, PE_NONE, 9, "opacity:0.7") == 1, "new animation sample changes source");
    declarations_get(&a->declarations, a->text, "about:blank", &failure);
    check(!strcmp(declaration_value(a->declarations, "opacity"), "0.7"), "new animation sample replaces immutable AST");
    css_animation_free(&n); check(!n.animations, "effect removal releases AST and source"); ar_free(&failure);
}
static style_t *fresh_style(web_doc *d, node_t *n, const style_t *parent) {
    style_t *s = style_slot_alloc(d, n, &n->style_mem); css_style_init(s, parent); return s;
}
static void pseudo_generations(void) {
    web_doc d = {0}; node_t n = {0}; new_pool(&d);
    n.style = fresh_style(&d, &n, NULL);
    n.style->before = fresh_style(&d, &n, n.style);
    n.style->after = fresh_style(&d, &n, n.style);
    n.style->backdrop = fresh_style(&d, &n, NULL);
    n.animation_base_style = fresh_style(&d, &n, NULL);
    n.animation_base_style->before = fresh_style(&d, &n, n.animation_base_style);
    n.animation_base_style->after = fresh_style(&d, &n, n.animation_base_style);
    n.style->before->content = ar_strdup(&n.style_mem, "old before");
    n.style->after->content = ar_strdup(&n.style_mem, "old after");
    style_t *old = n.style;
    struct cascade c = {.d=&d}; retire_style(&c, &n);
    n.style = fresh_style(&d, &n, old); n.animation_base_style = NULL;
    check(!n.style->before && !n.style->after && !n.style->backdrop,
        "replacement body does not inherit retired pseudo pointers");
    n.style->before = fresh_style(&d, &n, n.style);
    n.style->after = fresh_style(&d, &n, n.style);
    n.style->backdrop = fresh_style(&d, &n, NULL);
    check(d.style_pool->live == 11 && !strcmp(old->before->content, "old before") &&
        !strcmp(old->after->content, "old after"), "all seven previous pseudo/base slots and tail survive before join");
    css_styles_release(&d);
    check(d.style_pool->live == 4 && !n.style_previous_mem.allocated && !n.style_previous_slots,
        "join returns every previous pseudo and underlying slot plus tail");
    css_node_style_free(&n); check(!d.style_pool->live, "node cleanup returns every current pseudo slot");
    css_style_doc_free(&d);
}
static void cancelled_tail(void) {
    /* Heap state stays defined after the deliberate longjmp failure. */
    web_doc *d = calloc(1, sizeof *d); node_t *nodes = calloc(2, sizeof *nodes);
    new_pool(d); node_t *parent=&nodes[0], *child=&nodes[1]; parent->first=child; child->parent=parent;
    parent->style = fresh_style(d, parent, NULL);
    parent->style->vars = ar_alloc(&parent->style_mem, sizeof(struct custom_prop));
    parent->style->vars->name = ar_strdup(&parent->style_mem, "--held");
    parent->style->vars->value = ar_strdup(&parent->style_mem, "red");
    child->style = fresh_style(d, child, parent->style);
    check(child->style->vars == parent->style->vars, "actual style initializer borrows parent variable tail");
    jmp_buf trap; struct cascade c = {.d=d, .trap=&trap};
    if (!setjmp(trap)) {
        retire_style(&c, parent);
        (void)fresh_style(d, parent, NULL); /* unpublished replacement slot */
        parent->style_mem.limit = 1;
        (void)ar_strdup(&parent->style_mem, "forced tail quota failure");
        check(false, "tail failure must longjmp");
    } else {
        check(child->style->vars == parent->style_previous->vars &&
            !strcmp(child->style->vars->value, "red"), "tail failure leaves old inherited borrow live until cancellation join");
        d->native_cancelled=true;
        css_node_style_unpublish(parent); css_styles_release(d);
        check(!parent->style && !child->style && !parent->style_mem.trap &&
            !parent->style_previous_slots && !parent->style_previous_mem.allocated,
            "cancel unpublishes borrowers and releases prior generation with no stale trap");
        /* Resume from coherent DOM; abandoned partial slot is retired too. */
        c.trap=NULL; retire_style(&c,parent); parent->style=fresh_style(d,parent,NULL);
        retire_style(&c,child); child->style=fresh_style(d,child,parent->style);
        css_styles_release(d);
        check(d->style_pool->live==2 && !parent->style_previous_slots && !child->style_previous_slots,
            "recovery returns abandoned partial slot and stale child body");
    }
    css_node_style_free(child); css_node_style_free(parent); css_style_doc_free(d); free(nodes); free(d);
}
static void allocation_failures(void) {
    web_doc *d=calloc(1,sizeof *d); node_t *nodes=calloc(65,sizeof *nodes); new_pool(d);
    for (size_t i=0;i<64;i++) nodes[i].style=fresh_style(d,&nodes[i],NULL);
    jmp_buf trap; nodes[64].style_mem.trap=&trap;
    if (!setjmp(trap)) {
        fail_style_malloc=1; (void)fresh_style(d,&nodes[64],NULL);
        check(false,"slab allocator failure must longjmp");
    } else {
        check(d->style_pool->lock==0 && d->style_pool->live==64 && !nodes[64].style_slots,
            "slab OOM unlocks pool and publishes no partial slot");
        nodes[64].style_mem.trap=NULL;
        nodes[64].style=fresh_style(d,&nodes[64],NULL);
        check(d->style_pool->live==65 && slab_count(d->style_pool)==2,"pool remains usable after slab OOM");
    }
    for (size_t i=0;i<65;i++)css_node_style_free(&nodes[i]);
    css_style_doc_free(d); free(nodes); free(d);
    arena_t failure={.trap=&trap}; struct css_declarations **slot=calloc(1,sizeof *slot);
    declarations_get(slot,"color:red","about:blank",&failure);
    if (!setjmp(trap)) {
        fail_cache_calloc=1; declarations_get(slot,"color:blue","about:blank",&failure);
        check(false,"cache allocator failure must longjmp");
    } else {
        check(!strcmp(declaration_value(*slot,"color"),"red") && !(*slot)->arena.trap,
            "cache OOM preserves last complete immutable AST without stack trap");
    }
    declarations_free(slot);free(slot); ar_free(&failure);
}
static void font_sources_native(void) {
    struct source_case { const char *value; const char *first; const char *second; };
    static const struct source_case cases[]={
        {"url(a.eot) format('embedded-opentype'),url(a.woff2) format('woff2'),url(a.woff) format('woff')","a.woff2","a.woff"},
        {"url(a.eot) format(embedded-opentype),url(a.woff2) format(woff2)","a.woff2",NULL},
        {"url(a.eot)","a.eot",NULL},
        {"url(a.woff2) format(woff2) tech(variations),url(b.woff2) format(woff2)","b.woff2",NULL},
        {"url(a.woff2) format(),url(b.ttf)","b.ttf",NULL},
        {"url(a.woff2) format(unknown),url(b.ttf)","b.ttf",NULL},
        {"url(a.ttf) format('truetype'),url(b.otf) format(opentype)","a.ttf","b.otf"},
        {"url(a.eot) format(embedded-opentype)",NULL,NULL},
        {"url(a.svg) format(svg)",NULL,NULL},
        {"url(a.woff2) format(woff2) dummy()",NULL,NULL},
        {"url(a.ttf) format(opentype,truetype)",NULL,NULL},
        {"url(a.ttf) format(unknown),url(b.ttf) tech(broken)",NULL,NULL},
    };
    arena_t memory={0};struct pctx pc={.a=&memory,.base="https://font.test/path/style.css"};
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        struct font_source *sources=font_sources(&pc,cases[i].value,cases[i].value+strlen(cases[i].value));
        char expected[100];bool ok;
        if(cases[i].first){snprintf(expected,sizeof expected,"https://font.test/path/%s",cases[i].first);ok=sources&&!strcmp(sources->url,expected);}
        else ok=sources==NULL;
        check(ok,cases[i].value);
        if(cases[i].second){snprintf(expected,sizeof expected,"https://font.test/path/%s",cases[i].second);check(sources&&sources->next&&!strcmp(sources->next->url,expected)&&!sources->next->next,"font source authored fallback order");}
        else check(!sources||!sources->next,"font source no spurious unsupported item");
        ar_free(&memory);
    }
    const char *bodies[]={
        "font-family:Example;src:url(a.woff2) format(woff2);src:url(a.eot) format(embedded-opentype)",
        "font-family:Example;src:url(a.eot);src:url(a.eot) format(embedded-opentype),url(b.woff2) format(woff2)",
    };
    for(size_t i=0;i<2;i++) {
        struct sheet sheet={0};pc.sh=&sheet;
        parse_font_face(&pc,bodies[i],bodies[i]+strlen(bodies[i]),NULL);
        const char *want=i?"https://font.test/path/b.woff2":"https://font.test/path/a.woff2";
        check(sheet.fonts&&sheet.fonts->sources&&!strcmp(sheet.fonts->sources->url,want),
              i?"later supported src replaces earlier source":"all unsupported src is ignored, not empty override");
        ar_free(&memory);
    }
}
#include "cssmediacalc_cases.h"
static void flex_shorthand_cases(void) {
    static const struct { const char *value; float grow, shrink, px, pct; bool valid; } cases[]={
        {"1 1 0",1,1,0,0,true}, {"2 3 0",2,3,0,0,true},
        {"0 0 0",0,0,0,0,true}, {"1",1,1,0,0,true},
        {"2 3",2,3,0,0,true}, {"6px 4 5",4,5,6,0,true},
        {"6px 4",4,1,6,0,true}, {"7% 8",8,1,0,7,true},
        {"1 1 calc(10px - 10px)",1,1,0,0,true},
        {"2 3 4",0,0,0,0,false}, {"1 auto 2",0,0,0,0,false},
        {"1 2 0 4",0,0,0,0,false}, {"5px 7%",0,0,0,0,false},
        {"9 none",0,0,0,0,false}, {"-1 1 0",0,0,0,0,false},
        {"1 -1 0",0,0,0,0,false}, {"1 2 calc(0)",0,0,0,0,false},
        {"1 2 calc(3 - 3)",0,0,0,0,false},
    };
    const struct propdef *p=css_prop_lookup("flex",4);
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        arena_t arena={0}; style_t style; css_style_init(&style,NULL);
        style.flex_grow=7; style.flex_shrink=8; style.flex_basis=(len_t){.kind=LK_LEN,.px=9};
        uint8_t *set=calloc((size_t)css_prop_count(),1);
        struct cx cx={.a=&arena,.s=&style,.set=set,.em=16,.rem=16,.vw=1920,.vh=1080};
        bool ok=css_apply(p,cases[i].value,strlen(cases[i].value),&cx);
        check(ok==cases[i].valid,cases[i].value);
        check(css_value_supported(p,cases[i].value,strlen(cases[i].value))==cases[i].valid,
              "supports and cascade share flex grammar");
        if(ok) check(style.flex_grow==cases[i].grow && style.flex_shrink==cases[i].shrink &&
                     style.flex_basis.kind==LK_LEN && style.flex_basis.px==cases[i].px &&
                     style.flex_basis.pct==cases[i].pct,"all flex components published");
        else check(style.flex_grow==7 && style.flex_shrink==8 && style.flex_basis.px==9,
                   "invalid shorthand preserves all prior components");
        free(set); ar_free(&arena);
    }
}
static void font_shorthand_cases(void) {
    static const char *const values[]={
        "600 1rem/1.5 -apple-system,BlinkMacSystemFont,\"Segoe UI\",\"Noto Sans\",Helvetica,Arial,sans-serif,\"Apple Color Emoji\",\"Segoe UI Emoji\"",
        "600 1rem / 1.5 sans-serif", "600 1rem/ 1.5 sans-serif",
        "600 1rem /1.5 sans-serif", "normal 1rem/1.5 sans-serif"
    };
    const struct propdef *p=css_prop_lookup("font",4);
    for(size_t i=0;i<sizeof values/sizeof *values;i++) {
        arena_t arena={0};style_t parent,style;css_style_init(&parent,NULL);parent.font_size=14;
        css_style_init(&style,&parent);uint8_t *set=calloc((size_t)css_prop_count(),1);
        struct cx cx={.a=&arena,.s=&style,.parent=&parent,.set=set,.em=14,.rem=16,.vw=1920,.vh=1080,.font_pass=true};
        bool size=css_apply(p,values[i],strlen(values[i]),&cx);cx.font_pass=false;cx.em=style.font_size;
        bool rest=css_apply(p,values[i],strlen(values[i]),&cx);
        printf("font shorthand: %s size_ok=%d rest_ok=%d size=%.3f weight=%u family=%s\n",
            values[i],size,rest,style.font_size,style.font_weight,style.font_names?style.font_names:"(none)");
        check(size&&rest&&style.font_size==16&&style.font_weight==(i==4?400:600),"font shorthand two-pass computed components");
        check(css_value_supported(p,values[i],strlen(values[i])),"font shorthand feature query uses same grammar");
        free(set);ar_free(&arena);
    }
}
/* A restyle that changes only values the children do not inherit leaves the
   children's borrowed inherited values valid: the new style takes them over. */
static void inherited_split_cases(void) {
    arena_t arena={0};style_t parent,was,now;css_style_init(&parent,NULL);parent.color=0xFF102030u;
    css_style_init(&was,&parent);was.font_names=ar_strdup(&arena,"serif");was.bg_color=0xFF0000FFu;was.display=D_BLOCK;
    now=was;now.font_names=ar_strdup(&arena,"serif");now.bg_color=0xFF00FF00u;
    css_style_adopt_inherited(&now,&was);
    check(now.font_names==was.font_names,"an adopted style borrows the retired style's inherited values");
    check(now.bg_color==0xFF00FF00u && now.display==D_BLOCK && now.color==0xFF102030u,"an adopted style keeps its own values");
    const struct propdef *border=css_prop_lookup("border-top-color",16),*color=css_prop_lookup("color",5);
    uint8_t *set=calloc((size_t)css_prop_count(),1);
    style_t child;css_style_init(&child,&now);
    struct cx cx={.a=&arena,.s=&child,.parent=&now,.set=set,.em=16,.rem=16,.vw=1920,.vh=1080};
    css_apply(color,"inherit",7,&cx);
    check(!child.explicit_inherit,"inheriting an inherited property reads only what css_style_init copies");
    css_apply(border,"inherit",7,&cx);
    check(child.explicit_inherit,"inheriting a property that is not inherited reads the parent's own value");
    style_t grandchild;css_style_init(&grandchild,&child);
    check(!grandchild.explicit_inherit,"the mark belongs to the style that read its parent");
    free(set);ar_free(&arena);
}
static void text_overflow_cases(void) {
    static const struct {const char *value;bool valid;uint8_t computed;} cases[]={
        {"clip",true,TO_CLIP},{"ellipsis",true,TO_ELLIPSIS},{"ELLIPSIS",true,TO_ELLIPSIS},
        {"initial",true,TO_CLIP},{"unset",true,TO_CLIP},{"inherit",true,TO_ELLIPSIS},
        {"clip ellipsis",false,0},{"\"...\"",false,0},{"fade",false,0},{"",false,0}
    };
    const struct propdef *p=css_prop_lookup("text-overflow",13);
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++) {
        arena_t a={0};style_t parent,style;css_style_init(&parent,NULL);parent.text_overflow=TO_ELLIPSIS;
        css_style_init(&style,&parent);check(style.text_overflow==TO_CLIP,"text-overflow is not inherited by default");
        style.text_overflow=TO_ELLIPSIS;uint8_t *set=calloc((size_t)css_prop_count(),1);
        struct cx cx={.s=&style,.parent=&parent,.a=&a,.set=set,.em=16,.rem=16,.vw=1920,.vh=1080};
        bool applied=css_apply(p,cases[i].value,strlen(cases[i].value),&cx);
        check(applied==cases[i].valid, cases[i].value);
        check(css_value_supported(p,cases[i].value,strlen(cases[i].value))==cases[i].valid,"Overflow3 grammar matches feature query");
        check(style.text_overflow==(applied?cases[i].computed:TO_ELLIPSIS),"valid publishes or invalid preserves previous overflow marker");
        free(set);ar_free(&a);
    }
}
static void placeholder_cases(void) {
    check(PE_PLACEHOLDER!=PE_SLOTTED,"placeholder is not the internal assigned-element selector marker");
    static const char *const selectors[]={"input::placeholder", "textarea::placeholder", ".hint:focus::placeholder"};
    arena_t a={0};struct pctx pc={.a=&a};
    for(unsigned i=0;i<sizeof selectors/sizeof *selectors;i++) {
        struct sellist *list=parse_sellist(&pc,selectors[i],selectors[i]+strlen(selectors[i]),false);
        check(list&&list->n==1&&list->v[0]->pseudo==PE_PLACEHOLDER,"placeholder selector retains real pseudo kind");
    }
    pc.supports_probe=true;
    const char *valid="input::placeholder",*invalid="input::placeholder(foo)";
    check(parse_sellist(&pc,valid,valid+strlen(valid),false)!=NULL,"supports selector accepts implemented placeholder");
    check(parse_sellist(&pc,invalid,invalid+strlen(invalid),false)==NULL,"placeholder function syntax rejected");
    style_t parent,child,pseudo;css_style_init(&parent,NULL);css_style_init(&pseudo,&parent);
    parent.placeholder=&pseudo;parent.color=RGB(10,20,30);pseudo.color=0;pseudo.opacity=1;
    css_style_init(&child,&parent);
    check(!child.placeholder,"placeholder pointer is not inherited by children");
    check(control_text_color(&parent,false)==parent.color,"nonempty input value uses originating control colour");
    check(control_text_color(&parent,true)==0,"author transparent placeholder suppresses native text");
    pseudo.color=RGB(50,60,70);pseudo.opacity=0;
    check(control_text_color(&parent,true)==0,"author opacity zero suppresses only placeholder text");
    pseudo.opacity=.5f;
    check(control_text_color(&parent,true)==0x80323c46u,"placeholder opacity multiplies colour alpha once");
    parent.placeholder=NULL;
    check(control_text_color(&parent,true)==RGB(117,117,117),"no authored pseudo retains UA grey colour");
    web_doc d={0};node_t node={0};new_pool(&d);
    node.style=fresh_style(&d,&node,NULL);
    node.style->placeholder=fresh_style(&d,&node,node.style);
    style_t *old=node.style;struct cascade cascade={.d=&d};retire_style(&cascade,&node);
    node.style=fresh_style(&d,&node,NULL);node.style->placeholder=fresh_style(&d,&node,node.style);
    check(d.style_pool->live==4&&old->placeholder,"placeholder old generation stays alive until publication join");
    css_styles_release(&d);
    check(d.style_pool->live==2&&!node.style_previous_slots,"joined generation returns old placeholder slot");
    css_node_style_free(&node);check(d.style_pool->live==0,"originating node cleanup returns placeholder slot");
    css_style_doc_free(&d);
    style_t origin,body,descendant;css_style_init(&origin,NULL);css_style_init(&body,&origin);
    node_t element={.type=N_ELEM,.tag=T_div,.style=&origin};
    body.content="";
    check(box_pseudo_generated(&element,&origin,&body),"empty content generates a real pseudo box");
    body.content=NULL;
    check(!box_pseudo_generated(&element,&origin,&body),"content none retains only computed body");
    body.content="hint";body.display=D_NONE;
    check(!box_pseudo_generated(&element,&origin,&body),"display none body does not generate");
    body.display=D_INLINE;origin.display=D_NONE;
    check(!box_pseudo_generated(&element,&origin,&body),"hidden origin does not generate pseudo boxes");
    origin.display=D_BLOCK;element.tag=T_input;
    check(!box_pseudo_generated(&element,&origin,&body),"replaced input does not generate before or after");
    element.tag=T_button;
    check(box_pseudo_generated(&element,&origin,&body),"native button preserves pseudo children");
    origin.display=D_FLEX;
    check(box_pseudo_generated(&element,&origin,&body),"flex button preserves pseudo children");
    element.tag=T_object;
    check(box_pseudo_generated(&element,&origin,&body),"object fallback preserves pseudo children");
    origin.before_generated=origin.after_generated=true;css_style_init(&descendant,&origin);
    check(!descendant.before_generated&&!descendant.after_generated,"pseudo generation flags do not inherit");
    box_t root={.w=800,.h=400,.content_dy=3,.layout_post_valid=true,.cached_font_generation=3,
        .layout_post_w=800,.layout_post_h=400,.layout_post_content_dy=3};
    web_doc document={.root_box=&root,.width=800,.height=600};
    check(committed_geometry_current(&document,800,600,3),"published post geometry survives absent inner formatting cache");
    root.layout_dirty=true;check(!committed_geometry_current(&document,800,600,3),"dirty geometry cannot be republished by style-only join");root.layout_dirty=false;
    root.layout_post_valid=false;check(!committed_geometry_current(&document,800,600,3),"new or incomplete box cannot reuse post geometry");root.layout_post_valid=true;
    check(!committed_geometry_current(&document,800,600,4),"font metric generation change requires geometry");
    check(!committed_geometry_current(&document,900,600,3),"viewport width change requires geometry");
    check(!committed_geometry_current(&document,800,700,3),"viewport height change requires geometry");
    root.w++;check(!committed_geometry_current(&document,800,600,3),"post width mismatch cannot reuse geometry");root.w--;
    root.h++;check(!committed_geometry_current(&document,800,600,3),"post height mismatch cannot reuse geometry");root.h--;
    root.content_dy++;check(!committed_geometry_current(&document,800,600,3),"post content shift mismatch requires geometry");root.content_dy--;
    document.native_recovery_pending=true;check(!committed_geometry_current(&document,800,600,3),"recovery cannot short circuit coherent geometry reconstruction");
    ar_free(&a);
}
static void local_box_cases(void){
    style_t style;css_style_init(&style,NULL);css_style_finish(&style,NULL,false);style.display=D_BLOCK;
    style_t pre=style;pre.white_space=WS_PRE;
    box_t parent={.kind=B_BLOCK,.st=&style},flex={.kind=B_FLEX,.st=&style},row={.kind=B_ROW,.st=&style};
    box_t block={.kind=B_BLOCK,.st=&style,.parent=&parent},grid={.kind=B_GRID,.st=&style,.parent=&parent};
    box_t inline_box={.kind=B_INLINE,.st=&style,.parent=&parent};
    style_t inline_block=style;inline_block.display=D_INLINE_BLOCK;
    box_t atomic={.kind=B_ATOMIC,.atomic=AT_INLINE_BLOCK,.st=&inline_block,.parent=&parent};
    check(local_same_place(&block,&grid),"block and grid boxes take the same place in a block parent");
    check(local_same_place(&inline_box,&atomic),"inline and atomic inline boxes share one anonymous run");
    check(!local_same_place(&block,&inline_box),"a block turning inline needs its parent's fixup");
    box_t floated=block;floated.floated=true;
    check(!local_same_place(&block,&floated),"floating changes the run a box belongs to");
    box_t abspos=block;abspos.abspos=true;
    check(!local_same_place(&block,&abspos),"leaving the flow changes the run a box belongs to");
    box_t cell={.kind=B_CELL,.st=&style,.parent=&row},caption={.kind=B_CAPTION,.st=&style,.parent=&row},again=cell;
    check(!local_same_place(&cell,&caption),"a table part keeps its kind");
    check(local_same_place(&cell,&again),"a cell regenerated as a cell keeps its row");
    check(!inline_content(&flex,&abspos),"an absolutely positioned flex child is not wrapped");
    check(inline_content(&parent,&inline_box),"inline content beside blocks needs an anonymous block");
    check(!inline_content(&parent,&floated),"a float alone is not wrapped");
    check(!inline_content(&parent,&grid),"a block is not inline content");
    node_t text={.type=N_TEXT,.text=" \n\t ",.textlen=4};
    check(local_collapses(&text,&style),"white space alone collapses away between blocks");
    check(!local_collapses(&text,&pre),"preserved white space keeps its box");
    text.text=" a ";text.textlen=3;
    check(!local_collapses(&text,&style),"text keeps its box");
    style_t numbered=style,bullet=style;
    numbered.display=bullet.display=D_LIST_ITEM;numbered.list_style=LS_DECIMAL;bullet.list_style=LS_DISC;
    web_doc d={0};
    node_t ul={.type=N_ELEM,.tag=T_ul,.owner=&d,.style=&style};
    node_t first={.type=N_ELEM,.tag=T_li,.owner=&d,.style=&bullet,.parent=&ul};
    node_t second={.type=N_ELEM,.tag=T_li,.owner=&d,.style=&bullet,.parent=&ul};
    ul.first=&first;first.next=&second;ul.last=&second;
    check(!local_list_scope(&ul,&second),"bullets do not count their items");
    first.style=&numbered;
    check(local_list_scope(&ul,&second)==&ul,"an item beside a numbered one renumbers the list");
    first.style=&bullet;
    node_t ol={.type=N_ELEM,.tag=T_ol,.owner=&d,.style=&style,.parent=&second};
    node_t nested={.type=N_ELEM,.tag=T_li,.owner=&d,.style=&numbered,.parent=&ol};
    ol.first=ol.last=&nested;second.first=second.last=&ol;
    check(!local_list_scope(&second,&ol),"a nested list owns its own items");
    node_t div={.type=N_ELEM,.tag=T_div,.owner=&d,.style=&style};
    check(!local_list_scope(&div,&div),"no item, no list to renumber");
    second.first=second.last=NULL;second.style=NULL;
    box_t retiring={.kind=B_BLOCK,.st=&numbered,.node=&second};second.box=&retiring;
    check(local_list_scope(&ul,&second)==&ul,"a removed numbered item renumbers its former list");
}
int main(int argc, char **argv) {
    if(argc>1 && !strcmp(argv[1],"local-boxes"))local_box_cases();
    else
    if(argc>1 && !strcmp(argv[1],"placeholder"))placeholder_cases();
    else
    if(argc>1 && !strcmp(argv[1],"text-overflow"))text_overflow_cases();
    else
    if(argc>1 && !strcmp(argv[1],"font-shorthand"))font_shorthand_cases();
    else
    if(argc>1 && !strcmp(argv[1],"flex"))flex_shorthand_cases();
    else
    if(argc>1 && !strcmp(argv[1],"media-calc"))media_query_cases();
    else
    if(argc>1 && !strcmp(argv[1],"fonts"))font_sources_native();
    else
    if (argc>1 && !strcmp(argv[1],"second")) { pseudo_generations(); cancelled_tail(); allocation_failures(); }
    else { fixed_styles(); adopted_style(); declaration_cache(); inherited_split_cases(); }
    printf("cssperformance: %d checks, %d failures\n", checks, failed); return failed ? 1 : 0;
}
