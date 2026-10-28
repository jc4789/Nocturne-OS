/* Content Security Policy Level 3: the one owner of a document's or Worker's
   CSP list. Every policy is kept with its directive set; the checks other
   specifications call (Fetch, HTML, ECMAScript, WebAssembly) are below and
   other layers only ask them. Trusted Types and Wasm compilation CSP are
   native policy, not JS properties. Each header field/list policy is retained
   separately for intersection. */
#include "html_policy.h"
#include "frame.h"
#include "web_url.h"
#include <webnet.h>
#include <bearssl_hash.h>
#include <stdio.h>
#include <ctype.h>
bool html_policy_requires_script(const web_doc *d) {
    for(const struct web_html_policy_rule *r=d && d->html_policy?d->html_policy->first:NULL;r;r=r->next)
        if(r->require_script)return true;
    return false;
}
bool html_policy_wasm_allowed(const struct web_html_policy_rule *r) {
    return !r || !r->has_wasm_policy || r->wasm_allowed;
}
static void directive_free(struct web_csp_directive *d) {
    for(size_t i=0;i<d->count;i++)free(d->value[i]);
    free(d->value);free(d->name);
}
static void rule_free(struct web_html_policy_rule *rule) {
    for(size_t i=0;i<rule->count;i++)free(rule->names[i]);
    for(size_t i=0;i<rule->report_uri_count;i++)free(rule->report_uris[i]);
    for(size_t i=0;i<rule->directive_count;i++)directive_free(&rule->directives[i]);
    free(rule->directives);
    free(rule->report_uris);free(rule->original_policy);free(rule->base_url);free(rule->report_to);free(rule->names);free(rule);
}
void html_policy_free(web_doc *d) {
    if(!d || !d->html_policy)return;
    struct web_html_policy_rule *r=d->html_policy->first;
    while(r){struct web_html_policy_rule *next=r->next;rule_free(r);r=next;}
    free(d->html_policy->self_url);
    free(d->html_policy);d->html_policy=NULL;
}
void html_policy_rewind(web_doc *d,struct web_html_policy_rule *tail){
    if(!d||!d->html_policy)return;
    /* The saved tail belongs to this document's pre-parser policy prefix. */
    struct web_html_policy_rule *r=tail?tail->next:d->html_policy->first;
    while(r){struct web_html_policy_rule *next=r->next;rule_free(r);r=next;}
    if(tail)tail->next=NULL;else d->html_policy->first=NULL;
    d->html_policy->last=tail;
}
static bool named(const char *begin,const char *end,const char *name) {
    return (size_t)(end-begin)==strlen(name) && !strncasecmp(begin,name,end-begin);
}
static bool token(const char *begin,const char *end,const char *name) {
    return (size_t)(end-begin)==strlen(name) && !memcmp(begin,name,end-begin);
}
static char *copy_text(const char *begin,const char *end){size_t n=end-begin;char *s=malloc(n+1);if(s){memcpy(s,begin,n);s[n]=0;}return s;}
/* frame-ancestors matches the serialized ancestor origin, not its document
 * path. Both URLs are http(s) URL records: the URL parser supplies canonical
 * hosts, and a null port is the scheme's default port. */
