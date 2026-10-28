/* The Encoding Standard over Lexbor's encoding module (web_encoding.h).
   https://encoding.spec.whatwg.org/
   Lexbor's per-encoding handlers and indexes are the standard's; local fixes to
   them are recorded in third_party/lexbor/NOCTURNE_PATCHES.json and checked
   against the standard's indexes by tests/check_encoding.py. */
#include "web_encoding.h"
#include <stdlib.h>
#include <string.h>

web_encoding web_encoding_get(const char *label, size_t length) {
    const lxb_encoding_data_t *data = label && length ?
        lxb_encoding_data_by_pre_name((const lxb_char_t *)label, length) : NULL;
    return data ? data->encoding : WEB_ENCODING_FAILURE;
}

const char *web_encoding_name(web_encoding encoding) {
    const lxb_encoding_data_t *data = encoding > LXB_ENCODING_UNDEFINED &&
        encoding < LXB_ENCODING_LAST_ENTRY ? lxb_encoding_data(encoding) : NULL;
    return data ? (const char *)data->name : "";
}

web_encoding web_encoding_output(web_encoding encoding) {
    return encoding == LXB_ENCODING_REPLACEMENT || encoding == LXB_ENCODING_UTF_16BE ||
           encoding == LXB_ENCODING_UTF_16LE ? WEB_ENCODING_UTF8 : encoding;
}

web_encoding web_encoding_bom_sniff(const uint8_t *bytes, size_t length, size_t *bom_length) {
    web_encoding encoding = bytes ? lxb_encoding_bom_sniff(bytes, length) : WEB_ENCODING_FAILURE;
    if (bom_length) *bom_length = encoding == WEB_ENCODING_UTF8 ? 3 : encoding ? 2 : 0;
    return encoding;
}

bool web_bytes_put(struct web_bytes *out, const void *bytes, size_t length) {
    if (!length) return true;
    if (length > SIZE_MAX - 1 - out->length) return false;
    size_t wanted = out->length + length + 1;
    if (wanted > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 64;
        while (capacity < wanted) capacity = capacity > SIZE_MAX / 2 ? wanted : capacity * 2;
        char *grown = realloc(out->data, capacity);
        if (!grown) return false;
        out->data = grown;
        out->capacity = capacity;
    }
    memcpy(out->data + out->length, bytes, length);
    out->length += length;
    out->data[out->length] = 0;
    return true;
}

/* What an error outputs in replacement mode. (Lexbor's LXB_ENCODING_REPLACEMENT_BUFFER
   is a compound literal, which lives only in the block that names it.) */
static const lxb_codepoint_t replacement = LXB_ENCODING_REPLACEMENT_CODEPOINT;

/* The instance's handler state afresh; the encoding and modes stay. */
static void decoder_start(struct web_decoder *d) {
    d->started = false;
    lxb_encoding_decode_init(&d->lexbor, lxb_encoding_data(d->encoding), d->buffer,
                             sizeof d->buffer / sizeof *d->buffer);
    if (!d->fatal) lxb_encoding_decode_replace_set(&d->lexbor, &replacement, 1);
}

void web_decoder_init(struct web_decoder *d, web_encoding encoding, bool fatal) {
    d->encoding = encoding;
    d->fatal = fatal;
    d->drop_bom = false;
    decoder_start(d);
}

/* The decoder's buffered scalar values as UTF-8. */
static bool flush(struct web_decoder *d, struct web_bytes *out) {
    size_t used = lxb_encoding_decode_buf_used(&d->lexbor), i = 0;
    lxb_encoding_decode_buf_used_set(&d->lexbor, 0);
    if (!used) return true;
    if (!d->started) {
        d->started = true;
        if (d->drop_bom && d->buffer[0] == 0xfeff) i = 1;
    }
    char text[sizeof d->buffer], *p = text;
    for (; i < used; i++) {
        lxb_codepoint_t cp = d->buffer[i];
        if (cp < 0x80) *p++ = (char)cp;
        else if (cp < 0x800) { *p++ = (char)(0xc0 | cp >> 6); *p++ = (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            *p++ = (char)(0xe0 | cp >> 12); *p++ = (char)(0x80 | (cp >> 6 & 63)); *p++ = (char)(0x80 | (cp & 63));
        } else {
            *p++ = (char)(0xf0 | cp >> 18); *p++ = (char)(0x80 | (cp >> 12 & 63));
            *p++ = (char)(0x80 | (cp >> 6 & 63)); *p++ = (char)(0x80 | (cp & 63));
        }
    }
    return web_bytes_put(out, text, (size_t)(p - text));
}

size_t web_utf8_valid(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t count; unsigned low = 0x80, high = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) count = 1;
        else if (c >= 0xe0 && c <= 0xef) { count = 2; if (c == 0xe0) low = 0xa0; if (c == 0xed) high = 0x9f; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; if (c == 0xf0) low = 0x90; if (c == 0xf4) high = 0x8f; }
        else break;
        if (count > n - i - 1 || s[i + 1] < low || s[i + 1] > high) break;
        size_t k = 2;
        while (k <= count && (s[i + k] & 0xc0) == 0x80) k++;
        if (k <= count) break;
        i += count + 1;
    }
    return i;
}

