/* Host side of tests/check_encoding.py: runs user/libc/web/web_encoding.c and
   Lexbor's URL percent-encoding over a file of requests and writes a file of
   results. Each request: op, label length, label, flags, chunk size (u32),
   data length (u32), data. Each result: status (i8), length (u32), data.
     L  get an encoding of data; data out is the name (status 0 on failure)
     D  decoder of label over data in chunks; flags 1 fatal, 2 drop BOM, 4 sniff;
        out is UTF-8, status as web_decoder_run
     T  one decoder of label (flags as D) over a run of TextDecoder decode() calls,
        each u8 last (not stream), u32 length, bytes; out per call: status (i8),
        u32 length, UTF-8
     B  decode (BOM sniff, label as fallback); out is name, NUL, UTF-8
     S  data is u32 scalar values; for each a fresh encoder, encode or fail,
        then end-of-queue; out per value: byte count (u8), bytes, error (u32)
     H  encode in html mode of data (UTF-8)
     P  percent-encode after encoding, x-www-form-urlencoded set, space as +
     Q  URL parser over data (UTF-8) with label's encoding; out is the href.
        With a NUL in data, the part before it is the base (parsed as UTF-8) */
#include "../user/libc/web/web_encoding.h"
#include <lexbor/url/url.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void reply(FILE *out, int status, const void *data, size_t length) {
    uint8_t head[5] = {(uint8_t)(int8_t)status, (uint8_t)length, (uint8_t)(length >> 8),
                       (uint8_t)(length >> 16), (uint8_t)(length >> 24)};
    fwrite(head, 1, 5, out);
    fwrite(data, 1, length, out);
}

static lxb_status_t put(const lxb_char_t *data, size_t length, void *ctx) {
    return web_bytes_put(ctx, data, length) ? LXB_STATUS_OK : LXB_STATUS_ERROR_MEMORY_ALLOCATION;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) return 2;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    uint8_t *all = malloc(size ? (size_t)size : 1);
    if (!all || fread(all, 1, (size_t)size, in) != (size_t)size) return 2;
    lexbor_mraw_t *mraw = lexbor_mraw_create();
    lexbor_mraw_init(mraw, 4096);
    lxb_url_parser_t parser;
    lxb_url_parser_init(&parser, mraw);
    struct web_decoder *decoder = malloc(sizeof *decoder);
    for (size_t at = 0; at < (size_t)size;) {
        uint8_t op = all[at++], label_length = all[at++];
        const char *label = (const char *)all + at;
        at += label_length;
        uint8_t flags = all[at++];
        uint32_t chunk = u32(all + at), length = u32(all + at + 4);
        at += 8;
        const uint8_t *data = all + at;
        at += length;
        web_encoding encoding = web_encoding_get(label, label_length);
        struct web_bytes result = {0};
        int status = 1;
        switch (op) {
        case 'L': {
            web_encoding found = web_encoding_get((const char *)data, length);
            const char *name = web_encoding_name(found);
            status = found != WEB_ENCODING_FAILURE;
            web_bytes_put(&result, name, strlen(name));
            break;
        }
        case 'D':
            web_decoder_init(decoder, encoding, flags & 1);
            decoder->drop_bom = flags & 2;
            decoder->sniff = flags & 4;
            if (!chunk) chunk = length ? length : 1;
            for (uint32_t i = 0;; i += chunk) {
                uint32_t n = length - i < chunk ? length - i : chunk;
                status = web_decoder_run(decoder, data + i, n, i + n >= length, &result);
                if (status != 1 || i + n >= length) break;
            }
            break;
        case 'T':
            web_decoder_init(decoder, encoding, flags & 1);
            decoder->drop_bom = flags & 2;
            for (uint32_t i = 0; i + 5 <= length;) {
                bool last = data[i];
                uint32_t n = u32(data + i + 1);
                struct web_bytes call = {0};
                int8_t called = (int8_t)web_decoder_run(decoder, data + i + 5, n, last, &call);
                uint8_t head[5] = {(uint8_t)called, (uint8_t)call.length, (uint8_t)(call.length >> 8),
                                   (uint8_t)(call.length >> 16), (uint8_t)(call.length >> 24)};
                web_bytes_put(&result, head, 5);
                if (call.length) web_bytes_put(&result, call.data, call.length);
                free(call.data);
                i += 5 + n;
            }
            break;
        case 'B': {
            web_encoding used;
            size_t n;
            char *text = web_decode(data, length, encoding, &used, &n);
            const char *name = web_encoding_name(used);
            web_bytes_put(&result, name, strlen(name) + 1);
            if (text) web_bytes_put(&result, text, n);
            else status = -1;
            free(text);
            break;
        }
        case 'S':
            for (uint32_t i = 0; i + 4 <= length; i += 4) {
                struct web_encoder e;
                uint8_t bytes[2 * WEB_ENCODER_MAX + 1];
                uint32_t error;
                web_encoder_init(&e, encoding);
                size_t n = web_encoder_put(&e, u32(data + i), bytes + 1, &error);
                n += web_encoder_finish(&e, bytes + 1 + n);
                bytes[0] = (uint8_t)n;
                web_bytes_put(&result, bytes, n + 1);
                uint8_t e4[4] = {(uint8_t)error, (uint8_t)(error >> 8), (uint8_t)(error >> 16),
                                 (uint8_t)(error >> 24)};
                web_bytes_put(&result, e4, 4);
            }
            break;
        case 'H':
            status = web_encode_html(encoding, (const char *)data, length, &result) ? 1 : -1;
            break;
        case 'P': {
            lexbor_str_t str = {0};
            lxb_status_t s = lxb_url_percent_encode_encoding(data, length, &str, mraw,
                lxb_url_get_percent_encoding_map(), lxb_encoding_data(web_encoding_output(encoding)),
                LXB_URL_MAP_X_WWW_FORM, true);
            status = s == LXB_STATUS_OK;
            if (status) web_bytes_put(&result, str.data, str.length);
            lexbor_mraw_clean(mraw);
            break;
        }
        case 'Q': {
            const uint8_t *nul = memchr(data, 0, length);
            lxb_url_t *base = NULL;
            if (nul) {
                base = lxb_url_parse_basic(&parser, NULL, NULL, data, (size_t)(nul - data),
                                           LXB_URL_STATE__UNDEF, LXB_ENCODING_UTF_8) == LXB_STATUS_OK ?
                    parser.url : NULL;
                lxb_url_parser_clean(&parser);
                length -= (uint32_t)(nul + 1 - data);
                data = nul + 1;
            }
            lxb_url_t *url = !nul || base ?
                (lxb_url_parse_basic(&parser, NULL, base, data, length, LXB_URL_STATE__UNDEF, encoding) ==
                 LXB_STATUS_OK ? parser.url : NULL) : NULL;
            lxb_url_parser_clean(&parser);
            status = url != NULL;
            if (url) lxb_url_serialize(url, put, &result, false);
            lexbor_mraw_clean(mraw);
            break;
        }
        default:
            status = -2;
        }
        reply(out, status, result.data, result.length);
        free(result.data);
    }
    fclose(out);
    return 0;
}
