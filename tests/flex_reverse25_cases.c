static unsigned checks, failures;
static arena_t style_arena;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static bool near(float a,float b){return fabsf(a-b)<0.03f;}
static len_t px(float x){return (len_t){.kind=LK_LEN,.px=x};}
static bool apply(style_t *s,const char *name,const char *value){
    uint8_t *set=calloc((size_t)css_prop_count(),1);
    struct cx cx={.s=s,.a=&style_arena,.set=set,.em=16,.rem=16,.vw=1920,.vh=1080};
    bool ok=css_apply(css_prop_lookup(name,strlen(name)),value,strlen(value),&cx);
    free(set);return ok;
}
struct scene {style_t group_style,styles[4],child_style;box_t group,items[4],child;};
static void scene(struct scene *s,bool column){
    memset(s,0,sizeof *s);css_style_init(&s->group_style,NULL);
    memset(s->group_style.border_width,0,sizeof s->group_style.border_width);
    s->group_style.display=D_FLEX;s->group_style.flex_wrap=FW_WRAP_REVERSE;
    s->group_style.align_items=AI_START;s->group_style.align_content=AC_FLEX_START;
    s->group_style.flex_direction=column?FD_COLUMN:FD_ROW;
    s->group_style.gap_row=5;s->group_style.gap_col=column?7:10;
    s->group=(box_t){.kind=B_FLEX,.st=&s->group_style,.w=column?120:110};
    static const float heights[]={20,40,30,10},widths[]={20,30,40,10};
    for(int i=0;i<4;i++){
        css_style_init(&s->styles[i],NULL);memset(s->styles[i].border_width,0,sizeof s->styles[i].border_width);
        s->styles[i].width=px(column?widths[i]:50);s->styles[i].height=px(column?30:heights[i]);
        s->styles[i].flex_shrink=0;
        s->items[i]=(box_t){.kind=B_BLOCK,.st=&s->styles[i],.parent=&s->group};
        if(i)s->items[i-1].next=&s->items[i];
    }
    s->group.first=&s->items[0];s->group.last=&s->items[3];
    css_style_init(&s->child_style,NULL);memset(s->child_style.border_width,0,sizeof s->child_style.border_width);
    s->child=(box_t){.kind=B_BLOCK,.st=&s->child_style};
    if(column)s->group_style.height=px(70);
}
static void run(struct scene *s){layout_inner(&s->group,NULL,0,0,-1);}
int main(void){
    web_doc doc={0};box_t other={0};D=&doc;doc.root_box=&other;VH=1080;
    style_t parsed;css_style_init(&parsed,NULL);char value[128]={0};
    check(apply(&parsed,"flex-wrap","wrap-reverse")&&parsed.flex_wrap==FW_WRAP_REVERSE,"real parser distinguishes reverse wrapping");
    serialize_flex(&parsed,"flex-wrap",value);check(!strcmp(value,"wrap-reverse"),"native computed serializer publishes reverse keyword");
    check(apply(&parsed,"flex-flow","column-reverse wrap-reverse")&&parsed.flex_direction==FD_COLUMN_REVERSE&&parsed.flex_wrap==FW_WRAP_REVERSE,"real flex-flow preserves both independent reverse axes");
    check(parsed.align_content==AC_NORMAL,"initial align-content is actual normal value");
    check(apply(&parsed,"align-content","stretch")&&parsed.align_content==AC_STRETCH,"explicit stretch has its own native value");
    serialize_flex(&parsed,"align-content",value);check(!strcmp(value,"stretch"),"computed stretch is not collapsed into normal");
    apply(&parsed,"align-content","normal");serialize_flex(&parsed,"align-content",value);
    check(!strcmp(value,"normal"),"computed normal is not relabeled stretch");
    check(!apply(&parsed,"align-content","safe center")&&!apply(&parsed,"align-content","baseline"),"unsupported content alignment is rejected not advertised");
    apply(&parsed,"align-content","start");serialize_flex(&parsed,"align-content",value);
    check(parsed.align_content==AC_START&&!strcmp(value,"start"),"logical start is not collapsed into flex-start");

    struct scene s;scene(&s,false);run(&s);
    check(near(s.group.h,75)&&near(s.items[0].y,55)&&near(s.items[1].y,35),"auto row reverse puts first line at cross-end without reversing its items");
    check(near(s.items[2].y,0)&&near(s.items[3].y,20),"second row advances toward physical top with per-item flex-start");
    check(near(s.items[0].x,0)&&near(s.items[1].x,60)&&s.items[0].next==&s.items[1],"reverse wrapping retains native item order and main gap");
    scene(&s,false);s.group_style.height=px(155);s.group_style.align_content=AC_STRETCH;run(&s);
    check(near(s.items[0].y,135)&&near(s.items[1].y,115)&&near(s.items[2].y,40),"definite reverse rows distribute stretch among lines before item alignment");
    scene(&s,false);s.group_style.height=px(155);s.group_style.align_content=AC_STRETCH;s.group_style.align_items=AI_END;run(&s);
    check(near(s.items[0].y,75)&&near(s.items[1].y,75)&&near(s.items[2].y,0),"reverse flex-end aligns item physical top");
    scene(&s,false);s.group_style.height=px(155);s.group_style.align_content=AC_STRETCH;s.group_style.align_items=AI_CENTER;run(&s);
    check(near(s.items[0].y,105)&&near(s.items[1].y,95)&&near(s.items[2].y,20),"reverse center uses final line sizes");
    scene(&s,false);s.group_style.height=px(155);run(&s);
    check(near(s.items[0].y,135)&&near(s.items[2].y,80),"content flex-start packs at reversed container edge");
    s.group_style.align_content=AC_FLEX_END;run(&s);
    check(near(s.items[0].y,55)&&near(s.items[2].y,0),"content flex-end uses opposite reversed edge");
    s.group_style.align_content=AC_START;run(&s);
    check(near(s.items[0].y,55)&&near(s.items[2].y,0),"content start stays physical block start under wrap reversal");
    s.group_style.align_content=AC_END;run(&s);
    check(near(s.items[0].y,135)&&near(s.items[2].y,80),"content end stays physical block end under wrap reversal");
    s.group_style.align_content=AC_CENTER;run(&s);
    check(near(s.items[0].y,95)&&near(s.items[2].y,40),"reverse content center retains author line order");
    s.group_style.align_content=AC_BETWEEN;run(&s);
    check(near(s.items[0].y,135)&&near(s.items[2].y,0),"reverse content space-between expands only interline gap");
    s.group_style.align_content=AC_AROUND;run(&s);
    check(near(s.items[0].y,115)&&near(s.items[2].y,20),"reverse content space-around preserves half outer spaces");
    s.group_style.align_content=AC_EVENLY;run(&s);
    check(near(s.items[0].y,108.33333f)&&near(s.items[2].y,26.66667f),"reverse content space-evenly uses equal outer spaces");
    scene(&s,false);s.group_style.height=px(155);s.group_style.align_content=AC_NORMAL;s.styles[0].margin[0].kind=LK_AUTO;run(&s);
    check(near(s.items[0].y,135)&&near(s.items[0].h,20),"physical auto top margin consumes reverse line free space");
    s.styles[0].margin[0]=px(0);s.styles[0].margin[2].kind=LK_AUTO;run(&s);
    check(near(s.items[0].y,75),"physical auto bottom margin is not mirrored into top");
    s.styles[0].margin[0].kind=LK_AUTO;run(&s);
    check(near(s.items[0].y,105),"two cross auto margins center independently of align-self");
    scene(&s,false);s.styles[0].margin[0]=px(5);s.styles[0].margin[2]=px(15);
    s.styles[0].padding[0]=px(2);s.styles[0].padding[2]=px(3);s.styles[0].border_width[0]=1;s.styles[0].border_width[2]=4;run(&s);
    check(near(s.group.h,85)&&near(s.items[0].y,43),"asymmetric margin border padding remain physical in reverse cross coordinates");
    scene(&s,false);s.group_style.flex_direction=FD_ROW_REVERSE;run(&s);
    check(near(s.items[0].x,60)&&near(s.items[1].x,0)&&near(s.items[0].y,55),"row-reverse main direction and wrap-reverse cross direction are independent");
    scene(&s,false);s.styles[0].order=2;run(&s);
    check(near(s.items[0].x,60)&&near(s.items[0].y,0)&&near(s.items[1].y,25),"stable order sorting happens before reverse line collection");
    scene(&s,false);s.group_style.height=px(155);s.group_style.align_content=AC_NORMAL;s.group_style.align_items=AI_STRETCH;s.styles[0].height.kind=LK_AUTO;
    s.child.parent=&s.items[0];s.items[0].first=s.items[0].last=&s.child;s.child_style.height=(len_t){.kind=LK_LEN,.pct=100};run(&s);
    check(near(s.items[0].h,80)&&near(s.child.h,80)&&near(s.items[0].y,75),"reverse stretched line reaches real percentage descendant layout");
    check(s.styles[0].height.kind==LK_AUTO&&s.child_style.height.pct==100,"reverse final pass does not mutate author preferred sizes");
    s.styles[0].max_height=px(50);run(&s);
    check(near(s.items[0].h,50)&&near(s.child.h,50)&&near(s.items[0].y,105),"clamped stretch remains flush with reverse cross-start");
    scene(&s,false);s.group_style.height=px(55);run(&s);
    check(near(s.items[0].y,35)&&near(s.items[2].y,-20),"reverse overflow is preserved rather than discarding overflowing lines");

    scene(&s,true);run(&s);
    check(near(s.items[0].x,100)&&near(s.items[1].x,90)&&near(s.items[2].x,43),"definite column reverse wrapping creates real separate right-to-left columns");
    check(near(s.items[0].y,0)&&near(s.items[1].y,35)&&near(s.items[2].y,0),"column wrapping restarts main positions and preserves row gap");
    s.group_style.flex_direction=FD_COLUMN_REVERSE;run(&s);
    check(near(s.items[0].y,40)&&near(s.items[1].y,5)&&near(s.items[2].x,43),"column-reverse and reverse wrapping independently swap their two axes");
    scene(&s,true);s.group_style.align_content=AC_STRETCH;s.group_style.align_items=AI_END;run(&s);
    check(near(s.items[0].x,68.5f)&&near(s.items[1].x,68.5f)&&near(s.items[2].x,0),"column line stretch precedes reversed flex-end item placement");
    s.group_style.align_items=AI_CENTER;run(&s);
    check(near(s.items[0].x,84.25f)&&near(s.items[2].x,10.75f),"column reverse center respects per-column final width");
    scene(&s,true);s.group_style.align_content=AC_STRETCH;s.group_style.align_items=AI_STRETCH;s.styles[0].width.kind=LK_AUTO;
    s.child.parent=&s.items[0];s.items[0].first=s.items[0].last=&s.child;s.child_style.width=(len_t){.kind=LK_LEN,.pct=100};run(&s);
    check(near(s.items[0].w,51.5f)&&near(s.child.w,51.5f)&&near(s.items[0].x,68.5f),"column reverse final width stretch relayouts real percentage child");
    check(near(s.items[0].h,30)&&s.styles[0].width.kind==LK_AUTO,"column cross relayout preserves used main height and author auto width");
    s.styles[0].max_width=px(25);run(&s);
    check(near(s.items[0].w,25)&&near(s.items[0].x,95),"column max-width stretch stays at reverse cross-start");
    scene(&s,true);s.group_style.align_content=AC_NORMAL;s.styles[0].margin[3].kind=LK_AUTO;run(&s);
    check(near(s.items[0].x,100),"column physical left auto margin consumes free cross width");
    s.styles[0].margin[3]=px(0);s.styles[0].margin[1].kind=LK_AUTO;run(&s);
    check(near(s.items[0].x,68.5f),"column physical right auto margin is not reversed");
    scene(&s,true);s.group_style.height.kind=LK_AUTO;run(&s);
    check(near(s.group.h,135)&&near(s.items[0].x,100)&&near(s.items[3].y,105),"indefinite column height does not invent main-axis line breaks");
    scene(&s,true);s.styles[0].height=px(100);run(&s);
    check(near(s.items[0].y,0)&&near(s.items[1].y,0)&&s.items[0].x>s.items[1].x,"oversized column item remains alone instead of empty line or loop");
    scene(&s,false);s.group.first=s.group.last=NULL;s.group_style.height=px(40);run(&s);
    check(near(s.group.h,40),"empty reverse container retains its definite height");

    /* First-baseline row groups use real baselines, not reversed text glyphs.
       Inline font shaping / vertical writing modes are not supplied here. */
    scene(&s,false);s.group_style.height=px(100);s.group_style.align_content=AC_FLEX_START;
    struct fitem items[2]={{.b=&s.items[0],.align=AI_BASELINE},{.b=&s.items[1],.align=AI_BASELINE}};
    s.items[0].h=20;s.items[0].baseline=15;s.items[1].h=40;s.items[1].baseline=10;
    struct fline line={.first=0,.end=2,.cross=45,.ascent=15,.descent=30};
    flex_cross_lines(&s.group,items,&line,1,false,false,100,0);
    check(near(s.items[0].y+s.items[0].baseline,70)&&near(s.items[1].y+s.items[1].baseline,70),"reverse first-baseline group uses maximum descent from its cross-start");
    check(!unexpected,"no unrelated shaping replaced table grid or float stub supplied the new results");
    ar_free(&style_arena);printf("flex-reverse25: %u checks, %u failures\n",checks,failures);return failures!=0;
}
