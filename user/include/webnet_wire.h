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
#define WEBNET_WIRE_REQUEST_FLAGS (WEBNET_WIRE_USER_NAVIGATION | WEBNET_WIRE_FORCE_PREFLIGHT | WEBNET_WIRE_REDIRECT_ERROR | WEBNET_WIRE_SAME_ORIGIN | WEBNET_WIRE_STATUS_LINE | WEBNET_WIRE_LARGE_HEADERS | WEBNET_WIRE_CACHE_MASK | WEBNET_WIRE_LARGE_SCRIPT)
static inline bool webnet_wire_script_flag_valid(uint32_t kind, uint32_t flags) {
    return !(flags & WEBNET_WIRE_LARGE_SCRIPT) || kind == WEBNET_CLASSIC || kind == WEBNET_MODULE;
}
static inline size_t webnet_wire_response_limit(uint32_t kind, uint32_t flags) {
    return flags & WEBNET_WIRE_LARGE_SCRIPT ? webnet_response_limit((enum webnet_kind)kind) : WEBNET_BODY_LIMIT;
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
/* Privileged updates, not JS-visible response headers. Never combine Set-Cookie
   fields. Each record is followed by url_len URL bytes and value_len field bytes. */
#define WEBNET_COOKIE_EVENTS_MAX (256u * 1024u)
struct webnet_wire_cookie {
    uint32_t url_len, value_len, redirect_cross_site, reserved;
    int64_t received;
};
