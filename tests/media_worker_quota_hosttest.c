/* Real private allocator only. Does not claim a child/reap/codec acceptance. */
#include "../user/include/media_alloc_private.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("FAIL quota:%d %s\n",__LINE__,#x);}}while(0)
int main(int argc,char **argv) {
    struct nmedia_alloc_stats a,b;
    if(argc>1&&!strcmp(argv[1],"worker")) {
        CHECK(nmedia_alloc_restrict_worker());CHECK(!nmedia_alloc_restrict_worker());
        nmedia_alloc_snapshot(&a);CHECK(a.limit==NMEDIA_WORKER_BYTES&&a.current==0&&a.reserved==0);
        unsigned char *p=nmedia_ff_malloc(31u*1024u*1024u);CHECK(p!=NULL);
        if(p){p[0]=0x39;p[31u*1024u*1024u-1]=0x65;}
        nmedia_alloc_snapshot(&a);CHECK(!nmedia_ff_malloc(2u*1024u*1024u));
        nmedia_alloc_snapshot(&b);CHECK(b.current==a.current&&b.blocks==a.blocks&&b.rejects==a.rejects+1);
        if(p)CHECK(p[0]==0x39&&p[31u*1024u*1024u-1]==0x65);
        nmedia_ff_free(p);nmedia_alloc_snapshot(&b);CHECK(b.current==0&&b.blocks==0&&b.peak<=b.limit);
    } else {
        unsigned char *p=nmedia_ff_malloc(16u*1024u*1024u);CHECK(p!=NULL);
        if(p){p[0]=0x17;p[16u*1024u*1024u-1]=0x67;}
        CHECK(nmedia_alloc_reserve_worker());nmedia_alloc_snapshot(&a);
        CHECK(a.reserved==NMEDIA_WORKER_BYTES&&a.current+a.reserved<=a.limit);
        CHECK(!nmedia_alloc_reserve_worker());CHECK(!nmedia_ff_malloc(17u*1024u*1024u));
        CHECK(!nmedia_ff_realloc(p,20u*1024u*1024u));
        nmedia_alloc_snapshot(&b);CHECK(b.current==a.current&&b.reserved==a.reserved&&b.blocks==a.blocks);
        if(p)CHECK(p[0]==0x17&&p[16u*1024u*1024u-1]==0x67);
        nmedia_ff_free(p);nmedia_alloc_snapshot(&a);CHECK(a.current==0&&a.reserved==NMEDIA_WORKER_BYTES);
        nmedia_alloc_release_worker();nmedia_alloc_snapshot(&a);CHECK(a.current==0&&a.reserved==0&&a.blocks==0);
        p=nmedia_ff_malloc(31u*1024u*1024u);CHECK(p!=NULL);nmedia_ff_free(p);
        nmedia_alloc_snapshot(&a);CHECK(a.current==0&&a.peak<=a.limit);
    }
    printf("media_worker_quota_host: %u checks, %u failed; allocator only\n",checks,failed);return failed?1:0;
}
