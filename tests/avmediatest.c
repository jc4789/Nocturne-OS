/* Native browser fetch/QuickJS -> real FFmpeg -> frame/audio lifetime regression.
 * The host serves actual compressed synthetic bytes, not decoder/player mocks. */
#include "nocturne.h"
#include "web.h"
#include "media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef AVMEDIA_CASES_SCRIPT
#define AVMEDIA_CASES_SCRIPT "/data/tests/js_avmedia_cases.js"
#define AVMEDIA_FRAME_FIXTURE "h264.mp4"
#define AVMEDIA_TEST_NAME "avmediatest"
#endif
static int failed,checks,done,paint_ready,cancels,requested,completed;
static web_doc *doc;
static struct {bool used;uint64_t id,due;char url[256];} queue[8];
static uint32_t pixels[320*240];
static uint32_t first_frame[96*32];
static uint64_t paint_started;
static int early_frames;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("FAIL avmediatest:%d %s\n",__LINE__,#x);}}while(0)
static int audio_slots(void){int fd[8],n=0;for(;n<8;n++){uint32_t queued=0;fd[n]=open("/dev/audio",O_RDWR|O_NONBLOCK);if(fd[n]<0)break;if(read(fd[n],&queued,4)!=4){close(fd[n]);break;}}for(int i=0;i<n;i++)close(fd[i]);return n;}
static void console(void *opaque,int level,const char *text){
    (void)opaque;
    if(!strncmp(text,"AVMEDIA_DONE ",13))done=atoi(text+13);
    if(!strcmp(text,"AVMEDIA_PAINT_READY")){paint_ready++;paint_started=uptime_ms();}
    if(level==2||!strncmp(text,"FAIL",4)){failed++;printf("%s\n",text);}
}
static bool request(void *opaque,const struct web_request *r){
    (void)opaque;if(r->kind!=WEB_RESOURCE_FETCH)return false;
    for(int i=0;i<8;i++)if(!queue[i].used){queue[i].used=true;queue[i].id=r->id;queue[i].due=uptime_ms()+20;strlcpy(queue[i].url,r->url,sizeof queue[i].url);requested++;return true;}
    return false;
}
static void cancel(void *opaque,uint64_t id){(void)opaque;for(int i=0;i<8;i++)if(queue[i].used&&queue[i].id==id){queue[i].used=false;cancels++;}}
static void deliver(void){
    for(int i=0;i<8;i++)if(queue[i].used&&queue[i].due<=uptime_ms()){
        queue[i].used=false;struct web_response response={0};strlcpy(response.url,queue[i].url,sizeof response.url);
        const char *name=strrchr(queue[i].url,'/');name=name?name+1:"";
        bool overlong=!strcmp(name,"overlong.avi");char path[256];snprintf(path,sizeof path,"/data/tests/media-fixtures/%s",overlong?"mjpeg.avi":name);
        FILE *f=fopen(path,"rb");response.status=f?200:404;
        if(f){fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n>0&&n<131072){response.body=malloc((size_t)n);if(response.body){response.body_len=fread(response.body,1,(size_t)n,f);}}fclose(f);}
        /* Own fixture, unchanged compressed frames: falsify AVI frame-count
         * metadata to 1,000,000 frames (100,000 seconds at 10 fps). */
        if(overlong&&response.body)for(size_t at=12;at+64<response.body_len;at++){
            size_t field=0;
            if(!memcmp(response.body+at,"avih",4))field=at+24;
            if(!memcmp(response.body+at,"strh",4)&&!memcmp(response.body+at+8,"vids",4))field=at+40;
            if(field){response.body[field]=0x40;response.body[field+1]=0x42;response.body[field+2]=0x0f;response.body[field+3]=0;}
        }
        snprintf(response.headers,sizeof response.headers,"HTTP/1.1 %d OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\n\r\n",response.status,(unsigned)response.body_len);
        web_resource_loaded(doc,queue[i].id,&response);completed++;
        if(response.body)memset(response.body,0,response.body_len);web_response_free(&response);
    }
}
static void step(void){
    deliver();web_tick(doc,uptime_ms());
    if(web_dirty(doc)||paint_ready){web_layout(doc,320,240);canvas_t c;gfx_init(&c,pixels,320,240,320);web_paint(doc,&c,0,0,320,240,0,0);}
    if(paint_ready&&uptime_ms()-paint_started<70){
        bool same=true;for(int y=0;y<32;y++)if(memcmp(pixels+y*320,first_frame+y*96,96*4)){same=false;break;}
        CHECK(same);early_frames++; /* Future 100ms decoder frame must not overwrite presentation. */
    }
    msleep(2);
}
static void first_picture(void){
    char error[160];nmedia *m=nmedia_open("/data/tests/media-fixtures/" AVMEDIA_FRAME_FIXTURE,error,sizeof error);CHECK(m!=NULL);if(!m)return;
    struct nmedia_output out;int r=NMEDIA_AGAIN;for(int i=0;i<100&&r==NMEDIA_AGAIN;i++)r=nmedia_step(m,&out);
    CHECK(r==NMEDIA_VIDEO&&out.pts_ms==0);
    canvas_t c;gfx_init(&c,first_frame,96,32,96);gfx_fill(&c,0,0,96,32,RGB(0,0,0));
    if(r==NMEDIA_VIDEO)nmedia_draw(&c,out.pixels,out.width,out.height,0,0,96,32);nmedia_close(m);
}
int main(void){
    int slots=audio_slots();first_picture();
    FILE *f=fopen(AVMEDIA_CASES_SCRIPT,"rb");CHECK(f!=NULL);if(!f)return 1;
    char *script=malloc(32768);CHECK(script!=NULL);if(!script){fclose(f);return 1;}
    size_t n=fread(script,1,32767,f);script[n]=0;fclose(f);
    const char *prefix="<!doctype html><style>body{margin:0;background:white}video{display:block;width:96px;height:64px}</style><body><video id=visible controls></video><script>";
    const char *tail=";runAvmediaCases().catch(e=>console.error('FAIL '+e+' '+e.stack));</script>";
    size_t size=strlen(prefix)+n+strlen(tail)+100;char *page=malloc(size);CHECK(page!=NULL);if(!page){free(script);return 1;}
    snprintf(page,size,"%svar nativeAudioAvailable=%s;%s%s",prefix,slots>0?"true":"false",script,tail);free(script);
    struct web_host host={.request=request,.cancel=cancel,.console=console};doc=web_live(page,strlen(page),"https://avmedia.test/","utf-8",&host);free(page);CHECK(doc!=NULL);if(!doc)return 1;
    web_layout(doc,320,240);uint64_t deadline=uptime_ms()+20000;
    while(!done&&!failed&&uptime_ms()<deadline)step();
    CHECK(done>=17);CHECK(paint_ready==1);CHECK(early_frames>=2);CHECK(requested>=4&&completed>=4);
    bool colored=false;for(int i=0;i<96*64;i++)if(pixels[(i/96)*320+i%96]!=RGB(0,0,0)&&pixels[(i/96)*320+i%96]!=RGB(255,255,255))colored=true;
    CHECK(colored);web_free(doc);doc=NULL;CHECK(audio_slots()==slots);
    /* Free while fetch is pending: cancellation, then never deliver to dead doc. */
    memset(queue,0,sizeof queue);requested=0;
    const char *pending="<script>var a=document.createElement('video');a.preload='none';a.src='" AVMEDIA_FRAME_FIXTURE "';a.play().catch(()=>{});</script>";
    doc=web_live(pending,strlen(pending),"https://avmedia.test/","utf-8",&host);CHECK(doc!=NULL);
    if(doc){deadline=uptime_ms()+3000;while(!requested&&uptime_ms()<deadline){web_tick(doc,uptime_ms());msleep(1);}CHECK(requested==1);int before=cancels;web_free(doc);doc=NULL;CHECK(cancels==before+1);}
    CHECK(audio_slots()==slots);
    printf(AVMEDIA_TEST_NAME ": %d checks, %d failed; audio_slots=%d\n",checks,failed,slots);return failed!=0;
}
