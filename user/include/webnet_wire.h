/* Private browser/child framing. No pointers or compiler-dependent bool fields on the wire. */
#pragma once
#include "webnet.h"
#include "webcookie.h"
#define WEBNET_MAGIC 0x574e4554u
/* Preserve the 64-byte request layout used by persisted statically-linked apps.
   Legacy user_navigation 0/1 remains valid; new flags use reserved bits. */
#define WEBNET_WIRE_USER_NAVIGATION 1u
#define WEBNET_WIRE_FORCE_PREFLIGHT 2u
#define WEBNET_WIRE_REQUEST_FLAGS (WEBNET_WIRE_USER_NAVIGATION | WEBNET_WIRE_FORCE_PREFLIGHT)
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
