#include "media_rate_private.h"
#include "nocturne.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned failures,checks;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL: %s\n",name);}}
static void run(double rate,bool pitch){
    nmedia_rate *r=nmedia_rate_create(rate,pitch);check(r!=NULL,"native tempo creates");if(!r)return;
    int16_t samples[1024*2];size_t output=0;unsigned crosses=0;int previous=0;int64_t stamp=-1;bool valid=true;
    for(size_t at=0;at<48000;at+=1024){size_t frames=48000-at;if(frames>1024)frames=1024;
        for(size_t i=0;i<frames;i++){int16_t value=(int16_t)(12000*sin((at+i)*440.0*2*3.141592653589793/48000));samples[i*2]=samples[i*2+1]=value;}
        struct nmedia_output input={.kind=NMEDIA_AUDIO,.pts_ms=(int64_t)(at*1000/48000),.samples=samples,.frames=frames};
        if(!nmedia_rate_push(r,&input)){valid=false;break;}
        for(;;){struct nmedia_output o;int kind=nmedia_rate_pull(r,&o);if(kind==NMEDIA_AGAIN)break;if(kind!=NMEDIA_AUDIO){valid=false;break;}
            if(o.pts_ms<stamp||o.frames>4096)valid=false;stamp=o.pts_ms;
            for(size_t i=0;i<o.frames;i++){int value=o.samples[i*2];if(previous<=0&&value>0)crosses++;previous=value;}output+=o.frames;}
    }
    check(nmedia_rate_end(r),"tempo EOS flush");
    for(;;){struct nmedia_output o;int kind=nmedia_rate_pull(r,&o);if(kind==NMEDIA_END)break;if(kind!=NMEDIA_AUDIO){valid=false;break;}
        if(o.pts_ms<stamp||o.frames>4096)valid=false;stamp=o.pts_ms;
        for(size_t i=0;i<o.frames;i++){int value=o.samples[i*2];if(previous<=0&&value>0)crosses++;previous=value;}output+=o.frames;}
    check(valid,"tempo bounds and source PTS monotonic");
    double expected=48000/rate;check(fabs(output-expected)<2048,"tempo sample duration within one WSOLA analysis window");
    double frequency=output?(double)crosses*48000/output:0;double target=pitch?440:440*rate;
    check(fabs(frequency-target)<target*0.03,"pitch preservation or intentional pitch change");
    printf("RATE %.2f pitch %u frames %zu frequency %.1f\n",rate,pitch,output,frequency);nmedia_rate_close(r);
}
int main(void){run(0.5,true);run(2,true);run(0.5,false);run(2,false);check(!nmedia_rate_create(0,true),"zero rate freezes outside DSP");check(!nmedia_rate_create(-1,true),"reverse rejected");printf("mediaratetest: %u checks, %u failures\n",checks,failures);return failures?1:0;}
