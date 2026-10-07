/* QuickJS browser host. Nocturne DOM nodes are the only native objects exposed
   to scripts; there is deliberately no filesystem, process or native module API. */
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <nocturne.h>
#include <http.h>
#include <webnet.h>
#include <quickjs.h>
#include "webi.h"
#include "js_canvas.h"
#include "avmedia.h"
#include "form_value.h"
#include "form_validation.h"
#include "elements.h"
#include "js_crypto.h"
#include "js_collator_native.h"
#include "js_bootstrap.inc"

/* Live YouTube's 10.8 MB program was rejected while compiling at exactly the
   old 64 MiB runtime quota, not by Nocturne's allocator (245228 KiB VM free).
   Keep a finite per-document bound while allowing that normal compilation. */
#define JS_HEAP_LIMIT (128u * 1024u * 1024u)
#define JS_STACK_LIMIT (512u * 1024u)
#define JS_BODY_LIMIT (16u * 1024u * 1024u)
/* Execution, microtasks and synchronous DOM/layout share one watchdog. Source
   preparation through COMPILE_ONLY has a separate, cumulative task allowance:
   live YouTube's finite 10.8 MB compilation takes over 10 seconds under QEMU.
   Neither nested callbacks nor repeated compilations reset either allowance.
   JavaScript eval()/Function() remain inside the execution budget. */
#define JS_TASK_MS 5000u
#define JS_COMPILE_MS 30000u
#define JS_STARTUP_MS 10000u /* trusted, built-in platform initialization only */
#define JS_TIMERS 128
#define JS_REQUESTS 64

#define JS_NODE_BUCKETS 4096
struct js_node_ref { node_t *node; JSValue object; bool force_async_set, force_async, image_notified; int image; uint64_t image_generation; struct js_node_ref *next, *hash_next; };
struct js_timer { uint32_t id; int kind; uint64_t due, interval; JSValue fn, args; };
struct js_posted_task { struct js_posted_task *next; JSValue fn; uint32_t id; };
struct js_script {
    node_t *node;
    char *url, *base, *source;
    size_t len;
    bool module, deferred, asynchronous, dynamic, ready, failed, executed, notified;
    JSValue evaluation;
    uint64_t request;
    struct js_script *next;
};
enum { P_SCRIPT, P_FETCH, P_CSS, P_IMAGE };
struct js_pending {
    uint64_t id, deadline;
    int kind, credentials, cache_mode;
    int image;
    struct js_script *script;
    JSValue resolve, reject;
    char *url;
    bool done, aborted, force_preflight, redirect_error, same_origin;
    struct web_response response;
    struct js_pending *next;
};
struct js_module { char *url, *base, *source; size_t len; JSValue compiled, error; struct js_module *next; };
struct js_rejection { JSValue promise, reason; bool used; };
struct js_resource_event { node_t *node; bool failed, image, selection; uint64_t generation; struct js_resource_event *next; };
struct js_image_decode { node_t *node; uint64_t generation; JSValue resolve, reject; struct js_image_decode *next; };
/* Order is shared with the bootstrap's private nodeProtos array. */
enum { NP_NODE, NP_DOCUMENT, NP_ELEMENT, NP_HTML, NP_TEXT, NP_COMMENT, NP_FRAGMENT, NP_IFRAME, NP_IMAGE,
       NP_INPUT, NP_BUTTON, NP_SELECT, NP_TEXTAREA, NP_FIELDSET, NP_OBJECT, NP_OUTPUT, NP_OPTION, NP_TEMPLATE, NP_DOCTYPE,
       NP_SCRIPT, NP_FORM, NP_ANCHOR, NP_AREA, NP_SVG, NP_SVGSVG, NP_PI, NP_ATTR,
       NP_META, NP_LINK, NP_STYLE, NP_BASE, NP_TITLE, NP_HEAD, NP_SHADOW, NP_SLOT,
       NP_TIME, NP_DATA, NP_DETAILS, NP_OL, NP_LI, NP_UNKNOWN, NP_CANVAS,
       NP_AVMEDIA, NP_AUDIO, NP_VIDEO, NP_COUNT };
struct js_alloc_diagnostics { size_t peak, requested, used, limit; unsigned failures, reported; bool quota; };
struct web_js_state {
    struct js_alloc_diagnostics allocation;
    web_doc *doc;
    struct web_host host;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue hooks, dispatch, response, reject, node_protos[NP_COUNT];
    struct js_node_ref *nodes;
    struct js_node_ref *node_buckets[JS_NODE_BUCKETS];
    struct js_script *scripts, *last_script, *blocker;
    struct js_pending *pending;
    struct js_module *modules;
    const char *module_entry_url;
    struct js_timer timers[JS_TIMERS];
    struct js_posted_task *posted, *last_posted;
    unsigned posted_count;
    uint32_t next_posted;
    bool posted_turn;
    struct js_rejection rejections[32];
    struct js_resource_event *events, *last_event;
    struct js_image_decode *image_decodes, *last_image_decode;
    uint64_t next_request, task_deadline, now;
    uint64_t task_layout_ms, task_compile_ms, compile_wait_ms, task_microtask_ms;
    int compiling;
    bool compile_timed_out;
    unsigned task_layout_flushes;
    unsigned profile_dom_depth;
    bool observers_active, observers_force;
    uint64_t observer_due, observer_layout, observer_dom;
    int observer_x, observer_y, observer_w, observer_h;
    uint32_t next_timer;
    int pending_count, running;
    int script_count;
    int media_width, media_height;
    node_t *current_script;
    bool parsing_done, domcontent_sent, load_sent, disabled, timed_out, task_timed_out, parser_write, starting;
};
static JSClassID node_class;
/* This cache contains only bytecode compiled from the checked-in browser
   bindings. Never read page/network bytecode through JS_ReadObject. It avoids
   recompiling the platform implementation for every document in the process. */
static uint8_t *bindings_bytecode;
static size_t bindings_bytecode_len;
static struct js_script *queue_script(struct web_js_state *s, node_t *n, bool dynamic);
static void dynamic_scripts(struct web_js_state *s, node_t *n);
static void run_script(struct web_js_state *s, struct js_script *script);
static struct js_resource_event *script_event(struct web_js_state *s, node_t *n, bool failed);
static JSValue custom_element_hook(struct web_js_state *s, const char *name, int argc, JSValueConst *argv);
static void exception(struct web_js_state *s);
static void signal_slots(struct web_js_state *s) {
    web_doc *family = s->doc->dom_family ? s->doc->dom_family : s->doc;
    if (!s->disabled && family->shadow_slots_pending) {
        JSValue value = custom_element_hook(s, "slotChanges", 0, NULL);
        if (JS_IsException(value)) exception(s); else JS_FreeValue(s->ctx, value);
    }
}

/* Alignment and accounting include the allocation header, not an invented
   malloc_usable_size. This also handles realloc failure without losing state. */
union alloc_header { size_t size; long double align; };
static void allocation_failed(JSMallocState *s, size_t requested, bool quota) {
    struct js_alloc_diagnostics *d=s->opaque;if(!d)return;
    d->requested=requested;d->used=s->malloc_size;d->limit=s->malloc_limit;d->quota=quota;d->failures++;
}
static void allocation_peak(JSMallocState *s) {
    struct js_alloc_diagnostics *d=s->opaque;if(d && s->malloc_size>d->peak)d->peak=s->malloc_size;
}
static void *qmalloc(JSMallocState *s, size_t n) {
    if (!n || n > SIZE_MAX - sizeof(union alloc_header)) return NULL;
    size_t total = n + sizeof(union alloc_header);
    if (s->malloc_size > s->malloc_limit || total > s->malloc_limit - s->malloc_size) { allocation_failed(s,total,true);return NULL; }
    union alloc_header *h = malloc(total);
    if (!h) { allocation_failed(s,total,false);return NULL; }
    h->size = n; s->malloc_count++; s->malloc_size += total;
    allocation_peak(s);
    return h + 1;
}
static void qfree(JSMallocState *s, void *p) {
    if (!p) return;
    union alloc_header *h = (union alloc_header *)p - 1;
    s->malloc_count--; s->malloc_size -= h->size + sizeof *h; free(h);
}
static void *qrealloc(JSMallocState *s, void *p, size_t n) {
    if (!p) return qmalloc(s, n);
    if (!n) { qfree(s, p); return NULL; }
    if (n > SIZE_MAX - sizeof(union alloc_header)) return NULL;
    union alloc_header *h = (union alloc_header *)p - 1;
    size_t old = h->size, total = n + sizeof *h;
    if (n > old && (s->malloc_size > s->malloc_limit || n - old > s->malloc_limit - s->malloc_size)) { allocation_failed(s,total,true);return NULL; }
    if (n == old) return p;
    /* Nocturne's realloc intentionally keeps the entire adjacent free block
       when growing in place, and retains capacity when shrinking. QuickJS
       frequently resizes many small tables: using that policy here makes
       their physical footprint diverge from the runtime's exact quota.
       Fit each engine allocation to its requested size without changing the
       OS allocator or ABI; the finite runtime quota remains enforced here. */
    union alloc_header *next = malloc(total);
    if (!next) { allocation_failed(s,total,false);return NULL; }
    memcpy(next + 1, p, MIN(old, n)); free(h);
    next->size = n; s->malloc_size = s->malloc_size - old + n;
    allocation_peak(s);
    return next + 1;
}
static size_t qusable(const void *p) { return p ? ((const union alloc_header *)p - 1)->size : 0; }
static const JSMallocFunctions allocator = {qmalloc, qfree, qrealloc, qusable};

