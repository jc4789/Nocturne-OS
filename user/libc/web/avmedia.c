/* Private native HTML media bridge. It never opens a page-selected filesystem
 * path: bytes arrive through the browser's existing fetch/CORS path. */
#include "avmedia.h"
#include "media.h"
#include "media_alloc_private.h"
#include "nocturne.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define DOCUMENT_MEDIA_LIMIT 64
#define DOCUMENT_DECODER_LIMIT 4
#define DOCUMENT_MEDIA_BYTES (64u * 1024u * 1024u)
struct web_avmedia {
    struct web_avmedia *next;
    web_doc *doc;
    node_t *node;
    uint32_t generation;
    nmedia *decoder;
    size_t input_size, audio_at;
    struct nmedia_output pending;
    uint32_t *pixels;
    size_t pixel_capacity;
    int width, height, fd;
    bool playing, ended, muted;
    int ready;
    double volume;
    int64_t base_ms;
    uint64_t started;
    char error[160];
    int16_t scaled[4096 * 2];
};
static struct web_avmedia *streams;
static bool media_node(node_t *n) { return n && n->type == N_ELEM && !n->foreign && (n->tag == T_audio || n->tag == T_video); }
static struct web_avmedia *find(web_doc *d, node_t *n, bool create) {
    unsigned count = 0;
    for (struct web_avmedia *s = streams; s; s = s->next) {
        if (s->doc == d) { count++; if (s->node == n) return s; }
    }
    if (!create || count >= DOCUMENT_MEDIA_LIMIT) return NULL;
    struct web_avmedia *s = nmedia_ff_mallocz(sizeof *s);
    if (!s) return NULL;
    s->doc = d; s->node = n; s->fd = -1; s->volume = 1; s->next = streams; streams = s;
    return s;
}
static void close_audio(struct web_avmedia *s) { if (s->fd >= 0) { audio_flush(s->fd); close(s->fd); } s->fd = -1; }
static void unload(struct web_avmedia *s) {
    close_audio(s); nmedia_close(s->decoder); s->decoder = NULL;
    nmedia_ff_free(s->pixels); s->pixels = NULL; s->pixel_capacity = 0;
    s->width = s->height = 0; s->playing = s->ended = false;
    s->base_ms = 0; s->input_size = 0; s->audio_at = 0; s->pending.kind = 0; s->ready = 0; s->error[0] = 0;
}
static int64_t current(struct web_avmedia *s, uint64_t now) {
    int64_t ms = s->base_ms + (s->playing && now >= s->started ? (int64_t)(now - s->started) : 0);
    const struct nmedia_info *info = nmedia_get_info(s->decoder);
    if (info && info->duration_ms >= 0 && ms > info->duration_ms) ms = info->duration_ms;
    return ms;
}
static bool audio_open(struct web_avmedia *s) {
    const struct nmedia_info *info = nmedia_get_info(s->decoder);
    if (!info || !info->audio) return true;
    s->fd = open("/dev/audio", O_RDWR | O_NONBLOCK);
    uint32_t queued=0;
    if (s->fd < 0 || read(s->fd,&queued,4)!=4) { close_audio(s);strlcpy(s->error,"native audio output unavailable",sizeof s->error);return false; }
    s->error[0]=0;return true;
}
static bool seek(struct web_avmedia *s, int64_t ms, uint64_t now) {
    if (!s->decoder || !nmedia_seek(s->decoder,ms)) return false;
    close_audio(s); s->base_ms=ms; s->started=now; s->pending.kind=0; s->audio_at=0;s->ended=false;
    if(s->playing&&!audio_open(s)){s->playing=false;return false;}
    return true;
}
/* Decoder output is borrowed only until its next step. The display owns its
 * last presented frame, separate from a future-PTS pending decoder frame. */