static bool ancestor_source(const char *p,const char *end,const struct web_url *ancestor,const struct web_url *self) {
    bool ancestor_tls=web_url_scheme_is(ancestor,"https"),self_tls=web_url_scheme_is(self,"https");
    int ancestor_port=web_url_port_or_default(ancestor),self_port=web_url_port_or_default(self);
    size_t an,sn;const char *ancestor_host=web_url_host_text(ancestor,&an),*self_host=web_url_host_text(self,&sn);
    if(named(p,end,"'self'"))return an==sn && !memcmp(ancestor_host,self_host,an) &&
        (ancestor_port==self_port || (ancestor_port==(ancestor_tls?443:80) && self_port==(self_tls?443:80))) &&
        (ancestor_tls || !self_tls);
    if(token(p,end,"*"))return true; /* Both parsed origins are HTTP(S). */
    if(named(p,end,"http:"))return true;
    if(named(p,end,"https:"))return ancestor_tls;
    const char *host=p,*scheme_end=NULL;
    for(const char *q=p;end-q>=3;q++)if(q[0]==':' && q[1]=='/' && q[2]=='/'){scheme_end=q;host=q+3;break;}
    bool secure=self_tls;
    if(scheme_end){
        if(named(p,scheme_end,"https"))secure=true;
        else if(named(p,scheme_end,"http"))secure=false;
        else return false;
    }
    if(secure && !ancestor_tls)return false;
    const char *host_end=host;while(host_end<end && *host_end!=':' && *host_end!='/')host_end++;
    size_t hn=host_end-host;
    if(!hn)return false;
    bool wildcard=hn>=2 && host[0]=='*' && host[1]=='.';
    const char *domain=host+(wildcard?2:0);bool numeric=true;
    for(const char *q=domain;q<host_end;q++){
        unsigned char c=(unsigned char)*q;
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.')){
            if(!(hn==1 && c=='*'))return false;
        }
        if((c<'0'||c>'9') && c!='.')numeric=false;
    }
    /* CSP's host-part matching: a host that is not a domain never matches. */
    if(numeric || ancestor->host!=WEB_URL_HOST_DOMAIN)return false;
    if(!(hn==1 && *host=='*')){
        if(wildcard){if(an<hn || strncasecmp(ancestor_host+an-(hn-1),host+1,hn-1))return false;}
        else if(an!=hn || strncasecmp(ancestor_host,host,hn))return false;
    }
    const char *path=host_end;
    if(path<end && *path==':'){
        const char *port=++path;while(path<end && *path!='/')path++;
        if(!token(port,path,"*")){
            unsigned value=0;if(port==path)return false;
            for(const char *q=port;q<path;q++){
                if(*q<'0'||*q>'9'||value>(65535u-(unsigned)(*q-'0'))/10)return false;
                value=value*10+(unsigned)(*q-'0');
            }
            if((int)value!=ancestor_port)return false;
        }
    }else if(ancestor_port!=(ancestor_tls?443:80))return false;
    return path==end || token(path,end,"/");
}
/* An http(s) URL's record, or false (not a URL, out of memory, another scheme). */
static bool http_record(const char *url,struct web_url *record) {
    if(!url || web_url_read(url,strlen(url),record)!=1)return false;
    if(web_url_http_scheme(record))return true;
    web_url_free(record);return false;
}
static bool ancestor_list(web_doc *parent,const char *p,const char *end,const char *url) {
    struct web_url self;
    if(!http_record(url,&self))return false;
    bool allowed=true;
    for(web_doc *d=parent;d;d=d->frame_parent){
        struct web_url ancestor;
        if((d->sandbox_flags&SB_ORIGIN) || !http_record(web_effective_url(d),&ancestor)){allowed=false;break;}
        bool matched=false;
        for(const char *q=p;q<end;){
            while(q<end && is_space((unsigned char)*q))q++;
            const char *start=q;while(q<end && !is_space((unsigned char)*q))q++;
            if(ancestor_source(start,q,&ancestor,&self)){matched=true;break;}
        }
        web_url_free(&ancestor);if(!matched){allowed=false;break;}
    }
    web_url_free(&self);return allowed;
}
bool html_policy_embedding_allowed(web_doc *parent,const char *headers,const char *url,uint32_t *sandbox) {
    uint32_t restrictions=0;bool has_ancestors=false;
    bool deny=false,same=false,allowall=false,other=false;
    for(const char *p=headers?headers:"";*p;){
        const char *end=p;while(*end && *end!='\r' && *end!='\n')end++;
        const char *colon=memchr(p,':',end-p);
        if(colon && named(p,colon,"Content-Security-Policy")){
            const char *policy=colon+1;
            while(policy<end){
                const char *policy_end=policy;while(policy_end<end && *policy_end!=',')policy_end++;
                bool seen_ancestors=false,seen_sandbox=false;
                for(const char *q=policy;q<policy_end;){
                    while(q<policy_end && (is_space((unsigned char)*q)||*q==';'))q++;
                    const char *name=q;while(q<policy_end && !is_space((unsigned char)*q)&&*q!=';')q++;
                    const char *name_end=q,*value=q;while(q<policy_end && *q!=';')q++;
                    if(!seen_ancestors && named(name,name_end,"frame-ancestors")){
                        seen_ancestors=has_ancestors=true;
                        if(parent && !ancestor_list(parent,value,q,url))return false;
                    }
                    if(!seen_sandbox && named(name,name_end,"sandbox")){
                        seen_sandbox=true;char *text=copy_text(value,q);if(!text)return false;
                        restrictions|=sandbox_parse(text,NULL,0);free(text);
                    }
                }
                policy=policy_end<end?policy_end+1:end;
            }
        }else if(colon && named(p,colon,"X-Frame-Options")){
            const char *value=colon+1;
            for(;;){
                const char *next=value;while(next<end && *next!=',')next++;
                const char *start=value,*stop=next;
                while(start<stop && is_space((unsigned char)*start))start++;
                while(stop>start && is_space((unsigned char)stop[-1]))stop--;
                if(named(start,stop,"deny"))deny=true;
                else if(named(start,stop,"sameorigin"))same=true;
                else if(named(start,stop,"allowall"))allowall=true;
                else other=true;
                if(next==end)break;value=next+1;
            }
        }
        p=end;while(*p=='\r'||*p=='\n')p++;
    }
    /* Enforced frame-ancestors takes precedence over XFO. Unknown XFO values
     * are ignored alone, but conflicting recognized/invalid lists are denied. */
    if(parent && !has_ancestors){
        if(deny || (same && (allowall||other)) || (allowall&&other))return false;
        if(same){
            for(web_doc *d=parent;d;d=d->frame_parent)
                if((d->sandbox_flags&SB_ORIGIN) || web_url_same_origin(url,web_effective_url(d))!=1)return false;
        }
    }
    if(sandbox)*sandbox=restrictions;
    return true;
}
static bool policy_name(const char *begin,const char *end){
    if(token(begin,end,"*"))return true;if(begin==end)return false;
    for(const char *p=begin;p<end;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-#=_/@.%",*p)))return false;
    return true;
}
/* CSP's report-uri: each token through the URL parser (UTF-8) against the
   policy's URL. */
static bool report_uri(struct web_html_policy_rule *r,const char *begin,const char *end){
    char *relative=copy_text(begin,end);if(!relative)return false;char *absolute=NULL;
    int resolved=web_resolve_url_owned(r->base_url,relative,&absolute);free(relative);
    if(resolved<0)return false;
    /* Reports go to http(s) URLs without credentials (a report request carries none). */
    struct web_url record;int read=resolved?web_url_read(absolute,strlen(absolute),&record):0;
    if(read<0){free(absolute);return false;}
    bool usable=read==1 && web_url_http_scheme(&record) && !web_url_includes_credentials(&record);
    if(read==1)web_url_free(&record);
    if(!usable){free(absolute);return true;}
    char **grown=r->report_uri_count<SIZE_MAX/sizeof *r->report_uris?
        realloc(r->report_uris,(r->report_uri_count+1)*sizeof *grown):NULL;
    if(!grown){free(absolute);return false;}
    r->report_uris=grown;r->report_uris[r->report_uri_count++]=absolute;return true;
}
static const struct web_csp_directive *directive_find(const struct web_html_policy_rule *r,const char *name){
    for(size_t i=0;r && i<r->directive_count;i++)if(!strcmp(r->directives[i].name,name))return &r->directives[i];
    return NULL;
}
/* An ASCII case-insensitive match for one complete source expression. */
static bool value_has(const struct web_csp_directive *d,const char *expression){
    for(size_t i=0;d && i<d->count;i++)if(!strcasecmp(d->value[i],expression))return true;
    return false;
}
/* CSP3 §2.2.1 Parse a serialized CSP: strictly split on ';', strip ASCII
   whitespace, skip an empty or non-ASCII token and a repeated name, and split
   the value on ASCII whitespace. A meta policy drops report-uri,
   frame-ancestors and sandbox (HTML's http-equiv processing). */
static bool parse_directives(struct web_html_policy_rule *r,const char *begin,const char *end,bool meta){
    for(const char *p=begin;;){
        const char *stop=memchr(p,';',(size_t)(end-p));if(!stop)stop=end;
        const char *a=p,*b=stop;
        while(a<b && is_space((unsigned char)*a))a++;while(b>a && is_space((unsigned char)b[-1]))b--;
        bool ascii=true;for(const char *q=a;q<b;q++)if((unsigned char)*q>0x7f){ascii=false;break;}
        if(a<b && ascii){
            const char *n=a;while(n<b && !is_space((unsigned char)*n))n++;
            size_t length=(size_t)(n-a);bool seen=false;
            for(size_t i=0;i<r->directive_count && !seen;i++)
                seen=strlen(r->directives[i].name)==length && !strncasecmp(r->directives[i].name,a,length);
            bool dropped=meta && (named(a,n,"report-uri")||named(a,n,"frame-ancestors")||named(a,n,"sandbox"));
            if(!seen && !dropped){
                if(r->directive_count==SIZE_MAX/sizeof *r->directives)return false;
                struct web_csp_directive *grown=realloc(r->directives,(r->directive_count+1)*sizeof *grown);
                if(!grown)return false;r->directives=grown;
                struct web_csp_directive *d=&r->directives[r->directive_count];memset(d,0,sizeof *d);
                d->name=copy_text(a,n);if(!d->name)return false;
                for(char *c=d->name;*c;c++)if(*c>='A'&&*c<='Z')*c+=32;
                r->directive_count++;
                for(const char *v=n;v<b;){
                    while(v<b && is_space((unsigned char)*v))v++;
                    const char *t=v;while(v<b && !is_space((unsigned char)*v))v++;
                    if(t==v)break;
                    if(d->count==SIZE_MAX/sizeof *d->value)return false;
                    char **values=realloc(d->value,(d->count+1)*sizeof *values);if(!values)return false;d->value=values;
                    if(!(d->value[d->count]=copy_text(t,v)))return false;d->count++;
                }
            }
        }
        if(stop==end)break;
        p=stop+1;
    }
    return true;
}
static bool append(web_doc *d,const char *begin,const char *end,bool report,bool meta,const char *base) {
    struct web_html_policy_rule *r=calloc(1,sizeof *r);
    if(!r)return false;
    while(begin<end&&is_space((unsigned char)*begin))begin++;while(end>begin&&is_space((unsigned char)end[-1]))end--;
    r->original_policy=copy_text(begin,end);base=base?base:d->url?d->url:"";r->base_url=copy_text(base,base+strlen(base));
    if(!r->original_policy||!r->base_url){rule_free(r);return false;}
    r->report_only=report;r->from_meta=meta;
    if(!parse_directives(r,begin,end,meta)){rule_free(r);return false;}
    /* §2.2.2: a policy whose directive set is empty is not appended. */
    if(!r->directive_count){rule_free(r);return true;}
    const struct web_csp_directive *require=directive_find(r,"require-trusted-types-for"),*names=directive_find(r,"trusted-types");
    const struct web_csp_directive *uri=directive_find(r,"report-uri"),*to=directive_find(r,"report-to");
    for(size_t i=0;require && i<require->count;i++)if(!strcmp(require->value[i],"'script'"))r->require_script=true;
    if(names){
        r->has_names=true;
        for(size_t i=0;i<names->count;i++){
            const char *t=names->value[i],*e=t+strlen(t);
            if(token(t,e,"'allow-duplicates'")){r->allow_duplicates=true;continue;}
            if(token(t,e,"'none'") || !policy_name(t,e))continue;
            if(r->count==SIZE_MAX/sizeof *r->names){rule_free(r);return false;}
            char **grown=realloc(r->names,(r->count+1)*sizeof *grown);
            if(!grown){rule_free(r);return false;}r->names=grown;
            if(!(r->names[r->count]=copy_text(t,e))){rule_free(r);return false;}r->count++;
        }
    }
    for(size_t i=0;uri && i<uri->count;i++)if(!report_uri(r,uri->value[i],uri->value[i]+strlen(uri->value[i]))){rule_free(r);return false;}
    if(to && to->count && !(r->report_to=copy_text(to->value[0],to->value[0]+strlen(to->value[0])))){rule_free(r);return false;}
    /* CSP3 §4.5.1 checks these complete source expressions with ASCII
     * case-insensitivity, not URL hosts, prefixes or script-src-elem. */
    const struct web_csp_directive *wasm=directive_find(r,"script-src");if(!wasm)wasm=directive_find(r,"default-src");
    r->has_wasm_policy=wasm!=NULL;
    r->wasm_allowed=!wasm || value_has(wasm,"'unsafe-eval'") || value_has(wasm,"'wasm-unsafe-eval'");
    if(!d->html_policy){d->html_policy=calloc(1,sizeof *d->html_policy);if(!d->html_policy){rule_free(r);return false;}}
    if(d->html_policy->last)d->html_policy->last->next=r;else d->html_policy->first=r;
    d->html_policy->last=r;return true;
}
static bool list(web_doc *d,const char *begin,const char *end,bool report,bool meta){
    while(begin<end){const char *next=begin;while(next<end && *next!=',')next++;
        if(!append(d,begin,next,report,meta,NULL))return false;begin=next<end?next+1:end;}
    return true;
}
/* The CSP list's self-origin, set once when its first policy arrives. */
static bool self_origin(web_doc *d,const char *url){
    if(!d->html_policy || d->html_policy->self_url)return true;
    if(!url)url=web_effective_url(d);
    return (d->html_policy->self_url=strdup(url?url:"about:blank"))!=NULL;
}
bool html_policy_init(web_doc *d,const char *headers,const web_doc *inherit){
    if(inherit && inherit->html_policy){
        for(const struct web_html_policy_rule *r=inherit->html_policy->first;r;r=r->next){
            if(!append(d,r->original_policy,r->original_policy+strlen(r->original_policy),r->report_only,r->from_meta,r->base_url))return false;
        }
    }
    /* A policy container cloned from the creator keeps its self-origin; a
       response's own list is the response URL's (§2.2.2). */
    if(!headers && inherit && !self_origin(d,inherit->html_policy && inherit->html_policy->self_url?inherit->html_policy->self_url:
                                                web_effective_url((web_doc *)inherit)))return false;
    for(const char *p=headers?headers:"";*p;){
        const char *end=p;while(*end && *end!='\r' && *end!='\n')end++;
        const char *colon=memchr(p,':',end-p);
        if(colon){bool enforce=named(p,colon,"Content-Security-Policy"),report=named(p,colon,"Content-Security-Policy-Report-Only");
            if((enforce||report) && !list(d,colon+1,end,report,false))return false;}
        p=end;while(*p=='\r'||*p=='\n')p++;
    }
    /* The transport puts the response's status line first ("HTTP/1.1 200 ");
       a document with no response of its own (about:blank, srcdoc) has 0. */
    if(d->html_policy && headers && !strncmp(headers,"HTTP/",5)){
        const char *s=headers+5;while(*s && *s!=' ' && *s!='\r' && *s!='\n')s++;
        if(*s==' ' && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]))
            d->html_policy->status=(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
    }
    return self_origin(d,headers?d->url:NULL);
}
/* The bare token of a structured-field item, without its parameters. Field
   lines combine into a list, which is not an item: a repeat yields none. */