static struct web_js_state *state(JSContext *ctx) { return JS_GetContextOpaque(ctx); }
static void log_text(struct web_js_state *s, int level, const char *msg) {
    if (s->host.console) s->host.console(s->host.opaque, level, msg ? msg : "JavaScript error");
}
void web_js_console(web_doc *d, int level, const char *message) {
    /* Native resource/layout failures remain observable after scripting stops. */
    if (d && d->js) log_text(d->js, level, message);
}
static void report_allocation_failure(struct web_js_state *s) {
    if(s->allocation.failures!=s->allocation.reported) {
        const struct js_alloc_diagnostics *d=&s->allocation;char message[256];
        snprintf(message,sizeof message,"JavaScript allocation failure: %s; requested %lu, charged %lu, limit %lu, peak %lu bytes",
            d->quota?"runtime quota":"Nocturne allocator",(unsigned long)d->requested,(unsigned long)d->used,(unsigned long)d->limit,(unsigned long)d->peak);
        s->allocation.reported=d->failures;log_text(s,0,message);
        JSMemoryUsage usage; JS_ComputeMemoryUsage(s->rt, &usage);
        snprintf(message,sizeof message,"JavaScript heap: %ld allocations; %ld objects, %ld properties; strings %ld, functions %ld, bytecode %ld bytes",
            (long)usage.malloc_count,(long)usage.obj_count,(long)usage.prop_count,
            (long)usage.str_size,(long)usage.js_func_size,(long)usage.js_func_code_size);
        log_text(s,0,message);
    }
}
static void exception(struct web_js_state *s) {
    report_allocation_failure(s);
    bool temporary = !s->running;
    if (temporary) { JS_UpdateStackTop(s->rt); s->task_deadline = uptime_ms() + JS_TASK_MS; s->running = 1; }
    JSValue e = JS_GetException(s->ctx);
    JSValue stack = JS_IsObject(e) ? JS_GetPropertyStr(s->ctx, e, "stack") : JS_UNDEFINED;
    const char *a = JS_ToCString(s->ctx, e), *b = JS_IsUndefined(stack) ? NULL : JS_ToCString(s->ctx, stack);
    sbuf text = {0}; sb_puts(&text, a ? a : "JavaScript exception");
    if (b && *b && (!a || strcmp(a, b))) { sb_putc(&text, '\n'); sb_puts(&text, b); }
    log_text(s, 2, sb_cstr(&text)); sb_free(&text);
    JS_FreeCString(s->ctx, a); JS_FreeCString(s->ctx, b);
    JS_FreeValue(s->ctx, stack); JS_FreeValue(s->ctx, e);
    if (temporary) s->running = 0;
}
static int interrupt(JSRuntime *rt, void *opaque) {
    struct web_js_state *s = opaque;
    if (s->disabled) return 1;
    if (s->running && uptime_ms() >= s->task_deadline) { s->timed_out = true; s->task_timed_out = true; s->disabled = true; return 1; }
    return 0;
}
static void begin_task(struct web_js_state *s) {
    if (!s->running) {
        JS_UpdateStackTop(s->rt); s->task_deadline = uptime_ms() + JS_TASK_MS;
        s->task_layout_ms = 0; s->task_layout_flushes = 0;
        s->task_compile_ms = 0; s->compile_timed_out = false;
        s->task_microtask_ms = 0; s->task_timed_out = false;
    }
    s->running++;
}
static void end_task(struct web_js_state *s) {
    if (s->running == 1 && !s->disabled) {
        signal_slots(s);
        JSContext *ctx;
        uint64_t microtask_start = uptime_ms();
        while (JS_IsJobPending(s->rt)) {
            if (interrupt(s->rt, s)) break;
            int r = JS_ExecutePendingJob(s->rt, &ctx);
            if (r < 0) exception(s);
            if (r == 0) break;
        }
        s->task_microtask_ms += uptime_ms() - microtask_start;
    }
    if (s->running == 1) {
        /* Module evaluation failures can be promise rejections, not exceptions
           returned to C. Keep the native allocation cause visible there too. */
        report_allocation_failure(s);
        for (int i = 0; i < 32; i++) if (s->rejections[i].used) {
            const char *p = JS_ToCString(s->ctx, s->rejections[i].reason);
            sbuf b = {0}; sb_puts(&b, "Unhandled promise rejection: ");
            sb_puts(&b, p ? p : "JavaScript exception");
            /* Module failures are rejected promises. Preserve their engine
               backtrace too, without invoking a page-defined stack getter or
               a Proxy trap while diagnosing an interrupted task. */
            if (JS_IsError(s->ctx, s->rejections[i].reason)) {
                JSAtom atom = JS_NewAtom(s->ctx, "stack");
                JSPropertyDescriptor desc;
                if (atom && JS_GetOwnProperty(s->ctx, &desc, s->rejections[i].reason, atom) > 0) {
                    if (JS_IsString(desc.value)) {
                        const char *stack = JS_ToCString(s->ctx, desc.value);
                        if (stack && *stack) { sb_putc(&b, '\n'); sb_puts(&b, stack); }
                        JS_FreeCString(s->ctx, stack);
                    }
                    JS_FreeValue(s->ctx, desc.value); JS_FreeValue(s->ctx, desc.getter); JS_FreeValue(s->ctx, desc.setter);
                }
                JS_FreeAtom(s->ctx, atom);
            }
            log_text(s, 2, sb_cstr(&b)); sb_free(&b);
            JS_FreeCString(s->ctx, p);
            JS_FreeValue(s->ctx, s->rejections[i].promise); JS_FreeValue(s->ctx, s->rejections[i].reason);
            s->rejections[i].used = false;
        }
        if (s->timed_out) {
            log_text(s, 2, s->compile_timed_out ? "JavaScript stopped: source preparation exceeded its 30 second task budget." :
                     s->starting ? "Browser API initialization exceeded its 10 second startup budget." :
                     "JavaScript stopped: the task exceeded its 5 second execution budget.");
            s->timed_out = false;
        }
        if (s->doc->resources_dirty) doc_rescan(s->doc);
    }
    if (s->running) s->running--;
}
static void promise_rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                              JS_BOOL handled, void *opaque) {
    struct web_js_state *s = opaque;
    for (int i = 0; i < 32; i++) if (s->rejections[i].used && JS_StrictEq(ctx, s->rejections[i].promise, promise)) {
        if (handled) { JS_FreeValue(ctx, s->rejections[i].promise); JS_FreeValue(ctx, s->rejections[i].reason); s->rejections[i].used = false; }
        return;
    }
    if (handled) return;
    for (int i = 0; i < 32; i++) if (!s->rejections[i].used) {
        s->rejections[i].promise = JS_DupValue(ctx, promise); s->rejections[i].reason = JS_DupValue(ctx, reason); s->rejections[i].used = true; return;
    }
}
static node_t *unwrap(JSContext *ctx, JSValueConst v) {
    if (JS_IsNull(v) || JS_IsUndefined(v)) return NULL;
    return JS_GetOpaque2(ctx, v, node_class);
}
static bool unknown_html_interface(const node_t *n) {
    const char *name = n->name;
    static const char *obsolete[] = {"applet","bgsound","blink","isindex","keygen","multicol","nextid","spacer"};
    for (size_t i=0;i<sizeof obsolete/sizeof *obsolete;i++) if (!strcmp(name,obsolete[i])) return true;
    if (n->tag && n->tag != T_image && n->tag != T_math && n->tag != T_svg) return false;
    static const char *generic[] = {"acronym","basefont","rb","rtc"};
    for (size_t i=0;i<sizeof generic/sizeof *generic;i++) if (!strcmp(name,generic[i])) return false;
    /* Keep potential autonomous custom elements on HTMLElement. This matches
       the current valid-local-name rule used by js_custom_elements.js. */
    if (name[0] < 'a' || name[0] > 'z' || !strchr(name,'-')) return true;
    static const char *reserved[] = {"annotation-xml","color-profile","font-face","font-face-src",
        "font-face-uri","font-face-format","font-face-name","missing-glyph"};
    for (size_t i=0;i<sizeof reserved/sizeof *reserved;i++) if (!strcmp(name,reserved[i])) return true;
    for (const unsigned char *p=(const unsigned char *)name;*p;p++)
        if (is_space(*p) || *p=='/' || *p=='>' || (*p>='A' && *p<='Z')) return true;
    return false;
}
static JSValueConst node_prototype(struct web_js_state *s, const node_t *n) {
    int kind = n->type == N_DOC ? NP_DOCUMENT : n->type == N_ELEM ?
               (n->namespace_id == NS_SVG ? (!strcmp(n->raw_name, "svg") ? NP_SVGSVG : NP_SVG) :
                n->foreign ? NP_ELEMENT : n->tag == T_iframe ? NP_IFRAME : NP_HTML) :
               n->type == N_TEXT ? NP_TEXT : n->type == N_COMMENT ? NP_COMMENT : n->type == N_DOCTYPE ? NP_DOCTYPE : n->type == N_PI ? NP_PI : n->type == N_ATTR ? NP_ATTR : n->shadow_host ? NP_SHADOW : NP_FRAGMENT;
    if (n->type == N_ELEM && !n->foreign) switch (n->tag) {
        case T_img: kind = NP_IMAGE; break;
        case T_input: kind = NP_INPUT; break;
        case T_button: kind = NP_BUTTON; break;
        case T_select: kind = NP_SELECT; break;
        case T_textarea: kind = NP_TEXTAREA; break;
        case T_fieldset: kind = NP_FIELDSET; break;
        case T_object: kind = NP_OBJECT; break;
        case T_output: kind = NP_OUTPUT; break;
        case T_option: kind = NP_OPTION; break;
        case T_template: kind = NP_TEMPLATE; break;
        case T_script: kind = NP_SCRIPT; break;
        case T_form: kind = NP_FORM; break;
        case T_a: kind = NP_ANCHOR; break;
        case T_area: kind = NP_AREA; break;
        case T_meta: kind = NP_META; break;
        case T_link: kind = NP_LINK; break;
        case T_style: kind = NP_STYLE; break;
        case T_base: kind = NP_BASE; break;
        case T_title: kind = NP_TITLE; break;
        case T_head: kind = NP_HEAD; break;
        case T_slot: kind = NP_SLOT; break;
        case T_time: kind = NP_TIME; break;
        case T_data: kind = NP_DATA; break;
        case T_details: kind = NP_DETAILS; break;
        case T_ol: kind = NP_OL; break;
        case T_li: kind = NP_LI; break;
        case T_canvas: kind = NP_CANVAS; break;
        case T_audio: kind = NP_AUDIO; break;
        case T_video: kind = NP_VIDEO; break;
    }
    if (kind == NP_HTML && unknown_html_interface(n)) kind = NP_UNKNOWN;
    return s->node_protos[kind];
}
static unsigned node_bucket(const node_t *n) {
    uintptr_t key = (uintptr_t)n >> 4;
    key ^= key >> 17; key *= UINT64_C(0x9e3779b97f4a7c15);
    return (unsigned)(key >> 32) & (JS_NODE_BUCKETS - 1);
}
static struct js_node_ref *node_ref(struct web_js_state *s, const node_t *n) {
    for (struct js_node_ref *r = s->node_buckets[node_bucket(n)]; r; r = r->hash_next)
        if (r->node == n) return r;
    return NULL;
}
static JSValue wrap(struct web_js_state *s, node_t *n) {
    if (!n) return JS_NULL;
    struct js_node_ref *cached = node_ref(s, n);
    if (cached) return JS_DupValue(s->ctx, cached->object);
    struct js_node_ref *r = js_mallocz(s->ctx, sizeof *r);
    if (!r) return JS_EXCEPTION;
    /* The document wrapper exists before bootstrap. All later wrappers use the
       actual interface prototype without changing their native opaque brand. */
    r->object = JS_IsUndefined(s->node_protos[NP_NODE]) ? JS_NewObjectClass(s->ctx, node_class) :
                JS_NewObjectProtoClass(s->ctx, node_prototype(s, n), node_class);
    if (JS_IsException(r->object)) { js_free(s->ctx, r); return JS_EXCEPTION; }
    JS_SetOpaque(r->object, n); r->node = n; r->image = -2; r->next = s->nodes; s->nodes = r;
    unsigned bucket = node_bucket(n); r->hash_next = s->node_buckets[bucket]; s->node_buckets[bucket] = r;
    return JS_DupValue(s->ctx, r->object);
}
static JSValue oom(JSContext *ctx) { return JS_ThrowOutOfMemory(ctx); }
static JSValue str_or_null(JSContext *ctx, const char *p) { return p ? JS_NewString(ctx, p) : JS_NULL; }
static bool connected(struct web_js_state *s, node_t *n) {
    return n && doc_node_root(n, true) == s->doc->root;
}
static bool equal_nodes(const node_t *a, const node_t *b) {
    if (a == b) return true;
    if (!a || !b || a->type != b->type) return false;
    if (a->type == N_ATTR) {
        const struct attr *aa = a->attribute, *ba = b->attribute;
        if (strcmp(aa->namespace_uri ? aa->namespace_uri : "", ba->namespace_uri ? ba->namespace_uri : "") ||
            strcmp(aa->local ? aa->local : aa->raw, ba->local ? ba->local : ba->raw) || strcmp(aa->value, ba->value)) return false;
    } else if (a->type == N_ELEM) {
        if (a->foreign != b->foreign || a->namespace_id != b->namespace_id || a->nattrs != b->nattrs ||
            strcmp(a->foreign ? a->raw_name : a->name, b->foreign ? b->raw_name : b->name)) return false;
        for (int i = 0; i < a->nattrs; i++) {
            const struct attr *aa = &a->attrs[i];
            bool found = false;
            for (int j = 0; j < b->nattrs; j++)
                if (!strcmp(aa->namespace_uri ? aa->namespace_uri : "", b->attrs[j].namespace_uri ? b->attrs[j].namespace_uri : "") &&
                    !strcmp(aa->local ? aa->local : aa->raw, b->attrs[j].local ? b->attrs[j].local : b->attrs[j].raw) &&
                    !strcmp(aa->value, b->attrs[j].value)) { found = true; break; }
            if (!found) return false;
        }
    } else if (a->type == N_DOCTYPE) {
        if (strcmp(a->name, b->name) || strcmp(a->public_id ? a->public_id : "", b->public_id ? b->public_id : "") ||
            strcmp(a->system_id ? a->system_id : "", b->system_id ? b->system_id : "")) return false;
    } else if (a->type == N_TEXT || a->type == N_COMMENT || a->type == N_PI) {
        if (a->type == N_PI && strcmp(a->name, b->name)) return false;
        if (a->textlen != b->textlen || (a->textlen && memcmp(a->text, b->text, a->textlen))) return false;
    }
    const node_t *ac = a->first, *bc = b->first;
    while (ac && bc) { if (!equal_nodes(ac, bc)) return false; ac = ac->next; bc = bc->next; }
    return !ac && !bc;
}
static bool custom_candidate(const node_t *n) {
    return n && n->type == N_ELEM && !n->foreign && n->name && strchr(n->name, '-');
}
static bool custom_candidates_walk(struct web_js_state *s, node_t *n, const char *name, JSValue result, uint32_t *index) {
    if (custom_candidate(n) && (!name || !strcmp(n->name, name))) {
        JSValue object = wrap(s, n);
        if (JS_IsException(object) || JS_SetPropertyUint32(s->ctx, result, (*index)++, object) < 0) return false;
    }
    /* Shadow-including tree order, not the flattened tree: unassigned light
       children still receive CE lifecycle reactions; template content is inert. */
    if (n->shadow_root && !custom_candidates_walk(s, n->shadow_root, name, result, index)) return false;
    for (node_t *c = n->first; c; c = c->next)
        if (!custom_candidates_walk(s, c, name, result, index)) return false;
    return true;
}
static JSValue custom_candidates(struct web_js_state *s, node_t *root, const char *name) {
    JSValue result = JS_NewArray(s->ctx); uint32_t index = 0;
    if (!JS_IsException(result) && !custom_candidates_walk(s, root, name, result, &index)) {
        JS_FreeValue(s->ctx, result); return JS_EXCEPTION;
    }
    return result;
}
static node_t *find_id(node_t *p, const char *id) {
    for (node_t *n = p->first; n; n = n->next) {
        if (n->type == N_ELEM && n->id && !strcmp(n->id, id)) return n;
        node_t *r = find_id(n, id); if (r) return r;
    }
    return NULL;
}
static void escape_html(sbuf *b, const char *p, size_t n, bool attribute) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == '&') sb_puts(b, "&amp;");
        else if (p[i] == '<') sb_puts(b, "&lt;");
        else if (p[i] == '>') sb_puts(b, "&gt;");
        else if (attribute && p[i] == '"') sb_puts(b, "&quot;");
        else sb_putc(b, p[i]);
    }
}
static bool void_tag(int t) { return t == T_area || t == T_base || t == T_br || t == T_col || t == T_embed || t == T_hr || t == T_img || t == T_input || t == T_link || t == T_meta || t == T_param || t == T_source || t == T_track || t == T_wbr; }
static void serialize(node_t *n, sbuf *b) {
    if (n->type == N_TEXT) {
        if (n->parent && (n->parent->tag == T_script || n->parent->tag == T_style)) sb_put(b, n->text, n->textlen);
        else escape_html(b, n->text ? n->text : "", n->textlen, false);
    } else if (n->type == N_COMMENT) { sb_puts(b, "<!--"); sb_put(b, n->text ? n->text : "", n->textlen); sb_puts(b, "-->"); }
    else if (n->type == N_PI) {
        sb_puts(b, "<?"); sb_puts(b, n->name); sb_putc(b, ' ');
        sb_put(b, n->text ? n->text : "", n->textlen); sb_putc(b, '>');
    } else if (n->type == N_DOCTYPE) {
        sb_puts(b, "<!DOCTYPE "); sb_puts(b, n->name);
        if (n->public_id && *n->public_id) { sb_puts(b, " PUBLIC \""); sb_puts(b, n->public_id); sb_putc(b, '"'); }
        if (n->system_id && *n->system_id) { sb_puts(b, n->public_id && *n->public_id ? " \"" : " SYSTEM \""); sb_puts(b, n->system_id); sb_putc(b, '"'); }
        sb_putc(b, '>');
    }
    else {
        if (n->type == N_ELEM) {
            sb_putc(b, '<'); sb_puts(b, n->raw_name ? n->raw_name : n->name);
            for (int i = 0; i < n->nattrs; ++i) {
                sb_putc(b, ' '); sb_puts(b, n->attrs[i].raw ? n->attrs[i].raw : n->attrs[i].name); sb_puts(b, "=\"");
                escape_html(b, n->attrs[i].value, strlen(n->attrs[i].value), true); sb_putc(b, '"');
            }
            sb_putc(b, '>');
        }
        node_t *parent = n->template_content ? n->template_content : n;
        for (node_t *c = parent->first; c; c = c->next) serialize(c, b);
        if (n->type == N_ELEM && !void_tag(n->tag)) { sb_puts(b, "</"); sb_puts(b, n->raw_name ? n->raw_name : n->name); sb_putc(b, '>'); }
    }
}
static JSValue children(struct web_js_state *s, node_t *n) {
    JSValue a = JS_NewArray(s->ctx); uint32_t i = 0;
    if (JS_IsException(a)) return a;
    for (node_t *c = n->first; c; c = c->next) {
        JSValue v = wrap(s, c);
        if (JS_IsException(v) || JS_SetPropertyUint32(s->ctx, a, i++, v) < 0) { JS_FreeValue(s->ctx, a); return JS_EXCEPTION; }
    }
    return a;
}
static node_t *option_at_js(node_t *select, int wanted) { return doc_select_option(select, wanted); }
static JSValue option_value(JSContext *ctx, node_t *option) {
    sbuf b = {0}; web_option_value(option, &b);
    JSValue result = JS_NewStringLen(ctx, b.p ? b.p : "", b.n); sb_free(&b); return result;
}
static bool html_image(const node_t *n) { return n && n->type == N_ELEM && !n->foreign && n->tag == T_img; }
static JSValue get_image(struct web_js_state *s, node_t *n, const char *p) {
    JSContext *ctx = s->ctx; web_doc *d = n->owner ? n->owner : s->doc;
    if (!html_image(n)) return JS_ThrowTypeError(ctx, "HTMLImageElement receiver required");
    if (!strcmp(p, "imageBrand")) return JS_UNDEFINED;
    if (d->resources_dirty) doc_sync_tree(d);
    doc_image_sync(d, n);
    struct web_image *current = n->image >= 0 && n->image < d->images.n ? d->images.v[n->image] : NULL;
    struct web_image *request = n->image_request >= 0 && n->image_request < d->images.n ? d->images.v[n->image_request] : NULL;
    int w = current && current->img ? current->img->w : 0, h = current && current->img ? current->img->h : 0;
    if (!strcmp(p, "imageNaturalWidth")) return JS_NewInt32(ctx, w);
    if (!strcmp(p, "imageNaturalHeight")) return JS_NewInt32(ctx, h);
    if (!strcmp(p, "imageComplete")) return JS_NewBool(ctx, !request || request->done);
    if (!strcmp(p, "imageCurrentSrc")) return JS_NewString(ctx, current ? current->url : "");
    if (!strcmp(p, "imageWidth") || !strcmp(p, "imageHeight")) {
        bool width = !strcmp(p, "imageWidth");
        if (connected(s, n) && d->width > 0) {
            web_layout(d, d->width, d->height);
            if (n->box) { double value = width ? n->box->w : n->box->h; return JS_NewUint32(ctx, !(value > 0) ? 0 : value >= UINT32_MAX ? UINT32_MAX : (uint32_t)value); }
        }
        const char *value = node_attr(n, width ? "width" : "height");
        if (value) {
            while (is_space((unsigned char)*value)) value++;
            if (*value == '+') value++;
            if (*value >= '0' && *value <= '9') {
                uint64_t number = 0;
                while (*value >= '0' && *value <= '9') { number = number * 10 + (unsigned)(*value++ - '0'); if (number > UINT32_MAX) return JS_NewUint32(ctx, 0); }
                return JS_NewUint32(ctx, (uint32_t)number);
            }
        }
        return JS_NewInt32(ctx, width ? w : h);
    }
    return JS_UNDEFINED;
}
static JSValue image_decode_promise(struct web_js_state *s, node_t *n) {
    if (!html_image(n)) return JS_ThrowTypeError(s->ctx, "HTMLImageElement receiver required");
    if (n->owner && n->owner->inert) return JS_ThrowTypeError(s->ctx, "Image decoding requires an active document");
    if (s->doc->resources_dirty) doc_sync_tree(s->doc);
    doc_image_sync(s->doc, n);
    JSValue functions[2] = {JS_UNDEFINED, JS_UNDEFINED};
    JSValue promise = JS_NewPromiseCapability(s->ctx, functions);
    if (JS_IsException(promise)) return promise;
    struct js_image_decode *request = js_mallocz(s->ctx, sizeof *request);
    if (!request) { JS_FreeValue(s->ctx, promise); JS_FreeValue(s->ctx, functions[0]); JS_FreeValue(s->ctx, functions[1]); return JS_EXCEPTION; }
    request->node = n; request->generation = n->image_generation;
    request->resolve = functions[0]; request->reject = functions[1];
    if (s->last_image_decode) s->last_image_decode->next = request; else s->image_decodes = request;
    s->last_image_decode = request;
    return promise;
}
static JSValue get_dom(struct web_js_state *s, node_t *n, const char *p) {
    JSContext *ctx = s->ctx; web_doc *d = n->owner ? n->owner : s->doc;
    if (!strcmp(p, "shadowRoot")) return wrap(s, n->shadow_root);
    if (!strcmp(p, "shadowHost")) return wrap(s, n->shadow_host);
    if (!strcmp(p, "shadowHostValid")) return JS_NewBool(ctx, doc_shadow_host_valid(n));
    if (!strcmp(p, "assignedSlot")) return wrap(s, doc_assigned_slot(n, false));
    if (!strcmp(p, "assignedSlotOpen")) return wrap(s, doc_assigned_slot(n, true));
    if (!strcmp(p, "slotBrand")) return n->type == N_ELEM && !n->foreign && n->tag == T_slot ? JS_TRUE : JS_ThrowTypeError(ctx, "HTMLSlotElement receiver required");
    if (!strcmp(p, "shadowMode") || !strcmp(p, "shadowDelegatesFocus") || !strcmp(p, "shadowClonable") ||
        !strcmp(p, "shadowSerializable") || !strcmp(p, "shadowSlotAssignment")) {
        if (!n->shadow_host) return JS_ThrowTypeError(ctx, "ShadowRoot receiver required");
        if (!strcmp(p, "shadowMode")) return JS_NewString(ctx, n->shadow_closed ? "closed" : "open");
        if (!strcmp(p, "shadowSlotAssignment")) return JS_NewString(ctx, n->shadow_manual ? "manual" : "named");
        return JS_NewBool(ctx, !strcmp(p, "shadowDelegatesFocus") ? n->shadow_delegates_focus :
            !strcmp(p, "shadowClonable") ? n->shadow_clonable : n->shadow_serializable);
    }
    if (!strncmp(p, "svg", 3)) {
        if (n->type != N_ELEM || n->namespace_id != NS_SVG)
            return JS_ThrowTypeError(ctx, "SVGElement receiver required");
        if (!strcmp(p, "svgRootBrand")) {
            if (strcmp(n->raw_name, "svg")) return JS_ThrowTypeError(ctx, "SVGSVGElement receiver required");
            return JS_TRUE;
        }
        if (!strcmp(p, "svgBrand")) return JS_TRUE;
        if (!strcmp(p, "svgOwner") || !strcmp(p, "svgViewport")) {
            /* No use-instance shadow tree is implemented. In the native DOM,
               ancestor <svg> elements establish the supported SVG viewports. */
            for (node_t *a = n->parent; a; a = a->parent)
                if (a->type == N_ELEM && a->namespace_id == NS_SVG && !strcmp(a->raw_name, "svg")) return wrap(s, a);
            return JS_NULL;
        }
    }
    if (!strcmp(p, "ownerDocument")) return n->type == N_DOC ? JS_NULL : wrap(s, d->root);
    if (!strcmp(p, "scripting")) return JS_NewBool(ctx, d == s->doc);
    if (!strcmp(p, "templateContent")) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_template) return JS_ThrowTypeError(ctx, "HTMLTemplateElement receiver required");
        node_t *content = doc_template_content(d, n);
        return content ? wrap(s, content) : oom(ctx);
    }
    if (!strcmp(p, "URL") || !strcmp(p, "documentURI")) return JS_NewString(ctx, d->url);
    /* Nocturne currently sends no navigation referrer. Expose the standard
       empty DOMString, not undefined (and never invent a previous URL). */
    if (!strcmp(p, "referrer")) return JS_NewString(ctx, "");
    if (!strcmp(p, "contentType")) return JS_NewString(ctx, "text/html");
    if (!strcmp(p, "characterSet")) return JS_NewString(ctx, "UTF-8");
    if (!strcmp(p, "compatMode")) return JS_NewString(ctx, d->quirks ? "BackCompat" : "CSS1Compat");
    if (!strcmp(p, "doctype")) { for (node_t *c = d->root->first; c; c = c->next) if (c->type == N_DOCTYPE) return wrap(s, c); return JS_NULL; }
    if (!strcmp(p, "doctypeName") || !strcmp(p, "publicId") || !strcmp(p, "systemId")) {
        if (n->type != N_DOCTYPE) return JS_ThrowTypeError(ctx, "DocumentType receiver required");
        return JS_NewString(ctx, !strcmp(p, "doctypeName") ? n->name : !strcmp(p, "publicId") ? (n->public_id ? n->public_id : "") : (n->system_id ? n->system_id : ""));
    }
    if (!strncmp(p, "image", 5)) return get_image(s, n, p);
    if (!strncmp(p, "form:", 5)) {
        if (n->type != N_ELEM || n->foreign || strcmp(n->name, p + 5)) return JS_ThrowTypeError(ctx, "Form control interface receiver required");
        node_t *control = n->tag == T_option ? node_ancestor(n, T_select) : n;
        if (!control) return JS_NULL;
        return wrap(s, web_form_owner(d, control));
    }
    if (d->resources_dirty && (!strcmp(p, "documentElement") || !strcmp(p, "head") || !strcmp(p, "body") || !strcmp(p, "baseURI") || !strcmp(p, "activeElement"))) doc_sync_tree(d);
    if (!strcmp(p, "value") || !strcmp(p, "checked") || !strcmp(p, "selectedIndex") || !strcmp(p, "selected")) doc_control_init(d, n);
    if (!strcmp(p, "attrBrand")) return n->type == N_ATTR ? JS_TRUE : JS_ThrowTypeError(ctx, "Attr receiver required");
    if (!strcmp(p, "elementBrand")) return n->type == N_ELEM ? JS_TRUE : JS_ThrowTypeError(ctx, "Element receiver required");
    if (n->type == N_ATTR) {
        struct attr *a = n->attribute;
        if (!strcmp(p, "attrName") || !strcmp(p, "nodeName")) return JS_NewString(ctx, a->raw);
        if (!strcmp(p, "localName")) return JS_NewString(ctx, a->local ? a->local : a->raw);
        if (!strcmp(p, "namespaceURI")) return str_or_null(ctx, a->namespace_uri);
        if (!strcmp(p, "prefix")) return str_or_null(ctx, a->prefix);
        if (!strcmp(p, "attrValue") || !strcmp(p, "nodeValue") || !strcmp(p, "textContent")) return JS_NewString(ctx, a->value);
        if (!strcmp(p, "attrOwner")) return wrap(s, n->attr_owner);
    }
    if (!strcmp(p, "nodeType")) return JS_NewInt32(ctx, n->type == N_DOC ? 9 : n->type == N_ELEM ? 1 : n->type == N_TEXT ? 3 : n->type == N_COMMENT ? 8 : n->type == N_DOCTYPE ? 10 : n->type == N_PI ? 7 : n->type == N_ATTR ? 2 : 11);
    if (!strcmp(p, "nodeName")) {
        if (n->type != N_ELEM) return JS_NewString(ctx, n->type == N_DOC ? "#document" : n->type == N_TEXT ? "#text" : n->type == N_COMMENT ? "#comment" : n->type == N_DOCTYPE || n->type == N_PI ? n->name : "#document-fragment");
        char name[256]; snprintf(name, sizeof name, "%s", n->foreign ? n->raw_name : n->name);
        if (!n->foreign) for (char *q = name; *q; ++q) *q = (char)toupper((unsigned char)*q);
        return JS_NewString(ctx, name);
    }
    if (!strcmp(p, "localName")) return n->type == N_ELEM ? JS_NewString(ctx, n->foreign ? n->raw_name : n->name) : JS_NULL;
    if (!strcmp(p, "namespaceURI")) return n->type != N_ELEM ? JS_NULL : JS_NewString(ctx,
        n->namespace_id == NS_SVG ? "http://www.w3.org/2000/svg" :
        n->namespace_id == NS_MATHML ? "http://www.w3.org/1998/Math/MathML" : "http://www.w3.org/1999/xhtml");
    if (!strcmp(p, "piTarget")) return n->type == N_PI ? JS_NewString(ctx, n->name) : JS_ThrowTypeError(ctx, "ProcessingInstruction receiver required");
    if (!strcmp(p, "nodeValue")) return n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI ? JS_NewStringLen(ctx, n->text ? n->text : "", n->textlen) : JS_NULL;
    if (!strcmp(p, "parentNode")) return wrap(s, n->parent);
    if (!strcmp(p, "isConnected")) return JS_NewBool(ctx, doc_node_connected(n));
    if (!strcmp(p, "firstChild")) return wrap(s, n->first);
    if (!strcmp(p, "lastChild")) return wrap(s, n->last);
    if (!strcmp(p, "nextSibling")) return wrap(s, n->next);
    if (!strcmp(p, "previousSibling")) return wrap(s, n->prev);
    if (!strcmp(p, "childNodes")) return children(s, n);
    if (!strcmp(p, "textContent")) {
        if (n->type == N_DOC || n->type == N_DOCTYPE) return JS_NULL;
        if (n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI) return JS_NewStringLen(ctx, n->text ? n->text : "", n->textlen);
        sbuf b = {0}; node_text_content(n, &b); JSValue v = JS_NewStringLen(ctx, b.p ? b.p : "", b.n); sb_free(&b); return v;
    }
    if (!strcmp(p, "innerHTML") || !strcmp(p, "outerHTML")) {
        sbuf b = {0};
        if (!strcmp(p, "outerHTML")) serialize(n, &b);
        else for (node_t *c = n->template_content ? n->template_content->first : n->first; c; c = c->next) serialize(c, &b);
        JSValue v = JS_NewStringLen(ctx, b.p ? b.p : "", b.n); sb_free(&b); return v;
    }
    if (!strcmp(p, "attributeNames")) {
        JSValue a = JS_NewArray(ctx);
        for (int i = 0; i < n->nattrs; ++i) JS_SetPropertyUint32(ctx, a, i, JS_NewString(ctx, n->attrs[i].raw));
        return a;
    }
    if (!strcmp(p, "documentElement")) return wrap(s, d->html);
    if (!strcmp(p, "head")) return wrap(s, d->head);
    if (!strcmp(p, "body")) return wrap(s, d->body);
    if (!strcmp(p, "activeElement")) {
        node_t *focus = d->focus && connected(s, d->focus) ? d->focus : NULL;
        node_t *scope = n->shadow_host ? n : d->root;
        while (focus) {
            node_t *root = doc_node_root(focus, false);
            if (root == scope) return wrap(s, focus);
            focus = root->shadow_host;
        }
        return wrap(s, n->shadow_host ? NULL : d->body ? d->body : d->html);
    }
    if (!strcmp(p, "baseURI")) return JS_NewString(ctx, d->base);
    if (!strcmp(p, "title")) {
        if (d->resources_dirty) doc_rescan(d);
        if (d->inert) {
            sbuf text = {0};
            for (node_t *c = d->head ? d->head->first : NULL; c; c = c->next)
                if (c->tag == T_title) { node_text_content(c, &text); break; }
            JSValue result = JS_NewStringLen(ctx, text.p ? text.p : "", text.n); sb_free(&text); return result;
        }
        return JS_NewString(ctx, d->title ? d->title : "");
    }
    if (!strcmp(p, "value")) {
        if (n->tag == T_select) { node_t *option = option_at_js(n, n->selected); return option ? option_value(ctx, option) : JS_NewString(ctx, ""); }
        if (n->tag == T_option) return option_value(ctx, n);
        const char *v = n->value ? n->value : node_attr(n, "value"); return JS_NewString(ctx, v ? v : "");
    }
    if (!strcmp(p, "checked")) return JS_NewBool(ctx, n->checked);
    if (!strcmp(p, "selectedIndex")) return JS_NewInt32(ctx, n->selected);
    if (!strcmp(p, "selected")) return JS_NewBool(ctx, doc_option_selected(n));
    if (!strcmp(p, "selectOptions")) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_select) return JS_ThrowTypeError(ctx, "HTMLSelectElement receiver required");
        doc_control_init(d, n); JSValue values = JS_NewArray(ctx); uint32_t i = 0;
        if (JS_IsException(values)) return values;
        for (node_t *o = web_select_next_option(n, NULL); o; o = web_select_next_option(n, o)) {
            JSValue option = wrap(s, o);
            if (JS_IsException(option)) { JS_FreeValue(ctx, values); return option; }
            if (JS_SetPropertyUint32(ctx, values, i++, option) < 0) { JS_FreeValue(ctx, values); return JS_EXCEPTION; }
        }
        return values;
    }
    if (!strcmp(p, "optionIndex")) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_option) return JS_ThrowTypeError(ctx, "HTMLOptionElement receiver required");
        node_t *select = web_option_select(n); int i = 0;
        if (select) for (node_t *o = web_select_next_option(select, NULL); o; o = web_select_next_option(select, o), i++) if (o == n) return JS_NewInt32(ctx, i);
        return JS_NewInt32(ctx, 0);
    }
    if (!strcmp(p, "optionText")) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_option) return JS_ThrowTypeError(ctx, "HTMLOptionElement receiver required");
        sbuf b = {0}; web_option_text(n, &b); JSValue result = JS_NewStringLen(ctx, b.p ? b.p : "", b.n); sb_free(&b); return result;
    }
    if (!strcmp(p, "form")) return wrap(s, web_form_owner(d, n));
    if (!strcmp(p, "async")) {
        if (n->tag != T_script) return JS_UNDEFINED;
        struct js_node_ref *r = node_ref(s, n);
        return JS_NewBool(ctx, (r && r->force_async) || node_attr(n, "async") != NULL);
    }
    return JS_UNDEFINED;
}

