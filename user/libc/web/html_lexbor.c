/* Lexbor owns only the suspended HTML tree-builder state. Native node_t remains
   the DOM exposed to QuickJS, resources, selectors, style and layout. */
#include <stdio.h>
#include <ctype.h>
#include "html_lexbor.h"
#include "web_encoding.h"
#include <lexbor/html/tree/insertion_mode.h>
#include <lexbor/html/interfaces/form_element.h>
#include <lexbor/html/interfaces/script_element.h>
#include <lexbor/dom/interfaces/element.h>

#define HTML_INPUT_LIMIT (16u << 20)

/* Incoming token spans can point into previously supplied chunks. Never move
   or free a span while its tokenizer is alive, including document.write input. */
struct html_input {
    struct html_input *next, *owned_next;
    char *data;
    size_t length, offset, capacity;
};

static bool valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) return false;
        for (size_t k = 1; k < len; k++) if ((s[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

static bool is_latin(const char *cs) {
    return cs && (str_ieq(cs, "windows-1252") || str_ieq(cs, "iso-8859-1") || str_ieq(cs, "latin1") ||
                  str_ieq(cs, "iso-8859-15") || str_ieq(cs, "us-ascii") || str_ieq(cs, "cp1252"));
}

/* Preserve the existing transport/BOM/meta and Shift-JIS policy. Lexbor's tree
   builder does not itself decode a transport charset or restart at <meta>. */
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

struct decoded_html { char *data; size_t length, capacity; bool after_cr; };

static bool decoded_bytes(struct decoded_html *text, const char *s, size_t n) {
    if (n > text->capacity - text->length) return false;
    memcpy(text->data + text->length, s, n);
    text->length += n;
    return true;
}

static bool emit_decoded_html(void *opaque, uint32_t cp, bool malformed) {
    struct decoded_html *text = opaque;
    (void)malformed;
    if (cp == '\r') {
        text->after_cr = true;
        return decoded_bytes(text, "\n", 1);
    }
    bool skip = cp == '\n' && text->after_cr;
    text->after_cr = false;
    if (skip) return true;
    char utf8[4];
    return decoded_bytes(text, utf8, (size_t)utf8_put(utf8, cp ? cp : 0xfffd));
}

static char *decode_input(const char *src, size_t n, const char *charset, size_t *length) {
    if ((!src && n) || n > HTML_INPUT_LIMIT) return NULL;
    if (!src) src = "";
    char meta_cs[32];
    prescan_charset(src, n, meta_cs, sizeof meta_cs);
    const char *cs = charset && *charset ? charset : meta_cs[0] ? meta_cs : NULL;
    if (n >= 3 && (unsigned char)src[0] == 0xef && (unsigned char)src[1] == 0xbb && (unsigned char)src[2] == 0xbf) {
        src += 3; n -= 3; cs = "utf-8";
    }
    size_t capacity = n * 3;
    if (capacity > HTML_INPUT_LIMIT) capacity = HTML_INPUT_LIMIT;
    struct decoded_html text = {malloc(capacity + 1), 0, capacity, false};
    if (!text.data) return NULL;
    bool ok = true;
    if (web_shift_jis_label(cs)) {
        web_shift_jis_decoder decoder = {0};
        ok = web_shift_jis_decode(&decoder, (const uint8_t *)src, n, true, emit_decoded_html, &text);
    } else {
        static const uint16_t win1252[32] = {
            0x20ac, 0x81, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8d, 0x017d, 0x8f,
            0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x9d, 0x017e, 0x0178
        };
        bool latin = is_latin(cs) || (!cs && !valid_utf8((const unsigned char *)src, n));
        if (cs && !latin && !valid_utf8((const unsigned char *)src, n) && !str_ieq(cs, "utf-8")) latin = true;
        for (size_t i = 0; ok && i < n; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c == '\r') {
                ok = decoded_bytes(&text, "\n", 1);
                if (i + 1 < n && src[i + 1] == '\n') i++;
            } else if (!c || (latin && c >= 0x80)) {
                uint32_t cp = !c ? 0xfffd : c < 0xa0 ? win1252[c - 0x80] : c;
                char utf8[4];
                ok = decoded_bytes(&text, utf8, (size_t)utf8_put(utf8, cp));
            } else ok = decoded_bytes(&text, src + i, 1);
        }
    }
    if (!ok) { free(text.data); return NULL; }
    text.data[text.length] = 0;
    *length = text.length;
    return text.data;
}

static bool add_input(struct html_parser *p, char *data, size_t length, bool writing) {
    if (length > HTML_INPUT_LIMIT - p->input_bytes || p->input_spans >= HTML_INPUT_LIMIT / sizeof(struct html_input)) {
        free(data); return false;
    }
    struct html_input *in = calloc(1, sizeof *in);
    if (!in) { free(data); return false; }
    in->data = data; in->length = length; in->capacity = length + 1;
    in->owned_next = p->inputs; p->inputs = in;
    if (writing && p->write_tail) {
        in->next = p->write_tail->next;
        p->write_tail->next = in;
    } else {
        in->next = p->input;
        p->input = in;
    }
    if (writing) p->write_tail = in;
    p->input_bytes += length;
    p->input_spans++;
    return true;
}

/* Always forward to Lexbor's tree callback first. A successful callback must
   return its token; NULL/non-OK is a fatal parser error, not a resumable pause. */
static lxb_html_token_t *token_done(lxb_html_tokenizer_t *tkz, lxb_html_token_t *token, void *ctx) {
    struct html_parser *p = ctx;
    lxb_html_tree_t *tree = p->lex->tree;
    lxb_dom_node_t *current = lxb_html_tree_current_node(tree);
    bool script_text = tree->mode == lxb_html_tree_insertion_mode_text && current &&
                       current->ns == LXB_NS_HTML && current->local_name == LXB_TAG_SCRIPT;
    bool closed = script_text && token->tag_id == LXB_TAG_SCRIPT && (token->type & LXB_HTML_TOKEN_TYPE_CLOSE);
    bool execute = closed && p->scripting && !p->fragment && !p->d->inert &&
                   !lxb_html_tree_parsing_template_contents(tree);
    if (script_text && token->tag_id == LXB_TAG__END_OF_FILE) p->eof_script = current;
    lxb_html_token_t *result = p->original_callback(tkz, token, p->original_callback_context);
    if (result && execute) p->pending_script = current;
    return result;
}

static bool install_callback(struct html_parser *p) {
    lxb_html_tokenizer_t *tkz = p->lex->tkz;
    p->original_callback = tkz->callback_token_done;
    p->original_callback_context = tkz->callback_token_ctx;
    lxb_html_tokenizer_callback_token_done_set(tkz, token_done, p);
    return true;
}

static struct html_parser *parser_create(web_doc *d, bool scripting) {
    if (!d) return NULL;
    struct html_parser *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->d = d; p->scripting = scripting;
    p->lex = lxb_html_parser_create();
    if (!p->lex || lxb_html_parser_init(p->lex) != LXB_STATUS_OK) { html_finish(p); return NULL; }
    lxb_html_parser_scripting_set(p->lex, scripting);
    return p;
}

struct html_parser *html_begin(web_doc *d, const char *src, size_t n, const char *charset, bool scripting) {
    size_t length = 0;
    char *input = decode_input(src, n, charset, &length);
    if (!input) return NULL;
    struct html_parser *p = parser_create(d, scripting);
    if (!p) { free(input); return NULL; }
    if (!add_input(p, input, length, false)) { html_finish(p); return NULL; }
    p->document = lxb_html_parse_chunk_begin(p->lex);
    if (!p->document) { html_finish(p); return NULL; }
    p->root = lxb_dom_interface_node(p->document);
    install_callback(p);
    if (!html_bridge_init(p) || !html_bridge_import(p)) { html_finish(p); return NULL; }
    d->root = p->native_root;
    d->quirks = true;
    return p;
}

int html_resume(struct html_parser *p, node_t **script) {
    if (script) *script = NULL;
    if (!p || p->failed) return -1;
    if (p->finished) return 0;
    doc_dom_budget(p->d);
    if (p->yielded && !html_bridge_export(p)) { p->failed = true; return -1; }
    p->yielded = false; p->pending_script = NULL; p->write_tail = NULL;
    while (p->input && !p->pending_script) {
        struct html_input *in = p->input;
        if (in->offset == in->length) { p->input = in->next; continue; }
        const char *start = in->data + in->offset;
        size_t remaining = in->length - in->offset;
        const char *gt = memchr(start, '>', remaining);
        size_t size = gt ? (size_t)(gt - start) + 1 : remaining;
        /* An actual script-close token can only complete at a '>'. Ending the
           input chunk exactly there lets all tokenizer cleanup finish normally
           before yielding, without consuming any following document input. */
        lxb_status_t status = p->fragment ? lxb_html_parse_fragment_chunk_process(p->lex, (const lxb_char_t *)start, size) :
                                           lxb_html_parse_chunk_process(p->lex, (const lxb_char_t *)start, size);
        if (status != LXB_STATUS_OK || p->lex->tree->open_elements->length > 400) {
            p->failed = true; return -1;
        }
        in->offset += size;
    }
    if (!p->pending_script) {
        bool ok = p->fragment ? lxb_html_parse_fragment_chunk_end(p->lex) != NULL :
                               lxb_html_parse_chunk_end(p->lex) == LXB_STATUS_OK;
        if (!ok) { p->failed = true; return -1; }
        p->finished = true;
    }
    if (!html_bridge_import(p)) { p->failed = true; return -1; }
    if (p->eof_script) {
        node_t *node = html_bridge_native(p, p->eof_script);
        if (!node) { p->failed = true; return -1; }
        node->script_started = true;
    }
    if (p->pending_script) {
        node_t *node = html_bridge_native(p, p->pending_script);
        if (!node) { p->failed = true; return -1; }
        p->yielded = true;
        if (script) *script = node;
    }
    return p->yielded ? 1 : 0;
}

bool html_write(struct html_parser *p, const char *text, size_t n) {
    if (!p || p->finished || p->failed || !p->yielded || !p->scripting || (!text && n) ||
        n > HTML_INPUT_LIMIT - p->input_bytes) return false;
    if (!n) return true;
    /* Consecutive writes during one suspended script have not reached Lexbor
       yet, so they may share a growing span. Previously fed spans stay intact.
       This also bounds bookkeeping for many one-byte/empty writes. */
    if (p->write_tail) {
        struct html_input *in = p->write_tail;
        size_t length = in->length + n;
        if (length + 1 > in->capacity) {
            size_t capacity = in->capacity * 2;
            if (capacity < length + 1) capacity = length + 1;
            if (capacity > HTML_INPUT_LIMIT + 1) capacity = HTML_INPUT_LIMIT + 1;
            char *data = realloc(in->data, capacity);
            if (!data) return false;
            in->data = data; in->capacity = capacity;
        }
        memcpy(in->data + in->length, text, n);
        in->length = length; in->data[length] = 0;
        p->input_bytes += n;
        return true;
    }
    char *data = malloc(n + 1);
    if (!data) return false;
    if (n) memcpy(data, text, n);
    data[n] = 0;
    return add_input(p, data, n, true);
}

void html_finish(struct html_parser *p) {
    if (!p) return;
    html_bridge_destroy(p);
    if (p->lex) {
        /* Fragment end normally releases its inherited document/context. A
           cancelled fragment must release those too, before the original pool. */
        lxb_html_tree_t *tree = p->lex->tree;
        if (p->fragment && tree) {
            if (p->lex->form) { lxb_html_interface_destroy(p->lex->form); p->lex->form = NULL; }
            if (tree->fragment) { lxb_html_interface_destroy(tree->fragment); tree->fragment = NULL; }
            if (tree->document && tree->document != p->document) {
                lxb_html_document_interface_destroy(tree->document); tree->document = NULL;
            }
        }
        lxb_html_parser_destroy(p->lex);
    }
    if (p->document) lxb_html_document_destroy(p->document);
    for (struct html_input *in = p->inputs; in;) {
        struct html_input *next = in->owned_next;
        free(in->data); free(in); in = next;
    }
    free(p);
}

node_t *html_parse(web_doc *d, const char *src, size_t n, const char *charset) {
    struct html_parser *p = html_begin(d, src, n, charset, false);
    if (!p) return NULL;
    node_t *ignored;
    int result = html_resume(p, &ignored);
    node_t *root = result < 0 ? NULL : p->native_root;
    html_finish(p);
    return root;
}

static bool fragment_context(struct html_parser *p, node_t *context) {
    lxb_html_tree_t *tree = p->lex->tree;
    if (context) for (int i = 0; i < context->nattrs; i++) {
        struct attr *attr = &context->attrs[i];
        const char *name = attr->raw ? attr->raw : attr->name;
        if (!lxb_dom_element_set_attribute(lxb_dom_interface_element(tree->fragment),
                                           (const lxb_char_t *)name, strlen(name),
                                           (const lxb_char_t *)attr->value, strlen(attr->value))) return false;
    }
    /* Lexbor's by-tag fragment API cannot inspect native ancestors. Supply the
       form pointer required for form-context parsing without putting a fake
       form element into the stack of open elements. */
    for (node_t *node = context; node; node = node->parent) {
        if (node->type == N_ELEM && !node->foreign && node->tag == T_template) break;
        if (node->type == N_ELEM && !node->foreign && node->tag == T_form) {
            if (!p->lex->form) p->lex->form = lxb_html_interface_create(tree->document, LXB_TAG_FORM, LXB_NS_HTML);
            if (!p->lex->form) return false;
            tree->form = lxb_html_interface_form(p->lex->form);
            break;
        }
    }
    return true;
}

node_t *html_fragment(web_doc *d, node_t *context, const char *src, size_t n) {
    size_t length = 0;
    char *input = decode_input(src, n, "utf-8", &length);
    if (!input) return NULL;
    struct html_parser *p = parser_create(d, d && d->live && !d->inert);
    if (!p) { free(input); return NULL; }
    p->fragment = true;
    if (!add_input(p, input, length, false)) { html_finish(p); return NULL; }
    p->document = lxb_html_document_create();
    if (!p->document) { html_finish(p); return NULL; }
    const char *name = context && context->name ? context->name : "div";
    lxb_html_element_t *context_element = lxb_html_document_create_element(p->document, (const lxb_char_t *)name, strlen(name), NULL);
    if (!context_element) { html_finish(p); return NULL; }
    lxb_tag_id_t tag = lxb_dom_interface_node(context_element)->local_name;
    lxb_ns_id_t ns = context && context->namespace_id == NS_SVG ? LXB_NS_SVG :
                     context && context->namespace_id == NS_MATHML ? LXB_NS_MATH : LXB_NS_HTML;
    if (lxb_html_parse_fragment_chunk_begin(p->lex, p->document, tag, ns) != LXB_STATUS_OK) { html_finish(p); return NULL; }
    p->root = p->lex->root;
    install_callback(p);
    if (!fragment_context(p, context) || !html_bridge_init(p)) { html_finish(p); return NULL; }
    node_t *ignored;
    int result = html_resume(p, &ignored);
    node_t *root = result < 0 ? NULL : p->native_root;
    html_finish(p);
    return root;
}