int web_decoder_run(struct web_decoder *d, const uint8_t *bytes, size_t length, bool last,
                    struct web_bytes *out) {
    if (!bytes) length = 0;
    /* UTF-8 between sequences: a valid prefix is its own output. */
    if (d->encoding == WEB_ENCODING_UTF8 && !d->lexbor.u.utf_8.need && !d->lexbor.have_error) {
        size_t valid = web_utf8_valid(bytes, length), skip = 0;
        if (valid && !d->started) {
            d->started = true;
            if (d->drop_bom && valid >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) skip = 3;
        }
        if (!web_bytes_put(out, bytes + skip, valid - skip)) return -1;
        bytes += valid; length -= valid;
    }
    const lxb_char_t *p = bytes, *end = bytes + length;
    while (p < end) {
        const lxb_char_t *before = p;
        lxb_status_t status = d->lexbor.encoding_data->decode(&d->lexbor, &p, end);
        bool output = lxb_encoding_decode_buf_used(&d->lexbor) != 0;
        if (!flush(d, out)) return -1;
        /* An error in fatal mode; anything else that neither reads nor outputs
           would not end. */
        if (status == LXB_STATUS_ERROR || (p == before && !output)) return 0;
    }
    if (last) {
        lxb_status_t status = lxb_encoding_decode_finish(&d->lexbor);
        if (!flush(d, out)) return -1;
        if (status == LXB_STATUS_ERROR) return 0;
        decoder_start(d);
    }
    return 1;
}

char *web_decode(const uint8_t *bytes, size_t length, web_encoding fallback,
                 web_encoding *used, size_t *out_length) {
    size_t bom = 0;
    web_encoding encoding = web_encoding_bom_sniff(bytes, length, &bom);
    if (!encoding) encoding = fallback ? fallback : WEB_ENCODING_UTF8;
    if (used) *used = encoding;
    struct web_decoder *d = malloc(sizeof *d);
    struct web_bytes out = {0};
    if (!d || !web_bytes_put(&out, "", 1)) { free(d); free(out.data); return NULL; }
    out.length = 0;
    web_decoder_init(d, encoding, false);
    int result = web_decoder_run(d, bytes ? bytes + bom : NULL, length - bom, true, &out);
    free(d);
    if (result != 1) { free(out.data); return NULL; }
    *out_length = out.length;
    return out.data;
}

void web_encoder_init(struct web_encoder *e, web_encoding encoding) {
    e->encoding = web_encoding_output(encoding);
    if (!e->encoding) e->encoding = WEB_ENCODING_UTF8;
    lxb_encoding_encode_init_single(&e->lexbor, lxb_encoding_data(e->encoding));
}

size_t web_encoder_put(struct web_encoder *e, uint32_t scalar, uint8_t out[WEB_ENCODER_MAX],
                       uint32_t *error) {
    lxb_char_t *p = out;
    lxb_codepoint_t failed = UINT32_MAX;
    int8_t n = lxb_encoding_encode_or_fail_single(&e->lexbor, &p, out + WEB_ENCODER_MAX, scalar, &failed);
    *error = n == LXB_ENCODING_ENCODE_ERROR ? failed : UINT32_MAX;
    return (size_t)(p - out);
}

size_t web_encoder_finish(struct web_encoder *e, uint8_t out[WEB_ENCODER_MAX]) {
    lxb_char_t *p = out;
    lxb_encoding_encode_finish_single(&e->lexbor, &p, out + WEB_ENCODER_MAX);
    return (size_t)(p - out);
}

/* One scalar value from UTF-8 known to be of scalar values; *at moves past it. */
static uint32_t utf8_next(const uint8_t *s, size_t n, size_t *at) {
    size_t i = *at;
    unsigned c = s[i++];
    uint32_t cp = c;
    size_t more = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    if (more) cp = c & (0x3f >> more);
    while (more-- && i < n) cp = cp << 6 | (s[i++] & 63);
    *at = i;
    return cp;
}

bool web_encode_html(web_encoding encoding, const char *utf8, size_t length, struct web_bytes *out) {
    const uint8_t *s = (const uint8_t *)utf8;
    struct web_encoder e;
    web_encoder_init(&e, encoding);
    if (e.encoding == WEB_ENCODING_UTF8) return web_bytes_put(out, utf8, length);
    uint8_t bytes[WEB_ENCODER_MAX];
    for (size_t i = 0; i < length;) {
        uint32_t error, cp = utf8_next(s, length, &i);
        size_t n = web_encoder_put(&e, cp, bytes, &error);
        if (!web_bytes_put(out, bytes, n)) return false;
        if (error != UINT32_MAX) {
            char reference[16];
            int k = 0;
            char digits[12];
            do digits[k++] = (char)('0' + error % 10); while (error /= 10);
            size_t r = 0;
            reference[r++] = '&'; reference[r++] = '#';
            while (k) reference[r++] = digits[--k];
            reference[r++] = ';';
            if (!web_bytes_put(out, reference, r)) return false;
        }
    }
    return web_bytes_put(out, bytes, web_encoder_finish(&e, bytes));
}
