/* The browser's preferences: what the user chose in the browser's Settings window, kept in the
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

/* A language the Settings window offers: a valid BCP 47 tag (RFC 5646, registered subtags) in its
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

/* Each change is written at once: the document is claimed, read again, changed in that one line
   and saved whole (lines this build does not know are kept). 0, -N_ERR_INVALID (not in the
   catalog, empty, a tag twice, does not fit), or the document's failure. */
n_result webprefs_set_languages(const char *const *tags, size_t count);
n_result webprefs_reset_languages(void); /* removes the line: the default applies */
n_result webprefs_set_search_history(bool on);
