#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#define DOM_MAX_DEPTH 256
enum {N_DOC=1,N_ELEM,N_FRAGMENT,N_ATTR,N_TEXT,N_DOCTYPE};
/* PRODUCT_TAGS */
typedef struct web_doc web_doc;
typedef struct node node_t;
struct attr {node_t *node;};
struct node {web_doc *owner;int type,tag,elem_index;bool foreign;node_t *parent,*first,*last,*next,*prev,*shadow_root,*shadow_host,*template_host,*attr_owner,*slot_assigned_first;struct attr *attrs,*attribute;int nattrs;uint64_t resource_revision;};
struct svg_cache {void *img;char *src;};
struct web_doc {node_t *root,*focus,*find_node;void *find_run;int caret;web_doc *dom_family;bool js_nodes_dirty,dirty,need_style,resources_dirty;uint64_t dom_revision;struct {uint64_t inserts,index_visits,index_ms;} profile;struct {int n;struct svg_cache **v;} svgs;};
struct web_js_state {bool node_cache_failed,node_cache_syncing;struct web_js_state *node_cache_next;};
static struct web_js_state *node_cache_states;
static unsigned syncs,assignments,frame_detaches,dialog_removes;
static bool assignment=true;
static uint64_t uptime_ms(void){return 0;}
static void textarea_changed(node_t *n){}
static void web_js_nodes_changed(web_doc *d){syncs++;}
static void web_dialog_sync(web_doc *d){}
static void image_free(void *p){}
static void stylesheet_detach(node_t *n,int generation){}
static void doc_shadow_reassign(web_doc *d){assignments++;}
static bool is_slot(node_t *n){return n&&n->tag==T_slot;}
static bool assignment_structure(node_t *p,node_t *c){return assignment;}
static void doc_slot_signal(node_t *n){}
static void web_dialog_removed(web_doc *d,node_t *c){dialog_removes++;}
static void web_frames_detach_tree(web_doc *d,node_t *c){frame_detaches++;}
static void doc_details_inserted(node_t *c){}
static void web_select_inserted(web_doc *d,node_t *c,node_t *old){}
static void web_select_sync(web_doc *d,node_t *n,bool b){}
static void doc_node_remove(web_doc *d,node_t *n);
static bool doc_node_adopt(web_doc *d,node_t *n){if(n->parent)doc_node_remove(n->owner,n);n->owner=d;web_js_nodes_changed(d);return true;}
static node_t *node_ancestor(node_t *n,int tag){for(;n;n=n->parent)if(n->tag==tag)return n;return NULL;}
/* PRODUCT_FUNCTIONS */
static int checks,failures;
#define CHECK(name,expr) do{checks++;if(!(expr)){printf("FAIL %s\n",name);failures++;}}while(0)
int main(void){
    web_doc d={0};node_t root={.owner=&d,.type=N_DOC};d.root=&root;
    node_t left={.owner=&d,.type=N_ELEM,.parent=&root},right={.owner=&d,.type=N_ELEM,.parent=&root};
    node_t child={.owner=&d,.type=N_ELEM,.parent=&left};left.first=left.last=&child;
    CHECK("same forest query",web_js_nodes_same_forest(&d,&child,&right));
    CHECK("same forest move",doc_node_move(&d,&right,&child,NULL));
    CHECK("no full lifetime walk",syncs==0);
    CHECK("normal parent links",child.parent==&right&&right.first==&child&&right.last==&child&&!left.first&&!left.last);
    CHECK("revision and selector layout retained",d.dom_revision==2&&d.dirty&&d.need_style);
    CHECK("assignment and frame removal retained",assignments==1&&frame_detaches==1&&dialog_removes==1);
    node_t second={.owner=&d,.type=N_ELEM};CHECK("new detached insertion uses full sync",doc_node_move(&d,&right,&second,NULL)&&syncs==1);
    unsigned before=syncs;CHECK("same parent reorder",doc_node_move(&d,&right,&second,&child)&&right.first==&second&&second.next==&child&&syncs==before);
    d.js_nodes_dirty=true;before=syncs;CHECK("pending owner dirty not erased",doc_node_move(&d,&left,&child,NULL)&&syncs==before+1&&!d.js_nodes_dirty);
    node_t forest={.owner=&d,.type=N_ELEM},a={.owner=&d,.type=N_ELEM,.parent=&forest},b={.owner=&d,.type=N_ELEM,.parent=&forest},leaf={.owner=&d,.type=N_ELEM,.parent=&a};a.first=a.last=&leaf;
    before=syncs;CHECK("detached same forest move",doc_node_move(&d,&b,&leaf,NULL)&&syncs==before);
    before=syncs;CHECK("different forest move fully synchronizes",doc_node_move(&d,&left,&leaf,NULL)&&syncs==before+2);
    before=syncs;doc_node_remove(&d,&leaf);CHECK("explicit detach fully synchronizes",syncs==before+1&&!leaf.parent);
    web_doc other={0};node_t otherroot={.owner=&other,.type=N_DOC},otherparent={.owner=&other,.type=N_ELEM,.parent=&otherroot};other.root=&otherroot;
    CHECK("owner mismatch cannot skip",!web_js_nodes_same_forest(&other,&child,&otherparent));before=syncs;CHECK("adoption fully synchronizes",doc_node_move(&other,&otherparent,&child,NULL)&&syncs>=before+3&&child.owner==&other);
    struct web_js_state stale={.node_cache_failed=true};node_cache_states=&stale;CHECK("uncertain prior OOM refuses skip",!web_js_nodes_same_forest(&d,&second,&right));before=syncs;CHECK("uncertain same forest still syncs",doc_node_move(&d,&left,&second,NULL)&&syncs==before+2);node_cache_states=NULL;
    node_t shadow={.owner=&d,.type=N_FRAGMENT,.shadow_host=&right},templ={.owner=&d,.type=N_FRAGMENT,.template_host=&right},attr={.owner=&d,.type=N_ATTR,.attr_owner=&right};
    CHECK("shadow lifetime forest includes host",web_js_nodes_same_forest(&d,&shadow,&left));CHECK("template lifetime forest includes host",web_js_nodes_same_forest(&d,&templ,&left));CHECK("attribute lifetime forest includes owner",web_js_nodes_same_forest(&d,&attr,&left));
    before=syncs;doc_mutated(&d,NULL);CHECK("parser transaction fully synchronizes",syncs==before+1);
    struct attr oldattrs[]={{.node=&attr}};right.attrs=oldattrs;right.nattrs=1;doc_attrs_publish(&right,NULL,0);before=syncs;changed(&d,&right,false);CHECK("Attr owner detach fully synchronizes",!attr.attr_owner&&syncs==before+1);
    child.owner=&d;child.parent=&right;child.next=child.prev=NULL;right.first=right.last=&child;child.tag=T_style;d.resources_dirty=false;before=syncs;
    CHECK("resource invalidation retained in same forest",doc_node_move(&d,&left,&child,NULL)&&d.resources_dirty&&syncs==before);
    printf("same-forest-native: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
