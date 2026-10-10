static int checks,failures;
static void check(bool ok,const char*n){checks++;if(!ok){failures++;printf("FAIL %s\n",n);}}
int main(void){
    const struct propdef *width=css_prop_lookup("stroke-width",12);
    check(css_value_supported(width,"20%",3),"SVG normalized-diagonal percentage is supported");
    check(!css_value_supported(width,"calc(1px + 20%)",15),"unimplemented mixed expression is not claimed");
    web_doc d={.url="https://svg.test/document",.base="https://svg.test/document"};
    node_t doc={.type=N_DOC,.owner=&d},svg={.type=N_ELEM,.namespace_id=NS_SVG,.foreign=true,.tag=T_svg,.name="svg",.raw_name="svg",.owner=&d,.parent=&doc};
    node_t path={.type=N_ELEM,.namespace_id=NS_SVG,.foreign=true,.tag=T_UNKNOWN,.name="path",.raw_name="path",.owner=&d,.parent=&svg};
    struct attr sa[]={{.name="width",.raw="width",.local="width",.value="20"},{.name="height",.raw="height",.local="height",.value="20"}};
    struct attr pa[3]={{.name="d",.raw="d",.local="d",.value="M0 0L20 20"},{.name="stroke-width",.raw="stroke-width",.local="stroke-width",.value="50%"},{.name="stroke",.raw="stroke",.local="stroke",.value="red"}};
    svg.attrs=sa;svg.nattrs=2;path.attrs=pa;path.nattrs=3;doc.first=&svg;svg.first=svg.last=&path;d.root=&doc;d.html=&svg;
    const char *text="path{stroke-width:20%}";sheet_t *sheet=css_parse_sheet(&d.cssmem,text,strlen(text),d.url,1,NULL);pv_push(&d.sty.sheets,sheet);d.sty.index_dirty=true;css_cascade(&d,100,100);
    check(path.style->svg_stroke_width.pct==20&&path.style->svg_stroke_width.px==0,"CSS preserves percentage and overrides SVG percentage attribute");
    sbuf source={0};svg_ser(&d,&source,&svg,"#000000");sb_cstr(&source);check(strstr(source.p,"stroke-width:20%;")!=NULL,"percentage reaches codec instead of being resolved against HTML parent");
    NSVGimage *decoded=nsvgParse(source.p,"px",96);check(decoded&&decoded->shapes&&fabsf(decoded->shapes->strokeWidth-4)<.001f,"actual codec resolves normalized 20 by 20 SVG diagonal");nsvgDelete(decoded);sb_free(&source);
    path.style->svg_stroke_width=(len_t){.kind=LK_LEN,.px=0};svg_ser(&d,&source,&svg,"#000000");sb_cstr(&source);decoded=nsvgParse(source.p,"px",96);
    check(decoded&&decoded->shapes&&decoded->shapes->strokeWidth==0,"explicit zero stroke width is preserved at codec boundary");nsvgDelete(decoded);sb_free(&source);
    css_styling_free(&d.sty);pv_free(&d.sty.sheets);ar_free(&d.cssmem);ar_free(&d.smem);
    printf("svg-paint27-percent: %d checks, %d failures\n",checks,failures);return failures!=0;
}
