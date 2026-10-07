/* Supplemental native web_live attribute batch. Reuses the real native
   fixture and drives incremental parser export/import, not a mocked DOM. */
#define main nocturne_jstest_main
#include "jstest.c"
#undef main

static void attribute_source_case(const char *html, const char *marker) {
    if (!open_case(html, false)) return;
    test_check(marker, pump(marker, 12000));
    test_check("attribute-parser-idle", pump(NULL, 5000));
    test_check("attribute-no-js-errors", fixture.errors == 0 && fixture.js_failures == 0);
    test_check("attribute-native-task-boundary", fixture.outside_running == 0);
    close_case();
}
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    external_case("js_attributes_cases.js", "runAttributeCases().then(n=>{check('attribute-case-batch-count',n>=150);mark('api-done');},e=>{console.error(e);mark('api-done');});", BASE);
    attribute_source_case(START "</head><body><div id='round' data-a='one'><script>"
        "globalThis.roundElement=document.getElementById('round');globalThis.roundMap=roundElement.attributes;"
        "globalThis.roundAttr=roundElement.getAttributeNode('data-a');roundAttr.value='two';"
        "roundElement.setAttributeNS('urn:Nocturne:Case','P:MiXeD','Case');"
        "roundElement.setAttributeNS('urn:first','p:same','1');roundElement.setAttributeNS('urn:second','p:same','2');"
        "roundElement.setAttributeNS('urn:Nocturne:Case','id','not-the-id');"
        "roundElement.setAttributeNS('http://www.w3.org/XML/1998/namespace','xml:lang','ja');"
        "roundElement.setAttributeNS(null,'context','plain');globalThis.roundBuiltin=[];"
        "for(const ns of ['http://www.w3.org/1999/xhtml','http://www.w3.org/2000/svg','http://www.w3.org/1998/Math/MathML']){"
        "roundElement.setAttributeNS(ns,'context','explicit');roundElement.setAttributeNS(ns,'C:BuiltinCase','prefixed');"
        "roundBuiltin.push([ns,roundElement.getAttributeNodeNS(ns,'context'),roundElement.getAttributeNodeNS(ns,'BuiltinCase')]);}"
        "roundElement.setAttribute('class','native-class');globalThis.roundNS=roundElement.getAttributeNodeNS('urn:Nocturne:Case','MiXeD');"
        "roundElement.removeAttributeNode(roundAttr);roundElement.setAttribute('data-z','last');roundElement.setAttributeNode(roundAttr);"
        "document.write('<span id=written>continued</span>');"
        "</script><script>"
        "check('attribute-round-map-identity',roundElement.attributes===roundMap);"
        "check('attribute-round-native-identity',roundElement.getAttributeNode('data-a')===roundAttr&&roundAttr.ownerElement===roundElement&&roundAttr.value==='two');"
        "check('attribute-round-ns-identity',roundElement.getAttributeNodeNS('urn:Nocturne:Case','MiXeD')===roundNS&&roundNS.name==='P:MiXeD'&&roundNS.prefix==='P');"
        "check('attribute-round-ns-case',roundNS.namespaceURI==='urn:Nocturne:Case'&&roundNS.localName==='MiXeD');"
        "check('attribute-round-duplicate-qualified',roundElement.getAttributeNames().filter(x=>x==='p:same').length===2&&roundElement.getAttributeNS('urn:first','same')==='1'&&roundElement.getAttributeNS('urn:second','same')==='2');"
        "check('attribute-round-id-boundary',document.getElementById('round')===roundElement&&roundElement.id==='round'&&roundElement.getAttributeNS('urn:Nocturne:Case','id')==='not-the-id');"
        "check('attribute-round-plain-namespace',roundElement.getAttributeNode('id').namespaceURI===null&&roundElement.getAttributeNodeNS(null,'context').value==='plain');"
        "check('attribute-round-explicit-builtin-namespaces',roundBuiltin.every(([ns,a,p])=>roundElement.getAttributeNodeNS(ns,'context')===a&&a.namespaceURI===ns&&a.prefix===null&&a.localName==='context'&&a.value==='explicit'&&roundElement.getAttributeNodeNS(ns,'BuiltinCase')===p&&p.namespaceURI===ns&&p.prefix==='C'&&p.localName==='BuiltinCase'));"
        "check('attribute-round-builtin-duplicate-qualified',roundElement.getAttributeNames().filter(x=>x==='context').length===4);"
        "check('attribute-round-order',roundElement.getAttributeNames().at(-1)==='data-a');"
        "check('attribute-round-class-cache',roundElement.classList.contains('native-class'));"
        "check('attribute-round-write',document.getElementById('written').textContent==='continued');"
        "roundAttr.value='three';roundElement.removeAttributeNS('urn:second','same');"
        "</script><script>"
        "check('attribute-round-second-native-identity',roundElement.getAttributeNode('data-a')===roundAttr&&roundAttr.value==='three'&&roundElement.getAttributeNS('urn:second','same')===null);"
        "check('attribute-round-third-ns-identity',roundElement.getAttributeNodeNS('urn:Nocturne:Case','MiXeD')===roundNS);"
        "check('attribute-round-second-builtin-namespaces',roundBuiltin.every(([ns,a,p])=>roundElement.getAttributeNodeNS(ns,'context')===a&&roundElement.getAttributeNodeNS(ns,'BuiltinCase')===p&&a.namespaceURI===ns&&p.prefix==='C'));"
        "mark('attribute-round-done');</script></div></body>", "attribute-round-done");
    attribute_source_case(START "</head><body><svg id='round-svg' viewBox='0 0 10 10' xlink:href='#a' xml:lang='ja'></svg>"
        "<script>globalThis.roundSVG=document.getElementById('round-svg');globalThis.roundSVGView=roundSVG.getAttributeNode('viewBox');"
        "globalThis.roundSVGLink=roundSVG.getAttributeNodeNS('http://www.w3.org/1999/xlink','href');"
        "roundSVGView.value='0 0 20 20';roundSVGLink.value='#b';"
        "roundSVG.setAttributeNS('urn:Nocturne:Case','P:MiXeD','svg');"
        "roundSVG.setAttributeNS('http://www.w3.org/2000/svg','viewBox','explicit-svg');globalThis.roundSVGExplicit=roundSVG.getAttributeNodeNS('http://www.w3.org/2000/svg','viewBox');"
        "</script><script>"
        "check('attribute-svg-round-view-identity',roundSVG.getAttributeNode('viewBox')===roundSVGView&&roundSVGView.localName==='viewBox'&&roundSVGView.namespaceURI===null&&roundSVGView.value==='0 0 20 20');"
        "check('attribute-svg-round-link-identity',roundSVG.getAttributeNodeNS('http://www.w3.org/1999/xlink','href')===roundSVGLink&&roundSVGLink.name==='xlink:href'&&roundSVGLink.value==='#b');"
        "check('attribute-svg-round-arbitrary-namespace',roundSVG.getAttributeNS('urn:Nocturne:Case','MiXeD')==='svg');"
        "check('attribute-svg-round-explicit-element-namespace',roundSVG.getAttributeNodeNS('http://www.w3.org/2000/svg','viewBox')===roundSVGExplicit&&roundSVGExplicit.namespaceURI==='http://www.w3.org/2000/svg'&&roundSVGExplicit.prefix===null&&roundSVGExplicit.value==='explicit-svg'&&roundSVG.getAttributeNodeNS(null,'viewBox')===roundSVGView);"
        "mark('attribute-svg-round-done');</script></body>", "attribute-svg-round-done");
    printf("js_attributes_native: %d checks, %d failed\n", total, failed);
    return failed != 0;
}
