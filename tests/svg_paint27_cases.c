static int checks, failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static bool supports(const char*p,const char*v){return css_value_supported(css_prop_lookup(p,strlen(p)),v,strlen(v));}
static void attribute(node_t*n,int at,const char*name,const char*value){n->attrs[at]=(struct attr){.name=name,.raw=name,.local=name,.value=value};if(at>=n->nattrs)n->nattrs=at+1;}
static void sheet(web_doc*d,const char*s){sheet_t*x=css_parse_sheet(&d->cssmem,s,strlen(s),d->url,2,NULL);pv_push(&d->sty.sheets,x);d->sty.index_dirty=true;}
int main(void){
    check(supports("fill","red")&&supports("stroke","none"),"paint colors and none are supported");
    check(supports("fill","rgba(0,128,255,.5)")&&supports("stroke","currentColor"),"CSS alpha and currentColor are parsed");
    check(!supports("fill","bad-paint")&&!supports("fill","context-fill"),"unknown and unsupported context paints reject honestly");
    check(supports("fill","url(#gradient)")&&!supports("fill","url(https://other.test/a.svg#g)"),"only supported local paint-server syntax");
    check(!supports("fill","url(#g) red")&&!supports("fill","url(#g);stroke:red"),"unimplemented fallback and extra declarations are not advertised");
    check(supports("fill-opacity","25%")&&!supports("stroke-width","-1px"),"paint opacity percentage and negative stroke width validation");
    web_doc d={.url="https://svg.test/document",.base="https://svg.test/document"};
    node_t doc={.type=N_DOC,.owner=&d}, svg={.type=N_ELEM,.namespace_id=NS_SVG,.foreign=true,.tag=T_svg,.name="svg",.raw_name="svg",.owner=&d,.parent=&doc};
    node_t g={.type=N_ELEM,.namespace_id=NS_SVG,.foreign=true,.tag=T_UNKNOWN,.name="g",.raw_name="g",.owner=&d,.parent=&svg};
    node_t path={.type=N_ELEM,.namespace_id=NS_SVG,.foreign=true,.tag=T_UNKNOWN,.name="path",.raw_name="path",.owner=&d,.parent=&g};
    node_t stop={.type=N_ELEM,.namespace_id=NS_SVG,.foreign=true,.tag=T_UNKNOWN,.name="stop",.raw_name="stop",.owner=&d,.parent=&svg};
    struct attr sa[5]={0},ga[5]={0},pa[9]={0},ta[3]={0};svg.attrs=sa;g.attrs=ga;path.attrs=pa;stop.attrs=ta;
    doc.first=&svg;svg.first=&g;g.first=&path;g.next=&stop;svg.last=&stop;g.last=&path;d.root=&doc;d.html=&svg;
    attribute(&svg,0,"width","20");attribute(&svg,1,"height","20");attribute(&svg,2,"fill","red");attribute(&svg,3,"style","opacity:.5");
    attribute(&g,0,"fill","green");attribute(&g,1,"stroke","red");
    attribute(&path,0,"fill","yellow");attribute(&path,1,"style","stroke: blue;stroke-dasharray:2 3");attribute(&path,2,"d","M0 0L10 0L10 10Z");
    attribute(&stop,0,"stop-color","red");
    sheet(&d,"svg {fill:blue} g {fill:currentColor;color:red;stroke-width:2px;fill-opacity:50%} path {fill:currentColor;color:#123456;stroke:green!important;stroke-linecap:round;stroke-linejoin:bevel;fill-rule:evenodd} stop {stop-color:blue;stop-opacity:25%}");
    css_cascade(&d,100,100);
    check(svg.style->svg_fill.color==0xff0000ff,"author SVG CSS overrides presentation fill");
    check(g.style->svg_fill.color==COLOR_CURRENT&&g.style->color==0xffff0000,"SVG descendant presentation and inherited painting properties cascade");
    check(path.style->svg_fill.color==COLOR_CURRENT&&path.style->color==0xff123456,"descendant currentColor remains an element-local used value");
    check(path.style->svg_stroke.color==0xff008000,"important author stroke overrides inline stroke");
    check(path.style->svg_fill_opacity==.5f&&path.style->svg_stroke_width.px==2,"fill opacity and stroke width inherit");
    check(stop.style->svg_stop_color==0xff0000ff&&stop.style->svg_stop_opacity==.25f,"gradient stop styling cascades");
    sbuf b={0};check(svg_ser(&d,&b,&svg,"#000000"),"actual SVG serializer accepts CSS-styled tree");sb_cstr(&b);
    check(strstr(b.p,"fill:#123456;")!=NULL&&strstr(b.p,"stroke:#008000;")!=NULL,"per-element colors reach serialized codec source");
    check(strstr(b.p,"fill-opacity:0.5;stroke-opacity:1;stroke-width:2;")!=NULL,"inherited effective paint values reach codec");
    check(strstr(b.p,"fill-rule:evenodd;stroke-linecap:round;stroke-linejoin:bevel;")!=NULL,"stroke and fill geometry keywords reach codec");
    check(strstr(b.p,"stop-color:#0000ff;stop-opacity:0.25;")!=NULL,"gradient stops reach codec");
    check(strstr(b.p,"stroke-dasharray:2 3")!=NULL,"unimplemented native CSS codec-supported inline property is retained");
    check(strstr(b.p,"opacity:1;")!=NULL,"atomic SVG root opacity is not composited twice");sb_free(&b);
    struct svg_cache *cached=doc_svg(&d,&svg,0xff000000);check(cached&&d.svgs.n==1,"real SVG cache published");
    check(doc_svg(&d,&svg,0xff000000)==cached&&d.svgs.n==1,"unchanged serialized source reuses identity");
    cached->img=(image_t *)(uintptr_t)1;path.style->svg_fill=(struct svg_paint){.kind=SVG_PAINT_COLOR,.color=0x8000ff00};
    check(doc_svg(&d,&svg,0xff000000)==cached&&freed_images==1&&!cached->img,"paint change invalidates old raster but preserves cache owner");
    check(strstr(cached->src,"fill:#00ff00;")&&strstr(cached->src,"fill-opacity:0.25098"),"color alpha multiplies fill opacity at codec boundary");
    attribute(&path,1,"style","fill:url(#gradient)");d.sty.index_dirty=true;css_cascade(&d,100,100);
    check(path.style->svg_fill.kind==SVG_PAINT_REF&&!strcmp(path.style->svg_fill.ref,"#gradient"),"absolute declaration URL is validated against actual owner document then retained locally");
    /* This member has an SVG-looking spelling but the wrong attribute namespace. */
    path.attrs[3]=(struct attr){.name="fill",.raw="Wrong:fill",.local="fill",.namespace_uri="https://wrong.test/",.value="red"};path.nattrs=4;
    css_cascade(&d,100,100);check(path.style->svg_fill.kind==SVG_PAINT_REF,"namespaced arbitrary fill cannot become presentation attribute");
    for(int i=0;i<d.svgs.n;i++){struct svg_cache*c=d.svgs.v[i];free(c->src);free(c);}pv_free(&d.svgs);
    css_styling_free(&d.sty);pv_free(&d.sty.sheets);ar_free(&d.cssmem);ar_free(&d.smem);
    printf("svg-paint27: %d checks, %d failures\n",checks,failures);return failures!=0;
}
