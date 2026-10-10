/* Resumable RFC 1951/1952 decoder for HTTP callbacks. Input is never retained:
 * only the bit reservoir, Huffman tables and the 32 KiB distance window survive
 * a read. Output goes through the HTTP sink (and its cancellation/backpressure).
 */
enum { G_HEADER, G_EXTRA_LEN, G_EXTRA, G_NAME, G_COMMENT, G_HCRC,
       G_BLOCK, G_STORED_LEN, G_STORED, G_DYNAMIC, G_CODE_LENGTHS,
       G_LENGTHS, G_REPEAT, G_SYMBOL, G_LENGTH_EXTRA, G_DISTANCE,
       G_DISTANCE_EXTRA, G_TRAILER_CRC, G_TRAILER_SIZE };
struct gzip_huffman { uint16_t count[16], symbol[288]; };
struct http_gzip {
    uint64_t bits; unsigned nbits,phase,header_pos,flags,extra,index;
    unsigned hlit,hdist,hclen,repeat,repeat_bits,repeat_value,length,distance;
    bool final,invalid; uint32_t crc,header_crc,size,members,crc_table[256];
    uint8_t lengths[320],code_lengths[19],window[32768],output[16384];
    size_t window_at,window_used,output_used;
    struct gzip_huffman lit,dist,code;
    const unsigned char *input; size_t remaining;
};
static uint32_t gzip_update(struct http_gzip *g,uint32_t crc,unsigned byte){
    return (crc>>8)^g->crc_table[(crc^byte)&255];
}
static bool gzip_bits(struct http_gzip *g,unsigned n){
    while(g->nbits<n){
        if(!g->remaining)return false;
        g->bits|=(uint64_t)*g->input++<<g->nbits;g->remaining--;g->nbits+=8;
    }
    return true;
}
static unsigned gzip_take(struct http_gzip *g,unsigned n){
    unsigned value=(unsigned)(g->bits&(((uint64_t)1<<n)-1));
    g->bits>>=n;g->nbits-=n;return value;
}
static bool gzip_tree(struct gzip_huffman *h,const uint8_t *lengths,unsigned n,bool complete){
    memset(h,0,sizeof *h);unsigned offsets[16]={0},used=0,max=0;int left=1;
    for(unsigned i=0;i<n;i++){if(lengths[i]>15)return false;h->count[lengths[i]]++;if(lengths[i]){used++;if(lengths[i]>max)max=lengths[i];}}
    for(unsigned i=1;i<=15;i++){left=left*2-h->count[i];if(left<0)return false;}
    if(left&&(complete||max>1))return false;
    for(unsigned i=1;i<15;i++)offsets[i+1]=offsets[i]+h->count[i];
    for(unsigned i=0;i<n;i++)if(lengths[i])h->symbol[offsets[lengths[i]]++]=(uint16_t)i;
    return used||!complete;
}
/* 0 needs another byte, -1 is invalid, 1 decoded a symbol. */
static int gzip_symbol(struct http_gzip *g,const struct gzip_huffman *h,unsigned *value){
    unsigned code=0,first=0,index=0;
    for(unsigned len=1;len<=15;len++){
        if(!gzip_bits(g,len))return 0;
        code=(code<<1)|((unsigned)(g->bits>>(len-1))&1);
        unsigned count=h->count[len];
        if(code>=first&&code-first<count){*value=h->symbol[index+code-first];gzip_take(g,len);return 1;}
        index+=count;first=(first+count)<<1;
    }
    return -1;
}
static bool gzip_flush(struct http_gzip *g,struct sink *sink){
    if(!g->output_used)return true;
    bool ok=sink_plain_put(sink,(const char *)g->output,g->output_used);g->output_used=0;return ok;
}
static bool gzip_emit(struct http_gzip *g,struct sink *sink,unsigned byte){
    g->window[g->window_at++&32767]=(uint8_t)byte;if(g->window_used<32768)g->window_used++;
    g->output[g->output_used++]=(uint8_t)byte;g->crc=gzip_update(g,g->crc,byte);g->size++;
    return g->output_used<sizeof g->output||gzip_flush(g,sink);
}
static void gzip_after_extra(struct http_gzip *g){
    if(g->flags&8){g->flags&=~8u;g->phase=G_NAME;}
    else if(g->flags&16){g->flags&=~16u;g->phase=G_COMMENT;}
    else if(g->flags&2){g->flags&=~2u;g->phase=G_HCRC;}
    else {g->phase=G_BLOCK;g->crc=0xffffffffu;g->size=0;g->window_at=g->window_used=0;}
}
static struct http_gzip *gzip_create(void){
    struct http_gzip *g=calloc(1,sizeof *g);if(!g)return NULL;
    for(unsigned i=0;i<256;i++){uint32_t c=i;for(unsigned j=0;j<8;j++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));g->crc_table[i]=c;}
    g->header_crc=0xffffffffu;return g;
}
static bool gzip_feed(struct http_gzip *g,struct sink *sink,const char *input,size_t n,bool finish){
    static const uint8_t code_order[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    static const uint16_t length_base[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t length_extra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distance_base[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    g->input=(const unsigned char *)input;g->remaining=n;
    for(;;){
        unsigned v=0;int decoded;
        switch(g->phase){
        case G_HEADER:
            if(!gzip_bits(g,8))goto wait;
            v=gzip_take(g,8);g->header_crc=gzip_update(g,g->header_crc,v);
            if((g->header_pos==0&&v!=31)||(g->header_pos==1&&v!=139)||(g->header_pos==2&&v!=8)||(g->header_pos==3&&(v&224)))goto invalid;
            if(g->header_pos==3)g->flags=v;
            if(++g->header_pos==10){if(g->flags&4){g->flags&=~4u;g->phase=G_EXTRA_LEN;}else gzip_after_extra(g);}
            break;
        case G_EXTRA_LEN:
            if(!gzip_bits(g,16))goto wait;
            v=gzip_take(g,16);g->header_crc=gzip_update(g,gzip_update(g,g->header_crc,v&255),v>>8);g->extra=v;g->phase=G_EXTRA;break;
        case G_EXTRA:
            if(!g->extra){gzip_after_extra(g);break;}
            if(!gzip_bits(g,8))goto wait;
            g->header_crc=gzip_update(g,g->header_crc,gzip_take(g,8));g->extra--;break;
        case G_NAME:case G_COMMENT:
            if(!gzip_bits(g,8))goto wait;
            v=gzip_take(g,8);g->header_crc=gzip_update(g,g->header_crc,v);if(!v)gzip_after_extra(g);break;
        case G_HCRC:
            if(!gzip_bits(g,16))goto wait;
            if(gzip_take(g,16)!=((~g->header_crc)&65535))goto invalid;
            gzip_after_extra(g);break;
        case G_BLOCK:
            if(!gzip_bits(g,3))goto wait;
            v=gzip_take(g,3);g->final=(v&1)!=0;v>>=1;
            if(v==0){gzip_take(g,g->nbits%8);g->phase=G_STORED_LEN;}
            else if(v==1){
                for(unsigned i=0;i<288;i++)g->lengths[i]=i<144?8:i<256?9:i<280?7:8;
                if(!gzip_tree(&g->lit,g->lengths,288,true))goto invalid;
                memset(g->lengths,5,32);gzip_tree(&g->dist,g->lengths,32,true);g->phase=G_SYMBOL;
            }else if(v==2)g->phase=G_DYNAMIC;else goto invalid;
            break;
        case G_STORED_LEN:
            if(!gzip_bits(g,32))goto wait;
            v=gzip_take(g,32);if((v&65535)!=((~v>>16)&65535))goto invalid;
            g->length=v&65535;g->phase=G_STORED;break;
        case G_STORED:
            if(!g->length){g->phase=g->final?G_TRAILER_CRC:G_BLOCK;break;}
            if(!gzip_bits(g,8))goto wait;
            if(!gzip_emit(g,sink,gzip_take(g,8)))return false;g->length--;break;
        case G_DYNAMIC:
            if(!gzip_bits(g,14))goto wait;
            v=gzip_take(g,14);g->hlit=(v&31)+257;g->hdist=((v>>5)&31)+1;g->hclen=(v>>10)+4;
            if(g->hlit>286)goto invalid;
            memset(g->code_lengths,0,sizeof g->code_lengths);g->index=0;g->phase=G_CODE_LENGTHS;break;
        case G_CODE_LENGTHS:
            if(g->index==g->hclen){if(!gzip_tree(&g->code,g->code_lengths,19,true))goto invalid;g->index=0;g->phase=G_LENGTHS;break;}
            if(!gzip_bits(g,3))goto wait;
            g->code_lengths[code_order[g->index++]]=(uint8_t)gzip_take(g,3);break;
        case G_LENGTHS:
            if(g->index==g->hlit+g->hdist){
                if(!g->lengths[256]||!gzip_tree(&g->lit,g->lengths,g->hlit,false)||!gzip_tree(&g->dist,g->lengths+g->hlit,g->hdist,false))goto invalid;
                g->phase=G_SYMBOL;break;
            }
            decoded=gzip_symbol(g,&g->code,&v);if(!decoded)goto wait;if(decoded<0)goto invalid;
            if(v<16)g->lengths[g->index++]=(uint8_t)v;
            else {if(v==16&&!g->index)goto invalid;g->repeat=v==16?3:v==17?3:11;g->repeat_bits=v==16?2:v==17?3:7;g->repeat_value=v==16?g->lengths[g->index-1]:0;g->phase=G_REPEAT;}
            break;
        case G_REPEAT:
            if(!gzip_bits(g,g->repeat_bits))goto wait;
            v=g->repeat+gzip_take(g,g->repeat_bits);if(v>g->hlit+g->hdist-g->index)goto invalid;
            memset(g->lengths+g->index,g->repeat_value,v);g->index+=v;g->phase=G_LENGTHS;break;
        case G_SYMBOL:
            decoded=gzip_symbol(g,&g->lit,&v);if(!decoded)goto wait;if(decoded<0)goto invalid;
            if(v<256){if(!gzip_emit(g,sink,v))return false;}
            else if(v==256){g->phase=g->final?G_TRAILER_CRC:G_BLOCK;if(g->final)gzip_take(g,g->nbits%8);}
            else {if(v>285)goto invalid;v-=257;g->length=length_base[v];g->extra=length_extra[v];g->phase=G_LENGTH_EXTRA;}
            break;
        case G_LENGTH_EXTRA:
            if(!gzip_bits(g,g->extra))goto wait;
            g->length+=gzip_take(g,g->extra);g->phase=G_DISTANCE;break;
        case G_DISTANCE:
            decoded=gzip_symbol(g,&g->dist,&v);if(!decoded)goto wait;if(decoded<0||v>29)goto invalid;
            g->distance=distance_base[v];g->extra=v<4?0:v/2-1;g->phase=G_DISTANCE_EXTRA;break;
        case G_DISTANCE_EXTRA:
            if(!gzip_bits(g,g->extra))goto wait;
            g->distance+=gzip_take(g,g->extra);if(g->distance>g->window_used)goto invalid;
            for(unsigned i=0;i<g->length;i++)if(!gzip_emit(g,sink,g->window[(g->window_at-g->distance)&32767]))return false;
            g->phase=G_SYMBOL;break;
        case G_TRAILER_CRC:
            if(!gzip_bits(g,32))goto wait;
            if(gzip_take(g,32)!=~g->crc)goto invalid;g->phase=G_TRAILER_SIZE;break;
        case G_TRAILER_SIZE:
            if(!gzip_bits(g,32))goto wait;
            if(gzip_take(g,32)!=g->size)goto invalid;
            g->members++;g->phase=G_HEADER;g->header_pos=0;g->header_crc=0xffffffffu;break;
        default:goto invalid;
        }
    }
wait:
    if(!gzip_flush(g,sink))return false;
    if(finish&&!(g->members&&g->phase==G_HEADER&&!g->header_pos&&!g->nbits))goto invalid;
    return true;
invalid:
    g->invalid=true;snprintf(sink->rs->error,sizeof sink->rs->error,"invalid or truncated gzip response");return false;
}