static bool policy_token(const char *headers,const char *name,const char **value,size_t *length){
    bool found=false;
    for(const char *p=headers?headers:"";*p;){
        const char *end=p;while(*end && *end!='\r' && *end!='\n')end++;
        const char *colon=memchr(p,':',end-p);
        if(colon && named(p,colon,name)){
            if(found)return false;
            const char *v=colon+1;while(v<end && (*v==' '||*v=='\t'))v++;
            const char *t=v;while(t<end && (isalnum((unsigned char)*t)||*t=='-'||*t=='_'||*t=='.'||*t=='*'))t++;
            const char *rest=t;while(rest<end && (*rest==' '||*rest=='\t'))rest++;
            if(t==v || (rest<end && *rest!=';'))return false;
            *value=v;*length=(size_t)(t-v);found=true;
        }
        p=end;while(*p=='\r'||*p=='\n')p++;
    }
    return found;
}
int html_policy_embedder(const char *headers){
    const char *v;size_t n;
    if(!policy_token(headers,"Cross-Origin-Embedder-Policy",&v,&n))return HTML_COEP_UNSAFE_NONE;
    if(n==12 && !memcmp(v,"require-corp",12))return HTML_COEP_REQUIRE_CORP;
    if(n==14 && !memcmp(v,"credentialless",14))return HTML_COEP_CREDENTIALLESS;
    return HTML_COEP_UNSAFE_NONE;
}
bool html_policy_opener_isolated(const char *headers){
    const char *v;size_t n;
    return policy_token(headers,"Cross-Origin-Opener-Policy",&v,&n) && n==11 && !memcmp(v,"same-origin",11);
}
void html_policy_isolation(web_doc *d,const char *headers,const web_doc *inherit){
    /* Local documents (about:blank, srcdoc, blob:) carry no response: their
       policy container is the creator's. */
    d->embedder_policy=headers?html_policy_embedder(headers):inherit?inherit->embedder_policy:HTML_COEP_UNSAFE_NONE;
    web_doc *parent=d->frame_parent;
    if(!parent)d->cross_origin_isolated=headers && html_policy_opener_isolated(headers) &&
        d->embedder_policy!=HTML_COEP_UNSAFE_NONE && web_url_potentially_trustworthy(d->url);
    else d->cross_origin_isolated=parent->cross_origin_isolated && d->embedder_policy!=HTML_COEP_UNSAFE_NONE &&
        web_frame_same_origin(parent,d);
}
/* HTML's http-equiv parses its content as one serialized CSP (a comma is no
   list separator there). */
