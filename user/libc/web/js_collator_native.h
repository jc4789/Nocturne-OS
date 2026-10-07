#pragma once
#include "quickjs.h"

/* Private CPU-only table decoder; no host locale, file, network, or OS ABI. */
void web_js_collator_init(JSContext *ctx, JSValue host);
