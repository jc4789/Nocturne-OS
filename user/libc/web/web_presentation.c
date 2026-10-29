#include "web_presentation.h"
#include "avmedia.h"
#include "frame.h"
#include "js_canvas.h"
#include "js_webgl.h"
#include "web_dialog.h"
#include "nocturne.h"
#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
/* The Permissions Policy features this owner decides: 1 fullscreen and 3
   focus-without-user-activation (default allowlist 'self'), 2 picture-in-picture ('*'). */
static unsigned feature_bit(int kind){return kind==1?1u:kind==2?2u:kind==3?4u:0;}
static const char *feature_name(int kind){return kind==1?"fullscreen":kind==2?"picture-in-picture":"focus-without-user-activation";}
static web_doc *top_document(web_doc *d){while(d&&d->frame_parent)d=d->frame_parent;return d;}
static bool active(web_doc *d){
    for(web_doc *at=d;at;at=at->frame_parent){
        if(!at->live||at->inert||(at->sandbox_flags&SB_SCRIPTS))return false;
        if(at->frame_parent){struct web_frame *f=web_frame_find(at->frame_parent,at->frame_element);
            if(!f||f->detached||f->document!=at||!doc_node_connected(f->element))return false;}
    }return d!=NULL;
}
web_doc *web_presentation_root(web_doc *d){return active(d)?top_document(d):NULL;}
static bool valid(web_doc *d,node_t *n){return active(d)&&n&&n->owner==d&&n->type==N_ELEM&&doc_node_connected(n);}
static bool origin_is(const char *a,const char *b){return a&&b&&!strcmp(a,b);}
static char *document_origin(const web_doc *d){char *out=NULL;if(d&&(d->sandbox_flags&SB_ORIGIN)==0&&web_url_origin(web_effective_url((web_doc *)d),&out)==WEB_URL_TUPLE)return out;free(out);return NULL;}
static bool origin_match(const char *text,size_t size,const char *origin,bool pattern){
    if(!origin)return false;
    if(pattern&&size==6&&!memcmp(text,"https:",6))return web_url_has_scheme(origin,"https");
    if(pattern&&size==5&&!memcmp(text,"http:",5))return web_url_has_scheme(origin,"http")||web_url_has_scheme(origin,"https");
    char *value=malloc(size+1);if(!value)return false;memcpy(value,text,size);value[size]=0;
    if(!pattern){char *parsed=NULL;bool matched=web_url_origin(value,&parsed)==WEB_URL_TUPLE&&origin_is(parsed,origin);free(parsed);free(value);return matched;}
    char *wild=strstr(value,"://*.");bool domain_wild=wild!=NULL,port_wild=false,matched=false;
    if(wild){size_t prefix=(size_t)(wild-value)+3;memmove(value+prefix,value+prefix+2,strlen(value+prefix+2)+1);}
    char *scheme=strstr(value,"://");if(scheme){char *authority=scheme+3,*end=authority;while(*end&&*end!='/'&&*end!='?'&&*end!='#')end++;
        if(end-authority>=2&&end[-2]==':'&&end[-1]=='*'){port_wild=true;memmove(end-2,end,strlen(end)+1);}}
    /* The pattern, its wildcards taken out, and the origin are URLs: their schemes, hosts and
       ports come from their records. */
    struct web_url source,target;
    if(web_url_read(value,strlen(value),&source)==1){
        if(web_url_read(origin,strlen(origin),&target)==1){
            bool source_tls=web_url_scheme_is(&source,"https"),target_tls=web_url_scheme_is(&target,"https");
            bool http=web_url_http_scheme(&source)&&web_url_http_scheme(&target);
            size_t path_length,a,b;const char *path=web_url_target(&source,&path_length);
            const char *source_host=web_url_host_text(&source,&a),*target_host=web_url_host_text(&target,&b);
            int source_port=web_url_port_or_default(&source),target_port=web_url_port_or_default(&target);
            bool host=a==b&&!memcmp(source_host,target_host,a);
            if(domain_wild)host=b>a+1&&target_host[b-a-1]=='.'&&!memcmp(target_host+b-a,source_host,a);
            matched=http&&path_length==1&&*path=='/'&&host&&(!source_tls||target_tls)&&
                (port_wild||source_port==target_port||(!source_tls&&source_port==80&&target_tls&&target_port==443));
            web_url_free(&target);
        }
        web_url_free(&source);
    }
    free(value);return matched;
}
static bool policy_token(const char *text,size_t size,const char *origin,const char *self,const char *src,bool header){
    if(size==1&&text[0]=='*')return true;
    if((header&&size==4&&!memcmp(text,"self",4))||(!header&&size==6&&!strncasecmp(text,"'self'",6)))return origin_is(origin,self);
    if(!header&&size==5&&!strncasecmp(text,"'src'",5))return origin_is(origin,src);
    if(!header&&size==6&&!strncasecmp(text,"'none'",6))return false;
    return origin_match(text,size,origin,header);
}
/* Parameters do not contribute origins. Consume them without mistaking a
   parameter's string/token for a second allowlist entry. */