bool html_policy_meta(web_doc *d,const char *content,size_t length){
    return d && content && append(d,content,content+length,false,true,NULL) && self_origin(d,NULL);
}
char *html_policy_headers(const web_doc *d){
    sbuf text={0};
    for(const struct web_html_policy_rule *r=d && d->html_policy?d->html_policy->first:NULL;r;r=r->next){
        sb_puts(&text,r->report_only?"Content-Security-Policy-Report-Only: ":"Content-Security-Policy: ");
        sb_puts(&text,r->original_policy);
        sb_puts(&text,"\r\n");
    }
    char *value=sb_cstr(&text);return value;
}
/* Private native IPC snapshot, never interpreted as a response header. Length
   framing preserves the original policy and its original URL resolution base,
   and the list's self-origin URL first. */
char *html_policy_snapshot(const web_doc *d){
    sbuf text={0};const char *self=d&&d->html_policy&&d->html_policy->self_url?d->html_policy->self_url:"";
    char head[48];snprintf(head,sizeof head,"NCTT2 %zu\n",strlen(self));sb_puts(&text,head);sb_puts(&text,self);sb_putc(&text,'\n');
    for(const struct web_html_policy_rule *r=d&&d->html_policy?d->html_policy->first:NULL;r;r=r->next){
        char heading[160];int length=snprintf(heading,sizeof heading,"%u %u %zu %zu\n",r->report_only?1u:0u,r->from_meta?1u:0u,strlen(r->original_policy),strlen(r->base_url));
        if(length<0||(size_t)length>=sizeof heading){sb_free(&text);return NULL;}
        sb_puts(&text,heading);sb_puts(&text,r->original_policy);sb_puts(&text,r->base_url);sb_putc(&text,'\n');
    }
    return sb_cstr(&text);
}
static bool snapshot_number(const char **p,const char *end,size_t *value,char separator){
    const char *s=*p;if(s==end||*s<'0'||*s>'9')return false;size_t n=0;
    while(s<end&&*s>='0'&&*s<='9'){unsigned digit=*s++-'0';if(n>(SIZE_MAX-digit)/10)return false;n=n*10+digit;}
    if(s==end||*s++!=separator)return false;*p=s;*value=n;return true;
}
bool html_policy_snapshot_init(web_doc *d,const char *text){
    if(!d||!text||strncmp(text,"NCTT2 ",6))return false;const char *p=text+6,*end=text+strlen(text);
    size_t self_length;
    if(!snapshot_number(&p,end,&self_length,'\n')||self_length>=(size_t)(end-p)||p[self_length]!='\n')return false;
    char *self=self_length?copy_text(p,p+self_length):NULL;if(self_length && !self)return false;p+=self_length+1;
    while(p<end){size_t report,meta,length,base_length;
        if(!snapshot_number(&p,end,&report,' ')||!snapshot_number(&p,end,&meta,' ')||!snapshot_number(&p,end,&length,' ')||!snapshot_number(&p,end,&base_length,'\n')||report>1||meta>1){free(self);return false;}
        if(length>(size_t)(end-p)||base_length>(size_t)(end-p)-length||(size_t)(end-p)-length-base_length<1||p[length+base_length]!='\n'){free(self);return false;}
        char *base=copy_text(p+length,p+length+base_length);if(!base){free(self);return false;}
        bool ok=append(d,p,p+length,report!=0,meta!=0,base);free(base);if(!ok){free(self);return false;}p+=length+base_length+1;
    }
    bool ok=self_origin(d,self);free(self);return ok;
}

/* ---------------------------------------------------------------------------
   CSP3 §6.7 matching. URLs are the URL parser's records (weburl.h): their
   component marks, host kind and port; nothing here parses a URL by hand. */
struct csp_url {
    struct web_url parsed;
    const char *scheme; size_t scheme_n;
    const char *host; size_t host_n;
    long port;            /* -1: null (the scheme's default) */
    const char *path; size_t path_n;
};
static bool csp_url(const char *text,struct csp_url *u){
    memset(u,0,sizeof *u);u->port=-1;
    if(!text || web_url_read(text,strlen(text),&u->parsed)!=1)return false;
    const char *h=u->parsed.href;const size_t *m=u->parsed.mark;
    u->scheme=h;u->scheme_n=m[WEB_URL_PROTOCOL_END]?m[WEB_URL_PROTOCOL_END]-1:0;
    u->host=web_url_host_text(&u->parsed,&u->host_n);
    u->port=u->parsed.port;
    u->path=h+m[WEB_URL_PATH_START];u->path_n=m[WEB_URL_QUERY_START]-m[WEB_URL_PATH_START];
    return true;
}
static void csp_url_free(struct csp_url *u){web_url_free(&u->parsed);}
static bool span_is(const char *p,size_t n,const char *text){return strlen(text)==n && !strncasecmp(p,text,n);}
/* A host that is a domain: neither an IP address nor an opaque or empty host. */
static bool is_domain(const struct csp_url *u){return u->parsed.host==WEB_URL_HOST_DOMAIN;}
/* A URL's origin as (scheme, host, port), for a tuple origin; a blob: URL's
   is its path URL's. Self-origins are kept as URLs. */
