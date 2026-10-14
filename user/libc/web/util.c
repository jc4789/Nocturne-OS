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
        if (a->chunk_size) {
            cap = a->chunk_size > sizeof(struct achunk) ?
                (a->chunk_size - sizeof(struct achunk)) & ~(size_t)7 : 8;
            if (cap < n) cap = n;
        }
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

/* ------------------------------------------------------- Fetch data URLs */
/* Check at native page-sized work boundaries, not once per byte/syscall.
 * This is a scheduling quantum, never a limit on accepted input. */
enum { DATA_URL_CHECKPOINT_BYTES = 4096 };
struct data_work { bool (*check)(void *); void *opaque; size_t remaining; };
static bool data_step(struct data_work *w) {
    if (w->remaining) { w->remaining--; return true; }
    w->remaining=DATA_URL_CHECKPOINT_BYTES-1;
    return !w->check || w->check(w->opaque);
}
static bool data_ascii_space(unsigned char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static bool data_http_space(unsigned char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'; }
static bool data_token(unsigned char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
        (c && strchr("!#$%&'*+-.^_`|~",c)!=NULL);
}
static bool data_quoted(unsigned char c) { return c=='\t'||(c>=0x20&&c!=0x7f); }
static int data_hex_digit(unsigned char c) {
    return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
}
static int data_base64_digit(unsigned char c) {
    return c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;
}
struct data_mime_parameter { char *name,*value; size_t nn,vn,next; };
static size_t data_name_hash(const char *s,size_t n) {
    size_t h=0;for(size_t i=0;i<n;i++)h=h*31u+(unsigned char)s[i];return h;
}
/* MIME Sniffing parse + serialize. Parameters retain insertion order and use
 * a growing hash index for first-wins duplicates, not quadratic rescanning. */
static int data_mime(char *input,size_t length,char **out,struct data_work *work) {
    struct data_mime_parameter *parameters=NULL;
    size_t *buckets=NULL,count=0,capacity=0,bc=0;
    char *p=input,*end=input+length,*type,*subtype,*type_end,*subtype_end;
    int result=-1;*out=NULL;
    while(p<end&&data_http_space((unsigned char)*p))p++;
    while(end>p&&data_http_space((unsigned char)end[-1]))end--;
    type=p;while(p<end&&*p!='/'){if(!data_step(work)){result=-2;goto done;}if(!data_token((unsigned char)*p))goto fallback;*p=(char)lower((unsigned char)*p);p++;}
    type_end=p;if(p==type||p==end)goto fallback;p++;subtype=p;
    while(p<end&&*p!=';'){if(!data_step(work)){result=-2;goto done;}p++;}
    subtype_end=p;while(subtype_end>subtype&&data_http_space((unsigned char)subtype_end[-1]))subtype_end--;
    if(subtype==subtype_end)goto fallback;
    for(char *q=subtype;q<subtype_end;q++){if(!data_step(work)){result=-2;goto done;}if(!data_token((unsigned char)*q))goto fallback;*q=(char)lower((unsigned char)*q);}
    while(p<end){
        p++;while(p<end&&data_http_space((unsigned char)*p))p++;
        char *name=p;bool valid_name=true;
        while(p<end&&*p!=';'&&*p!='='){if(!data_step(work)){result=-2;goto done;}if(!data_token((unsigned char)*p))valid_name=false;*p=(char)lower((unsigned char)*p);p++;}
        size_t nn=(size_t)(p-name);if(p==end)break;if(*p==';')continue;p++;if(p==end)break;
        char *value=p,*ve;bool valid_value=true,quoted=*p=='"';
        if(quoted){
            p++;value=p;char *write=p;
            while(p<end){if(!data_step(work)){result=-2;goto done;}char c=*p++;if(c=='"')break;if(c=='\\'&&p<end)c=*p++;*write++=c;if(!data_quoted((unsigned char)c))valid_value=false;}
            ve=write;while(p<end&&*p!=';'){if(!data_step(work)){result=-2;goto done;}p++;}
        }else{
            while(p<end&&*p!=';'){if(!data_step(work)){result=-2;goto done;}p++;}
            ve=p;while(ve>value&&data_http_space((unsigned char)ve[-1]))ve--;
            for(char *q=value;q<ve;q++)if(!data_quoted((unsigned char)*q))valid_value=false;
        }
        size_t vn=(size_t)(ve-value);if(!nn||!valid_name||!valid_value||(!quoted&&!vn))continue;
        if(count==bc){
            size_t next=bc?bc*2:8;if(next<bc||next>SIZE_MAX/sizeof *buckets)goto done;
            size_t *grown=malloc(next*sizeof *grown);if(!grown)goto done;
            for(size_t i=0;i<next;i++)grown[i]=SIZE_MAX;
            for(size_t i=0;i<count;i++){size_t h=data_name_hash(parameters[i].name,parameters[i].nn)%next;parameters[i].next=grown[h];grown[h]=i;}
            free(buckets);buckets=grown;bc=next;
        }
        size_t hash=data_name_hash(name,nn)%bc,index=buckets[hash];
        while(index!=SIZE_MAX){if(!data_step(work)){result=-2;goto done;}if(parameters[index].nn==nn&&!memcmp(parameters[index].name,name,nn))break;index=parameters[index].next;}
        if(index!=SIZE_MAX)continue;
        if(count==capacity){size_t next=capacity?capacity*2:8;if(next<capacity||next>SIZE_MAX/sizeof *parameters)goto done;void *grown=realloc(parameters,next*sizeof *parameters);if(!grown)goto done;parameters=grown;capacity=next;}
        parameters[count]=(struct data_mime_parameter){name,value,nn,vn,buckets[hash]};buckets[hash]=count++;
    }
    /* A quoted escape can double its original byte's serialization, plus
     * delimiters; each parameter consumes at least that many source bytes. */
    if(length>(SIZE_MAX-1)/2)goto done;
    char *serialized=malloc(length*2+1);if(!serialized)goto done;
    char *write=serialized;size_t tn=(size_t)(type_end-type),sn=(size_t)(subtype_end-subtype);
    memcpy(write,type,tn);write+=tn;*write++='/';memcpy(write,subtype,sn);write+=sn;
    for(size_t i=0;i<count;i++){
        struct data_mime_parameter *parameter=&parameters[i];*write++=';';memcpy(write,parameter->name,parameter->nn);write+=parameter->nn;*write++='=';
        bool quote=!parameter->vn;for(size_t j=0;j<parameter->vn;j++)if(!data_token((unsigned char)parameter->value[j]))quote=true;
        if(quote)*write++='"';
        for(size_t j=0;j<parameter->vn;j++){if(!data_step(work)){free(serialized);result=-2;goto done;}char c=parameter->value[j];if(quote&&(c=='"'||c=='\\'))*write++='\\';*write++=c;}
        if(quote)*write++='"';
    }
    *write=0;*out=serialized;result=1;goto done;
fallback:
    *out=malloc(sizeof "text/plain;charset=US-ASCII");if(*out){memcpy(*out,"text/plain;charset=US-ASCII",sizeof "text/plain;charset=US-ASCII");result=1;}
done:
    free(parameters);free(buckets);return result;
}
void web_data_url_free(struct web_data_url *data) { if(data){free(data->body);free(data->mime);memset(data,0,sizeof *data);} }
int web_data_url_process(const char *url,struct web_data_url *out,bool (*checkpoint)(void *),void *opaque) {
    if(!out)return 0;memset(out,0,sizeof *out);
    if(!url||strncasecmp(url,"data:",5))return 0;
    struct data_work work={checkpoint,opaque,0};const char *end=url+5,*comma=NULL;
    while(*end&&*end!='#'){if(!data_step(&work))return -2;if(*end==','&&!comma)comma=end;end++;}
    if(!comma)return 0;
    const char *meta=url+5,*me=comma;while(meta<me&&data_ascii_space((unsigned char)*meta))meta++;while(me>meta&&data_ascii_space((unsigned char)me[-1]))me--;
    bool base64=false;
    if(me-meta>=7&&!strncasecmp(me-6,"base64",6)){const char *marker=me-6;while(marker>meta&&marker[-1]==' ')marker--;if(marker>meta&&marker[-1]==';'){base64=true;me=marker-1;}}
    size_t mn=(size_t)(me-meta),prefix=mn&&*meta==';'?sizeof "text/plain"-1:0;
    if(mn>SIZE_MAX-prefix-1)return -1;
    char *mime_input=malloc(prefix+mn+1);if(!mime_input)return -1;
    if(prefix)memcpy(mime_input,"text/plain",prefix);memcpy(mime_input+prefix,meta,mn);mime_input[prefix+mn]=0;
    int result=data_mime(mime_input,prefix+mn,&out->mime,&work);free(mime_input);if(result!=1)return result;
    size_t encoded=(size_t)(end-comma-1);if(encoded==SIZE_MAX){web_data_url_free(out);return -1;}
    out->body=malloc(encoded+1);if(!out->body){web_data_url_free(out);return -1;}
    size_t n=0;
    for(const char *p=comma+1;p<end;p++){
        if(!data_step(&work)){result=-2;goto failed;}int hi,lo;
        if(*p=='%'&&end-p>=3&&(hi=data_hex_digit((unsigned char)p[1]))>=0&&(lo=data_hex_digit((unsigned char)p[2]))>=0){out->body[n++]=(unsigned char)((hi<<4)|lo);p+=2;}
        else out->body[n++]=(unsigned char)*p;
    }
    if(base64){
        size_t count=0;for(size_t i=0;i<n;i++){if(!data_step(&work)){result=-2;goto failed;}if(!data_ascii_space(out->body[i]))out->body[count++]=out->body[i];}n=count;
        if(n%4==0&&n&&out->body[n-1]=='='){n--;if(n&&out->body[n-1]=='=')n--;}
        if(n%4==1){result=0;goto failed;}
        unsigned bits=0,buffer=0;count=0;
        for(size_t i=0;i<n;i++){if(!data_step(&work)){result=-2;goto failed;}int digit=data_base64_digit(out->body[i]);if(digit<0){result=0;goto failed;}buffer=(buffer<<6)|(unsigned)digit;bits+=6;if(bits>=8){bits-=8;out->body[count++]=(unsigned char)(buffer>>bits);}}n=count;
    }
    out->body[n]=0;out->length=n;return 1;
failed:
    web_data_url_free(out);return result;
}