static bool skip_parameters(const char **cursor,const char *end){
    const char *p=*cursor;
    while(p<end&&*p==';'){
        p++;while(p<end&&*p==' ')p++;
        if(p==end||!((*p>='a'&&*p<='z')||*p=='*'))return false;
        do{p++;}while(p<end&&((*p>='a'&&*p<='z')||isdigit((unsigned char)*p)||*p=='_'||*p=='-'||*p=='.'||*p=='*'));
        if(p<end&&*p=='='){
            p++;if(p==end)return false;
            if(*p=='"'){p++;bool closed=false;while(p<end){char c=*p++;if(c=='"'){closed=true;break;}
                    if(c=='\\'){if(p==end||(*p!='"'&&*p!='\\'))return false;p++;}
                    else if((unsigned char)c<0x20||(unsigned char)c>0x7e)return false;}
                if(!closed)return false;
            }else{const char *start=p;while(p<end&&*p!=';'&&*p!=' '&&*p!='\t'&&*p!=')')p++;
                if(p==start)return false;}
        }
    }
    *cursor=p;return true;
}
/* The supported features use Structured Fields strings/tokens/inner lists.
   Skip dictionary separators only outside quoted strings and parentheses. */
static bool header_feature(const char *policy,int kind,const char *origin,const char *self){
    if(!policy||!*policy)return true;const char *p=policy;bool result=true;size_t feature_size=strlen(feature_name(kind));
    while(*p){while(*p==' '||*p=='\t'||*p==',')p++;if(!*p)break;const char *start=p;
        bool quote=false,escape=false;unsigned depth=0;
        while(*p){char c=*p;if(quote){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quote=false;}
            else if(c=='"')quote=true;else if(c=='(')depth++;else if(c==')'){if(!depth)return false;depth--;}
            else if(c==','&&!depth)break;p++;}
        if(quote||depth)return false;const char *end=p,*name=start;while(name<end&&(isalnum((unsigned char)*name)||*name=='-'||*name=='_'||*name=='*'))name++;
        if((size_t)(name-start)!=feature_size||memcmp(start,feature_name(kind),feature_size)){if(*p)p++;continue;}
        const char *v=name;while(v<end&&(*v==' '||*v=='\t'))v++;if(v==end||*v++!='=')return false;
        while(v<end&&(*v==' '||*v=='\t'))v++;bool list=v<end&&*v=='(';if(list)v++;
        bool match=false,closed=!list;
        while(v<end){while(v<end&&(*v==' '||*v=='\t'))v++;if(v==end)break;if(list&&*v==')'){closed=true;v++;break;}
            if(!list&&*v==';')break;const char *token=v;size_t n=0;
            if(*v=='"'){v++;token=v;while(v<end&&*v!='"'&&*v!='\\')v++;if(v==end||*v!='"')return false;n=(size_t)(v-token);v++;
                match=origin_match(token,n,origin,true)||match;
            }else{while(v<end&&*v!=' '&&*v!='\t'&&*v!=')'&&*v!=';')v++;n=(size_t)(v-token);if(!n)return false;match=policy_token(token,n,origin,self,NULL,true)||match;}
            if(!skip_parameters(&v,end))return false;
            if(!list){closed=true;break;}
        }
        if(!closed)return false;if(!skip_parameters(&v,end))return false;
        while(v<end&&(*v==' '||*v=='\t'))v++;if(v!=end)return false;
        result=match;if(*p)p++;
    }return result;
}
static bool declared_feature(const web_doc *d,int kind,const char *origin){
    if(!d||!d->presentation_policy_ready||!(d->presentation_inherited&feature_bit(kind)))return false;
    char *self=document_origin(d);bool allowed=header_feature(d->presentation_policy,kind,origin,self);free(self);return allowed;
}
static bool container_feature(node_t *container,int kind,const char *origin,const char *self,const char *src){
    if(container->foreign||container->tag!=T_iframe)return kind==2||origin_is(origin,self);
    const char *allow=node_attr(container,"allow"),*p=allow?allow:"";size_t feature_size=strlen(feature_name(kind));
    bool declared=false,result=false;
    while(*p){while(*p&&(*p==';'||isspace((unsigned char)*p)))p++;const char *name=p;while(*p&&*p!=';'&&!isspace((unsigned char)*p))p++;
        bool found=(size_t)(p-name)==feature_size&&!memcmp(name,feature_name(kind),feature_size),any=false,match=false;
        while(*p&&*p!=';'){while(*p&&*p!=';'&&isspace((unsigned char)*p))p++;const char *token=p;while(*p&&*p!=';'&&!isspace((unsigned char)*p))p++;
            if(p!=token){any=true;if(found)match=policy_token(token,(size_t)(p-token),origin,self,src,false)||match;}}
        /* Each declaration updates the ordered-map entry; a later explicit
           denial must not be hidden by an earlier permission. */
        if(found){declared=true;result=any?match:origin_is(origin,src);}
    }
    if(declared)return result;
    if(kind==1&&node_attr(container,"allowfullscreen"))return true;
    return kind==2||origin_is(origin,self);
}
static char *declared_origin(web_doc *parent,node_t *container,const char *self){
    if(node_attr(container,"srcdoc"))return self?strdup(self):NULL;
    const char *src=node_attr(container,"src");if(!src||!*src)return self?strdup(self):NULL;
    /* The iframe's src as its navigation parses it (encoding-parse a URL). */
    char *url=NULL,*origin=NULL;if(doc_encoding_parse_url(parent,src,&url)==1){
        if(!strcmp(url,"about:blank")||!strncmp(url,"about:blank#",12))origin=self?strdup(self):NULL;
        else web_url_origin(url,&origin);}
    free(url);return origin;
}
bool web_presentation_policy_init(web_doc *d,const char *headers,const web_doc *inherit){
    if(!d)return false;char *policy=NULL;size_t used=0;
    if(inherit&&inherit->presentation_policy){policy=strdup(inherit->presentation_policy);if(!policy)return false;used=strlen(policy);}
    if(headers){for(const char *p=headers;*p;){const char *end=strchr(p,'\n');if(!end)end=p+strlen(p);const char *stop=end;while(stop>p&&(stop[-1]=='\r'||stop[-1]==' '||stop[-1]=='\t'))stop--;
            const char *colon=memchr(p,':',(size_t)(stop-p));if(colon&&(size_t)(colon-p)==18&&!strncasecmp(p,"Permissions-Policy",18)){
                const char *value=colon+1;while(value<stop&&(*value==' '||*value=='\t'))value++;size_t n=(size_t)(stop-value);
                if(n>SIZE_MAX-used-2){free(policy);return false;}char *next=realloc(policy,used+n+2);if(!next){free(policy);return false;}policy=next;if(used)policy[used++]=',';memcpy(policy+used,value,n);used+=n;policy[used]=0;}
            p=*end?end+1:end;}}
    unsigned inherited=7;
    if(d->frame_parent&&d->frame_element){web_doc *parent=d->frame_parent;char *self=document_origin(parent),*origin=document_origin(d),*src=declared_origin(parent,d->frame_element,self);
        for(int kind=1;kind<=3;kind++)if(!declared_feature(parent,kind,self)||!declared_feature(parent,kind,origin)||!container_feature(d->frame_element,kind,origin,self,src))inherited&=~feature_bit(kind);
        free(self);free(origin);free(src);}
    free(d->presentation_policy);d->presentation_policy=policy;d->presentation_inherited=(uint8_t)inherited;d->presentation_policy_ready=true;return true;
}
/* Permissions Policy "allowed to use" focus-without-user-activation, for HTML's allow focus
   steps (doc.c web_allow_focus): a fully active document whose policy enables the feature for
   its own origin. */
