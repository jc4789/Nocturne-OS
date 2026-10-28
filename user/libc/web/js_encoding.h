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
/* host.getEncoding(label): get an encoding's name ("UTF-8", "Shift_JIS"), or
   null for failure (web_encoding.c owns the labels). */
JSValue web_js_get_encoding(JSContext *ctx, int argc, JSValueConst *argv);
/* host.textDecoder(name, fatal, ignoreBOM): a decoder instance of any encoding
   but replacement (RangeError), for TextDecoder and TextDecoderStream.
   host.textDecoder(label, fatal, ignoreBOM, true): the Encoding Standard's
   decode as a stream (XHR's text response): label's encoding or UTF-8,
   replacement too, overridden by a BOM, which is not output. */
JSValue web_js_text_decoder(JSContext *ctx, int argc, JSValueConst *argv);
/* host.textDecode(decoder, buffer, offset, length, stream): the string of the
   view's bytes, then end-of-queue unless stream; TypeError at a fatal error.
   After end-of-queue or an error the instance starts afresh. prepare as for
   decodeUTF8. */
JSValue web_js_text_decode(JSContext *ctx, int argc, JSValueConst *argv,
                           void (*prepare)(JSContext *, size_t));
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
