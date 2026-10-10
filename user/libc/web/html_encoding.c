#include <stdio.h>
#include "html_encoding.h"
#include "web_encoding.h"
#include <lexbor/html/encoding.h>
#include <lexbor/encoding/encoding.h>
bool html_meta_label(const char *label,size_t length,char *canonical,size_t capacity){
    if(!label||!canonical||!capacity)return false;
    const lxb_encoding_data_t *enc=lxb_encoding_data_by_pre_name((const lxb_char_t *)label,length);
    if(!enc)return false;
    if(enc->encoding==LXB_ENCODING_UTF_16BE||enc->encoding==LXB_ENCODING_UTF_16LE)enc=lxb_encoding_data(LXB_ENCODING_UTF_8);
    else if(enc->encoding==LXB_ENCODING_X_USER_DEFINED)enc=lxb_encoding_data(LXB_ENCODING_WINDOWS_1252);
    size_t n=strlen((const char *)enc->name);if(n>=capacity)return false;
    memcpy(canonical,enc->name,n+1);return true;
}
void html_transport_label(const char *type,char *label,size_t capacity) {
    if(!capacity)return;label[0]=0;if(!type)return;
    const lxb_char_t *end=NULL;
    const lxb_char_t *name=lxb_html_encoding_content((const lxb_char_t *)type,(const lxb_char_t *)type+strlen(type),&end);
    if(name && (size_t)(end-name)<capacity){memcpy(label,name,end-name);label[end-name]=0;}
}

static bool strict_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++];
        if (c < 128) continue;
        unsigned count, cp, minimum;
        if (c >= 0xc2 && c <= 0xdf) { count=1; cp=c&31; minimum=0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count=2; cp=c&15; minimum=0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count=3; cp=c&7; minimum=0x10000; }
        else return false;
        if (count > n-i) return false;
        while (count--) { c=s[i++]; if ((c&0xc0)!=0x80) return false; cp=(cp<<6)|(c&63); }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}
struct html_decoded { char *data; size_t length, capacity; };
static bool emit(void *opaque, uint32_t cp, bool malformed) {
    (void)malformed;
    struct html_decoded *out = opaque;
    char bytes[4]; size_t count=(size_t)utf8_put(bytes,cp);
    if (out->length > UINT32_MAX-count) return false;
    size_t wanted=out->length+count+1;
    if (wanted > out->capacity) {
        size_t cap=out->capacity > SIZE_MAX/2 ? SIZE_MAX : out->capacity*2;
        if (cap < wanted) cap=wanted;
        char *grown=realloc(out->data,cap);
        if (!grown) return false;
        out->data=grown; out->capacity=cap;
    }
    memcpy(out->data+out->length,bytes,count); out->length+=count;
    return true;
}
char *html_decode_bytes(web_doc *d, const char *src, size_t n, const char *label, size_t *length) {
    if (!d || (!src && n) || n > UINT32_MAX || !length) return NULL;
    if (!src) src="";
    const lxb_char_t *bytes=(const lxb_char_t *)src;
    lxb_encoding_t bom=lxb_encoding_bom_sniff(bytes,n);
    const lxb_encoding_data_t *enc=NULL;
    d->encoding_certain=false;
    if (bom==LXB_ENCODING_UTF_8 || bom==LXB_ENCODING_UTF_16BE || bom==LXB_ENCODING_UTF_16LE) {
        enc=lxb_encoding_data(bom);
        d->encoding_certain=true;
        size_t skip=bom==LXB_ENCODING_UTF_8?3:2; bytes+=skip; n-=skip;
    } else if (label && *label){enc=lxb_encoding_data_by_pre_name((const lxb_char_t *)label,strlen(label));d->encoding_certain=enc!=NULL;}
    if (!enc) {
        lxb_html_encoding_t scan={0};
        if (lxb_html_encoding_init(&scan)!=LXB_STATUS_OK) return NULL;
        size_t name_length=0;
        const lxb_char_t *name=lxb_html_encoding_prescan(&scan,bytes,bytes+(n<1024?n:1024),&name_length);
        if (name){
            enc=lxb_encoding_data_by_name(name,name_length);
            /* Meta declarations normalize these labels. Preserve the special
             * UTF-16 XML-declaration heuristic when actual NUL bytes occur. */
            if(enc && enc->encoding==LXB_ENCODING_X_USER_DEFINED)enc=lxb_encoding_data(LXB_ENCODING_WINDOWS_1252);
            else if(enc && (enc->encoding==LXB_ENCODING_UTF_16BE||enc->encoding==LXB_ENCODING_UTF_16LE) &&
                    !(n>=6 && (bytes[0]==0||bytes[1]==0)))enc=lxb_encoding_data(LXB_ENCODING_UTF_8);
        }
        lxb_html_encoding_destroy(&scan,false);
    }
    /* Preserve Nocturne's existing UTF-8-first heuristic for unlabeled pages,
       but only scalar-valid UTF-8 qualifies. Explicit UTF-8 never falls back. */
    if (!enc) enc=lxb_encoding_data(strict_utf8(bytes,n)?LXB_ENCODING_UTF_8:LXB_ENCODING_WINDOWS_1252);
    if (!enc) return NULL;
    snprintf(d->encoding,sizeof d->encoding,"%s",(const char *)enc->name);
    struct html_decoded out={malloc(256),0,256};
    if (!out.data) return NULL;
    bool ok=true;
    if (web_shift_jis_label((const char *)enc->name)) {
        /* Keep the project's independently tested Shift_JIS decoder. */
        web_shift_jis_decoder decoder={0};
        ok=web_shift_jis_decode(&decoder,bytes,n,true,emit,&out);
    } else {
        lxb_codepoint_t buffer[512], replacement=0xfffd;
        lxb_encoding_decode_t decoder;
        ok=lxb_encoding_decode_init(&decoder,enc,buffer,512)==LXB_STATUS_OK &&
           lxb_encoding_decode_replace_set(&decoder,&replacement,1)==LXB_STATUS_OK;
        const lxb_char_t *end=bytes+n;
        while (ok && bytes<end) {
            const lxb_char_t *before=bytes;
            lxb_status_t status=enc->decode(&decoder,&bytes,end);
            size_t used=lxb_encoding_decode_buf_used(&decoder);
            for (size_t i=0;ok && i<used;i++) ok=emit(&out,buffer[i],false);
            lxb_encoding_decode_buf_used_set(&decoder,0);
            if ((status!=LXB_STATUS_OK && status!=LXB_STATUS_SMALL_BUFFER) || (before==bytes && !used)) ok=false;
        }
        if (ok) {
            ok=lxb_encoding_decode_finish(&decoder)==LXB_STATUS_OK;
            size_t used=lxb_encoding_decode_buf_used(&decoder);
            for (size_t i=0;ok && i<used;i++) ok=emit(&out,buffer[i],false);
        }
    }
    if (!ok) { free(out.data); return NULL; }
    out.data[out.length]=0; *length=out.length; return out.data;
}
