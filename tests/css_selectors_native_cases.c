/* tests/check_css_selectors.py の道具。css.c をそのまま取り込んだ後に置かれ、dom.c の
   節と属性の作り方で文書を組み、選択子の解析・直列化・照合を製品の関数で答える。
   OLD_SOURCE のときは変更前の製品の口（selectorText は書いたままの文字、querySelector は
   css_select）で同じ問いに答える。

   問いは一行に一つ、欄はタブで区切り、\\ \t \n \r \0 で逃がす。答えも同じ形で一行。
     R <sheet>                       sheet を解析：単一の規則か、規則の数、各規則の型・selectorText・cssText
     W <rule> <selector>             <rule> の一つの style 規則の selectorText に <selector> を入れた後の selectorText
     Q <selector>                    HTML の文書の querySelector が受け付けるか（1/0）
     S <condition>                   CSS.supports(condition) の一引数の形（1/0）
     D <mode> <chain> <attrs> <sheet> <selector>
                                     mode（html・quirks・xml）の文書に chain の要素を組み、
                                     sheet の規則の数と、規則のどれかが要素に当たるか、
                                     querySelector(selector) の最初が要素か（E）・null（N）・他（O）・無効（!）、
                                     要素.matches(selector)（1/0/!）
   chain は body の下の要素を / で並べ、各々は [ns:][prefix|]local[@name=value;...]（ns は svg・mathml・none）。
   最後の要素が問いの要素で、attrs（ns \x1f name \x1f value を \x1e で並べる）を setAttributeNS で付ける。 */
#include <stdio.h>

/* ---- the host's ends of what building a document reaches (the rest stop the tool by name) */
void doc_dom_budget(web_doc *d) { (void)d; }                /* no quota on a test document */
size_t doc_dom_remaining(web_doc *d) { (void)d; return SIZE_MAX; }
void doc_image_node_owned(node_t *n) { (void)n; }           /* no image loads */
void web_js_nodes_changed(web_doc *d) { (void)d; }          /* no script realm */
bool web_js_enabled(web_doc *d) { (void)d; return false; }
void web_js_console(web_doc *d, int level, const char *message) { (void)d; (void)level; (void)message; }
void html_bridge_native_changed(node_t *node, unsigned flags) { (void)node; (void)flags; }
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
bool parallel_active(void) { return false; }
unsigned parallel_worker_index(void) { return 0; }
uint64_t uptime_ms(void) { return 0; }
static void *thread_slots[64];
void *thread_local_get(unsigned slot) { return slot < 64 ? thread_slots[slot] : NULL; }
void thread_local_set(unsigned slot, void *value) { if (slot < 64) thread_slots[slot] = value; }
node_t *doc_active_target(web_doc *d) { (void)d; return NULL; } /* no pointer is pressed */
/* no element here has ElementInternals */
bool web_custom_state_has(const node_t *n, const char *name) { (void)n; (void)name; return false; }
/* what an insertion or an attribute change tells the subsystems a test document has none of:
   script wrappers, boxes, dialogs, frames, details, select, canvas, CSSOM owners */
bool web_js_nodes_same_forest(web_doc *d, node_t *node, node_t *parent) { (void)d; (void)node; (void)parent; return true; }
void web_js_nodes_reparented(web_doc *d, node_t *subtree) { (void)d; (void)subtree; }
void web_js_link_changed(web_doc *d) { (void)d; }
void web_js_window_names_changed(web_doc *d) { (void)d; }
void web_dialog_removed(web_doc *d, node_t *subtree) { (void)d; (void)subtree; }
void web_dialog_sync(web_doc *d) { (void)d; }
void web_frames_detach_tree(web_doc *d, node_t *root) { (void)d; (void)root; }
void doc_details_inserted(node_t *subtree) { (void)subtree; }
void doc_details_attribute_changed(web_doc *d, node_t *details, const char *name, bool old_present) {
    (void)d; (void)details; (void)name; (void)old_present;
}
void web_select_inserted(web_doc *d, node_t *subtree, node_t *old_parent) { (void)d; (void)subtree; (void)old_parent; }
void web_select_sync(web_doc *d, node_t *select, bool reset) { (void)d; (void)select; (void)reset; }
void web_select_attribute_changed(web_doc *d, node_t *node, const char *name, bool present) {
    (void)d; (void)node; (void)name; (void)present;
}
void web_canvas_attr_changed(node_t *node, const char *name) { (void)node; (void)name; }
/* no form control is built here: an attribute change asks every element, and reads
   the answers only for controls */
