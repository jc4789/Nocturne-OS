/* Fuzzes user/libc/media_mse_parser.c: MP4/WebM framing, trex/trun sample sizes, durations. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../../user/libc/media_mse_parser.h"
static long mse_live;
void nmedia_ff_free(void *p);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    unsigned f = data[0]; size_t n = size - 1;
    bool webm = f & 1, have_init = f & 2, eos = f & 4;
    uint8_t *b = malloc(n); memcpy(b, data + 1, n);
    char err[128];
    size_t init_end = 0, segment_end = 0;
    nmedia_mse_boundary(b, n, webm, have_init, eos, &init_end, &segment_end, err, sizeof err);
    if (init_end > n || segment_end > n) __builtin_trap(); /* published spans must lie in the input */
    nmedia_mse_init_duration(b, n, webm);
    struct nmedia_mse_track_size *sizes = NULL; size_t sc = 0;
    if (nmedia_mse_init_sizes(b, n, &sizes, &sc, err, sizeof err) < 0) { sizes = NULL; sc = 0; }
    size_t ie = 0, se = 0;
    nmedia_mse_boundary_sizes(b, n, webm, have_init, eos, &ie, &se, err, sizeof err, sizes, sc);
    if (ie > n || se > n) __builtin_trap();
    struct nmedia_mse_sample_time *times = NULL; size_t tc = 0;
    if (nmedia_mse_fragment_times(b, n, sizes, sc, &times, &tc, err, sizeof err) >= 0) {
        for (size_t i = 0; i < tc; i++) if (times[i].offset > n) __builtin_trap();
        nmedia_ff_free(times);
    }
    nmedia_ff_free(sizes);
    free(b);
    if (mse_live) __builtin_trap(); /* leaked parser metadata */
    return 0;
}

/* The media allocator is a thin accounting layer over the C heap; the harness uses the host heap directly. */
/* Live-block count lets the harness catch leaks (no LeakSanitizer on Windows). */
void *nmedia_ff_malloc(size_t n) { void *p = malloc(n); mse_live += p != NULL; return p; }
void *nmedia_ff_realloc(void *p, size_t n) { void *q = realloc(p, n); if (!p && q) mse_live++; return q; }
void nmedia_ff_free(void *p) { if (p) mse_live--; free(p); }
