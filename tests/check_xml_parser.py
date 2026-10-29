"""Compile the exact XML parser (user/libc/web/xml.c) and Expat with host clang.

xml.c and Expat are the product sources; Expat keeps the product configuration
(ports/expat/expat_config.h, XML_NOCTURNE). The native tree, the arena, the
encoding labels, the checkpoint and the document's policy, console and script
hooks are small shims below, so this checks the parser's grammar, namespaces,
entity handling, accounting, cancellation, nesting and the browsing context's
resumable parse (script boundaries, slices, the error document), not Lexbor's
decoders, the DOM or script execution. It supplements guest tests and WPT; it
is not rendering evidence.
"""
from pathlib import Path
import hashlib
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/xml-parser-host"
out.mkdir(parents=True, exist_ok=True)
web = root / "user/libc/web"
source = (web / "xml.c").read_text(encoding="utf-8")
contract = (web / "xml.h").read_text(encoding="utf-8")
header = (web / "webi.h").read_text(encoding="utf-8")
dom = (web / "dom.c").read_text(encoding="utf-8")

def between(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]

prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>
#include <stddef.h>
#include <limits.h>
/* Every allocation of this translation unit (xml.c, which also serves Expat's
   memory suite, and the shims) is counted, can be refused by index, and can be
   capped by live bytes. */
typedef union { size_t size; max_align_t align; } host_block;
static size_t heap_live, heap_cap = SIZE_MAX;
static long allocations, fail_allocation = -1;
static void *host_malloc(size_t n) {
    if (allocations++ == fail_allocation || heap_live > heap_cap || n > heap_cap - heap_live) return NULL;
    host_block *b = (malloc)(sizeof *b + n);
    if (!b) return NULL;
    b->size = n; heap_live += n;
    return b + 1;
}
static void host_free(void *p) {
    if (!p) return;
    host_block *b = (host_block *)p - 1;
    heap_live -= b->size;
    (free)(b);
}
static void *host_realloc(void *p, size_t n) {
    if (!p) return host_malloc(n);
    host_block *b = (host_block *)p - 1;
    size_t old = b->size;
    if (allocations++ == fail_allocation || (n > old && (heap_live > heap_cap || n - old > heap_cap - heap_live))) return NULL;
    host_block *g = (realloc)(b, sizeof *g + n);
    if (!g) return NULL;
    heap_live = heap_live - old + n; g->size = n;
    return g + 1;
}
static char *host_strdup(const char *s) {
    size_t n = strlen(s) + 1; char *p = host_malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
#define malloc host_malloc
#define realloc host_realloc
#define free host_free
#define strdup host_strdup
/* Our libc's qsort_s is C11's (context last), unlike the host CRT's. */
static void host_qsort_s(void *base, size_t n, size_t size, int (*compare)(const void *, const void *, void *), void *context) {
    char *a = base, swap[64];
    if (size > sizeof swap) abort();
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && compare(a + (j - 1) * size, a + j * size, context) > 0; j--) {
            memcpy(swap, a + j * size, size); memcpy(a + j * size, a + (j - 1) * size, size); memcpy(a + (j - 1) * size, swap, size);
        }
}
#define qsort_s host_qsort_s
static inline int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* The arena: zeroed blocks freed together; a failure jumps to the trap. */
struct ablock { struct ablock *next; max_align_t data[]; };
typedef struct arena { struct ablock *head; size_t allocated, limit; jmp_buf *trap; } arena_t;
static void *ar_alloc(arena_t *a, size_t n) {
    struct ablock *b = n > SIZE_MAX - sizeof *b ? NULL : host_malloc(sizeof *b + n);
    if (!b) { if (a->trap) longjmp(*a->trap, 3); abort(); }
    memset(b->data, 0, n); b->next = a->head; a->head = b; a->allocated += n;
    return b->data;
}
static char *ar_strndup(arena_t *a, const char *s, size_t n) { char *p = ar_alloc(a, n + 1); memcpy(p, s, n); p[n] = 0; return p; }
static char *ar_strdup(arena_t *a, const char *s) { return ar_strndup(a, s, strlen(s)); }
static void ar_free(arena_t *a) { while (a->head) { struct ablock *b = a->head; a->head = b->next; host_free(b); } }
'''
types = between(header, "#define WEB_TAGS(X)", "int tag_lookup(")
types += between(header, "enum { N_DOC", "struct attr {")
types += between(header, "enum html_parse_result {", "enum html_scripting_mode {")
shim = r'''
typedef struct web_doc web_doc;
typedef struct node node_t;
struct attr { const char *name, *raw, *value, *namespace_uri, *prefix, *local; };
struct node {
    int type, namespace_id, tag, nattrs, elem_index, custom_registry_id;
    web_doc *owner; node_t *parent, *prev, *next, *first, *last, *template_content;
    bool cdata, script_started, html_policy_processed, value_dirty, control_ready;
    const char *name, *raw_name, *prefix, *local_name, *namespace_uri, *custom_is, *public_id, *system_id;
    char *text; size_t textlen; struct attr *attrs; void *custom_registry_realm;
};
struct web_doc {
    arena_t mem; size_t control_bytes; node_t *root; web_doc *template_doc;
    int custom_registry_id, mutated, synced; void *custom_registry_realm;
    char encoding[32]; bool encoding_certain, xml, xml_interface, resources_dirty;
    char *content_type;
    /* What the browsing context's hooks saw. */
    int policies, referrers, details, captures, removals, console_lines;
    char policy[128], console[256];
};
static long checkpoints, cancel_at = -1;
static bool web_native_checkpoint(web_doc *d) { (void)d; return checkpoints++ != cancel_at; }
static void doc_mutated(web_doc *d, node_t *n) { (void)n; d->mutated++; }
static void doc_sync_tree(web_doc *d) { d->synced++; }
static uint64_t host_now;
static uint64_t uptime_ms(void) { return host_now; }
static bool str_ieq(const char *a, const char *b) {
    while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return !*a && !*b;
}
static const char *node_attr(const node_t *n, const char *name) {
    for (int i = 0; i < n->nattrs; i++) if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}
