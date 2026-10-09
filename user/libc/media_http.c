/* Anonymous Range input over Nocturne's verified TLS / HTTP client.
 * One 256 KiB cache, private media quota, no FF network protocols or global
 * cookies. Refills are synchronous: this is not asynchronous streaming.
 * RFC 9110 sections 13.1.5 and 14: never splice different representations. */
#include "media_http_private.h"
#include "media_alloc_private.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define MEDIA_HTTP_TIMEOUT_MS 5000
struct nmedia_http {
    char *url;
    char etag[160], modified[30], error[160];
    uint8_t *complete;
    bool use_modified, need_complete;
    char *origin; /* common HTTP origin owns an exact malloc string */
    int64_t size, position, cache_start;
    size_t cache_len;
    bool initialized, browser, cross;
    unsigned char cache[NMEDIA_HTTP_CACHE_BYTES];
};
struct transfer {
    nmedia_http *reader;
    struct http_resp *response;
    int64_t first, last, range_first, range_last, total, length;
    size_t got, expected;
    bool have_range, have_length, have_encoding, have_type, have_etag;
    bool have_date, have_modified;
    bool checked, discard;
    bool have_origin, have_methods, have_headers;
    char *allow_origin, *allow_methods, *allow_headers;
    char *location;
    char etag[160], date[30], modified[30], error[160];
};
static int rejected(struct transfer *t, const char *message) {
    snprintf(t->error, sizeof t->error, "%s", message);
    return -1;
}
static bool decimal(const char **text, int64_t *out) {
    const char *p = *text;
    uint64_t n = 0;
    if (*p < '0' || *p > '9') return false;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (n > ((uint64_t)INT64_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *text = p; *out = (int64_t)n;
    return true;
}
static bool content_range(const char *p, int64_t *first, int64_t *last, int64_t *total) {
    if (strncasecmp(p, "bytes ", 6)) return false;
    p += 6;
    return decimal(&p, first) && *p++ == '-' && decimal(&p, last) &&
        *p++ == '/' && decimal(&p, total) && !*p &&
        *last >= *first && *total > *last;
}
/* Strict IMF-fixdate; no locale, host timezone, or time_t dependency. Only a
 * pinned representation validator uses this, not a general date parser. */
static bool http_date(const char *s, int64_t *out) {
    if(strlen(s)!=29 || s[3]!=',' || s[4]!=' ' || s[7]!=' ' || s[11]!=' ' ||
       s[16]!=' ' || s[19]!=':' || s[22]!=':' || strcmp(s+25," GMT"))return false;
    static const char *days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int week=-1,month=-1;for(int i=0;i<7;i++)if(!memcmp(s,days[i],3))week=i;
    for(int i=0;i<12;i++)if(!memcmp(s+8,months[i],3))month=i+1;
    const int digit[]={5,6,12,13,14,15,17,18,20,21,23,24};
    for(size_t i=0;i<sizeof digit/sizeof digit[0];i++)if(s[digit[i]]<'0'||s[digit[i]]>'9')return false;
    int day=(s[5]-'0')*10+s[6]-'0',year=(s[12]-'0')*1000+(s[13]-'0')*100+(s[14]-'0')*10+s[15]-'0';
    int hour=(s[17]-'0')*10+s[18]-'0',minute=(s[20]-'0')*10+s[21]-'0',second=(s[23]-'0')*10+s[24]-'0';
    bool leap=year%4==0&&(year%100!=0||year%400==0);
    static const int count[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(week<0||month<1||year<1601||day<1||day>count[month-1]+(month==2&&leap)||hour>23||minute>59||second>59)return false;
    int y=year-(month<=2),era=y/400,yoe=y-era*400;
    int doy=(153*(month+(month>2?-3:9))+2)/5+day-1;
    int64_t date=(int64_t)era*146097+yoe*365+yoe/4-yoe/100+doy-719468;
    int actual=(int)((date+4)%7);if(actual<0)actual+=7;if(actual!=week)return false;
    *out=date*86400+hour*3600+minute*60+second;return true;
}
static bool strong_modified(const struct transfer *t) {
    int64_t date,modified;
    /* RFC 9110 8.8.2.2 allows >=1s with common-clock evidence. The conservative
     * >=60s gap avoids relying on clock synchronization of unknown CDNs. */
    return t->have_date&&t->have_modified&&http_date(t->date,&date)&&
        http_date(t->modified,&modified)&&date-modified>=60;
}

static bool strong_tag(const char *p) {
    size_t n = strlen(p);
    if (n < 2 || p[0] != '"' || p[n-1] != '"') return false;
    for (size_t i = 1; i + 1 < n; i++)
        if ((unsigned char)p[i] < 33 || (unsigned char)p[i] > 126 || p[i] == '"') return false;
    return true;
}
/* Media resources are explicit HTTP(S), not bare-host, userinfo or fragment
 * aliases. Shared owned parsing supplies exact host/path and allocation status.
 * No media-only spelling length, hostname or path quota is imposed. */
static enum http_url_result valid_url(const char *s,struct url_owned *parsed) {
    memset(parsed,0,sizeof *parsed);
    if(!s||(strncmp(s,"https://",8)&&strncmp(s,"http://",7)))return HTTP_URL_INVALID;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++)
        if(*p<=32||*p>=127||*p=='#'||*p=='\\')return HTTP_URL_INVALID;
    return url_parse_owned_result_n(s,strlen(s),parsed);
}
static enum http_url_result valid_url_text(const char *s){
    struct url_owned parsed;
    enum http_url_result result=valid_url(s,&parsed);
    url_owned_free(&parsed);return result;
}
static char *url_join(const char *a,size_t an,const char *b,size_t bn,const char *c,size_t cn) {
    if(cn==SIZE_MAX||an>SIZE_MAX-bn||an+bn>SIZE_MAX-cn-1)return NULL;
    size_t length=an+bn+cn;char *owned=nmedia_ff_malloc(length+1);
    if(!owned)return NULL;
    if(an)memcpy(owned,a,an);if(bn)memcpy(owned+an,b,bn);if(cn)memcpy(owned+an+bn,c,cn);
    owned[length]=0;return owned;
}
/* Document origins use the same exact serializer as browser fetch/frames.
 * Opaque documents still fail closed: never manufacture an Origin:null grant.
 * The media resource's explicit HTTP grammar is independent of this. */
static enum http_url_result browser_origin(const char *url,const char *document_url,
                                          char **site,bool *cross) {
    char *target=NULL;*site=NULL;
    enum http_url_result result=http_origin_owned(document_url,site);
    if(result==HTTP_URL_TUPLE)result=http_origin_owned(url,&target);
    if(result==HTTP_URL_TUPLE&&!strncmp(*site,"https://",8)&&strncmp(target,"https://",8))
        result=HTTP_URL_INVALID;
    if(result==HTTP_URL_TUPLE)*cross=strcmp(*site,target)!=0;
    else {free(*site);*site=NULL;}
    free(target);return result;
}
bool nmedia_http_browser_url(const char *url,const char *document_url) {
    if(valid_url_text(url)!=HTTP_URL_TUPLE)return false;
    char *site;bool cross;
    enum http_url_result result=browser_origin(url,document_url,&site,&cross);
    free(site);return result==HTTP_URL_TUPLE;
}
static bool reader_origin(nmedia_http *r,const char *document_url) {
    enum http_url_result result=browser_origin(r->url,document_url,&r->origin,&r->cross);
    if(result!=HTTP_URL_TUPLE){
        snprintf(r->error,sizeof r->error,"%s",result==HTTP_URL_OOM?
            "media origin allocation failed":"invalid, opaque or mixed-content native media origin");
        return false;
    }
    r->browser=true;return true;
}
static void transfer_free(struct transfer *t) {
    nmedia_ff_free(t->allow_origin);nmedia_ff_free(t->allow_methods);nmedia_ff_free(t->allow_headers);
    t->allow_origin=t->allow_methods=t->allow_headers=NULL;
    nmedia_ff_free(t->location);t->location=NULL;
}
/* Complete field bytes, never a security-sensitive prefix. The transport
 * rejects controls; the span guard also prevents an embedded NUL alias. */
static int cors_field(struct transfer *t,const char *value,size_t length,
                      char **out,bool *seen,const char *invalid) {
    if(*seen||length==SIZE_MAX||memchr(value,0,length))return rejected(t,invalid);
    char *owned=nmedia_ff_malloc(length+1);
    if(!owned)return rejected(t,"media CORS header allocation failed");
    memcpy(owned,value,length);owned[length]=0;*out=owned;*seen=true;return 0;
}
/* The variable document origin can exceed the old stack header arrays. Keep
 * exact request storage alive through the synchronous HTTP call only. */
static char *request_headers(nmedia_http *r,const char *fields) {
    size_t field_size=strlen(fields),origin_size=r->browser?strlen(r->origin):0;
    size_t framing=r->browser?sizeof "Origin: \r\n"-1:0;
    if(origin_size>SIZE_MAX-framing-1||field_size>SIZE_MAX-origin_size-framing-1){
        snprintf(r->error,sizeof r->error,"media request header size overflow");return NULL;
    }
    size_t length=origin_size+framing+field_size;
    char *owned=nmedia_ff_malloc(length+1);
    if(!owned){snprintf(r->error,sizeof r->error,"media request header allocation failed");return NULL;}
    char *at=owned;
    if(r->browser){memcpy(at,"Origin: ",8);at+=8;memcpy(at,r->origin,origin_size);at+=origin_size;memcpy(at,"\r\n",2);at+=2;}
    memcpy(at,fields,field_size+1);return owned;
}
static int cors(struct transfer *t) {
    if(t->reader->cross&&(!t->have_origin||
       (strcmp(t->allow_origin,"*")&&strcmp(t->allow_origin,t->reader->origin))))
        return rejected(t,"anonymous media CORS response denied");
    return 0;
}
/* RFC 3986 dot-segment removal, without browser whitespace repair or path
 * truncation. Only the path is normalized; the query remains byte-for-byte. */
static void remove_dots(char *path) {
    char *query=strchr(path,'?');
    size_t length=query?(size_t)(query-path):strlen(path),query_length=query?strlen(query):0;
    size_t used=0,at=0;
    while(at<length){
        size_t left=length-at;const char *in=path+at;
        if(left>=3&&!memcmp(in,"../",3))at+=3;
        else if(left>=2&&!memcmp(in,"./",2))at+=2;
        else if(left>=3&&!memcmp(in,"/./",3))at+=2;
        else if(left==2&&!memcmp(in,"/.",2)){at+=2;path[used++]='/';}
        else if((left>=4&&!memcmp(in,"/../",4))||(left==3&&!memcmp(in,"/..",3))){
            at+=3;while(used&&path[used-1]!='/')used--;if(used)used--;
            if(at==length)path[used++]='/';
        }else if((left==1&&in[0]=='.')||(left==2&&!memcmp(in,"..",2)))at=length;
        else {
            size_t end=at+1;while(end<length&&path[end]!='/')end++;
            size_t take=end-at;memmove(path+used,path+at,take);used+=take;at=end;
        }
    }
    /* Every path operation is deletion-only. Move the original query after
     * the compacted path; it is never normalized or interpreted as segments. */
    if(query_length)memmove(path+used,query,query_length);
    path[used+query_length]=0;
}
enum http_url_result nmedia_http_resolve(const char *base,const char *location,char **out) {
    *out=NULL;struct url_owned before={0},after={0};char *candidate=NULL;
    enum http_url_result result=valid_url(base,&before);
    if(result!=HTTP_URL_TUPLE)goto done;
    if(!location||!location[0]){result=HTTP_URL_INVALID;goto done;}
    size_t supplied=strlen(location);
    if(!strncmp(location,"http://",7)||!strncmp(location,"https://",8))
        candidate=url_join(NULL,0,NULL,0,location,supplied);
    else if(!strncmp(location,"//",2)){
        const char *scheme=before.tls?"https:":"http:";
        candidate=url_join(scheme,strlen(scheme),NULL,0,location,supplied);
    }else {
        const char *colon=strchr(location,':');
        if(colon&&colon<location+strcspn(location,"/?")){result=HTTP_URL_INVALID;goto done;}
        const char *authority=strstr(base,"://")+3;
        size_t origin=(size_t)(authority+strcspn(authority,"/?")-base);
        size_t path_size=strcspn(before.path,"?");
        if(location[0]=='/')path_size=0;
        else if(location[0]!='?')while(path_size&&before.path[path_size-1]!='/')path_size--;
        candidate=url_join(base,origin,before.path,path_size,location,supplied);
    }
    if(!candidate){result=HTTP_URL_OOM;goto done;}
    result=valid_url(candidate,&after);
    if(result!=HTTP_URL_TUPLE)goto done;
    if(before.tls&&!after.tls){result=HTTP_URL_INVALID;goto done;}
    remove_dots(after.path);
    const char *authority=strstr(candidate,"://")+3;
    size_t origin=(size_t)(authority+strcspn(authority,"/?")-candidate);
    *out=url_join(candidate,origin,NULL,0,after.path,strlen(after.path));
    if(!*out)result=HTTP_URL_OOM;
done:
    nmedia_ff_free(candidate);url_owned_free(&before);url_owned_free(&after);return result;
}
static int header(void *opaque, const char *line, size_t size) {
    struct transfer *t = opaque;
    const char *colon = memchr(line, ':', size);
    if (!colon) return rejected(t, "invalid media response header");
    size_t name = (size_t)(colon - line);
    const char *value = colon + 1, *end = line + size;
    while (value < end && (*value == ' ' || *value == '\t')) value++;
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
    size_t n = (size_t)(end - value);
    char small[160];
#define FIELD(s) (name == sizeof(s)-1 && !strncasecmp(line, s, name))
    if (FIELD("Content-Encoding")) {
        if (t->have_encoding || n != 8 || strncasecmp(value, "identity", n))
            return rejected(t, "media requires identity Content-Encoding");
        t->have_encoding = true;
    } else if (t->reader->browser && FIELD("Access-Control-Allow-Origin")) {
        if(!n)return rejected(t,"ambiguous media CORS origin");
        return cors_field(t,value,n,&t->allow_origin,&t->have_origin,"ambiguous media CORS origin");
    } else if (t->reader->browser && FIELD("Access-Control-Allow-Methods")) {
        return cors_field(t,value,n,&t->allow_methods,&t->have_methods,"ambiguous media CORS methods");
    } else if (t->reader->browser && FIELD("Access-Control-Allow-Headers")) {
        return cors_field(t,value,n,&t->allow_headers,&t->have_headers,"ambiguous media CORS headers");
    } else if (FIELD("Content-Type")) {
        if (t->have_type || (n >= 10 && !strncasecmp(value, "multipart/", 10)))
            return rejected(t, "multipart/ambiguous media response rejected");
        t->have_type = true;
    } else if (FIELD("Content-Range")) {
        if (t->have_range || n >= sizeof small) return rejected(t, "ambiguous Content-Range");
        memcpy(small, value, n); small[n] = 0;
        if (!content_range(small, &t->range_first, &t->range_last, &t->total))
            return rejected(t, "invalid Content-Range");
        t->have_range = true;
    } else if (FIELD("Content-Length")) {
        if (n >= sizeof small) return rejected(t, "invalid media Content-Length");
        memcpy(small, value, n); small[n] = 0; const char *p = small; int64_t length;
        if (!decimal(&p, &length) || *p || (t->have_length && length != t->length))
            return rejected(t, "invalid media Content-Length");
        t->length = length; t->have_length = true;
    } else if (FIELD("ETag")) {
        if (t->have_etag || n >= sizeof t->etag) return rejected(t, "ambiguous/oversized media ETag");
        memcpy(t->etag, value, n); t->etag[n] = 0; t->have_etag = true;
    } else if (FIELD("Date") || FIELD("Last-Modified")) {
        bool modified=FIELD("Last-Modified");
        bool *seen=modified?&t->have_modified:&t->have_date;
        char *field=modified?t->modified:t->date;
        if(*seen)return rejected(t,"ambiguous media HTTP date");
        *seen=true;
        /* An unsupported date spelling is not a usable strong validator, but
         * does not invalidate an otherwise valid ETag or a complete GET. */
        if(n==29){memcpy(field,value,n);field[n]=0;}
    } else if (FIELD("Location")) {
        if(t->location||!n||n==SIZE_MAX||memchr(value,0,n))return rejected(t,"invalid media redirect");
        t->location=nmedia_ff_malloc(n+1);
        if(!t->location)return rejected(t,"media redirect header allocation failed");
        memcpy(t->location,value,n);t->location[n]=0;
    }
#undef FIELD
    return 0;
}
static bool redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
static int check_response(struct transfer *t) {
    nmedia_http *r = t->reader;
    if(cors(t)<0)return -1; /* before any bytes enter the decoder's cache */
    if (t->response->status == 206) {
        if (!t->have_range || t->range_first != t->first || t->range_last > t->last ||
            t->range_last != (t->total-1 < t->last ? t->total-1 : t->last))
            return rejected(t, "Range response does not match requested bytes");
        t->expected = (size_t)(t->range_last - t->range_first + 1);
        if ((t->have_length && t->length != (int64_t)t->expected) ||
            (r->initialized && (t->total != r->size ||
                (r->use_modified ? strcmp(t->modified,r->modified) : strcmp(t->etag,r->etag)))))
            return rejected(t, "media representation changed or has wrong length");
        if (t->total > NMEDIA_HTTP_CACHE_BYTES && !strong_tag(t->etag) && !strong_modified(t)) {
            r->need_complete=!r->initialized&&!t->first;
            return rejected(t, "large Range media requires a strong validator or bounded complete download");
        }
    } else if (t->response->status == 200) {
        if(!r->initialized&&!t->first&&!t->have_range&&t->have_length&&t->length>NMEDIA_HTTP_CACHE_BYTES)r->need_complete=true;
        if (r->initialized || t->first || t->have_range || !t->have_length ||
            t->length <= 0 || t->length > NMEDIA_HTTP_CACHE_BYTES)
            return rejected(t, "server ignored Range; unbounded download refused");
        t->total = t->length; t->expected = (size_t)t->length;
    } else return rejected(t, "media server did not return 200/206");
    if (!t->expected || t->expected > NMEDIA_HTTP_CACHE_BYTES) return rejected(t, "media cache limit");
    t->checked = true;
    return 0;
}
static int body(void *opaque, const char *bytes, size_t size) {
    struct transfer *t = opaque;
    if (redirect_status(t->response->status)) { t->discard = true; return -1; }
    if (!t->checked && check_response(t) < 0) return -1;
    if (size > t->expected - t->got) return rejected(t, "Range response body exceeds advertised bytes");
    memcpy(t->reader->cache + t->got, bytes, size); t->got += size;
    return 0;
}
static bool refill(nmedia_http *r, int64_t at) {
    r->cache_len = 0;
    /* Cross-origin refills send only a safelisted single Range. Each response
     * must still match the pinned strong validator and total before body bytes
     * enter the cache. Same-origin refills additionally send If-Range. */
    const char *validator=r->use_modified?r->modified:r->etag;
    bool conditional=!r->cross&&*validator;
    for (unsigned redirects = 0; redirects <= 5; redirects++) {
        struct http_resp response;
        struct transfer t = { .reader = r, .response = &response, .first = at,
            .last = at > INT64_MAX - NMEDIA_HTTP_CACHE_BYTES ? INT64_MAX : at + NMEDIA_HTTP_CACHE_BYTES - 1 };
        char fields[320]; /* two int64s plus the existing exact validator */
        int n = snprintf(fields, sizeof fields, "Range: bytes=%lld-%lld\r\nAccept-Encoding: identity\r\n%s%s%s",
            (long long)t.first, (long long)t.last, conditional ? "If-Range: " : "", conditional ? validator : "",
            conditional ? "\r\n" : "");
        if (n < 0 || (size_t)n >= sizeof fields) { snprintf(r->error,sizeof r->error,"media request field size overflow"); return false; }
        char *headers=request_headers(r,fields);if(!headers)return false;
        struct http_req request = { .url = r->url, .headers = headers, .timeout_ms = MEDIA_HTTP_TIMEOUT_MS,
            .on_header = header, .on_body = body, .ctx = &t };
        int result = http_request(&request, &response);
        nmedia_ff_free(headers);
        if (redirect_status(response.status) && (result == 0 || t.discard) && !t.error[0]) {
            char *next=NULL;enum http_url_result redirect=HTTP_URL_INVALID;
            if(!r->browser&&!r->initialized&&redirects<5)redirect=nmedia_http_resolve(r->url,t.location,&next);
            http_resp_free(&response);transfer_free(&t);
            if(redirect!=HTTP_URL_TUPLE){snprintf(r->error,sizeof r->error,"%s",redirect==HTTP_URL_OOM?
                "media redirect allocation failed":"unsafe, changed or excessive media redirect");return false;}
            nmedia_ff_free(r->url);r->url=next;continue;
        }
        if (result == 0 && !t.checked) result = check_response(&t);
        bool ok = result == 0 && t.checked && t.got == t.expected;
        if (ok) {
            r->size = t.total; r->cache_start = at; r->cache_len = t.got;
            if (!r->initialized) {
                if(strong_tag(t.etag))memcpy(r->etag,t.etag,strlen(t.etag)+1);
                else if(strong_modified(&t)){memcpy(r->modified,t.modified,strlen(t.modified)+1);r->use_modified=true;}
            }
            r->initialized = true;
        } else snprintf(r->error,sizeof r->error,"%s",t.error[0] ? t.error : "media HTTP transfer failed or cut short");
        http_resp_free(&response);transfer_free(&t);return ok;
    }
    return false;
}
/* A complete response has one representation: no validator is required to
 * join bytes from separate requests. This is used for manifests and bounded
 * segments, and as a small-file fallback for servers without validators. */
struct download { struct transfer t; uint8_t *bytes; size_t maximum, capacity; };
static int check_download(struct download *d) {
    struct transfer *t=&d->t;
    if(cors(t)<0)return -1;
    if(t->response->status!=200||t->have_range)return rejected(t,"complete media GET requires 200 without Content-Range");
    if(t->have_length&&(uint64_t)t->length>d->maximum)return rejected(t,"complete media GET exceeds the download bound");
    t->checked=true;return 0;
}
static int download_body(void *opaque,const char *bytes,size_t size) {
    struct download *d=opaque;struct transfer *t=&d->t;
    if(redirect_status(t->response->status)){t->discard=true;return -1;}
    if(!t->checked&&check_download(d)<0)return -1;
    if(size>d->maximum-t->got||(t->have_length&&size>(size_t)t->length-t->got))return rejected(t,"complete media GET body exceeds its bound");
    size_t needed=t->got+size;
    if(needed>d->capacity){size_t capacity=d->capacity?d->capacity:(d->maximum<4096?d->maximum:4096);
        while(capacity<needed)capacity=capacity>d->maximum/2?d->maximum:capacity*2;
        uint8_t *next=nmedia_ff_realloc(d->bytes,capacity);
        if(!next)return rejected(t,"complete media GET allocation failed");
        d->bytes=next;d->capacity=capacity;
    }
    if(size)memcpy(d->bytes+t->got,bytes,size);t->got=needed;return 0;
}
bool nmedia_http_get_bounded_cors(const char *url,const char *document_url,size_t maximum,
    uint8_t **bytes,size_t *length,char *error,size_t error_size) {
    if(bytes)*bytes=NULL;if(length)*length=0;
    enum http_url_result valid=valid_url_text(url);
    if(!bytes||!length||!maximum||maximum>NMEDIA_HTTP_COMPLETE_BYTES||valid!=HTTP_URL_TUPLE){
        if(error&&error_size)snprintf(error,error_size,"%s",valid==HTTP_URL_OOM?
            "complete media URL allocation failed":"invalid or unbounded complete media URL");
        return false;
    }
    nmedia_http *r=nmedia_ff_mallocz(sizeof *r);
    if(!r){if(error&&error_size)snprintf(error,error_size,"complete media GET allocation failed");return false;}
    r->url=url_join(NULL,0,NULL,0,url,strlen(url));
    if(!r->url){if(error&&error_size)snprintf(error,error_size,"media URL storage allocation failed");nmedia_http_close(r);return false;}
    if(document_url&&!reader_origin(r,document_url)){
        if(error&&error_size)snprintf(error,error_size,"%s",r->error);
        nmedia_http_close(r);return false;
    }
    bool ok=false;
    for(unsigned redirects=0;redirects<=5;redirects++){
        struct http_resp response;
        struct download d={.t={.reader=r,.response=&response},.maximum=maximum};
        char *headers=request_headers(r,"Accept-Encoding: identity\r\n");
        if(!headers)break;
        struct http_req request={.url=r->url,.headers=headers,.timeout_ms=MEDIA_HTTP_TIMEOUT_MS,
            .on_header=header,.on_body=download_body,.ctx=&d};
        int result=http_request(&request,&response);
        nmedia_ff_free(headers);
        if(redirect_status(response.status)&&(result==0||d.t.discard)&&!d.t.error[0]){
            char *next=NULL;enum http_url_result redirect=HTTP_URL_INVALID;
            if(!r->browser&&redirects<5)redirect=nmedia_http_resolve(r->url,d.t.location,&next);
            http_resp_free(&response);transfer_free(&d.t);nmedia_ff_free(d.bytes);
            if(redirect!=HTTP_URL_TUPLE){snprintf(r->error,sizeof r->error,"%s",redirect==HTTP_URL_OOM?
                "complete media redirect allocation failed":"unsafe or excessive complete media redirect");break;}
            nmedia_ff_free(r->url);r->url=next;continue;
        }
        if(result==0&&!d.t.checked)result=check_download(&d);
        ok=result==0&&d.t.checked&&(!d.t.have_length||d.t.got==(size_t)d.t.length);
        if(ok){*bytes=d.bytes;*length=d.t.got;}
        else {snprintf(r->error,sizeof r->error,"%s",d.t.error[0]?d.t.error:"complete media GET failed or cut short");nmedia_ff_free(d.bytes);}
        http_resp_free(&response);transfer_free(&d.t);break;
    }
    if(error&&error_size)snprintf(error,error_size,"%s",ok?"":r->error[0]?r->error:"complete media request failed");
    nmedia_http_close(r);return ok;
}

static nmedia_http *open_reader(const char *url,const char *document_url,char *error,size_t error_size) {
    enum http_url_result valid=valid_url_text(url);
    if (valid!=HTTP_URL_TUPLE) {
        if(error&&error_size)snprintf(error,error_size,"%s",valid==HTTP_URL_OOM?
            "media URL allocation failed":"invalid native media URL");
        return NULL;
    }
    nmedia_http *r = nmedia_ff_mallocz(sizeof *r);
    if (!r) { if(error&&error_size)snprintf(error,error_size,"media HTTP cache allocation");return NULL; }
    r->url=url_join(NULL,0,NULL,0,url,strlen(url));
    if(!r->url){if(error&&error_size)snprintf(error,error_size,"media URL storage allocation failed");nmedia_http_close(r);return NULL;}
    if(document_url&&!reader_origin(r,document_url)) {
        if(error&&error_size)snprintf(error,error_size,"%s",r->error);
        nmedia_http_close(r);return NULL;
    }
    if (!refill(r,0)) {
        size_t length=0;
        if(!r->need_complete||!nmedia_http_get_bounded_cors(url,document_url,NMEDIA_HTTP_COMPLETE_BYTES,&r->complete,&length,r->error,sizeof r->error)||!length){
            if(error&&error_size)snprintf(error,error_size,"%s",r->error);nmedia_http_close(r);return NULL;
        }
        r->size=(int64_t)length;r->position=0;r->initialized=true;r->error[0]=0;
    }
    if(error&&error_size)*error=0;
    return r;
}
nmedia_http *nmedia_http_open(const char *url,char *error,size_t error_size) {
    return open_reader(url,NULL,error,error_size);
}
nmedia_http *nmedia_http_open_cors(const char *url,const char *document_url,char *error,size_t error_size) {
    if(!document_url){if(error&&error_size)snprintf(error,error_size,"native document origin required");return NULL;}
    return open_reader(url,document_url,error,error_size);
}
int nmedia_http_read(nmedia_http *r, void *out, int count) {
    if (!r || !out || count <= 0 || r->error[0]) return -1;
    if (r->position == r->size) return 0;
    if(r->complete){size_t take=(size_t)(r->size-r->position);if(take>(size_t)count)take=(size_t)count;
        memcpy(out,r->complete+r->position,take);r->position+=(int64_t)take;return (int)take;}
    if (r->position < r->cache_start || r->position-r->cache_start >= (int64_t)r->cache_len) {
        int64_t at = r->position / NMEDIA_HTTP_CACHE_BYTES * NMEDIA_HTTP_CACHE_BYTES;
        if (!refill(r,at)) return -1;
    }
    size_t offset = (size_t)(r->position-r->cache_start), take = r->cache_len-offset;
    if (take > (size_t)count) take = (size_t)count;
    memcpy(out,r->cache+offset,take);r->position+=(int64_t)take;return (int)take;
}
int64_t nmedia_http_seek(nmedia_http *r, int64_t offset, int whence) {
    if (!r || r->error[0]) return -1;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? r->position : whence == SEEK_END ? r->size : -1;
    if (base < 0 || offset < -base || offset > r->size-base) return -1;
    r->position=base+offset;return r->position;
}
int64_t nmedia_http_size(const nmedia_http *r) { return r ? r->size : -1; }
const char *nmedia_http_error(const nmedia_http *r) { return r ? r->error : "no HTTP media reader"; }
void nmedia_http_close(nmedia_http *r) {
    if(r){free(r->origin);nmedia_ff_free(r->url);nmedia_ff_free(r->complete);}nmedia_ff_free(r);
}
