static struct layout_text_cache *start_cache(void){
    struct layout_text_cache *c=calloc(1,sizeof *c);layout_text_active=c;layout_text_begin(c);return c;
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
    check(!layout_text_active&&d.layout_text_cache&&live_allocations&&!d.lmem.trap,"normal layout retains document table and restores trap");
    before=measured;layout_doc(&d,100,100);
    check(measured==before,"subsequent layout reuses owned document text");
    before=measured;scope_mode=1;layout_doc(&d,100,100);
    check(measured==before+1&&nested_doc.layout_text_cache&&!layout_text_active,"nested document independent cache and context restore");
    before=measured;scope_mode=3;layout_doc(&d,100,100);
    check(measured==before+1&&!d.layout_text_cache->busy&&!layout_text_active,"same-document reentrant layout gets temporary independent cache");
    layout_cache_free(&d);layout_cache_free(&nested_doc);
    scope_mode=0;struct layout_text_cache *outer=start_cache();float a=text_width(&st,&test_font,"scope",5);
    fail_at=allocation_attempts+1;before=measured;layout_doc(&d,100,100);fail_at=0;
    check(measured==before+2&&layout_text_active==outer,"context OOM does not borrow outer cache");
    before=measured;check(text_width(&st,&test_font,"scope",5)==a&&measured==before,"outer context restored after OOM");
    stop_cache(outer);check(!live_allocations,"OOM scope all allocations released");
    jmp_buf trap;d.lmem.trap=&trap;scope_mode=2;int failed=setjmp(trap);
    if(!failed){layout_doc(&d,100,100);check(false,"arena failure did not propagate");}
    else check(failed==3,"arena failure code preserved");
    check(d.lmem.trap==&trap&&!layout_text_active&&d.layout_text_cache&&!d.layout_text_cache->busy,"arena longjmp preserves document cache and restores original trap");
    layout_cache_free(&d);check(!live_allocations,"document cache teardown after longjmp");
    d.root_box=NULL;scope_mode=0;before=(unsigned)allocation_attempts;layout_doc(&d,100,100);
    check(allocation_attempts==before&&d.lmem.trap==&trap,"empty root has no cache lifetime");
}
static void test_choice_values(void){
    struct layout_text_cache *c=start_cache();style_t s={0};wfont f={NULL,1,false};
    struct web_font_resource resource={.face=(font_t *)(uintptr_t)3};
    struct web_font_range ranges[]={{'a','z'}},copied[]={{'a','z'}};
    struct web_font_choice choice={.resource=&resource,.ranges=ranges,.range_count=1};
    struct web_font_choice other={.resource=&resource,.ranges=copied,.range_count=1};
    f.choices=&choice;unsigned before=measured;float a=text_width(&s,&f,"a",1);
    f.choices=&other;check(text_width(&s,&f,"a",1)==a&&measured==before+1,"restyled font choices compare owned values not CSS arena addresses");
    copied[0].last='m';text_width(&s,&f,"a",1);
    check(measured==before+2,"unicode ranges participate in value key");
    f.choices=&choice;check(text_width(&s,&f,"a",1)==a&&measured==before+2,"old unicode range key owns bytes after CSS mutation");
    resource.face=(font_t *)(uintptr_t)7;
    check(text_width(&s,&f,"a",1)==a+4&&measured==before+3,"choice backend face change invalidates measurement");
    resource.failed=true;
    check(text_width(&s,&f,"a",1)==a+104&&measured==before+4,"failed resource invalidates without global epoch change");
    resource.failed=false;resource.face=NULL;text_width(&s,&f,"a",1);resource.wanted=false;
    before=measured;text_width(&s,&f,"a",1);
    check(resource.wanted&&measured==before,"cached subset text preserves resource demand after partial cascade");
    resource.wanted=false;snapshot_probe=true;text_width(&s,&f,"a",1);snapshot_probe=false;
    check(!resource.wanted,"read-only layout probe never demands font on cache hit");
    resource.wanted=false;text_width(&s,&f,"!",1);resource.wanted=false;before=measured;text_width(&s,&f,"!",1);
    check(!resource.wanted&&measured==before,"unused unicode subset stays undemanded");
    stop_cache(c);check(!live_allocations,"owned font-choice keys released");
}
static void test_retirement(void){
    struct layout_text_cache *c=start_cache();style_t s={0};wfont f={NULL,1,false};char text[32];
    for(unsigned i=0;i<512;i++){snprintf(text,sizeof text,"retire-%u",i);text_width(&s,&f,text,strlen(text));}
    text_width(&s,&f,"keep",4);size_t capacity=c->capacity;unsigned before=measured;
    for(unsigned i=0;i<10;i++)layout_text_begin(c);
    check(c->count==513&&c->capacity==capacity,"cached unchanged layouts do not age unvisited measurements");
    text_width(&s,&f,"keep",4);layout_text_begin(c);text_width(&s,&f,"keep",4);
    layout_text_begin(c);text_width(&s,&f,"keep",4);
    check(c->count==1&&c->capacity==16&&measured==before,"text replacement retires unused owned entries and shrinks buckets");
    font_metrics_epoch++;layout_text_begin(c);
    check(!c->count,"backend generation change frees stale owned keys");
    stop_cache(c);check(!live_allocations,"retired cache has no leaked allocations");
}
static void test_intern_choices(void){
    struct layout_text_cache *c=start_cache();style_t s={0};wfont f={NULL,1,false};
    struct web_font_resource resource={.face=(font_t *)(uintptr_t)3};
    struct web_font_range *ranges=malloc(1000*sizeof *ranges),*copy=malloc(1000*sizeof *copy);
    for(unsigned i=0;i<1000;i++)ranges[i]=(struct web_font_range){i*2,i*2+1};
    memcpy(copy,ranges,1000*sizeof *ranges);
    struct web_font_choice choice={.resource=&resource,.ranges=ranges,.range_count=1000};
    struct web_font_choice same={.resource=&resource,.ranges=copy,.range_count=1000};
    f.choices=&choice;char text[32];size_t bytes_before=allocation_bytes;
    for(unsigned i=0;i<1000;i++){snprintf(text,sizeof text,"intern-%u",i);text_width(&s,&f,text,strlen(text));}
    struct layout_text_choices *key=c->oldest->choices;
    check(c->choices_count==1&&key->refs==1000,"thousand measurements own one shared unicode-range key");
    check(allocation_bytes-bytes_before < key->length+1000*(sizeof(struct layout_text_entry)+32)+32768,"unicode-range tail allocation grows once not once per word");
    f.choices=&same;unsigned before=measured;text_width(&s,&f,"intern-0",8);
    check(measured==before&&c->newest->choices==key,"distinct restyle arena reuses interned value key");
    copy[0].last=1234;text_width(&s,&f,"modified",8);
    check(c->choices_count==2&&c->newest->choices!=key,"mutated range values allocate different owned key");
    f.choices=&choice;text_width(&s,&f,"survivor",8);
    layout_text_begin(c);text_width(&s,&f,"survivor",8);layout_text_begin(c);text_width(&s,&f,"survivor",8);
    layout_text_begin(c);text_width(&s,&f,"survivor",8);
    check(c->count==1&&c->choices_count==1&&key->refs==1,"LRU release preserves last shared key while dropping retired measurements");
    f.choices=NULL;layout_text_begin(c);text_width(&s,&f,"plain",5);layout_text_begin(c);text_width(&s,&f,"plain",5);
    layout_text_begin(c);text_width(&s,&f,"plain",5);
    check(!c->choices_count&&!c->choices_capacity&&!c->choices_buckets,"final reference retirement releases choices and their bucket table");
    free(ranges);free(copy);stop_cache(c);check(!live_allocations,"shared key and owned tails freed with document");
}
static void test_intern_collisions_and_oom(void){
    struct layout_text_cache *c=start_cache();style_t s={0};wfont f={NULL,1,false};
    struct web_font_resource resources[20]={0};struct web_font_range range={0};
    range.first='a';range.last='z';
    struct web_font_choice a={.resource=&resources[0],.ranges=&range,.range_count=1};
    struct web_font_choice b={.resource=&resources[1],.ranges=&range,.range_count=1};
    f.choices=&a;size_t length=layout_text_choices_size(&f);
    struct layout_text_choices *ka=layout_text_choices_get(c,&f,length,77);
    f.choices=&b;struct layout_text_choices *kb=layout_text_choices_get(c,&f,length,77);
    f.choices=&a;struct layout_text_choices *same=layout_text_choices_get(c,&f,length,77);
    check(ka&&kb&&ka!=kb&&same==ka&&ka->refs==2,"forced fontkey hash collision compares complete values");
    layout_text_choices_release(c,same);layout_text_choices_release(c,ka);layout_text_choices_release(c,kb);
    check(!c->choices_count&&!c->choices_capacity,"collision keys release at exact last reference");
    f.choices=NULL;text_width(&s,&f,"anchor",6);f.choices=&a;
    unsigned before=measured;fail_at=allocation_attempts+1;text_width(&s,&f,"bucket-oom",10);fail_at=0;
    check(measured==before+1&&!c->choices_count&&c->count==1,"choice bucket OOM leaves real measurement and existing cache");
    layout_text_choices_rehash(c,16);before=measured;fail_at=allocation_attempts+1;text_width(&s,&f,"key-oom",7);fail_at=0;
    check(measured==before+1&&!c->choices_count&&c->count==1,"choice tail OOM publishes no half-built entry");
    text_width(&s,&f,"shared",6);ka=c->newest->choices;size_t refs=ka->refs,entries=c->count;
    before=measured;fail_at=allocation_attempts+1;text_width(&s,&f,"entry-oom",9);fail_at=0;
    check(measured==before+1&&ka->refs==refs&&c->count==entries,"entry OOM rolls back borrowed shared key reference");
    f.choices=&b;before=measured;fail_at=allocation_attempts+2;text_width(&s,&f,"new-entry-oom",13);fail_at=0;
    check(measured==before+1&&c->choices_count==1&&c->count==entries,"new key is freed if subsequent entry allocation fails");
    f.choices=NULL;char text[32];for(unsigned i=0;i<40;i++){snprintf(text,sizeof text,"reserve-%u",i);text_width(&s,&f,text,strlen(text));}
    for(unsigned i=1;i<12;i++){a.resource=&resources[i];f.choices=&a;snprintf(text,sizeof text,"face-%u",i);text_width(&s,&f,text,strlen(text));}
    check(c->choices_count==12&&c->choices_capacity==16,"distinct owned choice keys grow independently from text table");
    a.resource=&resources[12];before=measured;entries=c->count;fail_at=allocation_attempts+1;text_width(&s,&f,"grow-oom",8);fail_at=0;
    check(measured==before+1&&c->choices_count==12&&c->choices_capacity==16&&c->count==entries,"choice rehash OOM preserves all prior shared keys");
    text_width(&s,&f,"grow-oom",8);check(c->choices_count==13&&c->choices_capacity==32,"choice table grows after allocation recovery");
    stop_cache(c);check(!live_allocations,"every shared ref and OOM table reclaimed");
}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--round2")){test_intern_choices();test_intern_collisions_and_oom();}
    else {test_keys();test_generations();test_grow_and_oom();test_scope();test_choice_values();test_retirement();}
    printf("layout-text26: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
