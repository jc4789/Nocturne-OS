/* Native GUI audio/video player. Decoding is in-process; IO, audio queues and
 * windows are Nocturne's, never an external player or a POSIX runtime. */
#include "nocturne.h"
#include "media.h"
#include "media_alloc_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define WIDTH 768
#define HEIGHT 584
static window_t *win;
static nmedia *media;
static const struct nmedia_info *info;
static struct nmedia_output pending;
static size_t audio_at;
static int audio_fd = -1;
static bool playing, finished;
static int64_t base_ms;
static uint64_t started;
static uint32_t *picture;
static size_t picture_capacity;
static int picture_w, picture_h;
static char error[160], path[2048];
static bool input_focus;
static int64_t position(void) { return base_ms + (playing ? (int64_t)(uptime_ms() - started) : 0); }
static void clear_audio(void) { if (audio_fd >= 0) { audio_flush(audio_fd);close(audio_fd); } audio_fd = -1; }
static void open_audio(void) {
    audio_fd=open("/dev/audio",O_RDWR|O_NONBLOCK);uint32_t queued=0;
    if(audio_fd>=0&&read(audio_fd,&queued,4)!=4)clear_audio();
}
static bool present_picture(void) {
    if(!pending.pixels||pending.width<=0||pending.height<=0||(uint64_t)pending.width*pending.height>NMEDIA_MAX_PIXELS){strlcpy(error,"Invalid video frame.",sizeof error);return false;}
    size_t count=(size_t)pending.width*pending.height;
    if(count>picture_capacity){uint32_t *p=nmedia_ff_realloc(picture,count*4);if(!p){strlcpy(error,"Video presentation allocation failed.",sizeof error);return false;}picture=p;picture_capacity=count;}
    memcpy(picture,pending.pixels,count*4);picture_w=pending.width;picture_h=pending.height;return true;
}
static bool seek_to(int64_t ms) {
    if (!media || !nmedia_seek(media, ms)) { strlcpy(error,"This input cannot seek to that position.",sizeof error); return false; }
    clear_audio();
    if (playing && info->audio) open_audio();
    base_ms = ms; started = uptime_ms(); pending.kind = 0; audio_at = 0; finished = false;
    return true;
}
static void toggle(void) {
    if (!media) return;
    if (playing) {
        int64_t now = position(); playing = false;
        if (!seek_to(now)) { base_ms = now; clear_audio(); }
    } else {
        if (finished && !seek_to(0)) return;
        playing = true; started = uptime_ms();
        if (info->audio) open_audio();
    }
}
static void clock_text(char *s, int64_t ms) {
    if (ms < 0) { strlcpy(s,"--:--",16); return; }
    snprintf(s,16,"%u:%02u",(unsigned)(ms/60000),(unsigned)(ms/1000%60));
}
static void draw(void) {
    canvas_t *c = &win->c;
    gfx_fill(c,0,0,WIDTH,HEIGHT,UI_BG);
    gfx_fill(c,12,12,WIDTH-24,408,RGB(0,0,0));
    if (picture) nmedia_draw(c,picture,picture_w,picture_h,12,12,WIDTH-24,408);
    else gfx_text(c,24,196,media ? "Audio playback" : "Open a file or HTTP(S) URL below.",UI_FG,TRANSPARENT,FONT_LARGE);
    char t1[16],t2[16],line[256]; clock_text(t1,position());clock_text(t2,info?info->duration_ms:-1);
    snprintf(line,sizeof line,"%s  /  %s    %s%s%s",t1,t2,info?info->audio_codec:"",info&&info->audio&&info->video?" + ":"",info?info->video_codec:"");
    gfx_text(c,16,430,line,UI_FG,TRANSPARENT,FONT_SMALL);
    if (info && info->duration_ms > 0) {
        int f = (int)(MIN(position(),info->duration_ms)*(WIDTH-32)/info->duration_ms);
        gfx_fill(c,16,456,WIDTH-32,6,UI_PANEL); if (f>0) gfx_fill(c,16,456,f,6,UI_ACCENT);
    }
    gfx_fill_round(c,16,476,130,32,6,UI_BTN);
    gfx_text(c,30,484,playing?"Pause":"Play",UI_FG,TRANSPARENT,FONT_SMALL);
    gfx_text(c,164,484,"Space: pause   Left/Right: seek 5 seconds",UI_DIM,TRANSPARENT,FONT_SMALL);
    ui_textfield(c,16,516,WIDTH-136,path,input_focus);
    ui_button(c,WIDTH-108,516,92,24,"Open",false,false);
    if (error[0]) gfx_text(c,16,558,error,UI_ACCENT2,TRANSPARENT,FONT_SMALL);
    else if (playing && info && info->audio && audio_fd < 0) gfx_text(c,16,558,"Audio unavailable; video continues silently.",UI_ACCENT2,TRANSPARENT,FONT_SMALL);
    else gfx_text(c,16,558,"Ctrl+O: input   HTTP refills wait synchronously; no cookies or login.",UI_DIM,TRANSPARENT,FONT_SMALL);
    win_update(win);
}
static void release_media(void) {
    clear_audio(); nmedia_close(media);media=NULL;info=NULL;
    nmedia_ff_free(picture);picture=NULL;picture_capacity=0;picture_w=picture_h=0;
    memset(&pending,0,sizeof pending);audio_at=0;playing=finished=false;base_ms=0;
}
static void open_media(void) {
    release_media();error[0]=0;input_focus=false;
    if (!path[0]) { strlcpy(error,"Enter a file path or anonymous HTTP(S) URL.",sizeof error);return; }
    strlcpy(error,"Opening media (network reads are synchronous)...",sizeof error);draw();
    bool url = !strncmp(path,"http://",7)||!strncmp(path,"https://",8);
    media=url?nmedia_open_url(path,error,sizeof error):nmedia_open(path,error,sizeof error);
    if(media){info=nmedia_get_info(media);toggle();}
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
            if (audio_fd>=0) {
                uint32_t queued=0;
                if (read(audio_fd,&queued,4)!=4 || queued>=4800) return changed;
                size_t frames=MIN(pending.frames-audio_at,(size_t)(4800-queued));
                ssize_t n=write(audio_fd,pending.samples+audio_at*2,frames*4);
                if (n<0 && errno==EAGAIN) return changed;
                if (n<=0 || (n&3)) { clear_audio();strlcpy(error,"Audio output failed.",sizeof error); }
                else audio_at+=(size_t)n/4;
                if (audio_fd>=0 && audio_at<pending.frames) return changed;
            }
            pending.kind=0;
        } else if (pending.kind==NMEDIA_END) {
            uint32_t queued=0;if(audio_fd>=0)read(audio_fd,&queued,4);
            if (queued) return changed;
            int64_t at=position();
            if(info&&info->video&&info->duration_ms>at&&info->duration_ms-at<=1000)return changed;
            base_ms=position();playing=false;finished=true;clear_audio();return true;
        }
    }
    return changed;
}
int main(int argc,char **argv) {
    win=win_open(WIDTH,HEIGHT,"Media Player",0);
    if (!win) return 1;
    if (argc>1) {
        if(strlen(argv[1])>=sizeof path)strlcpy(error,"Media path/URL is too long; nothing was opened.",sizeof error);
        else {memcpy(path,argv[1],strlen(argv[1])+1);open_media();}
    } else input_focus=true;
    draw();uint64_t last=0;
    for (;;) {
        struct gui_event e;int r=win_event(win,&e,playing?10:-1);bool redraw=false;
        if (r<0 || (r>0&&e.type==EV_CLOSE)) break;
        if (r>0&&e.type==EV_KEY&&e.pressed) {
            if((e.mods&NMOD_CTRL)&&(e.key=='o'||e.key=='O'))input_focus=true;
            else if(input_focus){int edited=ui_edit_key(path,sizeof path,&e);if(edited==1)open_media();else if(edited<0)input_focus=false;}
            else if(e.key==' ')toggle();
            else if(e.key==NKEY_LEFT)seek_to(MAX(0,position()-5000));
            else if(e.key==NKEY_RIGHT)seek_to(info&&info->duration_ms>=0?MIN(info->duration_ms,position()+5000):position()+5000);
            redraw=true;
        } else if(r>0&&e.type==EV_MOUSE_DOWN&&(e.buttons&1)) {
            input_focus=ui_hit(e.x,e.y,16,516,WIDTH-136,24);
            if(ui_hit(e.x,e.y,WIDTH-108,516,92,24))open_media();
            if(ui_hit(e.x,e.y,16,476,130,32))toggle();
            if(info&&info->duration_ms>0&&ui_hit(e.x,e.y,16,448,WIDTH-32,24))seek_to((int64_t)(e.x-16)*info->duration_ms/(WIDTH-32));
            redraw=true;
        }
        redraw|=pump();
        if(redraw||(playing&&uptime_ms()-last>=100)){draw();last=uptime_ms();}
    }
    release_media();win_close(win);return 0;
}