static JSValue set_dom(struct web_js_state *s, node_t *n, const char *p, JSValueConst value) {
    JSContext *ctx = s->ctx; web_doc *d = n->owner ? n->owner : s->doc;
    if (d->resources_dirty && !strcmp(p, "title")) doc_rescan(d);
    if (!strcmp(p, "imageWidth") || !strcmp(p, "imageHeight")) {
        if (!html_image(n)) return JS_ThrowTypeError(ctx, "HTMLImageElement receiver required");
        uint32_t number; if (JS_ToUint32(ctx, &number, value) < 0) return JS_EXCEPTION;
        char text[16]; snprintf(text, sizeof text, "%u", number);
        return doc_attr_set_ns(d, n, NULL, NULL, !strcmp(p, "imageWidth") ? "width" : "height", text) ? JS_UNDEFINED : oom(ctx);
    }
    if (!strcmp(p, "value") || !strcmp(p, "checked") || !strcmp(p, "selectedIndex") || !strcmp(p, "selected")) doc_control_init(d, n);
    if (!strcmp(p, "checked")) { int b = JS_ToBool(ctx, value); if (b < 0) return JS_EXCEPTION; doc_control_checked(d, n, b); doc_mutated(d, n); return JS_UNDEFINED; }
    if (!strcmp(p, "async")) {
        if (n->tag != T_script) return JS_UNDEFINED;
        int b = JS_ToBool(ctx, value); if (b < 0) return JS_EXCEPTION;
        struct js_node_ref *r = node_ref(s, n);
        /* The IDL value reflects the attribute; it is not the force-async flag. */
        if (r) { r->force_async_set = true; r->force_async = false; }
        return doc_attr_set_ns(d, n, NULL, NULL, "async", b ? "" : NULL) ? JS_UNDEFINED : oom(ctx);
    }
    if (!strcmp(p, "selectedIndex")) { int32_t i; if (JS_ToInt32(ctx, &i, value)) return JS_EXCEPTION; web_select_set_index(d, n, i); doc_mutated(d, n); return JS_UNDEFINED; }
    if (!strcmp(p, "selected") || !strcmp(p, "optionFactorySelected")) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_option) return JS_ThrowTypeError(ctx, "HTMLOptionElement receiver required");
        int b = JS_ToBool(ctx, value); if (b < 0) return JS_EXCEPTION;
        node_t *select = web_option_select(n);
        web_option_set_selected(d, n, b, strcmp(p, "optionFactorySelected") != 0);
        doc_mutated(d, select ? select : n); return JS_UNDEFINED;
    }
    size_t len; const char *text = JS_ToCStringLen(ctx, &len, value);
    if (!text) return JS_EXCEPTION;
    if (n->type == N_ATTR && strlen(text) != len) { JS_FreeCString(ctx, text); return JS_ThrowTypeError(ctx, "NUL in native attribute values is not implemented"); }
    bool ok = true;
    if (!strcmp(p, "attrValue") && n->type == N_ATTR) ok = doc_attr_value(d, n, text);
    else if (!strcmp(p, "nodeValue")) { if (n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI || n->type == N_ATTR) ok = doc_node_text(d, n, text, len); }
    else if (!strcmp(p, "textContent")) { if (n->type != N_DOC) ok = doc_node_text(d, n, text, len); }
    else if (!strcmp(p, "innerHTML")) {
        if (n->type != N_ELEM && n->type != N_FRAGMENT) { JS_FreeCString(ctx, text); return JS_ThrowTypeError(ctx, "innerHTML requires an element"); }
        ok = doc_node_html(d, n, text, len);
    } else if (!strcmp(p, "value")) {
        if (n->tag == T_select) {
            int found = -1, i = 0;
            for (node_t *option = web_select_next_option(n, NULL); option; option = web_select_next_option(n, option), i++) {
                JSValue v = option_value(ctx, option); const char *p = JS_ToCString(ctx, v);
                if (!p) { JS_FreeValue(ctx, v); JS_FreeCString(ctx, text); return JS_EXCEPTION; }
                bool matches = !strcmp(p, text); JS_FreeCString(ctx, p); JS_FreeValue(ctx, v);
                if (matches) { found = i; break; }
            }
            web_select_set_index(d, n, found);
            doc_mutated(d, n);
        } else if (n->tag == T_option) ok = doc_attr_set_ns(d, n, NULL, NULL, "value", text);
        else {
            bool control = n->tag == T_input || n->tag == T_textarea;
            char *before = control ? strdup(n->value ? n->value : "") : NULL;
            ok = (!control || before) && len <= 1024u * 1024u && doc_node_value(d, n, text, len);
            if (ok && control) n->selection_set = true;
            if (ok && control && strcmp(before, n->value)) {
                /* The value IDL setters move the cursor only when the sanitized
                   value actually changes; a React same-value write is harmless. */
                n->selection_start = n->selection_end = doc_utf16_length(n->value);
                n->selection_direction = 0; doc_control_caret(d, n);
            }
            free(before);
        }
    } else if (!strcmp(p, "title")) {
        node_t *title = NULL;
        if (d->head) for (node_t *c = d->head->first; c; c = c->next) if (c->tag == T_title) { title = c; break; }
        if (!title && d->head) { title = doc_node_create(d, N_ELEM, "title", NULL, 0); if (title) ok = doc_node_move(d, d->head, title, NULL); }
        if (title && ok) ok = doc_node_text(d, title, text, len); else ok = false;
    } else { JS_FreeCString(ctx, text); return JS_ThrowTypeError(ctx, "Unknown DOM setter"); }
    JS_FreeCString(ctx, text);
    return ok ? JS_UNDEFINED : oom(ctx);
}

/* CSS declaration boundaries must not split quoted strings or url()/calc(). */
static const char *css_separator(const char *p, const char *end, int wanted) {
    int quote = 0, depth = 0;
    while (p < end) {
        int c = (unsigned char)*p;
        if (c == '\\' && p + 1 < end) { p += 2; continue; }
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        else if (c == '/' && p + 1 < end && p[1] == '*') { p += 2; while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++; if (p + 1 < end) p += 2; continue; }
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
        else if (!depth && c == wanted) return p;
        p++;
    }
    return end;
}
static bool css_property_equal(const char *begin, const char *end, const char *property) {
    while (begin < end && is_space((unsigned char)*begin)) begin++;
    while (end > begin && is_space((unsigned char)end[-1])) end--;
    size_t n = (size_t)(end - begin);
    return strlen(property) == n && (!strncmp(property, "--", 2) ? !strncmp(begin, property, n) : !strncasecmp(begin, property, n));
}
static JSValue style_access(struct web_js_state *s, node_t *n, const char *property,
                            bool set, JSValueConst value, JSValueConst priority) {
    JSContext *ctx = s->ctx;
    const char *css = node_attr(n, "style"); if (!css) css = "";
    sbuf b = {0}; const char *found = NULL, *found_end = NULL;
    const char *end = css + strlen(css);
    for (const char *p = css; p < end;) {
        const char *e = css_separator(p, end, ';'), *colon = css_separator(p, e, ':');
        if (colon < e && css_property_equal(p, colon, property)) {
            found = colon + 1; found_end = e;
            while (found < found_end && is_space((unsigned char)*found)) found++;
            while (found_end > found && is_space((unsigned char)found_end[-1])) found_end--;
        } else if (set) { sb_put(&b, p, (size_t)(e - p)); if (e < end) sb_putc(&b, ';'); }
        p = e < end ? e + 1 : end;
    }
    if (!set) { sb_free(&b); return JS_NewStringLen(ctx, found ? found : "", found ? (size_t)(found_end - found) : 0); }
    const char *v = JS_ToCString(ctx, value), *prio = JS_IsUndefined(priority) ? NULL : JS_ToCString(ctx, priority);
    if (!v || (!JS_IsUndefined(priority) && !prio)) { JS_FreeCString(ctx, v); JS_FreeCString(ctx, prio); sb_free(&b); return JS_EXCEPTION; }
    if (strchr(property, ':') || strchr(property, ';') || strchr(property, '{') || strchr(property, '}')) { JS_FreeCString(ctx, v); JS_FreeCString(ctx, prio); sb_free(&b); return JS_ThrowTypeError(ctx, "Invalid CSS property name"); }
    if (prio && *prio && strcasecmp(prio, "important")) { JS_FreeCString(ctx, v); JS_FreeCString(ctx, prio); sb_free(&b); return JS_UNDEFINED; }
    if (*v) {
        if (b.n && b.p[b.n - 1] != ';') sb_putc(&b, ';');
        sb_puts(&b, property); sb_putc(&b, ':'); sb_puts(&b, v);
        if (prio && *prio) sb_puts(&b, " !important"); sb_putc(&b, ';');
    }
    bool ok = doc_attr_set_ns(n->owner ? n->owner : s->doc, n, NULL, NULL, "style", sb_cstr(&b));
    JS_FreeCString(ctx, v); JS_FreeCString(ctx, prio); sb_free(&b);
    return ok ? JS_UNDEFINED : oom(ctx);
}
static void flush_layout(struct web_js_state *s) {
    web_doc *d = s->doc;
    if (d->width < 1) return;
    uint64_t start = uptime_ms();
    web_layout(d, d->width, d->height);
    s->task_layout_ms += uptime_ms() - start; s->task_layout_flushes++;
}
static JSValue rect_array(JSContext *ctx, float x, float y, float w, float h) {
    JSValue a = JS_NewArray(ctx);
    const float values[] = {x,y,w,h};
    for (int i=0;i<4;i++) JS_SetPropertyUint32(ctx,a,i,JS_NewFloat64(ctx,values[i]));
    return a;
}
static JSValue observer_geometry(struct web_js_state *s, node_t *n, node_t *root) {
    JSContext *ctx=s->ctx; web_doc *d=s->doc;
    if (!n || n->type!=N_ELEM || (root && root->type!=N_ELEM && root!=d->root))
        return JS_ThrowTypeError(ctx,"Observer requires an Element and an Element/Document root");
    flush_layout(s);
    bool visible=connected(s,n) && n->box && n->style && n->style->display!=D_NONE;
    box_t *b=visible?n->box:NULL;
    int sx=0,sy=0;if(s->host.scroll)s->host.scroll(s->host.opaque,&sx,&sy);
    JSValue out=JS_NewObject(ctx),clips=JS_NewArray(ctx);
    float x=0,y=0,w=0,h=0;
    if(b){x=box_abs_x(b)-b->p[3]-b->b[3]-sx;y=box_abs_y(b)-b->p[0]-b->b[0]-sy;
        w=b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3];h=b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2];}
    JS_SetPropertyStr(ctx,out,"rect",rect_array(ctx,x,y,w,h));
    bool resizable=b && (b->kind!=B_INLINE || b->kind==B_ATOMIC);
    JS_SetPropertyStr(ctx,out,"content",rect_array(ctx,b?b->p[3]:0,b?b->p[0]:0,resizable?b->w:0,resizable?b->h:0));
    JS_SetPropertyStr(ctx,out,"border",rect_array(ctx,0,0,resizable?w:0,resizable?h:0));
    float rx=0,ry=0,rw=d->width,rh=d->height;
    bool root_ok=!root || root==d->root;
    if(root && root!=d->root) {
        box_t *r=connected(s,root)?root->box:NULL;
        if(r){bool clip=r->st && r->st->overflow!=OV_VISIBLE;
            rx=box_abs_x(r)-r->p[3]-(clip?0:r->b[3])-sx;
            ry=box_abs_y(r)-r->p[0]-(clip?0:r->b[0])-sy;
            rw=r->w+r->p[1]+r->p[3]+(clip?0:r->b[1]+r->b[3]);
            rh=r->h+r->p[0]+r->p[2]+(clip?0:r->b[0]+r->b[2]);
            for(node_t *p=n->parent;p;p=p->parent)if(p==root){root_ok=true;break;}
        }else rw=rh=0;
    }
    unsigned count=0;
    /* Use the same overflow/containing-block rule as native painting. */
    bool reached=!b || !b->st || b->st->position!=POS_ABSOLUTE;
    for(box_t *a=b?b->parent:NULL;a && a->parent;a=a->parent) {
        if(root && a->node==root)break;
        if(a->st && a->kind!=B_INLINE && a->st->position!=POS_STATIC)reached=true;
        if(a->st && a->st->overflow!=OV_VISIBLE && a->kind!=B_INLINE && reached &&
           (!b->st || b->st->position!=POS_FIXED))
            JS_SetPropertyUint32(ctx,clips,count++,rect_array(ctx,box_abs_x(a)-a->p[3]-sx,
                box_abs_y(a)-a->p[0]-sy,a->w+a->p[1]+a->p[3],a->h+a->p[0]+a->p[2]));
    }
    JS_SetPropertyStr(ctx,out,"root",rect_array(ctx,rx,ry,rw,rh));
    JS_SetPropertyStr(ctx,out,"clips",clips);
    JS_SetPropertyStr(ctx,out,"intersectsRoot",JS_NewBool(ctx,visible && root_ok));
    return out;
}
static node_t *offset_parent(struct web_js_state *s, node_t *n) {
    if (!n || !n->box || n == s->doc->html || n == s->doc->body ||
        (n->style && n->style->position == POS_FIXED)) return NULL;
    for (node_t *p = n->parent; p; p = p->parent) {
        if (!p->box || !p->style) continue;
        if (p == s->doc->body || p->style->position != POS_STATIC ||
            (n->style && n->style->position == POS_STATIC &&
             (p->tag == T_table || p->tag == T_td || p->tag == T_th))) return p;
    }
    return NULL;
}
static JSValue geometry(struct web_js_state *s, node_t *n, const char *property) {
    web_doc *d = s->doc; JSContext *ctx = s->ctx;
    if (!n || n->type != N_ELEM) return JS_ThrowTypeError(ctx, "Geometry requires an element");
    bool visible = connected(s, n);
    if (visible) flush_layout(s);
    box_t *b = visible ? n->box : NULL;
    node_t *parent = b ? offset_parent(s, n) : NULL;
    if (!strcmp(property, "offsetParent")) return wrap(s, parent);
    float value = 0;
    if ((!strcmp(property, "clientWidth") || !strcmp(property, "clientHeight")) &&
        (n == d->html || (d->quirks && n == d->body)))
        value = !strcmp(property, "clientWidth") ? d->width : d->height;
    else if (b) {
        if (!strcmp(property, "offsetWidth")) value = b->w + b->p[1] + b->p[3] + b->b[1] + b->b[3];
        else if (!strcmp(property, "offsetHeight")) value = b->h + b->p[0] + b->p[2] + b->b[0] + b->b[2];
        else if (!strcmp(property, "clientWidth")) value = b->w + b->p[1] + b->p[3];
        else if (!strcmp(property, "clientHeight")) value = b->h + b->p[0] + b->p[2];
        else if (!strcmp(property, "clientLeft")) value = b->b[3];
        else if (!strcmp(property, "clientTop")) value = b->b[0];
        else if (!strcmp(property, "offsetLeft")) {
            value = box_abs_x(b) - b->p[3] - b->b[3];
            if (parent) value -= box_abs_x(parent->box) - parent->box->p[3];
            if (n == d->body) value = 0;
        } else if (!strcmp(property, "offsetTop")) {
            value = box_abs_y(b) - b->p[0] - b->b[0];
            if (parent) value -= box_abs_y(parent->box) - parent->box->p[0];
            if (n == d->body) value = 0;
        }
    }
    return JS_NewInt32(ctx, (int32_t)roundf(value));
}
static JSValue computed(struct web_js_state *s, node_t *n, const char *property) {
    web_doc *d = s->doc; JSContext *ctx = s->ctx;
    if (!connected(s, n)) return JS_NewString(ctx, "");
    if (d->resources_dirty) doc_rescan(d);
    flush_layout(s);
    style_t *st = n->style; char out[128]; out[0] = 0;
    if (!st) return JS_NewString(ctx, "");
    uint32_t color = 0; bool is_color = false; float px = 0; bool is_px = false;
    if (!strcmp(property, "color")) { color = st->color; is_color = true; }
    else if (!strcmp(property, "background-color")) { color = st->bg_color; is_color = true; }
    else if (!strcmp(property, "font-size")) { px = st->font_size; is_px = true; }
    else if (!strcmp(property, "font-weight")) snprintf(out, sizeof out, "%u", st->font_weight);
    else if (!strcmp(property, "opacity")) snprintf(out, sizeof out, "%g", (double)st->opacity);
    else if (!strcmp(property, "display")) {
        static const char *const names[] = {"none", "inline", "block", "list-item", "inline-block", "table", "inline-table", "table-row-group", "table-header-group", "table-footer-group", "table-row", "table-cell", "table-column", "table-column-group", "table-caption", "flex", "inline-flex", "grid", "inline-grid", "contents", "flow-root"};
        snprintf(out, sizeof out, "%s", st->display < sizeof names / sizeof *names ? names[st->display] : "block");
    } else if (!strcmp(property, "position")) { static const char *const names[] = {"static", "relative", "absolute", "fixed", "sticky"}; snprintf(out, sizeof out, "%s", names[st->position < 5 ? st->position : 0]); }
    else if (!strcmp(property, "width") || !strcmp(property, "height")) { px = n->box ? (!strcmp(property, "width") ? n->box->w : n->box->h) : 0; is_px = true; }
    else {
        static const char *const sides[] = {"top", "right", "bottom", "left"};
        for (int i = 0; i < 4; i++) {
            char key[32];
            snprintf(key, sizeof key, "margin-%s", sides[i]);
            if (!strcmp(property, key) && n->box) { px = n->box->m[i]; is_px = true; }
            snprintf(key, sizeof key, "padding-%s", sides[i]);
            if (!strcmp(property, key) && n->box) { px = n->box->p[i]; is_px = true; }
            snprintf(key, sizeof key, "border-%s-width", sides[i]);
            if (!strcmp(property, key) && n->box) { px = n->box->b[i]; is_px = true; }
        }
    }
    if (is_color) snprintf(out, sizeof out, "rgba(%u, %u, %u, %g)", (color >> 16) & 255, (color >> 8) & 255, color & 255, (double)((color >> 24) & 255) / 255);
    if (is_px) snprintf(out, sizeof out, "%gpx", (double)px);
    return JS_NewString(ctx, out);
}
/* Private CE hooks retain native wrapper identity. They only enqueue reactions;
   JavaScript's CEReactions scope invokes callbacks after the DOM operation. */
