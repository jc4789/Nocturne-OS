/* The production Fetch data URL processor; no browser/site acceptance claim. */
#include "../user/libc/web/webi.h"
#include <stdio.h>
/* URL canonicalization is a different util.c entry point. Fail if the data
 * processor ever tries to use an HTTP parser or transport in this test. */
enum http_url_result http_canonical_owned_n(const char *input,size_t length,char **url,char **origin){(void)input;(void)length;(void)url;(void)origin;abort();}
static unsigned checks,failed;
#define CHECK(c) do{checks++;if(!(c)){failed++;printf("FAIL data URL:%d %s\n",__LINE__,#c);}}while(0)
static void good(const char *url,const char *mime,const unsigned char *body,size_t length){
    struct web_data_url data;int result=web_data_url_process(url,&data,NULL,NULL);
    CHECK(result==1);if(result!=1)return;
    CHECK(!strcmp(data.mime,mime));CHECK(data.length==length&&!memcmp(data.body,body,length));
    CHECK(data.body[length]==0);web_data_url_free(&data);CHECK(!data.body&&!data.mime&&!data.length);
}
static bool cancel(void *opaque){unsigned *calls=opaque;(*calls)++;return false;}
int main(int argc,char **argv){
    if(argc==3&&!strcmp(argv[1],"--decode")){
        struct web_data_url data;if(web_data_url_process(argv[2],&data,NULL,NULL)!=1)return 2;
        puts(data.mime);for(size_t i=0;i<data.length;i++)printf("%02x",data.body[i]);putchar('\n');web_data_url_free(&data);return 0;
    }
    const unsigned char binary[]={0,0x41,0xff,0x2b,0x2b,0x25,0x25,0x47,0x30};
    good("data:,%00A%FF%2B+%25%G0#lost","text/plain;charset=US-ASCII",binary,sizeof binary);
    good("data:,%23keep#drop","text/plain;charset=US-ASCII",(const unsigned char *)"#keep",5);
    good("data:application%2Fwasm,abc","text/plain;charset=US-ASCII",(const unsigned char *)"abc",3);
    good("data:text/plain; BaSe64,YR","text/plain",(const unsigned char *)"a",1);
    for(unsigned i=0;i<4;i++){
        const char *urls[]={"data:;base64,YQ","data:;base64,YQ==","data:;base64,Y Q==","data:;base64,YQ%0C=="};
        good(urls[i],"text/plain;charset=US-ASCII",(const unsigned char *)"a",1);
    }
    good("data:text/plain;%09base64,YQ==","text/plain",(const unsigned char *)"YQ==",4);
    good("data:;charset=utf-8,x","text/plain;charset=utf-8",(const unsigned char *)"x",1);
    good("data:TEXT/PLAIN;CHARSET=\"utf-8\";charset=bad,x","text/plain;charset=utf-8",(const unsigned char *)"x",1);
    good("data:application/wasm;,x","application/wasm",(const unsigned char *)"x",1);
    good("data:text/X;a=\"x;y\";b=\"\\\"\\\\\";c=\"\";d=skip;d=other,x","text/x;a=\"x;y\";b=\"\\\"\\\\\";c=\"\";d=skip",(const unsigned char *)"x",1);
    good("data:text/plain;a=\"utf-8\"ignored;bad name=no;a=ignored;x=y  ,x","text/plain;a=utf-8;x=y",(const unsigned char *)"x",1);
    good("DATA: text/plain ,x","text/plain",(const unsigned char *)"x",1);
    good("data:text/plain,","text/plain",(const unsigned char *)"",0);
    const char *bad[]={"data:","data:text/plain#missing,comma","data:;base64,YQ=","data:;base64,YQ===","data:;base64,=YQ=","data:;base64,YQ==%00evil","data:;base64,YQ-_","data:;base64,A","data:;base64,YQ%0B==","http://example.test/"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++){struct web_data_url data;CHECK(web_data_url_process(bad[i],&data,NULL,NULL)==0);CHECK(!data.body&&!data.mime);}
    struct web_data_url data;unsigned calls=0;CHECK(web_data_url_process("data:,abc",&data,cancel,&calls)==-2);CHECK(calls==1&&!data.body&&!data.mime);
    /* Valid metadata exceeding the old loader's 128-byte essence buffer. */
    const size_t extra=4096;char *url=malloc(extra+64),*mime=malloc(extra+32);CHECK(url&&mime);
    if(url&&mime){strcpy(url,"data:text/plain;x=");memset(url+18,'a',extra);strcpy(url+18+extra,",ok");strcpy(mime,"text/plain;x=");memset(mime+13,'a',extra);mime[13+extra]=0;good(url,mime,(const unsigned char *)"ok",2);}free(url);free(mime);
    printf("data_url_hosttest: %u checks, %u failed\n",checks,failed);return failed!=0;
}
