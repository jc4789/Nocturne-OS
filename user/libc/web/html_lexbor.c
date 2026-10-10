/* Lexbor owns only the suspended HTML tree-builder state. Native node_t remains
   the DOM exposed to QuickJS, resources, selectors, style and layout. */
#include <stdio.h>
#include <ctype.h>
#include "html_lexbor.h"
#include "html_encoding.h"
#include "html_policy.h"
#include <lexbor/html/tree/insertion_mode.h>
#include <lexbor/html/encoding.h>
#include <lexbor/html/interfaces/form_element.h>
#include <lexbor/html/interfaces/script_element.h>
#include <lexbor/dom/interfaces/element.h>

#define HTML_INPUT_LIMIT ((size_t)UINT32_MAX)

/* Incoming token spans can point into previously supplied chunks. Never move
   or free a span while its tokenizer is alive, including document.write input. */
struct html_input {
    struct html_input *next, *owned_next;
    char *data;
    size_t length, offset, capacity;
};

/* DOMString input is already Unicode (UTF-8/WTF-8 from QuickJS). Preserve
   FEFF, NUL and lone surrogates; tokenizer states own NUL replacement and
   cross-chunk CR/LF normalization. Transport decoding is a separate API. */
static char *string_input(const char *src, size_t n, size_t *length) {
    if ((!src && n) || n > HTML_INPUT_LIMIT || n == SIZE_MAX) return NULL;
    char *data=malloc(n+1);
    if (!data) return NULL;
    if (n) memcpy(data,src,n);
    data[n]=0; *length=n; return data;
}

