static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static bool near(float a,float b){return fabsf(a-b)<0.02f;}
static len_t px(float value){return (len_t){.kind=LK_LEN,.px=value};}
static len_t pct(float value){return (len_t){.kind=LK_LEN,.pct=value};}
static style_t base_style(void){
    style_t s={0};s.display=D_BLOCK;s.opacity=1;
    s.max_height.kind=s.max_width.kind=LK_NONE;
    for(int i=0;i<4;i++)s.margin[i].kind=LK_LEN;
    s.align_self=s.justify_self=255;
    return s;
}
struct scene {
    style_t group_style,item_style,percent_style,leaf_style,sibling_style;
    box_t group,item,percent,leaf,sibling;
    struct gtrack row_tracks[2],col_tracks[2];
    struct gtemplate rows,cols;
};
static void scene(struct scene *s,int kind){
    memset(s,0,sizeof *s);
    s->group_style=s->item_style=s->percent_style=s->leaf_style=s->sibling_style=base_style();
    s->group_style.display=kind==B_GRID?D_GRID:kind==B_FLEX?D_FLEX:D_BLOCK;
    s->percent_style.height=pct(100);s->leaf_style.height=pct(50);
    s->group=(box_t){.kind=kind,.st=&s->group_style,.w=300,.first=&s->item,.last=&s->item};
    s->item=(box_t){.kind=B_BLOCK,.st=&s->item_style,.parent=&s->group,.first=&s->percent,.last=&s->percent};
    s->percent=(box_t){.kind=B_BLOCK,.st=&s->percent_style,.parent=&s->item,.first=&s->leaf,.last=&s->leaf};
    s->leaf=(box_t){.kind=B_BLOCK,.st=&s->leaf_style,.parent=&s->percent};
    s->sibling=(box_t){.kind=B_BLOCK,.st=&s->sibling_style,.parent=&s->group};
    for(int i=0;i<2;i++){
        s->col_tracks[i]=(struct gtrack){.min_kind=GT_LEN,.max_kind=GT_LEN,.min=px(150),.max=px(150)};
        s->row_tracks[i]=(struct gtrack){.min_kind=GT_LEN,.max_kind=GT_LEN,.min=px(100),.max=px(100)};
    }
    s->rows=(struct gtemplate){.n=1,.t=s->row_tracks};s->cols=(struct gtemplate){.n=1,.t=s->col_tracks};
    if(kind==B_GRID){s->group_style.grid_rows=&s->rows;s->group_style.grid_cols=&s->cols;}
}
static void sibling(struct scene *s,float height){
    s->item.next=&s->sibling;s->group.last=&s->sibling;
    s->sibling_style.height=px(height);
    if(s->group.kind==B_GRID)s->cols.n=2;
}
static void run(struct scene *s){layout_inner(&s->group,NULL,0,0,999);}
int main(void){
    web_doc doc={0};box_t unrelated_root={0};doc.root_box=&unrelated_root;D=&doc;VH=1080;
    struct scene s;
    scene(&s,B_FLEX);s.group_style.height=px(120);run(&s);
    check(near(s.item.h,120)&&near(s.percent.h,120),"final definite flex line reaches percentage child");
    check(near(s.leaf.h,60),"stretched native height reaches nested percentage chain");
    check(s.item_style.height.kind==LK_AUTO&&s.percent_style.height.pct==100,"CSS height auto and author percentage values stay unchanged");
    check(s.item.cb==&s.group&&s.percent.cb==&s.item&&s.leaf.cb==&s.percent,"native containing-block identity survives second pass");
    check(near(s.item.w,20)&&near(s.percent.w,20),"second height pass does not reflex main-axis width");
    scene(&s,B_FLEX);s.group_style.height=px(120);s.item_style.padding[0]=px(10);s.item_style.padding[2]=px(20);
    s.item_style.border_width[0]=s.item_style.border_width[2]=2;s.item_style.margin[0]=px(5);s.item_style.margin[2]=px(7);run(&s);
    check(near(s.item.h,74)&&near(s.percent.h,74),"flex used content height subtracts padding border margins once");
    scene(&s,B_FLEX);s.group_style.height=px(120);s.item_style.max_height=px(60);run(&s);
    check(near(s.item.h,60)&&near(s.percent.h,60),"flex max-height clamps child percentage reference");
    scene(&s,B_FLEX);s.group_style.height=px(120);s.item_style.min_height=px(140);run(&s);
    check(near(s.item.h,140)&&near(s.percent.h,140)&&near(s.group.h,120),"flex min-height wins without enlarging final line container");
    scene(&s,B_FLEX);s.group_style.height=px(120);s.item_style.margin[0].kind=LK_AUTO;run(&s);
    check(near(s.item.h,0)&&near(s.percent.h,0),"auto cross margin prevents flex stretch definiteness");
    scene(&s,B_FLEX);s.group_style.height=px(120);s.item_style.height=px(40);run(&s);
    check(near(s.item.h,40)&&near(s.percent.h,40),"explicit preferred height is not overridden by stretch");
    scene(&s,B_FLEX);s.item.kind=B_ATOMIC;s.item.atomic=AT_IMG;
    check(!stretches_height(&s.item,AI_STRETCH),"replaced image rejects native nonreplaced stretch helper");
    scene(&s,B_FLEX);sibling(&s,80);run(&s);
    check(near(s.group.h,80)&&near(s.item.h,80)&&near(s.percent.h,80),"auto flex cross size is definite only after sibling-sized final line");
    scene(&s,B_FLEX);sibling(&s,80);s.percent_style.height=pct(200);run(&s);
    check(near(s.group.h,80)&&near(s.item.h,80)&&near(s.percent.h,160),"cyclic flex percentage overflows without resizing the measured line");
    scene(&s,B_FLEX);sibling(&s,80);s.item_style.min_height=px(80);run(&s);
    check(near(s.item.h,80)&&near(s.percent.h,80),"same numeric measured and used height still changes child definiteness");
    scene(&s,B_FLEX);run(&s);
    check(near(s.group.h,0)&&near(s.item.h,0)&&near(s.percent.h,0),"finite ancestor and gap-free cyclic empty auto flex invent no positive height");
    scene(&s,B_FLEX);s.group_style.height=px(200);s.group_style.flex_wrap=1;s.item_style.width=px(180);sibling(&s,70);s.sibling_style.width=px(180);run(&s);
    check(near(s.item.h,0)&&near(s.percent.h,0)&&near(s.group.h,200),"wrapped flex item does not use whole definite container height as its line");
    scene(&s,B_FLEX);s.group_style.height=px(100);s.item.kind=B_ATOMIC;s.item.atomic=AT_INLINE_BLOCK;s.item_style.display=D_INLINE_BLOCK;run(&s);
    check(near(s.item.h,100)&&near(s.percent.h,100),"nonreplaced inline-block item uses actual descendant layout");
    scene(&s,B_GRID);run(&s);
    check(near(s.group.h,100)&&near(s.item.h,100)&&near(s.percent.h,100)&&near(s.leaf.h,50),"final fixed grid area propagates definite used height");
    check(s.item_style.height.kind==LK_AUTO&&s.item.st==&s.item_style,"grid sizing does not replace or mutate author computed style");
    scene(&s,B_GRID);s.row_tracks[0].min_kind=s.row_tracks[0].max_kind=GT_AUTO;sibling(&s,70);run(&s);
    check(near(s.group.h,70)&&near(s.item.h,70)&&near(s.percent.h,70),"auto grid row becomes definite after real sibling contribution");
    scene(&s,B_GRID);s.row_tracks[0].min_kind=s.row_tracks[0].max_kind=GT_AUTO;sibling(&s,70);s.percent_style.height=pct(200);run(&s);
    check(near(s.group.h,70)&&near(s.item.h,70)&&near(s.percent.h,140),"cyclic grid percentage final pass does not regrow auto track");
    scene(&s,B_GRID);s.row_tracks[0].min_kind=s.row_tracks[0].max_kind=GT_AUTO;s.percent_style.height=pct(200);run(&s);
    check(near(s.group.h,0)&&near(s.item.h,0)&&near(s.percent.h,0),"unconstrained empty auto grid cycle supplies no invented height");
    scene(&s,B_GRID);s.row_tracks[0].min_kind=s.row_tracks[0].max_kind=GT_AUTO;s.group_style.min_height=px(100);run(&s);
    check(near(s.group.h,100)&&near(s.item.h,0)&&near(s.percent.h,0),"min-height-only container is not a premeasurement percentage reference");
    scene(&s,B_GRID);s.item_style.align_self=AI_START;run(&s);
    check(near(s.item.h,0)&&near(s.percent.h,0),"nonstretch grid item auto height remains content-based");
    scene(&s,B_GRID);s.item_style.margin[2].kind=LK_AUTO;run(&s);
    check(near(s.item.h,0)&&near(s.percent.h,0),"auto grid block margin prevents stretch");
    scene(&s,B_GRID);s.item_style.height=px(30);run(&s);
    check(near(s.item.h,30)&&near(s.percent.h,30),"grid explicit item height keeps its own percentage base");
    scene(&s,B_GRID);s.row_tracks[0].min=s.row_tracks[0].max=px(50);
    s.sibling.parent=&s.item;s.item.first=&s.sibling;s.item.last=&s.percent;s.sibling.next=&s.percent;s.sibling_style.height=px(80);run(&s);
    check(near(s.group.h,50)&&near(s.item.h,50)&&near(s.sibling.h,80)&&near(s.percent.h,50),"fixed grid stretch can shrink used box while preserving overflowing content");
    scene(&s,B_GRID);s.item_style.max_height=px(40);run(&s);
    check(near(s.item.h,40)&&near(s.percent.h,40),"grid max-height clamps descendant percentage base");
    scene(&s,B_GRID);s.item_style.min_height=px(120);run(&s);
    check(near(s.group.h,100)&&near(s.item.h,120)&&near(s.percent.h,120),"grid item minimum does not resize fixed track in final pass");
    scene(&s,B_GRID);s.rows.n=2;s.row_tracks[0].min=s.row_tracks[0].max=px(40);s.row_tracks[1].min=s.row_tracks[1].max=px(60);s.group_style.gap_row=10;
    s.item_style.grid_place[0]=(struct gline){.kind=GL_LINE,.n=1};s.item_style.grid_place[2]=(struct gline){.kind=GL_LINE,.n=3};run(&s);
    check(near(s.group.h,110)&&near(s.item.h,110)&&near(s.percent.h,110),"spanned final grid area includes only its intervening gap");
    scene(&s,B_FLEX);s.group_style.height=px(120);s.item.kind=B_GRID;s.item_style.display=D_GRID;s.percent_style.height.kind=LK_AUTO;run(&s);
    check(near(s.item.h,120)&&near(s.percent.h,120)&&near(s.leaf.h,60),"used override reaches nested grid internal track sizing");
    scene(&s,B_GRID);s.item.kind=B_FLEX;s.item_style.display=D_FLEX;s.percent_style.height.kind=LK_AUTO;run(&s);
    check(near(s.item.h,100)&&near(s.percent.h,100)&&near(s.leaf.h,50),"used override reaches nested flex final line sizing");
    scene(&s,B_FLEX);s.group_style.height=px(90);run(&s);s.group_style.height.kind=LK_AUTO;run(&s);
    check(near(s.group.h,0)&&near(s.item.h,0)&&near(s.percent.h,0),"new native layout does not retain previous used-height override");
    scene(&s,B_BLOCK);s.item_style.min_height=px(95);run(&s);
    check(near(s.item.h,95)&&near(s.percent.h,0),"ordinary min-height-only block is outside flex grid definite override");
    check(!unexpected,"no unrelated table float replaced font frameset stub supplied a height");
    printf("stretch-height21: %u checks, %u failures\n",checks,failures);return failures!=0;
}
