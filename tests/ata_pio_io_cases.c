/* New sector-string-I/O boundary: isolated test disk only, never a user disk. The document lives in
   the temporary folder and is removed again. */
#include <nocturne.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    char path[4096];
    if(n_path_join(n_temp_folder(),"ata-pio-once.bin",path,sizeof path)<0)return 1;
    const size_t length=65537;
    unsigned char *allocation=malloc(length+3),*bytes=allocation?allocation+1:NULL;
    if(!bytes)return 1;
    for(size_t i=0;i<length;i++)bytes[i]=(unsigned char)((i*37u+(i>>8))^0xa5u);
    uint64_t start=uptime_ms();
    n_handle document=n_document_open(path,N_DOCUMENT_CREATE);
    int failed=document<0;
    for(size_t at=0;!failed&&at<length;){
        size_t chunk=length-at<4093?length-at:4093;
        n_result n=n_document_write_at(document,at,bytes+at,chunk);
        if(n<=0 || (size_t)n>chunk){failed=1;break;}
        at+=(size_t)n;
    }
    if(!failed&&n_document_commit(document,N_COMMIT_WAIT)<0)failed=1;
    if(document>=0&&n_handle_close(document)<0)failed=1;
    document=failed?-1:n_document_open(path,N_DOCUMENT_READ);
    if(document<0)failed=1;
    unsigned char readback[1031];size_t count=0;
    while(!failed&&count<length){
        n_result n=n_document_read_at(document,count,readback+1,sizeof(readback)-1);
        if(n<=0 || (size_t)n>length-count){failed=1;break;}
        for(size_t i=0;i<(size_t)n;i++)if(readback[i+1]!=bytes[count+i]){failed=1;break;}
        count+=(size_t)n;
    }
    if(document>=0){unsigned char extra;if(!failed&&n_document_read_at(document,count,&extra,1)!=0)failed=1;if(n_handle_close(document)<0)failed=1;}
    if(n_entry_remove(path)<0)failed=1;
    printf("ATA_PIO_NEW_BOUNDARY bytes=%lu elapsed=%lu ms result=%s\n",(unsigned long)count,
        (unsigned long)(uptime_ms()-start),failed?"FAIL":"OK");
    free(allocation);return failed;
}
