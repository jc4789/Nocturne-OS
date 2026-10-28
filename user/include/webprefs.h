/* The browser's preferences: what the user chose on the browser's settings page, kept in the
   document "preferences" of the browser's profile folder (web_profile_folder, webstorage.h). Every
   browser process reads them here and hears of a change through a watch on that document in the
   queue its loop waits on; only the functions here write the document. They are the browser's,
   not the system's: no other program reads or writes them.

   The document: UTF-8 lines, LF, at most 64 KiB; its first line "Nocturne browser preferences 1",
   then "name=value" lines, name [a-z0-9-]+. A first line other than that: every preference has
   its default. A line this build does not know is kept when the document is written again; a
   known name with a bad value has its default. No document: every default, and reading writes
   nothing. */
#pragma once
#include <nocturne.h>

#define WEBPREFS_TOKEN (UINT64_C(0x4c) << 56) /* the watch's token in a process's queue */
#define WEBPREFS_ACCEPT_LANGUAGE_MAX 128u    /* a CORS-safelisted value's bytes (Fetch) */
/* The languages while the document has no languages line: Japanese, then Chinese (the user's
   choice, 2026-10-28). Catalog tags, commas between. */
#define WEBPREFS_DEFAULT_LANGUAGES "ja,zh"

/* A language the settings page offers: a valid BCP 47 tag (RFC 5646, registered subtags) in its
   usual case (§2.1.1), its own name, its name in English. The list holds only these. */
struct webprefs_language {
    const char *tag, *own, *english;
};
const struct webprefs_language *webprefs_catalog(size_t *count);

/* Watches the document in queue under WEBPREFS_TOKEN, then reads it (the watch first, so no
   change between is missed). Without a watch, the first use reads the document once. */
n_result webprefs_watch(n_handle queue);
/* The events of a wait: those with WEBPREFS_TOKEN take the watch's changes and read the document
   again; the others are left alone. */
void webprefs_events(const struct n_event *events, size_t count);
/* Takes a change that waits now, without the loop: for a decision (privacy) that must not act on
   a value another process has already changed. */
void webprefs_sync(void);

/* Grows each time this process's list of languages changes. */
uint64_t webprefs_languages_generation(void);
/* The languages, most wanted first, tags between commas ("ja,en-US"), never empty, with a zero:
   the length, or -N_ERR_TOO_LARGE. The same list navigator.languages and Accept-Language use
   (HTML). */
n_result webprefs_languages(char *out, size_t cap);
/* Whether the list of languages is the default one: no languages line, or one naming the same tags in the same order. */
bool webprefs_languages_default(void);
/* The Accept-Language of a fetch that begins now: the first tag bare, then each with ";q=0.9"
   falling by a tenth to ";q=0.1"; at most WEBPREFS_ACCEPT_LANGUAGE_MAX bytes of [0-9A-Za-z,;=.-],
   with a zero. The length, or -N_ERR_TOO_LARGE. */
n_result webprefs_accept_language(char *out, size_t cap);
/* Whether tags (catalog tags, each once) make a list whose Accept-Language fits. */
bool webprefs_languages_fit(const char *const *tags, size_t count);
/* Whether the user turned the search history on (off until then). */
bool webprefs_search_history(void);

/* Cookies and site data. A site's data is its cookies (webnet.c) and its storage (localStorage,
   IndexedDB, OPFS). The user's exceptions name hosts: one covers the host and its subdomains, and
   of several the most specific (longest) applies; an IP address or [IPv6] only itself. Lines:
   "clear-site-data-on-close=on|off" (off by default) and "site-data-exceptions=" items
   "rule:host" (rule allow, session or block) between spaces. */
enum webprefs_site_rule {
    WEBPREFS_SITE_DEFAULT, /* no exception: kept, unless the browser clears site data when it closes */
    WEBPREFS_SITE_ALLOW,   /* kept, even when the browser clears site data when it closes */
    WEBPREFS_SITE_SESSION, /* cleared when the browser closes */
    WEBPREFS_SITE_BLOCK,   /* no cookie set or sent (RFC 6265bis §7.2); storage refused, as the
                              Storage Standard's "the user has disabled storage" (SecurityError) */
};
#define WEBPREFS_HOST_MAX 253u
struct webprefs_site_exception {
    char host[WEBPREFS_HOST_MAX + 1]; /* lowercase ASCII (A-labels), no port */
    enum webprefs_site_rule rule;
};
/* The rule for a host (ASCII, any case, a final dot ignored); DEFAULT for none. */
enum webprefs_site_rule webprefs_site_rule(const char *host);
/* The rule for the host of an http(s) or ws(s) URL (or origin), as the URL parser reads it;
   DEFAULT for any other URL; BLOCK when there is no memory to read it. */
enum webprefs_site_rule webprefs_site_rule_url(const char *url);
/* Whether a document or worker of url may use storage and cookies: not when its host is blocked.
   (A URL without a host is the storage owners' own refusal, opaque origins.) */
bool webprefs_storage_allowed(const char *url);
/* Whether the data of host outlives the browser session: ALLOW, or DEFAULT and BLOCK while the
   browser does not clear site data when it closes. */
bool webprefs_site_kept(const char *host);
/* Whether closing the browser clears anything: the choice is on, or an exception is SESSION. */
bool webprefs_site_data_transient(void);
bool webprefs_clear_on_close(void);
/* The cookie jar's rule (webcookie_set_rule's webcookie_rule; opaque unused): WEBCOOKIE_BLOCK for
   a blocked host, WEBCOOKIE_TRANSIENT for a host whose data is not kept. */
unsigned webprefs_cookie_rule(void *opaque, const char *host);
/* Grows each time this process's exceptions or the choice to clear on close change. */
uint64_t webprefs_site_generation(void);
/* The exceptions, sorted by host: at most cap of them in out, and how many there are. */
size_t webprefs_site_exceptions(struct webprefs_site_exception *out, size_t cap);
/* The host of what the user typed: a URL ("https://www.example.com:8080/a" gives www.example.com)
   or a host ("Example.COM." gives example.com, "*.example.com" or ".example.com" example.com), in
   out. 0, or -N_ERR_INVALID (no host of ASCII letters, digits, '-' and '.', IPv4 or [IPv6]:
   a Unicode name must come here as its A-labels). */
n_result webprefs_site_host(const char *input, char out[WEBPREFS_HOST_MAX + 1]);

/* Each change is written at once: the document is claimed, read again, changed in that one line
   and saved whole (lines this build does not know are kept). 0, -N_ERR_INVALID (not in the
   catalog, empty, a tag twice, does not fit), or the document's failure. */
n_result webprefs_set_languages(const char *const *tags, size_t count);
n_result webprefs_reset_languages(void); /* removes the line: the default applies */
n_result webprefs_set_search_history(bool on);
n_result webprefs_set_clear_on_close(bool on);
/* host's exception (webprefs_site_host's form, -N_ERR_INVALID otherwise); DEFAULT removes it. The
   change is made to the exceptions the document holds when it is claimed, so another process's
   change made meanwhile stays. -N_ERR_TOO_LARGE when the document would pass 64 KiB. */
n_result webprefs_set_site_rule(const char *host, enum webprefs_site_rule rule);
