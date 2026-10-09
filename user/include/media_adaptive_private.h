#pragma once
#include "media.h"
#include "http.h"
#include <stdint.h>
#define NMEDIA_ADAPTIVE_MANIFEST_BYTES (256u * 1024u)
#define NMEDIA_ADAPTIVE_SEGMENT_BYTES (8u * 1024u * 1024u)
#define NMEDIA_ADAPTIVE_INIT_BYTES (1024u * 1024u)
#define NMEDIA_ADAPTIVE_SEGMENTS 2048u
struct nmedia_adaptive_segment {
    const char *uri;
    int64_t start_us, duration_us, range_start, range_bytes;
};
struct nmedia_adaptive_track {
    const char *base, *init;
    int64_t init_start, init_bytes, presentation_offset_us;
    unsigned count;
    bool mp4;
    struct nmedia_adaptive_segment segments[NMEDIA_ADAPTIVE_SEGMENTS];
};
struct nmedia_adaptive_owned;
struct nmedia_adaptive_manifest {
    struct nmedia_adaptive_track tracks[2];
    unsigned count;
    bool dash;
    int64_t duration_us;
    const char *variant;
    struct nmedia_adaptive_owned *owned;
    bool allocation_failed;
};
/* Parse into a fresh (or cleared) output. Strings are manifest-owned and
 * survive input text release; inherited plans borrow from that same owner.
 * Failure clears every allocation. Clear before replacing a successful plan. */
void nmedia_adaptive_manifest_clear(struct nmedia_adaptive_manifest *);
/* Successful exact URL belongs to nmedia_ff_free; failure leaves NULL. */
enum http_url_result nmedia_adaptive_resolve_owned(const char *base, const char *uri, char **out);
/* Compatibility output-span wrapper, not an internal URL size ceiling. */
bool nmedia_adaptive_resolve(const char *base, const char *uri, char *out, size_t capacity);
int nmedia_adaptive_parse_hls(char *text, size_t bytes, const char *base,
                             struct nmedia_adaptive_manifest *, char *error, size_t capacity);
bool nmedia_adaptive_parse_dash(char *text, size_t bytes, const char *base,
                                struct nmedia_adaptive_manifest *, char *error, size_t capacity);
bool nmedia_adaptive_url(const char *url);
/* Anonymous native document policy on manifest AND every initialization/media
 * segment. Bounded clear VOD only; fetch/demux belongs to isolated media child.
 * Owns all packet provider state until ordinary nmedia_close(). */
nmedia *nmedia_adaptive_open_cors(const char *url, const char *native_document,
                                 char *error, size_t capacity);

