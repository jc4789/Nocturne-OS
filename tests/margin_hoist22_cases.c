static unsigned checks,failures;
static void check(bool ok,const char *name){checks++;if(!ok){failures++;printf("FAIL %s\n",name);}}
static bool near(float a,float b){return fabsf(a-b)<0.02f;}
static len_t px(float value){return (len_t){.kind=LK_LEN,.px=value};}
static style_t base_style(void){
    style_t s={0};s.display=D_BLOCK;s.opacity=1;s.max_height.kind=s.max_width.kind=LK_NONE;
    for(int i=0;i<4;i++)s.margin[i].kind=LK_LEN;
    return s;
}
static void reset_counts(void){top_calls=top_nodes=checkpoint_calls=0;}
struct pair {style_t ps,as,bs;box_t outside,p,a,b;};
static void pair(struct pair *p){
    memset(p,0,sizeof *p);p->ps=p->as=p->bs=base_style();
    p->outside=(box_t){.kind=B_BLOCK,.st=&p->ps,.is_bfc=true};
    p->p=(box_t){.kind=B_BLOCK,.st=&p->ps,.parent=&p->outside,.w=200,.first=&p->a,.last=&p->a};
    p->a=(box_t){.kind=B_BLOCK,.st=&p->as,.parent=&p->p};
    p->b=(box_t){.kind=B_BLOCK,.st=&p->bs,.parent=&p->p};
    p->as.height=p->bs.height=px(10);p->as.margin[0]=px(7);p->bs.margin[0]=px(4);
}
static void run_pair(struct pair *p){reset_counts();layout_inner(&p->p,NULL,0,0,-1);}
int main(void){
    web_doc doc={0};box_t unrelated={0};D=&doc;doc.root_box=&unrelated;VH=1080;
    enum{DEPTH=257};box_t *boxes=calloc(DEPTH,sizeof *boxes);style_t *styles=calloc(DEPTH,sizeof *styles);
    if(!boxes||!styles){printf("native allocation failure\n");free(boxes);free(styles);return 1;}
    for(int i=0;i<DEPTH;i++){
        styles[i]=base_style();styles[i].margin[0]=px(i==128?-2:3);
        boxes[i]=(box_t){.kind=B_BLOCK,.st=&styles[i],.w=200};
        if(i){boxes[i].parent=&boxes[i-1];boxes[i-1].first=boxes[i-1].last=&boxes[i];}
    }
    boxes[0].is_bfc=true;styles[DEPTH-1].height=px(10);
    reset_counts();layout_inner(&boxes[0],NULL,0,0,-1);
    check(top_calls==1,"one required top margin chain for deep native block forest");
    check(top_nodes==DEPTH-1,"same nested suffix is resolved once not quadratically");
    check(near(boxes[1].y,1)&&near(boxes[0].h,11),"positive negative collapsed group preserves outer geometry");
    bool positions=true;for(int i=2;i<DEPTH;i++)positions&=near(boxes[i].y,0)&&boxes[i].cb==&boxes[i-1];
    check(positions,"hoisted descendants keep native positions and containing blocks");
    check(checkpoint_calls==DEPTH,"every actual layout entry retains its native checkpoint");
    free(styles);free(boxes);
    struct pair p;
    pair(&p);run_pair(&p);
    check(top_calls==0&&near(p.a.y,0)&&near(p.p.h,10),"already hoisted first child never calculates discarded chain");
    pair(&p);p.a.next=&p.b;p.p.last=&p.b;run_pair(&p);
    check(top_calls==1&&top_nodes==1&&near(p.b.y,14)&&near(p.p.h,24),"nonfirst nonempty sibling still resolves required top margin");
    pair(&p);p.ps.border_width[0]=1;run_pair(&p);
    check(top_calls==1&&near(p.a.y,7)&&near(p.p.h,17),"top border prevents hoisting and keeps required chain");
    pair(&p);p.ps.padding[0]=px(2);run_pair(&p);
    check(top_calls==1&&near(p.a.y,7),"top padding prevents hoisting");
    pair(&p);p.ps.display=D_FLOW_ROOT;run_pair(&p);
    check(top_calls==1&&near(p.a.y,7),"flow root BFC preserves first-child margin calculation");
    pair(&p);p.ps.overflow=OV_AUTO;run_pair(&p);
    check(top_calls==1&&near(p.a.y,7),"scroll formatting context preserves first-child margin calculation");
    pair(&p);p.outside.kind=B_FLEX;run_pair(&p);
    check(top_calls==1&&near(p.a.y,7),"native flex item BFC preserves its margin boundary");
    pair(&p);p.a.next=&p.b;p.p.last=&p.b;p.bs.margin[0]=(len_t){.kind=LK_LEN,.pct=10};run_pair(&p);
    check(top_calls==1&&near(p.b.y,30),"required sibling percentage resolves actual current parent width");
    p.p.w=500;run_pair(&p);
    check(top_calls==1&&near(p.b.y,60),"width mutation is not hidden by a new result cache");
    pair(&p);p.a.next=&p.b;p.p.last=&p.b;p.as.height.kind=LK_AUTO;run_pair(&p);
    check(top_calls==0&&near(p.b.y,0),"empty first child retains existing through-collapse first state");
    pair(&p);p.a.next=&p.b;p.p.last=&p.b;p.bs.margin[0]=px(-6);run_pair(&p);
    check(top_calls==1&&near(p.b.y,4),"required negative sibling margin is preserved");
    pair(&p);p.a.next=&p.b;p.p.last=&p.b;p.bs.margin[0].kind=LK_AUTO;run_pair(&p);
    check(top_calls==1&&near(p.b.y,10),"auto top margin remains zero at required boundary");
    pair(&p);box_t skip={.kind=B_BLOCK,.st=&p.bs,.floated=true,.next=&p.a};p.p.first=&skip;
    reset_counts();float margin=top_chain(&p.p,200);
    check(near(margin,7)&&top_nodes==2,"required first-inflow chain still skips native floats");
    skip.floated=false;skip.abspos=true;reset_counts();margin=top_chain(&p.p,200);
    check(near(margin,7)&&top_nodes==2,"required first-inflow chain still skips abspos boxes");
    check(!unexpected,"no irrelevant font table flex grid float replaced stub supplied sizes");
    printf("margin-hoist22: %u checks, %u failures\n",checks,failures);return failures!=0;
}