static bool present(struct web_avmedia *s) {
    struct nmedia_output *o=&s->pending;
    if(!o->pixels||o->width<=0||o->height<=0||(uint64_t)o->width*o->height>NMEDIA_MAX_PIXELS){strlcpy(s->error,"invalid native video span",sizeof s->error);return false;}
    size_t count=(size_t)o->width*o->height;
    if(count>s->pixel_capacity){uint32_t *p=nmedia_ff_realloc(s->pixels,count*4);if(!p){strlcpy(s->error,"video presentation allocation",sizeof s->error);return false;}s->pixels=p;s->pixel_capacity=count;}
    memcpy(s->pixels,o->pixels,count*4);s->width=o->width;s->height=o->height;return true;
}
static JSValue state(JSContext *ctx, struct web_avmedia *s) {
    JSValue o=JS_NewObject(ctx);
    const struct nmedia_info *info=s?nmedia_get_info(s->decoder):NULL;
    JS_SetPropertyStr(ctx,o,"readyState",JS_NewInt32(ctx,s?s->ready:0));
    JS_SetPropertyStr(ctx,o,"duration",JS_NewFloat64(ctx,info&&info->duration_ms>=0?info->duration_ms/1000.0:NAN));
    JS_SetPropertyStr(ctx,o,"currentTime",JS_NewFloat64(ctx,s?current(s,uptime_ms())/1000.0:0));
    JS_SetPropertyStr(ctx,o,"paused",JS_NewBool(ctx,!s||!s->playing));
    JS_SetPropertyStr(ctx,o,"ended",JS_NewBool(ctx,s&&s->ended));
    JS_SetPropertyStr(ctx,o,"videoWidth",JS_NewInt32(ctx,info?info->width:0));
    JS_SetPropertyStr(ctx,o,"videoHeight",JS_NewInt32(ctx,info?info->height:0));
    JS_SetPropertyStr(ctx,o,"error",s&&s->error[0]?JS_NewString(ctx,s->error):JS_NULL);
    return o;
}
JSValue web_avmedia_call(JSContext *ctx, web_doc *d, node_t *n, const char *op, int argc, JSValueConst *argv) {
    if (!strcmp(op,"type")) {
        const char *type=argc?JS_ToCString(ctx,argv[0]):NULL;
        if(!type&&argc)return JS_EXCEPTION;
        JSValue result=JS_NewString(ctx,nmedia_can_play_type(type));JS_FreeCString(ctx,type);return result;
    }
    if(!media_node(n)||!d)return JS_ThrowTypeError(ctx,"HTMLMediaElement receiver required");
    if(n->owner!=d && (!n->owner || n->owner->dom_family!=d))return JS_ThrowTypeError(ctx,"foreign document media receiver");
    struct web_avmedia *s=find(d,n,strcmp(op,"state")!=0);
    if(!s)return !strcmp(op,"state")?state(ctx,NULL):JS_ThrowRangeError(ctx,"document media limit");
    uint64_t now=uptime_ms();
    if(!strcmp(op,"state"))return state(ctx,s);
    if(!strcmp(op,"reset")){
        uint32_t generation;if(!argc||JS_ToUint32(ctx,&generation,argv[0])<0)return JS_EXCEPTION;
        unload(s);s->generation=generation;d->dirty=true;return JS_UNDEFINED;
    }
    if(!strcmp(op,"load")){
        uint32_t generation;size_t size=0;
        if(argc<2||JS_ToUint32(ctx,&generation,argv[1])<0)return JS_EXCEPTION;
        if(s->generation!=generation)return JS_FALSE;
        uint8_t *bytes=JS_GetArrayBuffer(ctx,&size,argv[0]);if(!bytes)return JS_EXCEPTION;
        size_t total=0;unsigned decoders=0;for(struct web_avmedia *p=streams;p;p=p->next)if(p->doc==d&&p!=s){total+=p->input_size;if(p->decoder)decoders++;}
        if(size>NMEDIA_MAX_BYTES||total>DOCUMENT_MEDIA_BYTES-size||decoders>=DOCUMENT_DECODER_LIMIT)return JS_ThrowRangeError(ctx,"document media resource limit");
        unload(s);s->generation=generation;
        s->decoder=nmedia_open_memory(bytes,size,s->error,sizeof s->error);
        if(s->decoder){
            s->input_size=size;s->ready=1;
            int result=NMEDIA_AGAIN;
            for(int i=0;i<4&&result==NMEDIA_AGAIN;i++)result=nmedia_step(s->decoder,&s->pending);
            if(result==NMEDIA_ERROR){strlcpy(s->error,nmedia_error(s->decoder),sizeof s->error);nmedia_close(s->decoder);s->decoder=NULL;s->input_size=0;s->ready=0;}
            else if(result==NMEDIA_AUDIO||result==NMEDIA_VIDEO){
                s->ready=4;
                if(result==NMEDIA_VIDEO&&!present(s)){nmedia_close(s->decoder);s->decoder=NULL;s->input_size=0;s->ready=0;}
            }
        }
        d->dirty=true;return JS_NewBool(ctx,s->decoder!=NULL);
    }
    if(!strcmp(op,"play")){
        if(!s->decoder)return JS_FALSE;
        if(s->playing)return JS_TRUE;
        if(s->ended&&!seek(s,0,now))return JS_FALSE;
        if(!audio_open(s))return JS_FALSE;
        s->playing=true;s->started=now;s->ended=false;return JS_TRUE;
    }
    if(!strcmp(op,"pause")){
        if(s->playing){int64_t ms=current(s,now);s->playing=false;if(!seek(s,ms,now))s->base_ms=ms;close_audio(s);}
        return JS_UNDEFINED;
    }
    if(!strcmp(op,"seek")){
        double seconds;if(!argc||JS_ToFloat64(ctx,&seconds,argv[0])<0)return JS_EXCEPTION;
        if(!isfinite(seconds)||seconds<0||seconds>1e9)return JS_ThrowRangeError(ctx,"invalid media time");
        return JS_NewBool(ctx,seek(s,(int64_t)(seconds*1000),now));
    }
    if(!strcmp(op,"volume")){
        double v;if(argc<2||JS_ToFloat64(ctx,&v,argv[0])<0)return JS_EXCEPTION;
        int muted=JS_ToBool(ctx,argv[1]);if(muted<0)return JS_EXCEPTION;
        if(!isfinite(v)||v<0||v>1)return JS_ThrowRangeError(ctx,"invalid media volume");
        s->volume=v;s->muted=muted;return JS_UNDEFINED;
    }
    return JS_ThrowTypeError(ctx,"unknown native media operation");
}
static void pump(struct web_avmedia *s,uint64_t now){
    if(!s->decoder||!s->playing)return;
    for(int budget=0;budget<8;budget++){
        if(!s->pending.kind){int r=nmedia_step(s->decoder,&s->pending);s->audio_at=0;
            if(r==NMEDIA_AGAIN)return;
            if(r==NMEDIA_ERROR){strlcpy(s->error,nmedia_error(s->decoder),sizeof s->error);s->base_ms=current(s,now);s->playing=false;close_audio(s);s->doc->dirty=true;return;}
            if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO)s->ready=4;
        }
        if(s->pending.kind==NMEDIA_VIDEO){
            if(s->pending.pts_ms>current(s,now)+5)return;
            if(!present(s)){s->base_ms=current(s,now);s->playing=false;close_audio(s);s->doc->dirty=true;return;}
            s->doc->dirty=true;s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_AUDIO){
            if(s->pending.pts_ms>current(s,now)+100)return;
            if(s->fd>=0){uint32_t queued=0;if(read(s->fd,&queued,4)!=4||queued>=4800)return;
                if(s->pending.frames>4096||s->audio_at>s->pending.frames){strlcpy(s->error,"invalid native audio span",sizeof s->error);s->playing=false;close_audio(s);return;}
                size_t frames=MIN(MIN(s->pending.frames-s->audio_at,(size_t)(4800-queued)),(size_t)4096);
                double gain=s->muted?0:s->volume;
                for(size_t i=0;i<frames*2;i++)s->scaled[i]=(int16_t)(s->pending.samples[s->audio_at*2+i]*gain);
                ssize_t n=write(s->fd,s->scaled,frames*4);
                if(n<0&&errno==EAGAIN)return;
                if(n<=0||(n&3)){strlcpy(s->error,"native audio write failed",sizeof s->error);s->base_ms=current(s,now);s->playing=false;close_audio(s);return;}
                s->audio_at+=(size_t)n/4;if(s->audio_at<s->pending.frames)return;
            }s->pending.kind=0;
        }else if(s->pending.kind==NMEDIA_END){
            uint32_t queued=0;if(s->fd>=0)read(s->fd,&queued,4);if(queued)return;
            /* EOF is not the end of the last displayed frame. Honor a known
             * duration only for a bounded <=1s tail; corrupt/unknown duration
             * must not hold an already-drained decoder forever. */
            const struct nmedia_info *info=nmedia_get_info(s->decoder);
            int64_t at=current(s,now);
            if(info&&info->video&&info->duration_ms>at&&info->duration_ms-at<=1000)return;
            s->base_ms=current(s,now);s->playing=false;s->ended=true;close_audio(s);s->doc->dirty=true;return;
        }
    }
}
void web_avmedia_tick(web_doc *d,uint64_t now){
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d){
        if(s->node->owner!=d&&(!s->node->owner||s->node->owner->dom_family!=d))unload(s);
        else pump(s,now);
    }
}
int64_t web_avmedia_deadline(web_doc *d,uint64_t now){
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->doc==d&&s->playing)return (int64_t)now+10;
    return -1;
}
void web_avmedia_free(web_doc *d){
    struct web_avmedia **link=&streams;
    while(*link){struct web_avmedia *s=*link;if(s->doc!=d){link=&s->next;continue;}*link=s->next;unload(s);nmedia_ff_free(s);}
}
bool web_avmedia_paint(web_doc *d,node_t *n,canvas_t *c,int x,int y,int w,int h){
    struct web_avmedia *s=find(d,n,false);
    if(!media_node(n)||w<=0||h<=0)return false;
    int64_t x0=MAX(MAX((int64_t)x,c->cx0),0),y0=MAX(MAX((int64_t)y,c->cy0),0);
    int64_t x1=MIN(MIN((int64_t)x+w,c->cx1),c->w),y1=MIN(MIN((int64_t)y+h,c->cy1),c->h);
    if(x0>=x1||y0>=y1)return true;
    int ox0=c->cx0,oy0=c->cy0,ox1=c->cx1,oy1=c->cy1;
    c->cx0=(int)x0;c->cy0=(int)y0;c->cx1=(int)x1;c->cy1=(int)y1;
    gfx_fill(c,(int)x0,(int)y0,(int)(x1-x0),(int)(y1-y0),RGB(0,0,0));
    bool controls=node_attr(n,"controls")!=NULL;
    int strip=controls?MIN(h,32):0;
    if(s&&s->pixels&&n->tag==T_video)nmedia_draw(c,s->pixels,s->width,s->height,x,y,w,h-strip);
    if(controls){
        int64_t top=MAX((int64_t)y+h-strip,y0);
        if(top<y1){
            gfx_fill(c,(int)x0,(int)top,(int)(x1-x0),(int)(y1-top),RGB(38,38,44));
            const char *text=s&&s->error[0]?"Media error":s&&s->playing?"Pause":"Play";
            gfx_text(c,(int)x0+8,(int)top+7,text,RGB(255,255,255),TRANSPARENT,FONT_SMALL);
        }
    }
    c->cx0=ox0;c->cy0=oy0;c->cx1=ox1;c->cy1=oy1;return true;
}
bool web_avmedia_size(node_t *n,int *w,int *h){
    for(struct web_avmedia *s=streams;s;s=s->next)if(s->node==n&&s->decoder){
        const struct nmedia_info *info=nmedia_get_info(s->decoder);
        if(info->video&&info->width>0&&info->height>0){*w=info->width;*h=info->height;return true;}
    }
    return false;
}
