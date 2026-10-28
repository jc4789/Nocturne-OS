/* The URL Standard's URL record for every native layer of the browser: the engine, the request
   layer (http.c), the cookie jar (webcookie.c), the network and fetch processes (webnet.c,
   webfetch.c), media and the storage layers. One parser makes it, user/libc/web/web_url.c's
   (Lexbor's WHATWG URL parser, which every program links with the rest of libc), and a layer
   that is given a URL reads its scheme, host, port, path, query and origin from the record
   rather than splitting the text again with a grammar of its own.
   https://url.spec.whatwg.org/ and HTML's origins
   (https://html.spec.whatwg.org/multipage/browsers.html#origin). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Where each component of a URL's serialization lies. A component the URL lacks is an empty
   span where it would be. */
enum web_url_mark {
    WEB_URL_PROTOCOL_END,                       /* after the scheme's ':' */
    WEB_URL_USERNAME_START, WEB_URL_USERNAME_END,
    WEB_URL_PASSWORD_START, WEB_URL_PASSWORD_END,
    WEB_URL_HOST_START, WEB_URL_HOST_END,
    WEB_URL_PORT_START, WEB_URL_PORT_END,       /* the digits, without ':' */
    WEB_URL_PATH_START,
    WEB_URL_QUERY_START,                        /* at its '?' */
    WEB_URL_FRAGMENT_START,                     /* at its '#' */
    WEB_URL_MARKS
};

/* The kind of a URL's host (the URL Standard's host, null when the URL has none). */
enum web_url_host_type {
    WEB_URL_HOST_NULL, WEB_URL_HOST_DOMAIN, WEB_URL_HOST_OPAQUE, WEB_URL_HOST_IPV4,
    WEB_URL_HOST_IPV6, WEB_URL_HOST_EMPTY
};

/* A URL: its serialization (always ASCII, NUL-terminated, malloc'd) and where each component
   lies in it. port is the URL's port, -1 when it is null (a special scheme's default port is
   always null in the record). */
struct web_url {
    char *href;
    size_t length;
    size_t mark[WEB_URL_MARKS];
    enum web_url_host_type host;
    int port;
};

/* The URL interface's setters, in its order. */
enum web_url_part {
    WEB_URL_PROTOCOL, WEB_URL_USERNAME, WEB_URL_PASSWORD, WEB_URL_HOST,
    WEB_URL_HOSTNAME, WEB_URL_PORT, WEB_URL_PATHNAME, WEB_URL_SEARCH, WEB_URL_HASH
};

/* The URL parser, UTF-8 for the query: rel against base (NULL, or a base that is not a URL:
   none), on any thread. 1: *out is the URL's serialization, malloc'd for the caller to free;
   0: rel is not a URL (*out NULL); -1: out of memory. No length limit. (HTML's encoding-parse
   with a document's encoding is web_url.h's web_url_resolve.) */
int web_resolve_url_owned(const char *base, const char *rel, char **out);
/* The URL parser given input alone (no base, UTF-8 for the query), on any thread, with a
   parser lent for the call: a URL's serialization reads back as the same URL. A lone
   surrogate's three bytes parse as U+FFFD. 1: url holds the record (web_url_free it); 0: input
   is not a URL; -1: out of memory. url is left empty unless 1. */
int web_url_read(const char *input, size_t length, struct web_url *url);
void web_url_free(struct web_url *url);
/* The URL href becomes when part's setter is given value (UTF-8), as the URL interface's
   setters change it; a value the setter turns down leaves it as it was. 1: *out is the
   serialization, malloc'd; 0: href is not a URL; -1: out of memory. */
int web_url_change(const char *href, enum web_url_part part, const char *value, char **out);

/* A component of url: its first byte in the serialization, and its length in *length. */
static inline const char *web_url_span(const struct web_url *url, enum web_url_mark start,
                                       enum web_url_mark end, size_t *length) {
    *length = url->mark[end] - url->mark[start];
    return url->href + url->mark[start];
}
/* Whether url's scheme is scheme (ASCII lowercase, as every scheme in a record is). */
static inline bool web_url_scheme_is(const struct web_url *url, const char *scheme) {
    size_t n = strlen(scheme);
    return url->mark[WEB_URL_PROTOCOL_END] == n + 1 && !memcmp(url->href, scheme, n);
}
/* Fetch's "HTTP(S) scheme": whether url's scheme is http or https. */
static inline bool web_url_http_scheme(const struct web_url *url) {
    return web_url_scheme_is(url, "http") || web_url_scheme_is(url, "https");
}
/* Whether the serialization of a URL (any string a URL record or the URL parser gave) has
   scheme: the scheme of a serialization is all of it before its first ':', in ASCII lowercase.
   Not for text that has not been through the URL parser, whose scheme the parser finds. */
