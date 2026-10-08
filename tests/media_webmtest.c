/* Real WebM/Ogg compressed inputs, including Opus SILK and CELT paths.
 * The host-reference frame totals are recorded in webm-manifest.json. */
#include "media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static int checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("FAIL media_webmtest:%d %s\n",__LINE__,#x);}}while(0)
static const struct fixture {const char *name,*audio_codec;bool video;int frames;const char *video_codec;} fixtures[]={
    {"stereo.opus","opus",false,9600},{"stereo-silk.opus","opus",false,9600},
    {"stereo.ogg","vorbis",false,9472},{"opus.webm","opus",false,9600},
    {"vorbis.webm","vorbis",false,9600},{"vp9.webm",NULL,true,0},
    {"vp9-opus.webm","opus",true,9600},{"vp9-vorbis.webm","vorbis",true,9600},
    {"unsupported.webm",NULL,true,0,"vp8"}, /* Original VP8 fixture bytes retained. */
};
static uint8_t *read_fixture(const char *name,size_t *size){
    char path[256];snprintf(path,sizeof path,"/data/tests/media-fixtures/%s",name);
    FILE *f=fopen(path,"rb");CHECK(f!=NULL);if(!f)return NULL;
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);CHECK(n>0&&n<65536);
    if(n<=0||n>=65536){fclose(f);return NULL;}
    uint8_t *p=malloc((size_t)n);CHECK(p!=NULL);
    if(p)CHECK(fread(p,1,(size_t)n,f)==(size_t)n);
    fclose(f);*size=(size_t)n;return p;
}
static void decode(const struct fixture *fixture,bool memory){
    char path[256],error[160];snprintf(path,sizeof path,"/data/tests/media-fixtures/%s",fixture->name);
    nmedia *m=NULL;
    if(memory){size_t n=0;uint8_t *p=read_fixture(fixture->name,&n);if(!p)return;
        m=nmedia_open_memory(p,n,error,sizeof error);memset(p,0,n);free(p);
    }else m=nmedia_open(path,error,sizeof error);
    CHECK(m!=NULL);if(!m){printf("WEBM_OPEN_ERROR %s %s\n",fixture->name,error);return;}
    const struct nmedia_info *info=nmedia_get_info(m);
    CHECK(info&&info->audio==(fixture->audio_codec!=NULL)&&info->video==fixture->video);
    if(fixture->audio_codec)CHECK(!strcmp(info->audio_codec,fixture->audio_codec));
    if(fixture->video)CHECK(!strcmp(info->video_codec,fixture->video_codec?fixture->video_codec:"vp9")&&info->width==96&&info->height==64);
    int frames=0,pictures=0,r=NMEDIA_AGAIN;int64_t energy[2]={0},lasta=INT64_MIN,lastv=INT64_MIN;
    struct nmedia_output out;
    for(int budget=0;budget<1000;budget++){
        r=nmedia_step(m,&out);if(r==NMEDIA_END||r==NMEDIA_ERROR)break;
        if(r==NMEDIA_AUDIO){
            CHECK(out.frames>0&&out.frames<=4096&&out.samples);CHECK(out.pts_ms>=lasta);lasta=out.pts_ms;
            /* Actual inputs start at negative Opus pre-roll PTS. Automatic
             * pre-skip must produce zero PTS and exactly 9600 output frames,
             * not apply the inactive (-1ms) seek sentinel as another trim. */
            if(!frames&&fixture->audio_codec&&!strcmp(fixture->audio_codec,"opus"))CHECK(out.pts_ms==0);
            frames+=(int)out.frames;for(size_t i=0;i<out.frames;i++)for(int c=0;c<2;c++)energy[c]+=abs(out.samples[2*i+c]);
        }else if(r==NMEDIA_VIDEO){
            CHECK(out.width==96&&out.height==64&&out.pixels);CHECK(out.pts_ms>=lastv);lastv=out.pts_ms;pictures++;
            bool colored=false;for(int i=0;i<96*64;i++){CHECK((out.pixels[i]>>24)==255);colored|=(out.pixels[i]&0xffffff)!=0;}CHECK(colored);
        }
    }
    if(r==NMEDIA_ERROR)printf("WEBM_DECODE_ERROR %s %s\n",fixture->name,nmedia_error(m));
    CHECK(r==NMEDIA_END);CHECK(frames==fixture->frames);CHECK(pictures==(fixture->video?4:0));
    if(fixture->audio_codec)CHECK(energy[0]>1000000&&energy[1]>1000000);
    printf("WEBM_DECODE %s memory=%d audio_frames=%d video_frames=%d energy=%lld/%lld\n",fixture->name,memory,frames,pictures,(long long)energy[0],(long long)energy[1]);
    CHECK(nmedia_seek(m,100));bool gota=!fixture->audio_codec,gotv=!fixture->video;
    for(int i=0;i<1000&&(!gota||!gotv);i++){
        r=nmedia_step(m,&out);
        if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO){CHECK(out.pts_ms>=100);if(r==NMEDIA_AUDIO)gota=true;else gotv=true;}
        if(r==NMEDIA_END||r==NMEDIA_ERROR)break;
    }
    CHECK(gota&&gotv);CHECK(nmedia_seek(m,0));r=NMEDIA_AGAIN;
    for(int i=0;i<1000&&r==NMEDIA_AGAIN;i++)r=nmedia_step(m,&out);
    CHECK(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO);CHECK(out.pts_ms>=-20&&out.pts_ms<=20);
    nmedia_close(m);
}
static void reject(const char *name){
    char path[256],error[160];snprintf(path,sizeof path,"/data/tests/media-fixtures/%s",name);
    nmedia *m=nmedia_open(path,error,sizeof error);bool rejected=!m;
    if(m){struct nmedia_output out;int r=NMEDIA_AGAIN;
        for(int i=0;i<1000&&r==NMEDIA_AGAIN;i++)r=nmedia_step(m,&out);
        rejected=r==NMEDIA_ERROR;nmedia_close(m);
    }
    CHECK(rejected);printf("WEBM_REJECT %s rejected=%d\n",name,rejected);
}
static void capabilities(void){
    const char *positive[]={"video/webm; codecs=vp9","video/webm; codecs=vp9.0",
        "video/webm; codecs=vp8","video/webm; codecs=vp8.0","video/webm; codecs=\"vp8, vorbis\"",
        "video/webm; codecs=\"vp09.00.10.08, opus\"","video/webm; codecs=\"vp9, vorbis\"",
        "video/webm; codecs=vp09.00.41.08.01.01.01.01.00",
        "audio/webm; codecs=opus","audio/webm; codecs=vorbis","audio/ogg; codecs=opus","audio/ogg; codecs=vorbis"};
    const char *negative[]={"audio/webm; codecs=vp8","video/webm; codecs=vp8.1","video/webm; codecs=av01.0.04M.08",
        "video/webm; codecs=vp09.02.10.10","video/webm; codecs=vp09.01.10.08",
        "video/webm; codecs=vp09.00.99.08","video/webm; codecs=vp09.00.10.08.01",
        "video/webm; codecs=vp09.00.10.08.01.09.16.09.01","video/webm; codecs=vp09.00.10.08junk",
        "video/webm; codecs=\"vp9,\"","audio/webm; codecs=vp9","video/ogg; codecs=theora",
        "audio/ogg; codecs=speex","audio/ogg; codecs=\"opus\"; codecs=vorbis",
        "application/vnd.apple.mpegurl","video/mp4; codecs=av01.0.04M.08"};
    for(size_t i=0;i<sizeof positive/sizeof *positive;i++)CHECK(!strcmp(nmedia_can_play_type(positive[i]),"probably"));
    for(size_t i=0;i<sizeof negative/sizeof *negative;i++)CHECK(!*nmedia_can_play_type(negative[i]));
    CHECK(!strcmp(nmedia_can_play_type("audio/webm"),"maybe"));CHECK(!strcmp(nmedia_can_play_type("audio/ogg"),"maybe"));
    CHECK(nmedia_audio_extension("SONG.OPUS")&&nmedia_audio_extension("song.ogg")&&!nmedia_audio_extension("movie.webm"));
}
int main(void){
    for(size_t i=0;i<sizeof fixtures/sizeof *fixtures;i++){decode(&fixtures[i],false);decode(&fixtures[i],true);}
    reject("vp9-high10.webm");reject("unsupported-av1.webm");
    size_t n=0;uint8_t *p=read_fixture("vp9-opus.webm",&n);if(p){char error[160];CHECK(nmedia_open_memory(p,16,error,sizeof error)==NULL);free(p);}
    capabilities();printf("media_webmtest: %d checks, %d failed\n",checks,failed);return failed!=0;
}
