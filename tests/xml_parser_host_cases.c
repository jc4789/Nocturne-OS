/* Cases for tests/check_xml_parser.py: the HTML "XML parser" (xml.c over
   Expat) into the shim tree. WPT's domparsing and xhr directories hold the
   page-level checks; these hold the native contract and its failure paths. */
static int checks, failures;
#define CHECK(condition, label) do { checks++; if (!(condition)) { failures++; printf("FAIL %d: %s\n", __LINE__, label); } } while (0)

static web_doc *new_doc(void) {
    web_doc *d = shim_doc();
    if (!d) abort();
    d->xml = true;
    d->root = doc_node_create(d, N_DOC, NULL, "", 0);
    if (!d->root) abort();
    return d;
}
static enum xml_parse_result parse(web_doc *d, const char *s, size_t n, bool string, const char *label, struct xml_parse_error *e) {
    return xml_parse_document(d, d, s, n, string, label, e);
}
static enum xml_parse_result parse_string(web_doc *d, const char *s) { return parse(d, s, strlen(s), true, NULL, NULL); }
static node_t *child(node_t *n, int i) { node_t *c = n ? n->first : NULL; while (c && i--) c = c->next; return c; }
static int count(node_t *n) { int k = 0; for (node_t *c = n ? n->first : NULL; c; c = c->next) k++; return k; }
static bool same(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }
static const char *ns(const node_t *e) {
    switch (e->namespace_id) {
    case NS_HTML: return WEB_NS_HTML; case NS_SVG: return WEB_NS_SVG; case NS_MATHML: return WEB_NS_MATHML;
    case NS_OTHER: return e->namespace_uri; default: return NULL;
    }
}
static bool element(const node_t *e, const char *namespace_uri, const char *prefix, const char *local) {
    return e && e->type == N_ELEM && same(ns(e), namespace_uri) && same(e->prefix, prefix) && same(e->local_name, local);
}
static bool text(const node_t *t, const char *s) { return t && t->type == N_TEXT && t->textlen == strlen(s) && !memcmp(t->text, s, t->textlen); }
static bool attribute(const node_t *e, int i, const char *raw, const char *namespace_uri, const char *prefix, const char *local, const char *value) {
    if (!e || i >= e->nattrs) return false;
    const struct attr *a = &e->attrs[i];
    return same(a->raw, raw) && same(a->namespace_uri, namespace_uri) && same(a->prefix, prefix) && same(a->local, local) && same(a->value, value);
}

