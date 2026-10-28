"""動きの値の層（cssprop.c の css_anim_*、緩急の css_easing_*）を実 C のままホストで確かめる。
カスケード・時間の模型・JS・実サイトの代わりではない。期待値は WPT の *-interpolation.html と
css-easing の試験の値から取った一部。"""
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
static int checks,failed;
static void check(bool ok,const char *name,const char *got){checks++;if(!ok){failed++;printf("FAIL animation-values %s: %s\n",name,got?got:"(null)");}}
static style_t parent,base;
static arena_t arena;
static struct css_anim_ctx ctx;
static sbuf text;
static const char *mix(const char *property,const char *a,const char *b,double t){
    const struct propdef *p=css_prop_lookup(property,strlen(property));style_t x,y;
    if(!p||!css_anim_parse(&ctx,p,a,strlen(a),&x,NULL)||!css_anim_parse(&ctx,p,b,strlen(b),&y,NULL))return "PARSE";
    text.n=0;if(text.p)text.p[0]=0;
    if(!css_anim_interpolate(&ctx,p,&x,&y,t,&text))return css_anim_discrete_second(p,&x,&y,t)?"DISCRETE-B":"DISCRETE-A";
    return sb_cstr(&text);
}
static const char *compose(const char *property,const char *a,const char *b,bool accumulate){
    const struct propdef *p=css_prop_lookup(property,strlen(property));style_t x,y;
    if(!p||!css_anim_parse(&ctx,p,a,strlen(a),&x,NULL)||!css_anim_parse(&ctx,p,b,strlen(b),&y,NULL))return "PARSE";
    text.n=0;if(text.p)text.p[0]=0;
    if(!css_anim_compose(&ctx,p,&x,&y,accumulate,&text))return "REPLACE";
    return sb_cstr(&text);
}
static const char *computed(const char *property,const char *v){
    const struct propdef *p=css_prop_lookup(property,strlen(property));style_t x;
    if(!p||!css_anim_parse(&ctx,p,v,strlen(v),&x,NULL))return "PARSE";
    text.n=0;if(text.p)text.p[0]=0;
    return css_computed_value(&x,p,&text,NULL)?sb_cstr(&text):"NONE";
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
    EXPECT(mix("scale","none","3",.5),"2 2");
    EXPECT(mix("rotate","none","90deg",.5),"45deg");
    EXPECT(compose("transform","translate(10px)","scale(2)",false),"translate(10px, 0px) scale(2, 2)");
    EXPECT(compose("transform","scale(2)","scale(3)",true),"scale(4, 4)");
    EXPECT(compose("scale","2","3",false),"6 6");
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
    printf("animation-values: %d checks, %d failures\n",checks,failed);
    return failed!=0;
}
'''
source = out / "values.c"
source.write_text(css_host + production + main, encoding="utf-8")
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
exe = out / "values.exe"
command = [str(cc), "-std=gnu11", "-O1", "-Wno-incompatible-library-redeclaration", "-I" + str(abi), "-Iuser/include",
           "-Icommon", "-Iuser/libc", "-Iuser/libc/web", "-Ithird_party/quickjs", str(source), "user/libc/web/cssprop.c",
           "user/libc/web/util.c", "user/libc/web/html.c", str(url), "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=60)
