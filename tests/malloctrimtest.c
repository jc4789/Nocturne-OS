/* Native allocator reclamation over n_memory regions. MALLOCTRIM_MODEL
   additionally runs the exact allocator source over an isolated page pool with
   guard pages, released-page poisoning, call counting and injected failures. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "nocturne.h"

#define KIB 1024u
#define MIB (1024u * 1024u)
#define TEST_PAGE 4096u
/* The heap block a request occupies: 16-byte header, 16-byte granules. */
static size_t span(size_t n) { size_t s=(n+16+15)&~(size_t)15; return s<32?32:s; }

#ifdef MALLOCTRIM_MODEL
#define POOL_PAGES 16384u
_Alignas(4096) static unsigned char model_pool[POOL_PAGES*TEST_PAGE];
static unsigned char model_state[POOL_PAGES+1]; /* 0 free, 1 reserved, 2 guard; the extra entry is past the pool */
static unsigned model_reserves, model_releases;
static int model_reject_reserve, model_reject_release;
n_result n_memory_reserve(size_t bytes, int access, void **memory) {
    size_t pages=(bytes+TEST_PAGE-1)/TEST_PAGE;
    *memory=NULL;
    if(!bytes || access!=N_MEMORY_READ_WRITE)return -N_ERR_INVALID;
    model_reserves++;
    if(model_reject_reserve || pages>=POOL_PAGES)return -N_ERR_NO_MEMORY;
    for(size_t first=0;first+pages<POOL_PAGES;first++) {
        size_t k=0;
        while(k<=pages && model_state[first+k]==0)k++;
        if(k<=pages){first+=k;continue;}
        memset(model_state+first,1,pages);
        model_state[first+pages]=2;
        memset(model_pool+first*TEST_PAGE,0,pages*TEST_PAGE);
        *memory=model_pool+first*TEST_PAGE;
        return 0;
    }
    return -N_ERR_NO_MEMORY;
}
n_result n_memory_release(void *addr, size_t bytes) {
    uintptr_t a=(uintptr_t)addr,base=(uintptr_t)model_pool;
    model_releases++;
    if(!bytes || (a&(TEST_PAGE-1)) || a<base || a>=base+sizeof model_pool)return -N_ERR_INVALID;
    size_t first=(a-base)/TEST_PAGE,pages=(bytes+TEST_PAGE-1)/TEST_PAGE;
    if(pages>POOL_PAGES-first)return -N_ERR_INVALID;
    for(size_t k=0;k<pages;k++)if(model_state[first+k]!=1)return -N_ERR_INVALID;
    if(model_reject_release)return -N_ERR_NO_MEMORY;
    memset(model_pool+first*TEST_PAGE,0xdd,pages*TEST_PAGE);
    memset(model_state+first,0,pages);
    if(model_state[first+pages]==2 && first+pages<POOL_PAGES) {
        /* The reservation's tail went: its guard follows what remains. */
        model_state[first+pages]=0;
        if(first && model_state[first-1]==1)model_state[first]=2;
    }
    return 0;
}
static unsigned model_calls(void){return model_reserves+model_releases;}
#define malloc trim_malloc
#define free trim_free
#define calloc trim_calloc
#define realloc trim_realloc
#define aligned_alloc trim_aligned_alloc
void *malloc(size_t n);void free(void *p);void *calloc(size_t n,size_t m);void *realloc(void *p,size_t n);
void *aligned_alloc(size_t align,size_t n);
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
/* Bytes of physical pages this process holds. Reservations past a few pages
   receive frames on first touch, so callers write a whole block before measuring;
   the process's own count is unaffected by other apps. */
static uint64_t resident(void) {
#ifdef MALLOCTRIM_MODEL
    uint64_t n=0;
    for(size_t k=0;k<POOL_PAGES;k++)n+=model_state[k]==1;
    return n*TEST_PAGE;
#else
    static struct n_procinfo processes[64];
    int pid=(int)n_app_self(),n=proclist(processes,64);
    for(int i=0;i<n;i++)if(processes[i].pid==pid)return processes[i].mem_kb*KIB;
    return 0;
#endif
}
static size_t heap_bytes(void) { size_t heap=0; malloc_usage(&heap,NULL); return heap; }
static void mark_bytes(unsigned char *p,size_t n,unsigned char tag) {
    p[0]=tag;p[n-1]=(unsigned char)(tag^0x55);
}
static int bytes_intact(const unsigned char *p,size_t n,unsigned char tag) {
    return p[0]==tag && p[n-1]==(unsigned char)(tag^0x55);
}

