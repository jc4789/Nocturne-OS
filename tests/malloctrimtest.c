/* Native allocator reclamation. MALLOCTRIM_MODEL additionally runs the exact
   allocator source with an isolated break model and injected shrink failure. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

#ifdef MALLOCTRIM_MODEL
_Alignas(4096) static unsigned char model_heap[64u * 1024u * 1024u];
static size_t model_break=3; /* heap_init must preserve an unaligned foreign prefix. */
static unsigned model_queries, model_shrinks;
static int model_reject_shrink;
static void *model_sbrk(long inc) {
    if (!inc) { model_queries++; return model_heap + model_break; }
    size_t old=model_break;
    if (inc<0) {
        model_shrinks++;
        size_t n=(size_t)(-(inc+1))+1;
        if(model_reject_shrink || n>old){errno=ENOMEM;return (void *)-1;}
        model_break-=n;
        /* Poison released data pages, not the retained partial sentinel page. */
        size_t first=(model_break+4095)&~(size_t)4095;
        if(first<old)memset(model_heap+first,0xdd,old-first);
    } else {
        if((size_t)inc>sizeof model_heap-old){errno=ENOMEM;return (void *)-1;}
        model_break+=(size_t)inc;
    }
    return model_heap+old;
}
#define malloc trim_malloc
#define free trim_free
#define calloc trim_calloc
#define realloc trim_realloc
#define sbrk model_sbrk
#include "../user/libc/malloc.c"
#define main malloc_stress_main
#include "memtest.c"
#undef main
#endif

