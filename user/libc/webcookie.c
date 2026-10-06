/* Cookie policy follows RFC 6265bis section 5 (HTTPWG, September 2026).
   Restrictive choices: 256 cookies / 64 per domain, 256 KiB snapshot, 400-day
   lifetime, no Lax-allowing-unsafe grace, no partitioned-cookie emulation.
   URLs must already use ASCII/A-label hosts, as Nocturne's HTTP worker does. */
#include "webcookie.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define COOKIE_MAX 256
#define DOMAIN_MAX 253
#define COOKIE_PATH_MAX 1024
#define COOKIE_DAYS (400LL * 86400LL)
#define RECORD_SIZE 40u
#define SNAP_HEADER 24u
#define NEVER INT64_MAX
enum { S_LAX, S_STRICT, S_NONE };
enum { F_HOST = 1, F_SECURE = 2, F_HTTP = 4, F_PERSIST = 8 };
struct cookie {
    char *name, *value, *domain, *path;
    uint16_t nl, vl, dl, pl;
    unsigned flags, same;
    int64_t expires;
    uint64_t created, access;
    char data[];
};
struct webcookie_jar {
    struct cookie *cookies[COOKIE_MAX];
    size_t count, bytes;
    uint64_t serial;
    char *psl_text, **rules;
    size_t rule_count;
};
struct address { char host[DOMAIN_MAX + 1], path[2048]; bool secure, ip; };