static void well_formed(void) {
    web_doc *d = new_doc();
    const char *s = "<?xml version='1.0'?><!-- c0 --><r xmlns='urn:a' xmlns:p='urn:p' p:x='1' y='2' xml:lang='ja'>"
                    "<p:k/>t&amp;&#x41;<![CDATA[c<d]]><![CDATA[]]><!--c1--><?pi data?></r><?after?>";
    CHECK(parse_string(d, s) == XML_PARSED, "well-formed document parses");
    node_t *r = child(d->root, 1);
    CHECK(count(d->root) == 3 && child(d->root, 0)->type == N_COMMENT && !strcmp(child(d->root, 0)->text, " c0 "), "prolog comment");
    CHECK(element(r, "urn:a", NULL, "r") && r->elem_index == 1, "default namespace element");
    CHECK(child(d->root, 2)->type == N_PI && !strcmp(child(d->root, 2)->name, "after"), "epilog PI");
    CHECK(r->nattrs == 5, "attributes in source order with xmlns");
    CHECK(attribute(r, 0, "xmlns", WEB_NS_XMLNS, NULL, "xmlns", "urn:a"), "xmlns attribute");
    CHECK(attribute(r, 1, "xmlns:p", WEB_NS_XMLNS, "xmlns", "p", "urn:p"), "xmlns:p attribute");
    CHECK(attribute(r, 2, "p:x", "urn:p", "p", "x", "1"), "prefixed attribute");
    CHECK(attribute(r, 3, "y", NULL, NULL, "y", "2"), "unprefixed attribute has no namespace");
    CHECK(attribute(r, 4, "xml:lang", WEB_NS_XML, "xml", "lang", "ja"), "xml prefix is bound");
    CHECK(count(r) == 6, "content nodes");
    CHECK(element(child(r, 0), "urn:p", "p", "k") && child(r, 0)->elem_index == 1, "prefixed child");
    CHECK(text(child(r, 1), "t&A") && !child(r, 1)->cdata, "character and entity references");
    CHECK(text(child(r, 2), "c<d") && child(r, 2)->cdata, "CDATA section");
    CHECK(text(child(r, 3), "") && child(r, 3)->cdata, "empty CDATA section");
    CHECK(child(r, 4)->type == N_COMMENT && !strcmp(child(r, 4)->text, "c1"), "comment");
    CHECK(child(r, 5)->type == N_PI && !strcmp(child(r, 5)->name, "pi") && !strcmp(child(r, 5)->text, "data"), "PI");
    CHECK(d->control_bytes == 0, "Expat's memory returned");
    CHECK(d->mutated && d->synced, "document told of its tree");
    doc_inert_discard(d);

    d = new_doc();
    CHECK(parse_string(d, "<a xmlns:p='u1'><p:b xmlns:p='u2'/><p:c/><d xmlns='u3'><e xmlns=''/></d></a>") == XML_PARSED, "scoped declarations");
    node_t *a = child(d->root, 0);
    CHECK(element(child(a, 0), "u2", "p", "b") && element(child(a, 1), "u1", "p", "c"), "an inner declaration shadows and ends");
    CHECK(element(child(a, 2), "u3", NULL, "d") && element(child(child(a, 2), 0), NULL, NULL, "e"), "xmlns='' undeclares the default");
    doc_inert_discard(d);

    d = new_doc();
    CHECK(parse_string(d, "<r a=' x\ty\nz ' b='&#9;'>a\r\nb\rc</r>") == XML_PARSED, "normalization");
    r = child(d->root, 0);
    CHECK(attribute(r, 0, "a", NULL, NULL, "a", " x y z ") && attribute(r, 1, "b", NULL, NULL, "b", "\t"), "attribute value normalization");
    CHECK(text(child(r, 0), "a\nb\nc"), "line endings");
    doc_inert_discard(d);

    d = new_doc();
    CHECK(parse_string(d, "<a xmlns='u' xmlns:p='u'><b p:x='1' x='2'/><xml:c/></a>") == XML_PARSED, "distinct expanded names");
    CHECK(element(child(child(d->root, 0), 1), WEB_NS_XML, "xml", "c"), "xml-prefixed element");
    doc_inert_discard(d);

    /* Many declarations grow the binding table while inner ones shadow. */
    char big[20000]; size_t at = 0;
    at += (size_t)snprintf(big + at, sizeof big - at, "<r");
    for (int i = 0; i < 300; i++) at += (size_t)snprintf(big + at, sizeof big - at, " xmlns:p%d='u%d'", i, i);
    at += (size_t)snprintf(big + at, sizeof big - at, "><p299:c/><q xmlns:p7='v'><p7:d/><p8:e/></q><p7:f/></r>");
    d = new_doc();
    CHECK(parse_string(d, big) == XML_PARSED, "300 declarations");
    r = child(d->root, 0);
    CHECK(element(child(r, 0), "u299", "p299", "c"), "lookup after growth");
    CHECK(element(child(child(r, 1), 0), "v", "p7", "d") && element(child(child(r, 1), 1), "u8", "p8", "e"), "shadow after growth");
    CHECK(element(child(r, 2), "u7", "p7", "f"), "shadow ends after growth");
    doc_inert_discard(d);
}

static void malformed(void) {
    static const char *const cases[] = {
        "", "<a>", "<a></b>", "<a x='1' x='2'/>", "<p:a/>", "<a xmlns:p=''/>", "<a xmlns:xmlns='urn:x'/>",
        "<a xmlns:xml='urn:x'/>", "<a xmlns:p='http://www.w3.org/XML/1998/namespace'/>",
        "<a xmlns='http://www.w3.org/XML/1998/namespace'/>", "<a xmlns='http://www.w3.org/2000/xmlns/'/>",
        "<a xmlns:p='http://www.w3.org/2000/xmlns/'/>", "<a xmlns:p='u' xmlns:q='u' p:x='' q:x=''/>",
        "<xmlns:a xmlns:xmlns='u'/>", "<a:b:c xmlns:a='u'/>", "<a b:c:d='' xmlns:b='u'/>", "<a :b='1'/>", "<:a/>",
        "<a q:b='1'/>", "<?a:b?><r/>", "<!DOCTYPE r [<!ENTITY a:b 'x'>]><r/>", "<!DOCTYPE r [<!NOTATION n:o SYSTEM 'x'>]><r/>",
        "<!DOCTYPE :r><r/>", "<r>&undefined;</r>", "<r/><r/>", "<r>\x01</r>", "<r a='<'/>", "<r>]]></r>", "text",
        "<?xml version='1.0' standalone='yes'?><!DOCTYPE r SYSTEM 'x.dtd'><r>&ent;</r>",
        "<!DOCTYPE r [<!ENTITY e '<b>'>]><r>&e;</b></r>", "<r xmlns:a='u' a:x='1' a:x='2'/>",
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        web_doc *d = new_doc();
        struct xml_parse_error e;
        enum xml_parse_result result = parse(d, cases[i], strlen(cases[i]), true, NULL, &e);
        char label[160]; snprintf(label, sizeof label, "malformed #%zu: %s", i, cases[i]);
        CHECK(result == XML_PARSE_MALFORMED && !d->root->first && e.line >= 1 && e.reason[0], label);
        CHECK(d->control_bytes == 0, "memory returned after an error");
        doc_inert_discard(d);
    }
    web_doc *d = new_doc();
    struct xml_parse_error e;
    CHECK(parse(d, "<a>\n  <b></a>", 13, true, NULL, &e) == XML_PARSE_MALFORMED && e.line == 2 && e.column > 1, "error position");
    doc_inert_discard(d);
}