static int passed,failed;
static void check(int ok,const char *what) {
    printf("%s %s\n",ok?"PASS":"FAIL",what);
    if(ok)passed++;else failed++;
}
static uint64_t free_memory(void) {
#ifdef MALLOCTRIM_MODEL
    return sizeof model_heap-((model_break+4095)&~(size_t)4095);
#else
    struct n_sysinfo info;
    return sysinfo(&info)==0?info.free_mem:0;
#endif
}
static void mark_bytes(unsigned char *p,size_t n,unsigned char tag) {
    p[0]=tag;p[n-1]=(unsigned char)(tag^0x55);
}
static int bytes_intact(const unsigned char *p,size_t n,unsigned char tag) {
    return p[0]==tag && p[n-1]==(unsigned char)(tag^0x55);
}
static int adjacent_realloc(void) {
    unsigned char *small=malloc(128),*hole=malloc(8u*1024u*1024u),*tail=malloc(128);
    check(small!=NULL && hole!=NULL && tail!=NULL,"small growth beside a large free hole and live tail");
    if(!small || !hole || !tail){free(small);free(hole);free(tail);return 0;}
    mark_bytes(small,128,0x58);mark_bytes(tail,128,0x46);
    uintptr_t hole_start=(uintptr_t)hole,high=(uintptr_t)sbrk(0);
    free(hole);
    unsigned char *grown=realloc(small,256);
    check(grown==small,"small growth reuses its adjacent hole in place");
    if(!grown){free(small);free(tail);return 0;}
    check(bytes_intact(grown,128,0x58),"in-place growth preserves the existing bytes");
    check((uintptr_t)sbrk(0)==high,"in-place growth does not move the break");
#ifdef MALLOCTRIM_MODEL
    blk *b=(blk *)(grown-HDR),*r=next_blk(b);
    check(bsize(b)==256+HDR && (b->size&USED) && !(r->size&USED) &&
          r->prev==bsize(b) && next_blk(r)->prev==bsize(r),"split remainder has exact size and consistent boundary tags");
#endif
    mark_bytes(grown,256,0x58);
    unsigned char *reused=malloc(7u*1024u*1024u);
    check(reused!=NULL,"most of the large adjacent hole remains allocatable");
    if(!reused){free(grown);free(tail);return 0;}
    check((uintptr_t)sbrk(0)==high,"large split remainder is reused without break growth");
    check((uintptr_t)reused>=hole_start && (uintptr_t)reused+7u*1024u*1024u<=(uintptr_t)tail,
          "large reuse comes from the split hole before the live tail");
    mark_bytes(reused,7u*1024u*1024u,0x32);
    check(bytes_intact(grown,256,0x58) && bytes_intact(tail,128,0x46),"split-hole reuse preserves both live neighbors");
    free(grown);free(reused);
    check((uintptr_t)sbrk(0)==high && bytes_intact(tail,128,0x46),"split blocks coalesce without reclaiming the live tail");
    free(tail);
    check((uintptr_t)sbrk(0)+7u*1024u*1024u<=high,"freeing the tail reclaims the reunited split hole");
    return 1;
}
#ifdef MALLOCTRIM_MODEL
static int realloc_split_boundaries(void) {
    for(int minimum=0;minimum<2;minimum++) {
        unsigned char *small=malloc(64),*neighbor=malloc(32),*tail=malloc(64);
        check(small && neighbor && tail,"realloc split-boundary allocations");
        if(!small || !neighbor || !tail){free(small);free(neighbor);free(tail);return 0;}
        mark_bytes(small,64,0x18);mark_bytes(tail,64,0x27);free(neighbor);
        unsigned char *grown=realloc(small,minimum?80:96);
        check(grown==small,"boundary-sized growth stays in place");
        if(!grown){free(small);free(tail);return 0;}
        blk *b=(blk *)(grown-HDR),*end=(blk *)(tail-HDR);
        if(minimum) {
            blk *r=next_blk(b);
            check(bsize(b)==96 && bsize(r)==MIN_BLK && !(r->size&USED) && r->prev==96 &&
                  end->prev==MIN_BLK,"MIN_BLK remainder is split with correct neighbor tags");
            unsigned char *unit=malloc(1);
            check(unit==(unsigned char *)r+HDR,"MIN_BLK remainder is on the free list");free(unit);
        } else {
            check(bsize(b)==128 && next_blk(b)==end && end->prev==128,
                  "sub-MIN_BLK remainder is absorbed without an unusable fragment");
        }
        check(bytes_intact(grown,64,0x18) && bytes_intact(tail,64,0x27),"split boundaries preserve both neighbors");
        free(grown);free(tail);
    }
    return 1;
}
#endif
int main(void) {
    printf("malloctrimtest: allocator top-page reclamation and adjacent realloc splitting\n");
#ifdef MALLOCTRIM_MODEL
    model_heap[0]=0x9a;model_heap[1]=0x8b;model_heap[2]=0x7c;
#endif
    unsigned char *anchor=malloc(128);
    check(anchor!=NULL,"small live allocation");if(!anchor)return 1;
    mark_bytes(anchor,128,0x71);
    unsigned char *large=malloc(8u*1024u*1024u);
    check(large!=NULL,"large top allocation");if(!large)return 1;
    mark_bytes(large,8u*1024u*1024u,0x63);
    uintptr_t high=(uintptr_t)sbrk(0);uint64_t before=free_memory();
    errno=EIO;free(large);
    check(errno==EIO,"free preserves errno across native shrink");
    uintptr_t low=(uintptr_t)sbrk(0);uint64_t after=free_memory();
    check(high>=low+7u*1024u*1024u,"large top free reduces the break");
    check(after>=before+7u*1024u*1024u,"large top free returns physical data pages");
    check(bytes_intact(anchor,128,0x71),"shrink preserves an earlier live block");
#ifdef MALLOCTRIM_MODEL
    check(model_heap[0]==0x9a && model_heap[1]==0x8b && model_heap[2]==0x7c,"initial sentinel padding preserves the foreign prefix");
#endif
    unsigned char *small=malloc(128);check(small!=NULL,"warm residual allocation");
#ifdef MALLOCTRIM_MODEL
    unsigned queries=model_queries,shrinks=model_shrinks;
#endif
    free(small);
#ifdef MALLOCTRIM_MODEL
    check(model_queries==queries && model_shrinks==shrinks,"small top free makes no break syscall");
#endif
    check((uintptr_t)sbrk(0)==low,"small top free retains the warm growth quantum");
    for(int i=0;i<8;i++){
        unsigned char *again=calloc(1,2u*1024u*1024u);
        check(again!=NULL,"regrowth after reclamation");if(!again)break;
        check(again[0]==0 && again[2u*1024u*1024u-1]==0,"regrowth calloc initializes reused bytes");
        mark_bytes(again,2u*1024u*1024u,0x42);free(again);
        check((uintptr_t)sbrk(0)<=low+4096,"repeated top frees do not retain a growing high water mark");
    }
    if(!adjacent_realloc())return 1;
#ifdef MALLOCTRIM_MODEL
    if(!realloc_split_boundaries())return 1;
#endif
    unsigned char *middle=malloc(4u*1024u*1024u),*tail=malloc(128);
    check(middle!=NULL && tail!=NULL,"interior free with a live tail");
    if(!middle || !tail)return 1;
    mark_bytes(tail,128,0x37);high=(uintptr_t)sbrk(0);
#ifdef MALLOCTRIM_MODEL
    queries=model_queries;shrinks=model_shrinks;
#endif
    free(middle);
#ifdef MALLOCTRIM_MODEL
    check(model_queries==queries && model_shrinks==shrinks,"interior free makes no break syscall");
#endif
    check((uintptr_t)sbrk(0)==high,"a live tail prevents reclaiming an interior hole");
    check(bytes_intact(tail,128,0x37),"interior free preserves the live tail");
    free(tail);check((uintptr_t)sbrk(0)+3u*1024u*1024u<=high,"neighbor coalescing reclaims the newly free tail");
#ifdef MALLOCTRIM_MODEL
    large=malloc(4u*1024u*1024u);check(large!=NULL,"injected-failure allocation");if(!large)return 1;
    high=(uintptr_t)sbrk(0);blk *old_sentinel=sentinel;
    model_reject_shrink=1;errno=EIO;free(large);model_reject_shrink=0;
    check((uintptr_t)sbrk(0)==high && sentinel==old_sentinel && errno==EIO,"failed shrink is atomic and preserves errno");
    large=malloc(4u*1024u*1024u);check(large!=NULL,"allocation reuses the untrimmed free block");
    if(large){mark_bytes(large,4u*1024u*1024u,0x11);check(bytes_intact(large,4u*1024u*1024u,0x11),"failed shrink leaves usable block tags");free(large);}
    check((uintptr_t)sbrk(0)+3u*1024u*1024u<=high,"shrink succeeds after injected failure is removed");
#endif
    large=malloc(2u*1024u*1024u);check(large!=NULL,"allocation before foreign break movement");if(!large)return 1;
    unsigned char *foreign=sbrk(4096);check(foreign!=(void *)-1,"foreign allocator moves the break");
    if(foreign==(void *)-1)return 1;
    mark_bytes(foreign,4096,0x29);high=(uintptr_t)sbrk(0);free(large);
    check((uintptr_t)sbrk(0)==high,"foreign break movement disables trimming the old region");
    check(bytes_intact(foreign,4096,0x29),"old-region free leaves foreign bytes intact");
    /* Force a new region rather than reuse the old region's smaller free hole. */
    large=malloc(4u*1024u*1024u);check(large!=NULL,"allocator creates a later independent region");if(!large)return 1;
    high=(uintptr_t)sbrk(0);free(large);
    check((uintptr_t)sbrk(0)+3u*1024u*1024u<=high,"only the owned last region is reclaimed");
    check(bytes_intact(foreign,4096,0x29) && bytes_intact(anchor,128,0x71),"multiregion reclamation preserves foreign and earlier bytes");
    free(anchor);
    /* The foreign page is deliberately kept: process exit owns its teardown. */
#ifdef MALLOCTRIM_MODEL
    foreign=sbrk(17);check(foreign!=(void *)-1,"unaligned foreign break movement");
    if(foreign==(void *)-1)return 1;mark_bytes(foreign,17,0x35);
    large=malloc(8u*1024u*1024u);check(large!=NULL,"unaligned independent-region allocation");if(!large)return 1;
    high=(uintptr_t)sbrk(0);free(large);
    check((uintptr_t)sbrk(0)==high,"a multi-region trailing alignment gap conservatively disables trimming");
    check(bytes_intact(foreign,17,0x35),"unaligned foreign bytes stay intact");
    old_sentinel=sentinel;high=(uintptr_t)sbrk(0);
    check(malloc(sizeof model_heap)==NULL,"injected bounded-model growth failure");
    check((uintptr_t)sbrk(0)==high && sentinel==old_sentinel,"failed growth preserves the existing sentinel and break");
    check(malloc_stress_main()==0,"existing memtest patterns under the exact trimmed allocator");
#endif
    printf("malloctrimtest: %d passed, %d failed\n",passed,failed);
    return failed!=0;
}