static JSValue custom_element_hook(struct web_js_state *s, const char *name, int argc, JSValueConst *argv) {
    if (s->disabled || !JS_IsObject(s->hooks)) return JS_NULL;
    JSValue fn = JS_GetPropertyStr(s->ctx, s->hooks, name);
    if (JS_IsException(fn)) return fn;
    if (!JS_IsFunction(s->ctx, fn)) { JS_FreeValue(s->ctx, fn); return JS_NULL; }
    JSValue result = JS_Call(s->ctx, fn, s->hooks, argc, argv);
    JS_FreeValue(s->ctx, fn);
    return result;
}
static JSValue attribute_native_changed(struct web_js_state *s, node_t *n, const char *name, bool old_present) {
    JSContext *ctx = s->ctx;
    const char *value = node_attr(n, name);
    if (n->tag == T_script && !n->foreign && !strcmp(name, "async") && value && !old_present) {
        struct js_node_ref *r = node_ref(s, n);
        if (r) { r->force_async_set = true; r->force_async = false; }
    }
    if (!strncmp(name, "on", 2) && (value || old_present)) {
        JSValue args[] = {wrap(s, n), JS_NewString(ctx, name), str_or_null(ctx, value)};
        JSValue notified = custom_element_hook(s, "eventHandlerAttribute", 3, args);
        for (int i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
        if (JS_IsException(notified)) return notified;
        JS_FreeValue(ctx, notified);
    }
    if (html_image(n) && (!strcmp(name, "src") || !strcmp(name, "srcset") || !strcmp(name, "sizes") ||
        !strcmp(name, "data-src") || !strcmp(name, "data-lazy-src") || !strcmp(name, "data-original") || !strcmp(name, "data-srcset"))) n->image_invalidated = true;
    if (n->tag == T_input && !strcmp(name, "src")) {
        struct js_node_ref *r = node_ref(s, n); if (r) r->image_notified = false;
    }
    return JS_UNDEFINED;
}
static JSValue form_value_dom(struct web_js_state *s, node_t *n, int argc, JSValueConst *argv) {
    JSContext *ctx = s->ctx; web_doc *d = n->owner ? n->owner : s->doc;
    if (n->type != N_ELEM || n->foreign || n->tag != T_input || argc < 3)
        return JS_ThrowTypeError(ctx, "HTMLInputElement receiver required");
    const char *op = JS_ToCString(ctx, argv[2]); if (!op) return JS_EXCEPTION;
    JSValue result;
    if (!strcmp(op,"type")) result = JS_NewString(ctx,web_input_type_name(web_input_type(n)));
    else if (!strcmp(op,"number")) result = JS_NewFloat64(ctx,web_input_value_number(d,n));
    else if (!strcmp(op,"date")) result = JS_NewFloat64(ctx,web_input_value_date(d,n));
    else if (!strcmp(op,"setNumber") || !strcmp(op,"setDate")) {
        double value;
        result = argc < 4 || JS_ToFloat64(ctx,&value,argv[3]) < 0 ? JS_EXCEPTION :
            JS_NewInt32(ctx,web_input_set_number(d,n,value,!strcmp(op,"setDate")));
    } else if (!strcmp(op,"stepUp") || !strcmp(op,"stepDown")) {
        int32_t count;
        result = argc < 4 || JS_ToInt32(ctx,&count,argv[3]) < 0 ? JS_EXCEPTION :
            JS_NewInt32(ctx,web_input_step(d,n,count,!strcmp(op,"stepDown")));
    } else result = JS_ThrowTypeError(ctx,"Unknown input value operation");
    JS_FreeCString(ctx,op); return result;
}
static JSValue validation_dom(struct web_js_state *s, node_t *n, int argc, JSValueConst *argv) {
    JSContext *ctx=s->ctx; web_doc *d=n->owner?n->owner:s->doc;
    if (argc < 3) return JS_ThrowTypeError(ctx,"Validation operation required");
    const char *op=JS_ToCString(ctx,argv[2]); if(!op) return JS_EXCEPTION;
    bool form=n->type==N_ELEM && !n->foreign && n->tag==T_form;
    bool form_op=!strcmp(op,"formCheck") || !strcmp(op,"formReport");
    JSValue result=JS_UNDEFINED;
    if (!strcmp(op,"submitButton")) result=JS_NewBool(ctx,web_control_submit_button(n));
    else if (form_op) result=form?JS_NewBool(ctx,web_form_check_validity(d,n,!strcmp(op,"formReport"))):JS_ThrowTypeError(ctx,"HTMLFormElement receiver required");
    else if (!strcmp(op,"submission")) result=JS_NewBool(ctx,web_form_submission_validate(d,n));
    else if (!web_control_validation_interface(n)) result=JS_ThrowTypeError(ctx,"Constraint validation interface receiver required");
    else if (!strcmp(op,"state")) result=JS_NewUint32(ctx,web_control_validity(d,n));
    else if (!strcmp(op,"will")) result=JS_NewBool(ctx,web_control_will_validate(n));
    else if (!strcmp(op,"message")) {
        const char *message=web_control_validation_message(d,n);
        size_t length=web_control_will_validate(n)&&n->custom_validity_length?n->custom_validity_length:strlen(message);
        result=JS_NewStringLen(ctx,message,length);
    }
    else if (!strcmp(op,"check") || !strcmp(op,"report")) result=JS_NewBool(ctx,web_control_check_validity(d,n,!strcmp(op,"report")));
    else if (!strcmp(op,"custom")) {
        size_t length=0; const char *text=argc>3?JS_ToCStringLen(ctx,&length,argv[3]):NULL;
        if(!text) result=JS_EXCEPTION;
        else { if(!web_control_set_custom_validity(d,n,text,length)) result=oom(ctx); JS_FreeCString(ctx,text); }
    } else result=JS_ThrowTypeError(ctx,"Unknown validation operation");
    JS_FreeCString(ctx,op); return result;
}
static JSValue control_selection_dom(struct web_js_state *s, node_t *n, int argc, JSValueConst *argv) {
    JSContext *ctx = s->ctx; web_doc *d = n->owner ? n->owner : s->doc;
    if (argc < 4) return JS_ThrowTypeError(ctx, "Selection operation requires an interface and operation");
    const char *brand = JS_ToCString(ctx, argv[2]), *op = JS_ToCString(ctx, argv[3]);
    if (!brand || !op) { JS_FreeCString(ctx, brand); JS_FreeCString(ctx, op); return JS_EXCEPTION; }
    bool valid = n->type == N_ELEM && !n->foreign &&
        ((!strcmp(brand, "input") && n->tag == T_input) || (!strcmp(brand, "textarea") && n->tag == T_textarea));
    JS_FreeCString(ctx, brand);
    JSValue result = JS_UNDEFINED;
    if (!valid) result = JS_ThrowTypeError(ctx, "Text control interface receiver required");
    else {
        doc_control_init(d, n);
        bool supports = doc_control_selection_supported(n);
        if (!strcmp(op, "start")) result = supports ? JS_NewUint32(ctx, n->selection_start) : JS_NULL;
        else if (!strcmp(op, "end")) result = supports ? JS_NewUint32(ctx, n->selection_end) : JS_NULL;
        else if (!strcmp(op, "direction")) result = supports ? JS_NewString(ctx, n->selection_direction == 2 ? "backward" : n->selection_direction == 1 ? "forward" : "none") : JS_NULL;
        else if (!strcmp(op, "select")) {
            const char *type = node_attr(n, "type");
            bool text_ui = supports || (n->tag == T_input && type &&
                (str_ieq(type, "email") || str_ieq(type, "number") || str_ieq(type, "date") ||
                 str_ieq(type, "month") || str_ieq(type, "week") || str_ieq(type, "time") || str_ieq(type, "datetime-local")));
            if (text_ui) doc_control_selection(d, n, 0, UINT32_MAX, 0);
        }
        else if (!strcmp(op, "range")) {
            uint32_t start = 0, end = 0; int32_t direction = 0;
            if (!supports) result = JS_FALSE;
            else if (argc < 7 || JS_ToUint32(ctx, &start, argv[4]) < 0 || JS_ToUint32(ctx, &end, argv[5]) < 0 || JS_ToInt32(ctx, &direction, argv[6]) < 0) result = JS_EXCEPTION;
            else { doc_control_selection(d, n, start, end, (uint8_t)direction); result = JS_TRUE; }
        } else result = JS_ThrowTypeError(ctx, "Unknown selection operation");
    }
    JS_FreeCString(ctx, op); return result;
}
static JSValue native_dom_impl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); web_doc *d = s->doc;
    if (argc < 2) return JS_ThrowTypeError(ctx, "DOM operation requires a receiver");
    const char *op = JS_ToCString(ctx, argv[0]); if (!op) return JS_EXCEPTION;
    node_t *n = unwrap(ctx, argv[1]);
    if (!n && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) { JS_FreeCString(ctx, op); return JS_EXCEPTION; }
    if (n && n->owner) d = n->owner;
    doc_dom_budget(d);
    bool mutation = !strcmp(op, "shadowAttach") || !strcmp(op, "slotAssign") || !strcmp(op, "insert") || !strcmp(op, "remove") || !strcmp(op, "adopt") || !strcmp(op, "clone") || !strcmp(op, "set") ||
                    !strcmp(op, "attrSetNode") || !strcmp(op, "attrRemoveNode") ||
                    ((!strcmp(op, "attr") || !strcmp(op, "style")) && argc > 3) || (!strcmp(op, "attrNS") && argc > 4);
    JSValue ce_token = mutation ? custom_element_hook(s, "customElementBefore", argc, argv) : JS_NULL;
    if (JS_IsException(ce_token)) { JS_FreeCString(ctx, op); return ce_token; }
    JSValue result = JS_UNDEFINED;
    const char *p = NULL;
    node_t *attribute_target = NULL;
    const char *attribute_name = NULL;
    bool old_attribute_present = false;
    char created_attribute_name[128];
    if (mutation && n && n->type == N_ATTR && !strcmp(op, "set") && n->attr_owner && !n->attribute->namespace_uri) {
        attribute_target = n->attr_owner; attribute_name = n->attribute->local ? n->attribute->local : n->attribute->raw;
        old_attribute_present = true;
    }
    if (!strcmp(op, "adopt") && argc > 2) {
        node_t *a = unwrap(ctx, argv[2]);
        if (a && a->type == N_ATTR && a->attr_owner && !a->attribute->namespace_uri) {
            attribute_target = a->attr_owner; attribute_name = a->attribute->local ? a->attribute->local : a->attribute->raw;
            old_attribute_present = true;
        }
    }
    if (!strcmp(op, "isNode")) {
        result = JS_NewBool(ctx, argc > 2 && JS_GetOpaque(argv[2], node_class) != NULL);
    } else if (!strcmp(op, "slotChanges")) {
        web_doc *family = s->doc->dom_family ? s->doc->dom_family : s->doc;
        result = JS_NewArray(ctx); uint32_t index = 0;
        while (!JS_IsException(result) && family->shadow_slots_first) {
            node_t *slot = family->shadow_slots_first;
            JSValue object = wrap(s, slot);
            if (JS_IsException(object) || JS_SetPropertyUint32(ctx, result, index++, object) < 0) {
                JS_FreeValue(ctx, result); result = JS_EXCEPTION; break;
            }
            family->shadow_slots_first = slot->slot_change_next;
            slot->slot_change_pending = false; slot->slot_change_next = NULL;
        }
        if (!family->shadow_slots_first) { family->shadow_slots_last = NULL; family->shadow_slots_pending = false; }
    } else if (!strcmp(op, "parseDocument")) {
        size_t len; const char *html = argc > 2 ? JS_ToCStringLen(ctx, &len, argv[2]) : NULL;
        bool blank = argc > 3 && JS_ToBool(ctx, argv[3]) > 0;
        web_doc *made = html ? doc_inert(s->doc, html, len, blank ? "about:blank" : s->doc->url) : NULL;
        result = !html ? JS_EXCEPTION : made ? wrap(s, made->root) : oom(ctx);
        JS_FreeCString(ctx, html);
    } else if (!strcmp(op, "create")) {
        int32_t type; if (argc < 5 || JS_ToInt32(ctx, &type, argv[2])) result = JS_EXCEPTION;
        else {
            size_t name_len, len;
            const char *name = JS_ToCStringLen(ctx, &name_len, argv[3]); const char *text = JS_ToCStringLen(ctx, &len, argv[4]);
            if (!name || !text) result = JS_EXCEPTION;
            else if (type == 1 && (!*name || strpbrk(name, "<> \t\r\n/"))) result = JS_ThrowTypeError(ctx, "Invalid element name");
            else if (type == 7 && !doc_pi_target_valid(name, name_len)) result = JS_ThrowTypeError(ctx, "Invalid processing instruction target");
            else {
                bool invalid_pi_data = false;
                if (type == 7) for (size_t i = 1; i < len; i++) if (text[i - 1] == '?' && text[i] == '>') { invalid_pi_data = true; break; }
                int native_type = type == 1 ? N_ELEM : type == 3 ? N_TEXT : type == 8 ? N_COMMENT : type == 7 ? N_PI : N_FRAGMENT;
                node_t *made = invalid_pi_data ? NULL : doc_node_create(d, native_type,
                    native_type == N_ELEM || native_type == N_PI ? name : NULL, text, len);
                if (made && argc > 5 && JS_ToBool(ctx, argv[5]) > 0) { made->foreign = true; made->namespace_id = NS_SVG; }
                /* The namespace must be known before establishing HTML-only
                   template contents. SVG <template> has ordinary children. */
                if (made && made->type == N_ELEM && !made->foreign && made->tag == T_template && !doc_template_content(d, made)) made = NULL;
                result = invalid_pi_data ? JS_ThrowTypeError(ctx, "Invalid processing instruction data") : made ? wrap(s, made) : oom(ctx);
                if (made && made->tag == T_script) {
                    struct js_node_ref *r = node_ref(s, made);
                    if (r) { r->force_async_set = true; r->force_async = true; }
                }
            }
            JS_FreeCString(ctx, name); JS_FreeCString(ctx, text);
        }
    } else if (!strcmp(op, "viewport")) { int32_t axis = 0; if (argc > 2) JS_ToInt32(ctx, &axis, argv[2]); result = JS_NewInt32(ctx, axis ? d->height : d->width); }
    else if (!strcmp(op, "focus") || !strcmp(op, "blur")) {
        node_t *old = d->focus;
        bool permitted = true;
        if (!strcmp(op, "blur")) {
            permitted = n && old == n;
            if (n && n->shadow_root && n->shadow_root->shadow_delegates_focus) {
                for (node_t *target = old; target; target = doc_shadow_parent(target))
                    if (target == n->shadow_root) { permitted = true; break; }
            }
            n = NULL;
        }
        if (permitted) web_js_focus_control(d,n);
        d->dirty = true;
    } else if (!n) result = JS_ThrowTypeError(ctx, "Invalid DOM receiver");
    else if (!strcmp(op, "root")) result = wrap(s, doc_node_root(n, argc > 2 && JS_ToBool(ctx, argv[2]) > 0));
    else if (!strcmp(op, "shadowAttach")) {
        if (argc < 7 || !doc_shadow_host_valid(n) || n->shadow_root) result = JS_ThrowTypeError(ctx, "Invalid shadow host");
        else {
            node_t *root = doc_shadow_attach(d, n, JS_ToBool(ctx, argv[2]) > 0, JS_ToBool(ctx, argv[3]) > 0,
                JS_ToBool(ctx, argv[4]) > 0, JS_ToBool(ctx, argv[5]) > 0, JS_ToBool(ctx, argv[6]) > 0);
            result = root ? wrap(s, root) : oom(ctx);
        }
    }
    else if (!strcmp(op, "slotNodes")) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_slot) result = JS_ThrowTypeError(ctx, "HTMLSlotElement receiver required");
        else {
            pvec nodes = {0}; doc_slot_nodes(n, argc > 2 && JS_ToBool(ctx, argv[2]) > 0, &nodes);
            result = JS_NewArray(ctx);
            for (int i = 0; !JS_IsException(result) && i < nodes.n; i++) {
                JSValue object = wrap(s, nodes.v[i]);
                if (JS_IsException(object) || JS_SetPropertyUint32(ctx, result, i, object) < 0) { JS_FreeValue(ctx, result); result = JS_EXCEPTION; }
            }
            pv_free(&nodes);
        }
    }
    else if (!strcmp(op, "slotAssign")) {
        int count = argc - 2; node_t **nodes = count ? js_malloc(ctx, (size_t)count * sizeof *nodes) : NULL;
        bool valid = n->type == N_ELEM && !n->foreign && n->tag == T_slot;
        if (count && !nodes) result = JS_EXCEPTION;
        else {
            for (int i = 0; i < count; i++) {
                nodes[i] = JS_GetOpaque(argv[i + 2], node_class);
                if (!nodes[i] || (nodes[i]->type != N_ELEM && nodes[i]->type != N_TEXT)) valid = false;
            }
            result = !valid ? JS_ThrowTypeError(ctx, "Slot assignment requires Elements or Text") :
                doc_slot_assign(n, nodes, count) ? JS_UNDEFINED : oom(ctx);
        }
        js_free(ctx, nodes);
    }
    else if (!strcmp(op, "attrCreate")) {
        const char *ns = argc > 2 && !JS_IsNull(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
        const char *prefix = argc > 3 && !JS_IsNull(argv[3]) ? JS_ToCString(ctx, argv[3]) : NULL;
        const char *local = argc > 4 ? JS_ToCString(ctx, argv[4]) : NULL;
        if (n->type != N_DOC) result = JS_ThrowTypeError(ctx, "Document receiver required");
        else if (!local || (argc > 2 && !JS_IsNull(argv[2]) && !ns) || (argc > 3 && !JS_IsNull(argv[3]) && !prefix)) result = JS_EXCEPTION;
        else { node_t *made = doc_attr_create(d, ns, prefix, local, ""); result = made ? wrap(s, made) : oom(ctx); }
        JS_FreeCString(ctx, ns); JS_FreeCString(ctx, prefix); JS_FreeCString(ctx, local);
    }
    else if (!strcmp(op, "attrList")) {
        if (n->type != N_ELEM) result = JS_ThrowTypeError(ctx, "Element receiver required");
        else {
            result = JS_NewArray(ctx);
            for (int i = 0; !JS_IsException(result) && i < n->nattrs; i++) {
                node_t *a = doc_attr_node(d, n, i);
                if (!a || JS_SetPropertyUint32(ctx, result, (uint32_t)i, wrap(s, a)) < 0) {
                    JS_FreeValue(ctx, result); result = a ? JS_EXCEPTION : oom(ctx); break;
                }
            }
        }
    }
    else if (!strcmp(op, "attrNode") || !strcmp(op, "attrNodeNS") || !strcmp(op, "attrNS")) {
        bool namespaced = strcmp(op, "attrNode") != 0;
        const char *ns = namespaced && argc > 2 && !JS_IsNull(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
        const char *name = argc > (namespaced ? 3 : 2) ? JS_ToCString(ctx, argv[namespaced ? 3 : 2]) : NULL;
        if (n->type != N_ELEM) result = JS_ThrowTypeError(ctx, "Element receiver required");
        else if (!name || (namespaced && argc > 2 && !JS_IsNull(argv[2]) && !ns)) result = JS_EXCEPTION;
        else {
            int i = doc_attr_index(n, ns, name, namespaced);
            if (!strcmp(op, "attrNS") && argc > 4) {
                if (!ns || !*ns) {
                    attribute_target = n; old_attribute_present = i >= 0;
                    if (i >= 0) attribute_name = n->attrs[i].local ? n->attrs[i].local : n->attrs[i].raw;
                    else { snprintf(created_attribute_name, sizeof created_attribute_name, "%s", name); attribute_name = created_attribute_name; }
                }
                const char *value = JS_IsNull(argv[4]) ? NULL : JS_ToCString(ctx, argv[4]);
                const char *prefix = argc > 5 && !JS_IsNull(argv[5]) ? JS_ToCString(ctx, argv[5]) : NULL;
                result = (!JS_IsNull(argv[4]) && !value) || (argc > 5 && !JS_IsNull(argv[5]) && !prefix) ? JS_EXCEPTION :
                    (value ? doc_attr_set_ns(d, n, ns, prefix, name, value) : doc_attr_remove(d, n, i)) ? JS_UNDEFINED : oom(ctx);
                JS_FreeCString(ctx, value); JS_FreeCString(ctx, prefix);
            } else if (!strcmp(op, "attrNS")) result = str_or_null(ctx, i >= 0 ? n->attrs[i].value : NULL);
            else { node_t *a = i >= 0 ? doc_attr_node(d, n, i) : NULL; result = i >= 0 && !a ? oom(ctx) : wrap(s, a); }
        }
        JS_FreeCString(ctx, ns); JS_FreeCString(ctx, name);
    }
    else if (!strcmp(op, "attrSetNode") || !strcmp(op, "attrRemoveNode")) {
        node_t *a = argc > 2 ? unwrap(ctx, argv[2]) : NULL;
        if (!a || a->type != N_ATTR || n->type != N_ELEM) result = JS_ThrowTypeError(ctx, "Element and Attr required");
        else if (!strcmp(op, "attrRemoveNode")) {
            int i = a->attr_owner == n ? (int)(a->attribute - n->attrs) : -1;
            if (i >= 0 && !a->attribute->namespace_uri) {
                attribute_target = n; attribute_name = a->attribute->local ? a->attribute->local : a->attribute->raw; old_attribute_present = true;
            }
            result = i < 0 ? JS_ThrowTypeError(ctx, "Attribute is not owned by this element") :
                doc_attr_remove(d, n, i) ? wrap(s, a) : oom(ctx);
        } else {
            struct attr *data = a->attribute;
            int i = doc_attr_index(n, data->namespace_uri, data->local ? data->local : data->raw, true);
            node_t *old = i >= 0 ? doc_attr_node(d, n, i) : NULL;
            if (old != a && !data->namespace_uri) {
                attribute_target = n; attribute_name = data->local ? data->local : data->raw; old_attribute_present = old != NULL;
            }
            result = i >= 0 && !old ? oom(ctx) : doc_attr_set_node(d, n, a) ? wrap(s, old) : oom(ctx);
        }
    }
    else if (!strcmp(op, "selection")) result = control_selection_dom(s, n, argc, argv);
    else if (!strcmp(op, "formValue")) result = form_value_dom(s, n, argc, argv);
    else if (!strcmp(op, "validation")) result = validation_dom(s, n, argc, argv);
    else if (!strcmp(op,"observerGeometry")) {
        node_t *root=argc>2 && !JS_IsNull(argv[2])?unwrap(ctx,argv[2]):NULL;
        result=argc>2 && !JS_IsNull(argv[2]) && !root?JS_EXCEPTION:observer_geometry(s,n,root);
    }
    else if (!strcmp(op, "imageDecode")) result = image_decode_promise(s, n);
    else if (!strcmp(op, "insert")) {
        node_t *child = argc > 2 ? unwrap(ctx, argv[2]) : NULL, *before = argc > 3 ? unwrap(ctx, argv[3]) : NULL;
        if (!child || child->type == N_DOC || (n->type != N_ELEM && n->type != N_DOC && n->type != N_FRAGMENT)) result = JS_ThrowTypeError(ctx, "HierarchyRequestError");
        else if (before && before->parent != n) result = JS_ThrowTypeError(ctx, "NotFoundError");
        else {
            bool cycle = false; for (node_t *a = n; a; a = a->parent ? a->parent : a->shadow_host ? a->shadow_host : a->template_host) if (a == child) cycle = true;
            result = cycle ? JS_ThrowTypeError(ctx, "HierarchyRequestError") : doc_node_move(d, n, child, before) ? JS_UNDEFINED : oom(ctx);
        }
    } else if (!strcmp(op, "remove")) doc_node_remove(d, n);
    else if (!strcmp(op, "equal") || !strcmp(op, "same")) {
        node_t *other = argc > 2 ? unwrap(ctx, argv[2]) : NULL;
        if (argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2]) && !other) result = JS_EXCEPTION;
        else result = JS_NewBool(ctx, !strcmp(op, "equal") ? equal_nodes(n, other) : n == other);
    }
    else if (!strcmp(op, "adopt") || !strcmp(op, "import")) {
        node_t *child = argc > 2 ? unwrap(ctx, argv[2]) : NULL;
        if (n->type != N_DOC || !child || child->type == N_DOC) result = JS_ThrowTypeError(ctx, "Invalid document node transfer");
        else if (!strcmp(op, "adopt")) result = doc_node_adopt(d, child) ? wrap(s, child) : oom(ctx);
        else { node_t *copy = doc_node_clone(d, child, argc > 3 && JS_ToBool(ctx, argv[3]) > 0); result = copy ? wrap(s, copy) : oom(ctx); }
    }
    else if (!strcmp(op, "clone")) {
        bool deep = argc > 2 && JS_ToBool(ctx, argv[2]) > 0;
        if (n->type == N_DOC) {
            web_doc *made = doc_inert(s->doc, "", 0, d->url);
            if (!made) result = oom(ctx);
            else {
                while (made->root->first) doc_node_remove(made, made->root->first);
                bool ok = true; made->quirks = d->quirks;
                for (node_t *c = deep ? n->first : NULL; c && ok; c = c->next) {
                    node_t *copy = doc_node_clone(made, c, true);
                    ok = copy && doc_node_move(made, made->root, copy, NULL);
                }
                result = ok ? wrap(s, made->root) : oom(ctx);
            }
        } else { node_t *copy = doc_node_clone(d, n, deep); result = copy ? wrap(s, copy) : oom(ctx); }
    }
    else if (!strcmp(op, "customCandidates")) {
        if (argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) p = JS_ToCString(ctx, argv[2]);
        result = argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2]) && !p ? JS_EXCEPTION : custom_candidates(s, n, p);
    }
    else if (!strcmp(op, "rect")) {
        flush_layout(s);
        int x = 0, y = 0, w = 0, h = 0;
        bool visible = connected(s, n);
        if (visible) web_node_rect(d, n, &x, &y, &w, &h);
        int sx = 0, sy = 0;
        if (s->host.scroll) s->host.scroll(s->host.opaque, &sx, &sy);
        if (visible) { x -= sx; y -= sy; }
        result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "x", JS_NewInt32(ctx, x)); JS_SetPropertyStr(ctx, result, "y", JS_NewInt32(ctx, y));
        JS_SetPropertyStr(ctx, result, "left", JS_NewInt32(ctx, x)); JS_SetPropertyStr(ctx, result, "top", JS_NewInt32(ctx, y));
        JS_SetPropertyStr(ctx, result, "right", JS_NewInt32(ctx, x + w)); JS_SetPropertyStr(ctx, result, "bottom", JS_NewInt32(ctx, y + h));
        JS_SetPropertyStr(ctx, result, "width", JS_NewInt32(ctx, w)); JS_SetPropertyStr(ctx, result, "height", JS_NewInt32(ctx, h));
    } else if (!strcmp(op, "submit")) {
        char *url = NULL, *body = NULL;
        if (!d->inert && web_submit(d, n, &url, &body)) { if (s->host.navigate) s->host.navigate(s->host.opaque, url, body); free(url); free(body); }
    } else if (!strcmp(op, "reset")) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_form) result = JS_ThrowTypeError(ctx, "HTMLFormElement receiver required");
        else result = doc_form_reset(d, n) ? JS_UNDEFINED : oom(ctx);
    } else if (argc < 3 || !(p = JS_ToCString(ctx, argv[2]))) result = JS_EXCEPTION;
    else if (!strcmp(op, "get")) result = get_dom(s, n, p);
    else if (!strcmp(op, "set")) result = argc > 3 ? set_dom(s, n, p, argv[3]) : JS_ThrowTypeError(ctx, "Missing DOM value");
    else if (!strcmp(op, "attr")) {
        if (n->type != N_ELEM) result = JS_ThrowTypeError(ctx, "Attributes require an element");
        else if (argc == 3) {
            const char *value = NULL;
            int index = doc_attr_index(n, NULL, p, false);
            if (index >= 0) value = n->attrs[index].value;
            result = str_or_null(ctx, value);
        }
        else {
            int index = doc_attr_index(n, NULL, p, false);
            if (index >= 0 && !n->attrs[index].namespace_uri) {
                attribute_target = n; attribute_name = n->attrs[index].local ? n->attrs[index].local : n->attrs[index].raw;
                old_attribute_present = true;
            } else if (index < 0) {
                snprintf(created_attribute_name, sizeof created_attribute_name, "%s", p);
                if (!n->foreign) for (char *q = created_attribute_name; *q; q++) *q = (char)lower((unsigned char)*q);
                attribute_target = n; attribute_name = created_attribute_name;
            }
            const char *v = JS_IsNull(argv[3]) ? NULL : JS_ToCString(ctx, argv[3]);
            result = !JS_IsNull(argv[3]) && !v ? JS_EXCEPTION : doc_node_attr(d, n, p, v) ? JS_UNDEFINED : oom(ctx);
            JS_FreeCString(ctx, v);
        }
    } else if (!strcmp(op, "id")) result = wrap(s, find_id(n, p));
    else if (!strcmp(op, "query")) {
        pvec found = {0};
        if (!css_select(d, n, p, &found)) result = JS_ThrowSyntaxError(ctx, "Invalid CSS selector");
        else if (argc > 3 && JS_ToBool(ctx, argv[3]) > 0) result = wrap(s, found.n ? found.v[0] : NULL);
        else { result = JS_NewArray(ctx); for (int i = 0; i < found.n; ++i) if (JS_SetPropertyUint32(ctx, result, i, wrap(s, found.v[i])) < 0) { JS_FreeValue(ctx, result); result = JS_EXCEPTION; break; } }
        pv_free(&found);
    } else if (!strcmp(op, "matches")) { bool valid; bool matches = css_matches(n, p, &valid); result = valid ? JS_NewBool(ctx, matches) : JS_ThrowSyntaxError(ctx, "Invalid CSS selector"); }
    else if (!strcmp(op, "style")) result = style_access(s, n, p, argc > 3, argc > 3 ? argv[3] : JS_UNDEFINED, argc > 4 ? argv[4] : JS_UNDEFINED);
    else if (!strcmp(op, "computed")) result = computed(s, n, p);
    else if (!strcmp(op, "geometry")) result = geometry(s, n, p);
    else result = JS_ThrowTypeError(ctx, "Unknown DOM operation");
    if (!JS_IsException(result) && attribute_target && attribute_name) {
        JSValue notified = attribute_native_changed(s, attribute_target, attribute_name, old_attribute_present);
        if (JS_IsException(notified)) { JS_FreeValue(ctx, result); result = notified; } else JS_FreeValue(ctx, notified);
    }
    if (!JS_IsException(result) && !JS_IsNull(ce_token)) {
        JSValue args[] = {ce_token, result};
        JSValue notified = custom_element_hook(s, "customElementAfter", 2, args);
        if (JS_IsException(notified)) { JS_FreeValue(ctx, result); result = notified; }
        else JS_FreeValue(ctx, notified);
    }
    JS_FreeValue(ctx, ce_token);
    if (!JS_IsException(result) && mutation) signal_slots(s);
    if (!JS_IsException(result) && n && !strcmp(op, "insert") && connected(s, n)) {
        dynamic_scripts(s, n);
        for (struct js_script *script = s->scripts; script; script = script->next)
            if (script->dynamic && script->ready && !script->executed && !script->module && !node_attr(script->node, "src")) run_script(s, script);
    }
    if (!JS_IsException(result) && n && n->tag == T_script && !n->script_started && connected(s, n) && (!strcmp(op, "attr") || !strcmp(op, "set"))) {
        struct js_script *script = queue_script(s, n, true);
        if (script && script->ready && !script->module && !node_attr(n, "src")) run_script(s, script);
    }
    if (!JS_IsException(result) && attribute_target && attribute_target->tag == T_script && !attribute_target->script_started && connected(s, attribute_target)) {
        struct js_script *script = queue_script(s, attribute_target, true);
        if (script && script->ready && !script->module && !node_attr(attribute_target, "src")) run_script(s, script);
    }
    JS_FreeCString(ctx, p); JS_FreeCString(ctx, op); return result;
}

