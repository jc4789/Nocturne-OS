/* Anonymous Range input over Nocturne's verified TLS / HTTP client.
 * One 256 KiB cache, private media quota, no FF network protocols or global
 * cookies. Refills are synchronous: this is not asynchronous streaming.
 * RFC 9110 sections 13.1.5 and 14: never splice different representations. */
#include "media_http_private.h"
#include "media_alloc_private.h"
#include "http.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define MEDIA_HTTP_TIMEOUT_MS 5000
struct nmedia_http {
    char url[NMEDIA_HTTP_URL_BYTES], etag[160], error[160];
    char origin[160];
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
    bool checked, discard;
    bool have_origin, have_methods, have_headers;
    char allow_origin[160], allow_methods[160], allow_headers[256];
    char etag[160], location[NMEDIA_HTTP_URL_BYTES], error[160];
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
static bool strong_tag(const char *p) {
    size_t n = strlen(p);
    if (n < 2 || p[0] != '"' || p[n-1] != '"') return false;
    for (size_t i = 1; i + 1 < n; i++)
        if ((unsigned char)p[i] < 33 || (unsigned char)p[i] > 126 || p[i] == '"') return false;
    return true;
}
/* url_parse itself permits bare hosts and truncates paths. Validate exact
 * spelling first; credentials, fragments, controls and truncation are errors. */
static bool valid_url(const char *s, struct url *parsed) {
    if (!s || strlen(s) >= NMEDIA_HTTP_URL_BYTES) return false;
    const char *authority = !strncmp(s, "https://", 8) ? s + 8 :
                            !strncmp(s, "http://", 7) ? s + 7 : NULL;
    if (!authority) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p <= 32 || *p >= 127 || *p == '#' || *p == '\\') return false;
    const char *end = authority + strcspn(authority, "/?");
    const char *colon = NULL;
    for (const char *p = authority; p < end; p++) {
        if (*p == ':') { if (colon) return false; colon = p; continue; }
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-') continue;
        return false; /* includes userinfo and currently unsupported IPv6 */
    }
    size_t host_size = (size_t)((colon ? colon : end) - authority);
    if (!host_size || host_size >= sizeof parsed->host) return false;
    if (colon) {
        const char *p = colon + 1; int64_t port;
        if (!decimal(&p, &port) || p != end || port < 1 || port > 65535) return false;
    }
    size_t path_size = *end == '?' ? strlen(end) + 1 : *end ? strlen(end) : 1;
    return path_size < sizeof parsed->path && url_parse(s, parsed);
}
/* Serialize a native HTTP document's origin. Fragment/path never grant a new
 * origin; userinfo/opaque URLs fail closed. Host case/default ports normalize. */
static bool origin(const char *url, char out[160], bool *tls) {
    if(!url || strlen(url)>=NMEDIA_HTTP_URL_BYTES)return false;
    for(const unsigned char *p=(const unsigned char *)url;*p;p++)
        if(*p<=32||*p>=127||*p=='\\')return false;
    const char *a=!strncasecmp(url,"https://",8)?url+8:!strncasecmp(url,"http://",7)?url+7:NULL;
    if(!a)return false;
    char raw[160];size_t n=(size_t)(a+strcspn(a,"/?#")-url);
    if(n+2>=sizeof raw)return false;
    memcpy(raw,url,n);raw[n]='/';raw[n+1]=0;
    memcpy(raw,!strncasecmp(url,"https://",8)?"https://":"http://",(size_t)(a-url));
    struct url parsed;if(!valid_url(raw,&parsed))return false;
    for(char *p=parsed.host;*p;p++)if(*p>='A'&&*p<='Z')*p=(char)(*p+'a'-'A');
    int got=parsed.port==(parsed.tls?443:80)?snprintf(out,160,"%s://%s",parsed.tls?"https":"http",parsed.host):
        snprintf(out,160,"%s://%s:%u",parsed.tls?"https":"http",parsed.host,parsed.port);
    if(tls)*tls=parsed.tls;
    return got>0&&got<160;
}
bool nmedia_http_browser_url(const char *url,const char *document_url) {
    struct url parsed;char site[160],target[160];bool tls;
    return valid_url(url,&parsed)&&origin(document_url,site,&tls)&&
        origin(url,target,NULL)&&(!tls||parsed.tls);
}
static int cors(struct transfer *t) {
    if(t->reader->cross&&(!t->have_origin||
       (strcmp(t->allow_origin,"*")&&strcmp(t->allow_origin,t->reader->origin))))
        return rejected(t,"anonymous media CORS response denied");
    return 0;
}
static bool token_list(const char *list,const char *name,bool insensitive) {
    for(const char *p=list;*p;) {
        while(*p==' '||*p=='\t'||*p==',')p++;
        const char *end=p+strcspn(p,","),*trim=end;
        while(trim>p&&(trim[-1]==' '||trim[-1]=='\t'))trim--;
        size_t n=(size_t)(trim-p);
        if((n==1&&*p=='*')||(n==strlen(name)&&
            (insensitive?!strncasecmp(p,name,n):!strncmp(p,name,n))))return true;
        p=*end?end+1:end;
    }
    return false;
}
/* RFC 3986 dot-segment removal, without browser whitespace repair or path
 * truncation. Only the path is normalized; the query remains byte-for-byte. */
