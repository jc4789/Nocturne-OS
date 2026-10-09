/* Bounded manifest parser. No network, XML entities, FFmpeg protocols,
 * JavaScript, or playback capability substitution. Unknown control extensions
 * fail explicitly instead of silently applying the wrong presentation. */
#include "media_adaptive_private.h"
#include "media_video_private.h"
#include "media_http_private.h"
#include "media_alloc_private.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
/* One manifest owns all persistent strings, including inherited templates.
 * No static URI ceiling or temporary whole-manifest stack copy. */
struct nmedia_adaptive_owned {struct nmedia_adaptive_owned *next;char data[];};
static char *manifest_storage(struct nmedia_adaptive_manifest *m,size_t n){
    if(n>SIZE_MAX-sizeof(struct nmedia_adaptive_owned)-1){m->allocation_failed=true;return NULL;}
    struct nmedia_adaptive_owned *p=nmedia_ff_malloc(sizeof *p+n+1);
    if(!p){m->allocation_failed=true;return NULL;}
    p->next=m->owned;m->owned=p;p->data[n]=0;return p->data;
}
static char *manifest_copy(struct nmedia_adaptive_manifest *m,const char *s,size_t n){
    char *out=manifest_storage(m,n);if(out&&n)memcpy(out,s,n);return out;
}
void nmedia_adaptive_manifest_clear(struct nmedia_adaptive_manifest *m){
    if(!m)return;struct nmedia_adaptive_owned *p=m->owned;
    while(p){struct nmedia_adaptive_owned *next=p->next;nmedia_ff_free(p);p=next;}
    memset(m,0,sizeof *m);
}
static bool reject(char *e,size_t z,const char *s){if(e&&z)snprintf(e,z,"%s",s);return false;}
static bool copy(char *out,size_t z,const char *p,size_t n){
    if(n>=z)return false;memcpy(out,p,n);out[n]=0;return true;
}
static bool uint_value(const char *p,int64_t *out){
    uint64_t v=0;if(!p||!*p)return false;
    for(;*p;p++){if(*p<'0'||*p>'9'||v>((uint64_t)INT64_MAX-(*p-'0'))/10)return false;v=v*10+(*p-'0');}
    *out=(int64_t)v;return true;
}
static bool seconds(const char *p,int64_t *out){
    uint64_t n=0,f=0,scale=1;if(*p<'0'||*p>'9')return false;
    while(*p>='0'&&*p<='9'){if(n>1000000000u)return false;n=n*10+(*p++-'0');}
    if(*p=='.'){p++;if(*p<'0'||*p>'9')return false;
        for(unsigned digits=0;*p>='0'&&*p<='9';digits++){if(digits>=6)return false;f=f*10+(*p++-'0');scale*=10;}}
    if(*p||n>1000000000u)return false;*out=(int64_t)(n*1000000+f*1000000/scale);return true;
}
static bool safe_uri(const char *p){
    if(!p||!*p)return false;
    for(const unsigned char *q=(const unsigned char *)p;*q;q++)
        if(*q<=32||*q>=127||*q=='#'||*q=='\\')return false;
    const char *a=strstr(p,"://");if(a&&strncmp(p,"http://",7)&&strncmp(p,"https://",8))return false;
    if(a){a+=3;const char *end=a+strcspn(a,"/?");if(a==end||memchr(a,'@',(size_t)(end-a)))return false;}
    return true;
}
enum http_url_result nmedia_adaptive_resolve_owned(const char *base,const char *uri,char **out){
    *out=NULL;if(!safe_uri(base)||!safe_uri(uri))return HTTP_URL_INVALID;
    return nmedia_http_resolve(base,uri,out);
}
bool nmedia_adaptive_resolve(const char *base,const char *uri,char *out,size_t z){
    char *owned=NULL;enum http_url_result result=nmedia_adaptive_resolve_owned(base,uri,&owned);
    bool ok=result==HTTP_URL_TUPLE&&out&&strlen(owned)<z;
    if(ok)memcpy(out,owned,strlen(owned)+1);nmedia_ff_free(owned);return ok;
}
struct attribute {char key[64];const char *value;};
static int attributes(struct nmedia_adaptive_manifest *m,const char *p,struct attribute *a,unsigned max){
    unsigned n=0;
    while(*p){
        if(n==max)return -1;const char *k=p;while(*p&&*p!='=')p++;
        if(*p!='='||!copy(a[n].key,sizeof a[n].key,k,(size_t)(p-k)))return -1;p++;
        const char *v=p;size_t len;
        if(*p=='"'){v=++p;while(*p&&*p!='"')p++;if(*p!='"')return -1;len=(size_t)(p-v);p++;}
        else{while(*p&&*p!=',')p++;len=(size_t)(p-v);}
        if(!len||!(a[n].value=manifest_copy(m,v,len)))return -1;
        for(unsigned i=0;i<n;i++)if(!strcmp(a[i].key,a[n].key))return -1;n++;
        if(*p){if(*p++!=','||!*p)return -1;}
    }return (int)n;
}
static const char *attr(struct attribute *a,int n,const char *key){
    for(int i=0;i<n;i++)if(!strcmp(a[i].key,key))return a[i].value;return NULL;
}
static bool range_value(char *p,int64_t *start,int64_t *size){
    char *at=strchr(p,'@');if(at)*at++=0;
    if(!uint_value(p,size)||!*size)return false;
    if(at&&!uint_value(at,start))return false;return (!at&&*start==-1)||(*start>=0&&*start<=INT64_MAX-*size);
}
static bool codec_list(const char *list){
    if(!list)return true;char b[256];if(!copy(b,sizeof b,list,strlen(list)))return false;
    for(char *p=b;p;){char *next=strchr(p,',');if(next)*next++=0;
        while(*p==' ')p++;size_t n=strlen(p);while(n&&p[n-1]==' ')p[--n]=0;
        bool ok=!strcmp(p,"mp4a.40.2")||!strcmp(p,"mp4a.40.5")||!strcmp(p,"mp4a.40.29");
        if((!strncmp(p,"avc1.",5)||!strncmp(p,"avc3.",5))&&n==11){
            bool hex=true;for(unsigned i=5;i<11;i++)if(!isxdigit((unsigned char)p[i]))hex=false;
            unsigned profile=(unsigned)strtoul(p+5,NULL,16)>>16;
            ok=hex&&(profile==66||profile==77||profile==88||profile==100);
        }
        if(!ok)return false;p=next;
    }return true;
}
static int parse_hls(char *text,size_t bytes,const char *base,
                             struct nmedia_adaptive_manifest *m,char *e,size_t z){
    if(e&&z)*e=0;if(!text||bytes<8||bytes>NMEDIA_ADAPTIVE_MANIFEST_BYTES||memchr(text,0,bytes)||
        strncmp(text,"#EXTM3U",7)||(text[7]!='\n'&&text[7]!='\r'))return (reject(e,z,"invalid HLS manifest"),-1);
    if(!safe_uri(base)||(m->tracks[0].base=manifest_copy(m,base,strlen(base)))==NULL)goto syntax;
    struct nmedia_adaptive_track *t=&m->tracks[0];
    bool master=false,ended=false,have_duration=false,target=false,have_map=false,pending_variant=false,variant_ok=false;
    int64_t duration=0,range_start=0,range_bytes=0,previous_end=0,best=INT64_MAX,bandwidth=0;
    const char *previous_uri="";
    for(char *line=text;line&&*line;){
        char *next=strchr(line,'\n');if(next)*next++=0;
        size_t n=strlen(line);if(n&&line[n-1]=='\r')line[--n]=0;
        if(!n||!strcmp(line,"#EXTM3U")){line=next;continue;}
        if(line[0]!='#'){
            if(ended)goto syntax;
            if(pending_variant){if(variant_ok&&bandwidth<best){if(!safe_uri(line)||(m->variant=manifest_copy(m,line,n))==NULL)goto syntax;best=bandwidth;}pending_variant=false;}
            else{
                if(master||!have_duration||t->count>=NMEDIA_ADAPTIVE_SEGMENTS||!safe_uri(line))goto syntax;
                if(range_bytes&&range_start<0){if(strcmp(previous_uri,line))goto syntax;range_start=previous_end;}
                struct nmedia_adaptive_segment *s=&t->segments[t->count++];if(!(s->uri=manifest_copy(m,line,n)))goto syntax;
                s->start_us=m->duration_us;s->duration_us=duration;s->range_start=range_start;s->range_bytes=range_bytes;
                if(m->duration_us>1000000000000000LL-duration)goto syntax;m->duration_us+=duration;
                previous_end=range_start+range_bytes;previous_uri=s->uri;have_duration=false;range_bytes=0;range_start=0;
            }
        }else if(!strncmp(line,"#EXTINF:",8)){
            if(master||have_duration)goto syntax;char *comma=strchr(line+8,',');if(!comma)goto syntax;*comma=0;
            if(!seconds(line+8,&duration)||duration<=0||duration>60000000)goto syntax;have_duration=true;
        }else if(!strncmp(line,"#EXT-X-STREAM-INF:",18)){
            if(t->count||pending_variant)goto syntax;master=true;pending_variant=true;
            struct attribute a[24];int na=attributes(m,line+18,a,24);const char *v=attr(a,na,"BANDWIDTH");
            if(na<0||!uint_value(v,&bandwidth)||bandwidth<=0)goto syntax;
            variant_ok=codec_list(attr(a,na,"CODECS"));
            if(attr(a,na,"AUDIO")||attr(a,na,"VIDEO")||attr(a,na,"SUBTITLES"))variant_ok=false;
            v=attr(a,na,"RESOLUTION");if(v){char b[64];int64_t w,h;
                if(!copy(b,sizeof b,v,strlen(v)))goto syntax;char *x=strchr(b,'x');if(!x)goto syntax;*x++=0;
                if(!uint_value(b,&w)||!uint_value(x,&h)||w<=0||h<=0)goto syntax;
                if(w>INT_MAX||h>INT_MAX||!nmedia_video_size((int)w,(int)h,NULL,NULL))variant_ok=false;}
            for(int i=0;i<na;i++)if(strcmp(a[i].key,"BANDWIDTH")&&strcmp(a[i].key,"AVERAGE-BANDWIDTH")&&
                strcmp(a[i].key,"CODECS")&&strcmp(a[i].key,"RESOLUTION")&&strcmp(a[i].key,"FRAME-RATE")&&
                strcmp(a[i].key,"PROGRAM-ID")&&strcmp(a[i].key,"NAME")&&strcmp(a[i].key,"CLOSED-CAPTIONS")&&
                strcmp(a[i].key,"AUDIO")&&strcmp(a[i].key,"VIDEO")&&strcmp(a[i].key,"SUBTITLES"))return (reject(e,z,"unsupported HLS variant extension"),-1);
        }else if(!strcmp(line,"#EXT-X-ENDLIST")){ended=true;}
        else if(!strncmp(line,"#EXT-X-TARGETDURATION:",22)){int64_t v;if(!uint_value(line+22,&v)||v<=0||v>60)goto syntax;target=true;}
        else if(!strncmp(line,"#EXT-X-VERSION:",15)){int64_t v;if(!uint_value(line+15,&v)||v<1||v>7)return (reject(e,z,"unsupported HLS protocol version"),-1);}
        else if(!strncmp(line,"#EXT-X-MEDIA-SEQUENCE:",22)){int64_t v;if(t->count||!uint_value(line+22,&v))goto syntax;}
        else if(!strncmp(line,"#EXT-X-PLAYLIST-TYPE:",21)){if(strcmp(line+21,"VOD"))return (reject(e,z,"live/event HLS is unsupported"),-1);}
        else if(!strcmp(line,"#EXT-X-INDEPENDENT-SEGMENTS")){}
        else if(!strncmp(line,"#EXT-X-PROGRAM-DATE-TIME:",25)){} /* Wall clock metadata; VOD media timing comes from packets. */
        else if(!strncmp(line,"#EXT-X-BYTERANGE:",17)){if(range_bytes)goto syntax;range_start=strchr(line+17,'@')?0:-1;
            if(!range_value(line+17,&range_start,&range_bytes))goto syntax;}
        else if(!strncmp(line,"#EXT-X-KEY:",11)){
            struct attribute a[8];int na=attributes(m,line+11,a,8);const char *v=attr(a,na,"METHOD");
            if(na!=1||!v||strcmp(v,"NONE"))return (reject(e,z,"encrypted HLS is unsupported"),-1);
        }else if(!strncmp(line,"#EXT-X-MAP:",11)){
            if(master||t->count||have_map)goto syntax;struct attribute a[8];int na=attributes(m,line+11,a,8);
            const char *v=attr(a,na,"URI");if(na<1||!v||!safe_uri(v))goto syntax;
            v=attr(a,na,"BYTERANGE");if(v){char b[128];if(!copy(b,sizeof b,v,strlen(v))||!strchr(b,'@')||
                !range_value(b,&t->init_start,&t->init_bytes))goto syntax;}
            for(int i=0;i<na;i++)if(strcmp(a[i].key,"URI")&&strcmp(a[i].key,"BYTERANGE"))goto syntax;
            t->init=attr(a,na,"URI");t->mp4=true;have_map=true;
        }else if(!strncmp(line,"#EXT-X-",7))return (reject(e,z,"unsupported HLS control tag"),-1);
        line=next;
    }
    if(pending_variant||have_duration||range_bytes)goto syntax;
    if(master){if(!m->variant||!m->variant[0])return (reject(e,z,"no supported muxed HLS variant"),-1);return 2;}
    if(!ended)return (reject(e,z,"live HLS reload is unsupported"),-1);
    if(!target||!t->count)goto syntax;m->count=1;return 1;
syntax:reject(e,z,"invalid or oversized HLS playlist");return -1;
}