static bool tuple_origin(const char *url,struct csp_url *o){
    if(!csp_url(url,o))return false;
    if(span_is(o->scheme,o->scheme_n,"blob")){
        char *inner=copy_text(o->path,o->path+o->path_n);csp_url_free(o);
        bool ok=inner && csp_url(inner,o);free(inner);
        if(!ok)return false;
    }
    if(web_url_default_port(&o->parsed)<0){csp_url_free(o);return false;}
    return true;
}
static bool same_origin(const struct csp_url *a,const struct csp_url *b){
    return a->scheme_n==b->scheme_n && !strncasecmp(a->scheme,b->scheme,a->scheme_n) &&
        a->host_n==b->host_n && !strncasecmp(a->host,b->host,a->host_n) && a->port==b->port;
}
/* §6.7.2.9: A scheme-part matches B, with the secure upgrades. */
static bool scheme_part_match(const char *a,size_t an,const char *b,size_t bn){
    if(an==bn && !strncasecmp(a,b,an))return true;
    if(span_is(a,an,"http"))return span_is(b,bn,"https");
    if(span_is(a,an,"ws"))return span_is(b,bn,"wss")||span_is(b,bn,"http")||span_is(b,bn,"https");
    if(span_is(a,an,"wss"))return span_is(b,bn,"https");
    return false;
}
static bool scheme_char(char c,bool first){
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(!first && ((c>='0'&&c<='9')||c=='+'||c=='-'||c=='.'));
}
static bool host_char(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-';}
static bool hex_digit(char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');}
/* pchar of RFC 3986 path-absolute, without ';' and ','. */
static bool path_char(char c){
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||strchr("-._~!$&'()*+=:@/",c);
}
/* The parts of a host-source (§2.3.1 grammar), or false when the expression
   is not one. */
struct host_source { const char *scheme,*host,*port,*path; size_t scheme_n,host_n,port_n,path_n; };
static bool host_source(const char *e,struct host_source *s){
    memset(s,0,sizeof *s);
    const char *p=e;
    if(scheme_char(*p,true)){
        const char *q=p+1;while(scheme_char(*q,false))q++;
        if(q[0]==':' && q[1]=='/' && q[2]=='/'){s->scheme=p;s->scheme_n=(size_t)(q-p);p=q+3;}
    }
    s->host=p;
    if(p[0]=='*' && p[1]!='.')p++;
    else{
        if(p[0]=='*')p+=2;
        if(!host_char(*p))return false;
        for(;;){
            while(host_char(*p))p++;
            if(*p!='.')break;
            if(!host_char(p[1])){p++;break;} /* the optional final "." */
            p++;
        }
    }
    s->host_n=(size_t)(p-s->host);
    if(*p==':'){
        s->port=++p;
        if(*p=='*')p++;else{if(*p<'0'||*p>'9')return false;while(*p>='0'&&*p<='9')p++;}
        s->port_n=(size_t)(p-s->port);
    }
    if(*p=='/'){
        if(p[1]=='/')return false; /* path-absolute's first segment is not empty */
        s->path=p;
        for(;*p;p++){
            if(*p=='%'){if(!hex_digit(p[1])||!hex_digit(p[2]))return false;p+=2;continue;}
            if(!path_char(*p))return false;
        }
        s->path_n=(size_t)(p-s->path);
    }
    return *p==0;
}
static bool scheme_source(const char *e,size_t *n){
    if(!scheme_char(*e,true))return false;
    const char *p=e+1;while(scheme_char(*p,false))p++;
    if(p[0]!=':' || p[1])return false;
    *n=(size_t)(p-e);return true;
}
static bool base64_value(const char *p,const char *end){
    const char *q=p;
    while(q<end && ((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='+'||*q=='/'||*q=='-'||*q=='_'))q++;
    if(q==p)return false;
    size_t pad=0;while(q<end && *q=='='){q++;pad++;}
    return q==end && pad<=2;
}
/* nonce-source: the base64-value part, or NULL. */
static const char *nonce_source(const char *e,size_t *n){
    size_t length=strlen(e);
    if(length<9 || strncasecmp(e,"'nonce-",7) || e[length-1]!='\'' || !base64_value(e+7,e+length-1))return NULL;
    *n=length-8;return e+7;
}
/* hash-source: its algorithm (256, 384 or 512) and base64-value, or 0. */
static int hash_source(const char *e,const char **value,size_t *n){
    size_t length=strlen(e);
    if(length<10 || e[0]!='\'' || e[length-1]!='\'' || strncasecmp(e+1,"sha",3) || e[7]!='-')return 0;
    int algorithm=!strncmp(e+4,"256",3)?256:!strncmp(e+4,"384",3)?384:!strncmp(e+4,"512",3)?512:0;
    if(!algorithm || !base64_value(e+8,e+length-1))return 0;
    *value=e+8;*n=length-9;return algorithm;
}
static int unhex(char c){return c<='9'?c-'0':(c|32)-'a'+10;}
/* Percent-decoded byte equality of two path pieces. */
static bool piece_equal(const char *a,size_t an,const char *b,size_t bn){
    size_t i=0,j=0;
    for(;;){
        if(i==an || j==bn)return i==an && j==bn;
        int ca=(unsigned char)a[i],cb=(unsigned char)b[j];
        if(ca=='%' && i+2<an && hex_digit(a[i+1]) && hex_digit(a[i+2])){ca=unhex(a[i+1])*16+unhex(a[i+2]);i+=3;}else i++;
        if(cb=='%' && j+2<bn && hex_digit(b[j+1]) && hex_digit(b[j+2])){cb=unhex(b[j+1])*16+unhex(b[j+2]);j+=3;}else j++;
        if(ca!=cb)return false;
    }
}
/* §6.7.2.12 path-part matching: A (the expression's) against B (the URL's). */
static bool path_part_match(const char *a,size_t an,const char *b,size_t bn){
    if(!an)return true;
    if(an==1 && a[0]=='/' && !bn)return true;
    bool exact=a[an-1]!='/';
    size_t pieces_a=1,pieces_b=1;
    for(size_t i=0;i<an;i++)if(a[i]=='/')pieces_a++;
    for(size_t i=0;i<bn;i++)if(b[i]=='/')pieces_b++;
    if(pieces_a>pieces_b || (exact && pieces_a!=pieces_b))return false;
    if(!exact)pieces_a--; /* the final empty piece */
    size_t i=0,j=0;
    for(size_t k=0;k<pieces_a;k++){
        size_t ie=i;while(ie<an && a[ie]!='/')ie++;
        size_t je=j;while(je<bn && b[je]!='/')je++;
        if(!piece_equal(a+i,ie-i,b+j,je-j))return false;
        i=ie+1;j=je+1;
    }
    return true;
}
/* §6.7.2.10 host-part matching. */
static bool host_part_match(const char *pattern,size_t pn,const struct csp_url *u){
    if(!is_domain(u))return false;
    if(pn==1 && pattern[0]=='*')return true;
    if(pn>=2 && pattern[0]=='*' && pattern[1]=='.'){
        size_t rn=pn-1;
        return u->host_n>=rn && !strncasecmp(u->host+u->host_n-rn,pattern+1,rn);
    }
    return pn==u->host_n && !strncasecmp(pattern,u->host,pn);
}
/* §6.7.2.11 port-part matching. */
static bool port_part_match(const char *port,size_t n,const struct csp_url *u){
    if(n==1 && port[0]=='*')return true;
    long input=-1;
    if(n){input=0;for(size_t i=0;i<n;i++){input=input*10+(port[i]-'0');if(input>99999)input=99999;}}
    if(input==u->port)return true;
    if(u->port<0)return input>=0 && input==web_url_default_port(&u->parsed);
    return false;
}
/* §6.7.2.8 Does url match expression in origin with redirect count?
   self is the self-origin's URL view, or NULL for an opaque one. */
static bool url_matches_expression(const struct csp_url *u,const char *e,const struct csp_url *self,unsigned redirects){
    if(!strcmp(e,"*")){
        if(span_is(u->scheme,u->scheme_n,"http")||span_is(u->scheme,u->scheme_n,"https"))return true;
        return self && self->scheme_n==u->scheme_n && !strncasecmp(self->scheme,u->scheme,u->scheme_n);
    }
    size_t scheme_n;
    if(scheme_source(e,&scheme_n))return scheme_part_match(e,scheme_n,u->scheme,u->scheme_n);
    struct host_source s;
    if(e[0]!='\'' && host_source(e,&s)){
        if(s.scheme && !scheme_part_match(s.scheme,s.scheme_n,u->scheme,u->scheme_n))return false;
        if(!u->host_n)return false;
        if(!s.scheme && (!self || !scheme_part_match(self->scheme,self->scheme_n,u->scheme,u->scheme_n)))return false;
        if(!host_part_match(s.host,s.host_n,u))return false;
        if(!port_part_match(s.port,s.port_n,u))return false;
        if(s.path_n && !redirects && !path_part_match(s.path,s.path_n,u->path,u->path_n))return false;
        return true;
    }
    if(!strcasecmp(e,"'self'")){
        if(span_is(u->scheme,u->scheme_n,"blob") || !self)return false;
        if(same_origin(self,u))return true;
        if(self->host_n!=u->host_n || strncasecmp(self->host,u->host,u->host_n) || self->port!=u->port)return false;
        return span_is(u->scheme,u->scheme_n,"https")||span_is(u->scheme,u->scheme_n,"wss")||
            (span_is(self->scheme,self->scheme_n,"http") && (span_is(u->scheme,u->scheme_n,"http")||span_is(u->scheme,u->scheme_n,"ws")));
    }
    return false;
}
/* §6.7.2.7 Does url match source list in origin with redirect count? */
static bool url_matches_list(const struct csp_url *u,const struct web_csp_directive *list,const struct csp_url *self,unsigned redirects){
    if(!list->count || (list->count==1 && !strcasecmp(list->value[0],"'none'")))return false;
    for(size_t i=0;i<list->count;i++)if(url_matches_expression(u,list->value[i],self,redirects))return true;
    return false;
}
/* §6.7.2.3 Does nonce match source list? */
static bool nonce_matches_list(const char *nonce,const struct web_csp_directive *list){
    if(!nonce || !*nonce)return false;
    size_t length=strlen(nonce),n;
    for(size_t i=0;i<list->count;i++){
        const char *value=nonce_source(list->value[i],&n);
        if(value && n==length && !memcmp(value,nonce,n))return true;
    }
    return false;
}
/* §6.7.2.4 Does integrity metadata match source list? (SRI's parse metadata:
   space-separated "alg-value[?options]" with a known algorithm.) */
static bool integrity_matches_list(const char *metadata,const struct web_csp_directive *list){
    bool any_hash=false;const char *v;size_t n;
    for(size_t i=0;i<list->count && !any_hash;i++)any_hash=hash_source(list->value[i],&v,&n)!=0;
    if(!any_hash || !metadata)return false;
    size_t sources=0;
    for(const char *p=metadata;*p;){
        while(*p && is_space((unsigned char)*p))p++;
        const char *t=p;while(*p && !is_space((unsigned char)*p))p++;
        if(t==p)break;
        const char *q=memchr(t,'?',(size_t)(p-t));const char *e=q?q:p;
        const char *dash=memchr(t,'-',(size_t)(e-t));if(!dash)continue;
        int algorithm=span_is(t,(size_t)(dash-t),"sha256")?256:span_is(t,(size_t)(dash-t),"sha384")?384:
            span_is(t,(size_t)(dash-t),"sha512")?512:0;
        if(!algorithm)continue;
        sources++;bool found=false;
        for(size_t i=0;i<list->count && !found;i++)
            found=hash_source(list->value[i],&v,&n)==algorithm && n==(size_t)(e-dash-1) && !memcmp(v,dash+1,n);
        if(!found)return false;
    }
    return sources!=0;
}
/* §6.8.1 Get the effective directive for request. */
static const char *request_directive(const struct html_csp_request *r){
    if(r->prefetch)return "default-src";
    switch(r->destination){
    case WEBNET_DEST_MANIFEST: return "manifest-src";
    case WEBNET_DEST_OBJECT: case WEBNET_DEST_EMBED: return "object-src";
    case WEBNET_DEST_FRAME: case WEBNET_DEST_IFRAME: return "frame-src";
    case WEBNET_DEST_AUDIO: case WEBNET_DEST_TRACK: case WEBNET_DEST_VIDEO: return "media-src";
    case WEBNET_DEST_FONT: return "font-src";
    case WEBNET_DEST_IMAGE: return "img-src";
    case WEBNET_DEST_STYLE: return "style-src-elem";
    case WEBNET_DEST_SCRIPT: case WEBNET_DEST_XSLT: case WEBNET_DEST_AUDIOWORKLET: case WEBNET_DEST_PAINTWORKLET: return "script-src-elem";
    case WEBNET_DEST_SERVICEWORKER: case WEBNET_DEST_SHAREDWORKER: case WEBNET_DEST_WORKER: return "worker-src";
    case WEBNET_DEST_REPORT: return NULL;
    default: return "connect-src";
    }
}
static bool script_like(int destination){
    return destination==WEBNET_DEST_AUDIOWORKLET||destination==WEBNET_DEST_PAINTWORKLET||destination==WEBNET_DEST_SCRIPT||
        destination==WEBNET_DEST_SERVICEWORKER||destination==WEBNET_DEST_SHAREDWORKER||destination==WEBNET_DEST_WORKER||
        destination==WEBNET_DEST_XSLT;
}
/* §6.8.2 Get the effective directive for inline checks. */
static const char *inline_directive(enum html_csp_inline type){
    switch(type){
    case HTML_CSP_SCRIPT: case HTML_CSP_NAVIGATION: return "script-src-elem";
    case HTML_CSP_SCRIPT_ATTRIBUTE: return "script-src-attr";
    case HTML_CSP_STYLE: return "style-src-elem";
    case HTML_CSP_STYLE_ATTRIBUTE: return "style-src-attr";
    }
    return NULL;
}
/* §6.8.3 Get fetch directive fallback list, then §6.8.4 Should fetch
   directive execute. */
static bool should_execute(const char *effective,const char *name,const struct web_html_policy_rule *policy){
    static const char *const lists[][5]={
        {"script-src-elem","script-src","default-src"},{"script-src-attr","script-src","default-src"},
        {"style-src-elem","style-src","default-src"},{"style-src-attr","style-src","default-src"},
        {"worker-src","child-src","script-src","default-src"},{"connect-src","default-src"},
        {"manifest-src","default-src"},{"object-src","default-src"},{"frame-src","child-src","default-src"},
        {"media-src","default-src"},{"font-src","default-src"},{"img-src","default-src"},
        {"default-src"}};
    if(!effective)return false;
    for(size_t i=0;i<sizeof lists/sizeof *lists;i++){
        if(strcmp(lists[i][0],effective))continue;
        for(size_t k=0;k<5 && lists[i][k];k++){
            if(!strcmp(name,lists[i][k]))return true;
            if(directive_find(policy,lists[i][k]))return false;
        }
        return false;
    }
    return false;
}
/* The pre-request check of the directive named algorithm (§6.1, §6.7.1.1),
   comparing against list. */
static bool pre_request_check(const char *algorithm,const struct web_csp_directive *list,const struct html_csp_request *r,
                              const struct csp_url *u,const struct csp_url *self){
    if(!strcmp(algorithm,"script-src")||!strcmp(algorithm,"script-src-elem")){
        if(script_like(r->destination)){
            if(nonce_matches_list(r->nonce,list) || integrity_matches_list(r->integrity,list))return true;
            if(value_has(list,"'strict-dynamic'"))return !r->parser_inserted;
        }
    }else if(!strcmp(algorithm,"style-src")||!strcmp(algorithm,"style-src-elem")){
        if(nonce_matches_list(r->nonce,list))return true;
    }
    return u && url_matches_list(u,list,self,r->redirect_count);
}
static bool is_fetch_directive(const char *name){
    static const char *const names[]={"child-src","connect-src","default-src","font-src","frame-src","img-src","manifest-src",
        "media-src","object-src","script-src","script-src-elem","style-src","style-src-elem","worker-src"};
    for(size_t i=0;i<sizeof names/sizeof *names;i++)if(!strcmp(names[i],name))return true;
    return false;
}
/* The list's self-origin as a URL view; false for an opaque one. */
static bool self_view(struct web_html_policy *l,struct csp_url *self){
    return l->self_url && tuple_origin(l->self_url,self);
}
static void violation(html_csp_reporter report,void *opaque,struct web_html_policy_rule *policy,const char *directive,
                      const char *resource,const char *sample,size_t sample_length){
    if(!report)return;
    struct html_csp_violation v={.policy=policy,.directive=directive,.resource=resource,.sample=sample,.sample_length=sample_length};
    report(opaque,&v);
}
/* §6.7.2.2 Does resource hint request violate policy? */
static bool resource_hint_violates(const struct web_html_policy_rule *policy,const struct csp_url *u,const struct csp_url *self,
                                   const struct html_csp_request *r){
    if(!directive_find(policy,"default-src"))return false;
    for(size_t i=0;i<policy->directive_count;i++){
        const struct web_csp_directive *d=&policy->directives[i];
        if(!strcmp(d->name,"default-src") || !is_fetch_directive(d->name))continue;
        if(u && url_matches_list(u,d,self,r->redirect_count))return false;
    }
    return !(u && url_matches_list(u,directive_find(policy,"default-src"),self,r->redirect_count));
}
bool html_policy_request_allowed(web_doc *doc,const struct html_csp_request *r,html_csp_reporter report,void *opaque){
    struct web_html_policy *l=doc?doc->html_policy:NULL;
    if(!l || !l->first || !r)return true;
    const char *name=request_directive(r);
    if(!name)return true;
    struct csp_url u,self;bool have_url=csp_url(r->url,&u),have_self=self_view(l,&self);
    bool allowed=true;
    for(struct web_html_policy_rule *policy=l->first;policy;policy=policy->next){
        const char *violated=NULL;
        if(r->prefetch){if(resource_hint_violates(policy,have_url?&u:NULL,have_self?&self:NULL,r))violated="default-src";}
        else for(size_t i=0;i<policy->directive_count;i++){
            const struct web_csp_directive *d=&policy->directives[i];
            if(!is_fetch_directive(d->name) || !should_execute(name,d->name,policy))continue;
            /* default-src and child-src run the check of the directive they
               stand in for, with their own value. */
            const char *algorithm=!strcmp(d->name,"default-src")||!strcmp(d->name,"child-src")?name:d->name;
            if(!pre_request_check(algorithm,d,r,have_url?&u:NULL,have_self?&self:NULL))violated=d->name;
        }
        if(!violated)continue;
        /* §2.4.2: the violation's effective directive is the request's. */
        violation(report,opaque,policy,name,r->original_url?r->original_url:r->url,NULL,0);
        if(!policy->report_only)allowed=false;
    }
    if(have_url)csp_url_free(&u);if(have_self)csp_url_free(&self);
    return allowed;
}
bool html_policy_nonceable(const node_t *n){
    if(!n || n->type!=N_ELEM)return false;
    bool nonce=false;
    for(int i=0;i<n->nattrs && !nonce;i++)nonce=!n->attrs[i].namespace_uri && n->attrs[i].name && !strcmp(n->attrs[i].name,"nonce");
    if(!nonce)return false;
    if(n->tag!=T_script)return true;
    static const char *const markers[]={"<script","<style","<link"};
    for(int i=0;i<n->nattrs;i++){
        const char *fields[]={n->attrs[i].name,n->attrs[i].value};
        for(int f=0;f<2;f++)for(int m=0;m<3;m++){
            const char *text=fields[f];size_t ml=strlen(markers[m]);
            for(const char *p=text;p && *p;p++)if(!strncasecmp(p,markers[m],ml))return false;
        }
    }
    return true;
}
/* The JavaScript string conversion of WTF-8 text, then UTF-8: an encoded
   surrogate becomes U+FFFD. Returns the bytes to hash, malloc'd or borrowed. */
static const unsigned char *scalar_text(const char *source,size_t length,unsigned char **owned){
    *owned=NULL;
    for(size_t i=0;i+2<length;i++)if((unsigned char)source[i]==0xED && ((unsigned char)source[i+1]&0xE0)==0xA0){
        unsigned char *copy=malloc(length?length:1);if(!copy)return NULL;
        memcpy(copy,source,length);
        for(size_t k=0;k+2<length;k++)if(copy[k]==0xED && (copy[k+1]&0xE0)==0xA0){copy[k]=0xEF;copy[k+1]=0xBF;copy[k+2]=0xBD;k+=2;}
        *owned=copy;return copy;
    }
    return (const unsigned char *)source;
}
static void base64_encode(const unsigned char *data,size_t n,char *out){
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o=0;
    for(size_t i=0;i<n;i+=3){
        unsigned v=(unsigned)data[i]<<16|(i+1<n?(unsigned)data[i+1]<<8:0)|(i+2<n?data[i+2]:0);
        out[o++]=alphabet[v>>18&63];out[o++]=alphabet[v>>12&63];
        out[o++]=i+1<n?alphabet[v>>6&63]:'=';out[o++]=i+2<n?alphabet[v&63]:'=';
    }
    out[o]=0;
}
/* The base64 digest of source under the algorithm (256, 384, 512). */
static void digest(int algorithm,const unsigned char *source,size_t length,char out[96]){
    unsigned char hash[64];size_t n=0;
    if(algorithm==256){br_sha256_context c;br_sha256_init(&c);br_sha256_update(&c,source,length);br_sha256_out(&c,hash);n=32;}
    else if(algorithm==384){br_sha384_context c;br_sha384_init(&c);br_sha384_update(&c,source,length);br_sha384_out(&c,hash);n=48;}
    else{br_sha512_context c;br_sha512_init(&c);br_sha512_update(&c,source,length);br_sha512_out(&c,hash);n=64;}
    base64_encode(hash,n,out);
}
/* §6.7.3.2 Does a source list allow all inline behavior for type? */
static bool allows_all_inline(const struct web_csp_directive *list,enum html_csp_inline type){
    bool unsafe_inline=false;const char *v;size_t n;
    for(size_t i=0;i<list->count;i++){
        const char *e=list->value[i];
        if(nonce_source(e,&n) || hash_source(e,&v,&n))return false;
        if((type==HTML_CSP_SCRIPT||type==HTML_CSP_SCRIPT_ATTRIBUTE||type==HTML_CSP_NAVIGATION) && !strcasecmp(e,"'strict-dynamic'"))return false;
        if(!strcasecmp(e,"'unsafe-inline'"))unsafe_inline=true;
    }
    return unsafe_inline;
}
/* §6.7.3.3 Does element match source list for type and source? hashes
   carries the digests already computed for this source. */
struct inline_digests { const unsigned char *bytes; size_t length; bool ready[3]; char value[3][96]; };
static bool element_matches_list(const struct html_csp_element *element,const struct web_csp_directive *list,
                                 enum html_csp_inline type,struct inline_digests *digests){
    if(allows_all_inline(list,type))return true;
    size_t n;const char *v;
    bool element_type=type==HTML_CSP_SCRIPT||type==HTML_CSP_STYLE;
    if(element_type && element && element->nonceable && element->nonce){
        size_t length=strlen(element->nonce);
        for(size_t i=0;i<list->count;i++){
            const char *value=nonce_source(list->value[i],&n);
            if(value && n==length && !memcmp(value,element->nonce,n))return true;
        }
    }
    if(!element_type && !value_has(list,"'unsafe-hashes'"))return false;
    for(size_t i=0;i<list->count;i++){
        const char *e=list->value[i];
        if(!strcasecmp(e,"'strict-dynamic'")){
            if(type==HTML_CSP_SCRIPT && element && !element->parser_inserted)return true;
            continue;
        }
        int algorithm=hash_source(e,&v,&n);
        if(!algorithm || !digests->bytes)continue;
        int slot=algorithm==256?0:algorithm==384?1:2;
        if(!digests->ready[slot]){digest(algorithm,digests->bytes,digests->length,digests->value[slot]);digests->ready[slot]=true;}
        const char *actual=digests->value[slot];
        if(strlen(actual)!=n)continue;
        bool same=true;
        for(size_t k=0;k<n && same;k++){char c=v[k]=='-'?'+':v[k]=='_'?'/':v[k];same=c==actual[k];}
        if(same)return true;
    }
    return false;
}
/* The first 40 UTF-16 code units of UTF-8 text, as a byte length. */
static size_t sample_length(const char *text,size_t length){
    size_t units=0,i=0;
    while(i<length){
        unsigned char c=(unsigned char)text[i];
        size_t bytes=c<0x80?1:c<0xE0?2:c<0xF0?3:4,width=bytes==4?2:1;
        if(units+width>40 || i+bytes>length)break;
        units+=width;i+=bytes;
    }
    return i;
}
bool html_policy_inline_allowed(web_doc *doc,enum html_csp_inline type,const struct html_csp_element *element,
                                const char *source,size_t length,html_csp_reporter report,void *opaque){
    struct web_html_policy *l=doc?doc->html_policy:NULL;
    if(!l || !l->first)return true;
    if(!source)length=0;
    const char *name=inline_directive(type);
    unsigned char *owned=NULL;
    struct inline_digests digests={.bytes=scalar_text(source?source:"",length,&owned),.length=length};
    bool allowed=true;
    for(struct web_html_policy_rule *policy=l->first;policy;policy=policy->next){
        for(size_t i=0;i<policy->directive_count;i++){
            const struct web_csp_directive *d=&policy->directives[i];
            /* The directives with an inline check (§6.1.3.3, §6.1.10.3–§6.1.15.1). */
            if(strcmp(d->name,"default-src") && strcmp(d->name,"script-src") && strcmp(d->name,"script-src-elem") &&
               strcmp(d->name,"script-src-attr") && strcmp(d->name,"style-src") && strcmp(d->name,"style-src-elem") &&
               strcmp(d->name,"style-src-attr"))continue;
            if(!should_execute(name,d->name,policy))continue;
            if(element_matches_list(element,d,type,&digests))continue;
            bool sample=value_has(d,"'report-sample'");
            violation(report,opaque,policy,name,"inline",sample?source:NULL,sample?sample_length(source,length):0);
            if(!policy->report_only)allowed=false;
        }
    }
    free(owned);
    return allowed;
}
static bool tt_eval_skips(const struct web_html_policy *l){
    for(const struct web_html_policy_rule *r=l->first;r;r=r->next)if(r->require_script && !r->report_only)return true;
    return false;
}
bool html_policy_eval_allowed(web_doc *doc,const char *source,size_t length,html_csp_reporter report,void *opaque){
    struct web_html_policy *l=doc?doc->html_policy:NULL;
    if(!l || !l->first)return true;
    bool required=tt_eval_skips(l),allowed=true;
    for(struct web_html_policy_rule *policy=l->first;policy;policy=policy->next){
        const struct web_csp_directive *list=directive_find(policy,"script-src");
        if(!list)list=directive_find(policy,"default-src");
        if(!list)continue;
        if(required && value_has(list,"'trusted-types-eval'"))continue;
        if(value_has(list,"'unsafe-eval'"))continue;
        bool sample=value_has(list,"'report-sample'") && source;
        violation(report,opaque,policy,"script-src","eval",sample?source:NULL,sample?sample_length(source,length):0);
        if(!policy->report_only)allowed=false;
    }
    return allowed;
}
bool html_policy_base_allowed(web_doc *doc,const char *url,html_csp_reporter report,void *opaque){
    struct web_html_policy *l=doc?doc->html_policy:NULL;
    if(!l || !l->first)return true;
    struct csp_url u,self;bool have_url=csp_url(url,&u),have_self=self_view(l,&self);
    bool allowed=true;
    for(struct web_html_policy_rule *policy=l->first;policy;policy=policy->next){
        const struct web_csp_directive *list=directive_find(policy,"base-uri");
        if(!list || (have_url && url_matches_list(&u,list,have_self?&self:NULL,0)))continue;
        violation(report,opaque,policy,"base-uri","inline",NULL,0);
        if(!policy->report_only){allowed=false;break;}
    }
    if(have_url)csp_url_free(&u);if(have_self)csp_url_free(&self);
    return allowed;
}
bool html_policy_navigation_allowed(web_doc *doc,const struct html_csp_request *r,bool form_submission,
                                    html_csp_reporter report,void *opaque){
    struct web_html_policy *l=doc?doc->html_policy:NULL;
    if(!l || !l->first || !r)return true;
    bool allowed=true;
    if(form_submission){
        struct csp_url u,self;bool have_url=csp_url(r->url,&u),have_self=self_view(l,&self);
        for(struct web_html_policy_rule *policy=l->first;policy;policy=policy->next){
            const struct web_csp_directive *list=directive_find(policy,"form-action");
            if(!list || (have_url && url_matches_list(&u,list,have_self?&self:NULL,r->redirect_count)))continue;
            violation(report,opaque,policy,"form-action",r->original_url?r->original_url:r->url,NULL,0);
            if(!policy->report_only)allowed=false;
        }
        if(have_url)csp_url_free(&u);if(have_self)csp_url_free(&self);
    }
    if(allowed && r->url && !strncasecmp(r->url,"javascript:",11))
        allowed=html_policy_inline_allowed(doc,HTML_CSP_NAVIGATION,NULL,r->url,strlen(r->url),report,opaque);
    return allowed;
}
bool html_policy_worker_allowed(const web_doc *doc){
    for(const struct web_html_policy_rule *r=doc && doc->html_policy?doc->html_policy->first:NULL;r;r=r->next){
        const struct web_csp_directive *list=directive_find(r,"sandbox");
        if(!list || r->report_only || r->from_meta)continue;
        sbuf text={0};
        for(size_t i=0;i<list->count;i++){if(i)sb_putc(&text,' ');sb_puts(&text,list->value[i]);}
        uint32_t flags=sandbox_parse(list->count?sb_cstr(&text):"",NULL,0);sb_free(&text);
        if(flags&(SB_SCRIPTS|SB_ORIGIN))return false;
    }
    return true;
}
bool html_policy_upgrade_insecure(const web_doc *doc){
    for(const struct web_html_policy_rule *r=doc && doc->html_policy?doc->html_policy->first:NULL;r;r=r->next)
        if(!r->report_only && directive_find(r,"upgrade-insecure-requests"))return true;
    return false;
}
char *html_policy_report_url(web_doc *doc,const char *url){
    if(!url)return strdup("");
    (void)doc;
    struct csp_url u;
    if(!csp_url(url,&u))return strdup("");
    char *out;
    if(!span_is(u.scheme,u.scheme_n,"http") && !span_is(u.scheme,u.scheme_n,"https"))out=copy_text(u.scheme,u.scheme+u.scheme_n);
    else{
        /* The serialization without fragment, username and password. */
        const char *h=u.parsed.href;const size_t *m=u.parsed.mark;
        sbuf b={0};sb_put(&b,h,m[WEB_URL_PROTOCOL_END]);sb_puts(&b,"//");
        sb_put(&b,h+m[WEB_URL_HOST_START],m[WEB_URL_FRAGMENT_START]-m[WEB_URL_HOST_START]);
        out=sb_cstr(&b);
        if(!out)sb_free(&b);
    }
    csp_url_free(&u);
    return out;
}