static JSValue native_dom(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    bool outer = s->profile_dom_depth++ == 0;
    uint64_t start = outer ? uptime_ms() : 0;
    s->doc->profile.native_calls++;
    JSValue result = native_dom_impl(ctx, this_val, argc, argv);
    s->profile_dom_depth--;
    if (outer) s->doc->profile.native_ms += uptime_ms() - start;
    return result;
}

static bool make_origin(const char *url, char *out, size_t size) {
    struct url u;
    if (!url_parse(url, &u)) { snprintf(out, size, "null"); return false; }
    if (u.port == (u.tls ? 443 : 80)) snprintf(out, size, "%s://%s", u.tls ? "https" : "http", u.host);
    else snprintf(out, size, "%s://%s:%u", u.tls ? "https" : "http", u.host, u.port);
    return true;
}
static bool permitted_url(struct web_js_state *s, const char *url, bool fetch) {
    if (!strncasecmp(url, "https://", 8) || !strncasecmp(url, "http://", 7)) {
        struct url parsed; return url_parse(url, &parsed) && !strchr(parsed.host, '@');
    }
    if (fetch || strncasecmp(url, "file://", 7) || strncasecmp(s->doc->url, "file://", 7)) return false;
    /* Resolution already removes dot segments. Encoded path separators and dot
       segments must not create a second, more privileged interpretation. */
    const char *u = url + 7, *base = s->doc->url + 7;
    if (*u != '/' || strchr(u, '%') || strchr(u, '\\')) return false;
    const char *slash = strrchr(base, '/');
    return slash && !strncmp(u, base, (size_t)(slash + 1 - base));
}
static JSValue native_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); int32_t level = 0;
    if (argc) JS_ToInt32(ctx, &level, argv[0]);
    sbuf text = {0};
    for (int i = 1; i < argc; i++) {
        const char *p = JS_ToCString(ctx, argv[i]);
        if (!p) { sb_free(&text); return JS_EXCEPTION; }
        if (i > 1) sb_putc(&text, ' '); sb_puts(&text, p); JS_FreeCString(ctx, p);
        if (JS_IsError(ctx, argv[i])) {
            JSValue stack = JS_GetPropertyStr(ctx, argv[i], "stack");
            if (JS_IsException(stack)) { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); }
            else if (JS_IsString(stack)) {
                const char *where = JS_ToCString(ctx, stack);
                if (where) { sb_putc(&text, '\n'); sb_puts(&text, where); JS_FreeCString(ctx, where); }
                else { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); }
            }
            JS_FreeValue(ctx, stack);
        }
        if (text.n > 16384) { text.n = 16384; break; }
    }
    log_text(s, level, sb_cstr(&text)); sb_free(&text); return JS_UNDEFINED;
}
static JSValue native_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, (double)(uptime_ms() - state(ctx)->now));
}
static JSValue native_screen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct n_sysinfo info;
    if (sysinfo(&info) < 0) return JS_ThrowInternalError(ctx, "Could not read desktop dimensions");
    int available_w = (int)info.fb_w, available_h = (int)info.fb_h;
    if (screen_size(&available_w, &available_h) < 0) { available_w = (int)info.fb_w; available_h = (int)info.fb_h; }
    JSValue result = JS_NewArray(ctx);
    if (JS_IsException(result)) return result;
    uint32_t dimensions[] = {info.fb_w, info.fb_h, available_w > 0 ? (uint32_t)available_w : 0,
                             available_h > 0 ? (uint32_t)available_h : 0};
    for (uint32_t i = 0; i < 4; i++) if (JS_SetPropertyUint32(ctx, result, i, JS_NewUint32(ctx, dimensions[i])) < 0) {
        JS_FreeValue(ctx, result); return JS_EXCEPTION;
    }
    return result;
}
static JSValue native_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return JS_NewString(ctx, state(ctx)->doc->url); }
static JSValue native_origin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char out[256]; make_origin(state(ctx)->doc->url, out, sizeof out); return JS_NewString(ctx, out);
}
static JSValue native_class_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewUint32(ctx, argc ? JS_GetClassID(argv[0]) : 0);
}
static JSValue native_detach(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc) JS_DetachArrayBuffer(ctx, argv[0]);
    return JS_UNDEFINED;
}
static JSValue native_pack(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t size = 0;
    uint8_t *data = JS_WriteObject(ctx, &size, argc ? argv[0] : JS_UNDEFINED, JS_WRITE_OBJ_REFERENCE);
    if (!data) return JS_EXCEPTION;
    if (size > 1024u * 1024u) { js_free(ctx, data); return JS_ThrowRangeError(ctx, "History state exceeds 1 MiB"); }
    JSValue out = JS_NewArrayBufferCopy(ctx, data, size); js_free(ctx, data); return out;
}
static JSValue native_unpack(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t size = 0; uint8_t *data = argc ? JS_GetArrayBuffer(ctx, &size, argv[0]) : NULL;
    if (!data) return JS_EXCEPTION;
    return JS_ReadObject(ctx, data, size, JS_READ_OBJ_REFERENCE);
}
static JSValue native_history(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    int32_t op = 0, value = 0;
    if ((argc && JS_ToInt32(ctx, &op, argv[0])) || (argc > 3 && JS_ToInt32(ctx, &value, argv[3]))) return JS_EXCEPTION;
    if (!s->host.history) return JS_ThrowTypeError(ctx, "Embedder has no session history");
    const char *url = argc > 1 && !JS_IsNull(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    if (argc > 1 && !JS_IsNull(argv[1]) && !url) return JS_EXCEPTION;
    size_t len = 0; uint8_t *data = NULL;
    if (argc > 2 && !JS_IsNull(argv[2])) {
        data = JS_GetArrayBuffer(ctx, &len, argv[2]);
        if (!data) { JS_FreeCString(ctx, url); return JS_EXCEPTION; }
    }
    struct web_history result = {0};
    bool ok = len <= (1024u * 1024u) && s->host.history(s->host.opaque, op, url, data, len, value, &result);
    JS_FreeCString(ctx, url);
    if (!ok) return JS_ThrowTypeError(ctx, "Session history operation failed");
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "entry", JS_NewInt64(ctx, result.entry));
    JS_SetPropertyStr(ctx, out, "length", JS_NewInt32(ctx, result.length));
    JS_SetPropertyStr(ctx, out, "manual", JS_NewBool(ctx, result.manual_scroll));
    JS_SetPropertyStr(ctx, out, "data", op == WEB_HISTORY_INFO && value && result.state_len ? JS_NewArrayBufferCopy(ctx, result.state, result.state_len) : JS_NULL);
    return out;
}
static JSValue native_canvas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Canvas node and operation required");
    node_t *node = unwrap(ctx, argv[0]);
    if (!node) return JS_ThrowTypeError(ctx, "Native Canvas receiver required");
    return web_canvas_native(ctx, node, argc - 1, argv + 1);
}

static JSValue native_avmedia(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Media operation and receiver required");
    const char *op = JS_ToCString(ctx, argv[0]);
    if (!op) return JS_EXCEPTION;
    node_t *node = !strcmp(op, "type") ? NULL : unwrap(ctx, argv[1]);
    if (!node && strcmp(op, "type")) {
        JS_FreeCString(ctx, op);
        return JS_ThrowTypeError(ctx, "Native media receiver required");
    }
    JSValue result = web_avmedia_call(ctx, state(ctx)->doc, node, op, argc - 2, argv + 2);
    JS_FreeCString(ctx, op);
    return result;
}

static JSValue native_media(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_ThrowTypeError(ctx, "Media query is required");
    size_t len; const char *query = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!query) return JS_EXCEPTION;
    if (len > 16384 || memchr(query, 0, len)) { JS_FreeCString(ctx, query); return JS_ThrowRangeError(ctx, "Media query exceeds parser limits"); }
    struct web_js_state *s = state(ctx); bool serialize = argc > 1 && JS_ToBool(ctx, argv[1]);
    size_t cap = len * 9 + 16; char *text = serialize ? js_malloc(ctx, cap) : NULL;
    if (serialize && !text) { JS_FreeCString(ctx, query); return JS_EXCEPTION; }
    bool matches = css_media_evaluate(query, s->doc->width, s->doc->height, web_js_enabled(s->doc), text, cap);
    JS_FreeCString(ctx, query);
    if (!serialize) return JS_NewBool(ctx, matches);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "matches", JS_NewBool(ctx, matches));
    JS_SetPropertyStr(ctx, result, "media", JS_NewString(ctx, text)); js_free(ctx, text); return result;
}
static JSValue native_observers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    s->observers_active=argc && JS_ToBool(ctx,argv[0])>0;
    s->observers_force=s->observers_active;
    if(!s->observer_due)s->observer_due=uptime_ms();
    return JS_UNDEFINED;
}
static JSValue native_cookie(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if (argc) {
        const char *v = JS_ToCString(ctx, argv[0]); if (!v) return JS_EXCEPTION;
        if (s->host.cookie_set) s->host.cookie_set(s->host.opaque, s->doc->url, v);
        JS_FreeCString(ctx, v); return JS_UNDEFINED;
    }
    char *value = s->host.cookie_get ? s->host.cookie_get(s->host.opaque, s->doc->url) : NULL;
    JSValue out = JS_NewString(ctx, value ? value : ""); free(value); return out;
}
/* Returns [status, result]. DOMException creation belongs to the private JS
 * binding, not a replaceable public constructor. Never accepts an origin/path
 * from page code; even after argument conversion the current native URL wins. */
static JSValue native_storage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    int32_t kind=0, op=0; uint32_t index=0;
    if (argc<2) return JS_ThrowTypeError(ctx,"Storage kind/operation required");
    if (JS_ToInt32(ctx,&kind,argv[0])<0 || JS_ToInt32(ctx,&op,argv[1])<0) return JS_EXCEPTION;
    if (op==WEB_STORAGE_KEY) {
        if (argc<5) return JS_ThrowTypeError(ctx,"Storage index required");
        if (JS_ToUint32(ctx,&index,argv[4])<0) return JS_EXCEPTION;
    }
    struct web_storage_request request={.kind=kind,.operation=op,.index=index};
    const char *key=NULL,*value=NULL;
    if (op==WEB_STORAGE_GET || op==WEB_STORAGE_SET || op==WEB_STORAGE_REMOVE) {
        if (argc<3) return JS_ThrowTypeError(ctx,"Storage key required");
        key=JS_ToCStringLen2(ctx,&request.key_len,argv[2],true);
        if (!key) return JS_EXCEPTION;
        request.key=key;
    }
    if (op==WEB_STORAGE_SET) {
        if (argc<4) { JS_FreeCString(ctx,key); return JS_ThrowTypeError(ctx,"Storage value required"); }
        value=JS_ToCStringLen2(ctx,&request.value_len,argv[3],true);
        if (!value) { JS_FreeCString(ctx,key); return JS_EXCEPTION; }
        request.value=value;
    }
    JSValue fn=JS_GetPropertyStr(ctx,s->hooks,"storageOrigin"), result=JS_EXCEPTION;
    JSValue origin_value=JS_UNDEFINED, argument=JS_UNDEFINED;
    const char *origin=NULL;
    struct web_storage_result out={0}; int status=WEB_STORAGE_SECURITY;
    if (JS_IsException(fn)) goto done;
    if (!JS_IsFunction(ctx,fn)) { JS_ThrowInternalError(ctx,"Missing private storage origin parser"); goto done; }
    argument=JS_NewString(ctx,s->doc->url);
    if (JS_IsException(argument)) goto done;
    origin_value=JS_Call(ctx,fn,s->hooks,1,&argument);
    if (JS_IsException(origin_value)) goto done;
    if (!JS_IsNull(origin_value)) {
        size_t len=0; origin=JS_ToCStringLen(ctx,&len,origin_value);
        if (!origin) goto done;
        if (len && len<2048 && len==strlen(origin) && s->host.storage)
            status=s->host.storage(s->host.opaque,origin,&request,&out);
    }
    result=JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    JSValue payload=JS_NULL;
    if (status==WEB_STORAGE_OK) {
        if (op==WEB_STORAGE_LENGTH) payload=JS_NewUint32(ctx,out.length);
        else if (out.text) payload=JS_NewStringLen(ctx,out.text,out.text_len);
    }
    if (JS_IsException(payload) || JS_SetPropertyUint32(ctx,result,0,JS_NewInt32(ctx,status))<0) {
        JS_FreeValue(ctx,payload); JS_FreeValue(ctx,result); result=JS_EXCEPTION;
    } else if (JS_SetPropertyUint32(ctx,result,1,payload)<0) { JS_FreeValue(ctx,result); result=JS_EXCEPTION; }
done:
    free(out.text); JS_FreeCString(ctx,origin); JS_FreeValue(ctx,origin_value);
    JS_FreeValue(ctx,argument); JS_FreeValue(ctx,fn); JS_FreeCString(ctx,key); JS_FreeCString(ctx,value);
    return result;
}
static JSValue native_scroll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); int x = 0, y = 0;
    if (argc > 1) {
        int32_t a, b;
        if (JS_ToInt32(ctx, &a, argv[0]) || JS_ToInt32(ctx, &b, argv[1])) return JS_EXCEPTION;
        if (s->host.scroll_to) s->host.scroll_to(s->host.opaque, a, b);
        return JS_UNDEFINED;
    }
    if (s->host.scroll) s->host.scroll(s->host.opaque, &x, &y);
    int32_t axis = 0; if (argc && JS_ToInt32(ctx, &axis, argv[0])) return JS_EXCEPTION;
    return JS_NewInt32(ctx, axis ? y : x);
}
static JSValue native_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_UNDEFINED;
    const char *rel = JS_ToCString(ctx, argv[0]); if (!rel) return JS_EXCEPTION;
    struct web_js_state *s = state(ctx); if (s->doc->resources_dirty) doc_sync_tree(s->doc);
    char url[2048]; bool ok = url_resolve(s->doc->base, rel, url, sizeof url);
    JS_FreeCString(ctx, rel); return ok ? JS_NewString(ctx, url) : JS_ThrowTypeError(ctx, "Invalid URL");
}
static JSValue native_ready(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    return JS_NewString(ctx, s->load_sent ? "complete" : s->parsing_done ? "interactive" : "loading");
}
static JSValue native_current(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return wrap(state(ctx), state(ctx)->current_script); }
static JSValue native_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if (!s->parser_write || !s->doc->parser || s->parsing_done) return JS_ThrowTypeError(ctx, "document.write is only available in a parser-blocking classic script");
    if (!argc) return JS_UNDEFINED;
    size_t n; const char *text = JS_ToCStringLen(ctx, &n, argv[0]); if (!text) return JS_EXCEPTION;
    bool ok = html_write(s->doc->parser, text, n); JS_FreeCString(ctx, text);
    return ok ? JS_UNDEFINED : oom(ctx);
}
static JSValue native_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_NewArrayBufferCopy(ctx, NULL, 0);
    size_t n; const char *text = JS_ToCStringLen(ctx, &n, argv[0]); if (!text) return JS_EXCEPTION;
    JSValue v = JS_NewArrayBufferCopy(ctx, (const uint8_t *)text, n); JS_FreeCString(ctx, text); return v;
}
static JSValue native_inline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_UNDEFINED;
    size_t n; const char *text = JS_ToCStringLen(ctx, &n, argv[0]); if (!text) return JS_EXCEPTION;
    if (n > 1024u * 1024u) { JS_FreeCString(ctx, text); return JS_ThrowRangeError(ctx, "Inline event handler is too large"); }
    sbuf b = {0}; sb_puts(&b, "(function(event){\n"); sb_put(&b, text, n); sb_puts(&b, "\n})");
    JSValue result = JS_Eval(ctx, sb_cstr(&b), b.n, state(ctx)->doc->url, JS_EVAL_TYPE_GLOBAL);
    sb_free(&b); JS_FreeCString(ctx, text); return result;
}
static JSValue native_navigate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_UNDEFINED;
    struct web_js_state *s = state(ctx); const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION;
    if (s->doc->resources_dirty) doc_sync_tree(s->doc);
    char url[2048]; bool ok = url_resolve(s->doc->base, p, url, sizeof url) && permitted_url(s, url, false);
    JS_FreeCString(ctx, p);
    if (!ok) return JS_ThrowTypeError(ctx, "Navigation URL is not permitted");
    int32_t mode = 0;
    if (argc > 1 && JS_ToInt32(ctx, &mode, argv[1])) return JS_EXCEPTION;
    if (s->host.navigate_mode) s->host.navigate_mode(s->host.opaque, url, mode);
    else if (s->host.navigate) s->host.navigate(s->host.opaque, url, NULL);
    return JS_UNDEFINED;
}
static JSValue native_click(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); node_t *n = argc ? unwrap(ctx, argv[0]) : NULL;
    if (!n) return JS_EXCEPTION;
    if (n->owner && n->owner->inert) {
        JSValue args[] = {argv[0], JS_NewString(ctx, "click")};
        JSValue value = custom_element_hook(s, "inertClick", 2, args);
        JS_FreeValue(ctx, args[1]); return value;
    }
    if (web_control_disabled(n)) return JS_UNDEFINED;
    struct web_event e = {.type="click", .bubbles=true, .cancelable=true};
    if (!web_js_dispatch(s->doc, n, &e)) return JS_UNDEFINED;
    if (s->doc->resources_dirty) doc_rescan(s->doc);
    node_t *disclosure=doc_details_activation(n);
    if (disclosure) doc_details_toggle(s->doc,disclosure);
    else if (n->tag == T_audio || n->tag == T_video) web_media_activate(s->doc, n);
    else if (n->tag == T_a) {
        const char *href = doc_link_href(s->doc, n);
        if (href && permitted_url(s, href, false) && s->host.navigate) s->host.navigate(s->host.opaque, href, NULL);
    } else if (n->tag == T_input) {
        const char *type = node_attr(n, "type");
        if (type && (str_ieq(type, "checkbox") || str_ieq(type, "radio"))) {
            web_toggle(s->doc, n); e.type = "input"; e.cancelable = false; web_js_dispatch(s->doc, n, &e); e.type = "change"; web_js_dispatch(s->doc, n, &e); s->doc->dirty = true;
        } else if (type && (str_ieq(type, "submit") || str_ieq(type, "image"))) {
            node_t *form = web_form_owner(s->doc, n); e.type = "submit";
            e.submitter=n;
            if (form && web_form_submission_validate(s->doc,n) && web_js_dispatch(s->doc, form, &e)) {
                char *url = NULL, *body = NULL; if (web_submit(s->doc, n, &url, &body)) { if (s->host.navigate) s->host.navigate(s->host.opaque, url, body); free(url); free(body); }
            }
        } else if (type && str_ieq(type,"reset")) web_reset(s->doc,n);
    } else if (n->tag == T_button) {
        const char *type = node_attr(n, "type"); node_t *form = web_form_owner(s->doc, n);
        if (type && str_ieq(type,"reset")) web_reset(s->doc,n);
        else if (form && web_control_submit_button(n)) { e.type = "submit"; e.submitter=n; if (web_form_submission_validate(s->doc,n) && web_js_dispatch(s->doc, form, &e)) { char *url = NULL, *body = NULL; if (web_submit(s->doc, n, &url, &body)) { if (s->host.navigate) s->host.navigate(s->host.opaque, url, body); free(url); free(body); } } }
    }
    return JS_UNDEFINED;
}
static JSValue native_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); int32_t kind = 0; double delay = 0;
    if (argc < 4 || JS_ToInt32(ctx, &kind, argv[0]) || !JS_IsFunction(ctx, argv[1]) || JS_ToFloat64(ctx, &delay, argv[2])) return JS_ThrowTypeError(ctx, "Invalid timer");
    if (!isfinite(delay) || delay < 0) delay = 0;
    if (delay > 2147483647.0) delay = 2147483647.0;
    uint64_t ms = (uint64_t)delay; if (kind == 1 && ms < 4) ms = 4; if (kind == 2) ms = 16;
    for (int i = 0; i < JS_TIMERS; ++i) if (!s->timers[i].id) {
        struct js_timer *t = &s->timers[i];
        uint32_t id = ++s->next_timer; if (!id) id = ++s->next_timer;
        t->id = id; t->kind = kind; t->interval = ms; t->due = uptime_ms() + ms;
        t->fn = JS_DupValue(ctx, argv[1]); t->args = JS_DupValue(ctx, argv[3]);
        return JS_NewUint32(ctx, id);
    }
    return JS_ThrowRangeError(ctx, "The document's 128 timer limit was reached");
}
/* Posted messages are tasks, not Promise jobs or clamped timers. Each callback
   receives the usual watchdog and microtask checkpoint; navigation frees it. */
