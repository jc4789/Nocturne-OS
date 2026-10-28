/* Native GUI audio/video player. Decoding is in-process; IO, audio queues and
 * windows are Nocturne's, never an external player or a POSIX runtime.
 * The controls are widgets of the interface engine (libc/ui.c); the video is a
 * deep-colour surface that this program draws each new picture into. */
#include "nocturne.h"
#include "media.h"
#include "display.h"
#include "ui.h"
#include "media_video_private.h"
#include "media_alloc_private.h"
#include "weburl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIDTH 768
#define HEIGHT 584
#define PAD 12
#define VIDEO_W (WIDTH - 2 * PAD)
#define VIDEO_H 408
#define SEEK_PX 500 /* the slider moves on screen in about this many steps */
#define WINDOW 1
#define VIDEO 1
static struct ui_session *ui;
static nmedia *media;
static const struct nmedia_info *info;
static struct nmedia_output pending;
static size_t audio_at;
static n_handle audio = -1;
static bool playing, finished;
static int64_t base_ms;
static uint64_t started;
static uint32_t *picture;
static uint8_t *picture_deep; /* low bits of a 10-bit frame (display_abi.h) */
static size_t picture_deep_capacity;
static size_t picture_capacity;
static int picture_w, picture_h;
static bool has_deep;
static char error[160], path[2048];
static bool picture_new; /* a picture arrived that the surface does not show yet */
/* the seek slider while it is dragged, the field's "focus" (counted up: positive takes the keyboard, negative
   gives it up), the places of the widgets in the last view and what that view showed of the time */
static bool scrubbing;
static int64_t scrub_ms, shown_ms = -1;
static int shown_px = -1;
static int field_focus;
static uint32_t n_play, n_seek, n_field, n_open;
static void focus_field(bool on) { field_focus = (abs(field_focus) + 1) * (on ? 1 : -1); }
static int64_t position(void) { return base_ms + (playing ? (int64_t)(n_clock_uptime_ms() - started) : 0); }
static void clear_audio(void) { if (audio >= 0) { n_audio_flush(audio);n_handle_close(audio); } audio = -1; }
/* frames the stream still holds; -1 if it failed */
static int64_t audio_queued(void) { struct n_audio_status st;return n_audio_status(audio,&st)<0?-1:(int64_t)st.queued_frames; }
static void open_audio(void) {
    audio=n_audio_open();
    if(audio>=0&&audio_queued()<0)clear_audio();
}
static bool present_picture(void) {
    size_t count,bytes;
    if(!pending.pixels||!nmedia_video_size(pending.width,pending.height,&count,&bytes)){strlcpy(error,"Invalid video frame.",sizeof error);return false;}
    if(count>picture_capacity){uint32_t *p=nmedia_ff_realloc(picture,bytes);if(!p){strlcpy(error,"Video presentation allocation failed.",sizeof error);return false;}picture=p;picture_capacity=count;}
    memcpy(picture,pending.pixels,bytes);picture_w=pending.width;picture_h=pending.height;
    if(pending.deep){
        if(count>picture_deep_capacity){uint8_t *p=nmedia_ff_realloc(picture_deep,count);if(!p){strlcpy(error,"Video presentation allocation failed.",sizeof error);return false;}picture_deep=p;picture_deep_capacity=count;}
        memcpy(picture_deep,pending.deep,count);
    }
    has_deep=pending.deep!=NULL;picture_new=true;return true;
}
static bool seek_to(int64_t ms) {
    if (!media || !nmedia_seek(media, ms)) { strlcpy(error,"This input cannot seek to that position.",sizeof error); return false; }
    clear_audio();
    if (playing && info->audio) open_audio();
    base_ms = ms; started = n_clock_uptime_ms(); pending.kind = 0; audio_at = 0; finished = false;
    return true;
}
static void toggle(void) {
    if (!media) return;
    if (playing) {
        int64_t now = position(); playing = false;
        if (!seek_to(now)) { base_ms = now; clear_audio(); }
    } else {
        if (finished && !seek_to(0)) return;
        playing = true; started = n_clock_uptime_ms();
        if (info->audio) open_audio();
    }
}
static void clock_text(char *s, int64_t ms) {
    if (ms < 0) { strlcpy(s,"--:--",16); return; }
    snprintf(s,16,"%u:%02u",(unsigned)(ms/60000),(unsigned)(ms/1000%60));
}
/* The video surface: black, the picture fitted into it, and its 10-bit plane where the desktop keeps one (the
   plane is cleared to 0 where nothing deep is drawn; on an 8 bpc desktop the picture is dithered to 8 bits). */
