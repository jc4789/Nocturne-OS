#pragma once
#include "media.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
/* Browser MSE packet track-buffer, not an FFmpeg protocol or host ABI. */
typedef int (*nmedia_packet_reader)(void *, AVPacket *);
typedef bool (*nmedia_packet_seeker)(void *, int64_t);
nmedia *nmedia_open_packets(const AVFormatContext *, nmedia_packet_reader,
                           nmedia_packet_seeker, void *, char *, size_t);
bool nmedia_packets_resume(nmedia *);
/* After an eviction seek: release decoder high-water pools and borrowed RGB
 * output by reopening the same codec metadata. Provider/cursor/trim survive.
 * Caller must invalidate pending output; a failed reopen is not recoverable
 * by running the old decoder, and is reported through nmedia_error. */
bool nmedia_packets_reclaim(nmedia *);
bool nmedia_packets_compatible(const nmedia *,const AVFormatContext *);

/* Provider release runs once after decoder destruction; retain native input errors. */
void nmedia_packets_set_lifecycle(nmedia *, void (*)(void *), const char *(*)(void *));
