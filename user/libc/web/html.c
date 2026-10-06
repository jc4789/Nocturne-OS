/* HTML parsing: a tokenizer and a tree builder that follows the HTML5 rules where they matter
   most for real pages (implied html/head/body, paragraphs and list items closing each other,
   tables, raw text elements, void elements, foreign SVG), and keeps it simple elsewhere: there
   is no adoption agency and no foster parenting. */
#include <stdio.h>
#include <ctype.h>
#include "webi.h"

const char *const tag_names[T_COUNT] = {
    "",
#define X(n) #n,
    WEB_TAGS(X)
#undef X
};

static int tag_order[T_COUNT]; /* tag ids sorted by name, for bsearch */
static bool tags_sorted;

static int cmp_tag(const void *a, const void *b) { return strcmp(tag_names[*(const int *)a], tag_names[*(const int *)b]); }

int tag_lookup(const char *name, size_t n) {
    if (!tags_sorted) {
        for (int i = 0; i < T_COUNT - 1; i++) tag_order[i] = i + 1;
        qsort(tag_order, T_COUNT - 1, sizeof(int), cmp_tag);
        tags_sorted = true;
    }
    char low[16];
    if (n >= sizeof low) return T_UNKNOWN;
    for (size_t i = 0; i < n; i++) low[i] = (char)lower((unsigned char)name[i]);
    name = low;
    int lo = 0, hi = T_COUNT - 2;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *t = tag_names[tag_order[mid]];
        int c = strncmp(name, t, n);
        if (c == 0) c = t[n] ? -1 : 0;
        if (c == 0) return tag_order[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return T_UNKNOWN;
}

/* ---------------------------------------------------------------- character references */
struct entity {
    const char *name;
    uint32_t cp;
};

static struct entity entities[] = {
    {"AElig", 198}, {"AMP", 38}, {"Aacute", 193}, {"Acirc", 194}, {"Agrave", 192}, {"Alpha", 913},
    {"Aring", 197}, {"Atilde", 195}, {"Auml", 196}, {"Beta", 914}, {"COPY", 169}, {"Ccedil", 199},
    {"Chi", 935}, {"Dagger", 8225}, {"Delta", 916}, {"ETH", 208}, {"Eacute", 201}, {"Ecirc", 202},
    {"Egrave", 200}, {"Epsilon", 917}, {"Eta", 919}, {"Euml", 203}, {"GT", 62}, {"Gamma", 915},
    {"Iacute", 205}, {"Icirc", 206}, {"Igrave", 204}, {"Iota", 921}, {"Iuml", 207}, {"Kappa", 922},
    {"LT", 60}, {"Lambda", 923}, {"Mu", 924}, {"NewLine", 10}, {"Ntilde", 209}, {"Nu", 925},
    {"OElig", 338}, {"Oacute", 211}, {"Ocirc", 212}, {"Ograve", 210}, {"Omega", 937}, {"Omicron", 927},
    {"Oslash", 216}, {"Otilde", 213}, {"Ouml", 214}, {"Phi", 934}, {"Pi", 928}, {"Prime", 8243},
    {"Psi", 936}, {"QUOT", 34}, {"REG", 174}, {"Rho", 929}, {"Scaron", 352}, {"Sigma", 931},
    {"THORN", 222}, {"TRADE", 8482}, {"Tab", 9}, {"Tau", 932}, {"Theta", 920}, {"Uacute", 218},
    {"Ucirc", 219}, {"Ugrave", 217}, {"Upsilon", 933}, {"Uuml", 220}, {"Xi", 926}, {"Yacute", 221},
    {"Yuml", 376}, {"Zeta", 918}, {"aacute", 225}, {"acirc", 226}, {"acute", 180}, {"aelig", 230},
    {"agrave", 224}, {"alefsym", 8501}, {"alpha", 945}, {"amp", 38}, {"and", 8743}, {"ang", 8736},
    {"apos", 39}, {"aring", 229}, {"ast", 42}, {"asymp", 8776}, {"atilde", 227}, {"auml", 228},
    {"bdquo", 8222}, {"beta", 946}, {"brvbar", 166}, {"bsol", 92}, {"bull", 8226}, {"bullet", 8226},
    {"cap", 8745}, {"ccedil", 231}, {"cedil", 184}, {"cent", 162}, {"check", 10003}, {"checkmark", 10003},
    {"chi", 967}, {"circ", 710}, {"clubs", 9827}, {"colon", 58}, {"comma", 44}, {"commat", 64},
    {"cong", 8773}, {"copy", 169}, {"crarr", 8629}, {"cross", 10007}, {"cup", 8746}, {"curren", 164},
    {"dArr", 8659}, {"dagger", 8224}, {"darr", 8595}, {"dash", 8208}, {"deg", 176}, {"delta", 948},
    {"diams", 9830}, {"divide", 247}, {"dollar", 36}, {"eacute", 233}, {"ecirc", 234}, {"egrave", 232},
    {"empty", 8709}, {"emsp", 8195}, {"ensp", 8194}, {"epsilon", 949}, {"equals", 61}, {"equiv", 8801},
    {"eta", 951}, {"eth", 240}, {"euml", 235}, {"euro", 8364}, {"excl", 33}, {"exist", 8707},
    {"fnof", 402}, {"forall", 8704}, {"frac12", 189}, {"frac13", 8531}, {"frac14", 188}, {"frac23", 8532},
    {"frac34", 190}, {"frasl", 8260}, {"gamma", 947}, {"ge", 8805}, {"grave", 96}, {"gt", 62},
    {"hArr", 8660}, {"hairsp", 8202}, {"half", 189}, {"harr", 8596}, {"hearts", 9829}, {"hellip", 8230},
    {"horbar", 8213}, {"hyphen", 8208}, {"iacute", 237}, {"icirc", 238}, {"iexcl", 161}, {"igrave", 236},
    {"image", 8465}, {"infin", 8734}, {"int", 8747}, {"iota", 953}, {"iquest", 191}, {"isin", 8712},
    {"iuml", 239}, {"kappa", 954}, {"lArr", 8656}, {"lambda", 955}, {"lang", 10216}, {"laquo", 171},
    {"larr", 8592}, {"lbrace", 123}, {"lbrack", 91}, {"lceil", 8968}, {"lcub", 123}, {"ldquo", 8220},
    {"le", 8804}, {"lfloor", 8970}, {"lowast", 8727}, {"lowbar", 95}, {"loz", 9674}, {"lpar", 40},
    {"lrm", 8206}, {"lsaquo", 8249}, {"lsqb", 91}, {"lsquo", 8216}, {"lt", 60}, {"macr", 175},
    {"mdash", 8212}, {"micro", 181}, {"middot", 183}, {"minus", 8722}, {"mldr", 8230}, {"mu", 956},
    {"nabla", 8711}, {"nbsp", 160}, {"ndash", 8211}, {"ne", 8800}, {"ni", 8715}, {"nldr", 8229},
    {"not", 172}, {"notin", 8713}, {"nsub", 8836}, {"ntilde", 241}, {"nu", 957}, {"num", 35},
    {"numsp", 8199}, {"oacute", 243}, {"ocirc", 244}, {"oelig", 339}, {"ograve", 242}, {"oline", 8254},
    {"omega", 969}, {"omicron", 959}, {"oplus", 8853}, {"or", 8744}, {"ordf", 170}, {"ordm", 186},
    {"oslash", 248}, {"otilde", 245}, {"otimes", 8855}, {"ouml", 246}, {"para", 182}, {"part", 8706},
    {"percnt", 37}, {"period", 46}, {"permil", 8240}, {"perp", 8869}, {"phi", 966}, {"pi", 960},
    {"piv", 982}, {"plus", 43}, {"plusmn", 177}, {"pound", 163}, {"prime", 8242}, {"prod", 8719},
    {"prop", 8733}, {"psi", 968}, {"quest", 63}, {"quot", 34}, {"rArr", 8658}, {"radic", 8730},
    {"rang", 10217}, {"raquo", 187}, {"rarr", 8594}, {"rbrace", 125}, {"rbrack", 93}, {"rceil", 8969},
    {"rcub", 125}, {"rdquo", 8221}, {"real", 8476}, {"reg", 174}, {"rfloor", 8971}, {"rho", 961},
    {"rlm", 8207}, {"rpar", 41}, {"rsaquo", 8250}, {"rsqb", 93}, {"rsquo", 8217}, {"sbquo", 8218},
    {"scaron", 353}, {"sdot", 8901}, {"sect", 167}, {"semi", 59}, {"shy", 173}, {"sigma", 963},
    {"sigmaf", 962}, {"sim", 8764}, {"sol", 47}, {"spades", 9824}, {"star", 9734}, {"starf", 9733},
    {"sub", 8834}, {"sube", 8838}, {"sum", 8721}, {"sup", 8835}, {"sup1", 185}, {"sup2", 178},
    {"sup3", 179}, {"supe", 8839}, {"szlig", 223}, {"tau", 964}, {"there4", 8756}, {"theta", 952},
    {"thetasym", 977}, {"thinsp", 8201}, {"thorn", 254}, {"tilde", 732}, {"times", 215}, {"trade", 8482},
    {"uArr", 8657}, {"uacute", 250}, {"uarr", 8593}, {"ucirc", 251}, {"ugrave", 249}, {"uml", 168},
    {"upsih", 978}, {"upsilon", 965}, {"uuml", 252}, {"verbar", 124}, {"vert", 124}, {"weierp", 8472},
    {"xi", 958}, {"yacute", 253}, {"yen", 165}, {"yuml", 255}, {"zeta", 950}, {"zwj", 8205},
    {"zwnj", 8204}, {"ZeroWidthSpace", 8203}, {"ThinSpace", 8201}, {"nbsp", 160},
};
#define NENT (int)(sizeof entities / sizeof *entities)
static bool ent_sorted;

static int cmp_ent(const void *a, const void *b) {
    return strcmp(((const struct entity *)a)->name, ((const struct entity *)b)->name);
}

static uint32_t entity_find(const char *name, size_t n) {
    if (!ent_sorted) {
        qsort(entities, NENT, sizeof *entities, cmp_ent);
        ent_sorted = true;
    }
    int lo = 0, hi = NENT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strncmp(name, entities[mid].name, n);
        if (c == 0) c = entities[mid].name[n] ? -1 : 0;
        if (c == 0) return entities[mid].cp;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return 0;
}

/* the names that may appear without a semicolon (HTML's legacy list, Latin-1 and the basics) */
static bool legacy_entity(const char *name, size_t n) {
    uint32_t cp = entity_find(name, n);
    return cp && (cp < 256 || cp == 338 || cp == 339) && strncmp(name, "apos", n) != 0;
}

static const uint16_t win1252[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178,
};

/* decode a character reference at s[0] == '&'; returns bytes consumed (0: not a reference) */
static size_t char_ref(const char *s, size_t n, bool in_attr, sbuf *out) {
    if (n < 2) return 0;
    if (s[1] == '#') {
        size_t i = 2;
        bool hex = i < n && (s[i] == 'x' || s[i] == 'X');
        if (hex) i++;
        size_t start = i;
        uint32_t v = 0;
        while (i < n) {
            int c = (unsigned char)s[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && (c | 32) >= 'a' && (c | 32) <= 'f') d = (c | 32) - 'a' + 10;
            else break;
            if (v < 0x110000) v = v * (hex ? 16 : 10) + (uint32_t)d;
            i++;
        }
        if (i == start) return 0;
        if (i < n && s[i] == ';') i++;
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v < 0xE000)) v = 0xFFFD;
        else if (v >= 0x80 && v < 0xA0) v = win1252[v - 0x80];
        sb_utf8(out, v);
        return i;
    }
    size_t i = 1;
    while (i < n && i < 40 && (((s[i] | 32) >= 'a' && (s[i] | 32) <= 'z') || (s[i] >= '0' && s[i] <= '9'))) i++;
    size_t len = i - 1;
    if (!len) return 0;
    if (i < n && s[i] == ';') {
        uint32_t cp = entity_find(s + 1, len);
        if (cp) {
            sb_utf8(out, cp);
            return i + 1;
        }
    }
    /* no semicolon: the longest legacy name that prefixes it */
    for (size_t k = len; k >= 2; k--) {
        if (!legacy_entity(s + 1, k)) continue;
        char next = 1 + k < n ? s[1 + k] : 0;
        if (in_attr && (next == '=' || ((next | 32) >= 'a' && (next | 32) <= 'z') || (next >= '0' && next <= '9')))
            return 0;
        sb_utf8(out, entity_find(s + 1, k));
        return 1 + k;
    }
    return 0;
}

