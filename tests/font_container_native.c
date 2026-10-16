/* Host decoder/raster boundary check; not evidence of browser/site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <nocturne.h>
#include <font.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include "../user/libc/font_sfnt.h"
n_result wait_on_address(volatile uint32_t *p,uint32_t value,unsigned timeout) {(void)p;(void)value;(void)timeout;return 0;}
n_result wake_address(volatile uint32_t *p,unsigned count) {(void)p;(void)count;return 0;}
int gfx_utf8_decode(const char *p,uint32_t *cp) {*cp=(unsigned char)*p;return 1;}
void gfx_fill(canvas_t *c,int x,int y,int w,int h,uint32_t color) {
    for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)
        if(i>=c->cx0 && i<c->cx1 && j>=c->cy0 && j<c->cy1)c->px[(size_t)j*c->pitch+i]=color;
}
void gfx_rect(canvas_t *c,int x,int y,int w,int h,uint32_t color) {
    gfx_fill(c,x,y,w,1,color);gfx_fill(c,x,y+h-1,w,1,color);
    gfx_fill(c,x,y,1,h,color);gfx_fill(c,x+w-1,y,1,h,color);
}
static int failures,checks;
static void check(int ok,const char *name) {checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static unsigned char *read_bytes(const char *path,size_t *n) {
    FILE *f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long size=ftell(f);fseek(f,0,SEEK_SET);
    unsigned char *p=size>0?malloc((size_t)size):NULL;
    if(!p || fread(p,1,(size_t)size,f)!=(size_t)size){free(p);p=NULL;}fclose(f);*n=(size_t)size;return p;
}
static uint32_t raster(font_t *font) {
    uint32_t pixels[128*64]={0};canvas_t c={.px=pixels,.w=128,.h=64,.pitch=128,.cx0=0,.cy0=0,.cx1=128,.cy1=64};
    float end=font_draw(&c,font,24,4,36,"AWi",3,0xffffffff);
    check(fabsf(end-4-font_width(font,24,"AWi",3))<.01f,"metrics equal painted advance");
    uint32_t hash=2166136261u;unsigned ink=0;
    for(unsigned i=0;i<128*64;i++){hash=(hash^pixels[i])*16777619u;ink+=pixels[i]!=0;}
    check(ink>0,"actual outline pixels");return hash;
}
static void le32(unsigned char *p,uint32_t n) {for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--font")) {
        size_t length=0;unsigned char *bytes=read_bytes(argv[2],&length);
        font_t *face=bytes?font_open_memory(bytes,length):NULL;
        check(face!=NULL,"original resource decodes");
        if(face) {
            check(font_has_primary(face,'A'),"original primary glyph exists");
            printf("original font raster=%08x bytes=%zu\n",raster(face),length);
        }
        font_close(face);free(bytes);
        printf("font-container-native: %d checks, %d failures\n",checks,failures);
        return failures!=0;
    }
    if(argc!=6)return 2;
    size_t n[5];unsigned char *p[5];font_t *f[5];uint32_t hashes[5]={0};
    for(unsigned i=0;i<5;i++) {
        p[i]=read_bytes(argv[i+1],&n[i]);f[i]=p[i]?font_open_memory(p[i],n[i]):NULL;
        check(f[i]!=NULL,argv[i+1]);if(f[i])hashes[i]=raster(f[i]);
        if(!f[i] && p[i]) {
            FT_Library lib;FT_Face face=NULL;FT_Error err=FT_Init_FreeType(&lib);
            if(!err)err=FT_New_Memory_Face(lib,p[i],(FT_Long)n[i],0,&face);
            printf("decoder diagnostic %s: FreeType error %d\n",argv[i+1],err);
            if(!err) {
                FT_ULong size=0;err=FT_Load_Sfnt_Table(face,0,0,NULL,&size);
                unsigned char *expanded=malloc(size);
                if(!err && expanded)err=FT_Load_Sfnt_Table(face,0,0,expanded,&size);
                printf("expanded %lu bytes: error %d, signature %08x, gate %d\n",size,err,expanded&&size>=4?sf_u32(expanded):0,expanded&&!err?font_sfnt_valid(expanded,size):0);
                if(expanded && !err) {FILE *dump=fopen("build/font-container-rejected.ttf","wb");if(dump){fwrite(expanded,1,size,dump);fclose(dump);}}
                free(expanded);FT_Done_Face(face);
            }
            FT_Done_FreeType(lib);
        }
        if(p[i]){check(!font_open_memory(p[i],n[i]<40?n[i]:40),"truncated container rejected");}
    }
    check(hashes[0] && hashes[0]==hashes[1] && hashes[0]==hashes[2],"TrueType WOFF/WOFF2 preserve existing raster");
    check(hashes[3] && hashes[3]==hashes[4],"CFF WOFF2 preserves CFF raster");
    if(p[0]) {
        unsigned char *wide=malloc(n[0]);check(wide!=NULL,"wide advance fixture allocation");
        if(wide) {
            memcpy(wide,p[0],n[0]);struct sf_span file={wide,n[0]},head,hhea,hmtx;
            bool tables=sf_table(file,"head",&head)&&sf_table(file,"hhea",&hhea)&&sf_table(file,"hmtx",&hmtx);
            check(tables,"wide advance tables");
            if(tables) {
                unsigned count=sf_u16(hhea.p+34);
                for(unsigned i=0;i<count;i++)((unsigned char *)hmtx.p)[i*4]=((unsigned char *)hmtx.p)[i*4+1]=255;
                font_t *face=font_open_memory(wide,n[0]);
                float expected=65535.0f*1000.0f/sf_u16(head.p+18);
                check(face && font_has(face,'A') && fabsf(font_advance(face,1000,'A')-expected)<0.01f,
                      "unsigned hmtx advance retains 65535 without signed wrap");
                check(face && fabsf(font_advance(face,1000,'A')-expected)<0.01f,"wide advance cache preserves value");
                font_close(face);
            }
            free(wide);
        }
    }
    if(p[0])for(unsigned xor_data=0;xor_data<2;xor_data++) {
        size_t total=n[0]+82;unsigned char *eot=calloc(total,1);
        le32(eot,(uint32_t)total);le32(eot+4,(uint32_t)n[0]);le32(eot+8,0x20001);le32(eot+12,xor_data?0x10000000:0);
        eot[34]=0x4c;eot[35]=0x50;
        for(size_t j=0;j<n[0];j++)eot[82+j]=p[0][j]^(xor_data?0x50:0);
        font_t *face=font_open_memory(eot,total);check(face && raster(face)==hashes[0],"EOT wrapper and XOR preserve raster");
        font_close(face);le32(eot+12,4);check(!font_open_memory(eot,total),"unsupported MicroType EOT rejected");free(eot);
    }
    for(unsigned i=0;i<5;i++){font_close(f[i]);free(p[i]);}
    printf("font-container-native: %d checks, %d failures\n",checks,failures);return failures!=0;
}
