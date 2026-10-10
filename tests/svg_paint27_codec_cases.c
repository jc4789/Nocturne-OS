static int checks,failures;
static void check(bool ok,const char*n){checks++;if(!ok){failures++;printf("FAIL %s\n",n);}}
static void attr(node_t*n,int i,const char*name,const char*value){n->attrs[i]=(struct attr){.name=name,.raw=name,.local=name,.value=value};if(n->nattrs<=i)n->nattrs=i+1;}
int main(void){
    arena_t arena={0};struct hints h={.a=&arena};
    for(int i=0;i<128;i++)hint_value(&h,"fill","blue");
    check(h.n==128&&h.cap>=128,"presentation hint count grows beyond 24 without silently refusing");
    char *long_value=malloc(7001);memset(long_value,'a',7000);long_value[7000]=0;
    hint(&h,"fill","%s",long_value);
    check(strlen(h.d[128].value)==7000&&!memcmp(h.d[128].value,long_value,7000),"formatted presentation values preserve all bytes beyond 256");
    struct pctx pc={.a=&arena,.base="https://svg.test/document"};sbuf author={0};sb_puts(&author,"url(https://images.test/");sb_puts(&author,long_value);sb_puts(&author,")");
    char *resolved=absolute_urls(&pc,author.p,author.p+author.n);
    check(strlen(resolved)==author.n+2&&strstr(resolved,long_value),"CSS resource URLs keep full 7000-byte path beyond old 2048");
    sb_free(&author);free(long_value);ar_free(&arena);
    arena_t limited={.limit=1};jmp_buf trap;limited.trap=&trap;struct hints empty={.a=&limited};
    int jumped=setjmp(trap);
    if(!jumped)hint_value(&empty,"fill","red");
    check(jumped!=0&&empty.n==0&&!empty.d,"real arena allocation failure does not publish partial hint");ar_free(&limited);
    web_doc d={.url="https://svg.test/document",.base="https://svg.test/document"};
    node_t doc={.type=N_DOC,.owner=&d},svg={.type=N_ELEM,.namespace_id=NS_SVG,.foreign=true,.tag=T_svg,.name="svg",.raw_name="svg",.owner=&d,.parent=&doc};
    node_t path={.type=N_ELEM,.namespace_id=NS_SVG,.foreign=true,.tag=T_UNKNOWN,.name="path",.raw_name="path",.owner=&d,.parent=&svg};
    struct attr sa[3]={0},pa[2]={0};svg.attrs=sa;path.attrs=pa;doc.first=&svg;svg.first=svg.last=&path;d.root=&doc;d.html=&svg;
    attr(&svg,0,"width","20");attr(&svg,1,"height","20");attr(&svg,2,"style","opacity:.25");attr(&path,0,"d","M1 1L19 1L19 19L1 19Z");
    const char *text="path{fill:rgba(240,20,80,.8);fill-opacity:75%;stroke:#204060;stroke-opacity:50%;stroke-width:3px;stroke-linecap:square;stroke-linejoin:round;fill-rule:evenodd}";
    sheet_t *sheet=css_parse_sheet(&d.cssmem,text,strlen(text),d.url,1,NULL);pv_push(&d.sty.sheets,sheet);d.sty.index_dirty=true;css_cascade(&d,100,100);
    sbuf serialized={0};check(svg_ser(&d,&serialized,&svg,"#000000"),"new codec source generated from actual CSS cascade");sb_cstr(&serialized);
    NSVGimage *decoded=nsvgParse(serialized.p,"px",96);
    check(decoded&&decoded->shapes,"actual product NanoSVG codec builds geometry from computed source");
    if(decoded&&decoded->shapes){NSVGshape*s=decoded->shapes;
        check((s->fill.color&0xffffff)==NSVG_RGB(240,20,80),"codec uses author CSS fill rather than default black");
        check((s->fill.color>>24)==153,"codec carries multiplied fill alpha and fill opacity");
        check((s->stroke.color&0xffffff)==NSVG_RGB(32,64,96)&&(s->stroke.color>>24)==127,"codec uses computed stroke color and opacity");
        check(s->strokeWidth==3&&s->strokeLineCap==NSVG_CAP_SQUARE&&s->strokeLineJoin==NSVG_JOIN_ROUND,"codec receives real computed stroke geometry");
        check(s->fillRule==NSVG_FILLRULE_EVENODD,"codec uses computed CSS fill rule");
        check(s->opacity==1&&svg.style->opacity==.25f,"native outer compositor retains root alpha without codec double alpha");
    }
    nsvgDelete(decoded);sb_free(&serialized);css_styling_free(&d.sty);pv_free(&d.sty.sheets);ar_free(&d.cssmem);ar_free(&d.smem);
    printf("svg-paint27-codec: %d checks, %d failures\n",checks,failures);return failures!=0;
}
