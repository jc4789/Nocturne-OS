#ifndef NOCTURNE_MEDIA_MSE_PARSER_H
#define NOCTURNE_MEDIA_MSE_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* No I/O, FFmpeg state, or mutation of bytes. Demand-sized temporary traversal
 * and run arrays use the media allocator and are released on every return. All offsets are
 * relative to bytes. 1 publishes a complete initialization/media span,
 * NMEDIA_MSE_PARSE_METADATA publishes a segment_end span of WebM metadata only
 * (Cues, Chapters, Tags, SeekHead, Void after the last Cluster at EOS) that
 * the caller consumes without demuxing, as engines skip those elements,
 * 0 needs more bytes, -1 is a permanent append error, and
 * NMEDIA_MSE_PARSE_MEMORY is an actual allocation/metadata size failure. An init-only result
 * has init_end > 0 and segment_end == 0; otherwise segment_end includes any
 * initialization/prefix bytes preceding the first complete media segment.
 * The caller retains init bytes separately and removes only the published
 * span. On later calls have_init=true and bytes starts at a media prefix.
 * This is framing/clear-content validation, not codec or timeline validation.
 * Unknown-size WebM Clusters wait for the next Cluster/init header or EOS.
 * The caller owns staging allocation and checks its size_t/AVIO arithmetic. */
#define NMEDIA_MSE_PARSE_MEMORY (-2)
#define NMEDIA_MSE_PARSE_METADATA 2
struct nmedia_mse_track_size {uint32_t track_id,sample_size,sample_duration;};
struct nmedia_mse_sample_time {size_t offset;uint32_t track_id,duration;};
/* Sorted immutable trex defaults extracted once per validated init. The caller
 * owns the media-allocator array. Missing defaults retain conservative framing. */
int nmedia_mse_init_sizes(const uint8_t *,size_t,struct nmedia_mse_track_size **,
                         size_t *,char *,size_t);
/* Sorted by encoded-byte position and track ID. Durations are in the MP4
   track's timebase units, before conversion into the browser's timeline.
   Caller owns the finite media-allocator array and releases it after demux. */
int nmedia_mse_fragment_times(const uint8_t *,size_t,const struct nmedia_mse_track_size *,size_t,
                             struct nmedia_mse_sample_time **,size_t *,char *,size_t);
int nmedia_mse_boundary_sizes(const uint8_t *,size_t,bool,bool,bool,size_t *,size_t *,
                             char *,size_t,const struct nmedia_mse_track_size *,size_t);
int nmedia_mse_boundary(const uint8_t *bytes, size_t length, bool webm,
                        bool have_init, bool eos, size_t *init_end,
                        size_t *segment_end, char *error, size_t error_size);
/* Explicit initialization duration in milliseconds, or -1 when absent,
 * unknown, invalid, or outside the supported 1e12 ms timeline. Fragmented
 * MP4 mehd overrides mvhd; WebM Duration uses TimestampScale (default 1 ms).
 * Never infer a finite duration from the currently buffered fragment. */
int64_t nmedia_mse_init_duration(const uint8_t *bytes, size_t length, bool webm);

#endif
