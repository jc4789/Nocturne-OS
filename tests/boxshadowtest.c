/* Actual native shadow parser/cascade/paint/cache/retirement, not site acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "webi.h"

static int checks, failed;
static void check(bool value,const char *name){checks++;if(!value){failed++;printf("FAIL boxshadow %s\n",name);}}
struct cancel_state { bool stop; };
static bool checkpoint(void *opaque){return !((struct cancel_state *)opaque)->stop;}
static uint32_t pixels[256*160];
static void paint(web_doc *doc,int scroll){canvas_t canvas;gfx_init(&canvas,pixels,256,160,256);web_paint(doc,&canvas,0,0,256,160,0,scroll);}
static void set(web_doc *doc,node_t *node,const char *shadow,const char *extra){
    char style[4096];snprintf(style,sizeof style,"display:inline-block;position:absolute;left:50px;top:50px;width:100px;height:40px;background:transparent;box-shadow:%s;%s",shadow,extra?extra:"");
    doc_node_attr(doc,node,"style",style);web_layout(doc,256,160);paint(doc,0);
}
static uint32_t pixel(int x,int y){return pixels[y*256+x]&0xffffffu;}
static node_t *query(web_doc *doc,const char *selector){pvec nodes={0};css_select(doc,doc->root,selector,&nodes);node_t *result=nodes.n?nodes.v[0]:NULL;pv_free(&nodes);return result;}
static int fragment_caches(node_t *inline_node,bool populated){
    box_t *source=inline_node->box?inline_node->box->cb:NULL;int found=0;
    if(!source)return 0;
    for(int i=0;i<source->ndecos;i++)if(source->decos[i].node==inline_node){
        if(!!source->decos[i].shadow_cache!=populated)return -1;found++;
    }
    return found;
}
struct radius_state {struct cancel_state control;uint64_t image_id;};
static bool radius_request(void *opaque,const struct web_request *request){
    if(request->kind!=WEB_RESOURCE_IMAGE)return false;
    ((struct radius_state *)opaque)->image_id=request->id;return true;
}
static int radius_main(void){
    const char *html="<!doctype html><html><head><style>body{margin:0}#round{position:absolute;left:50px;top:50px;width:100px;height:40px;background:red;border-radius:50%}#avatar{position:absolute;left:180px;top:20px;width:40px;height:40px;border-radius:50%}#clip{position:absolute;left:10px;top:10px;width:40px;height:40px;overflow:hidden;border-radius:50%;background:blue}#child{width:40px;height:40px;background:red}#fixed{position:fixed;left:0;top:0;width:5px;height:5px;background:green}</style></head><body><div id='round'></div><img id='avatar' src='https://radius.test/image.svg'><div id='clip'><div id='child'></div><div id='fixed'></div></div></body></html>";
    struct radius_state state={0};struct web_host host={.opaque=&state,.request=radius_request,.script_checkpoint=checkpoint};
    web_doc *doc=web_live(html,strlen(html),"https://radius.test/","utf-8",&host);check(doc!=NULL,"radius live document");if(!doc)return 1;
    uint64_t start=uptime_ms();while(doc->parser&&!doc->native_cancelled&&uptime_ms()-start<5000)web_tick(doc,uptime_ms());
    for(int i=0;i<8;i++)web_tick(doc,uptime_ms());
    node_t *round=query(doc,"#round"),*avatar=query(doc,"#avatar"),*child=query(doc,"#child"),*fixed=query(doc,"#fixed");
    check(round&&avatar&&child&&fixed,"radius parser supplies all actual targets");if(!round||!avatar||!child||!fixed){web_free(doc);return 1;}
    check(state.image_id!=0,"image uses native resource request claim");
    const char *svg="<svg xmlns='http://www.w3.org/2000/svg' width='40' height='40'><rect width='40' height='40' fill='blue'/></svg>";
    struct web_response response={.status=200,.body=(char *)svg,.body_len=strlen(svg)};snprintf(response.headers,sizeof response.headers,"Content-Type: image/svg+xml\r\n");
    if(state.image_id)web_resource_loaded(doc,state.image_id,&response);
    for(int i=0;i<8;i++)web_tick(doc,uptime_ms());web_layout(doc,256,160);paint(doc,0);
    check(pixel(50,50)==0xffffff&&pixel(100,70)==0xff0000,"percentage ellipse background retains corner knockout and center");
    check(pixel(180,20)==0xffffff&&pixel(200,40)==0x0000ff,"native decoded image is clipped by actual circular content edge");
    check(pixel(10,10)==0xffffff&&pixel(30,30)==0xff0000,"overflow clips ordinary descendants to rounded padding edge");
    check(pixel(2,2)==0x008000,"viewport-fixed child escapes ancestor rounded overflow correctly");
    check(web_node_at(doc,2,2)==fixed,"fixed-child hit shares its unclipped paint context");
    check(web_node_at(doc,10,10)!=child&&web_node_at(doc,180,20)!=avatar,"spatial hit excludes rounded ancestor and replaced corner cutouts");
    struct radius_cache *retained=round->box->radius_clip[0];paint(doc,0);
    check(retained&&round->box->radius_clip[0]==retained,"unchanged native paint reuses retained mask");
    doc_node_attr(doc,round,"style","position:absolute;left:50px;top:50px;width:100px;height:40px;background:red;border-radius:50%;border-top-left-radius:0");web_layout(doc,256,160);paint(doc,0);
    check(pixel(50,50)==0xff0000&&pixel(149,50)==0xffffff,"single longhand overrides only its corner after shorthand");
    sbuf text={0};check(css_radius_serialize(round->style,-1,&text,doc)&&strstr(text.p,"0px 50% 50%")&&strstr(text.p," / ")==NULL,"computed radius keeps percentages and compresses repeated corners");sb_free(&text);
    doc_node_attr(doc,round,"style","position:absolute;left:50px;top:50px;width:100px;height:40px;background:red;border-radius:0 / 50%");web_layout(doc,256,160);paint(doc,0);
    check(pixel(50,50)==0xff0000,"zero on either radius axis makes the corner square");
    web_free(doc);printf("radius-native: %d checks, %d failures\n",checks,failed);return failed!=0;
}
static int damage_main(void){
    const char *html="<!doctype html><html><head><style>body{margin:0}</style></head><body><div id='pane' style='display:inline-block;position:absolute;left:50px;top:50px;width:100px;height:40px;box-shadow:0 0 0 1px green'></div><img id='atomic' style='position:absolute;left:50px;top:50px;width:100px;height:40px;box-shadow:10px 0 4px 6px green'></body></html>";
    struct cancel_state state={0};struct web_host host={.opaque=&state,.script_checkpoint=checkpoint};
    web_doc *doc=web_live(html,strlen(html),"https://shadow.test/","utf-8",&host);
    check(doc!=NULL,"damage live document");if(!doc)return 1;
    uint64_t start=uptime_ms();while(doc->parser&&!doc->native_cancelled&&uptime_ms()-start<5000)web_tick(doc,uptime_ms());
    node_t *pane=query(doc,"#pane"),*atomic=query(doc,"#atomic");
    check(pane&&atomic&&!doc->parser,"damage normal parser supplies both actual targets");
    if(!pane||!atomic){web_free(doc);return 1;}
    web_layout(doc,256,160);int x,y,w,h;
    doc->paint_dirty=false;doc->paint_damage_full=doc->paint_damage_valid=false;
    doc_paint_damage(doc,pane);bool damage=web_take_paint_damage(doc,&x,&y,&w,&h);
    check(pane->box&&pane->box->kind==B_BLOCK&&!damage,"blockified author inline-block keeps conservative full damage for unknown descendants");
    check(atomic->box&&atomic->box->kind==B_ATOMIC,"damage fixture traverses actual native atomic box rather than a blockified block");
    doc->paint_dirty=false;doc->paint_damage_full=doc->paint_damage_valid=false;
    doc_paint_damage(doc,atomic);damage=web_take_paint_damage(doc,&x,&y,&w,&h);
    check(damage&&x<50&&y<50&&x+w>150&&y+h>90,"known atomic damage includes shadow ink outside its box");
    check(damage&&x<=47&&y<=37&&x+w>=173&&y+h>=103,"atomic damage includes shifted spread and Gaussian halo rather than only old two-pixel padding");
    web_free(doc);printf("boxshadow-damage: %d checks, %d failures\n",checks,failed);return failed!=0;
}
struct image_association_state {uint64_t red,blue;};
static bool association_request(void *opaque,const struct web_request *request){
    if(request->kind!=WEB_RESOURCE_IMAGE)return false;
    struct image_association_state *state=opaque;
    if(strstr(request->url,"/red.svg"))state->red=request->id;
    else if(strstr(request->url,"/blue.svg"))state->blue=request->id;
    else return false;return true;
}
static void association_supply(web_doc *doc,uint64_t id,const char *color){
    char svg[256];snprintf(svg,sizeof svg,"<svg xmlns='http://www.w3.org/2000/svg' width='100' height='90'><rect width='100' height='90' fill='%s'/></svg>",color);
    struct web_response response={.status=200,.body=svg,.body_len=strlen(svg)};
    snprintf(response.headers,sizeof response.headers,"Content-Type: image/svg+xml\r\n");
    if(id)web_resource_loaded(doc,id,&response);
}
static int association_main(void){
    const char *html="<!doctype html><html><head><style>body{margin:0}#owner,#mask-owner{position:absolute;top:10px;width:100px;height:90px;background-image:url('/red.svg')}#owner{left:10px}#mask-owner{left:130px;mask-image:url('/red.svg');background:red}#with,#plain,#mask-child{position:absolute;top:10px;width:30px;height:30px}#with,#mask-child{left:10px;background-image:url('/blue.svg')}#plain{left:50px;background:green}#with:hover,#plain:hover,#mask-child:hover{color:lime}</style></head><body><div id='owner'><div id='with'></div><div id='plain'></div></div><div id='mask-owner'><div id='mask-child'></div></div></body></html>";
    struct image_association_state state={0};struct web_host host={.opaque=&state,.request=association_request};
    web_doc *doc=web_live(html,strlen(html),"https://image.test/","utf-8",&host);check(doc!=NULL,"image association live document");if(!doc)return 1;
    uint64_t start=uptime_ms();while(doc->parser&&!doc->native_cancelled&&uptime_ms()-start<5000)web_tick(doc,uptime_ms());
    web_layout(doc,256,160);for(int i=0;i<8;i++)web_tick(doc,uptime_ms());
    node_t *owner=query(doc,"#owner"),*with=query(doc,"#with"),*plain=query(doc,"#plain"),*masked=query(doc,"#mask-child");
    check(owner&&with&&plain&&masked&&!doc->parser,"normal parser supplies actual hover targets");
    if(!owner||!with||!plain||!masked){web_free(doc);return 1;}
    check(state.red&&state.blue,"native CSS images acquire both actual request claims");
    association_supply(doc,state.red,"red");association_supply(doc,state.blue,"blue");
    for(int i=0;i<8;i++)web_tick(doc,uptime_ms());web_layout(doc,256,160);paint(doc,0);
    check(pixel(25,25)==0x0000ff&&pixel(65,25)==0x008000&&pixel(145,25)==0x0000ff,"initial decoded background/mask scene");
    doc_hover_update(doc,with);web_layout(doc,256,160);paint(doc,0);
    check(with->style->bg_img!=owner->style->bg_img&&pixel(25,25)==0x0000ff,"partial hover restyle does not substitute parent's decoded background handle");
    doc_hover_update(doc,plain);web_layout(doc,256,160);paint(doc,0);
    check(!plain->style->bg_img&&!plain->style->mask_img&&pixel(65,25)==0x008000,"plain child cannot paint parent's URL after hover");
    doc_hover_update(doc,masked);web_layout(doc,256,160);paint(doc,0);
    check(!masked->style->mask_img&&pixel(145,25)==0x0000ff,"non-inherited mask cannot replace child's background after hover");
    paint(doc,5);paint(doc,0);check(pixel(25,25)==0x0000ff&&pixel(145,25)==0x0000ff,"scroll away and return preserves current image associations");
    web_free(doc);printf("image-association-native: %d checks, %d failures\n",checks,failed);return failed!=0;
}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--radius"))return radius_main();
    if(argc>1&&!strcmp(argv[1],"--damage-only"))return damage_main();
    if(argc>1&&!strcmp(argv[1],"--image-association"))return association_main();
    static const struct {const char *value;bool valid;} cases[]={
        {"none",true},{"1px 2px",true},{"red 1px 2px",true},{"1px 2px red",true},{"1px 2px 3px -4px rgba(0,0,0,.2) inset",true},
        {"inset 1px 2px,0 0 0 1px green",true},{"calc(1em + 2px) -2px min(4px, 5px)",true},
        {"1px",false},{"1px 2px 3px 4px 5px",false},{"1px 2px -3px",false},{"1px 2px 3%",false},
        {"1px 2px red blue",false},{"inset inset 1px 2px",false},{"none,1px 2px",false},{"1px 2px,",false},
        {"-4px red 4px",false},{"4px -4px inset 0",false},{"4px -4px 0 red 0",false},
        {"1px 1px calc(-1px)",true},{"inset green 4px -4px 0 0",true},
    };
    for(size_t i=0;i<sizeof cases/sizeof *cases;i++)check(css_supports_declaration("box-shadow",10,cases[i].value,strlen(cases[i].value))==cases[i].valid,cases[i].value);
    const char *html="<!doctype html><html><head><style>body{margin:0}</style></head><body><div id='pane'></div><img id='atomic-damage' style='display:none'><p id='fragment-parent' style='position:absolute;left:210px;top:5px;width:40px;margin:0;font-size:10px;line-height:12px'><span id='fragment'>one two three four five six</span></p></body></html>";
    struct cancel_state state={0};struct web_host host={.opaque=&state,.script_checkpoint=checkpoint};
    web_doc *doc=web_live(html,strlen(html),"https://shadow.test/","utf-8",&host);
    check(doc!=NULL,"live document");if(!doc)return 1;
    uint64_t start=uptime_ms();while(doc->parser&&!doc->native_cancelled&&uptime_ms()-start<5000)web_tick(doc,uptime_ms());
    pvec nodes={0};css_select(doc,doc->root,"#pane",&nodes);node_t *pane=nodes.n?nodes.v[0]:NULL;pv_free(&nodes);
    check(pane!=NULL&&!doc->parser,"normal parser tick and actual target");if(!pane){web_free(doc);return 1;}
    node_t *fragment=query(doc,"#fragment"),*fragment_parent=query(doc,"#fragment-parent");
    check(fragment&&fragment_parent,"ordinary inline fragment targets");if(!fragment||!fragment_parent){web_free(doc);return 1;}
    web_layout(doc,256,160);
    check(fragment_caches(fragment,false)>1,"fresh fragmented inline cache owners are explicitly null");
    paint(doc,0);web_layout(doc,256,160);paint(doc,0);
    check(fragment_caches(fragment,false)>1,"shadowless inline first paint and repeated layout never free garbage");
    doc_node_attr(doc,fragment,"style","box-shadow:0 1px 3px red");web_layout(doc,256,160);paint(doc,0);
    check(fragment_caches(fragment,true)>1,"ordinary inline fragments each own their shadow mask");
    doc_node_attr(doc,fragment_parent,"style","position:absolute;left:210px;top:5px;width:42px;margin:0;font-size:10px;line-height:12px");
    web_layout(doc,256,160);paint(doc,0);
    check(fragment_caches(fragment,true)>1,"inline reflow retires old geometry before building new masks");
    doc_node_attr(doc,fragment,"style","box-shadow:none");web_layout(doc,256,160);paint(doc,0);
    check(fragment_caches(fragment,false)>1,"inline shadow removal retires all fragment masks");
    set(doc,pane,"var(--shadow)","--shadow:0 10px 20px 0 rgba(0,0,0,.02),0 2px 6px 0 rgba(0,0,0,.06),0 0 0 1px rgba(0,0,0,.08);border-radius:20px");
    unsigned count=0;for(const struct css_box_shadow *s=pane->style->box_shadow;s;s=s->next)count++;
    check(count==3,"real variable supplies all three authored layers");
    check(pixel(49,70)!=0xffffff&&((pixel(49,70)>>16)&255)>190,"spread layer paints missing outside outline with alpha");
    check(pixel(100,70)==0xffffff,"outset shadow knocks out transparent border-box interior");
    check(pixel(49,49)==0xffffff||pixel(49,49)>pixel(49,70),"rounded outline corner does not become square");
    struct shadow_cache *retained=pane->box->shadow_cache;paint(doc,0);
    check(retained&&pane->box->shadow_cache==retained,"unchanged repaint reuses Gaussian masks without rebuilding");
    sbuf serialized={0};check(css_shadow_serialize(pane->style->box_shadow,&serialized,doc)&&strstr(serialized.p,"20px")&&strstr(serialized.p,", "),"computed serializer preserves full list");sb_free(&serialized);
    set(doc,pane,"inset 4px 0 0 red",NULL);
    check(css_shadow_serialize(pane->style->box_shadow,&serialized,doc)&&!strcmp(serialized.p,"rgb(255, 0, 0) 4px 0px 0px 0px inset"),"computed shadow canonical inset suffix matches CSSOM order");sb_free(&serialized);
    check(pixel(51,70)==0xff0000&&pixel(100,70)==0xffffff,"inset paints padding-edge knockout and leaves middle clear");
    check(pixel(49,70)==0xffffff,"inset never paints outside border box");
    set(doc,pane,"0 0 0 5px red,0 0 0 10px blue",NULL);
    check(pixel(46,70)==0xff0000&&pixel(41,70)==0x0000ff,"first shadow is above later shadows");
    set(doc,pane,"0 0 0 1px","color:green");check(pixel(49,70)==0x008000,"omitted shadow color uses computed currentColor");
    doc->paint_dirty=false;doc->paint_damage_full=doc->paint_damage_valid=false;
    doc_paint_damage(doc,pane);int x,y,w,h;bool damage=web_take_paint_damage(doc,&x,&y,&w,&h);
    check(pane->box->kind==B_BLOCK&&!damage,"blockified author inline-block keeps conservative full damage for unknown descendants");
    node_t *atomic=query(doc,"#atomic-damage");check(atomic!=NULL,"actual replaced atomic damage target");if(!atomic){web_free(doc);return 1;}
    doc_node_attr(doc,atomic,"style","position:absolute;left:50px;top:50px;width:100px;height:40px;box-shadow:10px 0 4px 6px green");
    web_layout(doc,256,160);
    check(atomic->box&&atomic->box->kind==B_ATOMIC,"damage fixture traverses actual native atomic box rather than a blockified block");
    doc->paint_dirty=false;doc->paint_damage_full=doc->paint_damage_valid=false;
    doc_paint_damage(doc,atomic);damage=web_take_paint_damage(doc,&x,&y,&w,&h);
    check(damage&&x<50&&y<50&&x+w>150&&y+h>90,"known atomic damage includes shadow ink outside its box");
    check(damage&&x<=47&&y<=37&&x+w>=173&&y+h>=103,"atomic damage includes shifted spread and Gaussian halo rather than only old two-pixel padding");
    doc_node_attr(doc,atomic,"style","display:none");web_layout(doc,256,160);
    set(doc,pane,"0 0 0 2px red","height:1000000px");
    check(pixel(49,70)==0xff0000,"tall author box shadows render with viewport-cropped masks");
    paint(doc,100000);check(pixel(49,70)==0xff0000,"scroll reveals a new cropped shadow slice rather than an old cache");
    set(doc,pane,"0 0 0 4px blue",NULL);uint32_t before[256*160];memcpy(before,pixels,sizeof before);
    doc_node_attr(doc,pane,"style","display:inline-block;position:absolute;left:50px;top:50px;width:100px;height:40px;box-shadow:0 10px 20px red");
    web_layout(doc,256,160);state.stop=true;doc->native_checkpoint_count=63;paint(doc,0);
    check(doc->native_cancelled&&doc->native_recovery_pending&&!memcmp(before,pixels,sizeof before),"cancelled shadow preflight preserves coherent canvas");
    state.stop=false;web_layout(doc,256,160);paint(doc,0);
    check(!doc->native_cancelled&&!doc->native_recovery_pending&&pane->style->box_shadow&&pixel(49,70)!=0xffffff,"outer recovery builds current shadow geometry");
    web_free(doc);printf("boxshadowtest: %d checks, %d failures\n",checks,failed);return failed!=0;
}
