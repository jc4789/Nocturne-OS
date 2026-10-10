/* Trusted Types CSP directives are native document policy, not JS properties.
   Each header field/list policy is retained separately for intersection. */
#include "html_policy.h"
#include <stdio.h>
bool html_policy_requires_script(const web_doc *d) {
    for(const struct web_html_policy_rule *r=d && d->html_policy?d->html_policy->first:NULL;r;r=r->next)
        if(r->require_script)return true;
    return false;
}
static void rule_free(struct web_html_policy_rule *rule) {
    for(size_t i=0;i<rule->count;i++)free(rule->names[i]);
    for(size_t i=0;i<rule->report_uri_count;i++)free(rule->report_uris[i]);
    free(rule->report_uris);free(rule->original_policy);free(rule->base_url);free(rule->report_to);free(rule->names);free(rule);
}
void html_policy_free(web_doc *d) {
    if(!d || !d->html_policy)return;
    struct web_html_policy_rule *r=d->html_policy->first;
    while(r){struct web_html_policy_rule *next=r->next;rule_free(r);r=next;}
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
static bool policy_name(const char *begin,const char *end){
    if(token(begin,end,"*"))return true;if(begin==end)return false;
    for(const char *p=begin;p<end;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-#=_/@.%",*p)))return false;
    return true;
}
static bool report_uri(struct web_html_policy_rule *r,const char *begin,const char *end){
    char *relative=copy_text(begin,end);if(!relative)return false;char absolute[HTTP_URL_MAX];
    bool valid=url_resolve(r->base_url,relative,absolute,sizeof absolute);free(relative);
    if(!valid || (strncasecmp(absolute,"https://",8)&&strncasecmp(absolute,"http://",7)))return true;
    const char *authority=strstr(absolute,"://")+3,*path=authority+strcspn(authority,"/?#");
    if(memchr(authority,'@',path-authority))return true;
    if(r->report_uri_count==SIZE_MAX/sizeof *r->report_uris)return false;
    char **grown=realloc(r->report_uris,(r->report_uri_count+1)*sizeof *grown);if(!grown)return false;r->report_uris=grown;
    char *value=copy_text(absolute,absolute+strlen(absolute));if(!value)return false;r->report_uris[r->report_uri_count++]=value;return true;
}
static bool append(web_doc *d,const char *begin,const char *end,bool report,bool meta,const char *base) {
    struct web_html_policy_rule *r=calloc(1,sizeof *r);
    if(!r)return false;
    while(begin<end&&is_space((unsigned char)*begin))begin++;while(end>begin&&is_space((unsigned char)end[-1]))end--;
    r->original_policy=copy_text(begin,end);base=base?base:d->url?d->url:"";r->base_url=copy_text(base,base+strlen(base));
    if(!r->original_policy||!r->base_url){rule_free(r);return false;}
    r->report_only=report;r->from_meta=meta;bool seen_require=false,seen_names=false,seen_uri=false,seen_to=false;
    while(begin<end){
        while(begin<end && (is_space((unsigned char)*begin)||*begin==';'))begin++;
        const char *name=begin;while(begin<end && !is_space((unsigned char)*begin)&&*begin!=';')begin++;
        const char *name_end=begin,*value=begin;while(begin<end && *begin!=';')begin++;
        bool require=!seen_require && named(name,name_end,"require-trusted-types-for");
        bool names=!seen_names && named(name,name_end,"trusted-types");
        bool uri=!seen_uri && named(name,name_end,"report-uri"),to=!seen_to && named(name,name_end,"report-to");
        if(require)seen_require=true;
        if(names){seen_names=true;r->has_names=true;}
        if(uri)seen_uri=true;if(to)seen_to=true;
        while((require||names||uri||to) && value<begin){
            while(value<begin && is_space((unsigned char)*value))value++;
            const char *t=value;while(value<begin && !is_space((unsigned char)*value))value++;
            if(t==value)break;
            if(require && token(t,value,"'script'"))r->require_script=true;
            if(names){
                if(token(t,value,"'allow-duplicates'")){r->allow_duplicates=true;continue;}
                if(token(t,value,"'none'"))continue;
                if(!policy_name(t,value))continue;
                if(r->count==SIZE_MAX/sizeof *r->names){rule_free(r);return false;}
                char **grown=realloc(r->names,(r->count+1)*sizeof *grown);
                if(!grown){rule_free(r);return false;}r->names=grown;
                size_t n=value-t;char *s=malloc(n+1);
                if(!s){rule_free(r);return false;}memcpy(s,t,n);s[n]=0;r->names[r->count++]=s;
            }
            if(uri&&!meta&&!report_uri(r,t,value)){rule_free(r);return false;}
            if(to&&!meta&&!r->report_to){r->report_to=copy_text(t,value);if(!r->report_to){rule_free(r);return false;}}
        }
    }
    if(!r->has_names && !r->require_script){rule_free(r);return true;}
    if(!d->html_policy){d->html_policy=calloc(1,sizeof *d->html_policy);if(!d->html_policy){rule_free(r);return false;}}
    if(d->html_policy->last)d->html_policy->last->next=r;else d->html_policy->first=r;
    d->html_policy->last=r;return true;
}
static bool list(web_doc *d,const char *begin,const char *end,bool report,bool meta){
    while(begin<end){const char *next=begin;while(next<end && *next!=',')next++;
        if(!append(d,begin,next,report,meta,NULL))return false;begin=next<end?next+1:end;}
    return true;
}
bool html_policy_init(web_doc *d,const char *headers,const web_doc *inherit){
    if(inherit && inherit->html_policy){
        for(const struct web_html_policy_rule *r=inherit->html_policy->first;r;r=r->next){
            if(!append(d,r->original_policy,r->original_policy+strlen(r->original_policy),r->report_only,r->from_meta,r->base_url))return false;
        }
    }
    for(const char *p=headers?headers:"";*p;){
        const char *end=p;while(*end && *end!='\r' && *end!='\n')end++;
        const char *colon=memchr(p,':',end-p);
        if(colon){bool enforce=named(p,colon,"Content-Security-Policy"),report=named(p,colon,"Content-Security-Policy-Report-Only");
            if((enforce||report) && !list(d,colon+1,end,report,false))return false;}
        p=end;while(*p=='\r'||*p=='\n')p++;
    }
    return true;
}
bool html_policy_meta(web_doc *d,const char *content,size_t length){return d && content && list(d,content,content+length,false,true);}
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
   framing preserves the original policy and its original URL resolution base. */
char *html_policy_snapshot(const web_doc *d){
    sbuf text={0};sb_puts(&text,"NCTT1\n");
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
    if(!d||!text||strncmp(text,"NCTT1\n",6))return false;const char *p=text+6,*end=text+strlen(text);
    while(p<end){size_t report,meta,length,base_length;
        if(!snapshot_number(&p,end,&report,' ')||!snapshot_number(&p,end,&meta,' ')||!snapshot_number(&p,end,&length,' ')||!snapshot_number(&p,end,&base_length,'\n')||report>1||meta>1)return false;
        if(length>(size_t)(end-p)||base_length>(size_t)(end-p)-length||(size_t)(end-p)-length-base_length<1||p[length+base_length]!='\n')return false;
        char *base=copy_text(p+length,p+length+base_length);if(!base)return false;
        bool ok=append(d,p,p+length,report!=0,meta!=0,base);free(base);if(!ok)return false;p+=length+base_length+1;
    }
    return true;
}
