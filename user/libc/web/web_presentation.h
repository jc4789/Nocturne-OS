#pragma once
#include "webi.h"
/* Finite top-level native presentation. Errors are not capability stubs. */
enum { WEB_PRESENT_OK,WEB_PRESENT_INACTIVE,WEB_PRESENT_ACTIVATION,WEB_PRESENT_UNSUPPORTED,WEB_PRESENT_MEDIA,WEB_PRESENT_BACKEND };
int web_presentation_request(web_doc *,node_t *,int,const struct web_host *);
bool web_presentation_exit(web_doc *,int);
node_t *web_presentation_element(web_doc *,int);
void web_presentation_sync(web_doc *);
void web_presentation_removed(web_doc *,node_t *);
void web_presentation_free(web_doc *);
void web_presentation_style(web_doc *,node_t *,style_t *);
void web_presentation_size(web_doc *,int *,int *);
