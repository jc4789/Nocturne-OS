#ifndef NOCTURNE_HTML_LEXBOR_H
#define NOCTURNE_HTML_LEXBOR_H

#include "webi.h"
#include <lexbor/html/parser.h>
#include <lexbor/html/interfaces/document.h>

struct html_bridge;
struct html_input;

/* Lexbor is the tree builder's private state, not a second Web API DOM.
   At each script boundary native nodes retain their identity; mutations made
   by QuickJS are exported back before the tree builder resumes. */
struct html_parser {
    web_doc *d;
    lxb_html_parser_t *lex;
    lxb_html_document_t *document;
    lxb_dom_node_t *root;
    node_t *native_root;
    lxb_dom_node_t *pending_script;
    lxb_dom_node_t *eof_script;
    struct html_bridge *bridge;
    struct html_input *input, *inputs, *write_tail;
    size_t input_bytes;
    size_t input_spans;
    lxb_html_tokenizer_token_f original_callback;
    void *original_callback_context;
    /* Contextual fragments may run eligible scripts only after insertion;
       ordinary innerHTML fragments remain inert. Neither mode yields scripts
       during parsing, and scripting-disabled documents cannot enable it. */
    bool scripting, fragment, contextual_fragment, finished, failed, yielded, stream_open;
    bool import_changed; /* actual transaction published by the latest resume */
    bool allow_declarative_shadow;
    node_t *context_node;
    unsigned callback_depth; /* author reactions inside an unfinished Lexbor token */
    char *raw_bytes,*initial_base;size_t raw_length;
    char restart_encoding[32];
    struct web_html_policy_rule *initial_policy_tail;
};

bool html_bridge_init(struct html_parser *p);
bool html_bridge_import(struct html_parser *p);
bool html_bridge_export(struct html_parser *p);
node_t *html_bridge_native(struct html_parser *p, lxb_dom_node_t *node);
void html_bridge_destroy(struct html_parser *p);
/* Called only after a real HTML meta element was created by tree insertion.
 * Requests a restart; never destroys Lexbor inside its token callback. */
bool html_parser_meta_encoding(struct html_parser *,lxb_html_token_t *);

#endif
