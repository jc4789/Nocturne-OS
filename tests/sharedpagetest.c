/* Real Nocturne shared-page ABI/lifetime check; run in a dedicated test VM.
 * Does not assert decoder, GUI, real-site or Hyper-V acceptance. */
#include "nocturne.h"
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static unsigned checks,failed;
#define CHECK(x) do{checks++;if(!(x)){failed++;printf("共有ページ失敗 行 %d\n",__LINE__);}}while(0)
static bool exact(int fd,void *bytes,size_t size,bool writing){
    size_t at=0;while(at<size){ssize_t n=writing?write(fd,(char *)bytes+at,size-at):read(fd,(char *)bytes+at,size-at);
        if(n<0&&errno==EINTR)continue;if(n<=0)return false;at+=(size_t)n;
    }return true;
}
static int child(void){
    const size_t bytes=8193;int fd=n_shared_create(bytes);if(fd<0)return 2;
    unsigned char *p=n_shared_map(0,fd,bytes,N_PROT_WRITE);if(!p){close(fd);return 3;}
    for(size_t i=0;i<bytes;i++)p[i]=(unsigned char)(i*31u+7);
    if(!exact(1,&fd,sizeof fd,true))return 4;
    int ack=0;if(!exact(0,&ack,sizeof ack,false)||ack!=1||p[8192]!=0x49)return 5;
    /* A parent mapping must outlive this descriptor, mapping and process. */
    close(fd);if(munmap(p,bytes)<0)return 6;return 0;
}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"child"))return child();
    const size_t bytes=8193;
    int fd=n_shared_create(bytes);CHECK(fd>=0);if(fd<0)return 1;
    unsigned char *p=n_shared_map(0,fd,bytes,N_PROT_WRITE);CHECK(p!=NULL);
    unsigned char *ro=n_shared_map(0,fd,bytes,0);CHECK(ro!=NULL);
    CHECK(n_shared_map(0,fd,4096,N_PROT_WRITE)==NULL);
    CHECK(n_shared_map(0,fd,bytes,N_PROT_EXEC)==NULL);
    CHECK(n_shared_map(0,1,bytes,0)==NULL);
    CHECK(n_shared_map(0x7fffffff,fd,bytes,0)==NULL);
    CHECK(close(fd)==0);
    if(p&&ro){p[0]=0x71;p[4096]=0x26;p[8192]=0x58;
        CHECK(ro[0]==0x71&&ro[4096]==0x26&&ro[8192]==0x58);
        CHECK(munmap(p,4096)<0&&p[8192]==0x58);
        CHECK(mprotect(ro,bytes,PROT_READ|PROT_WRITE)<0);
        CHECK(munmap(p,bytes)==0);
        CHECK(ro[8192]==0x58); /* final mapping remains the page owner */
        CHECK(munmap(ro,bytes)==0);
    }
    int command[2]={-1,-1},response[2]={-1,-1};
    int made=pipe(command);CHECK(made==0);if(made<0)return 1;
    made=pipe(response);CHECK(made==0);if(made<0){close(command[0]);close(command[1]);return 1;}
    int nullfd=open("/dev/null",O_WRONLY);int map[3]={command[0],response[1],nullfd};
    char *args[]={argv[0],"child",NULL};int pid=spawn(argv[0],args,map,0);CHECK(pid>0);
    close(command[0]);close(response[1]);close(nullfd);
    if(pid>0){int child_fd=-1;bool got=exact(response[0],&child_fd,sizeof child_fd,false);CHECK(got&&child_fd>=0);
        unsigned char *imported=got?n_shared_map(pid,child_fd,bytes,N_PROT_WRITE):NULL;CHECK(imported!=NULL);
        if(imported){CHECK(imported[0]==7&&imported[8192]==7);imported[8192]=0x49;}
        int ack=imported?1:0;CHECK(exact(command[1],&ack,sizeof ack,true));close(command[1]);close(response[0]);
        int status=-1;CHECK(waitpid(pid,&status,0)==pid&&status==0);
        if(imported){CHECK(imported[8192]==0x49);imported[0]=0x93;CHECK(imported[0]==0x93);CHECK(munmap(imported,bytes)==0);}
    }else {close(command[1]);close(response[0]);}
    printf("共有ページ %u 検査 %u 失敗\n",checks,failed);return failed?1:0;
}
