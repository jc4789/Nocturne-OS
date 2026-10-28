/* The URL Standard's algorithms for the browser: the WHATWG URL parser, the URL
   interface's setters and the application/x-www-form-urlencoded format. The URL record
   and what every native layer reads from it are weburl.h's.
   https://url.spec.whatwg.org/ */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <weburl.h>
#include "webi.h"
#include "web_encoding.h"

/* The URL parser for the engine's native callers, on any thread: input
   against base (NULL: none; a base that is not a URL is none), with encoding
   for input's query. HTML's encoding-parse a URL gives its document's
   character encoding (doc_encoding_parse_url); CSS's url(), Fetch, a worker's
   own URLs and every other caller UTF-8, as web_resolve_url_owned (web.h)
   parses. A lone surrogate's three bytes (the DOM's and QuickJS's strings
   keep them) parse as U+FFFD, a USVString's conversion. 1: *out is the URL's
   serialization, malloc'd; 0: not a URL (*out NULL); -1: out of memory. */
int web_url_resolve(const char *base, const char *input, web_encoding encoding, char **out);

/* Lexbor's URL parser and the scratch memory it parses in. Every call leaves
   the scratch memory empty again (the last base parsed is kept apart, to be
   used again for the same base); one thread uses a parser at a time. */
struct web_url_parser;
struct web_url_parser *web_url_parser_new(void);
void web_url_parser_free(struct web_url_parser *parser);

/* Parses input against base, which is parsed first when it is not NULL. Both
   are the UTF-8 of scalar values. encoding is the URL parser's for input's
   query: HTML's encoding-parse gives the document's, everything else UTF-8
   (WEB_ENCODING_UTF8 or failure); the parser itself keeps UTF-8 for a URL that
   is not special or is ws: or wss:. url is given empty ({0}). 1: url holds the
   result (web_url_free it); 0: input or base is not a URL; -1: out of memory. */
int web_url_parse(struct web_url_parser *parser, const char *input, size_t length,
                  const char *base, size_t base_length, web_encoding encoding, struct web_url *url);
/* Whether input alone (no base) is a URL. */
bool web_url_valid(struct web_url_parser *parser, const char *input, size_t length);

/* The URL that href, a URL's serialization, becomes when part's setter is
   given value; a value the setter turns down leaves it as it was. encoding is
   the search setter's for the query: Location's is its document's, the URL
   interface's and HTMLHyperlinkElementUtils' UTF-8. 1 or -1 as for parsing. */
int web_url_set(struct web_url_parser *parser, const char *href, size_t href_length,
                enum web_url_part part, const char *value, size_t length, web_encoding encoding,
                struct web_url *url);

/* application/x-www-form-urlencoded parsing: each name and value, '+' as a
   space and percent-decoded, still to be UTF-8 decoded. 1 when every pair has
   been given, 0 when pair returned false, -1 when out of memory. */
int web_url_form_parse(const char *input, size_t length,
                       bool (*pair)(void *opaque, const char *name, size_t name_length,
                                    const char *value, size_t value_length),
                       void *opaque);
/* The urlencoded serializer's percent-encoding of one name or value already
   in the form's encoding: each byte outside the application/x-www-form-urlencoded
   percent-encode set as it is, a space as '+', every other byte as %XX. UTF-8
   bytes go in as they are; for another encoding, web_encode_html's bytes:
   percent-encode after encoding's %26%23N%3B for an unencodable scalar value is
   exactly html mode's &#N; percent-encoded (ISO-2022-JP's ESC ( B comes first
   in both). */
void url_encode_form(sbuf *out, const char *bytes, size_t length);