/* ---------------------------------------------------------------- tree building */
enum { M_BEFORE_HEAD, M_IN_HEAD, M_AFTER_HEAD, M_IN_BODY };

#define MAX_STACK 400

struct html_parser {
    web_doc *d;
    arena_t *a;
    bool scripting, fragment, finished, failed;
    node_t *yield;
    size_t write_offset;
    sbuf input, raw;
    const char *s;
    size_t n, i;
    node_t *stack[MAX_STACK];
    int sp;
    node_t *doc, *html, *head, *body;
    int mode;
    bool skip_lf; /* after <pre>, <listing>, <textarea> */
    sbuf text;
    /* the current tag token */
    char tname[64];
    size_t tlen;
    struct attr tattrs[64];
    int ntattrs;
    bool self_closing;
    sbuf aval;
};

static node_t *cur(struct html_parser *p) { return p->stack[p->sp - 1]; }

static void append(node_t *parent, node_t *c) {
    c->parent = parent;
    c->prev = parent->last;
    if (parent->last) parent->last->next = c;
    else parent->first = c;
    parent->last = c;
    if (c->type == N_ELEM) {
        int idx = 1;
        for (node_t *s = c->prev; s; s = s->prev)
            if (s->type == N_ELEM) {
                idx = s->elem_index + 1;
                break;
            }
        c->elem_index = idx;
    }
}

