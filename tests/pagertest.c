/* Nocturne pager-backed shared memory (n_pager_*) check; no GPU needed. The pager is a thread of
 * this app: it answers touches of unbound pages from a backing object it holds. Covered: the
 * fault round trip, bound pages needing no pager, the write upgrade, revocation of write access
 * and of frames, replacing a bound frame, kernel copies through paged memory, a failed fault
 * (access violation in another app), that no other app can use or receive the pager, the end
 * of the pager (bound pages stay, unbound ones fail) and of the memory (N_ERR_CLOSED); also that
 * n_memory_protect holds for pages bound later and for a page bound for writing (a kernel copy
 * is a bad address, another app's write an access violation), that revoking a view a page was
 * supplied from (or its parent) unbinds the page in every mapping, and that a composite is no
 * source. */
#include "nocturne.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("ページャー失敗 行 %d\n",__LINE__);}}while(0)
#define PAGE N_PAGE_SIZE
#define PAGES 10
#define BYTES ((size_t)PAGES*PAGE)
#define FAIL_PAGE 6   /* the pager fails touches of this page */
#define KFAIL_PAGE 7  /* and of this one, touched by a kernel copy */
#define PROTECT_PAGE 8 /* made read-only by n_memory_protect before it is bound */
#define VIEW_PAGE 9   /* supplied from views that are then revoked */

static n_handle pager=-1,memory=-1,backing=-1;
static volatile int stop;
static volatile unsigned faults;
static volatile uint32_t last_access,last_app;
static volatile size_t last_page;

static unsigned char pattern(size_t i){return (unsigned char)(i/PAGE*16u+i%251u+1u);}

/* The pager: takes faults in batches and binds each page with the access asked for. */
static void server(void *arg){
    (void)arg;
    struct n_pager_fault f[16];
    while(!stop){
        n_result n=n_pager_take(pager,f,16,50);
        if(n==-N_ERR_TIMEOUT)continue;
        if(n<0)break;
        for(n_result i=0;i<n;i++){
            size_t page=(size_t)(f[i].offset/PAGE);
            /* counted before the answer: the toucher looks at these as soon as it runs again */
            last_access=f[i].access;last_app=f[i].app;last_page=page;faults++;
            if(page==FAIL_PAGE || page==KFAIL_PAGE)n_pager_fail(pager,f[i].offset,PAGE);
            else {
                struct n_pager_supply s={.offset=f[i].offset,.bytes=PAGE,.source_offset=f[i].offset,
                                         .source=backing,.access=f[i].access};
                n_pager_supply(pager,&s,1);
            }
        }
    }
}

static bool receive(n_handle h,void *buf,size_t size,n_handle *attached){
    return n_channel_receive(h,buf,size,N_CHANNEL_WAIT,attached)==(n_result)size;
}
/* Launches this program in mode with a channel as its handle 0 and `extra` as its handle 3. */
static n_handle launch(const char *self,const char *mode,n_handle extra,n_handle *parent_end){
    n_handle c[2];
    if(n_channel_pair(c)<0)return -1;
    const char *args[]={self,mode};
    int32_t handles[]={c[1],-1,-1,extra};
    struct n_launch l={.arguments=args,.argument_count=2,.handles=handles,.handle_count=extra>=0?4:3};
    n_handle app=n_app_launch(self,&l);
    n_handle_close(c[1]);
    if(app<0){n_handle_close(c[0]);return app;}
    *parent_end=c[0];
    return app;
}

/* Another app with the paged memory: reads a bound page, reports it, then touches `page`. */
static int toucher(size_t page){
    n_handle h=-1;char tag[3]; /* "mem" */
    if(!receive(0,tag,sizeof tag,&h)||h<0||n_handle_kind(h)!=N_KIND_SHARED)return 2;
    void *mm;if(n_shared_map(h,BYTES,N_MEMORY_READ,&mm)<0)return 3;
    const volatile unsigned char *m=mm;
    unsigned char seen=m[PAGE+5];
    if(n_channel_send(0,&seen,1,-1)!=1)return 4;
    char go;if(!receive(0,&go,1,NULL))return 5;
    return m[page*PAGE]==0xEE?6:7; /* a failed touch never returns */
}
/* Another app maps the memory for writing and makes a page bound for writing read-only: its write
   there is an access violation (status 128+14) and never lands. */
