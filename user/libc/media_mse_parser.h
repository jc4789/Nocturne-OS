#ifndef NOCTURNE_MEDIA_MSE_PARSER_H
#define NOCTURNE_MEDIA_MSE_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* No allocations, I/O, FFmpeg state, or mutation of bytes. All offsets are
 * relative to bytes. 1 publishes a complete initialization/media span,
 * 0 needs more bytes, and -1 is a permanent append error. An init-only result
 * has init_end > 0 and segment_end == 0; otherwise segment_end includes any
 * initialization/prefix bytes preceding the first complete media segment.
 * The caller retains init bytes separately and removes only the published
 * span. On later calls have_init=true and bytes starts at a media prefix.
 * This is framing/clear-content validation, not codec or timeline validation.
 * Unknown-size WebM Clusters wait for the next Cluster/init header or EOS.
 * The caller must impose the same 32 MiB staging limit while waiting. */
int nmedia_mse_boundary(const uint8_t *bytes, size_t length, bool webm,
                        bool have_init, bool eos, size_t *init_end,
                        size_t *segment_end, char *error, size_t error_size);
/* Explicit initialization duration in milliseconds, or -1 when absent,
 * unknown, invalid, or outside the supported 1e12 ms timeline. Fragmented
 * MP4 mehd overrides mvhd; WebM Duration uses TimestampScale (default 1 ms).
 * Never infer a finite duration from the currently buffered fragment. */
int64_t nmedia_mse_init_duration(const uint8_t *bytes, size_t length, bool webm);

#endif
