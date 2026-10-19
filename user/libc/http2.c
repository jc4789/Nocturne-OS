/* HTTP/2 and HPACK, implemented from RFC 9113 and RFC 7541, without OS code.
   One connection mutex serializes the TLS engine. It is never held during a
   blocking receive or an application callback. Callers pump bounded frame
   work, so multiple native request tasks share one wire connection. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include "http2.h"
extern uint64_t uptime_ms(void);
extern void yield(void);
extern int msleep(unsigned);

/* Each stream may buffer as much as one HTTP/1.1 connection's TCP receive
   storage, so multiplexing never narrows a transfer below the separate
   connection it replaces. Stream credit is returned on consumption. */
#define H2_WINDOW (1024u*1024u)
/* Connection credit is returned when DATA is queued, so this only bounds
   bytes in flight on the wire; stream windows bound buffered memory. */
#define H2_CONNECTION_WINDOW (16u*1024u*1024u)
#define H2_HEADERS (1024u*1024u) /* advertised field-list memory budget */
#define H2_STREAMS 64u
#define H2_OUTPUT_MAX (4u*1024u*1024u)
#define H2_CONTROL_RESERVE (64u*1024u)
#define H2_APPLICATION_OUTPUT (H2_OUTPUT_MAX-H2_CONTROL_RESERVE)
/* Refresh after a quarter window: few WINDOW_UPDATE frames, no stall. */
#define H2_CREDIT_BATCH (H2_WINDOW/4u)
#define H2_CONNECTION_CREDIT_BATCH (H2_CONNECTION_WINDOW/16u)
struct hpack_entry { struct hpack_entry *next; char *name,*value; size_t name_len,value_len,size; };
struct h2_data { struct h2_data *next; size_t at,len; unsigned char bytes[]; };
struct h2_output { struct h2_output *next; size_t at,len; uint32_t stream; unsigned type,flags; unsigned char bytes[]; };
struct http2_stream {
    struct http2_connection *connection;
    struct http2_stream *next;
    const struct http_req *request;
    const struct http_stream_hooks *hooks;
    struct http_transport_result *result;
    uint32_t id;
    int64_t send_window;
    uint32_t recv_window;
    uint32_t pending_credit;
    size_t sent,reported,received,content_length;
    size_t accepted;
    bool have_length,headers_ready,ended,local_end,head,body_forbidden;
    int error;
    char message[128];
    char *headers; size_t headers_len,headers_at;
    struct h2_data *first,*last;
    uint64_t progress,timeout;
};
struct bytes{unsigned char *p;size_t n,cap;};
/* Incremental HPACK state: a header block may be split across any number of
   HEADERS/CONTINUATION frames and string/integer boundaries. */
