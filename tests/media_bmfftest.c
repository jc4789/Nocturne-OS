#include "media.h"
#include "media_mse_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL BMFF: %s\n",name);}}
static const struct {const char *file,*mime,*codec;unsigned frames,pictures;} fixtures[]={
    {"mp4-opus.mp4","audio/mp4; codecs=opus","opus",10248,0},
    {"mp4-ac3-delayed.mp4","audio/mp4; codecs=ac-3","ac3",10752,0},
    {"mp4-eac3-delayed.mp4","audio/mp4; codecs=ec-3","eac3",10752,0},
    {"mp4-vp9.mp4","video/mp4; codecs=vp09.00.10.08","vp9",0,4},
    {"mp4-hevc.mp4","video/mp4; codecs=hvc1.1.6.L30.B0","hevc",0,4},
    {"mp4-hevc10.mp4","video/mp4; codecs=hvc1.2.4.L30.B0","hevc",0,4},
};
int main(void){
    for(unsigned i=0;i<sizeof fixtures/sizeof *fixtures;i++){
        char path[256],error[160];snprintf(path,sizeof path,"/data/tests/media-fixtures/%s",fixtures[i].file);
        check(!strcmp(nmedia_can_play_type(fixtures[i].mime),"probably"),"built codec capability");
        FILE *f=fopen(path,"rb");check(f!=NULL,"fixture opens");if(!f)continue;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
        check(size>0&&size<65536,"finite fixture size");if(size<=0||size>=65536){fclose(f);continue;}
        void *data=malloc((size_t)size);check(data!=NULL,"fixture allocation");if(!data){fclose(f);continue;}
        check(fread(data,1,(size_t)size,f)==(size_t)size,"fixture read");fclose(f);
        nmedia *direct=nmedia_open_memory(data,(size_t)size,error,sizeof error);check(direct!=NULL,"real BMFF decoder opens");
        nmedia_mse *mse=nmedia_mse_create(fixtures[i].mime,error,sizeof error);check(mse!=NULL,"real MSE stream opens");
        if(mse){check(nmedia_mse_append(mse,data,(size_t)size,0,0,INT64_MAX,false),"whole fragmented BMFF append");check(nmedia_mse_end(mse,true),"MSE EOS");
            if(!strcmp(fixtures[i].codec,"vp9")){
                struct nmedia_time_range ranges[8];size_t count=nmedia_mse_ranges(mse,ranges,8);
                check(count==1&&ranges[0].start_ms==0&&ranges[0].end_ms==400,"MP4 sample durations preserve contiguous 10fps range");
            }
        }
        free(data);
        for(unsigned mode=0;mode<2;mode++){
            if(mode?!mse:!direct)continue;
            const struct nmedia_info *info=mode?nmedia_mse_info(mse):nmedia_get_info(direct);
            check(info&&(!strcmp(fixtures[i].codec,fixtures[i].pictures?info->video_codec:info->audio_codec)),"real codec metadata");
            unsigned frames=0,pictures=0;uint64_t energy=0;int kind=NMEDIA_AGAIN;int64_t last=-1000000;
            for(unsigned work=0;work<2000;work++){
                struct nmedia_output o;kind=mode?nmedia_mse_step(mse,&o):nmedia_step(direct,&o);
                if(kind==NMEDIA_END||kind==NMEDIA_ERROR)break;if(kind==NMEDIA_AGAIN)continue;
                check(o.pts_ms>=last,"authored timestamps monotonic");last=o.pts_ms;
                if(kind==NMEDIA_AUDIO){frames+=(unsigned)o.frames;for(size_t j=0;j<o.frames*2;j++)energy+=(unsigned)abs(o.samples[j]);}
                if(kind==NMEDIA_VIDEO){pictures++;check(o.width==96&&o.height==64&&o.pixels,"native pixels exist");bool color=false;for(unsigned j=0;j<96*64;j++)color|=(o.pixels[j]&0xffffff)!=0;check(color,"decoded picture is not blank");}
            }
            if(kind==NMEDIA_ERROR)printf("BMFF_ERROR %s mode %u: %s\n",fixtures[i].file,mode,mode?nmedia_mse_error(mse):nmedia_error(direct));
            check(kind==NMEDIA_END,"real codec reaches EOS");check(pictures==fixtures[i].pictures,"all pictures decoded");
            if(!mode)check(frames==fixtures[i].frames,"PCM matches independent host frame count");
            else check(fixtures[i].frames?frames>0&&frames<=fixtures[i].frames:frames==0,"MSE append-window PCM output");
            if(fixtures[i].frames)check(energy>1000000,"decoded stereo audio energy");
            printf("BMFF %s mode %u audio %u video %u energy %llu\n",fixtures[i].file,mode,frames,pictures,(unsigned long long)energy);
        }
        nmedia_close(direct);nmedia_mse_close(mse);
    }
    check(!*nmedia_can_play_type("video/mp4; codecs=av01.0.04M.08"),"unbuilt AV1 remains rejected");
    check(!*nmedia_can_play_type("video/mp4; codecs=vp09.02.10.10.01.09.16.09.01"),"HDR capability remains rejected");
    printf("media_bmfftest: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
