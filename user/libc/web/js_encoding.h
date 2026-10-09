#pragma once
#include "quickjs.h"
/* Private, synchronous UTF-8 conversion; the bootstrap owns input validation. */
void web_js_encoding_init(JSContext *ctx, JSValue host);
/* Independent realms must not interpret ContextOpaque as a document state. */
void web_js_encoding_init_isolated(JSContext *ctx, JSValue host);
