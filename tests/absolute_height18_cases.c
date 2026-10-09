static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static bool near(float a,float b){return fabsf(a-b)<0.02f;}
static style_t base_style(void){style_t s={0};s.display=D_BLOCK;s.opacity=1;s.max_height.kind=s.max_width.kind=LK_NONE;s.inset[0].kind=s.inset[1].kind=s.inset[2].kind=s.inset[3].kind=LK_AUTO;return s;}
int main(void){
    web_doc doc={0};D=&doc;VW=1086;VH=1080;
    style_t rs=base_style(),as=base_style(),cs=base_style(),ns=base_style();
    box_t root={.kind=B_BLOCK,.st=&rs,.w=VW,.h=VH},aspect={.kind=B_BLOCK,.st=&rs,.w=1086,.h=0},a={.kind=B_BLOCK,.st=&as,.w=1086,.abspos=true},custom={.kind=B_BLOCK,.st=&cs},nested={.kind=B_BLOCK,.st=&ns};
    node_t custom_node={.type=N_ELEM,.tag=0,.box=&custom};custom.node=&custom_node;
    doc.root_box=&root;aspect.parent=aspect.cb=&root;rs.position=POS_RELATIVE;aspect.p[0]=611;
    a.parent=&aspect;as.position=POS_ABSOLUTE;as.inset[0].kind=as.inset[1].kind=as.inset[2].kind=as.inset[3].kind=LK_LEN;
    a.first=a.last=&custom;custom.parent=&a;custom.first=custom.last=&nested;nested.parent=&custom;
    cs.height=(len_t){.kind=LK_LEN,.pct=100};cs.overflow=OV_HIDDEN;ns.height=cs.height;
    check(near(resolved_content_height(&a,611),611),"auto absolute top-bottom produces definite content height");
    layout_abs(&a);
    check(near(a.h,611),"actual absolute layout retains outer 611 height");
    check(near(custom.h,611),"custom element percentage height receives definite parent before layout");
    check(near(nested.h,611),"nested percentage chain does not collapse behind overflow clipping");
    check(custom.cb==&a&&nested.cb==&custom,"actual block layout preserves native containing-block chain");
    check(near(a.y,-611)&&near(custom.y,0),"padding-derived aspect containing block uses actual content origin");
    check(near(custom.w,1086),"height repair does not change horizontal layout");
    a.abspos=false;as.position=POS_STATIC;layout_inner(&a,NULL,0,0,611);
    check(near(a.h,0)&&near(custom.h,0)&&near(nested.h,0),"ordinary auto height remains indefinite despite finite ancestor cbh");
    check(resolved_content_height(&a,611)<0,"normal-flow auto helper does not invent a definite height");
    as.min_height=(len_t){.kind=LK_LEN,.px=300};layout_inner(&a,NULL,0,0,611);
    check(near(a.h,300)&&near(custom.h,0),"min-height-only normal box does not give percentage children a base");as.min_height.kind=LK_AUTO;
    a.abspos=true;as.position=POS_ABSOLUTE;as.inset[2].kind=LK_AUTO;layout_abs(&a);
    check(near(a.h,0)&&near(custom.h,0),"one auto inset leaves intrinsic absolute height indefinite");
    as.inset[2].kind=LK_LEN;
    check(resolved_content_height(&a,-1)<0,"indefinite containing height cannot resolve insets");
    as.height=(len_t){.kind=LK_LEN,.pct=100};
    check(resolved_content_height(&a,-1)<0,"indefinite percentage height is not replaced by zero");
    as.height.kind=LK_AUTO;as.height.pct=0;as.inset[0].px=40;as.inset[2].px=71;layout_abs(&a);
    check(near(a.h,500)&&near(custom.h,500)&&near(nested.h,500),"finite inset subtraction reaches each percentage descendant");
    as.inset[0]=(len_t){.kind=LK_LEN,.pct=10};as.inset[2]=(len_t){.kind=LK_LEN,.pct=20};layout_abs(&a);
    check(near(a.h,427.7f)&&near(custom.h,427.7f),"percentage insets use absolute padding-box containing height");
    as.inset[0]=(len_t){.kind=LK_LEN};as.inset[2]=as.inset[0];as.padding[0]=(len_t){.kind=LK_LEN,.px=10};as.padding[2]=(len_t){.kind=LK_LEN,.px=20};as.border_width[0]=as.border_width[2]=2;
    as.margin[0]=(len_t){.kind=LK_LEN,.px=5};as.margin[2]=(len_t){.kind=LK_LEN,.px=7};layout_abs(&a);
    check(near(a.h,565)&&near(custom.h,565),"content height subtracts actual padding border and margins");
    as.max_height=(len_t){.kind=LK_LEN,.px=400};layout_abs(&a);
    check(near(a.h,400)&&near(custom.h,400)&&near(nested.h,400),"maximum height constrains descendant percentage reference before layout");
    as.min_height=(len_t){.kind=LK_LEN,.px=450};layout_abs(&a);
    check(near(a.h,450)&&near(custom.h,450),"minimum wins constrained maximum for native descendants");
    as.height=(len_t){.kind=LK_LEN,.px=220};as.min_height.kind=LK_AUTO;as.max_height.kind=LK_NONE;layout_abs(&a);
    check(near(a.h,220)&&near(custom.h,220),"explicit height still uses content-box percentage reference");
    as.box_sizing=1;layout_abs(&a);check(near(a.h,186)&&near(custom.h,186),"explicit border-box height subtracts native edges once");
    as.height.kind=LK_AUTO;layout_abs(&a);check(near(a.h,565)&&near(custom.h,565),"auto inset fill is not double-subtracted by border-box sizing");
    a.kind=B_ATOMIC;a.atomic=AT_IMG;check(resolved_content_height(&a,611)<0,"replaced elements retain their separate intrinsic sizing path");a.kind=B_BLOCK;
    node_t modal_node={.type=N_ELEM,.box=&a};a.node=modal=&modal_node;
    check(resolved_content_height(&a,611)<0,"top-layer modal auto height is not stretched by arbitrary insets");a.node=modal=NULL;
    as.position=POS_FIXED;a.parent=NULL;layout_abs(&a);
    check(near(a.h,1034)&&near(custom.h,1034),"fixed inset fill uses real viewport and propagates before children");
    check(!unexpected,"no unrelated stub path supplied sizes or suppressed a failed layout");
    printf("absolute-height18: %u checks, %u failures\n",checks,failures);return failures!=0;
}