static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool ascii_host(char *out, const char *src, size_t n) {
    if (!n || n > DOMAIN_MAX) return false;
    size_t label = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = lower((unsigned char)src[i]);
        if (c == '.') {
            if (!label || src[i - 1] == '-') return false;
            label = 0;
        } else {
            if (!(digit(c) || (c >= 'a' && c <= 'z') || c == '-') || (!label && c == '-') || ++label > 63) return false;
        }
        out[i] = (char)c;
    }
    if (!label || src[n-1] == '-') return false;
    out[n] = 0; return true;
}
static bool address(const char *url, struct address *a) {
    if (!url) return false;
    const char *p;
    if (!strncasecmp(url, "https://", 8)) { a->secure = true; p = url + 8; }
    else if (!strncasecmp(url, "http://", 7)) { a->secure = false; p = url + 7; }
    else return false;
    size_t n = strlen(url);
    if (n > 8192) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] >= 127 || url[i] == '\\') return false;
    const char *end = p + strcspn(p, "/?#"), *colon = memchr(p, ':', (size_t)(end - p));
    const char *host_end = colon ? colon : end;
    if (!ascii_host(a->host, p, (size_t)(host_end - p))) return false;
    if (colon) {
        unsigned port = 0; p = colon + 1;
        if (p == end) return false;
        while (p < end) { if (!digit((unsigned char)*p)) return false; port = port * 10 + (unsigned)(*p++ - '0'); if (port > 65535) return false; }
        if (!port) return false;
    }
    a->ip = true;
    for (p = a->host; *p; p++) if (!digit((unsigned char)*p) && *p != '.') a->ip = false;
    size_t pathlen = *end == '/' ? strcspn(end, "?#") : 0;
    if (pathlen >= sizeof a->path) return false;
    if (!pathlen) { strcpy(a->path, "/"); return true; }
    memcpy(a->path, end, pathlen); a->path[pathlen] = 0; return true;
}
static bool domain_match(const char *host, const char *domain) {
    size_t h = strlen(host), d = strlen(domain);
    return h == d ? !strcmp(host, domain) : h > d && host[h-d-1] == '.' && !strcmp(host+h-d, domain);
}
static bool path_match(const char *path, const char *cookie_path) {
    size_t n = strlen(cookie_path), p = strlen(path);
    return p >= n && !memcmp(path, cookie_path, n) && (p == n || cookie_path[n-1] == '/' || path[n] == '/');
}
static int rule_compare(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static bool rule(const webcookie_jar *j, const char *key) {
    size_t lo = 0, hi = j->rule_count;
    while (lo < hi) { size_t mid = lo + (hi-lo)/2; int c = strcmp(key, j->rules[mid]); if (!c) return true; if (c < 0) hi = mid; else lo = mid + 1; }
    return false;
}
static unsigned labels(const char *host) { unsigned n = 1; for (; *host; host++) n += *host == '.'; return n; }
static unsigned suffix_labels(const webcookie_jar *j, const char *host) {
    unsigned best = 1, count = labels(host);
    char key[DOMAIN_MAX + 3];
    for (const char *p = host; p; count--) {
        key[0] = '!'; strcpy(key+1, p);
        if (rule(j, key)) return count - 1;
        if (rule(j, p) && count > best) best = count;
        const char *dot = strchr(p, '.');
        if (dot) { key[0] = '*'; strcpy(key+1, dot); if (rule(j, key) && count > best) best = count; }
        p = dot ? dot+1 : NULL;
    }
    return best;
}
static const char *site_host(const webcookie_jar *j, const struct address *a) {
    if (!j || !j->rule_count || a->ip) return a->host;
    unsigned total = labels(a->host), suffix = suffix_labels(j, a->host);
    if (total <= suffix) return a->host;
    const char *p = a->host;
    for (unsigned i = total; i > suffix + 1; i--) p = strchr(p, '.') + 1;
    return p;
}
bool webcookie_same_site(const webcookie_jar *j, const char *x, const char *y) {
    struct address a, b;
    return address(x, &a) && address(y, &b) && a.secure == b.secure && !strcmp(site_host(j, &a), site_host(j, &b));
}
static bool same_context(const webcookie_jar *j, const struct webcookie_context *c) {
    return !c->redirect_cross_site && (c->site_url ? webcookie_same_site(j, c->url, c->site_url) : c->http && c->top_level);
}
webcookie_jar *webcookie_create(void) { return calloc(1, sizeof(webcookie_jar)); }
void webcookie_free(webcookie_jar *j) {
    if (!j) return;
    for (size_t i = 0; i < j->count; i++) free(j->cookies[i]);
    free(j->rules); free(j->psl_text); free(j);
}
bool webcookie_psl_load(webcookie_jar *j, const char *text, size_t len) {
    if (!j || !text || !len || len > WEBCOOKIE_PSL_MAX || memchr(text, 0, len)) return false;
    char *copy = malloc(len+1); char **rules = malloc(20000 * sizeof *rules);
    if (!copy || !rules) { free(copy); free(rules); return false; }
    memcpy(copy, text, len); copy[len] = 0; size_t count = 0;
    for (char *p = copy; *p;) {
        char *end = strchr(p, '\n'); if (end) *end = 0;
        char *tail = p + strlen(p); while (tail > p && (tail[-1] == '\r' || tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && strncmp(p, "//", 2)) {
            char *host = p + (*p == '!' ? 1 : !strncmp(p, "*.", 2) ? 2 : 0);
            char canonical[DOMAIN_MAX+1];
            if (count == 20000 || !ascii_host(canonical, host, strlen(host))) { free(copy); free(rules); return false; }
            strcpy(host, canonical); rules[count++] = p;
        }
        p = end ? end + 1 : tail;
    }
    if (!count) { free(copy); free(rules); return false; }
    qsort(rules, count, sizeof *rules, rule_compare);
    free(j->rules); free(j->psl_text); j->rules = rules; j->psl_text = copy; j->rule_count = count;
    return true;
}
static size_t record_bytes(const struct cookie *c) { return RECORD_SIZE + c->nl + c->vl + c->dl + c->pl; }
static void remove_cookie(webcookie_jar *j, size_t i) {
    j->bytes -= record_bytes(j->cookies[i]); free(j->cookies[i]);
    memmove(j->cookies+i, j->cookies+i+1, (j->count-i-1) * sizeof *j->cookies); j->count--;
}
static void purge(webcookie_jar *j, int64_t now) {
    for (size_t i = 0; i < j->count;) if (j->cookies[i]->expires <= now) remove_cookie(j, i); else i++;
}
static char *trim(char *s) { while (*s == ' ' || *s == '\t') s++; char *end = s + strlen(s); while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0; return s; }
static bool clean(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127) return false;
    return true;
}
static bool name_ok(const char *s) {
    for (; *s; s++) if ((unsigned char)*s <= 32 || (unsigned char)*s >= 127 || strchr("()<>@,;:\\\"/[]?={}", *s)) return false;
    return true;
}
static bool date_delim(unsigned char c) { return c == 9 || (c >= 32 && c <= 47) || (c >= 59 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126); }
static bool number(const char *p, size_t n, int min, int max, int *out, size_t *used) {
    size_t k = 0; int v = 0;
    while (k < n && digit((unsigned char)p[k]) && k <= (size_t)max) v = v*10 + p[k++] - '0';
    if (k < (size_t)min || k > (size_t)max) return false;
    *out = v; if (used) *used = k; return true;
}
static bool cookie_date(const char *p, int64_t *out) {
    int year = -1, month = -1, day = -1, hour = -1, minute = -1, second = -1;
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    while (*p) {
        while (*p && date_delim((unsigned char)*p)) p++;
        const char *end = p; while (*end && !date_delim((unsigned char)*end)) end++;
        size_t n = (size_t)(end-p), k, l, m; int h, mi, sec, v;
        if (!n) break;
        if (hour < 0 && number(p,n,1,2,&h,&k) && k < n && p[k] == ':' &&
            number(p+k+1,n-k-1,1,2,&mi,&l) && k+l+1 < n && p[k+l+1] == ':' &&
            number(p+k+l+2,n-k-l-2,1,2,&sec,&m)) { hour=h; minute=mi; second=sec; }
        else if (day < 0 && number(p,n,1,2,&v,NULL)) day=v;
        else if (month < 0 && n >= 3) {
            for (int i = 0; i < 12; i++) if (!strncasecmp(p, months+i*3, 3)) { month=i+1; break; }
            if (month < 0 && year < 0 && number(p,n,2,4,&v,NULL)) year=v;
        } else if (year < 0 && number(p,n,2,4,&v,NULL)) year=v;
        p=end;
    }
    if (year >= 0 && year <= 69) year += 2000; else if (year >= 70 && year <= 99) year += 1900;
    if (year < 1601 || month < 1 || day < 1 || hour < 0 || hour > 23 || minute > 59 || second > 59) return false;
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = year%4 == 0 && (year%100 != 0 || year%400 == 0);
    if (day > days[month-1] + (month == 2 && leap)) return false;
    int64_t y = year-1;
    int64_t d = 365*(year-1970LL) + (y/4-1969/4) - (y/100-1969/100) + (y/400-1969/400);
    for (int i=1;i<month;i++) d += days[i-1] + (i == 2 && leap);
    *out = (d+day-1)*86400 + hour*3600 + minute*60 + second; return true;
}
static bool max_age(const char *p, int64_t now, int64_t *expiry) {
    bool neg = *p == '-'; if (neg) p++;
    if (!*p) return false;
    int64_t n = 0;
    for (; *p; p++) { if (!digit((unsigned char)*p)) return false; if (n < COOKIE_DAYS) n = n*10 + *p-'0'; }
    if (neg || !n) *expiry = 0;
    else *expiry = now + (n > COOKIE_DAYS ? COOKIE_DAYS : n);
    return true;
}
static struct cookie *make_cookie(const char *name, const char *value, const char *domain, const char *path) {
    size_t sizes[4] = {strlen(name), strlen(value), strlen(domain), strlen(path)}, total = 4;
    for (int i=0;i<4;i++) total += sizes[i];
    struct cookie *c = calloc(1, sizeof *c + total); if (!c) return NULL;
    char *dst=c->data; char **fields[4] = {&c->name,&c->value,&c->domain,&c->path};
    const char *src[4] = {name,value,domain,path};
    for (int i=0;i<4;i++) { *fields[i]=dst; memcpy(dst,src[i],sizes[i]+1); dst+=sizes[i]+1; }
    c->nl=sizes[0]; c->vl=sizes[1]; c->dl=sizes[2]; c->pl=sizes[3]; return c;
}
int webcookie_set(webcookie_jar *j, const struct webcookie_context *ctx, const char *input, size_t len, int64_t now) {
    if (!j || !ctx || !input || now < 0 || now > NEVER-COOKIE_DAYS || len > WEBCOOKIE_FIELD_MAX) return -1;
    struct address a; if (!address(ctx->url,&a)) return 0;
    if (memchr(input,0,len) || !clean(input,len)) return 0;
    char *copy=malloc(len+1); if (!copy) return -1; memcpy(copy,input,len); copy[len]=0;
    char *attrs=strchr(copy,';'); if (attrs) *attrs++=0;
    char *eq=strchr(copy,'='), *name, *value;
    if (eq) { *eq=0; name=trim(copy); value=trim(eq+1); } else { name=""; value=trim(copy); }
    if ((!*name && !*value) || !name_ok(name) || strlen(name)+strlen(value)>4096) { free(copy); return 0; }
    char domain[DOMAIN_MAX+1], path[COOKIE_PATH_MAX+1], default_path[COOKIE_PATH_MAX+1]; strcpy(domain,a.host);
    size_t pl=strlen(a.path); while (pl>1 && a.path[pl-1]!='/') pl--; if (pl>1) pl--;
    if (pl > COOKIE_PATH_MAX) { free(copy); return 0; } memcpy(path,a.path,pl); path[pl]=0; strcpy(default_path,path);
    bool domain_attr=false, path_root=false, secure=false, http_only=false, age=false, persistent=false, bad=false, partitioned=false;
    unsigned same=S_LAX; int64_t expires=NEVER, age_time=NEVER;
    for (char *p=attrs;p;) {
        char *next=strchr(p,';'); if (next) *next++=0;
        char *v=strchr(p,'='); if (v) *v++=0; else v=p+strlen(p);
        p=trim(p); v=trim(v);
        if (!strcasecmp(p,"Domain")) {
            if (*v) { if (*v=='.') v++; domain_attr=true; if (!ascii_host(domain,v,strlen(v))) bad=true; }
        } else if (!strcasecmp(p,"Path")) {
            size_t n=strlen(v); if (n<=COOKIE_PATH_MAX) { if (*v=='/') memcpy(path,v,n+1); else strcpy(path,default_path); path_root=n==1&&*v=='/'; }
        } else if (!strcasecmp(p,"Secure")) secure=true;
        else if (!strcasecmp(p,"HttpOnly")) http_only=true;
        else if (!strcasecmp(p,"Partitioned")) partitioned=true;
        else if (!strcasecmp(p,"SameSite")) same=!strcasecmp(v,"None") ? S_NONE : !strcasecmp(v,"Strict") ? S_STRICT : S_LAX;
        else if (!strcasecmp(p,"Max-Age")) { int64_t t; if (max_age(v,now,&t)) { age=true; age_time=t; } }
        else if (!strcasecmp(p,"Expires")) { int64_t t; if (cookie_date(v,&t)) { persistent=true; expires=t; } }
        p=next;
    }
    bool host_only=!domain_attr;
    if (domain_attr) {
        if (!domain_match(a.host,domain) || (a.ip && strcmp(a.host,domain))) bad=true;
        else if (!j->rule_count || a.ip || suffix_labels(j,domain)==labels(domain)) {
            if (strcmp(a.host,domain)) bad=true; else host_only=true;
        }
    }
    if (secure && !a.secure) bad=true;
    if (http_only && !ctx->http) bad=true;
    if (same==S_NONE && !secure) bad=true;
    if (same!=S_NONE && !same_context(j,ctx) && !(ctx->http && ctx->top_level)) bad=true;
    if (partitioned) bad=true; /* No partition key exists in this browser yet. */
    const char *prefix=*name ? name : value;
    if ((!strncasecmp(prefix,"__Secure-",9) || !strncasecmp(prefix,"__Host-",7)) && !secure) bad=true;
    if (!strncasecmp(prefix,"__Host-",7) && (domain_attr || !path_root)) bad=true;
    /* Also enforce the newer HTTP-only prefixes; never weaken them to JS cookies. */
    if ((!strncasecmp(prefix,"__Http-",7) || !strncasecmp(prefix,"__Host-Http-",12)) && (!secure || !http_only || !ctx->http)) bad=true;
    if (!*name && (!strncasecmp(value,"__Secure-",9) || !strncasecmp(value,"__Host-",7) || !strncasecmp(value,"__Http-",7))) bad=true;
    purge(j,now); size_t old=j->count;
    for (size_t i=0;i<j->count;i++) {
        struct cookie *c=j->cookies[i];
        if (strcmp(c->name,name)) continue;
        if (!a.secure && (c->flags&F_SECURE) && (domain_match(c->domain,domain) || domain_match(domain,c->domain)) && path_match(path,c->path)) bad=true;
        if (!strcmp(c->domain,domain) && !strcmp(c->path,path) && !!(c->flags&F_HOST)==host_only) {
            old=i; if (!ctx->http && (c->flags&F_HTTP)) bad=true;
        }
    }
    if (bad) { free(copy); return 0; }
    if (age) { expires=age_time; persistent=true; }
    if (persistent && expires>now+COOKIE_DAYS) expires=now+COOKIE_DAYS;
    if (expires<=now) { if (old<j->count) remove_cookie(j,old); free(copy); return 1; }
    struct cookie *c=make_cookie(name,value,domain,path); free(copy); if (!c) return -1;
    c->flags=(host_only?F_HOST:0)|(secure?F_SECURE:0)|(http_only?F_HTTP:0)|(persistent?F_PERSIST:0); c->same=same; c->expires=expires;
    c->created=old<j->count ? j->cookies[old]->created : ++j->serial; c->access=++j->serial;
    if (old<j->count) remove_cookie(j,old);
    size_t domain_count=0; for (size_t i=0;i<j->count;i++) domain_count+=!strcmp(j->cookies[i]->domain,c->domain);
    while (domain_count>=64 || j->count>=COOKIE_MAX || j->bytes+record_bytes(c)+SNAP_HEADER>WEBCOOKIE_SNAPSHOT_MAX) {
        size_t victim=j->count;
        for (size_t i=0;i<j->count;i++) {
            struct cookie *candidate=j->cookies[i];
            if (domain_count>=64 && strcmp(candidate->domain,c->domain)) continue;
            if (victim==j->count || ((j->cookies[victim]->flags&F_SECURE) && !(candidate->flags&F_SECURE)) ||
                (!!(candidate->flags&F_SECURE)==!!(j->cookies[victim]->flags&F_SECURE) && candidate->access<j->cookies[victim]->access)) victim=i;
        }
        if (victim==j->count) { free(c); return -1; }
        if (!strcmp(j->cookies[victim]->domain,c->domain)) domain_count--;
        remove_cookie(j,victim);
    }
    j->cookies[j->count++]=c; j->bytes+=record_bytes(c); return 1;
}
static int send_order(const void *a,const void *b) {
    const struct cookie *x=*(const struct cookie *const *)a,*y=*(const struct cookie *const *)b;
    if (x->pl!=y->pl) return x->pl>y->pl?-1:1;
    return x->created<y->created?-1:x->created>y->created?1:0;
}
long webcookie_get(webcookie_jar *j,const struct webcookie_context *ctx,char *out,size_t cap,int64_t now) {
    if (out && cap) out[0]=0;
    struct address a; if (!j || !ctx || (out&&!cap) || !address(ctx->url,&a)) return -1;
    purge(j,now); struct cookie *list[COOKIE_MAX]; size_t count=0,len=0;
    bool same=same_context(j,ctx);
    const char *method=ctx->method?ctx->method:"GET";
    bool safe=!strcmp(method,"GET")||!strcmp(method,"HEAD")||!strcmp(method,"OPTIONS")||!strcmp(method,"TRACE");
    for (size_t i=0;i<j->count;i++) {
        struct cookie *c=j->cookies[i];
        if ((c->flags&F_HOST) ? strcmp(a.host,c->domain)!=0 : !domain_match(a.host,c->domain)) continue;
        if (!path_match(a.path,c->path) || ((c->flags&F_SECURE)&&!a.secure) || ((c->flags&F_HTTP)&&!ctx->http)) continue;
        if (!same && c->same!=S_NONE && !(c->same==S_LAX && ctx->http && ctx->top_level && safe)) continue;
        list[count++]=c; len+=c->nl+c->vl+(c->nl?1:0)+(count>1?2:0);
    }
    if (len>WEBCOOKIE_OUTPUT_MAX) return -1;
    if (!out) return (long)len;
    if (len>=cap) return -1;
    qsort(list,count,sizeof *list,send_order); size_t pos=0;
    for (size_t i=0;i<count;i++) {
        struct cookie *c=list[i]; if (i) { out[pos++]=';';out[pos++]=' '; }
        if (c->nl) { memcpy(out+pos,c->name,c->nl);pos+=c->nl;out[pos++]='='; }
        memcpy(out+pos,c->value,c->vl);pos+=c->vl;c->access=++j->serial;
    }
    out[pos]=0;return (long)pos;
}

static void put16(unsigned char *p,unsigned n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static unsigned get16(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static void put64(unsigned char *p,uint64_t n){for(int i=0;i<8;i++)p[i]=(unsigned char)(n>>(i*8));}
static uint64_t get64(const unsigned char *p){uint64_t n=0;for(int i=0;i<8;i++)n|=(uint64_t)p[i]<<(i*8);return n;}
long webcookie_export(webcookie_jar *j,void *output,size_t cap,int64_t now){
    if(!j)return -1;purge(j,now);size_t n=SNAP_HEADER+j->bytes;if(!output)return (long)n;if(cap<n)return -1;
    unsigned char *p=output;memset(p,0,SNAP_HEADER);memcpy(p,"NCK1",4);put16(p+4,(unsigned)j->count);put64(p+8,j->serial);p+=SNAP_HEADER;
    for(size_t i=0;i<j->count;i++){
        struct cookie *c=j->cookies[i];memset(p,0,RECORD_SIZE);put16(p,c->nl);put16(p+2,c->vl);put16(p+4,c->dl);put16(p+6,c->pl);
        p[8]=(unsigned char)c->flags;p[9]=(unsigned char)c->same;put64(p+16,(uint64_t)c->expires);put64(p+24,c->created);put64(p+32,c->access);p+=RECORD_SIZE;
        memcpy(p,c->name,c->nl);p+=c->nl;memcpy(p,c->value,c->vl);p+=c->vl;memcpy(p,c->domain,c->dl);p+=c->dl;memcpy(p,c->path,c->pl);p+=c->pl;
    }return (long)n;
}
bool webcookie_import(webcookie_jar *j,const void *input,size_t len,int64_t now){
    if(!j||j->count||!input||len<SNAP_HEADER||len>WEBCOOKIE_SNAPSHOT_MAX)return false;
    const unsigned char *p=input,*end=p+len;if(memcmp(p,"NCK1",4)||get16(p+4)>COOKIE_MAX)return false;
    size_t count=get16(p+4);uint64_t serial=get64(p+8);p+=SNAP_HEADER;webcookie_jar *temp=webcookie_create();if(!temp)return false;
    bool ok=false;
    for(size_t i=0;i<count;i++){
        if((size_t)(end-p)<RECORD_SIZE)goto done;
        unsigned nl=get16(p),vl=get16(p+2),dl=get16(p+4),pl=get16(p+6),flags=p[8],same=p[9];
        int64_t expires=(int64_t)get64(p+16);uint64_t created=get64(p+24),access=get64(p+32);p+=RECORD_SIZE;
        size_t size=(size_t)nl+vl+dl+pl;
        if(nl+vl>4096||!dl||dl>DOMAIN_MAX||!pl||pl>COOKIE_PATH_MAX||size>(size_t)(end-p)||flags>15||same>S_NONE||created>serial||access>serial||expires<0||memchr(p,0,size)||!clean((const char*)p,size))goto done;
        char fields[4096+DOMAIN_MAX+COOKIE_PATH_MAX+4],*f=fields;const char *parts[4];unsigned sizes[4]={nl,vl,dl,pl};
        for(int k=0;k<4;k++){parts[k]=f;memcpy(f,p,sizes[k]);f[sizes[k]]=0;p+=sizes[k];f+=sizes[k]+1;}
        char domain[DOMAIN_MAX+1];if(!name_ok(parts[0])||strchr(parts[1],';')||!ascii_host(domain,parts[2],dl)||strcmp(domain,parts[2])||parts[3][0]!='/')goto done;
        if(!(flags&F_HOST)&&(!j->rule_count||suffix_labels(j,domain)==labels(domain)))goto done;
        for(size_t k=0;k<temp->count;k++){struct cookie *c=temp->cookies[k];if(!strcmp(c->name,parts[0])&&!strcmp(c->domain,parts[2])&&!strcmp(c->path,parts[3])&&!!(c->flags&F_HOST)==!!(flags&F_HOST))goto done;}
        struct cookie *c=make_cookie(parts[0],parts[1],parts[2],parts[3]);if(!c)goto done;
        c->flags=flags;c->same=same;c->expires=expires;c->created=created;c->access=access;temp->cookies[temp->count++]=c;temp->bytes+=record_bytes(c);
    }
    if(p!=end)goto done;
    purge(temp,now);memcpy(j->cookies,temp->cookies,temp->count*sizeof *j->cookies);j->count=temp->count;j->bytes=temp->bytes;j->serial=serial;temp->count=0;ok=true;
done:webcookie_free(temp);return ok;
}
