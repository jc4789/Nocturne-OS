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
    /* The last base parsed, in memory of its own that outlives a parse: one
       document's URLs are mostly against one base. base_url is NULL when
       there is none; base_text is its input. */
    lexbor_mraw_t *base_mraw;
    lxb_url_t *base_url;
    char *base_text;
    size_t base_length, base_capacity;
};

struct web_url_parser *web_url_parser_new(void) {
    struct web_url_parser *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->mraw = lexbor_mraw_create();
    p->base_mraw = p->mraw ? lexbor_mraw_create() : NULL;
    if (!p->base_mraw || lexbor_mraw_init(p->mraw, 4096) != LXB_STATUS_OK ||
        lexbor_mraw_init(p->base_mraw, 1024) != LXB_STATUS_OK ||
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
    if (p->base_mraw) lexbor_mraw_destroy(p->base_mraw, true);
    free(p->base_text);
    free(p);
}

/* Appends to the serialization, kept NUL-terminated; false when out of
   memory, the buffer as it was (sbuf's own append aborts instead). */
static bool href_put(sbuf *b, const void *data, size_t length) {
    if (!length) return true;
    if (length > SIZE_MAX - b->n - 1) return false;
    size_t need = b->n + length + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < need) cap = cap > SIZE_MAX / 2 ? need : cap * 2;
        char *grown = realloc(b->p, cap);
        if (!grown) return false;
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, length);
    b->n += length;
    b->p[b->n] = 0;
    return true;
}

static lxb_status_t put(const lxb_char_t *data, size_t length, void *out) {
    return href_put(out, data, length) ? LXB_STATUS_OK : LXB_STATUS_ERROR_MEMORY_ALLOCATION;
}

_Static_assert((int)WEB_URL_HOST_NULL == (int)LXB_URL_HOST_TYPE__UNDEF &&
               (int)WEB_URL_HOST_DOMAIN == (int)LXB_URL_HOST_TYPE_DOMAIN &&
               (int)WEB_URL_HOST_OPAQUE == (int)LXB_URL_HOST_TYPE_OPAQUE &&
               (int)WEB_URL_HOST_IPV4 == (int)LXB_URL_HOST_TYPE_IPV4 &&
               (int)WEB_URL_HOST_IPV6 == (int)LXB_URL_HOST_TYPE_IPV6 &&
               (int)WEB_URL_HOST_EMPTY == (int)LXB_URL_HOST_TYPE_EMPTY, "Lexbor's host types");

void web_url_free(struct web_url *url) {
    if (!url) return;
    free(url->href);
    memset(url, 0, sizeof *url);
}

/* The URL serializer, noting each component's span on the way; false when
   out of memory (out then empty). */
static bool serialize(const lxb_url_t *url, struct web_url *out) {
    sbuf text = {0}, *b = &text;
    size_t *mark = out->mark;
    bool ok = lxb_url_serialize_scheme(url, put, b) == LXB_STATUS_OK && href_put(b, ":", 1);
    mark[WEB_URL_PROTOCOL_END] = b->n;
    if (url->host.type != LXB_URL_HOST_TYPE__UNDEF) {
        bool credentials = url->username.length || url->password.length;
        ok = ok && href_put(b, "//", 2);
        mark[WEB_URL_USERNAME_START] = b->n;
        ok = ok && lxb_url_serialize_username(url, put, b) == LXB_STATUS_OK;
        mark[WEB_URL_USERNAME_END] = b->n;
        ok = ok && (!url->password.length || href_put(b, ":", 1));
        mark[WEB_URL_PASSWORD_START] = b->n;
        ok = ok && lxb_url_serialize_password(url, put, b) == LXB_STATUS_OK;
        mark[WEB_URL_PASSWORD_END] = b->n;
        ok = ok && (!credentials || href_put(b, "@", 1));
        mark[WEB_URL_HOST_START] = b->n;
        ok = ok && lxb_url_serialize_host(&url->host, put, b) == LXB_STATUS_OK;
        mark[WEB_URL_HOST_END] = b->n;
        ok = ok && (!url->has_port || href_put(b, ":", 1));
        mark[WEB_URL_PORT_START] = b->n;
        ok = ok && lxb_url_serialize_port(url, put, b) == LXB_STATUS_OK;
        mark[WEB_URL_PORT_END] = b->n;
    } else {
        for (int i = WEB_URL_USERNAME_START; i <= WEB_URL_PORT_END; i++) mark[i] = b->n;
        /* A path that would read as an authority after the scheme. */
        const lexbor_str_t *path = &url->path.str;
        if (!url->path.opaque && path->length > 1 && path->data[0] == '/' && path->data[1] == '/')
            ok = ok && href_put(b, "/.", 2);
    }
    mark[WEB_URL_PATH_START] = b->n;
    ok = ok && lxb_url_serialize_path(&url->path, put, b) == LXB_STATUS_OK;
    mark[WEB_URL_QUERY_START] = b->n;
    if (url->query.data) ok = ok && href_put(b, "?", 1) && href_put(b, url->query.data, url->query.length);
    mark[WEB_URL_FRAGMENT_START] = b->n;
    if (url->fragment.data)
        ok = ok && href_put(b, "#", 1) && href_put(b, url->fragment.data, url->fragment.length);
    if (!ok) {
        free(text.p);
        memset(out, 0, sizeof *out);
        return false;
    }
    out->href = text.p;
    out->length = text.n;
    out->host = (enum web_url_host_type)url->host.type;
    out->port = url->has_port ? url->port : -1;
    return true;
}