static void doctype(void) {
    web_doc *d = new_doc();
    CHECK(parse_string(d, "<!DOCTYPE r [<!ENTITY e 'v<b/>'><!-- in subset --><?pis x?><!ATTLIST r d CDATA 'dv'>]><r>&e;</r>") == XML_PARSED,
          "internal subset");
    CHECK(count(d->root) == 2 && d->root->first->type == N_DOCTYPE && !strcmp(d->root->first->name, "r"), "doctype node only");
    node_t *r = child(d->root, 1);
    CHECK(attribute(r, 0, "d", NULL, NULL, "d", "dv"), "ATTLIST default");
    CHECK(count(r) == 2 && text(child(r, 0), "v") && element(child(r, 1), NULL, NULL, "b"), "entity replacement with markup");
    doc_inert_discard(d);

    d = new_doc();
    CHECK(parse_string(d, "<!DOCTYPE r [<!ENTITY e '<p:b/>'>]><r xmlns:p='u'>&e;</r>") == XML_PARSED, "entity markup in scope");
    CHECK(element(child(child(d->root, 1), 0), "u", "p", "b"), "replacement text sees the bindings");
    doc_inert_discard(d);

    d = new_doc();
    const char *xhtml = "<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.0 Strict//EN' 'http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd'>"
                        "<html xmlns='http://www.w3.org/1999/xhtml'>&nbsp;&eacute;&amp;&NotNestedGreaterGreater;</html>";
    CHECK(parse_string(d, xhtml) == XML_PARSED, "XHTML public identifier reads the entity DTD");
    node_t *t = d->root->first;
    CHECK(t->type == N_DOCTYPE && !strcmp(t->public_id, "-//W3C//DTD XHTML 1.0 Strict//EN") &&
          !strcmp(t->system_id, "http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd"), "doctype identifiers");
    CHECK(text(child(child(d->root, 1), 0), "\xc2\xa0\xc3\xa9&\xe2\xaa\xa2\xcc\xb8"), "HTML named references");
    CHECK(element(child(d->root, 1), WEB_NS_HTML, NULL, "html"), "XHTML element");
    doc_inert_discard(d);

    d = new_doc();
    CHECK(parse_string(d, "<!DOCTYPE math PUBLIC '-//W3C//DTD MathML 2.0//EN' 'x'><math>&InvisibleTimes;</math>") == XML_PARSED, "MathML public identifier");
    doc_inert_discard(d);

    /* No external entity or DTD is read. */
    static const char *const external[] = {
        "<!DOCTYPE r SYSTEM 'http://example.invalid/x.dtd'><r>&ent;</r>",
        "<!DOCTYPE r [<!ENTITY ext SYSTEM 'file:///c:/windows/win.ini'>]><r>&ext;</r>",
        "<!DOCTYPE r [<!ENTITY % pe SYSTEM 'http://example.invalid/p'> %pe;]><r>&x;</r>",
        "<!DOCTYPE r PUBLIC '-//Not//Listed//EN' 'x.dtd'><r>&nbsp;</r>",
    };
    for (size_t i = 0; i < sizeof external / sizeof *external; i++) {
        d = new_doc();
        CHECK(parse_string(d, external[i]) == XML_PARSED && !child(d->root, 1)->first, "external reference skipped, nothing inserted");
        doc_inert_discard(d);
    }
}

static void html_elements(void) {
    web_doc *d = new_doc();
    CHECK(parse_string(d, "<r xmlns='http://www.w3.org/1999/xhtml'><template><b/>t</template><script>x</script></r>") == XML_PARSED, "XHTML template");
    node_t *template = child(child(d->root, 0), 0), *script = child(child(d->root, 0), 1);
    CHECK(!template->first && template->template_content && count(template->template_content) == 2, "template children are its contents");
    CHECK(template->template_content->owner == d->template_doc && child(template->template_content, 0)->owner == d->template_doc,
          "contents belong to the template document");
    CHECK(script->tag == T_script && script->script_started, "XML scripting disabled: the script is already started");
    doc_inert_discard(d);
}

