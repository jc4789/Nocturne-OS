/* The URL Standard's algorithms for the browser: the WHATWG URL parser, the URL
   interface's setters and the application/x-www-form-urlencoded format.
   https://url.spec.whatwg.org/ */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "webi.h"

/* Lexbor's URL parser and the scratch memory it parses in. Every call leaves
   the scratch memory empty again; one thread uses a parser at a time. */
struct web_url_parser;
struct web_url_parser *web_url_parser_new(void);
void web_url_parser_free(struct web_url_parser *parser);

/* A URL as its serialization (always ASCII) and where each component lies in
   it. A component the URL lacks is an empty span where it would be. */
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
struct web_url {
    sbuf href;
    size_t mark[WEB_URL_MARKS];
};

/* Parses input against base, which is parsed first when it is not NULL. Both
   are the UTF-8 of scalar values. 1: url holds the result (sb_free its href);
   0: input or base is not a URL; -1: out of memory. */
int web_url_parse(struct web_url_parser *parser, const char *input, size_t length,
                  const char *base, size_t base_length, struct web_url *url);
/* Whether input alone (no base) is a URL. */
bool web_url_valid(struct web_url_parser *parser, const char *input, size_t length);

/* The URL interface's setters, in its order. */
enum web_url_part {
    WEB_URL_PROTOCOL, WEB_URL_USERNAME, WEB_URL_PASSWORD, WEB_URL_HOST,
    WEB_URL_HOSTNAME, WEB_URL_PORT, WEB_URL_PATHNAME, WEB_URL_SEARCH, WEB_URL_HASH
};
/* The URL that href, a URL's serialization, becomes when part's setter is
   given value; a value the setter turns down leaves it as it was. 1 or -1 as
   for parsing. */
int web_url_set(struct web_url_parser *parser, const char *href, size_t href_length,
                enum web_url_part part, const char *value, size_t length, struct web_url *url);

/* application/x-www-form-urlencoded parsing: each name and value, '+' as a
   space and percent-decoded, still to be UTF-8 decoded. 1 when every pair has
   been given, 0 when pair returned false, -1 when out of memory. */
int web_url_form_parse(const char *input, size_t length,
                       bool (*pair)(void *opaque, const char *name, size_t name_length,
                                    const char *value, size_t value_length),
                       void *opaque);
/* The urlencoded serializer's encoding of one name or value (UTF-8). */
void url_encode_form(sbuf *out, const char *text, size_t length);