/* A parse in the parser's scratch memory: NULL for failure, with *status.
   Lexbor takes failure, as every encoding without an encoder, as UTF-8. */
static lxb_url_t *parse(struct web_url_parser *p, const lxb_url_t *base, const char *input,
                        size_t length, web_encoding encoding, lxb_status_t *status) {
    *status = lxb_url_parse_basic(&p->parser, NULL, base, (const lxb_char_t *)input, length,
                                  LXB_URL_STATE__UNDEF, encoding);
    lxb_url_t *url = *status == LXB_STATUS_OK ? p->parser.url : NULL;
    lxb_url_parser_clean(&p->parser);
    return url;
}

static int result_of(lxb_status_t status) {
    return status == LXB_STATUS_ERROR_MEMORY_ALLOCATION ? -1 : 0;
}

/* base parsed alone (UTF-8), the last parse's when it was the same input:
   NULL for failure, with *status. */
static const lxb_url_t *base_of(struct web_url_parser *p, const char *base, size_t length,
                                lxb_status_t *status) {
    if (p->base_url && p->base_length == length && !memcmp(p->base_text, base, length)) {
        *status = LXB_STATUS_OK;
        return p->base_url;
    }
    p->base_url = NULL;
    lexbor_mraw_clean(p->base_mraw);
    if (length >= p->base_capacity) {
        char *text = length < SIZE_MAX ? realloc(p->base_text, length + 1) : NULL;
        if (!text) {
            *status = LXB_STATUS_ERROR_MEMORY_ALLOCATION;
            return NULL;
        }
        p->base_text = text;
        p->base_capacity = length + 1;
    }
    p->parser.mraw = p->base_mraw;
    lxb_url_t *url = parse(p, NULL, base, length, WEB_ENCODING_UTF8, status);
    p->parser.mraw = p->mraw;
    if (url) {
        memcpy(p->base_text, base, length);
        p->base_length = length;
        p->base_url = url;
    }
    return url;
}

/* input against base_url (or none) into out: 1, 0 or -1, out empty unless 1. */
static int parse_into(struct web_url_parser *p, const lxb_url_t *base_url, const char *input,
                      size_t length, web_encoding encoding, struct web_url *out) {
    lxb_status_t status;
    lxb_url_t *url = parse(p, base_url, input, length, encoding, &status);
    int result = url ? serialize(url, out) ? 1 : -1 : result_of(status);
    lexbor_mraw_clean(p->mraw);
    return result;
}

int web_url_parse(struct web_url_parser *p, const char *input, size_t length,
                  const char *base, size_t base_length, web_encoding encoding, struct web_url *out) {
    lxb_status_t status = LXB_STATUS_OK;
    const lxb_url_t *base_url = base ? base_of(p, base, base_length, &status) : NULL;
    if (base && !base_url) return result_of(status);
    return parse_into(p, base_url, input, length, encoding, out);
}

bool web_url_valid(struct web_url_parser *p, const char *input, size_t length) {
    lxb_status_t status;
    bool valid = parse(p, NULL, input, length, WEB_ENCODING_UTF8, &status) != NULL;
    lexbor_mraw_clean(p->mraw);
    return valid;
}