static node_t *new_elem(struct html_parser *p, const char *name, size_t len, bool foreign) {
    node_t *e = ar_alloc(p->a, sizeof *e);
    e->owned_next = p->d->owned_nodes;
    p->d->owned_nodes = e;
    e->type = N_ELEM;
    char lname[64];
    size_t ln = len < sizeof lname - 1 ? len : sizeof lname - 1;
    for (size_t i = 0; i < ln; i++) lname[i] = (char)lower((unsigned char)name[i]);
    e->tag = (uint16_t)tag_lookup(lname, ln);
    e->name = e->tag ? tag_names[e->tag] : ar_strndup(p->a, lname, ln);
    e->raw_name = foreign ? ar_strndup(p->a, name, ln) : e->name;
    e->foreign = foreign;
    e->image = -1;
    return e;
}

/* give the element the attributes of the current token */
static void set_attrs(struct html_parser *p, node_t *e) {
    if (!p->ntattrs) return;
    e->attrs = ar_alloc(p->a, sizeof(struct attr) * (size_t)p->ntattrs);
    e->nattrs = p->ntattrs;
    memcpy(e->attrs, p->tattrs, sizeof(struct attr) * (size_t)p->ntattrs);
    for (int i = 0; i < e->nattrs; i++) {
        if (!strcmp(e->attrs[i].name, "id") && e->attrs[i].value[0]) e->id = e->attrs[i].value;
        else if (!strcmp(e->attrs[i].name, "class")) {
            const char *v = e->attrs[i].value;
            int cnt = 0;
            for (const char *q = v; *q;) {
                while (is_space((unsigned char)*q)) q++;
                if (!*q) break;
                cnt++;
                while (*q && !is_space((unsigned char)*q)) q++;
            }
            if (!cnt) continue;
            e->classes = ar_alloc(p->a, sizeof(char *) * (size_t)cnt);
            for (const char *q = v; *q;) {
                while (is_space((unsigned char)*q)) q++;
                if (!*q) break;
                const char *st = q;
                while (*q && !is_space((unsigned char)*q)) q++;
                e->classes[e->nclasses++] = ar_strndup(p->a, st, (size_t)(q - st));
            }
        }
    }
}

static void flush_text(struct html_parser *p);

static void push(struct html_parser *p, node_t *e) {
    if (p->sp < MAX_STACK) p->stack[p->sp++] = e;
}

static void pop(struct html_parser *p) {
    if (p->sp > 1) p->sp--;
}

static void ensure_html(struct html_parser *p) {
    if (p->html) return;
    p->html = new_elem(p, "html", 4, false);
    append(p->doc, p->html);
    p->sp = 0;
    push(p, p->html);
}

static void ensure_head(struct html_parser *p) {
    ensure_html(p);
    if (p->head) return;
    p->head = new_elem(p, "head", 4, false);
    append(p->html, p->head);
}

static void ensure_body(struct html_parser *p) {
    if (p->mode == M_IN_BODY) return;
    ensure_head(p);
    p->sp = 1; /* close head (and anything left open in it) */
    if (!p->body) {
        p->body = new_elem(p, "body", 4, false);
        append(p->html, p->body);
    }
    push(p, p->body);
    p->mode = M_IN_BODY;
}

static void insert_text(struct html_parser *p, const char *s, size_t n) {
    node_t *parent = cur(p);
    node_t *last = parent->last;
    if (last && last->type == N_TEXT) {
        char *t = ar_alloc(p->a, last->textlen + n + 1);
        memcpy(t, last->text, last->textlen);
        memcpy(t + last->textlen, s, n);
        last->text = t;
        last->textlen += n;
        return;
    }
    node_t *t = ar_alloc(p->a, sizeof *t);
    t->owned_next = p->d->owned_nodes;
    p->d->owned_nodes = t;
    t->type = N_TEXT;
    t->text = ar_strndup(p->a, s, n);
    t->textlen = n;
    t->image = -1;
    append(parent, t);
}