static int protect_writer(void){
    n_handle h=-1;char tag[3]; /* "mem" */
    if(!receive(0,tag,sizeof tag,&h)||h<0||n_handle_kind(h)!=N_KIND_SHARED)return 2;
    void *mm;if(n_shared_map(h,BYTES,N_MEMORY_READ_WRITE,&mm)<0)return 3;
    volatile unsigned char *m=mm;
    unsigned char seen=m[PROTECT_PAGE*PAGE];
    if(n_memory_protect((void *)(m+PROTECT_PAGE*PAGE),PAGE,N_MEMORY_READ)!=0)return 4;
    if(n_channel_send(0,&seen,1,-1)!=1)return 5;
    m[PROTECT_PAGE*PAGE]=0xEE;
    return 6;
}
/* Another app given the pager handle at launch: it may neither take nor supply. */
static int thief(void){
    struct n_pager_fault f;
    struct n_pager_supply s={.offset=0,.bytes=PAGE,.source_offset=0,.source=3,.access=N_MEMORY_READ};
    if(n_handle_kind(3)!=N_KIND_PAGER)return 2;
    if(n_pager_take(3,&f,1,0)!=-N_ERR_DENIED)return 3;
    if(n_pager_supply(3,&s,1)!=-N_ERR_DENIED)return 4;
    if(n_pager_revoke(3,0,PAGE,0)!=-N_ERR_DENIED)return 5;
    if(n_pager_fail(3,0,PAGE)!=-N_ERR_DENIED)return 6;
    return 0;
}
/* A child touching an unbound page that fails: an access violation, reported by status 128+14. */
static void expect_fault(const char *self,size_t page,const char *why){
    n_handle end=-1;
    n_handle app=launch(self,page==FAIL_PAGE?"touch-fail":"touch-ended",-1,&end);CHECK(app>=0);
    if(app<0)return;
    CHECK(n_channel_send(end,"mem",3,memory)==3);
    unsigned char seen=0;CHECK(receive(end,&seen,1,NULL)&&seen==pattern(PAGE+5));
    unsigned before=faults;
    CHECK(n_channel_send(end,"g",1,-1)==1);
    int status=-1;CHECK(n_app_result(app,-1,&status)==0&&status==128+14);
    if(status==128+14)printf("pagertest: expected toucher fault status=%d (%s)\n",status,why); /* test.py lets it be */
    if(page==FAIL_PAGE)CHECK(faults==before+1&&last_page==FAIL_PAGE&&last_app==(uint32_t)n_app_id(app));
    else CHECK(faults==before); /* nobody was asked */
    n_handle_close(app);n_handle_close(end);
}

