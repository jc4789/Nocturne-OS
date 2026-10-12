#pragma once
#include "webi.h"
/* Native presentation across a validated, policy-enabled frame chain. */
enum { WEB_PRESENT_OK,WEB_PRESENT_INACTIVE,WEB_PRESENT_ACTIVATION,WEB_PRESENT_UNSUPPORTED,WEB_PRESENT_MEDIA,WEB_PRESENT_BACKEND,WEB_PRESENT_POLICY };
bool web_presentation_policy_init(web_doc *,const char *,const web_doc *);
bool web_presentation_enabled(web_doc *,int,const struct web_host *);
bool web_presentation_matches(node_t *);
struct web_presentation_event {struct web_presentation_event *next;web_doc *document;node_t *node;int kind,width,height;uint64_t session;};
const struct web_presentation_event *web_presentation_event_peek(web_doc *);
void web_presentation_event_pop(web_doc *);
int web_presentation_request(web_doc *,node_t *,int,const struct web_host *);
bool web_presentation_exit(web_doc *,int);
node_t *web_presentation_element(web_doc *,int);
void web_presentation_sync(web_doc *);
void web_presentation_removed(web_doc *,node_t *);
void web_presentation_free(web_doc *);
void web_presentation_style(web_doc *,node_t *,style_t *);
void web_presentation_size(web_doc *,int *,int *);
