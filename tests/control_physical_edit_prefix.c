/* Actual doc.c UTF16/key functions and actual control_edit.h; bounded form,
 * ownership and event callback fixtures. Not real GUI/QuickJS acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
enum {N_DOC=9,N_ELEM=1,T_input=1,T_textarea=2};
enum web_input_kind {WEB_INPUT_TEXT,WEB_INPUT_SEARCH,WEB_INPUT_TEL,WEB_INPUT_URL,WEB_INPUT_EMAIL,WEB_INPUT_PASSWORD,WEB_INPUT_HIDDEN,WEB_INPUT_DATE,WEB_INPUT_MONTH,WEB_INPUT_WEEK,WEB_INPUT_TIME,WEB_INPUT_DATETIME_LOCAL,WEB_INPUT_NUMBER,WEB_INPUT_RANGE,WEB_INPUT_COLOR,WEB_INPUT_CHECKBOX,WEB_INPUT_RADIO,WEB_INPUT_FILE};
enum {WEB_INPUT_OK,WEB_INPUT_OOM};
enum {NKEY_BACKSPACE=8,NKEY_ENTER=10,NKEY_LEFT=0x100,NKEY_RIGHT,NKEY_HOME,NKEY_END,NKEY_UP,NKEY_DOWN,NKEY_DELETE};
enum {NMOD_SHIFT=1,NMOD_CTRL=2,NMOD_ALT=4};
typedef struct web_doc web_doc;
typedef struct node {web_doc *owner;int type,tag,kind;bool foreign,disabled,read_only,connected;char *value;const char *maxlength;uint32_t selection_start,selection_end;uint8_t selection_direction;bool selection_set,control_user_edited;} node_t;
typedef node_t web_node;
struct web_doc {node_t *root,*focus;bool live,inert,dirty;int caret;web_doc *frame_parent;};
struct web_frame {web_doc *document;bool detached;};
struct gui_event {uint32_t key,mods;};
struct web_event {const char *type;bool bubbles;};
typedef struct {char *p;size_t n,cap;} sbuf;
static web_doc document,other;static node_t root,alternate_root,control,other_control;
static unsigned checks,failed,before_count,input_count;static int mutation,alloc_fail=-1,alloc_index;static bool pending_navigation;
static char observed_type[40],observed_data[80];static bool observed_null;
static void *fixture_alloc(size_t n){if(alloc_index++==alloc_fail)return NULL;return malloc(n);}
#define malloc fixture_alloc
static void sb_put(sbuf *b,const void *text,size_t len){if(len>b->cap-b->n-1)abort();memcpy(b->p+b->n,text,len);b->n+=len;b->p[b->n]=0;}
static void sb_puts(sbuf *b,const char *text){sb_put(b,text,strlen(text));}
static void sb_putc(sbuf *b,char c){sb_put(b,&c,1);}
static void sb_free(sbuf *b){free(b->p);}
static enum web_input_kind web_input_type(const node_t *n){return n->kind;}
static const char *node_attr(node_t *n,const char *name){return !strcmp(name,"readonly")?(n->read_only?"":NULL):!strcmp(name,"maxlength")?n->maxlength:NULL;}
static const char *web_input_edit_text(const node_t *n){return n->value?n->value:"";}
static bool web_control_disabled(node_t *n){return n->disabled;}
static bool web_dialog_inert(web_doc *d,node_t *n){return d->inert;}
static bool doc_node_connected(node_t *n){return n->connected;}
static node_t *doc_node_root(node_t *n,bool composed){return n->type==N_DOC?n:n->connected?n->owner->root:NULL;}
static void doc_control_init(web_doc *d,node_t *n){}
static void web_js_selection_changed(web_doc *d,node_t *n){}
static bool web_input_is_file(node_t *n){return n->kind==WEB_INPUT_FILE;}
static bool web_frame_element(node_t *n){return false;}
static struct web_frame *web_frame_find(web_doc *d,node_t *n){return NULL;}
static node_t *web_focused(web_doc *d){return d->focus;}
static bool doc_control_selection_supported(node_t *n){return n->tag==T_textarea||n->kind<=WEB_INPUT_URL||n->kind==WEB_INPUT_PASSWORD;}
static bool web_input_numeric(enum web_input_kind kind){return kind>=WEB_INPUT_DATE&&kind<=WEB_INPUT_RANGE;}
static bool web_input_user_value(web_doc *d,node_t *n,const char *text,size_t len){char *p=malloc(len+1);if(!p)return false;memcpy(p,text,len);p[len]=0;free(n->value);n->value=p;n->control_user_edited=true;return true;}
static bool doc_node_value(web_doc *d,node_t *n,const char *text,size_t len){return web_input_user_value(d,n,text,len);}
static int web_input_step(web_doc *d,node_t *n,int count,bool down){return web_input_user_value(d,n,down?"0":"2",1)?WEB_INPUT_OK:WEB_INPUT_OOM;}
static bool web_js_dispatch(web_doc *d,node_t *n,const struct web_event *e){input_count++;return true;}
static void *web_js_edit_begin(web_doc *d){return NULL;}
static void web_js_edit_end(void *scope){}
static int utf8_put(char *out,uint32_t cp){if(cp<128){out[0]=cp;return 1;}if(cp<2048){out[0]=0xc0|(cp>>6);out[1]=0x80|(cp&63);return 2;}if(cp<65536){out[0]=0xe0|(cp>>12);out[1]=0x80|((cp>>6)&63);out[2]=0x80|(cp&63);return 3;}out[0]=0xf0|(cp>>18);out[1]=0x80|((cp>>12)&63);out[2]=0x80|((cp>>6)&63);out[3]=0x80|(cp&63);return 4;}
static bool web_js_input_event(web_doc *d,node_t *n,const char *type,const char *input_type,const char *data,size_t len){
    snprintf(observed_type,sizeof observed_type,"%s",input_type);observed_null=data==NULL;
    if(data){size_t copy=len<sizeof observed_data-1?len:sizeof observed_data-1;memcpy(observed_data,data,copy);observed_data[copy]=0;}
    if(!strcmp(type,"input")){input_count++;return true;}before_count++;
    switch(mutation){
    case 1:return false;
    case 2:d->focus=&other_control;break;
    case 3:web_input_user_value(d,n,"handler",7);break;
    case 4:n->selection_start=n->selection_end=0;break;
    case 5:d->live=false;break;
    case 6:d->root=&alternate_root;break;
    case 7:n->connected=false;break;
    case 8:n->owner=&other;break;
    case 9:n->read_only=true;break;
    case 10:n->kind=WEB_INPUT_URL;break;
    case 11:n->maxlength="1";break;
    case 12:n->selection_direction=2;break;
    case 13:pending_navigation=true;break;
    case 14:alloc_fail=alloc_index;break;
    }
    return !pending_navigation;
}
/* ACTUAL_CONTROL_FUNCTIONS */
static void reset(const char *value,uint32_t start,uint32_t end){
    alloc_fail=-1;alloc_index=0;free(control.value);
    document=(web_doc){.root=&root,.focus=&control,.live=true};root=(node_t){.owner=&document,.type=N_DOC,.connected=true};alternate_root=(node_t){.owner=&document,.type=N_DOC,.connected=true};
    control=(node_t){.owner=&document,.type=N_ELEM,.tag=T_input,.kind=WEB_INPUT_TEXT,.connected=true,.value=strdup(value),.selection_start=start,.selection_end=end};
    before_count=input_count=0;mutation=0;pending_navigation=false;observed_type[0]=observed_data[0]=0;observed_null=false;
}
static void check(bool value,const char *name){checks++;if(!value){failed++;printf("失敗 %s\n",name);}}
static int key(uint32_t k,uint32_t mods){struct gui_event e={k,mods};return web_key(&document,&e);}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    reset("ab\xf0\x9f\x98\x80" "cd",2,4);check(key('Z',0)==1&&!strcmp(control.value,"abZcd")&&control.selection_end==3&&before_count==1&&input_count==1&&!strcmp(observed_type,"insertText")&&!strcmp(observed_data,"Z")&&!observed_null,"文字置換");
    reset("ab\xf0\x9f\x98\x80" "cd",4,4);check(key(NKEY_BACKSPACE,0)==1&&!strcmp(control.value,"abcd")&&control.selection_end==2&&observed_null&&!strcmp(observed_type,"deleteContentBackward"),"補助平面後退");
    reset("ab\xf0\x9f\x98\x80" "cd",2,2);check(key(NKEY_DELETE,0)==1&&!strcmp(control.value,"abcd")&&control.selection_end==2&&!strcmp(observed_type,"deleteContentForward"),"補助平面前方削除");
    reset("ab\xf0\x9f\x98\x80" "cd",3,3);check(key(NKEY_BACKSPACE,0)==1&&!strcmp(control.value,"ab\xed\xb8\x80" "cd")&&control.selection_end==2,"分割サロゲート後退");
    reset("ab\xf0\x9f\x98\x80" "cd",3,3);check(key(NKEY_DELETE,0)==1&&!strcmp(control.value,"ab\xed\xa0\xbd" "cd")&&control.selection_end==3,"分割サロゲート前方");
    reset("ab",1,1);control.tag=T_textarea;check(key(NKEY_ENTER,0)==1&&!strcmp(control.value,"a\nb")&&observed_null&&!strcmp(observed_type,"insertLineBreak"),"textarea改行");
    reset("ab",1,1);check(key(NKEY_ENTER,0)==2&&before_count==0&&input_count==0,"inputEnter");
    reset("ab",1,1);check(key(NKEY_LEFT,0)==1&&before_count==0&&input_count==0&&control.selection_end==0,"非編集方向キー");
    reset("ab",1,1);check(key('a',NMOD_CTRL)==1&&before_count==0&&input_count==0&&control.selection_start==0&&control.selection_end==2,"全選択非編集");
    reset("ab",1,1);check(key('x',NMOD_ALT)==0&&before_count==0&&input_count==0,"Alt非編集");
    reset("",0,0);check(key(NKEY_BACKSPACE,0)==1&&before_count==0&&input_count==0,"空後退消費");
    reset("ab",2,2);check(key(NKEY_DELETE,0)==1&&before_count==0&&input_count==0,"末尾削除消費");
    reset("ab",1,1);control.read_only=true;check(key('x',0)==1&&!strcmp(control.value,"ab")&&before_count==0,"readonly");
    reset("ab",1,1);control.disabled=true;check(key('x',0)==0&&before_count==0,"disabled");
    reset("ab",1,1);document.inert=true;check(key('x',0)==0&&before_count==0,"inert");
    reset("ab",1,1);control.kind=WEB_INPUT_CHECKBOX;check(key('x',0)==0&&before_count==0,"非テキストcheckbox");
    reset("1",1,1);control.kind=WEB_INPUT_NUMBER;check(key(NKEY_UP,0)==1&&!strcmp(control.value,"2")&&before_count==0&&input_count==1,"数値step通知保存");
    for(int i=1;i<=13;i++){reset("ab",1,1);mutation=i;key('x',0);check(before_count==1&&input_count==0&&(i==3?!strcmp(control.value,"handler"):!strcmp(control.value,"ab")),"before後stale取消");}
    for(int i=0;i<3;i++){reset("ab",1,1);alloc_fail=i;key('x',0);check(!strcmp(control.value,"ab")&&before_count==0&&input_count==0,"準備OOM");}
    reset("ab",1,1);mutation=14;key('x',0);check(!strcmp(control.value,"ab")&&before_count==1&&input_count==0,"編集OOM");
    reset("ab",1,1);control.maxlength="2";key('x',0);check(!strcmp(control.value,"ab")&&before_count==0,"maxlength成長拒否");
    reset("abcd",1,3);control.maxlength="4";check(key('x',0)==1&&!strcmp(control.value,"axd")&&input_count==1,"maxlength置換");
    reset("ab",1,1);control.tag=T_textarea;check(web_control_edit(&document,&control,"insertFromPaste","x\r\ny\rz",6)==3&&!strcmp(control.value,"ax\ny\nzb")&&!strcmp(observed_type,"insertFromPaste")&&!observed_null,"textareaPaste正規化");
    reset("ab",1,1);check(web_control_edit(&document,&control,"insertFromPaste","x\r\ny\rz",6)==3&&!strcmp(control.value,"axyzb"),"inputPaste改行除去");
    reset("abcd",1,3);check(web_control_edit(&document,&control,"deleteByCut",NULL,0)==3&&!strcmp(control.value,"ad")&&observed_null&&!strcmp(observed_type,"deleteByCut"),"選択cut");
    reset("abcd",1,3);mutation=1;check(web_control_edit(&document,&control,"deleteByCut",NULL,0)==1&&!strcmp(control.value,"abcd")&&input_count==0,"cut取消");
    reset("abcd",1,3);control.read_only=true;size_t n;char *copy=web_control_selected_text(&document,&control,&n);check(copy&&n==2&&!memcmp(copy,"bc",2),"readonlycopy");free(copy);
    reset("abcd",1,3);control.kind=WEB_INPUT_PASSWORD;copy=web_control_selected_text(&document,&control,&n);check(!copy&&!n,"passwordcopy拒否");
    reset("ab",1,1);check(web_control_edit(&document,&control,"insertFromPaste","x\0y",3)==1&&before_count==0&&!strcmp(control.value,"ab"),"NUL拒否");
    const char *invalid[]={"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82","\x80"};
    const size_t sizes[]={2,3,4,2,1};for(int i=0;i<5;i++){reset("ab",1,1);web_control_edit(&document,&control,"insertFromPaste",invalid[i],sizes[i]);check(before_count==0&&!strcmp(control.value,"ab"),"UTF8不正拒否");}
    reset("ab",1,1);check(web_control_edit(&document,&control,"insertFromPaste",NULL,(16u<<20)+1)==1&&before_count==0,"有限payload境界");
    reset("ab\xf0\x9f\x98\x80" "cd",0,3);copy=web_control_selected_text(&document,&control,&n);check(copy&&n==5&&!strcmp(copy,"ab\xed\xa0\xbd"),"selectedtext分割サロゲート");free(copy);
    printf("physical native edit: %u new checks, %u failed\n",checks,failed);free(control.value);return failed!=0;
}
