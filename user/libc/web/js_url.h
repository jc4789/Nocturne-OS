#pragma once
#include "quickjs.h"

/* The native half of js_url.js on host: urlParse(input, base) and
   urlSet(href, part, value) return a URL's record, its serialization followed
   by web_url.h's marks (null when input is not a URL); urlFormParse(string)
   gives the urlencoded pairs flat, [name, value, ...]; urlFormSerialize(pairs)
   reverses it. Strings must hold scalar values (js_url.js's usv). The URL
   parser belongs to the realm. */
void web_js_url_init(JSContext *ctx, JSValue host);