static JSValue native_post_task(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected posted task callback");
    if (s->posted_count >= 4096) return JS_ThrowRangeError(ctx, "Document posted task limit reached");
    struct js_posted_task *p = js_malloc(ctx, sizeof *p); if (!p) return JS_EXCEPTION;
    p->fn = JS_DupValue(ctx, argv[0]); p->next = NULL;
    p->id = ++s->next_posted; if (!p->id) p->id = ++s->next_posted;
    if (s->last_posted) s->last_posted->next = p; else s->posted = p;
    s->last_posted = p; s->posted_count++;
    return JS_NewUint32(ctx, p->id);
}
static JSValue native_cancel_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); uint32_t id;
    if (!argc || JS_ToUint32(ctx, &id, argv[0])) return JS_EXCEPTION;
    struct js_posted_task **link = &s->posted, *previous = NULL;
    while (*link) {
        struct js_posted_task *p = *link;
        if (p->id == id) {
            *link = p->next; if (s->last_posted == p) s->last_posted = previous;
            s->posted_count--; JS_FreeValue(ctx, p->fn); js_free(ctx, p); break;
        }
        previous = p; link = &p->next;
    }
    return JS_UNDEFINED;
}
static JSValue native_css_supports(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_ThrowTypeError(ctx, "CSS.supports requires an argument");
    size_t an, bn = 0;
    const char *a = JS_ToCStringLen(ctx, &an, argv[0]); if (!a) return JS_EXCEPTION;
    const char *b = argc > 1 ? JS_ToCStringLen(ctx, &bn, argv[1]) : NULL;
    if (argc > 1 && !b) { JS_FreeCString(ctx, a); return JS_EXCEPTION; }
    bool result = b ? css_supports_declaration(a, an, b, bn) : css_supports_condition(a, an, true);
    JS_FreeCString(ctx, a); if (b) JS_FreeCString(ctx, b);
    return JS_NewBool(ctx, result);
}
static JSValue native_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    uint32_t id = 0; if (argc && JS_ToUint32(ctx, &id, argv[0])) return JS_EXCEPTION;
    struct web_js_state *s = state(ctx);
    for (int i = 0; i < JS_TIMERS; i++) if (s->timers[i].id == id && id) { JS_FreeValue(ctx, s->timers[i].fn); JS_FreeValue(ctx, s->timers[i].args); memset(&s->timers[i], 0, sizeof s->timers[i]); break; }
    return JS_UNDEFINED;
}
static JSValue microtask_job(JSContext *ctx, int argc, JSValueConst *argv) {
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}
static JSValue native_microtask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected callback");
    /* A browser microtask is a host job, not a call through the page's mutable
       Promise constructor. Promise polyfills themselves use queueMicrotask;
       implementing it with Promise.resolve causes recursive scheduling. The
       engine retains the callback until execution or runtime destruction. */
    if (JS_EnqueueJob(ctx, microtask_job, 1, argv) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}
static struct js_pending *pending_new(struct web_js_state *s, int kind, const char *url) {
    if (s->pending_count >= JS_REQUESTS) return NULL;
    struct js_pending *p = js_mallocz(s->ctx, sizeof *p); if (!p) return NULL;
    p->url = js_strdup(s->ctx, url);
    if (!p->url) { js_free(s->ctx, p); return NULL; }
    p->id = ++s->next_request; p->kind = kind; p->deadline = uptime_ms() + 30000;
    p->resolve = p->reject = JS_UNDEFINED; p->next = s->pending; s->pending = p; s->pending_count++;
    return p;
}
static void pending_error(struct js_pending *p, const char *error) { p->done = true; snprintf(p->response.error, sizeof p->response.error, "%s", error); }
static bool send_request(struct web_js_state *s, struct js_pending *p, int kind, const char *method,
                         const char *headers, const void *body, size_t len) {
    struct web_request r = {.id=p->id, .kind=kind, .url=p->url, .method=method, .headers=headers, .body=body, .body_len=len,
        .credentials=kind == WEB_RESOURCE_FETCH ? p->credentials : kind == WEB_RESOURCE_MODULE ? 1 : 2,
        .force_preflight=p->force_preflight, .redirect_error=p->redirect_error, .same_origin=p->same_origin,
        .cache_mode=p->cache_mode};
    bool ok = s->host.request && s->host.request(s->host.opaque, &r);
    if (!ok) pending_error(p, "The browser rejected the resource request");
    return ok;
}
static JSValue native_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if (argc < 4) return JS_ThrowTypeError(ctx, "Invalid fetch request");
    const char *rel = JS_ToCString(ctx, argv[0]), *method = JS_ToCString(ctx, argv[1]), *headers = JS_ToCString(ctx, argv[2]);
    size_t n = 0; const char *body_string = NULL;
    const void *body;
    if (JS_IsString(argv[3])) body = body_string = JS_ToCStringLen(ctx, &n, argv[3]);
    else body = JS_GetArrayBuffer(ctx, &n, argv[3]);
    char url[2048];
    JSValue result = JS_EXCEPTION;
    if (!rel || !method || !headers || (!body && JS_HasException(ctx))) goto out;
    if (s->doc->resources_dirty) doc_sync_tree(s->doc);
    if (!url_resolve(s->doc->base, rel, url, sizeof url) || !permitted_url(s, url, true)) { result = JS_ThrowTypeError(ctx, "fetch supports HTTP(S) resources only"); goto out; }
    if (!webnet_method_valid(method) || ((!strcmp(method, "GET") || !strcmp(method, "HEAD")) && n)) { result = JS_ThrowTypeError(ctx, "Invalid HTTP method or body"); goto out; }
    if (n > JS_BODY_LIMIT || strlen(headers) > 8192) { result = JS_ThrowRangeError(ctx, "Request exceeds the browser's size limit"); goto out; }
    bool same_origin = argc > 4 && JS_ToBool(ctx, argv[4]) > 0;
    if (same_origin) { char a[256], b[256]; make_origin(s->doc->url, a, sizeof a); make_origin(url, b, sizeof b); if (strcmp(a, b)) { result = JS_ThrowTypeError(ctx, "Cross-origin fetch is forbidden by same-origin mode"); goto out; } }
    int32_t credentials = 1, redirect = 0, cache_mode = WEBNET_CACHE_DEFAULT;
    if ((argc > 5 && (JS_ToInt32(ctx, &credentials, argv[5]) || credentials < 0 || credentials > 2)) ||
        (argc > 7 && (JS_ToInt32(ctx, &redirect, argv[7]) || redirect < 0 || redirect > 1)) ||
        (argc > 8 && (JS_ToInt32(ctx, &cache_mode, argv[8]) || cache_mode < WEBNET_CACHE_DEFAULT || cache_mode > WEBNET_CACHE_ONLY_IF_CACHED))) {
        result = JS_ThrowTypeError(ctx, "Invalid fetch policy"); goto out;
    }
    /* No HTTP response cache is present. Never turn a cache-only miss into I/O,
       even if a caller bypasses the bootstrap's early rejection. */
    if (cache_mode == WEBNET_CACHE_ONLY_IF_CACHED) { result = JS_ThrowTypeError(ctx, "No cached response is available"); goto out; }
    struct js_pending *p = pending_new(s, P_FETCH, url);
    if (!p) { result = oom(ctx); goto out; }
    p->credentials = credentials;
    p->cache_mode = cache_mode;
    p->same_origin = same_origin;
    p->redirect_error = redirect == 1;
    p->force_preflight = argc > 6 && JS_ToBool(ctx, argv[6]) > 0;
    JSValue funcs[2]; JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { pending_error(p, "Out of memory"); goto out; }
    p->resolve = funcs[0]; p->reject = funcs[1];
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) { JS_FreeValue(ctx, promise); pending_error(p, "Out of memory"); goto out; }
    JS_SetPropertyStr(ctx, result, "id", JS_NewInt64(ctx, (int64_t)p->id));
    JS_SetPropertyStr(ctx, result, "promise", promise);
    send_request(s, p, WEB_RESOURCE_FETCH, method, headers, body, n);
out:
    JS_FreeCString(ctx, rel); JS_FreeCString(ctx, method); JS_FreeCString(ctx, headers); JS_FreeCString(ctx, body_string); return result;
}
static JSValue native_cancel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int64_t id; if (!argc || JS_ToInt64(ctx, &id, argv[0])) return JS_EXCEPTION;
    struct web_js_state *s = state(ctx);
    for (struct js_pending *p = s->pending; p; p = p->next) if (p->id == (uint64_t)id && p->kind == P_FETCH && !p->done) {
        if (s->host.cancel) s->host.cancel(s->host.opaque, p->id);
        p->aborted = true; pending_error(p, "The operation was aborted"); break;
    }
    return JS_UNDEFINED;
}

static JSValue module_bridge_call(struct web_js_state *s, const char *hook, const char *input, size_t len, const char *base) {
    JSContext *ctx = s->ctx;
    JSValue fn = JS_GetPropertyStr(ctx, s->hooks, hook);
    if (JS_IsException(fn)) return fn;
    if (!JS_IsFunction(ctx, fn)) { JS_FreeValue(ctx, fn); return JS_ThrowInternalError(ctx, "Missing module loader hook: %s", hook); }
    JSValue args[] = {JS_NewStringLen(ctx, input, len), JS_NewString(ctx, base)};
    JSValue result = JS_IsException(args[0]) || JS_IsException(args[1]) ? JS_EXCEPTION : JS_Call(ctx, fn, s->hooks, 2, args);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, fn);
    return result;
}
static char *module_url(struct web_js_state *s, const char *hook, const char *name, const char *base) {
    JSValue value = module_bridge_call(s, hook, name, strlen(name), base);
    if (JS_IsException(value)) return NULL;
    size_t len; const char *url = JS_ToCStringLen(s->ctx, &len, value);
    char *result = NULL;
    if (url && len < 2048 && len == strlen(url) && permitted_url(s, url, false)) result = js_strdup(s->ctx, url);
    else if (url) JS_ThrowTypeError(s->ctx, "Module URL is not permitted or exceeds the URL limit: %s", name);
    JS_FreeCString(s->ctx, url); JS_FreeValue(s->ctx, value); return result;
}
static char *normalize_module(JSContext *ctx, const char *base, const char *name, void *opaque) {
    struct web_js_state *s = opaque;
    /* An entry script's src is a URL, not an import specifier. Consume this
       bypass before linking so every dependency still goes through the map. */
    if (s->module_entry_url && !strcmp(s->module_entry_url, name)) {
        s->module_entry_url = NULL; return js_strdup(ctx, name);
    }
    const char *resolve_base = base && *base ? base : s->doc->base;
    for (struct js_module *m = s->modules; m; m = m->next) if (!strcmp(m->url, resolve_base)) { resolve_base = m->base; break; }
    return module_url(s, "resolveModule", name, resolve_base);
}
/* The complete MIME Sniffing Standard JavaScript essence set. In particular,
   application/x-javascript is still valid for modules; it is not plain text. */
static bool javascript_essence(const char *value, size_t len) {
    static const char *const types[] = {
        "application/ecmascript", "application/javascript",
        "application/x-ecmascript", "application/x-javascript",
        "text/ecmascript", "text/javascript", "text/javascript1.0",
        "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
        "text/javascript1.4", "text/javascript1.5", "text/jscript",
        "text/livescript", "text/x-ecmascript", "text/x-javascript"
    };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++)
        if (strn_ieq(value, types[i], len)) return true;
    return false;
}
static bool mime_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool mime_token(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("!#$%&'*+-.^_`|~", c));
}
static bool valid_mime_essence(const char *v, const char *end) {
    const char *slash = NULL;
    for (const char *p = v; p < end; p++) {
        if (*p == '/' && !slash) slash = p;
        else if (!mime_token((unsigned char)*p)) return false;
    }
    return slash && slash != v && slash + 1 != end;
}
static bool javascript_mime(const char *raw) {
    bool matched = false;
    for (const char *p = raw; *p;) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        if ((size_t)(e - p) >= 13 && !strncasecmp(p, "content-type:", 13)) {
            p += 13;
            /* Fetch's get/decode/split honors quoted commas. Each successfully
               parsed member replaces the previous MIME type; malformed members
               and the wildcard type do not. */
            while (p < e) {
                const char *v = p, *essence_end = NULL; bool quoted = false;
                while (p < e) {
                    if (quoted && *p == '\\' && p + 1 < e) { p += 2; continue; }
                    if (*p == '"') quoted = !quoted;
                    if (!quoted && *p == ',') break;
                    if (!quoted && *p == ';' && !essence_end) essence_end = p;
                    p++;
                }
                const char *end = essence_end ? essence_end : p;
                while (v < end && mime_space((unsigned char)*v)) v++;
                while (end > v && mime_space((unsigned char)end[-1])) end--;
                if (valid_mime_essence(v, end) && !strn_ieq(v, "*/*", (size_t)(end - v)))
                    matched = javascript_essence(v, (size_t)(end - v));
                if (p < e) p++;
            }
        }
        p = *e ? e + 1 : e;
    }
    return matched;
}
/* Only the host's explicit source-preparation boundary gets this allowance.
   COMPILE_ONLY does not run page code. Recursive module preparation is counted
   once, and the sum is retained until the outer task (including jobs) ends.
   QuickJS's parser is not fully interruptible: also check the elapsed time on
   return, before executing any resulting bytecode. Body/heap limits still apply. */
static JSValue compile_source(struct web_js_state *s, const char *source, size_t len, const char *url, int kind) {
    if (interrupt(s->rt, s)) return JS_ThrowInternalError(s->ctx, "JavaScript execution was stopped");
    if (len > JS_BODY_LIMIT) return JS_ThrowRangeError(s->ctx, "JavaScript source exceeds the 16 MiB limit");
    bool outer = s->compiling++ == 0;
    uint64_t before = uptime_ms(), saved = s->task_deadline, waited = s->compile_wait_ms;
    if (outer) s->task_deadline = before + (s->task_compile_ms < JS_COMPILE_MS ? JS_COMPILE_MS - s->task_compile_ms : 0);
    JSValue result = JS_Eval(s->ctx, source, len, url, kind | JS_EVAL_FLAG_COMPILE_ONLY);
    if (outer) {
        uint64_t elapsed = uptime_ms() - before;
        s->task_compile_ms += elapsed - (s->compile_wait_ms - waited);
        bool exceeded = interrupt(s->rt, s) != 0 || s->task_compile_ms >= JS_COMPILE_MS;
        s->task_deadline = saved + elapsed;
        if (exceeded) {
            s->disabled = s->timed_out = s->task_timed_out = true;
            s->compile_timed_out = true;
            JS_FreeValue(s->ctx, result);
            result = JS_ThrowInternalError(s->ctx, "JavaScript source preparation was stopped");
        }
    }
    s->compiling--;
    return result;
}
static void set_import_meta(JSContext *ctx, JSValueConst module, const char *url) {
    JSModuleDef *m = JS_VALUE_GET_PTR(module);
    JSValue meta = JS_GetImportMeta(ctx, m);
    if (!JS_IsException(meta)) JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, url));
    JS_FreeValue(ctx, meta);
}
static struct js_module *cache_module(struct web_js_state *s, const char *url, const char *source, size_t len, const char *base) {
    for (struct js_module *m = s->modules; m; m = m->next) if (!strcmp(m->url, url)) return m;
    struct js_module *m = js_mallocz(s->ctx, sizeof *m); if (!m) return NULL;
    m->compiled = m->error = JS_UNDEFINED;
    m->url = js_strdup(s->ctx, url); m->base = js_strdup(s->ctx, base ? base : url); m->source = js_strndup(s->ctx, source ? source : "", len);
    if (!m->url || !m->base || !m->source) { js_free(s->ctx, m->url); js_free(s->ctx, m->base); js_free(s->ctx, m->source); js_free(s->ctx, m); return NULL; }
    m->len = len; m->next = s->modules; s->modules = m; return m;
}
static JSModuleDef *load_module(JSContext *ctx, const char *url, void *opaque) {
    struct web_js_state *s = opaque;
    if (interrupt(s->rt, s)) { JS_ThrowInternalError(ctx, "JavaScript execution was stopped"); return NULL; }
    if (!permitted_url(s, url, false)) { JS_ThrowReferenceError(ctx, "Module URL is not permitted"); return NULL; }
    struct js_module *m = NULL;
    for (m = s->modules; m; m = m->next) if (!strcmp(m->url, url)) break;
    if (!m) {
        struct web_response r = {0}; uint64_t before = uptime_ms();
        bool ok = s->host.sync_load && s->host.sync_load(s->host.opaque, url, WEB_RESOURCE_MODULE, &r);
        /* Native UI/network wait is not JavaScript execution. The synchronous
           loader is the one place this adjustment is allowed. */
        uint64_t waited = uptime_ms() - before;
        if (s->running) s->task_deadline += waited;
        if (s->compiling) s->compile_wait_ms += waited;
        bool local = !strncasecmp(url, "file://", 7);
        if (!ok || r.error[0] || r.status < 200 || r.status >= 300 || r.body_len > JS_BODY_LIMIT ||
            strlen(web_response_headers(&r)) >= WEB_RESPONSE_HEADERS_MAX ||
            (!local && !javascript_mime(web_response_headers(&r))) || (r.url[0] && !permitted_url(s, r.url, false))) {
            JS_ThrowReferenceError(ctx, "Cannot load JavaScript module %s: %s", url, r.error[0] ? r.error : "invalid response or JavaScript MIME type"); web_response_free(&r); return NULL;
        }
        char *final = r.url[0] ? module_url(s, "moduleURL", r.url, url) : js_strdup(ctx, url);
        m = final ? cache_module(s, url, r.body, r.body_len, final) : NULL;
        js_free(ctx, final);
        web_response_free(&r); if (!m) return NULL;
    }
    if (!JS_IsUndefined(m->error)) { JS_Throw(ctx, JS_DupValue(ctx, m->error)); return NULL; }
    if (JS_IsUndefined(m->compiled)) {
        JSValue compiled = compile_source(s, m->source, m->len, m->url, JS_EVAL_TYPE_MODULE);
        if (JS_IsException(compiled)) {
            m->error = JS_GetException(ctx); JS_Throw(ctx, JS_DupValue(ctx, m->error)); return NULL;
        }
        set_import_meta(ctx, compiled, m->base); m->compiled = compiled;
    }
    return JS_VALUE_GET_PTR(m->compiled);
}
static void prepare_module_script(struct web_js_state *s, struct js_script *script) {
    if (!script->module || !script->ready || script->failed || !script->source) return;
    /* Module graphs are resolved when prepared, not when later deferred
       execution reaches them. A following import map cannot rewrite this
       graph. COMPILE_ONLY links dependencies but never evaluates page code. */
    begin_task(s);
    struct js_module *m = cache_module(s, script->url, script->source, script->len, script->base);
    if (!m || !load_module(s->ctx, script->url, s)) { script->failed = true; exception(s); }
    end_task(s);
}
static bool script_type(node_t *n, bool *module) {
    const char *type = node_attr(n, "type"); *module = type && str_ieq(type, "module");
    if (*module) return true;
    if (node_attr(n, "nomodule")) return false;
    if (!type || !*type) return true;
    while (is_space((unsigned char)*type)) type++;
    size_t len = strlen(type); while (len && is_space((unsigned char)type[len - 1])) len--;
    return javascript_essence(type, len);
}
static struct js_script *queue_script(struct web_js_state *s, node_t *n, bool dynamic) {
    if (n->script_started) return NULL;
    if (s->doc->resources_dirty) doc_sync_tree(s->doc);
    const char *src = node_attr(n, "src");
    const char *type = node_attr(n, "type");
    bool importmap = type && str_ieq(type, "importmap");
    if (!src || (!*src && !importmap)) {
        bool text = false;
        for (node_t *c = n->first; c; c = c->next) if (c->type == N_TEXT && c->textlen) { text = true; break; }
        if (!text) return NULL;
    }
    n->script_started = true;
    if (importmap) {
        if (s->disabled || node_ancestor(n, T_template)) return NULL;
        if (src) { log_text(s, 2, "Import maps must be inline; src is not permitted"); script_event(s, n, true); return NULL; }
        sbuf text = {0}; node_text_content(n, &text);
        begin_task(s);
        JSValue value = text.n > JS_BODY_LIMIT ? JS_ThrowRangeError(s->ctx, "Import map exceeds the 16 MiB limit") :
            module_bridge_call(s, "registerImportMap", text.p ? text.p : "", text.n, s->doc->base);
        if (JS_IsException(value)) exception(s); else JS_FreeValue(s->ctx, value);
        end_task(s); sb_free(&text); return NULL;
    }
    bool module;
    if (s->disabled || !script_type(n, &module) || node_ancestor(n, T_template)) return NULL;
    if (s->script_count >= 1024) { log_text(s, 2, "The document's 1024 script limit was reached"); return NULL; }
    struct js_script *script = js_mallocz(s->ctx, sizeof *script);
    if (!script) { exception(s); return NULL; }
    script->evaluation = JS_UNDEFINED; script->node = n; script->module = module; script->dynamic = dynamic;
    bool force_async = dynamic;
    struct js_node_ref *r = node_ref(s, n);
    if (r && r->force_async_set) force_async = r->force_async;
    /* Capture preparation-time state for both classic and module scripts. */
    script->asynchronous = (force_async || node_attr(n, "async")) && (module || (src && *src));
    script->deferred = !dynamic && !script->asynchronous && (module || (src && *src && node_attr(n, "defer")));
    char url[2048] = {0};
    if (src && *src) {
        if (module) {
            begin_task(s); script->url = module_url(s, "moduleURL", src, s->doc->base);
            if (!script->url) { script->ready = script->failed = true; exception(s); }
            end_task(s);
            if (script->url) snprintf(url, sizeof url, "%s", script->url);
        } else {
            if (!url_resolve(s->doc->base, src, url, sizeof url) || !permitted_url(s, url, false)) { script->ready = script->failed = true; log_text(s, 2, "Script URL is not permitted"); }
            script->url = js_strdup(s->ctx, url);
        }
        if (!script->failed) {
            struct js_pending *p = pending_new(s, P_SCRIPT, url);
            if (!p) script->ready = script->failed = true;
            else { p->script = script; script->request = p->id; send_request(s, p, module ? WEB_RESOURCE_MODULE : WEB_RESOURCE_SCRIPT, "GET", NULL, NULL, 0); }
        }
    } else {
        if (module) snprintf(url, sizeof url, "%s#nocturne-inline-module-%d", s->doc->url, s->script_count + 1);
        else snprintf(url, sizeof url, "%s", s->doc->url);
        if (module) {
            begin_task(s); script->url = module_url(s, "moduleURL", url, s->doc->base);
            if (!script->url) exception(s);
            end_task(s);
        } else script->url = js_strdup(s->ctx, url);
        sbuf b = {0}; node_text_content(n, &b);
        script->source = js_strndup(s->ctx, b.p ? b.p : "", b.n); script->len = b.n; sb_free(&b);
        script->ready = true; if (!script->source) script->failed = true;
    }
    if (module && script->url && !script->failed) {
        begin_task(s); script->base = module_url(s, "moduleURL", src && *src ? script->url : s->doc->base, s->doc->base);
        if (!script->base) { script->ready = script->failed = true; exception(s); }
        end_task(s);
    }
    if (!script->url) {
        js_free(s->ctx, script->base); js_free(s->ctx, script->source); js_free(s->ctx, script);
        if (JS_HasException(s->ctx)) exception(s);
        script_event(s, n, true); return NULL;
    }
    prepare_module_script(s, script);
    if (s->last_script) s->last_script->next = script; else s->scripts = script;
    s->last_script = script; s->script_count++;
    return script;
}
static void dynamic_scripts_walk(struct web_js_state *s, node_t *n) {
    if (n->shadow_root) dynamic_scripts_walk(s, n->shadow_root);
    for (node_t *c = n->first; c; c = c->next) {
        s->doc->profile.script_visits++;
        if (c->type != N_ELEM || c->tag == T_template) continue;
        if (c->tag == T_script && !c->script_started) queue_script(s, c, true);
        dynamic_scripts_walk(s, c);
    }
}
static void dynamic_scripts(struct web_js_state *s, node_t *n) {
    uint64_t start = uptime_ms();
    s->doc->profile.script_scans++;
    dynamic_scripts_walk(s, n);
    s->doc->profile.script_ms += uptime_ms() - start;
}
static bool script_finished(struct web_js_state *s, struct js_script *script) {
    if (!script->executed) return false;
    if (JS_IsUndefined(script->evaluation)) return true;
    return JS_PromiseState(s->ctx, script->evaluation) != JS_PROMISE_PENDING;
}
static struct js_resource_event *script_event(struct web_js_state *s, node_t *n, bool failed) {
    if (s->disabled) return NULL;
    struct js_resource_event *e = js_mallocz(s->ctx, sizeof *e);
    if (!e) { JSValue error = JS_GetException(s->ctx); JS_FreeValue(s->ctx, error); return NULL; }
    e->node = n; e->failed = failed;
    if (s->last_event) s->last_event->next = e; else s->events = e;
    s->last_event = e;
    return e;
}
void web_js_selection_changed(web_doc *d, node_t *n) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->disabled) return;
    struct js_resource_event *event = script_event(s, n, false);
    if (event) event->selection = true;
}
static void run_script(struct web_js_state *s, struct js_script *script) {
    if (script->executed || !script->ready) return;
    script->executed = true;
    if (s->disabled || script->failed || !script->source) { if (!s->disabled) { script_event(s, script->node, true); script->notified = true; } return; }
    begin_task(s);
    node_t *old_current = s->current_script; bool old_write = s->parser_write;
    s->current_script = script->module ? NULL : script->node;
    s->parser_write = !script->module && s->blocker == script && !s->parsing_done;
    uint64_t source_start = uptime_ms(), compiled_at = source_start;
    JSValue result;bool compilation_failed=false;
    if (script->module) {
        struct js_module *m = cache_module(s, script->url, script->source, script->len, script->base);
        s->module_entry_url = script->url;
        result = m ? JS_LoadModule(s->ctx, s->doc->base, script->url) : JS_EXCEPTION;
        s->module_entry_url = NULL;
    } else {
        result = compile_source(s, script->source, script->len, script->url, JS_EVAL_TYPE_GLOBAL);
        compiled_at = uptime_ms();
        compilation_failed=JS_IsException(result);
        if (!JS_IsException(result)) result = JS_EvalFunction(s->ctx, result);
    }
    if (s->timed_out || uptime_ms() - source_start >= 250) {
        char message[192];
        snprintf(message, sizeof message, "Script task profile: %lu bytes, compile %lu ms, execute %lu ms, layout %lu ms in %u reads%s",
                 (unsigned long)script->len, (unsigned long)(compiled_at - source_start),
                 (unsigned long)(uptime_ms() - compiled_at), (unsigned long)s->task_layout_ms, s->task_layout_flushes,compilation_failed?", compilation failed":"");
        log_text(s, 0, message);
    }
    s->current_script = old_current; s->parser_write = old_write;
    if (JS_IsException(result)) { exception(s); script->failed = true; }
    else if (script->module && JS_IsObject(result)) script->evaluation = result;
    else JS_FreeValue(s->ctx, result);
    end_task(s);
    /* Inline classic scripts have no resource load event. */
    if (!script->module && node_attr(script->node, "src")) { script_event(s, script->node, script->failed); script->notified = true; }
    js_free(s->ctx, script->source); script->source = NULL;
}
static void pending_free(struct web_js_state *s, struct js_pending *p) {
    JS_FreeValue(s->ctx, p->resolve); JS_FreeValue(s->ctx, p->reject);
    js_free(s->ctx, p->url); js_free(s->ctx, p->response.body); js_free(s->ctx, p->response.headers_full);
    js_free(s->ctx, p); s->pending_count--;
}
void web_js_loaded(web_doc *d, uint64_t id, const struct web_response *response) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || !response) return;
    for (struct js_pending *p = s->pending; p; p = p->next) if (p->id == id && !p->done) {
        p->response = *response; p->response.body = NULL; p->response.headers_full = NULL;
        const char *headers = web_response_headers(response);
        size_t hn = strlen(headers);
        if (hn >= WEB_RESPONSE_HEADERS_MAX) { pending_error(p, "Resource response headers exceed the native limit"); return; }
        if (hn < sizeof p->response.headers) memcpy(p->response.headers, headers, hn + 1);
        else {
            p->response.headers[0] = 0;
            p->response.headers_full = js_strndup(s->ctx, headers, hn);
            if (!p->response.headers_full) {
                JSValue e = JS_GetException(s->ctx); JS_FreeValue(s->ctx, e);
                pending_error(p, "Resource headers exceeded the JavaScript heap limit"); return;
            }
        }
        if (response->body_len > JS_BODY_LIMIT || (response->body_len && !response->body)) pending_error(p, "Resource exceeds the 16 MiB limit or has an invalid body");
        else {
            p->response.body = js_malloc(s->ctx, response->body_len + 1);
            if (!p->response.body) { JSValue e = JS_GetException(s->ctx); JS_FreeValue(s->ctx, e); pending_error(p, "Resource buffering exceeded the JavaScript heap limit"); }
            else { if (response->body_len) memcpy(p->response.body, response->body, response->body_len); p->response.body[response->body_len] = 0; p->done = true; }
        }
        return;
    }
}
static bool process_pending(struct web_js_state *s) {
    struct js_pending **link = &s->pending;
    uint64_t now = uptime_ms();
    bool ran_task = false;
    while (*link) {
        struct js_pending *p = *link;
        if (!p->done && now >= p->deadline) { if (s->host.cancel) s->host.cancel(s->host.opaque, p->id); pending_error(p, "Resource request exceeded its 30 second deadline"); }
        if (!p->done) { link = &p->next; continue; }
        if (p->kind == P_FETCH && ran_task && !s->disabled) { link = &p->next; continue; }
        *link = p->next;
        bool ok = !p->response.error[0] && p->response.status >= 200 && p->response.status < 300;
        if (p->kind == P_SCRIPT) {
            struct js_script *script = p->script;
            if (script) {
                bool local = !strncasecmp(p->url, "file://", 7);
                if (script->module && !local && !javascript_mime(web_response_headers(&p->response))) ok = false;
                script->ready = true; script->failed = !ok;
                if (ok) {
                    script->source = p->response.body; p->response.body = NULL; script->len = p->response.body_len;
                    if (p->response.url[0]) {
                        /* Module-map identity stays the requested URL. The
                           redirect URL is only the relative-import/meta base. */
                        char *final;
                        if (script->module) { begin_task(s); final = module_url(s, "moduleURL", p->response.url, script->url); end_task(s); }
                        else final = js_strdup(s->ctx, p->response.url);
                        if (final) {
                            char **slot = script->module ? &script->base : &script->url;
                            js_free(s->ctx, *slot); *slot = final;
                        } else { script->failed = true; exception(s); }
                    }
                } else {
                    char message[2304];
                    snprintf(message,sizeof message,"Script failed: HTTP %d, %s: %s",p->response.status,
                        p->response.error[0]?p->response.error:script->module && !javascript_mime(web_response_headers(&p->response))?"invalid module MIME type":"unsuccessful response",p->url);
                    log_text(s,2,message);
                }
                prepare_module_script(s, script);
            }
        } else if (p->kind == P_CSS) doc_css_loaded(s->doc, p->url, p->response.url[0] ? p->response.url : p->url, ok ? p->response.body : NULL, ok ? p->response.body_len : 0);
        else if (p->kind == P_IMAGE) {
            web_image_loaded(s->doc, p->image, ok ? p->response.body : NULL, ok ? p->response.body_len : 0);
            s->doc->dirty = true; s->doc->need_style = true;
        } else if (!s->disabled && p->kind == P_FETCH) {
            ran_task = true;
            begin_task(s);
            JSValue value;
            bool success = !p->response.error[0];
            if (success) {
                JSValue bytes = JS_NewArrayBufferCopy(s->ctx, (const uint8_t *)(p->response.body ? p->response.body : ""), p->response.body_len);
                JSValue args[] = {JS_NewInt32(s->ctx, p->response.status), JS_NewString(s->ctx, p->response.url[0] ? p->response.url : p->url), JS_NewString(s->ctx, web_response_headers(&p->response)), JS_NewStringLen(s->ctx, p->response.body ? p->response.body : "", p->response.body_len), bytes, JS_NewBool(s->ctx, p->response.url[0] && strcmp(p->url, p->response.url))};
                value = JS_Call(s->ctx, s->response, JS_UNDEFINED, 6, args);
                for (int i = 0; i < 6; ++i) JS_FreeValue(s->ctx, args[i]);
                if (JS_IsException(value)) { exception(s); success = false; value = JS_NewString(s->ctx, "Could not construct fetch response"); }
            } else { JSValue args[] = {JS_NewString(s->ctx, p->response.error), JS_NewBool(s->ctx, p->aborted)}; value = JS_Call(s->ctx, s->reject, JS_UNDEFINED, 2, args); JS_FreeValue(s->ctx, args[0]); JS_FreeValue(s->ctx, args[1]); }
            JSValue r = JS_Call(s->ctx, success ? p->resolve : p->reject, JS_UNDEFINED, 1, &value);
            JS_FreeValue(s->ctx, value); if (JS_IsException(r)) exception(s); else JS_FreeValue(s->ctx, r);
            end_task(s);
        }
        pending_free(s, p);
    }
    return ran_task;
}