static void remove_dots(char *path) {
    char out[1024]; size_t used = 0; const char *in = path;
    while (*in) {
        if (!strncmp(in,"../",3)) in+=3;
        else if (!strncmp(in,"./",2)) in+=2;
        else if (!strncmp(in,"/./",3)) in+=2;
        else if (!strcmp(in,"/.")) in="/";
        else if (!strncmp(in,"/../",4)||!strcmp(in,"/..")) {
            in=in[3]?in+3:"/";
            while(used&&out[used-1]!='/')used--;
            if(used)used--;
        } else if (!strcmp(in,".")||!strcmp(in,"..")) in+=strlen(in);
        else do { out[used++]=*in++; } while(*in&&*in!='/');
    }
    out[used]=0;memcpy(path,out,used+1); /* deletion-only, prevalidated <1024 */
}
static bool redirect_url(const char *base, const char *location, char *out, size_t cap) {
    struct url before, after;
    if (!valid_url(base, &before) || !location[0]) return false;
    int n;
    if (!strncmp(location, "http://", 7) || !strncmp(location, "https://", 8))
        n = snprintf(out, cap, "%s", location);
    else if (!strncmp(location, "//", 2))
        n = snprintf(out, cap, "%s:%s", before.tls ? "https" : "http", location);
    else {
        const char *colon=strchr(location,':');
        if (colon && colon<location+strcspn(location,"/?")) return false;
        const char *start = strstr(base, "://") + 3;
        size_t origin = (size_t)(start + strcspn(start, "/?") - base);
        char path[1024];
        snprintf(path, sizeof path, "%s", before.path);
        char *query = strchr(path, '?'); if (query) *query = 0;
        if (location[0] == '/') n = snprintf(out, cap, "%.*s%s", (int)origin, base, location);
        else if (location[0] == '?') n = snprintf(out, cap, "%.*s%s%s", (int)origin, base, path, location);
        else {
            char *slash = strrchr(path, '/'); if (slash) slash[1] = 0;
            n = snprintf(out, cap, "%.*s%s%s", (int)origin, base, path, location);
        }
    }
    if(n<0||(size_t)n>=cap||!valid_url(out,&after)||(before.tls&&!after.tls))return false;
    char query[1024], normalized[NMEDIA_HTTP_URL_BYTES];
    char *q=strchr(after.path,'?');snprintf(query,sizeof query,"%s",q?q:"");if(q)*q=0;
    remove_dots(after.path);
    const char *authority=strstr(out,"://")+3;
    size_t origin=(size_t)(authority+strcspn(authority,"/?")-out);
    n=snprintf(normalized,sizeof normalized,"%.*s%s%s",(int)origin,out,after.path[0]?after.path:"/",query);
    if(n<0||(size_t)n>=sizeof normalized||(size_t)n>=cap||!valid_url(normalized,&after))return false;
    memcpy(out,normalized,(size_t)n+1);return true;
}
static int header(void *opaque, const char *line, size_t size) {
    struct transfer *t = opaque;
    const char *colon = strchr(line, ':');
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
        if(t->have_origin||!n||n>=sizeof t->allow_origin)return rejected(t,"ambiguous media CORS origin");
        memcpy(t->allow_origin,value,n);t->allow_origin[n]=0;t->have_origin=true;
    } else if (t->reader->browser && FIELD("Access-Control-Allow-Methods")) {
        if(t->have_methods||n>=sizeof t->allow_methods)return rejected(t,"ambiguous media CORS methods");
        memcpy(t->allow_methods,value,n);t->allow_methods[n]=0;t->have_methods=true;
    } else if (t->reader->browser && FIELD("Access-Control-Allow-Headers")) {
        if(t->have_headers||n>=sizeof t->allow_headers)return rejected(t,"ambiguous media CORS headers");
        memcpy(t->allow_headers,value,n);t->allow_headers[n]=0;t->have_headers=true;
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
    } else if (FIELD("Location")) {
        if (t->location[0] || !n || n >= sizeof t->location) return rejected(t, "invalid media redirect");
        memcpy(t->location, value, n); t->location[n] = 0;
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
            (r->initialized && (t->total != r->size || strcmp(t->etag, r->etag))))
            return rejected(t, "media representation changed or has wrong length");
        if (t->total > NMEDIA_HTTP_CACHE_BYTES && (!t->have_etag || !strong_tag(t->etag)))
            return rejected(t, "large Range media requires a strong ETag");
    } else if (t->response->status == 200) {
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
static int discard_body(void *opaque,const char *bytes,size_t size) {
    (void)bytes;(void)size;
    ((struct transfer *)opaque)->discard=true;return -1;
}
static bool preflight(nmedia_http *r) {
    struct http_resp response;
    struct transfer t={.reader=r,.response=&response};
    char headers[512];
    int n=snprintf(headers,sizeof headers,"Origin: %s\r\nAccess-Control-Request-Method: GET\r\n"
        "Access-Control-Request-Headers: %s\r\nAccept-Encoding: identity\r\n",
        r->origin,"if-range");
    if(n<0||(size_t)n>=sizeof headers)return false;
    struct http_req request={.method="OPTIONS",.url=r->url,.headers=headers,
        .timeout_ms=MEDIA_HTTP_TIMEOUT_MS,.on_header=header,.on_body=discard_body,.ctx=&t};
    int result=http_request(&request,&response);
    bool ok=(result==0||t.discard)&&!t.error[0]&&response.status>=200&&response.status<300&&
        cors(&t)==0&&token_list(t.allow_headers,"if-range",true);
    if(!ok)snprintf(r->error,sizeof r->error,"media CORS Range preflight denied");
    http_resp_free(&response);return ok;
}
static bool refill(nmedia_http *r, int64_t at) {
    r->cache_len = 0;
    /* Fetch safelists this single bytes=start-end Range and the GET method.
     * If-Range is not safelisted, so representation-validation refills still
     * need permission for that header. Every response must pass cors() before
     * any bytes enter the cache; this does not waive origin validation. */
    if(r->cross&&r->etag[0]&&!preflight(r))return false;
    for (unsigned redirects = 0; redirects <= 5; redirects++) {
        struct http_resp response;
        struct transfer t = { .reader = r, .response = &response, .first = at,
            .last = at > INT64_MAX - NMEDIA_HTTP_CACHE_BYTES ? INT64_MAX : at + NMEDIA_HTTP_CACHE_BYTES - 1 };
        char headers[768];
        int n = snprintf(headers, sizeof headers, "%s%s%sRange: bytes=%lld-%lld\r\nAccept-Encoding: identity\r\n%s%s%s",
            r->browser?"Origin: ":"",r->browser?r->origin:"",r->browser?"\r\n":"",
            (long long)t.first, (long long)t.last, r->etag[0] ? "If-Range: " : "", r->etag,
            r->etag[0] ? "\r\n" : "");
        if (n < 0 || (size_t)n >= sizeof headers) { snprintf(r->error,sizeof r->error,"media request limit"); return false; }
        struct http_req request = { .url = r->url, .headers = headers, .timeout_ms = MEDIA_HTTP_TIMEOUT_MS,
            .on_header = header, .on_body = body, .ctx = &t };
        int result = http_request(&request, &response);
        if (redirect_status(response.status) && (result == 0 || t.discard) && !t.error[0]) {
            char next[NMEDIA_HTTP_URL_BYTES];
            bool ok = !r->browser && !r->initialized && redirects < 5 && redirect_url(r->url,t.location,next,sizeof next);
            http_resp_free(&response);
            if (!ok) { snprintf(r->error,sizeof r->error,"unsafe, changed or excessive media redirect"); return false; }
            memcpy(r->url,next,strlen(next)+1); continue;
        }
        if (result == 0 && !t.checked) result = check_response(&t);
        bool ok = result == 0 && t.checked && t.got == t.expected;
        if (ok) {
            r->size = t.total; r->cache_start = at; r->cache_len = t.got;
            if (!r->initialized && strong_tag(t.etag)) memcpy(r->etag,t.etag,strlen(t.etag)+1);
            r->initialized = true;
        } else snprintf(r->error,sizeof r->error,"%s",t.error[0] ? t.error : "media HTTP transfer failed or cut short");
        http_resp_free(&response); return ok;
    }
    return false;
}
static nmedia_http *open_reader(const char *url,const char *document_url,char *error,size_t error_size) {
    struct url parsed;
    if (!valid_url(url,&parsed)||(document_url&&!nmedia_http_browser_url(url,document_url))) {
        if(error&&error_size)snprintf(error,error_size,"invalid, opaque or mixed-content media URL");return NULL;
    }
    nmedia_http *r = nmedia_ff_mallocz(sizeof *r);
    if (!r) { if(error&&error_size)snprintf(error,error_size,"media HTTP cache allocation");return NULL; }
    memcpy(r->url,url,strlen(url)+1);
    if(document_url) {
        char target[160];r->browser=true;
        if(!origin(document_url,r->origin,NULL)||!origin(url,target,NULL)) {
            if(error&&error_size)snprintf(error,error_size,"invalid native media origin");
            nmedia_http_close(r);return NULL;
        }
        r->cross=strcmp(r->origin,target)!=0;
    }
    if (!refill(r,0)) { if(error&&error_size)snprintf(error,error_size,"%s",r->error);nmedia_http_close(r);return NULL; }
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
void nmedia_http_close(nmedia_http *r) { nmedia_ff_free(r); }
