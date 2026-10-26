#pragma once
#include <stdbool.h>
#include "quickjs.h"
/* Private, synchronous UTF-8 conversion; the bootstrap owns input validation. */
void web_js_encoding_init(JSContext *ctx, JSValue host);
/* Independent realms must not interpret ContextOpaque as a document state. */
void web_js_encoding_init_isolated(JSContext *ctx, JSValue host);
/* host.decodeUTF8(buffer, offset, length, fatal, ignoreBOM); prepare (or NULL)
   charges the realm before the result string is allocated. */
JSValue web_js_decode_utf8(JSContext *ctx, int argc, JSValueConst *argv,
                           void (*prepare)(JSContext *, size_t));
/* A string from UTF-8 as the Encoding Standard decodes it without a BOM check:
   each invalid sequence becomes U+FFFD. */
JSValue web_js_utf8_text(JSContext *ctx, const uint8_t *bytes, size_t length);
/* QuickJS's UTF-8 of a string keeps a lone surrogate in its three-byte form;
   this makes the UTF-8 of the scalar values (a USVString's), in place. */
void web_utf8_replace_lone_surrogates(uint8_t *bytes, size_t length);
/* host.encodeUTF8(string): an ArrayBuffer of the string's scalar values in
   UTF-8, each lone surrogate as U+FFFD; prepare as for decoding. */
JSValue web_js_encode_utf8(JSContext *ctx, int argc, JSValueConst *argv,
                           void (*prepare)(JSContext *, size_t));
/* host.encodeUTF8Into(string, buffer, offset, length): TextEncoder.encodeInto's
   {read, written} after writing the whole scalar values that fit. */
JSValue web_js_encode_utf8_into(JSContext *ctx, int argc, JSValueConst *argv);
/* host.concatUTF8(parts): one ArrayBuffer of an array's parts in order, each a
   string (encoded as encodeUTF8 does) or a byte view; prepare as for decoding. */
JSValue web_js_concat_utf8(JSContext *ctx, int argc, JSValueConst *argv,
                           void (*prepare)(JSContext *, size_t));
/* host.btoa(string) / host.atob(string): HTML's base64 of a binary string and
   its forgiving-base64 decode, or null where HTML throws InvalidCharacterError
   (a code unit above U+00FF; invalid base64); prepare as for decoding. */
JSValue web_js_btoa(JSContext *ctx, int argc, JSValueConst *argv,
                    void (*prepare)(JSContext *, size_t));
JSValue web_js_atob(JSContext *ctx, int argc, JSValueConst *argv,
                    void (*prepare)(JSContext *, size_t));
/* Shared private Fetch data URL conversion; caller supplies its realm-safe
 * native cancellation hook. Never interpret an isolated realm's opaque as a
 * document state. */
JSValue web_js_data_url(JSContext *ctx, JSValueConst input,
                       int (*poll)(JSContext *), bool document);