static bool pending_kind(struct web_js_state *s, int kind) {
    for (struct js_pending *p = s->pending; p; p = p->next) if (p->kind == kind) return true;
    return false;
}
static bool dispatch_resource_event(struct web_js_state *s) {
    struct js_resource_event *e = s->events;
    if (!e) return false;
    s->events = e->next; if (!s->events) s->last_event = NULL;
    if (e->image && (e->node->image_invalidated || e->node->image_generation != e->generation)) {
        js_free(s->ctx, e); return false;
    }
    struct web_event event = {.type=e->selection ? "select" : e->failed ? "error" : "load", .bubbles=e->selection};
    web_js_dispatch(s->doc, e->node, &event); js_free(s->ctx, e); return true;
}
static void image_events(struct web_js_state *s) {
    if (s->disabled) return;
    for (node_t *n = doc_image_node_next(s->doc, NULL); n; n = doc_image_node_next(s->doc, n)) {
        bool in_template = false;
        for (node_t *p = n; p; p = p->parent ? p->parent : p->template_host) {
            if (p->tag == T_template || p->template_host) { in_template = true; break; }
        }
        if (in_template) continue;
        bool img = html_image(n);
        if ((!img && (n->tag != T_input || !connected(s, n))) || n->foreign) continue;
        int selected = img ? n->image_request : n->image;
        if (img && (!n->image_initialized || !n->image_has_source || n->image_invalidated)) continue;
        if (!img && (selected < 0 || selected >= s->doc->images.n)) continue;
        struct web_image *image = selected >= 0 && selected < s->doc->images.n ? s->doc->images.v[selected] : NULL;
        if (image && !image->done) continue;
        JSValue object = wrap(s, n);
        if (JS_IsException(object)) { JSValue error = JS_GetException(s->ctx); JS_FreeValue(s->ctx, error); return; }
        JS_FreeValue(s->ctx, object);
        struct js_node_ref *r = node_ref(s, n);
        if (r) {
            if (r->image != selected || (img && r->image_generation != n->image_generation)) {
                r->image = selected; r->image_generation = n->image_generation; r->image_notified = false;
            }
            if (!r->image_notified) {
                struct js_resource_event *event = script_event(s, n, !image || image->failed);
                if (event) { event->image = img; event->generation = n->image_generation; r->image_notified = true; }
            }
        }
    }
}
static bool image_decode_ready(struct web_js_state *s, struct js_image_decode *p) {
    if (p->node->image_generation != p->generation || p->node->image_request < 0) return true;
    return ((struct web_image *)s->doc->images.v[p->node->image_request])->done;
}
static bool dispatch_image_decode(struct web_js_state *s) {
    struct js_image_decode **link = &s->image_decodes;
    struct js_image_decode *previous = NULL;
    while (*link && !image_decode_ready(s, *link)) { previous = *link; link = &(*link)->next; }
    struct js_image_decode *p = *link; if (!p) return false;
    *link = p->next;
    if (s->last_image_decode == p) s->last_image_decode = previous;
    struct web_image *image = p->node->image_request >= 0 ? s->doc->images.v[p->node->image_request] : NULL;
    bool failed = p->node->image_generation != p->generation || !image || image->failed || !image->img;
    begin_task(s);
    JSValue value = failed ? custom_element_hook(s, "imageError", 0, NULL) : JS_UNDEFINED;
    if (JS_IsException(value)) value = JS_GetException(s->ctx);
    {
        JSValue result = JS_Call(s->ctx, failed ? p->reject : p->resolve, JS_UNDEFINED, 1, &value);
        if (JS_IsException(result)) exception(s); else JS_FreeValue(s->ctx, result);
        JS_FreeValue(s->ctx, value);
    }
    JS_FreeValue(s->ctx, p->resolve); JS_FreeValue(s->ctx, p->reject); js_free(s->ctx, p);
    end_task(s); return true;
}
static void request_styles(struct web_js_state *s) {
    if (!s->ctx) return;
    if (s->doc->resources_dirty) doc_rescan(s->doc);
    const char *url = web_pending_stylesheet(s->doc);
    if (!url || pending_kind(s, P_CSS)) return;
    struct js_pending *p = pending_new(s, P_CSS, url);
    if (!p) {
        if (s->pending_count < JS_REQUESTS) { JSValue e = JS_GetException(s->ctx); JS_FreeValue(s->ctx, e); doc_css_loaded(s->doc, url, url, NULL, 0); }
        return;
    }
    send_request(s, p, WEB_RESOURCE_CSS, "GET", NULL, NULL, 0);
}
static void request_images(struct web_js_state *s) {
    if (!s->ctx) return;
    for (int i = 0; i < web_image_count(s->doc) && s->pending_count < 8; ++i) {
        if (!web_image_wanted(s->doc, i)) continue;
        bool pending = false;
        for (struct js_pending *p = s->pending; p; p = p->next) if (p->kind == P_IMAGE && p->image == i) { pending = true; break; }
        if (pending) continue;
        const char *url = web_image_url(s->doc, i); if (!url) continue;
        struct js_pending *p = pending_new(s, P_IMAGE, url);
        if (!p) { JSValue e = JS_GetException(s->ctx); JS_FreeValue(s->ctx, e); web_image_loaded(s->doc, i, NULL, 0); s->doc->dirty = true; continue; }
        p->image = i; send_request(s, p, WEB_RESOURCE_IMAGE, "GET", NULL, NULL, 0);
    }
}
static bool styles_busy(struct web_js_state *s) { return pending_kind(s, P_CSS) || web_pending_stylesheet(s->doc); }
static bool run_timer(struct web_js_state *s, uint64_t now) {
    struct js_timer *timer = NULL;
    for (int i = 0; i < JS_TIMERS; i++) if (s->timers[i].id && s->timers[i].due <= now && (!timer || s->timers[i].due < timer->due)) timer = &s->timers[i];
    if (!timer) return false;
    uint32_t timer_id = timer->id;
    JSValue fn = JS_DupValue(s->ctx, timer->fn), args_array = JS_DupValue(s->ctx, timer->args);
    int kind = timer->kind;
    if (kind == 1) timer->due = now + timer->interval;
    else { JS_FreeValue(s->ctx, timer->fn); JS_FreeValue(s->ctx, timer->args); memset(timer, 0, sizeof *timer); }
    uint64_t start = uptime_ms();
    struct web_profile before = s->doc->profile;
    uint64_t initial_dom_rev = s->doc ? s->doc->dom_revision : 0;
    begin_task(s);
    uint64_t initial_layout_ms = s->task_layout_ms;
    unsigned initial_flushes = s->task_layout_flushes;
    JSValue args[64]; int count = 0;
    if (kind == 2) args[count++] = JS_NewFloat64(s->ctx, (double)(now - s->now));
    else {
        JSValue len = JS_GetPropertyStr(s->ctx, args_array, "length"); uint32_t n = 0;
        if (JS_ToUint32(s->ctx, &n, len) < 0) exception(s);
        JS_FreeValue(s->ctx, len); if (n > 64) n = 64;
        for (uint32_t i = 0; i < n; ++i) args[count++] = JS_GetPropertyUint32(s->ctx, args_array, i);
    }
    uint64_t js_start = uptime_ms();
    JSValue global = JS_GetGlobalObject(s->ctx), result = JS_Call(s->ctx, fn, global, count, args);
    uint64_t js_ms = uptime_ms() - js_start;
    if (JS_IsException(result)) exception(s); else JS_FreeValue(s->ctx, result);
    for (int i = 0; i < count; i++) JS_FreeValue(s->ctx, args[i]);
    JS_FreeValue(s->ctx, global); JS_FreeValue(s->ctx, fn); JS_FreeValue(s->ctx, args_array);
    end_task(s);
    uint64_t elapsed = uptime_ms() - start;
    if (elapsed >= 250 || s->task_timed_out) {
        char message[768];
        uint64_t layout_ms = s->task_layout_ms >= initial_layout_ms ? s->task_layout_ms - initial_layout_ms : s->task_layout_ms;
        unsigned flushes = s->task_layout_flushes >= initial_flushes ? s->task_layout_flushes - initial_flushes : s->task_layout_flushes;
        uint64_t dom_rev = s->doc && s->doc->dom_revision >= initial_dom_rev ? s->doc->dom_revision - initial_dom_rev : 0;
        snprintf(message, sizeof message,
                 "Timer task profile: id %u, total %lu ms (callback-inclusive %lu ms, microtasks-inclusive %lu ms, layout %lu ms in %u flushes, DOM revisions %lu)",
                 timer_id, (unsigned long)elapsed, (unsigned long)js_ms,
                 (unsigned long)s->task_microtask_ms, (unsigned long)layout_ms, flushes, (unsigned long)dom_rev);
        log_text(s, 0, message);
        const struct web_profile *p = &s->doc->profile;
        snprintf(message, sizeof message,
                 "Timer native profile: calls %lu/%lu ms, inserts %lu, indices %lu visits/%lu ms, scripts %lu scans/%lu visits/%lu ms, resources %lu scans/%lu ms (times inclusive)",
                 (unsigned long)(p->native_calls-before.native_calls), (unsigned long)(p->native_ms-before.native_ms),
                 (unsigned long)(p->inserts-before.inserts), (unsigned long)(p->index_visits-before.index_visits),
                 (unsigned long)(p->index_ms-before.index_ms), (unsigned long)(p->script_scans-before.script_scans),
                 (unsigned long)(p->script_visits-before.script_visits), (unsigned long)(p->script_ms-before.script_ms),
                 (unsigned long)(p->rescans-before.rescans), (unsigned long)(p->rescan_ms-before.rescan_ms));
        log_text(s, 0, message);
        snprintf(message, sizeof message,
                 "Timer metadata profile: %lu syncs/%lu visits/%lu ms (inclusive)",
                 (unsigned long)(s->doc->profile.metadata_syncs-before.metadata_syncs),
                 (unsigned long)(s->doc->profile.metadata_visits-before.metadata_visits),
                 (unsigned long)(s->doc->profile.metadata_ms-before.metadata_ms));
        log_text(s, 0, message);
    }
    return true;
}
static bool run_posted_task(struct web_js_state *s) {
    struct js_posted_task *p = s->posted; if (!p) return false;
    s->posted = p->next; if (!s->posted) s->last_posted = NULL;
    s->posted_count--;
    JSValue fn = p->fn; js_free(s->ctx, p);
    uint64_t start = uptime_ms();
    struct web_profile before = s->doc->profile;
    begin_task(s);
    JSValue result = JS_Call(s->ctx, fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(result)) exception(s); else JS_FreeValue(s->ctx, result);
    JS_FreeValue(s->ctx, fn); end_task(s);
    uint64_t elapsed = uptime_ms() - start;
    if (elapsed >= 250 || s->task_timed_out) {
        char message[320];
        snprintf(message, sizeof message,
                 "Posted task profile: total %lu ms, microtasks-inclusive %lu ms, native %lu calls/%lu ms, resources %lu scans/%lu ms",
                 (unsigned long)elapsed, (unsigned long)s->task_microtask_ms,
                 (unsigned long)(s->doc->profile.native_calls-before.native_calls),
                 (unsigned long)(s->doc->profile.native_ms-before.native_ms),
                 (unsigned long)(s->doc->profile.rescans-before.rescans),
                 (unsigned long)(s->doc->profile.rescan_ms-before.rescan_ms));
        log_text(s, 0, message);
        snprintf(message, sizeof message,
                 "Posted metadata profile: %lu syncs/%lu visits/%lu ms (inclusive)",
                 (unsigned long)(s->doc->profile.metadata_syncs-before.metadata_syncs),
                 (unsigned long)(s->doc->profile.metadata_visits-before.metadata_visits),
                 (unsigned long)(s->doc->profile.metadata_ms-before.metadata_ms));
        log_text(s, 0, message);
    }
    return true;
}
static JSValue event_init(struct web_js_state *s, const struct web_event *event) {
    int sx = 0, sy = 0; if (s->host.scroll) s->host.scroll(s->host.opaque, &sx, &sy);
    JSValue init = JS_NewObjectProto(s->ctx, JS_NULL);
    if (JS_IsException(init)) return init;
#define SET(k,v) do { JSValue value_ = (v); if (JS_IsException(value_) || \
    JS_SetPropertyStr(s->ctx, init, k, value_) < 0) goto failed; } while (0)
#define SET_B(k,v) SET(k, JS_NewBool(s->ctx, v))
#define SET_I(k,v) SET(k, JS_NewInt32(s->ctx, v))
    SET_B("bubbles", event->bubbles); SET_B("cancelable", event->cancelable);
    SET_B("ctrlKey", event->ctrl); SET_B("shiftKey", event->shift); SET_B("altKey", event->alt);
    SET_I("clientX", event->x); SET_I("clientY", event->y); SET_I("pageX", event->x + sx); SET_I("pageY", event->y + sy);
    SET_I("button", event->button); SET_I("buttons", event->buttons);
    SET_I("keyCode", event->key_code); SET_I("which", event->key_code);
    SET("relatedTarget", wrap(s, event->related_target));
    /* web_hover supplies pointer state only; its individual event types are
       generated by the hover hook after this shared dictionary is created. */
    if (event->type && !strcmp(event->type,"submit")) SET("submitter", wrap(s, event->submitter));
    SET("key", JS_NewString(s->ctx, event->key ? event->key : ""));
#undef SET_B
#undef SET_I
#undef SET
    return init;
