/* The content codings of an HTTP response (RFC 9110 §8.4), undone while the body
 * arrives: gzip (RFC 1952, members one after another), deflate (RFC 1950's zlib
 * wrapper, or a bare RFC 1951 stream as some servers send it) and br (RFC 7932).
 * The decoders are libc's own: DEFLATE's in deflate.c, brotli's behind
 * brotli_codec.h. The sink holds them in the order they are undone, the last one
 * the server applied first; each passes its output to the next and the last one
 * to the HTTP sink (and its cancellation and backpressure). Input is never
 * retained: gzip and deflate keep only the bit reservoir, Huffman tables and the
 * 32 KiB distance window.
 */
#include "deflate.h"
#include "brotli_codec.h"
enum { CODING_GZIP, CODING_DEFLATE, CODING_BR };
#define HTTP_BR_OUTPUT 16384u
struct http_coding {
    unsigned kind;
    bool fed;                     /* a byte reached it: a coded body of no bytes is empty */
    bool done;                    /* br: the stream ended */
    struct inflate_stream *inflate; /* gzip and deflate */
    void *br;                     /* br: the decoder */
    unsigned char *output;        /* br: HTTP_BR_OUTPUT bytes */
    struct sink *sink;            /* gzip and deflate: the sink of the call in progress */
    size_t next;                  /* the stage its output goes to */
};
static bool coding_put(struct sink *sink,size_t stage,const char *data,size_t n);
static bool coding_inflated(void *context,const uint8_t *bytes,size_t n){
    struct http_coding *c=context;
    return coding_put(c->sink,c->next,(const char *)bytes,n);
}
/* Nothing may follow the end of a zlib or bare stream, nor bytes that are not a
   gzip member after the last member. */
static bool inflate_stage(struct http_coding *c,struct sink *sink,const char *input,size_t n,bool finish){
    size_t used=0;
    c->sink=sink;
    enum inflate_result result=inflate_feed(c->inflate,input,n,finish,&used);
    if(result==INFLATE_STOPPED)return false; /* a later stage failed and said why */
    if(result==INFLATE_MORE||(result==INFLATE_END&&used==n))return true;
    snprintf(sink->rs->error,sizeof sink->rs->error,"invalid or truncated %s response",c->kind==CODING_GZIP?"gzip":"deflate");
    return false;
}
static bool br_feed(struct http_coding *c,struct sink *sink,size_t next,const char *input,size_t n,bool finish){
    static const unsigned char none;
    const unsigned char *in=input?(const unsigned char *)input:&none;
    for(;;){
        if(c->done){if(n)goto invalid;break;}
        unsigned char *out=c->output;size_t room=HTTP_BR_OUTPUT;
        int result=brotli_decode(c->br,&in,&n,&out,&room);
        if(out>c->output&&!coding_put(sink,next,(const char *)c->output,(size_t)(out-c->output)))return false;
        if(result<0)goto invalid;
        if(result>0)c->done=true;
        else if(room)break; /* every byte taken: the rest comes with the next read */
    }
    if(finish&&!c->done)goto invalid;
    return true;
invalid:
    snprintf(sink->rs->error,sizeof sink->rs->error,"invalid or truncated br response");return false;
}
static bool coding_put(struct sink *sink,size_t stage,const char *data,size_t n){
    if(stage==sink->codings)return sink_plain_put(sink,data,n);
    struct http_coding *c=&sink->coding[stage];
    if(n)c->fed=true;
    return c->kind==CODING_BR?br_feed(c,sink,stage+1,data,n,false):inflate_stage(c,sink,data,n,false);
}
/* The body ended: each coding that got a byte must have ended too. Stage by
   stage, so what one still holds reaches the next before that one is asked. */
static bool coding_finish(struct sink *sink){
    for(size_t i=0;i<sink->codings;i++){
        struct http_coding *c=&sink->coding[i];
        if(!c->fed)continue;
        if(!(c->kind==CODING_BR?br_feed(c,sink,i+1,NULL,0,true):inflate_stage(c,sink,NULL,0,true)))return false;
    }
    return true;
}
static bool coding_tchar(unsigned char c){
    return (c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c&&strchr("!#$%&'*+-.^_`|~",c));
}
/* CODING_*, -1 for identity (no coding), -2 for one this decoder does not have. */
static int coding_kind(const char *p,size_t n){
    if((n==4&&!strncasecmp(p,"gzip",4))||(n==6&&!strncasecmp(p,"x-gzip",6)))return CODING_GZIP;
    if(n==7&&!strncasecmp(p,"deflate",7))return CODING_DEFLATE;
    if(n==2&&!strncasecmp(p,"br",2))return CODING_BR;
    if(n==8&&!strncasecmp(p,"identity",8))return -1;
    return -2;
}
static void coding_free(struct sink *sink){
    for(size_t i=0;i<sink->codings;i++){
        struct http_coding *c=&sink->coding[i];
        if(c->inflate)inflate_end(c->inflate);if(c->br)brotli_decoder_end(c->br);free(c->output);
    }
    free(sink->coding);sink->coding=NULL;sink->codings=0;
}
/* value: every Content-Encoding field's list, in order and joined by commas
   (RFC 9110 §5.3, §8.4); empty elements are ignored. Fetch's "handle content
   codings" leaves the bytes as they are when a coding is not supported or the
   list does not parse, so that is 0, as is a list naming only identity. 1: the
   decoders are in sink->coding, the last applied first. -1: out of memory
   (coding_free releases what was made). */
static int coding_create(struct sink *sink,const char *value){
    size_t count=0;
    for(int pass=0;pass<2;pass++){
        size_t at=count;
        for(const char *p=value;;p++){
            while(*p==' '||*p=='\t')p++;
            const char *start=p;while(*p&&*p!=',')p++;
            const char *end=p;while(end>start&&(end[-1]==' '||end[-1]=='\t'))end--;
            if(end>start){
                for(const char *c=start;c<end;c++)if(!coding_tchar((unsigned char)*c))return 0;
                int kind=coding_kind(start,(size_t)(end-start));
                if(kind==-2)return 0;
                if(kind>=0){if(pass)sink->coding[--at].kind=(unsigned)kind;else count++;}
            }
            if(!*p)break;
        }
        if(!pass){
            if(!count)return 0;
            if(!(sink->coding=calloc(count,sizeof *sink->coding)))return -1;
            sink->codings=count;
        }
    }
    for(size_t i=0;i<count;i++){
        struct http_coding *c=&sink->coding[i];
        c->next=i+1;
        if(c->kind==CODING_BR){if(!(c->br=brotli_decoder_begin())||!(c->output=malloc(HTTP_BR_OUTPUT)))return -1;}
        else if(!(c->inflate=c->kind==CODING_GZIP?inflate_begin(DEFLATE_GZIP,INFLATE_MEMBERS,coding_inflated,c):
                                                   inflate_begin(DEFLATE_ZLIB,INFLATE_ZLIB_OR_RAW,coding_inflated,c)))return -1;
    }
    return 1;
}
