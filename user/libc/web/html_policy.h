#pragma once
#include "webi.h"
/* Content Security Policy Level 3 (https://w3c.github.io/webappsec-csp/).
   A policy is its directive set (§2.2.1: names ASCII-lowercased, the first of
   a repeated name kept, each value split on ASCII whitespace), a disposition
   (report_only) and a source (from_meta). The other fields are the parts of
   that set other layers read often: Trusted Types, Wasm and reporting. */
struct web_csp_directive { char *name; char **value; size_t count; };
struct web_html_policy_rule {
    struct web_html_policy_rule *next;
    char **names; size_t count;
    char *original_policy, *base_url, *report_to;
    char **report_uris; size_t report_uri_count;
    bool require_script, has_names, allow_duplicates, report_only;
    bool has_wasm_policy, wasm_allowed;
    bool from_meta;
    struct web_csp_directive *directives; size_t directive_count;
};
/* A CSP list (§2.2): its policies and its self-origin, kept as the URL whose
   origin 'self' means (the response URL, or the creator's for a document or
   Worker that inherited its policy container). */
struct web_html_policy {
    struct web_html_policy_rule *first, *last;
    char *self_url;
    int status; /* §2.4.1: the status of the response the list came with; 0 for none */
};
bool html_policy_requires_script(const web_doc *document);
/* Whether the CSP list contains a header-delivered policy (HTML's nonce hiding). */
bool html_policy_header_delivered(const web_doc *document);
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
/* HTML's Content-Security-Policy http-equiv: enforced, without report-uri,
 * frame-ancestors and sandbox. */
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

/* ---- The checks other specifications call (CSP3 §4) ----
   Each walks every policy of the document's CSP list, hands every violation
   to report (both dispositions; the reporter runs §5.5 "report a violation"
   for it), and returns false ("Blocked") only when an enforced policy is
   violated. A list that cannot be matched for want of memory blocks. */
struct html_csp_violation {
    struct web_html_policy_rule *policy;
    const char *directive; /* the effective directive */
    const char *resource;  /* the request's URL, or "inline", "eval" */
    const char *sample;    /* UTF-8, at most 40 UTF-16 code units */
    size_t sample_length;
};
typedef void (*html_csp_reporter)(void *opaque,const struct html_csp_violation *violation);
/* A request as CSP reads it (Fetch's request). url is its current URL;
   original_url its URL (the violation's resource, never a redirect target). */
struct html_csp_request {
    const char *url, *original_url;
    int destination;          /* enum webnet_destination */
    const char *nonce;        /* cryptographic nonce metadata */
    const char *integrity;    /* integrity metadata */
    bool parser_inserted;     /* parser metadata */
    bool prefetch;            /* initiator "prefetch" or "prerender" */
    unsigned redirect_count;
};
/* §4.1.1 and §4.1.2 together: report-only policies report, enforced ones
   report and block. After a redirect, call it again with the response's URL
   and the redirect count (§4.1.3's post-request checks give the same answer
   for a response no service worker substituted). */
bool html_policy_request_allowed(web_doc *document,const struct html_csp_request *request,
                                 html_csp_reporter report,void *opaque);
/* §4.2.3, for the inline check types. */
enum html_csp_inline { HTML_CSP_SCRIPT, HTML_CSP_SCRIPT_ATTRIBUTE, HTML_CSP_STYLE,
                       HTML_CSP_STYLE_ATTRIBUTE, HTML_CSP_NAVIGATION };
struct html_csp_element {
    const char *nonce;  /* the element's [[CryptographicNonce]] */
    bool nonceable;     /* §6.7.3.1 */
    bool parser_inserted;
};
/* source: UTF-8 (an unpaired surrogate in WTF-8 counts as U+FFFD, as the
   JavaScript string conversion makes it). element is NULL for "navigation". */
bool html_policy_inline_allowed(web_doc *document,enum html_csp_inline type,const struct html_csp_element *element,
                                const char *source,size_t length,html_csp_reporter report,void *opaque);
/* §6.7.3.1 for an element of document: a nonce attribute, and on a script no
   "<script", "<style" or "<link" in an attribute's name or value. */
bool html_policy_nonceable(const node_t *element);
/* §4.4.1 after the Trusted Types step: string compilation (eval, Function,
   a timer's string). source is the compiled text. */
bool html_policy_eval_allowed(web_doc *document,const char *source,size_t length,
                              html_csp_reporter report,void *opaque);
/* §6.3.1.1 Is base allowed for document? */
bool html_policy_base_allowed(web_doc *document,const char *url,html_csp_reporter report,void *opaque);
/* §4.2.4 Should navigation request of type be blocked? (form-action, and a
   javascript: URL's inline check, whose source is the URL itself). */
bool html_policy_navigation_allowed(web_doc *document,const struct html_csp_request *request,bool form_submission,
                                    html_csp_reporter report,void *opaque);
/* §4.2.6 for a WorkerGlobalScope: an enforced sandbox directive without
   allow-scripts and allow-same-origin blocks the worker. */
bool html_policy_worker_allowed(const web_doc *document);
/* Upgrade Insecure Requests: an enforced upgrade-insecure-requests. */
bool html_policy_upgrade_insecure(const web_doc *document);
/* §5.4 Strip URL for use in reports, malloc'd (NULL for no memory). */
char *html_policy_report_url(web_doc *document,const char *url);