static inline bool web_url_has_scheme(const char *serialized, const char *scheme) {
    size_t n = strlen(scheme);
    return serialized && !strncmp(serialized, scheme, n) && serialized[n] == ':';
}
/* The default port of url's scheme (the URL Standard's special schemes), -1 for none. */
static inline int web_url_default_port(const struct web_url *url) {
    return web_url_scheme_is(url, "http") || web_url_scheme_is(url, "ws") ? 80 :
           web_url_scheme_is(url, "https") || web_url_scheme_is(url, "wss") ? 443 :
           web_url_scheme_is(url, "ftp") ? 21 : -1;
}
/* The port a connection to url goes to: its port, or its scheme's default port; -1 for none. */
static inline int web_url_port_or_default(const struct web_url *url) {
    return url->port >= 0 ? url->port : web_url_default_port(url);
}
/* The URL Standard's "includes credentials": a username or a password. */
static inline bool web_url_includes_credentials(const struct web_url *url) {
    return url->mark[WEB_URL_USERNAME_END] > url->mark[WEB_URL_USERNAME_START] ||
           url->mark[WEB_URL_PASSWORD_END] > url->mark[WEB_URL_PASSWORD_START];
}
/* url's host, serialized (an IPv6 address in brackets, an IPv4 address in dotted decimal, a
   domain in ASCII lowercase with its labels as A-labels). */
static inline const char *web_url_host_text(const struct web_url *url, size_t *length) {
    return web_url_span(url, WEB_URL_HOST_START, WEB_URL_HOST_END, length);
}
/* url's host and, when url has a port, ':' and the port: what HTTP's Host field and HTTP/2's
   :authority carry, never the credentials. */
static inline const char *web_url_authority(const struct web_url *url, size_t *length) {
    return web_url_span(url, WEB_URL_HOST_START, WEB_URL_PORT_END, length);
}
/* url's path and query, without the fragment: an HTTP request's target in origin-form. */
static inline const char *web_url_target(const struct web_url *url, size_t *length) {
    return web_url_span(url, WEB_URL_PATH_START, WEB_URL_FRAGMENT_START, length);
}

/* What a URL's origin is. Each function that answers with it gives one of these. */
enum web_url_result {
    WEB_URL_OOM = -2,       /* out of memory */
    WEB_URL_INVALID = -1,   /* the text is not a URL */
    WEB_URL_OPAQUE = 0,     /* an opaque origin: serialized "null", same origin with nothing */
    WEB_URL_TUPLE = 1       /* a tuple origin */
};
/* The origin of a URL (the URL Standard's origin getter): a tuple (scheme, host, port) for
   ftp, http, https, ws and wss; for blob: the origin of the URL its path parses as, when that
   is an http, https or file URL; opaque for every other scheme, file included (the URL
   Standard leaves file's to the implementation and asks for opaque when in doubt). The
   serialization of a tuple origin (HTML) is scheme "://" host [":" port], malloc'd in *origin
   (NULL for any other result). */
enum web_url_result web_url_origin_of(const struct web_url *url, char **origin);
enum web_url_result web_url_origin(const char *url, char **origin);
/* HTML's same origin for the origins of two URLs: 1 when both are tuple origins and the same,
   0 otherwise (two URLs' opaque origins are never the same one), -1 out of memory. */
int web_url_same_origin(const char *a, const char *b);
/* Whether origin is the serialization of a tuple origin whose scheme is http or https: the key
   of every web storage area (Web Storage, IndexedDB, Cache Storage, OPFS). */
bool web_origin_http(const char *origin);
/* Whether host (length bytes) is the serialization of a host a special URL can have: a domain,
   an IPv4 address or an IPv6 address in brackets, exactly as the URL parser writes it. */
bool web_url_host_serialized(const char *host, size_t length);
/* Secure Contexts' "Is url potentially trustworthy?": about:blank and about:srcdoc, data:,
   file:, https: and wss:, and a URL whose host is a loopback address (127.0.0.0/8 or ::1) or
   exactly "localhost" (Nocturne's resolver keeps that one name on the loopback interface, not
   localhost. or *.localhost). */
bool web_url_potentially_trustworthy(const char *url);
/* The URL Standard's equality of two URLs (their serializations), the fragments left out with
   exclude_fragments: 1 equal, 0 not (or either is not a URL), -1 out of memory. */
int web_url_equal(const char *a, const char *b, bool exclude_fragments);