static bool add_input(struct html_parser *p, char *data, size_t length, bool writing) {
    if (p->input_bytes > HTML_INPUT_LIMIT || length > HTML_INPUT_LIMIT - p->input_bytes ||
        length == SIZE_MAX || p->input_spans == SIZE_MAX) {
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
static void parser_release_content(struct html_parser *);

bool html_parser_meta_encoding(struct html_parser *p,lxb_html_token_t *token){
    if(!p||p->fragment||p->d->encoding_certain||!p->raw_bytes)return true;
    const char *charset=NULL,*equiv=NULL,*content=NULL;size_t charset_n=0,equiv_n=0,content_n=0;
    for(lxb_html_token_attr_t *a=token->attr_first;a;a=a->next){
        size_t n=0;const lxb_char_t *name=lxb_html_token_attr_name(a,&n);
        if(!charset&&strn_ieq((const char *)name,"charset",n)){charset=(const char *)(a->value?a->value:(const lxb_char_t *)"");charset_n=a->value_size;}
        if(!equiv&&strn_ieq((const char *)name,"http-equiv",n)){equiv=(const char *)(a->value?a->value:(const lxb_char_t *)"");equiv_n=a->value_size;}
        if(!content&&strn_ieq((const char *)name,"content",n)){content=(const char *)(a->value?a->value:(const lxb_char_t *)"");content_n=a->value_size;}
    }
    if(!charset&&equiv&&strn_ieq(equiv,"content-type",equiv_n)&&content){
        const lxb_char_t *end=NULL,*name=lxb_html_encoding_content((const lxb_char_t *)content,(const lxb_char_t *)content+content_n,&end);
        if(name){charset=(const char *)name;charset_n=(size_t)(end-name);}
    }
    char canonical[32];if(!charset||!html_meta_label(charset,charset_n,canonical,sizeof canonical))return true;
    if(!strncasecmp(p->d->encoding,"UTF-16",6)||!strcasecmp(p->d->encoding,canonical)){p->d->encoding_certain=true;return true;}
    snprintf(p->restart_encoding,sizeof p->restart_encoding,"%s",canonical);return true;
}

/* Always forward to Lexbor's tree callback first. A successful callback must
   return its token; NULL/non-OK is a fatal parser error, not a resumable pause. */
static lxb_html_token_t *token_done(lxb_html_tokenizer_t *tkz, lxb_html_token_t *token, void *ctx) {
    struct html_parser *p = ctx;
    lxb_html_tree_t *tree = p->lex->tree;
    lxb_dom_node_t *current = lxb_html_tree_current_node(tree);
    if (!p->fragment && token->tag_id==LXB_TAG_META && !(token->type&LXB_HTML_TOKEN_TYPE_CLOSE) &&
        tree->mode==lxb_html_tree_insertion_mode_in_head) {
        const char *equiv=NULL,*content=NULL;size_t equiv_length=0,content_length=0;
        for(lxb_html_token_attr_t *a=token->attr_first;a;a=a->next){
            size_t name_length=0;const lxb_char_t *name=lxb_html_token_attr_name(a,&name_length);
            if(!equiv && strn_ieq((const char *)name,"http-equiv",name_length)){equiv=(const char *)a->value;equiv_length=a->value_size;}
            if(!content && strn_ieq((const char *)name,"content",name_length)){content=(const char *)a->value;content_length=a->value_size;}
        }
        if(equiv && strn_ieq(equiv,"content-security-policy",equiv_length) && content &&
            !html_policy_meta(p->d,content,content_length)){p->failed=true;return NULL;}
    }
    bool script_text = tree->mode == lxb_html_tree_insertion_mode_text && current &&
                       current->ns == LXB_NS_HTML && current->local_name == LXB_TAG_SCRIPT;
    bool svg_script=current && current->ns==LXB_NS_SVG && current->local_name==LXB_TAG_SCRIPT;
    bool closed = (script_text || svg_script) && token->tag_id == LXB_TAG_SCRIPT && (token->type & LXB_HTML_TOKEN_TYPE_CLOSE);
    bool execute = closed && p->scripting && !p->fragment && !p->d->inert &&
                   !lxb_html_tree_parsing_inert_template_contents(tree);
    if (script_text && token->tag_id == LXB_TAG__END_OF_FILE) p->eof_script = current;
    lxb_html_token_t *result = p->original_callback(tkz, token, p->original_callback_context);
    if(result && closed && p->fragment && p->scripting && !lxb_html_tree_parsing_inert_template_contents(tree)){
        if(!html_bridge_import(p)){p->failed=true;return NULL;}
        node_t *node=html_bridge_native(p,current);
        if(!node||!web_js_script_parser_capture(p->d,node)){p->failed=true;return NULL;}
    }
    if (result && execute) p->pending_script = current;
    if (result && p->scripting && !p->fragment && !p->d->inert &&
        token->tag_id==LXB_TAG_SCRIPT && !(token->type&LXB_HTML_TOKEN_TYPE_CLOSE) &&
        (token->type&LXB_HTML_TOKEN_TYPE_CLOSE_SELF) && current && current->last_child &&
        current->last_child->ns==LXB_NS_SVG && current->last_child->local_name==LXB_TAG_SCRIPT &&
        !lxb_html_tree_parsing_inert_template_contents(tree))
        p->pending_script=current->last_child;
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
    char *input = html_decode_bytes(d, src, n, charset, &length);
    if (!input) return NULL;
    struct html_parser *p = parser_create(d, scripting);
    if (!p) { free(input); return NULL; }
    p->allow_declarative_shadow = true;
    p->initial_policy_tail=d->html_policy?d->html_policy->last:NULL;
    if(!d->encoding_certain){
        p->raw_bytes=malloc(n+1);p->initial_base=strdup(d->base);
        if(!p->raw_bytes||!p->initial_base){free(input);html_finish(p);return NULL;}
        if(n)memcpy(p->raw_bytes,src,n);p->raw_bytes[n]=0;p->raw_length=n;
    }
    if (!add_input(p, input, length, false)) { html_finish(p); return NULL; }
    p->document = lxb_html_parse_chunk_begin(p->lex);
    if (!p->document) { html_finish(p); return NULL; }
    p->root = lxb_dom_interface_node(p->document);
    install_callback(p);
    if (!html_bridge_init(p) || !html_bridge_import(p)) { html_finish(p); return NULL; }
    d->root = p->native_root;
    d->document_mode = d->html_srcdoc ? 0 : 1;
    d->quirks = !d->html_srcdoc;
    p->document->dom_document.compat_mode = d->document_mode;
    if (d->html_srcdoc) p->lex->tree->mode = lxb_html_tree_insertion_mode_before_html;
    return p;
}

struct html_parser *html_open(web_doc *d) {
    struct html_parser *p=parser_create(d,true);
    if(!p)return NULL;
    p->allow_declarative_shadow=true;
    snprintf(d->encoding,sizeof d->encoding,"UTF-8");
    d->encoding_certain=true;
    p->stream_open=true;p->native_root=d->root;
    p->document=lxb_html_parse_chunk_begin(p->lex);
    if(!p->document){html_finish(p);return NULL;}
    p->root=lxb_dom_interface_node(p->document);install_callback(p);
    if(!html_bridge_init(p) || !html_bridge_import(p)){html_finish(p);return NULL;}
    return p;
}
void html_close(struct html_parser *p){if(p)p->stream_open=false;}

bool html_pending_input(const struct html_parser *p) {
    if (!p || p->failed || p->finished) return false;
    for (const struct html_input *in = p->input; in; in = in->next)
        if (in->offset < in->length) return true;
    return false;
}

bool html_import_changed(const struct html_parser *p) { return p && p->import_changed; }
void html_declarative_shadow_set(struct html_parser *p,bool enabled){if(p)p->allow_declarative_shadow=enabled;}

static bool parser_restart_encoding(struct html_parser *p){
    web_doc *d=p->d;size_t length=0;
    /* The old pass is still tentative here. Native script queues must retire
     * before the new definite decoder publishes the replacement confidence. */
    if(!web_js_encoding_restart(d))return false;
    char *input=html_decode_bytes(d,p->raw_bytes,p->raw_length,p->restart_encoding,&length);if(!input)return false;
    struct html_parser *fresh=parser_create(d,p->scripting);if(!fresh){free(input);return false;}
    if(!add_input(fresh,input,length,false)){html_finish(fresh);return false;}
    fresh->document=lxb_html_parse_chunk_begin(fresh->lex);
    if(!fresh->document){html_finish(fresh);return false;}
    fresh->root=lxb_dom_interface_node(fresh->document);fresh->native_root=p->native_root;
    fresh->allow_declarative_shadow=p->allow_declarative_shadow;
    html_policy_rewind(d,p->initial_policy_tail);
    while(p->native_root->first)doc_node_remove(d,p->native_root->first);
    d->html=d->head=d->body=NULL;d->title=NULL;d->base_seen=d->scan_title_seen=false;
    snprintf(d->base,sizeof d->base,"%s",p->initial_base?p->initial_base:"");
    d->document_mode=d->html_srcdoc?0:1;d->quirks=!d->html_srcdoc;
    fresh->document->dom_document.compat_mode=d->document_mode;
    if(d->html_srcdoc)fresh->lex->tree->mode=lxb_html_tree_insertion_mode_before_html;
    fresh->raw_bytes=p->raw_bytes;fresh->raw_length=p->raw_length;fresh->initial_base=p->initial_base;
    fresh->initial_policy_tail=p->initial_policy_tail;
    parser_release_content(p);*p=*fresh;free(fresh);
    install_callback(p);
    return html_bridge_init(p)&&html_bridge_import(p);
}

static int resume_until(struct html_parser *p, node_t **script, struct html_input *boundary, bool bounded) {
    if (script) *script = NULL;
    if (p) p->import_changed = false;
    if (!p || p->failed) return -1;
    if (p->finished) return 0;
    /* A document.open stream can remain suspended for arbitrarily many ticks.
       No token can change while it has neither input nor EOF. In particular,
       importing the unchanged Lexbor tree would republish every native link.
       Keep yielded set so native script mutations are exported before the next
       actual write or close resumes the tree builder. */
    if (p->stream_open && !html_pending_input(p)) { p->yielded = true; return 2; }
    doc_dom_budget(p->d);
    if (p->yielded && !html_bridge_export(p)) { p->failed = true; return -1; }
    p->yielded = false; p->pending_script = NULL; p->write_tail = NULL;
    while (p->input && !p->pending_script && (!bounded || p->input != boundary)) {
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
        if (status != LXB_STATUS_OK) {
            p->failed = true; return -1;
        }
        in->offset += size;
        if(p->restart_encoding[0]){
            if(!parser_restart_encoding(p)){p->failed=true;return -1;}
            /* Old tokenizer pointers and old insertion-point spans are gone.
             * No author has run, so this cannot be a bounded document.write. */
            p->pending_script=NULL;continue;
        }
    }
    if (!p->pending_script && !p->stream_open && !bounded) {
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
        node->script_parse_eligible = false;
    }
    if (p->pending_script) {
        node_t *node = html_bridge_native(p, p->pending_script);
        if (!node) { p->failed = true; return -1; }
        if(!web_js_script_parser_capture(p->d,node)){p->failed=true;return -1;}
        p->yielded = true;
        if (script) *script = node;
    }
    if((p->stream_open || bounded) && !p->pending_script){p->yielded=true;return 2;}
    return p->yielded ? 1 : 0;
}
int html_resume(struct html_parser *p, node_t **script) { return resume_until(p,script,NULL,false); }
void *html_write_boundary(struct html_parser *p) {
    if (!p) return NULL;
    /* Never append a nested write to a span already seen by the tokenizer. */
    p->write_tail=NULL; return p->input;
}
int html_resume_written(struct html_parser *p, node_t **script, void *boundary) {
    return resume_until(p,script,boundary,true);
}

bool html_write(struct html_parser *p, const char *text, size_t n) {
    if (!p || p->finished || p->failed || (!p->yielded && !p->stream_open && !p->callback_depth) || !p->scripting || (!text && n) ||
        p->input_bytes > HTML_INPUT_LIMIT || n > HTML_INPUT_LIMIT - p->input_bytes) return false;
    if (!n) return true;
    /* Consecutive writes during one suspended script have not reached Lexbor
       yet, so they may share a growing span. Previously fed spans stay intact.
       This also bounds bookkeeping for many one-byte/empty writes. */
    if (p->write_tail) {
        struct html_input *in = p->write_tail;
        if (in->length == SIZE_MAX || n > SIZE_MAX - in->length - 1) return false;
        size_t length = in->length + n;
        if (length + 1 > in->capacity) {
            size_t capacity = in->capacity > SIZE_MAX / 2 ? SIZE_MAX : in->capacity * 2;
            if (capacity < length + 1) capacity = length + 1;
            size_t maximum = HTML_INPUT_LIMIT < SIZE_MAX ? HTML_INPUT_LIMIT + 1 : SIZE_MAX;
            if (capacity > maximum) capacity = maximum;
            char *data = realloc(in->data, capacity);
            if (!data) return false;
            in->data = data; in->capacity = capacity;
        }
        memcpy(in->data + in->length, text, n);
        in->length = length; in->data[length] = 0;
        p->input_bytes += n;
        return true;
    }
    if (n == SIZE_MAX) return false;
    char *data = malloc(n + 1);
    if (!data) return false;
    if (n) memcpy(data, text, n);
    data[n] = 0;
    return add_input(p, data, n, true);
}

static void parser_release_content(struct html_parser *p) {
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
    p->lex=NULL;p->document=NULL;p->inputs=p->input=p->write_tail=NULL;
}
void html_finish(struct html_parser *p){
    if(!p)return;
    parser_release_content(p);free(p->raw_bytes);free(p->initial_base);free(p);
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
struct html_parser *html_begin_string(web_doc *d, const char *src, size_t n, bool scripting, bool allow_shadow) {
    size_t length=0; char *input=string_input(src,n,&length);
    if (!input) return NULL;
    struct html_parser *p=parser_create(d,scripting);
    if (!p) { free(input); return NULL; }
    p->allow_declarative_shadow=allow_shadow;
    snprintf(d->encoding,sizeof d->encoding,"UTF-8");
    d->encoding_certain=true;
    if (!add_input(p,input,length,false)) { html_finish(p); return NULL; }
    p->document=lxb_html_parse_chunk_begin(p->lex);
    if (!p->document) { html_finish(p); return NULL; }
    p->root=lxb_dom_interface_node(p->document); install_callback(p);
    if (!html_bridge_init(p) || !html_bridge_import(p)) { html_finish(p); return NULL; }
    d->root=p->native_root;
    if(d->html_srcdoc){
        p->document->dom_document.compat_mode=0;
        p->lex->tree->mode=lxb_html_tree_insertion_mode_before_html;
        d->document_mode=0;d->quirks=false;
    }
    return p;
}
node_t *html_parse_string(web_doc *d, const char *src, size_t n, bool allow_shadow) {
    struct html_parser *p=html_begin_string(d,src,n,false,allow_shadow);
    if(!p)return NULL;
    node_t *ignored; int result=html_resume(p,&ignored);
    node_t *root=result<0?NULL:p->native_root;
    html_finish(p); return root;
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

node_t *html_fragment_ex(web_doc *d, node_t *context, const char *src, size_t n, bool allow_shadow, bool contextual) {
    size_t length = 0;
    /* Fragment markup is a DOMString, not transport bytes. U+FEFF at its
       beginning is text, while CR preprocessing and input bounds still apply. */
    char *input = string_input(src, n, &length);
    if (!input) return NULL;
    struct html_parser *p = parser_create(d, d && d->live && !d->inert);
    if (!p) { free(input); return NULL; }
    p->fragment = true;
    p->allow_declarative_shadow = allow_shadow;
    p->context_node = context;
    p->contextual_fragment = contextual && p->scripting;
    if (!add_input(p, input, length, false)) { html_finish(p); return NULL; }
    p->document = lxb_html_document_create();
    if (!p->document) { html_finish(p); return NULL; }
    p->document->dom_document.compat_mode=d->document_mode;
    p->document->dom_document.scripting=p->scripting;
    node_t *element_context=context && context->shadow_host?context->shadow_host:context;
    const char *name = element_context && element_context->name ? element_context->name : "div";
    lxb_html_element_t *context_element = lxb_html_document_create_element(p->document, (const lxb_char_t *)name, strlen(name), NULL);
    if (!context_element) { html_finish(p); return NULL; }
    lxb_tag_id_t tag = lxb_dom_interface_node(context_element)->local_name;
    lxb_ns_id_t ns = element_context && element_context->namespace_id == NS_SVG ? LXB_NS_SVG :
                     element_context && element_context->namespace_id == NS_MATHML ? LXB_NS_MATH : LXB_NS_HTML;
    if (lxb_html_parse_fragment_chunk_begin(p->lex, p->document, tag, ns) != LXB_STATUS_OK) { html_finish(p); return NULL; }
    p->root = p->lex->root;
    install_callback(p);
    if (!fragment_context(p, element_context) || !html_bridge_init(p)) { html_finish(p); return NULL; }
    node_t *ignored;
    int result = html_resume(p, &ignored);
    node_t *root = result < 0 ? NULL : p->native_root;
    html_finish(p);
    return root;
}

node_t *html_fragment(web_doc *d, node_t *context, const char *src, size_t n) {
    return html_fragment_ex(d, context, src, n, false, false);
}

node_t *html_contextual_fragment(web_doc *d, node_t *context, const char *src, size_t n) {
    /* Do not re-enable scripts after parsing: the EOF handling in html_resume
       must keep an unclosed script's already-started state intact. */
    return html_fragment_ex(d, context, src, n, false, true);
}
