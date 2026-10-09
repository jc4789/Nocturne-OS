static struct layout_text_cache *start_cache(void){
    struct layout_text_cache *c=calloc(1,sizeof *c);layout_text_active=c;return c;
}
static void stop_cache(struct layout_text_cache *c){layout_text_active=NULL;layout_text_cache_free(c);}
static void test_keys(void){
    struct layout_text_cache *c=start_cache();style_t s={0};wfont f={NULL,1,false};
    unsigned before=measured;char reused[4]="abc",copy[4]="abc";
    float a=text_width(&s,&f,reused,3);
    check(text_width(&s,&f,copy,3)==a&&measured==before+1,"equal contents distinct address reuse");
    reused[0]='z';float z=text_width(&s,&f,reused,3);
    check(z!=a&&measured==before+2,"reused address different contents miss");
    check(text_width(&s,&f,copy,3)==a&&measured==before+2,"entry owns old text");
    check(text_width(&s,&f,copy,2)!=a&&measured==before+3,"length key");
    f.px=2;float px=text_width(&s,&f,copy,3);
    check(px==a*2&&measured==before+4,"font size key");
    f.bold=true;check(text_width(&s,&f,copy,3)==px+1&&measured==before+5,"bold key");
    f.ttf=(font_t *)(uintptr_t)7;check(text_width(&s,&f,copy,3)==px+8&&measured==before+6,"font face key");
    s.letter_spacing=2;float letter=text_width(&s,&f,"a b",3);
    s.word_spacing=3;check(text_width(&s,&f,"a b",3)==letter+3,"word spacing key");
    s.letter_spacing=4;check(text_width(&s,&f,"a b",3)==letter+9,"letter spacing key");
    char embedded[3]={'a',0,'b'},different[3]={'a',0,'c'};
    check(text_width(&s,&f,embedded,3)!=text_width(&s,&f,different,3),"embedded NUL content");
    char utf8[3]={(char)0xc2,(char)0xa0,' '};s.letter_spacing=2;s.word_spacing=3;
    float actual=text_width(&s,&f,utf8,3);
    float raw=wf_width(&f,utf8,3);
    check(actual==raw+7,"actual UTF8 leading byte spacing preserved");
    struct layout_text_entry *collision=malloc(sizeof *collision+2);
    memset(collision,0,sizeof *collision);collision->hash=99;collision->length=2;
    collision->font_generation=font_metrics_generation();collision->font=f.ttf;collision->px=f.px;collision->bold=f.bold;
    collision->letter_spacing=s.letter_spacing;collision->word_spacing=s.word_spacing;memcpy(collision->text,"ab",2);
    check(layout_text_same(collision,99,&f,&s,"ab",2,font_metrics_generation()),"collision comparator identical content");
    check(!layout_text_same(collision,99,&f,&s,"cd",2,font_metrics_generation()),"hash collision rejected by bytes");
    free(collision);stop_cache(c);check(!live_allocations,"all key entries freed");
}
static void test_generations(void){
    struct layout_text_cache *c=start_cache();style_t s={0};wfont f={NULL,1,false};
    unsigned before=measured;text_width(&s,&f,"g",1);font_metrics_epoch++;
    text_width(&s,&f,"g",1);check(measured==before+2,"backend generation invalidation");
    before=measured;change_backend=true;text_width(&s,&f,"lazy",4);
    text_width(&s,&f,"lazy",4);text_width(&s,&f,"lazy",4);
    check(measured==before+2,"lazy load measurement not published across generations");
    font_metrics_epoch=UINT64_MAX;before=measured;text_width(&s,&f,"wrap",4);
    if(font_metrics_epoch)font_metrics_epoch++;
    check(!font_metrics_generation(),"generation wrap becomes disabled identity");
    text_width(&s,&f,"wrap",4);text_width(&s,&f,"wrap",4);
    check(measured==before+3,"zero generation cannot reuse old entry");
    stop_cache(c);font_metrics_epoch=42;check(!live_allocations,"generation entries freed");
}
static void test_grow_and_oom(void){
    struct layout_text_cache *c=start_cache();style_t s={0};wfont f={NULL,1,false};char text[40];
    for(unsigned i=0;i<2049;i++){snprintf(text,sizeof text,"distinct-%u",i);text_width(&s,&f,text,strlen(text));}
    check(c->count==2049&&c->capacity>=2049,"dynamic entries exceed old timer-sized quota");
    unsigned before=measured;snprintf(text,sizeof text,"distinct-%u",2048u);text_width(&s,&f,text,strlen(text));
    check(measured==before,"rehash preserves real entries");stop_cache(c);check(!live_allocations,"grown table released");
    c=start_cache();fail_at=allocation_attempts+1;before=measured;float a=text_width(&s,&f,"oom",3);
    check(a==331&&measured==before+1&&c->count==0,"bucket OOM returns real measurement");
    fail_at=0;text_width(&s,&f,"oom",3);fail_at=allocation_attempts+1;
    before=measured;text_width(&s,&f,"entry",5);
    check(measured==before+1&&c->count==1,"entry OOM retains prior table");
    fail_at=0;before=measured;text_width(&s,&f,"oom",3);
    check(measured==before,"existing entry survives allocation failure");
    c->count=SIZE_MAX;before=measured;text_width(&s,&f,"overflow",8);
    check(measured==before+1&&c->count==SIZE_MAX,"count overflow only bypasses insertion");
    stop_cache(c);check(!live_allocations,"failed insertion teardown");
}
static void test_scope(void){
    style_t st={0};box_t root={.st=&st};web_doc d={.root_box=&root};
    nested_box.st=&st;nested_doc.root_box=&nested_box;
    scope_mode=0;unsigned before=measured;layout_doc(&d,100,100);
    check(measured==before+1,"real layout scope shares repeated measurement");
    check(!layout_text_active&&!live_allocations&&!d.lmem.trap,"normal layout releases table and trap");
    before=measured;layout_doc(&d,100,100);
    check(measured==before+1,"subsequent layout starts fresh");
    before=measured;scope_mode=1;layout_doc(&d,100,100);
    check(measured==before+2&&!live_allocations&&!layout_text_active,"nested layout independent cache and teardown");
    scope_mode=0;struct layout_text_cache *outer=start_cache();float a=text_width(&st,&test_font,"scope",5);
    fail_at=allocation_attempts+1;before=measured;layout_doc(&d,100,100);fail_at=0;
    check(measured==before+2&&layout_text_active==outer,"context OOM does not borrow outer cache");
    before=measured;check(text_width(&st,&test_font,"scope",5)==a&&measured==before,"outer context restored after OOM");
    stop_cache(outer);check(!live_allocations,"OOM scope all allocations released");
    jmp_buf trap;d.lmem.trap=&trap;scope_mode=2;int failed=setjmp(trap);
    if(!failed){layout_doc(&d,100,100);check(false,"arena failure did not propagate");}
    else check(failed==3,"arena failure code preserved");
    check(d.lmem.trap==&trap&&!layout_text_active&&!live_allocations,"arena longjmp releases cache and restores original trap");
    d.root_box=NULL;scope_mode=0;before=(unsigned)allocation_attempts;layout_doc(&d,100,100);
    check(allocation_attempts==before&&d.lmem.trap==&trap,"empty root has no cache lifetime");
}
int main(void){
    test_keys();test_generations();test_grow_and_oom();test_scope();
    printf("layout-text26: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
