#include "media.h"
#include "media_alloc_private.h"
#include "media_http_private.h"
#include "media_feed_private.h"
#include "media_adaptive_private.h"
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"
#include "libavutil/pixfmt.h"
#include "libavutil/dict.h"
#include "libswresample/swresample.h"

struct nmedia {
    AVFormatContext *format;
    AVIOContext *io;
    AVCodecContext *audio, *video, *pending;
    AVPacket *packet;
    AVFrame *frame;
    int fd, audio_index, video_index, flush, eof;
    nmedia_http *http;
    uint8_t *bytes;
    size_t size, position;
    struct nmedia_info info;
    char error[160];
    uint32_t *pixels;
    size_t pixel_capacity;
    int16_t samples[4096 * 2];
    uint64_t phase, step;
    int frame_audio, source_rate;
    AVChannelLayout mix_layout;
    int32_t mix[2][8]; /* Q15, including unity (32768). */
    bool mix_valid;
    nmedia_packet_reader packet_reader;
    nmedia_packet_seeker packet_seeker;
    void *packet_owner;
    void (*packet_release)(void *);
    const char *(*packet_error)(void *);
    int64_t audio_pts, audio_clock, video_clock, audio_seek_ms, video_seek_ms;
};

static int fail(nmedia *m, const char *what, int code) {
    if (m->http && nmedia_http_error(m->http)[0]) {
        snprintf(m->error, sizeof m->error, "HTTP input: %s", nmedia_http_error(m->http));
        return NMEDIA_ERROR;
    }
    if (m->packet_error && m->packet_error(m->packet_owner)[0]) {
        snprintf(m->error, sizeof m->error, "%s", m->packet_error(m->packet_owner));
        return NMEDIA_ERROR;
    }
    char detail[80];
    av_strerror(code, detail, sizeof detail);
    snprintf(m->error, sizeof m->error, "%s: %s", what, detail);
    return NMEDIA_ERROR;
}
static int input_read(void *opaque, uint8_t *out, int count) {
    nmedia *m = opaque;
    if(count<=0)return AVERROR(EINVAL);
    if (m->http) {
        int n = nmedia_http_read(m->http, out, count);
        return n > 0 ? n : n == 0 ? AVERROR_EOF : AVERROR(EIO);
    }
    if (m->bytes) {
        size_t n = MIN((size_t)count, m->size - m->position);
        if (!n) return AVERROR_EOF;
        memcpy(out, m->bytes + m->position, n); m->position += n;
        return (int)n;
    }
    ssize_t n = read(m->fd, out, (size_t)count);
    return n > 0 ? (int)n : n == 0 ? AVERROR_EOF : AVERROR(EIO);
}
static int64_t input_seek(void *opaque, int64_t offset, int whence) {
    nmedia *m = opaque;
    if (whence & AVSEEK_SIZE) return (int64_t)m->size;
    whence &= ~AVSEEK_FORCE;
    if (m->http) {
        int64_t at = nmedia_http_seek(m->http, offset, whence);
        return at < 0 ? AVERROR(EIO) : at;
    }
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ?
        (m->bytes ? (int64_t)m->position : lseek(m->fd, 0, SEEK_CUR)) : whence == SEEK_END ? (int64_t)m->size : -1;
    if (base < 0 || offset < -base || offset > (int64_t)m->size - base) return AVERROR(EINVAL);
    int64_t at = base + offset;
    if (m->bytes) { m->position = (size_t)at; return at; }
    return lseek(m->fd, (long)at, SEEK_SET);
}
static AVCodecContext *open_decoder(nmedia *m, int stream) {
    AVStream *s = m->format->streams[stream];
    const AVCodec *codec = avcodec_find_decoder(s->codecpar->codec_id);
    if (!codec) { fail(m, "unsupported codec", AVERROR_DECODER_NOT_FOUND); return NULL; }
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c) { fail(m, "decoder allocation", AVERROR(ENOMEM)); return NULL; }
    c->thread_count = 1; c->thread_type = 0;
    c->max_pixels = NMEDIA_MAX_PIXELS;
    /* Decoder-side pre-skip/discard adjusts frame PTS in packet time units. */
    c->pkt_timebase = s->time_base;
    int r = avcodec_parameters_to_context(c, s->codecpar);
    if (r >= 0 && c->codec_id == AV_CODEC_ID_VP9 && c->profile != AV_PROFILE_UNKNOWN && c->profile != 0)
        r = AVERROR(ENOSYS);
    if (r >= 0) r = avcodec_open2(c, codec, NULL);
    if (r < 0) { fail(m, "open decoder", r); avcodec_free_context(&c); return NULL; }
    return c;
}
static nmedia *open_input(nmedia *m, char *error, size_t error_size) {
    m->audio_index = m->video_index = -1;
    m->audio_seek_ms = m->video_seek_ms = -1;
    av_max_alloc(64u * 1024u * 1024u);
    uint8_t *buffer = av_malloc(32768);
    m->io = buffer ? avio_alloc_context(buffer, 32768, 0, m, input_read, NULL, input_seek) : NULL;
    if (!m->io) { av_free(buffer); fail(m, "input allocation", AVERROR(ENOMEM)); goto bad; }
    m->format = avformat_alloc_context();
    if (!m->format) { fail(m, "container allocation", AVERROR(ENOMEM)); goto bad; }
    m->format->pb = m->io; m->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    m->format->probesize = 1024 * 1024; m->format->max_analyze_duration = 2000000;
    m->format->max_streams = 8;
    int r = avformat_open_input(&m->format, NULL, NULL, NULL);
    if (r < 0) { fail(m, "container", r); goto bad; }
    AVDictionary *probe_options[8] = {0};
    unsigned probe_streams = m->format->nb_streams;
    if (probe_streams > 8) { fail(m, "stream count", AVERROR(EINVAL)); goto bad; }
    for (unsigned i = 0; i < probe_streams; i++) {
        AVCodecParameters *p = m->format->streams[i]->codecpar;
        if ((p->width > 0 && p->height > 0 && (uint64_t)p->width * p->height > NMEDIA_MAX_PIXELS) || p->sample_rate > 384000 || p->ch_layout.nb_channels > 8) {
            fail(m, "stream resource limits", AVERROR(EINVAL));
            for (unsigned j = 0; j < probe_streams; j++) av_dict_free(&probe_options[j]);
            goto bad;
        }
        if(av_dict_set(&probe_options[i], "max_pixels", "2359296", 0)<0 || av_dict_set(&probe_options[i], "threads", "1", 0)<0){
            fail(m,"probe options allocation",AVERROR(ENOMEM));
            for(unsigned j=0;j<probe_streams;j++)av_dict_free(&probe_options[j]);goto bad;
        }
    }
    r = avformat_find_stream_info(m->format, probe_options);
    for (unsigned i = 0; i < probe_streams; i++) av_dict_free(&probe_options[i]);
    if (r < 0) { fail(m, "stream information", r); goto bad; }
    strlcpy(m->info.container, m->format->iformat->name, sizeof m->info.container);
    m->info.duration_ms = m->format->duration == AV_NOPTS_VALUE ? -1 : m->format->duration / 1000;
    for (unsigned i = 0; i < m->format->nb_streams; i++) {
        AVCodecParameters *p = m->format->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_AUDIO && m->audio_index < 0) m->audio_index = (int)i;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && m->video_index < 0 && !(m->format->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)) m->video_index = (int)i;
    }
    if (m->audio_index >= 0) {
        m->audio = open_decoder(m, m->audio_index); if (!m->audio) goto bad;
        m->info.audio = true; m->info.sample_rate = m->audio->sample_rate;
        m->info.channels = m->audio->ch_layout.nb_channels;
        strlcpy(m->info.audio_codec, avcodec_get_name(m->audio->codec_id), sizeof m->info.audio_codec);
    }
    if (m->video_index >= 0) {
        m->video = open_decoder(m, m->video_index); if (!m->video) goto bad;
        m->info.video = true; m->info.width = m->video->width; m->info.height = m->video->height;
        strlcpy(m->info.video_codec, avcodec_get_name(m->video->codec_id), sizeof m->info.video_codec);
    }
    if (!m->audio && !m->video) { fail(m, "no playable streams", AVERROR_STREAM_NOT_FOUND); goto bad; }
    m->packet = av_packet_alloc(); m->frame = av_frame_alloc();
    if (!m->packet || !m->frame) { fail(m, "frame allocation", AVERROR(ENOMEM)); goto bad; }
    if (error && error_size) *error = 0;
    return m;