bool web_presentation_focus_allowed(web_doc *d){
    if(!d||!web_doc_fully_active(d))return false;
    char *origin=document_origin(d);bool enabled=declared_feature(d,3,origin);free(origin);return enabled;
}
bool web_presentation_enabled(web_doc *d,int kind,const struct web_host *host){
    if(!feature_bit(kind)||!host||!host->presentation||!active(d))return false;
    char *origin=document_origin(d);bool enabled=declared_feature(d,kind,origin);free(origin);return enabled;
}
static bool contains(node_t *root,node_t *node){for(;node;node=doc_shadow_parent(node))if(root==node)return true;return false;}
static bool ancestor(web_doc *owner,web_doc *document){for(;owner;owner=owner->frame_parent)if(owner==document)return true;return false;}
static void events_free(struct web_presentation_event *events){while(events){struct web_presentation_event *event=events;events=event->next;free(event);}}
static struct web_presentation_event *events_reserve(web_doc *d,node_t *n,int kind,bool chain){
    struct web_presentation_event *first=NULL,**tail=&first;
    do{struct web_presentation_event *event=calloc(1,sizeof *event);if(!event){events_free(first);return NULL;}
        event->document=d;event->node=n;event->kind=kind;event->width=d->pip_width;event->height=d->pip_height;event->session=kind==1?0:d->pip_session;*tail=event;tail=&event->next;
        n=d->frame_element;d=d->frame_parent;
    }while(chain&&d);return first;
}
static void events_publish(struct web_presentation_event *events){
    while(events){struct web_presentation_event *event=events;events=event->next;event->next=NULL;web_doc *d=event->document;
        if(d->presentation_events_tail)d->presentation_events_tail->next=event;else d->presentation_events=event;
        d->presentation_events_tail=event;}
}
const struct web_presentation_event *web_presentation_event_peek(web_doc *d){return d?d->presentation_events:NULL;}
void web_presentation_event_pop(web_doc *d){if(d&&d->presentation_events){struct web_presentation_event *event=d->presentation_events;d->presentation_events=event->next;if(!d->presentation_events)d->presentation_events_tail=NULL;free(event);}}
node_t *web_presentation_element(web_doc *d,int kind){
    if(!d)return NULL;node_t *n=kind==1?d->fullscreen_element:d->pip_element;return valid(d,n)?n:NULL;
}
bool web_presentation_matches(node_t *n){
    if(!n||n->type!=N_ELEM||!n->owner)return false;
    node_t *full=n->owner->fullscreen_element;if(full==n)return true;
    if(!full||!n->shadow_root)return false;
    /* Retarget only across native shadow roots, not ordinary ancestors. */
    for(node_t *at=full;at;){node_t *root=doc_node_root(at,false);
        if(!root||!root->shadow_host)return false;at=root->shadow_host;if(at==n)return true;}
    return false;
}
void web_presentation_resize(web_doc *d,int w,int h){if(d&&w>0&&h>0&&(d->pip_width!=w||d->pip_height!=h)){
    d->pip_width=w;d->pip_height=h;d->presentation_revision++;
    if(d->pip_element){struct web_presentation_event *event=d->presentation_events_tail;
        if(event&&event->kind==4&&event->node==d->pip_element){event->width=w;event->height=h;}
        else{event=events_reserve(d,d->pip_element,4,false);if(event)events_publish(event);}}
}}
void web_presentation_size(web_doc *d,int *w,int *h){if(w)*w=d?d->pip_width:0;if(h)*h=d?d->pip_height:0;}
static void changed(web_doc *d,node_t *n){
    d->presentation_revision++;d->dirty=d->paint_dirty=true;if(!n)return;css_mark_dirty(d,n);
    /* Entry and exit change :fullscreen on each retargeted shadow host. The
       native root walk is proportional to embedding depth, not all nodes. */
    for(node_t *at=n;at;){node_t *root=doc_node_root(at,false);
        if(!root||!root->shadow_host)break;at=root->shadow_host;css_mark_dirty(d,at);}
}
int web_presentation_request(web_doc *d,node_t *n,int kind,const struct web_host *host){
    if(!valid(d,n))return WEB_PRESENT_INACTIVE;
    if(kind==1&&n->namespace_id!=NS_HTML&&
       !(n->namespace_id==NS_SVG&&n->name&&!strcmp(n->name,"svg"))&&
       !(n->namespace_id==NS_MATHML&&n->name&&!strcmp(n->name,"math")))return WEB_PRESENT_UNSUPPORTED;
    if((kind!=1&&kind!=2)||n->tag==T_dialog||!host||!host->presentation)return WEB_PRESENT_UNSUPPORTED;
    if(!web_presentation_enabled(d,kind,host))return WEB_PRESENT_POLICY;
    if(kind==2&&(n->foreign||n->tag!=T_video||node_attr(n,"disablepictureinpicture")||!web_avmedia_pip_ready(d,n)))return WEB_PRESENT_MEDIA;
    if(kind==1&&d->fullscreen_element==n)return WEB_PRESENT_OK;
    if(kind==2&&d->pip_element==n)return WEB_PRESENT_OK;
    if(kind==2&&d->pip_session==UINT64_MAX)return WEB_PRESENT_BACKEND;
    bool consume_activation=kind==1||!d->pip_element;
    if(consume_activation&&d->sandbox_activation_until<=uptime_ms())return WEB_PRESENT_ACTIVATION;
    web_doc *top=top_document(d),*previous=kind==1?top->fullscreen_owner:top->pip_owner;
    struct web_presentation_event *events=events_reserve(d,n,kind==1?1:2,kind==1);if(!events)return WEB_PRESENT_BACKEND;
    if(previous&&!web_presentation_exit(previous,kind)){events_free(events);return WEB_PRESENT_BACKEND;}
    /* PiP and fullscreen cannot retain simultaneous native presentation of
       this video. Release the fullscreen lease through its real owner. */
    if(kind==2&&d->fullscreen_element==n&&!web_presentation_exit(d,1)){events_free(events);return WEB_PRESENT_BACKEND;}
    if(!active(d)||!valid(d,n)||!web_presentation_enabled(d,kind,host)){events_free(events);return WEB_PRESENT_INACTIVE;}
    if(!host->presentation(host->opaque,d,n,kind)){events_free(events);return WEB_PRESENT_BACKEND;}
    d->presentation_host=host->presentation;d->presentation_opaque=host->opaque;
    if(kind==1){top->fullscreen_owner=d;node_t *element=n;
        for(web_doc *at=d;at;at=at->frame_parent){at->fullscreen_element=element;changed(at,element);element=at->frame_element;}}
    else{top->pip_owner=d;d->pip_element=n;events->session=++d->pip_session;events->width=d->pip_width;events->height=d->pip_height;changed(d,n);}
    events_publish(events);
    if(consume_activation)web_user_activation_consume(top);
    return WEB_PRESENT_OK;
}
bool web_presentation_exit(web_doc *d,int kind){
    if(!d||!feature_bit(kind))return false;web_doc *top=top_document(d),*owner=kind==1?top->fullscreen_owner:top->pip_owner;
    if(owner&&ancestor(owner,d))d=owner;
    node_t *n=kind==1?d->fullscreen_element:d->pip_element;
    if(!n)return true;
    struct web_presentation_event *events=events_reserve(d,n,kind==1?1:3,kind==1);if(!events)return false;
    if(d->presentation_host&&!d->presentation_host(d->presentation_opaque,d,n,kind==1?0:3)){events_free(events);return false;}
    if(kind==1){top->fullscreen_owner=NULL;for(web_doc *at=d;at;at=at->frame_parent){node_t *element=at->fullscreen_element;at->fullscreen_element=NULL;changed(at,element);}}
    else{top->pip_owner=NULL;d->pip_element=NULL;changed(d,n);}
    events_publish(events);return true;
}
void web_presentation_sync(web_doc *d){if(!d)return;if(d->fullscreen_element&&!valid(d,d->fullscreen_element))web_presentation_exit(d,1);if(d->pip_element&&!valid(d,d->pip_element))web_presentation_exit(d,2);}
void web_presentation_removed(web_doc *d,node_t *subtree){
    if(!d||!subtree)return;
    node_t *full=d->fullscreen_element,*pip=d->pip_element;
    if(full&&contains(subtree,full))web_presentation_exit(d,1);
    if(pip&&contains(subtree,pip))web_presentation_exit(d,2);
    web_doc *owner=top_document(d)->pip_owner;
    for(web_doc *at=owner;at&&at->frame_parent;at=at->frame_parent)if(at->frame_parent==d&&contains(subtree,at->frame_element)){web_presentation_exit(owner,2);break;}
}
void web_presentation_free(web_doc *d){if(!d)return;
    /* Teardown must release the native window even when reserving author event
       records fails. Retired realms cannot receive those events in any case. */
    web_doc *top=top_document(d);
    for(int kind=1;kind<=2;kind++){web_doc *owner=kind==1?top->fullscreen_owner:top->pip_owner;
        if(!web_presentation_exit(d,kind)&&owner&&ancestor(owner,d)&&owner->presentation_host){node_t *n=kind==1?owner->fullscreen_element:owner->pip_element;
            owner->presentation_host(owner->presentation_opaque,owner,n,kind==1?4:3);}}
    d->presentation_host=NULL;d->presentation_opaque=NULL;
    if(top->fullscreen_owner&&ancestor(top->fullscreen_owner,d)){web_doc *owner=top->fullscreen_owner;top->fullscreen_owner=NULL;
        for(web_doc *at=owner;at;at=at->frame_parent){node_t *element=at->fullscreen_element;at->fullscreen_element=NULL;changed(at,element);}}
    if(top->pip_owner&&ancestor(top->pip_owner,d)){web_doc *owner=top->pip_owner;node_t *element=owner->pip_element;
        owner->pip_element=NULL;top->pip_owner=NULL;changed(owner,element);}
    events_free(d->presentation_events);d->presentation_events=d->presentation_events_tail=NULL;free(d->presentation_policy);d->presentation_policy=NULL;d->presentation_policy_ready=false;
}
void web_presentation_style(web_doc *d,node_t *n,style_t *s){
    if(!d||!n||!s||n->owner!=d||!web_presentation_matches(n))return;
    /* Fullscreen's UA rule excludes :root; root viewport layout is already
       provided by the host. iframe has its separate border/padding rule. */
    if(!n->foreign&&n->tag==T_iframe)for(unsigned i=0;i<4;i++){
        s->border_style[i]=BS_NONE;s->border_width[i]=0;s->padding[i]=(len_t){.kind=LK_LEN};}
    if(n->parent&&n->parent->type==N_DOC)return;
    if(s->display==D_CONTENTS||s->display==D_INLINE)s->display=D_BLOCK;
    s->position=POS_FIXED;s->float_=FL_NONE;s->box_sizing=1;
    s->width=(len_t){.kind=LK_LEN,.pct=100};s->height=(len_t){.kind=LK_LEN,.pct=100};
    s->min_width=s->min_height=(len_t){.kind=LK_LEN};s->max_width=s->max_height=(len_t){.kind=LK_NONE};
    s->transform=s->rotate=s->scale=NULL;
    for(unsigned i=0;i<4;i++){s->inset[i]=(len_t){.kind=LK_LEN};s->margin[i]=(len_t){.kind=LK_LEN};}
}

