"""動きの値の層（cssprop.c の css_anim_*、緩急の css_easing_*）を実 C のままホストで確かめる。
カスケード・時間の模型・JS・実サイトの代わりではない。期待値は WPT の *-interpolation.html と
css-easing の試験の値から取った一部。色は color.c の型を通し、css-color/animation の
color-interpolation・color-composition、css-backgrounds の background-color-interpolation、
web-animations の property-types.js（colorType・影）と responsive/to-color-change の値を、
カスケードが標本を当てた後の見え方（shown）で比べる。"""
from pathlib import Path
import os
import subprocess
root = Path(__file__).resolve().parent.parent
out = root / "build/browser-animation-values-host"
out.mkdir(parents=True, exist_ok=True)
css = (root / "user/libc/web/css.c").read_text(encoding="utf-8")
production = css[:css.index("/* ---------------------------------------------------------------- matching */")]
production += css[css.index("/* ---------------------------------------------------------------- feature queries"):css.index("struct sheet_parse_job {")]
production += css[css.index("/* ---------------------------------------------------------------- media queries"):css.index("static bool media_chain_ok(")]
production = production.replace("#include <nocturne.h>\n", "")
css_host = "void *thread_local_get(unsigned slot);\nint display_bits_per_channel(void){return 8;}\n#define MIN(a,b) ((a)<(b)?(a):(b))\n#define MAX(a,b) ((a)>(b)?(a):(b))\n"
main = r'''
bool web_native_checkpoint(web_doc *d){(void)d;return true;}
void css_transform_list_matrix(const struct css_transform *t,float w,float h,double m[16]){(void)t;(void)w;(void)h;memset(m,0,16*sizeof *m);m[0]=m[5]=m[10]=m[15]=1;}
/* layout.c's, reached only from Geometry's matrix strings */
void css_transform_list_matrix_3d(const struct css_transform *t,double m[16]){(void)t;(void)m;abort();}
/* web_url.c's (Lexbor's URL parser): CSS url() resolution, not reached by these values */
int web_resolve_url_owned(const char *base,const char *rel,char **out){(void)base;(void)rel;(void)out;abort();}
static int checks,failed;
static void check(bool ok,const char *name,const char *got){checks++;if(!ok){failed++;printf("FAIL animation-values %s: %s\n",name,got?got:"(null)");}}
static style_t parent,base;
static arena_t arena;
static struct css_anim_ctx ctx;
static sbuf text;
static const char *mix(const char *property,const char *a,const char *b,double t){
    const struct propdef *p=css_prop_lookup(property,strlen(property));style_t x,y;
    if(!p||!css_anim_parse(&ctx,p,a,strlen(a),false,&x,NULL)||!css_anim_parse(&ctx,p,b,strlen(b),false,&y,NULL))return "PARSE";
    text.n=0;if(text.p)text.p[0]=0;
    if(!css_anim_interpolate(&ctx,p,&x,&y,t,&text))return css_anim_discrete_second(p,&x,&y,t)?"DISCRETE-B":"DISCRETE-A";
    return sb_cstr(&text);
}
static const char *compose(const char *property,const char *a,const char *b,bool accumulate){
    const struct propdef *p=css_prop_lookup(property,strlen(property));style_t x,y;
    if(!p||!css_anim_parse(&ctx,p,a,strlen(a),false,&x,NULL)||!css_anim_parse(&ctx,p,b,strlen(b),false,&y,NULL))return "PARSE";
    text.n=0;if(text.p)text.p[0]=0;
    if(!css_anim_compose(&ctx,p,&x,&y,accumulate,&text))return "REPLACE";
    return sb_cstr(&text);
}
static const char *computed(const char *property,const char *v){
    const struct propdef *p=css_prop_lookup(property,strlen(property));style_t x;
    if(!p||!css_anim_parse(&ctx,p,v,strlen(v),false,&x,NULL))return "PARSE";
    text.n=0;if(text.p)text.p[0]=0;
    return css_computed_value(&x,p,&text,NULL)?sb_cstr(&text):"NONE";
}
static bool same(const char *property,const char *a,const char *b){
    const struct propdef *p=css_prop_lookup(property,strlen(property));style_t x,y;
    return p&&css_anim_parse(&ctx,p,a,strlen(a),false,&x,NULL)&&css_anim_parse(&ctx,p,b,strlen(b),false,&y,NULL)&&css_anim_equal(p,&x,&y);
}
/* a registered custom property's computed values a and b (css-properties-values-api-1 §2.5) */
static const char *custom(const char *syntax,const char *a,const char *b,double t){
    const struct css_syntax *syn=css_syntax_parse(syntax,strlen(syntax),&arena);
    if(!syn)return "SYNTAX";
    text.n=0;if(text.p)text.p[0]=0;
    return css_custom_interpolate(&ctx,syn,a,b,t,&text)?sb_cstr(&text):"DISCRETE";
}
static const char *custom_compose(const char *syntax,const char *a,const char *b,bool accumulate){
    const struct css_syntax *syn=css_syntax_parse(syntax,strlen(syntax),&arena);
    if(!syn)return "SYNTAX";
    text.n=0;if(text.p)text.p[0]=0;
    return css_custom_compose(&ctx,syn,a,b,accumulate,&text)?sb_cstr(&text):"REPLACE";
}
/* the base style's color (what currentcolor is) */
static void base_color(const char *c){
    css_style_init(&base,&parent);
    uint8_t set[css_prop_count()];memset(set,0,sizeof set);
    struct cx cx={.s=&base,.parent=&parent,.em=16,.rem=16,.vw=800,.vh=600,.a=&arena,.set=set};
    css_apply(css_prop_lookup("color",5),c,strlen(c),&cx);
    css_style_finish(&base,&parent,false);
}
/* what getComputedStyle shows once the cascade applies a sampled value at the animation origin */
static char shown_text[512];
static const char *shown(const char *property,const char *v){
    const struct propdef *p=css_prop_lookup(property,strlen(property));style_t x;
    if(!p||!css_anim_parse(&ctx,p,v,strlen(v),false,&x,NULL))return "PARSE";
    css_style_finish(&x,&parent,false);
    sbuf b={0};
    bool ok=!strcmp(property,"box-shadow")?css_shadow_serialize(x.box_shadow,&b,NULL):css_computed_longhand(&x,property,&b,NULL)==1;
    snprintf(shown_text,sizeof shown_text,"%s",ok?sb_cstr(&b):"NONE");
    sb_free(&b);
    return shown_text;
}
/* interpolation-testcommon.js's normalizeValue: numbers with a fraction to two places */
static char round_text[512];
static const char *wpt_round(const char *s){
    size_t n=0;
    while(*s&&n+32<sizeof round_text){
        const char *q=s;if(*q=='-')q++;
        const char *dot=q;while(*dot>='0'&&*dot<='9')dot++;
        bool number=(q==s||s[-1]==' '||s[-1]=='('||s[-1]==',')&&*dot=='.'&&dot[1]>='0'&&dot[1]<='9'&&(dot>q||*q=='.');
        if(!number){round_text[n++]=*s++;continue;}
        char *end;double v=strtod(s,&end);
        char t[64];snprintf(t,sizeof t,"%.2f",v);
        size_t k=strlen(t);while(k&&t[k-1]=='0')t[--k]=0;if(k&&t[k-1]=='.')t[--k]=0;
        if(!strcmp(t,"-0"))strcpy(t,"0");
        memcpy(round_text+n,t,strlen(t));n+=strlen(t);s=end;
    }
    round_text[n]=0;return round_text;
}
/* one keyframe pair of an effect on underlying (animation.c's effect value): from and to composite
   ('r' replace, 'a' add, 'c' accumulate) onto it, then interpolated at t, as the cascade shows it */
static const char *effect(const char *property,const char *under,const char *from,char fc,const char *to,char tc,double t){
    const struct propdef *p=css_prop_lookup(property,strlen(property));
    style_t u,v[2];const char *ends[2]={from,to};char ops[2]={fc,tc};
    if(!p||!css_anim_parse(&ctx,p,under,strlen(under),false,&u,NULL))return "PARSE";
    for(int i=0;i<2;i++){
        if(!css_anim_parse(&ctx,p,ends[i],strlen(ends[i]),false,&v[i],NULL))return "PARSE";
        if(ops[i]=='r')continue;
        text.n=0;if(text.p)text.p[0]=0;
        if(!css_anim_compose(&ctx,p,&u,&v[i],ops[i]=='c',&text))continue;
        char *combined=strdup(sb_cstr(&text));
        bool ok=css_anim_parse(&ctx,p,combined,strlen(combined),true,&v[i],NULL);
        free(combined);
        if(!ok)return "PARSE-COMBINED";
    }
    text.n=0;if(text.p)text.p[0]=0;
    if(!css_anim_interpolate(&ctx,p,&v[0],&v[1],t,&text))return "DISCRETE";
    char *sampled=strdup(sb_cstr(&text));
    const char *r=shown(property,sampled);
    free(sampled);
    return r;
}
#define EXPECT(call,want) do{const char *got_=(call);check(!strcmp(got_,want),#call,got_);}while(0)
static double ease(const char *e,double x,bool before){struct css_easing v;if(!css_easing_parse(e,strlen(e),&v))return NAN;return css_easing_apply(&v,x,before);}
static const char *easing_text(const char *e){struct css_easing v;if(!css_easing_parse(e,strlen(e),&v))return "INVALID";text.n=0;if(text.p)text.p[0]=0;css_easing_serialize(&v,&text);return sb_cstr(&text);}
int main(void){
    css_style_init(&parent,NULL);css_style_init(&base,&parent);css_style_finish(&base,&parent,false);
    ctx=(struct css_anim_ctx){.base=&base,.parent=&parent,.rem=16,.vw=800,.vh=600,.a=&arena};
    /* lengths: by computed value, the range clamp of non-negative properties */
    EXPECT(mix("width","10px","20px",.5),"15px");
    EXPECT(mix("width","10px","20px",-2),"0px");
    EXPECT(mix("left","10px","20px",-2),"-10px");
    EXPECT(mix("width","10px","50%",.5),"calc(25% + 5px)");
    EXPECT(mix("width","auto","10px",.3),"DISCRETE-A");
    EXPECT(mix("width","auto","10px",.6),"DISCRETE-B");
    EXPECT(mix("width","1em","3em",.5),"32px");
    /* numbers, integers */
    EXPECT(mix("opacity","0","1",1.5),"1");
    EXPECT(mix("opacity","0","1",.25),"0.25");
    EXPECT(mix("z-index","1","4",.5),"3");
    EXPECT(mix("z-index","auto","4",.5),"DISCRETE-B");
    EXPECT(mix("font-weight","100","900",.5),"500");
    EXPECT(mix("flex-grow","1","2",-1),"0");
    /* colours: premultiplied sRGB */
    EXPECT(mix("color","rgb(255, 0, 0)","rgba(0, 0, 255, 0)",.5),"rgba(255, 0, 0, 0.5)");
    EXPECT(mix("background-color","black","white",.5),"rgb(128, 128, 128)");
    /* visibility and display */
    EXPECT(mix("visibility","visible","hidden",.5),"visible");
    EXPECT(mix("visibility","visible","hidden",1),"hidden");
    EXPECT(mix("visibility","hidden","visible",0),"hidden");
    EXPECT(mix("visibility","hidden","visible",.1),"visible");
    EXPECT(mix("display","none","flex",.3),"DISCRETE-B");
    EXPECT(mix("display","flex","none",.7),"DISCRETE-A");
    EXPECT(mix("display","flex","none",1),"DISCRETE-B");
    EXPECT(mix("display","grid","flex",.4),"DISCRETE-A");
    EXPECT(mix("content-visibility","hidden","visible",.1),"DISCRETE-B");
    EXPECT(mix("float","left","right",.5),"DISCRETE-B");
    /* transforms */
    EXPECT(mix("transform","translate(10px)","translate(30px)",.5),"translate(20px, 0px)");
    EXPECT(mix("transform","none","scale(3)",.5),"scale(2, 2)");
    EXPECT(mix("transform","rotate(0deg)","rotate(180deg)",.5),"rotate(1.570796rad)");
    EXPECT(mix("translate","none","10px 20px",.5),"5px 10px");
    EXPECT(mix("scale","none","3",.5),"2"); /* css-transforms scale-parsing-computed: the shortest form */
    EXPECT(mix("rotate","none","90deg",.5),"45deg");
    EXPECT(compose("transform","translate(10px)","scale(2)",false),"translate(10px, 0px) scale(2, 2)");
    EXPECT(compose("transform","scale(2)","scale(3)",true),"scale(4, 4)");
    EXPECT(compose("scale","2","3",false),"6");
    /* shadow and filter lists */
    EXPECT(mix("box-shadow","none","10px 20px 30px 40px rgb(0, 0, 255)",.5),"rgba(0, 0, 255, 0.5) 5px 10px 15px 20px");
    EXPECT(mix("box-shadow","10px 10px red","10px 10px blue inset",.5),"DISCRETE-B");
    EXPECT(mix("filter","none","opacity(0.5)",.5),"opacity(0.75)");
    /* addition and accumulation */
    EXPECT(compose("width","10px","20px",false),"30px");
    EXPECT(compose("opacity","0.6","0.6",false),"1");
    EXPECT(compose("display","block","flex",false),"REPLACE");
    /* computed values read back */
    EXPECT(computed("transform","translateX(5px) rotate(90deg)"),"translate(5px, 0px) rotate(1.570796rad)");
    EXPECT(computed("display","inline-flex"),"inline-flex");
    EXPECT(computed("z-index","auto"),"auto");
    EXPECT(computed("line-height","1.5"),"1.5");
    EXPECT(computed("background-position","left 10px top 20%"),"10px 20%");
    /* registered custom properties (css-properties-values-api/animation: the keyframes composited
       with the underlying value, then interpolated at the middle) */
    EXPECT(custom("<length>","100px","200px",.5),"150px");
    EXPECT(custom_compose("<length>","100px","200px",false),"300px");
    EXPECT(custom("<integer>","100","200",.5),"150");
    EXPECT(custom("<integer>","1","2",.5),"2");
    EXPECT(custom("<number>","0","1",.25),"0.25");
    EXPECT(custom("<angle>","0deg","180deg",.5),"90deg");
    EXPECT(custom("<time>","1s","2s",.5),"1.5s");
    EXPECT(custom("<percentage>","10%","20%",.5),"15%");
    EXPECT(custom("<resolution>","1dppx","3dppx",.5),"2dppx");
    EXPECT(custom("<color>","rgb(100, 100, 100)","rgb(200, 200, 200)",.5),"rgb(150, 150, 150)");
    EXPECT(custom_compose("<color>","rgb(100, 100, 100)","rgb(50, 50, 50)",false),"rgb(150, 150, 150)");
    EXPECT(custom("<length-percentage>#","100px, 150%","200%, 250px",.5),"calc(100% + 50px), calc(75% + 125px)");
    EXPECT(custom("<length-percentage>#","10px, 20%","30px",.5),"DISCRETE");
    EXPECT(custom("<length>+","1px 2px","3px 4px",.5),"2px 3px");
    EXPECT(custom("<custom-ident>","a","b",.5),"DISCRETE");
    EXPECT(custom("<number>+ | <transform-list>","translateX(100px)","200",.5),"DISCRETE");
    EXPECT(custom("<transform-function>","translateX(100px)","translateX(200px)",.5),"translateX(150px)");
    EXPECT(custom_compose("<transform-function>","translateX(100px)","translateX(200px)",false),"translateX(300px)");
    EXPECT(custom_compose("<transform-function>","translateX(100px)","scale(2)",false),"REPLACE");
    EXPECT(custom("<transform-list>","translateX(100px) scale(2)","translateX(200px) scale(4)",.5),"translateX(150px) scale(3)");
    EXPECT(custom("<transform-list>","scale(2)","scale(4) rotate(90deg)",.5),"scale(3) rotate(45deg)");
    EXPECT(custom("<transform-list>","translateX(100px)","translateY(100px)",.5),"translate(50px, 50px)");
    EXPECT(custom_compose("<transform-list>","translateX(100px) scale(2)","translateX(200px) scale(3)",false),
           "translateX(100px) scale(2) translateX(200px) scale(3)");
    EXPECT(custom_compose("<transform-list>","translateX(50px) scale(3.5)","translateX(100px) scale(4)",true),"translateX(150px) scale(6.5)");
    EXPECT(custom("<transform-function>+","rotate(0deg)","rotate(90deg)",.5),"rotate(45deg)");
    /* the animation-* and transition-* lists (css-animations/parsing, css-transitions/parsing) */
    EXPECT(computed("animation-duration","1s, 200ms"),"1s, 0.2s");
    EXPECT(computed("animation-duration","auto"),"auto");
    EXPECT(computed("animation-iteration-count","infinite, 2.5"),"infinite, 2.5");
    EXPECT(computed("animation-name","none, foo, \"bar baz\""),"none, foo, \"bar baz\"");
    EXPECT(computed("animation-name","\"\""),"PARSE"); /* parsing/animation-name-invalid.html */
    EXPECT(computed("animation-delay","-1s"),"-1s");
    EXPECT(computed("animation-direction","alternate-reverse, normal"),"alternate-reverse, normal");
    EXPECT(computed("animation-fill-mode","both"),"both");
    EXPECT(computed("animation-play-state","paused, running"),"paused, running");
    EXPECT(computed("animation-composition","add, accumulate"),"add, accumulate");
    EXPECT(computed("animation-timing-function","ease, step-start, cubic-bezier(0.1, 0.2, 0.3, 0.4)"),"ease, steps(1, start), cubic-bezier(0.1, 0.2, 0.3, 0.4)");
    EXPECT(computed("transition-property","opacity, WIDTH, --x, unknown-thing"),"opacity, width, --x, unknown-thing");
    EXPECT(computed("transition-property","none"),"none");
    EXPECT(computed("transition-property","all"),"all");
    EXPECT(computed("transition-behavior","allow-discrete, normal"),"allow-discrete, normal");
    EXPECT(computed("transition-duration","-1s"),"PARSE");
    /* <time> math functions (css-values/calc-time-values.html; css-easing/step-jump-*.html's
       animation-delay: calc(var(--at) * -1s)); a negative duration calculation clamps to 0 */
    EXPECT(computed("transition-delay","calc(4s + 1s)"),"5s");
    EXPECT(computed("transition-delay","calc(4s + 1ms)"),"4.001s");
    EXPECT(computed("transition-delay","calc(4ms + 1ms)"),"0.005s");
    EXPECT(computed("transition-delay","calc(4s - 1ms)"),"3.999s");
    EXPECT(computed("transition-delay","calc(4 * 1s)"),"4s");
    EXPECT(computed("transition-delay","calc(4ms / 2)"),"0.002s");
    EXPECT(computed("transition-delay","calc(4000ms)"),"4s");
    EXPECT(computed("animation-delay","calc(3.0 * -1s)"),"-3s");
    EXPECT(computed("animation-duration","min(1s, 200ms), max(1s, 200ms)"),"0.2s, 1s");
    EXPECT(computed("transition-duration","calc(-1s)"),"0s");
    EXPECT(computed("transition-delay","calc(1px + 1s)"),"PARSE");
    EXPECT(computed("transition-delay","calc(1s * 2s)"),"PARSE");
    EXPECT(computed("transition-delay","calc(50%)"),"PARSE");
    EXPECT(computed("transition-delay","calc(2)"),"PARSE");
    EXPECT(computed("transition-property","none, opacity"),"PARSE");
    /* a shorthand's longhands */
    uint8_t set[css_prop_count()];css_prop_longhands(css_prop_lookup("margin",6),set);
    int count=0;for(int i=0;i<css_prop_count();i++)count+=set[i];
    check(count==4,"margin has four longhands",count==4?"4":"other");
    /* css-easing-1 */
    check(ease("steps(1, end)",.5,false)==0,"steps(1, end) at 0.5","");
    check(ease("steps(1, start)",.5,false)==1,"steps(1, start) at 0.5","");
    check(ease("steps(1, start)",0,true)==0,"steps(1, start) before flag at 0","");
    check(ease("steps(1, start)",0,false)==1,"steps(1, start) at 0","");
    check(ease("steps(2, jump-none)",.5,false)==1,"steps(2, jump-none) at 0.5","");
    check(ease("steps(3, jump-both)",.5,false)==.5,"steps(3, jump-both) at 0.5","");
    check(fabs(ease("ease",.5,false)-0.8024033877399112)<1e-6,"ease at 0.5","");
    check(fabs(ease("cubic-bezier(0, 1.5, 1, 1.5)",.5,false)-1.25)<1e-6,"overshooting curve","");
    check(fabs(ease("cubic-bezier(0.5, 0.5, 0.5, 0.5)",-1,false)+1)<1e-9,"start tangent","");
    check(fabs(ease("cubic-bezier(0.5, 0.5, 0.5, 0.5)",2,false)-2)<1e-9,"end tangent","");
    EXPECT(easing_text("step-start"),"steps(1, start)");
    EXPECT(easing_text("step-end"),"steps(1)");
    EXPECT(easing_text("steps(2, jump-end)"),"steps(2)");
    EXPECT(easing_text("steps(2, jump-start)"),"steps(2, jump-start)");
    EXPECT(easing_text("steps(1, jump-none)"),"INVALID");
    EXPECT(easing_text("ease-in-out"),"ease-in-out");
    EXPECT(easing_text("cubic-bezier(0.1, 0.2, 0.8, 0.9)"),"cubic-bezier(0.1, 0.2, 0.8, 0.9)");
    /* <color> (color.c's type): css-color/animation/color-interpolation.html */
    EXPECT(wpt_round(shown("color",mix("color","black","orange",-.3))),"rgb(0, 0, 0)");
    EXPECT(wpt_round(shown("color",mix("color","black","orange",.3))),"rgb(77, 50, 0)");
    EXPECT(wpt_round(shown("color",mix("color","black","orange",.6))),"rgb(153, 99, 0)");
    EXPECT(wpt_round(shown("color",mix("color","black","orange",1.5))),"rgb(255, 248, 0)");
    /* from neutral (the target's yellow) to green: 178.5 and 64.5 round up */
    EXPECT(wpt_round(shown("color",mix("color","yellow","green",.3))),"rgb(179, 217, 0)");
    EXPECT(wpt_round(shown("color",mix("color","yellow","green",.6))),"rgb(102, 179, 0)");
    EXPECT(wpt_round(shown("color",mix("color","yellow","green",1.5))),"rgb(0, 65, 0)");
    EXPECT(wpt_round(shown("color",mix("color","rgb(0 0 0)","color(srgb 1 1 1)",-.3))),"oklab(0 0 0)");
    EXPECT(wpt_round(shown("color",mix("color","rgb(0 0 0)","color(srgb 1 1 1)",0))),"oklab(0 0 0)");
    EXPECT(wpt_round(shown("color",mix("color","rgb(0 0 0)","color(srgb 1 1 1)",.3))),"oklab(0.3 0 0)");
    EXPECT(wpt_round(shown("color",mix("color","color(srgb 0 0 0)","rgb(255 255 255)",.6))),"oklab(0.6 0 0)");
    EXPECT(wpt_round(shown("color",mix("color","color(srgb 0 0 0)","color(srgb 1 1 1)",1.5))),"oklab(1 0 0)");
    /* css-backgrounds/animations/background-color-interpolation.html */
    EXPECT(wpt_round(shown("background-color",mix("background-color","transparent","green",.3))),"rgba(0, 128, 0, 0.3)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","transparent","green",1.5))),"rgb(0, 192, 0)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","white","orange",.3))),"rgb(255, 228, 179)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","white","orange",1.5))),"rgb(255, 120, 0)");
    base_color("rgba(0, 0, 255, 0.5)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","currentcolor","rgba(0, 255, 0, 0.75)",-.5))),"rgba(0, 0, 255, 0.38)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","currentcolor","rgba(0, 255, 0, 0.75)",0))),"rgba(0, 0, 255, 0.5)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","currentcolor","rgba(0, 255, 0, 0.75)",.25))),"rgba(0, 85, 170, 0.56)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","currentcolor","rgba(0, 255, 0, 0.75)",.5))),"rgba(0, 153, 102, 0.63)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","currentcolor","rgba(0, 255, 0, 0.75)",.75))),"rgba(0, 208, 47, 0.69)");
    EXPECT(wpt_round(shown("background-color",mix("background-color","currentcolor","rgba(0, 255, 0, 0.75)",1.5))),"rgba(0, 255, 0, 0.88)");
    /* an interval of currentcolor stays currentcolor, for the cascade to resolve against the element */
    EXPECT(mix("border-top-color","currentcolor","currentcolor",.4),"currentcolor");
    EXPECT(computed("background-color","currentcolor"),"currentcolor");
    EXPECT(computed("outline-color","color-mix(in srgb, currentcolor, white)"),"color-mix(in srgb, currentcolor, rgb(255, 255, 255))");
    check(same("border-top-color","currentcolor","currentcolor"),"currentcolor is currentcolor","");
    check(!same("border-top-color","currentcolor","rgba(0, 0, 255, 0.5)"),"currentcolor is not its colour","");
    /* web-animations/responsive/to-color-change.html: premultiplied */
    base_color("rgba(160, 190, 180, 0.980392157)");
    EXPECT(shown("background-color",mix("background-color","rgba(250, 240, 220, 0.431372549)","currentcolor",.3)),"rgba(206, 215, 200, 0.596)");
    /* web-animations/animation-model/animation-types/property-types.js (colorType, shadow lists) */
    base_color("rgb(0, 255, 0)");
    EXPECT(shown("box-shadow",mix("box-shadow","currentcolor 0px 0px 0px 0px","currentcolor 10px 10px 10px 10px",.5)),"rgb(0, 255, 0) 5px 5px 5px 5px");
    css_style_init(&base,&parent);css_style_finish(&base,&parent,false);
    EXPECT(shown("color",mix("color","#ff000066","#0000ffcc",.5)),"rgba(85, 0, 170, 0.6)");
    EXPECT(shown("color",mix("color","hsl(0,   100%, 50%)","hsl(240, 100%, 50%)",.5)),"rgb(128, 0, 128)");
    for(int accumulate=0;accumulate<2;accumulate++){
        char op=accumulate?'c':'a';
        EXPECT(effect("color","rgb(128, 128, 128)","rgb(255, 0, 0)",op,"rgb(0, 0, 255)",op,0),"rgb(255, 128, 128)");
        EXPECT(effect("color","rgb(128, 128, 128)","rgb(255, 0, 0)",op,"rgb(0, 0, 255)",op,.5),"rgb(255, 128, 255)");
        EXPECT(effect("color","rgb(128, 128, 128)","#ff000066",op,"#0000ffcc",op,0),"rgb(230, 128, 128)");
        EXPECT(effect("background-color","rgb(128, 128, 128)","rgba(255, 0, 0, 0.4)",op,"rgba(0, 0, 255, 0.8)",op,0),"rgb(230, 128, 128)");
    }
    EXPECT(compose("box-shadow","rgb(120, 120, 120) 10px 10px 10px 10px","rgb(120, 120, 120) 10px 10px 10px 10px",true),"rgb(240, 240, 240) 20px 20px 20px 20px");
    EXPECT(compose("box-shadow","rgb(0, 0, 0) 0px 0px 0px 0px","rgb(120, 120, 120) 10px 10px 10px 0px",false),"rgb(0, 0, 0) 0px 0px 0px 0px, rgb(120, 120, 120) 10px 10px 10px 0px");
    /* css-color/animation/color-composition.html */
    EXPECT(effect("color","rgb(50, 50, 50)","rgb(10, 10, 10)",'a',"rgb(30, 30, 30)",'r',0),"rgb(60, 60, 60)");
    EXPECT(effect("color","rgb(50, 50, 50)","rgb(10, 10, 10)",'a',"rgb(30, 30, 30)",'r',.2),"rgb(54, 54, 54)");
    EXPECT(effect("color","rgb(50, 50, 50)","rgb(10, 10, 10)",'a',"rgb(30, 30, 30)",'r',1.5),"rgb(15, 15, 15)");
    EXPECT(effect("color","rgb(60, 60, 60)","rgb(0, 0, 0)",'a',"rgb(50, 50, 50)",'r',1.2),"rgb(48, 48, 48)");
    /* addition leaves a legacy colour outside the cube until the final value's parse */
    EXPECT(compose("color","rgb(128, 128, 128)","rgb(255, 0, 0)",false),"rgb(383 128 128)");
    EXPECT(shown("color","rgb(383 128 128)"),"rgb(255, 128, 128)");
    /* the same computed value: rgb() and hsl() compute alike, color(srgb) is another value */
    check(same("color","red","hsl(0 100% 50%)"),"red is hsl(0 100% 50%)","");
    check(!same("color","red","color(srgb 1 0 0)"),"red is not color(srgb 1 0 0)","");
    EXPECT(computed("color","color(display-p3 1 0 0)"),"color(display-p3 1 0 0)");
    /* currentcolor computes to itself (CSS Color 4 §4.4): after the cascade finishes the style the
       animation layer still reads currentcolor, getComputedStyle the used colour, and inherit takes
       currentcolor for the child's own colour */
    {
        static style_t up,child;
        uint8_t set2[css_prop_count()];
        const struct propdef *colour=css_prop_lookup("color",5),*bg=css_prop_lookup("background-color",16);
        css_style_init(&up,&parent);memset(set2,0,sizeof set2);
        struct cx ux={.s=&up,.parent=&parent,.em=16,.rem=16,.vw=800,.vh=600,.a=&arena,.set=set2};
        css_apply(colour,"red",3,&ux);css_apply(bg,"currentcolor",12,&ux);css_style_finish(&up,&parent,false);
        sbuf b={0};
        check(css_computed_value(&up,bg,&b,NULL) && !strcmp(sb_cstr(&b),"currentcolor"),"finished currentcolor computes to currentcolor",sb_cstr(&b));
        b.n=0;check(css_computed_longhand(&up,"background-color",&b,NULL)==1 && !strcmp(sb_cstr(&b),"rgb(255, 0, 0)"),"its resolved value",sb_cstr(&b));
        css_style_init(&child,&up);memset(set2,0,sizeof set2);
        struct cx cx2={.s=&child,.parent=&up,.em=16,.rem=16,.vw=800,.vh=600,.a=&arena,.set=set2};
        css_apply(colour,"blue",4,&cx2);css_apply(bg,"inherit",7,&cx2);css_style_finish(&child,&up,false);
        b.n=0;check(css_computed_longhand(&child,"background-color",&b,NULL)==1 && !strcmp(sb_cstr(&b),"rgb(0, 0, 255)"),"inherited currentcolor is the child's colour",sb_cstr(&b));
        memset(set2,0,sizeof set2);css_apply(bg,"lime",4,&cx2);css_style_finish(&child,&up,false);
        b.n=0;check(css_computed_value(&child,bg,&b,NULL) && !strcmp(sb_cstr(&b),"rgb(0, 255, 0)"),"a new value clears currentcolor",sb_cstr(&b));
        sb_free(&b);
    }
    printf("animation-values: %d checks, %d failures\n",checks,failed);
    return failed!=0;
}
'''
source = out / "values.c"
source.write_text(css_host + production + main, encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
if not cc.exists():
    cc = root.parents[2] / "tools/msys64/ucrt64/bin/clang.exe"  # a git worktree shares the main tree's tools
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
(abi / "time.h").write_text('#include "' + (cc.parent.parent / "include/time.h").as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "values.exe"
command = [str(cc), "-std=gnu11", "-O1", "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include",
           "-Icommon", "-Iuser/libc", "-Iuser/libc/web", "-Ithird_party/quickjs", str(source), "user/libc/web/cssprop.c", "user/libc/web/shape.c",
           "user/libc/web/color.c", "user/libc/web/cssmath.c", "user/libc/web/util.c", "user/libc/web/html.c", "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=60)