struct hpack_decoder {
    uint32_t stream;
    unsigned state,action,shift,tail_bits;
    size_t number,remaining,decoded,name_len,value_len;
    unsigned node;uint32_t tail;
    int status;
    bool discard,trailers,regular,have_status,add,any,huffman;
    struct bytes name,value,fields;
};
/* FIFO admission for new streams; lives on the starting caller's stack. */
struct h2_starter { struct h2_starter *next; };
struct http2_connection {
    net_stream *wire;
    uint32_t lock,next_id,active,max_streams,max_frame,initial_window;
    int64_t send_window;
    bool dead,goaway,settings;
    uint32_t continuation;
    bool header_end;
    unsigned char frame_header[9],*frame,*block;
    size_t frame_header_at,frame_at,frame_len,block_len,block_cap;
    struct http2_stream *streams;
    struct hpack_entry *table;
    size_t table_size,table_max;
    /* Our encoder's dynamic table, as the peer's decoder holds it. Header
       blocks are encoded and queued in wire order under the lock, and a
       queued block is never withdrawn once it may have changed the table. */
    struct hpack_entry *encoder;
    size_t encoder_size,encoder_max,encoder_low;
    bool encoder_resize;
    struct h2_output *output,*output_last;
    size_t output_bytes;
    uint32_t output_continuation;
    uint32_t send_cursor,recv_window,pending_credit;
    unsigned header_waiters;
    bool local_settings_sent;
    struct hpack_decoder decoder;
    struct h2_starter *start_first,*start_last;
};
static void enter(struct http2_connection *c) {
    while(__atomic_exchange_n(&c->lock,1,__ATOMIC_ACQUIRE))yield();
}
static void leave(struct http2_connection *c){__atomic_store_n(&c->lock,0,__ATOMIC_RELEASE);}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put32(unsigned char *p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static void stream_fail(struct http2_stream *s,int error,const char *message) {
    if(s->error)return;s->error=error;snprintf(s->message,sizeof s->message,"%s",message);
}
static void fail(struct http2_connection *c,int error,const char *message) {
    c->dead=true;for(struct http2_stream *s=c->streams;s;s=s->next)if(!s->ended)stream_fail(s,error,message);
}
static struct h2_output *packet(unsigned type,unsigned flags,uint32_t id,const void *data,size_t n) {
    struct h2_output *o=malloc(sizeof *o+9+n);
    if(!o)return NULL;
    o->next=NULL;o->at=0;o->len=9+n;o->stream=(type==0||type==1||type==9)?id:0;
    o->type=type;o->flags=flags;
    o->bytes[0]=n>>16;o->bytes[1]=n>>8;o->bytes[2]=n;o->bytes[3]=type;o->bytes[4]=flags;put32(o->bytes+5,id);
    if(n)memcpy(o->bytes+9,data,n);
    return o;
}
/* Control frames can pass queued DATA, but never a partially accepted frame,
   an open HEADERS/CONTINUATION block, or the mandatory client preface/SETTINGS. */
static struct h2_output *control_boundary(struct http2_connection *c) {
    struct h2_output *anchor=NULL,*head=c->output;
    if(!c->local_settings_sent) {
        for(struct h2_output *o=head;o;o=o->next) {
            if(o->type==255)anchor=o;
            if(o->type==4&&!(o->flags&1)){anchor=o;break;}
        }
    }
    if(head&&(c->output_continuation||head->at)) {
        struct h2_output *partial=head;
        bool continuation=c->output_continuation||((head->type==1||head->type==9)&&!(head->flags&4));
        if(continuation)for(struct h2_output *o=head;o;o=o->next)
            if((o->type==1||o->type==9)&&(o->flags&4)){partial=o;break;}
        if(!anchor)anchor=partial;
        else for(struct h2_output *o=head;o;o=o->next){if(o==partial)break;if(o==anchor){anchor=partial;break;}}
    }
    return anchor;
}
static bool frame(struct http2_connection *c,unsigned type,unsigned flags,uint32_t id,const void *data,size_t n) {
    if(c->dead)return false;
    if(c->output_bytes>H2_OUTPUT_MAX||n+9>H2_OUTPUT_MAX-c->output_bytes) {
        fail(c,ENOBUFS,"HTTP/2 output control queue exhausted");return false;
    }
    struct h2_output *o=packet(type,flags,id,data,n);
    if(!o){fail(c,ENOMEM,"HTTP/2 send allocation failed");return false;}
    bool control=type!=0&&type!=1&&type!=9;
    if(control&&c->output) {
        struct h2_output *anchor=control_boundary(c);
        if(anchor){o->next=anchor->next;anchor->next=o;if(c->output_last==anchor)c->output_last=o;}
        else {o->next=c->output;c->output=o;}
    }else {if(c->output_last)c->output_last->next=o;else c->output=o;c->output_last=o;}
    c->output_bytes+=9+n;
    return true;
}
static void update(struct http2_connection *c,uint32_t id,uint32_t count) {
    if(!count||c->dead)return;unsigned char p[4];put32(p,count);frame(c,8,0,id,p,4);
}
static void reset(struct http2_connection *c,uint32_t id,uint32_t error) {
    unsigned char p[4];put32(p,error);frame(c,3,0,id,p,4);
}
static void connection_credit(struct http2_connection *c,uint32_t count) {
    c->pending_credit+=count;
    if(c->pending_credit>=H2_CONNECTION_CREDIT_BATCH) {
        uint32_t amount=c->pending_credit;c->pending_credit=0;c->recv_window+=amount;update(c,0,amount);
    }
}
static void stream_credit(struct http2_stream *s,uint32_t count) {
    s->pending_credit+=count;
    if(s->pending_credit>=H2_CREDIT_BATCH&&!s->ended&&!s->error) {
        uint32_t amount=s->pending_credit;s->pending_credit=0;s->recv_window+=amount;update(s->connection,s->id,amount);
    }
}
static struct http2_stream *find_stream(struct http2_connection *c,uint32_t id) {
    for(struct http2_stream *s=c->streams;s;s=s->next)if(s->id==id)return s;return NULL;
}
static void drain(struct http2_connection *c) {
    size_t budget=262144;
    while(c->output&&budget&&!c->dead) {
        struct h2_output *o=c->output;size_t count=o->len-o->at;if(count>budget)count=budget;
        long n=ns_try_write(c->wire,o->bytes+o->at,count);
        if(n==-2)return;if(n<=0){fail(c,errno?errno:ECONNRESET,ns_error(c->wire));return;}
        struct http2_stream *s=find_stream(c,o->stream);
        if(s){s->accepted+=(size_t)n;
            if(s->result)s->result->request_bytes_sent+=(size_t)n;}
        o->at+=(size_t)n;budget-=(size_t)n;c->output_bytes-=(size_t)n;
        if(o->at==o->len){
            if(o->type==1||o->type==9)c->output_continuation=o->flags&4?0:o->stream;
            if(o->type==4&&!(o->flags&1))c->local_settings_sent=true;
            c->output=o->next;if(!c->output)c->output_last=NULL;free(o);
        }
    }
    if(!c->output&&!c->dead&&ns_try_write(c->wire,NULL,0)==-1)fail(c,errno?errno:EIO,ns_error(c->wire));
}
/* Preserve a partially accepted frame and every queued HEADERS/CONTINUATION:
   their field block already changed the shared HPACK dynamic table, so the
   peer must decode it even when the request is abandoned. Unsent DATA has not
   entered the TLS engine and can be removed safely. Return reserved DATA
   credit before another stream is selected. RST is inserted at the first legal
   frame boundary after the retained frames, ahead of unrelated uploads. */
static void cancel_output(struct http2_connection *c,struct http2_stream *s,bool send_reset) {
    struct h2_output **at=&c->output,*last=NULL,*retained=NULL;bool removed=false,headers=false;
    while(*at) {
        struct h2_output *o=*at;
        if(o->stream==s->id&&!o->at&&o->type==0) {
            *at=o->next;c->output_bytes-=o->len;removed=true;
            c->send_window+=(int64_t)(o->len-9);s->send_window+=(int64_t)(o->len-9);
            free(o);continue;
        }
        if(o->stream==s->id){retained=o;if(o->type==1||o->type==9)headers=true;}
        last=o;at=&o->next;
    }
    c->output_last=last;
    if(c->dead||(!s->accepted&&!headers)||(!send_reset&&!removed&&!headers))return;
    struct h2_output *anchor=NULL,*head=c->output;
    if(head&&(c->output_continuation||head->at)) {
        anchor=head;
        bool continuation=c->output_continuation||((head->type==1||head->type==9)&&!(head->flags&4));
        if(continuation)for(struct h2_output *o=head;o;o=o->next)if((o->type==1||o->type==9)&&(o->flags&4)){anchor=o;break;}
    }
    if(retained) {
        if(!anchor)anchor=retained;
        else for(struct h2_output *o=head;o;o=o->next){if(o==retained)break;if(o==anchor){anchor=retained;break;}}
    }
    unsigned char p[4];put32(p,8);struct h2_output *rst=packet(3,0,s->id,p,4);
    if(!rst){fail(c,ENOMEM,"HTTP/2 cancellation allocation failed");return;}
    if(anchor){rst->next=anchor->next;anchor->next=rst;if(c->output_last==anchor)c->output_last=rst;}
    else {rst->next=c->output;c->output=rst;if(!c->output_last)c->output_last=rst;}
    c->output_bytes+=rst->len;
}

/* RFC 7541 Appendix A/B protocol tables. Data, not imported implementation. */
static const char *const static_name[]={"",":authority",":method",":method",":path",":path",":scheme",":scheme",":status",":status",":status",":status",":status",":status",":status",
    "accept-charset","accept-encoding","accept-language","accept-ranges","accept","access-control-allow-origin","age","allow","authorization","cache-control","content-disposition","content-encoding","content-language","content-length","content-location","content-range","content-type","cookie","date","etag","expect","expires","from","host","if-match","if-modified-since","if-none-match","if-range","if-unmodified-since","last-modified","link","location","max-forwards","proxy-authenticate","proxy-authorization","range","referer","refresh","retry-after","server","set-cookie","strict-transport-security","transfer-encoding","user-agent","vary","via","www-authenticate"};
static const char *const static_value[]={"","","GET","POST","/","/index.html","http","https","200","204","206","304","400","404","500","","gzip, deflate"};
struct huff_code{uint32_t code;unsigned char bits;};
static const struct huff_code huffman[257]={
 {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},{0xfffffe4,28},{0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},
 {0xfffffe8,28},{0xffffea,24},{0x3ffffffc,30},{0xfffffe9,28},{0xfffffea,28},{0x3ffffffd,30},{0xfffffeb,28},{0xfffffec,28},
 {0xfffffed,28},{0xfffffee,28},{0xfffffef,28},{0xffffff0,28},{0xffffff1,28},{0xffffff2,28},{0x3ffffffe,30},{0xffffff3,28},
 {0xffffff4,28},{0xffffff5,28},{0xffffff6,28},{0xffffff7,28},{0xffffff8,28},{0xffffff9,28},{0xffffffa,28},{0xffffffb,28},
 {0x14,6},{0x3f8,10},{0x3f9,10},{0xffa,12},{0x1ff9,13},{0x15,6},{0xf8,8},{0x7fa,11},{0x3fa,10},{0x3fb,10},{0xf9,8},{0x7fb,11},{0xfa,8},{0x16,6},{0x17,6},{0x18,6},
 {0,5},{1,5},{2,5},{0x19,6},{0x1a,6},{0x1b,6},{0x1c,6},{0x1d,6},{0x1e,6},{0x1f,6},{0x5c,7},{0xfb,8},{0x7ffc,15},{0x20,6},{0xffb,12},{0x3fc,10},
 {0x1ffa,13},{0x21,6},{0x5d,7},{0x5e,7},{0x5f,7},{0x60,7},{0x61,7},{0x62,7},{0x63,7},{0x64,7},{0x65,7},{0x66,7},{0x67,7},{0x68,7},{0x69,7},{0x6a,7},
 {0x6b,7},{0x6c,7},{0x6d,7},{0x6e,7},{0x6f,7},{0x70,7},{0x71,7},{0x72,7},{0xfc,8},{0x73,7},{0xfd,8},{0x1ffb,13},{0x7fff0,19},{0x1ffc,13},{0x3ffc,14},{0x22,6},
 {0x7ffd,15},{3,5},{0x23,6},{4,5},{0x24,6},{5,5},{0x25,6},{0x26,6},{0x27,6},{6,5},{0x74,7},{0x75,7},{0x28,6},{0x29,6},{0x2a,6},{7,5},
 {0x2b,6},{0x76,7},{0x2c,6},{8,5},{9,5},{0x2d,6},{0x77,7},{0x78,7},{0x79,7},{0x7a,7},{0x7b,7},{0x7ffe,15},{0x7fc,11},{0x3ffd,14},{0x1ffd,13},{0xffffffc,28},
 {0xfffe6,20},{0x3fffd2,22},{0xfffe7,20},{0xfffe8,20},{0x3fffd3,22},{0x3fffd4,22},{0x3fffd5,22},{0x7fffd9,23},{0x3fffd6,22},{0x7fffda,23},{0x7fffdb,23},{0x7fffdc,23},{0x7fffdd,23},{0x7fffde,23},{0xffffeb,24},{0x7fffdf,23},
 {0xffffec,24},{0xffffed,24},{0x3fffd7,22},{0x7fffe0,23},{0xffffee,24},{0x7fffe1,23},{0x7fffe2,23},{0x7fffe3,23},{0x7fffe4,23},{0x1fffdc,21},{0x3fffd8,22},{0x7fffe5,23},{0x3fffd9,22},{0x7fffe6,23},{0x7fffe7,23},{0xffffef,24},
 {0x3fffda,22},{0x1fffdd,21},{0xfffe9,20},{0x3fffdb,22},{0x3fffdc,22},{0x7fffe8,23},{0x7fffe9,23},{0x1fffde,21},{0x7fffea,23},{0x3fffdd,22},{0x3fffde,22},{0xfffff0,24},{0x1fffdf,21},{0x3fffdf,22},{0x7fffeb,23},{0x7fffec,23},
 {0x1fffe0,21},{0x1fffe1,21},{0x3fffe0,22},{0x1fffe2,21},{0x7fffed,23},{0x3fffe1,22},{0x7fffee,23},{0x7fffef,23},{0xfffea,20},{0x3fffe2,22},{0x3fffe3,22},{0x3fffe4,22},{0x7ffff0,23},{0x3fffe5,22},{0x3fffe6,22},{0x7ffff1,23},
 {0x3ffffe0,26},{0x3ffffe1,26},{0xfffeb,20},{0x7fff1,19},{0x3fffe7,22},{0x7ffff2,23},{0x3fffe8,22},{0x1ffffec,25},{0x3ffffe2,26},{0x3ffffe3,26},{0x3ffffe4,26},{0x7ffffde,27},{0x7ffffdf,27},{0x3ffffe5,26},{0xfffff1,24},{0x1ffffed,25},
 {0x7fff2,19},{0x1fffe3,21},{0x3ffffe6,26},{0x7ffffe0,27},{0x7ffffe1,27},{0x3ffffe7,26},{0x7ffffe2,27},{0xfffff2,24},{0x1fffe4,21},{0x1fffe5,21},{0x3ffffe8,26},{0x3ffffe9,26},{0xffffffd,28},{0x7ffffe3,27},{0x7ffffe4,27},{0x7ffffe5,27},
 {0xfffec,20},{0xfffff3,24},{0xfffed,20},{0x1fffe6,21},{0x3fffe9,22},{0x1fffe7,21},{0x1fffe8,21},{0x7ffff3,23},{0x3fffea,22},{0x3fffeb,22},{0x1ffffee,25},{0x1ffffef,25},{0xfffff4,24},{0xfffff5,24},{0x3ffffea,26},{0x7ffff4,23},
 {0x3ffffeb,26},{0x7ffffe6,27},{0x3ffffec,26},{0x3ffffed,26},{0x7ffffe7,27},{0x7ffffe8,27},{0x7ffffe9,27},{0x7ffffea,27},{0x7ffffeb,27},{0xffffffe,28},{0x7ffffec,27},{0x7ffffed,27},{0x7ffffee,27},{0x7ffffef,27},{0x7fffff0,27},{0x3ffffee,26},{0x3fffffff,30}};
struct huff_node{uint16_t child[2];int16_t symbol;};
static struct huff_node huff_tree[514];static uint32_t huff_ready;
static void huff_init(void) {
    if(__atomic_load_n(&huff_ready,__ATOMIC_ACQUIRE)==2)return;
    uint32_t expected=0;
    if(__atomic_compare_exchange_n(&huff_ready,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        unsigned used=1;for(unsigned i=0;i<514;i++)huff_tree[i].symbol=-1;
        for(unsigned symbol=0;symbol<257;symbol++) {
            unsigned at=0;for(int bit=huffman[symbol].bits-1;bit>=0;bit--) {
                unsigned b=(huffman[symbol].code>>bit)&1;
                if(!huff_tree[at].child[b])huff_tree[at].child[b]=used++;
                at=huff_tree[at].child[b];
            }
            huff_tree[at].symbol=(int16_t)symbol;
        }
        __atomic_store_n(&huff_ready,2,__ATOMIC_RELEASE);
    }else while(__atomic_load_n(&huff_ready,__ATOMIC_ACQUIRE)!=2)yield();
}
static void table_evict(struct hpack_entry **table,size_t *size,size_t max,size_t needed) {
    while(*table&&*size+needed>max) {
        struct hpack_entry **p=table;while((*p)->next)p=&(*p)->next;
        struct hpack_entry *e=*p;*p=NULL;*size-=e->size;free(e->name);free(e->value);free(e);
    }
}
static void evict(struct http2_connection *c,size_t needed) {
    table_evict(&c->table,&c->table_size,c->table_max,needed);
}
static bool indexed(struct http2_connection *c,size_t index,const char **name,size_t *name_len,const char **value,size_t *value_len) {
    if(index&&index<62){*name=static_name[index];*value=index<17?static_value[index]:"";*name_len=strlen(*name);*value_len=strlen(*value);return true;}
    if(index<62)return false;index-=62;struct hpack_entry *e=c->table;
    while(e&&index--){e=e->next;}if(!e)return false;*name=e->name;*value=e->value;*name_len=e->name_len;*value_len=e->value_len;return true;
}
static bool table_insert(struct hpack_entry **table,size_t *table_size,size_t max,
    const char *name,size_t name_len,const char *value,size_t value_len) {
    size_t size=name_len+value_len+32;
    if(size>max){table_evict(table,table_size,max,max+1);return true;}
    struct hpack_entry *e=calloc(1,sizeof *e);if(!e)return false;
    e->name=malloc(name_len+1);e->value=malloc(value_len+1);e->name_len=name_len;e->value_len=value_len;e->size=size;
    if(!e->name||!e->value){free(e->name);free(e->value);free(e);return false;}
    memcpy(e->name,name,name_len);e->name[name_len]=0;memcpy(e->value,value,value_len);e->value[value_len]=0;
    table_evict(table,table_size,max,size);e->next=*table;*table=e;*table_size+=size;return true;
}
static bool table_add(struct http2_connection *c,const char *name,size_t name_len,const char *value,size_t value_len) {
    return table_insert(&c->table,&c->table_size,c->table_max,name,name_len,value,value_len);
}
static bool append(struct bytes *b,const void *data,size_t n) {
    if(n>H2_HEADERS-b->n)return false;
    if(b->n+n+1>b->cap){size_t cap=b->cap?b->cap:256;while(cap<b->n+n+1)cap*=2;
        unsigned char *p=realloc(b->p,cap);if(!p)return false;b->p=p;b->cap=cap;}
    memcpy(b->p+b->n,data,n);b->n+=n;b->p[b->n]=0;return true;
}
static bool number(struct bytes *b,size_t n,unsigned bits,unsigned prefix) {
    unsigned char p[16];size_t used=0,max=((size_t)1<<bits)-1;
    if(n<max){p[used++]=prefix|n;}else{p[used++]=prefix|max;n-=max;while(n>=128){p[used++]=(n&127)|128;n>>=7;}p[used++]=n;}
    return append(b,p,used);
}
/* RFC 7541 5.2: a string is Huffman coded whenever that is shorter. */
static bool string(struct bytes *b,const char *text,size_t n) {
    uint64_t bits=0;for(size_t i=0;i<n;i++)bits+=huffman[(unsigned char)text[i]].bits;
    size_t coded=(size_t)((bits+7)/8);
    if(coded>=n)return number(b,n,7,0)&&append(b,text,n);
    unsigned char *out=malloc(coded);if(!out)return false;
    size_t at=0;uint64_t pending=0;unsigned held=0;
    for(size_t i=0;i<n;i++) {
        const struct huff_code *k=&huffman[(unsigned char)text[i]];
        pending=pending<<k->bits|k->code;held+=k->bits;
        while(held>=8){held-=8;out[at++]=(unsigned char)(pending>>held);}
    }
    if(held)out[at++]=(unsigned char)(pending<<(8-held)|0xffu>>held); /* EOS-prefix padding */
    bool ok=number(b,at,7,128)&&append(b,out,at);free(out);return ok;
}
enum h2_field_mode { FIELD_INDEX,FIELD_PLAIN,FIELD_NEVER };
/* Exact (name,value) match first, else the lowest index with the name. */
static size_t encoder_find(struct http2_connection *c,const char *name,size_t name_len,
    const char *value,size_t value_len,size_t *name_index) {
    *name_index=0;
    for(size_t i=1;i<62;i++)if(!strcmp(name,static_name[i])) {
        if(!*name_index)*name_index=i;
        if(!strcmp(value,i<17?static_value[i]:""))return i;
    }
    size_t i=62;
    for(struct hpack_entry *e=c->encoder;e;e=e->next,i++)if(e->name_len==name_len&&!memcmp(e->name,name,name_len)) {
        if(!*name_index)*name_index=i;
        if(e->value_len==value_len&&!memcmp(e->value,value,value_len))return i;
    }
    return 0;
}
/* Caller holds c->lock and queues the finished block before releasing it. A
   failure after a table insertion leaves the peer's table unknowable. */
static bool field(struct http2_connection *c,struct bytes *b,const char *name,const char *value,enum h2_field_mode mode) {
    size_t name_len=strlen(name),value_len=strlen(value),name_index=0;
    size_t exact=encoder_find(c,name,name_len,value,value_len,&name_index);
    if(exact)return number(b,exact,7,128);
    bool add=mode==FIELD_INDEX&&name_len+value_len+32<=c->encoder_max;
    if(!number(b,name_index,add?6:4,add?64:mode==FIELD_NEVER?16:0))return false;
    if(!name_index&&!string(b,name,name_len))return false;
    if(!string(b,value,value_len))return false;
    return !add||table_insert(&c->encoder,&c->encoder_size,c->encoder_max,name,name_len,value,value_len);
}
static enum h2_field_mode field_mode(const char *name,const char *value) {
    if(!strcmp(name,"authorization")||!strcmp(name,"proxy-authorization"))return FIELD_NEVER;
    /* Short cookie crumbs are cheap to guess through compression (RFC 7541
       7.1.3); longer ones are indexed so repeat requests stay small. */
    if(!strcmp(name,"cookie"))return strlen(value)<20?FIELD_NEVER:FIELD_INDEX;
    /* Per-request values would only evict reusable entries. */
    if(!strcmp(name,":path")||!strcmp(name,"content-length"))return FIELD_PLAIN;
    return FIELD_INDEX;
}
static bool forbidden(const char *name) {
    return !strcmp(name,"connection")||!strcmp(name,"proxy-connection")||!strcmp(name,"keep-alive")||
        !strcmp(name,"transfer-encoding")||!strcmp(name,"upgrade");
}
static bool connection_named(const char *head,const char *name) {
    const char *p=strstr(head,"\r\n");if(!p)return false;p+=2;size_t length=strlen(name);
    while(*p&&strncmp(p,"\r\n",2)) {
        const char *end=strstr(p,"\r\n");if(!end)return true;
        if((size_t)(end-p)>11&&!strncasecmp(p,"Connection:",11)) {
            const char *v=p+11;
            while(v<end){while(v<end&&(*v==','||*v==' '||*v=='\t'))v++;
                const char *last=v;while(last<end&&*last!=',')last++;const char *trim=last;
                while(trim>v&&(trim[-1]==' '||trim[-1]=='\t'))trim--;
                if((size_t)(trim-v)==length&&!strncasecmp(v,name,length))return true;v=last;}
        }
        p=end+2;
    }return false;
}
/* Request fields are gathered before admission and encoded under the lock;
   bound is the plain-literal size, which compression never exceeds. */
static bool field_list(struct bytes *list,size_t *bound,const char *name,size_t name_len,const char *value,size_t value_len) {
    static const char zero=0;
    if(name_len+value_len+12>H2_HEADERS-*bound)return false;
    *bound+=name_len+value_len+12;
    return append(list,name,name_len)&&append(list,&zero,1)&&append(list,value,value_len)&&append(list,&zero,1);
}
enum { HP_REP,HP_INTEGER,HP_NAME_LENGTH,HP_NAME,HP_VALUE_LENGTH,HP_VALUE };
enum { HP_INDEX,HP_SIZE,HP_NAME_INDEX,HP_NAME_SIZE,HP_VALUE_SIZE };
static void bytes_free(struct bytes *b){free(b->p);memset(b,0,sizeof *b);}
static void header_discard(struct http2_connection *c,int error,uint32_t reset_code,const char *message) {
    struct hpack_decoder *h=&c->decoder;struct http2_stream *s=find_stream(c,h->stream);
    if(s&&!s->error){stream_fail(s,error,message);reset(c,s->id,reset_code);}
    h->discard=true;bytes_free(&h->fields);
}
static void header_refresh(struct http2_connection *c) {
    struct hpack_decoder *h=&c->decoder;struct http2_stream *s=find_stream(c,h->stream);
    /* RST and its response may cross. Released/failed streams still change
       the shared HPACK context, but have no response semantics or consumer. */
    if(!s||s->error){h->discard=true;bytes_free(&h->fields);}
}
static bool header_field(struct http2_connection *c,const char *name,size_t nl,const char *value,size_t vl) {
    struct hpack_decoder *h=&c->decoder;struct http2_stream *s=find_stream(c,h->stream);
    if(h->discard)return true;
    if(nl>H2_HEADERS-h->decoded||vl>H2_HEADERS-h->decoded-nl||32>H2_HEADERS-h->decoded-nl-vl) {
        header_discard(c,ENOBUFS,11,"HTTP/2 decoded header budget exhausted");return true;
    }
    h->decoded+=nl+vl+32;
    if(!nl||memchr(name,0,nl)||memchr(value,0,vl))goto invalid;
    for(size_t i=0;i<nl;i++) {
        unsigned ch=(unsigned char)name[i];
        if(!((ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||strchr("!#$%&'*+-.^_`|~",ch)||(ch==':'&&!i)))goto invalid;
    }
    for(size_t i=0;i<vl;i++){unsigned ch=(unsigned char)value[i];if(ch=='\r'||ch=='\n'||ch==127||(ch<32&&ch!='\t'))goto invalid;}
    if(vl&&(value[0]==' '||value[0]=='\t'||value[vl-1]==' '||value[vl-1]=='\t'))goto invalid;
    if(*name==':') {
        if(h->trailers||h->regular||h->have_status||strcmp(name,":status")||vl!=3||
            value[0]<'1'||value[0]>'5'||value[1]<'0'||value[1]>'9'||value[2]<'0'||value[2]>'9')goto invalid;
        h->have_status=true;h->status=(value[0]-'0')*100+(value[1]-'0')*10+value[2]-'0';
        if(h->status==101)goto invalid;
    }else {
        h->regular=true;
        if(forbidden(name)||!strcmp(name,"te")||(h->trailers&&(!strcmp(name,"content-length")||!strcmp(name,"host"))))goto invalid;
        if(!h->trailers&&(!append(&h->fields,name,nl)||!append(&h->fields,": ",2)||
            !append(&h->fields,value,vl)||!append(&h->fields,"\r\n",2))) {
            header_discard(c,ENOMEM,2,"HTTP/2 header allocation failed");return true;
        }
        if(!h->trailers&&!strcmp(name,"content-length")) {
            size_t length=0;if(!vl)goto invalid;
            for(size_t i=0;i<vl;i++){if(value[i]<'0'||value[i]>'9'||length>(SIZE_MAX-(unsigned)(value[i]-'0'))/10)goto invalid;length=length*10+value[i]-'0';}
            if(s->have_length&&s->content_length!=length)goto invalid;s->have_length=true;s->content_length=length;
        }
    }
    return true;
invalid:header_discard(c,EPROTO,1,"invalid HTTP/2 response header field");return true;
}
static bool header_literal(struct http2_connection *c) {
    struct hpack_decoder *h=&c->decoder;
    const char *name=h->name.p?(char*)h->name.p:"",*value=h->value.p?(char*)h->value.p:"";
    if(!header_field(c,name,h->name_len,value,h->value_len))return false;
    /* Oversized incremental entries evict the table even when their stream
       was refused. Small entries retain exact lengths (including NUL bytes),
       so a later index cannot silently change the compression context. */
    if(h->add) {
        if(h->name_len>c->table_max||h->value_len>c->table_max-h->name_len||32>c->table_max-h->name_len-h->value_len)evict(c,c->table_max+1);
        else if(!table_add(c,name,h->name_len,value,h->value_len))return false;
    }
    bytes_free(&h->name);bytes_free(&h->value);h->name_len=h->value_len=0;h->state=HP_REP;return true;
}
static bool header_emit(struct http2_connection *c,const unsigned char *p,size_t n) {
    struct hpack_decoder *h=&c->decoder;size_t *length=h->state==HP_NAME?&h->name_len:&h->value_len;
    struct bytes *out=h->state==HP_NAME?&h->name:&h->value;
    if(n>SIZE_MAX-*length)return false;*length+=n;
    if(!h->discard&&(h->name_len>H2_HEADERS-h->decoded||h->value_len>H2_HEADERS-h->decoded-h->name_len||
        32>H2_HEADERS-h->decoded-h->name_len-h->value_len))header_discard(c,ENOBUFS,11,"HTTP/2 decoded header budget exhausted");
    if(h->discard&&(!h->add||h->name_len>c->table_max||h->value_len>c->table_max-h->name_len||
        32>c->table_max-h->name_len-h->value_len)) {
        bytes_free(&h->name);bytes_free(&h->value);return true;
    }
    if(!append(out,p,n)) {
        header_discard(c,ENOMEM,2,"HTTP/2 literal allocation failed");
        /* The table still needs at most 4096 bytes. If that allocation also
           fails, the connection compression context cannot be recovered. */
        if(h->add&&h->name_len<=c->table_max&&h->value_len<=c->table_max-h->name_len&&32<=c->table_max-h->name_len-h->value_len)return false;
        bytes_free(&h->name);bytes_free(&h->value);
    }
    return true;
}
static bool header_string_end(struct http2_connection *c) {
    struct hpack_decoder *h=&c->decoder;
    if(h->huffman&&(h->tail_bits>7||(h->tail_bits&&h->tail!=((1u<<h->tail_bits)-1))))return false;
    if(h->state==HP_NAME){h->state=HP_VALUE_LENGTH;return true;}
    return header_literal(c);
}
static bool header_integer_end(struct http2_connection *c) {
    struct hpack_decoder *h=&c->decoder;const char *name,*value;size_t nl,vl;
    switch(h->action) {
    case HP_INDEX:
        if(!indexed(c,h->number,&name,&nl,&value,&vl))return false;
        h->state=HP_REP;return header_field(c,name,nl,value,vl);
    case HP_SIZE:
        if(h->any||h->number>4096)return false;c->table_max=h->number;evict(c,0);h->state=HP_REP;return true;
    case HP_NAME_INDEX:
        if(!h->number){h->state=HP_NAME_LENGTH;return true;}
        if(!indexed(c,h->number,&name,&nl,&value,&vl))return false;
        h->state=HP_NAME;if(!header_emit(c,(const unsigned char*)name,nl))return false;h->state=HP_VALUE_LENGTH;return true;
    case HP_NAME_SIZE:case HP_VALUE_SIZE:
        h->state=h->action==HP_NAME_SIZE?HP_NAME:HP_VALUE;h->remaining=h->number;h->node=h->tail=h->tail_bits=0;
        return h->remaining||header_string_end(c);
    }
    return false;
}
static bool header_prefix(struct http2_connection *c,unsigned first,unsigned bits,unsigned action) {
    struct hpack_decoder *h=&c->decoder;size_t mask=((size_t)1<<bits)-1;
    h->action=action;h->number=first&mask;h->shift=0;
    if(h->number==mask){h->state=HP_INTEGER;return true;}return header_integer_end(c);
}
static void header_begin(struct http2_connection *c,struct http2_stream *s,uint32_t id) {
    struct hpack_decoder *h=&c->decoder;bytes_free(&h->name);bytes_free(&h->value);bytes_free(&h->fields);memset(h,0,sizeof *h);
    h->stream=id;h->trailers=s&&s->headers_ready;h->discard=!s||s->error;
}
static bool decode_headers(struct http2_connection *c,const unsigned char *p,size_t n) {
    struct hpack_decoder *h=&c->decoder;header_refresh(c);
    while(n) {
        unsigned first=*p++;n--;
        if(h->state==HP_REP) {
            h->add=(first&0xc0)==0x40;
            if(first&0x80){h->any=true;if(!header_prefix(c,first,7,HP_INDEX))return false;}
            else if((first&0xe0)==0x20){if(!header_prefix(c,first,5,HP_SIZE))return false;}
            else {h->any=true;if(!header_prefix(c,first,h->add?6:4,HP_NAME_INDEX))return false;}
        }else if(h->state==HP_INTEGER) {
            size_t part=first&127u;
            if(h->shift>=sizeof(size_t)*CHAR_BIT||part>(SIZE_MAX-h->number)>>h->shift)return false;
            h->number+=part<<h->shift;
            if(first&128)h->shift+=7;else if(!header_integer_end(c))return false;
        }else if(h->state==HP_NAME_LENGTH||h->state==HP_VALUE_LENGTH) {
            unsigned action=h->state==HP_NAME_LENGTH?HP_NAME_SIZE:HP_VALUE_SIZE;h->huffman=first&128;
            if(!header_prefix(c,first,7,action))return false;
        }else {
            if(h->huffman) {
                for(int bit=7;bit>=0;bit--) {
                    unsigned b=(first>>bit)&1;h->tail=(h->tail<<1)|b;h->tail_bits++;h->node=huff_tree[h->node].child[b];
                    if(!h->node)return false;int symbol=huff_tree[h->node].symbol;
                    if(symbol>=0){if(symbol==256)return false;unsigned char ch=(unsigned char)symbol;
                        if(!header_emit(c,&ch,1))return false;h->node=h->tail=h->tail_bits=0;}
                }
                h->remaining--;
            }else {
                size_t count=h->remaining;if(count>n+1)count=n+1;
                if(!header_emit(c,p-1,count))return false;h->remaining-=count;p+=count-1;n-=count-1;
            }
            if(!h->remaining&&!header_string_end(c))return false;
        }
    }
    return true;
}
static bool header_end(struct http2_connection *c,bool end_stream) {
    struct hpack_decoder *h=&c->decoder;struct http2_stream *s=find_stream(c,h->stream);header_refresh(c);
    if(h->state!=HP_REP)return false;
    if(!h->discard&&((!h->trailers&&!h->have_status)||(h->trailers&&!end_stream)))
        header_discard(c,EPROTO,1,"invalid HTTP/2 response header block");
    if(!h->discard) {
        if(!h->trailers&&h->status<200) {
            s->have_length=false;s->content_length=0;if(end_stream)header_discard(c,EPROTO,1,"informational HTTP/2 response ended stream");
        }else if(!h->trailers) {
            struct bytes head={0};char line[32];int length=snprintf(line,sizeof line,"HTTP/1.1 %d \r\n",h->status);
            if(!append(&head,line,(size_t)length)||!append(&head,h->fields.p,h->fields.n)||!append(&head,"\r\n",2)) {
                bytes_free(&head);header_discard(c,ENOMEM,2,"HTTP/2 header allocation failed");
            }else {s->headers=(char*)head.p;s->headers_len=head.n;s->headers_ready=true;s->body_forbidden=s->head||h->status==204||h->status==304;}
        }
        if(!h->discard&&end_stream){s->ended=true;if(s->have_length&&!s->body_forbidden&&s->received!=s->content_length)stream_fail(s,EPROTO,"HTTP/2 Content-Length mismatch");}
    }
    bytes_free(&h->fields);return true;
}
static void dispatch(struct http2_connection *c) {
    unsigned type=c->frame_header[3],flags=c->frame_header[4];uint32_t id=be32(c->frame_header+5)&0x7fffffff;
    unsigned char *p=c->frame;size_t n=c->frame_len;struct http2_stream *s=find_stream(c,id);
    if(c->continuation&&(type!=9||id!=c->continuation))goto protocol;
    if(!c->settings&&type!=4)goto protocol;
    switch(type) {
    case 0: { /* DATA: connection credit is returned on queueing, stream credit on consumption. */
        if(!(id&1)||id>=c->next_id)goto protocol;size_t pad=0,start=0;
        if(flags&8){if(!n)goto protocol;pad=p[0];start=1;if(pad>=n)goto protocol;}
        size_t data=n-start-pad;
        if(n>c->recv_window)goto protocol;c->recv_window-=(uint32_t)n;
        /* A released stream was either ended by the peer or already reset by
           us; frames still in flight are ignored, never answered with another
           RST_STREAM (RFC 9113 5.1, 6.4). */
        if(!s||s->error){connection_credit(c,(uint32_t)n);break;}
        if(!s->headers_ready||s->ended||s->recv_window<n||(data&&s->body_forbidden))goto protocol;
        s->recv_window-=(uint32_t)n;
        if(data&&!s->error) {
            struct h2_data *d=malloc(sizeof *d+data);if(!d){stream_fail(s,ENOMEM,"HTTP/2 receive allocation failed");reset(c,id,8);}
            else {d->next=NULL;d->at=0;d->len=data;memcpy(d->bytes,p+start,data);
                if(s->last)s->last->next=d;else s->first=d;s->last=d;}
        }
        s->received+=data;s->progress=uptime_ms();
        if(s->result)s->result->response_bytes_received+=data;
        connection_credit(c,(uint32_t)n);
        if(start+pad)stream_credit(s,(uint32_t)(start+pad));
        if(flags&1){s->ended=true;if(s->have_length&&!s->body_forbidden&&s->received!=s->content_length)stream_fail(s,EPROTO,"HTTP/2 Content-Length mismatch");}
        break;
    }
    case 1:case 9: {
        if(!id)goto protocol;
        if(type==9&&!c->continuation)goto protocol;
        size_t start=0,pad=0;
        if(type==1) {
            if(!(id&1)||id>=c->next_id||(s&&s->ended&&!s->error))goto protocol;
            header_begin(c,s,id);c->header_end=flags&1;
            if(flags&8){if(!n)goto protocol;pad=p[start++];}
            if(flags&32){if(n-start<5)goto protocol;if((be32(p+start)&0x7fffffff)==id)goto protocol;start+=5;}
            if(pad>n-start)goto protocol;
        }
        if(!decode_headers(c,p+start,n-start-pad))goto compression;
        if(s){s->progress=uptime_ms();if(s->result)s->result->response_bytes_received+=n-start-pad;}
        if(flags&4) {
            c->continuation=0;if(!header_end(c,c->header_end))goto compression;
        }else c->continuation=id;
        break;
    }
    case 2:if(!id||n!=5)goto protocol;break;
    case 3:if(!id||n!=4||id>=c->next_id)goto protocol;
        if(s){if(be32(p)==7&&s->result)s->result->request_unprocessed=true;
            stream_fail(s,be32(p)==7?EAGAIN:ECONNRESET,"HTTP/2 peer reset stream");}break;
    case 4:
        if(id||((flags&1)&&n)||n%6)goto protocol;
        if(flags&1)break;c->settings=true;
        for(size_t at=0;at<n;at+=6) {
            unsigned setting=(unsigned)p[at]<<8|p[at+1];uint32_t value=be32(p+at+2);
            if(setting==1) {
                /* Our table never exceeds the default; a smaller peer limit is
                   signalled at the start of the next header block (RFC 7541 4.2). */
                size_t max=value<4096?value:4096;
                if(max!=c->encoder_max) {
                    if(!c->encoder_resize||max<c->encoder_low)c->encoder_low=max;
                    c->encoder_max=max;c->encoder_resize=true;
                    table_evict(&c->encoder,&c->encoder_size,max,0);
                }
            }
            else if(setting==2){if(value>1)goto protocol;}
            else if(setting==3)c->max_streams=value;
            else if(setting==4){if(value>INT_MAX)goto protocol;int64_t delta=(int64_t)value-c->initial_window;
                for(struct http2_stream *q=c->streams;q;q=q->next){q->send_window+=delta;if(q->send_window>INT_MAX)goto protocol;}c->initial_window=value;}
            else if(setting==5){if(value<16384||value>16777215)goto protocol;c->max_frame=value;}
        }
        frame(c,4,1,0,NULL,0);break;
    case 5:goto protocol; /* PUSH disabled in the initial SETTINGS. */
    case 6:if(id||n!=8)goto protocol;if(!(flags&1))frame(c,6,1,0,p,n);break;
    case 7:
        if(id||n<8)goto protocol;c->goaway=true;
        for(struct http2_stream *q=c->streams;q;q=q->next)
            if(q->id>(be32(p)&0x7fffffff)){if(q->result)q->result->request_unprocessed=true;stream_fail(q,EAGAIN,"HTTP/2 GOAWAY unprocessed stream");}
        break;
    case 8: {
        if(n!=4)goto protocol;uint32_t delta=be32(p)&0x7fffffff;if(!delta)goto protocol;
        if(!id){if(c->send_window+delta>INT_MAX)goto protocol;c->send_window+=delta;}
        else if(s){if(s->send_window+delta>INT_MAX)goto protocol;s->send_window+=delta;}
        else if(id>=c->next_id)goto protocol;
        break;
    }
    default:break;
    }
    return;
compression:fail(c,EPROTO,"invalid HTTP/2 HPACK block");return;
protocol:fail(c,EPROTO,"invalid HTTP/2 frame or stream state");
}
/* Caller holds c->lock. Read at most 256 KiB / 32 frames per turn. Outgoing
   request bodies get one frame each per turn and respect both peer windows. */
static void pump(struct http2_connection *c) {
    if(c->dead)return;
    drain(c);
    /* Rotate the first eligible stream on every turn. A nearly full queue
       must not repeatedly give its last few bytes to the newest request. */
    struct http2_stream *first=c->streams;
    for(struct http2_stream *s=c->streams;s;s=s->next)if(s->id==c->send_cursor){first=s->next?s->next:c->streams;break;}
    struct http2_stream *s=first;
    while(s) {
        struct http2_stream *next=s->next?s->next:c->streams;
        if(!s->error&&!s->local_end&&!c->header_waiters&&
           c->output_bytes<H2_APPLICATION_OUTPUT&&H2_APPLICATION_OUTPUT-c->output_bytes>9) {
        size_t n=s->request->body_len-s->sent;
        if(n>16384)n=16384;
        size_t available=H2_APPLICATION_OUTPUT-c->output_bytes-9;
        if(n>available)n=available;
        if(c->send_window<=0||s->send_window<=0)goto next_stream;
        if(n>(size_t)c->send_window)n=(size_t)c->send_window;
        if(n>(size_t)s->send_window)n=(size_t)s->send_window;
        if(!n)goto next_stream;bool last=s->sent+n==s->request->body_len;
        bool ok=frame(c,0,last?1:0,s->id,(const char*)s->request->body+s->sent,n);
        if(!ok)return;
        s->sent+=n;s->local_end=last;s->send_window-=n;c->send_window-=n;
        c->send_cursor=s->id;
        }
next_stream:
        if(next==first)break;s=next;
    }
    drain(c);
    size_t bytes=0;unsigned frames=0;
    while(!c->dead&&bytes<262144&&frames<32) {
        /* A blocked peer can also flood control frames. Retain half of the
           control reserve for reader credits/cancellation, and backpressure
           TCP input until pending output makes progress. */
        if(c->output_bytes>H2_OUTPUT_MAX-H2_CONTROL_RESERVE/2){drain(c);return;}
        if(c->frame_header_at<9) {
            long n=ns_try_read(c->wire,c->frame_header+c->frame_header_at,9-c->frame_header_at);
            if(n==-2){drain(c);return;}if(n<=0){fail(c,n==0?ECONNRESET:errno,n==0?"HTTP/2 connection closed":ns_error(c->wire));return;}
            c->frame_header_at+=(size_t)n;bytes+=(size_t)n;if(c->frame_header_at<9)continue;
            c->frame_len=(size_t)c->frame_header[0]<<16|(size_t)c->frame_header[1]<<8|c->frame_header[2];
            if(c->frame_len>16384){fail(c,EPROTO,"HTTP/2 frame exceeds advertised maximum");return;}
            /* Zero-length frames (empty HEADERS/DATA) still get a base pointer: dispatch does pointer arithmetic on it. */
            if(!c->frame){c->frame=malloc(16384);if(!c->frame){fail(c,ENOMEM,"HTTP/2 frame allocation failed");return;}}
        }
        if(c->frame_at<c->frame_len) {
            long n=ns_try_read(c->wire,c->frame+c->frame_at,c->frame_len-c->frame_at);
            if(n==-2){drain(c);return;}if(n<=0){fail(c,n==0?ECONNRESET:errno,"HTTP/2 frame cut short");return;}
            c->frame_at+=(size_t)n;bytes+=(size_t)n;if(c->frame_at<c->frame_len)continue;
        }
        dispatch(c);frames++;c->frame_header_at=c->frame_at=c->frame_len=0;
    }
    drain(c);
}
struct http2_connection *http2_new(net_stream *wire) {
    struct http2_connection *c=calloc(1,sizeof *c);if(!c)return NULL;
    c->wire=wire;c->next_id=1;c->max_streams=UINT32_MAX;c->max_frame=16384;
    c->initial_window=65535;c->send_window=65535;c->recv_window=65535;c->table_max=4096;c->encoder_max=4096;huff_init();
    static const char preface[]="PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    /* ENABLE_PUSH=0, INITIAL_WINDOW_SIZE, MAX_HEADER_LIST_SIZE. */
    unsigned char settings[18]={0,2,0,0,0,0,0,4};put32(settings+8,H2_WINDOW);
    settings[12]=0;settings[13]=6;put32(settings+14,H2_HEADERS);
    struct h2_output *o=malloc(sizeof *o+sizeof preface-1);
    if(!o){http2_free(c);return NULL;}
    o->next=NULL;o->at=0;o->len=sizeof preface-1;o->stream=0;o->type=255;o->flags=0;memcpy(o->bytes,preface,o->len);
    c->output=c->output_last=o;c->output_bytes=o->len;
    if(!frame(c,4,0,0,settings,sizeof settings)) {
        http2_free(c);return NULL;
    }
    /* The connection window starts at 65535 regardless of SETTINGS. */
    c->recv_window=H2_CONNECTION_WINDOW;update(c,0,H2_CONNECTION_WINDOW-65535u);
    if(c->dead){http2_free(c);return NULL;}
    return c;
}
void http2_free(struct http2_connection *c) {
    if(!c)return;c->table_max=0;evict(c,0);table_evict(&c->encoder,&c->encoder_size,0,0);bytes_free(&c->decoder.name);bytes_free(&c->decoder.value);bytes_free(&c->decoder.fields);free(c->frame);
    struct h2_output *o=c->output;while(o){struct h2_output *next=o->next;free(o);o=next;}free(c);
}
bool http2_available(struct http2_connection *c) {
    enter(c);bool ok=!c->dead&&!c->goaway&&c->next_id<0x7fffffffu&&c->active<H2_STREAMS&&c->active<c->max_streams;leave(c);return ok;
}
bool http2_reusable(struct http2_connection *c) {
    enter(c);bool ok=!c->dead&&!c->goaway&&c->next_id<0x7fffffffu;leave(c);return ok;
}
static void start_remove(struct http2_connection *c,struct h2_starter *waiter) {
    struct h2_starter **at=&c->start_first,*previous=NULL;
    while(*at&&*at!=waiter){previous=*at;at=&(*at)->next;}
    if(*at)*at=waiter->next;if(c->start_last==waiter)c->start_last=previous;
}
struct http2_stream *http2_start(struct http2_connection *c,const struct http_req *rq,
    const struct url_owned *url,const char *head,const struct http_stream_hooks *hooks,
    struct http_transport_result *result,char *error,size_t capacity) {
    struct http2_stream *s=calloc(1,sizeof *s);struct bytes b={0},list={0};char *authority=NULL;
    size_t bound=16; /* worst-case block size: plain literals plus a table-size update */
    size_t h=strlen(url->host);if(h>SIZE_MAX-8||!(authority=malloc(h+8)))goto invalid;
    if(url->port==(url->tls?443:80))snprintf(authority,h+8,"%s",url->host);
    else snprintf(authority,h+8,"%s:%u",url->host,url->port);
    const char *method=rq->method?rq->method:"GET";
    if(!s||!field_list(&list,&bound,":method",3+4,method,strlen(method))||
        !field_list(&list,&bound,":scheme",7,url->tls?"https":"http",url->tls?5:4)||
        !field_list(&list,&bound,":authority",10,authority,strlen(authority))||
        !field_list(&list,&bound,":path",5,url->path,strlen(url->path)))goto invalid;
    const char *p=strstr(head,"\r\n");if(!p)goto invalid;p+=2;
    while(*p&&strncmp(p,"\r\n",2)) {
        const char *end=strstr(p,"\r\n"),*colon=strchr(p,':');if(!end||!colon||colon>end)goto invalid;
        size_t n=(size_t)(colon-p);char *name=malloc(n+1);if(!name)goto invalid;
        for(size_t i=0;i<n;i++)name[i]=p[i]>='A'&&p[i]<='Z'?p[i]+32:p[i];name[n]=0;
        const char *v=colon+1;while(v<end&&(*v==' '||*v=='\t'))v++;
        const char *last=end;while(last>v&&(last[-1]==' '||last[-1]=='\t'))last--;
        bool ok=true;
        /* RFC 9113 8.2.2: connection-specific fields are not sent; TE only as
           "trailers". 8.2.3: cookies travel as separate crumbs. */
        bool te=!strcmp(name,"te"),trailers=te&&(size_t)(last-v)==8&&!strncasecmp(v,"trailers",8);
        if(forbidden(name)||!strcmp(name,"host")||connection_named(head,name)||(te&&!trailers));
        else if(!strcmp(name,"cookie"))for(const char *crumb=v;ok&&crumb<last;) {
            const char *stop=crumb;while(stop<last&&*stop!=';')stop++;
            const char *tail=stop;while(tail>crumb&&(tail[-1]==' '||tail[-1]=='\t'))tail--;
            if(tail>crumb)ok=field_list(&list,&bound,name,n,crumb,(size_t)(tail-crumb));
            crumb=stop<last?stop+1:last;while(crumb<last&&(*crumb==' '||*crumb=='\t'))crumb++;
        }
        else ok=field_list(&list,&bound,name,n,v,(size_t)(last-v));
        free(name);if(!ok)goto invalid;p=end+2;
    }
    s->connection=c;s->request=rq;s->hooks=hooks;s->result=result;s->head=!strcmp(method,"HEAD");
    s->recv_window=H2_WINDOW;s->timeout=rq->timeout_ms>0?(uint64_t)rq->timeout_ms:INT_MAX;s->progress=uptime_ms();
    enter(c);
    /* Checkout is only a capacity hint: another caller or SETTINGS can change
       it before start. Queue admission fairly instead of failing that request.
       A waiter never owns the mutex while sleeping or running cancellation. */
    struct h2_starter waiter={0};
    if(c->start_last)c->start_last->next=&waiter;else c->start_first=&waiter;c->start_last=&waiter;
    bool waiting=false;
    while(!c->dead&&!c->goaway&&c->next_id<0x7fffffffu) {
        bool turn=c->start_first==&waiter,capacity=c->active<c->max_streams&&c->active<H2_STREAMS;
        bool space=bound<=H2_APPLICATION_OUTPUT&&c->output_bytes+bound+(bound/16384+1)*9<=H2_APPLICATION_OUTPUT;
        bool needs_space=turn&&capacity&&!space;
        if(needs_space&&!waiting){c->header_waiters++;waiting=true;}
        if(!needs_space&&waiting){c->header_waiters--;waiting=false;}
        if(turn&&capacity&&space)break;
        pump(c);leave(c);
        bool cancelled=hooks&&hooks->cancelled&&hooks->cancelled(rq->ctx);
        if(cancelled||uptime_ms()-s->progress>=s->timeout) {
            enter(c);if(waiting)c->header_waiters--;start_remove(c,&waiter);leave(c);errno=cancelled?ECANCELED:ETIMEDOUT;goto invalid;
        }
        msleep(1);enter(c);
    }
    if(waiting)c->header_waiters--;
    start_remove(c,&waiter);
    if(c->dead||c->goaway||c->next_id>=0x7fffffffu){
        if(result)result->request_unprocessed=true;int error=c->dead?ECONNRESET:EAGAIN;leave(c);errno=error;goto invalid;
    }
    /* Encode in wire order: the dynamic table must match the peer's decoder. */
    bool encoded=true;
    if(c->encoder_resize) {
        encoded=(c->encoder_low>=c->encoder_max||number(&b,c->encoder_low,5,32))&&number(&b,c->encoder_max,5,32);
        c->encoder_resize=false;
    }
    for(size_t at=0;encoded&&at<list.n;) {
        const char *name=(const char *)list.p+at,*value=name+strlen(name)+1;
        encoded=field(c,&b,name,value,field_mode(name,value));
        at=(size_t)(value-(const char *)list.p)+strlen(value)+1;
    }
    if(!encoded){fail(c,ENOMEM,"HTTP/2 header compression failed");leave(c);errno=ENOMEM;goto invalid;}
    s->id=c->next_id;c->next_id+=2;s->send_window=c->initial_window;
    if(result)result->stream_id=s->id;
    s->local_end=!rq->body_len;s->next=c->streams;c->streams=s;c->active++;
    for(size_t at=0;at<b.n;) {
        size_t n=b.n-at;if(n>16384)n=16384;
        unsigned flags=(at+n==b.n?4:0)|(!at&&s->local_end?1:0);
        bool ok=frame(c,at?9:1,flags,s->id,b.p+at,n);
        if(!ok)break;at+=n;
    }
    pump(c);leave(c);free(b.p);free(list.p);free(authority);return s;
invalid:free(s);free(b.p);free(list.p);free(authority);snprintf(error,capacity,"cannot open HTTP/2 stream");if(!errno)errno=ENOMEM;return NULL;
}
static bool check(struct http2_stream *s,bool cancelled) {
    if(cancelled) {
        cancel_output(s->connection,s,true);stream_fail(s,ECANCELED,"request cancelled");
    }
    if(!s->error&&uptime_ms()-s->progress>=s->timeout){cancel_output(s->connection,s,true);stream_fail(s,ETIMEDOUT,"HTTP/2 stream timed out");}
    return !s->error;
}
long http2_read(struct http2_stream *s,void *out,size_t n) {
    struct http2_connection *c=s->connection;
    for(;;) {
        bool cancelled=s->hooks&&s->hooks->cancelled&&s->hooks->cancelled(s->request->ctx);
        enter(c);check(s,cancelled);pump(c);size_t sent=s->sent;leave(c);
        if(s->hooks&&s->hooks->upload&&s->reported!=sent) {
            s->reported=sent;if(s->hooks->upload(s->request->ctx,sent,s->request->body_len)<0) {
                enter(c);cancel_output(c,s,true);stream_fail(s,ECANCELED,"upload cancelled");leave(c);
            }
        }
        enter(c);
        if(s->error){int error=s->error;leave(c);errno=error;return -1;}
        if(s->headers_ready&&s->headers_at<s->headers_len) {
            size_t count=s->headers_len-s->headers_at;if(count>n)count=n;
            memcpy(out,s->headers+s->headers_at,count);s->headers_at+=count;leave(c);return (long)count;
        }
        if(s->first) {
            struct h2_data *d=s->first;size_t count=d->len-d->at;if(count>n)count=n;
            memcpy(out,d->bytes+d->at,count);d->at+=count;
            if(d->at==d->len){s->first=d->next;if(!s->first)s->last=NULL;free(d);}
            stream_credit(s,(uint32_t)count);leave(c);return (long)count;
        }
        if(s->ended){leave(c);return 0;}
        leave(c);
        msleep(1);
    }
}
int http2_finish(struct http2_stream *s,bool consumed,const struct http_stream_hooks *hooks) {
    struct http2_connection *c=s->connection;s->hooks=hooks;
    if(!consumed){enter(c);cancel_output(c,s,!s->ended);leave(c);return 0;}
    for(;;) {
        bool cancelled=hooks&&hooks->cancelled&&hooks->cancelled(s->request->ctx);
        enter(c);check(s,cancelled);pump(c);
        if(s->first){reset(c,s->id,1);stream_fail(s,EPROTO,"unexpected HTTP/2 response body");}
        bool done=s->ended||s->error;int error=s->error;leave(c);
        if(done){if(error){errno=error;return -1;}return 0;}msleep(1);
    }
}
const char *http2_error(struct http2_stream *s){return s->message;}
void http2_release(struct http2_stream *s) {
    if(!s)return;struct http2_connection *c=s->connection;enter(c);
    cancel_output(c,s,!s->ended&&!s->error);
    struct http2_stream **p=&c->streams;while(*p&&*p!=s)p=&(*p)->next;
    if(*p){*p=s->next;c->active--;}leave(c);
    struct h2_data *d=s->first;while(d){struct h2_data *next=d->next;free(d);d=next;}
    free(s->headers);free(s);
}