static void encodings(void) {
    static const char declared[] = "<?xml version='1.0' encoding='windows-1252'?><x>\xe6\xa9\x9f</x>";
    struct { const char *label; const char *expect; } cases[] = {
        {NULL, "\xc3\xa6\xc2\xa9\xc5\xb8"}, {"utf-8", "\xe6\xa9\x9f"}, {"windows-1252", "\xc3\xa6\xc2\xa9\xc5\xb8"},
        {"bogus-label", "\xc3\xa6\xc2\xa9\xc5\xb8"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        web_doc *d = new_doc();
        CHECK(parse(d, declared, sizeof declared - 1, false, cases[i].label, NULL) == XML_PARSED &&
              text(child(child(d->root, 0), 0), cases[i].expect), "transport label, then the declaration");
        doc_inert_discard(d);
    }
    web_doc *d = new_doc();
    CHECK(parse(d, declared, sizeof declared - 1, true, NULL, NULL) == XML_PARSED &&
          text(child(child(d->root, 0), 0), "\xe6\xa9\x9f"), "a DOMString is UTF-8 whatever it declares");
    doc_inert_discard(d);
    d = new_doc();
    static const char string_declared[] = "<?xml version='1.0' encoding='windows-1252'?><x>\xc3\xa9</x>";
    CHECK(parse(d, string_declared, sizeof string_declared - 1, true, NULL, NULL) == XML_PARSED &&
          text(child(child(d->root, 0), 0), "\xc3\xa9"), "a DOMString ignores the declared encoding");
    doc_inert_discard(d);

    static const char bom16[] = "\xff\xfe<\0x\0>\0\xe9\0<\0/\0x\0>\0";
    d = new_doc();
    CHECK(parse(d, bom16, sizeof bom16 - 1, false, "windows-1252", NULL) == XML_PARSED && !strcmp(d->encoding, "UTF-16LE") &&
          text(child(child(d->root, 0), 0), "\xc3\xa9"), "a BOM wins over the label");
    doc_inert_discard(d);
    static const char le16[] = "<\0?\0x\0m\0l\0 \0v\0e\0r\0s\0i\0o\0n\0=\0'\0" "1\0.\0" "0\0'\0?\0>\0<\0x\0/\0>\0";
    d = new_doc();
    CHECK(parse(d, le16, sizeof le16 - 1, false, NULL, NULL) == XML_PARSED && !strcmp(d->encoding, "UTF-16LE"), "UTF-16LE without a BOM");
    doc_inert_discard(d);
    static const char ascii16[] = "<?xml version='1.0' encoding='utf-16'?><x>\xc3\xa9</x>";
    d = new_doc();
    CHECK(parse(d, ascii16, sizeof ascii16 - 1, false, NULL, NULL) == XML_PARSED && text(child(child(d->root, 0), 0), "\xc3\xa9"),
          "a UTF-16 declaration in ASCII bytes is UTF-8");
    doc_inert_discard(d);
    static const char bogus[] = "<?xml version='1.0' encoding='x-bogus'?><x/>";
    d = new_doc();
    CHECK(parse(d, bogus, sizeof bogus - 1, false, NULL, NULL) == XML_PARSE_MALFORMED, "an unsupported encoding fails");
    doc_inert_discard(d);
    d = new_doc();
    CHECK(parse(d, "<x>\xff</x>", 5, false, NULL, NULL) == XML_PARSE_MALFORMED, "malformed UTF-8 is fatal");
    doc_inert_discard(d);
}

static char *repeat_document(size_t body, size_t multibyte_at) {
    char *s = (malloc)(body + 16);
    if (!s) abort();
    memcpy(s, "<r>", 3);
    memset(s + 3, 'a', body);
    if (multibyte_at) { s[3 + multibyte_at] = (char)0xc3; s[4 + multibyte_at] = (char)0xa9; }
    memcpy(s + 3 + body, "</r>", 5);
    return s;
}
static void feeding(void) {
    size_t body = 3u << 20, split = (1u << 20) - 4;
    char *s = repeat_document(body, split);
    web_doc *d = new_doc();
    CHECK(parse(d, s, body + 7, false, NULL, NULL) == XML_PARSED, "input fed in parts");
    node_t *t = child(child(d->root, 0), 0);
    CHECK(t && t->textlen == body && (unsigned char)t->text[split] == 0xc3 && (unsigned char)t->text[split + 1] == 0xa9,
          "a character split across parts");
    doc_inert_discard(d);
    (free)(s);
}

static void nesting(void) {
    enum { DEPTH = 100000 };
    size_t length = (size_t)DEPTH * (sizeof "<a xmlns:p='u'>" + sizeof "</a>") + 16;
    char *s = (malloc)(length), *p = s;
    if (!s) abort();
    for (int i = 0; i < DEPTH; i++) { memcpy(p, "<a xmlns:p='u'>", 15); p += 15; }
    memcpy(p, "<p:b/>", 6); p += 6;
    for (int i = 0; i < DEPTH; i++) { memcpy(p, "</a>", 4); p += 4; }
    web_doc *d = new_doc();
    CHECK(parse(d, s, (size_t)(p - s), false, NULL, NULL) == XML_PARSED, "100000 nested elements with declarations");
    int depth = 0; node_t *n = d->root->first;
    while (n && n->type == N_ELEM && n->first) { depth++; n = n->first; }
    CHECK(depth == DEPTH && element(n, "u", "p", "b"), "the tree is as deep as the input");
    CHECK(d->control_bytes == 0, "binding stack released");
    doc_inert_discard(d);
    (free)(s);
}

static const char laughs[] =
    "<!DOCTYPE l [<!ENTITY a 'lollollollollollollollollollol'>"
    "<!ENTITY b '&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;'><!ENTITY c '&b;&b;&b;&b;&b;&b;&b;&b;&b;&b;'>"
    "<!ENTITY d '&c;&c;&c;&c;&c;&c;&c;&c;&c;&c;'><!ENTITY e '&d;&d;&d;&d;&d;&d;&d;&d;&d;&d;'>"
    "<!ENTITY f '&e;&e;&e;&e;&e;&e;&e;&e;&e;&e;'><!ENTITY g '&f;&f;&f;&f;&f;&f;&f;&f;&f;&f;'>"
    "<!ENTITY h '&g;&g;&g;&g;&g;&g;&g;&g;&g;&g;'><!ENTITY i '&h;&h;&h;&h;&h;&h;&h;&h;&h;&h;'>]><l>&i;</l>";
static void expansion(void) {
    size_t before = heap_live;
    web_doc *d = new_doc();
    checkpoints = 0; cancel_at = 20000;
    CHECK(parse_string(d, laughs) == XML_PARSE_CANCELLED && !d->root->first, "the checkpoint cancels an expansion");
    CHECK(d->control_bytes == 0, "cancelled parse returns its memory");
    doc_inert_discard(d);
    CHECK(heap_live == before, "nothing kept after cancellation");
    cancel_at = -1;

    d = new_doc();
    heap_cap = heap_live + (64u << 20);
    CHECK(parse_string(d, laughs) == XML_PARSE_OOM && !d->root->first, "the expansion is charged until memory refuses");
    CHECK(d->control_bytes == 0, "refused parse returns its memory");
    heap_cap = SIZE_MAX;
    doc_inert_discard(d);
    CHECK(heap_live == before, "nothing kept after refusal");

    /* A small expansion is ordinary XML. */
    d = new_doc();
    CHECK(parse_string(d, "<!DOCTYPE l [<!ENTITY a 'lol'><!ENTITY b '&a;&a;&a;'>]><l>&b;&b;</l>") == XML_PARSED &&
          text(child(child(d->root, 1), 0), "lollollollollollol"), "nested entities expand as specified");
    doc_inert_discard(d);
}

static void allocation_failures(void) {
    static const char *const inputs[] = {
        "<?xml version='1.0'?><!DOCTYPE r [<!ENTITY e 'v<b/>'><!ATTLIST r d CDATA 'dv'>]>"
        "<r xmlns='urn:a' xmlns:p='urn:p' p:x='1' y='2'><p:k/>t&amp;<![CDATA[c]]><!--c--><?pi d?>&e;"
        "<t xmlns='http://www.w3.org/1999/xhtml'><template><i/></template></t></r>",
        "<!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.1//EN' 'x'><html xmlns='http://www.w3.org/1999/xhtml'>&eacute;</html>",
    };
    for (size_t k = 0; k < sizeof inputs / sizeof *inputs; k++) {
        size_t before = heap_live;
        allocations = 0;
        web_doc *d = new_doc();
        CHECK(parse_string(d, inputs[k]) == XML_PARSED, "fault sweep input parses");
        long needed = allocations;
        doc_inert_discard(d);
        bool sound = true, clean = true;
        for (long i = 0; i < needed; i++) {
            allocations = 0; fail_allocation = -1;
            d = new_doc();
            allocations = 0; fail_allocation = i;
            enum xml_parse_result result = parse_string(d, inputs[k]);
            fail_allocation = -1;
            if (result != XML_PARSED && (result != XML_PARSE_OOM || d->root->first)) sound = false;
            if (d->control_bytes) clean = false;
            doc_inert_discard(d);
            if (heap_live != before) { clean = false; heap_live = before; }
        }
        char label[96];
        snprintf(label, sizeof label, "each of %ld allocations fails as OOM (input %zu)", needed, k);
        printf("%s\n", label);
        CHECK(sound, label);
        CHECK(clean, "every failure returns its memory");
    }
}

static void documents(void) {
    web_doc *family = new_doc();
    enum xml_parse_result result;
    web_doc *d = xml_document_parse(family, family, "<a>", 3, true, NULL, "about:blank", "text/xml", true, &result);
    CHECK(d && result == XML_PARSE_MALFORMED && count(d->root) == 1, "DOMParser's error document");
    node_t *e = d ? d->root->first : NULL;
    CHECK(element(e, XML_PARSERERROR_NS, NULL, "parsererror") && e->first && e->first->type == N_TEXT, "parsererror element");
    CHECK(d && !strcmp(d->content_type, "text/xml") && d->xml && !d->xml_interface, "a Document of the given type");
    doc_inert_discard(d);
    d = xml_document_parse(family, family, "<a>", 3, true, NULL, "about:blank", "text/xml", false, &result);
    CHECK(!d && result == XML_PARSE_MALFORMED, "XHR's failure is no document");
    d = xml_document_parse(family, family, "<a/>", 4, true, NULL, "about:blank", NULL, false, &result);
    CHECK(d && result == XML_PARSED && !strcmp(d->content_type, "application/xml"), "default content type");
    doc_inert_discard(d);
    doc_inert_discard(family);
}

/* ------------------------------------------- a browsing context's document */
static web_doc *live_doc(void) {
    web_doc *d = shim_doc();
    if (!d) abort();
    return d;
}
/* Resume past slices to the next script, the end or a failure. */
static int boundary(struct xml_parser *x, node_t **script, int *yields) {
    for (;;) {
        int result = xml_parser_resume(x, script);
        if (result != HTML_PARSE_YIELD) return result;
        if (yields) (*yields)++;
    }
}
static void link_last(node_t *parent, node_t *n) {
    n->parent = parent; n->prev = parent->last; n->next = NULL;
    if (parent->last) parent->last->next = n; else parent->first = n;
    parent->last = n;
}

static void scripts(void) {
    static const char page[] =
        "<?xml version='1.0' encoding='utf-8'?>"
        "<html xmlns='http://www.w3.org/1999/xhtml'><head><script>one()</script></head>"
        "<body><p>x</p><script><![CDATA[if (1 < 2) two();]]></script>"
        "<svg xmlns='http://www.w3.org/2000/svg'><script>three()</script></svg>"
        "<template><script>inert()</script></template><s:script xmlns:s='urn:other'>no</s:script><i/></body></html>";
    size_t before = heap_live;
    web_doc *d = live_doc();
    struct xml_parser *x = xml_parser_begin(d, page, sizeof page - 1, NULL);
    CHECK(x && d->xml && d->root && !strcmp(d->encoding, "UTF-8") && d->encoding_certain, "a browsing context's XML document");
    node_t *s = NULL;
    CHECK(boundary(x, &s, NULL) == HTML_PARSE_SCRIPT && element(s, WEB_NS_HTML, NULL, "script"),
          "a script's end tag stops the parser");
    node_t *html = child(d->root, 0), *head = child(html, 0);
    CHECK(s && !s->script_started && s->parent == head && text(child(s, 0), "one()"),
          "the script is left to be prepared, its text parsed");
    CHECK(count(html) == 1 && xml_parser_changed(x) && d->mutated && d->synced && d->captures == 1,
          "nothing after the end tag yet, and the document was told");
    CHECK(boundary(x, &s, NULL) == HTML_PARSE_SCRIPT && element(s, WEB_NS_HTML, NULL, "script") &&
          text(child(s, 0), "if (1 < 2) two();") && child(s, 0)->cdata, "a script in a CDATA section");
    node_t *body = child(html, 1);
    CHECK(body && count(body) == 2, "the body up to that script");
    CHECK(boundary(x, &s, NULL) == HTML_PARSE_SCRIPT && element(s, WEB_NS_SVG, NULL, "script"), "an SVG script");
    CHECK(boundary(x, &s, NULL) == HTML_PARSE_DONE && !s, "the end: no template or foreign script returns");
    node_t *template = child(body, 3), *other = child(body, 4);
    CHECK(template && template->template_content && child(template->template_content, 0)->script_started,
          "a template's script is already started");
    CHECK(element(other, "urn:other", "s", "script"), "a script element of another namespace");
    CHECK(element(child(body, 5), WEB_NS_HTML, NULL, "i") && child(body, 5)->elem_index == 6, "element indexes");
    CHECK(xml_parser_resume(x, &s) == HTML_PARSE_DONE && !xml_parser_changed(x), "the end stays the end");
    xml_parser_finish(x);
    CHECK(d->control_bytes == 0 && d->captures == 3 && !d->console_lines, "Expat's memory returned");
    doc_inert_discard(d);
    CHECK(heap_live == before, "nothing kept");

    /* A script changes an open element between resumes. */
    d = live_doc();
    static const char changed[] = "<r><script xmlns='http://www.w3.org/1999/xhtml'>s</script><b/></r>";
    x = xml_parser_begin(d, changed, sizeof changed - 1, NULL);
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_SCRIPT, "the script's boundary");
    node_t *r = child(d->root, 0), *inserted = doc_element_create(d, NULL, NULL, "x");
    link_last(r, inserted); inserted->elem_index = 2;
    CHECK(boundary(x, &s, NULL) == HTML_PARSE_DONE && element(child(r, 2), NULL, NULL, "b") && child(r, 2)->elem_index == 3,
          "the parser appends after what the script inserted");
    xml_parser_finish(x);
    doc_inert_discard(d);

    /* The response bytes may go once the document has begun. */
    d = live_doc();
    char bytes[] = "<x>abc</x>";
    x = xml_parser_begin(d, bytes, sizeof bytes - 1, NULL);
    memset(bytes, 'z', sizeof bytes - 1);
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_DONE && text(child(child(d->root, 0), 0), "abc"), "the parser reads its own copy");
    xml_parser_finish(x);
    doc_inert_discard(d);
}

