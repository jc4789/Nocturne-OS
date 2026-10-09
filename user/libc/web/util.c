/* Arenas, string buffers and URL resolution for the web engine. */
#include <stdio.h>
#include "webi.h"

/* ---------------------------------------------------------------- arena */
struct achunk {
    struct achunk *next;
    size_t used, cap;
    char data[];
};

void *ar_alloc(arena_t *a, size_t n) {
    if (n > SIZE_MAX - 7) {
        if (a->trap) longjmp(*a->trap, 1);
        abort();
    }
    n = (n + 7) & ~(size_t)7;
    struct achunk *c = a->head;
    if (!c || n > c->cap - c->used) {
        size_t cap = n > 60000 ? n : 65536 - sizeof(struct achunk);
        bool quota=a->limit && (cap>a->limit || a->allocated>a->limit-cap);
        if (quota || cap > SIZE_MAX - sizeof(struct achunk) || cap > SIZE_MAX - a->allocated) {
            /* Every existing arena trap tests nonzero; retain the failure
               semantics while distinguishing quota from allocator failure. */
            if (a->trap) longjmp(*a->trap, quota ? 2 : 1);
            abort();
        }
        c = malloc(sizeof(struct achunk) + cap);
        if (!c) {
            if (a->trap) longjmp(*a->trap, 3);
            abort();
        }
        a->allocated += cap;
        c->used = 0;
        c->cap = cap;
        c->next = a->head;
        a->head = c;
    }
    void *p = c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

char *ar_strndup(arena_t *a, const char *s, size_t n) {
    if (n == SIZE_MAX) {
        if (a->trap) longjmp(*a->trap, 1);
        abort();
    }
    char *p = ar_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *ar_strdup(arena_t *a, const char *s) { return ar_strndup(a, s, strlen(s)); }

void ar_free(arena_t *a) {
    for (struct achunk *c = a->head, *nx; c; c = nx) {
        nx = c->next;
        free(c);
    }
    a->head = NULL;
    a->allocated = 0;
}

/* ---------------------------------------------------------------- buffers */
static void sb_grow(sbuf *b, size_t need) {
    if (b->n == SIZE_MAX || need > SIZE_MAX - b->n - 1) abort();
    size_t target = b->n + need + 1;
    if (target <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < target) cap = cap > SIZE_MAX / 2 ? target : cap * 2;
    char *p = realloc(b->p, cap);
    if (!p) abort();
    b->p = p;
    b->cap = cap;
}

void sb_put(sbuf *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

void sb_puts(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

void sb_putc(sbuf *b, char c) {
    sb_grow(b, 1);
    b->p[b->n++] = c;
}

int utf8_put(char *o, uint32_t cp) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | cp >> 6);
        o[1] = (char)(0x80 | (cp & 63));
        return 2;
    }
    if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12);
        o[1] = (char)(0x80 | (cp >> 6 & 63));
        o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    if (cp > 0x10FFFF) return utf8_put(o, 0xFFFD);
    o[0] = (char)(0xF0 | cp >> 18);
    o[1] = (char)(0x80 | (cp >> 12 & 63));
    o[2] = (char)(0x80 | (cp >> 6 & 63));
    o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

void sb_utf8(sbuf *b, uint32_t cp) {
    char t[4];
    sb_put(b, t, (size_t)utf8_put(t, cp));
}

char *sb_cstr(sbuf *b) {
    sb_grow(b, 0);
    b->p[b->n] = 0;
    return b->p;
}

void sb_free(sbuf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void pv_push(pvec *p, void *x) {
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        void **v = realloc(p->v, sizeof(void *) * (size_t)cap);
        if (!v) abort();
        p->v = v;
        p->cap = cap;
    }
    p->v[p->n++] = x;
}

void pv_free(pvec *p) {
    free(p->v);
    p->v = NULL;
    p->n = p->cap = 0;
}

bool str_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return false;
    return *a == *b;
}

bool strn_ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++, b++)
        if (!*b || lower((unsigned char)a[i]) != lower((unsigned char)*b)) return false;
    return *b == 0;
}

/* ---------------------------------------------------------------- URLs */
struct url_span { const char *p; size_t n; };
struct urlparts {
    struct url_span scheme, auth, path, query, frag;
    bool has_auth, has_query, has_frag;
};
struct url_buffer { char *p; size_t length, capacity; };

