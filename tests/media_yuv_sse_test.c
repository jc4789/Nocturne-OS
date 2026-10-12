/* Exact native SSE2 converter, not a mock decoder or site-playback claim. */
#ifdef _WIN32
/* Only unused OS syscall declarations need a distinct host-ABI typedef.
   The converter uses actual AVFrame/intrinsic types and no OS syscall. */
#include <corecrt.h>
#define ssize_t nocturne_host_ssize_t
#include "../user/include/nocturne.h"
#undef ssize_t
#endif
#include "../user/libc/media.c"
static unsigned failures,rows_checked,pixels_checked;
static uint32_t random_state=0x315f729bu;
static uint32_t random_word(void){random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static unsigned reference_channel(int value){return value<0?0:value>255?255:(unsigned)value;}
static uint32_t reference(int y,int u,int v,int depth,bool full,const struct video_matrix *matrix){
    int center=1<<(depth-1),offset=16<<(depth-8);int64_t luma=(int64_t)matrix->yc*(full?y:y>offset?y-offset:0),rounding=1<<(depth-1);
    int r=(int)((luma+(int64_t)matrix->rv*(v-center)+rounding)>>depth);
    int g=(int)((luma-(int64_t)matrix->gu*(u-center)-(int64_t)matrix->gv*(v-center)+rounding)>>depth);
    int b=(int)((luma+(int64_t)matrix->bu*(u-center)+rounding)>>depth);
    return 0xff000000u|reference_channel(r)<<16|reference_channel(g)<<8|reference_channel(b);
}
static void put_sample(uint8_t *p,unsigned depth,unsigned value){if(depth>8){uint16_t word=(uint16_t)value;memcpy(p,&word,2);}else *p=(uint8_t)value;}
static unsigned get_sample(const uint8_t *p,unsigned depth){if(depth>8){uint16_t value;memcpy(&value,p,2);return value;}return *p;}
int main(void){
    const int depths[]={8,10,12,16},widths[]={1,7,8,9,17,32,63};
    const enum AVColorSpace spaces[]={AVCOL_SPC_BT470BG,AVCOL_SPC_BT709,AVCOL_SPC_FCC,AVCOL_SPC_SMPTE240M};
    uint8_t planes[3][5*160];uint32_t output[65];
    for(unsigned di=0;di<4;di++)for(unsigned si=0;si<4;si++)for(unsigned sampling=0;sampling<3;sampling++)for(unsigned wi=0;wi<7;wi++)for(unsigned full=0;full<2;full++)for(unsigned negative=0;negative<2;negative++){
        int depth=depths[di],width=widths[wi],height=5,sx=sampling==2?0:1,sy=sampling==0?1:0,unit=depth>8?2:1;
        unsigned maximum=(1u<<depth)-1;AVFrame frame={0};struct video_matrix matrix;
        if(!video_matrix(spaces[si],full,&matrix)){failures++;continue;}
        for(unsigned plane=0;plane<3;plane++){
            int columns=plane?(width+(1<<sx)-1)>>sx:width,rows=plane?(height+(1<<sy)-1)>>sy:height;
            int stride=columns*unit+5;memset(planes[plane],0x6d,sizeof planes[plane]);
            for(int y=0;y<rows;y++)for(int x=0;x<columns;x++){
                unsigned value=random_word()&maximum;
                if(x==0)value=plane?(1u<<(depth-1)):(16u<<(depth-8));
                if(x==1)value=plane?(1u<<(depth-1)):(235u<<(depth-8));
                put_sample(planes[plane]+(size_t)y*stride+x*unit,depth,value);
            }
            frame.data[plane]=planes[plane]+(negative?(size_t)(rows-1)*stride:0);frame.linesize[plane]=negative?-stride:stride;
        }
        for(int row=0;row<height;row++){
            for(unsigned i=0;i<65;i++)output[i]=0x15aa52fdu;
            int converted=video_yuv_row(&frame,row,width,sx,sy,depth,full,&matrix,output+1);rows_checked++;
            if(converted!=width||output[0]!=0x15aa52fdu||output[width+1]!=0x15aa52fdu){failures++;continue;}
            const uint8_t *y=frame.data[0]+(ptrdiff_t)row*frame.linesize[0],*u=frame.data[1]+(ptrdiff_t)(row>>sy)*frame.linesize[1],*v=frame.data[2]+(ptrdiff_t)(row>>sy)*frame.linesize[2];
            for(int x=0;x<width;x++){
                uint32_t expected=reference(get_sample(y+x*unit,depth),get_sample(u+(x>>sx)*unit,depth),get_sample(v+(x>>sx)*unit,depth),depth,full,&matrix);pixels_checked++;
                if(output[x+1]!=expected){if(failures<5)printf("FAIL yuv depth%d sampling%u width%d row%d x%d range%u stride%u got%08x expected%08x\n",depth,sampling,width,row,x,full,negative,output[x+1],expected);failures++;}
            }
        }
    }
    printf("media_yuv_sse_test: %u rows, %u pixels, %u failures\n",rows_checked,pixels_checked,failures);return failures?1:0;
}