static void slices(void) {
    enum { ELEMENTS = 100000 };
    size_t length = 3 + (size_t)ELEMENTS * 4 + 4;
    char *s = (malloc)(length + 1), *p = s;
    if (!s) abort();
    memcpy(p, "<r>", 3); p += 3;
    for (int i = 0; i < ELEMENTS; i++) { memcpy(p, "<i/>", 4); p += 4; }
    memcpy(p, "</r>", 4); p += 4;
    web_doc *d = live_doc();
    struct xml_parser *x = xml_parser_begin(d, s, length, NULL);
    node_t *script = NULL;
    CHECK(x && xml_parser_resume(x, &script) == HTML_PARSE_YIELD && xml_parser_changed(x), "a resume reads a slice");
    int first = count(child(d->root, 0)), yields = 1;
    CHECK(first > 0 && first < ELEMENTS, "part of the document after one slice");
    CHECK(boundary(x, &script, &yields) == HTML_PARSE_DONE && count(child(d->root, 0)) == ELEMENTS && yields >= 3,
          "the rest over later resumes");
    CHECK(child(child(d->root, 0), 0)->elem_index == 1 && child(d->root, 0)->last->elem_index == ELEMENTS, "indexes across slices");
    xml_parser_finish(x);
    CHECK(d->control_bytes == 0, "memory returned");
    doc_inert_discard(d);
    (free)(s);
}

