static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static bool near(float a,float b){return fabsf(a-b)<0.01f;}
static void reset(box_t *cb){cb->scroll_w=cb->scroll_h=0;}
int main(void){
    web_doc doc={0};D=&doc;style_t style={.display=D_BLOCK};
    box_t grand={.kind=B_BLOCK,.st=&style,.x=1e20f,.y=-1e20f};
    box_t cb={.kind=B_BLOCK,.st=&style,.cb=&grand,.content_dy=11,.p={2,3,5,7}};
    box_t b={.kind=B_BLOCK,.st=&style,.cb=&cb,.parent=&cb,.x=13,.y=17,.rel_dx=19,.rel_dy=23,.w=40,.h=50,.scroll_w=90,.scroll_h=110,.p={6,4,8,10},.b={0,2,3,0},.m={0,5,7,0}};
    scroll_area_publish(&b);
    check(near(cb.scroll_w,129)&&near(cb.scroll_h,172),"visible overflow contributes local content and native edges");
    check(near(cb.scroll_h,172),"parent content alignment offset is preserved exactly once");
    check(near(cb.scroll_w,129),"huge shared ancestor cannot erase small local width");
    grand.x=grand.y=3e30f;reset(&cb);scroll_area_publish(&b);
    check(near(cb.scroll_w,129)&&near(cb.scroll_h,172),"shared ancestor movement does not change relative bounds");
    style.overflow=OV_HIDDEN;reset(&cb);scroll_area_publish(&b);
    check(near(cb.scroll_w,93)&&near(cb.scroll_h,126),"clipped private overflow stays inside native border bounds");
    style.overflow=OV_AUTO;reset(&cb);scroll_area_publish(&b);
    check(near(cb.scroll_w,93)&&near(cb.scroll_h,126),"scrolling child does not leak private scroll area into parent");
    viewport=&b;reset(&cb);scroll_area_publish(&b);
    check(near(cb.scroll_w,129)&&near(cb.scroll_h,172),"viewport-overflow policy remains authoritative");viewport=NULL;
    style.overflow=OV_VISIBLE;box_t dom_parent={.kind=B_BLOCK,.st=&style};b.parent=&dom_parent;
    style.position=POS_ABSOLUTE;reset(&cb);scroll_area_publish(&b);
    check(near(cb.scroll_w,129)&&near(cb.scroll_h,172)&&!dom_parent.scroll_w&&!dom_parent.scroll_h,"absolute contribution goes to actual cb not unrelated DOM paint parent");
    style.position=POS_FIXED;reset(&cb);scroll_area_publish(&b);
    check(!cb.scroll_w&&!cb.scroll_h,"fixed content still excluded from document scroller");style.position=POS_STATIC;
    top_layer=&b;reset(&cb);scroll_area_publish(&b);
    check(!cb.scroll_w&&!cb.scroll_h,"top-layer native guard still excludes contribution");top_layer=NULL;
    b.kind=B_INLINE;scroll_area_publish(&b);check(!cb.scroll_w&&!cb.scroll_h,"inline fragments not counted twice");
    b.kind=B_TEXT;scroll_area_publish(&b);check(!cb.scroll_w&&!cb.scroll_h,"text run box not counted twice");
    b.kind=B_BR;scroll_area_publish(&b);check(!cb.scroll_w&&!cb.scroll_h,"break box excluded");b.kind=B_BLOCK;
    b.anon=true;reset(&cb);scroll_area_publish(&b);check(near(cb.scroll_w,129),"anonymous block contributes without fake DOM node");
    b.cb=NULL;reset(&cb);scroll_area_publish(&b);check(!cb.scroll_w&&!cb.scroll_h,"root with no coordinate parent is safe");b.cb=&cb;
    b.x=33;cb.content_dy=21;reset(&cb);scroll_area_publish(&b);
    check(near(cb.scroll_w,149)&&near(cb.scroll_h,182),"native mutation is recalculated directly without stale cache");
    cb.scroll_w=300;cb.scroll_h=400;scroll_area_publish(&b);
    check(near(cb.scroll_w,300)&&near(cb.scroll_h,400),"contribution preserves existing larger sibling overflow");
    enum{DEPTH=4097};box_t *forest=calloc(DEPTH,sizeof *forest);
    if(!forest){printf("native allocation failed\n");return 1;}
    for(int i=0;i<DEPTH;i++){forest[i].kind=B_BLOCK;forest[i].st=&style;forest[i].x=3;forest[i].y=4;forest[i].content_dy=5;forest[i].scroll_w=1;forest[i].scroll_h=2;forest[i].cb=i?&forest[i-1]:NULL;}
    for(int i=DEPTH-1;i>=0;i--)scroll_area_publish(&forest[i]);
    check(near(forest[0].scroll_w,1+3*(DEPTH-1))&&near(forest[0].scroll_h,2+9*(DEPTH-1)),"deep forest contributions are exact with one constant-time operation per edge");
    check(!grand.scroll_w&&!grand.scroll_h,"helper never mutates remote shared ancestors");
    free(forest);printf("scroll-area20: %u checks, %u failures\n",checks,failures);return failures!=0;
}
