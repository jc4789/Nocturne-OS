"""The product text geometry, compiled from its sources: box.c's processed-text source map,
doc.c's UTF-16 offsets of counted data, layout.c's text geometry section (Range and
CaretPosition rectangles, boundaries, DOM positions) and paint.c's find-in-page text and text
control line. Fonts and box placement are fixed stubs; the cases are
tests/text_geometry_native_cases.c."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/text-geometry-native'
out.mkdir(parents=True, exist_ok=True)


def read(name):
    return (root / name).read_text(encoding='utf-8')


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2] + '\n'


def between(text, first, stop):
    start = text.index(first)
    return text[start:text.index(stop, start)]


def line(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n', start) + 1]


layout, box, doc, paint, util = (read('user/libc/web/' + name + '.c') for name in ('layout', 'box', 'doc', 'paint', 'util'))
pieces = between(util, 'struct achunk {', 'char *ar_strndup(') + function(util, 'void ar_free(')
pieces += between(util, 'static void sb_grow(', 'int utf8_put(')
pieces += function(util, 'char *sb_cstr(') + function(util, 'void sb_free(')
pieces += between(layout, '#include "unicode_grapheme_data.h"', '/* A following variation selector')
pieces += line(layout, 'static float hext(') + line(layout, 'static float vext(')
pieces += between(layout, '/* ---------------------------------------------------------------- text geometry */',
                  '/* ---------------------------------------------------------------- floats */')
pieces += function(box, 'static void transform(') + function(box, 'static const char *ws_process(')
pieces += function(box, 'uint32_t *box_text_source_map(')
pieces += function(doc, 'static uint32_t utf16_decode(') + function(doc, 'uint32_t doc_text_utf16(')
pieces += function(doc, 'size_t doc_text_byte(')
pieces += function(paint, 'static size_t password_offset(')
pieces += between(paint, 'struct input_line {', 'static bool input_password(')
pieces += function(paint, 'static bool find_block_text(') + function(paint, 'static bool highlight(')
# js.c's Range walk. Element rectangles (collect_client_rects) and the script-facing append
# are recorded instead; the Text rectangles come from the real layout_text_rects above.
js = read('user/libc/web/js.c')
pieces += r'''
typedef struct JSContext JSContext;
typedef struct { void *unused; } JSValueConst;
#define JS_ThrowOutOfMemory(ctx) ((void)(ctx))
'''
pieces += between(js, 'struct client_rect_output {', 'static bool client_rect_append(')
pieces += r'''
static node_t *elements[16];
static double fragments[16][4];
static int element_count, fragment_count;
static bool collect_client_rects(struct client_rect_output *out, node_t *n, int sx, int sy) {
    (void)out; (void)sx; (void)sy;
    if (element_count < 16) elements[element_count] = n;
    element_count++;
    return true;
}
static bool client_rect_fragment(void *opaque, const box_t *space, double x, double y, double w, double h) {
    struct client_rect_output *out = opaque;
    out->space = space;
    if (fragment_count < 16) { fragments[fragment_count][0] = x; fragments[fragment_count][1] = y; fragments[fragment_count][2] = w; fragments[fragment_count][3] = h; }
    fragment_count++;
    return true;
}
'''
pieces += function(js, 'static int node_address_order(') + function(js, 'static node_t *range_after_subtree(')
pieces += line(js, 'static bool range_character_data(') + function(js, 'static node_t *range_boundary_node(')
pieces += function(js, 'static bool range_client_rects(')

prefix = r'''
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "webi.h"
static size_t code_points(const char *s, size_t n) {
    size_t count = 0;
    for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) count++;
    return count;
}
wfont style_font(const style_t *st) { return (wfont){NULL, st->font_size, false, NULL}; }
void wf_metrics(const wfont *f, float *ascent, float *descent) { *ascent = f->px * 0.8f; *descent = f->px * 0.2f; }
float wf_width(const wfont *f, const char *s, size_t n) { (void)f; return 10.0f * (float)code_points(s, n); }
float style_text_width(const style_t *st, const char *s, size_t n) { (void)st; return 10.0f * (float)code_points(s, n); }
float box_abs_x(const box_t *b) { return b->x; }
float box_abs_y(const box_t *b) { return b->y; }
float box_visual_x(const box_t *b) { return b->x; }
float box_visual_y(const box_t *b) { return b->y; }
bool web_marquee_box(const box_t *b) { (void)b; return false; }
bool box_element_scrollable(const box_t *b) { (void)b; return false; }
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
'''
c = out / 'text-geometry-native-generated.c'
c.write_text(prefix + pieces + read('tests/text_geometry_native_cases.c'), encoding='utf-8', newline='\n')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
abi = out / 'host-abi'
abi.mkdir(exist_ok=True)
(abi / 'setjmp.h').write_text('#include "' + (cc.parent.parent / 'include/setjmp.h').as_posix() + '"\n', encoding='utf-8', newline='\n')
exe = out / 'text-geometry-native.exe'
subprocess.run([str(cc), '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-I' + str(abi), '-Iuser/include', '-Icommon',
                '-Iuser/libc/web', str(c), '-o', str(exe)], cwd=root, env=env, check=True)
if os.environ.get('NOCTURNE_COMPILE_ONLY') != '1':
    subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=20)