/* Allocations made back to back are carved from the front of one free block. */
static int adjacent_realloc(void) {
    unsigned char *small=malloc(128),*hole=malloc(64u*KIB),*tail=malloc(128);
    check(small!=NULL && hole!=NULL && tail!=NULL,"small growth beside a large free hole and live tail");
    if(!small || !hole || !tail){free(small);free(hole);free(tail);return 0;}
    check(hole==small+span(128) && tail==hole+span(64u*KIB),"back-to-back allocations are laid out contiguously");
    if(hole!=small+span(128) || tail!=hole+span(64u*KIB)){free(small);free(hole);free(tail);return 0;}
    mark_bytes(small,128,0x58);mark_bytes(tail,128,0x46);
    size_t heap=heap_bytes();
    free(hole);
    unsigned char *grown=realloc(small,256);
    check(grown==small,"small growth reuses its adjacent hole in place");
    if(!grown){free(small);free(tail);return 0;}
    check(bytes_intact(grown,128,0x58),"in-place growth preserves the existing bytes");
    check(heap_bytes()==heap,"in-place growth takes no new region");
#ifdef MALLOCTRIM_MODEL
    blk *b=(blk *)(grown-HDR),*r=next_blk(b);
    check(bsize(b)==256+HDR && (b->size&USED) && !(r->size&USED) &&
          r->prev==bsize(b) && next_blk(r)->prev==bsize(r),"split remainder has exact size and consistent boundary tags");
#endif
    mark_bytes(grown,256,0x58);
    unsigned char *reused=malloc(48u*KIB);
    check(reused!=NULL,"most of the adjacent hole remains allocatable");
    if(!reused){free(grown);free(tail);return 0;}
    check(heap_bytes()==heap,"the split remainder is reused without a new region");
    check(reused>grown && reused+48u*KIB<=tail,"reuse comes from the split hole before the live tail");
    mark_bytes(reused,48u*KIB,0x32);
    check(bytes_intact(grown,256,0x58) && bytes_intact(tail,128,0x46),"split-hole reuse preserves both live neighbors");
    free(grown);free(reused);
    check(heap_bytes()==heap && bytes_intact(tail,128,0x46),"split blocks coalesce around the live tail");
    free(tail);
    check(heap_bytes()<=heap,"freeing the tail leaves no extra region");
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

/* A request that leaves exactly 2 KiB of its fresh region free: the region is
   600 KiB, and the request's block fills all but the end marker and 2 KiB. */
#define FILLER (600u*KIB-16u-2048u-16u)

int main(void) {
    printf("malloctrimtest: allocator region reclamation and adjacent realloc splitting\n");
    unsigned char *anchor=malloc(128);
    check(anchor!=NULL,"small live allocation");if(!anchor)return 1;
    mark_bytes(anchor,128,0x71);
    if(!adjacent_realloc())return 1;
#ifdef MALLOCTRIM_MODEL
    if(!realloc_split_boundaries())return 1;
#endif

    /* The newest region keeps one growth quantum warm and returns the rest. */
    unsigned char *big=malloc(900u*KIB);
    check(big!=NULL,"a block larger than any free space");if(!big)return 1;
    memset(big,0x63,900u*KIB);mark_bytes(big,900u*KIB,0x63);
    size_t high=heap_bytes();uint64_t before=resident();
    errno=EIO;free(big);
    check(errno==EIO,"free preserves errno across page release");
    size_t low=heap_bytes();
    check(low+600u*KIB<=high,"freeing the newest region's tail returns its pages to the system");
    check(resident()+600u*KIB<=before,"the returned pages are physical pages");
    check(bytes_intact(anchor,128,0x71),"page release preserves an earlier live block");
    unsigned char *small=malloc(128);check(small!=NULL,"warm residual allocation");
#ifdef MALLOCTRIM_MODEL
    unsigned calls=model_calls();
#endif
    free(small);
#ifdef MALLOCTRIM_MODEL
    check(model_calls()==calls,"a small free makes no system call");
#endif
    check(heap_bytes()==low,"a small free keeps the warm growth quantum");
    for(int i=0;i<8;i++){
        unsigned char *again=calloc(1,900u*KIB);
        check(again!=NULL,"regrowth after reclamation");if(!again)break;
        check(again[0]==0 && again[900u*KIB-1]==0,"regrowth calloc initializes reused bytes");
        mark_bytes(again,900u*KIB,0x42);free(again);
        check(heap_bytes()<=low+TEST_PAGE,"repeated growth and free do not retain a growing high water mark");
    }

    /* A live block after a free one keeps the region; freeing it returns the tail. */
    unsigned char *middle=malloc(FILLER),*tail=malloc(2048-16);
    check(middle!=NULL && tail!=NULL,"interior free with a live tail");
    if(!middle || !tail)return 1;
    check(tail==middle+span(FILLER),"the tail fills the rest of the middle block's region");
    if(tail!=middle+span(FILLER))return 1;
    mark_bytes(tail,2048-16,0x37);high=heap_bytes();
#ifdef MALLOCTRIM_MODEL
    calls=model_calls();
#endif
    free(middle);
#ifdef MALLOCTRIM_MODEL
    check(model_calls()==calls,"an interior free makes no system call");
#endif
    check(heap_bytes()==high,"a live tail prevents reclaiming an interior hole");
    check(bytes_intact(tail,2048-16,0x37),"an interior free preserves the live tail");
    free(tail);
    check(heap_bytes()+300u*KIB<=high,"neighbor coalescing reclaims the newly free tail");

    /* An older region that becomes wholly free goes back at once. */
    unsigned char *older=malloc(FILLER);
    check(older!=NULL,"allocation in a region about to become older");if(!older)return 1;
    unsigned char *newer=malloc(700u*KIB);
    check(newer!=NULL,"a newer region");if(!newer)return 1;
    mark_bytes(newer,700u*KIB,0x29);high=heap_bytes();
#ifdef MALLOCTRIM_MODEL
    unsigned releases=model_releases;
#endif
    free(older);
    check(heap_bytes()+590u*KIB<=high,"a wholly free older region returns all its pages");
#ifdef MALLOCTRIM_MODEL
    check(model_releases==releases+1,"a wholly free older region returns in one release");
#endif
    check(bytes_intact(newer,700u*KIB,0x29) && bytes_intact(anchor,128,0x71),"region release preserves live blocks elsewhere");
    free(newer);

#ifdef MALLOCTRIM_MODEL
    big=malloc(FILLER);check(big!=NULL,"injected-failure allocation");if(!big)return 1;
    high=heap_bytes();blk *old_end=newest_end;
    model_reject_release=1;errno=EIO;free(big);model_reject_release=0;
    check(heap_bytes()==high && newest_end==old_end && errno==EIO,"a failed release is atomic and preserves errno");
    unsigned char *again=malloc(FILLER);check(again==big,"allocation reuses the untrimmed free block");
    if(again){mark_bytes(again,FILLER,0x11);check(bytes_intact(again,FILLER,0x11),"a failed release leaves usable block tags");free(again);}
    check(heap_bytes()+300u*KIB<=high,"release succeeds after the injected failure is removed");
#endif

    /* Large blocks are regions of their own. */
    before=resident();
    big=malloc(8u*MIB);check(big!=NULL,"large own-region allocation");if(!big)return 1;
    memset(big,0x5a,8u*MIB);mark_bytes(big,8u*MIB,0x5a);high=heap_bytes();
    uint64_t taken=resident();
    check(taken>=before+7u*MIB,"a large allocation takes its pages");
    unsigned char *kept=realloc(big,3u*MIB);check(kept==big && big[0]==0x5a,"large shrink keeps the block in place");
    check(heap_bytes()+4u*MIB<=high,"large shrink returns the pages it no longer needs");
    errno=EIO;free(big);check(errno==EIO,"large free preserves errno");
    check(resident()+7u*MIB<=taken,"large free returns all its pages");
    check(bytes_intact(anchor,128,0x71),"large-block traffic leaves small blocks intact");

#ifdef MALLOCTRIM_MODEL
    high=heap_bytes();old_end=newest_end;
    model_reject_reserve=1;
    check(malloc(900u*KIB)==NULL && malloc(8u*MIB)==NULL,"injected region reservation failure");
    model_reject_reserve=0;
    check(heap_bytes()==high && newest_end==old_end,"failed growth preserves the existing regions");
    check(malloc(sizeof model_pool)==NULL,"a request beyond the pool fails");
    check(malloc_stress_main()==0,"existing memtest patterns under the exact allocator");
#endif
    free(anchor);
    printf("malloctrimtest: %d passed, %d failed\n",passed,failed);
    return failed!=0;
}
