#pragma once
#include "webi.h"
struct web_html_policy_rule {
    struct web_html_policy_rule *next;
    char **names; size_t count;
    char *original_policy, *base_url, *report_to;
    char **report_uris; size_t report_uri_count;
    bool require_script, has_names, allow_duplicates, report_only;
    bool has_wasm_policy, wasm_allowed;
    bool from_meta;
};
struct web_html_policy { struct web_html_policy_rule *first, *last; };
bool html_policy_requires_script(const web_doc *document);
/* Source-list decision only. Callers must report every denied rule and block
 * only enforcing rules; report-only must not be hidden by this predicate. */
bool html_policy_wasm_allowed(const struct web_html_policy_rule *rule);
bool html_policy_init(web_doc *document,const char *headers,const web_doc *inherit);
/* Cross-Origin-Embedder-Policy of a response. An absent, repeated or
 * unparsable field is unsafe-none; report-only never enforces. */
enum html_embedder_policy { HTML_COEP_UNSAFE_NONE, HTML_COEP_REQUIRE_CORP, HTML_COEP_CREDENTIALLESS };
int html_policy_embedder(const char *headers);
/* Cross-Origin-Opener-Policy: same-origin, the opener policy that isolates
 * a top-level browsing context group. */
bool html_policy_opener_isolated(const char *headers);
/* Document creation: COEP is the response's own, or the creator's for local
 * documents; isolation requires a trustworthy isolated top-level document
 * and, in frames, a same-origin isolated parent with its own COEP. */
void html_policy_isolation(web_doc *document,const char *headers,const web_doc *inherit);
bool html_policy_meta(web_doc *document,const char *content,size_t length);
void html_policy_free(web_doc *document);
char *html_policy_headers(const web_doc *document);
char *html_policy_snapshot(const web_doc *document);
bool html_policy_snapshot_init(web_doc *document,const char *snapshot);
void html_policy_rewind(web_doc *document,struct web_html_policy_rule *tail);
/* Response-only embedding directives. Every ancestor must match every
 * enforced frame-ancestors policy; report-only/meta cannot grant or deny.
 * On success sandbox receives CSP restrictions to union before realm creation. */
bool html_policy_embedding_allowed(web_doc *parent,const char *headers,const char *url,uint32_t *sandbox);
