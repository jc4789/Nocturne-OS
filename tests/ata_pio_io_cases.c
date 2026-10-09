/* New sector-string-I/O boundary: isolated QA FAT only, never a user disk. */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
int main(void) {
    const char *path="/data/ata-pio-once.bin";
    const size_t length=65537;
    unsigned char *allocation=malloc(length+3),*bytes=allocation?allocation+1:NULL;
    if(!bytes)return 1;
    for(size_t i=0;i<length;i++)bytes[i]=(unsigned char)((i*37u+(i>>8))^0xa5u);
    uint64_t start=uptime_ms();
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC),failed=fd<0;
    for(size_t at=0;!failed&&at<length;){
        size_t chunk=length-at<4093?length-at:4093;
        ssize_t n=write(fd,bytes+at,chunk);
        if(n<0&&errno==EINTR)continue;
        if(n<=0 || (size_t)n>chunk){failed=1;break;}
        at+=(size_t)n;
    }
    if(fd>=0&&close(fd)<0)failed=1;
    fd=failed?-1:open(path,O_RDONLY);
    if(fd<0)failed=1;
    unsigned char readback[1031];size_t count=0;
    while(!failed&&count<length){
        ssize_t n=read(fd,readback+1,sizeof(readback)-1);
        if(n<0&&errno==EINTR)continue;
        if(n<=0 || (size_t)n>length-count){failed=1;break;}
        for(size_t i=0;i<(size_t)n;i++)if(readback[i+1]!=bytes[count+i]){failed=1;break;}
        count+=(size_t)n;
    }
    if(fd>=0){unsigned char extra;if(!failed&&read(fd,&extra,1)!=0)failed=1;if(close(fd)<0)failed=1;}
    if(unlink(path)<0)failed=1;
    printf("ATA_PIO_NEW_BOUNDARY bytes=%lu elapsed=%lu ms result=%s\n",(unsigned long)count,
        (unsigned long)(uptime_ms()-start),failed?"FAIL":"OK");
    free(allocation);return failed;
}
