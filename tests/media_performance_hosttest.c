/* Actual product MIME/configuration matcher; no mock supported/smooth result. */
#include "../user/libc/media_types.c"
#include <stdio.h>
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("failed line %d\n",__LINE__);}}while(0)
int main(void){
    struct nmedia_video_configuration a,b;uint32_t codec,profile;
    CHECK(nmedia_performance_codec("video/mp4; codecs=\"avc1.42e01e\"",&a));
    CHECK(a.codec==NMEDIA_VCODEC_AVC&&a.profile==66&&a.level==30&&a.depth==8&&a.compatibility==224);
    CHECK(nmedia_performance_codec("video/mp4; codecs=avc1.640028",&b));
    CHECK(!nmedia_video_configuration_equal(&a,&b));
    CHECK(!nmedia_performance_codec("video/webm; codecs=vp9",&a));
    CHECK(nmedia_performance_codec("video/webm; codecs=vp09.02.30.10",&a));
    CHECK(a.codec==NMEDIA_VCODEC_VP9&&a.profile==2&&a.depth==10);
    CHECK(nmedia_performance_codec("video/webm; codecs=vp09.00.30.08",&b));
    CHECK(!nmedia_video_configuration_equal(&a,&b));
    CHECK(nmedia_performance_codec("video/webm; codecs=vp8",&a));
    CHECK(a.codec==NMEDIA_VCODEC_VP8&&a.depth==8);
    b=a;b.bitrate=200000;CHECK(nmedia_video_configuration_equal(&a,&b));
    CHECK(!nmedia_performance_codec("video/mp4; codecs=hvc1.2.4.L120.B0",&a));
    CHECK(!nmedia_performance_codec("video/mp4; codecs=\"avc1.640028,mp4a.40.2\"",&a));
    CHECK(nmedia_performance_audio("audio/mp4; codecs=mp4a.40.2",&codec,&profile));
    CHECK(codec==NMEDIA_ACODEC_AAC&&profile==1);
    CHECK(nmedia_performance_audio("audio/mp4; codecs=mp4a.40.5",&codec,&profile));
    CHECK(codec==NMEDIA_ACODEC_AAC&&profile==4);
    CHECK(nmedia_performance_audio("audio/webm; codecs=opus",&codec,&profile));
    CHECK(codec==NMEDIA_ACODEC_OPUS&&profile==0);
    CHECK(!nmedia_performance_audio("audio/mp4; codecs=\"opus,mp4a.40.2\"",&codec,&profile));
    printf("native performance configuration: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
