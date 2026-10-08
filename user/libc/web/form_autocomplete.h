#pragma once
#include "web.h"

/* Native-only search history. No author script can enumerate this store.
   Settings are opt-in, and values are assigned only on a trusted popup choice. */
int web_autocomplete_options(web_doc *d, web_node *input, const char **labels, int capacity);
bool web_autocomplete_choose(web_doc *d, web_node *input, int index);
void web_autocomplete_record(web_doc *d, web_node *submitter, const char *destination);
void web_autocomplete_settings(void);
