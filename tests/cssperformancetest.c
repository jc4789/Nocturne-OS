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
int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1],"second")) { pseudo_generations(); cancelled_tail(); allocation_failures(); }
    else { fixed_styles(); adopted_style(); declaration_cache(); }
    printf("cssperformance: %d checks, %d failures\n", checks, failed); return failed ? 1 : 0;
}
