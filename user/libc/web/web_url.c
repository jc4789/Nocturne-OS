/* The URL Standard's parser, setters and serializer are Lexbor's WHATWG
   implementation; this keeps a parser's scratch memory and records where each
   component of a serialization lies. The urlencoded format follows the
   standard directly.
   https://url.spec.whatwg.org/ */
#include "web_url.h"
#include <lexbor/url/url.h>

struct web_url_parser {
    lexbor_mraw_t *mraw;
    lxb_url_parser_t parser;
};

struct web_url_parser *web_url_parser_new(void) {
    struct web_url_parser *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->mraw = lexbor_mraw_create();
    if (!p->mraw || lexbor_mraw_init(p->mraw, 4096) != LXB_STATUS_OK ||
        lxb_url_parser_init(&p->parser, p->mraw) != LXB_STATUS_OK) {
        web_url_parser_free(p);
        return NULL;
    }
    return p;
}

void web_url_parser_free(struct web_url_parser *p) {
    if (!p) return;
    lxb_url_parser_destroy(&p->parser, false);
    if (p->mraw) lexbor_mraw_destroy(p->mraw, true);
    free(p);
}

static lxb_status_t put(const lxb_char_t *data, size_t length, void *out) {
    if (length) sb_put(out, (const char *)data, length);
    return LXB_STATUS_OK;
}

/* The URL serializer, noting each component's span on the way. */
static void serialize(const lxb_url_t *url, struct web_url *out) {
    sbuf *b = &out->href;
    size_t *mark = out->mark;
    lxb_url_serialize_scheme(url, put, b);
    sb_putc(b, ':');
    mark[WEB_URL_PROTOCOL_END] = b->n;
    if (url->host.type != LXB_URL_HOST_TYPE__UNDEF) {
        bool credentials = url->username.length || url->password.length;
        sb_put(b, "//", 2);
        mark[WEB_URL_USERNAME_START] = b->n;
        lxb_url_serialize_username(url, put, b);
        mark[WEB_URL_USERNAME_END] = b->n;
        if (url->password.length) sb_putc(b, ':');
        mark[WEB_URL_PASSWORD_START] = b->n;
        lxb_url_serialize_password(url, put, b);
        mark[WEB_URL_PASSWORD_END] = b->n;
        if (credentials) sb_putc(b, '@');
        mark[WEB_URL_HOST_START] = b->n;
        lxb_url_serialize_host(&url->host, put, b);
        mark[WEB_URL_HOST_END] = b->n;
        if (url->has_port) sb_putc(b, ':');
        mark[WEB_URL_PORT_START] = b->n;
        lxb_url_serialize_port(url, put, b);
        mark[WEB_URL_PORT_END] = b->n;
    } else {
        for (int i = WEB_URL_USERNAME_START; i <= WEB_URL_PORT_END; i++) mark[i] = b->n;
        /* A path that would read as an authority after the scheme. */
        const lexbor_str_t *path = &url->path.str;
        if (!url->path.opaque && path->length > 1 && path->data[0] == '/' && path->data[1] == '/')
            sb_put(b, "/.", 2);
    }
    mark[WEB_URL_PATH_START] = b->n;
    lxb_url_serialize_path(&url->path, put, b);
    mark[WEB_URL_QUERY_START] = b->n;
    if (url->query.data) {
        sb_putc(b, '?');
        put(url->query.data, url->query.length, b);
    }
    mark[WEB_URL_FRAGMENT_START] = b->n;
    if (url->fragment.data) {
        sb_putc(b, '#');
        put(url->fragment.data, url->fragment.length, b);
    }
}

/* A parse in the parser's scratch memory: NULL for failure, with *status. */
static lxb_url_t *parse(struct web_url_parser *p, const lxb_url_t *base,
                        const char *input, size_t length, lxb_status_t *status) {
    *status = lxb_url_parse_basic(&p->parser, NULL, base, (const lxb_char_t *)input, length,
                                  LXB_URL_STATE__UNDEF, LXB_ENCODING_AUTO);
    lxb_url_t *url = *status == LXB_STATUS_OK ? p->parser.url : NULL;
    lxb_url_parser_clean(&p->parser);
    return url;
}

static int result_of(lxb_status_t status) {
    return status == LXB_STATUS_ERROR_MEMORY_ALLOCATION ? -1 : 0;
}

