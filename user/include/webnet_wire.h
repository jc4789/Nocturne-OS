/* Private browser/child framing. No pointers or compiler-dependent bool fields on the wire. */
#pragma once
#include "webnet.h"
#include "webcookie.h"
#define WEBNET_MAGIC 0x574e4554u
/* Preserve the 64-byte request layout used by persisted statically-linked apps.
   Legacy user_navigation 0/1 remains valid; new flags use reserved bits. */
#define WEBNET_WIRE_USER_NAVIGATION 1u
#define WEBNET_WIRE_FORCE_PREFLIGHT 2u
#define WEBNET_WIRE_REDIRECT_ERROR 4u
#define WEBNET_WIRE_SAME_ORIGIN 8u
/* New clients request the genuine HTTP status line as a header-block prefix.
   Old persisted clients keep the same response framing and header-only block. */
#define WEBNET_WIRE_STATUS_LINE 16u
/* Opt in to the larger response bound without changing either wire layout.
   Without this bit, a worker must reject output >= the legacy 8192-byte bound
   rather than silently shorten fields or surprise an old persisted client. */
#define WEBNET_WIRE_LARGE_HEADERS 32u
/* Cache policy is metadata, not an author header: generated cache headers must
   not change CORS preflight classification. Zero keeps legacy default policy. */
#define WEBNET_WIRE_CACHE_SHIFT 6u
#define WEBNET_WIRE_CACHE_MASK (7u << WEBNET_WIRE_CACHE_SHIFT)
#define WEBNET_WIRE_CACHE_MODE(flags) (((flags) & WEBNET_WIRE_CACHE_MASK) >> WEBNET_WIRE_CACHE_SHIFT)
/* New clients may receive up to 32 MiB only for classic/module scripts. An old
   persisted client omitting this flag still receives the legacy 16 MiB bound. */
#define WEBNET_WIRE_LARGE_SCRIPT 512u
/* New clients use the actual 32-bit body-length representation. Preserve
   legacy response ceilings only for persisted clients that did not opt in. */
#define WEBNET_WIRE_DYNAMIC_BODY 1024u
/* Response header storage grows on demand, up to the uint32 wire length.
   Persisted clients without this flag keep their old complete-block bounds. */
#define WEBNET_WIRE_DYNAMIC_HEADERS 2048u
/* New writers opt in to heap-backed request fields. No layout/offset change;
   an old worker rejects this unknown bit instead of truncating the request. */
#define WEBNET_WIRE_DYNAMIC_REQUEST_HEADERS 4096u
/* Trusted destination/initiator metadata, never an author request header.
   RESOURCE only; old workers reject the unknown bit and never load HTTP. */
#define WEBNET_WIRE_IMAGE_UPGRADE 8192u
/* URL/origin/final URL storage is owned and uses the existing uint32 lengths.
   Unknown-bit rejection in old workers prevents prefix/truncation fallback. */
#define WEBNET_WIRE_DYNAMIC_URL 16384u
#define WEBNET_WIRE_REQUEST_FLAGS (WEBNET_WIRE_USER_NAVIGATION | WEBNET_WIRE_FORCE_PREFLIGHT | WEBNET_WIRE_REDIRECT_ERROR | WEBNET_WIRE_SAME_ORIGIN | WEBNET_WIRE_STATUS_LINE | WEBNET_WIRE_LARGE_HEADERS | WEBNET_WIRE_CACHE_MASK | WEBNET_WIRE_LARGE_SCRIPT | WEBNET_WIRE_DYNAMIC_BODY | WEBNET_WIRE_DYNAMIC_HEADERS | WEBNET_WIRE_DYNAMIC_REQUEST_HEADERS | WEBNET_WIRE_IMAGE_UPGRADE | WEBNET_WIRE_DYNAMIC_URL)
static inline size_t webnet_wire_url_limit(uint32_t flags) {
    return flags & WEBNET_WIRE_DYNAMIC_URL ? (size_t)UINT32_MAX : WEBNET_URL_MAX - 1u;
}
static inline bool webnet_wire_image_flag_valid(uint32_t kind, uint32_t flags) {
    return !(flags & WEBNET_WIRE_IMAGE_UPGRADE) || kind == WEBNET_RESOURCE;
}
static inline size_t webnet_wire_request_header_limit(uint32_t flags) {
    return flags & WEBNET_WIRE_DYNAMIC_REQUEST_HEADERS ? (size_t)UINT32_MAX : WEBNET_REQUEST_HEADERS_MAX - 1u;
}
static inline size_t webnet_wire_header_limit(uint32_t flags) {
    return flags & WEBNET_WIRE_DYNAMIC_HEADERS ? (size_t)UINT32_MAX :
        flags & WEBNET_WIRE_LARGE_HEADERS ? WEBNET_RESPONSE_HEADERS_MAX - 1u : WEBNET_REQUEST_HEADERS_MAX - 1u;
}
static inline bool webnet_wire_script_flag_valid(uint32_t kind, uint32_t flags) {
    return !(flags & WEBNET_WIRE_LARGE_SCRIPT) || kind == WEBNET_CLASSIC || kind == WEBNET_MODULE;
}
static inline size_t webnet_wire_response_limit(uint32_t kind, uint32_t flags) {
    (void)kind;
    return flags & WEBNET_WIRE_DYNAMIC_BODY ? WEBNET_BODY_LIMIT :
        flags & WEBNET_WIRE_LARGE_SCRIPT ? (32u * 1024u * 1024u) : (16u * 1024u * 1024u);
}
struct webnet_wire_request {
    uint32_t magic, kind, user_navigation, credentials; /* user_navigation is a flags word. */
    uint64_t id, generation, deadline;
    uint32_t url_len, origin_len, method_len, headers_len, body_len, cookie_len;
};
struct webnet_wire_response {
    uint32_t magic, status;
    uint64_t id, generation;
    uint32_t url_len, headers_len, body_len, error_len, cookie_len;
};
static inline bool webnet_wire_request_payload_size(const struct webnet_wire_request *r, size_t *out) {
    uint64_t total = (uint64_t)r->url_len + r->origin_len + r->method_len + r->headers_len + r->body_len + r->cookie_len;
    /* The worker terminates each of the six separately allocated fields. */
    if (total > SIZE_MAX - 6) return false;
    *out = (size_t)total; return true;
}
static inline bool webnet_wire_payload_size(const struct webnet_wire_response *r, size_t *out) {
    uint64_t total = (uint64_t)r->url_len + r->headers_len + r->body_len + r->error_len + r->cookie_len;
    /* Four native strings add NULs when unpacked; raw framing adds none. */
    if (total > SIZE_MAX - 4) return false;
    *out = (size_t)total;
    return true;
}
/* Privileged updates, not JS-visible response headers. Never combine Set-Cookie
   fields. Each record is followed by url_len URL bytes and value_len field bytes. */
#define WEBNET_COOKIE_EVENTS_MAX (256u * 1024u)
static inline size_t webnet_wire_cookie_limit(uint32_t flags) {
    return flags & WEBNET_WIRE_DYNAMIC_URL ? (size_t)UINT32_MAX : WEBNET_COOKIE_EVENTS_MAX;
}
struct webnet_wire_cookie {
    uint32_t url_len, value_len, redirect_cross_site, reserved;
    int64_t received;
};
_Static_assert(sizeof(struct webnet_wire_request)==64,"webfetch request framing");
_Static_assert(sizeof(struct webnet_wire_response)==48,"webfetch response framing");
