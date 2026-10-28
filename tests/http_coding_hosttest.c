/* Feed the product content-coding decoders across every byte/bit boundary on the host:
     http_coding_hosttest "<Content-Encoding value>" [chunk] < coded > decoded
   A list this decoder leaves alone (Fetch: the codings are not supported) is copied as it is.
   Built with user/libc/third_party_brotli.c and third_party/brotli/c/{common,dec}. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <io.h>
struct http_resp { char error[256]; };
struct sink { struct http_resp *rs; struct http_coding *coding; size_t codings; };
static bool sink_plain_put(struct sink *sink,const char *bytes,size_t n){
    (void)sink;return fwrite(bytes,1,n,stdout)==n;
}
#include "../user/libc/http_coding.h"
int main(int argc,char **argv){
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
    if(argc<2)return 2;
    size_t chunk=argc>2?(size_t)atoi(argv[2]):1;if(!chunk||chunk>8192)return 2;
    struct http_resp response={0};struct sink sink={&response,NULL,0};
    int made=coding_create(&sink,argv[1]);if(made<0){coding_free(&sink);return 2;}
    char input[8192];size_t n;bool ok=true;
    while((n=fread(input,1,chunk,stdin)))if(!(made?coding_put(&sink,0,input,n):sink_plain_put(&sink,input,n))){ok=false;break;}
    if(ok&&made)ok=coding_finish(&sink);
    coding_free(&sink);
    if(!ok)fprintf(stderr,"%s\n",response.error);return ok?0:1;
}