bad:
    if (error && error_size) strlcpy(error, m->error, error_size);
    return NULL; /* caller retains input ownership, including seek restart */
}
nmedia *nmedia_open(const char *path, char *error, size_t error_size) {
    nmedia *m = nmedia_ff_mallocz(sizeof *m);
    if (!m) { if (error && error_size) strlcpy(error, "media allocation", error_size); return NULL; }
    m->fd = open(path, O_RDONLY);
    struct n_stat st;
    if (m->fd < 0 || fstat(m->fd, &st) < 0 || st.type != N_FT_FILE || st.size > INT64_MAX) {
        if (error && error_size) strlcpy(error, "cannot open media file", error_size);
        nmedia_close(m); return NULL;
    }
    m->size = (size_t)st.size;
    if (!open_input(m, error, error_size)) { nmedia_close(m); return NULL; }
    return m;
}
nmedia *nmedia_open_memory(const void *bytes, size_t size, char *error, size_t error_size) {
    if (!bytes || !size || size > NMEDIA_MAX_BYTES) {
        if (error && error_size) strlcpy(error, "empty or oversized media input", error_size); return NULL;
    }
    nmedia *m = nmedia_ff_mallocz(sizeof *m);
    if (!m) { if (error && error_size) strlcpy(error, "media allocation", error_size); return NULL; }
    m->fd = -1; m->bytes = nmedia_ff_malloc(size);
    if (!m->bytes) { if (error && error_size) strlcpy(error, "media input allocation", error_size); nmedia_close(m); return NULL; }
    memcpy(m->bytes, bytes, size); m->size = size;
    if (!open_input(m, error, error_size)) { nmedia_close(m); return NULL; }
    return m;
}
static nmedia *open_url(const char *url, const char *document_url, char *error, size_t error_size) {
    /* Native Media Player uses the same decoder, with the manifest URL as
       its anonymous policy origin. Redirects remain rejected so relative
       segment URLs can never be resolved against a stale manifest base. */
    if (nmedia_adaptive_url(url))
        return nmedia_adaptive_open_cors(url,document_url?document_url:url,error,error_size);
    nmedia *m = nmedia_ff_mallocz(sizeof *m);
    if (!m) { if(error&&error_size)strlcpy(error,"media allocation",error_size);return NULL; }
    m->fd = -1;
    m->http = document_url ? nmedia_http_open_cors(url,document_url,error,error_size) :
                            nmedia_http_open(url,error,error_size);
    if (!m->http) { nmedia_close(m);return NULL; }
    m->size = (size_t)nmedia_http_size(m->http);
    if (!open_input(m,error,error_size)) { nmedia_close(m);return NULL; }
    return m;
}
nmedia *nmedia_open_url(const char *url, char *error, size_t error_size) {
    return open_url(url,NULL,error,error_size);
}
nmedia *nmedia_open_url_cors(const char *url, const char *document_url, char *error, size_t error_size) {
    if (!document_url) { if(error&&error_size)strlcpy(error,"native document origin required",error_size);return NULL; }
    return open_url(url,document_url,error,error_size);
}
const struct nmedia_info *nmedia_get_info(const nmedia *m) { return m ? &m->info : NULL; }
const char *nmedia_error(const nmedia *m) { return m ? m->error : "no media"; }
nmedia *nmedia_open_packets(const AVFormatContext *source, nmedia_packet_reader reader,
                           nmedia_packet_seeker seeker, void *owner, char *error, size_t size) {
    nmedia *m = nmedia_ff_mallocz(sizeof *m);
    if (!m) { if(error&&size)strlcpy(error,"packet decoder allocation",size);return NULL; }
    m->fd=-1;m->audio_index=m->video_index=-1;m->audio_seek_ms=m->video_seek_ms=-1;
    m->packet_reader=reader;m->packet_seeker=seeker;m->packet_owner=owner;
    m->format=avformat_alloc_context();
    if(!m->format||!source||!reader||source->nb_streams>8)goto bad;
    for(unsigned i=0;i<source->nb_streams;i++){
        AVStream *s=avformat_new_stream(m->format,NULL);
        if(!s||avcodec_parameters_copy(s->codecpar,source->streams[i]->codecpar)<0)goto bad;
        s->time_base=source->streams[i]->time_base;s->avg_frame_rate=source->streams[i]->avg_frame_rate;
        AVCodecParameters *p=s->codecpar;
        if(s->time_base.num<=0||s->time_base.den<=0||p->width<0||p->height<0||
           (uint64_t)p->width*p->height>NMEDIA_MAX_PIXELS||p->sample_rate>384000||p->ch_layout.nb_channels>8)goto bad;
        if(p->codec_type==AVMEDIA_TYPE_AUDIO&&m->audio_index<0)m->audio_index=(int)i;
        if(p->codec_type==AVMEDIA_TYPE_VIDEO&&m->video_index<0)m->video_index=(int)i;
    }
    if(m->audio_index>=0){m->audio=open_decoder(m,m->audio_index);if(!m->audio)goto bad;
        m->info.audio=true;m->info.sample_rate=m->audio->sample_rate;m->info.channels=m->audio->ch_layout.nb_channels;
        strlcpy(m->info.audio_codec,avcodec_get_name(m->audio->codec_id),32);}
    if(m->video_index>=0){m->video=open_decoder(m,m->video_index);if(!m->video)goto bad;
        m->info.video=true;m->info.width=m->video->width;m->info.height=m->video->height;
        strlcpy(m->info.video_codec,avcodec_get_name(m->video->codec_id),32);}
    if(!m->audio&&!m->video)goto bad;
    m->info.duration_ms=source->duration>0&&source->duration!=AV_NOPTS_VALUE?source->duration/1000:-1;if(source->iformat)strlcpy(m->info.container,source->iformat->name,32);
    m->packet=av_packet_alloc();m->frame=av_frame_alloc();if(!m->packet||!m->frame)goto bad;
    if(error&&size)*error=0;return m;
bad:
    if(error&&size)strlcpy(error,m->error[0]?m->error:"invalid MSE packet decoder metadata",size);
    nmedia_close(m);return NULL;
}
void nmedia_packets_set_lifecycle(nmedia *m, void (*release)(void *), const char *(*error)(void *)) {
    if (!m || !m->packet_reader) return;
    m->packet_release=release; m->packet_error=error;
}
bool nmedia_packets_resume(nmedia *m){
    if(!m||!m->packet_reader)return false;bool flushed=m->eof!=0;
    if(flushed){if(m->audio)avcodec_flush_buffers(m->audio);if(m->video)avcodec_flush_buffers(m->video);m->pending=NULL;m->frame_audio=0;av_frame_unref(m->frame);}
    m->eof=m->flush=0;return flushed;
}
bool nmedia_packets_compatible(const nmedia *m,const AVFormatContext *source){
    if(!m||!source||source->nb_streams!=m->format->nb_streams)return false;
    for(unsigned i=0;i<source->nb_streams;i++){
        const AVStream *a=m->format->streams[i],*b=source->streams[i];
        const AVCodecParameters *p=a->codecpar,*q=b->codecpar;
        if(a->time_base.num!=b->time_base.num||a->time_base.den!=b->time_base.den||p->codec_id!=q->codec_id||p->codec_type!=q->codec_type||
           p->extradata_size!=q->extradata_size||p->sample_rate!=q->sample_rate||av_channel_layout_compare(&p->ch_layout,&q->ch_layout)||
           (p->extradata_size&&memcmp(p->extradata,q->extradata,(size_t)p->extradata_size)))return false;
    }return true;
}
static int32_t audio_sample(AVFrame *f, int index, int channel) {
    enum AVSampleFormat fmt = (enum AVSampleFormat)f->format;
    bool planar = av_sample_fmt_is_planar(fmt);
    int bytes = av_get_bytes_per_sample(fmt), count = f->ch_layout.nb_channels;
    const uint8_t *p = planar ? f->extended_data[channel] + (size_t)index * bytes : f->extended_data[0] + ((size_t)index * count + channel) * bytes;
    fmt = av_get_packed_sample_fmt(fmt);
    int16_t s16; int32_t s32; int64_t s64; float flt; double dbl;
    switch (fmt) {
    case AV_SAMPLE_FMT_U8: return ((int32_t)*p - 128) * 256;
    case AV_SAMPLE_FMT_S16: memcpy(&s16, p, 2); return s16;
    case AV_SAMPLE_FMT_S32: memcpy(&s32, p, 4); return s32 >> 16;
    case AV_SAMPLE_FMT_S64: memcpy(&s64, p, 8); return (int32_t)(s64 >> 48);
    case AV_SAMPLE_FMT_FLT: memcpy(&flt, p, 4); dbl = flt; break;
    case AV_SAMPLE_FMT_DBL: memcpy(&dbl, p, 8); break;
    default: return 0;
    }
    if (!isfinite(dbl)) return 0;
    return (int32_t)(fmax(-1, fmin(1, dbl)) * 32767);
}
/* Retain the bounded native resampler, but use FFmpeg's channel-layout matrix
 * rather than silently discarding every channel after the first two. */