/* The search setter with the query's encoding (Lexbor's own setter parses it
   as UTF-8): empty, no query; otherwise the value without one leading '?',
   basic URL parsed into an empty query in query state. */
static lxb_status_t search_set(lxb_url_t *url, lxb_url_parser_t *parser, const lxb_char_t *value,
                               size_t length, web_encoding encoding) {
    lexbor_str_destroy(&url->query, url->mraw, false);
    url->query.length = 0;
    if (!length) return LXB_STATUS_OK;
    if (*value == '?') { value++; length--; }
    return lxb_url_parse_basic(parser, url, NULL, value, length, LXB_URL_STATE_QUERY_STATE, encoding);
}

int web_url_set(struct web_url_parser *p, const char *href, size_t href_length,
                enum web_url_part part, const char *value, size_t length, web_encoding encoding,
                struct web_url *out) {
    lxb_status_t status;
    lxb_url_t *url = parse(p, NULL, href, href_length, WEB_ENCODING_UTF8, &status);
    if (url) {
        lxb_url_parser_t *parser = &p->parser;
        const lxb_char_t *v = (const lxb_char_t *)value;
        if (part == WEB_URL_SEARCH && web_encoding_output(encoding) != WEB_ENCODING_UTF8)
            status = search_set(url, parser, v, length, encoding);
        else switch (part) {
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
    if (result == 1 && !serialize(url, out)) result = -1;
    lexbor_mraw_clean(p->mraw);
    return result;
}

/* Parsers lent to the engine's native callers. A caller takes one from a slot
   for its parse alone, so no two threads ever parse in one; a caller that
   finds every slot empty (the CSS parse and cascade helpers resolve url()s
   beside the main thread) makes one, and a parser given back to full slots is
   freed. One slot per parallel.c worker and its caller. */
enum { LENT_PARSERS = 32 };
static struct web_url_parser *lent[LENT_PARSERS];

static struct web_url_parser *borrow(void) {
    for (unsigned i = 0; i < LENT_PARSERS; i++) {
        struct web_url_parser *p;
        if (__atomic_load_n(&lent[i], __ATOMIC_RELAXED) &&
            (p = __atomic_exchange_n(&lent[i], NULL, __ATOMIC_ACQUIRE)))
            return p;
    }
    return web_url_parser_new();
}

static void give_back(struct web_url_parser *p) {
    for (unsigned i = 0; i < LENT_PARSERS; i++) {
        struct web_url_parser *empty = NULL;
        if (!__atomic_load_n(&lent[i], __ATOMIC_RELAXED) &&
            __atomic_compare_exchange_n(&lent[i], &empty, p, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
    }
    web_url_parser_free(p);
}

/* The DOM's and QuickJS's strings keep a lone surrogate as its three UTF-8
   bytes; the URL parser takes scalar values, a USVString's conversion making
   it U+FFFD. *copy is text so converted, or NULL when it has none; false when
   out of memory. */
static bool scalar_values(const char *text, size_t length, char **copy) {
    *copy = NULL;
    const unsigned char *s = (const unsigned char *)text, *end = s + length;
    for (const unsigned char *at = memchr(s, 0xed, length); at; at = memchr(at, 0xed, (size_t)(end - at))) {
        if (++at < end && *at >= 0xa0) {
            if (!(*copy = malloc(length + 1))) return false;
            memcpy(*copy, text, length + 1);
            web_utf8_replace_lone_surrogates((uint8_t *)*copy, length);
            return true;
        }
    }
    return true;
}

int web_url_resolve(const char *base, const char *input, web_encoding encoding, char **out) {
    if (!out) return 0;
    *out = NULL;
    if (!input) return 0;
    char *input_copy, *base_copy = NULL;
    size_t length = strlen(input), base_length = base ? strlen(base) : 0;
    if (!scalar_values(input, length, &input_copy) || (base && !scalar_values(base, base_length, &base_copy))) {
        free(input_copy);
        return -1;
    }
    struct web_url_parser *p = borrow();
    int result = -1;
    if (p) {
        lxb_status_t status = LXB_STATUS_OK;
        /* A base that is not a URL is no base: an absolute URL still parses. */
        const lxb_url_t *base_url = base ? base_of(p, base_copy ? base_copy : base, base_length, &status) : NULL;
        struct web_url url = {0};
        result = status == LXB_STATUS_ERROR_MEMORY_ALLOCATION ? -1 :
            parse_into(p, base_url, input_copy ? input_copy : input, length, encoding, &url);
        if (result == 1) *out = url.href;
        give_back(p);
    }
    free(input_copy);
    free(base_copy);
    return result;
}

int web_resolve_url_owned(const char *base, const char *rel, char **out) {
    return web_url_resolve(base, rel, WEB_ENCODING_UTF8, out);
}

int web_url_read(const char *input, size_t length, struct web_url *url) {
    if (!url) return 0;
    memset(url, 0, sizeof *url);
    if (!input) return 0;
    char *copy;
    if (!scalar_values(input, length, &copy)) return -1;
    struct web_url_parser *p = borrow();
    int result = p ? parse_into(p, NULL, copy ? copy : input, length, WEB_ENCODING_UTF8, url) : -1;
    if (p) give_back(p);
    free(copy);
    return result;
}

int web_url_change(const char *href, enum web_url_part part, const char *value, char **out) {
    if (!out) return 0;
    *out = NULL;
    if (!href || !value) return 0;
    struct web_url_parser *p = borrow();
    if (!p) return -1;
    struct web_url url = {0};
    int result = web_url_set(p, href, strlen(href), part, value, strlen(value), WEB_ENCODING_UTF8, &url);
    give_back(p);
    if (result == 1) *out = url.href;
    return result;
}

/* A tuple origin's serialization: scheme "://" and the host and port as the URL's own
   serialization has them (the port is null when it is the scheme's default). */
static enum web_url_result tuple_origin(const struct web_url *url, char **origin) {
    size_t scheme = url->mark[WEB_URL_PROTOCOL_END], authority;
    const char *at = web_url_authority(url, &authority);
    char *text = malloc(scheme + 2 + authority + 1);
    if (!text) return WEB_URL_OOM;
    memcpy(text, url->href, scheme);
    memcpy(text + scheme, "//", 2);
    memcpy(text + scheme + 2, at, authority);
    text[scheme + 2 + authority] = 0;
    *origin = text;
    return WEB_URL_TUPLE;
}

enum web_url_result web_url_origin_of(const struct web_url *url, char **origin) {
    if (!origin) return WEB_URL_INVALID;
    *origin = NULL;
    if (!url || !url->href) return WEB_URL_INVALID;
    if (web_url_scheme_is(url, "ftp") || web_url_scheme_is(url, "http") || web_url_scheme_is(url, "https") ||
        web_url_scheme_is(url, "ws") || web_url_scheme_is(url, "wss"))
        return tuple_origin(url, origin);
    if (!web_url_scheme_is(url, "blob")) return WEB_URL_OPAQUE;
    /* A blob URL entry's environment's origin is the blob store's; without one, the origin of
       the URL its path parses as, when that is http, https or file (whose origin is opaque here). */
    size_t length;
    const char *path = web_url_span(url, WEB_URL_PATH_START, WEB_URL_QUERY_START, &length);
    struct web_url inner;
    int parsed = web_url_read(path, length, &inner);
    if (parsed < 0) return WEB_URL_OOM;
    enum web_url_result result = WEB_URL_OPAQUE;
    if (parsed == 1 && web_url_http_scheme(&inner))
        result = tuple_origin(&inner, origin);
    web_url_free(&inner);
    return result;
}

enum web_url_result web_url_origin(const char *text, char **origin) {
    if (origin) *origin = NULL;
    if (!origin || !text) return WEB_URL_INVALID;
    struct web_url url;
    int parsed = web_url_read(text, strlen(text), &url);
    if (parsed != 1) return parsed < 0 ? WEB_URL_OOM : WEB_URL_INVALID;
    enum web_url_result result = web_url_origin_of(&url, origin);
    web_url_free(&url);
    return result;
}

int web_url_same_origin(const char *a, const char *b) {
    char *x = NULL, *y = NULL;
    enum web_url_result xr = web_url_origin(a, &x), yr = web_url_origin(b, &y);
    int same = xr == WEB_URL_OOM || yr == WEB_URL_OOM ? -1 :
               xr == WEB_URL_TUPLE && yr == WEB_URL_TUPLE && !strcmp(x, y);
    free(x);
    free(y);
    return same;
}

bool web_origin_http(const char *origin) {
    if (!origin) return false;
    size_t length = strlen(origin);
    struct web_url url;
    if (web_url_read(origin, length, &url) != 1) return false;
    /* The origin's own text, and the URL it reads as is that origin's URL: scheme, host,
       port and the path "/", nothing else. */
    bool valid = web_url_http_scheme(&url) &&
                 !web_url_includes_credentials(&url) && url.length == length + 1 &&
                 url.mark[WEB_URL_PATH_START] == length && url.href[length] == '/' &&
                 !memcmp(url.href, origin, length);
    web_url_free(&url);
    return valid;
}

bool web_url_host_serialized(const char *host, size_t length) {
    if (!host || !length || memchr(host, 0, length) || length > SIZE_MAX - 9) return false;
    char *text = malloc(length + 9);
    if (!text) return false;
    memcpy(text, "http://", 7);
    memcpy(text + 7, host, length);
    memcpy(text + 7 + length, "/", 2);
    struct web_url url;
    bool valid = false;
    if (web_url_read(text, length + 8, &url) == 1) {
        size_t n;
        const char *parsed = web_url_host_text(&url, &n);
        valid = url.port < 0 && n == length && !memcmp(parsed, host, length) && url.length == length + 8;
        web_url_free(&url);
    }
    free(text);
    return valid;
}

/* Secure Contexts' "Is origin potentially trustworthy?" for the tuple origin of url (ftp,
   http, https, ws or wss): https and wss, then a loopback host. Every serialization of an
   IPv4 address in 127.0.0.0/8 starts "127." and ::1's is "[::1]". */
static bool tuple_trustworthy(const struct web_url *url) {
    size_t n;
    const char *host = web_url_host_text(url, &n);
    return web_url_scheme_is(url, "https") || web_url_scheme_is(url, "wss") ||
           (url->host == WEB_URL_HOST_IPV4 && n > 4 && !memcmp(host, "127.", 4)) ||
           (url->host == WEB_URL_HOST_IPV6 && n == 5 && !memcmp(host, "[::1]", 5)) ||
           (url->host == WEB_URL_HOST_DOMAIN && n == 9 && !memcmp(host, "localhost", 9));
}

/* HTML's "matches about:blank" (or about:srcdoc): the about scheme, no credentials, a null
   host and the path name, whatever the query and fragment. */
static bool about_is(const struct web_url *url, const char *name) {
    size_t n;
    const char *path = web_url_span(url, WEB_URL_PATH_START, WEB_URL_QUERY_START, &n);
    return web_url_scheme_is(url, "about") && url->host == WEB_URL_HOST_NULL &&
           n == strlen(name) && !memcmp(path, name, n);
}

bool web_url_potentially_trustworthy(const char *text) {
    if (!text) return false;
    struct web_url url;
    if (web_url_read(text, strlen(text), &url) != 1) return false;
    bool trustworthy = about_is(&url, "blank") || about_is(&url, "srcdoc") ||
                       web_url_scheme_is(&url, "data") || web_url_scheme_is(&url, "file");
    if (!trustworthy && web_url_default_port(&url) >= 0) trustworthy = tuple_trustworthy(&url);
    else if (!trustworthy && web_url_scheme_is(&url, "blob")) {
        /* A blob URL's origin is its path's when that is an http or https URL. */
        size_t length;
        const char *path = web_url_span(&url, WEB_URL_PATH_START, WEB_URL_QUERY_START, &length);
        struct web_url inner;
        if (web_url_read(path, length, &inner) == 1) {
            trustworthy = web_url_http_scheme(&inner) &&
                          tuple_trustworthy(&inner);
            web_url_free(&inner);
        }
    }
    web_url_free(&url);
    return trustworthy;
}

int web_url_equal(const char *a, const char *b, bool exclude_fragments) {
    struct web_url x, y;
    int xr = web_url_read(a, a ? strlen(a) : 0, &x);
    if (xr != 1) return xr;
    int yr = web_url_read(b, b ? strlen(b) : 0, &y);
    if (yr != 1) { web_url_free(&x); return yr; }
    size_t xn = exclude_fragments ? x.mark[WEB_URL_FRAGMENT_START] : x.length,
           yn = exclude_fragments ? y.mark[WEB_URL_FRAGMENT_START] : y.length;
    int equal = xn == yn && !memcmp(x.href, y.href, xn);
    web_url_free(&x);
    web_url_free(&y);
    return equal;
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
