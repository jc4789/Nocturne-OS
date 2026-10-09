/* Real apply_decl/subst plus real cssprop longhand/shorthand parsing.
   Counters observe work; no CSS result or lookup is replaced. */
static int checks, failures;
static void check(bool ok, const char *name) {
    checks++; printf("%s %s\n", ok ? "OK" : "FAIL", name);
    if (!ok) failures++;
    fflush(NULL);
}
static void counters(void) { walk_allocs = subst_calls = var_probes = 0; }
static const struct propdef *property(const char *name) { return css_prop_lookup(name, strlen(name)); }
static void apply(struct cascade *c, struct cx *cx, const char *name, const char *value) {
    struct decl d = {property(name), NULL, value, false};
    if (!d.p) { check(false, "fixture property exists"); return; }
    apply_decl(c, cx, &d);
}
static bool set(struct cx *cx, const char *name) { return cx->set[css_prop_index(property(name))] != 0; }
static void reset(style_t *s, struct cx *cx, uint8_t *bits, arena_t *a) {
    css_style_init(s, NULL);
    memset(bits, 0, (size_t)css_prop_count());
    *cx = (struct cx){s, NULL, NULL, 16, 16, 800, 600, a, bits, false, false, false, 0};
    counters(); fail_calloc = 0;
}
int main(void) {
    struct cascade c = {0}; arena_t arena = {0}; style_t s;
    /* Initialize/sort the real property table before taking property indices. */
    (void)property("width");
    uint8_t *bits = calloc((size_t)css_prop_count(), 1);
    struct cx cx;
    if (!bits) return 2;
    enum { COUNT = 256 };
    struct custom_prop vars[COUNT]; char names[COUNT][24];
    for (int i = 0; i < COUNT; i++) {
        snprintf(names[i], sizeof names[i], "--candidate%d", i);
        vars[i] = (struct custom_prop){names[i], "3px", i + 1 < COUNT ? &vars[i + 1] : NULL};
    }
    vars[COUNT - 1].name = "--target";
    reset(&s, &cx, bits, &arena); s.vars = vars;
    apply(&c, &cx, "width", "17px"); counters();
    for (int i = 0; i < 1024; i++) apply(&c, &cx, "width", "var(--target)");
    check(s.width.kind == LK_LEN && s.width.px == 17 && set(&cx, "width"), "winner retained across 1024 losing declarations");
    check(subst_calls == 0 && walk_allocs == 0 && var_probes == 0, "losing longhands perform no substitution allocation or var traversal");
    apply(&c, &cx, "height", "var(--target)");
    check(s.height.kind == LK_LEN && s.height.px == 3 && set(&cx, "height"), "unrelated needed longhand still resolves its custom value");
    check(subst_calls == 1 && walk_allocs == 2 && var_probes == COUNT, "needed longhand uses the actual complete linked lookup");
    counters(); fail_calloc = 1;
    apply(&c, &cx, "width", "var(--target)");
    check(fail_calloc == 1 && subst_calls == 0 && walk_allocs == 0 && s.width.px == 17, "losing declaration cannot consume an allocation failure");
    fail_calloc = 0;
    struct custom_prop cycle = {"--cycle", "var(--cycle)", NULL}; s.vars = &cycle;
    apply(&c, &cx, "width", "var(--cycle)");
    check(subst_calls == 0 && var_probes == 0 && s.width.px == 17, "unused cyclic value is never evaluated");

    reset(&s, &cx, bits, &arena); s.vars = vars;
    apply(&c, &cx, "width", "nonsense");
    check(!set(&cx, "width") && s.width.kind == LK_AUTO, "invalid ordinary longhand leaves winner bit unset");
    apply(&c, &cx, "width", "var(--target)");
    check(set(&cx, "width") && s.width.px == 3 && subst_calls == 1, "unset bit cannot hide a subsequent valid declaration");
    reset(&s, &cx, bits, &arena); s.vars = vars; fail_calloc = 1;
    apply(&c, &cx, "width", "var(--target)");
    check(!set(&cx, "width") && s.width.kind == LK_AUTO && subst_calls == 1 && walk_allocs == 1, "needed scratch OOM preserves initial value and unset bit");

    struct custom_prop space = {"--space", "4px 6px", NULL};
    reset(&s, &cx, bits, &arena); s.vars = &space;
    apply(&c, &cx, "margin-top", "10px"); counters();
    apply(&c, &cx, "margin", "var(--space)");
    check(s.margin[0].px == 10 && s.margin[1].px == 6 && s.margin[2].px == 4 && s.margin[3].px == 6, "partial shorthand supplies missing sides without replacing top winner");
    check(subst_calls == 1 && !set(&cx, "margin") && set(&cx, "margin-left"), "shorthand itself is not a winning-longhand marker");
    counters(); apply(&c, &cx, "margin", "var(--space)");
    check(subst_calls == 1 && s.margin[0].px == 10 && s.margin[1].px == 6, "even fully supplied shorthand remains conservatively evaluated");
    reset(&s, &cx, bits, &arena);
    apply(&c, &cx, "margin-left", "12px");
    apply(&c, &cx, "margin", "var(--absent, 2px 5px)");
    check(s.margin[0].px == 2 && s.margin[1].px == 5 && s.margin[2].px == 2 && s.margin[3].px == 12, "partial shorthand keeps real variable fallback semantics");

    struct custom_prop font = {"--font", "20px", NULL};
    reset(&s, &cx, bits, &arena); s.vars = &font; cx.font_pass = true;
    apply(&c, &cx, "font-size", "var(--font)");
    check(s.font_size == 20 && set(&cx, "font-size") && subst_calls == 1, "font first pass resolves font-size and establishes its bit");
    cx.font_pass = false; cx.em = s.font_size; counters();
    apply(&c, &cx, "font-size", "var(--font)");
    check(s.font_size == 20 && subst_calls == 0 && walk_allocs == 0, "font second pass skips resolved longhand substitution");
    font.value = "italic 700 18px sans-serif";
    reset(&s, &cx, bits, &arena); s.vars = &font; cx.font_pass = true;
    apply(&c, &cx, "font", "var(--font)");
    check(s.font_size == 18 && set(&cx, "font-size") && !set(&cx, "font") && !set(&cx, "font-weight"), "font shorthand first pass does not mark unprocessed longhands");
    cx.font_pass = false; cx.em = s.font_size; counters();
    apply(&c, &cx, "font", "var(--font)");
    check(subst_calls == 1 && s.font_size == 18 && s.font_weight == 700 && s.font_style == 1 && s.font_family == FONT_FAMILY_SANS, "font shorthand second pass resolves remaining font longhands");

    reset(&s, &cx, bits, &arena); s.vars = vars;
    apply(&c, &cx, "width", "11px");
    style_t pseudo; css_style_init(&pseudo, &s);
    memset(bits, 0, (size_t)css_prop_count()); cx.s = &pseudo; cx.parent = &s; counters();
    apply(&c, &cx, "width", "var(--target)");
    check(pseudo.width.px == 3 && s.width.px == 11 && subst_calls == 1, "per-compute reset keeps base and pseudo winners independent");
    check(pseudo.vars == s.vars && vars[COUNT - 1].value && !strcmp(vars[COUNT - 1].value, "3px"), "consuming inherited computed values does not mutate the shared list");
    memset(bits, 0, (size_t)css_prop_count()); vars[COUNT - 1].value = "9px"; counters();
    apply(&c, &cx, "width", "var(--target)");
    check(pseudo.width.px == 9 && subst_calls == 1, "new compute reset observes a changed custom value without a stale cache");
    struct custom_prop lower = {"--case", "7px", NULL}, upper = {"--Case", "8px", &lower};
    reset(&s, &cx, bits, &arena); s.vars = &upper;
    apply(&c, &cx, "width", "var(--case)");
    check(s.width.px == 7 && var_probes == 2, "needed lookup remains case-sensitive");
    struct custom_prop empty = {"--empty", "", NULL};
    reset(&s, &cx, bits, &arena); s.vars = &empty;
    apply(&c, &cx, "width", "var(--empty, 8px)");
    check(!set(&cx, "width") && s.width.kind == LK_AUTO, "valid empty custom value still suppresses its fallback");
    sb_free(&c.vbuf); ar_free(&arena); free(bits);
    printf("cascade-winner23-native: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
