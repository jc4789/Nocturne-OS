/* The Encoding Standard for the browser: labels, every encoding's decoder and
   encoder, BOM sniffing, output encodings and the hooks the standard gives other
   standards. https://encoding.spec.whatwg.org/
   Lexbor's encoding module holds the indexes and the handlers; this is the one
   owner the HTML parser, TextDecoder, the URL parser and form submission decode
   and encode through. It needs only the C library and Lexbor (tests/ builds it
   on the host). Not an OS or process ABI. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lexbor/encoding/encoding.h>

/* An encoding: Lexbor's identifier. WEB_ENCODING_FAILURE is no encoding. */
typedef lxb_encoding_t web_encoding;
#define WEB_ENCODING_FAILURE LXB_ENCODING_DEFAULT
#define WEB_ENCODING_UTF8 LXB_ENCODING_UTF_8

/* get an encoding: label with ASCII whitespace trimmed, ASCII case-insensitive.
   Every name lowercased is a label, so a name from web_encoding_name comes back. */
web_encoding web_encoding_get(const char *label, size_t length);
/* The encoding's name ("UTF-8", "Shift_JIS"), "" for failure. TextDecoder's
   encoding attribute is it ASCII-lowercased. */
const char *web_encoding_name(web_encoding encoding);
/* get an output encoding: replacement and UTF-16BE/LE become UTF-8. */
web_encoding web_encoding_output(web_encoding encoding);
/* BOM sniff: UTF-8, UTF-16BE or UTF-16LE with its BOM's length, or failure. */
web_encoding web_encoding_bom_sniff(const uint8_t *bytes, size_t length, size_t *bom_length);

/* The length of bytes' longest prefix of whole, valid UTF-8 sequences: the
   UTF-8 decoder outputs exactly those bytes for it. */
size_t web_utf8_valid(const uint8_t *bytes, size_t length);

/* A growing malloc'd buffer the decoders append UTF-8 to and the encoders bytes
   to. Zero-initialize; the caller frees data. */
struct web_bytes { char *data; size_t length, capacity; };
bool web_bytes_put(struct web_bytes *out, const void *bytes, size_t length);

/* A decoder instance of one encoding in the "replacement" or "fatal" error
   mode, keeping its state between chunks of a stream. */
struct web_decoder {
    lxb_encoding_decode_t lexbor;
    web_encoding encoding;
    bool fatal;
    /* TextDecoder's serialize I/O queue: the first scalar value of UTF-8 and
       UTF-16BE/LE, if U+FEFF, is not output. Set after init by the caller. */
    bool drop_bom;
    bool started; /* a scalar value has been output (or dropped) */
    lxb_codepoint_t buffer[256];
};
/* A new instance of the encoding's decoder (not failure). */
void web_decoder_init(struct web_decoder *decoder, web_encoding encoding, bool fatal);
/* Processes bytes, then end-of-queue when last, appending each scalar value's
   UTF-8 to out. 1; 0 at an error in fatal mode (out has the scalar values
   before it); -1 when out of memory. After end-of-queue, an error or running
   out of memory the instance starts afresh (TextDecoder's new instance). */
int web_decoder_run(struct web_decoder *decoder, const uint8_t *bytes, size_t length,
                    bool last, struct web_bytes *out);

/* decode: BOM sniff, which overrides fallback and is skipped, then the
   encoding's decoder in replacement mode over all of bytes. *used (or NULL) is
   the encoding decoded with. NULL when out of memory; else a NUL-terminated
   malloc'd UTF-8 string of *length bytes. */
char *web_decode(const uint8_t *bytes, size_t length, web_encoding fallback,
                 web_encoding *used, size_t *out_length);

/* An encoder instance of an output encoding. */
struct web_encoder {
    lxb_encoding_encode_t lexbor;
    web_encoding encoding;
};
/* At most this many bytes come from one scalar value or end-of-queue. */
#define WEB_ENCODER_MAX 8
/* A new instance of the encoding's encoder; replacement and UTF-16BE/LE have
   none and get UTF-8's (as get an output encoding gives). */
void web_encoder_init(struct web_encoder *encoder, web_encoding encoding);
/* encode or fail, one scalar value: the bytes it gives (their count returned),
   and in *error the code point of an error or UINT32_MAX. ISO-2022-JP may give
   ESC ( B before an error; the caller's replacement goes after those bytes. */
size_t web_encoder_put(struct web_encoder *encoder, uint32_t scalar,
                       uint8_t out[WEB_ENCODER_MAX], uint32_t *error);
/* end-of-queue: the bytes the encoder still gives (ISO-2022-JP's return to ASCII). */
size_t web_encoder_finish(struct web_encoder *encoder, uint8_t out[WEB_ENCODER_MAX]);
/* encode in the "html" error mode (HTML forms): the UTF-8 of scalar values
   encoded, each error as &#N;, appended to out. false when out of memory. */
bool web_encode_html(web_encoding encoding, const char *utf8, size_t length, struct web_bytes *out);
