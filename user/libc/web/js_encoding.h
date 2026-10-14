#pragma once
#include <stdbool.h>
#include "quickjs.h"
/* Private, synchronous UTF-8 conversion; the bootstrap owns input validation. */
void web_js_encoding_init(JSContext *ctx, JSValue host);
/* Independent realms must not interpret ContextOpaque as a document state. */
void web_js_encoding_init_isolated(JSContext *ctx, JSValue host);
/* Shared private Fetch data URL conversion; caller supplies its realm-safe
 * native cancellation hook. Never interpret an isolated realm's opaque as a
 * document state. */
JSValue web_js_data_url(JSContext *ctx, JSValueConst input,
                       int (*poll)(JSContext *), bool document);