static size_t scheme_len(const char *s) {
    size_t i=0;
    if(!((s[0]>='a'&&s[0]<='z')||(s[0]>='A'&&s[0]<='Z')))return 0;
    while(s[i]&&(((s[i]|32)>='a'&&(s[i]|32)<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='+'||s[i]=='-'||s[i]=='.'))i++;
    return s[i]==':'?i:0;
}
static bool span_is(struct url_span s,const char *text) {
    size_t n=strlen(text);if(s.n!=n)return false;
    for(size_t i=0;i<n;i++)if(lower((unsigned char)s.p[i])!=(unsigned char)text[i])return false;
    return true;
}
static bool span_same(struct url_span a,struct url_span b) {
    if(a.n!=b.n)return false;
    for(size_t i=0;i<a.n;i++)if(lower((unsigned char)a.p[i])!=lower((unsigned char)b.p[i]))return false;
    return true;
}
static void url_split(const char *s,struct urlparts *u) {
    memset(u,0,sizeof *u);size_t sl=scheme_len(s);u->scheme=(struct url_span){s,sl};if(sl)s+=sl+1;
    if(s[0]=='/'&&s[1]=='/'){s+=2;size_t n=strcspn(s,"/?#");u->auth=(struct url_span){s,n};u->has_auth=true;s+=n;}
    size_t n=strcspn(s,"?#");u->path=(struct url_span){s,n};s+=n;
    if(*s=='?'){s++;n=strcspn(s,"#");u->query=(struct url_span){s,n};u->has_query=true;s+=n;}
    if(*s=='#'){s++;u->frag=(struct url_span){s,strlen(s)};u->has_frag=true;}
}
static bool special_scheme(struct url_span s) {
    return span_is(s,"http")||span_is(s,"https")||span_is(s,"file")||span_is(s,"ftp");
}
/* RFC3986 dot removal, without a maximum-sized scratch or recursive walk. */
static char *remove_dots(struct url_span path) {
    if(path.n==SIZE_MAX)return NULL;
    char *out=malloc(path.n+1);if(!out)return NULL;size_t on=0,i=0;
    while(i<path.n){
        const char *p=path.p+i;size_t n=path.n-i;
        if(n>=3&&!memcmp(p,"../",3))i+=3;
        else if(n>=2&&!memcmp(p,"./",2))i+=2;
        else if(n>=3&&!memcmp(p,"/./",3))i+=2;
        else if(n==2&&!memcmp(p,"/.",2)){out[on++]='/';i+=2;}
        else if((n>=4&&!memcmp(p,"/../",4))||(n==3&&!memcmp(p,"/..",3))){
            while(on&&out[on-1]!='/')on--;if(on)on--;
            if(n==3){out[on++]='/';i+=3;}else i+=3;
        }else if((n==1&&p[0]=='.')||(n==2&&!memcmp(p,"..",2)))i=path.n;
        else{do{out[on++]=path.p[i++];}while(i<path.n&&path.p[i]!='/');}
    }
    out[on]=0;return out;
}
static int url_append(struct url_buffer *b,const char *s,size_t n,bool normalize,bool encode,int part) {
    size_t extra=0;
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)s[i];bool escape=encode&&(c<=' '||c>=127||c=='"'||c=='<'||c=='>'||c=='`'||(!part&&(c=='{'||c=='}')));
        size_t bytes=escape?3:1;if(bytes>UINT32_MAX-extra)return 0;extra+=bytes;
    }
    if(b->length>UINT32_MAX||extra>UINT32_MAX-b->length||b->length+extra==SIZE_MAX)return 0;
    size_t wanted=b->length+extra+1;
    if(wanted>b->capacity){
        size_t cap=b->capacity?b->capacity:128;
        while(cap<wanted){if(cap>SIZE_MAX/2){cap=wanted;break;}cap*=2;}
        char *p=realloc(b->p,cap);if(!p)return -1;b->p=p;b->capacity=cap;
    }
    static const char hex[]="0123456789ABCDEF";
    for(size_t i=0;i<n;i++){
        unsigned char c=(unsigned char)s[i];bool escape=encode&&(c<=' '||c>=127||c=='"'||c=='<'||c=='>'||c=='`'||(!part&&(c=='{'||c=='}')));
        if(escape){b->p[b->length++]='%';b->p[b->length++]=hex[c>>4];b->p[b->length++]=hex[c&15];}
        else b->p[b->length++]=normalize?(char)lower(c):(char)c;
    }
    b->p[b->length]=0;return 1;
}
int web_resolve_url_owned(const char *base,const char *rel,char **out) {
    if(!out)return 0;*out=NULL;if(!rel)return 0;
    size_t rn=strlen(rel);if(rn>UINT32_MAX||rn==SIZE_MAX)return 0;
    char *clean=malloc(rn+1),*merged=NULL,*dots=NULL;struct url_buffer assembled={0};int result=0;
    if(!clean)return -1;
    while(is_space((unsigned char)*rel))rel++;
    size_t cn=0;for(const char *p=rel;*p;p++){if(*p=='\t'||*p=='\n'||*p=='\r')continue;clean[cn++]=*p=='\\'?'/':*p;}
    while(cn&&is_space((unsigned char)clean[cn-1]))cn--;clean[cn]=0;
    struct urlparts b,r,t;url_split(clean,&r);
    if(r.scheme.n&&!special_scheme(r.scheme)){*out=clean;return 1;}
    size_t bn=base?strlen(base):0;if(bn>UINT32_MAX||bn==SIZE_MAX)goto done;
    url_split(base?base:"",&b);bool clean_path=false;
    if(r.scheme.n&&(r.has_auth||!span_same(r.scheme,b.scheme))){t=r;clean_path=true;}
    else{
        if(!b.scheme.n)goto done;t=(struct urlparts){0};t.scheme=b.scheme;
        if(r.has_auth){t.auth=r.auth;t.has_auth=true;t.path=r.path;t.query=r.query;t.has_query=r.has_query;clean_path=true;}
        else{
            t.auth=b.auth;t.has_auth=b.has_auth;
            if(!r.path.n){t.path=b.path;t.query=r.has_query?r.query:b.query;t.has_query=r.has_query||b.has_query;}
            else{
                if(r.path.p[0]=='/')t.path=r.path;
                else{
                    size_t keep=b.path.n;while(keep&&b.path.p[keep-1]!='/')keep--;
                    size_t slash=!keep&&b.has_auth;
                    if(keep>UINT32_MAX-r.path.n||slash>UINT32_MAX-keep-r.path.n||keep+r.path.n+slash==SIZE_MAX)goto done;
                    size_t n=keep+r.path.n+slash;merged=malloc(n+1);if(!merged){result=-1;goto done;}
                    if(keep)memcpy(merged,b.path.p,keep);if(slash)merged[keep++]='/';memcpy(merged+keep,r.path.p,r.path.n);merged[n]=0;t.path=(struct url_span){merged,n};
                }
                clean_path=true;t.query=r.query;t.has_query=r.has_query;
            }
        }
        t.frag=r.frag;t.has_frag=r.has_frag;
    }
    if(clean_path){dots=remove_dots(t.path);if(!dots){result=-1;goto done;}t.path=(struct url_span){dots,strlen(dots)};}
    if(!t.path.n&&t.has_auth)t.path=(struct url_span){"/",1};
#define APPEND(p,n,normalize,encode,k) do{result=url_append(&assembled,p,n,normalize,encode,k);if(result!=1)goto done;}while(0)
    APPEND(t.scheme.p,t.scheme.n,true,false,0);APPEND(":",1,false,false,0);
    if(t.has_auth){APPEND("//",2,false,false,0);APPEND(t.auth.p,t.auth.n,true,false,0);}
    struct url_span parts[3]={t.path,t.query,t.frag};bool present[3]={true,t.has_query,t.has_frag};
    for(int k=0;k<3;k++){if(!present[k])continue;if(k)APPEND(k==1?"?":"#",1,false,false,k);APPEND(parts[k].p,parts[k].n,false,true,k);}
#undef APPEND
    if(span_is(t.scheme,"http")||span_is(t.scheme,"https")){
        char *canonical=NULL,*origin=NULL;enum http_url_result parsed=http_canonical_owned_n(assembled.p,assembled.length,&canonical,&origin);
        if(parsed!=HTTP_URL_TUPLE){result=parsed==HTTP_URL_OOM?-1:0;goto done;}
        const char *fragment=strchr(assembled.p,'#');size_t fl=fragment?strlen(fragment):0,cl=strlen(canonical);
        if(fl>UINT32_MAX-cl||cl+fl==SIZE_MAX){free(canonical);free(origin);result=0;goto done;}
        if(fl){char *p=realloc(canonical,cl+fl+1);if(!p){free(canonical);free(origin);result=-1;goto done;}canonical=p;memcpy(canonical+cl,fragment,fl+1);}
        free(origin);*out=canonical;
    }else{*out=assembled.p;assembled.p=NULL;}
    result=1;
done:
    free(assembled.p);free(dots);free(merged);free(clean);return result;
}
bool url_resolve(const char *base,const char *rel,char *out,size_t n) {
    if(!out||!n)return false;char *resolved=NULL;int result=web_resolve_url_owned(base,rel,&resolved);
    size_t length=resolved?strlen(resolved):0;bool ok=result==1&&length<n;
    if(ok)memcpy(out,resolved,length+1);else out[0]=0;free(resolved);return ok;
}
bool web_resolve_url(const char *base,const char *rel,char *out,size_t n){return url_resolve(base,rel,out,n);}

void url_encode_form(sbuf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '*' ||
            *p == '-' || *p == '.' || *p == '_')
            sb_putc(b, (char)*p);
        else if (*p == ' ') sb_putc(b, '+');
        else {
            char hx[4];
            snprintf(hx, sizeof hx, "%%%02X", *p);
            sb_puts(b, hx);
        }
    }
}
