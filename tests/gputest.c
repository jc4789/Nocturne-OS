/* Runs through the real native GPU syscalls; CPU fallback is tested separately. */
#include "nocturne.h"
#include "gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int failures,checks;
static void check(const char *name,bool ok) { checks++; if (!ok) { printf("FAIL GPU %s\n",name); failures++; } }
static uint32_t hash_bytes(const void *data,size_t bytes) {
    const unsigned char *p=data; uint32_t hash=2166136261u;
    for (size_t i=0;i<bytes;i++) { hash^=p[i];hash*=16777619u; }
    return hash;
}
static void blit_test(bool required,const struct n_gpu_info *info) {
    const uint32_t input[12]={0xff010203,0xff102030,0xff405060,0xff708090,
        0xffa1b2c3,0x7f112233,0x80224466,0xffabcdef,
        0xff998877,0x20345678,0x4056789a,0xff987654};
    uint32_t output[18]; output[0]=0x12345678;output[17]=0x87654321;
    struct n_gpu_blit r={.source_width=4,.source_height=3,.source_x=1,.source_y=1,
        .source_w=2,.source_h=2,.width=4,.height=4};
    check("blit-abi",sizeof r==32);
    check("blit-request-span",gpu_blit((void *)1,input,sizeof input,output+1,64)<0 && errno==EFAULT);
    check("blit-source-span",gpu_blit(&r,(void *)1,sizeof input,output+1,64)<0 && errno==EFAULT);
    check("blit-output-span",gpu_blit(&r,input,sizeof input,(void *)1,64)<0 && errno==EFAULT);
    check("blit-source-overflow-span",gpu_blit(&r,(void *)(uintptr_t)-16,sizeof input,output+1,64)<0 && errno==EFAULT);
    check("blit-output-overflow-span",gpu_blit(&r,input,sizeof input,(void *)(uintptr_t)-16,64)<0 && errno==EFAULT);
    check("blit-short-source",gpu_blit(&r,input,sizeof input-4,output+1,64)<0 && errno==EINVAL);
    check("blit-short-output",gpu_blit(&r,input,sizeof input,output+1,60)<0 && errno==EINVAL);
    struct n_gpu_blit bad=r;bad.source_x=UINT32_MAX;
    check("blit-crop-overflow",gpu_blit(&bad,input,sizeof input,output+1,64)<0 && errno==EINVAL);
    bad=r;bad.source_width=513;
    check("blit-reject-dimensions",gpu_blit(&bad,input,sizeof input,output+1,64)<0 && errno==EINVAL);
    if (info->capabilities&N_GPU_CAP_BLIT) {
        struct n_gpu_info before,after;
        bool ok=gpu_info(&before)==0 && gpu_blit(&r,input,sizeof input,output+1,64)==0 && gpu_info(&after)==0;
        check("blit-one-fenced-submission",ok && after.submissions==before.submissions+1 && after.failures==before.failures);
        for (unsigned y=0;y<4;y++) for (unsigned x=0;x<4;x++)
            if (output[1+y*4+x]!=input[(1+y/2)*4+1+x/2]) ok=false;
        check("blit-argb-y-crop-scale",ok && output[0]==0x12345678 && output[17]==0x87654321);
        memcpy(output+1,input,sizeof input);
        ok=gpu_blit(&r,output+1,sizeof input,output+1,64)==0;
        for (unsigned y=0;y<4;y++) for (unsigned x=0;x<4;x++)
            if (output[1+y*4+x]!=input[(1+y/2)*4+1+x/2]) ok=false;
        check("blit-source-output-alias",ok && output[0]==0x12345678 && output[17]==0x87654321);
        r.source_x=r.source_y=0;r.source_w=4;r.source_h=3;r.width=r.height=2;
        ok=gpu_blit(&r,input,sizeof input,output+1,16)==0;
        for (unsigned y=0;y<2;y++) for (unsigned x=0;x<2;x++)
            if (output[1+y*2+x]!=input[((2*y+1)*3/4)*4+(2*x+1)]) ok=false;
        check("blit-nearest-downscale",ok);
        size_t bytes=(size_t)512*512*4;uint32_t *large=malloc(bytes);
        check("blit-max-source-allocation",large!=NULL);
        if (large) {
            memset(large,0,bytes);large[512*512-1]=0x42123456;
            r=(struct n_gpu_blit){.source_width=512,.source_height=512,.source_x=511,.source_y=511,
                .source_w=1,.source_h=1,.width=1,.height=1};
            check("blit-max-source-last-pixel",gpu_blit(&r,large,bytes,output+1,4)==0 && output[1]==0x42123456);
            free(large);
        }
    } else check("blit-unavailable-truthful",gpu_blit(&r,input,sizeof input,output+1,64)<0 && errno==ENOSYS && !required);
    printf("GPU blit cap=%u (texture replace/nearest; not WebGL)\n",(unsigned)((info->capabilities&N_GPU_CAP_BLIT)!=0));
}
static void batch_test(bool required,const struct n_gpu_info *info) {
    uint32_t pixels[32*32+2]; pixels[0]=0x12345678; pixels[32*32+1]=0x87654321;
    canvas_t c; gfx_init(&c,pixels+1,32,32,32);
    struct n_gpu_batch r={.width=32,.height=32,.clear_argb=0xff284c78,.triangle_count=N_GPU_MAX_TRIANGLES};
    for (unsigned i=0;i<N_GPU_MAX_TRIANGLES*3;i++)
        r.vertex[i]=(struct n_gpu_vertex){131072,0,0,65536,0xff00ff00};
    r.vertex[0]=(struct n_gpu_vertex){-58982,-49152,0,65536,0xffff0000};
    r.vertex[1]=(struct n_gpu_vertex){-6554,-49152,0,65536,0xffff0000};
    r.vertex[2]=(struct n_gpu_vertex){-32768,49152,0,65536,0xffff0000};
    unsigned last=(N_GPU_MAX_TRIANGLES-1)*3;
    r.vertex[last]=(struct n_gpu_vertex){6554,-49152,0,65536,0xff0000ff};
    r.vertex[last+1]=(struct n_gpu_vertex){58982,-49152,0,65536,0xff0000ff};
    r.vertex[last+2]=(struct n_gpu_vertex){32768,49152,0,65536,0xff0000ff};
    check("batch-abi",sizeof r==1936 && sizeof(struct n_gpu_render)==76);
    int backend=gfx_render3d_batch(&c,&r);
    check("batch-max-first-last-pixels",backend>=0 && pixels[1]==r.clear_argb &&
        pixels[1+16*32+8]==0xffff0000 && pixels[1+16*32+24]==0xff0000ff);
    if (info->capabilities&N_GPU_CAP_BATCH) {
        struct n_gpu_info before,after;
        bool ok=gpu_info(&before)==0 && gpu_render_batch(&r,pixels+1,32*32*4)==0 && gpu_info(&after)==0;
        check("batch-one-fenced-submission",ok && after.submissions==before.submissions+1 && after.failures==before.failures);
        check("batch-strict-device-pixels",ok && backend==N_GPU_BACKEND_VIRGL &&
            pixels[1]==r.clear_argb && pixels[1+16*32+8]==0xffff0000 && pixels[1+16*32+24]==0xff0000ff);
    } else check("batch-fallback-truthful",backend==N_GPU_BACKEND_CPU && !required);
    struct n_gpu_batch bad=r; bad.triangle_count=N_GPU_MAX_TRIANGLES+1;
    uint32_t unchanged=pixels[1+16*32+24];
    check("batch-reject-count",gpu_render_batch(&bad,pixels+1,32*32*4)<0 && errno==EINVAL &&
        gfx_render3d_batch(&c,&bad)<0 && pixels[1+16*32+24]==unchanged);
    bad=r; bad.vertex[last+2].w=0;
    check("batch-reject-last-vertex",gpu_render_batch(&bad,pixels+1,32*32*4)<0 && errno==EINVAL && gfx_render3d_batch(&c,&bad)<0);
    check("batch-reject-short-output",gpu_render_batch(&r,pixels+1,4)<0 && errno==EINVAL);
    /* Overlapping primitives are ordered overwrite, not independent clears. */
    r.triangle_count=2;
    memcpy(&r.vertex[3],r.vertex,3*sizeof r.vertex[0]);
    for (unsigned i=3;i<6;i++) r.vertex[i].argb=0xff0000ff;
    check("batch-primitive-order",gfx_render3d_batch(&c,&r)==backend && pixels[1+16*32+8]==0xff0000ff);
    r.triangle_count=0;
    bool equal=gfx_render3d_batch(&c,&r)==backend;
    for (unsigned i=1;i<=32*32;i++) if (pixels[i]!=r.clear_argb) equal=false;
    check("batch-empty-clear",equal);
    for (unsigned i=1;i<=32*32;i++) pixels[i]=0xff445566;
    gfx_clip(&c,8,8,16,16);
    check("batch-destination-clip",gfx_render3d_batch(&c,&r)==backend && pixels[1]==0xff445566 &&
        pixels[1+9*32+9]==r.clear_argb && pixels[1+24*32+24]==0xff445566);
    check("batch-output-canaries",pixels[0]==0x12345678 && pixels[32*32+1]==0x87654321);
    printf("GPU batch max=%u backend=%d (same-frame triangles; not WebGL)\n",N_GPU_MAX_TRIANGLES,backend);
}
static void benchmark(bool required) {
    /* Exactly the same public entry/request on a virgl VM and a no-GPU VM.
       Include native upload/readback/syscall/copy, not just the GPU draw.
       Input hash is comparable; output hashes need not be identical because
       GL and the software rasterizer have different edge/color rounding. */
    struct n_gpu_render r={.width=400,.height=400,.operation=N_GPU_TRIANGLE,.clear_argb=0xff284c78,
        .vertex={{-49152,-49152,0,65536,0xffff0000},{49152,-49152,0,65536,0xff00ff00},{0,49152,0,65536,0xff0000ff}}};
    size_t count=(size_t)r.width*r.height;
    uint32_t *pixels=malloc((count+2)*4);
    check("benchmark-allocation",pixels!=NULL); if (!pixels) return;
    pixels[0]=0x12345678;pixels[count+1]=0x87654321;
    canvas_t c;gfx_init(&c,pixels+1,r.width,r.height,r.width);
    bool ok=true;int backend=-1;uint64_t begin=uptime_ms();
    for (unsigned i=0;i<3;i++) {
        int actual=gfx_render3d(&c,&r);
        if (i==0) backend=actual;
        uint32_t center=pixels[1+200*400+200];
        if (actual<0 || actual!=backend || pixels[1]!=r.clear_argb || center==r.clear_argb ||
            (center>>24)!=255 || pixels[0]!=0x12345678 || pixels[count+1]!=0x87654321) ok=false;
    }
    uint64_t elapsed=uptime_ms()-begin;
    check("benchmark-real-pixels",ok);
    check("benchmark-required-device",!required || backend==N_GPU_BACKEND_VIRGL);
    printf("gpu-bench: input_hash=%x output_hash=%x size=400x400 rounds=3 backend=%d elapsed_ms=%lu pixels=%s\n",
        hash_bytes(&r,sizeof r),hash_bytes(pixels+1,count*4),backend,(unsigned long)elapsed,ok?"PASS":"FAIL");
    free(pixels);
}
int main(int argc,char **argv) {
    bool required=false,bench=false;
    for (int i=1;i<argc;i++) { if (!strcmp(argv[i],"--require-gpu")) required=true; if (!strcmp(argv[i],"--bench")) bench=true; }
    struct n_gpu_info info={0};
    check("info",gpu_info(&info)==0);
    printf("GPU backend=%u caps=%u name=%s submissions=%lu failures=%lu\n",info.backend,info.capabilities,info.name,(unsigned long)info.submissions,(unsigned long)info.failures);
    uint32_t storage[32*32+2]; storage[0]=0x12345678; storage[32*32+1]=0x87654321;
    canvas_t c; gfx_init(&c,storage+1,32,32,32);
    struct n_gpu_render r={.width=32,.height=32,.operation=N_GPU_CLEAR,.clear_argb=0xff284c78};
    check("bounded-render",gfx_render3d(&c,&r)>=0);
    bool equal=true; for (int i=1;i<=32*32;i++) if (storage[i]!=r.clear_argb) equal=false;
    check("clear-pixels",equal);
    check("output-canaries",storage[0]==0x12345678 && storage[32*32+1]==0x87654321);
    struct n_gpu_render bad=r; bad.width=513;
    check("reject-dimensions",gpu_render(&bad,storage+1,32*32*4)<0 && errno==EINVAL);
    check("reject-short-buffer",gpu_render(&r,storage+1,4)<0);
    r.operation=N_GPU_TRIANGLE;
    r.vertex[0]=(struct n_gpu_vertex){-49152,-49152,0,65536,0xffff0000};
    r.vertex[1]=(struct n_gpu_vertex){49152,-49152,0,65536,0xff00ff00};
    r.vertex[2]=(struct n_gpu_vertex){0,49152,0,65536,0xff0000ff};
    int backend=gfx_render3d(&c,&r);
    check("triangle-actual-pixels",backend>=0 && storage[1]==r.clear_argb && storage[1+16*32+16]!=r.clear_argb);
    bad=r; bad.vertex[0].w=0;
    check("reject-zero-w",gpu_render(&bad,storage+1,32*32*4)<0 && errno==EINVAL && gfx_render3d(&c,&bad)<0);
    if (info.capabilities&N_GPU_CAP_TRIANGLE) {
        check("strict-device",gpu_render(&r,storage+1,32*32*4)==0);
        check("device-output",storage[1]==r.clear_argb && storage[1+16*32+16]!=r.clear_argb);
        check("device-used",backend==N_GPU_BACKEND_VIRGL);
        gpu_info(&info); check("fences-and-submissions",info.submissions>10 && info.failures==0);
    } else {
        printf("GPU native unavailable; CPU fallback only\n");
        check("fallback-truthful",backend==N_GPU_BACKEND_CPU);
        check("required-device",!required);
    }
    struct n_gpu_render outside=r;
    for (unsigned i=0;i<3;i++) outside.vertex[i].x=2*65536;
    check("outside-homogeneous-plane",gfx_render3d(&c,&outside)==backend);
    equal=true; for (int i=1;i<=32*32;i++) if (storage[i]!=r.clear_argb) equal=false;
    check("fully-clipped-pixels",equal);
    for (int i=1;i<=32*32;i++) storage[i]=0xff445566;
    gfx_clip(&c,8,8,16,16); r.operation=N_GPU_CLEAR;
    check("destination-clip",gfx_render3d(&c,&r)==backend && storage[1]==0xff445566 && storage[1+9*32+9]==r.clear_argb && storage[1+24*32+24]==0xff445566);
    check("final-output-canaries",storage[0]==0x12345678 && storage[32*32+1]==0x87654321);
    batch_test(required,&info);
    blit_test(required,&info);
    if (bench) benchmark(required);
    printf("gputest: %d checks, %d failed\n",checks,failures);
    return failures!=0;
}