static void errors(void) {
    /* A well-formedness error after a script: the document becomes its error. */
    static const char broken[] = "<r xmlns='http://www.w3.org/1999/xhtml'><script>s</script><p>unfinished</r>";
    web_doc *d = live_doc();
    struct xml_parser *x = xml_parser_begin(d, broken, sizeof broken - 1, NULL);
    node_t *s = NULL;
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_SCRIPT, "the script before the error runs");
    node_t *r = child(d->root, 0);
    CHECK(boundary(x, &s, NULL) == HTML_PARSE_DONE && xml_parser_changed(x), "the error ends the parse");
    node_t *e = child(d->root, 0);
    CHECK(count(d->root) == 1 && element(e, XML_PARSERERROR_NS, NULL, "parsererror") && e->first && e->first->type == N_TEXT,
          "the parsererror element is the document's tree");
    CHECK(!r->parent && d->removals == 1, "the tree built so far left the document");
    CHECK(d->console_lines == 1 && strstr(d->console, "XML") && strstr(e->first->text, "1 行"), "the error is reported");
    xml_parser_finish(x);
    CHECK(d->control_bytes == 0, "memory returned after the error");
    doc_inert_discard(d);

    static const char bogus[] = "<?xml version='1.0' encoding='x-bogus'?><x/>";
    d = live_doc();
    x = xml_parser_begin(d, bogus, sizeof bogus - 1, NULL);
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_DONE && element(child(d->root, 0), XML_PARSERERROR_NS, NULL, "parsererror"),
          "an encoding Expat cannot read is the document's error");
    xml_parser_finish(x);
    doc_inert_discard(d);

    static const char latin[] = "<x>\xe9</x>";
    d = live_doc();
    x = xml_parser_begin(d, latin, sizeof latin - 1, "windows-1252");
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_DONE && text(child(child(d->root, 0), 0), "\xc3\xa9") &&
          !strcmp(d->encoding, "windows-1252"), "the transport label decodes");
    xml_parser_finish(x);
    doc_inert_discard(d);

    /* A native checkpoint cancels: the tree so far stays. */
    enum { ELEMENTS = 5000 };
    size_t length = 3 + (size_t)ELEMENTS * 4 + 4;
    char *big = (malloc)(length + 1), *p = big;
    if (!big) abort();
    memcpy(p, "<r>", 3); p += 3;
    for (int i = 0; i < ELEMENTS; i++) { memcpy(p, "<i/>", 4); p += 4; }
    memcpy(p, "</r>", 4);
    d = live_doc();
    x = xml_parser_begin(d, big, length, NULL);
    checkpoints = 0; cancel_at = 2000;
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_CANCELLED, "the checkpoint cancels");
    cancel_at = -1;
    int kept = count(child(d->root, 0));
    CHECK(kept > 0 && kept < ELEMENTS && d->mutated && xml_parser_changed(x), "the tree so far stays, and the document knows");
    CHECK(xml_parser_resume(x, &s) == HTML_PARSE_CANCELLED, "a cancelled parse stays cancelled");
    xml_parser_finish(x);
    CHECK(d->control_bytes == 0, "memory returned after cancellation");
    doc_inert_discard(d);
    (free)(big);
}

