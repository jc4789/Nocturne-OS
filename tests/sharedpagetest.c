/* Real Nocturne shared-page ABI/lifetime check; run in a dedicated test VM.
 * Does not assert decoder, GUI, real-site or Hyper-V acceptance. A shared handle reaches
 * another app only as a channel attachment, so there is no way for an app to name a
 * page it was never sent. */
#include "nocturne.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("共有ページ失敗 行 %d\n",__LINE__);}}while(0)
#define BYTES 8193u
/* n_shared_map takes the page-rounded size of the object. */
#define MAPPED (((size_t)BYTES+N_PAGE_SIZE-1)&~((size_t)N_PAGE_SIZE-1))

/* Waits for one message on channel h; *attached is the shared handle it carried, or -1. */
static bool receive(n_handle h,void *buf,size_t size,n_handle *attached){
    n_result n=n_channel_receive(h,buf,size,N_CHANNEL_WAIT,attached);
    return n==(n_result)size;
}
static n_handle launch(const char *self,const char *mode,n_handle *parent_end){
    n_handle c[2];
    if(n_channel_pair(c)<0)return -1;
    const char *args[]={self,mode};
    int32_t handles[]={c[1],-1,-1};
    struct n_launch l={.arguments=args,.argument_count=2,.handles=handles,.handle_count=3};
    n_handle app=n_app_launch(self,&l);
    n_handle_close(c[1]);
    if(app<0){n_handle_close(c[0]);return app;}
    *parent_end=c[0];
    return app;
}
/* The child makes the page object, fills it and sends it to the parent; the parent
   mapping must outlive this child's handle, mapping and process. */
static int child(void){
    n_handle h=n_shared_create(BYTES);if(h<0)return 2;
    void *m;if(n_shared_map(h,MAPPED,N_MEMORY_READ_WRITE,&m)<0){n_handle_close(h);return 3;}
    unsigned char *p=m;
    for(size_t i=0;i<BYTES;i++)p[i]=(unsigned char)(i*31u+7);
    if(n_channel_send(0,"shared",6,h)!=6)return 4;
    int ack=0;if(!receive(0,&ack,sizeof ack,NULL)||ack!=1||p[8192]!=0x49)return 5;
    n_handle_close(h);
    if(n_memory_release(p,MAPPED)<0)return 6;
    return 0;
}
/* The parent made the object and sent it down: this app maps it read-only and sees the
   parent's later writes through that mapping. */
