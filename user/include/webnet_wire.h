/* Private browser/child framing. No pointers or compiler-dependent bool fields on the wire. */
#pragma once
#include "webnet.h"
#include "webcookie.h"
#define WEBNET_MAGIC 0x574e4554u
struct webnet_wire_request {
    uint32_t magic, kind, user_navigation, credentials;
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