/* ---- direct scanout of a fullscreen WebGL canvas (win_direct) ---- */
struct web_direct {struct web_webgl *gl;uint64_t serial;bool shown;int rect[4];};
/* Nothing of the element's own box is painted around, over or through its content. */
static bool direct_plain(const box_t *b){
    const style_t *s=b->st;
    if(!s||s->visibility||s->opacity<1||s->filter||s->backdrop_filter||s->box_shadow||css_mask_layers_set(css_style_mask(s))||s->animated)return false;
    if(s->bg_color>>24||s->has_grad||s->bg_image||s->outline_width>0)return false;
    for(int i=0;i<4;i++){if(b->b[i]>0)return false;
        for(int axis=0;axis<2;axis++)if(s->corner_radius[i].axis[axis].kind!=LK_AUTO&&len_resolve(&s->corner_radius[i].axis[axis],axis?b->h:b->w)!=0)return false;}
    return s->border_radius==0;
}
/* The WebGL canvas d (a top document) shows fullscreen with nothing over it and black around it, and where its
   picture lands in d's viewport: fullscreen elements down the frame chain, no top-layer dialog in any of
   those documents, nothing painted with or over the canvas, and its fitted picture inside its content box. */
static node_t *direct_canvas(web_doc *d,int rect[4]){
    if(!d||d->frame_parent||!active(d))return NULL;
    web_doc *owner=d->fullscreen_owner;node_t *canvas=owner?owner->fullscreen_element:NULL;
    if(!canvas||!valid(owner,canvas)||canvas->foreign||canvas->tag!=T_canvas||!canvas->box)return NULL;
    struct web_webgl *gl=web_canvas_webgl(canvas);
    if(!gl||!web_webgl_direct_capable(gl))return NULL;
    double x=box_visual_x(canvas->box)-owner->view_x,y=box_visual_y(canvas->box)-owner->view_y;
    for(web_doc *at=owner;;at=at->frame_parent){
        node_t *e=at->fullscreen_element;
        if(web_dialog_count(at)||!valid(at,e)||!e->box||!direct_plain(e->box))return NULL;
        if(at==d)break;
        /* The child's viewport is the content box of its fullscreen frame element. */
        web_doc *parent=at->frame_parent;
        if(!parent||parent->fullscreen_element!=at->frame_element||!at->frame_element->box)return NULL;
        x+=box_visual_x(at->frame_element->box)-parent->view_x;y+=box_visual_y(at->frame_element->box)-parent->view_y;
    }
    box_t *b=canvas->box;
    int X=(int)round(x),Y=(int)round(y),W=(int)roundf(b->w),H=(int)roundf(b->h);
    if(!isfinite(x)||!isfinite(y)||!web_canvas_fit(canvas,b->st,X,Y,W,H,rect))return NULL;
    /* The display scales the whole picture; one cropped by its box is painted through the window. */
    if(rect[0]<X||rect[1]<Y||(int64_t)rect[0]+rect[2]>(int64_t)X+W||(int64_t)rect[1]+rect[3]>(int64_t)Y+H)return NULL;
    return canvas;
}
web_direct *web_direct_open(web_doc *d,int screen_width,int screen_height){
    int rect[4];node_t *canvas=direct_canvas(d,rect);
    if(!canvas||screen_width<=0||screen_height<=0)return NULL;
    struct web_webgl *gl=web_canvas_webgl(canvas);
    if(!web_webgl_direct_begin(gl,(unsigned)screen_width,(unsigned)screen_height))return NULL;
    web_direct *v=calloc(1,sizeof *v);if(!v)return NULL;
    v->gl=gl;web_webgl_retain(gl);
    return v;
}
int web_direct_present(web_direct *v,web_doc *d,int screen_width,int screen_height,bool shown){
    int rect[4];node_t *canvas=v?direct_canvas(d,rect):NULL;
    if(!canvas||web_canvas_webgl(canvas)!=v->gl)return -2;
    /* The display shows the window's own buffer: the next frame shown directly is presented whole. */
    if(!shown){v->shown=false;return -1;}
    uint64_t serial=web_webgl_serial(v->gl);
    if(v->shown&&serial==v->serial&&!memcmp(rect,v->rect,sizeof rect))return 0;
    if(screen_width<=0||screen_height<=0||
       !web_webgl_direct_present(v->gl,rect[0],rect[1],(unsigned)rect[2],(unsigned)rect[3],(unsigned)screen_width,(unsigned)screen_height))
        return -1;
    v->serial=serial;v->shown=true;memcpy(v->rect,rect,sizeof rect);
    return 1;
}
void web_direct_close(web_direct *v){
    if(!v)return;
    web_webgl_direct_end(v->gl);web_webgl_release(v->gl);free(v);
}