static void inserted(void) {
    static const char page[] =
        "<html xmlns='http://www.w3.org/1999/xhtml'><head>"
        "<meta http-equiv='Content-Security-Policy' content=\"script-src 'none'\"/><meta name='referrer' content='no-referrer'/>"
        "</head><body><meta http-equiv='content-security-policy' content='img-src none'/><details open=''/>"
        "<textarea>v</textarea></body></html>";
    web_doc *d = live_doc();
    struct xml_parser *x = xml_parser_begin(d, page, sizeof page - 1, NULL);
    node_t *s = NULL;
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_DONE, "metadata document parses");
    node_t *meta = child(child(child(d->root, 0), 0), 0);
    CHECK(d->policies == 1 && !strcmp(d->policy, "script-src 'none'") && meta->html_policy_processed,
          "a head meta's Content Security Policy applies; one in body does not");
    CHECK(d->referrers == 3 && d->details == 1, "referrer metadata and details groups");
    xml_parser_finish(x);
    doc_inert_discard(d);

    d = new_doc();
    CHECK(parse_string(d, page) == XML_PARSED && !d->policies && !d->referrers, "scripting disabled: no policy from the parse");
    doc_inert_discard(d);

    d = live_doc();
    policy_fails = true;
    x = xml_parser_begin(d, page, sizeof page - 1, NULL);
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_FAILED, "a policy that cannot be kept fails the parse");
    policy_fails = false;
    xml_parser_finish(x);
    doc_inert_discard(d);

    static const char script[] = "<r xmlns='http://www.w3.org/1999/xhtml'><script>s</script></r>";
    d = live_doc();
    capture_fails = true;
    x = xml_parser_begin(d, script, sizeof script - 1, NULL);
    CHECK(x && boundary(x, &s, NULL) == HTML_PARSE_FAILED && !s, "a script text that cannot be kept fails the parse");
    capture_fails = false;
    xml_parser_finish(x);
    doc_inert_discard(d);
}

