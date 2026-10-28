"""The product CSSOM View geometry, compiled from its sources: js_geometry_native.h
(GeometryUtils' complete transform, box offsets, documents check, getBoxQuads fragments and
coordinate conversion, checkVisibility), layout.c's transform chain and js.c's scrollParent.
Box placement, transforms, line fragments, viewport scroll positions and frame origins are
stubs set by each case; the cases are tests/cssom_view_native_cases.c."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / 'build/cssom-view-native'
out.mkdir(parents=True, exist_ok=True)


def read(name):
    return (root / name).read_text(encoding='utf-8')


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2] + '\n'


def between(text, first, stop):
    start = text.index(first)
    return text[start:text.index(stop, start)]


layout, js, native = read('user/libc/web/layout.c'), read('user/libc/web/js.c'), read('user/libc/web/js_geometry_native.h')
pieces = function(layout, 'void box_transform_chain(') + function(layout, 'void box_transform_rect(')
pieces += function(js, 'static box_t *geometry_principal_box(')
pieces += function(js, 'static bool element_viewport(')
pieces += function(js, 'static node_t *scrolling_element(') + function(js, 'static node_t *scroll_parent(')
pieces += between(native, 'enum geometry_box {', 'static JSValue geometry_numbers(')
pieces += function(native, 'static bool geometry_check_visibility(')

prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>
#include "webi.h"
/* js.c's realm, reduced to what the geometry reads: each document's state and scroll position. */
struct web_js_state { web_doc *doc; int sx, sy; bool foreign; };
static struct web_js_state *state_of(web_doc *d) { return d ? d->js : NULL; }
static struct web_js_state *geometry_owner(struct web_js_state *s, web_doc *d) {
    (void)s; struct web_js_state *o = state_of(d);
    return d && d->live && o && !o->foreign ? o : NULL;
}
static bool connected(struct web_js_state *s, node_t *n) { (void)s; return n && n->owner; }
static void flush_layout(struct web_js_state *s) { (void)s; }
static void viewport_scroll_position(struct web_js_state *s, int *x, int *y) { *x = s->sx; *y = s->sy; }
bool web_frame_same_origin(web_doc *a, web_doc *b) { return !(state_of(a)->foreign ^ state_of(b)->foreign); }
node_t *doc_flat_parent(node_t *n) { return n ? n->parent : NULL; }
static bool geometry_closed_shadow_hidden(node_t *candidate, node_t *reference) { (void)candidate; (void)reference; return false; }
/* Boxes sit at x, y (their content edge, as box_visual_x/y report); transforms come from a
   per-box table. */
float box_visual_x(const box_t *b) { return b->x; }
float box_visual_y(const box_t *b) { return b->y; }
static const box_t *transformed[8]; static double matrices[8][6]; static int transform_count;
bool box_transform_matrix(const box_t *b, double m[6]) {
    for (int i = 0; i < transform_count; i++) if (transformed[i] == b) { memcpy(m, matrices[i], sizeof matrices[i]); return true; }
    return false;
}
bool box_element_scrollable(const box_t *b) { return b && b->st && (b->st->overflow == OV_AUTO || b->st->overflow == OV_SCROLL || b->st->overflow == OV_HIDDEN) && b->node && b->node->box == b; }
bool box_viewport_fixed(const box_t *b) { return b && b->st && b->st->position == POS_FIXED && (!b->cb || !b->cb->parent); }
/* line fragments: one table for inline elements, one for Text */
static node_t *fragment_node[8]; static const box_t *fragment_space[8]; static double fragment_rect[8][4]; static int fragment_count;
bool layout_inline_rects(node_t *n, bool visual, layout_rect_fn visit, void *opaque) {
    (void)visual;
    for (int i = 0; i < fragment_count; i++) if (fragment_node[i] == n &&
        !visit(opaque, fragment_space[i], fragment_rect[i][0], fragment_rect[i][1], fragment_rect[i][2], fragment_rect[i][3])) return false;
    return true;
}
bool layout_text_rects(node_t *n, uint32_t start, uint32_t end, bool visual, layout_rect_fn visit, void *opaque) {
    (void)start; (void)end;
    return layout_inline_rects(n, visual, visit, opaque);
}
'''
c = out / 'cssom-view-native-generated.c'
c.write_text(prefix + pieces + read('tests/cssom_view_native_cases.c'), encoding='utf-8', newline='\n')
cc = root / 'tools/msys64/ucrt64/bin/clang.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
abi = out / 'host-abi'
abi.mkdir(exist_ok=True)
(abi / 'setjmp.h').write_text('#include "' + (cc.parent.parent / 'include/setjmp.h').as_posix() + '"\n', encoding='utf-8', newline='\n')
exe = out / 'cssom-view-native.exe'
subprocess.run([str(cc), '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-I' + str(abi), '-Iuser/include', '-Icommon',
                '-Iuser/libc/web', str(c), '-o', str(exe)], cwd=root, env=env, check=True)
if os.environ.get('NOCTURNE_COMPILE_ONLY') != '1':
    subprocess.run([str(exe)], cwd=root, env=env, check=True, timeout=20)