int web_url_parse(struct web_url_parser *p, const char *input, size_t length,
                  const char *base, size_t base_length, struct web_url *out) {
    lxb_status_t status = LXB_STATUS_OK;
    lxb_url_t *base_url = base ? parse(p, NULL, base, base_length, &status) : NULL;
    lxb_url_t *url = !base || base_url ? parse(p, base_url, input, length, &status) : NULL;
    int result = url ? 1 : result_of(status);
    if (url) serialize(url, out);
    lexbor_mraw_clean(p->mraw);
    return result;
}

bool web_url_valid(struct web_url_parser *p, const char *input, size_t length) {
    lxb_status_t status;
    bool valid = parse(p, NULL, input, length, &status) != NULL;
    lexbor_mraw_clean(p->mraw);
    return valid;
}

int web_url_set(struct web_url_parser *p, const char *href, size_t href_length,
                enum web_url_part part, const char *value, size_t length, struct web_url *out) {
    lxb_status_t status;
    lxb_url_t *url = parse(p, NULL, href, href_length, &status);
    if (url) {
        lxb_url_parser_t *parser = &p->parser;
        const lxb_char_t *v = (const lxb_char_t *)value;
        switch (part) {
        case WEB_URL_PROTOCOL: status = lxb_url_api_protocol_set(url, parser, v, length); break;
        case WEB_URL_USERNAME: status = lxb_url_api_username_set(url, v, length); break;
        case WEB_URL_PASSWORD: status = lxb_url_api_password_set(url, v, length); break;
        case WEB_URL_HOST: status = lxb_url_api_host_set(url, parser, v, length); break;
        case WEB_URL_HOSTNAME: status = lxb_url_api_hostname_set(url, parser, v, length); break;
        case WEB_URL_PORT: status = lxb_url_api_port_set(url, parser, v, length); break;
        case WEB_URL_PATHNAME: status = lxb_url_api_pathname_set(url, parser, v, length); break;
        case WEB_URL_SEARCH: status = lxb_url_api_search_set(url, parser, v, length); break;
        case WEB_URL_HASH: status = lxb_url_api_hash_set(url, parser, v, length); break;
        }
        lxb_url_parser_clean(parser);
    }
    /* A value the setter turns down is no error: the URL stays as it was. */
    int result = url && status != LXB_STATUS_ERROR_MEMORY_ALLOCATION ? 1 : url ? -1 : result_of(status);
    if (result == 1) serialize(url, out);
    lexbor_mraw_clean(p->mraw);
    return result;
}

/* '+' as a space, then percent-decoding, in place. */
static size_t form_decode(char *s, size_t length) {
    size_t n = 0;
    for (size_t i = 0; i < length; i++) {
        int high, low;
        if (s[i] == '%' && i + 2 < length && (high = web_hex_digit((unsigned char)s[i + 1])) >= 0 &&
            (low = web_hex_digit((unsigned char)s[i + 2])) >= 0) {
            s[n++] = (char)(high << 4 | low);
            i += 2;
        } else s[n++] = s[i] == '+' ? ' ' : s[i];
    }
    return n;
}

int web_url_form_parse(const char *input, size_t length,
                       bool (*pair)(void *, const char *, size_t, const char *, size_t),
                       void *opaque) {
    char *bytes = malloc(length ? length : 1);
    if (!bytes) return -1;
    memcpy(bytes, input, length);
    int result = 1;
    for (size_t at = 0; at < length && result;) {
        char *sequence = bytes + at, *end = memchr(sequence, '&', length - at);
        size_t size = end ? (size_t)(end - sequence) : length - at;
        at += size + 1;
        if (!size) continue;
        char *equals = memchr(sequence, '=', size);
        size_t name = equals ? (size_t)(equals - sequence) : size;
        char *value = equals ? equals + 1 : sequence + size;
        size_t value_size = equals ? size - name - 1 : 0;
        if (!pair(opaque, sequence, form_decode(sequence, name), value, form_decode(value, value_size)))
            result = 0;
    }
    free(bytes);
    return result;
}

void url_encode_form(sbuf *out, const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '*' || c == '-' || c == '.' || c == '_')
            sb_putc(out, (char)c);
        else if (c == ' ') sb_putc(out, '+');
        else {
            static const char digits[] = "0123456789ABCDEF";
            char escape[3] = {'%', digits[c >> 4], digits[c & 15]};
            sb_put(out, escape, sizeof escape);
        }
    }
}