static void flush_text(struct html_parser *p) {
    if (!p->text.n) return;
    const char *s = p->text.p;
    size_t n = p->text.n;
    p->text.n = 0;
    if (p->mode != M_IN_BODY) {
        size_t ws = 0;
        while (ws < n && is_space((unsigned char)s[ws])) ws++;
        if (ws == n) return; /* whitespace before the body is dropped */
        ensure_body(p);
        s += ws;
        n -= ws;
    }
    insert_text(p, s, n);
}

static bool tag_in(int t, const int *list) {
    for (; *list; list++)
        if (*list == t) return true;
    return false;
}

static const int special_tags[] = {
    T_address, T_applet, T_area, T_article, T_aside, T_base, T_blockquote, T_body, T_br, T_button,
    T_caption, T_center, T_col, T_colgroup, T_dd, T_details, T_dir, T_div, T_dl, T_dt, T_embed,
    T_fieldset, T_figcaption, T_figure, T_footer, T_form, T_frame, T_frameset, T_h1, T_h2, T_h3, T_h4,
    T_h5, T_h6, T_head, T_header, T_hgroup, T_hr, T_html, T_iframe, T_img, T_input, T_keygen, T_li,
    T_link, T_listing, T_main, T_marquee, T_menu, T_meta, T_nav, T_noembed, T_noframes, T_noscript,
    T_object, T_ol, T_p, T_param, T_plaintext, T_pre, T_script, T_search, T_section, T_select, T_source,
    T_style, T_summary, T_table, T_tbody, T_td, T_template, T_textarea, T_tfoot, T_th, T_thead, T_title,
    T_tr, T_track, T_ul, T_wbr, T_xmp, 0};

static const int scope_base[] = {T_applet, T_caption, T_html, T_table, T_td, T_th, T_marquee, T_object, T_template, 0};

/* is an element with this tag open, within the given scope? returns its stack index or -1 */
static int in_scope(struct html_parser *p, int tag, int kind /* 0 normal, 1 button, 2 list item, 3 table */) {
    for (int i = p->sp - 1; i >= 0; i--) {
        node_t *e = p->stack[i];
        if (e->tag == tag && !e->foreign) return i;
        if (e->foreign) continue;
        if (kind == 3) {
            if (e->tag == T_html || e->tag == T_table || e->tag == T_template) return -1;
            continue;
        }
        if (tag_in(e->tag, scope_base)) return -1;
        if (kind == 1 && e->tag == T_button) return -1;
        if (kind == 2 && (e->tag == T_ol || e->tag == T_ul)) return -1;
    }
    return -1;
}

static void pop_to(struct html_parser *p, int idx) {
    if (idx >= 1) p->sp = idx;
}

static void close_p(struct html_parser *p) {
    int i = in_scope(p, T_p, 1);
    if (i >= 0) pop_to(p, i);
}

static bool is_heading(int t) { return t >= T_h1 && t <= T_h6; }

static node_t *insert_elem(struct html_parser *p, bool foreign) {
    node_t *e = new_elem(p, p->tname, p->tlen, foreign);
    set_attrs(p, e);
    append(cur(p), e);
    return e;
}

/* read raw text up to </tag>; RCDATA decodes character references */
static void read_raw(struct html_parser *p, const char *tag, bool rcdata, sbuf *out) {
    size_t tl = strlen(tag);
    while (p->i < p->n) {
        const char *s = p->s + p->i;
        if (s[0] == '<' && p->i + 2 + tl <= p->n && s[1] == '/') {
            bool m = true;
            for (size_t k = 0; k < tl; k++)
                if (lower((unsigned char)s[2 + k]) != tag[k]) {
                    m = false;
                    break;
                }
            char after = p->i + 2 + tl < p->n ? s[2 + tl] : '>';
            if (m && (after == '>' || after == '/' || is_space((unsigned char)after))) {
                p->i += 2 + tl;
                while (p->i < p->n && p->s[p->i] != '>') p->i++;
                if (p->i < p->n) p->i++;
                return;
            }
        }
        if (rcdata && s[0] == '&') {
            size_t k = char_ref(s, p->n - p->i, false, out);
            if (k) {
                p->i += k;
                continue;
            }
        }
        sb_putc(out, s[0]);
        p->i++;
    }
}

static void raw_element(struct html_parser *p, node_t *e, bool rcdata) {
    sbuf *t = &p->raw;
    t->n = 0;
    read_raw(p, e->name, rcdata, t);
    if (e->tag == T_textarea && t->n && t->p[0] == '\n') memmove(t->p, t->p + 1, --t->n);
    if (t->n) {
        node_t *tn = ar_alloc(p->a, sizeof *tn);
        tn->owned_next = p->d->owned_nodes;
        p->d->owned_nodes = tn;
        tn->type = N_TEXT;
        tn->text = ar_strndup(p->a, t->p, t->n);
        tn->textlen = t->n;
        tn->image = -1;
        append(e, tn);
    }
    t->n = 0;
    if (e->tag == T_script) {
        if (p->scripting && !node_ancestor(e, T_template)) p->yield = e;
        else if (p->fragment || node_ancestor(e, T_template)) e->script_started = true;
    }
}

static const int head_tags[] = {T_base, T_link, T_meta, T_style, T_script, T_title, T_noscript, T_template, 0};
static const int closes_p[] = {
    T_address, T_article, T_aside, T_blockquote, T_center, T_details, T_dialog, T_dir, T_div, T_dl,
    T_fieldset, T_figcaption, T_figure, T_footer, T_header, T_hgroup, T_main, T_menu, T_nav, T_ol, T_p,
    T_search, T_section, T_summary, T_ul, 0};
static const int void_tags[] = {T_area, T_base, T_br, T_col, T_embed, T_hr, T_img, T_input, T_keygen, T_link,
                                T_meta, T_param, T_source, T_track, T_wbr, T_image, 0};
