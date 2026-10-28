static unsigned checks,failed;
static web_doc document={.live=true};
static void check(bool condition,const char *label){checks++;if(!condition){failed++;printf("FAIL %s\n",label);}}
static node_t element(int ns,const char *name,int tag){return (node_t){.owner=&document,.type=N_ELEM,.namespace_id=ns,.name=name,.raw_name=name,.tag=tag};}
static node_t data(int type,const char *text){return (node_t){.owner=&document,.type=type,.text=(char*)text,.textlen=strlen(text)};}
static void expected(node_t *n,bool strict,const char *value,const char *label){
    sbuf b={0};enum xml_serialization_result result=xml_serialize_node(n,&b,strict);
    check(result==XML_SERIALIZED&&b.n==strlen(value)&&(!b.n||!memcmp(b.p,value,b.n)),label);
    if(result==XML_SERIALIZED&&b.p&&strcmp(b.p,value))printf("  actual: %s\n  expected: %s\n",b.p,value);
    sb_free(&b);check(live_allocations==0,"temporary serializer storage freed");
}
static void invalid(node_t *n,const char *label){
    sbuf b={0};sb_puts(&b,"kept");enum xml_serialization_result result=xml_serialize_node(n,&b,true);
    check(result==XML_SERIALIZE_INVALID&&b.n==4&&!strcmp(b.p,"kept"),label);sb_free(&b);
    check(live_allocations==0,"invalid rollback frees scopes");
}
int main(void){
    node_t div=element(NS_HTML,"div",T_div),br=element(NS_HTML,"br",T_br),script=element(NS_HTML,"script",T_script);
    node_t text=data(N_TEXT,"<&>\xc2\xa0"),comment=data(N_COMMENT,"note"),pi=data(N_PI,"x='1'");pi.name="target";
    node_t svg=element(NS_SVG,"svg",T_svg),g=element(NS_SVG,"g",T_UNKNOWN),math=element(NS_MATHML,"math",T_math);
    node_t plain=element(NS_NONE,"plain",T_UNKNOWN),fragment={.owner=&document,.type=N_FRAGMENT},doc={.owner=&document,.type=N_DOC};
    expected(&div,false,"<div xmlns=\"http://www.w3.org/1999/xhtml\"></div>","HTML element pair and namespace");
    expected(&br,false,"<br xmlns=\"http://www.w3.org/1999/xhtml\" />","HTML void XML spacing");
    expected(&svg,false,"<svg xmlns=\"http://www.w3.org/2000/svg\"/>","foreign empty tag");
    expected(&plain,false,"<plain/>","null namespace empty tag");
    expected(&text,false,"&lt;&amp;&gt;\xc2\xa0","XML escapes with literal NBSP");
    script.first=&text;text.parent=&script;
    expected(&script,false,"<script xmlns=\"http://www.w3.org/1999/xhtml\">&lt;&amp;&gt;\xc2\xa0</script>","XML script text escaped");
    sbuf html={0};html_serialize_node(&script,&html);check(!strcmp(sb_cstr(&html),"<script><&>\xc2\xa0</script>"),"existing HTML raw script behavior preserved");sb_free(&html);
    expected(&comment,true,"<!--note-->","comment");expected(&pi,true,"<?target x='1'?>","processing instruction");
    node_t attr={.owner=&document,.type=N_ATTR};expected(&attr,false,"","Attr serializes empty");
    fragment.first=&div;div.next=&svg;
    expected(&fragment,false,"<div xmlns=\"http://www.w3.org/1999/xhtml\"></div><svg xmlns=\"http://www.w3.org/2000/svg\"/>","fragment sibling namespace isolation");div.next=NULL;
    div.first=&svg;svg.first=&g;g.first=&plain;plain.first=&math;
    expected(&div,true,"<div xmlns=\"http://www.w3.org/1999/xhtml\"><svg xmlns=\"http://www.w3.org/2000/svg\"><g><plain xmlns=\"\"><math xmlns=\"http://www.w3.org/1998/Math/MathML\"/></plain></g></svg></div>","default namespace transitions and reset");
    div.first=svg.first=g.first=plain.first=NULL;
    struct attr values[]={{.name="v",.local="v",.value="\t\n\r<&>\"\xc2\xa0"}};div.attrs=values;div.nattrs=1;
    expected(&div,false,"<div xmlns=\"http://www.w3.org/1999/xhtml\" v=\"&#9;&#xA;&#xD;&lt;&amp;&gt;&quot;\xc2\xa0\"></div>","attribute XML escaping and whitespace");
    struct attr namespaces[]={
        {.name="xmlns:p",.raw="xmlns:p",.local="p",.prefix="xmlns",.namespace_uri=XMLNS_NS,.value="urn:test"},
        {.name="p:one",.raw="p:one",.local="one",.prefix="p",.namespace_uri="urn:test",.value="1"},
        {.name="two",.raw="two",.local="two",.namespace_uri="urn:test",.value="2"},
        {.name="xml:lang",.raw="xml:lang",.local="lang",.prefix="xml",.namespace_uri=XML_NS,.value="ja"}};
    div.attrs=namespaces;div.nattrs=4;
    expected(&div,true,"<div xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:p=\"urn:test\" p:one=\"1\" p:two=\"2\" xml:lang=\"ja\"></div>","authored attribute namespace reused and xml implicit");
    struct attr generated[]={
        {.name="one",.local="one",.namespace_uri="urn:a",.value="1"},
        {.name="two",.local="two",.namespace_uri="urn:a",.value="2"},
        {.name="xmlns:ns1",.raw="xmlns:ns1",.local="ns1",.prefix="xmlns",.namespace_uri=XMLNS_NS,.value="urn:occupied"}};
    div.attrs=generated;div.nattrs=3;
    expected(&div,true,"<div xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:ns2=\"urn:a\" ns2:one=\"1\" ns2:two=\"2\" xmlns:ns1=\"urn:occupied\"></div>","generated collision-free prefix and reuse");
    struct attr sg[]={
        {.name="xmlns:s",.raw="xmlns:s",.local="s",.prefix="xmlns",.namespace_uri=XMLNS_NS,.value="http://www.w3.org/2000/svg"},
        {.name="xmlns",.raw="xmlns",.local="xmlns",.namespace_uri=XMLNS_NS,.value="http://www.w3.org/2000/svg"}};
    div.attrs=sg;div.nattrs=1;div.first=&svg;svg.attrs=&sg[1];svg.nattrs=1;
    expected(&div,true,"<div xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:s=\"http://www.w3.org/2000/svg\"><svg xmlns=\"http://www.w3.org/2000/svg\"/></div>","authored matching default preferred over ancestor alias");
    svg.attrs=NULL;svg.nattrs=0;div.first=NULL;
    svg.raw_name="s:svg";svg.prefix="s";svg.local_name="svg";
    expected(&svg,true,"<s:svg xmlns:s=\"http://www.w3.org/2000/svg\"/>","native qualified foreign prefix");svg.raw_name="svg";svg.prefix=NULL;
    node_t other=element(NS_OTHER,"x:item",T_UNKNOWN);other.prefix="x";other.local_name="item";other.namespace_uri="urn:x";
    expected(&other,true,"<x:item xmlns:x=\"urn:x\"/>","arbitrary namespace element keeps its prefix");
    node_t colon=element(NS_SVG,"a:b",T_UNKNOWN);colon.local_name="a:b";
    expected(&colon,false,"<a:b xmlns=\"http://www.w3.org/2000/svg\"/>","HTML-parser local name with a colon is not a prefix");
    invalid(&colon,"strict local name with a colon rejected");
    node_t upper=element(NS_HTML,"div",T_UNKNOWN);upper.local_name="DIV";upper.raw_name="DIV";
    sbuf upper_html={0};html_serialize_node(&upper,&upper_html);check(!strcmp(sb_cstr(&upper_html),"<DIV></DIV>"),"HTML serialization uses the HTML local name");sb_free(&upper_html);
    node_t section=data(N_TEXT,"a<b]]");section.cdata=true;
    expected(&section,true,"<![CDATA[a<b]]]]>","CDATA section syntax kept");
    section.text="x]]>y";section.textlen=5;invalid(&section,"strict CDATA end inside data rejected");
    expected(&section,false,"<![CDATA[x]]>y]]>","API false flag keeps CDATA data");
    sbuf section_html={0};html_serialize_node(&section,&section_html);check(!strcmp(sb_cstr(&section_html),"x]]&gt;y"),"HTML serialization escapes CDATA as text");sb_free(&section_html);
    struct attr default_wrong={.name="xmlns",.local="xmlns",.namespace_uri=XMLNS_NS,.value="urn:wrong"};div.attrs=&default_wrong;div.nattrs=1;
    expected(&div,true,"<div xmlns=\"http://www.w3.org/1999/xhtml\"></div>","conflicting local default repaired");
    struct attr empty_binding={.name="xmlns:p",.raw="xmlns:p",.local="p",.prefix="xmlns",.namespace_uri=XMLNS_NS,.value=""};
    div.attrs=&empty_binding;div.nattrs=1;div.first=&plain;
    expected(&div,false,"<div xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:p=\"\"><plain xmlns=\"\"/></div>","empty prefix declaration cannot alias null namespace child");
    invalid(&div,"strict empty prefixed namespace rejected");div.first=NULL;
    struct attr default_no_ns={.name="xmlns",.raw="xmlns",.local="xmlns",.value="urn:wrong"};div.attrs=&default_no_ns;
    expected(&div,false,"<div xmlns=\"http://www.w3.org/1999/xhtml\"></div>","HTML authored no-NS xmlns does not change node namespace");
    invalid(&div,"strict no-namespace xmlns rejected");
    node_t template=element(NS_HTML,"template",T_template),content={.owner=&document,.type=N_FRAGMENT};content.first=&br;template.template_content=&content;
    expected(&template,true,"<template xmlns=\"http://www.w3.org/1999/xhtml\"><br /></template>","template content uses native inert tree");
    div.attrs=NULL;div.nattrs=0;div.shadow_root=&content;
    expected(&div,true,"<div xmlns=\"http://www.w3.org/1999/xhtml\"></div>","shadow tree not leaked");div.shadow_root=NULL;
    node_t doctype={.owner=&document,.type=N_DOCTYPE,.name="html",.public_id="pub",.system_id="sys"};
    expected(&doctype,true,"<!DOCTYPE html PUBLIC \"pub\" \"sys\">","doctype public and system");
    doctype.public_id="";doctype.system_id="\"quoted\"";
    expected(&doctype,true,"<!DOCTYPE html SYSTEM '\"quoted\"'>","system literal alternate delimiter");
    doctype.system_id="'\"";invalid(&doctype,"system literal both quotes strict error");
    doctype.system_id="";doctype.public_id="nul\0tail";doctype.public_id_len=8;doctype.system_id_len=0;doctype.doctype_identifiers_sized=true;
    invalid(&doctype,"sized public identifier embedded NUL strict error");
    comment.text="bad--comment";comment.textlen=strlen(comment.text);invalid(&comment,"comment double-hyphen strict error");expected(&comment,false,"<!--bad--comment-->","API false wellformed flag does not reject comment");
    comment.text="end-";comment.textlen=4;invalid(&comment,"comment trailing hyphen strict error");
    pi.name="XML";invalid(&pi,"PI reserved xml target strict error");pi.name="ok";pi.text="?>";pi.textlen=2;invalid(&pi,"PI closing sequence strict error");expected(&pi,false,"<?ok ?>?>","API false flag preserves PI");
    text.text="\xc0\x80";text.textlen=2;invalid(&text,"invalid UTF-8 strict error");text.text="nul\0x";text.textlen=5;invalid(&text,"embedded NUL text strict error");
    invalid(&doc,"strict document missing element");expected(&doc,false,"","false document empty allowed");
    doctype=(node_t){.owner=&document,.type=N_DOCTYPE,.name="html"};doc.first=&doctype;doctype.next=&div;
    expected(&doc,true,"<!DOCTYPE html><div xmlns=\"http://www.w3.org/1999/xhtml\"></div>","document doctype and root namespace");
    struct attr badname={.name="bad:name",.local="bad:name",.value="x"};div.attrs=&badname;div.nattrs=1;invalid(&div,"strict attribute colon local name rejected");
    struct attr duplicate[]={{.name="x",.local="x",.value="a"},{.name="x",.local="x",.value="b"}};div.attrs=duplicate;div.nattrs=2;invalid(&div,"strict duplicate namespace/local rejected");
    div.attrs=NULL;div.nattrs=0;
    enum {DEPTH=20000};node_t *deep=malloc(DEPTH*sizeof *deep);
    for(int i=0;i<DEPTH;i++){deep[i]=element(NS_HTML,"div",T_div);if(i+1<DEPTH)deep[i].first=&deep[i+1];}
    sbuf deepout={0};check(xml_serialize_node(deep,&deepout,false)==XML_SERIALIZED&&deepout.n==DEPTH*11+37,"native deep tree grows frames without recursion/cap");sb_free(&deepout);free(deep);
    check(live_allocations==0,"deep traversal temporary storage freed");
    div.attrs=generated;div.nattrs=3;int needed=0;{sbuf b={0};sb_puts(&b,"kept");allocations=0;check(xml_serialize_node(&div,&b,false)==XML_SERIALIZED,"fault baseline serialization");needed=allocations;sb_free(&b);}
    for(int i=0;i<needed;i++){sbuf b={0};sb_puts(&b,"kept");allocations=0;fail_allocation=i;enum xml_serialization_result result=xml_serialize_node(&div,&b,false);fail_allocation=-1;check(result==XML_SERIALIZE_OOM&&b.n==4&&!strcmp(b.p,"kept"),"allocator failure rolls back output");sb_free(&b);check(live_allocations==0,"allocator failure frees scoped bindings");}
    div.attrs=NULL;div.nattrs=0;div.first=&text;text.text="payload";text.textlen=7;checkpoints=0;cancel_at=1;
    sbuf cancel={0};sb_puts(&cancel,"kept");check(xml_serialize_node(&div,&cancel,false)==XML_SERIALIZE_CANCELLED&&cancel.n==4&&!strcmp(cancel.p,"kept"),"native interruption rollback");cancel_at=-1;sb_free(&cancel);check(live_allocations==0,"interruption temporary storage freed");
    web_doc inactive={.inert=true,.dom_family=&document};div.owner=&inactive;checkpoints=0;cancel_at=0;
    sbuf inert_cancel={0};check(xml_serialize_node(&div,&inert_cancel,false)==XML_SERIALIZE_CANCELLED&&last_checkpoint_document==&document&&div.owner==&inactive,"inert native tree services owning family checkpoint without adoption");cancel_at=-1;sb_free(&inert_cancel);
    printf("HTML/XML serializer supporting checks %u, failed %u\n",checks,failed);return failed?1:0;
}
