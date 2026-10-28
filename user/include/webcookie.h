/* The browser's cookie jar (RFC 6265bis). No kernel, filesystem, or JS dependency: the window
   process's jar is kept in the profile by webnet.c, a worker's jar lives for one request. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define WEBCOOKIE_FIELD_MAX 8192u
#define WEBCOOKIE_OUTPUT_MAX 32768u
/* How many cookies the jar holds (RFC 6265bis §6.1 asks for at least 50 per domain and 3000 in
   all; §5.7 evicts past an implementation's bound). 180 per domain field is what Firefox and
   Chrome keep for one domain; 3000 in all is §6.1's figure. Past either, §5.7's order evicts:
   per domain the non-Secure least recently used first, in all the least recently used. */
#define WEBCOOKIE_DOMAIN_MAX 180u
#define WEBCOOKIE_TOTAL_MAX 3000u
/* One record of a snapshot or of the saved store at its largest: 40 bytes of fields, the name
   and value (4096 together, §5.7 step 4), the domain (253) and the path (1024). */
#define WEBCOOKIE_RECORD_MAX (40u + 4096u + 253u + 1024u)
/* The largest snapshot a full jar makes: the transport to a worker never refuses a jar. */
#define WEBCOOKIE_SNAPSHOT_MAX (24u + WEBCOOKIE_TOTAL_MAX * WEBCOOKIE_RECORD_MAX)
#define WEBCOOKIE_PSL_MAX (2u * 1024u * 1024u)
/* What the saved store keeps of the clears it records for other processes (webcookie_clear). */
#define WEBCOOKIE_CLEARS_KEPT 64u
/* The largest store a full jar saves: a 48-byte header, the clears (10 bytes and a site each) and
   the records. A larger document is no store this build wrote. */
#define WEBCOOKIE_STORE_MAX (48u + WEBCOOKIE_CLEARS_KEPT * (10u + 253u) + WEBCOOKIE_TOTAL_MAX * WEBCOOKIE_RECORD_MAX)

typedef struct webcookie_jar webcookie_jar;
struct webcookie_context {
    const char *url;
    /* Trusted top-level site-for-cookies, never a page-supplied Origin header.
       NULL is no client (address-bar navigation); "" is an opaque client. */
    const char *site_url;
    const char *method;
    bool top_level;
    bool http; /* false for document.cookie: HttpOnly is never exposed */
    bool redirect_cross_site;
};

webcookie_jar *webcookie_create(void);
void webcookie_free(webcookie_jar *);
/* Load trusted complete ASCII/A-label PSL, including PRIVATE rules. Unicode
   rules must be converted on the host. Failed loads keep the prior policy.
   Without a PSL, Domain cookies cannot widen scope and sites are exact-host. */
bool webcookie_psl_load(webcookie_jar *, const char *text, size_t len);
/* HTML's same site for the origins of the URLs a and b (both tuple origins, the same scheme,
   the same registrable domain or, without one, the same host), the PSL being this jar's. The
   one owner of same site: Fetch Metadata, SameSite and the redirect taint all ask it. */
bool webcookie_same_site(const webcookie_jar *, const char *a, const char *b);
/* The site (registrable domain, PSL) of a host as the URL parser writes it: a suffix of host
   itself; host when it is an IP address, a public suffix, or there is no PSL. */
const char *webcookie_site_of(const webcookie_jar *, const char *host);

/* The user's choice for a host (RFC 6265bis §7.2 cookie policy, §7.3 user controls): the jar asks
   it for the host of each URL it sets or sends cookies for (BLOCK: none set, none sent, §5.3 and
   §5.8.1) and for the domain of each persistent cookie it would save (TRANSIENT: the cookie lives
   only until the session ends, as §7.3 has it when persistent storage is prevented). */
enum { WEBCOOKIE_ALLOW = 0, WEBCOOKIE_BLOCK = 1, WEBCOOKIE_TRANSIENT = 2 };
typedef unsigned (*webcookie_rule)(void *opaque, const char *host);
void webcookie_set_rule(webcookie_jar *, webcookie_rule rule, void *opaque);

/* One Set-Cookie field VALUE (not a combined header). 1 accepted (including
   deletion), 0 rejected by policy/syntax, -1 allocation/size failure. now: UTC s. */
int webcookie_set(webcookie_jar *, const struct webcookie_context *, const char *value, size_t len, int64_t now);
/* Cookie value only (no "Cookie:" or CRLF). Empty is valid. out=NULL queries
   length (excluding NUL). -1 on invalid URL or insufficient capacity; output
   is cleared, never partially serialized. */
long webcookie_get(webcookie_jar *, const struct webcookie_context *, char *out, size_t cap, int64_t now);
/* Privileged browser->worker transport ONLY: includes HttpOnly secrets.
   No pointers/padding/native-endian fields. out=NULL queries required bytes.
   Import is atomic and only permitted into an empty jar. Never merge a worker
   snapshot into the browser jar; merge new response events instead. */
long webcookie_export(webcookie_jar *, void *out, size_t cap, int64_t now);
bool webcookie_import(webcookie_jar *, const void *data, size_t len, int64_t now);

/* The saved store, which every browser process shares: the persistent cookies of the hosts the
   rule keeps (never a session cookie, RFC 6265bis §5.7: they end with the session), and the last
   WEBCOOKIE_CLEARS_KEPT clears. A jar that tracks (only the window process's) records each change
   to what it would save until webcookie_saved, so a store another process wrote meanwhile is
   merged under its changes rather than over them. */
void webcookie_track(webcookie_jar *);
/* Merges a saved store (len 0: none) into the jar: its persistent cookies replace the ones the
   jar had saved, the jar's unsaved changes apply on top, its session and transient cookies stay
   unless the store has the same cookie; clears it recorded since the jar last looked remove the
   jar's own matching cookies too. 1; 0 for a malformed store (what this build cannot read:
   saving replaces it) and -1 for no memory, the jar unchanged in both. Records the policy
   refuses are left out one by one. */
int webcookie_load(webcookie_jar *, const void *data, size_t len, int64_t now);
/* The store as the jar would save it now: call webcookie_load with the document as it is first,
   under the document's claim. out=NULL measures; -1 too small or not tracking. */
long webcookie_save(webcookie_jar *, void *out, size_t cap, int64_t now);
/* After webcookie_save's bytes were written whole: they are what the store holds. */
void webcookie_saved(webcookie_jar *);
/* Whether the jar has changes the store does not hold yet. */
bool webcookie_changed(const webcookie_jar *);
/* After the rule's answers changed: cookies it now keeps are saved, those it no longer keeps
   leave the store (they stay in the jar until the session ends). */
void webcookie_rule_changed(webcookie_jar *);
/* The end of the session (RFC 6265bis §5.7): removes the session cookies and the transient ones. */
void webcookie_session_end(webcookie_jar *);

/* What the jar holds of each site (webcookie_site_of each cookie's domain), sorted by site: a
   malloc'd array (free it) in *out and its length, or -1 for no memory. bytes: name, value,
   domain and path. */
struct webcookie_site {
    char site[254];
    size_t cookies;
    uint64_t bytes;
};
long webcookie_sites(webcookie_jar *, struct webcookie_site **out, int64_t now);
/* Removes the cookies of site and of every subdomain of it (NULL or "": every cookie), session
   ones too, and records the clear for the store: the count removed. */
size_t webcookie_clear(webcookie_jar *, const char *site);