static const int html_breakout[] = {T_b, T_big, T_blockquote, T_body, T_br, T_center, T_code, T_dd, T_div, T_dl,
                                    T_dt, T_em, T_embed, T_h1, T_h2, T_h3, T_h4, T_h5, T_h6, T_head, T_hr, T_i,
                                    T_img, T_li, T_listing, T_menu, T_meta, T_nobr, T_ol, T_p, T_pre, T_ruby,
                                    T_s, T_small, T_span, T_strong, T_strike, T_sub, T_sup, T_table, T_tt, T_u,
                                    T_ul, T_var, 0};

/* <!DOCTYPE ...>: standards mode for "html" without a legacy public identifier (a simplification
   of the HTML spec's quirks rules) */
static void doctype(struct html_parser *p, const char *s, size_t n) {
    char buf[256];
    size_t k = n < sizeof buf - 1 ? n : sizeof buf - 1;
    for (size_t i = 0; i < k; i++) buf[i] = (char)lower((unsigned char)s[i]);
    buf[k] = 0;
    const char *b = buf;
    while (is_space((unsigned char)*b)) b++;
    if (strncmp(b, "html", 4) || (b[4] && !is_space((unsigned char)b[4]) && b[4] != '>')) return; /* quirks */
    bool has_system = strstr(b, "http://") || strstr(b, "https://");
    if (strstr(b, "html 3") || strstr(b, "html 2") || strstr(b, "html pro") || strstr(b, "w3o//dtd") ||
        strstr(b, "-//ietf//") || strstr(b, "netscape") || strstr(b, "-//webtechs"))
        return;
    if ((strstr(b, "4.01 transitional") || strstr(b, "4.01 frameset") || strstr(b, "4.0 transitional")) && !has_system)
        return;
    p->d->quirks = false;
}

static void start_tag(struct html_parser *p) {
    int t = tag_lookup(p->tname, p->tlen);
    if (!p->sp) ensure_html(p);
    /* Inert template contents must not reset the head stack or execute scripts. */
    if (node_ancestor(cur(p), T_template)) {
        flush_text(p);
        node_t *e = insert_elem(p, cur(p)->foreign || t == T_svg || t == T_math);
        if (tag_in(t, void_tags) || p->self_closing) return;
        if (t == T_script || t == T_style) raw_element(p, e, false);
        else if (t == T_title || t == T_textarea) raw_element(p, e, true);
        else push(p, e);
        return;
    }

    /* inside SVG/MathML everything nests as written, until an HTML element breaks out */
    if (cur(p)->foreign) {
        if (!tag_in(t, html_breakout)) {
            flush_text(p);
            node_t *e = insert_elem(p, true);
            if (!p->self_closing) push(p, e);
            return;
        }
        while (p->sp > 1 && cur(p)->foreign) pop(p);
    }

    if (t == T_html) {
        ensure_html(p);
        if (!p->html->nattrs) set_attrs(p, p->html); /* <html lang=.. class=..> */
        return;
    }
    if (p->mode != M_IN_BODY && !node_ancestor(cur(p), T_template) && tag_in(t, head_tags)) {
        flush_text(p);
        ensure_head(p);
        p->sp = 1;
        push(p, p->head);
        p->mode = M_IN_HEAD;
        node_t *e = insert_elem(p, false);
        if (t == T_title) raw_element(p, e, true);
        else if (t == T_style || t == T_script) raw_element(p, e, false);
        else if (t == T_noscript && p->scripting) raw_element(p, e, false);
        else if (t == T_noscript || t == T_template) push(p, e);
        return;
    }
    if (t == T_head) {
        ensure_head(p);
        return;
    }
    if (t == T_body) {
        if (p->body) { /* a second <body>: just merge its attributes */
            return;
        }
        flush_text(p);
        ensure_head(p);
        p->sp = 1;
        p->body = insert_elem(p, false);
        push(p, p->body);
        p->mode = M_IN_BODY;
        return;
    }
    flush_text(p);
    ensure_body(p);

    if (tag_in(t, closes_p)) close_p(p);
    switch (t) {
    case T_h1: case T_h2: case T_h3: case T_h4: case T_h5: case T_h6:
        close_p(p);
        if (is_heading(cur(p)->tag)) pop(p);
        break;
    case T_pre: case T_listing: case T_form: case T_plaintext: case T_hr: case T_xmp: case T_table:
        close_p(p);
        break;
    case T_li: case T_dd: case T_dt:
        for (int i = p->sp - 1; i > 0; i--) {
            node_t *e = p->stack[i];
            bool match = t == T_li ? e->tag == T_li : (e->tag == T_dd || e->tag == T_dt);
            if (match) {
                pop_to(p, i);
                break;
            }
            if (tag_in(e->tag, special_tags) && e->tag != T_address && e->tag != T_div && e->tag != T_p) break;
        }
        close_p(p);
        break;
    case T_button: {
        int i = in_scope(p, T_button, 0);
        if (i >= 0) pop_to(p, i);
        break;
    }
    case T_a: {
        for (int i = p->sp - 1; i > 0; i--) {
            if (p->stack[i]->tag == T_a) {
                pop_to(p, i);
                break;
            }
            if (p->stack[i]->tag == T_table || p->stack[i]->tag == T_td || p->stack[i]->tag == T_th) break;
        }
        break;
    }
    case T_nobr: {
        int i = in_scope(p, T_nobr, 0);
        if (i >= 0) pop_to(p, i);
        break;
    }
    case T_option:
        if (cur(p)->tag == T_option) pop(p);
        break;
    case T_optgroup:
        if (cur(p)->tag == T_option) pop(p);
        if (cur(p)->tag == T_optgroup) pop(p);
        break;
    case T_td: case T_th: case T_tr: case T_tbody: case T_thead: case T_tfoot: case T_caption:
    case T_colgroup: case T_col: {
        int ti = in_scope(p, T_table, 3);
        if (ti < 0) return; /* table parts outside a table are ignored */
        if (t == T_td || t == T_th) {
            for (int i = p->sp - 1; i > ti; i--)
                if (p->stack[i]->tag == T_td || p->stack[i]->tag == T_th) {
                    pop_to(p, i);
                    break;
                }
            if (cur(p)->tag != T_tr) {
                /* a cell needs a row: close back to the row group or table and add one */
                while (p->sp - 1 > ti && cur(p)->tag != T_tbody && cur(p)->tag != T_thead && cur(p)->tag != T_tfoot)
                    pop(p);
                node_t *tr = new_elem(p, "tr", 2, false);
                append(cur(p), tr);
                push(p, tr);
            }
        } else if (t == T_tr) {
            for (int i = p->sp - 1; i > ti; i--)
                if (p->stack[i]->tag == T_tr) {
                    pop_to(p, i);
                    break;
                }
            while (p->sp - 1 > ti && cur(p)->tag != T_tbody && cur(p)->tag != T_thead && cur(p)->tag != T_tfoot)
                pop(p);
        } else {
            pop_to(p, ti + 1); /* back to the table */
        }
        break;
    }
    case T_frameset: case T_frame:
        return;
    }

    bool foreign = t == T_svg || t == T_math;
    node_t *e = insert_elem(p, foreign);
    if (t == T_image) {
        e->tag = T_img;
        e->name = e->raw_name = "img";
    }
    if (tag_in(t, void_tags)) return;
    if (foreign && p->self_closing) return;
    switch (t) {
    case T_noscript:
        if (!p->scripting) break;
        raw_element(p, e, false);
        return;
    case T_script: case T_style: case T_xmp: case T_iframe: case T_noembed: case T_noframes:
        raw_element(p, e, false);
        return;
    case T_title: case T_textarea:
        raw_element(p, e, true);
        return;
    case T_plaintext: {
        insert_text(p, "", 0);
        node_t *keep = cur(p);
        push(p, e);
        insert_text(p, p->s + p->i, p->n - p->i);
        p->i = p->n;
        (void)keep;
        return;
    }
    case T_pre: case T_listing:
        p->skip_lf = true;
        break;
    }
    push(p, e);
}

