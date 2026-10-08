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
bool nmedia_packets_compatible(const nmedia *,const AVFormatContext *);