static void draw_video(void) {
    canvas_t *c = ui_session_surface(ui, VIDEO);
    if (!c) return;
    gfx_fill(c,0,0,VIDEO_W,VIDEO_H,RGB(0,0,0));
    uint8_t *deep = ui_session_surface_deep(ui, VIDEO);
    if (deep) memset(deep,0,(size_t)VIDEO_W*VIDEO_H);
    if (picture) nmedia_draw_deep(c,deep,picture,has_deep?picture_deep:NULL,picture_w,picture_h,0,0,VIDEO_W,VIDEO_H);
    else {
        const char *msg = media ? "Audio playback" : "Open a file or HTTP(S) URL below.";
        float size = 22, tw = ui_text_width(msg,strlen(msg),size,1), lh = ui_line_height(size,1,NULL);
        ui_text(c,(VIDEO_W-tw)/2,(VIDEO_H-lh)/2,msg,strlen(msg),size,1,ui_theme.text);
    }
    ui_surface_changed(ui, VIDEO);
}
/* The controls. Sent when a word of them changes, not for every picture. */
static void show(void) {
    const struct ui_theme *t = &ui_theme;
    int64_t at = scrubbing ? scrub_ms : position(), duration = info ? info->duration_ms : -1;
    char t1[16],t2[16],codecs[96];
    clock_text(t1,at);clock_text(t2,duration);
    snprintf(codecs,sizeof codecs,"%s%s%s",info?info->audio_codec:"",info&&info->audio&&info->video?" + ":"",info?info->video_codec:"");
    const char *status = "Ctrl+O: input   HTTP refills wait synchronously; no cookies or login.";
    uint32_t status_colour = t->textDim;
    if (error[0]) { status = error; status_colour = t->danger; }
    else if (playing && info && info->audio && audio < 0) { status = "Audio unavailable; video continues silently."; status_colour = t->danger; }
    bool seekable = info && duration > 0;
    shown_ms = at;
    shown_px = seekable ? (int)(MIN(MAX(at,0),duration)*SEEK_PX/duration) : 0;
    char time[48];
    snprintf(time,sizeof time,"%s  /  %s",t1,t2);

    struct ui_msg m;
    ui_view(&m, WINDOW);
    ui_node(&m,"column"); ui_num(&m,"padding",PAD); ui_num(&m,"gap",10);
        ui_node(&m,"surface"); ui_num(&m,"surface",VIDEO); ui_num(&m,"width",VIDEO_W); ui_num(&m,"height",VIDEO_H); ui_end(&m);
        ui_node(&m,"row"); ui_num(&m,"gap",12);
            ui_node(&m,"text"); ui_str(&m,"text",time); ui_num(&m,"minWidth",110); ui_end(&m);
            n_seek = ui_node(&m,"slider"); ui_str(&m,"width","fill");
            ui_num(&m,"value",seekable?(double)MIN(MAX(at,0),duration):0); ui_num(&m,"max",seekable?(double)duration:1);
            ui_num(&m,"wheelStep",5000); ui_num(&m,"disabled",!seekable); ui_end(&m);
            ui_node(&m,"text"); ui_str(&m,"text",codecs); ui_str(&m,"style","caption"); ui_end(&m);
        ui_end(&m);
        ui_node(&m,"row"); ui_num(&m,"gap",12);
            n_play = ui_node(&m,"button"); ui_str(&m,"text",playing?"Pause":"Play"); ui_str(&m,"variant","primary");
            ui_num(&m,"minWidth",110); ui_end(&m);
            ui_node(&m,"text"); ui_str(&m,"text","Space: pause   Left/Right: seek 5 seconds"); ui_str(&m,"style","caption"); ui_end(&m);
        ui_end(&m);
        ui_node(&m,"row"); ui_num(&m,"gap",12);
            n_field = ui_node(&m,"field"); ui_num(&m,"focus",field_focus); ui_str(&m,"width","fill"); ui_str(&m,"value",path);
            ui_str(&m,"placeholder","File path or http(s) URL"); ui_end(&m);
            n_open = ui_node(&m,"button"); ui_str(&m,"text","Open"); ui_end(&m);
        ui_end(&m);
        ui_node(&m,"text"); ui_str(&m,"text",status); ui_str(&m,"style","caption"); ui_num(&m,"colour",status_colour); ui_end(&m);
    ui_end(&m);
    ui_send(ui, &m);
}
static void release_media(void) {
    clear_audio(); nmedia_close(media);media=NULL;info=NULL;
    nmedia_ff_free(picture);picture=NULL;picture_capacity=0;picture_w=picture_h=0;
    nmedia_ff_free(picture_deep);picture_deep=NULL;picture_deep_capacity=0;has_deep=false;
    memset(&pending,0,sizeof pending);audio_at=0;playing=finished=false;base_ms=0;
}
static void open_media(void) {
    release_media();error[0]=0;scrubbing=false;
    focus_field(false); /* the Enter that opened this does not leave the field taking Space and the arrows */
    draw_video();
    if (!path[0]) { strlcpy(error,"Enter a file path or anonymous HTTP(S) URL.",sizeof error);return; }
    strlcpy(error,"Opening media (network reads are synchronous)...",sizeof error);
    show();ui_session_pump(ui); /* the blocking open below must not leave the window blank */
    /* What the field holds is an http or https URL when the URL parser reads it as one. */
    struct web_url typed;bool url=web_url_read(path,strlen(path),&typed)==1&&
        web_url_http_scheme(&typed);
    web_url_free(&typed);
    media=url?nmedia_open_url(path,error,sizeof error):nmedia_open(path,error,sizeof error);
    /* 10-bit frames either way: scaled in 10 bits, then kept by a 10 bpc
       display or dithered where they land on an 8 bpc one. */
    if(media){info=nmedia_get_info(media);nmedia_set_deep(media,true);toggle();}
}
static bool pump(void) {
    if (!playing || !media || finished) return false;
    bool changed = false;
    for (int budget=0;budget<8;budget++) {
        if (!pending.kind) {
            int r = nmedia_step(media,&pending); audio_at=0;
            if (r==NMEDIA_ERROR) { strlcpy(error,nmedia_error(media),sizeof error);base_ms=position();playing=false;clear_audio();return true; }
            if (r==NMEDIA_AGAIN) return changed;
        }
        int64_t now=position();
        if (pending.kind==NMEDIA_VIDEO) {
            if (pending.pts_ms>now+5) return changed;
            if(!present_picture()){base_ms=position();playing=false;clear_audio();return true;}
            pending.kind=0;changed=true;
        } else if (pending.kind==NMEDIA_AUDIO) {
            if (pending.pts_ms>now+100) return changed;
            if (audio>=0) {
                int64_t queued=audio_queued();
                if (queued<0 || queued>=4800) return changed;
                size_t frames=MIN(pending.frames-audio_at,(size_t)(4800-queued));
                n_result n=n_audio_submit(audio,pending.samples+audio_at*2,frames,0);
                if (n==-N_ERR_NOT_READY) return changed;
                if (n<=0) { clear_audio();strlcpy(error,"Audio output failed.",sizeof error); }
                else audio_at+=(size_t)n;
                if (audio>=0 && audio_at<pending.frames) return changed;
            }
            pending.kind=0;
        } else if (pending.kind==NMEDIA_END) {
            int64_t queued=audio>=0?audio_queued():0;
            if (queued>0) return changed;
            int64_t at=position();
            if(info&&info->video&&info->duration_ms>at&&info->duration_ms-at<=1000)return changed;
            base_ms=position();playing=false;finished=true;clear_audio();return true;
        }
    }
    return changed;
}
int main(int argc,char **argv) {
    ui=ui_session_new();
    /* the engine paces each picture with win_begin_frame and paints it whole */
    if (!ui || ui_window(ui,WINDOW,WIDTH,HEIGHT,WIN_DOUBLE_BUFFER|WIN_DEEP_COLOR,"Media Player",NULL,NULL)<0) return 1;
    if (!ui_surface(ui,VIDEO,VIDEO_W,VIDEO_H,UI_SURFACE_DEEP)) return 1;
    if (argc>1) {
        if(strlen(argv[1])>=sizeof path)strlcpy(error,"Media path/URL is too long; nothing was opened.",sizeof error);
        else {memcpy(path,argv[1],strlen(argv[1])+1);open_media();}
    } else focus_field(true);
    draw_video();show();
    char text[sizeof path];
    for (bool running=true;running;) {
        ui_session_wait(ui,playing?10:-1);
        bool redraw=false;struct ui_event e;
        while (ui_next_event(ui,&e,text,sizeof text)) {
            switch (e.type) {
            case UI_EVENT_CLOSE: running=false; break;
            case UI_EVENT_KEY: { /* only when no field has the keyboard */
                if (e.flags&UI_UP) break;
                uint32_t key=(uint32_t)e.a;
                if((e.mods&NMOD_CTRL)&&(key=='o'||key=='O'))focus_field(true);
                else if(key==' ')toggle();
                else if(key==NKEY_LEFT)seek_to(MAX(0,position()-5000));
                else if(key==NKEY_RIGHT)seek_to(info&&info->duration_ms>=0?MIN(info->duration_ms,position()+5000):position()+5000);
                redraw=true;
                break;
            }
            case UI_EVENT_TEXT:
                if (e.node==n_field) strlcpy(path,text,sizeof path);
                break;
            case UI_EVENT_SUBMIT:
                if (e.node==n_field) { strlcpy(path,text,sizeof path);open_media();redraw=true; }
                break;
            case UI_EVENT_CLICK:
                if(e.node==n_open)open_media();
                else if(e.node==n_play)toggle();
                redraw=true;
                break;
            case UI_EVENT_SLIDE:
                if (e.node!=n_seek||!info||info->duration_ms<=0) break;
                if (e.flags&UI_DONE) { scrubbing=false;seek_to((int64_t)e.a); }
                else { scrubbing=true;scrub_ms=(int64_t)e.a; }
                redraw=true;
                break;
            }
        }
        if (!running||!ui_session_open(ui)) break;
        if (pump()) redraw=true;
        bool fresh=picture_new;
        if (fresh) { picture_new=false;draw_video(); }
        if (!redraw&&playing&&!scrubbing) { /* the controls change when the clock ticks over or the slider moves a step */
            int64_t at=position(),duration=info?info->duration_ms:-1;
            if(at/1000!=shown_ms/1000||(duration>0&&(int)(MIN(MAX(at,0),duration)*SEEK_PX/duration)!=shown_px))redraw=true;
        }
        if (redraw) show();
        if (fresh||redraw) ui_session_pump(ui); /* show the new picture now */
    }
    release_media();ui_session_free(ui);return 0;
}