static void no_control(const node_t *node) {
    if (!node->foreign && (node->tag == T_input || node->tag == T_textarea || node->tag == T_select)) {
        puts("unreached: form control");
        exit(3);
    }
}
bool doc_control_selection_supported(const node_t *node) { no_control(node); return false; }
enum web_input_kind web_input_type(const node_t *n) { no_control(n); return WEB_INPUT_TEXT; }
void boxes_note_child_list(web_doc *d, node_t *parent, node_t *subtree) { (void)d; (void)parent; (void)subtree; }
void boxes_note_foreign(web_doc *d, node_t *n) { (void)d; (void)n; }
void boxes_note_regen(web_doc *d, node_t *n) { (void)d; (void)n; }
void boxes_require_full(web_doc *d, const char *reason) { (void)d; (void)reason; }
void layout_invalidate(box_t *b) { (void)b; }
void cssom_style_lifecycle(node_t *subtree) { (void)subtree; }
void cssom_style_text_changed(node_t *node) { (void)node; }
void doc_referrer_meta(web_doc *d, node_t *node) { (void)d; (void)node; }
uint32_t doc_utf16_length(const char *text) {
    uint32_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if ((*p & 0xC0) != 0x80) n += *p >= 0xF0 ? 2 : 1;
    return n;
}

static char *unescape(const char *s, size_t n, size_t *out) {
    char *b = malloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\\' || i + 1 == n) { b[k++] = s[i]; continue; }
        char c = s[++i];
        b[k++] = c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c == '0' ? 0 : c;
    }
    b[k] = 0;
    *out = k;
    return b;
}
static void put_escaped(FILE *f, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\') fputs("\\\\", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (!c) fputs("\\0", f);
        else fputc(c, f);
    }
}

/* ---- the document */
static node_t *append(web_doc *d, node_t *parent, node_t *child) {
    if (!child || !doc_node_move(d, parent, child, NULL)) { puts("挿入できない"); exit(2); }
    return child;
}
static web_doc *document(int mode, node_t **body) {
    web_doc *d = calloc(1, sizeof *d);
    d->xml = mode == 2;
    d->quirks = mode == 1;
    d->document_mode = mode == 1 ? 1 : 0;
    d->live = true;
    d->root = doc_node_create(d, N_DOC, NULL, NULL, 0);
    d->root->owner = d;
    node_t *html = append(d, d->root, doc_element_create(d, WEB_NS_HTML, NULL, "html"));
    d->html = html;
    append(d, html, doc_element_create(d, WEB_NS_HTML, NULL, "head"));
    *body = append(d, html, doc_element_create(d, WEB_NS_HTML, NULL, "body"));
    return d;
}
static const char *namespace_of(const char *name, size_t n) {
    if (n == 3 && !memcmp(name, "svg", 3)) return WEB_NS_SVG;
    if (n == 6 && !memcmp(name, "mathml", 6)) return WEB_NS_MATHML;
    if (n == 4 && !memcmp(name, "none", 4)) return NULL;
    return WEB_NS_HTML;
}
/* chain: [ns:]local[@name=value;...] items separated by '/' */
static node_t *build_chain(web_doc *d, node_t *parent, const char *chain) {
    node_t *last = parent;
    for (const char *p = chain; *p;) {
        const char *end = strchr(p, '/');
        if (!end) end = p + strlen(p);
        const char *at = memchr(p, '@', (size_t)(end - p)), *name_end = at ? at : end;
        const char *colon = memchr(p, ':', (size_t)(name_end - p));
        const char *ns = colon ? namespace_of(p, (size_t)(colon - p)) : WEB_NS_HTML;
        const char *local_start = colon ? colon + 1 : p;
        /* prefix|local: createElementNS with a prefixed qualified name */
        const char *bar = memchr(local_start, '|', (size_t)(name_end - local_start));
        char *prefix = bar ? strndup(local_start, (size_t)(bar - local_start)) : NULL;
        if (bar) local_start = bar + 1;
        char *local = strndup(local_start, (size_t)(name_end - local_start));
        node_t *e = append(d, last, doc_element_create(d, ns, prefix, local));
        free(local); free(prefix);
        for (const char *a = at ? at + 1 : NULL; a && a < end;) {
            const char *stop = memchr(a, ';', (size_t)(end - a));
            if (!stop) stop = end;
            const char *eq = memchr(a, '=', (size_t)(stop - a));
            char *n = strndup(a, (size_t)((eq ? eq : stop) - a)), *v = eq ? strndup(eq + 1, (size_t)(stop - eq - 1)) : strdup("");
            doc_attr_set_ns(d, e, NULL, NULL, n, v);
            free(n); free(v);
            a = stop + 1;
        }
        last = e;
        p = *end ? end + 1 : end;
    }
    return last;
}
static void set_attributes(web_doc *d, node_t *e, const char *attrs) {
    for (const char *p = attrs; *p;) {
        const char *end = strchr(p, '\x1e');
        if (!end) end = p + strlen(p);
        const char *f1 = memchr(p, '\x1f', (size_t)(end - p)), *f2 = f1 ? memchr(f1 + 1, '\x1f', (size_t)(end - f1 - 1)) : NULL;
        if (f2) {
            char *ns = strndup(p, (size_t)(f1 - p)), *qualified = strndup(f1 + 1, (size_t)(f2 - f1 - 1)),
                 *value = strndup(f2 + 1, (size_t)(end - f2 - 1));
            char *colon = strchr(qualified, ':'), *prefix = NULL, *local = qualified;
            if (colon) { *colon = 0; prefix = qualified; local = colon + 1; }
            doc_attr_set_ns(d, e, *ns ? ns : NULL, prefix, local, value);
            free(ns); free(qualified); free(value);
        }
        p = *end ? end + 1 : end;
    }
}

