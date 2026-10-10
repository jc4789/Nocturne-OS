#pragma once
#include "webi.h"
struct web_html_policy_rule {
    struct web_html_policy_rule *next;
    char **names; size_t count;
    char *original_policy, *base_url, *report_to;
    char **report_uris; size_t report_uri_count;
    bool require_script, has_names, allow_duplicates, report_only;
    bool from_meta;
};
struct web_html_policy { struct web_html_policy_rule *first, *last; };
bool html_policy_init(web_doc *document,const char *headers,const web_doc *inherit);
bool html_policy_meta(web_doc *document,const char *content,size_t length);
void html_policy_free(web_doc *document);
char *html_policy_headers(const web_doc *document);
char *html_policy_snapshot(const web_doc *document);
bool html_policy_snapshot_init(web_doc *document,const char *snapshot);
void html_policy_rewind(web_doc *document,struct web_html_policy_rule *tail);
