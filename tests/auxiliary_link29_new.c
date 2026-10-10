/* 新29固有の製品関数 snapshot。実OSのspawn/GUI受入ではない。 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef struct web_doc web_doc;
typedef struct node node_t,web_node;
struct node {web_doc *owner;unsigned type,namespace_id;const char *name,*target,*rel,*sandbox,*frame_name;node_t *parent,*first,*next;};
struct web_frame {struct web_frame *next;web_doc *owner,*document;node_t *element;char *sandbox;bool detached;};
struct web_host {void *opaque;void(*navigate_form)(void*,const char*,const void*,size_t,const char*,const char*);};
struct web_js_state {struct web_host host;};
struct web_doc {bool live,inert,dirty;node_t *root,*frame_element,*address_frame;web_doc *frame_parent;struct web_frame *frames;struct web_js_state *js;};
enum {N_ELEM=1,NS_HTML=0,NS_SVG=1,NAV_PUSH=1};
static unsigned checks,failures,spawns,queues,errors,warnings;static char last_warning[256];
static char *spawn_url;static bool host_arguments_ok;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("失敗 %u: %s\n",checks,#x);}}while(0)
static const char *node_attr(node_t *n,const char *key){if(!n)return NULL;if(!strcmp(key,"target"))return n->target;if(!strcmp(key,"rel"))return n->rel;if(!strcmp(key,"name"))return n->frame_name;return NULL;}
static void web_js_console(web_doc *d,int level,const char *message){(void)d;if(level==1)warnings++;else errors++;snprintf(last_warning,sizeof last_warning,"%s",message);}
static struct web_frame *web_frame_find(web_doc *d,node_t *n){for(struct web_frame *f=d?d->frames:NULL;f;f=f->next)if(f->element==n)return f;return NULL;}
static struct web_frame *web_frame_walk_next(web_doc *d,struct web_frame *f,bool descend){(void)d;(void)descend;return f->next;}
static bool web_frame_set_navigation(struct web_frame *f,const char *url,web_doc *source){(void)url;(void)source;return f!=NULL;}
static bool has_prefix(const char *s,const char *prefix){return !strncmp(s,prefix,strlen(prefix));}
static void console_add(const char *level,const char *message){(void)level;(void)message;errors++;}
static void queue_navigation_body(const char *url,const void *body,size_t length,const char *content_type,int kind){(void)url;(void)body;(void)length;(void)content_type;(void)kind;queues++;}
static int spawn(const char *path,char **args,int *fds,int flags){spawns++;free(spawn_url);spawn_url=strdup(args[1]);host_arguments_ok=!strcmp(path,"/bin/browser")&&!strcmp(args[0],"browser")&&args[2]==NULL&&fds[0]==0&&fds[1]==1&&fds[2]==2&&flags==0;return 1;}
#include "auxiliary_link29_product.inc"
static void reset(void){spawns=queues=errors=warnings=0;last_warning[0]=0;}
int main(void){
    struct web_js_state script={.host={.navigate_form=host_navigate_form}};
    web_doc top={.live=true,.js=&script};node_t anchor={.owner=&top,.type=N_ELEM,.namespace_id=NS_HTML,.name="a",.target="_blank"};
    const char *url="https://www.youtube.com/watch?v=3nLT-W5h6U8#full-URL";
    CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1&&host_arguments_ok&&!strcmp(spawn_url,url)&&queues==0&&!top.dirty&&!top.address_frame);
    reset();anchor.target="_BLANK";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1);
    reset();anchor.rel="opener";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&warnings==1&&strstr(last_warning,"opener"));
    reset();anchor.rel="OPENER\tNoOpEnEr";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1);
    reset();anchor.rel="opener\nnoreferrer";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1);
    reset();anchor.rel="notopener noopener-extra";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1);
    reset();anchor.rel=NULL;anchor.target="_self";CHECK(!web_frame_navigate(&top,&anchor,url)&&spawns==0);
    reset();anchor.target="_top";CHECK(!web_frame_navigate(&top,&anchor,url)&&spawns==0);
    reset();anchor.target="missing-name";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&warnings==1);
    reset();anchor.target="_blank";CHECK(web_frame_navigate(&top,&anchor,"file:///data/private")&&spawns==0&&errors==1);
    reset();script.host.navigate_form=NULL;CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&errors==1);script.host.navigate_form=host_navigate_form;
    reset();top.live=false;CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&warnings==1);top.live=true;
    reset();top.inert=true;CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&warnings==1);top.inert=false;
    web_doc other={.live=true};reset();anchor.owner=&other;CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&warnings==1);anchor.owner=&top;
    node_t root={.owner=&top},base={.owner=&top,.type=N_ELEM,.namespace_id=NS_HTML,.name="base",.target="_blank",.parent=&root};root.first=&base;top.root=&root;
    reset();anchor.target=NULL;CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1);
    reset();anchor.target="";CHECK(!web_frame_navigate(&top,&anchor,url)&&spawns==0);
    reset();anchor.target=NULL;base.namespace_id=NS_SVG;CHECK(!web_frame_navigate(&top,&anchor,url)&&spawns==0);base.namespace_id=NS_HTML;
    node_t template={.owner=&top,.type=N_ELEM,.namespace_id=NS_HTML,.name="template",.first=&base,.parent=&root};root.first=&template;base.parent=&template;
    reset();CHECK(!web_frame_navigate(&top,&anchor,url)&&spawns==0);
    node_t embedding={.owner=&top};web_doc child={.live=true,.frame_parent=&top,.frame_element=&embedding,.js=&script};struct web_frame frame={.owner=&top,.document=&child,.element=&embedding};top.frames=&frame;anchor.owner=&child;anchor.target="_blank";
    reset();CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1&&!top.address_frame);
    reset();frame.sandbox="";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&strstr(last_warning,"disallows"));
    reset();frame.sandbox="allow-popups";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&strstr(last_warning,"inheritance"));
    reset();frame.sandbox="allow-popups-to-escape-sandbox";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&strstr(last_warning,"disallows"));
    reset();frame.sandbox="Allow-Popups\fALLOW-POPUPS-TO-ESCAPE-SANDBOX";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1);
    reset();frame.sandbox="allow-popups-extra allow-popups-to-escape-sandbox";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0);
    reset();frame.sandbox=NULL;frame.detached=true;CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&strstr(last_warning,"Detached"));frame.detached=false;
    reset();frame.document=&other;CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&warnings==1);frame.document=&child;
    node_t nested_embedding={.owner=&child};web_doc nested={.live=true,.frame_parent=&child,.frame_element=&nested_embedding,.js=&script};struct web_frame nested_frame={.owner=&child,.document=&nested,.element=&nested_embedding,.sandbox="allow-popups allow-popups-to-escape-sandbox"};child.frames=&nested_frame;anchor.owner=&nested;
    reset();frame.sandbox="allow-popups";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&strstr(last_warning,"inheritance"));
    reset();frame.sandbox="allow-popups allow-popups-to-escape-sandbox";CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==1);
    reset();child.live=false;CHECK(web_frame_navigate(&top,&anchor,url)&&spawns==0&&warnings==1);child.live=true;
    reset();frame.sandbox=NULL;anchor.owner=&top;char *large=malloc(70017);memcpy(large,"https://x.test/",15);memset(large+15,'q',70000);large[70015]=0;CHECK(web_frame_navigate(&top,&anchor,large)&&spawns==1&&strlen(spawn_url)==70015);free(large);
    reset();host_navigate_form(NULL,url,"p=1",3,"application/x-www-form-urlencoded","_blank");CHECK(spawns==0&&queues==0&&errors==1);
    reset();host_navigate_form(NULL,url,NULL,0,NULL,"named");CHECK(spawns==0&&queues==0&&errors==1);
    reset();host_navigate_form(NULL,url,NULL,0,NULL,"_self");CHECK(spawns==0&&queues==1);
    free(spawn_url);printf("新29固有条件 %u、失敗 %u\n",checks,failures);return failures?1:0;
}