int main(int argc,char **argv){
    char program[4096];
    if(n_path_complete(argv[0],program,sizeof program)>=0)argv[0]=program;
    if(argc>1&&!strcmp(argv[1],"touch-fail"))return toucher(FAIL_PAGE);
    if(argc>1&&!strcmp(argv[1],"touch-ended"))return toucher(5);
    if(argc>1&&!strcmp(argv[1],"thief"))return thief();
    if(argc>1&&!strcmp(argv[1],"protect-write"))return protect_writer();

    pager=n_pager_create(BYTES-100,&memory); /* rounded up to whole pages */
    CHECK(pager>=0&&memory>=0);if(pager<0||memory<0)return 1;
    CHECK(n_handle_kind(pager)==N_KIND_PAGER&&n_handle_kind(memory)==N_KIND_SHARED);
    backing=n_shared_create(BYTES);CHECK(backing>=0);
    n_handle alt=n_shared_create(BYTES);CHECK(alt>=0);
    void *bm=NULL,*am=NULL,*mm=NULL,*rm=NULL;
    CHECK(n_shared_map(backing,BYTES,N_MEMORY_READ_WRITE,&bm)==0);
    CHECK(n_shared_map(alt,BYTES,N_MEMORY_READ_WRITE,&am)==0);
    if(!bm||!am)return 1;
    unsigned char *back=bm,*other=am;
    for(size_t i=0;i<BYTES;i++){back[i]=pattern(i);other[i]=(unsigned char)(0xA0u+i/PAGE);}

    /* Arguments the kernel refuses. */
    struct n_pager_fault f;void *none=NULL;
    CHECK(n_pager_take(pager,&f,0,0)==-N_ERR_INVALID);
    CHECK(n_pager_take(pager,&f,1,0)==-N_ERR_TIMEOUT);
    CHECK(n_pager_take(memory,&f,1,0)==-N_ERR_WRONG_KIND);
    struct n_pager_supply bad={.offset=100,.bytes=PAGE,.source=backing,.access=N_MEMORY_READ};
    CHECK(n_pager_supply(pager,&bad,1)==-N_ERR_INVALID);
    bad=(struct n_pager_supply){.offset=0,.bytes=BYTES+PAGE,.source=backing,.access=N_MEMORY_READ};
    CHECK(n_pager_supply(pager,&bad,1)==-N_ERR_INVALID);
    bad=(struct n_pager_supply){.offset=0,.bytes=PAGE,.source=memory,.access=N_MEMORY_READ};
    CHECK(n_pager_supply(pager,&bad,1)==-N_ERR_DENIED); /* paged memory is no frame of its own */
    n_handle ro=n_shared_view_access(backing,0,BYTES,N_CACHE_DEFAULT,N_MEMORY_READ);CHECK(ro>=0);
    bad=(struct n_pager_supply){.offset=0,.bytes=PAGE,.source=ro,.access=N_MEMORY_READ_WRITE};
    CHECK(n_pager_supply(pager,&bad,1)==-N_ERR_DENIED); /* write access needs a writable source */
    struct n_shared_range part={.shared=alt,.offset=0,.bytes=PAGE};
    n_handle composite=n_shared_compose(&part,1,N_MEMORY_READ);CHECK(composite>=0);
    bad=(struct n_pager_supply){.offset=0,.bytes=PAGE,.source=composite,.access=N_MEMORY_READ};
    CHECK(n_pager_supply(pager,&bad,1)==-N_ERR_DENIED); /* a composite: its parts are supplied themselves */
    n_handle_close(composite);
    CHECK(n_pager_revoke(pager,0,PAGE,N_MEMORY_READ_WRITE)==-N_ERR_INVALID);
    CHECK(n_pager_revoke(pager,0,BYTES+PAGE,0)==-N_ERR_INVALID);
    CHECK(n_shared_view_access(memory,0,PAGE,N_CACHE_DEFAULT,N_MEMORY_READ)==-N_ERR_WRONG_KIND);
    struct n_shared_range range={.shared=memory,.offset=0,.bytes=PAGE};
    CHECK(n_shared_compose(&range,1,N_MEMORY_READ)==-N_ERR_WRONG_KIND);
    /* The pager handle never travels in a message. */
    n_handle c[2];CHECK(n_channel_pair(c)==0);
    CHECK(n_channel_send(c[0],"p",1,pager)==-N_ERR_WRONG_KIND);
    CHECK(n_shared_map(pager,BYTES,N_MEMORY_READ,&none)<0&&!none);

    int tid=thread_create(server,NULL);CHECK(tid>0);if(tid<=0)return 1;
    CHECK(n_shared_map(memory,BYTES,N_MEMORY_READ_WRITE,&mm)==0);
    if(!mm)return 1;
    volatile unsigned char *m=mm;

    /* Round trip: a read asks once for read access; the bound page then needs no pager. */
    CHECK(m[0]==pattern(0)&&faults==1&&last_access==N_MEMORY_READ&&last_page==0);
    CHECK(last_app==(uint32_t)n_app_self());
    CHECK(m[PAGE-1]==pattern(PAGE-1)&&faults==1);
    /* A write to the read-only bound page asks again, for write access; it lands in the backing. */
    m[1]=0x55;
    CHECK(faults==2&&last_access==N_MEMORY_READ_WRITE&&back[1]==0x55);
    m[2]=0x56;CHECK(faults==2&&back[2]==0x56);
    /* A second mapping reaches the bound page without the pager. */
    CHECK(n_shared_map(memory,BYTES,N_MEMORY_READ,&rm)==0);
    const volatile unsigned char *r=rm;
    CHECK(r&&r[1]==0x55&&faults==2);
    /* Write access taken away: reads stay, the next write asks again. */
    CHECK(n_pager_revoke(pager,0,PAGE,N_MEMORY_READ)==0);
    CHECK(m[1]==0x55&&faults==2);
    m[3]=0x57;CHECK(faults==3&&last_access==N_MEMORY_READ_WRITE&&back[3]==0x57);
    /* Frames taken away: every mapping asks again. */
    CHECK(n_pager_revoke(pager,0,PAGE,0)==0);
    CHECK(r[3]==0x57&&faults==4&&last_access==N_MEMORY_READ);
    /* Pages bound before anyone asked, then rebound to another frame: both mappings follow. */
    struct n_pager_supply run={.offset=PAGE,.bytes=2*PAGE,.source_offset=PAGE,.source=alt,.access=N_MEMORY_READ};
    CHECK(n_pager_supply(pager,&run,1)==0);
    CHECK(m[PAGE]==0xA1&&r[2*PAGE+9]==0xA2&&faults==4);
    run.source=backing;run.bytes=PAGE;
    CHECK(n_pager_supply(pager,&run,1)==0);
    CHECK(m[PAGE+5]==pattern(PAGE+5)&&r[PAGE]==pattern(PAGE)&&m[2*PAGE]==0xA2&&faults==4);
    other[2*PAGE]=0x77;CHECK(r[2*PAGE]==0x77); /* the frame itself, no copy */
    /* A kernel copy out of an unbound page goes through the pager like a touch. */
    char got[64];
    CHECK(n_channel_send(c[0],(const void *)(m+3*PAGE),64,-1)==64);
    CHECK(faults==5&&last_page==3&&receive(c[1],got,64,NULL)&&!memcmp(got,back+3*PAGE,64));
    /* ... and one the pager fails is a bad address, not a crash. */
    CHECK(n_channel_send(c[0],(const void *)(m+KFAIL_PAGE*PAGE),64,-1)==-N_ERR_BAD_ADDRESS);
    CHECK(faults==6&&last_page==KFAIL_PAGE);
    /* n_memory_protect holds for pages the pager binds later: a page made read-only before it was
       bound stays so after a binding for writing (a kernel copy into it is a bad address), and
       read/write gives the write back. */
    unsigned before=faults;
    volatile unsigned char *pp=m+PROTECT_PAGE*PAGE;
    CHECK(n_memory_protect((void *)pp,PAGE,N_MEMORY_READ)==0); /* nothing bound there yet */
    CHECK(pp[3]==pattern(PROTECT_PAGE*PAGE+3)&&faults==before+1&&last_access==N_MEMORY_READ);
    run=(struct n_pager_supply){.offset=PROTECT_PAGE*PAGE,.bytes=PAGE,.source_offset=PROTECT_PAGE*PAGE,
                                .source=backing,.access=N_MEMORY_READ_WRITE};
    CHECK(n_pager_supply(pager,&run,1)==0);
    CHECK(n_random((void *)pp,16)==-N_ERR_BAD_ADDRESS&&faults==before+1);
    CHECK(back[PROTECT_PAGE*PAGE]==pattern(PROTECT_PAGE*PAGE));
    CHECK(n_memory_protect((void *)pp,PAGE,N_MEMORY_READ_WRITE)==0);
    CHECK(n_channel_send(c[0],"pqrs",4,-1)==4&&receive(c[1],(void *)pp,4,NULL));
    CHECK(back[PROTECT_PAGE*PAGE]=='p'&&faults==before+1);
    /* Another app making the page read-only cannot write it either. */
    n_handle wend=-1;
    n_handle wapp=launch(argv[0],"protect-write",-1,&wend);CHECK(wapp>=0);
    if(wapp>=0){
        CHECK(n_channel_send(wend,"mem",3,memory)==3);
        unsigned char seen=0;CHECK(receive(wend,&seen,1,NULL)&&seen=='p');
        int status=-1;CHECK(n_app_result(wapp,-1,&status)==0&&status==128+14);
        if(status==128+14)printf("pagertest: expected writer fault status=%d (protected page)\n",status); /* test.py lets it be */
        CHECK(back[PROTECT_PAGE*PAGE]=='p'&&faults==before+1);
        n_handle_close(wapp);n_handle_close(wend);
    }
    /* A page supplied from a view goes with the revocation of the view (here its parent's), in
       every mapping: the next touch asks the pager again, and the revoked view binds nothing. */
    n_handle view=n_shared_view_access(alt,0,BYTES,N_CACHE_DEFAULT,N_MEMORY_READ_WRITE);CHECK(view>=0);
    n_handle child=n_shared_view_access(view,0,BYTES,N_CACHE_DEFAULT,N_MEMORY_READ);CHECK(child>=0);
    run=(struct n_pager_supply){.offset=VIEW_PAGE*PAGE,.bytes=PAGE,.source_offset=VIEW_PAGE*PAGE,
                                .source=child,.access=N_MEMORY_READ};
    CHECK(n_pager_supply(pager,&run,1)==0);
    CHECK(m[VIEW_PAGE*PAGE]==0xA9&&r[VIEW_PAGE*PAGE+7]==0xA9&&faults==before+1);
    CHECK(n_shared_revoke(view)==0);
    CHECK(r[VIEW_PAGE*PAGE+7]==pattern(VIEW_PAGE*PAGE+7)&&faults==before+2&&last_page==VIEW_PAGE);
    CHECK(m[VIEW_PAGE*PAGE]==pattern(VIEW_PAGE*PAGE)&&faults==before+2);
    CHECK(n_pager_supply(pager,&run,1)==-N_ERR_CLOSED);
    /* The same frame supplied again through the root: the page answers to the root now, so the
       revocation of the view it came through before leaves it bound. */
    n_handle again=n_shared_view(alt,0,BYTES,N_CACHE_DEFAULT);CHECK(again>=0);
    run.source=again;CHECK(n_pager_supply(pager,&run,1)==0);
    CHECK(m[VIEW_PAGE*PAGE]==0xA9&&faults==before+2);
    run.source=alt;CHECK(n_pager_supply(pager,&run,1)==0);
    CHECK(n_shared_revoke(again)==0);
    CHECK(m[VIEW_PAGE*PAGE]==0xA9&&r[VIEW_PAGE*PAGE]==0xA9&&faults==before+2);
    n_handle_close(view);n_handle_close(child);n_handle_close(again);
    /* A failed touch in another app is an access violation there; its bound pages read fine. */
    expect_fault(argv[0],FAIL_PAGE,"pager failed it");
    /* Nobody else may use the pager, even holding its handle. */
    n_handle end=-1;
    n_handle app=launch(argv[0],"thief",pager,&end);CHECK(app>=0);
    if(app>=0){int status=-1;CHECK(n_app_result(app,-1,&status)==0&&status==0);n_handle_close(app);n_handle_close(end);}

    /* The pager ends: bound pages stay, unbound ones are access violations at once. */
    stop=1;CHECK(thread_join(tid)==0);
    CHECK(n_handle_close(pager)==0);
    CHECK(m[PAGE+5]==pattern(PAGE+5)&&r[3]==0x57);
    CHECK(n_channel_send(c[0],(const void *)(m+5*PAGE),64,-1)==-N_ERR_BAD_ADDRESS);
    expect_fault(argv[0],5,"pager ended");
    CHECK(n_memory_release((void *)m,BYTES)==0&&n_memory_release((void *)r,BYTES)==0);
    CHECK(n_handle_close(memory)==0);
    CHECK(back[1]==0x55&&back[3]==0x57); /* the backing is the pager's own again */

    /* The memory goes (no handle, no mapping): the pager learns it. */
    n_handle mem2=-1;n_handle p2=n_pager_create(PAGE,&mem2);CHECK(p2>=0&&mem2>=0);
    void *m2=NULL;CHECK(n_shared_map(mem2,PAGE,N_MEMORY_READ,&m2)==0);
    CHECK(n_handle_close(mem2)==0);
    CHECK(n_pager_take(p2,&f,1,0)==-N_ERR_TIMEOUT); /* the mapping keeps it */
    CHECK(n_memory_release(m2,PAGE)==0);
    CHECK(n_pager_take(p2,&f,1,-1)==-N_ERR_CLOSED);
    run=(struct n_pager_supply){.offset=0,.bytes=PAGE,.source=backing,.access=N_MEMORY_READ};
    CHECK(n_pager_supply(p2,&run,1)==-N_ERR_CLOSED);
    CHECK(n_handle_close(p2)==0);

    n_handle_close(c[0]);n_handle_close(c[1]);n_handle_close(ro);
    n_memory_release(bm,BYTES);n_memory_release(am,BYTES);
    n_handle_close(backing);n_handle_close(alt);
    printf("ページャー %u 検査 %u 失敗\n",checks,failed);return failed?1:0;
}
