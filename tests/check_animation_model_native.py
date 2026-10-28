"""動きの時間の模型（animation.c）を実 C のままホストで確かめる：Web Animations 1 の play・pause・
finish・cancel・reverse・速さの変更の手順、効果の段と進み、CSS Transitions の開始・逆転・取消し、
CSS Animations の作成・再利用・取消し、事象の列、置き換えた動きの除去。カスケード・JS の層・
実サイトの代わりではない。期待値は仕様の手順から手で出したもの（WPT の web-animations/timing-model、
css-transitions、css-animations の試験と同じ場合を選んだ）。"""
from pathlib import Path
import os
import subprocess
root = Path(__file__).resolve().parent.parent
out = root / "build/browser-animation-model-host"
out.mkdir(parents=True, exist_ok=True)
css = (root / "user/libc/web/css.c").read_text(encoding="utf-8")
production = css[:css.index("/* ---------------------------------------------------------------- matching */")]
production += css[css.index("/* ---------------------------------------------------------------- feature queries"):css.index("struct sheet_parse_job {")]
production += css[css.index("/* ---------------------------------------------------------------- media queries"):css.index("static bool media_chain_ok(")]
production = production.replace("#include <nocturne.h>\n", "")
model = (root / "user/libc/web/animation.c").read_text(encoding="utf-8").replace("#include <nocturne.h>\n", "")
host = "#include <stdint.h>\nvoid *thread_local_get(unsigned slot);\nint display_bits_per_channel(void){return 8;}\nuint64_t uptime_ms(void);\n#define MIN(a,b) ((a)<(b)?(a):(b))\n#define MAX(a,b) ((a)>(b)?(a):(b))\n"
main = r'''
bool web_native_checkpoint(web_doc *d){(void)d;return true;}
void css_transform_list_matrix(const struct css_transform *t,float w,float h,double m[16]){(void)t;(void)w;(void)h;memset(m,0,16*sizeof *m);m[0]=m[5]=m[10]=m[15]=1;}
uint64_t uptime_ms(void){return 0;}
static int marks;
void css_mark_style_own(web_doc *d,node_t *n){(void)d;(void)n;marks++;}
bool css_substitute(const style_t *st,const char *v,sbuf *out){(void)st;sb_puts(out,v);return true;}
node_t *doc_node_root(node_t *node,bool composed){(void)composed;while(node&&node->parent)node=node->parent;return node;}
node_t *doc_flat_parent(node_t *node){return node?node->parent:NULL;}
bool web_js_enabled(web_doc *d){(void)d;return true;}
static int checks,failed;
static void check(bool ok,const char *name){checks++;if(!ok){failed++;printf("FAIL animation-model %s\n",name);}}
#define CHECK(x) check((x),#x)
#define NEAR(a,b) check(fabs((a)-(b))<1e-6,#a " == " #b)
static web_doc doc;
static node_t element;
static style_t parent,base;
static arena_t arena;
static uint8_t none_animated[1024];
static char emitted[8][2][64];static int nemitted;
static void emit(void *arg,const struct propdef *p,const char *value){(void)arg;if(nemitted<8){snprintf(emitted[nemitted][0],64,"%s",css_prop_name(css_prop_index(p),NULL));snprintf(emitted[nemitted][1],64,"%s",value);nemitted++;}}
static const char *sampled(const char *property){for(int i=0;i<nemitted;i++)if(!strcmp(emitted[i][0],property))return emitted[i][1];return "";}
static struct web_keyframe_value kv[4];static struct web_keyframe kf[4];
static const struct web_keyframe *frames_for(void *arg,const char *name,const struct css_easing *easing,uint32_t *count){
    (void)arg;if(strcmp(name,"fade"))return NULL;
    kv[0]=(struct web_keyframe_value){css_prop_lookup("opacity",7),"0"};kv[1]=(struct web_keyframe_value){css_prop_lookup("opacity",7),"1"};
    kf[0]=(struct web_keyframe){.offset=0,.easing=*easing,.composite=WA_COMPOSITE_AUTO,.count=1,.values=&kv[0]};
    kf[1]=(struct web_keyframe){.offset=1,.easing=*easing,.composite=WA_COMPOSITE_AUTO,.count=1,.values=&kv[1]};
    *count=2;return kf;
}
static struct web_motion_site site(const style_t *st){
    return (struct web_motion_site){.ctx={.base=st,.parent=&parent,.node=&element,.rem=16,.vw=800,.vh=600,.a=&arena,.d=&doc},
        .pseudo=PE_NONE,.keyframes=frames_for,.emit=emit};
}
static size_t sample(const style_t *st,bool transitions){
    nemitted=0;uint8_t animated[css_prop_count()];memset(animated,0,sizeof animated);struct web_motion_site s=site(st);
    return web_motion_sample(&s,transitions,animated);
}
static void at(uint64_t t){doc.visible=true;web_animations_tick(&doc,t);}
static style_t styled(const char *property,const char *value){
    struct css_anim_ctx c={.base=&base,.parent=&parent,.rem=16,.vw=800,.vh=600,.a=&arena};style_t s;
    const struct propdef *p=css_prop_lookup(property,strlen(property));
    if(!p||!css_anim_parse(&c,p,value,strlen(value),&s,NULL))printf("FAIL animation-model parse %s: %s\n",property,value);
    return s;
}
static struct web_animation *script_animation(double duration,const char *from,const char *to){
    struct web_animation *a=web_animation_new(&doc,WA_SCRIPT);struct web_effect *e=web_effect_new(&doc);
    struct web_keyframe_value v[2]={{css_prop_lookup("opacity",7),(char*)from},{css_prop_lookup("opacity",7),(char*)to}};
    struct web_keyframe f[2]={{.offset=NAN,.easing={.kind=CM_LINEAR},.composite=WA_COMPOSITE_AUTO,.count=1,.values=&v[0]},
                              {.offset=NAN,.easing={.kind=CM_LINEAR},.composite=WA_COMPOSITE_AUTO,.count=1,.values=&v[1]}};
    web_effect_set_keyframes(e,web_keyframes_build(f,2));
    e->timing.duration=duration;
    web_effect_set_target(e,&element,PE_NONE);
    web_animation_set_effect(a,e);
    a->wrapped=1;
    web_animations_join(&doc);
    return a;
}
static size_t events(const char **types,size_t max){
    web_animations_collect(&doc);struct web_animation_event *v;size_t n=web_animations_take_events(&doc,&v);
    for(size_t i=0;i<n&&i<max;i++)types[i]=v[i].type;
    for(size_t i=0;i<n;i++)free(v[i].name);free(v);return n;
}
int main(void){
    jmp_buf trap;arena.trap=&trap;if(setjmp(trap)){printf("FAIL animation-model arena\n");return 1;}
    css_style_init(&parent,NULL);css_style_init(&base,&parent);css_style_finish(&base,&parent,false);
    element.type=N_ELEM;element.owner=&doc;doc.js=(void *)1;
    web_animations_cascading(&doc,true);web_animations_cascading(&doc,false);
    web_animations_set_origin(&doc,1000);
    at(1100); /* the timeline at 100 */
    NEAR(doc.animations->now,100);

    /* play: pending, then the ready time is the start */
    struct web_animation *a=script_animation(1000,"0","1");
    CHECK(web_animation_play_state(a)==WA_IDLE);
    CHECK(web_animation_play(a,true)==0);
    CHECK(a->pending==WA_PENDING_PLAY && web_animation_play_state(a)==WA_RUNNING);
    NEAR(web_animation_current(a),0);
    CHECK(a->ready_state==WA_PROMISE_PENDING);
    at(1200);
    CHECK(!a->pending && a->ready_state==WA_PROMISE_RESOLVED);
    NEAR(a->start,200);
    at(1700);
    NEAR(web_animation_current(a),500);
    CHECK(sample(&base,false)==1 && !strcmp(sampled("opacity"),"0.5"));
    struct web_effect_progress p;web_effect_compute(a->effect,&p);
    CHECK(p.phase==WA_PHASE_ACTIVE);NEAR(p.progress,.5);NEAR(p.iteration,0);

    /* pause: the hold time is the current time at the pending task */
    CHECK(web_animation_pause(a)==0);
    CHECK(a->pending==WA_PENDING_PAUSE && web_animation_play_state(a)==WA_PAUSED);
    at(1800);
    CHECK(!a->pending);NEAR(a->hold,600);CHECK(isnan(a->start));
    at(1900);
    NEAR(web_animation_current(a),600);

    /* play again from the hold, then finish: the finished promise resolves at once */
    web_animation_play(a,true);at(2000);NEAR(a->start,400);
    CHECK(web_animation_finish(a)==0);
    CHECK(web_animation_play_state(a)==WA_FINISHED && a->finished_state==WA_PROMISE_RESOLVED);
    NEAR(web_animation_current(a),1000);
    const char *types[8];
    CHECK(events(types,8)==1 && !strcmp(types[0],"finish"));
    /* fill none: the finished effect sets nothing */
    CHECK(sample(&base,false)==0);

    /* reverse from the end, then a natural finish at 0 queues the finish notification */
    CHECK(web_animation_reverse(a)==0);
    CHECK(a->finished_gen==1 && a->finished_state==WA_PROMISE_PENDING);
    at(2100);NEAR(a->rate,-1);NEAR(web_animation_current(a),1000);
    at(2600);NEAR(web_animation_current(a),500);
    at(3200);CHECK(web_animation_play_state(a)==WA_FINISHED);NEAR(web_animation_current(a),0);
    CHECK(a->finish_task);
    web_animation_finish_notify(a);CHECK(a->finished_state==WA_PROMISE_RESOLVED);
    CHECK(events(types,8)==1 && !strcmp(types[0],"finish"));

    /* cancel: idle, the finished promise rejected and replaced, a cancel event */
    web_animation_play(a,true);at(3300);
    web_animation_cancel(a);
    CHECK(web_animation_play_state(a)==WA_IDLE && a->finished_prev==WA_PROMISE_REJECTED && a->finished_state==WA_PROMISE_PENDING);
    CHECK(events(types,8)==1 && !strcmp(types[0],"cancel"));
    CHECK(sample(&base,false)==0);

    /* the playback rate, seamlessly: the current time holds */
    web_animation_set_rate(a,1);web_animation_play(a,true);at(3400);at(3600);
    NEAR(web_animation_current(a),200);
    web_animation_update_rate(a,2);
    CHECK(a->pending==WA_PENDING_PLAY);NEAR(web_animation_current(a),200);
    /* the old rate holds until the pending task: 300 at the ready time, then twice as fast */
    at(3700);NEAR(a->rate,2);NEAR(web_animation_current(a),300);
    at(3800);NEAR(web_animation_current(a),500);
    /* set the current time while running: the start time moves */
    CHECK(web_animation_set_current(a,100)==0);NEAR(a->start,2750);
    CHECK(web_animation_set_current(a,NAN)<0); /* TypeError: an unresolved seek of a resolved time */
    web_animation_cancel(a);events(types,8);

    /* timing: delay, iterations, alternate, the fill */
    a=script_animation(100,"0","1");
    a->effect->timing.delay=50;a->effect->timing.iterations=3;a->effect->timing.direction=CM_ALTERNATE;a->effect->timing.fill=CM_FILL_BOTH;
    web_animation_play(a,true);at(4000);
    web_effect_compute(a->effect,&p);CHECK(p.phase==WA_PHASE_BEFORE);NEAR(p.progress,0);
    at(4175);web_effect_compute(a->effect,&p);CHECK(p.phase==WA_PHASE_ACTIVE);NEAR(p.iteration,1);NEAR(p.progress,.75);
    at(4400);web_effect_compute(a->effect,&p);CHECK(p.phase==WA_PHASE_AFTER);NEAR(p.iteration,2);NEAR(p.progress,1);
    CHECK(web_animation_play_state(a)==WA_FINISHED);
    CHECK(web_animation_relevant(a)); /* in effect: fill both */
    /* replaced: a later finished filling animation of the same property removes it */
    struct web_animation *b=script_animation(10,"0.2","0.4");b->effect->timing.fill=CM_FILL_FORWARDS;
    web_animation_play(b,true);at(4410);at(4500);
    CHECK(web_animation_play_state(b)==WA_FINISHED);
    CHECK(a->replace==WA_REMOVED && b->replace==WA_ACTIVE);
    events(types,8);
    CHECK(sample(&base,false)==1 && !strcmp(sampled("opacity"),"0.4"));
    web_animation_cancel(b);web_animation_cancel(a);events(types,8);

    /* composite add over the underlying value */
    a=script_animation(1000,"0.25","0.25");a->effect->composite=CM_COMPOSITE_ADD;a->effect->timing.fill=CM_FILL_FORWARDS;
    web_animation_play(a,true);at(5000);at(5500);
    style_t half=styled("opacity","0.5");
    CHECK(sample(&half,false)==1 && !strcmp(sampled("opacity"),"0.75"));
    web_animation_cancel(a);events(types,8);

    /* CSS Transitions: start, sample, reverse with a shortened duration, cancel */
    style_t before=styled("transition","opacity 1s linear"),after=before;before.opacity=0;after.opacity=1;
    struct web_motion_site s=site(&after);
    web_motion_transitions(&s,&before,&after,none_animated);
    struct web_animation *t=element.owned;
    CHECK(t && t->kind==WA_CSS_TRANSITION && !strcmp(t->name,"opacity") && !strcmp(t->to,"1"));
    web_animations_join(&doc);
    at(6000);NEAR(t->start,1000*5);
    CHECK(events(types,8)==2 && !strcmp(types[0],"transitionrun") && !strcmp(types[1],"transitionstart"));
    at(6250);CHECK(sample(&after,true)==1 && !strcmp(sampled("opacity"),"0.25"));
    /* back to 0 a quarter of the way: the reversing shortening factor is 0.25 */
    style_t back=before;back.motion=after.motion;
    s=site(&back);web_motion_transitions(&s,&after,&back,none_animated);
    struct web_animation *r=element.owned;
    CHECK(r!=t && r->kind==WA_CSS_TRANSITION && t->disowned && !strcmp(r->to,"0"));
    NEAR(r->shortening,.25);NEAR(r->effect->timing.duration,250);
    web_animations_join(&doc);at(6300);
    /* the old transition: cancel (its Animation) and transitioncancel; the new one runs and starts */
    CHECK(events(types,8)==4 && !strcmp(types[0],"cancel") && !strcmp(types[1],"transitioncancel") &&
          !strcmp(types[2],"transitionrun") && !strcmp(types[3],"transitionstart"));
    at(6425);CHECK(sample(&back,true)==1 && !strcmp(sampled("opacity"),"0.125"));
    at(6600);CHECK(web_animation_play_state(r)==WA_FINISHED);
    CHECK(events(types,8)==1 && !strcmp(types[0],"transitionend"));
    CHECK(sample(&back,true)==0);
    /* no transition-property matches: the completed transition is removed */
    style_t none=styled("transition","none");none.opacity=1;
    s=site(&none);web_motion_transitions(&s,&back,&none,none_animated);
    CHECK(r->disowned);
    at(6700);
    for(struct web_animation *o=element.owned;o;o=o->owner_next)CHECK(o->kind!=WA_CSS_TRANSITION || o->disowned);
    /* a discrete property transitions only with allow-discrete */
    style_t fb=styled("transition","float 1s"),fa=fb;fb.float_=FL_LEFT;fa.float_=FL_RIGHT;
    s=site(&fa);web_motion_transitions(&s,&fb,&fa,none_animated);
    bool started=false;for(struct web_animation *o=element.owned;o;o=o->owner_next)if(!o->disowned&&o->property==css_prop_lookup("float",5))started=true;
    CHECK(!started);
    fb=styled("transition","float 1s allow-discrete");fa=fb;fb.float_=FL_LEFT;fa.float_=FL_RIGHT;
    s=site(&fa);web_motion_transitions(&s,&fb,&fa,none_animated);
    started=false;for(struct web_animation *o=element.owned;o;o=o->owner_next)if(!o->disowned&&o->property==css_prop_lookup("float",5))started=true;
    CHECK(started);
    web_motion_cancel(&element,-1);at(6800);events(types,8);at(6900);

    /* CSS Animations: created from animation-name, reused while the name stays, canceled when it goes */
    style_t anim=styled("animation","fade 1s linear forwards");
    s=site(&anim);web_motion_animations(&s);
    struct web_animation *c=NULL;
    for(struct web_animation *o=element.owned;o;o=o->owner_next)if(o->kind==WA_CSS_ANIMATION&&!o->disowned)c=o;
    CHECK(c && !strcmp(c->name,"fade") && c->pending==WA_PENDING_PLAY);
    web_animations_join(&doc);at(7000);NEAR(c->start,6000);
    CHECK(events(types,8)==1 && !strcmp(types[0],"animationstart"));
    at(7500);CHECK(sample(&anim,false)==1 && !strcmp(sampled("opacity"),"0.5"));
    s=site(&anim);web_motion_animations(&s);
    struct web_animation *same=NULL;
    for(struct web_animation *o=element.owned;o;o=o->owner_next)if(o->kind==WA_CSS_ANIMATION&&!o->disowned)same=o;
    CHECK(same==c);
    style_t paused=styled("animation","fade 1s linear forwards paused");
    s=site(&paused);web_motion_animations(&s);CHECK(c->pending==WA_PENDING_PAUSE);
    at(7600);NEAR(c->hold,600);
    s=site(&anim);web_motion_animations(&s);at(7700);NEAR(web_animation_current(c),600);
    at(8200);CHECK(web_animation_play_state(c)==WA_FINISHED);
    CHECK(events(types,8)==1 && !strcmp(types[0],"animationend"));
    CHECK(sample(&anim,false)==1 && !strcmp(sampled("opacity"),"1")); /* fill forwards */
    style_t gone=styled("animation","none");
    s=site(&gone);web_motion_animations(&s);CHECK(c->disowned);
    at(8300);CHECK(!c->owner);
    CHECK(sample(&gone,false)==0);

    web_animations_doc_free(&doc);
    printf("animation-model: %d checks, %d failures\n",checks,failed);
    return failed!=0;
}
'''
source = out / "model.c"
source.write_text(host + production + model + main, encoding="utf-8")
http = (root / "user/libc/http.c").read_text(encoding="utf-8")
url = out / "url.c"
url.write_text(http[:http.index("/* The pool is process-local.")] + http[http.index("bool url_parse(const char *s, struct url *u)"):http.index("enum http_url_result http_origin_owned_n(")], encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
if not cc.exists():
    cc = root.parents[2] / "tools/msys64/ucrt64/bin/clang.exe"  # a git worktree shares the main tree's tools
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
(abi / "time.h").write_text('#include "' + (cc.parent.parent / "include/time.h").as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "model.exe"
command = [str(cc), "-std=gnu11", "-O1", "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include",
           "-Icommon", "-Iuser/libc", "-Iuser/libc/web", "-Ithird_party/quickjs", str(source), "user/libc/web/cssprop.c",
           "user/libc/web/util.c", "user/libc/web/html.c", str(url), "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=60)
