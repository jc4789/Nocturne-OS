"""実Cの影算術をhost分離確認。並列・DOM・実サイトの代用品ではない。"""
from pathlib import Path
import os
import subprocess
import json
import re
import sys
import url_owner
root = Path(__file__).resolve().parent.parent
out = root / "build/browser-compat-shadow-host"
out.mkdir(exist_ok=True)
paint = (root / "user/libc/web/paint.c").read_text(encoding="utf-8")
types = paint[paint.index("enum { M_PAINT"):paint.index('#include "paint_media_debug.h"')]
profile = paint.index("static struct { uint64_t paints,")
types += paint[profile:paint.index("} paint_profile;", profile)+len("} paint_profile;")] + "\n"
geometry = paint[paint.index("static float sdf_rrect("):paint.index("/* Each primitive is joined")]
start = paint.index("static float used_radius(", paint.index("struct raster_job"))
radius = paint[start:paint.index("\n}", start)+2]
prefix = '''#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <parallel.h>
#include "webi.h"
#include "avmedia.h"
#include <image.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
static bool cancelled;
bool web_native_checkpoint(web_doc *d){if(cancelled){d->native_cancelled=true;return false;}return true;}
void parallel_record_work(enum parallel_stage stage,uint64_t count){(void)stage;(void)count;}
bool parallel_for_stage(enum parallel_stage stage,size_t count,parallel_fn fn,void *context,parallel_service_fn service,void *opaque){
 (void)stage;for(size_t i=0;i<count;i++){fn(i,context);if(service)service(opaque);}return false;}
static unsigned fail_allocate;
static void *shadow_malloc(size_t n){if(fail_allocate&&! --fail_allocate)return NULL;return malloc(n);}
static void *shadow_calloc(size_t n,size_t z){if(fail_allocate&&! --fail_allocate)return NULL;return calloc(n,z);}
#define malloc shadow_malloc
#define calloc shadow_calloc
'''
cases = (root / ("tests/radius_native_cases.c" if any(x in sys.argv for x in ("--radius","--mask-performance","--image-association")) else "tests/boxshadow_native_cases.c")).read_text(encoding="utf-8")
css = (root / "user/libc/web/css.c").read_text(encoding="utf-8")
production = css[:css.index("/* ---------------------------------------------------------------- matching */")]
production += css[css.index("/* ---------------------------------------------------------------- feature queries"):css.index("struct sheet_parse_job {")]
production += css[css.index("/* ---------------------------------------------------------------- media queries"):css.index("static bool media_chain_ok(")]
production = production.replace("#include <nocturne.h>\n", "")
source = out / "native.c"
image_source=(root / "user/libc/image.c").read_text(encoding="utf-8")
image_draw=image_source[image_source.index("void image_draw("):]
source.write_text(prefix + types + geometry + radius + "\n#undef malloc\n#undef calloc\n" + image_draw + cases, encoding="utf-8")
support = out / "css-support.c"
# The host has no display: media queries see an 8-bit one.
css_host = "void *thread_local_get(unsigned slot);\nint display_bits_per_channel(void){return 8;}\n#define MIN(a,b) ((a)<(b)?(a):(b))\n#define MAX(a,b) ((a)>(b)?(a):(b))\n"
support.write_text(css_host+production,encoding="utf-8")
gfx = (root / "common/gfx.c").read_text(encoding="utf-8")
gfx_source = out / "gfx.c"
gfx_source.write_text(gfx[:gfx.index("void gfx_line(")] + gfx[gfx.index("void gfx_blit("):gfx.index("void gfx_triangle(")], encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
abi = out / "host-abi"
abi.mkdir(exist_ok=True)
(abi / "setjmp.h").write_text('#include "' + (cc.parent.parent / "include/setjmp.h").as_posix() + '"\n', encoding="utf-8")
(abi / "time.h").write_text('#include "' + (cc.parent.parent / "include/time.h").as_posix() + '"\n', encoding="utf-8")
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "native.exe"
command = [str(cc), "-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
           "-Wno-incompatible-library-redeclaration", "-I"+str(abi), "-Iuser/include", "-Icommon", "-Iuser/libc", "-Iuser/libc/web",
           "-Ithird_party/quickjs", str(source), str(gfx_source),str(support),"user/libc/web/cssprop.c", "user/libc/web/shape.c","user/libc/web/color.c","user/libc/web/util.c","user/libc/web/html.c",
           # CSS resolves url() through the URL record's owner (tests/url_owner.py).
           *url_owner.objects(root, cc, util=False), "-o", str(exe)]
subprocess.run(command, cwd=root, env=env, check=True)
if "--parser" not in sys.argv and "--radius-parser" not in sys.argv:
    subprocess.run([str(exe)] + [x for x in ("--mask-performance","--image-association") if x in sys.argv], cwd=root, env=env, check=True, timeout=60)
else:
    # Original WPT values test actual product grammar; this is NOT running
    # the browser testharness or claiming WPT CSSOM/VM conformance.
    wpt = Path("D:/Programes/cloned repos/wpt/css/css-backgrounds/parsing")
    checks = []
    feature="border-radius" if "--radius-parser" in sys.argv else "box-shadow"
    for name, valid in ((feature+"-valid.html", True), (feature+"-invalid.html", False)):
        for property,value in re.findall(r'test_(?:valid|invalid)_value\("([^"]+)",\s*"([^"]+)"',
                                         (wpt / name).read_text(encoding="utf-8")):
            literal = json.dumps(value)
            checks.append(f'check(css_supports_declaration({json.dumps(property)},{len(property)},{literal},strlen({literal}))=={str(valid).lower()},{literal});')
    if feature=="border-radius":
        checks.append('style_t style,parent;css_style_init(&parent,NULL);css_style_init(&style,&parent);arena_t arena={0};uint8_t property_bits[css_prop_count()];memset(property_bits,0,sizeof property_bits);struct cx cx={.s=&style,.parent=&parent,.em=16,.rem=16,.vw=800,.vh=600,.a=&arena,.set=property_bits};')
        checks.append('check(css_apply(css_prop_lookup("border-radius",13),"50%",3,&cx)&&!arena.head,"ordinary percentage radius stays in style POD without a computed arena chunk");')
        checks.append('sbuf radius_text={0};check(css_radius_serialize(&style,-1,&radius_text,NULL)&&!strcmp(radius_text.p,"50%"),"computed radius preserves both percentage axes");sb_free(&radius_text);ar_free(&arena);')
        checks.append('check(css_radius_specified("border-radius","1px 1px 1px 2% / 1px 2% 1px 2%",strlen("1px 1px 1px 2% / 1px 2% 1px 2%"),&radius_text)&&!strcmp(radius_text.p,"1px 1px 1px 2% / 1px 2%"),"specified radius shorthand canonical repeated-value reduction");sb_free(&radius_text);')
    css = (root / "user/libc/web/css.c").read_text(encoding="utf-8")
    production = css[:css.index("/* ---------------------------------------------------------------- matching */")]
    production += css[css.index("/* ---------------------------------------------------------------- feature queries"):css.index("struct sheet_parse_job {")]
    production += css[css.index("/* ---------------------------------------------------------------- media queries"):css.index("static bool media_chain_ok(")]
    production = production.replace("#include <nocturne.h>\n", "")
    production = css_host + production
    main = '''
bool web_native_checkpoint(web_doc *d){(void)d;return true;}
static int checks,failed;
static void check(bool valid,const char *name){checks++;if(!valid){failed++;printf("FAIL shadow-parser %s\\n",name);}}
int main(void){
''' + "\n".join(checks) + '''
sbuf out={0};
check(css_shadow_specified("inset 1px 2px red",17,&out)&&!strcmp(out.p,"red 1px 2px inset"),"specified canonical group order");sb_free(&out);
check(css_shadow_specified("1px 1px calc(-1px)",strlen("1px 1px calc(-1px)"),&out)&&!strcmp(out.p,"1px 1px calc(-1px)"),"specified calculation not computed/clamped");sb_free(&out);
struct css_box_shadow shadow={.x=1,.y=2,.blur=3,.spread=4,.color=0xff00ff00,.inset=true};
check(css_shadow_serialize(&shadow,&out,NULL)&&!strcmp(out.p,"rgb(0, 255, 0) 1px 2px 3px 4px inset"),"computed canonical inset suffix");sb_free(&out);
printf("boxshadow-parser: %d checks, %d failures\\n",checks,failed);return failed!=0;
}
'''
    source = out / "parser.c"
    source.write_text(production + main, encoding="utf-8")
    exe = out / "parser.exe"
    command = command[:command.index(str(out / "native.c"))] + [str(source),"user/libc/web/cssprop.c", "user/libc/web/shape.c","user/libc/web/color.c","user/libc/web/util.c","user/libc/web/html.c",*url_owner.objects(root,cc,util=False),"-o",str(exe)]
    subprocess.run(command,cwd=root,env=env,check=True)
    subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=60)