failed:
    JS_FreeValue(s->ctx, init); return JS_EXCEPTION;
}
void web_js_focus_control(web_doc *d, node_t *n) {
    if (!d || d->inert || (n && web_control_disabled(n))) return;
    node_t *old=d->focus;
    web_focus(d,n);
    node_t *actual=d->focus;
    if(old==actual)return;
    if(old) {
        struct web_event e={.type="blur",.related_target=actual};
        web_js_dispatch(d,old,&e); e.type="focusout"; e.bubbles=true; web_js_dispatch(d,old,&e);
    }
    if(actual && d->focus==actual) {
        struct web_event e={.type="focus",.related_target=old}; web_js_dispatch(d,actual,&e);
        if(d->focus==actual) {e.type="focusin";e.bubbles=true;web_js_dispatch(d,actual,&e);}
    }
}
bool web_js_dispatch(web_doc *d, node_t *target, const struct web_event *event) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->disabled || !event || !event->type || JS_IsUndefined(s->dispatch)) return true;
    begin_task(s);
    JSValue init = event_init(s, event);
    JSValue args[] = {wrap(s, target), JS_NewString(s->ctx, event->type), init};
    JSValue result = JS_EXCEPTION;
    if (!JS_IsException(args[0]) && !JS_IsException(args[1]) && !JS_IsException(args[2]))
        result = JS_Call(s->ctx, s->dispatch, JS_UNDEFINED, 3, args);
    bool allowed = true;
    if (JS_IsException(result)) { exception(s); allowed = false; } else { int r = JS_ToBool(s->ctx, result); allowed = r != 0; JS_FreeValue(s->ctx, result); }
    for (int i = 0; i < 3; ++i) JS_FreeValue(s->ctx, args[i]);
    end_task(s); return allowed;
}
bool web_media_activate(web_doc *d, web_node *target) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->disabled || !target || target->type != N_ELEM || target->foreign ||
        (target->tag != T_audio && target->tag != T_video) || !node_attr(target, "controls")) return false;
    begin_task(s);
    JSValue argument = wrap(s, target);
    JSValue result = JS_IsException(argument) ? JS_EXCEPTION : custom_element_hook(s, "avmediaActivate", 1, &argument);
    bool activated = false;
    if (JS_IsException(result)) exception(s);
    else { activated = JS_ToBool(s->ctx, result) > 0; JS_FreeValue(s->ctx, result); }
    JS_FreeValue(s->ctx, argument);
    end_task(s);
    return activated;
}
void web_hover(web_doc *d, web_node *target, const struct web_event *event) {
    struct web_js_state *s = d && d->live ? d->js : NULL;
    if (!s || s->disabled || s->running || !event) return;
    /* Native parents cannot be spoofed by page properties. Detached nodes and
       wrappers remain document-owned, so the previous JS snapshot stays valid. */
    begin_task(s);
    JSValue path = JS_NewArray(s->ctx), init = JS_UNDEFINED, fn = JS_UNDEFINED, next = JS_UNDEFINED;
    if (JS_IsException(path)) goto failed;
    uint32_t index = 0;
    for (node_t *n = target; n; n = n->assigned_slot ? n->assigned_slot : doc_shadow_parent(n)) {
        JSValue value = wrap(s, n);
        if (JS_IsException(value)) goto failed;
        if (JS_DefinePropertyValueUint32(s->ctx, path, index++, value, JS_PROP_C_W_E) < 0) goto failed;
    }
    init = event_init(s, event);
    if (JS_IsException(init)) goto failed;
    fn = JS_GetPropertyStr(s->ctx, s->hooks, "hover");
    if (JS_IsException(fn)) goto failed;
    JSValue args[] = {path, init};
    next = JS_Call(s->ctx, fn, JS_UNDEFINED, 2, args);
    if (JS_IsException(next)) goto failed;
    while (!s->disabled) {
        JSValue result = JS_Call(s->ctx, next, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(result)) goto failed;
        bool more = JS_ToBool(s->ctx, result) != 0;
        JS_FreeValue(s->ctx, result);
        if (!more) break;
        end_task(s); begin_task(s);
    }
    goto done;
failed:
    exception(s);
done:
    JS_FreeValue(s->ctx, fn); JS_FreeValue(s->ctx, init); JS_FreeValue(s->ctx, path);
    JS_FreeValue(s->ctx, next);
    end_task(s);
}
static bool unfinished_deferred(struct web_js_state *s) {
    for (struct js_script *p = s->scripts; p; p = p->next) if (p->deferred && !script_finished(s, p)) return true;
    return false;
}
static bool unfinished_scripts(struct web_js_state *s) {
    for (struct js_script *p = s->scripts; p; p = p->next) if (!script_finished(s, p)) return true;
    return false;
}
static bool ready_script(struct web_js_state *s, struct js_script **out) {
    for (struct js_script *p = s->scripts; p; p = p->next) {
        if (p->executed || !p->ready) continue;
        if (p == s->blocker) { if (!styles_busy(s)) { *out = p; return true; } continue; }
        if (p->asynchronous) { *out = p; return true; }
        if (p->dynamic && !p->deferred) {
            bool earlier = false;
            for (struct js_script *q = s->scripts; q != p; q = q->next) if (q->dynamic && !q->asynchronous && !script_finished(s, q)) { earlier = true; break; }
            if (!earlier) { *out = p; return true; }
        }
        if (s->parsing_done && p->deferred && !styles_busy(s)) {
            bool earlier = false;
            for (struct js_script *q = s->scripts; q != p; q = q->next) if (q->deferred && !script_finished(s, q)) { earlier = true; break; }
            if (!earlier) { *out = p; return true; }
        }
    }
    return false;
}
static bool observers_dirty(struct web_js_state *s) {
    if(!s->observers_active)return false;
    web_doc *d=s->doc;int x=0,y=0;if(s->host.scroll)s->host.scroll(s->host.opaque,&x,&y);
    return s->observers_force || d->need_style || !d->layout_valid || s->observer_dom!=d->dom_revision ||
        s->observer_layout!=d->layout_revision || s->observer_x!=x || s->observer_y!=y ||
        s->observer_w!=d->width || s->observer_h!=d->height;
}
static bool run_observers(struct web_js_state *s,uint64_t now) {
    if(now<s->observer_due || !observers_dirty(s))return false;
    begin_task(s);flush_layout(s);
    s->observers_force=false;s->observer_dom=s->doc->dom_revision;s->observer_layout=s->doc->layout_revision;
    s->observer_w=s->doc->width;s->observer_h=s->doc->height;
    if(s->host.scroll)s->host.scroll(s->host.opaque,&s->observer_x,&s->observer_y);
    JSValue result=custom_element_hook(s,"observerFrame",0,NULL);
    if(JS_IsException(result))exception(s);else JS_FreeValue(s->ctx,result);
    end_task(s);s->observer_due=uptime_ms()+16;
    return true;
}
static bool run_document_event(struct web_js_state *s) {
    web_doc *d = s->doc;
    if (s->parsing_done && !unfinished_deferred(s) && !s->domcontent_sent) {
        s->domcontent_sent = true;
        struct web_event e = {.type="DOMContentLoaded", .bubbles=true};
        web_js_dispatch(d, d->root, &e); return true;
    }
    if (s->domcontent_sent && !s->load_sent && !unfinished_scripts(s) && !styles_busy(s) &&
        !pending_kind(s, P_IMAGE) && !pending_kind(s, P_SCRIPT)) {
        for (int i = 0; i < web_image_count(d); i++) if (web_image_wanted(d, i)) return false;
        s->load_sent = true; struct web_event e = {.type="load"}; web_js_dispatch(d, NULL, &e); return true;
    }
    return false;
}
static bool run_details_toggle(struct web_js_state *s) {
    bool old_open,new_open;
    node_t *node=doc_details_take_toggle(s->doc,&old_open,&new_open);
    if(!node)return false;
    begin_task(s);
    JSValue args[]={wrap(s,node),JS_NewBool(s->ctx,old_open),JS_NewBool(s->ctx,new_open)};
    JSValue result=JS_IsException(args[0])?JS_EXCEPTION:custom_element_hook(s,"detailsToggle",3,args);
    if(JS_IsException(result))exception(s);else JS_FreeValue(s->ctx,result);
    for(int i=0;i<3;i++)JS_FreeValue(s->ctx,args[i]);
    end_task(s);return true;
}
void web_js_tick(web_doc *d, uint64_t now) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->running) return;
    bool ran = process_pending(s);
    request_styles(s);
    if (s->blocker && (s->blocker->executed || s->disabled)) s->blocker = NULL;
    /* A tick executes at most one JavaScript task plus its microtask checkpoint.
       Parsing and native completions may progress without invoking callbacks. */
    if (!s->parsing_done && !s->blocker && d->parser) {
        node_t *created_before = d->owned_nodes;
        for (int step = 0; step < 32 && !s->blocker; step++) {
            node_t *node = NULL; int result = html_resume(d->parser, &node);
            d->dirty = true; d->resources_dirty = true;
            if (result <= 0) {
                if (result < 0) log_text(s, 2, "HTML parsing stopped at the document's memory limit");
                html_finish(d->parser); d->parser = NULL; s->parsing_done = true; break;
            }
            if (node && node->tag == T_script) {
                struct js_script *script = queue_script(s, node, false);
                if (script && !script->deferred && !script->asynchronous) s->blocker = script;
            }
        }
        if (d->resources_dirty) doc_rescan(d);
        request_styles(s);
        /* Definitions can precede later parser chunks/document.write output.
           Queue upgrades after the native tree is stable, before its scripts. */
        bool custom_created = false;
        for (node_t *n = d->owned_nodes; n && n != created_before; n = n->owned_next)
            if (custom_candidate(n)) { custom_created = true; break; }
        if (!s->disabled && custom_created) {
            begin_task(s);
            JSValue ce = custom_element_hook(s, "customElementScan", 0, NULL);
            if (JS_IsException(ce)) exception(s); else JS_FreeValue(s->ctx, ce);
            end_task(s);
        }
    }
    if (s->parsing_done && !s->disabled) dynamic_scripts(s, d->root);
    web_doc *family = d->dom_family ? d->dom_family : d;
    if (!s->disabled && !ran && family->shadow_slots_pending) {
        /* Native/parser assignment changes also queue the mutation checkpoint,
           even when there is no timer or script task to drive it. */
        begin_task(s); end_task(s); ran = true;
    }
    image_events(s);
    /* Completion notification is queued before lifecycle/task selection. A
       perpetually active task source cannot starve a module's load/error, and
       Window load must not overtake that module's resource event. */
    if (!s->disabled) {
        for (struct js_script *p = s->scripts; p; p = p->next) if (p->module && p->executed && !p->notified && script_finished(s, p)) {
            p->notified = true;
            bool failed = p->failed || (!JS_IsUndefined(p->evaluation) && JS_PromiseState(s->ctx, p->evaluation) == JS_PROMISE_REJECTED);
            script_event(s, p->node, failed);
        }
    }
    if (!s->disabled && !ran) {
        struct js_script *script;
        if (s->media_width != d->width || s->media_height != d->height) {
            s->media_width = d->width; s->media_height = d->height;
            begin_task(s); JSValue fn = JS_GetPropertyStr(s->ctx, s->hooks, "mediaChanged");
            JSValue value = JS_Call(s->ctx, fn, s->hooks, 0, NULL);
            if (JS_IsException(value)) exception(s); else JS_FreeValue(s->ctx, value);
            JS_FreeValue(s->ctx, fn); end_task(s); ran = true;
        }
        else if (dispatch_resource_event(s)) ran = true;
        else if (dispatch_image_decode(s)) ran = true;
        else if (ready_script(s, &script)) { run_script(s, script); ran = true; }
        else if (run_document_event(s)) ran = true;
        else if (run_details_toggle(s)) ran = true;
        else if (run_observers(s, now)) ran = true;
        else if (s->posted_turn && run_posted_task(s)) { s->posted_turn = false; ran = true; }
        else if (run_timer(s, now)) { s->posted_turn = true; ran = true; }
        else if (run_posted_task(s)) { s->posted_turn = false; ran = true; }
        else if (JS_IsJobPending(s->rt)) { begin_task(s); end_task(s); ran = true; }
    }
    if (s->disabled) for (struct js_script *p = s->scripts; p; p = p->next) { p->executed = true; JS_FreeValue(s->ctx, p->evaluation); p->evaluation = JS_UNDEFINED; }
    request_styles(s); request_images(s);
    if (!ran) run_document_event(s);
}
int64_t web_js_deadline(web_doc *d) {
    struct web_js_state *s = d ? d->js : NULL; if (!s) return -1;
    uint64_t deadline = UINT64_MAX, now = uptime_ms();
    if (!s->parsing_done && (!s->blocker || s->blocker->executed || s->disabled)) return (int64_t)now;
    for (struct js_pending *p = s->pending; p; p = p->next) {
        if (p->done) return (int64_t)now;
        if (p->deadline < deadline) deadline = p->deadline;
    }
    if (!s->disabled) {
        if ((d->dom_family ? d->dom_family : d)->shadow_slots_pending) return (int64_t)now;
        if ((d->dom_family ? d->dom_family : d)->details_toggle_first) return (int64_t)now;
        if (s->media_width != d->width || s->media_height != d->height) return (int64_t)now;
        if (s->events || s->posted) return (int64_t)now;
        for (struct js_image_decode *p = s->image_decodes; p; p = p->next) if (image_decode_ready(s, p)) return (int64_t)now;
        if (JS_IsJobPending(s->rt)) return (int64_t)now;
        if (observers_dirty(s) && s->observer_due < deadline) deadline=s->observer_due;
        struct js_script *ready; if (ready_script(s, &ready)) return (int64_t)now;
        for (int i = 0; i < JS_TIMERS; i++) if (s->timers[i].id && s->timers[i].due < deadline) deadline = s->timers[i].due;
        for (struct js_script *p = s->scripts; p; p = p->next) if (p->module && p->executed && !p->notified && script_finished(s, p)) return (int64_t)now;
    }
    if (s->parsing_done && ((!s->domcontent_sent && !unfinished_deferred(s)) || (s->domcontent_sent && !s->load_sent && !unfinished_scripts(s) && !styles_busy(s) && !pending_kind(s, P_IMAGE) && !pending_kind(s, P_SCRIPT)))) return (int64_t)now;
    return deadline == UINT64_MAX ? -1 : (int64_t)deadline;
}
bool web_js_running(web_doc *d) { return d && d->js && d->js->running; }
/* This is the document's parsing/presentation policy, not whether the watchdog
   currently permits execution. Stopping a runaway task cannot reinterpret
   noscript raw text as fallback HTML or change scripting media queries. */
bool web_js_enabled(web_doc *d) { return d && d->live; }
void web_history_event(web_doc *d, const char *old_url, bool popstate) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->disabled || s->running) return;
    begin_task(s);
    JSValue fn = JS_GetPropertyStr(s->ctx, s->hooks, "historyEvent");
    JSValue args[] = {JS_NewString(s->ctx, old_url), JS_NewBool(s->ctx, popstate)};
    JSValue value = JS_Call(s->ctx, fn, s->hooks, 2, args);
    if (JS_IsException(value)) exception(s); else JS_FreeValue(s->ctx, value);
    JS_FreeValue(s->ctx, fn); JS_FreeValue(s->ctx, args[0]); JS_FreeValue(s->ctx, args[1]);
    end_task(s);
}
void web_document_scroll(web_doc *d) {
    if (!d || !d->js || d->js->running) return;
    struct web_event e = {.type="scroll", .bubbles=true};
    web_js_dispatch(d, d->root, &e);
}
static JSValue load_browser_bindings(JSContext *ctx) {
    JSValue code;
    if (bindings_bytecode) {
        code = JS_ReadObject(ctx, bindings_bytecode, bindings_bytecode_len, JS_READ_OBJ_BYTECODE);
    } else {
        code = JS_Eval(ctx, js_bootstrap, sizeof js_bootstrap - 1, "nocturne:browser-bindings",
                       JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(code)) {
            size_t size = 0;
            uint8_t *bytes = JS_WriteObject(ctx, &size, code, JS_WRITE_OBJ_BYTECODE);
            if (bytes) {
                if (size <= (16u << 20)) {
                    uint8_t *copy = malloc(size);
                    if (copy) { memcpy(copy, bytes, size); bindings_bytecode = copy; bindings_bytecode_len = size; }
                }
                js_free(ctx, bytes);
            } else {
                /* Caching is optional; the compiled code remains usable. */
                JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error);
            }
        }
    }
    return JS_IsException(code) ? code : JS_EvalFunction(ctx, code);
}
void web_js_start(web_doc *d, const struct web_host *host) {
    struct web_js_state *s = calloc(1, sizeof *s); if (!s) return;
    d->js = s; s->doc = d; if (host) s->host = *host;
    s->media_width = d->width; s->media_height = d->height;
    s->now = uptime_ms(); s->hooks = s->dispatch = s->response = s->reject = JS_UNDEFINED;
    for (int i = 0; i < NP_COUNT; i++) s->node_protos[i] = JS_UNDEFINED;
    s->rt = JS_NewRuntime2(&allocator, &s->allocation);
    if (!s->rt) goto failed;
    JS_SetMemoryLimit(s->rt, JS_HEAP_LIMIT); JS_SetMaxStackSize(s->rt, JS_STACK_LIMIT);
    JS_SetInterruptHandler(s->rt, interrupt, s); JS_SetCanBlock(s->rt, false);
    JS_SetModuleLoaderFunc(s->rt, normalize_module, load_module, s);
    JS_SetHostPromiseRejectionTracker(s->rt, promise_rejection, s);
    s->ctx = JS_NewContext(s->rt); if (!s->ctx) goto failed;
    JS_SetContextOpaque(s->ctx, s);
    if (!node_class) JS_NewClassID(&node_class);
    JSClassDef class_def = {.class_name="NocturneDOMNode"};
    if (JS_NewClass(s->rt, node_class, &class_def) < 0) goto failed;
    JSValue api = JS_NewObject(s->ctx);
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("dom", 2, native_dom), JS_CFUNC_DEF("log", 1, native_log),
        JS_CFUNC_DEF("now", 0, native_now), JS_CFUNC_DEF("url", 0, native_url), JS_CFUNC_DEF("origin", 0, native_origin),
        JS_CFUNC_DEF("screen", 0, native_screen),
        JS_CFUNC_DEF("pack", 1, native_pack), JS_CFUNC_DEF("unpack", 1, native_unpack),
        JS_CFUNC_DEF("classID", 1, native_class_id), JS_CFUNC_DEF("detach", 1, native_detach),
        JS_CFUNC_DEF("history", 4, native_history), JS_CFUNC_DEF("cookie", 1, native_cookie), JS_CFUNC_DEF("scroll", 2, native_scroll),
        JS_CFUNC_DEF("media", 1, native_media),
        JS_CFUNC_DEF("canvas", 2, native_canvas),
        JS_CFUNC_DEF("avmedia", 4, native_avmedia),
        JS_CFUNC_DEF("observers", 1, native_observers),
        JS_CFUNC_DEF("resolve", 1, native_resolve), JS_CFUNC_DEF("ready", 0, native_ready), JS_CFUNC_DEF("current", 0, native_current),
        JS_CFUNC_DEF("write", 1, native_write), JS_CFUNC_DEF("encode", 1, native_encode), JS_CFUNC_DEF("navigate", 1, native_navigate),
        JS_CFUNC_DEF("inline", 1, native_inline),
        JS_CFUNC_DEF("click", 1, native_click), JS_CFUNC_DEF("timer", 4, native_timer), JS_CFUNC_DEF("clear", 1, native_clear),
        JS_CFUNC_DEF("microtask", 1, native_microtask),
        JS_CFUNC_DEF("postTask", 1, native_post_task), JS_CFUNC_DEF("cancelPost", 1, native_cancel_post),
        JS_CFUNC_DEF("cssSupports", 2, native_css_supports),
        JS_CFUNC_DEF("storage", 5, native_storage),
        JS_CFUNC_DEF("fetch", 4, native_fetch), JS_CFUNC_DEF("cancel", 1, native_cancel)
    };
    JS_SetPropertyFunctionList(s->ctx, api, functions, sizeof functions / sizeof *functions);
    web_js_crypto_init(s->ctx, api);
    web_js_collator_init(s->ctx, api);
    JS_SetPropertyStr(s->ctx, api, "document", wrap(s, d->root));
    JSValue global = JS_GetGlobalObject(s->ctx);
    JS_SetPropertyStr(s->ctx, global, "__nocturne_host", api); JS_FreeValue(s->ctx, global);
    /* There is no page script or page callback during bootstrap. Charging the
       trusted library parser/CLDR setup to a future page task disabled all JS
       under QEMU. Keep startup bounded separately from the page task watchdog. */
    s->starting = true; begin_task(s); s->task_deadline = uptime_ms() + JS_STARTUP_MS;
    s->hooks = load_browser_bindings(s->ctx);
    if (JS_IsException(s->hooks)) { exception(s); s->hooks = JS_UNDEFINED; end_task(s); goto failed; }
    s->dispatch = JS_GetPropertyStr(s->ctx, s->hooks, "dispatch"); s->response = JS_GetPropertyStr(s->ctx, s->hooks, "response");
    s->reject = JS_GetPropertyStr(s->ctx, s->hooks, "reject");
    JSValue protos = JS_GetPropertyStr(s->ctx, s->hooks, "nodeProtos");
    if (JS_IsException(protos)) { exception(s); end_task(s); goto failed; }
    for (int i = 0; i < NP_COUNT; i++) {
        s->node_protos[i] = JS_GetPropertyUint32(s->ctx, protos, i);
        if (!JS_IsObject(s->node_protos[i])) {
            if (!JS_IsException(s->node_protos[i])) JS_ThrowTypeError(s->ctx, "Missing DOM interface prototype");
            JS_FreeValue(s->ctx, protos); exception(s); end_task(s); goto failed;
        }
    }
    JS_FreeValue(s->ctx, protos);
    JS_SetClassProto(s->ctx, node_class, JS_DupValue(s->ctx, s->node_protos[NP_NODE]));
    for (struct js_node_ref *r = s->nodes; r; r = r->next) {
        if (JS_SetPrototype(s->ctx, r->object, node_prototype(s, r->node)) < 0) {
            exception(s); end_task(s); goto failed;
        }
    }
    end_task(s); s->starting = false; return;
failed:
    s->starting = false;
    log_text(s, 2, "Could not initialize JavaScript; the document remains usable without scripting");
    s->disabled = true;
}
void web_js_free(web_doc *d) {
    struct web_js_state *s = d ? d->js : NULL; if (!s) return;
    d->js = NULL;
    if (s->ctx) {
        while (s->posted) { struct js_posted_task *p = s->posted; s->posted = p->next; JS_FreeValue(s->ctx, p->fn); js_free(s->ctx, p); }
        while (s->image_decodes) {
            struct js_image_decode *p = s->image_decodes; s->image_decodes = p->next;
            JS_FreeValue(s->ctx, p->resolve); JS_FreeValue(s->ctx, p->reject); js_free(s->ctx, p);
        }
        while (s->events) { struct js_resource_event *e = s->events; s->events = e->next; js_free(s->ctx, e); }
        while (s->pending) { struct js_pending *p = s->pending; s->pending = p->next; if (s->host.cancel) s->host.cancel(s->host.opaque, p->id); pending_free(s, p); }
        while (s->scripts) { struct js_script *p = s->scripts; s->scripts = p->next; JS_FreeValue(s->ctx, p->evaluation); js_free(s->ctx, p->url); js_free(s->ctx, p->base); js_free(s->ctx, p->source); js_free(s->ctx, p); }
        while (s->modules) { struct js_module *m = s->modules; s->modules = m->next; JS_FreeValue(s->ctx, m->compiled); JS_FreeValue(s->ctx, m->error); js_free(s->ctx, m->url); js_free(s->ctx, m->base); js_free(s->ctx, m->source); js_free(s->ctx, m); }
        for (int i = 0; i < JS_TIMERS; i++) if (s->timers[i].id) { JS_FreeValue(s->ctx, s->timers[i].fn); JS_FreeValue(s->ctx, s->timers[i].args); }
        for (int i = 0; i < 32; i++) if (s->rejections[i].used) { JS_FreeValue(s->ctx, s->rejections[i].promise); JS_FreeValue(s->ctx, s->rejections[i].reason); }
        while (s->nodes) { struct js_node_ref *n = s->nodes; s->nodes = n->next; JS_FreeValue(s->ctx, n->object); js_free(s->ctx, n); }
        JS_FreeValue(s->ctx, s->hooks); JS_FreeValue(s->ctx, s->dispatch); JS_FreeValue(s->ctx, s->response); JS_FreeValue(s->ctx, s->reject);
        for (int i = 0; i < NP_COUNT; i++) JS_FreeValue(s->ctx, s->node_protos[i]);
        JS_FreeContext(s->ctx);
    }
    if (s->rt) JS_FreeRuntime(s->rt);
    free(s);
}
