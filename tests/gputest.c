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
    if (bench) benchmark(required);
    printf("gputest: %d checks, %d failed\n",checks,failures);
    return failures!=0;
}