static int audio_mix(nmedia *m, const AVChannelLayout *layout) {
    if (m->mix_valid && !av_channel_layout_compare(&m->mix_layout, layout)) return 0;
    AVChannelLayout simple = {0};
    const AVChannelLayout *input = layout;
    if (layout->order == AV_CHANNEL_ORDER_UNSPEC && layout->nb_channels <= 2) {
        /* Mono/stereo PCM often has no speaker mask. Never guess a surround
         * order from the channel count alone. */
        av_channel_layout_default(&simple, layout->nb_channels); input = &simple;
    }
    if (!av_channel_layout_check(input) || input->order == AV_CHANNEL_ORDER_UNSPEC ||
        input->order == AV_CHANNEL_ORDER_AMBISONIC)
        return fail(m, "unsupported audio channel layout", AVERROR(ENOSYS));
    if (input->nb_channels == 1) {
        if (av_channel_layout_channel_from_index(input, 0) != AV_CHAN_FRONT_CENTER)
            return fail(m, "unsupported mono speaker layout", AVERROR(ENOSYS));
        memset(m->mix, 0, sizeof m->mix); m->mix[0][0] = m->mix[1][0] = 32768;
    } else {
        /* FFmpeg 9.0.2's public matrix utility normalizes a full 64x64 region,
         * not just the active two rows. Allocate its complete bounded span on
         * the heap; a 2x8 caller array would be overwritten. */
        double *matrix = nmedia_ff_mallocz(64 * 64 * sizeof *matrix);
        if (!matrix) return fail(m, "audio mixing matrix allocation", AVERROR(ENOMEM));
        const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        int r = swr_build_matrix2(input, &stereo, 0.7071067811865476, 0.7071067811865476,
                                 0.5, 1.0, 1.0, matrix, 64, AV_MATRIX_ENCODING_NONE, NULL);
        if (r >= 0) for (int ch = 0; ch < input->nb_channels; ch++) {
            /* The matrix utility may leave an unsupported named speaker
             * unmixed. Do not report successful playback with missing audio. */
            if (!isfinite(matrix[ch]) || !isfinite(matrix[64+ch]) ||
                (matrix[ch] == 0 && matrix[64+ch] == 0)) { r = AVERROR(ENOSYS); break; }
        }
        if (r >= 0) for (int side = 0; side < 2; side++) for (int ch = 0; ch < 8; ch++)
            m->mix[side][ch] = (int32_t)(matrix[64*side+ch] * 32768);
        nmedia_ff_free(matrix);
        if (r < 0) return fail(m, "unsupported audio mixing layout", r);
    }
    av_channel_layout_uninit(&m->mix_layout); m->mix_valid = false;
    int r = av_channel_layout_copy(&m->mix_layout, layout);
    if (r < 0) return fail(m, "audio channel layout allocation", r);
    m->mix_valid = true; return 0;
}
static int audio_output(nmedia *m, struct nmedia_output *o) {
    AVFrame *f = m->frame;
    int n = 0;
    uint64_t end = (uint64_t)f->nb_samples << 32;
    uint64_t start = m->phase;
    while (n < 4096 && m->phase < end) {
        int i = (int)(m->phase >> 32), j = MIN(i + 1, f->nb_samples - 1);
        int64_t frac = (int64_t)((m->phase & UINT32_MAX) >> 16);
        int64_t mixed[2] = {0};
        for (int ch = 0; ch < f->ch_layout.nb_channels; ch++) {
            int32_t a = audio_sample(f, i, ch), b = audio_sample(f, j, ch);
            int32_t value = (int32_t)(a + (((int64_t)b - a) * frac >> 16));
            for (int side = 0; side < 2; side++) mixed[side] += (int64_t)value * m->mix[side][ch];
        }
        for (int side = 0; side < 2; side++) {
            int64_t value = mixed[side] >> 15;
            m->samples[2*n+side] = (int16_t)MAX(-32768, MIN(32767, value));
        }
        n++; m->phase += m->step;
    }
    o->kind = NMEDIA_AUDIO; o->samples = m->samples; o->frames = (size_t)n;
    o->pts_ms = m->audio_pts + (int64_t)((start >> 16) * 1000 / ((uint64_t)m->source_rate << 16));
    if (m->phase >= end) { m->phase -= end; m->frame_audio = 0; av_frame_unref(f); }
    return o->kind;
}
static unsigned clamp8(int n) { return (unsigned)(n < 0 ? 0 : n > 255 ? 255 : n); }
static int video_output(nmedia *m, struct nmedia_output *o) {
    AVFrame *f = m->frame; int w = f->width, h = f->height;
    if (w <= 0 || h <= 0 || (uint64_t)w * h > NMEDIA_MAX_PIXELS) return fail(m, "video dimensions", AVERROR(EINVAL));
    size_t count = (size_t)w * h;
    if (count > m->pixel_capacity) {
        uint32_t *p = nmedia_ff_realloc(m->pixels, count * 4);
        if (!p) return fail(m, "video output allocation", AVERROR(ENOMEM));
        m->pixels = p; m->pixel_capacity = count;
    }
    enum AVPixelFormat format = (enum AVPixelFormat)f->format;
    /* Upstream also compiles 10/12-bit VP9 DSP, but that is not a promise
     * that our native ARGB output can render those profiles or HDR. */
    if (m->video->codec_id == AV_CODEC_ID_VP9 &&
        (m->video->profile != 0 || format != AV_PIX_FMT_YUV420P ||
         (f->colorspace != AVCOL_SPC_UNSPECIFIED && f->colorspace != AVCOL_SPC_BT709 &&
          f->colorspace != AVCOL_SPC_BT470BG && f->colorspace != AVCOL_SPC_SMPTE170M) ||
         (f->color_trc != AVCOL_TRC_UNSPECIFIED && f->color_trc != AVCOL_TRC_BT709 &&
          f->color_trc != AVCOL_TRC_GAMMA22 && f->color_trc != AVCOL_TRC_GAMMA28 && f->color_trc != AVCOL_TRC_SMPTE170M)))
        return fail(m, "unsupported VP9 output profile/color", AVERROR(ENOSYS));
    bool yuv = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUV422P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUV444P || format == AV_PIX_FMT_YUVJ444P;
    bool rgb = format == AV_PIX_FMT_RGB24 || format == AV_PIX_FMT_BGR24 || format == AV_PIX_FMT_BGRA || format == AV_PIX_FMT_RGBA || format == AV_PIX_FMT_BGR0 || format == AV_PIX_FMT_RGB0;
    if (!yuv && !rgb && format != AV_PIX_FMT_GRAY8) return fail(m, "unsupported decoded pixel format", AVERROR(ENOSYS));
    bool full = f->color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P;
    bool bt709 = f->colorspace == AVCOL_SPC_BT709;
    int sx = format == AV_PIX_FMT_YUV444P || format == AV_PIX_FMT_YUVJ444P ? 0 : 1;
    int sy = format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P ? 1 : 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int r, g, b;
        if (yuv) {
            int yy = f->data[0][(ptrdiff_t)y * f->linesize[0] + x];
            int u = f->data[1][(ptrdiff_t)(y >> sy) * f->linesize[1] + (x >> sx)] - 128;
            int v = f->data[2][(ptrdiff_t)(y >> sy) * f->linesize[2] + (x >> sx)] - 128;
            int c = full ? 256 * yy : 298 * MAX(yy - 16, 0);
            r = (c + (full ? bt709 ? 403 : 359 : bt709 ? 459 : 409) * v + 128) >> 8;
            g = (c - (full ? bt709 ? 48 : 88 : bt709 ? 55 : 100) * u - (full ? bt709 ? 120 : 183 : bt709 ? 136 : 208) * v + 128) >> 8;
            b = (c + (full ? bt709 ? 475 : 454 : bt709 ? 541 : 516) * u + 128) >> 8;
        } else if (format == AV_PIX_FMT_GRAY8) r = g = b = f->data[0][(ptrdiff_t)y * f->linesize[0] + x];
        else {
            int bytes = format == AV_PIX_FMT_RGB24 || format == AV_PIX_FMT_BGR24 ? 3 : 4;
            const uint8_t *p = f->data[0] + (ptrdiff_t)y * f->linesize[0] + x * bytes;
            bool bgr = format == AV_PIX_FMT_BGR24 || format == AV_PIX_FMT_BGRA || format == AV_PIX_FMT_BGR0;
            r = p[bgr ? 2 : 0]; g = p[1]; b = p[bgr ? 0 : 2];
        }
        m->pixels[(size_t)y * w + x] = 0xff000000u | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b);
    }
    m->info.width = w; m->info.height = h;
    o->kind = NMEDIA_VIDEO; o->pixels = m->pixels; o->width = w; o->height = h;
    AVStream *s = m->format->streams[m->video_index];
    int64_t stamp = f->best_effort_timestamp;
    o->pts_ms = stamp == AV_NOPTS_VALUE ? m->video_clock : av_rescale_q(stamp, s->time_base, (AVRational){1,1000});
    AVRational rate = s->avg_frame_rate.num ? s->avg_frame_rate : (AVRational){25,1};
    m->video_clock = o->pts_ms + av_rescale_q(1, av_inv_q(rate), (AVRational){1,1000});
    av_frame_unref(f); return o->kind;
}
int nmedia_step(nmedia *m, struct nmedia_output *o) {
    if (!m || !o) return NMEDIA_ERROR;
    memset(o, 0, sizeof *o);
    if (m->error[0]) return NMEDIA_ERROR;
    if (m->frame_audio) return audio_output(m, o);
    for (int budget = 0; budget < 64; budget++) {
        if (m->pending) {
            int r = avcodec_receive_frame(m->pending, m->frame);
            if (r >= 0) {
                if (m->pending == m->video) {
                    int result = video_output(m, o);
                    if (result == NMEDIA_VIDEO && m->video_seek_ms >= 0 && o->pts_ms < m->video_seek_ms) continue;
                    if (result == NMEDIA_VIDEO) m->video_seek_ms = -1;
                    return result;
                }
                AVFrame *f = m->frame;
                if (f->sample_rate < 1000 || f->sample_rate > 384000 || f->ch_layout.nb_channels < 1 || f->ch_layout.nb_channels > 8 || f->nb_samples <= 0 || f->nb_samples > 65536 || av_get_bytes_per_sample((enum AVSampleFormat)f->format) <= 0)
                    return fail(m, "audio frame limits", AVERROR(EINVAL));
                if (audio_mix(m, &f->ch_layout) < 0) return NMEDIA_ERROR;
                AVStream *s = m->format->streams[m->audio_index];
                int64_t stamp = f->best_effort_timestamp;
                m->audio_pts = stamp == AV_NOPTS_VALUE ? m->audio_clock : av_rescale_q(stamp, s->time_base, (AVRational){1,1000});
                m->audio_clock = m->audio_pts + (int64_t)f->nb_samples * 1000 / f->sample_rate;
                if (m->source_rate != f->sample_rate) m->phase = 0;
                m->source_rate = f->sample_rate; m->step = ((uint64_t)f->sample_rate << 32) / SOUND_RATE;
                if (m->audio_seek_ms >= 0 && m->audio_clock <= m->audio_seek_ms) { av_frame_unref(f); continue; }
                /* -1 means no seek. Negative pre-roll PTS must not turn that
                 * sentinel into a second trim (Opus -7ms -> lost 288 frames). */
                if (m->audio_seek_ms >= 0 && m->audio_seek_ms > m->audio_pts) m->phase = (uint64_t)(m->audio_seek_ms - m->audio_pts) * f->sample_rate / 1000 << 32;
                m->audio_seek_ms = -1; m->frame_audio = 1;
                return audio_output(m, o);
            }
            if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) return fail(m, "decode", r);
            m->pending = NULL;
        }
        if (m->eof) {
            AVCodecContext *c = m->flush == 0 ? m->audio : m->flush == 1 ? m->video : NULL;
            if (m->flush >= 2) { o->kind = NMEDIA_END; return NMEDIA_END; }
            m->flush++;
            if (c) { int r = avcodec_send_packet(c, NULL); if (r < 0 && r != AVERROR_EOF) return fail(m, "flush", r); m->pending = c; }
            continue;
        }
        int r = m->packet_reader ? m->packet_reader(m->packet_owner,m->packet) : av_read_frame(m->format, m->packet);
        if(r==AVERROR(EAGAIN))return NMEDIA_AGAIN;
        if (r == AVERROR_EOF) { m->eof = 1; continue; }
        if (r < 0) return fail(m, "read packet", r);
        if (m->packet->size > 16 * 1024 * 1024) { av_packet_unref(m->packet); return fail(m,"packet resource limit",AVERROR(EINVAL)); }
        AVCodecContext *c = m->packet->stream_index == m->audio_index ? m->audio : m->packet->stream_index == m->video_index ? m->video : NULL;
        if (c) { r = avcodec_send_packet(c, m->packet); m->pending = c; }
        av_packet_unref(m->packet);
        if (c && r < 0) return fail(m, "send packet", r);
    }
    return NMEDIA_AGAIN;
}
static void close_decoder(nmedia *m) {
    av_channel_layout_uninit(&m->mix_layout); m->mix_valid = false;
    av_frame_free(&m->frame); av_packet_free(&m->packet);
    avcodec_free_context(&m->audio); avcodec_free_context(&m->video);
    if(m->packet_reader){avformat_free_context(m->format);m->format=NULL;}
    else avformat_close_input(&m->format);
    if (m->io) { av_freep(&m->io->buffer); avio_context_free(&m->io); }
    m->pending = NULL;
}
/* Short raw FLAC without a SEEKTABLE can fail FFmpeg's binary timestamp
 * search after EOF. Recreate only the demux/decoder, retaining native input
 * ownership. Subsequent bounded steps discard samples up to the target.
 * This fallback is linear in the prefix, not a claim of fast indexed seek. */
