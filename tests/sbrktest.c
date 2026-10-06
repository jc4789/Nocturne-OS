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
    sysinfo(&before);
    int64_t request=(int64_t)before.free_mem+16*1024*1024;
    void *p=sbrk(request);int saved=errno;
    sysinfo(&after);
    printf("pressure: requested=%lld free_before=%llu free_after=%llu\n",(long long)request,
           (unsigned long long)before.free_mem,(unsigned long long)after.free_mem);
    check(p==(void *)-1 && saved==ENOMEM,"oversized growth is rejected with ENOMEM");
    check(sbrk(0)==old,"rejected growth leaves the break unchanged");
    int intact=1;for(int i=0;i<17;i++)if(guard[i]!=(uint8_t)(0x91+i))intact=0;
    check(intact,"rollback preserves the old partial page and contents");
    /* Empty page-table metadata may remain cached, but not the completed data pages. */
    check(after.free_mem+1024*1024>=before.free_mem,"rejected growth returns newly mapped data pages");
    if(after.free_mem>2*1024*1024) {
        for(int i=0;i<3;i++)check(sbrk(request)==(void *)-1 && errno==ENOMEM,"repeat rejection retains ENOMEM");
        sysinfo(&repeated);
        check(repeated.free_mem+65536>=after.free_mem,"repeat failures do not repeatedly consume physical pages");
        uint64_t began=uptime_ms();
        p=sbrk(1LL<<40);saved=errno;
        uint64_t elapsed=uptime_ms()-began;
        sysinfo(&repeated);
        printf("large reservation: elapsed=%llu ms free=%llu\n",(unsigned long long)elapsed,(unsigned long long)repeated.free_mem);
        check(p==(void *)-1 && saved==ENOMEM && sbrk(0)==old,"large valid-range request fails atomically");
        check(elapsed<5000,"rollback work is bounded by allocated pages, not requested range");
        check(repeated.free_mem+65536>=after.free_mem,"large reservation returns physical pages");
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