static void end_tag(struct html_parser *p) {
    int t = tag_lookup(p->tname, p->tlen);
    flush_text(p);
    if (!p->sp) ensure_html(p);
    if (cur(p)->foreign) {
        for (int i = p->sp - 1; i > 0 && p->stack[i]->foreign; i--)
            if (strn_ieq(p->tname, p->stack[i]->raw_name, p->tlen) || strn_ieq(p->tname, p->stack[i]->name, p->tlen)) {
                pop_to(p, i);
                return;
            }
        if (!tag_in(t, html_breakout) && t != T_svg && t != T_math) return;
        while (p->sp > 1 && cur(p)->foreign) pop(p);
    }
    switch (t) {
    case T_head:
        if (p->mode == M_IN_HEAD) {
            p->sp = 1;
            p->mode = M_AFTER_HEAD;
        }
        return;
    case T_noscript: case T_template:
        if (p->mode == M_IN_HEAD) {
            int i = in_scope(p, t, 0);
            if (i >= 0) pop_to(p, i);
            return;
        }
        break;
    case T_body: case T_html:
        return;
    case T_br:
        ensure_body(p);
        append(cur(p), new_elem(p, "br", 2, false));
        return;
    case T_p: {
        int i = in_scope(p, T_p, 1);
        if (i >= 0) pop_to(p, i);
        else if (p->mode == M_IN_BODY) append(cur(p), new_elem(p, "p", 1, false)); /* </p> alone makes an empty p */
        return;
    }
    case T_li: {
        int i = in_scope(p, T_li, 2);
        if (i >= 0) pop_to(p, i);
        return;
    }
    case T_h1: case T_h2: case T_h3: case T_h4: case T_h5: case T_h6:
        for (int i = p->sp - 1; i > 0; i--) {
            if (is_heading(p->stack[i]->tag)) {
                pop_to(p, i);
                return;
            }
            if (tag_in(p->stack[i]->tag, scope_base)) return;
        }
        return;
    case T_table: case T_tbody: case T_thead: case T_tfoot: case T_tr: case T_td: case T_th: case T_caption: {
        int i = in_scope(p, t, 3);
        if (i >= 0) pop_to(p, i);
        return;
    }
    }
    /* any other end tag: close the nearest open element with this name, unless a special
       element is in the way */
    for (int i = p->sp - 1; i > 0; i--) {
        node_t *e = p->stack[i];
        if (t ? e->tag == t : strn_ieq(p->tname, e->name, p->tlen)) {
            pop_to(p, i);
            return;
        }
        if (tag_in(e->tag, special_tags)) return;
    }
}

/* ---------------------------------------------------------------- tokenizer */
static void read_tag(struct html_parser *p, bool end) {
    const char *s = p->s;
    size_t n = p->n;
    size_t st = p->i;
    while (p->i < n && !is_space((unsigned char)s[p->i]) && s[p->i] != '/' && s[p->i] != '>') p->i++;
    p->tlen = p->i - st < sizeof p->tname - 1 ? p->i - st : sizeof p->tname - 1;
    memcpy(p->tname, s + st, p->tlen);
    p->tname[p->tlen] = 0;
    p->ntattrs = 0;
    p->self_closing = false;
    for (;;) {
        while (p->i < n && (is_space((unsigned char)s[p->i]) || s[p->i] == '/')) {
            if (s[p->i] == '/' && p->i + 1 < n && s[p->i + 1] == '>') p->self_closing = true;
            p->i++;
        }
        if (p->i >= n) return;
        if (s[p->i] == '>') {
            p->i++;
            return;
        }
        /* attribute name */
        size_t ns = p->i;
        p->i++; /* the first character may be anything, even '=' */
        while (p->i < n && !is_space((unsigned char)s[p->i]) && s[p->i] != '/' && s[p->i] != '>' && s[p->i] != '=')
            p->i++;
        size_t nl = p->i - ns;
        while (p->i < n && is_space((unsigned char)s[p->i])) p->i++;
        p->aval.n = 0;
        if (p->i < n && s[p->i] == '=') {
            p->i++;
            while (p->i < n && is_space((unsigned char)s[p->i])) p->i++;
            char q = p->i < n ? s[p->i] : 0;
            if (q == '"' || q == '\'') {
                p->i++;
                while (p->i < n && s[p->i] != q) {
                    if (s[p->i] == '&') {
                        size_t k = char_ref(s + p->i, n - p->i, true, &p->aval);
                        if (k) {
                            p->i += k;
                            continue;
                        }
                    }
                    sb_putc(&p->aval, s[p->i++]);
                }
                if (p->i < n) p->i++;
            } else {
                while (p->i < n && !is_space((unsigned char)s[p->i]) && s[p->i] != '>') {
                    if (s[p->i] == '&') {
                        size_t k = char_ref(s + p->i, n - p->i, true, &p->aval);
                        if (k) {
                            p->i += k;
                            continue;
                        }
                    }
                    sb_putc(&p->aval, s[p->i++]);
                }
            }
        }
        if (end || p->ntattrs >= (int)(sizeof p->tattrs / sizeof *p->tattrs)) continue;
        char lname[128];
        size_t ln = nl < sizeof lname - 1 ? nl : sizeof lname - 1;
        for (size_t k = 0; k < ln; k++) lname[k] = (char)lower((unsigned char)s[ns + k]);
        lname[ln] = 0;
        bool dup = false;
        for (int k = 0; k < p->ntattrs; k++)
            if (!strcmp(p->tattrs[k].name, lname)) dup = true;
        if (dup) continue;
        struct attr *a = &p->tattrs[p->ntattrs++];
        a->name = ar_strdup(p->a, lname);
        a->raw = ar_strndup(p->a, s + ns, ln);
        a->value = ar_strndup(p->a, p->aval.p ? p->aval.p : "", p->aval.n);
    }
}