struct xml_element {char name[64];const char *attributes,*attributes_end,*body,*end,*after;bool empty;};
struct xml_reader {char *error;size_t capacity;unsigned elements;struct nmedia_adaptive_manifest *owner;};
static const char *ws(const char *p,const char *end){while(p<end&&(*p==' '||*p=='\t'||*p=='\r'||*p=='\n'))p++;return p;}
static bool xml_token(struct xml_reader *r,const char *p,const char *end,
                      struct xml_element *x,bool *closing){
    if(p>=end||*p++!='<')return reject(r->error,r->capacity,"invalid DASH XML");
    *closing=p<end&&*p=='/';if(*closing)p++;
    const char *name=p;while(p<end&&(isalnum((unsigned char)*p)||*p=='_'||*p=='-'||*p==':'))p++;
    if(p==name||!copy(x->name,sizeof x->name,name,(size_t)(p-name))||strchr(x->name,':'))
        return reject(r->error,r->capacity,"unsupported DASH XML namespace prefix");
    x->attributes=p;char quote=0;
    while(p<end){
        if(quote){if(*p==quote)quote=0;}
        else if(*p=='"'||*p=='\'')quote=*p;
        else if(*p=='>')break;
        else if(*p=='<')return reject(r->error,r->capacity,"invalid DASH XML attribute");
        p++;
    }
    if(p==end||quote)return reject(r->error,r->capacity,"truncated DASH XML tag");
    x->empty=p>x->attributes&&p[-1]=='/';x->attributes_end=x->empty?p-1:p;
    x->body=x->after=p+1;x->end=p+1;
    if(++r->elements>32768)return reject(r->error,r->capacity,"DASH XML element budget");
    return true;
}
static bool xml_next(struct xml_reader *r,const char **cursor,const char *end,struct xml_element *x){
    const char *p=ws(*cursor,end);
    while(p<end&&(!strncmp(p,"<!--",4)||!strncmp(p,"<?",2))){
        const char *stop=strstr(p,p[1]=='!'?"-->":"?>");if(!stop||stop>=end)return reject(r->error,r->capacity,"truncated DASH XML comment");
        p=ws(stop+(p[1]=='!'?3:2),end);
    }
    if(p==end){*cursor=p;memset(x,0,sizeof *x);return true;}
    bool close;if(!xml_token(r,p,end,x,&close)||close)return reject(r->error,r->capacity,"invalid DASH XML element");
    if(!strcmp(x->name,"ContentProtection"))return reject(r->error,r->capacity,"encrypted DASH is unsupported");
    if(x->empty){x->end=x->body;*cursor=x->after;return true;}
    char stack[16][64];memcpy(stack[0],x->name,strlen(x->name)+1);unsigned depth=1;p=x->body;
    while(p<end){
        if(*p!='<'){p++;continue;}
        if(!strncmp(p,"<!--",4)||!strncmp(p,"<?",2)){
            const char *stop=strstr(p,p[1]=='!'?"-->":"?>");if(!stop||stop>=end)return reject(r->error,r->capacity,"truncated DASH XML comment");
            p=stop+(p[1]=='!'?3:2);continue;
        }
        struct xml_element t;if(!xml_token(r,p,end,&t,&close))return false;
        if(!strcmp(t.name,"ContentProtection"))return reject(r->error,r->capacity,"encrypted DASH is unsupported");
        if(close){
            if(!depth||strcmp(t.name,stack[depth-1])||ws(t.attributes,t.attributes_end)!=t.attributes_end)
                return reject(r->error,r->capacity,"unbalanced DASH XML");
            if(!--depth){x->end=p;x->after=t.after;*cursor=x->after;return true;}
        }else if(!t.empty){
            if(depth==16)return reject(r->error,r->capacity,"DASH XML nesting limit");
            memcpy(stack[depth++],t.name,strlen(t.name)+1);
        }
        p=t.after;
    }
    return reject(r->error,r->capacity,"truncated DASH XML element");
}
static char *xml_text(struct nmedia_adaptive_manifest *m,const char *p,const char *end){
    char *out=manifest_storage(m,(size_t)(end-p));if(!out)return NULL;size_t n=0;
    while(p<end){
        unsigned char c=(unsigned char)*p++;
        if(c=='&'){
            const char *tail=memchr(p,';',(size_t)(end-p));if(!tail)return false;size_t z=(size_t)(tail-p);
            if(z==3&&!strncmp(p,"amp",3))c='&';else if(z==2&&!strncmp(p,"lt",2))c='<';
            else if(z==2&&!strncmp(p,"gt",2))c='>';else if(z==4&&!strncmp(p,"quot",4))c='"';
            else if(z==4&&!strncmp(p,"apos",4))c='\'';else return false;p=tail+1;
        }
        if(c==0)return NULL;out[n++]=(char)c;
    }out[n]=0;return out;
}
static int xml_attributes(struct xml_reader *r,const struct xml_element *x,struct attribute *a,unsigned max){
    const char *p=x->attributes,*end=x->attributes_end;unsigned n=0;
    while((p=ws(p,end))<end){
        if(n==max)goto invalid;const char *k=p;while(p<end&&(isalnum((unsigned char)*p)||*p=='_'||*p==':'||*p=='-'||*p=='.'))p++;
        if(p==k||!copy(a[n].key,sizeof a[n].key,k,(size_t)(p-k)))goto invalid;
        p=ws(p,end);if(p==end||*p++!='=')goto invalid;p=ws(p,end);if(p==end||(*p!='"'&&*p!='\''))goto invalid;
        char quote=*p++;const char *v=p;while(p<end&&*p!=quote)p++;if(p==end||!(a[n].value=xml_text(r->owner,v,p)))goto invalid;p++;
        for(unsigned i=0;i<n;i++)if(!strcmp(a[i].key,a[n].key))goto invalid;n++;
        if(p<end&&ws(p,end)==p)goto invalid;
    }return (int)n;
invalid:reject(r->error,r->capacity,"invalid or oversized DASH XML attributes");return -1;
}
static bool allowed_attributes(struct xml_reader *r,struct attribute *a,int n,const char *names){
    if(n<0)return false;
    for(int i=0;i<n;i++){
        const char *p=names;bool ok=false;size_t z=strlen(a[i].key);
        while(*p){const char *end=strchr(p,'|');if(!end)end=p+strlen(p);
            if((size_t)(end-p)==z&&!strncmp(p,a[i].key,z))ok=true;p=*end?end+1:end;}
        if(!ok)return reject(r->error,r->capacity,"unsupported DASH attribute");
    }return true;
}
static bool iso_duration(const char *p,int64_t *out){
    if(!p||strncmp(p,"PT",2))return false;p+=2;int64_t sum=0;unsigned previous=0;
    while(*p){
        const char *first=p;while((*p>='0'&&*p<='9')||*p=='.')p++;char kind=*p++;char n[64];int64_t us;unsigned order;
        if(!copy(n,sizeof n,first,(size_t)(p-first-1))||!seconds(n,&us))return false;
        int multiplier=kind=='H'?3600:kind=='M'?60:kind=='S'?1:0;order=kind=='H'?1:kind=='M'?2:3;
        if(!multiplier||order<=previous||us>1000000000000000LL/multiplier||sum>1000000000000000LL-us*multiplier)return false;
        sum+=us*multiplier;previous=order;
    }*out=sum;return previous!=0;
}
struct dash_template {
    const char *media,*init;
    int64_t scale,duration,number,offset;
    const char *timeline,*timeline_end;
};
static bool dash_template_read(struct xml_reader *r,const struct xml_element *x,struct dash_template *t){
    struct attribute a[16];int n=xml_attributes(r,x,a,16);
    if(!allowed_attributes(r,a,n,"media|initialization|timescale|duration|startNumber|presentationTimeOffset"))return false;
    const char *v;
    if((v=attr(a,n,"media")))t->media=v;
    if((v=attr(a,n,"initialization")))t->init=v;
    if((v=attr(a,n,"timescale"))&&(!uint_value(v,&t->scale)||t->scale<1||t->scale>1000000000))goto invalid;
    if((v=attr(a,n,"duration"))&&(!uint_value(v,&t->duration)||t->duration<1))goto invalid;
    if((v=attr(a,n,"startNumber"))&&!uint_value(v,&t->number))goto invalid;
    if((v=attr(a,n,"presentationTimeOffset"))&&!uint_value(v,&t->offset))goto invalid;
    const char *p=x->body;struct xml_element child;bool timeline=false;
    while(p<x->end){if(!xml_next(r,&p,x->end,&child))return false;if(!child.name[0])break;
        if(strcmp(child.name,"SegmentTimeline")||timeline)return reject(r->error,r->capacity,"unsupported DASH segment template child");
        struct attribute ignored[1];if(xml_attributes(r,&child,ignored,1)!=0)return reject(r->error,r->capacity,"unsupported DASH timeline attributes");
        timeline=true;t->timeline=child.body;t->timeline_end=child.end;
    }return true;
invalid:return reject(r->error,r->capacity,"invalid DASH segment template value");
}
/* Two passes measure exact substitutions, then fill the same stable plan.
 * RepresentationID is data, not a printf format and has no arbitrary width. */