static int reader(void){
    n_handle h=-1;char tag[4];
    if(!receive(0,tag,sizeof tag,&h)||h<0||n_handle_kind(h)!=N_KIND_SHARED)return 2;
    void *m;if(n_shared_map(h,MAPPED,N_MEMORY_READ,&m)<0)return 3;
    const unsigned char *p=m;
    for(size_t i=0;i<BYTES;i++)if(p[i]!=(unsigned char)(i*13u+5))return 4;
    n_handle_close(h);
    if(n_channel_send(0,"r",1,-1)!=1)return 5;
    char go;if(!receive(0,&go,1,NULL)||p[100]!=0x5c)return 6;
    return n_memory_release((void *)p,MAPPED)<0?7:0;
}
/* A read-only mapping must fault on a write (page fault: status 128+14). */
static int writer_ro(void){
    n_handle h=-1;char tag[4];
    if(!receive(0,tag,sizeof tag,&h)||h<0)return 2;
    void *m;if(n_shared_map(h,MAPPED,N_MEMORY_READ,&m)<0)return 3;
    unsigned char *p=m;
    p[0]=1;
    return 0; /* survived: the mapping was writable */
}
int main(int argc,char **argv){
    char program[4096]; /* argv[0] is as it was typed; an app is launched by its complete path */
    if(n_path_complete(argv[0],program,sizeof program)>=0)argv[0]=program;
    if(argc>1&&!strcmp(argv[1],"child"))return child();
    if(argc>1&&!strcmp(argv[1],"reader"))return reader();
    if(argc>1&&!strcmp(argv[1],"writer-ro"))return writer_ro();
    n_handle h=n_shared_create(BYTES);CHECK(h>=0);if(h<0)return 1;
    CHECK(n_handle_kind(h)==N_KIND_SHARED);
    void *pm,*rom,*none;
    CHECK(n_shared_map(h,MAPPED,N_MEMORY_READ_WRITE,&pm)==0);unsigned char *p=pm;
    CHECK(n_shared_map(h,MAPPED,N_MEMORY_READ,&rom)==0);unsigned char *ro=rom;
    CHECK(n_shared_map(h,4096,N_MEMORY_READ_WRITE,&none)<0&&!none);
    CHECK(n_shared_map(h,MAPPED,N_MEMORY_READ_EXECUTE,&none)<0&&!none);
    n_handle doc=n_document_open("C:\\Nocturne\\Welcome.txt",N_DOCUMENT_READ);
    CHECK(doc>=0&&n_shared_map(doc,MAPPED,N_MEMORY_READ,&none)<0);
    if(doc>=0)n_handle_close(doc);
    CHECK(n_shared_map(0x7fffffff,MAPPED,N_MEMORY_READ,&none)<0);
    CHECK(n_handle_close(h)==0);
    CHECK(n_shared_map(h,MAPPED,N_MEMORY_READ,&none)<0); /* the handle is gone */
    if(p&&ro){p[0]=0x71;p[4096]=0x26;p[8192]=0x58;
        CHECK(ro[0]==0x71&&ro[4096]==0x26&&ro[8192]==0x58);
        CHECK(n_memory_release(p,4096)<0&&p[8192]==0x58);
        CHECK(n_memory_protect(ro,MAPPED,N_MEMORY_READ_WRITE)<0);
        CHECK(n_memory_release(p,MAPPED)==0);
        CHECK(ro[8192]==0x58); /* final mapping remains the page owner */
        CHECK(n_memory_release(ro,MAPPED)==0);
    }
    /* A lazily zeroed object reads as zero in every mapping. */
    n_handle lazy=n_shared_reserve(BYTES);CHECK(lazy>=0);
    if(lazy>=0){
        void *am,*bm;n_shared_map(lazy,MAPPED,N_MEMORY_READ_WRITE,&am);n_shared_map(lazy,MAPPED,N_MEMORY_READ,&bm);
        unsigned char *a=am,*b=bm;
        CHECK(a&&b&&!a[0]&&!b[8192]);
        if(a&&b){a[8192]=0x3c;CHECK(b[8192]==0x3c);}
        n_handle_close(lazy);
        if(a)CHECK(n_memory_release(a,MAPPED)==0);
        if(b)CHECK(n_memory_release(b,MAPPED)==0);
    }

    /* Child to parent: an object created by a child, whose handle travels as an attachment. */
    n_handle reader_end=-1;
    n_handle app=launch(argv[0],"child",&reader_end);CHECK(app>=0);
    if(app>=0){
        n_handle child_h=-1;char tag[6];
        bool got=receive(reader_end,tag,sizeof tag,&child_h);CHECK(got&&child_h>=0&&n_handle_kind(child_h)==N_KIND_SHARED);
        void *im=NULL;if(got)n_shared_map(child_h,MAPPED,N_MEMORY_READ_WRITE,&im);
        unsigned char *imported=im;CHECK(imported!=NULL);
        if(imported){CHECK(imported[0]==7&&imported[8192]==(unsigned char)(8192u*31u+7));imported[8192]=0x49;}
        int ack=imported?1:0;CHECK(n_channel_send(reader_end,&ack,sizeof ack,-1)==(n_result)sizeof ack);
        if(child_h>=0)n_handle_close(child_h);
        if(imported)CHECK(imported[8192]==0x49); /* the mapping keeps the pages after the handle closed */
        int status=-1;CHECK(n_app_result(app,-1,&status)==0&&status==0);
        n_handle_close(app);n_handle_close(reader_end);
        if(imported){CHECK(imported[8192]==0x49);imported[0]=0x93;CHECK(imported[0]==0x93);CHECK(n_memory_release(imported,MAPPED)==0);}
    }

    /* Parent to child: the child maps it read-only; the parent's later writes are visible there. */
    n_handle down=n_shared_create(BYTES);CHECK(down>=0);
    void *dm=NULL;if(down>=0)n_shared_map(down,MAPPED,N_MEMORY_READ_WRITE,&dm);
    unsigned char *dp=dm;CHECK(dp!=NULL);
    if(dp){
        for(size_t i=0;i<BYTES;i++)dp[i]=(unsigned char)(i*13u+5);
        app=launch(argv[0],"reader",&reader_end);CHECK(app>=0);
        if(app>=0){
            CHECK(n_channel_send(reader_end,"down",4,down)==4);
            char r;CHECK(receive(reader_end,&r,1,NULL)&&r=='r');
            dp[100]=0x5c;
            CHECK(n_channel_send(reader_end,"g",1,-1)==1);
            int status=-1;CHECK(n_app_result(app,-1,&status)==0&&status==0);
            n_handle_close(app);n_handle_close(reader_end);
        }
        /* A writer holding only a read-only mapping faults. */
        app=launch(argv[0],"writer-ro",&reader_end);CHECK(app>=0);
        if(app>=0){
            CHECK(n_channel_send(reader_end,"down",4,down)==4);
            int status=-1;CHECK(n_app_result(app,-1,&status)==0&&status==128+14);
            n_handle_close(app);n_handle_close(reader_end);
        }
        CHECK(dp[0]==5); /* the faulting write never reached the page */
        CHECK(n_memory_release(dp,MAPPED)==0);
    }
    if(down>=0)n_handle_close(down);
    printf("共有ページ %u 検査 %u 失敗\n",checks,failed);return failed?1:0;
}