static void skip_past(struct html_parser *p, const char *end) {
    const char *f = NULL;
    size_t el = strlen(end);
    for (size_t k = p->i; k + el <= p->n; k++)
        if (!memcmp(p->s + k, end, el)) {
            f = p->s + k;
            break;
        }
    p->i = f ? (size_t)(f - p->s) + el : p->n;
}

static void tokenize(struct html_parser *p) {
    const char *s = p->s;
    size_t n = p->n;
    while (p->i < n) {
        char c = s[p->i];
        if (c == '<' && p->i + 1 < n) {
            char c1 = s[p->i + 1];
            if (c1 == '!') {
                if (p->i + 3 < n && s[p->i + 2] == '-' && s[p->i + 3] == '-') {
                    p->i += 4;
                    if (p->i < n && s[p->i] == '>') p->i++;
                    else if (p->i + 1 < n && s[p->i] == '-' && s[p->i + 1] == '>') p->i += 2;
                    else skip_past(p, "-->");
                } else if (p->i + 9 <= n && !memcmp(s + p->i, "<![CDATA[", 9) && p->sp && cur(p)->foreign) {
                    p->i += 9;
                    size_t st = p->i;
                    skip_past(p, "]]>");
                    size_t e = p->i >= st + 3 ? p->i - 3 : st;
                    sb_put(&p->text, s + st, e - st);
                } else {
                    size_t st = p->i;
                    skip_past(p, ">");
                    if (st + 9 < n && strn_ieq(s + st + 2, "doctype", 7) && !p->html) doctype(p, s + st + 9, p->i - st - 9);
                }
                continue;
            }
            if (c1 == '?') {
                skip_past(p, ">");
                continue;
            }
            if (c1 == '/') {
                char c2 = p->i + 2 < n ? s[p->i + 2] : 0;
                if ((c2 | 32) >= 'a' && (c2 | 32) <= 'z') {
                    p->i += 2;
                    read_tag(p, true);
                    end_tag(p);
                } else if (c2 == '>') {
                    p->i += 3;
                } else {
                    skip_past(p, ">");
                }
                continue;
            }
            if ((c1 | 32) >= 'a' && (c1 | 32) <= 'z') {
                p->i++;
                read_tag(p, false);
                start_tag(p);
                if (p->yield) return;
                continue;
            }
        }
        if (c == '&') {
            size_t k = char_ref(s + p->i, n - p->i, false, &p->text);
            if (k) {
                p->i += k;
                p->skip_lf = false;
                continue;
            }
        }
        if (p->skip_lf) {
            p->skip_lf = false;
            if (c == '\n') {
                p->i++;
                continue;
            }
        }
        /* a run of plain text */
        size_t st = p->i;
        while (p->i < n && s[p->i] != '<' && s[p->i] != '&') p->i++;
        if (p->i == st) p->i++;
        sb_put(&p->text, s + st, p->i - st);
    }
    flush_text(p);
}