static char *dash_expand(struct nmedia_adaptive_manifest *m,const char *pattern,const char *id,
                         int64_t bandwidth,int64_t number,int64_t time){
    if(!pattern)return NULL;size_t measured=0;char *out=NULL;
    for(unsigned pass=0;pass<2;pass++){
        size_t used=0;
        for(const char *p=pattern;*p;){
            const char *value;char numeric[32];size_t n;
            if(*p!='$'){value=p++;n=1;}
            else if(*++p=='$'){value=p++;n=1;}
            else{
                const char *end=strchr(p,'$');char token[64];
                if(!end||!copy(token,sizeof token,p,(size_t)(end-p)))return NULL;p=end+1;
                char *format=strchr(token,'%');int width=0;
                if(format){*format++=0;if(*format++!='0'||*format<'1'||*format>'9')return NULL;
                    width=*format++-'0';if(*format++!='d'||*format)return NULL;}
                if(!strcmp(token,"RepresentationID")){if(width||!id)return NULL;value=id;n=strlen(id);}
                else{
                    int64_t v=!strcmp(token,"Bandwidth")?bandwidth:!strcmp(token,"Number")?number:!strcmp(token,"Time")?time:-1;
                    if(v<0)return NULL;int wrote=snprintf(numeric,sizeof numeric,"%0*lld",width,(long long)v);
                    if(wrote<0||(size_t)wrote>=sizeof numeric)return NULL;value=numeric;n=(size_t)wrote;
                }
            }
            if(n>SIZE_MAX-used-1){m->allocation_failed=true;return NULL;}
            if(pass){if(used>measured||n>measured-used)return NULL;memcpy(out+used,value,n);}used+=n;
        }
        if(!pass){if(!used)return NULL;measured=used;out=manifest_storage(m,used);if(!out)return NULL;}
        else{if(used!=measured)return NULL;out[used]=0;}
    }return out;
}
static bool scale_us(int64_t units,int64_t scale,int64_t *out){
    if(units<0||scale<=0||units/scale>1000000000)return false;
    *out=(units/scale)*1000000+(units%scale)*1000000/scale;return true;
}
static bool dash_segment(struct xml_reader *r,struct nmedia_adaptive_track *t,const struct dash_template *d,
                          const char *id,int64_t bandwidth,int64_t number,int64_t time,int64_t duration,int64_t presentation){
    int64_t start,span;if(t->count==NMEDIA_ADAPTIVE_SEGMENTS||!scale_us(time,d->scale,&start)||
        !scale_us(duration,d->scale,&span)||span<=0||span>60000000)return reject(r->error,r->capacity,"DASH segment budget or timeline limit");
    start-=t->presentation_offset_us;if(start<0)return reject(r->error,r->capacity,"DASH negative segment start is unsupported");
    if(start>=presentation)return true;
    struct nmedia_adaptive_segment *s=&t->segments[t->count++];
    if(!(s->uri=dash_expand(r->owner,d->media,id,bandwidth,number,time))||!safe_uri(s->uri))
        return reject(r->error,r->capacity,"unsupported DASH URL template");
    s->start_us=start;s->duration_us=span;s->range_start=s->range_bytes=0;return true;
}
static bool dash_track_build(struct xml_reader *r,struct nmedia_adaptive_track *t,const struct dash_template *d,
                              const char *id,int64_t bandwidth,int64_t presentation){
    if(!d->media||!d->init||!d->media[0]||!d->init[0]||!scale_us(d->offset,d->scale,&t->presentation_offset_us)||
       !(t->init=dash_expand(r->owner,d->init,id,bandwidth,d->number,0))||!safe_uri(t->init))
        return reject(r->error,r->capacity,"missing or unsupported DASH initialization");
    t->mp4=true;int64_t number=d->number,time=0;
    if(!d->timeline){
        if(d->offset)return reject(r->error,r->capacity,"DASH duration template with nonzero presentation offset is unsupported");
        if(!d->duration)return reject(r->error,r->capacity,"missing DASH segment duration");
        time=d->offset;
        for(unsigned i=0;i<NMEDIA_ADAPTIVE_SEGMENTS;i++){
            int64_t start;if(!scale_us(time,d->scale,&start))goto limit;
            if(start-t->presentation_offset_us>=presentation)return t->count!=0;
            if(!dash_segment(r,t,d,id,bandwidth,number,time,d->duration,presentation))return false;
            if(time>INT64_MAX-d->duration||number==INT64_MAX)goto limit;time+=d->duration;number++;
        }goto limit;
    }
    const char *p=d->timeline;struct xml_element s;bool first=true;
    while(p<d->timeline_end){
        if(!xml_next(r,&p,d->timeline_end,&s))return false;if(!s.name[0])break;
        if(strcmp(s.name,"S")||!s.empty)return reject(r->error,r->capacity,"unsupported DASH timeline entry");
        struct attribute a[4];int n=xml_attributes(r,&s,a,4);
        if(!allowed_attributes(r,a,n,"t|d|r"))return false;
        const char *v=attr(a,n,"d");int64_t dur,repeat=0;
        if(!uint_value(v,&dur)||dur<=0)goto limit;
        if((v=attr(a,n,"t"))){int64_t explicit;if(!uint_value(v,&explicit)||(!first&&explicit!=time))return reject(r->error,r->capacity,"DASH timeline gap/overlap is unsupported");time=explicit;}
        v=attr(a,n,"r");if(v&&!strcmp(v,"-1"))return reject(r->error,r->capacity,"open-ended DASH repeat is unsupported");
        if(v&&!uint_value(v,&repeat))goto limit;
        if(repeat>=NMEDIA_ADAPTIVE_SEGMENTS||repeat+1>NMEDIA_ADAPTIVE_SEGMENTS-t->count)goto limit;
        for(int64_t i=0;i<=repeat;i++){
            if(!dash_segment(r,t,d,id,bandwidth,number,time,dur,presentation))return false;
            if(time>INT64_MAX-dur||number==INT64_MAX)goto limit;time+=dur;number++;
        }first=false;
    }
    return t->count!=0||reject(r->error,r->capacity,"empty DASH timeline");
limit:return reject(r->error,r->capacity,"DASH segment count or time overflow");
}
static bool dash_base(struct xml_reader *r,const struct xml_element *x,const char *base,const char **out){
    struct attribute a[1];if(xml_attributes(r,x,a,1)!=0)return reject(r->error,r->capacity,"DASH BaseURL extensions are unsupported");
    const char *p=ws(x->body,x->end),*end=x->end;while(end>p&&isspace((unsigned char)end[-1]))end--;
    char *uri=xml_text(r->owner,p,end),*resolved=NULL;
    enum http_url_result result=uri?nmedia_adaptive_resolve_owned(base,uri,&resolved):HTTP_URL_INVALID;
    if(result==HTTP_URL_OOM)r->owner->allocation_failed=true;
    if(result==HTTP_URL_TUPLE)*out=manifest_copy(r->owner,resolved,strlen(resolved));
    nmedia_ff_free(resolved);
    return (result==HTTP_URL_TUPLE&&*out)||reject(r->error,r->capacity,"invalid DASH BaseURL");
}
struct dash_representation {const char *id,*codec,*mime,*base;int64_t bandwidth,width,height;struct dash_template templ;};
static bool dash_rep_read(struct xml_reader *r,const struct xml_element *x,const struct dash_representation *in,
                           struct dash_representation *out){
    *out=*in;struct attribute a[24];int n=xml_attributes(r,x,a,24);
    if(!allowed_attributes(r,a,n,"id|codecs|mimeType|bandwidth|width|height|frameRate|sar|scanType|audioSamplingRate|startWithSAP|qualityRanking"))return false;
    const char *v=attr(a,n,"id");if(!v||!*v)return reject(r->error,r->capacity,"invalid DASH representation ID");
    out->id=v;if((v=attr(a,n,"codecs")))out->codec=v;
    if((v=attr(a,n,"mimeType")))out->mime=v;
    if(!uint_value(attr(a,n,"bandwidth"),&out->bandwidth)||out->bandwidth<=0)return reject(r->error,r->capacity,"invalid DASH representation bandwidth");
    if((v=attr(a,n,"width"))&&!uint_value(v,&out->width))return false;
    if((v=attr(a,n,"height"))&&!uint_value(v,&out->height))return false;
    const char *p=x->body;struct xml_element c;bool base=false,templ=false;
    while(p<x->end){if(!xml_next(r,&p,x->end,&c))return false;if(!c.name[0])break;
        if(!strcmp(c.name,"BaseURL")){if(base||!dash_base(r,&c,in->base,&out->base))return false;base=true;}
        else if(!strcmp(c.name,"SegmentTemplate")){if(templ||!dash_template_read(r,&c,&out->templ))return false;templ=true;}
        else if(strcmp(c.name,"AudioChannelConfiguration"))return reject(r->error,r->capacity,"unsupported DASH representation child");
    }return true;
}
static bool dash_adaptation(struct xml_reader *r,const struct xml_element *x,const char *base,const struct dash_template *in,
                             struct nmedia_adaptive_manifest *m,int64_t presentation,bool kinds[2]){
    struct attribute a[32];int n=xml_attributes(r,x,a,32);
    if(!allowed_attributes(r,a,n,"id|mimeType|contentType|codecs|lang|segmentAlignment|subsegmentAlignment|subsegmentStartsWithSAP|startWithSAP|bitstreamSwitching|par|width|height|frameRate|sar|audioSamplingRate"))return false;
    struct dash_representation defaults={.id="",.codec="",.mime="",.base=base};defaults.templ=*in;
    const char *v=attr(a,n,"mimeType");if(v)defaults.mime=v;
    v=attr(a,n,"codecs");if(v)defaults.codec=v;
    if((v=attr(a,n,"width"))&&!uint_value(v,&defaults.width))return false;if((v=attr(a,n,"height"))&&!uint_value(v,&defaults.height))return false;
    const char *p=x->body;struct xml_element c;bool have_base=false,have_template=false;
    /* Resolve inherited siblings before selecting a representation, independent of ordering. */
    while(p<x->end){if(!xml_next(r,&p,x->end,&c))return false;if(!c.name[0])break;
        if(!strcmp(c.name,"BaseURL")){if(have_base||!dash_base(r,&c,base,&defaults.base))return false;have_base=true;}
        else if(!strcmp(c.name,"SegmentTemplate")){if(have_template||!dash_template_read(r,&c,&defaults.templ))return false;have_template=true;}
        else if(strcmp(c.name,"Representation")&&strcmp(c.name,"Role")&&strcmp(c.name,"Accessibility")&&strcmp(c.name,"AudioChannelConfiguration"))
            return reject(r->error,r->capacity,"unsupported DASH adaptation child");
    }
    struct dash_representation best={0};best.bandwidth=INT64_MAX;p=x->body;
    while(p<x->end){if(!xml_next(r,&p,x->end,&c))return false;if(!c.name[0])break;if(strcmp(c.name,"Representation"))continue;
        struct dash_representation rep;if(!dash_rep_read(r,&c,&defaults,&rep))return false;
        bool video=!strcmp(rep.mime,"video/mp4"),audio=!strcmp(rep.mime,"audio/mp4");
        if((!video&&!audio)||!rep.codec[0]||!codec_list(rep.codec))continue;
        if(video&&(rep.width<=0||rep.height<=0||rep.width>INT_MAX||rep.height>INT_MAX||!nmedia_video_size((int)rep.width,(int)rep.height,NULL,NULL)))continue;
        if(rep.bandwidth<best.bandwidth)best=rep;
    }
    if(best.bandwidth==INT64_MAX)return reject(r->error,r->capacity,"no supported DASH representation");
    unsigned kind=!strcmp(best.mime,"video/mp4")?1:0;
    if(kinds[kind]||m->count==2)return reject(r->error,r->capacity,"multiple DASH adaptations of one media kind are unsupported");
    kinds[kind]=true;struct nmedia_adaptive_track *t=&m->tracks[m->count++];
    t->base=best.base;
    return dash_track_build(r,t,&best.templ,best.id,best.bandwidth,presentation);
}
static bool parse_dash(char *text,size_t bytes,const char *base,struct nmedia_adaptive_manifest *m,char *e,size_t z){
    if(e&&z)*e=0;
    if(!text||!bytes||bytes>NMEDIA_ADAPTIVE_MANIFEST_BYTES||memchr(text,0,bytes))
        return reject(e,z,"invalid DASH manifest bytes");
    m->dash=true;struct xml_reader r={e,z,0,m};
    const char *p=text,*end=text+bytes;struct xml_element mpd;
    if(!xml_next(&r,&p,end,&mpd)||strcmp(mpd.name,"MPD")||ws(p,end)!=end)return reject(e,z,"invalid DASH root");
    struct attribute a[32];int n=xml_attributes(&r,&mpd,a,32);
    if(!allowed_attributes(&r,a,n,"xmlns|xmlns:xsi|xsi:schemaLocation|id|type|profiles|mediaPresentationDuration|minBufferTime|maxSegmentDuration|maxSubsegmentDuration"))return false;
    const char *v=attr(a,n,"type");if(v&&strcmp(v,"static"))return reject(e,z,"dynamic DASH is unsupported");
    v=attr(a,n,"xmlns");if(v&&strcmp(v,"urn:mpeg:dash:schema:mpd:2011"))return reject(e,z,"unsupported DASH namespace");
    if(!iso_duration(attr(a,n,"mediaPresentationDuration"),&m->duration_us)||m->duration_us<=0)return reject(e,z,"missing finite DASH duration");
    const char *root_base=manifest_copy(m,base,strlen(base));if(!root_base||!safe_uri(base))return false;
    struct dash_template root_template={.scale=1,.number=1};struct xml_element period={0},c;bool b=false,t=false;
    p=mpd.body;
    while(p<mpd.end){if(!xml_next(&r,&p,mpd.end,&c))return false;if(!c.name[0])break;
        if(!strcmp(c.name,"BaseURL")){if(b||!dash_base(&r,&c,base,&root_base))return false;b=true;}
        else if(!strcmp(c.name,"SegmentTemplate")){if(t||!dash_template_read(&r,&c,&root_template))return false;t=true;}
        else if(!strcmp(c.name,"Period")){if(period.name[0])return reject(e,z,"multiple DASH periods are unsupported");period=c;}
        else if(strcmp(c.name,"ProgramInformation"))return reject(e,z,"unsupported DASH root child");
    }
    if(!period.name[0])return reject(e,z,"missing DASH period");
    n=xml_attributes(&r,&period,a,32);if(!allowed_attributes(&r,a,n,"id|start|duration|bitstreamSwitching"))return false;
    int64_t start=0,presentation=m->duration_us;
    if((v=attr(a,n,"start"))&&(!iso_duration(v,&start)||start!=0))return reject(e,z,"nonzero DASH period start is unsupported");
    if((v=attr(a,n,"duration"))&&(!iso_duration(v,&presentation)||presentation<=0||presentation>m->duration_us))return reject(e,z,"invalid DASH period duration");
    const char *period_base=root_base;
    struct dash_template period_template=root_template;b=t=false;p=period.body;
    while(p<period.end){if(!xml_next(&r,&p,period.end,&c))return false;if(!c.name[0])break;
        if(!strcmp(c.name,"BaseURL")){if(b||!dash_base(&r,&c,root_base,&period_base))return false;b=true;}
        else if(!strcmp(c.name,"SegmentTemplate")){if(t||!dash_template_read(&r,&c,&period_template))return false;t=true;}
        else if(strcmp(c.name,"AdaptationSet"))return reject(e,z,"unsupported DASH period child");
    }
    bool kinds[2]={false,false};p=period.body;
    while(p<period.end){if(!xml_next(&r,&p,period.end,&c))return false;if(!c.name[0])break;
        if(!strcmp(c.name,"AdaptationSet")&&!dash_adaptation(&r,&c,period_base,&period_template,m,presentation,kinds))return false;
    }
    m->duration_us=presentation;return m->count!=0||reject(e,z,"missing DASH media adaptations");
}
int nmedia_adaptive_parse_hls(char *text,size_t bytes,const char *base,
                             struct nmedia_adaptive_manifest *m,char *e,size_t z){
    if(!m){reject(e,z,"invalid HLS parser owner");return -1;}
    memset(m,0,sizeof *m);if(!base){reject(e,z,"invalid HLS parser base");return -1;}
    int result=parse_hls(text,bytes,base,m,e,z);
    if(result<0){if(m->allocation_failed)reject(e,z,"adaptive manifest allocation or size failure");nmedia_adaptive_manifest_clear(m);}
    return result;
}
bool nmedia_adaptive_parse_dash(char *text,size_t bytes,const char *base,
                                struct nmedia_adaptive_manifest *m,char *e,size_t z){
    if(!m)return reject(e,z,"invalid DASH parser owner");
    memset(m,0,sizeof *m);if(!base)return reject(e,z,"invalid DASH parser base");
    bool result=parse_dash(text,bytes,base,m,e,z);
    if(!result){if(m->allocation_failed)reject(e,z,"adaptive manifest allocation or size failure");nmedia_adaptive_manifest_clear(m);}
    return result;
}