static bool policy_fails;
static bool html_policy_meta(web_doc *d, const char *content, size_t length) {
    if (policy_fails) return false;
    d->policies++; snprintf(d->policy, sizeof d->policy, "%.*s", (int)length, content);
    return true;
}
static void doc_referrer_meta(web_doc *d, node_t *n) { (void)n; d->referrers++; }
static void doc_details_parser_finish(node_t *root) { root->owner->details++; }
static bool capture_fails;
static bool web_js_script_parser_capture(web_doc *d, node_t *n) { (void)n; d->captures++; return !capture_fails; }
static void web_js_console(web_doc *d, int level, const char *message) {
    (void)level; d->console_lines++; snprintf(d->console, sizeof d->console, "%s", message);
}
static void doc_node_remove(web_doc *d, node_t *n) {
    node_t *p = n->parent;
    if (n->prev) n->prev->next = n->next; else p->first = n->next;
    if (n->next) n->next->prev = n->prev; else p->last = n->prev;
    n->parent = n->prev = n->next = NULL;
    d->removals++;
}
static void *doc_alloc(web_doc *d, size_t n) {
    jmp_buf trap, *old = d->mem.trap;
    d->mem.trap = &trap;
    if (setjmp(trap)) { d->mem.trap = old; return NULL; }
    void *p = ar_alloc(&d->mem, n);
    d->mem.trap = old;
    return p;
}
static char *doc_strndup(web_doc *d, const char *s, size_t n) { char *p = doc_alloc(d, n + 1); if (p) { memcpy(p, s, n); p[n] = 0; } return p; }
static node_t *doc_node_create(web_doc *d, int type, const char *name, const char *text, size_t len) {
    node_t *n = doc_alloc(d, sizeof *n);
    if (!n) return NULL;
    n->type = type; n->owner = d; n->custom_registry_id = -2;
    if (name && !(n->name = doc_strndup(d, name, strlen(name)))) return NULL;
    if ((type == N_TEXT || type == N_COMMENT || type == N_PI) && !(n->text = doc_strndup(d, text ? text : "", len))) return NULL;
    n->textlen = len;
    return n;
}
'''
namespace_id = between(dom, "int doc_namespace_id(", "const char *doc_element_namespace(")
names = between(dom, "static bool xml_name_start(", "void doc_create_diagnostic(")
shim2 = r'''
static node_t *doc_element_create(web_doc *d, const char *ns, const char *prefix, const char *local) {
    node_t *n = doc_node_create(d, N_ELEM, local, NULL, 0);
    if (!n) return NULL;
    n->namespace_id = doc_namespace_id(ns);
    if (n->namespace_id == NS_OTHER && !(n->namespace_uri = doc_strndup(d, ns, strlen(ns)))) return NULL;
    if (prefix && !(n->prefix = doc_strndup(d, prefix, strlen(prefix)))) return NULL;
    n->local_name = n->name;
    /* As tag_lookup: the tag follows the local name in any namespace. */
    static const struct { const char *name; int tag; } tags[] = {
        {"script", T_script}, {"template", T_template}, {"meta", T_meta}, {"head", T_head},
        {"details", T_details}, {"textarea", T_textarea},
    };
    n->tag = T_UNKNOWN;
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) if (!strcmp(local, tags[i].name)) n->tag = tags[i].tag;
    return n;
}
static node_t *doc_doctype_create(web_doc *d, const char *name, size_t nl, const char *pub, size_t pl, const char *sys, size_t sl) {
    node_t *n = doc_node_create(d, N_DOCTYPE, NULL, NULL, 0);
    if (!n || !(n->name = doc_strndup(d, name, nl)) || !(n->public_id = doc_strndup(d, pub, pl)) || !(n->system_id = doc_strndup(d, sys, sl))) return NULL;
    return n;
}
static void doc_attrs_initial(web_doc *d, node_t *n, struct attr *attrs, int count) { (void)d; n->attrs = attrs; n->nattrs = count; }
/* stylesheet blocking belongs to doc.c, which this check does not build */
static void doc_parser_element_created(node_t *n) { (void)n; }
static web_doc *shim_doc(void) {
    web_doc *d = host_malloc(sizeof *d);
    if (!d) return NULL;
    memset(d, 0, sizeof *d); d->custom_registry_id = -1;
    return d;
}
static node_t *doc_template_content(web_doc *d, node_t *element) {
    if (!d->template_doc && !(d->template_doc = shim_doc())) return NULL;
    node_t *f = doc_node_create(d->template_doc, N_FRAGMENT, NULL, NULL, 0);
    if (f) element->template_content = f;
    return f;
}
static web_doc *doc_inert_new(web_doc *family, const char *url) { (void)family; (void)url; return shim_doc(); }
static void doc_inert_discard(web_doc *d) {
    if (!d) return;
    if (d->template_doc) doc_inert_discard(d->template_doc);
    ar_free(&d->mem); host_free(d->content_type); host_free(d);
}
/* Encoding labels: the few the cases use (Lexbor's tables are not this test). */
static bool html_encoding_label(const char *label, size_t length, char *canonical, size_t capacity) {
    static const char *const table[][2] = {
        {"utf-8", "UTF-8"}, {"utf8", "UTF-8"}, {"utf-16", "UTF-16LE"}, {"utf-16le", "UTF-16LE"},
        {"utf-16be", "UTF-16BE"}, {"windows-1252", "windows-1252"}, {"latin1", "windows-1252"},
        {"iso-8859-1", "windows-1252"}, {"us-ascii", "windows-1252"},
    };
    while (length && strchr(" \t\n\f\r", *label)) { label++; length--; }
    while (length && strchr(" \t\n\f\r", label[length - 1])) length--;
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
        size_t n = strlen(table[i][0]);
        if (n != length) continue;
        size_t j = 0;
        while (j < n && lower((unsigned char)label[j]) == table[i][0][j]) j++;
        if (j == n) { snprintf(canonical, capacity, "%s", table[i][1]); return true; }
    }
    return false;
}
/* windows-1252 only. */
static char *html_decode_bytes(web_doc *d, const char *src, size_t n, const char *label, size_t *length) {
    static const uint16_t high[32] = {0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,
                                      0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
    if (strcmp(label, "windows-1252") || n > SIZE_MAX / 3 - 1) return NULL;
    char *out = host_malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = (unsigned char)src[i];
        if (c >= 0x80 && c < 0xa0) c = high[c - 0x80];
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) { out[k++] = (char)(0xc0 | c >> 6); out[k++] = (char)(0x80 | (c & 63)); }
        else { out[k++] = (char)(0xe0 | c >> 12); out[k++] = (char)(0x80 | (c >> 6 & 63)); out[k++] = (char)(0x80 | (c & 63)); }
    }
    out[k] = 0; *length = k;
    snprintf(d->encoding, sizeof d->encoding, "windows-1252");
    return out;
}
'''
body = source.replace('#include "xml.h"', contract.replace('#include "webi.h"', ''))
for include in ('#include "html_encoding.h"', '#include "html_policy.h"', '#include "elements.h"', '#include <nocturne.h>'):
    body = body.replace(include, '')
cases = (root / "tests/xml_parser_host_cases.c").read_text(encoding="utf-8")
generated = out / "xml_parser.c"
generated.write_text(prefix + types + shim + namespace_id + names + shim2 + body + cases, encoding="utf-8", newline="\n")
# Expat on the host takes its product branch: XML_NOCTURNE, whose only system
# need is n_random and uptime_ms, after the CRT headers have read _WIN32.
(out / "nocturne.h").write_text(
    "#include <stdlib.h>\n#include <stdint.h>\n"
    "static inline int n_random(void *b, size_t n) { unsigned char *p = b; for (size_t i = 0; i < n; i++) p[i] = (unsigned char)rand(); return 0; }\n"
    "static inline uint64_t uptime_ms(void) { return 0; }\n", encoding="utf-8", newline="\n")
(out / "prelude.h").write_text(
    "".join("#include <%s>\n" % h for h in ("stdbool.h", "stddef.h", "string.h", "assert.h", "limits.h", "stdio.h",
                                           "stdlib.h", "stdint.h", "math.h", "errno.h", "stdarg.h", "float.h")) +
    "#undef _WIN32\n#undef _WIN64\n", encoding="utf-8", newline="\n")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
exe = out / "xml_parser.exe"
log = out / "result.log"
includes = ["-I" + str(out), "-I" + str(root / "ports/expat"), "-I" + str(root / "third_party/expat/lib"), "-I" + str(web)]
with log.open("w", encoding="utf-8") as stream:
    for path in ("user/libc/web/xml.c", "user/libc/web/xml.h", "tests/xml_parser_host_cases.c"):
        stream.write(path + " SHA256 " + hashlib.sha256((root / path).read_bytes()).hexdigest() + "\n")
    stream.flush()
    objects = []
    for name in ("xmlparse", "xmlrole", "xmltok"):
        obj = out / (name + ".o")
        subprocess.run([str(cc), "-std=c11", "-w", "-O1", "-include", str(out / "prelude.h"), *includes, "-c",
                        str(root / "third_party/expat/lib" / (name + ".c")), "-o", str(obj)],
                       cwd=root, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)
        objects.append(str(obj))
    subprocess.run([str(cc), "-std=c11", "-Wall", "-Wextra", "-O1", *includes, str(generated), *objects, "-o", str(exe),
                    "-Wl,--stack,1048576"],
                   cwd=root, env=env, check=True, stdout=stream, stderr=subprocess.STDOUT)
    result = subprocess.run([str(exe)], cwd=root, env=env, timeout=300, stdout=stream, stderr=subprocess.STDOUT)
print(log.read_text(encoding="utf-8"))
raise SystemExit(result.returncode)