/* ---------------------------------------------------------------- charset handling */
static bool valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) return false;
        for (size_t k = 1; k < len; k++)
            if ((s[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

static bool is_latin(const char *cs) {
    return cs && (str_ieq(cs, "windows-1252") || str_ieq(cs, "iso-8859-1") || str_ieq(cs, "latin1") ||
                  str_ieq(cs, "iso-8859-15") || str_ieq(cs, "us-ascii") || str_ieq(cs, "cp1252"));
}

/* look for <meta charset=...> in the first bytes */
static void prescan_charset(const char *s, size_t n, char *out, size_t cap) {
    out[0] = 0;
    size_t lim = n < 2048 ? n : 2048;
    for (size_t i = 0; i + 8 < lim; i++) {
        if (strncasecmp(s + i, "charset", 7)) continue;
        size_t k = i + 7;
        while (k < lim && is_space((unsigned char)s[k])) k++;
        if (k >= lim || s[k] != '=') continue;
        k++;
        while (k < lim && (is_space((unsigned char)s[k]) || s[k] == '"' || s[k] == '\'')) k++;
        size_t o = 0;
        while (k < lim && o < cap - 1 && (isalnum((unsigned char)s[k]) || s[k] == '-' || s[k] == '_')) out[o++] = s[k++];
        out[o] = 0;
        return;
    }
}

struct html_parser *html_begin(web_doc *d, const char *src, size_t n, const char *charset, bool scripting) {
    /* to UTF-8, with CR LF and CR as LF and NUL as U+FFFD */
    char meta_cs[32];
    prescan_charset(src, n, meta_cs, sizeof meta_cs);
    const char *cs = charset && *charset ? charset : meta_cs[0] ? meta_cs : NULL;
    if (n >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        src += 3;
        n -= 3;
        cs = "utf-8";
    }
    bool latin = is_latin(cs) || (!cs && !valid_utf8((const unsigned char *)src, n));
    if (cs && !latin && !valid_utf8((const unsigned char *)src, n) && !str_ieq(cs, "utf-8")) latin = true;
    sbuf in = {0};
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r') {
            sb_putc(&in, '\n');
            if (i + 1 < n && src[i + 1] == '\n') i++;
        } else if (c == 0) sb_utf8(&in, 0xFFFD);
        else if (latin && c >= 0x80) sb_utf8(&in, c < 0xA0 ? win1252[c - 0x80] : c);
        else sb_putc(&in, (char)c);
    }

    struct html_parser *p = calloc(1, sizeof *p);
    if (!p) { sb_free(&in); return NULL; }
    p->d = d;
    p->a = &d->mem;
    p->scripting = scripting;
    p->input = in;
    d->quirks = true; /* until a doctype says otherwise */
    p->s = in.p ? in.p : "";
    p->n = in.n;
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) {
        d->mem.trap = old;
        html_finish(p);
        return NULL;
    }
    p->doc = ar_alloc(p->a, sizeof(node_t));
    p->doc->type = N_DOC;
    p->doc->image = -1;
    p->doc->owned_next = d->owned_nodes;
    d->owned_nodes = p->doc;
    d->root = p->doc;
    d->mem.trap = old;
    return p;
}

int html_resume(struct html_parser *p, node_t **script) {
    if (script) *script = NULL;
    if (!p || p->failed) return -1;
    if (p->finished) return 0;
    jmp_buf trap;
    jmp_buf *old = p->a->trap;
    p->a->trap = &trap;
    if (setjmp(trap)) {
        p->a->trap = old;
        p->failed = true;
        return -1;
    }
    p->yield = NULL;
    p->write_offset = 0;
    tokenize(p);
    if (!p->yield) {
        if (!p->fragment) ensure_body(p);
        p->finished = true;
    }
    if (!p->fragment) {
        p->d->html = p->html;
        p->d->head = p->head;
        p->d->body = p->body;
        p->d->resources_dirty = true;
        p->d->dirty = p->d->need_style = true;
    }
    p->a->trap = old;
    if (script) *script = p->yield;
    return p->yield ? 1 : 0;
}

bool html_write(struct html_parser *p, const char *text, size_t n) {
    if (!p || p->finished || !p->yield || !p->scripting || n > (16u << 20) || p->n > (16u << 20) - n)
        return false;
    size_t at = p->i + p->write_offset;
    char *next = realloc(p->input.p, p->n + n + 1);
    if (!next) return false;
    memmove(next + at + n, next + at, p->n - at);
    memcpy(next + at, text, n);
    p->n += n;
    next[p->n] = 0;
    p->input.p = next;
    p->input.n = p->n;
    p->input.cap = p->n + 1;
    p->s = next;
    p->write_offset += n;
    return true;
}

void html_finish(struct html_parser *p) {
    if (!p) return;
    sb_free(&p->text);
    sb_free(&p->aval);
    sb_free(&p->input);
    sb_free(&p->raw);
    free(p);
}

node_t *html_parse(web_doc *d, const char *src, size_t n, const char *charset) {
    struct html_parser *p = html_begin(d, src, n, charset, false);
    if (!p) return NULL;
    node_t *script;
    int r = html_resume(p, &script);
    node_t *root = r < 0 ? NULL : d->root;
    html_finish(p);
    return root;
}

node_t *html_fragment(web_doc *d, node_t *context, const char *src, size_t n) {
    node_t *saved_root = d->root;
    bool quirks = d->quirks;
    struct html_parser *p = html_begin(d, src, n, "utf-8", false);
    d->root = saved_root;
    d->quirks = quirks;
    if (!p) return NULL;
    p->fragment = true;
    p->doc->type = N_FRAGMENT;
    jmp_buf trap;
    jmp_buf *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) {
        d->mem.trap = old;
        html_finish(p);
        return NULL;
    }
    p->html = new_elem(p, "html", 4, false);
    p->body = new_elem(p, context && context->name ? context->name : "div",
                       context && context->name ? strlen(context->name) : 3,
                       context && context->foreign);
    append(p->doc, p->html);
    append(p->html, p->body);
    p->stack[0] = p->html;
    p->stack[1] = p->body;
    p->sp = 2;
    p->mode = M_IN_BODY;
    if (context && (context->tag == T_textarea || context->tag == T_title ||
                    context->tag == T_script || context->tag == T_style)) {
        raw_element(p, p->body, context->tag == T_textarea || context->tag == T_title);
        p->i = p->n;
    }
    d->mem.trap = old;
    node_t *ignored;
    if (html_resume(p, &ignored) < 0) { html_finish(p); return NULL; }
    node_t *frag = p->doc;
    node_t *first = p->body->first, *last = p->body->last;
    p->body->first = p->body->last = NULL;
    frag->first = first;
    frag->last = last;
    for (node_t *c = first; c; c = c->next) c->parent = frag;
    html_finish(p);
    return frag;
}

/* ---------------------------------------------------------------- node helpers */
const char *node_attr(const node_t *n, const char *name) {
    for (int i = 0; i < n->nattrs; i++)
        if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

bool node_has_class(const node_t *n, const char *cls) {
    for (int i = 0; i < n->nclasses; i++)
        if (!strcmp(n->classes[i], cls)) return true;
    return false;
}

node_t *node_ancestor(node_t *n, int tag) {
    for (; n; n = n->parent)
        if (n->type == N_ELEM && n->tag == tag) return n;
    return NULL;
}

void node_text_content(const node_t *n, sbuf *out) {
    if (n->type == N_TEXT) {
        sb_put(out, n->text, n->textlen);
        return;
    }
    for (node_t *c = n->first; c; c = c->next) node_text_content(c, out);
}
