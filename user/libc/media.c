#include "media.h"
#include "media_video_private.h"
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
#include "libavutil/pixdesc.h"
#include "libavutil/dict.h"
#include "libswresample/swresample.h"
#include <emmintrin.h>

struct nmedia {
    AVFormatContext *format;
    AVIOContext *io;
    AVCodecContext *audio, *video, *pending;
    AVPacket *packet;
    AVFrame *frame, *video_late;
    bool video_late_set; int64_t video_late_pts;
    int fd, audio_index, video_index, flush, eof;
    nmedia_http *http;
    uint8_t *bytes;
    size_t size, position;
    struct nmedia_info info;
    char error[160];
    uint32_t *pixels;
    size_t pixel_capacity;
    bool video_borrowed;
    int64_t video_loan_pts;
    int64_t video_present_ms;bool video_present_set;
    uint64_t video_decoded,video_dropped;
    uint64_t video_measured_frames,video_work_ms,video_decode_work,video_late_work;
    int video_measure_width,video_measure_height,video_measure_format,video_measure_space,video_measure_range;
    uint64_t video_measurement_id;
    struct nmedia_video_configuration video_configuration;
    struct nmedia_video_configuration video_late_configuration;
    struct nmedia_audio_observation audio_observation;
    uint64_t audio_decode_work;
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
    /* Decoder-side pre-skip/discard adjusts frame PTS in packet time units. */
    c->pkt_timebase = s->time_base;
    int r = avcodec_parameters_to_context(c, s->codecpar);
    if (r >= 0 && c->codec_id == AV_CODEC_ID_VP9 && c->profile != AV_PROFILE_UNKNOWN && (c->profile < 0 || c->profile > 3))
        r = AVERROR(ENOSYS);
    if (r >= 0) r = avcodec_open2(c, codec, NULL);
    if (r < 0) { fail(m, "open decoder", r); avcodec_free_context(&c); return NULL; }
    return c;
}
static void release_probe_options(AVDictionary **options,unsigned count){
    if(options)for(unsigned i=0;i<count;i++)av_dict_free(&options[i]);
    nmedia_ff_free(options);
}
static nmedia *open_input(nmedia *m, char *error, size_t error_size) {
    AVDictionary **probe_options=NULL;unsigned probe_streams=0;
    m->audio_index = m->video_index = -1;
    m->audio_seek_ms = m->video_seek_ms = -1;
    /* libavutil retains its own ptrdiff_t/size arithmetic representation. */
    av_max_alloc(SIZE_MAX);
    uint8_t *buffer = av_malloc(32768);
    m->io = buffer ? avio_alloc_context(buffer, 32768, 0, m, input_read, NULL, input_seek) : NULL;
    if (!m->io) { av_free(buffer); fail(m, "input allocation", AVERROR(ENOMEM)); goto bad; }
    m->format = avformat_alloc_context();
    if (!m->format) { fail(m, "container allocation", AVERROR(ENOMEM)); goto bad; }
    m->format->pb = m->io; m->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    m->format->probesize = 1024 * 1024; m->format->max_analyze_duration = 2000000;
    m->format->max_streams = INT_MAX;
    int r = avformat_open_input(&m->format, NULL, NULL, NULL);
    if (r < 0) { fail(m, "container", r); goto bad; }
    probe_streams = m->format->nb_streams;
    if(probe_streams>(unsigned)INT_MAX||(size_t)probe_streams>SIZE_MAX/sizeof *probe_options){fail(m,"stream count representation",AVERROR(EINVAL));goto bad;}
    if(probe_streams){
        probe_options=nmedia_ff_mallocz((size_t)probe_streams*sizeof *probe_options);
        if(!probe_options){fail(m,"probe options allocation",AVERROR(ENOMEM));goto bad;}
    }
    for (unsigned i = 0; i < probe_streams; i++) {
        AVCodecParameters *p = m->format->streams[i]->codecpar;
        if (!nmedia_video_dimensions(p->width,p->height) || p->sample_rate > 384000 || p->ch_layout.nb_channels > 8) {
            fail(m, "stream resource limits", AVERROR(EINVAL));
            goto bad;
        }
        if(av_dict_set(&probe_options[i], "threads", "1", 0)<0){
            fail(m,"probe options allocation",AVERROR(ENOMEM));
            goto bad;
        }
    }
    r = avformat_find_stream_info(m->format, probe_options);
    release_probe_options(probe_options,probe_streams);probe_options=NULL;probe_streams=0;
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
    release_probe_options(probe_options,probe_streams);
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
    if (!bytes || !size || size > INT64_MAX) {
        if (error && error_size) strlcpy(error, "empty or unrepresentable media input", error_size); return NULL;
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
    if(!m->format){fail(m,"packet container allocation",AVERROR(ENOMEM));goto bad;}
    if(!source||!reader||source->nb_streams>(unsigned)INT_MAX)goto bad;
    m->format->max_streams=INT_MAX;
    for(unsigned i=0;i<source->nb_streams;i++){
        AVStream *s=avformat_new_stream(m->format,NULL);
        if(!s||avcodec_parameters_copy(s->codecpar,source->streams[i]->codecpar)<0)goto bad;
        s->time_base=source->streams[i]->time_base;s->avg_frame_rate=source->streams[i]->avg_frame_rate;
        AVCodecParameters *p=s->codecpar;
        if(s->time_base.num<=0||s->time_base.den<=0||!nmedia_video_dimensions(p->width,p->height)||p->sample_rate>384000||p->ch_layout.nb_channels>8)goto bad;
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
static uint64_t video_work_add(uint64_t,uint64_t);
static uint64_t video_work_elapsed(uint64_t);
static uint64_t video_measurement_next(void);
static int audio_output(nmedia *m, struct nmedia_output *o) {
    AVFrame *f = m->frame;
    uint32_t codec=m->audio->codec_id==AV_CODEC_ID_OPUS?NMEDIA_ACODEC_OPUS:m->audio->codec_id==AV_CODEC_ID_VORBIS?NMEDIA_ACODEC_VORBIS:
        m->audio->codec_id==AV_CODEC_ID_MP3?NMEDIA_ACODEC_MP3:m->audio->codec_id==AV_CODEC_ID_AAC?NMEDIA_ACODEC_AAC:NMEDIA_ACODEC_NONE;
    uint32_t profile=codec==NMEDIA_ACODEC_AAC?(uint32_t)m->audio->profile:0;
    if(!m->audio_observation.id||m->audio_observation.codec!=codec||m->audio_observation.profile!=profile||
       m->audio_observation.channels!=(uint32_t)f->ch_layout.nb_channels||m->audio_observation.rate!=(uint32_t)f->sample_rate)
        m->audio_observation=(struct nmedia_audio_observation){.id=video_measurement_next(),.codec=codec,.profile=profile,.channels=(uint32_t)f->ch_layout.nb_channels,.rate=(uint32_t)f->sample_rate};
    uint64_t work_start=uptime_ms();
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
    o->duration_ms=(int64_t)n*1000/SOUND_RATE;
    o->pts_ms = m->audio_pts + (int64_t)((start >> 16) * 1000 / ((uint64_t)m->source_rate << 16));
    if(n){m->audio_observation.frames=video_work_add(m->audio_observation.frames,(uint64_t)n);
        m->audio_observation.work_ms=video_work_add(m->audio_observation.work_ms,video_work_add(m->audio_decode_work,video_work_elapsed(work_start)));
        m->audio_decode_work=0;o->audio_observation=m->audio_observation;}
    if (m->phase >= end) { m->phase -= end; m->frame_audio = 0; av_frame_unref(f); }
    return o->kind;
}
static unsigned clamp8(int n) { return (unsigned)(n < 0 ? 0 : n > 255 ? 255 : n); }
struct video_matrix {int yc,rv,gu,gv,bu;};
static bool video_matrix(enum AVColorSpace space,bool full,struct video_matrix *m){
    if(space==AVCOL_SPC_BT709)*m=full?(struct video_matrix){256,403,48,120,475}:(struct video_matrix){298,459,55,136,541};
    else if(space==AVCOL_SPC_FCC)*m=full?(struct video_matrix){256,358,85,182,456}:(struct video_matrix){298,408,97,207,519};
    else if(space==AVCOL_SPC_SMPTE240M)*m=full?(struct video_matrix){256,403,58,122,467}:(struct video_matrix){298,459,66,139,532};
    else if(space==AVCOL_SPC_UNSPECIFIED||space==AVCOL_SPC_BT470BG||space==AVCOL_SPC_SMPTE170M)
        *m=full?(struct video_matrix){256,359,88,183,454}:(struct video_matrix){298,409,100,208,516};
    else return false;
    return true;
}
static __m128i video_mul4(__m128i value,int coefficient,bool high){
    __m128i zero=_mm_setzero_si128();
    return _mm_madd_epi16(high?_mm_unpackhi_epi16(value,zero):_mm_unpacklo_epi16(value,zero),_mm_set1_epi32(coefficient));
}
static __m128i video_channel(__m128i y,__m128i u,__m128i v,int yc,int uc,int vc,int depth){
    __m128i lo=_mm_add_epi32(video_mul4(y,yc,false),_mm_add_epi32(video_mul4(u,uc,false),video_mul4(v,vc,false)));
    __m128i hi=_mm_add_epi32(video_mul4(y,yc,true),_mm_add_epi32(video_mul4(u,uc,true),video_mul4(v,vc,true)));
    if(depth==16){
        /* pmaddwd treats input words as signed. Restore Y's unsigned high
           bit without narrowing its precision; centered chroma is signed. */
        __m128i high=_mm_cmpgt_epi16(_mm_setzero_si128(),y),bias=_mm_set1_epi32(yc*65536);
        lo=_mm_add_epi32(lo,_mm_and_si128(_mm_unpacklo_epi16(high,high),bias));
        hi=_mm_add_epi32(hi,_mm_and_si128(_mm_unpackhi_epi16(high,high),bias));
    }
    __m128i rounding=_mm_set1_epi32(1<<(depth-1)),shift=_mm_cvtsi32_si128(depth);
    lo=_mm_sra_epi32(_mm_add_epi32(lo,rounding),shift);
    hi=_mm_sra_epi32(_mm_add_epi32(hi,rounding),shift);
    return _mm_packus_epi16(_mm_packs_epi32(lo,hi),_mm_setzero_si128());
}
/* Baseline x86-64 SSE2, not AVX or an external thread backend. Loads touch
   only complete eight-pixel groups; the existing scalar tail handles width
   and chroma oddities. Signed row strides and all existing color coefficients
   are preserved exactly. */
static int video_yuv_row(AVFrame *f,int row,int w,int sx,int sy,int depth,bool full,const struct video_matrix *matrix,uint32_t *out){
    const uint8_t *yp=f->data[0]+(ptrdiff_t)row*f->linesize[0];
    const uint8_t *up=f->data[1]+(ptrdiff_t)(row>>sy)*f->linesize[1];
    const uint8_t *vp=f->data[2]+(ptrdiff_t)(row>>sy)*f->linesize[2];
    __m128i zero=_mm_setzero_si128(),center=_mm_set1_epi16(1<<(depth-1)),alpha=_mm_set1_epi8(-1);
    int yc=matrix->yc,rv=matrix->rv,gu=matrix->gu,gv=matrix->gv,bu=matrix->bu,x=0;
    for(;w-x>=8;x+=8){
        __m128i y,u,v;
        if(depth>8){
            y=_mm_loadu_si128((const __m128i *)(yp+2*(ptrdiff_t)x));
            if(sx){u=_mm_loadl_epi64((const __m128i *)(up+2*(ptrdiff_t)(x>>sx)));v=_mm_loadl_epi64((const __m128i *)(vp+2*(ptrdiff_t)(x>>sx)));u=_mm_unpacklo_epi16(u,u);v=_mm_unpacklo_epi16(v,v);}
            else {u=_mm_loadu_si128((const __m128i *)(up+2*(ptrdiff_t)x));v=_mm_loadu_si128((const __m128i *)(vp+2*(ptrdiff_t)x));}
        }else if(sx){
            y=_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(yp+x)),zero);
            uint32_t a,b;memcpy(&a,up+(x>>sx),4);memcpy(&b,vp+(x>>sx),4);
            u=_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)a),zero);v=_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)b),zero);
            u=_mm_unpacklo_epi16(u,u);v=_mm_unpacklo_epi16(v,v);
        }else{
            y=_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(yp+x)),zero);
            u=_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(up+x)),zero);
            v=_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(vp+x)),zero);
        }
        u=_mm_sub_epi16(u,center);v=_mm_sub_epi16(v,center);
        if(!full)y=_mm_subs_epu16(y,_mm_set1_epi16(16<<(depth-8)));
        __m128i r=video_channel(y,u,v,yc,0,rv,depth),g=video_channel(y,u,v,yc,-gu,-gv,depth),b=video_channel(y,u,v,yc,bu,0,depth);
        __m128i bg=_mm_unpacklo_epi8(b,g),ra=_mm_unpacklo_epi8(r,alpha);
        _mm_storeu_si128((__m128i *)(out+x),_mm_unpacklo_epi16(bg,ra));
        _mm_storeu_si128((__m128i *)(out+x+4),_mm_unpackhi_epi16(bg,ra));
    }
    for(;x<w;x++){
        int yy,u,v;
        if(depth>8){uint16_t a,b,c;memcpy(&a,yp+2*(ptrdiff_t)x,2);memcpy(&b,up+2*(ptrdiff_t)(x>>sx),2);memcpy(&c,vp+2*(ptrdiff_t)(x>>sx),2);yy=a;u=(int)b-(1<<(depth-1));v=(int)c-(1<<(depth-1));}
        else {yy=yp[x];u=(int)up[x>>sx]-128;v=(int)vp[x>>sx]-128;}
        int c=yc*(full?yy:MAX(yy-(16<<(depth-8)),0)),rounding=1<<(depth-1);
        int r=(c+rv*v+rounding)>>depth,g=(c-gu*u-gv*v+rounding)>>depth,b=(c+bu*u+rounding)>>depth;
        out[x]=0xff000000u|clamp8(r)<<16|clamp8(g)<<8|clamp8(b);
    }
    return x;
}
static bool video_plane_valid(AVFrame *f,unsigned plane,int rows,size_t row_bytes){
    if(!f->data[plane]||rows<=0)return false;
    int64_t stride=f->linesize[plane],size=stride<0?-stride:stride;
    if((uint64_t)size<row_bytes||(uint64_t)size>(uint64_t)PTRDIFF_MAX/(unsigned)rows)return false;
    AVBufferRef *owner=av_frame_get_plane_buffer(f,(int)plane);if(!owner)return false;
    uintptr_t data=(uintptr_t)f->data[plane],base=(uintptr_t)owner->data;
    uint64_t extent=(uint64_t)size*(unsigned)(rows-1);uintptr_t low=data,high=data;
    if(stride<0){if(extent>data)return false;low-=extent;}
    else {if(extent>UINTPTR_MAX-high)return false;high+=(uintptr_t)extent;}
    if(row_bytes>UINTPTR_MAX-high||owner->size>UINTPTR_MAX-base)return false;high+=row_bytes;
    return low>=base&&high<=base+owner->size;
}
static int64_t video_stamp(nmedia *m,AVFrame *f){
    AVStream *stream=m->format->streams[m->video_index];
    int64_t stamp=f->best_effort_timestamp;
    int64_t pts=stamp==AV_NOPTS_VALUE?m->video_clock:av_rescale_q(stamp,stream->time_base,(AVRational){1,1000});
    AVRational rate=stream->avg_frame_rate.num?stream->avg_frame_rate:(AVRational){25,1};
    m->video_clock=pts+av_rescale_q(1,av_inv_q(rate),(AVRational){1,1000});
    return pts;
}
static int video_output(nmedia *m,AVFrame *f,int64_t pts,struct nmedia_output *o) {
    int w = f->width, h = f->height;
    size_t count,bytes;
    if (!nmedia_video_size(w,h,&count,&bytes)) return fail(m, "video dimensions/byte representation", AVERROR(EINVAL));
    enum AVPixelFormat format = (enum AVPixelFormat)f->format;
    /* Native surfaces remain SDR sRGB. Refuse an HDR transfer/BT2020 gamut
       rather than silently rendering it as washed-out 8-bit BT709. */
    if(f->colorspace==AVCOL_SPC_BT2020_NCL||f->colorspace==AVCOL_SPC_BT2020_CL||
       f->color_trc==AVCOL_TRC_SMPTE2084||f->color_trc==AVCOL_TRC_ARIB_STD_B67)
        return fail(m,"HDR requires native tone mapping/display support",AVERROR(ENOSYS));
    int depth=8;
    bool high420=format==AV_PIX_FMT_YUV420P10LE||format==AV_PIX_FMT_YUV420P12LE||format==AV_PIX_FMT_YUV420P16LE;
    bool high422=format==AV_PIX_FMT_YUV422P10LE||format==AV_PIX_FMT_YUV422P12LE||format==AV_PIX_FMT_YUV422P16LE;
    bool high444=format==AV_PIX_FMT_YUV444P10LE||format==AV_PIX_FMT_YUV444P12LE||format==AV_PIX_FMT_YUV444P16LE;
    if(format==AV_PIX_FMT_YUV420P10LE||format==AV_PIX_FMT_YUV422P10LE||format==AV_PIX_FMT_YUV444P10LE)depth=10;
    else if(format==AV_PIX_FMT_YUV420P12LE||format==AV_PIX_FMT_YUV422P12LE||format==AV_PIX_FMT_YUV444P12LE)depth=12;
    else if(high420||high422||high444)depth=16;
    bool yuv = high420||high422||high444||format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUV422P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUV444P || format == AV_PIX_FMT_YUVJ444P;
    bool rgb = format == AV_PIX_FMT_RGB24 || format == AV_PIX_FMT_BGR24 || format == AV_PIX_FMT_BGRA || format == AV_PIX_FMT_RGBA || format == AV_PIX_FMT_BGR0 || format == AV_PIX_FMT_RGB0;
    if (!yuv && !rgb && format != AV_PIX_FMT_GRAY8) return fail(m, "unsupported decoded pixel format", AVERROR(ENOSYS));
    if (count > m->pixel_capacity) {
        uint32_t *p = nmedia_ff_realloc(m->pixels, bytes);
        if (!p) return fail(m, "video output allocation", AVERROR(ENOMEM));
        m->pixels = p; m->pixel_capacity = count;
    }
    bool full = f->color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P || format == AV_PIX_FMT_YUVJ444P;
    struct video_matrix matrix;
    if(yuv&&!video_matrix(f->colorspace,full,&matrix))return fail(m,"unsupported YUV color matrix",AVERROR(ENOSYS));
    int sx = high444||format == AV_PIX_FMT_YUV444P || format == AV_PIX_FMT_YUVJ444P ? 0 : 1;
    int sy = high420||format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P ? 1 : 0;
    unsigned sample_bytes=depth>8?2:1;
    if(yuv){
        size_t cw=((size_t)w+(1u<<sx)-1)>>sx;int ch=(h+(1<<sy)-1)>>sy;
        if(!video_plane_valid(f,0,h,(size_t)w*sample_bytes)||!video_plane_valid(f,1,ch,cw*sample_bytes)||!video_plane_valid(f,2,ch,cw*sample_bytes))
            return fail(m,"invalid decoded YUV plane ownership/stride",AVERROR(EINVAL));
    }else {unsigned packed=format==AV_PIX_FMT_GRAY8?1:format==AV_PIX_FMT_RGB24||format==AV_PIX_FMT_BGR24?3:4;
        if(!video_plane_valid(f,0,h,(size_t)w*packed))return fail(m,"invalid decoded packed plane ownership/stride",AVERROR(EINVAL));}
    for (int y = 0; y < h; y++) {
      int x=yuv?video_yuv_row(f,y,w,sx,sy,depth,full,&matrix,m->pixels+(size_t)y*w):0;
      for (; x < w; x++) {
        int r, g, b;
        if (format == AV_PIX_FMT_GRAY8) r = g = b = f->data[0][(ptrdiff_t)y * f->linesize[0] + x];
        else {
            int bytes = format == AV_PIX_FMT_RGB24 || format == AV_PIX_FMT_BGR24 ? 3 : 4;
            const uint8_t *p = f->data[0] + (ptrdiff_t)y * f->linesize[0] + (ptrdiff_t)x * bytes;
            bool bgr = format == AV_PIX_FMT_BGR24 || format == AV_PIX_FMT_BGRA || format == AV_PIX_FMT_BGR0;
            r = p[bgr ? 2 : 0]; g = p[1]; b = p[bgr ? 0 : 2];
        }
        m->pixels[(size_t)y * w + x] = 0xff000000u | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b);
      }
    }
    m->info.width = w; m->info.height = h;
    o->kind = NMEDIA_VIDEO; o->pixels = m->pixels; o->width = w; o->height = h;
    o->pts_ms=pts;
    AVStream *stream=m->format->streams[m->video_index];
    o->duration_ms=f->duration>0?av_rescale_q(f->duration,stream->time_base,(AVRational){1,1000}):0;
    if(o->duration_ms<=0){AVRational rate=stream->avg_frame_rate;if(rate.num>0&&rate.den>0)o->duration_ms=av_rescale_q(1,av_inv_q(rate),(AVRational){1,1000});}
    av_frame_unref(f); return o->kind;
}
static uint64_t video_work_add(uint64_t a,uint64_t b){return b>UINT64_MAX-a?UINT64_MAX:a+b;}
static uint64_t video_measurement_next(void){static uint64_t serial;return __atomic_add_fetch(&serial,1,__ATOMIC_RELAXED);}
static struct nmedia_video_configuration video_configuration(nmedia *m,AVFrame *f){
    struct nmedia_video_configuration c={.profile=UINT32_MAX,.level=UINT32_MAX,.compatibility=UINT32_MAX};
    const AVPixFmtDescriptor *desc=av_pix_fmt_desc_get(f->format);if(desc)c.depth=desc->comp[0].depth;
    if(m->video->bit_rate>0)c.bitrate=(uint64_t)m->video->bit_rate;
    if(m->video->codec_id==AV_CODEC_ID_H264)c.codec=NMEDIA_VCODEC_AVC;
    else if(m->video->codec_id==AV_CODEC_ID_HEVC)c.codec=NMEDIA_VCODEC_HEVC;
    else if(m->video->codec_id==AV_CODEC_ID_VP8){c.codec=NMEDIA_VCODEC_VP8;c.profile=c.level=c.compatibility=0;}
    else if(m->video->codec_id==AV_CODEC_ID_VP9)c.codec=NMEDIA_VCODEC_VP9;
    if(c.codec!=NMEDIA_VCODEC_VP8){
        if(m->video->profile>=0)c.profile=(uint32_t)m->video->profile&0xffff;
        if(m->video->level>=0)c.level=(uint32_t)m->video->level;
        if(c.codec==NMEDIA_VCODEC_VP9)c.compatibility=c.level=0;
        if(c.codec==NMEDIA_VCODEC_AVC&&m->video->extradata&&m->video->extradata_size>=4&&m->video->extradata[0]==1)
            c.compatibility=m->video->extradata[2];
    }
    return c;
}
static uint64_t video_work_elapsed(uint64_t start){uint64_t now=uptime_ms();return now>=start?video_work_add(now-start,1):UINT64_MAX;}
static int video_receive(nmedia *m,AVCodecContext *c,AVFrame *f){
    uint64_t start=uptime_ms();int result=avcodec_receive_frame(c,f);
    uint64_t *work=c==m->video?&m->video_decode_work:&m->audio_decode_work;
    *work=video_work_add(*work,video_work_elapsed(start));return result;
}
static int video_send(nmedia *m,AVCodecContext *c,const AVPacket *p){
    uint64_t start=uptime_ms();int result=avcodec_send_packet(c,p);
    uint64_t *work=c==m->video?&m->video_decode_work:&m->audio_decode_work;
    *work=video_work_add(*work,video_work_elapsed(start));return result;
}
static int video_return(nmedia *m,AVFrame *f,int64_t pts,uint64_t decode_work,struct nmedia_output *o){
    struct nmedia_video_configuration configuration=f==m->video_late?m->video_late_configuration:video_configuration(m,f);
    /* A different actual output configuration starts a new prediction sample.
       No discarded/preroll frame can manufacture a converted-frame count. */
    if(m->video_measure_width!=f->width||m->video_measure_height!=f->height||m->video_measure_format!=f->format||
       m->video_measure_space!=f->colorspace||m->video_measure_range!=f->color_range||
       !m->video_measurement_id||!nmedia_video_configuration_equal(&m->video_configuration,&configuration)){
        m->video_measured_frames=m->video_work_ms=0;
        m->video_measure_width=f->width;m->video_measure_height=f->height;m->video_measure_format=f->format;
        m->video_measure_space=f->colorspace;m->video_measure_range=f->color_range;
        m->video_configuration=configuration;m->video_measurement_id=video_measurement_next();
    }
    m->video_configuration.bitrate=configuration.bitrate;
    uint64_t start=uptime_ms();
    int result=video_output(m,f,pts,o);
    if(result==NMEDIA_VIDEO){
        m->video_measured_frames=video_work_add(m->video_measured_frames,1);
        m->video_work_ms=video_work_add(m->video_work_ms,video_work_add(decode_work,video_work_elapsed(start)));
        o->video_measured_frames=m->video_measured_frames;o->video_work_ms=m->video_work_ms;
        o->video_measurement_id=m->video_measurement_id;o->video_configuration=m->video_configuration;
        m->video_seek_ms=-1;m->video_borrowed=true;m->video_loan_pts=pts;
    }
    return result;
}
static int video_candidate(nmedia *m,struct nmedia_output *o){
    if(!m->video_late_set)return NMEDIA_AGAIN;
    m->video_late_set=false;return video_return(m,m->video_late,m->video_late_pts,m->video_late_work,o);
}
int nmedia_step(nmedia *m, struct nmedia_output *o) {
    if (!m || !o) return NMEDIA_ERROR;
    m->video_borrowed=false;
    memset(o, 0, sizeof *o);
    if (m->error[0]) return NMEDIA_ERROR;
    if (m->frame_audio) return audio_output(m, o);
    for (int budget = 0; budget < 64; budget++) {
        if (m->pending) {
            int r = video_receive(m,m->pending,m->frame);
            if (r >= 0) {
                if (m->pending == m->video) {
                    m->video_decoded++;
                    uint64_t decode_work=m->video_decode_work;m->video_decode_work=0;
                    int64_t pts=video_stamp(m,m->frame);
                    if(m->video_seek_ms>=0&&pts<m->video_seek_ms){av_frame_unref(m->frame);continue;}
                    if(m->video_present_set&&pts<m->video_present_ms){
                        /* Own one real decoded candidate. Replace it only once
                           a newer real frame exists, never by a clock value. */
                        if(!m->video_late){m->video_late=av_frame_alloc();if(!m->video_late)return fail(m,"video catch-up ownership",AVERROR(ENOMEM));}
                        if(!m->video_late_set||pts>m->video_late_pts){
                            if(m->video_late_set)m->video_dropped++;
                            av_frame_unref(m->video_late);av_frame_move_ref(m->video_late,m->frame);
                            m->video_late_work=video_work_add(decode_work,m->video_late_set?m->video_late_work:0);
                            m->video_late_pts=pts;m->video_late_set=true;
                            m->video_late_configuration=video_configuration(m,m->video_late);
                        }else{m->video_late_work=video_work_add(m->video_late_work,decode_work);m->video_dropped++;av_frame_unref(m->frame);}
                        continue;
                    }
                    if(m->video_late_set){decode_work=video_work_add(decode_work,m->video_late_work);m->video_dropped++;av_frame_unref(m->video_late);m->video_late_set=false;}
                    return video_return(m,m->frame,pts,decode_work,o);
                }
                AVFrame *f = m->frame;
                if (f->sample_rate < 1000 || f->sample_rate > 384000 || f->ch_layout.nb_channels < 1 || f->ch_layout.nb_channels > 8 || f->nb_samples <= 0 || f->nb_samples > 65536 || av_get_bytes_per_sample((enum AVSampleFormat)f->format) <= 0)
                    return fail(m, "audio frame limits", AVERROR(EINVAL));
                uint64_t mix_start=uptime_ms();int mix_result=audio_mix(m,&f->ch_layout);
                m->audio_decode_work=video_work_add(m->audio_decode_work,video_work_elapsed(mix_start));
                if(mix_result<0)return NMEDIA_ERROR;
                AVStream *s = m->format->streams[m->audio_index];
                int64_t stamp = f->best_effort_timestamp;
                m->audio_pts = stamp == AV_NOPTS_VALUE ? m->audio_clock : av_rescale_q(stamp, s->time_base, (AVRational){1,1000});
                m->audio_clock = m->audio_pts + (int64_t)f->nb_samples * 1000 / f->sample_rate;
                if (m->source_rate != f->sample_rate) m->phase = 0;
                m->source_rate = f->sample_rate; m->step = ((uint64_t)f->sample_rate << 32) / SOUND_RATE;
                if (m->audio_seek_ms >= 0 && m->audio_clock <= m->audio_seek_ms) { m->audio_decode_work=0;av_frame_unref(f); continue; }
                /* -1 means no seek. Negative pre-roll PTS must not turn that
                 * sentinel into a second trim (Opus -7ms -> lost 288 frames). */
                if (m->audio_seek_ms >= 0 && m->audio_seek_ms > m->audio_pts) m->phase = (uint64_t)(m->audio_seek_ms - m->audio_pts) * f->sample_rate / 1000 << 32;
                m->audio_seek_ms = -1; m->frame_audio = 1;
                if(m->video_late_set)return video_candidate(m,o);
                return audio_output(m, o);
            }
            if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) return fail(m, "decode", r);
            m->pending = NULL;
        }
        if (m->eof) {
            AVCodecContext *c = m->flush == 0 ? m->audio : m->flush == 1 ? m->video : NULL;
            if (m->flush >= 2) {if(m->video_late_set)return video_candidate(m,o);o->kind = NMEDIA_END; return NMEDIA_END; }
            m->flush++;
            if (c) { int r = video_send(m,c,NULL); if (r < 0 && r != AVERROR_EOF) return fail(m, "flush", r); m->pending = c; }
            continue;
        }
        int r = m->packet_reader ? m->packet_reader(m->packet_owner,m->packet) : av_read_frame(m->format, m->packet);
        if(r==AVERROR(EAGAIN))return video_candidate(m,o);
        if (r == AVERROR_EOF) { m->eof = 1; continue; }
        if (r < 0) return fail(m, "read packet", r);
        if(m->packet->size<0||m->packet->stream_index<0||(unsigned)m->packet->stream_index>=m->format->nb_streams||
           (m->packet->size&&!m->packet->data)){av_packet_unref(m->packet);return fail(m,"packet representation",AVERROR(EINVAL));}
        AVCodecContext *c = m->packet->stream_index == m->audio_index ? m->audio : m->packet->stream_index == m->video_index ? m->video : NULL;
        if (c) { r = video_send(m,c,m->packet); m->pending = c; }
        av_packet_unref(m->packet);
        if (c && r < 0) return fail(m, "send packet", r);
    }
    return video_candidate(m,o);
}
void nmedia_video_time(nmedia *m,int64_t ms){if(m){m->video_present_ms=ms;m->video_present_set=ms>=0;}}
void nmedia_video_stats(const nmedia *m,uint64_t *decoded,uint64_t *dropped){if(decoded)*decoded=m?m->video_decoded:0;if(dropped)*dropped=m?m->video_dropped:0;}
void nmedia_video_performance(const nmedia *m,uint64_t *frames,uint64_t *work_ms){if(frames)*frames=m?m->video_measured_frames:0;if(work_ms)*work_ms=m?m->video_work_ms:0;}
bool nmedia_move_video(nmedia *m,const struct nmedia_output *o,uint32_t **pixels,size_t *capacity){
    size_t count;
    if(!m||!o||!pixels||!capacity||m->error[0]||!m->video_borrowed||o->kind!=NMEDIA_VIDEO||
       o->pixels!=m->pixels||o->pts_ms!=m->video_loan_pts||o->width!=m->info.width||o->height!=m->info.height||
       !nmedia_video_size(o->width,o->height,&count,NULL)||count>m->pixel_capacity||
       *capacity>SIZE_MAX/sizeof(uint32_t)||(!*pixels&&*capacity)||*pixels==m->pixels)return false;
    uint32_t *completed=m->pixels;size_t available=m->pixel_capacity;
    m->pixels=*pixels;m->pixel_capacity=*capacity;m->video_borrowed=false;*pixels=completed;*capacity=available;return true;
}
static void close_decoder(nmedia *m) {
    m->video_borrowed=false;
    av_channel_layout_uninit(&m->mix_layout); m->mix_valid = false;
    av_frame_free(&m->video_late);m->video_late_set=false;
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
    m->video_borrowed=false;m->video_late_set=false;if(m->video_late)av_frame_unref(m->video_late);
    m->video_decode_work=m->video_late_work=0;
    m->audio_decode_work=0;
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
    m->video_borrowed=false;m->video_late_set=false;if(m->video_late)av_frame_unref(m->video_late);
    m->video_decode_work=m->video_late_work=m->video_measured_frames=m->video_work_ms=0;
    m->video_measurement_id=0;
    memset(&m->audio_observation,0,sizeof m->audio_observation);m->audio_decode_work=0;
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
