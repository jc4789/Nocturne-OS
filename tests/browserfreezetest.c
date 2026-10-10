/* Focused web_live/native regression; not a real-site acceptance claim. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "webi.h"
#include "cssom.h"

static int checks, failed, js_checks, errors, checkpoints;
static bool stop_native;
static void check(bool ok, const char *name) {
    checks++; if (!ok) { failed++; printf("FAIL browserfreeze %s\n", name); }
}
static void console(void *opaque, int level, const char *message) {
    (void)opaque;
    if (!strncmp(message, "OK freeze ", 10)) js_checks++;
    else if (level == 2 || !strncmp(message, "FAIL freeze ", 12)) { errors++; printf("  %s\n", message); }
}
static bool checkpoint(void *opaque) { (void)opaque; checkpoints++; return !stop_native; }
static web_doc *load(void) {
    const char *html = "<!doctype html><html><head></head><body></body></html>";
    struct web_host host = {.console=console, .script_checkpoint=checkpoint};
    web_doc *d = web_live(html, strlen(html), "https://freeze.test/", "utf-8", &host);
    if (d) for (int i=0;i<8;i++) web_tick(d, uptime_ms());
    return d;
}
static node_t *element(web_doc *d, const char *name) { return doc_node_create(d, N_ELEM, name, NULL, 0); }
static void slots(void) {
    web_doc *d=load(); check(d!=NULL,"slot document"); if(!d)return;
    node_t *roots[24], *hosts[24], *slots[24];
    for(int i=0;i<24;i++) {
        hosts[i]=element(d,"div"); roots[i]=doc_shadow_attach(d,hosts[i],false,false,false,false,false);
        slots[i]=element(d,"slot"); doc_node_move(d,roots[i],slots[i],NULL);
    }
    doc_shadow_flush(d);
    uint64_t before=d->profile.shadow_reassigns, visits=d->profile.shadow_visits;
    for(int i=0;i<2000;i++) element(d,"span"); /* historical detached allocations are irrelevant */
    for(int i=0;i<120;i++) doc_node_move(d,roots[5],element(d,"i"),NULL);
    check(d->profile.shadow_reassigns==before,"many mutations remain batched");
    node_t *light=element(d,"span"); doc_node_move(d,hosts[5],light,NULL);
    check(doc_assigned_slot(light,false)==slots[5],"read flushes assignment");
    check(d->profile.shadow_reassigns==before+1,"only affected root recomputed");
    check(d->profile.shadow_visits-visits<250,"no historical allocation or unrelated-root walk");
    node_t *other=element(d,"span");doc_node_move(d,hosts[7],other,NULL);doc_shadow_flush(d);
    doc_node_move(d,roots[7],slots[5],slots[7]);
    check(doc_assigned_slot(other,false)==slots[5] && !doc_assigned_slot(light,false),"slot moved across dirty roots");
    check(!slots[7]->slot_assigned_first,"duplicate name first slot wins");
    doc_node_remove(d,slots[5]);
    check(doc_assigned_slot(other,false)==slots[7] && !slots[5]->slot_assigned_first,"removed slot clears old snapshots");
    node_t *named=element(d,"slot");doc_node_attr(d,named,"name","named");doc_node_move(d,roots[7],named,NULL);
    doc_node_attr(d,other,"slot","named");
    check(doc_assigned_slot(other,false)==named,"slot/name attributes invalidate local root");
    web_free(d);
}
static void css_api(void) {
    web_doc *d=load();check(d!=NULL,"CSS document");if(!d)return;
    const char *code=
        "function t(n,v){console.log((v?'OK':'FAIL')+' freeze '+n)};"
        "const sheet=new CSSStyleSheet();t('construct',sheet instanceof CSSStyleSheet);"
        "t('owner',sheet.ownerNode===null&&sheet.href===null&&sheet.title===null);"
        "sheet.replaceSync('div { height:17px; color:rgb(255,0,0) }');t('rules',sheet.cssRules.length===1);"
        "const a=document.createElement('div'),b=document.createElement('div');document.body.append(a,b);"
        "const ra=a.attachShadow({mode:'open'}),rb=b.attachShadow({mode:'open'});"
        "ra.innerHTML='<div id=x></div>';rb.innerHTML='<div id=x></div>';"
        "ra.adoptedStyleSheets=[sheet];rb.adoptedStyleSheets=[sheet];"
        "t('identity',ra.adoptedStyleSheets[0]===sheet&&rb.adoptedStyleSheets[0]===sheet);"
        "t('array',Array.isArray(ra.adoptedStyleSheets)&&ra.adoptedStyleSheets===ra.adoptedStyleSheets);"
        "t('scope',getComputedStyle(ra.querySelector('#x')).height==='17px');"
        "t('shared',getComputedStyle(rb.querySelector('#x')).height==='17px');"
        "sheet.replaceSync('div { height:29px }');"
        "t('replaceA',getComputedStyle(ra.querySelector('#x')).height==='29px');"
        "t('replaceB',getComputedStyle(rb.querySelector('#x')).height==='29px');"
        "t('noStyleClones',ra.querySelectorAll('style').length===0&&ra.styleSheets.length===0);"
        "ra.adoptedStyleSheets.pop();t('pop',ra.adoptedStyleSheets.length===0);"
        "ra.adoptedStyleSheets.push(sheet);t('push',ra.adoptedStyleSheets[0]===sheet);"
        "ra.adoptedStyleSheets.splice(0,1);t('splice',ra.adoptedStyleSheets.length===0);"
        "document.adoptedStyleSheets=[sheet];t('document',getComputedStyle(a).height==='29px');"
        "sheet.disabled=true;t('disable',getComputedStyle(a).height!=='29px');sheet.disabled=false;"
        "t('enable',getComputedStyle(a).height==='29px');"
        "sheet.replaceSync('@import url(https://never.test/a.css); div{height:31px}');"
        "t('noImport',sheet.cssRules.length===1&&sheet.cssRules[0].type===1);"
        "const style=document.createElement('style');style.textContent='p{color:red}';document.head.append(style);"
        "try{ra.adoptedStyleSheets=[style.sheet];t('rejectOwner',false)}catch(e){t('rejectOwner',e.name==='NotAllowedError')}"
        "t('atomic',ra.adoptedStyleSheets.length===0);"
        "try{style.sheet.replaceSync('p{}');t('rejectReplace',false)}catch(e){t('rejectReplace',e.name==='NotAllowedError')}"
        "sheet.insertRule('span{height:9px}',1);t('insert',sheet.cssRules.length===2);"
        "sheet.deleteRule(1);t('delete',sheet.cssRules.length===1);"
        "sheet.replace('div{height:33px}').then(s=>t('promise',s===sheet));";
    check(web_console_eval(d,code,strlen(code)),"real QuickJS/CSSOM evaluation");
    check(js_checks==23,"all constructed/adopted API assertions");check(errors==0,"no JS errors");
    node_t *root=d->body->first->shadow_root;
    check(root && root->adopted_count==0,"native observable list removal");
    web_doc *other=load();int error;struct cssom_sheet *foreign=cssom_construct(other,&error);
    check(cssom_adopt(root,&foreign,1)==CSSOM_NOT_ALLOWED,"native cross-document rejection");
    web_free(other);web_free(d);
}
static void cancellation(void) {
    web_doc *d=load();check(d!=NULL,"cancel document");if(!d)return;
    for(int i=0;i<250;i++)doc_node_move(d,d->body,element(d,"div"),NULL);
    stop_native=true;checkpoints=0;web_layout(d,800,600);
    check(checkpoints>0&&d->native_cancelled,"native scan/cascade observes host stop");
    check(!d->layout_valid && !d->root_box,"partial layout is unpublished");
    int calls=checkpoints;web_layout(d,800,600);
    check(checkpoints==calls,"stopped layout is not retried");
    stop_native=false;web_free(d);
}
static void layout_cancellation(bool grid) {
    web_doc *d=load();check(d!=NULL,"flex cancellation document");if(!d)return;
    doc_rescan(d);web_layout(d,800,600);
    node_t *flex=element(d,"div");doc_node_attr(d,flex,"style",grid?"display:grid;grid-template-columns:repeat(16,1fr)":"display:flex");doc_node_move(d,d->body,flex,NULL);
    for(int i=0;i<250;i++) {
        node_t *n=element(d,"div");char style[64];snprintf(style,sizeof style,"order:%d",grid?0:250-i);
        doc_node_attr(d,n,"style",style);doc_node_move(d,flex,n,NULL);
    }
    boxes_discard(d);css_cascade(d,800,600);boxes_build(d,&d->bmem);d->need_style=false;d->need_boxes=false;
    d->layout_valid=false;d->native_checkpoint_count=0;
    stop_native=true;checkpoints=0;web_layout(d,800,600);
    check(checkpoints>0 && d->native_cancelled && !d->root_box,grid?"native grid placement stops and discards partial geometry":"native flex sort stops and discards partial geometry");
    check(!d->layout_valid,"cancelled geometry is not cached");
    stop_native=false;web_free(d);
}
int main(int argc, char **argv) {
    if(argc>1&&!strcmp(argv[1],"--layout-only"))layout_cancellation(false);
    else if(argc>1&&!strcmp(argv[1],"--grid-only"))layout_cancellation(true);
    else { slots();css_api();cancellation();layout_cancellation(false);layout_cancellation(true); }
    printf("browserfreezetest: %d checks, %d failed\n",checks,failed);return failed?1:0;
}
