/* Feed the product decoder across every byte/bit boundary on the host. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
struct http_resp { char error[256]; };
struct sink { struct http_resp *rs; };
static bool sink_plain_put(struct sink *sink,const char *bytes,size_t n){
    (void)sink;return fwrite(bytes,1,n,stdout)==n;
}
#include "../user/libc/http_gzip.h"
int main(int argc,char **argv){
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    size_t chunk=argc>1?(size_t)atoi(argv[1]):1;if(!chunk||chunk>8192)return 2;
    struct http_resp response={0};struct sink sink={&response};struct http_gzip *g=gzip_create();
    if(!g)return 2;char input[8192];size_t n;bool ok=true;
    while((n=fread(input,1,chunk,stdin)))if(!gzip_feed(g,&sink,input,n,false)){ok=false;break;}
    if(ok)ok=gzip_feed(g,&sink,NULL,0,true);
    if(!ok)fprintf(stderr,"%s\n",response.error);free(g);return ok?0:1;
}
