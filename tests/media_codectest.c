/* Real compressed fixtures -> actual vendor decoders -> native PCM/frame API.
 * Synthetic clips are regression input, not proof of any real website. */
#include "media.h"
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("FAIL media_codectest:%d %s\n",__LINE__,#x); } } while(0)
static void decode(const char *name, bool audio, bool video, int min_frames, int max_frames) {
    char path[256], error[160]; snprintf(path,sizeof path,"/data/tests/media-fixtures/%s",name);
    nmedia *m=nmedia_open(path,error,sizeof error);
    CHECK(m!=NULL); if(!m){printf("%s: %s\n",name,error);return;}
    const struct nmedia_info *info=nmedia_get_info(m);
    CHECK(info&&info->audio==audio&&info->video==video);
    int output=0, blocks=0, pictures=0; int64_t energy=0, lasta=INT64_MIN,lastv=INT64_MIN;
    int result=0;
    for(int budget=0;budget<1000;budget++) {
        struct nmedia_output out;result=nmedia_step(m,&out);
        if(result==NMEDIA_END||result==NMEDIA_ERROR)break;
        if(result==NMEDIA_AUDIO){
            CHECK(out.frames>0&&out.frames<=4096&&out.samples!=NULL);
            CHECK(out.pts_ms>=lasta);lasta=out.pts_ms;blocks++;output+=(int)out.frames;
            for(size_t i=0;i<out.frames*2;i++)energy+=abs(out.samples[i]);
        } else if(result==NMEDIA_VIDEO){
            CHECK(out.width==96&&out.height==64&&out.pixels!=NULL);
            CHECK(out.pts_ms>=lastv);lastv=out.pts_ms;pictures++;
            bool colored=false;for(int i=0;i<96*64;i++){CHECK((out.pixels[i]>>24)==255);if((out.pixels[i]&0xffffff)!=0)colored=true;}
            CHECK(colored);
        }
    }
    if(result==NMEDIA_ERROR)printf("%s decode error: %s\n",name,nmedia_error(m));
    CHECK(result==NMEDIA_END);
    CHECK(audio?(blocks>0&&energy>1000000&&output>=min_frames&&output<=max_frames):output==0);
    CHECK(video?pictures==4:pictures==0);
    printf("MEDIA_DECODE %s audio_frames=%d video_frames=%d energy=%lld\n",name,output,pictures,(long long)energy);
    if(!strcmp(name,"stereo.wav")||!strcmp(name,"stereo.flac")||!strcmp(name,"h264.mp4")||!strcmp(name,"av.mp4")){
        CHECK(nmedia_seek(m,100));int r=0;struct nmedia_output out;
        for(int i=0;i<100&&r!=NMEDIA_AUDIO&&r!=NMEDIA_VIDEO&&r!=NMEDIA_ERROR&&r!=NMEDIA_END;i++)r=nmedia_step(m,&out);
        CHECK(audio&&video?(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO):r==(audio?NMEDIA_AUDIO:NMEDIA_VIDEO));
        if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO)CHECK(out.pts_ms>=100);
        if(audio&&video){
            bool gota=r==NMEDIA_AUDIO,gotv=r==NMEDIA_VIDEO;
            for(int i=0;i<100&&(!gota||!gotv);i++){r=nmedia_step(m,&out);if(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO){CHECK(out.pts_ms>=100);gota|=r==NMEDIA_AUDIO;gotv|=r==NMEDIA_VIDEO;}if(r==NMEDIA_ERROR||r==NMEDIA_END)break;}
            CHECK(gota&&gotv); /* each stream must honor its own seek target */
        }
        CHECK(nmedia_seek(m,0));r=0;
        for(int i=0;i<100&&r==NMEDIA_AGAIN;i++)r=nmedia_step(m,&out);
        CHECK(audio&&video?(r==NMEDIA_AUDIO||r==NMEDIA_VIDEO):r==(audio?NMEDIA_AUDIO:NMEDIA_VIDEO));
    }
    nmedia_close(m);
}
static void memory_cases(void){
    const char *path="/data/tests/media-fixtures/stereo.flac";FILE *f=fopen(path,"rb");CHECK(f!=NULL);if(!f)return;
    fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);CHECK(size>0&&size<65536);
    uint8_t *data=malloc((size_t)size);CHECK(data!=NULL);if(!data){fclose(f);return;}
    CHECK(fread(data,1,(size_t)size,f)==(size_t)size);fclose(f);
    char error[160];nmedia *m=nmedia_open_memory(data,(size_t)size,error,sizeof error);CHECK(m!=NULL);
    memset(data,0,(size_t)size);free(data); /* native handle must own a copy */
    if(m){
        struct nmedia_output out;int r=0;
        for(int i=0;i<1000&&r!=NMEDIA_END&&r!=NMEDIA_ERROR;i++)r=nmedia_step(m,&out);
        CHECK(r==NMEDIA_END);CHECK(nmedia_seek(m,100));r=0;
        for(int i=0;i<100&&r==NMEDIA_AGAIN;i++)r=nmedia_step(m,&out);
        CHECK(r==NMEDIA_AUDIO&&out.pts_ms>=100);
        CHECK(nmedia_seek(m,0));r=0;
        for(int i=0;i<100&&r==NMEDIA_AGAIN;i++)r=nmedia_step(m,&out);
        CHECK(r==NMEDIA_AUDIO&&out.pts_ms==0);nmedia_close(m);
    }
    const uint8_t tiny[4]={0};
    CHECK(nmedia_open_memory(tiny,NMEDIA_MAX_BYTES+1,error,sizeof error)==NULL);
    CHECK(nmedia_open_memory(tiny,0,error,sizeof error)==NULL);
    CHECK(nmedia_open_memory(tiny,sizeof tiny,error,sizeof error)==NULL);
    CHECK(nmedia_open("/data/tests/media-fixtures/invalid.mp4",error,sizeof error)==NULL);
    CHECK(nmedia_open("/data/tests/media-fixtures/not-found",error,sizeof error)==NULL);
    nmedia_close(NULL);CHECK(nmedia_get_info(NULL)==NULL);
}
static void capabilities(void){
    CHECK(!strcmp(nmedia_can_play_type("audio/mpeg"),"maybe"));
    CHECK(!strcmp(nmedia_can_play_type("audio/mpeg; codecs=mp3"),"probably"));
    CHECK(!strcmp(nmedia_can_play_type("video/mp4; codecs=\"avc1.42e01e, mp4a.40.2\""),"probably"));
    CHECK(!strcmp(nmedia_can_play_type("audio/flac; codecs=flac"),"probably"));
    CHECK(!*nmedia_can_play_type("video/mp4; codecs=\"avc1.6e001e\""));
    CHECK(!strcmp(nmedia_can_play_type("video/webm; codecs=vp9"),"probably"));
    CHECK(!*nmedia_can_play_type("application/vnd.apple.mpegurl"));
    CHECK(!*nmedia_can_play_type("video/mp4; codecs=\"avc1.42e01e,\""));
    CHECK(!*nmedia_can_play_type("audio/mp4; codecs=avc1.42e01e"));
    CHECK(!*nmedia_can_play_type("audio/wav; codecs=99"));
    CHECK(nmedia_audio_extension("SONG.FLAC")&&!nmedia_audio_extension("movie.webm"));
}
static void le16(uint8_t *p,unsigned v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void le32(uint8_t *p,unsigned v){le16(p,v);le16(p+2,v>>16);}
static void downmix_cases(void){
    /* Decoder-only PCM, never a synthetic webpage or real-site acceptance.
     * 5.1 speaker mask: FL, FR, FC, LFE, BL, BR; isolate each input channel. */
    uint8_t wav[68+16*6*2]={0};char error[160];
    memcpy(wav,"RIFF",4);le32(wav+4,sizeof wav-8);memcpy(wav+8,"WAVEfmt ",8);
    le32(wav+16,40);le16(wav+20,0xfffe);le16(wav+22,6);le32(wav+24,48000);
    le32(wav+28,48000*12);le16(wav+32,12);le16(wav+34,16);le16(wav+36,22);
    le16(wav+38,16);le32(wav+40,0x3f);
    const uint8_t pcm_guid[16]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
    memcpy(wav+44,pcm_guid,16);memcpy(wav+60,"data",4);le32(wav+64,sizeof wav-68);
    for(int channel=0;channel<6;channel++){
        memset(wav+68,0,sizeof wav-68);
        for(int frame=0;frame<16;frame++)le16(wav+68+(frame*6+channel)*2,12000);
        nmedia *m=nmedia_open_memory(wav,sizeof wav,error,sizeof error);CHECK(m!=NULL);if(!m)continue;
        struct nmedia_output out;int r=NMEDIA_AGAIN,total=0;int64_t energy[2]={0};
        for(int budget=0;budget<50;budget++){
            r=nmedia_step(m,&out);if(r==NMEDIA_END||r==NMEDIA_ERROR)break;
            if(r==NMEDIA_AUDIO){total+=(int)out.frames;for(size_t i=0;i<out.frames;i++)for(int c=0;c<2;c++)energy[c]+=abs(out.samples[i*2+c]);}
        }
        CHECK(r==NMEDIA_END&&total==16);
        CHECK(channel==0||channel==4 ? energy[0]>0&&energy[1]==0 :
              channel==1||channel==5 ? energy[0]==0&&energy[1]>0 : energy[0]>0&&energy[0]==energy[1]);
        printf("MEDIA_DOWNMIX channel=%d energy=%lld/%lld\n",channel,(long long)energy[0],(long long)energy[1]);
        nmedia_close(m);
    }
    /* Top-back speakers have no stereo coefficients in this bounded adapter.
     * Preserve rejection rather than successful audio missing two channels. */
    le32(wav+40,0x2800f);
    nmedia *m=nmedia_open_memory(wav,sizeof wav,error,sizeof error);
    bool rejected=!m;
    if(m){struct nmedia_output out;int r=NMEDIA_AGAIN;for(int i=0;i<50&&r==NMEDIA_AGAIN;i++)r=nmedia_step(m,&out);rejected=r==NMEDIA_ERROR;nmedia_close(m);}
    CHECK(rejected);
}
static void drawing(void){
    uint32_t guard[40], src[4]={0xff112233,0xff223344,0xff334455,0xff445566};
    for(int i=0;i<40;i++)guard[i]=0xdeadbeef;
    canvas_t c={.px=guard+8,.w=4,.h=4,.pitch=6,.cx0=1,.cy0=1,.cx1=3,.cy1=3};
    nmedia_draw(&c,src,2,2,0,0,4,4);
    for(int i=0;i<40;i++){
        int at=i-8,y=at/6,x=at%6;bool drawn=at>=0&&y>=1&&y<3&&x>=1&&x<3;
        CHECK(drawn?guard[i]!=0xdeadbeef:guard[i]==0xdeadbeef);
    }
    uint32_t old[40];memcpy(old,guard,sizeof old);
    nmedia_draw(&c,src,2,2,INT_MAX,INT_MAX,INT_MAX,INT_MAX);
    nmedia_draw(&c,src,INT_MAX,INT_MAX,0,0,4,4);
    CHECK(!memcmp(old,guard,sizeof old));
}
static void audio_flush_cases(void){
    errno=0;CHECK(audio_flush(-1)<0&&errno==EBADF);
    int other=open("/data/tests/media-fixtures/stereo.wav",O_RDONLY);CHECK(other>=0);
    if(other>=0){errno=0;CHECK(audio_flush(other)<0&&errno==EINVAL);close(other);}
    int fd=open("/dev/audio",O_RDWR|O_NONBLOCK);CHECK(fd>=0);if(fd<0)return;
    uint32_t queued=0;CHECK(read(fd,&queued,4)==4&&queued==0);
    int16_t *silence=calloc(8192,4);CHECK(silence!=NULL);if(!silence){close(fd);return;}
    CHECK(write(fd,silence,8192*4)==8192*4);
    CHECK(read(fd,&queued,4)==4&&queued>0); /* real native ring, not a callback stub */
    int alias=dup(fd);CHECK(alias>=0);
    CHECK(audio_flush(alias>=0?alias:fd)==0);CHECK(read(fd,&queued,4)==4&&queued==0);
    CHECK(write(fd,silence,4096*4)==4096*4);CHECK(audio_flush(fd)==0);
    CHECK(read(fd,&queued,4)==4&&queued==0);free(silence);if(alias>=0)close(alias);close(fd);
}
int main(void){
    decode("stereo.wav",true,false,9600,9600);decode("stereo.flac",true,false,9600,9600);
    decode("stereo.mp3",true,false,9600,12000);decode("stereo.aac",true,false,9600,13000);
    decode("mjpeg.avi",false,true,0,0);decode("h264.mp4",false,true,0,0);decode("av.mp4",true,true,9600,13000);
    decode("unsupported.webm",false,true,0,0); /* Kept legacy filename: actual VP8. */
    memory_cases();capabilities();downmix_cases();drawing();audio_flush_cases();
    printf("media_codectest: %d checks, %d failed\n",checks,failed);return failed!=0;
}