static void live_allocation_failures(void) {
    static const char page[] =
        "<?xml version='1.0'?><!DOCTYPE html PUBLIC '-//W3C//DTD XHTML 1.1//EN' 'x'>"
        "<html xmlns='http://www.w3.org/1999/xhtml'><head><meta http-equiv='Content-Security-Policy' content='a'/>"
        "<script>one()</script></head><body>&eacute;<template><i/></template><details/>"
        "<script><![CDATA[two()]]></script><p xmlns:q='urn:q' q:a='1'>t</p></body></html>";
    size_t before = heap_live;
    allocations = 0;
    web_doc *d = live_doc();
    allocations = 0;
    struct xml_parser *x = xml_parser_begin(d, page, sizeof page - 1, NULL);
    node_t *s = NULL;
    int result;
    while ((result = xml_parser_resume(x, &s)) == HTML_PARSE_SCRIPT || result == HTML_PARSE_YIELD) {}
    CHECK(result == HTML_PARSE_DONE, "fault sweep document parses");
    long needed = allocations;
    xml_parser_finish(x);
    doc_inert_discard(d);
    bool sound = true, clean = true;
    for (long i = 0; i < needed; i++) {
        allocations = 0; fail_allocation = -1;
        d = live_doc();
        allocations = 0; fail_allocation = i;
        x = xml_parser_begin(d, page, sizeof page - 1, NULL);
        result = HTML_PARSE_FAILED;
        if (x) while ((result = xml_parser_resume(x, &s)) == HTML_PARSE_SCRIPT || result == HTML_PARSE_YIELD) {}
        fail_allocation = -1;
        if (x && result != HTML_PARSE_FAILED) sound = false;
        xml_parser_finish(x);
        if (d->control_bytes) clean = false;
        doc_inert_discard(d);
        if (heap_live != before) { clean = false; heap_live = before; }
    }
    char label[96];
    snprintf(label, sizeof label, "each of %ld allocations of a browsing context's parse fails it", needed);
    printf("%s\n", label);
    CHECK(sound, label);
    CHECK(clean, "every failed browsing context parse returns its memory");
}

int main(void) {
    well_formed();
    malformed();
    doctype();
    html_elements();
    encodings();
    feeding();
    nesting();
    expansion();
    allocation_failures();
    documents();
    scripts();
    slices();
    errors();
    inserted();
    live_allocation_failures();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
