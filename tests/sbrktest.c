/* Nocturne native heap failure atomicity. Run only in an isolated test guest. */
#include "nocturne.h"
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
static int passed,failed;
static void check(int ok,const char *what) {
    printf("%s %s\n",ok?"PASS":"FAIL",what);
    if(ok)passed++;else failed++;
}
static uint64_t self_mapped_kb(void) {
    /* Static storage is already mapped by the loader, not grown by this probe. */
    static struct n_procinfo processes[64];
    int pid=getpid(),n=proclist(processes,64);
    for(int i=0;i<n;i++)if(processes[i].pid==pid)return processes[i].mem_kb;
    return UINT64_MAX;
}
static uint64_t retained_table_bound(void *old,uint64_t free_bytes) {
    /* A 4 KiB table contains 512 entries. New PT/PD/PDPT pages cover 2 MiB,
       1 GiB and 512 GiB respectively; the process's PML4 already exists.
       At most free_bytes/4096 data pages can be attempted before exhaustion,
       plus the failed mapping itself, which may create an empty upper table.
       Count every table intersecting that contiguous range, including ones
       already present, for a conservative RAM- and VA-alignment-based bound. */
    uint64_t first=((uintptr_t)old+4095)&~4095ULL;
    uint64_t last=first+(free_bytes/4096)*4096,tables=0;
    for(int shift=21;shift<=39;shift+=9)tables+=(last>>shift)-(first>>shift)+1;
    return tables*4096;
}
int main(void) {
    struct n_sysinfo before,after,repeated;
    printf("sbrktest: native sbrk failure atomicity\n");
    uint8_t *base=sbrk(0);
    /* Leave an old partial page with an externally observable marker. */
    uint8_t *guard=sbrk(17);
    check(guard!=(void *)-1 && guard==base,"small growth returns the original break");
    if(guard==(void *)-1)return 1;
    for(int i=0;i<17;i++)guard[i]=(uint8_t)(0x91+i);
    void *old=sbrk(0);
    uint64_t mapped_before=self_mapped_kb();
    check(mapped_before!=UINT64_MAX,"native proclist reports mapped data pages");
    sysinfo(&before);
    uint64_t table_bound=retained_table_bound(old,before.free_mem);
    int64_t request=(int64_t)before.free_mem+16*1024*1024;
    void *p=sbrk(request);int saved=errno;
    sysinfo(&after);
    printf("pressure: requested=%lld free_before=%llu free_after=%llu\n",(long long)request,
           (unsigned long long)before.free_mem,(unsigned long long)after.free_mem);
    uint64_t mapped_after=self_mapped_kb();
    printf("rollback: table_bound=%llu mapped_kb_before=%llu mapped_kb_after=%llu\n",
           (unsigned long long)table_bound,(unsigned long long)mapped_before,(unsigned long long)mapped_after);
    check(p==(void *)-1 && saved==ENOMEM,"oversized growth is rejected with ENOMEM");
    check(sbrk(0)==old,"rejected growth leaves the break unchanged");
    int intact=1;for(int i=0;i<17;i++)if(guard[i]!=(uint8_t)(0x91+i))intact=0;
    check(intact,"rollback preserves the old partial page and contents");
    /* Empty page-table metadata may remain cached; separately count leaf mappings
       so retained data mappings cannot hide inside the metadata allowance. */
    check(after.free_mem>=before.free_mem || before.free_mem-after.free_mem<=table_bound,
          "rejected growth retains at most the page-table hierarchy bound");
    check(mapped_before!=UINT64_MAX && mapped_after==mapped_before,
          "rejected growth returns all newly mapped data pages");
    if(after.free_mem>2*1024*1024) {
        for(int i=0;i<3;i++)check(sbrk(request)==(void *)-1 && errno==ENOMEM,"repeat rejection retains ENOMEM");
        sysinfo(&repeated);
        check(repeated.free_mem+65536>=after.free_mem,"repeat failures do not repeatedly consume physical pages");
        check(mapped_before!=UINT64_MAX && self_mapped_kb()==mapped_before,
              "repeat failures preserve the original data-page count");
        uint64_t began=uptime_ms();
        p=sbrk(1LL<<40);saved=errno;
        uint64_t elapsed=uptime_ms()-began;
        sysinfo(&repeated);
        printf("large reservation: elapsed=%llu ms free=%llu\n",(unsigned long long)elapsed,(unsigned long long)repeated.free_mem);
        check(p==(void *)-1 && saved==ENOMEM && sbrk(0)==old,"large valid-range request fails atomically");
        check(elapsed<5000,"rollback work is bounded by allocated pages, not requested range");
        check(repeated.free_mem+65536>=after.free_mem,"large reservation returns physical pages");
        check(mapped_before!=UINT64_MAX && self_mapped_kb()==mapped_before,
              "large reservation returns all newly mapped data pages");
        p=sbrk(65536);
        check(p==old,"normal growth still succeeds after failed requests");
        if(p!=(void *)-1)check(sbrk(-65536)==(uint8_t *)old+65536,"normal shrink returns the previous break");
    } else {
        check(0,"repeat/growth checks skipped because the failed request exhausted physical memory");
    }
    check(sbrk(INT64_MAX)==(void *)-1 && errno==ENOMEM && sbrk(0)==old,"large positive increment rejects before allocation");
    check(sbrk(INT64_MIN)==(void *)-1 && errno==ENOMEM && sbrk(0)==old,"large negative increment rejects without signed negation overflow");
    check(sbrk(-17)==old && sbrk(0)==base,"the original small growth can still be shrunk");
    printf("sbrktest: %d passed, %d failed\n",passed,failed);
    return failed!=0;
}