static bool restart_flac(nmedia *m) {
    close_decoder(m);
    if (input_seek(m, 0, SEEK_SET) < 0) { fail(m,"rewind FLAC",AVERROR(EIO)); return false; }
    memset(&m->info, 0, sizeof m->info);
    m->eof = m->flush = m->frame_audio = m->source_rate = 0;
    m->phase = m->step = 0; m->audio_pts = m->audio_clock = m->video_clock = 0;
    m->error[0] = 0;
    if (!open_input(m, NULL, 0)) { close_decoder(m); return false; }
    return true;
}
bool nmedia_seek(nmedia *m, int64_t ms) {
    if (!m || !m->format || ms < 0 || ms > INT64_MAX / 1000) return false;
    int r = m->packet_reader ? (m->packet_seeker&&m->packet_seeker(m->packet_owner,ms)?0:AVERROR(EINVAL)) :
        avformat_seek_file(m->format, -1, INT64_MIN, ms * 1000, ms * 1000, 0);
    if (r < 0) {
        if (strcmp(m->info.container,"flac") || !restart_flac(m)) return false;
        /* Fallback begins at zero; preserve zero clocks for missing PTS. */
    } else {
        if (m->audio) avcodec_flush_buffers(m->audio);
        if (m->video) avcodec_flush_buffers(m->video);
        av_frame_unref(m->frame); av_packet_unref(m->packet);
        m->audio_clock = m->video_clock = ms;
    }
    m->pending = NULL; m->eof = m->flush = m->frame_audio = 0;
    m->phase = 0; m->audio_seek_ms = m->video_seek_ms = ms; m->error[0] = 0;
    return true;
}
/* Eviction already invalidates output and seeks to a retained keyframe.
 * FFmpeg flush returns DPB frames to a high-water pool; it does not free that
 * pool. Close/reopen only codecs, preserving the packet provider, metadata,
 * selected cursor and seek trim. No original input/lifecycle is released. */