/* ---- the product's answers */
static arena_t scratch;
static sheet_t *parse(const char *text, size_t n) {
    ar_free(&scratch);
    memset(&scratch, 0, sizeof scratch);
    return css_parse_sheet(&scratch, text, n, "about:blank", 0, NULL);
}
static void selector_text(const struct css_rule_info *r, sbuf *b) {
#ifdef OLD_SOURCE
    if (r->selector) sb_puts(b, r->selector);
#else
    css_rule_selector_serialize(r, b);
#endif
}
static void css_text(const struct css_rule_info *r, sbuf *b) {
#ifdef OLD_SOURCE
    sb_put(b, r->text, r->text_len);
#else
    css_rule_text_serialize(r, b);
#endif
}
static bool query(web_doc *d, node_t *scope, const char *s, size_t n, pvec *out) {
#ifdef OLD_SOURCE
    (void)n;
    return css_select(d, scope, s, out);
#else
    return css_select_n(d, scope, s, n, out);
#endif
}
static bool matches(node_t *e, const char *s, size_t n, bool *valid) {
#ifdef OLD_SOURCE
    (void)n;
    return css_matches(e, s, valid);
#else
    return css_matches_n(e, NULL, s, n, valid);
#endif
}
static void answer_sheet(FILE *f, const char *text, size_t n) {
    sheet_t *sh = parse(text, n);
    uint32_t count;
    struct css_rule_info *r = css_sheet_rules(sh, &count);
    fprintf(f, "%d\t%u", css_sheet_single_rule(sh, text, n), count);
    for (; r; r = r->next) {
        sbuf b = {0};
        fprintf(f, "\t%u\t", r->type);
        if (r->type == 1) selector_text(r, &b);
        put_escaped(f, b.p ? b.p : "", b.n);
        b.n = 0;
        css_text(r, &b);
        fputc('\t', f);
        put_escaped(f, b.p ? b.p : "", b.n);
        sb_free(&b);
    }
}
static void answer_set(FILE *f, const char *rule, size_t n, const char *selector, size_t sn) {
    sheet_t *sh = parse(rule, n);
    struct css_rule_info *r = css_sheet_rules(sh, NULL);
    if (!r || r->type != 1) { fputs("!", f); return; }
#ifdef OLD_SOURCE
    /* before: CSSStyleRule had no selectorText setter, so the value stays */
    (void)selector; (void)sn;
    sbuf b = {0};
    selector_text(r, &b);
    put_escaped(f, b.p ? b.p : "", b.n);
    sb_free(&b);
#else
    size_t length;
    char *names = NULL, *text = css_rule_with_selector(sh, r, selector, sn, &length, &names);
    if (!text) {
        sbuf b = {0};
        selector_text(r, &b);
        put_escaped(f, b.p ? b.p : "", b.n);
        sb_free(&b);
        return;
    }
    /* the sheet is rebuilt from the rule's new text, as cssom_rule_selector does */
    sheet_t *again = parse(text, length);
    struct css_rule_info *x = css_sheet_rules(again, NULL);
    sbuf b = {0};
    if (x) selector_text(x, &b);
    put_escaped(f, b.p ? b.p : "", b.n);
    sb_free(&b);
    free(text); free(names);
#endif
}
static void answer_document(FILE *f, int mode, const char *chain, const char *attrs, const char *sheet_text,
                            size_t sheet_n, const char *selector, size_t selector_n) {
    node_t *body;
    web_doc *d = document(mode, &body);
    node_t *e = build_chain(d, body, chain);
    set_attributes(d, e, attrs);
    uint32_t count = 0;
    bool cascade = false;
    if (sheet_n) {
        sheet_t *sh = parse(sheet_text, sheet_n);
        css_sheet_rules(sh, &count);
        for (struct rule *r = sh->first; r && !cascade; r = r->next)
            for (int i = 0; r->sel && i < r->sel->n && !cascade; i++)
                if (!r->sel->v[i]->pseudo) cascade = match_from(r->sel->v[i]->right, e, NULL);
    }
    fprintf(f, "%u\t%d\t", count, cascade);
    if (!selector_n) { fputs("-\t-", f); return; }
    pvec out = {0};
    bool valid = query(d, d->root, selector, selector_n, &out);
    fputs(!valid ? "!" : !out.n ? "N" : out.v[0] == e ? "E" : "O", f);
    pv_free(&out);
    bool ok;
    bool m = matches(e, selector, selector_n, &ok);
    fputs(!ok ? "\t!" : m ? "\t1" : "\t0", f);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *outf = fopen(argv[2], "wb");
    if (!in || !outf) return 2;
    node_t *body;
    web_doc *html = document(0, &body);
    for (;;) {
        sbuf l = {0};
        int c;
        while ((c = fgetc(in)) != EOF && c != '\n') sb_putc(&l, (char)c);
        if (c == EOF && !l.n) { sb_free(&l); break; }
        char *fields[8] = {0};
        size_t lengths[8] = {0};
        int nf = 0;
        const char *p = l.p ? l.p : "", *e = p + l.n;
        while (nf < 8) {
            const char *t = memchr(p, '\t', (size_t)(e - p));
            const char *stop = t ? t : e;
            fields[nf] = unescape(p, (size_t)(stop - p), &lengths[nf]);
            nf++;
            if (!t) break;
            p = t + 1;
        }
        const char *kind = fields[0];
        if (!strcmp(kind, "R") && nf >= 2) answer_sheet(outf, fields[1], lengths[1]);
        else if (!strcmp(kind, "W") && nf >= 3) answer_set(outf, fields[1], lengths[1], fields[2], lengths[2]);
        else if (!strcmp(kind, "Q") && nf >= 2) {
            pvec out = {0};
            fputs(query(html, html->root, fields[1], lengths[1], &out) ? "1" : "0", outf);
            pv_free(&out);
        } else if (!strcmp(kind, "S") && nf >= 2) fputs(css_supports_condition(fields[1], lengths[1], true) ? "1" : "0", outf);
        else if (!strcmp(kind, "D") && nf >= 6) {
            int mode = !strcmp(fields[1], "quirks") ? 1 : !strcmp(fields[1], "xml") ? 2 : 0;
            answer_document(outf, mode, fields[2], fields[3], fields[4], lengths[4], fields[5], lengths[5]);
        } else fputs("?", outf);
        fputc('\n', outf);
        fflush(outf);
        for (int i = 0; i < nf; i++) free(fields[i]);
        sb_free(&l);
    }
    fclose(in);
    fclose(outf);
    return 0;
}
