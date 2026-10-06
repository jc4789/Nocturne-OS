#pragma once
#include "quickjs.h"

/* Private bootstrap bindings; does not expose files or processes to scripts. */
void web_js_crypto_init(JSContext *ctx, JSValue host);