bool nmedia_packets_reclaim(nmedia *m) {
    if (!m || !m->packet_reader) return false;
    m->pending = NULL; m->frame_audio = 0;
    av_frame_unref(m->frame); av_packet_unref(m->packet);
    avcodec_free_context(&m->audio); avcodec_free_context(&m->video);
    av_channel_layout_uninit(&m->mix_layout); m->mix_valid = false;
    nmedia_ff_free(m->pixels); m->pixels = NULL; m->pixel_capacity = 0;
    m->eof = m->flush = m->source_rate = 0; m->phase = m->step = 0;
    if (m->audio_index >= 0) {
        m->audio = open_decoder(m, m->audio_index);
        if (!m->audio) goto bad;
    }
    if (m->video_index >= 0) {
        m->video = open_decoder(m, m->video_index);
        if (!m->video) goto bad;
    }
    return true;
bad:
    avcodec_free_context(&m->audio); avcodec_free_context(&m->video);
    return false; /* Reopen failure is explicit; no stale decoder can run. */
}
void nmedia_close(nmedia *m) {
    if (!m) return;
    close_decoder(m);
    if (m->packet_release) m->packet_release(m->packet_owner);
    if (m->fd >= 0) close(m->fd);
    nmedia_http_close(m->http);
    nmedia_ff_free(m->bytes); nmedia_ff_free(m->pixels); nmedia_ff_free(m);
}
