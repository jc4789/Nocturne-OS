/* Nocturne n_memory_reserve failure atomicity and page accounting. Run only in an isolated test guest. */
#include "nocturne.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static int passed,failed;
static void check(int ok,const char *what) {
    printf("%s %s\n",ok?"PASS":"FAIL",what);
    if(ok)passed++;else failed++;
}
static uint64_t self_mapped_kb(void) {
    /* Static storage is already mapped by the loader, not grown by this probe. */
    static struct n_procinfo processes[64];
    int pid=(int)n_app_self(),n=proclist(processes,64);
    for(int i=0;i<n;i++)if(processes[i].pid==pid)return processes[i].mem_kb;
    return UINT64_MAX;
}
static uint64_t retained_table_bound(uint64_t free_bytes) {
    /* A 4 KiB table holds 512 entries. A failed reservation can leave empty
       PT/PD/PDPT pages (covering 2 MiB, 1 GiB, 512 GiB) behind: at most the
       tables a contiguous run of free_bytes/4096 data pages touches at its
       worst alignment, plus the failed mapping's own. The process's PML4 exists. */
    uint64_t pages=free_bytes/4096+1;
    uint64_t tables=(pages+511)/512+1;
    tables+=(pages+512ULL*512-1)/(512ULL*512)+1;
    tables+=(pages+512ULL*512*512-1)/(512ULL*512*512)+1;
    return tables*4096;
}
/* A refused reservation: the given n_result and no pages. */
static int rejected(size_t bytes,n_result code) {
    void *p;
    return n_memory_reserve(bytes,N_MEMORY_READ_WRITE,&p)==-code && p==NULL;
}
int main(void) {
    struct n_sysinfo before,after,repeated;
    printf("memreservetest: n_memory_reserve failure atomicity\n");
    /* A small region with an externally observable marker, standing in for the old partial page. */
    void *pages=NULL;
    n_memory_reserve(4096,N_MEMORY_READ_WRITE,&pages);
    uint8_t *guard=pages;
    check(guard!=NULL && !((uintptr_t)guard&4095),"small reservation returns a page-aligned region");
    if(!guard)return 1;
    check(!guard[0] && !guard[4095],"reserved pages are zero-filled");
    for(int i=0;i<17;i++)guard[i]=(uint8_t)(0x91+i);
    uint64_t mapped_before=self_mapped_kb();
    check(mapped_before!=UINT64_MAX,"native proclist reports mapped data pages");
    sysinfo(&before);
    uint64_t table_bound=retained_table_bound(before.free_mem);
    uint64_t request=before.free_mem+16*1024*1024;
    void *p;
    n_result saved=n_memory_reserve((size_t)request,N_MEMORY_READ_WRITE,&p);
    sysinfo(&after);
    printf("pressure: requested=%llu free_before=%llu free_after=%llu\n",(unsigned long long)request,
           (unsigned long long)before.free_mem,(unsigned long long)after.free_mem);
    uint64_t mapped_after=self_mapped_kb();
    printf("rollback: table_bound=%llu mapped_kb_before=%llu mapped_kb_after=%llu\n",
           (unsigned long long)table_bound,(unsigned long long)mapped_before,(unsigned long long)mapped_after);
    check(p==NULL && saved==-N_ERR_NO_MEMORY,"oversized reservation is rejected with N_ERR_NO_MEMORY");
    int intact=1;for(int i=0;i<17;i++)if(guard[i]!=(uint8_t)(0x91+i))intact=0;
    check(intact,"rollback preserves the earlier region and its contents");
    /* Empty page-table metadata may remain cached; separately count leaf mappings
       so retained data mappings cannot hide inside the metadata allowance. */
    check(after.free_mem>=before.free_mem || before.free_mem-after.free_mem<=table_bound,
          "rejected reservation retains at most the page-table hierarchy bound");
    check(mapped_before!=UINT64_MAX && mapped_after==mapped_before,
          "rejected reservation returns all newly mapped data pages");
    if(after.free_mem>2*1024*1024) {
        for(int i=0;i<3;i++)check(rejected((size_t)request,N_ERR_NO_MEMORY),"repeat rejection retains N_ERR_NO_MEMORY");
        sysinfo(&repeated);
        check(repeated.free_mem+65536>=after.free_mem,"repeat failures do not repeatedly consume physical pages");
        check(mapped_before!=UINT64_MAX && self_mapped_kb()==mapped_before,
              "repeat failures preserve the original data-page count");
        uint64_t began=uptime_ms();
        saved=n_memory_reserve((size_t)1<<40,N_MEMORY_READ_WRITE,&p);
        uint64_t elapsed=uptime_ms()-began;
        sysinfo(&repeated);
        printf("large reservation: elapsed=%llu ms free=%llu\n",(unsigned long long)elapsed,(unsigned long long)repeated.free_mem);
        check(p==NULL && saved==-N_ERR_NO_MEMORY,"large valid-range request fails atomically");
        check(elapsed<5000,"rollback work is bounded by allocated pages, not requested range");
        check(repeated.free_mem+65536>=after.free_mem,"large reservation returns physical pages");
        check(mapped_before!=UINT64_MAX && self_mapped_kb()==mapped_before,
              "large reservation returns all newly mapped data pages");
        /* Normal use still works after the failed requests, and gives its pages back. */
        n_memory_reserve(65536,N_MEMORY_READ_WRITE,&pages);
        uint8_t *more=pages;
        check(more!=NULL && !more[0] && !more[65535],"normal reservation still succeeds after failed requests");
        if(more){more[0]=more[65535]=1;check(n_memory_release(more,65536)==0,"normal release returns the pages");}
        check(mapped_before!=UINT64_MAX && self_mapped_kb()==mapped_before,"release returns the reserved data pages");
        /* Many reserve/release rounds must not accumulate physical pages. */
        sysinfo(&before);
        int cycles_ok=1;
        for(int i=0;i<256;i++){
            if(n_memory_reserve(64*4096,N_MEMORY_READ_WRITE,&pages)<0){cycles_ok=0;break;}
            uint8_t *q=pages;
            for(int k=0;k<64;k++)q[k*4096]=(uint8_t)i;
            if(n_memory_release(q,64*4096)){cycles_ok=0;break;}
        }
        sysinfo(&repeated);
        check(cycles_ok,"256 reserve/touch/release cycles succeed");
        check(repeated.free_mem+65536>=before.free_mem,"reserve/release cycles do not leak physical pages");
    } else {
        check(0,"repeat/growth checks skipped because the failed request exhausted physical memory");
    }
    n_result huge=n_memory_reserve(SIZE_MAX,N_MEMORY_READ_WRITE,&pages);
    check(!pages && (huge==-N_ERR_NO_MEMORY || huge==-N_ERR_INVALID),
          "maximum size request rejects before allocation");
    n_result rounded=n_memory_reserve(SIZE_MAX-4094,N_MEMORY_READ_WRITE,&pages);
    check(!pages && (rounded==-N_ERR_NO_MEMORY || rounded==-N_ERR_INVALID),
          "request whose page rounding overflows rejects without wrapping to a small mapping");
    check(n_memory_reserve(0,N_MEMORY_READ_WRITE,&pages)<0 && !pages,"zero-byte reservation is rejected");
    check(n_memory_release(guard,4096)==0,"the earlier region can still be released");
    check(mapped_before!=UINT64_MAX && self_mapped_kb()<mapped_before,"releasing the first region returns its page");
    void *heap=malloc(3*1024*1024);
    check(heap!=NULL,"malloc still grows after failed reservations");
    free(heap);
    printf("memreservetest: %d passed, %d failed\n",passed,failed);
    return failed!=0;
}
