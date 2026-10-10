#include "form_face.h"
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
#include "web_dialog.h"
#include "html_serialize.h"
#include "js_canvas.h"
#include "js_memory.h"
#include "avmedia.h"
#include "form_value.h"
#include "form_validation.h"
#include "form_file.h"
#include "elements.h"
#include "js_crypto.h"
#include "js_encoding.h"
#include "js_collator_native.h"
#include "js_worker.h"
#include "js_navigator_native.h"
#include "js_worker_wire.h"
#include "frame.h"
#include <limits.h>
#include "js_bootstrap.inc"

/* Runtime storage grows on demand. There is no arbitrary per-page heap quota. */
/* Real Polymer/CE construction uses over 100 finite nested frames. Keep a
   guard well below Nocturne's 8 MiB demand-paged user stack, not unlimited. */
#define JS_STACK_LIMIT (1024u * 1024u)
#define JS_BODY_LIMIT WEBNET_BODY_LIMIT
#define JS_SOURCE_LIMIT WEBNET_SCRIPT_BODY_LIMIT /* native wire length representation; allocation remains lazy */
/* Executable data URLs carry source, not an HTTP request target. Keep them
   within the source/heap bounds without truncating their module identity. */
#define JS_DATA_URL_LIMIT ((size_t)UINT32_MAX)
/* Execution, microtasks and synchronous DOM/layout share one watchdog. Source
   preparation through COMPILE_ONLY has a separate, cumulative task allowance:
   live YouTube's finite 10.8 MB compilation takes over 10 seconds under QEMU.
   Neither nested callbacks nor repeated compilations reset either allowance.
   JavaScript eval()/Function() remain inside the execution budget. */
#define JS_COMPILE_MS UINT32_MAX
#define JS_STARTUP_MS 10000u /* trusted, built-in platform initialization only */
#define JS_TIMERS_INITIAL 32u
#define JS_IDLE_PERIOD_MS 50u
#define JS_IDLE_PERIOD_CALLBACKS 8u

#define JS_NODE_BUCKETS 4096
struct js_node_ref {
    node_t *node;
    struct web_js_state *state;
    JSWeakRef *object;
    /* These are real, gc_mark-visible DOM graph edges, not cache roots. */
    JSValue root_object, tree_object, owner_object, hold;
    struct js_node_ref *tree_root, *members, *member_next, **member_prev;
    struct js_node_ref *next, **prev, *hash_next, **hash_prev;
};
struct js_timer { uint32_t id; int kind; uint64_t due, interval; JSValue fn, args; };
struct js_posted_task { struct js_posted_task *next; JSValue fn; uint32_t id; };
struct js_idle_task { struct js_idle_task *next; JSValue fn; uint32_t id; uint64_t due; };
struct js_script {
    node_t *node;
    char *url, *base, *source;
    size_t len;
    bool module, deferred, asynchronous, dynamic, ready, failed, executed, notified;
    JSValue evaluation;
    uint64_t request;
    struct js_script *next;
};
enum { P_SCRIPT, P_FETCH, P_CSS, P_IMAGE, P_WORKER, P_FRAME, P_FONT };
struct js_pending {
    uint64_t id, deadline;
    int kind, credentials, cache_mode;
    int image;
    uint32_t worker, worker_request;
    int worker_kind;
    struct js_script *script;
    struct web_frame *frame;
    struct web_font_resource *font;
    JSValue resolve, reject;
    char *url,*origin;
    bool done, aborted, force_preflight, redirect_error, same_origin, keepalive, no_cors, no_referrer;
    struct web_response response;
    struct js_pending *next;
};
struct js_module { char *url, *base, *source; size_t len; JSValue compiled, error; struct js_module *next; };
struct js_frame_proxy {struct js_frame_proxy *next;node_t *token;JSValue proxy;};
struct js_rejection { JSValue promise, reason; struct js_rejection *next; };
struct js_resource_event { node_t *node; bool failed, image, selection, stylesheet; uint64_t generation; struct js_resource_event *next; };
struct js_image_decode { node_t *node; uint64_t generation; JSValue resolve, reject; struct js_image_decode *next; };
/* Order is shared with the bootstrap's private nodeProtos array. */
enum { NP_NODE, NP_DOCUMENT, NP_ELEMENT, NP_HTML, NP_TEXT, NP_COMMENT, NP_FRAGMENT, NP_IFRAME, NP_IMAGE,
       NP_INPUT, NP_BUTTON, NP_SELECT, NP_TEXTAREA, NP_FIELDSET, NP_OBJECT, NP_OUTPUT, NP_OPTION, NP_TEMPLATE, NP_DOCTYPE,
       NP_SCRIPT, NP_FORM, NP_ANCHOR, NP_AREA, NP_SVG, NP_SVGSVG, NP_PI, NP_ATTR,
       NP_META, NP_LINK, NP_STYLE, NP_BASE, NP_TITLE, NP_HEAD, NP_SHADOW, NP_SLOT,
       NP_TIME, NP_DATA, NP_DETAILS, NP_OL, NP_LI, NP_UNKNOWN, NP_CANVAS,
       NP_AVMEDIA, NP_AUDIO, NP_VIDEO, NP_LABEL, NP_DATALIST, NP_PROGRESS, NP_METER, NP_HEADING, NP_PICTURE, NP_SOURCE, NP_MENU, NP_DIALOG, NP_DIV, NP_TRACK, NP_FRAME, NP_SVGGRAPHICS, NP_SVGPATH, NP_COUNT };
struct js_alloc_diagnostics { size_t current, peak, requested, used, limit; unsigned failures, reported; bool quota; };
struct js_storage_profile {
    uint64_t calls[2][7], inclusive_ms, backend_calls, backend_ms, set_bytes;
    uint64_t saves, save_ms, snapshot_bytes, cookie_calls, cookie_ms;
    unsigned depth;
};
enum { JS_NAV_START, JS_UNLOAD_START, JS_UNLOAD_END, JS_REDIRECT_START, JS_REDIRECT_END,
       JS_FETCH_START, JS_DNS_START, JS_DNS_END, JS_CONNECT_START, JS_CONNECT_END,
       JS_SECURE_START, JS_REQUEST_START, JS_RESPONSE_START, JS_RESPONSE_END,
       JS_DOM_LOADING, JS_DOM_INTERACTIVE, JS_DOM_CONTENT_START, JS_DOM_CONTENT_END,
       JS_DOM_COMPLETE, JS_LOAD_START, JS_LOAD_END, JS_TIMING_COUNT };
struct web_js_state {
    struct js_alloc_diagnostics allocation;
    web_doc *doc;
    struct web_host host;
    JSRuntime *rt;
    JSContext *ctx;
    struct web_js_state *runtime_owner, *runtime_active, *runtime_previous;
    struct js_frame_proxy *frame_proxies; /* only the runtime owner holds these */
    struct js_history_receiver *history_receiver;
    JSValue hooks, dispatch, response, reject, node_protos[NP_COUNT];
    struct js_node_ref *nodes;
    struct js_node_ref *node_buckets[JS_NODE_BUCKETS];
    bool node_cache_syncing, node_cache_failed;
    struct web_js_state *node_cache_next, **node_cache_prev;
    struct js_script *scripts, *last_script, *blocker;
    struct js_pending *pending;
    web_workers *workers;
    bool worker_turn;
    struct js_module *modules;
    const char *module_entry_url;
    struct js_timer *timers;
    unsigned timer_capacity;
    struct js_posted_task *posted, *last_posted;
    unsigned posted_count;
    uint32_t next_posted;
    bool posted_id_wrapped;
    bool posted_turn, observer_turn;
    uint32_t canvas_probe_seen;
    uint32_t image_bitmap_seen;
    struct js_idle_task *idle_pending, *last_idle_pending, *idle_runnable, *last_idle_runnable;
    unsigned idle_count, idle_period_callbacks;
    uint32_t next_idle;
    uint64_t idle_period_end;
    bool idle_timeout_turn, idle_period_stopped;
    struct js_rejection *rejections,*last_rejection;
    bool rejection_oom;
    struct js_resource_event *events, *last_event;
    struct js_image_decode *image_decodes, *last_image_decode;
    uint64_t next_request, task_deadline, now;
    uint64_t fetch_group;
    uint64_t task_started_ms, task_native_wait_ms, diagnostic_task_id;
    const char *diagnostic_task;
    uint32_t task_budget_ms;
    size_t heap_limit;
    uint64_t timing[JS_TIMING_COUNT];
    uint32_t timing_valid; /* Recorded uptime 0 is distinct from unavailable. */
    uint64_t task_layout_ms, task_compile_ms, compile_wait_ms, task_microtask_ms;
    int compiling;
    bool compile_timed_out;
    unsigned task_layout_flushes;
    unsigned profile_dom_depth;
    struct js_storage_profile storage_profile;
    bool ce_hooks_active, range_hooks_active, face_hooks_active;
    struct js_mutation_registration *mutation_registrations;
    struct js_mutation_record *mutation_first,*mutation_last;
    bool mutation_job_pending;
    bool observers_active, observers_force, pointer_capture_pending;
    uint64_t observer_due, observer_layout, observer_dom;
    int observer_x, observer_y, observer_w, observer_h;
    uint32_t next_timer;
    unsigned pending_count;
    int running;
    unsigned long diagnostic_sequence;
    unsigned resource_failures;
    unsigned suspicious_script_urls;
    bool document_open,document_waiting;
    bool frames_scanned;
    uint64_t frames_revision;
    uint32_t script_count;
    int media_width, media_height;
    node_t *current_script;
    bool parsing_done, domcontent_sent, load_sent, disabled, timed_out, task_timed_out, parser_write, starting;
};
static JSClassID node_class,history_class;
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
static void diagnostic_url(const char *url, char *out, size_t capacity);
static void diagnostic_resource(struct web_js_state *s, struct js_pending *p, const char *reason);
static JSValue native_frame(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue native_document_stream(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static void frame_sync(struct web_js_state *s,node_t *node);
static void frame_response(struct web_js_state *s,struct js_pending *pending,bool ok);
static bool frame_stream_pump(struct web_js_state *state);
static bool frame_string_replace(char **slot, const char *text);
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
    struct js_alloc_diagnostics *d=s->opaque;
    if(d){d->current=s->malloc_size;if(s->malloc_size>d->peak)d->peak=s->malloc_size;}
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
    struct js_alloc_diagnostics *d=s->opaque;if(d)d->current=s->malloc_size;
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
       OS allocator or ABI. Accounting stays exact without a per-page quota. */
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
static JSValue history_security_error(JSContext *ctx,const char *message);
static bool task_context_active(const struct web_js_state *state);
/* Keep the real native initiator across a borrowed realm method's begin_task.
 * Permissions are never inferred from a replaceable JS receiver or URL. */
static struct web_js_state *sandbox_actor(struct web_js_state *s) {
    struct web_js_state *active=s->runtime_owner && s->runtime_owner->runtime_active?
        s->runtime_owner->runtime_active:s;
    for(struct web_js_state *p=active;p;p=p->runtime_previous)
        if(p->doc->sandbox_flags && !p->starting)return p;
    return active;
}
static uint32_t sandbox_authority(struct web_js_state *s) {
    uint32_t flags=s->doc->sandbox_flags;
    struct web_js_state *active=s->runtime_owner && s->runtime_owner->runtime_active?
        s->runtime_owner->runtime_active:s;
    for(struct web_js_state *p=active;p;p=p->runtime_previous)flags|=p->doc->sandbox_flags;
    return flags;
}
static bool sandbox_borrowed(struct web_js_state *s) {
    struct web_js_state *actor=sandbox_actor(s);
    return actor->doc->sandbox_flags && actor->doc!=s->doc;
}
static struct web_js_state *profile_task(struct web_js_state *s) {
    return s->runtime_owner && s->runtime_owner->runtime_active ? s->runtime_owner->runtime_active : s;
}
static void log_text(struct web_js_state *s, int level, const char *msg) {
    if (s->host.console) s->host.console(s->host.opaque, level, msg ? msg : "JavaScript error");
}
static void storage_profile_report(struct web_js_state *s) {
    if(!s->host.debug_js)return;
    const struct web_js_state *task=profile_task(s);
    const struct js_storage_profile *p=&task->storage_profile;
    bool used=p->cookie_calls!=0;
    for(unsigned kind=0;kind<2;kind++)for(unsigned op=0;op<7;op++)used|=p->calls[kind][op]!=0;
    if(!used)return;
    char message[640];
    snprintf(message,sizeof message,
             "Task storage profile: %s #%lu, local ops L/K/G/S/R/C/check %lu/%lu/%lu/%lu/%lu/%lu/%lu, session %lu/%lu/%lu/%lu/%lu/%lu/%lu; storage-inclusive %lu ms, backend %lu calls/%lu ms, SET input %lu bytes",
             task->diagnostic_task?task->diagnostic_task:"callback",(unsigned long)task->diagnostic_task_id,
             (unsigned long)p->calls[0][0],(unsigned long)p->calls[0][1],(unsigned long)p->calls[0][2],
             (unsigned long)p->calls[0][3],(unsigned long)p->calls[0][4],(unsigned long)p->calls[0][5],(unsigned long)p->calls[0][6],
             (unsigned long)p->calls[1][0],(unsigned long)p->calls[1][1],(unsigned long)p->calls[1][2],
             (unsigned long)p->calls[1][3],(unsigned long)p->calls[1][4],(unsigned long)p->calls[1][5],(unsigned long)p->calls[1][6],
             (unsigned long)p->inclusive_ms,(unsigned long)p->backend_calls,(unsigned long)p->backend_ms,(unsigned long)p->set_bytes);
    log_text(s,0,message);
    snprintf(message,sizeof message,
             "Task storage publication profile: %lu attempts/%lu ms, %lu serialized snapshot bytes; cookie %lu calls/%lu ms (inclusive, not covered by native_dom profile)",
             (unsigned long)p->saves,(unsigned long)p->save_ms,(unsigned long)p->snapshot_bytes,
             (unsigned long)p->cookie_calls,(unsigned long)p->cookie_ms);
    log_text(s,0,message);
}
void web_js_prepare_bytes(JSContext *ctx, size_t bytes) {
    struct web_js_state *s=state(ctx);
    if(!s||!s->rt||s->heap_limit==SIZE_MAX||bytes>s->heap_limit-2048)return;
    struct js_alloc_diagnostics *allocation=&s->runtime_owner->allocation;
    size_t before=allocation->current,need=bytes+2048;
    if(before<=s->heap_limit&&need<=s->heap_limit-before)return;
    /* QuickJS triggers its automatic cycle GC on object creation, not this
     * js_malloc payload. Its adaptive threshold can exceed our finite limit.
     * The caller's JSValues are still rooted here; queued finalization jobs
     * run only at the normal microtask checkpoint, not during allocation. */
    JS_RunGC(s->rt);
    size_t after=allocation->current;
    if(after<before&&before-after>=1024u*1024u){
        char message[160];snprintf(message,sizeof message,
            "JavaScript pressure GC: reclaimed %lu bytes before %lu-byte host allocation",
            (unsigned long)(before-after),(unsigned long)bytes);
        log_text(s,0,message);
    }
}
void web_js_console(web_doc *d, int level, const char *message) {
    /* Native resource/layout failures remain observable after scripting stops. */
    if (d && d->js) log_text(d->js, level, message);
}
bool web_js_auxiliary_link(web_doc *d,const char *url) {
    /* The native click path has applied sandbox and opener policy. This GET
       uses the host's real new-window spawn, with no opener or initiator URL.
       As with existing navigation, Nocturne sends no referrer. */
    if(!d || !d->live || d->inert || !d->js || !d->js->host.navigate_form || !url || !web_sandbox_auxiliary_allowed(d))return false;
    if(strncmp(url,"https://",8) && strncmp(url,"http://",7))return false;
    d->js->host.navigate_form(d->js->host.opaque,url,NULL,0,NULL,"_blank");
    return true; /* Accepted by host, not a promise that native spawn succeeded. */
}
bool web_js_video_present(web_doc *d, const struct web_video_patch *patch) {
    /* This is a native published-scene patch, never a JS/event/layout hook. */
    if (!d || !d->live || d->inert || !d->js || !d->js->host.video_present) return false;
    return d->js->host.video_present(d->js->host.opaque,d,patch);
}
static void report_allocation_failure(struct web_js_state *s) {
    struct js_alloc_diagnostics *d=&s->runtime_owner->allocation;
    if(d->failures!=d->reported) {
        char message[256];
        snprintf(message,sizeof message,"JavaScript allocation failure: %s; requested %lu, charged %lu, limit %lu, peak %lu bytes",
            d->quota?"runtime quota":"Nocturne allocator",(unsigned long)d->requested,(unsigned long)d->used,(unsigned long)d->limit,(unsigned long)d->peak);
        d->reported=d->failures;log_text(s,0,message);
        JSMemoryUsage usage; JS_ComputeMemoryUsage(s->rt, &usage);
        snprintf(message,sizeof message,"JavaScript heap: %ld allocations; %ld objects, %ld properties; strings %ld, functions %ld, bytecode %ld bytes",
            (long)usage.malloc_count,(long)usage.obj_count,(long)usage.prop_count,
            (long)usage.str_size,(long)usage.js_func_size,(long)usage.js_func_code_size);
        log_text(s,0,message);
    }
}
/* Read engine-owned stack data without running an author getter while reporting
   an error. Source excerpts use existing script/module ownership: no additional
   copies of YouTube-sized programs or diagnostic network requests. */
static JSValue diagnostic_stack(JSContext *ctx, JSValueConst error) {
    if (!JS_IsError(ctx, error)) return JS_UNDEFINED;
    JSAtom atom = JS_NewAtom(ctx, "stack");
    JSPropertyDescriptor descriptor;
    JSValue result = JS_UNDEFINED;
    if (atom && JS_GetOwnProperty(ctx, &descriptor, error, atom) > 0) {
        if (JS_IsString(descriptor.value)) result = JS_DupValue(ctx, descriptor.value);
        JS_FreeValue(ctx, descriptor.value); JS_FreeValue(ctx, descriptor.getter); JS_FreeValue(ctx, descriptor.setter);
    }
    JS_FreeAtom(ctx, atom);
    if (JS_HasException(ctx)) { JSValue secondary = JS_GetException(ctx); JS_FreeValue(ctx, secondary); }
    return result;
}
static void diagnostic_excerpt(struct web_js_state *s, const char *stack) {
    unsigned emitted = 0;
    for (const char *frame = stack; frame && *frame && emitted < 3;) {
        const char *end = strchr(frame, '\n'); if (!end) end = frame + strlen(frame);
        const char *tail = end;
        while (tail > frame && (tail[-1] == ')' || tail[-1] == '\r' || tail[-1] == ' ')) tail--;
        const char *column = tail; while (column > frame && isdigit((unsigned char)column[-1])) column--;
        const char *line_end = column > frame && column[-1] == ':' ? column - 1 : NULL;
        const char *line = line_end;
        if (line) while (line > frame && isdigit((unsigned char)line[-1])) line--;
        if (line && line < line_end && line > frame && line[-1] == ':') {
            const char *url_end = line - 1, *url = url_end;
            while (url > frame && url[-1] != '(' && url[-1] != ' ') url--;
            size_t url_len = (size_t)(url_end - url), length = 0;
            const char *source = NULL;
            if (url_len == strlen("nocturne:browser-bindings") && !memcmp(url,"nocturne:browser-bindings",url_len)) {
                source = js_bootstrap; length = sizeof js_bootstrap - 1;
            }
            for (struct js_script *script = s->scripts; !source && script; script = script->next)
                if (script->source && script->url && strlen(script->url) == url_len && !memcmp(script->url,url,url_len)) {
                    source = script->source; length = script->len;
                }
            for (struct js_module *module = s->modules; !source && module; module = module->next)
                if (module->source && strlen(module->url) == url_len && !memcmp(module->url,url,url_len)) {
                    source = module->source; length = module->len;
                }
            if (source) {
                unsigned long row = strtoul(line,NULL,10), col = strtoul(column,NULL,10);
                size_t start = 0;
                for (unsigned long current = 1; current < row && start < length; current++) {
                    const char *nl = memchr(source + start,'\n',length - start);
                    start = nl ? (size_t)(nl + 1 - source) : length;
                }
                const char *nl = start < length ? memchr(source + start,'\n',length - start) : NULL;
                size_t finish = nl ? (size_t)(nl - source) : length;
                size_t at = start + MIN(col ? col - 1 : 0, finish - start);
                size_t left = at > start + 100 ? at - 100 : start;
                while (left < at && ((unsigned char)source[left] & 0xc0) == 0x80) left++;
                size_t count = MIN(finish - left, 260);
                while (count && left+count < finish && ((unsigned char)source[left+count] & 0xc0) == 0x80) count--;
                char message[384];
                snprintf(message,sizeof message,"Source %lu:%lu (%scolumn): %.*s",row,col,col?"":"unknown ",(int)count,source+left);
                for (char *p=message;*p;p++) if (*p=='\r'||*p=='\t') *p=' ';
                log_text(s,2,message); emitted++;
            }
        }
        frame = *end ? end + 1 : NULL;
    }
}
static void diagnostic_context(struct web_js_state *s) {
    char message[512];
    uint64_t now=uptime_ms(), wall=now>=s->task_started_ms?now-s->task_started_ms:0;
    uint64_t allowance=s->task_native_wait_ms+s->task_compile_ms;
    unsigned modules=0,timers=0;
    for(struct js_module *m=s->modules;m;m=m->next)modules++;
    for(unsigned i=0;i<s->timer_capacity;i++)if(s->timers[i].id)timers++;
    snprintf(message,sizeof message,"Diagnostic #%lu: %s #%lu, depth %d, execution %lu/%u ms (wall %lu, native wait %lu, compile %lu), pending %u, scripts %u, modules %u, timers %u, heap %lu/%zu bytes, layout %lu ms/%u flushes, DOM revision %lu; readyState %s",
             ++s->diagnostic_sequence,s->diagnostic_task?s->diagnostic_task:"callback",(unsigned long)s->diagnostic_task_id,s->running,
             (unsigned long)(wall>=allowance?wall-allowance:0),s->task_budget_ms,(unsigned long)wall,(unsigned long)s->task_native_wait_ms,(unsigned long)s->task_compile_ms,
             s->pending_count,s->script_count,modules,timers,(unsigned long)s->runtime_owner->allocation.current,s->heap_limit,
             (unsigned long)s->task_layout_ms,s->task_layout_flushes,(unsigned long)s->doc->dom_revision,
             s->load_sent?"complete":s->domcontent_sent?"interactive":"loading");
    log_text(s,2,message);
    storage_profile_report(s);
}
static void exception(struct web_js_state *s) {
    report_allocation_failure(s);
    bool temporary = !s->running;
    if (temporary) { JS_UpdateStackTop(s->rt); s->task_deadline = uptime_ms() + s->task_budget_ms; s->running = 1; }
    JSValue e = JS_GetException(s->ctx);
    JSValue stack = diagnostic_stack(s->ctx, e);
    const char *a = JS_ToCString(s->ctx, e), *b = JS_IsUndefined(stack) ? NULL : JS_ToCString(s->ctx, stack);
    sbuf text = {0}; sb_puts(&text, a ? a : "JavaScript exception");
    if (b && *b && (!a || strcmp(a, b))) { sb_putc(&text, '\n'); sb_puts(&text, b); }
    log_text(s, 2, sb_cstr(&text)); sb_free(&text);
    diagnostic_context(s);
    diagnostic_excerpt(s,b);
    JS_FreeCString(s->ctx, a); JS_FreeCString(s->ctx, b);
    JS_FreeValue(s->ctx, stack); JS_FreeValue(s->ctx, e);
    if (temporary) s->running = 0;
}
static int interrupt(JSRuntime *rt, void *opaque) {
    struct web_js_state *s = opaque;
    if(s->runtime_owner && s->runtime_owner->runtime_active)s=s->runtime_owner->runtime_active;
    if (s->disabled) return 1;
    if (s->running && !s->starting && s->host.script_checkpoint &&
        !s->host.script_checkpoint(s->host.opaque)) { s->disabled=true; return 1; }
    uint64_t now=uptime_ms();
    if (s->running && now >= s->task_deadline) { s->timed_out = true; s->task_timed_out = true; s->disabled = true; return 1; }
    /* The audio device continues consuming during a long author callback.
       Service only bounded native transport/PCM state here, never author JS,
       layout or UI painting. Its time still belongs to this task's watchdog. */
    web_avmedia_service(now);
    if (s->running && uptime_ms() >= s->task_deadline) { s->timed_out = true; s->task_timed_out = true; s->disabled = true; return 1; }
    return 0;
}
bool web_native_checkpoint(web_doc *d) {
    if (!d) return true;
    if (__atomic_load_n(&d->native_cancelled,__ATOMIC_ACQUIRE)) return false;
    if ((++d->native_checkpoint_count & 63u) != 0) return true;
    struct web_js_state *s = d->js;
    if (!s || s->starting) return true;
    bool stopped = s->running && !s->disabled ? interrupt(s->rt, s) != 0 :
        s->host.script_checkpoint && !s->host.script_checkpoint(s->host.opaque);
    if (stopped) { __atomic_store_n(&d->native_cancelled,true,__ATOMIC_RELEASE); s->disabled = true; return false; }
    return true;
}
static void begin_task(struct web_js_state *s) {
    if (!s->running) {
        /* A nested child-realm call must not move the shared stack guard past
           the parent's active C/JS frames. Refresh only at an outer boundary. */
        if (!s->runtime_owner || !s->runtime_owner->runtime_active) JS_UpdateStackTop(s->rt);
        s->task_deadline = uptime_ms() + s->task_budget_ms;
        if(s->runtime_owner){
            s->runtime_previous=s->runtime_owner->runtime_active;
            if(s->runtime_previous && s->runtime_previous->running && s->runtime_previous->task_deadline<s->task_deadline)
                s->task_deadline=s->runtime_previous->task_deadline;
            s->runtime_owner->runtime_active=s;
        }
        s->task_started_ms=uptime_ms();s->task_native_wait_ms=0;
        s->diagnostic_task="callback";s->diagnostic_task_id=0;
        s->task_layout_ms = 0; s->task_layout_flushes = 0;
        s->task_compile_ms = 0; s->compile_timed_out = false;
        s->task_microtask_ms = 0; s->task_timed_out = false;
        memset(&s->storage_profile,0,sizeof s->storage_profile);
    }
    s->running++;
}
static void begin_named_task(struct web_js_state *s,const char *name,uint64_t id) {
    begin_task(s);
    if(s->running==1){s->diagnostic_task=name;s->diagnostic_task_id=id;}
}
static void end_task(struct web_js_state *s) {
    doc_shadow_flush(s->doc);
    if (s->running == 1 && !s->disabled) {
        signal_slots(s);
        JSContext *ctx;
        uint64_t microtask_start = s->host.debug_js?uptime_ms():0;
        while (JS_IsJobPending(s->rt)) {
            if (interrupt(s->rt, s)) break;
            int r = JS_ExecutePendingJob(s->rt, &ctx);
            if (r < 0) exception(state(ctx)?state(ctx):s);
            if (r == 0) break;
        }
        if(s->host.debug_js)s->task_microtask_ms += uptime_ms() - microtask_start;
    }
    if (s->running == 1) {
        /* Interrupted/OOM JS may not reach its finally. No captured Worker
         * target is permitted to pin a native close beyond its outer task. */
        web_worker_cancel_captures(s->workers);
        /* Module evaluation failures can be promise rejections, not exceptions
           returned to C. Keep the native allocation cause visible there too. */
        report_allocation_failure(s);
        struct js_rejection *rejected=s->rejections,*batch_end=rejected;
        /* Keep the former 32-record reporting work budget, not its feature
           storage cap: all remaining records stay live for the next turn. */
        for(unsigned i=1;batch_end&&i<32&&batch_end->next;i++)batch_end=batch_end->next;
        if(batch_end){s->rejections=batch_end->next;batch_end->next=NULL;if(!s->rejections)s->last_rejection=NULL;}
        if(s->rejection_oom){s->rejection_oom=false;log_text(s,2,"Unhandled rejection tracking allocation failed");}
        /* Unpublish this batch before author ToString/finalizer reentry. Each
           record owns local roots; newly rejected promises remain queued. */
        while(rejected){
            struct js_rejection *record=rejected;rejected=record->next;
            const char *p = JS_ToCString(s->ctx, record->reason);
            sbuf b = {0}; sb_puts(&b, "Unhandled promise rejection: ");
            sb_puts(&b, p ? p : "JavaScript exception");
            /* Module failures are rejected promises. Preserve their engine
               backtrace too, without invoking a page-defined stack getter or
               a Proxy trap while diagnosing an interrupted task. */
            if (JS_IsError(s->ctx, record->reason)) {
                JSAtom atom = JS_NewAtom(s->ctx, "stack");
                JSPropertyDescriptor desc;
                if (atom && JS_GetOwnProperty(s->ctx, &desc, record->reason, atom) > 0) {
                    if (JS_IsString(desc.value)) {
                        const char *stack = JS_ToCString(s->ctx, desc.value);
                        if (stack && *stack) { sb_putc(&b, '\n'); sb_puts(&b, stack); }
                        JS_FreeCString(s->ctx, stack);
                    }
                    JS_FreeValue(s->ctx, desc.value); JS_FreeValue(s->ctx, desc.getter); JS_FreeValue(s->ctx, desc.setter);
                }
                JS_FreeAtom(s->ctx, atom);
            }
            log_text(s, 2, sb_cstr(&b)); diagnostic_context(s);
            diagnostic_excerpt(s,sb_cstr(&b)); sb_free(&b);
            JS_FreeCString(s->ctx, p);
            JS_FreeValue(s->ctx,record->promise);JS_FreeValue(s->ctx,record->reason);js_free(s->ctx,record);
        }
        if (s->timed_out) {
            char message[128];
            if (s->task_budget_ms % 1000u)
                snprintf(message,sizeof message,"JavaScript stopped: the task exceeded its %u ms execution budget.",s->task_budget_ms);
            else
                snprintf(message,sizeof message,"JavaScript stopped: the task exceeded its %u second execution budget.",s->task_budget_ms/1000u);
            log_text(s, 2, s->compile_timed_out ? "JavaScript stopped: source preparation exceeded its 30 second task budget." :
                     s->starting ? "Browser API initialization exceeded its 10 second startup budget." : message);
            s->timed_out = false;
        }
        if (s->task_timed_out || uptime_ms()-s->task_started_ms>=250)storage_profile_report(s);
        if (s->doc->resources_dirty || s->doc->images_dirty) doc_rescan(s->doc);
    }
    if (s->running) s->running--;
    if(!s->running && s->runtime_owner){s->runtime_owner->runtime_active=s->runtime_previous;s->runtime_previous=NULL;}
}
static JS_BOOL browser_job_allowed(JSContext *ctx, void *opaque) {
    struct web_js_state *s = state(ctx);
    return s && s->doc && s->doc->live && !s->disabled;
}
static void promise_rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                              JS_BOOL handled, void *opaque) {
    struct web_js_state *s = state(ctx);
    if(!s)return;
    struct js_rejection **link=&s->rejections,*previous=NULL;
    while(*link){struct js_rejection *p=*link;
        if(JS_StrictEq(ctx,p->promise,promise)){
            if(handled){*link=p->next;if(s->last_rejection==p)s->last_rejection=previous;
                JS_FreeValue(ctx,p->promise);JS_FreeValue(ctx,p->reason);js_free(ctx,p);}
            return;
        }previous=p;link=&p->next;
    }
    if (handled) return;
    /* The rejection tracker cannot throw into its host-hook caller. Preserve
       existing records and report a real allocation failure at task end. */
    struct js_rejection *p=js_malloc_rt(s->rt,sizeof *p);
    if(!p){s->rejection_oom=true;return;}
    p->promise=JS_DupValue(ctx,promise);p->reason=JS_DupValue(ctx,reason);p->next=NULL;
    if(s->last_rejection)s->last_rejection->next=p;else s->rejections=p;s->last_rejection=p;
}
static node_t *unwrap(JSContext *ctx, JSValueConst v) {
    if (JS_IsNull(v) || JS_IsUndefined(v)) return NULL;
    struct js_node_ref *ref = JS_GetOpaque2(ctx, v, node_class);
    return ref ? ref->node : NULL;
}
static node_t *node_opaque(JSValueConst v) {
    struct js_node_ref *ref = JS_GetOpaque(v, node_class);
    return ref ? ref->node : NULL;
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
               (n->namespace_id == NS_SVG ? (!strcmp(n->raw_name, "svg") ? NP_SVGSVG :
                !strcmp(n->raw_name,"path") ? NP_SVGPATH :
                (!strcmp(n->raw_name,"g")||!strcmp(n->raw_name,"rect")||!strcmp(n->raw_name,"circle")||
                 !strcmp(n->raw_name,"ellipse")||!strcmp(n->raw_name,"line")||!strcmp(n->raw_name,"polyline")||
                 !strcmp(n->raw_name,"polygon")||!strcmp(n->raw_name,"text")||!strcmp(n->raw_name,"image")||
                 !strcmp(n->raw_name,"use")||!strcmp(n->raw_name,"foreignObject")) ? NP_SVGGRAPHICS : NP_SVG) :
                n->foreign ? NP_ELEMENT : n->tag == T_iframe ? NP_IFRAME : NP_HTML) :
               n->type == N_TEXT ? NP_TEXT : n->type == N_COMMENT ? NP_COMMENT : n->type == N_DOCTYPE ? NP_DOCTYPE : n->type == N_PI ? NP_PI : n->type == N_ATTR ? NP_ATTR : n->shadow_host ? NP_SHADOW : NP_FRAGMENT;
    if (n->type == N_ELEM && !n->foreign) switch (n->tag) {
        case T_frame: kind = NP_FRAME; break;
        case T_img: kind = NP_IMAGE; break;
        case T_track: kind = NP_TRACK; break;
        case T_input: kind = NP_INPUT; break;
        case T_label: kind = NP_LABEL; break;
        case T_datalist: kind = NP_DATALIST; break;
        case T_progress: kind = NP_PROGRESS; break;
        case T_meter: kind = NP_METER; break;
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
        case T_h1: case T_h2: case T_h3: case T_h4: case T_h5: case T_h6: kind = NP_HEADING; break;
        case T_picture: kind = NP_PICTURE; break;
        case T_source: kind = NP_SOURCE; break;
        case T_menu: kind = NP_MENU; break;
        case T_dialog: kind = NP_DIALOG; break;
        case T_div: kind = NP_DIV; break;
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
#include "js_nodes.h"
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
        if (strcmp(a->name, b->name)) return false;
        size_t apl = a->doctype_identifiers_sized ? a->public_id_len : a->public_id ? strlen(a->public_id) : 0;
        size_t bpl = b->doctype_identifiers_sized ? b->public_id_len : b->public_id ? strlen(b->public_id) : 0;
        size_t asl = a->doctype_identifiers_sized ? a->system_id_len : a->system_id ? strlen(a->system_id) : 0;
        size_t bsl = b->doctype_identifiers_sized ? b->system_id_len : b->system_id ? strlen(b->system_id) : 0;
        if (apl != bpl || asl != bsl || (apl && memcmp(a->public_id, b->public_id, apl)) ||
            (asl && memcmp(a->system_id, b->system_id, asl))) return false;
    } else if (a->type == N_TEXT || a->type == N_COMMENT || a->type == N_PI) {
        if (a->type == N_PI && strcmp(a->name, b->name)) return false;
        if (a->textlen != b->textlen || (a->textlen && memcmp(a->text, b->text, a->textlen))) return false;
    }
    const node_t *ac = a->first, *bc = b->first;
    while (ac && bc) { if (!equal_nodes(ac, bc)) return false; ac = ac->next; bc = bc->next; }
    return !ac && !bc;
}
/* Ordinary DOM tree order: shadow/template hosts and assigned slots are not
   parent links. No JS getters, wrapper identity, or traversal allocation. */
static bool node_precedes(const node_t *a, const node_t *b) {
    unsigned da=0,db=0;
    for(const node_t *p=a;p->parent;p=p->parent)da++;
    for(const node_t *p=b;p->parent;p=p->parent)db++;
    unsigned original_a=da,original_b=db;
    while(da>db){a=a->parent;da--;}
    while(db>da){b=b->parent;db--;}
    if(a==b)return original_a<original_b;
    while(a->parent!=b->parent){a=a->parent;b=b->parent;}
    for(a=a->next;a;a=a->next)if(a==b)return true;
    return false;
}
static unsigned compare_node_position(node_t *self,node_t *other) {
    enum { DISCONNECTED=1,PRECEDING=2,FOLLOWING=4,CONTAINS=8,CONTAINED_BY=16,IMPLEMENTATION_SPECIFIC=32 };
    if(self==other)return 0;
    node_t *node1=other,*node2=self,*attr1=NULL,*attr2=NULL;
    if(node1->type==N_ATTR){attr1=node1;node1=attr1->attr_owner;}
    if(node2->type==N_ATTR){
        attr2=node2;node2=attr2->attr_owner;
        if(attr1 && node1 && node1==node2){
            for(int i=0;i<node1->nattrs;i++){
                if(node1->attrs[i].node==attr1)return IMPLEMENTATION_SPECIFIC|PRECEDING;
                if(node1->attrs[i].node==attr2)return IMPLEMENTATION_SPECIFIC|FOLLOWING;
            }
        }
    }
    if(!node1 || !node2 || doc_node_root(node1,false)!=doc_node_root(node2,false))
        /* Arenas retain native node addresses for the whole Document family
           lifetime, including detach/adopt and inactive frame Documents. */
        return DISCONNECTED|IMPLEMENTATION_SPECIFIC|((uintptr_t)other<(uintptr_t)self?PRECEDING:FOLLOWING);
    if(!attr1)for(node_t *p=node2->parent;p;p=p->parent)if(p==node1)return PRECEDING|CONTAINS;
    if(node1==node2 && attr2)return PRECEDING|CONTAINS;
    if(!attr2)for(node_t *p=node1->parent;p;p=p->parent)if(p==node2)return FOLLOWING|CONTAINED_BY;
    if(node1==node2 && attr1)return FOLLOWING|CONTAINED_BY;
    return node_precedes(node1,node2)?PRECEDING:FOLLOWING;
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
#include "js_inner_text.h"
static JSValue get_dom(struct web_js_state *s, node_t *n, const char *p) {
    JSContext *ctx = s->ctx; web_doc *d = n->owner ? n->owner : s->doc;
    if (!strcmp(p, "styleSheetAvailable")) {
        if (n->type != N_ELEM || n->foreign || n->tag != T_style)
            return JS_ThrowTypeError(ctx, "HTMLStyleElement receiver required");
        const char *type = node_attr(n, "type");
        return JS_NewBool(ctx, d->live && doc_node_root(n, true) == d->root &&
            !node_ancestor(n, T_template) && (!type || !*type || str_ieq(type, "text/css")));
    }
    if (!strcmp(p, "shadowRoot")) return wrap(s, n->shadow_root);
    if (!strcmp(p, "shadowHost")) return wrap(s, n->shadow_host);
    if (!strcmp(p, "insertionHost")) return wrap(s, n->shadow_host ? n->shadow_host : n->template_host);
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
    if (!strcmp(p, "collectionVersion")) {
        /* Private bootstrap bridge: adoption can change owner without changing
           either revision. A BigInt preserves the complete native counter. */
        JSValue version = JS_NewArray(ctx);
        if (JS_IsException(version)) return version;
        JSValue owner = wrap(s, d->root);
        if (JS_IsException(owner)) { JS_FreeValue(ctx, version); return owner; }
        if (JS_DefinePropertyValueUint32(ctx, version, 0, owner, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, version); return JS_EXCEPTION; }
        JSValue revision = JS_NewBigUint64(ctx, d->dom_revision);
        if (JS_IsException(revision)) { JS_FreeValue(ctx, version); return revision; }
        if (JS_DefinePropertyValueUint32(ctx, version, 1, revision, JS_PROP_C_W_E) < 0) { JS_FreeValue(ctx, version); return JS_EXCEPTION; }
        return version;
    }
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
        if (!strcmp(p, "doctypeName")) return JS_NewString(ctx, n->name);
        bool public = !strcmp(p, "publicId");
        const char *id = public ? n->public_id : n->system_id;
        size_t len = n->doctype_identifiers_sized ? (public ? n->public_id_len : n->system_id_len) : id ? strlen(id) : 0;
        return JS_NewStringLen(ctx, id ? id : "", len);
    }
    if (!strncmp(p, "image", 5)) return get_image(s, n, p);
    if (!strncmp(p, "form:", 5)) {
        if (n->type != N_ELEM || n->foreign || strcmp(n->name, p + 5)) return JS_ThrowTypeError(ctx, "Form control interface receiver required");
        node_t *control = n->tag == T_option ? node_ancestor(n, T_select) : n->tag == T_label ? web_label_control(n) : n;
        if (!control) return JS_NULL;
        if (n->tag == T_label && (control->tag == T_progress || control->tag == T_meter)) return JS_NULL;
        return wrap(s, web_form_owner(d, control));
    }
    if (d->resources_dirty && (!strcmp(p, "documentElement") || !strcmp(p, "head") || !strcmp(p, "body") || !strcmp(p, "baseURI") || !strcmp(p, "activeElement"))) doc_sync_tree(d);
    if (!strcmp(p, "value") || !strcmp(p, "checked") || !strcmp(p, "selectedIndex") || !strcmp(p, "selected")) doc_control_init(d, n);
    if (!strcmp(p, "attrBrand")) return n->type == N_ATTR ? JS_TRUE : JS_ThrowTypeError(ctx, "Attr receiver required");
    if (!strcmp(p, "elementBrand")) return n->type == N_ELEM ? JS_TRUE : JS_ThrowTypeError(ctx, "Element receiver required");
    if (!strcmp(p,"elementScrollActive")) {
        web_doc *owner=n->owner;
        return JS_NewBool(ctx,n->type==N_ELEM && owner && owner->live && !owner->inert &&
            owner->js && !owner->js->disabled && doc_node_root(n,true)==owner->root);
    }
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
    if (!strcmp(p, "innerText")) return rendered_inner_text(s, n);
    if (!strcmp(p, "textContent")) {
        if (n->type == N_DOC || n->type == N_DOCTYPE) return JS_NULL;
        if (n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI) return JS_NewStringLen(ctx, n->text ? n->text : "", n->textlen);
        sbuf b = {0}; node_text_content(n, &b); JSValue v = JS_NewStringLen(ctx, b.p ? b.p : "", b.n); sb_free(&b); return v;
    }
    if (!strcmp(p, "innerHTML") || !strcmp(p, "outerHTML")) {
        sbuf b = {0};
        if (!strcmp(p, "outerHTML")) html_serialize_node(n, &b);
        else html_serialize_children(n, &b);
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
        node_t *focus = d->focus && connected(s, d->focus) && !web_dialog_inert(d,d->focus) ? d->focus : NULL;
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
    if (!strcmp(p, "filesRevision")) return JS_NewFloat64(ctx, (double)n->file_revision);
    if (!strcmp(p, "fileValue")) {
        if (!n->files || !n->files->count) return JS_NewString(ctx, "");
        char name[280]; snprintf(name, sizeof name, "C:\\fakepath\\%s", n->files->files[0].name); return JS_NewString(ctx, name);
    }
    if (!strcmp(p, "filesSnapshot")) {
        JSValue list = JS_NewArray(ctx); unsigned count = n->files ? n->files->count : 0;
        for (unsigned index = 0; index < count; index++) {
            const struct web_form_file *file = &n->files->files[index]; JSValue snapshot = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, snapshot, 0, JS_NewArrayBufferCopy(ctx, file->bytes, file->size));
            JS_SetPropertyUint32(ctx, snapshot, 1, JS_NewString(ctx, file->type));
            JS_SetPropertyUint32(ctx, snapshot, 2, JS_NewString(ctx, file->name));
            JS_SetPropertyUint32(ctx, snapshot, 3, JS_NewFloat64(ctx, (double)file->last_modified));
            JS_SetPropertyUint32(ctx, snapshot, 4, JS_NewString(ctx, file->relative_path));
            if (JS_SetPropertyUint32(ctx, list, index, snapshot) < 0) { JS_FreeValue(ctx, list); return JS_EXCEPTION; }
        }
        return list;
    }
    if (!strcmp(p, "indeterminate")) return JS_NewBool(ctx, n->indeterminate);
    if (!strcmp(p, "labelControl")) return wrap(s, web_label_control(n));
    if (!strcmp(p, "inputList")) return wrap(s, web_input_datalist(n));
    if (!strncmp(p, "gauge:", 6)) return JS_NewFloat64(ctx, web_gauge_value(n, p + 6));
    if (!strcmp(p, "inputWidth") || !strcmp(p, "inputHeight")) {
        bool width = !strcmp(p, "inputWidth");
        if (web_input_type(n) == WEB_INPUT_IMAGE && connected(s, n) && d->width > 0) {
            web_layout(d, d->width, d->height);
            if (n->box) { double pixels = width ? n->box->w : n->box->h; return JS_NewUint32(ctx, !(pixels > 0) ? 0 : pixels >= UINT32_MAX ? UINT32_MAX : (uint32_t)pixels); }
        }
        const char *attribute = node_attr(n, width ? "width" : "height"); uint64_t number = 0;
        if (attribute) { while (is_space((unsigned char)*attribute)) attribute++; if (*attribute == '+') attribute++;
            while (*attribute >= '0' && *attribute <= '9') { number = number * 10 + (unsigned)(*attribute++ - '0'); if (number > UINT32_MAX) return JS_NewUint32(ctx, 0); } }
        return JS_NewUint32(ctx, (uint32_t)number);
    }
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
        return JS_NewBool(ctx, n->js_force_async || node_attr(n, "async") != NULL);
    }
    return JS_UNDEFINED;
}

#include "js_dom_opcodes.h"
#include "js_mutations.h"
static JSValue set_dom(struct web_js_state *s, node_t *n, const char *p, JSValueConst value,int argc,JSValueConst *argv) {
    JSContext *ctx = s->ctx; web_doc *d = n->owner ? n->owner : s->doc;
    if (d->resources_dirty && !strcmp(p, "title")) doc_rescan(d);
    if (!strcmp(p, "imageWidth") || !strcmp(p, "imageHeight")) {
        if (!html_image(n)) return JS_ThrowTypeError(ctx, "HTMLImageElement receiver required");
        uint32_t number; if (JS_ToUint32(ctx, &number, value) < 0) return JS_EXCEPTION;
        char text[16]; snprintf(text, sizeof text, "%u", number);
        struct js_mutation_snapshot observed=mutation_before(s,DOM_set,n,argc,argv);
        bool ok=doc_attr_set_ns(d,n,NULL,NULL,!strcmp(p,"imageWidth")?"width":"height",text);
        mutation_after(s,&observed,ok);return ok?JS_UNDEFINED:oom(ctx);
    }
    if (!strcmp(p, "value") || !strcmp(p, "checked") || !strcmp(p, "selectedIndex") || !strcmp(p, "selected")) doc_control_init(d, n);
    if (!strcmp(p, "indeterminate")) { int b = JS_ToBool(ctx, value); if (b < 0) return JS_EXCEPTION; n->indeterminate = b; doc_mutated(d, n); return JS_UNDEFINED; }
    if (!strcmp(p, "checked")) { int b = JS_ToBool(ctx, value); if (b < 0) return JS_EXCEPTION; doc_control_checked(d, n, b); doc_mutated(d, n); return JS_UNDEFINED; }
    if (!strcmp(p, "async")) {
        if (n->tag != T_script) return JS_UNDEFINED;
        int b = JS_ToBool(ctx, value); if (b < 0) return JS_EXCEPTION;
        /* The IDL value reflects the attribute; it is not the force-async flag. */
        n->js_force_async_set = true; n->js_force_async = false;
        struct js_mutation_snapshot observed=mutation_before(s,DOM_set,n,argc,argv);
        bool ok=doc_attr_set_ns(d,n,NULL,NULL,"async",b?"":NULL);
        mutation_after(s,&observed,ok);return ok?JS_UNDEFINED:oom(ctx);
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
    struct js_mutation_snapshot observed=mutation_before(s,DOM_set,n,argc,argv);
    bool ok = true;
    if (!strcmp(p, "attrValue") && n->type == N_ATTR) ok = doc_attr_value(d, n, text);
    else if (!strcmp(p, "nodeValue")) { if (n->type == N_TEXT || n->type == N_COMMENT || n->type == N_PI || n->type == N_ATTR) ok = doc_node_text(d, n, text, len); }
    else if (!strcmp(p, "textContent")) { if (n->type != N_DOC) ok = doc_node_text(d, n, text, len); }
    else if (!strcmp(p, "innerHTML")) {
        if (n->type != N_ELEM && n->type != N_FRAGMENT) { mutation_after(s,&observed,false);JS_FreeCString(ctx, text); return JS_ThrowTypeError(ctx, "innerHTML requires an element"); }
        ok = doc_node_html(d, n, text, len);
    } else if (!strcmp(p, "value")) {
        if (n->tag == T_select) {
            int found = -1, i = 0;
            for (node_t *option = web_select_next_option(n, NULL); option; option = web_select_next_option(n, option), i++) {
                JSValue v = option_value(ctx, option); const char *p = JS_ToCString(ctx, v);
                if (!p) { mutation_after(s,&observed,false);JS_FreeValue(ctx, v); JS_FreeCString(ctx, text); return JS_EXCEPTION; }
                bool matches = !strcmp(p, text); JS_FreeCString(ctx, p); JS_FreeValue(ctx, v);
                if (matches) { found = i; break; }
            }
            web_select_set_index(d, n, found);
            doc_mutated(d, n);
        } else if (n->tag == T_option) ok = doc_attr_set_ns(d, n, NULL, NULL, "value", text);
        else {
            bool control = n->tag == T_input || n->tag == T_textarea;
            char *before = control ? strdup(n->value ? n->value : "") : NULL;
            ok = (!control || before) && doc_node_value(d, n, text, len);
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
    } else { mutation_after(s,&observed,false);JS_FreeCString(ctx, text); return JS_ThrowTypeError(ctx, "Unknown DOM setter"); }
    mutation_after(s,&observed,ok);
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
static const char *css_style_ws(const char *p, const char *end) {
    for (;;) {
        while (p < end && is_space((unsigned char)*p)) p++;
        if (end - p < 2 || p[0] != '/' || p[1] != '*') return p;
        p += 2;
        while (end - p >= 2 && !(p[0] == '*' && p[1] == '/')) p++;
        if (end - p < 2) return end;
        p += 2;
    }
}
/* Priority is not part of the CSSOM value. Do not split quoted strings,
   functions or custom-property token streams at a nested exclamation mark. */
static bool css_style_priority(const char *begin, const char **end) {
    const char *bang = css_separator(begin, *end, '!');
    if (bang == *end) return false;
    const char *p = css_style_ws(bang + 1, *end);
    if (*end - p < 9 || strncasecmp(p, "important", 9) ||
        css_style_ws(p + 9, *end) != *end) return false;
    *end = bang;
    while (*end > begin && is_space((unsigned char)(*end)[-1])) (*end)--;
    return true;
}
static JSValue css_style_value(JSContext *ctx, const char *property,
                               const char *begin, const char *end) {
    /* Normalize supported numeric opacity, not arbitrary/custom values.
       Preserve var()/calc() token streams unchanged. */
    if (!strcasecmp(property, "opacity") && end - begin > 0 && end - begin < 64) {
        const char *p = begin, *number_end = end;
        bool percent = end[-1] == '%';
        if (percent) number_end--;
        if (p < number_end && (*p == '+' || *p == '-')) p++;
        const char *digits = p;
        while (p < number_end && *p >= '0' && *p <= '9') p++;
        bool any = p > digits;
        if (p < number_end && *p == '.') {
            p++; digits = p;
            while (p < number_end && *p >= '0' && *p <= '9') p++;
            any = p > digits;
        }
        if (any && p < number_end && (*p == 'e' || *p == 'E')) {
            p++; if (p < number_end && (*p == '+' || *p == '-')) p++;
            digits = p;
            while (p < number_end && *p >= '0' && *p <= '9') p++;
            any = p > digits;
        }
        if (any && p == number_end) {
            char number[64], result[64];
            memcpy(number, begin, (size_t)(number_end - begin));
            number[number_end - begin] = 0;
            double value = strtod(number, NULL);
            if (isfinite(value) && fabs(value) < 1e12) {
                if (fabs(value) < .0000005) value = 0;
                int len = snprintf(result, sizeof result, "%.6f", value);
                if (len > 0 && (size_t)len < sizeof result) {
                    while (len && result[len-1] == '0') len--;
                    if (len && result[len-1] == '.') len--;
                    if (percent) result[len++] = '%';
                    return JS_NewStringLen(ctx, result, (size_t)len);
                }
            }
        }
    }
    return JS_NewStringLen(ctx, begin, (size_t)(end - begin));
}
static JSValue style_access(struct web_js_state *s, node_t *n, const char *property,
                            bool set, JSValueConst value, JSValueConst priority, bool priority_only) {
    JSContext *ctx = s->ctx;
    const char *css = node_attr(n, "style"); if (!css) css = "";
    sbuf b = {0}; const char *found = NULL, *found_end = NULL;
    bool found_important = false;
    const char *end = css + strlen(css);
    for (const char *p = css; p < end;) {
        const char *e = css_separator(p, end, ';'), *colon = css_separator(p, e, ':');
        if (colon < e && css_property_equal(p, colon, property)) {
            const char *v = colon + 1, *ve = e;
            while (v < ve && is_space((unsigned char)*v)) v++;
            while (ve > v && is_space((unsigned char)ve[-1])) ve--;
            bool important = css_style_priority(v, &ve);
            if (!found || important || !found_important) {
                found = v; found_end = ve; found_important = important;
            }
        } else if (set) { sb_put(&b, p, (size_t)(e - p)); if (e < end) sb_putc(&b, ';'); }
        p = e < end ? e + 1 : end;
    }
    if (!set) {
        sb_free(&b);
        if (priority_only) return JS_NewString(ctx, found_important ? "important" : "");
        return found ? css_style_value(ctx, property, found, found_end) : JS_NewString(ctx, "");
    }
    const char *v = JS_ToCString(ctx, value), *prio = JS_IsUndefined(priority) ? NULL : JS_ToCString(ctx, priority);
    if (!v || (!JS_IsUndefined(priority) && !prio)) { JS_FreeCString(ctx, v); JS_FreeCString(ctx, prio); sb_free(&b); return JS_EXCEPTION; }
    if (strchr(property, ':') || strchr(property, ';') || strchr(property, '{') || strchr(property, '}')) { JS_FreeCString(ctx, v); JS_FreeCString(ctx, prio); sb_free(&b); return JS_ThrowTypeError(ctx, "Invalid CSS property name"); }
    const char *value_end = v + strlen(v);
    if (*v && ((prio && *prio && strcasecmp(prio, "important")) ||
               css_separator(v, value_end, '!') != value_end ||
               css_separator(v, value_end, ';') != value_end)) {
        JS_FreeCString(ctx, v); JS_FreeCString(ctx, prio); sb_free(&b); return JS_UNDEFINED;
    }
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
    web_avmedia_service(uptime_ms());
    uint64_t start = s->host.debug_js?uptime_ms():0;
    uint64_t revision = d->layout_revision;
    web_layout(d, d->width, d->height);
    if(s->host.debug_js){s->task_layout_ms += uptime_ms() - start;
        if (d->layout_revision != revision) s->task_layout_flushes++;}
    web_avmedia_service(uptime_ms());
}
static JSValue rect_array(JSContext *ctx, float x, float y, float w, float h) {
    JSValue a = JS_NewArray(ctx);
    const float values[] = {x,y,w,h};
    for (int i=0;i<4;i++) JS_SetPropertyUint32(ctx,a,i,JS_NewFloat64(ctx,values[i]));
    return a;
}
static void viewport_scroll_position(struct web_js_state *s,int *x,int *y);
#include "js_svg_native.h"
static JSValue observer_geometry(struct web_js_state *s, node_t *n, node_t *root) {
    JSContext *ctx=s->ctx; web_doc *d=s->doc;
    if (!n || n->type!=N_ELEM || (root && root->type!=N_ELEM && root!=d->root))
        return JS_ThrowTypeError(ctx,"Observer requires an Element and an Element/Document root");
    flush_layout(s);
    bool visible=connected(s,n) && n->box && n->style && n->style->display!=D_NONE;
    box_t *b=visible?n->box:NULL;
    int sx,sy;viewport_scroll_position(s,&sx,&sy);
    JSValue out=JS_NewObject(ctx),clips=JS_NewArray(ctx);
    float x=0,y=0,w=0,h=0;
    if(b){x=box_visual_x(b)-b->p[3]-b->b[3]-sx;y=box_visual_y(b)-b->p[0]-b->b[0]-sy;
        w=b->w+b->p[1]+b->p[3]+b->b[1]+b->b[3];h=b->h+b->p[0]+b->p[2]+b->b[0]+b->b[2];}
    JS_SetPropertyStr(ctx,out,"rect",rect_array(ctx,x,y,w,h));
    bool resizable=b && (b->kind!=B_INLINE || b->kind==B_ATOMIC);
    JS_SetPropertyStr(ctx,out,"content",rect_array(ctx,b?b->p[3]:0,b?b->p[0]:0,resizable?b->w:0,resizable?b->h:0));
    JS_SetPropertyStr(ctx,out,"border",rect_array(ctx,0,0,resizable?w:0,resizable?h:0));
    size_t depth=0;
    for(node_t *p=n;p;p=p->assigned_slot?p->assigned_slot:doc_shadow_parent(p))
        if(p->type==N_ELEM)depth++;
    JS_SetPropertyStr(ctx,out,"depth",JS_NewFloat64(ctx,(double)depth));
    float rx=0,ry=0,rw=d->width,rh=d->height;
    bool root_ok=!root || root==d->root;
    if(root && root!=d->root) {
        box_t *r=connected(s,root)?root->box:NULL;
        if(r){bool clip=r->st && r->st->overflow!=OV_VISIBLE && !doc_viewport_overflow_box(d,r);
            rx=box_visual_x(r)-r->p[3]-(clip?0:r->b[3])-sx;
            ry=box_visual_y(r)-r->p[0]-(clip?0:r->b[0])-sy;
            rw=r->w+r->p[1]+r->p[3]+(clip?0:r->b[1]+r->b[3]);
            rh=r->h+r->p[0]+r->p[2]+(clip?0:r->b[0]+r->b[2]);
            for(node_t *p=n->parent;p;p=p->parent)if(p==root){root_ok=true;break;}
        }else rw=rh=0;
    }
    unsigned count=0;
    /* Use the same overflow/containing-block rule as native painting. */
    bool reached=!b || !b->st || b->st->position!=POS_ABSOLUTE;
    for(box_t *a=b && (!b->st || b->st->position!=POS_FIXED)?b->parent:NULL;a && a->parent;a=a->parent) {
        if(root && a->node==root)break;
        if(a->st && a->kind!=B_INLINE && a->st->position!=POS_STATIC)reached=true;
        if(a->st && a->st->overflow!=OV_VISIBLE && !doc_viewport_overflow_box(d,a) && a->kind!=B_INLINE && reached)
            JS_SetPropertyUint32(ctx,clips,count++,rect_array(ctx,box_visual_x(a)-a->p[3]-sx,
                box_visual_y(a)-a->p[0]-sy,a->w+a->p[1]+a->p[3],a->h+a->p[0]+a->p[2]));
        if(a->st && a->st->position==POS_FIXED)break;
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
static bool element_viewport(web_doc *d,node_t *n) {
    if(n==d->html)return !d->quirks;
    if(!d->quirks || n!=d->body)return false;
    /* Quirks body forwards only when it is not potentially scrollable.
       A body with its own scrolling box keeps independent element offsets. */
    style_t *a=n->style,*p=n->parent?n->parent->style:NULL;
    return !n->box || !a || !p || a->overflow==OV_VISIBLE || a->overflow==OV_CLIP ||
           p->overflow==OV_VISIBLE || p->overflow==OV_CLIP;
}
static void viewport_scroll_position(struct web_js_state *s,int *x,int *y) {
    struct web_frame *f=s->doc->frame_element?web_frame_find(s->doc->frame_parent,s->doc->frame_element):NULL;
    *x=*y=0;
    if(f){*x=f->scroll_x;*y=f->scroll_y;}
    else if(s->host.scroll)s->host.scroll(s->host.opaque,x,y);
}
static struct web_js_state *geometry_owner(struct web_js_state *s,web_doc *d) {
    if(!d || !d->live || d->inert)return NULL;
    struct web_js_state *owner=d==s->doc?s:d->js;
    if(!owner || !owner->ctx || owner->disabled || owner->rt!=s->rt ||
       !web_frame_same_origin(s->doc,d))return NULL;
    return owner;
}
static bool viewport_scroll_to(struct web_js_state *s,double x,double y) {
    web_doc *d=s->doc;
    if(!isfinite(x))x=0;
    if(!isfinite(y))y=0;
    double content_w=MAX(d->doc_w,d->root_box?d->root_box->scroll_w:0);
    int a=(int)MIN(MAX(x,0),MIN((double)INT_MAX,MAX(0,content_w-d->width)));
    int b=(int)MIN(MAX(y,0),MIN((double)INT_MAX,(double)MAX(0,d->doc_h-d->height)));
    struct web_frame *f=d->frame_element?web_frame_find(d->frame_parent,d->frame_element):NULL;
    int old_x,old_y;viewport_scroll_position(s,&old_x,&old_y);
    if(f){if(f->scroll_x!=a || f->scroll_y!=b){f->scroll_x=a;f->scroll_y=b;d->paint_dirty=true;}}
    else if(s->host.scroll_to)s->host.scroll_to(s->host.opaque,a,b);
    int new_x,new_y;viewport_scroll_position(s,&new_x,&new_y);
    return old_x!=new_x || old_y!=new_y;
}
static JSValue geometry(struct web_js_state *s, node_t *n, const char *property) {
    JSContext *ctx = s->ctx;
    if (!n || n->type != N_ELEM) return JS_ThrowTypeError(ctx, "Geometry requires an element");
    struct web_js_state *owner=n->owner && n->owner->js && n->owner->js->rt==s->rt?n->owner->js:s;
    web_doc *d=owner->doc;
    bool visible = d->live && !owner->disabled && connected(owner,n);
    if (visible) flush_layout(owner);
    box_t *b = visible ? n->box : NULL;
    if(!strcmp(property,"scrollLeft") || !strcmp(property,"scrollTop")) {
        double v=0;
        if(d->quirks && n==d->html)return JS_NewFloat64(ctx,0);
        if(visible && element_viewport(d,n)){int x,y;viewport_scroll_position(owner,&x,&y);v=!strcmp(property,"scrollLeft")?x:y;}
        else if(b)v=!strcmp(property,"scrollLeft")?n->scroll_x:n->scroll_y;
        return JS_NewFloat64(ctx,v);
    }
    if(!strcmp(property,"scrollWidth") || !strcmp(property,"scrollHeight")) {
        float w=0,h=0;
        if(visible && element_viewport(d,n)){
            w=MAX(MAX(d->doc_w,d->width),d->root_box?d->root_box->scroll_w:0);
            h=MAX(MAX(d->doc_h,d->height),d->root_box?d->root_box->scroll_h:0);
        }
        else if(b)doc_element_scroll_metrics(d,n,&w,&h);
        double v=ceil(!strcmp(property,"scrollWidth")?w:h);
        return JS_NewInt32(ctx,(int32_t)MIN(MAX(v,0),INT_MAX));
    }
    node_t *parent = b ? offset_parent(owner,n) : NULL;
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
    if (!strcmp(property, "fill") || !strcmp(property, "stroke")) {
        const struct svg_paint *paint = !strcmp(property, "fill") ? &st->svg_fill : &st->svg_stroke;
        if (paint->kind == SVG_PAINT_NONE) return JS_NewString(ctx, "none");
        if (paint->kind == SVG_PAINT_REF) {
            char *absolute = NULL;
            int resolved = web_resolve_url_owned(d->url, paint->ref, &absolute);
            if (resolved < 0) return JS_ThrowInternalError(ctx, "SVG computed URL allocation failed");
            sbuf value = {0}; sb_puts(&value, "url(\""); sb_puts(&value, absolute ? absolute : paint->ref); sb_puts(&value, "\")");
            JSValue result = JS_NewStringLen(ctx, value.p, value.n);
            free(absolute); sb_free(&value); return result;
        }
        uint32_t c = paint->color == COLOR_CURRENT ? st->color : paint->color;
        snprintf(out, sizeof out, "rgba(%u, %u, %u, %g)", (c >> 16) & 255, (c >> 8) & 255, c & 255, (double)(c >> 24) / 255);
        return JS_NewString(ctx, out);
    }
    uint32_t color = 0; bool is_color = false; float px = 0; bool is_px = false;
    if (!strcmp(property, "color")) { color = st->color; is_color = true; }
    else if (!strcmp(property, "background-color")) { color = st->bg_color; is_color = true; }
    /* The layout engine currently resolves logical sides in horizontal LTR.
       Report that effective initial direction rather than an empty value:
       anchor controllers otherwise mistake the surface for RTL. This does
       not advertise RTL/bidi layout, which is not implemented yet. */
    else if (!strcmp(property, "direction")) snprintf(out, sizeof out, "ltr");
    else if (!strcmp(property, "font-size")) { px = st->font_size; is_px = true; }
    else if (!strcmp(property, "font-family")) return JS_NewString(ctx,st->font_names?st->font_names:st->font_family==FONT_FAMILY_MONO?"monospace":st->font_family==FONT_FAMILY_SANS?"sans-serif":"serif");
    else if (!strcmp(property, "font-weight")) snprintf(out, sizeof out, "%u", st->font_weight);
    else if (!strcmp(property, "opacity")) snprintf(out, sizeof out, "%g", (double)st->opacity);
    else if (!strcmp(property, "animation-name")) return JS_NewString(ctx,st->motion.name?st->motion.name:"none");
    else if (!strcmp(property, "animation-duration")) snprintf(out,sizeof out,"%gs",(double)st->motion.duration/1000);
    else if (!strcmp(property, "animation-delay")) snprintf(out,sizeof out,"%gs",(double)st->motion.delay/1000);
    else if (!strcmp(property, "animation-iteration-count")) {if(isfinite(st->motion.iterations))snprintf(out,sizeof out,"%g",(double)st->motion.iterations);else snprintf(out,sizeof out,"infinite");}
    else if (!strcmp(property, "animation-play-state")) snprintf(out,sizeof out,"%s",st->motion.paused?"paused":"running");
    else if (!strcmp(property, "animation-fill-mode")) {static const char *const names[]={"none","forwards","backwards","both"};snprintf(out,sizeof out,"%s",names[st->motion.fill<4?st->motion.fill:0]);}
    else if (!strcmp(property, "animation-direction")) {static const char *const names[]={"normal","reverse","alternate","alternate-reverse"};snprintf(out,sizeof out,"%s",names[st->motion.direction<4?st->motion.direction:0]);}
    else if (!strcmp(property, "animation-timing-function")) {const struct css_easing *e=&st->motion.easing;if(e->kind==CM_LINEAR)snprintf(out,sizeof out,"linear");else if(e->kind==CM_STEP_START)snprintf(out,sizeof out,"step-start");else if(e->kind==CM_STEP_END)snprintf(out,sizeof out,"step-end");else snprintf(out,sizeof out,"cubic-bezier(%g, %g, %g, %g)",(double)e->x1,(double)e->y1,(double)e->x2,(double)e->y2);}
    else if (!strcmp(property, "fill-opacity")) snprintf(out, sizeof out, "%g", (double)st->svg_fill_opacity);
    else if (!strcmp(property, "stroke-opacity")) snprintf(out, sizeof out, "%g", (double)st->svg_stroke_opacity);
    else if (!strcmp(property, "stop-opacity")) snprintf(out, sizeof out, "%g", (double)st->svg_stop_opacity);
    else if (!strcmp(property, "stop-color")) { color = st->svg_stop_color == COLOR_CURRENT ? st->color : st->svg_stop_color; is_color = true; }
    else if (!strcmp(property, "stroke-width")) {
        if (st->svg_stroke_width.pct) snprintf(out, sizeof out, "%g%%", (double)st->svg_stroke_width.pct);
        else { px = st->svg_stroke_width.px; is_px = true; }
    }
    else if (!strcmp(property, "fill-rule")) snprintf(out, sizeof out, "%s", st->svg_fill_rule ? "evenodd" : "nonzero");
    else if (!strcmp(property, "stroke-linecap")) { static const char *const names[] = {"butt", "round", "square"}; snprintf(out, sizeof out, "%s", names[st->svg_stroke_cap < 3 ? st->svg_stroke_cap : 0]); }
    else if (!strcmp(property, "stroke-linejoin")) { static const char *const names[] = {"miter", "round", "bevel"}; snprintf(out, sizeof out, "%s", names[st->svg_stroke_join < 3 ? st->svg_stroke_join : 0]); }
    else if (!strcmp(property, "flex-grow")) snprintf(out, sizeof out, "%g", (double)st->flex_grow);
    else if (!strcmp(property, "flex-shrink")) snprintf(out, sizeof out, "%g", (double)st->flex_shrink);
    else if (!strcmp(property, "order")) snprintf(out, sizeof out, "%d", st->order);
    else if (!strcmp(property,"visibility")) snprintf(out,sizeof out,"%s",st->visibility==0?"visible":st->visibility==2?"collapse":"hidden");
    else if (!strcmp(property,"content-visibility")) snprintf(out,sizeof out,"%s",st->content_visibility==CV_AUTO?"auto":st->content_visibility==CV_HIDDEN?"hidden":"visible");
    else if (!strcmp(property,"container-type")) snprintf(out,sizeof out,"%s",st->container_type==CT_SIZE?"size":st->container_type==CT_INLINE_SIZE?"inline-size":"normal");
    else if (!strcmp(property,"container-name")) return JS_NewString(ctx,st->container_names?st->container_names:"none");
    else if (!strcmp(property,"-webkit-box-orient")) snprintf(out,sizeof out,"%s",st->box_orient==BO_VERTICAL?"vertical":"horizontal");
    else if (!strcmp(property,"-webkit-line-clamp")) {if(st->line_clamp)snprintf(out,sizeof out,"%u",st->line_clamp);else snprintf(out,sizeof out,"none");}
    else if (!strcmp(property,"white-space")) {
        static const char *const names[]={"normal","pre","nowrap","pre-wrap","pre-line","break-spaces"};
        snprintf(out,sizeof out,"%s",names[st->white_space<6?st->white_space:0]);
    }
    else if (!strcmp(property,"overflow") || !strcmp(property,"overflow-x") || !strcmp(property,"overflow-y")) {
        static const char *const names[]={"visible","hidden","scroll","auto","clip"};
        snprintf(out,sizeof out,"%s",names[st->overflow<5?st->overflow:0]);
    }
    else if (!strcmp(property,"flex-direction")) {
        static const char *const names[]={"row","row-reverse","column","column-reverse"};
        snprintf(out,sizeof out,"%s",names[st->flex_direction<4?st->flex_direction:0]);
    }
    else if (!strcmp(property,"flex-wrap")) {
        static const char *const names[]={"nowrap","wrap","wrap-reverse"};
        snprintf(out,sizeof out,"%s",names[st->flex_wrap<3?st->flex_wrap:0]);
    }
    else if (!strcmp(property,"align-content")) {
        static const char *const names[]={"normal","stretch","flex-start","flex-end","center",
            "space-between","space-around","space-evenly","start","end"};
        snprintf(out,sizeof out,"%s",names[st->align_content<10?st->align_content:0]);
    }
    else if (!strcmp(property,"object-fit")) {
        static const char *const names[]={"fill","contain","cover","none","scale-down"};
        snprintf(out,sizeof out,"%s",st->object_fit<5?names[st->object_fit]:"fill");
    }
    else if (!strcmp(property, "display")) {
        static const char *const names[] = {"none", "inline", "block", "list-item", "inline-block", "table", "inline-table", "table-row-group", "table-header-group", "table-footer-group", "table-row", "table-cell", "table-column", "table-column-group", "table-caption", "flex", "inline-flex", "grid", "inline-grid", "contents", "flow-root"};
        if(st->legacy_box && !(st->box_orient==BO_VERTICAL && st->line_clamp)) {
            if(st->display==D_BLOCK)return JS_NewString(ctx,"-webkit-box");
            if(st->display==D_INLINE_BLOCK)return JS_NewString(ctx,"-webkit-inline-box");
        }
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
/* Private samples live in the animation cascade origin, never inline style.
   The numeric/color subset renders through the existing native CSS engine. */
static bool animated_property(const char *p) {
    static const char *const names[] = {"top", "right", "bottom", "left", "width", "height",
        "min-width", "max-width", "min-height", "max-height", "opacity", "color", "background-color",
        "border-radius", "font-size", "letter-spacing", "word-spacing",
        "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding-top", "padding-right", "padding-bottom", "padding-left",
        "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
        "border-top-color", "border-right-color", "border-bottom-color", "border-left-color"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(p, names[i])) return true;
    return false;
}
static JSValue animation_dom(struct web_js_state *s, node_t *n, bool read, int argc, JSValueConst *argv) {
    JSContext *ctx = s->ctx;
    if (!n || n->type != N_ELEM || argc < (read ? 4 : 5)) return JS_ThrowTypeError(ctx, "Animation Element receiver required");
    const char *pseudo = JS_ToCString(ctx, argv[2]);
    if (!pseudo) return JS_EXCEPTION;
    int pe = !*pseudo ? PE_NONE : !strcmp(pseudo, "::before") ? PE_BEFORE : !strcmp(pseudo, "::after") ? PE_AFTER : -1;
    JS_FreeCString(ctx, pseudo);
    if (pe < 0) return JS_ThrowSyntaxError(ctx, "Unsupported animation pseudo-element");
    if (read) {
        /* Detached nodes may retain an old smem pointer after a cascade. Do
           not dereference it; reconnection will produce fresh style values. */
        if (!connected(s, n)) return JS_NewString(ctx, "");
        const char *p = JS_ToCString(ctx, argv[3]);
        if (!p) return JS_EXCEPTION;
        if (s->doc->resources_dirty) doc_rescan(s->doc);
        flush_layout(s);
        style_t *st = n->animation_base_style ? n->animation_base_style : n->style;
        if (st && pe == PE_BEFORE) st = st->before;
        else if (st && pe == PE_AFTER) st = st->after;
        char out[128] = "";
        if (st) {
            const len_t *length = NULL; float px = 0; bool numeric = false, is_color = false;
            uint32_t color = 0;
            if (!strcmp(p, "opacity")) snprintf(out, sizeof out, "%g", (double)st->opacity);
            else if (!strcmp(p, "transform")) snprintf(out, sizeof out, "none"); /* not rendered yet */
            else if (!strcmp(p, "color")) { color = st->color; is_color = true; }
            else if (!strcmp(p, "background-color")) { color = st->bg_color; is_color = true; }
            else if (!strcmp(p, "border-radius")) { px = st->border_radius; numeric = true; }
            else if (!strcmp(p, "font-size")) { px = st->font_size; numeric = true; }
            else if (!strcmp(p, "letter-spacing")) { px = st->letter_spacing; numeric = true; }
            else if (!strcmp(p, "word-spacing")) { px = st->word_spacing; numeric = true; }
            else if (!strcmp(p, "width")) length = &st->width;
            else if (!strcmp(p, "height")) length = &st->height;
            else if (!strcmp(p, "min-width")) length = &st->min_width;
            else if (!strcmp(p, "max-width")) length = &st->max_width;
            else if (!strcmp(p, "min-height")) length = &st->min_height;
            else if (!strcmp(p, "max-height")) length = &st->max_height;
            else {
                static const char *const sides[] = {"top", "right", "bottom", "left"};
                for (int i = 0; i < 4; i++) {
                    char key[32];
                    if (!strcmp(p, sides[i])) length = &st->inset[i];
                    snprintf(key, sizeof key, "margin-%s", sides[i]); if (!strcmp(p, key)) length = &st->margin[i];
                    snprintf(key, sizeof key, "padding-%s", sides[i]); if (!strcmp(p, key)) length = &st->padding[i];
                    snprintf(key, sizeof key, "border-%s-width", sides[i]); if (!strcmp(p, key)) { px = st->border_width[i]; numeric = true; }
                    snprintf(key, sizeof key, "border-%s-color", sides[i]); if (!strcmp(p, key)) { color = st->border_color[i]; is_color = true; }
                }
            }
            if (length) {
                if (length->kind == LK_AUTO || length->kind == LK_NONE || length->kind == LK_NORMAL)
                    snprintf(out, sizeof out, "%s", length->kind == LK_AUTO ? "auto" : length->kind == LK_NONE ? "none" : "normal");
                else {
                    /* Resolve from the underlying declaration, not animated box
                       geometry: omitted keyframes cannot feed samples back. */
                    node_t *parent = pe ? n : doc_flat_parent(n);
                    float base = parent && parent->box ? (strstr(p, "height") || !strcmp(p,"top") || !strcmp(p,"bottom") ? parent->box->h : parent->box->w) : 0;
                    px = len_resolve(length, base); numeric = true;
                }
            }
            if (is_color) snprintf(out, sizeof out, "rgba(%u, %u, %u, %g)", (color >> 16) & 255, (color >> 8) & 255, color & 255, (double)(color >> 24) / 255);
            if (numeric) snprintf(out, sizeof out, "%gpx", (double)px);
        }
        JS_FreeCString(ctx, p); return JS_NewString(ctx, out);
    }
    uint32_t id;
    if (JS_ToUint32(ctx, &id, argv[3]) < 0) return JS_EXCEPTION;
    if (!id) return JS_ThrowRangeError(ctx, "Invalid animation id");
    sbuf text = {0}; JSValue result = JS_UNDEFINED;
    if (!JS_IsNull(argv[4])) {
        JSValue length = JS_GetPropertyStr(ctx, argv[4], "length"); uint32_t count = 0;
        int converted = JS_ToUint32(ctx, &count, length); JS_FreeValue(ctx, length);
        if (converted < 0) return JS_EXCEPTION;
        if (count % 2) return JS_ThrowRangeError(ctx, "Animation sample requires property/value pairs");
        for (uint32_t i = 0; i < count; i += 2) {
            JSValue key = JS_GetPropertyUint32(ctx, argv[4], i), val = JS_GetPropertyUint32(ctx, argv[4], i + 1);
            size_t kn = 0, vn = 0;
            const char *k = JS_ToCStringLen(ctx, &kn, key), *v = JS_ToCStringLen(ctx, &vn, val);
            JS_FreeValue(ctx, key); JS_FreeValue(ctx, val);
            if (!k || !v) result = JS_EXCEPTION;
            else if (strlen(k) != kn || strlen(v) != vn || strpbrk(v, ";{}!\r\n"))
                result = JS_ThrowRangeError(ctx, "Invalid animation property value");
            else if (animated_property(k)) {
                if (kn > SIZE_MAX - 3 || vn > SIZE_MAX - 3 - kn || text.n > SIZE_MAX - 3 - kn - vn)
                    result = JS_ThrowRangeError(ctx, "Animation sample size is not representable");
                else { sb_puts(&text, k); sb_putc(&text, ':'); sb_puts(&text, v); sb_putc(&text, ';'); }
            }
            JS_FreeCString(ctx, k); JS_FreeCString(ctx, v);
            if (JS_IsException(result)) break;
        }
    }
    if (!JS_IsException(result)) {
        int changed = css_animation_set(n, (uint8_t)pe, id, text.n ? sb_cstr(&text) : NULL);
        if (changed == -1) result = oom(ctx);
        else if (changed) {
            web_doc *d = n->owner ? n->owner : s->doc;
            d->need_style = d->dirty = true;
        }
    }
    sb_free(&text); return result;
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
static bool form_exotic_receiver(JSContext *ctx, JSValueConst object) {
    node_t *node=node_opaque(object); struct web_js_state *s=state(ctx);
    return node && node->type==N_ELEM && !node->foreign && node->tag==T_form && s && JS_IsObject(s->hooks);
}
static int form_get_own_property(JSContext *ctx, JSPropertyDescriptor *descriptor, JSValueConst object, JSAtom atom) {
    if(!form_exotic_receiver(ctx,object))return 0;
    JSValue key=JS_AtomToValue(ctx,atom);
    if(JS_IsException(key))return -1;
    if(!JS_IsString(key)){JS_FreeValue(ctx,key);return 0;}
    JSValue args[]={object,key},value=custom_element_hook(state(ctx),"formNamedProperty",2,args);
    const char *name=JS_ToCString(ctx,key); bool indexed=false;
    if(name && *name && (name[0]!='0' || !name[1])) {
        const char *p=name;while(isdigit((unsigned char)*p))p++;
        indexed=!*p && strtoull(name,NULL,10)<4294967295ULL;
    }
    bool valid_name=name!=NULL;JS_FreeCString(ctx,name);JS_FreeValue(ctx,key);
    if(JS_IsException(value))return -1;
    if(!valid_name){JS_FreeValue(ctx,value);return -1;}
    if(JS_IsUndefined(value)){JS_FreeValue(ctx,value);return 0;}
    if(descriptor){descriptor->flags=JS_PROP_CONFIGURABLE|(indexed?JS_PROP_ENUMERABLE:0);
        descriptor->value=value;descriptor->getter=descriptor->setter=JS_UNDEFINED;
    }else JS_FreeValue(ctx,value);
    return 1;
}
static int form_get_own_property_names(JSContext *ctx, JSPropertyEnum **table, uint32_t *length, JSValueConst object) {
    *table=NULL;*length=0;if(!form_exotic_receiver(ctx,object))return 0;
    JSValue keys=custom_element_hook(state(ctx),"formNamedKeys",1,&object);
    if(JS_IsException(keys))return -1;
    JSValue count=JS_GetPropertyStr(ctx,keys,"length");uint32_t n=0;
    int ok=JS_ToUint32(ctx,&n,count);JS_FreeValue(ctx,count);
    if(ok<0){JS_FreeValue(ctx,keys);return -1;}
    JSPropertyEnum *out=n?js_mallocz(ctx,(size_t)n*sizeof *out):NULL;
    if(n && !out){JS_FreeValue(ctx,keys);return -1;}
    for(uint32_t i=0;i<n;i++){
        JSValue key=JS_GetPropertyUint32(ctx,keys,i);out[i].atom=JS_IsException(key)?JS_ATOM_NULL:JS_ValueToAtom(ctx,key);JS_FreeValue(ctx,key);
        if(!out[i].atom){for(uint32_t j=0;j<i;j++)JS_FreeAtom(ctx,out[j].atom);js_free(ctx,out);JS_FreeValue(ctx,keys);return -1;}
    }
    JS_FreeValue(ctx,keys);*table=out;*length=n;return 0;
}
static JSClassExoticMethods node_exotic={.get_own_property=form_get_own_property,.get_own_property_names=form_get_own_property_names};

void web_js_face_reset(web_doc *d, node_t *form) {
    web_doc *live=d->dom_family?d->dom_family:d;
    struct web_js_state *s=live?live->js:NULL;
    if(!s||s->disabled)return;
    begin_task(s);JSValue arg=wrap(s,form),result=custom_element_hook(s,"customFormReset",1,&arg);
    if(JS_IsException(result))exception(s);else JS_FreeValue(s->ctx,result);
    JS_FreeValue(s->ctx,arg);end_task(s);
}

static JSValue attribute_native_changed(struct web_js_state *s, node_t *n, const char *name, bool old_present) {
    JSContext *ctx = s->ctx;
    const char *value = node_attr(n, name);
    if (n->tag == T_script && !n->foreign && !strcmp(name, "async") && value && !old_present) {
        n->js_force_async_set = true; n->js_force_async = false;
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
        n->js_image_notified = false;
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
static JSValue files_dom(struct web_js_state *s, node_t *n, int argc, JSValueConst *argv) {
    JSContext *ctx = s->ctx; web_doc *d = n && n->owner ? n->owner : s->doc;
    if (!web_input_is_file(n) || argc < 3) return JS_ThrowTypeError(ctx, "File input and data required");
    JSValue length = JS_GetPropertyStr(ctx, argv[2], "length"); uint32_t count = 0;
    int status = JS_ToUint32(ctx, &count, length); JS_FreeValue(ctx, length);
    if (status < 0) return JS_EXCEPTION;
    struct web_form_files *files = web_input_files_create(); if (!files) return oom(ctx);
    if (!web_input_files_reserve(files, count)) { web_input_files_dispose(files); return oom(ctx); }
    bool ok = true;
    bool allocation_failed = false;
    for (uint32_t index = 0; index < count && ok; index++) {
        JSValue snapshot = JS_GetPropertyUint32(ctx, argv[2], index);
        JSValue buffer = JS_GetPropertyUint32(ctx, snapshot, 0);
        size_t size = 0; uint8_t *bytes = JS_GetArrayBuffer(ctx, &size, buffer);
        struct web_form_file *file = &files->files[index];
        if ((!bytes && size) || (size ? size : 1) > SIZE_MAX - files->allocation) ok = false;
        JSValue name_value = JS_GetPropertyUint32(ctx, snapshot, 2), type_value = JS_GetPropertyUint32(ctx, snapshot, 1), relative_value = JS_GetPropertyUint32(ctx, snapshot, 4);
        const char *name = JS_ToCString(ctx, name_value), *type = JS_ToCString(ctx, type_value), *relative = JS_ToCString(ctx, relative_value);
        if (!name || !type || !relative || strlen(name) >= sizeof file->name || strlen(type) >= sizeof file->type || strlen(relative) >= sizeof file->relative_path || strchr(name,'/') || strchr(name,'\\')) ok = false;
        if (ok) {
            file->bytes = malloc(size ? size : 1); if (!file->bytes) { ok = false; allocation_failed = true; }
            else { if (size) memcpy(file->bytes, bytes, size); file->size = size; strcpy(file->name,name); strcpy(file->type,type); strcpy(file->relative_path,relative); files->allocation += size ? size : 1; files->count++; }
            JSValue modified = JS_GetPropertyUint32(ctx, snapshot, 3); if (JS_ToInt64(ctx,&file->last_modified,modified) < 0) ok = false; JS_FreeValue(ctx,modified);
        }
        JS_FreeCString(ctx,name); JS_FreeCString(ctx,type); JS_FreeCString(ctx,relative);
        JS_FreeValue(ctx,name_value); JS_FreeValue(ctx,type_value); JS_FreeValue(ctx,relative_value); JS_FreeValue(ctx,buffer); JS_FreeValue(ctx,snapshot);
    }
    if (ok) ok = web_input_files_replace(d,n,files);
    web_input_files_dispose(files);
    return ok ? JS_UNDEFINED : allocation_failed ? oom(ctx) : JS_ThrowRangeError(ctx,"File selection metadata or size is not representable");
}

/* Packets come only from the private FormData/Blob brands; all exported
   bytes are copied into native node ownership before the JS call returns. */
static bool face_copy_string(JSContext *ctx, JSValueConst value, char **copy,
                             size_t *length, struct web_face_value *v) {
    size_t size=0; const char *text=JS_ToCStringLen(ctx,&size,value);
    if(!text)return false;
    if(size==SIZE_MAX || size+1>SIZE_MAX-v->allocation){
        JS_FreeCString(ctx,text);JS_ThrowRangeError(ctx,"FACE value size overflow");return false;
    }
    char *bytes=malloc(size+1);
    if(!bytes){JS_FreeCString(ctx,text);oom(ctx);return false;}
    memcpy(bytes,text,size);bytes[size]=0;JS_FreeCString(ctx,text);
    *copy=bytes;if(length)*length=size;v->allocation+=size+1;return true;
}
static bool face_packet(JSContext *ctx, JSValueConst packet, struct web_face_value **out) {
    *out=NULL;if(JS_IsNull(packet))return true;
    JSValue single=JS_GetPropertyUint32(ctx,packet,0),entries=JS_GetPropertyUint32(ctx,packet,1);
    int boolean=JS_IsException(single)?-1:JS_ToBool(ctx,single);
    JSValue length=JS_GetPropertyStr(ctx,entries,"length");uint32_t count=0;
    bool ok=boolean>=0&&!JS_IsException(entries)&&!JS_IsException(length)&&JS_ToUint32(ctx,&count,length)>=0;
    JS_FreeValue(ctx,single);JS_FreeValue(ctx,length);
    if(ok&&boolean&&count!=1){JS_ThrowTypeError(ctx,"Single FACE value requires one entry");ok=false;}
    struct web_face_value *v=ok?web_face_value_create(boolean!=0):NULL;
    if(ok&&!v){oom(ctx);ok=false;}
    if(ok&&!web_face_value_reserve(v,count)){oom(ctx);ok=false;}
    for(uint32_t i=0;ok&&i<count;i++){
        JSValue entry=JS_GetPropertyUint32(ctx,entries,i),fields[5];
        for(unsigned j=0;j<5;j++)fields[j]=JS_GetPropertyUint32(ctx,entry,j);
        struct web_face_entry *e=&v->entries[i];v->count=i+1;
        for(unsigned j=0;j<5;j++)if(JS_IsException(fields[j]))ok=false;
        if(ok)ok=face_copy_string(ctx,fields[0],&e->name,&e->name_length,v);
        e->file=!JS_IsNull(fields[2]);
        if(ok&&e->file){
            size_t size=0;unsigned char *bytes=JS_GetArrayBuffer(ctx,&size,fields[1]);
            if(!bytes&&size){ok=false;}
            else if((size?size:1)>SIZE_MAX-v->allocation){JS_ThrowRangeError(ctx,"FACE file size overflow");ok=false;}
            else {
                e->bytes=malloc(size?size:1);
                if(!e->bytes){oom(ctx);ok=false;}
                else {if(size)memcpy(e->bytes,bytes,size);e->size=size;v->allocation+=size?size:1;}
            }
            if(ok)ok=face_copy_string(ctx,fields[2],&e->filename,&e->filename_length,v);
            if(ok)ok=face_copy_string(ctx,fields[3],&e->mime,NULL,v);
            if(ok)ok=JS_ToInt64(ctx,&e->last_modified,fields[4])>=0;
        }else if(ok){
            char *text=NULL;ok=face_copy_string(ctx,fields[1],&text,&e->size,v);e->bytes=(unsigned char*)text;
        }
        for(unsigned j=0;j<5;j++)JS_FreeValue(ctx,fields[j]);JS_FreeValue(ctx,entry);
    }
    JS_FreeValue(ctx,entries);
    if(!ok){web_face_value_free(v);return false;}*out=v;return true;
}
static node_t *face_tree_next(node_t *root,node_t *n){
    if(n->first&&!(n->type==N_ELEM&&!n->foreign&&n->tag==T_template))return n->first;
    while(n!=root&&!n->next)n=n->parent;
    return n==root?NULL:n->next;
}
static JSValue face_controls_dom(struct web_js_state *s,node_t *root,int argc,JSValueConst *argv,bool listed){
    JSContext *ctx=s->ctx;node_t *form=listed&&argc>2?unwrap(ctx,argv[2]):NULL;
    bool images=listed && argc>3 && JS_ToBool(ctx,argv[3])>0;
    if(!root||(listed&&(!form||form->type!=N_ELEM||form->foreign||form->tag!=T_form)))return JS_ThrowTypeError(ctx,"Native form receiver required");
    if(!listed&&root->type==N_ELEM&&!root->foreign&&root->tag==T_form)root=doc_node_root(root,false);
    JSValue array=JS_NewArray(ctx);uint32_t index=0;
    for(node_t *p=root;!JS_IsException(array)&&p;p=face_tree_next(root,p)){
        if(p->type!=N_ELEM||p->foreign)continue;
        bool include=p->face_associated;
        if(images)include=p->tag==T_img;
        else if(listed)include=include||p->tag==T_button||p->tag==T_fieldset||p->tag==T_object||p->tag==T_output||p->tag==T_select||p->tag==T_textarea||
            (p->tag==T_input&&web_input_type(p)!=WEB_INPUT_IMAGE);
        if(include&&(!listed||web_form_owner(form->owner, p)==form)&&JS_SetPropertyUint32(ctx,array,index++,wrap(s,p))<0){JS_FreeValue(ctx,array);array=JS_EXCEPTION;}
    }
    return array;
}
static JSValue face_dom(struct web_js_state *s,node_t *n,int argc,JSValueConst *argv){
    JSContext *ctx=s->ctx;web_doc *d=n&&n->owner?n->owner:s->doc;
    if(!n||n->type!=N_ELEM||n->foreign||argc<3)return JS_ThrowTypeError(ctx,"Native HTMLElement receiver required");
    const char *op=JS_ToCString(ctx,argv[2]);if(!op)return JS_EXCEPTION;
    JSValue result=JS_UNDEFINED;
    if(!strcmp(op,"metadata")){
        int associated=argc>3?JS_ToBool(ctx,argv[3]):-1;
        if(associated<0)result=JS_EXCEPTION;
        else if(!web_face_prepare(d,n,associated!=0))result=JS_ThrowRangeError(ctx,"Native internals invalid metadata or allocation/size failure");
    }else if(!strcmp(op,"associated"))result=JS_NewBool(ctx,n->face_associated);
    else if(!n->internals)result=JS_ThrowTypeError(ctx,"Custom element definition metadata required");
    else if(!strcmp(op,"attach")){
        if(n->internals->attached)result=JS_ThrowTypeError(ctx,"Internals already attached");else n->internals->attached=true;
    }else if(!n->face_associated)result=JS_ThrowTypeError(ctx,"Form-associated custom element required");
    else if(!strcmp(op,"form"))result=wrap(s,web_form_owner(d,n));
    else if(!strcmp(op,"message"))result=JS_NewStringLen(ctx,n->internals->message?n->internals->message:"",n->internals->message_length);
    else if(!strcmp(op,"disabled"))result=JS_NewBool(ctx,web_control_disabled(n));
    else if(!strcmp(op,"labels")){
        result=JS_NewArray(ctx);uint32_t index=0;node_t *root=doc_node_root(n,false);
        for(node_t *p=root;!JS_IsException(result)&&p;p=face_tree_next(root,p))
            if(web_label_control(p)==n&&JS_SetPropertyUint32(ctx,result,index++,wrap(s,p))<0){JS_FreeValue(ctx,result);result=JS_EXCEPTION;}
    }else if(!strcmp(op,"validity")){
        uint32_t bits=0;size_t length=0;const char *message=NULL;
        node_t *anchor=argc>5&&!JS_IsUndefined(argv[5])?unwrap(ctx,argv[5]):n;
        /* WebIDL HTMLElement conversion precedes the method body. In contrast,
           HTML setValidity steps 6-8 commit flags/message before step 10's
           NotFoundError; do not invent atomic rollback for a real HTMLElement. */
        if(!anchor||anchor->type!=N_ELEM||anchor->foreign)result=JS_ThrowTypeError(ctx,"HTMLElement validation anchor required");
        else if(argc<5||JS_ToUint32(ctx,&bits,argv[3])<0)result=JS_EXCEPTION;
        else if(!(message=JS_ToCStringLen(ctx,&length,argv[4])))result=JS_EXCEPTION;
        else if((bits&1023u)&&!length)result=JS_ThrowTypeError(ctx,"Invalid flags require a nonempty message");
        else if(!web_face_set_validity(d,n,bits,message,length))result=JS_ThrowRangeError(ctx,"Native validity invalid metadata or allocation/size failure");
        else {
            node_t *p=anchor;while(p&&p!=n)p=p->parent?p->parent:p->shadow_host;
            if(p!=n)result=JS_NewInt32(ctx,2);else n->internals->anchor=anchor;
        }
        JS_FreeCString(ctx,message);
    }else if(!strcmp(op,"value")){
        struct web_face_value *value=NULL,*state=NULL;bool shared=argc<5;
        bool ok=argc>3&&face_packet(ctx,argv[3],&value);
        if(ok){if(shared)state=value;else ok=face_packet(ctx,argv[4],&state);}
        if(!ok)result=JS_EXCEPTION;
        else if(!web_face_set_value(d,n,value,state))result=JS_ThrowRangeError(ctx,"Native form value invalid metadata or allocation/size failure");
        web_face_value_free(value);if(!shared)web_face_value_free(state);
    }else result=JS_ThrowTypeError(ctx,"Unknown native internals operation");
    JS_FreeCString(ctx,op);return result;
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
        size_t length=web_control_validation_message_length(d,n);
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
/* This final branch does not revalidate or fire submit: its callers already
   did so, except HTMLFormElement.submit(), which deliberately bypasses both.
   A recognized dialog method never falls back to navigation, even on failure. */
int web_js_dialog_submit(web_doc *d, web_node *submitter) {
    if(!web_sandbox_forms_allowed(d) || (d->js && (sandbox_authority(d->js)&SB_FORMS)))return -1;
    node_t *dialog; const char *result;
    int branch = web_dialog_submission(d, submitter, &dialog, &result);
    if (branch != 1) return branch;
    struct web_js_state *s = d->js;
    if (!s || s->disabled || s->doc != d || !JS_IsObject(s->hooks)) return -1;
    bool outer = !s->running;
    if (outer) begin_task(s);
    JSContext *ctx = s->ctx;
    JSValue fn = JS_GetPropertyStr(ctx, s->hooks, "dialogSubmit");
    JSValue args[] = {wrap(s, dialog), str_or_null(ctx, result)};
    JSValue closed = JS_UNDEFINED;
    int status = -1;
    if (JS_IsException(fn) || JS_IsException(args[0]) || JS_IsException(args[1])) exception(s);
    else if (JS_IsFunction(ctx, fn)) {
        closed = JS_Call(ctx, fn, s->hooks, 2, args);
        if (JS_IsException(closed)) exception(s);
        else if (JS_ToBool(ctx, closed) > 0) status = 1;
    }
    JS_FreeValue(ctx, closed); JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
    if (outer) end_task(s);
    return status;
}
static void navigate_form_request(struct web_js_state *s, node_t *submitter) {
    if(sandbox_authority(s)&SB_FORMS){log_text(s,1,"Sandbox denied form submission");return;}
    if (web_js_dialog_submit(s->doc,submitter) != 0) return;
    if(s->doc->frame_parent){web_js_console(s->doc,2,"Child form navigation requires a destination/origin-aware embedder and is not implemented");return;}
    struct web_form_request request;
    if (!web_submit_request(s->doc,submitter,&request)) return;
    web_autocomplete_record(s->doc,submitter,request.url);
    if (s->host.navigate_form) s->host.navigate_form(s->host.opaque,request.url,request.body,
        request.body_len,request.content_type,request.target);
    else if (s->host.navigate && (!request.body ||
        (request.content_type && !strcmp(request.content_type,"application/x-www-form-urlencoded"))) &&
        (!strcasecmp(request.target,"_self") || !strcasecmp(request.target,"_top") || !strcasecmp(request.target,"_parent")))
        s->host.navigate(s->host.opaque,request.url,request.body);
    else web_js_console(s->doc,2,"The embedder cannot preserve this form encoding or destination");
    web_submit_request_free(&request);
}
#include "js_document_commands.h"
#include "js_cssom.h"
static JSValue native_dom_hooks(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    (void)this_val;struct web_js_state *s=state(ctx);int32_t kind=0;
    if(argc<2||JS_ToInt32(ctx,&kind,argv[0])<0)return JS_EXCEPTION;
    bool active=JS_ToBool(ctx,argv[1])>0;
    if(kind==0)s->ce_hooks_active=active;else if(kind==1)s->range_hooks_active=active;
    else if(kind==2)s->face_hooks_active=active;
    return JS_UNDEFINED;
}
static bool dom_custom_subtree(node_t *node) {
    if(!node)return false;
    node_t *current=node;
    for(;;){
        if(custom_candidate(current))return true;
        if(current->shadow_root&&dom_custom_subtree(current->shadow_root))return true;
        if(current->first){current=current->first;continue;}
        while(current!=node&&!current->next)current=current->parent;
        if(current==node)return false;current=current->next;
    }
}
static bool dom_synchronous_hooks(struct web_js_state *s,int opcode,node_t *n,int argc,JSValueConst *argv) {
    node_t *incoming=argc>2?node_opaque(argv[2]):NULL,*old=argc>3?node_opaque(argv[3]):NULL;
    if(s->face_hooks_active)return true; /* form/id/fieldset changes affect FACE */
    bool structure=opcode==DOM_insert||opcode==DOM_replace||opcode==DOM_remove||opcode==DOM_adopt;
    if(s->range_hooks_active&&structure)return true;
    if(opcode==DOM_set&&argc>2&&JS_IsString(argv[2])){
        const char *key=JS_ToCString(s->ctx,argv[2]);
        bool content=key&&(!strcmp(key,"innerHTML")||!strcmp(key,"textContent")||!strcmp(key,"nodeValue")||!strcmp(key,"title"));
        bool markup=key&&!strcmp(key,"innerHTML");JS_FreeCString(s->ctx,key);
        if(s->range_hooks_active&&content)return true;
        if(s->ce_hooks_active&&(markup||(content&&dom_custom_subtree(n))))return true;
    }
    if(!s->ce_hooks_active)return false;
    if(opcode==DOM_adopt&&incoming&&incoming->type==N_ATTR)return custom_candidate(incoming->attr_owner);
    if(structure||opcode==DOM_clone||opcode==DOM_import){
        return dom_custom_subtree(opcode==DOM_remove||opcode==DOM_clone?n:incoming)||
            (opcode==DOM_replace&&dom_custom_subtree(old));
    }
    if(n&&n->type==N_ATTR)n=n->attr_owner;
    return custom_candidate(n);
}
static JSValue native_dom_impl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int opcode) {
    struct web_js_state *s = state(ctx); web_doc *d = s->doc;
    if (argc < 2) return JS_ThrowTypeError(ctx, "DOM operation requires a receiver");
    node_t *n = unwrap(ctx, argv[1]);
    if (!n && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) { return JS_EXCEPTION; }
    if (n && n->owner) d = n->owner;
    if(!s->starting && ((sandbox_authority(s)&SB_SCRIPTS) ||
       (sandbox_actor(s)->doc->sandbox_flags && sandbox_actor(s)->doc!=d)))
        return history_security_error(ctx,"Sandbox denied borrowed document authority");
    doc_dom_budget(d);
    bool mutation = opcode == DOM_shadowAttach || opcode == DOM_slotAssign || opcode == DOM_insert || opcode == DOM_replace || opcode == DOM_remove || opcode == DOM_adopt || opcode == DOM_clone || opcode == DOM_import || opcode == DOM_set ||
                    opcode == DOM_attrSetNode || opcode == DOM_attrRemoveNode ||
                    ((opcode == DOM_attr || opcode == DOM_style) && argc > 3) || (opcode == DOM_attrNS && argc > 4);
    bool synchronous_hooks=mutation&&(s->ce_hooks_active||s->range_hooks_active)&&dom_synchronous_hooks(s,opcode,n,argc,argv);
    JSValue operation=synchronous_hooks ? JS_NewString(ctx,dom_opcode_names[opcode]) : JS_UNDEFINED;
    if(JS_IsException(operation))return operation;
    /* Only the synchronous CE/Range slow path needs an operation string. */
    if(synchronous_hooks)((JSValue *)argv)[0]=operation;
    JSValue ce_token = synchronous_hooks ? custom_element_hook(s, "customElementBefore", argc, argv) : JS_NULL;
    JS_FreeValue(ctx,operation);
    if (JS_IsException(ce_token)) { return ce_token; }
    struct js_mutation_snapshot observed=opcode==DOM_set?(struct js_mutation_snapshot){0}:mutation_before(s,opcode,n,argc,argv);
    JSValue result = JS_UNDEFINED;
    const char *p = NULL;
    node_t *attribute_target = NULL;
    const char *attribute_name = NULL;
    bool old_attribute_present = false;
    char *created_attribute_name = NULL;
    if (mutation && n && n->type == N_ATTR && opcode == DOM_set && n->attr_owner && !n->attribute->namespace_uri) {
        attribute_target = n->attr_owner; attribute_name = n->attribute->local ? n->attribute->local : n->attribute->raw;
        old_attribute_present = true;
    }
    if (opcode == DOM_adopt && argc > 2) {
        node_t *a = unwrap(ctx, argv[2]);
        if (a && a->type == N_ATTR && a->attr_owner && !a->attribute->namespace_uri) {
            attribute_target = a->attr_owner; attribute_name = a->attribute->local ? a->attribute->local : a->attribute->raw;
            old_attribute_present = true;
        }
    }
    if(!n){switch(opcode){
    case DOM_dialogEnter: case DOM_dialogFocus: case DOM_dialogLeave: case DOM_dialogModal: case DOM_dialogPrepare: case DOM_root: case DOM_shadowAttach: case DOM_slotNodes: case DOM_slotAssign: case DOM_attrCreate: case DOM_attrList: case DOM_attrNode: case DOM_attrNodeNS: case DOM_attrNS: case DOM_attrSetNode: case DOM_attrRemoveNode: case DOM_documentCommand: case DOM_selection: case DOM_filesSet: case DOM_formValue: case DOM_face: case DOM_faceControls: case DOM_formControls: case DOM_validation: case DOM_observerGeometry: case DOM_elementScrollIntoView: case DOM_elementScroll: case DOM_imageDecode: case DOM_insert: case DOM_replace: case DOM_remove: case DOM_position: case DOM_equal: case DOM_same: case DOM_adopt: case DOM_import: case DOM_clone: case DOM_customCandidates: case DOM_rect: case DOM_submit: case DOM_reset: case DOM_get: case DOM_set: case DOM_attr: case DOM_id: case DOM_query: case DOM_matches: case DOM_style: case DOM_stylePriority: case DOM_computed: case DOM_geometry:
        result = JS_ThrowTypeError(ctx, "Invalid DOM receiver");goto dom_complete;
    default:break;
    }}
    switch(opcode){
    case DOM_get: case DOM_set: case DOM_attr: case DOM_id: case DOM_query: case DOM_matches: case DOM_style: case DOM_stylePriority: case DOM_computed: case DOM_geometry:
        if(argc<3 || !(p=JS_ToCString(ctx,argv[2]))){result=JS_EXCEPTION;goto dom_complete;}
        break;
    default:break;
    }
    switch(opcode){
    case DOM_isNode: {
        result = JS_NewBool(ctx, argc > 2 && node_opaque(argv[2]) != NULL);

    } break;
    case DOM_animationStyle: case DOM_animationComputed: {
        result = animation_dom(s, n, opcode == DOM_animationComputed, argc, argv);

    } break;
    case DOM_cssom: {
        result=cssom_dom(s,n,argc,argv);

    } break;
    case DOM_styleDisabled: {
        if (!n || n->type != N_ELEM || n->foreign || n->tag != T_style)
            result = JS_ThrowTypeError(ctx, "HTMLStyleElement receiver required");
        else if (argc > 2) {
            bool disabled = JS_ToBool(ctx, argv[2]) > 0;
            if (disabled != n->style_disabled) {
                n->style_disabled = disabled;
                /* This changes the native cascade, not DOM attributes or text:
                   no fabricated MutationObserver/CE reaction is generated. */
                d->resources_dirty = d->dirty = d->need_style = true;
            }
        } else result = JS_NewBool(ctx, n->style_disabled);

    } break;
    case DOM_slotChanges: {
        web_doc *family = s->doc->dom_family ? s->doc->dom_family : s->doc;
        result = JS_NewArray(ctx); uint32_t index = 0;
        doc_shadow_flush(d);
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

    } break;
    case DOM_parseDocument: {
        size_t len; const char *html = argc > 2 ? JS_ToCStringLen(ctx, &len, argv[2]) : NULL;
        bool blank = argc > 3 && JS_ToBool(ctx, argv[3]) > 0;
        web_doc *made = html ? doc_inert(s->doc, html, len, blank ? "about:blank" : s->doc->url) : NULL;
        result = !html ? JS_EXCEPTION : made ? wrap(s, made->root) : oom(ctx);
        JS_FreeCString(ctx, html);

    } break;
    case DOM_parseFragment: {
        size_t len = 0; const char *html = argc > 2 ? JS_ToCStringLen(ctx, &len, argv[2]) : NULL;
        if (!n || n->type != N_ELEM) result = JS_ThrowTypeError(ctx, "Element context required");
        else if (!html) result = JS_EXCEPTION;
        else {
            bool contextual=argc>3 && JS_ToBool(ctx,argv[3])>0;
            node_t *made=contextual?html_contextual_fragment(d,n,html,len):html_fragment(d,n,html,len);
            result=made?wrap(s,made):oom(ctx);
        }
        JS_FreeCString(ctx, html);

    } break;
    case DOM_doctypeCreate: {
        size_t nl = 0, pl = 0, sl = 0;
        const char *name = argc > 2 ? JS_ToCStringLen(ctx, &nl, argv[2]) : NULL;
        const char *public_id = argc > 3 ? JS_ToCStringLen(ctx, &pl, argv[3]) : NULL;
        const char *system_id = argc > 4 ? JS_ToCStringLen(ctx, &sl, argv[4]) : NULL;
        if (!n || n->type != N_DOC) result = JS_ThrowTypeError(ctx, "Document receiver required");
        else if (!name || !public_id || !system_id) result = JS_EXCEPTION;
        else {
            node_t *made = doc_doctype_create(d, name, nl, public_id, pl, system_id, sl);
            result = made ? wrap(s, made) : oom(ctx);
        }
        JS_FreeCString(ctx, name); JS_FreeCString(ctx, public_id); JS_FreeCString(ctx, system_id);

    } break;
    case DOM_insertionStatus: {
        node_t *child = argc > 2 ? unwrap(ctx, argv[2]) : NULL;
        node_t *before = argc > 3 && !JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3]) ? unwrap(ctx, argv[3]) : NULL;
        if (!n || !child || (argc > 3 && !JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3]) && !before)) result = JS_ThrowTypeError(ctx, "Node arguments are required");
        else result = JS_NewInt32(ctx, doc_node_insert_validity(n, child, before));

    } break;
    case DOM_replacementStatus: {
        node_t *child=argc>2?unwrap(ctx,argv[2]):NULL,*old=argc>3?unwrap(ctx,argv[3]):NULL;
        if(!n || !child || !old)result=JS_ThrowTypeError(ctx,"Node arguments are required");
        else result=JS_NewInt32(ctx,doc_node_replace_validity(n,child,old));

    } break;
    case DOM_create: {
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
                if (made && made->type == N_ELEM && !made->foreign && made->tag == T_template && !doc_template_content(d, made)) {
                    doc_create_diagnostic(d,DOC_CREATE_TEMPLATE,type,name_len,len);made=NULL;
                }
                result = invalid_pi_data ? JS_ThrowTypeError(ctx, "Invalid processing instruction data") : made ? wrap(s, made) : oom(ctx);
                if(made && JS_IsException(result))doc_create_diagnostic(d,DOC_CREATE_WRAPPER,type,name_len,len);
                if (made && made->tag == T_script) {
                    made->js_force_async_set = true; made->js_force_async = true;
                }
            }
            JS_FreeCString(ctx, name); JS_FreeCString(ctx, text);
        }

    } break;
    case DOM_elementFromPoint: {
        double x, y;
        struct web_js_state *owner=NULL;
        if (!n || (n->type != N_DOC && !(n->type == N_FRAGMENT && n->shadow_host)))
            result = JS_ThrowTypeError(ctx, "Document or ShadowRoot receiver required");
        else if (argc < 4) result = JS_ThrowTypeError(ctx, "Two coordinates required");
        else if (JS_ToFloat64(ctx, &x, argv[2]) || JS_ToFloat64(ctx, &y, argv[3]))
            result = JS_EXCEPTION;
        else if (!(owner=geometry_owner(s,d)) || !isfinite(x) || !isfinite(y) ||
                 x < 0 || y < 0 || x >= d->width || y >= d->height) result = JS_NULL;
        else {
            flush_layout(owner);
            viewport_scroll_position(owner,&d->view_x,&d->view_y);
            int64_t px = (int64_t)x + d->view_x, py = (int64_t)y + d->view_y;
            node_t *hit = px < INT32_MIN || px > INT32_MAX || py < INT32_MIN || py > INT32_MAX ? NULL :
                          doc_element_at(d, (int)px, (int)py);
            /* Return the host at each shadow boundary outside this scope,
               never expose a closed root through a Document query. */
            while (hit) {
                node_t *scope = doc_node_root(hit, false);
                if (!scope || !scope->shadow_host) break; /* slotted light DOM */
                bool within = false;
                for (node_t *ancestor = n; ancestor; ancestor = doc_shadow_parent(ancestor))
                    if (ancestor == scope) { within = true; break; }
                if (within) break;
                hit = scope->shadow_host;
            }
            result = wrap(s, hit);
        }

    } break;
    case DOM_viewport: { int32_t axis = 0; if (argc > 2) JS_ToInt32(ctx, &axis, argv[2]); result = JS_NewInt32(ctx, axis ? d->height : d->width);
    } break;
    case DOM_focus: case DOM_blur: {
        node_t *old = d->focus;
        bool permitted = true;
        if (opcode == DOM_blur) {
            permitted = n && old == n;
            if (n && n->shadow_root && n->shadow_root->shadow_delegates_focus) {
                for (node_t *target = old; target; target = doc_shadow_parent(target))
                    if (target == n->shadow_root) { permitted = true; break; }
            }
            n = NULL;
        }
        if (permitted) web_js_focus_control(d,n);
        d->dirty = true;

    } break;
    case DOM_dialogEnter: case DOM_dialogFocus: case DOM_dialogLeave: case DOM_dialogModal: case DOM_dialogPrepare: {
        if(n->type!=N_ELEM || n->foreign || n->tag!=T_dialog) result=JS_ThrowTypeError(ctx,"HTMLDialogElement receiver required");
        else if(opcode == DOM_dialogPrepare) result=JS_NewInt32(ctx,d==s->doc?web_dialog_prepare(d,n):WEB_DIALOG_INACTIVE);
        else if(opcode == DOM_dialogEnter) result=JS_NewBool(ctx,d==s->doc && web_dialog_enter(d,n));
        else if(opcode == DOM_dialogModal) result=JS_NewBool(ctx,web_dialog_is_modal(d,n));
        else if(opcode == DOM_dialogLeave) result=wrap(s,web_dialog_leave(d,n));
        else if(opcode == DOM_dialogFocus) result=wrap(s,d==s->doc?web_dialog_focus_target(d,n):NULL);
        else result=JS_ThrowTypeError(ctx,"Unknown dialog operation");

    } break;
    case DOM_root: {result = wrap(s, doc_node_root(n, argc > 2 && JS_ToBool(ctx, argv[2]) > 0));
    } break;
    case DOM_shadowAttach: {
        if (argc < 7 || !doc_shadow_host_valid(n) || n->shadow_root) result = JS_ThrowTypeError(ctx, "Invalid shadow host");
        else {
            node_t *root = doc_shadow_attach(d, n, JS_ToBool(ctx, argv[2]) > 0, JS_ToBool(ctx, argv[3]) > 0,
                JS_ToBool(ctx, argv[4]) > 0, JS_ToBool(ctx, argv[5]) > 0, JS_ToBool(ctx, argv[6]) > 0);
            result = root ? wrap(s, root) : oom(ctx);
        }

    } break;
    case DOM_slotNodes: {
        if (n->type != N_ELEM || n->foreign || n->tag != T_slot) result = JS_ThrowTypeError(ctx, "HTMLSlotElement receiver required");
        else {
            pvec nodes = {0};
            if (!doc_slot_nodes(n, argc > 2 && JS_ToBool(ctx, argv[2]) > 0, &nodes)) {
                result = oom(ctx);
            } else {
                result = JS_NewArray(ctx);
                for (int i = 0; !JS_IsException(result) && i < nodes.n; i++) {
                    JSValue object = wrap(s, nodes.v[i]);
                    if (JS_IsException(object) || JS_SetPropertyUint32(ctx, result, i, object) < 0) { JS_FreeValue(ctx, result); result = JS_EXCEPTION; }
                }
            }
            pv_free(&nodes);
        }

    } break;
    case DOM_slotAssign: {
        int count = argc - 2; node_t **nodes = count ? js_malloc(ctx, (size_t)count * sizeof *nodes) : NULL;
        bool valid = n->type == N_ELEM && !n->foreign && n->tag == T_slot;
        if (count && !nodes) result = JS_EXCEPTION;
        else {
            for (int i = 0; i < count; i++) {
                nodes[i] = node_opaque(argv[i + 2]);
                if (!nodes[i] || (nodes[i]->type != N_ELEM && nodes[i]->type != N_TEXT)) valid = false;
            }
            result = !valid ? JS_ThrowTypeError(ctx, "Slot assignment requires Elements or Text") :
                doc_slot_assign(n, nodes, count) ? JS_UNDEFINED : oom(ctx);
        }
        js_free(ctx, nodes);

    } break;
    case DOM_attrCreate: {
        const char *ns = argc > 2 && !JS_IsNull(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
        const char *prefix = argc > 3 && !JS_IsNull(argv[3]) ? JS_ToCString(ctx, argv[3]) : NULL;
        const char *local = argc > 4 ? JS_ToCString(ctx, argv[4]) : NULL;
        if (n->type != N_DOC) result = JS_ThrowTypeError(ctx, "Document receiver required");
        else if (!local || (argc > 2 && !JS_IsNull(argv[2]) && !ns) || (argc > 3 && !JS_IsNull(argv[3]) && !prefix)) result = JS_EXCEPTION;
        else { node_t *made = doc_attr_create(d, ns, prefix, local, ""); result = made ? wrap(s, made) : oom(ctx); }
        JS_FreeCString(ctx, ns); JS_FreeCString(ctx, prefix); JS_FreeCString(ctx, local);

    } break;
    case DOM_attrList: {
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

    } break;
    case DOM_attrNode: case DOM_attrNodeNS: case DOM_attrNS: {
        bool namespaced = opcode != DOM_attrNode;
        const char *ns = namespaced && argc > 2 && !JS_IsNull(argv[2]) ? JS_ToCString(ctx, argv[2]) : NULL;
        const char *name = argc > (namespaced ? 3 : 2) ? JS_ToCString(ctx, argv[namespaced ? 3 : 2]) : NULL;
        if (n->type != N_ELEM) result = JS_ThrowTypeError(ctx, "Element receiver required");
        else if (!name || (namespaced && argc > 2 && !JS_IsNull(argv[2]) && !ns)) result = JS_EXCEPTION;
        else {
            int i = doc_attr_index(n, ns, name, namespaced);
            if (opcode == DOM_attrNS && argc > 4) {
                if (!ns || !*ns) {
                    attribute_target = n; old_attribute_present = i >= 0;
                    if (i >= 0) attribute_name = n->attrs[i].local ? n->attrs[i].local : n->attrs[i].raw;
                    else { created_attribute_name = strdup(name); attribute_name = created_attribute_name; }
                }
                const char *value = JS_IsNull(argv[4]) ? NULL : JS_ToCString(ctx, argv[4]);
                const char *prefix = argc > 5 && !JS_IsNull(argv[5]) ? JS_ToCString(ctx, argv[5]) : NULL;
                result = (!JS_IsNull(argv[4]) && !value) || (argc > 5 && !JS_IsNull(argv[5]) && !prefix) ? JS_EXCEPTION :
                    (attribute_target && !attribute_name) ? oom(ctx) :
                    (value ? doc_attr_set_ns(d, n, ns, prefix, name, value) : doc_attr_remove(d, n, i)) ? JS_UNDEFINED : oom(ctx);
                JS_FreeCString(ctx, value); JS_FreeCString(ctx, prefix);
            } else if (opcode == DOM_attrNS) result = str_or_null(ctx, i >= 0 ? n->attrs[i].value : NULL);
            else { node_t *a = i >= 0 ? doc_attr_node(d, n, i) : NULL; result = i >= 0 && !a ? oom(ctx) : wrap(s, a); }
        }
        JS_FreeCString(ctx, ns); JS_FreeCString(ctx, name);

    } break;
    case DOM_attrSetNode: case DOM_attrRemoveNode: {
        node_t *a = argc > 2 ? unwrap(ctx, argv[2]) : NULL;
        if (!a || a->type != N_ATTR || n->type != N_ELEM) result = JS_ThrowTypeError(ctx, "Element and Attr required");
        else if (opcode == DOM_attrRemoveNode) {
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

    } break;
    case DOM_documentCommand: {result=document_command_dom(s,n,argc,argv);
    } break;
    case DOM_selection: {result = control_selection_dom(s, n, argc, argv);
    } break;
    case DOM_filesSet: {result = files_dom(s, n, argc, argv);
    } break;
    case DOM_formValue: {result = form_value_dom(s, n, argc, argv);
    } break;
    case DOM_face: {result = face_dom(s, n, argc, argv);
    } break;
    case DOM_faceControls: case DOM_formControls: {result = face_controls_dom(s,n,argc,argv,opcode == DOM_formControls);
    } break;
    case DOM_validation: {result = validation_dom(s, n, argc, argv);
    } break;
    case DOM_observerGeometry: {
        node_t *root=argc>2 && !JS_IsNull(argv[2])?unwrap(ctx,argv[2]):NULL;
        result=argc>2 && !JS_IsNull(argv[2]) && !root?JS_EXCEPTION:observer_geometry(s,n,root);

    } break;
    case DOM_elementScrollIntoView: {
        int32_t block,inline_;int nearest;
        if(!n || n->type!=N_ELEM)result=JS_ThrowTypeError(ctx,"Scrolling requires an Element");
        else if(argc<5)result=JS_ThrowTypeError(ctx,"Scroll alignment is required");
        else if(JS_ToInt32(ctx,&block,argv[2])<0 || JS_ToInt32(ctx,&inline_,argv[3])<0 ||
                (nearest=JS_ToBool(ctx,argv[4]))<0)result=JS_EXCEPTION;
        else if(block<0 || block>3 || inline_<0 || inline_>3)result=JS_ThrowTypeError(ctx,"Invalid scroll alignment");
        else {
            result=JS_NewArray(ctx);
            web_doc *owner=n->owner;struct web_js_state *target=owner?owner->js:NULL;
            if(!JS_IsException(result) && target && !target->disabled && owner->live && connected(target,n)) {
                flush_layout(target);
                int x,y;viewport_scroll_position(target,&x,&y);double vx=x,vy=y;
                pvec changed={0};
                int status=doc_element_scroll_into_view(owner,n,block,inline_,nearest,&vx,&vy,&changed);
                if(status<0){JS_FreeValue(ctx,result);result=status==-1?oom(ctx):
                    JS_ThrowRangeError(ctx,status==-2?"Invalid scroll ancestor cycle":"Scroll result exceeds its native index representation");}
                else if(status>0){
                    viewport_scroll_to(target,vx,vy);
                    for(int i=0;i<changed.n;i++){
                        JSValue node=wrap(s,changed.v[i]);
                        if(JS_IsException(node) || JS_SetPropertyUint32(ctx,result,(uint32_t)i,node)<0){
                            JS_FreeValue(ctx,result);result=JS_EXCEPTION;break;
                        }
                    }
                }
                pv_free(&changed);
            }
        }

    } break;
    case DOM_elementScroll: {
        double x,y;
        result=JS_FALSE;
        if(!n || n->type!=N_ELEM)result=JS_ThrowTypeError(ctx,"Scrolling requires an Element");
        else if(argc<4)result=JS_ThrowTypeError(ctx,"Scrolling requires two coordinates");
        else if(JS_ToFloat64(ctx,&x,argv[2])<0 || JS_ToFloat64(ctx,&y,argv[3])<0)result=JS_EXCEPTION;
        else {
            /* Numeric coercion may adopt the receiver or retire its realm.
               Resolve its current native owner only after both conversions. */
            web_doc *owner=n->owner;
            struct web_js_state *target=owner?owner->js:NULL;
            if(target && !target->disabled && owner->live && connected(target,n)) {
                flush_layout(target);
                if(element_viewport(owner,n))viewport_scroll_to(target,x,y);
                else result=JS_NewBool(ctx,doc_element_scroll(owner,n,x,y));
            }
        }

    } break;
    case DOM_imageDecode: {result = image_decode_promise(s, n);
    } break;
    case DOM_insert: {
        node_t *child = argc > 2 ? unwrap(ctx, argv[2]) : NULL, *before = argc > 3 ? unwrap(ctx, argv[3]) : NULL;
        if (!child || child->type == N_DOC || (n->type != N_ELEM && n->type != N_DOC && n->type != N_FRAGMENT)) result = JS_ThrowTypeError(ctx, "HierarchyRequestError");
        else if (before && before->parent != n) result = JS_ThrowTypeError(ctx, "NotFoundError");
        else {
            bool cycle = false; for (node_t *a = n; a; a = a->parent ? a->parent : a->shadow_host ? a->shadow_host : a->template_host) if (a == child) cycle = true;
            result = cycle ? JS_ThrowTypeError(ctx, "HierarchyRequestError") : doc_node_move(d, n, child, before) ? JS_UNDEFINED : oom(ctx);
        }

    } break;
    case DOM_replace: {
        node_t *child=argc>2?unwrap(ctx,argv[2]):NULL,*old=argc>3?unwrap(ctx,argv[3]):NULL;
        if(!n || !child || !old)result=JS_ThrowTypeError(ctx,"Node arguments are required");
        else if(doc_node_replace_validity(n,child,old))result=JS_ThrowTypeError(ctx,"Invalid native replacement");
        else result=doc_node_replace(d,n,child,old)?JS_UNDEFINED:oom(ctx);

    } break;
    case DOM_remove: {doc_node_remove(d, n);
    } break;
    case DOM_position: {
        node_t *other=argc>2?unwrap(ctx,argv[2]):NULL;
        /* The JS wrapper supplies undefined for an omitted argument. unwrap
         * accepts nullish values for optional-node operations, but position
         * requires a real Node. JS_EXCEPTION without a pending exception would
         * leak QuickJS's uninitialized exception sentinel into a catch binding. */
        if(!other)result=JS_ThrowTypeError(ctx,"compareDocumentPosition requires a Node argument");
        else result=JS_NewUint32(ctx,compare_node_position(n,other));

    } break;
    case DOM_equal: case DOM_same: {
        node_t *other = argc > 2 ? unwrap(ctx, argv[2]) : NULL;
        if (argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2]) && !other) result = JS_EXCEPTION;
        else result = JS_NewBool(ctx, opcode == DOM_equal ? equal_nodes(n, other) : n == other);

    } break;
    case DOM_adopt: case DOM_import: {
        node_t *child = argc > 2 ? unwrap(ctx, argv[2]) : NULL;
        if (n->type != N_DOC || !child || child->type == N_DOC) result = JS_ThrowTypeError(ctx, "Invalid document node transfer");
        else if (opcode == DOM_adopt) result = doc_node_adopt(d, child) ? wrap(s, child) : oom(ctx);
        else { node_t *copy = doc_node_clone(d, child, argc > 3 && JS_ToBool(ctx, argv[3]) > 0); result = copy ? wrap(s, copy) : oom(ctx); }

    } break;
    case DOM_clone: {
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

    } break;
    case DOM_customCandidates: {
        if (argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) p = JS_ToCString(ctx, argv[2]);
        result = argc > 2 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2]) && !p ? JS_EXCEPTION : custom_candidates(s, n, p);

    } break;
    case DOM_rect: {
        struct web_js_state *owner=geometry_owner(s,d);
        int x = 0, y = 0, w = 0, h = 0;
        bool visible = owner && connected(owner, n);
        int sx=0,sy=0;
        if(visible){
            flush_layout(owner);viewport_scroll_position(owner,&sx,&sy);
            d->view_x=sx;d->view_y=sy;
            visible=web_node_rect(d,n,&x,&y,&w,&h);
        }
        if(!visible)x=y=w=h=0;
        double left=visible?(double)x-sx:0,top=visible?(double)y-sy:0;
        result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "x", JS_NewFloat64(ctx, left)); JS_SetPropertyStr(ctx, result, "y", JS_NewFloat64(ctx, top));
        JS_SetPropertyStr(ctx, result, "left", JS_NewFloat64(ctx, left)); JS_SetPropertyStr(ctx, result, "top", JS_NewFloat64(ctx, top));
        JS_SetPropertyStr(ctx, result, "right", JS_NewFloat64(ctx, left + w)); JS_SetPropertyStr(ctx, result, "bottom", JS_NewFloat64(ctx, top + h));
        JS_SetPropertyStr(ctx, result, "width", JS_NewInt32(ctx, w)); JS_SetPropertyStr(ctx, result, "height", JS_NewInt32(ctx, h));

    } break;
    case DOM_submit: {
        if (!d->inert) navigate_form_request(s,n);

    } break;
    case DOM_reset: {
        if (n->type != N_ELEM || n->foreign || n->tag != T_form) result = JS_ThrowTypeError(ctx, "HTMLFormElement receiver required");
        else result = doc_form_reset(d, n) ? JS_UNDEFINED : oom(ctx);

    } break;
    case DOM_get: {result = get_dom(s, n, p);
    } break;
    case DOM_set: {result = argc > 3 ? set_dom(s, n, p, argv[3],argc,argv) : JS_ThrowTypeError(ctx, "Missing DOM value");
    } break;
    case DOM_attr: {
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
                created_attribute_name = strdup(p);
                if (created_attribute_name && !n->foreign) for (char *q = created_attribute_name; *q; q++) *q = (char)lower((unsigned char)*q);
                attribute_target = n; attribute_name = created_attribute_name;
            }
            const char *v = JS_IsNull(argv[3]) ? NULL : JS_ToCString(ctx, argv[3]);
            result = !JS_IsNull(argv[3]) && !v ? JS_EXCEPTION :
                (attribute_target && !attribute_name) ? oom(ctx) : doc_node_attr(d, n, p, v) ? JS_UNDEFINED : oom(ctx);
            JS_FreeCString(ctx, v);
        }

    } break;
    case DOM_id: {result = wrap(s, find_id(n, p));
    } break;
    case DOM_query: {
        pvec found = {0};
        if (!css_select(d, n, p, &found)) result = JS_ThrowSyntaxError(ctx, "Invalid CSS selector");
        else if (argc > 3 && JS_ToBool(ctx, argv[3]) > 0) result = wrap(s, found.n ? found.v[0] : NULL);
        else { result = JS_NewArray(ctx); for (int i = 0; i < found.n; ++i) if (JS_SetPropertyUint32(ctx, result, i, wrap(s, found.v[i])) < 0) { JS_FreeValue(ctx, result); result = JS_EXCEPTION; break; } }
        pv_free(&found);

    } break;
    case DOM_matches: { bool valid; bool matches = css_matches(n, p, &valid); result = valid ? JS_NewBool(ctx, matches) : JS_ThrowSyntaxError(ctx, "Invalid CSS selector");
    } break;
    case DOM_style: case DOM_stylePriority: {result = style_access(s, n, p, argc > 3, argc > 3 ? argv[3] : JS_UNDEFINED, argc > 4 ? argv[4] : JS_UNDEFINED, opcode == DOM_stylePriority);
    } break;
    case DOM_computed: {result = computed(s, n, p);
    } break;
    case DOM_geometry: {result = geometry(s, n, p);
    } break;
    default: result = JS_ThrowTypeError(ctx, "Unknown DOM operation"); break;
    }
dom_complete:;
mutation_after(s,&observed,!JS_IsException(result));
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
    if (!JS_IsException(result) && n && (opcode == DOM_insert || opcode == DOM_replace) && connected(s, n)) {
        dynamic_scripts(s, n);
        for (struct js_script *script = s->scripts; script; script = script->next)
            if (script->dynamic && script->ready && !script->executed && !script->module && !node_attr(script->node, "src")) run_script(s, script);
    }
    if (!JS_IsException(result) && n && n->tag == T_script && !n->script_started && connected(s, n) && (opcode == DOM_attr || opcode == DOM_set)) {
        struct js_script *script = queue_script(s, n, true);
        if (script && script->ready && !script->module && !node_attr(n, "src")) run_script(s, script);
    }
    if (!JS_IsException(result) && attribute_target && attribute_target->tag == T_script && !attribute_target->script_started && connected(s, attribute_target)) {
        struct js_script *script = queue_script(s, attribute_target, true);
        if (script && script->ready && !script->module && !node_attr(attribute_target, "src")) run_script(s, script);
    }
    free(created_attribute_name);
    JS_FreeCString(ctx, p); return result;
}

static JSValue native_dom_magic(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int opcode) {
    struct web_js_state *s = state(ctx);
    JSValueConst local[8], *args=local;
    if(argc>=8){args=js_malloc(ctx,(size_t)(argc+1)*sizeof *args);if(!args)return JS_EXCEPTION;}
    args[0]=JS_UNDEFINED;
    for(int i=0;i<argc;i++)args[i+1]=argv[i];
    bool profiling=s->host.debug_js;
    bool outer=profiling && s->profile_dom_depth++==0;
    uint64_t start=outer?uptime_ms():0;
    if(profiling)s->doc->profile.native_calls++;
    JSValue result=native_dom_impl(ctx,this_val,argc+1,args,opcode);
    if(profiling)s->profile_dom_depth--;
    if(outer)s->doc->profile.native_ms+=uptime_ms()-start;
    if(args!=local)js_free(ctx,args);
    return result;
}
/* Private compatibility bridge for dynamically selected operations only. Hot
 * bootstrap calls use JS_NewCFunctionMagic and never stringify/search an op. */
static JSValue native_dom(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if(argc<1)return JS_ThrowTypeError(ctx,"DOM operation is required");
    const char *name=JS_ToCString(ctx,argv[0]);if(!name)return JS_EXCEPTION;
    int opcode=dom_opcode(name);JS_FreeCString(ctx,name);
    if(opcode<0)return JS_ThrowTypeError(ctx,"Unknown DOM operation");
    return native_dom_magic(ctx,this_val,argc-1,argv+1,opcode);
}

static enum http_url_result make_origin(const char *url, char **out) {
    return http_origin_owned(url, out);
}
/* No prefix comparison or empty-string fallback on parse/allocation failure.
   Both results are transient native owners, including the failed second parse. */
static bool same_origin_urls(const char *a, const char *b, bool *allocation_failed) {
    char *x = NULL, *y = NULL;
    enum http_url_result xr = make_origin(a, &x), yr = make_origin(b, &y);
    *allocation_failed = xr == HTTP_URL_OOM || yr == HTTP_URL_OOM;
    bool same = xr == HTTP_URL_TUPLE && yr == HTTP_URL_TUPLE && !strcmp(x, y);
    free(x); free(y); return same;
}
static bool permitted_url(struct web_js_state *s, const char *url, bool fetch) {
    if (!strncasecmp(url, "https://", 8) || !strncasecmp(url, "http://", 7)) {
        struct url_owned parsed = {0};
        bool ok = url_parse_owned(url, &parsed) && !strchr(parsed.host, '@');
        url_owned_free(&parsed); return ok;
    }
    if (fetch || strncasecmp(url, "file://", 7) || strncasecmp(s->doc->url, "file://", 7)) return false;
    /* Resolution already removes dot segments. Encoded path separators and dot
       segments must not create a second, more privileged interpretation. */
    const char *u = url + 7, *base = s->doc->url + 7;
    if (*u != '/' || strchr(u, '%') || strchr(u, '\\')) return false;
    const char *slash = strrchr(base, '/');
    return slash && !strncmp(u, base, (size_t)(slash + 1 - base));
}
/* Opaque executable URLs stay in the script/module boundary. Blob contents
 * must also pass the document-private, revocable object-URL registry below;
 * this does not grant a native fetch, navigation or local-file allowance. */
static bool permitted_script_url(struct web_js_state *s,const char *url) {
    size_t length=strlen(url);
    if (!strncasecmp(url,"data:",5)) return length<=JS_DATA_URL_LIMIT;
    if (!strncasecmp(url,"blob:",5)) return length<=UINT32_MAX;
    return length<=UINT32_MAX && permitted_url(s,url,false);
}
static char *script_data_source(struct web_js_state *s,const char *url,bool module,size_t *length,char mime[128]);
static char *script_blob_source(struct web_js_state *s,const char *url,bool module,size_t *length,char mime[128]);
/* Console text is author data, not a diagnostic excerpt. Grow on demand and
 * report real allocation failure instead of truncating or aborting the task. */
static bool console_text_append(sbuf *text, const char *bytes, size_t length) {
    if (text->n == SIZE_MAX || length > SIZE_MAX - text->n - 1) return false;
    size_t needed = text->n + length + 1;
    if (needed > text->cap) {
        size_t capacity = text->cap ? text->cap : 64;
        while (capacity < needed) capacity = capacity > SIZE_MAX / 2 ? needed : capacity * 2;
        char *grown = realloc(text->p, capacity);
        if (!grown) return false;
        text->p = grown; text->cap = capacity;
    }
    if (length) memcpy(text->p + text->n, bytes, length);
    text->n += length; text->p[text->n] = 0;
    return true;
}
static bool console_text_render(sbuf *text, const char *bytes, size_t length) {
    /* The native host uses C strings. Show embedded NULs without losing the
     * remainder of a JavaScript string at that boundary. */
    while (length) {
        const char *zero = memchr(bytes, 0, length);
        size_t part = zero ? (size_t)(zero - bytes) : length;
        if (!console_text_append(text, bytes, part)) return false;
        if (!zero) return true;
        if (!console_text_append(text, "\\0", 2)) return false;
        bytes += part + 1; length -= part + 1;
    }
    return true;
}
static JSValue native_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); int32_t level = 0;
    if (argc && JS_ToInt32(ctx, &level, argv[0]) < 0) return JS_EXCEPTION;
    sbuf text = {0}; JSValue first_stack=JS_UNDEFINED;
    for (int i = 1; i < argc; i++) {
        size_t length = 0;
        const char *p = JS_ToCStringLen(ctx, &length, argv[i]);
        if (!p) { JS_FreeValue(ctx,first_stack); sb_free(&text); return JS_EXCEPTION; }
        bool ok = (i <= 1 || console_text_append(&text, " ", 1)) &&
                  console_text_render(&text, p, length);
        JS_FreeCString(ctx, p);
        if (!ok) goto allocation_failed;
        if (JS_IsError(ctx, argv[i])) {
            JSValue stack = diagnostic_stack(ctx, argv[i]);
            if (JS_IsString(stack)) {
                if(JS_IsUndefined(first_stack))first_stack=JS_DupValue(ctx,stack);
                const char *where = JS_ToCStringLen(ctx, &length, stack);
                if (where) {
                    ok = console_text_append(&text, "\n", 1) &&
                         console_text_render(&text, where, length);
                    JS_FreeCString(ctx, where);
                }
                else { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); }
            }
            JS_FreeValue(ctx, stack);
            if (!ok) goto allocation_failed;
        }
    }
    log_text(s, level, text.p ? text.p : ""); sb_free(&text);
    if(level>=2 && JS_IsString(first_stack)) {
        diagnostic_context(s); const char *where=JS_ToCString(ctx,first_stack);
        if(where){diagnostic_excerpt(s,where);JS_FreeCString(ctx,where);}
        else if(JS_HasException(ctx)){JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}
    }
    JS_FreeValue(ctx,first_stack); return JS_UNDEFINED;
allocation_failed:
    JS_FreeValue(ctx,first_stack); sb_free(&text);
    return JS_ThrowOutOfMemory(ctx);
}
static uint64_t relative_time(struct web_js_state *s, uint64_t absolute) {
    return absolute >= s->now ? absolute - s->now : 0;
}
static void timing_record(struct web_js_state *s, unsigned milestone, uint64_t observed) {
    uint32_t bit = 1u << milestone;
    if (!(s->timing_valid & bit)) { s->timing[milestone] = observed; s->timing_valid |= bit; }
}
static JSValue native_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, (double)relative_time(state(ctx), uptime_ms()));
}
static JSValue native_window_state(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    return JS_NewUint32(ctx,s->host.window_state?s->host.window_state(s->host.opaque):0);
}
/* Private: null means unobserved. A genuine recorded relative 0 is usable by
   User Timing; the public legacy interface maps null, and only null, to 0. */
static JSValue native_timing(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    static const char *const names[JS_TIMING_COUNT] = {
        "navigationStart", "unloadEventStart", "unloadEventEnd", "redirectStart", "redirectEnd",
        "fetchStart", "domainLookupStart", "domainLookupEnd", "connectStart", "connectEnd",
        "secureConnectionStart", "requestStart", "responseStart", "responseEnd",
        "domLoading", "domInteractive", "domContentLoadedEventStart", "domContentLoadedEventEnd",
        "domComplete", "loadEventStart", "loadEventEnd"
    };
    if (!argc) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]); if (!name) return JS_EXCEPTION;
    struct web_js_state *s = state(ctx); JSValue value = JS_NULL;
    for (unsigned i = 0; i < JS_TIMING_COUNT; i++) if (!strcmp(name, names[i])) {
        if (s->timing_valid & (1u << i)) value = JS_NewFloat64(ctx, (double)relative_time(s, s->timing[i]));
        break;
    }
    JS_FreeCString(ctx, name); return value;
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
    char *out = NULL;
    enum http_url_result status = make_origin(web_effective_url(state(ctx)->doc), &out);
    if (status == HTTP_URL_OOM) return JS_ThrowOutOfMemory(ctx);
    JSValue result = JS_NewString(ctx, status == HTTP_URL_TUPLE ? out : "null");
    free(out); return result;
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
    JSValue out = JS_NewArrayBufferCopy(ctx, data, size); js_free(ctx, data); return out;
}
static JSValue native_unpack(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t size = 0; uint8_t *data = argc ? JS_GetArrayBuffer(ctx, &size, argv[0]) : NULL;
    if (!data) return JS_EXCEPTION;
    return JS_ReadObject(ctx, data, size, JS_READ_OBJ_REFERENCE);
}
struct js_history_receiver { struct web_js_state *owner; };
static JSValue history_security_error(JSContext *ctx,const char *message) {
    struct web_js_state *s=state(ctx);
    JSValue fn=JS_GetPropertyStr(ctx,s->hooks,"historySecurityError"),arg=JS_NewString(ctx,message);
    if(JS_IsException(fn) || JS_IsException(arg)){JS_FreeValue(ctx,fn);JS_FreeValue(ctx,arg);return JS_EXCEPTION;}
    if(!JS_IsFunction(ctx,fn)){JS_FreeValue(ctx,fn);JS_FreeValue(ctx,arg);return JS_ThrowTypeError(ctx,"History security exception factory is unavailable");}
    JSValue error=JS_Call(ctx,fn,s->hooks,1,&arg);
    JS_FreeValue(ctx,fn);JS_FreeValue(ctx,arg);
    return JS_IsException(error)?error:JS_Throw(ctx,error);
}
static void history_forget(struct web_js_state *s) {
    if(s->history_receiver){s->history_receiver->owner=NULL;s->history_receiver=NULL;}
}
static void history_finalizer(JSRuntime *rt,JSValue object) {
    struct js_history_receiver *receiver=JS_GetOpaque(object,history_class);
    if(!receiver)return;
    if(receiver->owner && receiver->owner->history_receiver==receiver)receiver->owner->history_receiver=NULL;
    js_free_rt(rt,receiver);
}
static JSValue native_history_create(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    if(!s || s->disabled || s->history_receiver || argc<1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,"History initialization requires its private interface prototype");
    if(!history_class)JS_NewClassID(&history_class);
    if(!JS_IsRegisteredClass(s->rt,history_class)){
        JSClassDef definition={.class_name="History",.finalizer=history_finalizer};
        if(JS_NewClass(s->rt,history_class,&definition)<0)return oom(ctx);
    }
    struct js_history_receiver *receiver=js_mallocz(ctx,sizeof *receiver);
    if(!receiver)return JS_EXCEPTION;
    JSValue object=JS_NewObjectProtoClass(ctx,argv[0],history_class);
    if(JS_IsException(object)){js_free(ctx,receiver);return object;}
    receiver->owner=s;s->history_receiver=receiver;JS_SetOpaque(object,receiver);
    return object;
}
static JSValue native_history_call(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    struct web_js_state *source=state(ctx);
    struct js_history_receiver *receiver=argc?JS_GetOpaque2(ctx,argv[0],history_class):NULL;
    if(!argc)return JS_ThrowTypeError(ctx,"History receiver required");
    if(!receiver)return JS_EXCEPTION;
    struct web_js_state *target=receiver->owner;
    struct web_js_state *caller=sandbox_actor(source);
    if(!target || target->disabled || !target->doc->live || caller->disabled || !caller->doc->live)
        return history_security_error(ctx,"History belongs to an inactive Document");
    if(!web_frame_same_origin(caller->doc,target->doc))
        return history_security_error(ctx,"Cross-origin History access is forbidden");
    if(caller->doc->sandbox_flags && caller->doc!=target->doc)return history_security_error(ctx,"Sandbox denied borrowed History authority");
    int array=argc>2?JS_IsArray(ctx,argv[2]):0;
    if(array<0)return JS_EXCEPTION;
    if(argc<3 || !JS_IsString(argv[1]) || array!=1)
        return JS_ThrowTypeError(ctx,"History dispatch requires a private operation and argument list");
    /* The function's realm can differ from the History receiver's realm (e.g.
       iframe.history.replaceState.bind(parent.history)). Run private owner
       hooks, never author-replaced methods, URLs, or singleton identity tests. */
    begin_task(target);
    JSValue arguments[]={argv[1],argv[2]};
    JSValue result=custom_element_hook(target,"historyOperation",2,arguments);
    end_task(target);
    return result;
}
static JSValue native_history(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if(s->disabled || !s->doc->live || sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"History belongs to an inactive or sandbox-inaccessible Document");
    int32_t op = 0, value = 0;
    if ((argc && JS_ToInt32(ctx, &op, argv[0])) || (argc > 3 && JS_ToInt32(ctx, &value, argv[3]))) return JS_EXCEPTION;
    if(s->doc->frame_parent){
        if(op!=WEB_HISTORY_INFO)return JS_ThrowTypeError(ctx,"Child session-history mutations are not implemented");
        JSValue out=JS_NewObject(ctx);JS_SetPropertyStr(ctx,out,"entry",JS_NewInt32(ctx,1));
        JS_SetPropertyStr(ctx,out,"length",JS_NewInt32(ctx,1));JS_SetPropertyStr(ctx,out,"manual",JS_FALSE);
        JS_SetPropertyStr(ctx,out,"data",JS_NULL);return out;
    }
    if (!s->host.history) return JS_ThrowTypeError(ctx, "Embedder has no session history");
    const char *url = argc > 1 && !JS_IsNull(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    if (argc > 1 && !JS_IsNull(argv[1]) && !url) return JS_EXCEPTION;
    size_t len = 0; uint8_t *data = NULL;
    if (argc > 2 && !JS_IsNull(argv[2])) {
        data = JS_GetArrayBuffer(ctx, &len, argv[2]);
        if (!data) { JS_FreeCString(ctx, url); return JS_EXCEPTION; }
    }
    struct web_history result = {0};
    bool ok = s->host.history(s->host.opaque, op, url, data, len, value, &result);
    JS_FreeCString(ctx, url);
    if (!ok) return JS_ThrowTypeError(ctx, "Session history operation failed");
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "entry", JS_NewInt64(ctx, result.entry));
    JS_SetPropertyStr(ctx, out, "length", JS_NewInt32(ctx, result.length));
    JS_SetPropertyStr(ctx, out, "manual", JS_NewBool(ctx, result.manual_scroll));
    JS_SetPropertyStr(ctx, out, "data", op == WEB_HISTORY_INFO && value && result.state_len ? JS_NewArrayBufferCopy(ctx, result.state, result.state_len) : JS_NULL);
    return out;
}
static JSValue native_sandbox_flags(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    struct web_js_state *s=state(ctx);node_t *node=argc?unwrap(ctx,argv[0]):NULL;
    if(argc && !node)return JS_EXCEPTION;
    return JS_NewUint32(ctx,sandbox_authority(s)|(node && node->owner?node->owner->sandbox_flags:0));
}
static JSValue native_canvas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "Canvas node and operation required");
    node_t *node = unwrap(ctx, argv[0]);
    if (!node) return JS_ThrowTypeError(ctx, "Native Canvas receiver required");
    const char *op = JS_ToCString(ctx, argv[1]);
    if (!op) return JS_EXCEPTION;
    if(!strcmp(op,"contextProbe")) {
        JS_FreeCString(ctx,op);
        struct web_js_state *s=node->owner && node->owner->js?node->owner->js:state(ctx);
        if(s->host.debug_js && argc>2 && JS_IsString(argv[2])) {
            const char *type=JS_ToCString(ctx,argv[2]);if(!type)return JS_EXCEPTION;
            static const char *known[]={"2d","webgl","webgl2","experimental-webgl","bitmaprenderer","webgpu"};
            for(unsigned i=0;i<sizeof known/sizeof *known;i++)if(!strcmp(type,known[i])) {
                if(!(s->canvas_probe_seen&(1u<<i))) {
                    s->canvas_probe_seen|=1u<<i;
                    char *origin=NULL;enum http_url_result parsed=http_origin_owned(web_effective_url(s->doc),&origin);
                    char message[320];snprintf(message,sizeof message,
                        "Canvas context request: type=%s frame=%d connected=%d origin=%.160s",
                        known[i],s->doc->frame_parent!=NULL,connected(s,node),parsed==HTTP_URL_TUPLE?origin:"null");
                    log_text(s,0,message);free(origin);
                }
                break;
            }
            JS_FreeCString(ctx,type);
        }
        return JS_UNDEFINED;
    }
    bool draw_image = !strcmp(op, "drawImage");
    JS_FreeCString(ctx, op);
    if (draw_image) {
        if (argc < 3) return JS_ThrowTypeError(ctx, "Canvas image source required");
        if(web_image_bitmap_is(argv[2]))return web_canvas_draw_bitmap(ctx,node,argv[2],argc-3,argv+3);
        node_t *source = unwrap(ctx, argv[2]);
        if (!source) return JS_ThrowTypeError(ctx, "Native Canvas image source required");
        return web_canvas_draw_image(ctx, node, source, argc - 3, argv + 3);
    }
    return web_canvas_native(ctx, node, argc - 1, argv + 1);
}

static JSValue native_image_bitmap(JSContext *ctx,JSValueConst this_val,int argc,JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    if(!task_context_active(s) || sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))
        return history_security_error(ctx,"ImageBitmap authority is inactive or sandbox-inaccessible");
    node_t *source=NULL;int kind=-1;
    if(argc>1 && JS_IsString(argv[0]) && JS_IsNumber(argv[1])) {
        if(JS_ToInt32(ctx,&kind,argv[1])<0)return JS_EXCEPTION;
        if(kind==2) {
            if(argc<3 || !(source=unwrap(ctx,argv[2])))return JS_ThrowTypeError(ctx,"Native ImageBitmap image source required");
            if(!source->owner || !source->owner->live ||
               (source->owner->js && !task_context_active(source->owner->js)))
                return history_security_error(ctx,"ImageBitmap source belongs to a retired document");
        }
    }
    JSValue result=web_image_bitmap_native(ctx,s->doc,source,argc,argv);
    /* Actual decoder/snapshot success only; no URL, author text or encoded
       bytes. Debug-off takes no extra profiling/native bridge round trip. */
    if(s->host.debug_js && !JS_IsException(result) && web_image_bitmap_is(result)) {
        unsigned index=kind==0?0:kind==1?1:kind==2?(source&&source->tag==T_img?3:2):4;
        if(!(s->image_bitmap_seen&(1u<<index))) {
            unsigned w,h;
            if(web_image_bitmap_dimensions(result,&w,&h)) {
                static const char *const names[]={"Blob","ImageData","HTMLCanvasElement","HTMLImageElement","ImageBitmap"};
                char message[128];snprintf(message,sizeof message,"ImageBitmap decoded: source=%s width=%u height=%u",names[index],w,h);
                s->image_bitmap_seen|=1u<<index;log_text(s,0,message);
            }
        }
    }
    return result;
}

static JSValue native_avmedia_impl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if(state(ctx)->disabled)return JS_ThrowTypeError(ctx,"Media belongs to an inactive browsing context");
    if (argc < 2) return JS_ThrowTypeError(ctx, "Media operation and receiver required");
    const char *op = JS_ToCString(ctx, argv[0]);
    if (!op) return JS_EXCEPTION;
    bool capability = !strcmp(op, "type") || !strcmp(op, "mseType");
    node_t *node = capability ? NULL : unwrap(ctx, argv[1]);
    if (!node && !capability) {
        JS_FreeCString(ctx, op);
        return JS_ThrowTypeError(ctx, "Native media receiver required");
    }
    /* Direct native media IO is not appropriate for arbitrary embedders.
       Both capability discovery and loading must obey the same host opt-in;
       the media implementation separately validates owner/origin/CORS. */
    if ((!strcmp(op, "range") || !strcmp(op, "loadURL")) && !state(ctx)->host.media_range) {
        JS_FreeCString(ctx, op);
        return JS_FALSE;
    }
    JSValue result = web_avmedia_call(ctx, state(ctx)->doc, node, op, argc - 2, argv + 2);
    JS_FreeCString(ctx, op);
    return result;
}

static JSValue native_avmedia(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    /* Even operation/receiver coercion can execute author getters. Do not let
       the interrupt hook enter native media while that call is incomplete. */
    web_avmedia_enter();
    JSValue result=native_avmedia_impl(ctx,this_val,argc,argv);
    web_avmedia_leave();
    return result;
}

static JSValue native_media(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_ThrowTypeError(ctx, "Media query is required");
    size_t len; const char *query = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!query) return JS_EXCEPTION;
    if (memchr(query, 0, len)) { JS_FreeCString(ctx, query); return JS_ThrowTypeError(ctx, "Media query contains an embedded NUL"); }
    struct web_js_state *s = state(ctx); bool serialize = argc > 1 && JS_ToBool(ctx, argv[1]);
    if (serialize && len > (SIZE_MAX - 16) / 9) { JS_FreeCString(ctx, query); return JS_ThrowRangeError(ctx, "Media query serialization length is not representable"); }
    size_t cap = serialize ? len * 9 + 16 : 0; char *text = serialize ? js_malloc(ctx, cap) : NULL;
    if (serialize && !text) { JS_FreeCString(ctx, query); return JS_EXCEPTION; }
    bool matches = css_media_evaluate(query, s->doc->width, s->doc->height, web_js_enabled(s->doc), text, cap);
    JS_FreeCString(ctx, query);
    if (!serialize) return JS_NewBool(ctx, matches);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "matches", JS_NewBool(ctx, matches));
    JS_SetPropertyStr(ctx, result, "media", JS_NewString(ctx, text)); js_free(ctx, text); return result;
}
static JSValue native_pointer_capture_pending(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    int pending=argc?JS_ToBool(ctx,argv[0]):0;
    if(pending<0)return JS_EXCEPTION;
    s->pointer_capture_pending=pending!=0;
    return JS_UNDEFINED;
}
static JSValue native_observers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    s->observers_active=argc && JS_ToBool(ctx,argv[0])>0;
    s->observers_force=s->observers_active;
    if(!s->observer_due)s->observer_due=uptime_ms();
    return JS_UNDEFINED;
}
static JSValue native_cookie_enabled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    return JS_NewBool(ctx,s->host.cookie_get && s->host.cookie_set &&
        s->host.cookie_enabled && s->host.cookie_enabled(s->host.opaque));
}
static JSValue native_cookie_impl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if(!task_context_active(s) || sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Sandbox or inactive cookie authority");
    if (argc) {
        const char *v = JS_ToCString(ctx, argv[0]); if (!v) return JS_EXCEPTION;
        if (s->host.cookie_set) s->host.cookie_set(s->host.opaque, web_effective_url(s->doc), v);
        JS_FreeCString(ctx, v); return JS_UNDEFINED;
    }
    char *value = s->host.cookie_get ? s->host.cookie_get(s->host.opaque, web_effective_url(s->doc)) : NULL;
    JSValue out = JS_NewString(ctx, value ? value : ""); free(value); return out;
}
static JSValue native_cookie(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if(!state(ctx)->host.debug_js)return native_cookie_impl(ctx,this_val,argc,argv);
    struct js_storage_profile *p=&profile_task(state(ctx))->storage_profile;
    uint64_t start=uptime_ms();p->cookie_calls++;
    JSValue result=native_cookie_impl(ctx,this_val,argc,argv);
    p->cookie_ms+=uptime_ms()-start;return result;
}
/* Returns [status, result]. DOMException creation belongs to the private JS
 * binding, not a replaceable public constructor. Never accepts an origin/path
 * from page code; even after argument conversion the current native URL wins. */
static JSValue native_storage_impl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s=state(ctx);
    if(!task_context_active(s) || sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Sandbox or inactive storage authority");
    int32_t kind=0, op=0; uint32_t index=0;
    if (argc<2) return JS_ThrowTypeError(ctx,"Storage kind/operation required");
    if (JS_ToInt32(ctx,&kind,argv[0])<0 || JS_ToInt32(ctx,&op,argv[1])<0) return JS_EXCEPTION;
    struct js_storage_profile *profile=&profile_task(s)->storage_profile;
    if(s->host.debug_js&&kind>=0 && kind<2 && op>=0 && op<7)profile->calls[kind][op]++;
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
        if(s->host.debug_js)profile->set_bytes+=request.value_len;
    }
    JSValue fn=JS_GetPropertyStr(ctx,s->hooks,"storageOrigin"), result=JS_EXCEPTION;
    JSValue origin_value=JS_UNDEFINED, argument=JS_UNDEFINED;
    const char *origin=NULL;
    struct web_storage_result out={0}; int status=WEB_STORAGE_SECURITY;
    if (JS_IsException(fn)) goto done;
    if (!JS_IsFunction(ctx,fn)) { JS_ThrowInternalError(ctx,"Missing private storage origin parser"); goto done; }
    argument=JS_NewString(ctx,web_effective_url(s->doc));
    if (JS_IsException(argument)) goto done;
    origin_value=JS_Call(ctx,fn,s->hooks,1,&argument);
    if (JS_IsException(origin_value)) goto done;
    if (!JS_IsNull(origin_value)) {
        size_t len=0; origin=JS_ToCStringLen(ctx,&len,origin_value);
        if (!origin) goto done;
        if (len && len<2048 && len==strlen(origin) && s->host.storage) {
            uint64_t start=s->host.debug_js?uptime_ms():0;if(s->host.debug_js)profile->backend_calls++;
            status=s->host.storage(s->host.opaque,origin,&request,&out);
            if(s->host.debug_js){profile->backend_ms+=uptime_ms()-start;
                profile->saves+=out.save_attempts;profile->save_ms+=out.save_ms;
                profile->snapshot_bytes+=out.snapshot_bytes;}
        }
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
static JSValue native_storage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if(!state(ctx)->host.debug_js)return native_storage_impl(ctx,this_val,argc,argv);
    struct js_storage_profile *p=&profile_task(state(ctx))->storage_profile;
    bool outer=p->depth++==0;uint64_t start=outer?uptime_ms():0;
    JSValue result=native_storage_impl(ctx,this_val,argc,argv);
    p->depth--;if(outer)p->inclusive_ms+=uptime_ms()-start;
    return result;
}
static JSValue native_scroll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); int x = 0, y = 0;
    if(sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Sandbox denied borrowed viewport authority");
    if (argc > 1) {
        double a,b;
        if(JS_ToFloat64(ctx,&a,argv[0])<0 || JS_ToFloat64(ctx,&b,argv[1])<0)return JS_EXCEPTION;
        if(s->disabled || !s->doc->live)return JS_UNDEFINED;
        flush_layout(s);
        viewport_scroll_to(s,a,b);
        return JS_UNDEFINED;
    }
    viewport_scroll_position(s,&x,&y);
    int32_t axis = 0; if (argc && JS_ToInt32(ctx, &axis, argv[0])) return JS_EXCEPTION;
    return JS_NewInt32(ctx, axis ? y : x);
}
/* Native URL ownership follows actual length; the native wire uses uint32_t.
   Resolver OOM must remain OOM rather than being rewritten as a policy denial. */
static char *js_resolve_owned(JSContext *ctx,const char *base,const char *relative) {
    char *owned=NULL; int status=web_resolve_url_owned(base,relative,&owned);
    if(status!=1){if(status<0)JS_ThrowOutOfMemory(ctx);else JS_ThrowTypeError(ctx,"Invalid URL");return NULL;}
    char *resolved=js_strdup(ctx,owned);free(owned);return resolved;
}
static JSValue native_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_UNDEFINED;
    const char *rel = JS_ToCString(ctx, argv[0]); if (!rel) return JS_EXCEPTION;
    struct web_js_state *s = state(ctx); if (s->doc->resources_dirty) doc_sync_tree(s->doc);
    char *url = js_resolve_owned(ctx, s->doc->base, rel);
    JS_FreeCString(ctx, rel);
    if (!url) return JS_EXCEPTION;
    JSValue result = JS_NewString(ctx, url);
    js_free(ctx, url); return result;
}
static JSValue native_ready(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    return JS_NewString(ctx, s->load_sent ? "complete" : s->parsing_done ? "interactive" : "loading");
}
static JSValue native_current(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { return wrap(state(ctx), state(ctx)->current_script); }
static JSValue native_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if ((!s->parser_write && !s->document_open) || !s->doc->parser || s->parsing_done) return JS_ThrowTypeError(ctx, "document.write requires an open document stream or parser-blocking classic script");
    if (!argc) return JS_UNDEFINED;
    size_t n; const char *text = JS_ToCStringLen(ctx, &n, argv[0]); if (!text) return JS_EXCEPTION;
    bool ok = html_write(s->doc->parser, text, n); JS_FreeCString(ctx, text);
    if(ok)s->document_waiting=false;
    if(ok && s->document_open)ok=frame_stream_pump(s);
    return ok ? JS_UNDEFINED : oom(ctx);
}
static JSValue native_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_NewArrayBufferCopy(ctx, NULL, 0);
    size_t n; const char *text = JS_ToCStringLen(ctx, &n, argv[0]); if (!text) return JS_EXCEPTION;
    JSValue v = JS_NewArrayBufferCopy(ctx, (const uint8_t *)text, n); JS_FreeCString(ctx, text); return v;
}
static JSValue native_inline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if(sandbox_authority(state(ctx))&SB_SCRIPTS)return history_security_error(ctx,"Sandbox disallows scripts");
    if (!argc) return JS_UNDEFINED;
    size_t n; const char *text = JS_ToCStringLen(ctx, &n, argv[0]); if (!text) return JS_EXCEPTION;
    sbuf b = {0}; sb_puts(&b, "(function(event){\n"); sb_put(&b, text, n); sb_puts(&b, "\n})");
    JSValue result = JS_Eval(ctx, sb_cstr(&b), b.n, state(ctx)->doc->url, JS_EVAL_TYPE_GLOBAL);
    sb_free(&b); JS_FreeCString(ctx, text); return result;
}
static JSValue native_navigate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!argc) return JS_UNDEFINED;
    struct web_js_state *s = state(ctx);
    if(!task_context_active(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Inactive or script-disabled navigation context");
    struct web_js_state *actor=sandbox_actor(s);
    bool activated=actor->doc->sandbox_activation_until>uptime_ms();
    if(!web_sandbox_navigation_allowed(actor->doc,s->doc,activated))return history_security_error(ctx,"Sandbox denied destination navigation");
    const char *p = JS_ToCString(ctx, argv[0]); if (!p) return JS_EXCEPTION;
    if(!task_context_active(s) || !task_context_active(actor)){JS_FreeCString(ctx,p);return history_security_error(ctx,"Navigation context retired during conversion");}
    if (s->doc->resources_dirty) doc_sync_tree(s->doc);
    char *url = js_resolve_owned(ctx, s->doc->base, p);
    if (!url) { JS_FreeCString(ctx, p); return JS_EXCEPTION; }
    bool ok = permitted_url(s, url, false);
    JS_FreeCString(ctx, p);
    if (!ok) { js_free(ctx, url); return JS_ThrowTypeError(ctx, "Navigation URL is not permitted"); }
    int32_t mode = 0;
    if (argc > 1 && JS_ToInt32(ctx, &mode, argv[1])) { js_free(ctx, url); return JS_EXCEPTION; }
    if(!task_context_active(s) || !task_context_active(actor) || !web_sandbox_navigation_allowed(actor->doc,s->doc,actor->doc->sandbox_activation_until>uptime_ms())){js_free(ctx,url);return history_security_error(ctx,"Navigation authority changed during conversion");}
    if(actor->doc!=s->doc)actor->doc->sandbox_activation_until=0;
    if (s->doc->frame_parent) {
        struct web_frame *f=web_frame_find(s->doc->frame_parent,s->doc->frame_element);
        if (!web_frame_set_navigation(f,url,actor->doc)) { js_free(ctx,url); return oom(ctx); }
    }
    else if (s->host.navigate_mode) s->host.navigate_mode(s->host.opaque, url, mode);
    else if (s->host.navigate) s->host.navigate(s->host.opaque, url, NULL);
    js_free(ctx, url); return JS_UNDEFINED;
}
/* Capture the native activation path before author listeners can reparent
   the clicked descendant. The event target itself remains the clicked node. */
static node_t *native_click_anchor(web_doc *d, node_t *target) {
    return web_link_activation_anchor(d, target);
}
static JSValue native_click(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue native_click_impl(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); node_t *n = argc ? unwrap(ctx, argv[0]) : NULL;
    if (!n) return JS_EXCEPTION;
    if((sandbox_authority(s)&SB_SCRIPTS) || (sandbox_borrowed(s) && n->owner!=sandbox_actor(s)->doc))
        return history_security_error(ctx,"Sandbox denied borrowed activation authority");
    if (n->owner && n->owner->inert) {
        JSValue args[] = {argv[0], JS_NewString(ctx, "click")};
        JSValue value = custom_element_hook(s, "inertClick", 2, args);
        JS_FreeValue(ctx, args[1]); return value;
    }
    if (web_control_disabled(n)) return JS_UNDEFINED;
    node_t *anchor = native_click_anchor(s->doc, n);
    struct web_event e = {.type="click", .bubbles=true, .cancelable=true, .synthetic=true};
    struct web_control_activation activation;
    web_control_activation_begin(s->doc, n, &activation);
    bool allowed = web_js_dispatch(s->doc, n, &e);
    bool input_events = web_control_activation_end(s->doc, &activation, allowed);
    if (!allowed) return JS_UNDEFINED;
    e.synthetic = false; /* default-action input/change/submit events are UA generated */
    if (s->doc->resources_dirty) doc_rescan(s->doc);
    node_t *label_control = anchor ? NULL : web_label_activation(n);
    if (label_control && !web_control_disabled(label_control)) {
        web_js_focus_control(s->doc, label_control);
        JSValue argument = wrap(s, label_control);
        JSValue result = JS_IsException(argument) ? argument : native_click(ctx, this_val, 1, &argument);
        JS_FreeValue(ctx, argument); return result;
    }
    node_t *disclosure=anchor ? NULL : doc_details_activation(n);
    if (disclosure) doc_details_toggle(s->doc,disclosure);
    else if (!anchor && (n->tag == T_audio || n->tag == T_video)) web_media_activate(s->doc, n);
    else if (anchor) {
        if(node_attr(anchor,"download") && (sandbox_authority(s)&SB_DOWNLOADS))return JS_UNDEFINED;
        struct web_hit action = {0};
        if (web_link_action(s->doc, anchor, &action) && permitted_url(s, action.href, false)) {
            if(s->doc->frame_parent) {
                const char *target=node_attr(anchor,"target");
                struct web_frame *f=web_frame_find(s->doc->frame_parent,s->doc->frame_element);
                if(target && *target && strcasecmp(target,"_self"))log_text(s,1,"Scripted child link activation cannot navigate another browsing context");
                else if(!web_frame_set_navigation(f,action.href,s->doc))return oom(ctx);
            }else if(s->host.navigate)s->host.navigate(s->host.opaque, action.href, NULL);
        }
    } else if (n->tag == T_input) {
        const char *type = node_attr(n, "type");
        if (type && (str_ieq(type, "checkbox") || str_ieq(type, "radio"))) {
            if (input_events) { e.type = "input"; e.cancelable = false;  web_js_dispatch(s->doc, n, &e); e.type = "change";  web_js_dispatch(s->doc, n, &e); } s->doc->dirty = true;
        } else if (type && (str_ieq(type, "submit") || str_ieq(type, "image"))) {
            node_t *form = web_form_owner(s->doc, n); e.type = "submit";
            e.submitter=n;
            if (form && web_sandbox_forms_allowed(s->doc) && web_form_submission_validate(s->doc,n) && web_js_dispatch(s->doc, form, &e)) {
                navigate_form_request(s,n);
            }
        } else if (type && str_ieq(type,"reset")) web_reset(s->doc,n);
    } else if (n->tag == T_button) {
        const char *type = node_attr(n, "type"); node_t *form = web_form_owner(s->doc, n);
        if (type && str_ieq(type,"reset")) web_reset(s->doc,n);
        else if (form && web_control_submit_button(n) && web_sandbox_forms_allowed(s->doc)) { e.type = "submit"; e.submitter=n; if (web_form_submission_validate(s->doc,n) && web_js_dispatch(s->doc, form, &e)) navigate_form_request(s,n); }
    }
    return JS_UNDEFINED;
}
static JSValue native_click(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    node_t *n = argc ? unwrap(ctx, argv[0]) : NULL;
    if (!n) return JS_EXCEPTION;
    if (n->click_in_progress) return JS_UNDEFINED;
    n->click_in_progress = true;
    JSValue result = native_click_impl(ctx, this_val, argc, argv);
    n->click_in_progress = false;
    return result;
}
static void release_timer(struct web_js_state *s, unsigned index) {
    /* Unpublish before releasing references. Never retain a slot pointer across
       anything that can execute script or move the dynamically sized table. */
    struct js_timer old = s->timers[index];
    memset(&s->timers[index], 0, sizeof s->timers[index]);
    JS_FreeValue(s->ctx, old.fn); JS_FreeValue(s->ctx, old.args);
}
static bool task_context_active(const struct web_js_state *s) {
    return s && s->ctx && s->doc && s->doc->js == s && s->doc->live && !s->disabled;
}
static void release_document_tasks(struct web_js_state *s) {
    if (!s || !s->ctx) return; /* Failed Context creation owns no task values. */
    /* Dispatched callbacks own local references, not queue/table pointers.
       Unpublish every pending root before freeing any JS value: repeated
       retirement/native finalizer reentry cannot see a partly freed queue. */
    struct js_timer *timers = s->timers;
    unsigned capacity = s->timer_capacity;
    struct js_idle_task *pending = s->idle_pending, *runnable = s->idle_runnable;
    struct js_posted_task *posted = s->posted;
    struct js_rejection *rejected=s->rejections;
    s->timers = NULL; s->timer_capacity = 0;
    s->idle_pending = s->last_idle_pending = s->idle_runnable = s->last_idle_runnable = NULL;
    s->idle_count = s->idle_period_callbacks = 0;
    s->idle_period_end = 0; s->idle_period_stopped = true;
    s->posted = s->last_posted = NULL; s->posted_count = 0;
    s->rejections=s->last_rejection=NULL;s->rejection_oom=false;
    for (unsigned i = 0; i < capacity; i++) if (timers[i].id) {
        JS_FreeValue(s->ctx, timers[i].fn); JS_FreeValue(s->ctx, timers[i].args);
    }
    js_free(s->ctx, timers);
    while (pending) {
        struct js_idle_task *p = pending; pending = p->next;
        JS_FreeValue(s->ctx, p->fn); js_free(s->ctx, p);
    }
    while (runnable) {
        struct js_idle_task *p = runnable; runnable = p->next;
        JS_FreeValue(s->ctx, p->fn); js_free(s->ctx, p);
    }
    while (posted) {
        struct js_posted_task *p = posted; posted = p->next;
        JS_FreeValue(s->ctx, p->fn); js_free(s->ctx, p);
    }
    while(rejected){struct js_rejection *p=rejected;rejected=p->next;
        JS_FreeValue(s->ctx,p->promise);JS_FreeValue(s->ctx,p->reason);js_free(s->ctx,p);}
}
static uint32_t allocate_timer_id(struct web_js_state *s) {
    for (;;) {
        uint32_t id = ++s->next_timer;
        if (!id) continue;
        unsigned i;
        for (i = 0; i < s->timer_capacity; ++i) if (s->timers[i].id == id) break;
        if (i == s->timer_capacity) return id;
    }
}
static JSValue native_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); int32_t kind = 0; double delay = 0;
    if((sandbox_authority(s)&SB_SCRIPTS) || sandbox_borrowed(s))return history_security_error(ctx,"Sandbox denied borrowed timer authority");
    if (!task_context_active(s)) return JS_ThrowTypeError(ctx, "Timer browsing context is inactive");
    if (argc < 4 || JS_ToInt32(ctx, &kind, argv[0]) || !JS_IsFunction(ctx, argv[1]) || JS_ToFloat64(ctx, &delay, argv[2])) return JS_ThrowTypeError(ctx, "Invalid timer");
    if (!task_context_active(s)) return JS_ThrowTypeError(ctx, "Timer browsing context retired during conversion");
    if (!isfinite(delay) || delay < 0) delay = 0;
    if (delay > 2147483647.0) delay = 2147483647.0;
    uint64_t ms = (uint64_t)delay; if (kind == 1 && ms < 4) ms = 4; if (kind == 2) ms = 16;
    unsigned index;
    for (index = 0; index < s->timer_capacity; ++index) if (!s->timers[index].id) break;
    if (index == s->timer_capacity) {
        size_t addressable=SIZE_MAX/sizeof *s->timers;
        unsigned maximum=addressable<UINT_MAX-1u?(unsigned)addressable:UINT_MAX-1u;
        if(s->timer_capacity>=maximum)return JS_ThrowOutOfMemory(ctx);
        unsigned capacity=s->timer_capacity?s->timer_capacity>maximum/2?maximum:s->timer_capacity*2:JS_TIMERS_INITIAL;
        if(capacity>maximum)capacity=maximum;
        /* Charged to the QuickJS allocator. A failed realloc leaves
           every existing callback and deadline intact. Growth never runs JS. */
        struct js_timer *timers = js_realloc(ctx, s->timers, (size_t)capacity * sizeof *timers);
        if (!timers) return JS_EXCEPTION;
        memset(timers + s->timer_capacity, 0, (capacity - s->timer_capacity) * sizeof *timers);
        s->timers = timers; s->timer_capacity = capacity;
    }
    uint32_t id = allocate_timer_id(s);
    struct js_timer *t = &s->timers[index];
    t->id = id; t->kind = kind; t->interval = ms; t->due = uptime_ms() + ms;
    t->fn = JS_DupValue(ctx, argv[1]); t->args = JS_DupValue(ctx, argv[3]);
    return JS_NewUint32(ctx, id);
}
/* Posted messages are tasks, not Promise jobs or clamped timers. Each callback
   receives the usual watchdog and microtask checkpoint; navigation frees it. */
static uint32_t allocate_posted_id(struct web_js_state *s) {
    for(;;){
        uint32_t id=++s->next_posted;if(!id){s->posted_id_wrapped=true;continue;}
        if(!s->posted_id_wrapped)return id;
        struct js_posted_task *p=s->posted;
        while(p&&p->id!=id)p=p->next;
        if(!p)return id;
    }
}
#include "js_broadcast.h"
static JSValue native_post_task(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if(sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Sandbox denied borrowed asynchronous authority");
    if (!task_context_active(s)) return JS_ThrowTypeError(ctx, "Posted-task browsing context is inactive");
    if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected posted task callback");
    if(s->posted_count==UINT_MAX)return JS_ThrowOutOfMemory(ctx);
    struct js_posted_task *p = js_malloc(ctx, sizeof *p); if (!p) return JS_EXCEPTION;
    p->fn = JS_DupValue(ctx, argv[0]); p->next = NULL;
    p->id=allocate_posted_id(s);
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
/* Native idle-task source: no timer alias, no allocation until requested, and
   every callback reference/queue node uses the QuickJS allocator. */
static struct js_idle_task *take_idle(struct js_idle_task **first, struct js_idle_task **last, uint32_t id) {
    struct js_idle_task **link = first, *previous = NULL;
    while (*link) {
        struct js_idle_task *p = *link;
        if (p->id == id) {
            *link = p->next; if (*last == p) *last = previous;
            p->next = NULL; return p;
        }
        previous = p; link = &p->next;
    }
    return NULL;
}
static struct js_idle_task *take_idle_id(struct web_js_state *s, uint32_t id) {
    struct js_idle_task *p = take_idle(&s->idle_pending, &s->last_idle_pending, id);
    if (!p) p = take_idle(&s->idle_runnable, &s->last_idle_runnable, id);
    if (p) s->idle_count--;
    return p;
}
static bool idle_id_used(struct web_js_state *s, uint32_t id) {
    for (struct js_idle_task *p = s->idle_pending; p; p = p->next) if (p->id == id) return true;
    for (struct js_idle_task *p = s->idle_runnable; p; p = p->next) if (p->id == id) return true;
    return false;
}
static JSValue native_idle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx);
    if(sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Sandbox denied borrowed asynchronous authority"); uint32_t timeout = 0;
    if (!task_context_active(s)) return JS_ThrowTypeError(ctx, "Idle-callback browsing context is inactive");
    if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected idle callback");
    if (argc > 1 && JS_ToUint32(ctx, &timeout, argv[1]) < 0) return JS_EXCEPTION;
    if (!task_context_active(s)) return JS_ThrowTypeError(ctx, "Idle-callback browsing context retired during conversion");
    if(s->idle_count==UINT_MAX)return JS_ThrowOutOfMemory(ctx);
    struct js_idle_task *p = js_malloc(ctx, sizeof *p); if (!p) return JS_EXCEPTION;
    uint32_t id;
    do { id = ++s->next_idle; } while (!id || idle_id_used(s, id));
    p->id = id; p->fn = JS_DupValue(ctx, argv[0]); p->next = NULL;
    p->due = timeout ? uptime_ms() + timeout : UINT64_MAX;
    if (s->last_idle_pending) s->last_idle_pending->next = p; else s->idle_pending = p;
    s->last_idle_pending = p; s->idle_count++;
    return JS_NewUint32(ctx, id);
}
static JSValue native_cancel_idle(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); uint32_t id;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]) < 0) return JS_EXCEPTION;
    struct js_idle_task *p = take_idle_id(s, id);
    if (p) { JS_FreeValue(ctx, p->fn); js_free(ctx, p); }
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
    for (unsigned i = 0; i < s->timer_capacity; i++) if (s->timers[i].id == id && id) { release_timer(s, i); break; }
    return JS_UNDEFINED;
}
static JSValue microtask_job(JSContext *ctx, int argc, JSValueConst *argv) {
    return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
}
static JSValue native_microtask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if((sandbox_authority(state(ctx))&SB_SCRIPTS) || sandbox_borrowed(state(ctx)))return history_security_error(ctx,"Sandbox denied borrowed microtask authority");
    if (!argc || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "Expected callback");
    /* A browser microtask is a host job, not a call through the page's mutable
       Promise constructor. Promise polyfills themselves use queueMicrotask;
       implementing it with Promise.resolve causes recursive scheduling. The
       engine retains the callback until execution or runtime destruction. */
    if (JS_EnqueueJobForCallback(ctx, argv[0], microtask_job, 1, argv) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}
static struct js_pending *pending_new(struct web_js_state *s, int kind, const char *url) {
    if(s->pending_count==UINT_MAX){JS_ThrowOutOfMemory(s->ctx);return NULL;}
    struct js_pending *p = js_mallocz(s->ctx, sizeof *p); if (!p) return NULL;
    p->url = js_strdup(s->ctx, url);
    if (!p->url) { js_free(s->ctx, p); return NULL; }
    p->id = ++s->runtime_owner->next_request; p->kind = kind; p->deadline = uptime_ms() + WEBNET_TIMEOUT_MS;
    p->resolve = p->reject = JS_UNDEFINED; p->next = s->pending; s->pending = p; s->pending_count++;
    return p;
}
static void pending_error(struct js_pending *p, const char *error) { p->done = true; snprintf(p->response.error, sizeof p->response.error, "%s", error); }
static void release_pending_request(struct web_js_state *s, struct js_pending *p) {
    /* Teardown never routes a completed keepalive response into a retired realm.
       Explicit aborts use host.cancel, not this lifecycle-only operation. */
    if (p->keepalive && s->host.release_request) s->host.release_request(s->host.opaque, p->id);
    else if (s->host.cancel) s->host.cancel(s->host.opaque, p->id);
}
static bool send_request(struct web_js_state *s, struct js_pending *p, int kind, const char *method,
                         const char *headers, const void *body, size_t len) {
    if(s->disabled){pending_error(p,"Browsing context is inactive");return false;}
    if ((p->kind==P_SCRIPT || p->kind==P_FONT) && (!strncasecmp(p->url,"data:",5) || !strncasecmp(p->url,"blob:",5))) {
        char mime[128];
        p->response.body=!strncasecmp(p->url,"data:",5)
            ? script_data_source(s,p->url,kind==WEB_RESOURCE_MODULE,&p->response.body_len,mime)
            : script_blob_source(s,p->url,kind==WEB_RESOURCE_MODULE,&p->response.body_len,mime);
        if (!p->response.body) {
            if (JS_HasException(s->ctx)) exception(s);
            pending_error(p,"Unavailable local script URL or invalid JavaScript MIME type"); return false;
        }
        p->response.status=200;
        /* No redirect occurred. The complete URL already belongs to the
           pending/script record; an HTTP-sized field would truncate source. */
        p->response.url[0]=0;
        snprintf(p->response.headers,sizeof p->response.headers,"Content-Type: %s\r\n",mime);
        p->done=true; return true;
    }
    struct web_request r = {.id=p->id, .kind=kind, .url=p->url, .method=method, .headers=headers, .body=body, .body_len=len,
        .origin=p->origin?p->origin:web_effective_url(s->doc),
        .credentials=kind == WEB_RESOURCE_FETCH ? p->credentials : kind == WEB_RESOURCE_MODULE ? 1 : 2,
        .force_preflight=p->force_preflight, .redirect_error=p->redirect_error, .same_origin=p->same_origin,
        .no_cors=p->no_cors, .no_referrer=p->no_referrer,
        .cache_mode=p->cache_mode, .keepalive=p->keepalive, .fetch_group=p->keepalive?s->fetch_group:0};
    if(kind==WEB_RESOURCE_IMAGE && p->kind==P_IMAGE && p->image>=0 && p->image<s->doc->images.n)
        r.image_upgrade=((struct web_image *)s->doc->images.v[p->image])->image_upgrade;
    bool ok = s->host.request && s->host.request(s->host.opaque, &r);
    if (!ok) pending_error(p, "The browser rejected the resource request");
    return ok;
}
#include "frame_native.h"
/* Children have no networking/file host. Parent transport owns cookies, CORS,
   redirects and generation cancellation; completions only queue native bytes. */
static bool worker_load(void *opaque, uint32_t worker, uint32_t request,
                        const char *url, int kind) {
    struct web_js_state *s = opaque;
    if (!s || s->disabled || kind < 0 ||
        (kind != 0 && !njw_load_kind_valid((uint32_t)kind))) return false;
    const uint32_t policy = (uint32_t)kind & NJW_LOAD_POLICY_FLAGS;
    kind = (int)((uint32_t)kind & ~NJW_LOAD_POLICY_FLAGS);
    struct js_pending *p = pending_new(s, P_WORKER, url);
    if (!p) {
        /* Loader rejection is reported over Worker IPC. Do not leave an OOM
           exception pending in a parent realm that is not executing a task. */
        if (JS_HasException(s->ctx)) { JSValue error=JS_GetException(s->ctx); JS_FreeValue(s->ctx,error); }
        return false;
    }
    p->worker = worker; p->worker_request = request; p->worker_kind = kind;
    bool blob = !strncasecmp(url,"blob:",5);
    if (strlen(url) > UINT32_MAX || (!blob && !permitted_url(s,url,true)) || (blob && kind == 2)) {
        pending_error(p,"Worker resource URL is not permitted"); return true;
    }
    if (blob) {
        p->done = true; p->response.status = 200;
        /* Worker(blobURL) starts fetching now. Retain its bytes before author
           code can revoke the URL immediately after the constructor returns.
           Other child requests are resolved at a later bounded task boundary. */
        if (kind == 0 && s->running) {
            char mime[128];
            p->response.body=script_blob_source(s,url,true,&p->response.body_len,mime);
            if (!p->response.body) {
                if (JS_HasException(s->ctx)) { JSValue error=JS_GetException(s->ctx); JS_FreeValue(s->ctx,error); }
                pending_error(p,"Worker object URL is unavailable or has an invalid JavaScript MIME type");
            } else snprintf(p->response.headers,sizeof p->response.headers,"Content-Type: %s\r\n",mime);
        }
        return true;
    }
    bool allocation_failed = false;
    if (kind == 0 && !same_origin_urls(web_effective_url(s->doc), url, &allocation_failed)) {
        pending_error(p, allocation_failed ? "Out of memory while checking Worker origin" :
            "Worker startup requires a same-origin script"); return true;
    }
    struct web_request r = {.id=p->id, .kind=kind == 2 ? WEB_RESOURCE_FETCH : WEB_RESOURCE_SCRIPT,
        /* Worker settings inherit the creating realm's origin, including
           about:blank/srcdoc children. Never let the shared host substitute
           the top-level document as the initiator of startup/import/fetch. */
        .url=p->url, .origin=web_effective_url(s->doc), .method="GET",
        .credentials=1, .same_origin=kind == 0,
        .no_cors=(policy & NJW_LOAD_NO_CORS)!=0,
        .no_referrer=(policy & NJW_LOAD_NO_REFERRER)!=0};
    if (!s->host.request || !s->host.request(s->host.opaque,&r))
        pending_error(p,"The browser rejected the Worker resource request");
    return true;
}
static void worker_cancel(void *opaque,uint32_t worker,uint32_t request) {
    struct web_js_state *s=opaque;if(!s)return;
    for(struct js_pending *p=s->pending;p;p=p->next){
        if(p->kind!=P_WORKER||p->worker!=worker||p->done||
           (request&&(p->worker_request!=request||p->worker_kind!=2)))continue;
        /* The child's request identity never names a parent or sibling fetch.
           Retire before cancellation so a native completion cannot revive it. */
        p->aborted=true;pending_error(p,"The Worker resource was aborted");
        if(s->host.cancel)s->host.cancel(s->host.opaque,p->id);
    }
}
static JSValue native_worker(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    struct web_js_state *s = state(ctx); int32_t op;
    if (!argc || JS_ToInt32(ctx,&op,argv[0])) return JS_EXCEPTION;
    if((sandbox_authority(s)&SB_SCRIPTS) || sandbox_borrowed(s))return history_security_error(ctx,"Sandbox denied Worker authority");
    if(s->disabled)return op==2?JS_UNDEFINED:JS_ThrowTypeError(ctx,"Worker belongs to an inactive browsing context");
    if (op == 0) {
        struct node *generation = s->doc->root;
        const char *url = argc > 2 ? JS_ToCString(ctx,argv[2]) : NULL;
        if (!url) return JS_EXCEPTION;
        if (!task_context_active(s) || s->doc->root != generation) {
            JS_FreeCString(ctx, url); return JS_ThrowTypeError(ctx, "Worker context retired during URL conversion");
        }
        bool allocation_failed = false;
        bool blob = !strncasecmp(url,"blob:",5);
        bool allowed = strlen(url)<=UINT32_MAX && (blob || (permitted_url(s,url,true) &&
            same_origin_urls(web_effective_url(s->doc), url, &allocation_failed)));
        JS_FreeCString(ctx,url);
        if (allocation_failed) return JS_ThrowOutOfMemory(ctx);
        if (!allowed) return JS_ThrowTypeError(ctx,"SecurityError: Worker source must be same-origin");
    }
    if (!s->workers) {
        if (op != 0) return JS_UNDEFINED;
        static uint32_t generation;
        if (!++generation) ++generation;
        s->workers = web_worker_new(ctx,generation,worker_load,s);
        if (!s->workers) return oom(ctx);
        web_worker_set_canceler(s->workers,s->host.cancel?worker_cancel:NULL);
        JSValue fn = JS_GetPropertyStr(ctx,s->hooks,"workerNotify");
        if (JS_IsException(fn)) return fn;
        web_worker_set_callback(s->workers,fn); JS_FreeValue(ctx,fn);
    }
    if(op==8||op==9){
        if(argc<6)return JS_ThrowTypeError(ctx,"Worker capture arguments missing");
        uint32_t id=0,port=0,creator=0;
        if(JS_ToUint32(ctx,&id,argv[1])<0||JS_ToUint32(ctx,&port,argv[4])<0||JS_ToUint32(ctx,&creator,argv[5])<0)return JS_EXCEPTION;
        if(op==9){web_worker_release_capture(s->workers,port);return JS_UNDEFINED;}
        if(!task_context_active(s))return JS_ThrowTypeError(ctx,"Worker capture context is inactive");
        return web_worker_capture(s->workers,id,port,creator);
    }
    if(op==3||op==6||op==7){
        if(argc<4||!task_context_active(s))return JS_ThrowTypeError(ctx,"Worker transfer context is inactive");
        uint32_t id=0,port=0,creator=0;if(JS_ToUint32(ctx,&id,argv[1])<0)return JS_EXCEPTION;
        if(op!=3&&(argc<6||JS_ToUint32(ctx,&port,argv[4])<0||JS_ToUint32(ctx,&creator,argv[5])<0))return JS_EXCEPTION;
        uint32_t capture=0;if(argc>6&&JS_ToUint32(ctx,&capture,argv[6])<0)return JS_EXCEPTION;
        web_worker_publication *publication=web_worker_prepare_captured(s->workers,id,op==3?NJW_MESSAGE:op==6?NJW_PORT_MESSAGE:NJW_PORT_CLOSE,port,op==3?NJW_WITH_PORTS:creator,argv[2],capture);
        if(!publication)return JS_EXCEPTION;
        JSValue result=frame_transfer_commit(ctx,s,argv[3]);
        if(JS_IsException(result)){web_worker_abort(publication);return result;}
        web_worker_publish(publication);return result;
    }
    JSValue result=web_worker_native(s->workers,argc,argv);
    if (op == 2 && argc > 1 && !JS_IsException(result)) {
        uint32_t id;
        if (!JS_ToUint32(ctx,&id,argv[1])) {
            struct js_pending **link=&s->pending;
            while (*link) {
                struct js_pending *p=*link;
                if (p->kind != P_WORKER || p->worker != id) { link=&p->next; continue; }
                *link=p->next;
                if (s->host.cancel) s->host.cancel(s->host.opaque,p->id);
                JS_FreeValue(ctx,p->resolve); JS_FreeValue(ctx,p->reject);
                js_free(ctx,p->url); js_free(ctx,p->response.body); js_free(ctx,p->response.headers_full); js_free(ctx,p->response.url_full);
                js_free(ctx,p); s->pending_count--;
            }
        }
    }
    return result;
}
static JSValue native_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    static uint64_t fetch_group_serial;
    struct web_js_state *s = state(ctx);
    if(sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Sandbox denied borrowed asynchronous authority");
    if (argc < 4) return JS_ThrowTypeError(ctx, "Invalid fetch request");
    if (!task_context_active(s)) return JS_ThrowTypeError(ctx, "Fetch browsing context is inactive");
    struct node *generation = s->doc->root;
    size_t header_length = 0, method_length = 0;
    const char *rel = JS_ToCString(ctx, argv[0]), *method = JS_ToCStringLen(ctx, &method_length, argv[1]), *headers = JS_ToCStringLen(ctx, &header_length, argv[2]);
    size_t n = 0; const char *body_string = NULL;
    const void *body;
    if (JS_IsString(argv[3])) body = body_string = JS_ToCStringLen(ctx, &n, argv[3]);
    else body = JS_GetArrayBuffer(ctx, &n, argv[3]);
    char *url = NULL;
    JSValue result = JS_EXCEPTION;
    if (!rel || !method || !headers || (!body && JS_HasException(ctx))) goto out;
    if (!task_context_active(s) || s->doc->root != generation) { result = JS_ThrowTypeError(ctx, "Fetch browsing context retired during conversion"); goto out; }
    if (s->doc->resources_dirty) doc_sync_tree(s->doc);
    url = js_resolve_owned(ctx, s->doc->base, rel); if (!url) goto out;
    if (!permitted_url(s, url, true)) { result = JS_ThrowTypeError(ctx, "fetch supports HTTP(S) resources only"); goto out; }
    if (method_length > UINT32_MAX || memchr(method, 0, method_length) || !webnet_method_valid(method) || ((!strcmp(method, "GET") || !strcmp(method, "HEAD")) && n)) { result = JS_ThrowTypeError(ctx, "Invalid HTTP method or body"); goto out; }
    if (n > JS_BODY_LIMIT || header_length > UINT32_MAX) { result = JS_ThrowRangeError(ctx, "Request length is not representable on the native wire"); goto out; }
    if (memchr(headers, 0, header_length)) { result = JS_ThrowTypeError(ctx, "Invalid request header characters"); goto out; }
    bool same_origin = argc > 4 && JS_ToBool(ctx, argv[4]) > 0;
    if (same_origin) {
        bool allocation_failed = false;
        if (!same_origin_urls(web_effective_url(s->doc), url, &allocation_failed)) {
            result = allocation_failed ? JS_ThrowOutOfMemory(ctx) :
                JS_ThrowTypeError(ctx, "Cross-origin fetch is forbidden by same-origin mode"); goto out;
        }
    }
    int32_t credentials = 1, redirect = 0, cache_mode = WEBNET_CACHE_DEFAULT;
    if ((argc > 5 && (JS_ToInt32(ctx, &credentials, argv[5]) || credentials < 0 || credentials > 2)) ||
        (argc > 7 && (JS_ToInt32(ctx, &redirect, argv[7]) || redirect < 0 || redirect > 1)) ||
        (argc > 8 && (JS_ToInt32(ctx, &cache_mode, argv[8]) || cache_mode < WEBNET_CACHE_DEFAULT || cache_mode > WEBNET_CACHE_ONLY_IF_CACHED))) {
        result = JS_ThrowTypeError(ctx, "Invalid fetch policy"); goto out;
    }
    if (!task_context_active(s) || s->doc->root != generation) { result = JS_ThrowTypeError(ctx, "Fetch browsing context retired during conversion"); goto out; }
    /* Policy coercion can run author code and detach the caller's buffer.
       Acquire its backing pointer again only after the last such conversion. */
    if (!JS_IsString(argv[3])) {
        body = JS_GetArrayBuffer(ctx, &n, argv[3]);
        if (!body && JS_HasException(ctx)) goto out;
        if (n > JS_BODY_LIMIT) { result = JS_ThrowRangeError(ctx, "Request length is not representable on the native wire"); goto out; }
        if ((!strcmp(method, "GET") || !strcmp(method, "HEAD")) && n) { result = JS_ThrowTypeError(ctx, "Invalid HTTP method or body"); goto out; }
    }
    /* No HTTP response cache is present. Never turn a cache-only miss into I/O,
       even if a caller bypasses the bootstrap's early rejection. */
    if (cache_mode == WEBNET_CACHE_ONLY_IF_CACHED) { result = JS_ThrowTypeError(ctx, "No cached response is available"); goto out; }
    bool keepalive = argc > 9 && JS_ToBool(ctx, argv[9]) > 0;
    if (keepalive && !s->host.release_request) { result = JS_ThrowTypeError(ctx, "Native host cannot retain keepalive requests across document retirement"); goto out; }
    if (keepalive && !s->fetch_group) {
        /* A pointer or per-runtime request number can be reused after top-level
           navigation while an older native request is still alive. */
        if (fetch_group_serial == UINT64_MAX) { result = JS_ThrowRangeError(ctx, "Fetch group identity is not representable"); goto out; }
        s->fetch_group = ++fetch_group_serial;
    }
    struct js_pending *p = pending_new(s, P_FETCH, url);
    if (!p) { result = oom(ctx); goto out; }
    p->credentials = credentials;
    p->keepalive = keepalive;
    p->cache_mode = cache_mode;
    p->same_origin = same_origin;
    p->redirect_error = redirect == 1;
    p->force_preflight = argc > 6 && JS_ToBool(ctx, argv[6]) > 0;
    p->no_cors = argc > 10 && JS_ToBool(ctx, argv[10]) > 0;
    p->no_referrer = argc > 11 && JS_ToBool(ctx, argv[11]) > 0;
    JSValue funcs[2]; JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { pending_error(p, "Out of memory"); goto out; }
    p->resolve = funcs[0]; p->reject = funcs[1];
    result = JS_NewObject(ctx);
    if (JS_IsException(result)) { JS_FreeValue(ctx, promise); pending_error(p, "Out of memory"); goto out; }
    if (JS_SetPropertyStr(ctx, result, "id", JS_NewInt64(ctx, (int64_t)p->id)) < 0) {
        JS_FreeValue(ctx,promise); JS_FreeValue(ctx,result); result=JS_EXCEPTION;
        pending_error(p,"Out of memory preparing fetch result"); goto out;
    }
    if (JS_SetPropertyStr(ctx, result, "promise", promise) < 0) {
        JS_FreeValue(ctx,result); result=JS_EXCEPTION;
        pending_error(p,"Out of memory preparing fetch result"); goto out;
    }
    send_request(s, p, WEB_RESOURCE_FETCH, method, headers, body, n);
out:
    js_free(ctx, url); JS_FreeCString(ctx, rel); JS_FreeCString(ctx, method); JS_FreeCString(ctx, headers); JS_FreeCString(ctx, body_string); return result;
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
    if (url && len == strlen(url) && permitted_script_url(s, url)) result = js_strdup(s->ctx, url);
    else if (url) JS_ThrowTypeError(s->ctx, "Module URL is not permitted or exceeds the URL limit: %s", name);
    JS_FreeCString(s->ctx, url); JS_FreeValue(s->ctx, value); return result;
}
static char *normalize_module(JSContext *ctx, const char *base, const char *name, void *opaque) {
    struct web_js_state *s = state(ctx);
    /* An entry script's src is a URL, not an import specifier. Consume this
       bypass before linking so every dependency still goes through the map. */
    if (s->module_entry_url && !strcmp(s->module_entry_url, name)) {
        s->module_entry_url = NULL; return js_strdup(ctx, name);
    }
    const char *resolve_base = base && *base ? base : s->doc->base;
    for (struct js_module *m = s->modules; m; m = m->next) if (!strcmp(m->url, resolve_base)) { resolve_base = m->base; break; }
    if ((!strncasecmp(resolve_base,"data:",5) || !strncasecmp(resolve_base,"blob:",5)) &&
        (name[0]=='/' || !strncmp(name,"./",2) || !strncmp(name,"../",3))) {
        JS_ThrowTypeError(ctx,"Relative module imports require a hierarchical base URL"); return NULL;
    }
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
static bool data_space(unsigned char c) { return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f'; }
static int data_hex(unsigned char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static int data_base64(unsigned char c) {
    if (c>='A' && c<='Z') return c-'A';
    if (c>='a' && c<='z') return c-'a'+26;
    if (c>='0' && c<='9') return c-'0'+52;
    return c=='+'?62:c=='/'?63:-1;
}
/* Fetch data-URL processing and Infra forgiving-base64. The URL has already
 * been serialized by the private WHATWG URL binding. Metadata is NOT percent
 * decoded; the body is decoded before any base64 whitespace/padding processing.
 * Buffers use the document QuickJS allocator, not unbounded host allocations. */
static char *script_data_source(struct web_js_state *s,const char *url,bool module,size_t *length,char mime[128]) {
    if (!permitted_script_url(s,url) || strncasecmp(url,"data:",5)) {
        JS_ThrowTypeError(s->ctx,"Data script URL exceeds the URL limit"); return NULL;
    }
    const char *end=strchr(url,'#'); if (!end) end=url+strlen(url);
    const char *meta=url+5,*comma=memchr(meta,',',(size_t)(end-meta));
    if (!comma) { JS_ThrowTypeError(s->ctx,"Data script URL has no comma"); return NULL; }
    const char *meta_end=comma;
    while (meta<meta_end && data_space((unsigned char)*meta)) meta++;
    while (meta_end>meta && data_space((unsigned char)meta_end[-1])) meta_end--;
    bool base64=false;
    if (meta_end-meta>=7 && !strncasecmp(meta_end-6,"base64",6)) {
        const char *marker=meta_end-6;
        while (marker>meta && marker[-1]==' ') marker--;
        if (marker>meta && marker[-1]==';') { base64=true; meta_end=marker-1; }
    }
    /* A data URL has one MIME record, NOT an HTTP list. Validate its essence
     * before applying strict module MIME checks; invalid/default is text/plain. */
    const char *essence_end=memchr(meta,';',(size_t)(meta_end-meta));
    if (!essence_end) essence_end=meta_end;
    while (essence_end>meta && mime_space((unsigned char)essence_end[-1])) essence_end--;
    size_t mn=(size_t)(essence_end-meta);
    bool valid=valid_mime_essence(meta,essence_end) && mn<128;
    bool javascript=valid && javascript_essence(meta,mn);
    if (module && !javascript) { JS_ThrowTypeError(s->ctx,"Data module requires a JavaScript MIME type"); return NULL; }
    if (valid) { memcpy(mime,meta,mn); mime[mn]=0; }
    else strcpy(mime,"text/plain");
    size_t encoded=(size_t)(end-comma-1);
    if (encoded>JS_BODY_LIMIT) { JS_ThrowRangeError(s->ctx,"Data script length is not representable"); return NULL; }
    unsigned char *bytes=js_malloc(s->ctx,encoded+1);
    if (!bytes) return NULL;
    size_t n=0;
    for (const char *p=comma+1;p<end;p++) {
        int hi,lo;
        if (*p=='%' && end-p>=3 && (hi=data_hex((unsigned char)p[1]))>=0 && (lo=data_hex((unsigned char)p[2]))>=0) {
            bytes[n++]=(unsigned char)((hi<<4)|lo); p+=2;
        } else bytes[n++]=(unsigned char)*p; /* '+' is never a space here. */
    }
    if (base64) {
        size_t count=0;
        for (size_t i=0;i<n;i++) if (!data_space(bytes[i])) bytes[count++]=bytes[i];
        n=count;
        if (n%4==0 && n && bytes[n-1]=='=') { n--; if (n && bytes[n-1]=='=') n--; }
        if (n%4==1) goto malformed;
        for (size_t i=0;i<n;i++) if (data_base64(bytes[i])<0) goto malformed;
        unsigned bits=0,buffer=0; count=0;
        for (size_t i=0;i<n;i++) {
            buffer=(buffer<<6)|(unsigned)data_base64(bytes[i]); bits+=6;
            if (bits>=8) { bits-=8; bytes[count++]=(unsigned char)(buffer>>bits); }
        }
        n=count; /* Infra deliberately discards nonzero unused low bits. */
    }
    bytes[n]=0; *length=n; return (char *)bytes;
malformed:
    js_free(s->ctx,bytes); JS_ThrowTypeError(s->ctx,"Invalid forgiving-base64 data script body"); return NULL;
}
/* The private hook returns only real Blob bytes from this document's live
 * registry, never an author-visible URL property or an OS path. Root its result
 * while allocating the tracked source copy; MIME and byte limits remain native. */
static char *script_blob_source(struct web_js_state *s,const char *url,bool module,size_t *length,char mime[128]) {
    JSContext *ctx=s->ctx;
    JSValue value=module_bridge_call(s,"blobScript",url,strlen(url),s->doc->base);
    if (JS_IsException(value)) return NULL;
    JSValue buffer=JS_GetPropertyUint32(ctx,value,0),type=JS_UNDEFINED;
    const char *content_type=NULL; char *source=NULL; size_t n=0,tn=0;
    if (JS_IsException(buffer)) goto out;
    const uint8_t *bytes=JS_GetArrayBuffer(ctx,&n,buffer);
    if (!bytes && JS_HasException(ctx)) goto out;
    if (n>JS_BODY_LIMIT) { JS_ThrowRangeError(ctx,"Blob script length is not representable"); goto out; }
    type=JS_GetPropertyUint32(ctx,value,1);
    if (JS_IsException(type)) goto out;
    content_type=JS_ToCStringLen(ctx,&tn,type);
    if (!content_type) goto out;
    const char *first=content_type,*end=memchr(first,';',tn);
    if (!end) end=first+tn;
    while (first<end && mime_space((unsigned char)*first)) first++;
    while (end>first && mime_space((unsigned char)end[-1])) end--;
    size_t mn=(size_t)(end-first);
    bool valid=valid_mime_essence(first,end) && mn<128;
    if (module && (!valid || !javascript_essence(first,mn))) {
        JS_ThrowTypeError(ctx,"Blob module requires a JavaScript MIME type"); goto out;
    }
    if (valid) { memcpy(mime,first,mn); mime[mn]=0; } else strcpy(mime,"text/plain");
    web_js_prepare_bytes(ctx,n+1);
    source=js_malloc(ctx,n+1);
    if (source) { if (n) memcpy(source,bytes,n); source[n]=0; *length=n; }
out:
    JS_FreeCString(ctx,content_type); JS_FreeValue(ctx,type);
    JS_FreeValue(ctx,buffer); JS_FreeValue(ctx,value); return source;
}
/* Only the host's explicit source-preparation boundary gets this allowance.
   COMPILE_ONLY does not run page code. Recursive module preparation is counted
   once, and the sum is retained until the outer task (including jobs) ends.
   QuickJS's parser is not fully interruptible: also check the elapsed time on
   return, before executing any resulting bytecode. Body/heap limits still apply. */
static JSValue compile_source(struct web_js_state *s, const char *source, size_t len, const char *url, int kind) {
    if (interrupt(s->rt, s)) return JS_ThrowInternalError(s->ctx, "JavaScript execution was stopped");
    if (len > JS_SOURCE_LIMIT) return JS_ThrowRangeError(s->ctx, "JavaScript source length is not representable");
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
/* A native-only host wait cannot enter author JS or free these arenas. Still
   revalidate the actual child/ancestor navigables before publishing its bytes:
   a retired realm must never resume its module graph in a replacement child. */
static bool module_context_current(struct web_js_state *s, node_t *root) {
    if (!task_context_active(s) || s->doc->root != root) return false;
    for (web_doc *d=s->doc; d; d=d->frame_parent) {
        if (!task_context_active(d->js)) return false;
        if (d->frame_parent) {
            struct web_frame *f=d->frame_container;
            if (!f || f->owner!=d->frame_parent || f->document!=d || f->detached ||
                f->element!=d->frame_element || f->element->owner!=f->owner || !doc_node_connected(f->element)) return false;
        }
    }
    return true;
}
static JSModuleDef *load_module(JSContext *ctx, const char *url, void *opaque) {
    struct web_js_state *s = state(ctx);
    node_t *document_root=s->doc->root;
    if (!module_context_current(s,document_root)) { JS_ThrowTypeError(ctx,"Module browsing context is inactive"); return NULL; }
    if (interrupt(s->rt, s)) { JS_ThrowInternalError(ctx, "JavaScript execution was stopped"); return NULL; }
    if (!permitted_script_url(s, url)) { JS_ThrowReferenceError(ctx, "Module URL is not permitted"); return NULL; }
    struct js_module *m = NULL;
    for (m = s->modules; m; m = m->next) if (!strcmp(m->url, url)) break;
    if (!m && (!strncasecmp(url,"data:",5) || !strncasecmp(url,"blob:",5))) {
        size_t length; char mime[128];
        char *source=!strncasecmp(url,"data:",5)
            ? script_data_source(s,url,true,&length,mime)
            : script_blob_source(s,url,true,&length,mime);
        if (!source) return NULL;
        m=cache_module(s,url,source,length,url); js_free(ctx,source);
        if (!m) return NULL;
    } else if (!m) {
        if (s->doc->frame_parent && !s->host.sync_request) {
            JS_ThrowTypeError(ctx, "Child module synchronous loading requires an origin-aware host"); return NULL;
        }
        /* URL storage is deliberately large. Keep responses and diagnostic
           copies off the native stack, especially in recursive module graphs. */
        struct js_pending *diagnostic = js_mallocz(ctx, sizeof *diagnostic);
        if (!diagnostic) return NULL;
        struct web_response *r = &diagnostic->response; uint64_t before = uptime_ms();
        diagnostic->kind = P_SCRIPT; diagnostic->url = (char *)url; diagnostic->deadline = before + WEBNET_TIMEOUT_MS;
        struct web_request request={.kind=WEB_RESOURCE_MODULE, .url=url, .method="GET",
            .origin=web_effective_url(s->doc), .credentials=1};
        bool ok = s->host.sync_request
            ? s->host.sync_request(s->host.opaque, &request, r)
            : s->host.sync_load && s->host.sync_load(s->host.opaque, url, WEB_RESOURCE_MODULE, r);
        /* Native UI/network wait is not JavaScript execution. The synchronous
           loader is the one place this adjustment is allowed. */
        uint64_t waited = uptime_ms() - before;
        /* document.write can prepare a child graph inside a parent's task.
           Exclude this same native wait from every suspended task, not just
           the child's deadline (which otherwise inherits the parent's guard). */
        for (struct web_js_state *active=s; active; active=active->runtime_previous)
            if (active->running) { active->task_deadline += waited; active->task_native_wait_ms += waited; }
        if (s->compiling) s->compile_wait_ms += waited;
        if (!module_context_current(s,document_root)) {
            web_response_free(r); js_free(ctx,diagnostic);
            JS_ThrowTypeError(ctx,"Module browsing context changed during native loading"); return NULL;
        }
        bool local = !strncasecmp(url, "file://", 7);
        if (!ok || r->error[0] || r->status < 200 || r->status >= 300 || r->body_len > JS_SOURCE_LIMIT || (r->body_len && !r->body) ||
            strlen(web_response_headers(r)) > WEB_RESPONSE_HEADERS_MAX ||
            (!local && !javascript_mime(web_response_headers(r))) || (web_response_url(r)[0] && !permitted_url(s, web_response_url(r), false))) {
            diagnostic_resource(s,diagnostic,r->error[0]?r->error:"module response rejected (HTTP, MIME, size or URL policy)");
            JS_ThrowReferenceError(ctx, "Cannot load JavaScript module %s: %s", url, r->error[0] ? r->error : "invalid response or JavaScript MIME type");
            web_response_free(r); js_free(ctx, diagnostic); return NULL;
        }
        if(waited>=2000 && s->resource_failures<64){
            char safe[640],message[832];diagnostic_url(web_response_url(r)[0]?web_response_url(r):url,safe,sizeof safe);
            snprintf(message,sizeof message,"Module load: HTTP %d, %lu bytes, native wait %lu ms; %s",r->status,(unsigned long)r->body_len,(unsigned long)waited,safe);
            log_text(s,0,message);
        }
        char *final = web_response_url(r)[0] ? module_url(s, "moduleURL", web_response_url(r), url) : js_strdup(ctx, url);
        web_js_prepare_bytes(ctx, r->body_len + 1);
        m = final ? cache_module(s, url, r->body, r->body_len, final) : NULL;
        js_free(ctx, final);
        web_response_free(r); js_free(ctx, diagnostic); if (!m) return NULL;
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
    begin_named_task(s,"module preparation",script->request);
    web_js_prepare_bytes(s->ctx, script->len + 1);
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
    if(s->doc->sandbox_flags&SB_SCRIPTS){n->script_started=true;return NULL;}
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
        JSValue value = text.n > JS_BODY_LIMIT ? JS_ThrowRangeError(s->ctx, "Import map length is not representable") :
            module_bridge_call(s, "registerImportMap", text.p ? text.p : "", text.n, s->doc->base);
        if (JS_IsException(value)) exception(s); else JS_FreeValue(s->ctx, value);
        end_task(s); sb_free(&text); return NULL;
    }
    bool module;
    if (s->disabled || !script_type(n, &module) || node_ancestor(n, T_template)) return NULL;
    if (s->script_count == UINT32_MAX) { log_text(s, 2, "Script identity is not representable"); return NULL; }
    struct js_script *script = js_mallocz(s->ctx, sizeof *script);
    if (!script) { exception(s); return NULL; }
    script->evaluation = JS_UNDEFINED; script->node = n; script->module = module; script->dynamic = dynamic;
    bool force_async = dynamic;
    if (n->js_force_async_set) force_async = n->js_force_async;
    /* Capture preparation-time state for both classic and module scripts. */
    script->asynchronous = (force_async || node_attr(n, "async")) && (module || (src && *src));
    script->deferred = !dynamic && !script->asynchronous && (module || (src && *src && node_attr(n, "defer")));
    if (src && *src) {
        if (module) {
            begin_task(s); script->url = module_url(s, "moduleURL", src, s->doc->base);
            if (!script->url) { script->ready = script->failed = true; exception(s); }
            end_task(s);
        } else {
            /* The generic native resolver rewrites backslashes even for opaque
             * URLs and may truncate. Use the script-private WHATWG URL binding. */
            begin_task(s); script->url=module_url(s,"moduleURL",src,s->doc->base);
            if (!script->url) { script->ready=script->failed=true; exception(s); }
            end_task(s);
        }
        if (!script->failed) {
            /* Diagnose the author-provided value before requesting it. Never
               remove a substring or guess an extension: "undefined" can also
               be a legitimate URL path. Keep this warning finite per document. */
            if (dynamic && strstr(src,"undefined") && s->suspicious_script_urls++ < 8) {
                char author[400],resolved[640],message[1200];
                diagnostic_url(src,author,sizeof author);
                diagnostic_url(script->url,resolved,sizeof resolved);
                snprintf(message,sizeof message,"Script URL warning: dynamic src already contains literal 'undefined' before URL resolution; author src=%s; resolved=%s (URL left unchanged)",author,resolved);
                log_text(s,1,message);
            }
            struct js_pending *p = pending_new(s, P_SCRIPT, script->url);
            if (!p) script->ready = script->failed = true;
            else { p->script = script; script->request = p->id; send_request(s, p, module ? WEB_RESOURCE_MODULE : WEB_RESOURCE_SCRIPT, "GET", NULL, NULL, 0); }
        }
    } else {
        size_t capacity = strlen(s->doc->url) + 64;
        char *url = js_malloc(s->ctx, capacity);
        if (url) {
            if (module) snprintf(url, capacity, "%s#nocturne-inline-module-%u", s->doc->url, s->script_count + 1);
            else snprintf(url, capacity, "%s", s->doc->url);
        }
        if (url && module) {
            begin_task(s); script->url = module_url(s, "moduleURL", url, s->doc->base);
            if (!script->url) exception(s);
            end_task(s);
        } else if (url) script->url = js_strdup(s->ctx, url);
        js_free(s->ctx, url);
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
    if(web_frame_element(n) && connected(s,n))frame_sync(s,n);
    if (n->shadow_root) dynamic_scripts_walk(s, n->shadow_root);
    for (node_t *c = n->first; c; c = c->next) {
        if(s->host.debug_js)s->doc->profile.script_visits++;
        if (c->type != N_ELEM || c->tag == T_template) continue;
        if (c->tag == T_script && !c->script_started) queue_script(s, c, true);
        dynamic_scripts_walk(s, c);
    }
}
static void dynamic_scripts(struct web_js_state *s, node_t *n) {
    uint64_t start = s->host.debug_js?uptime_ms():0;
    if(s->host.debug_js)s->doc->profile.script_scans++;
    dynamic_scripts_walk(s, n);
    if(s->host.debug_js)s->doc->profile.script_ms += uptime_ms() - start;
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
bool web_js_stylesheet_event(web_doc *d, node_t *n, bool failed) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->disabled || !n) return false;
    struct js_resource_event *e = script_event(s,n,failed);
    if (!e) return false;
    e->stylesheet = true; e->generation = n->stylesheet_generation; return true;
}
void web_js_selection_changed(web_doc *d, node_t *n) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->disabled) return;
    struct js_resource_event *event = script_event(s, n, false);
    if (event) event->selection = true;
}
static void run_script(struct web_js_state *s, struct js_script *script) {
    if (script->executed || !script->ready) return;
    if(s->doc->sandbox_flags&SB_SCRIPTS){script->executed=true;return;}
    script->executed = true;
    if (s->disabled || script->failed || !script->source) { if (!s->disabled) { script_event(s, script->node, true); script->notified = true; } return; }
    begin_named_task(s,script->module?"module script":"classic script",script->request);
    node_t *old_current = s->current_script; bool old_write = s->parser_write;
    s->current_script = script->module ? NULL : script->node;
    s->parser_write = !script->module && s->blocker == script && !s->parsing_done;
    uint64_t source_start = s->host.debug_js?uptime_ms():0, compiled_at = source_start;
    JSValue result;bool compilation_failed=false;
    if (script->module) {
        struct js_module *m = cache_module(s, script->url, script->source, script->len, script->base);
        s->module_entry_url = script->url;
        result = m ? JS_LoadModule(s->ctx, s->doc->base, script->url) : JS_EXCEPTION;
        s->module_entry_url = NULL;
    } else {
        result = compile_source(s, script->source, script->len, script->url, JS_EVAL_TYPE_GLOBAL);
        compiled_at = s->host.debug_js?uptime_ms():0;
        compilation_failed=JS_IsException(result);
        if (!JS_IsException(result)) result = JS_EvalFunction(s->ctx, result);
    }
    if (s->host.debug_js&&(s->timed_out || uptime_ms() - source_start >= 250)) {
        char message[192];
        snprintf(message, sizeof message, "Script task profile: %lu bytes, compile %lu ms, execute %lu ms, layout %lu ms in %u reads%s",
                 (unsigned long)script->len, (unsigned long)(compiled_at - source_start),
                 (unsigned long)(uptime_ms() - compiled_at), (unsigned long)s->task_layout_ms, s->task_layout_flushes,compilation_failed?", compilation failed":"");
        log_text(s, 0, message);
    }
    if (JS_IsException(result)) { exception(s); script->failed = true; }
    else if (script->module && JS_IsObject(result)) script->evaluation = result;
    else JS_FreeValue(s->ctx, result);
    end_task(s);
    /* HTML's classic-script cleanup performs the stack-empty microtask
       checkpoint before execute-the-script restores currentScript. Promise
       continuations registered by this script must still see its element. */
    s->current_script = old_current; s->parser_write = old_write;
    /* Inline classic scripts have no resource load event. */
    if (!script->module && node_attr(script->node, "src")) { script_event(s, script->node, script->failed); script->notified = true; }
    js_free(s->ctx, script->source); script->source = NULL;
}
static void pending_free(struct web_js_state *s, struct js_pending *p) {
    if(p->kind==P_FONT && p->font)p->font->loading=false;
    JS_FreeValue(s->ctx, p->resolve); JS_FreeValue(s->ctx, p->reject);
    js_free(s->ctx, p->url);js_free(s->ctx,p->origin); js_free(s->ctx, p->response.body); js_free(s->ctx, p->response.headers_full); js_free(s->ctx,p->response.url_full);
    js_free(s->ctx, p); s->pending_count--;
}
void web_js_loaded(web_doc *d, uint64_t id, const struct web_response *response) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || !response) return;
    for (struct js_pending *p = s->pending; p; p = p->next) if (p->id == id && !p->done) {
        p->response = *response; p->response.body = NULL; p->response.headers_full = NULL; p->response.url_full = NULL;
        if (response->url_full) {
            p->response.url_full = js_strdup(s->ctx, web_response_url(response));
            if (!p->response.url_full) {
                JSValue error = JS_GetException(s->ctx); JS_FreeValue(s->ctx, error);
                pending_error(p, "Resource URL allocation failed"); return;
            }
        }
        const char *headers = web_response_headers(response);
        size_t hn = strlen(headers);
        if (hn > WEB_RESPONSE_HEADERS_MAX || hn == SIZE_MAX) { pending_error(p, "Resource response header length is not representable"); return; }
        if (hn < sizeof p->response.headers) memcpy(p->response.headers, headers, hn + 1);
        else {
            p->response.headers[0] = 0;
            p->response.headers_full = js_strndup(s->ctx, headers, hn);
            if (!p->response.headers_full) {
                JSValue e = JS_GetException(s->ctx); JS_FreeValue(s->ctx, e);
                pending_error(p, "Resource headers exceeded the JavaScript heap limit"); return;
            }
        }
        size_t limit = p->kind == P_SCRIPT ? JS_SOURCE_LIMIT : JS_BODY_LIMIT;
        if (response->body_len > limit || (response->body_len && !response->body))
            pending_error(p, "Resource body is invalid or its length is not representable");
        else {
            web_js_prepare_bytes(s->ctx,response->body_len+1);
            p->response.body = js_malloc(s->ctx, response->body_len + 1);
            if (!p->response.body) { JSValue e = JS_GetException(s->ctx); JS_FreeValue(s->ctx, e); pending_error(p, "Resource buffering exceeded the JavaScript heap limit"); }
            else { if (response->body_len) memcpy(p->response.body, response->body, response->body_len); p->response.body[response->body_len] = 0; p->done = true; }
        }
        return;
    }
}
static void fetch_body_free(JSRuntime *rt, void *opaque, void *bytes) {
    (void)opaque;
    js_free_rt(rt, bytes);
}
/* Diagnostics never print cookies, request bodies or URL query/fragment data.
   A page with hundreds of missing assets must not flood its console forever. */
static void diagnostic_url(const char *url, char *out, size_t capacity) {
    if (!capacity) return;
    if (!url) url = "";
    if (!strncasecmp(url,"data:",5) || !strncasecmp(url,"blob:",5)) {
        snprintf(out,capacity,"%s [local URL omitted]", !strncasecmp(url,"data:",5)?"data:":"blob:");
        return;
    }
    const char *end = url + strcspn(url,"?#"), *start = url;
    const char *scheme = strstr(url,"://");
    size_t used = 0;
    if (scheme && scheme < end) {
        const char *authority = scheme + 3, *tail = authority;
        while (tail < end && *tail != '/') tail++;
        const char *at = memchr(authority,'@',(size_t)(tail-authority));
        if (at) {
            used = MIN((size_t)(authority-url),capacity-1);
            memcpy(out,url,used); start=at+1;
        }
    }
    size_t n = MIN((size_t)(end-start),capacity-1-used);
    memcpy(out+used,start,n); used+=n; out[used]=0;
    if (*end && used+13 < capacity) snprintf(out+used,capacity-used," [URL suffix omitted]");
}
static void diagnostic_resource(struct web_js_state *s, struct js_pending *p, const char *reason) {
    if (s->resource_failures++ >= 64) {
        if (s->resource_failures == 65) log_text(s,1,"Resource diagnostics: further failures suppressed for this document (64 reported)");
        return;
    }
    char url[640], mime[128]="(not provided)", message[1152];
    diagnostic_url(web_response_url(&p->response)[0]?web_response_url(&p->response):p->url,url,sizeof url);
    const char *headers=web_response_headers(&p->response);
    for (const char *h=headers; *h;) {
        const char *end=strchr(h,'\n'); if(!end)end=h+strlen(h);
        if ((size_t)(end-h)>=13 && !strncasecmp(h,"content-type:",13)) {
            h+=13;while(h<end && (*h==' '||*h=='\t'))h++;
            while(end>h && (end[-1]=='\r'||end[-1]==' '||end[-1]=='\t'))end--;
            snprintf(mime,sizeof mime,"%.*s",(int)MIN((size_t)(end-h),sizeof mime-1),h);
            break;
        }
        h=*end?end+1:end;
    }
    static const char *const kinds[]={"script","fetch","stylesheet","image","worker","frame","font"};
    uint64_t started=p->deadline>=WEBNET_TIMEOUT_MS?p->deadline-WEBNET_TIMEOUT_MS:0;
    snprintf(message,sizeof message,"Resource #%lu %s: HTTP %d, %lu bytes, MIME %s, elapsed %lu ms, %s; %s",
        (unsigned long)p->id,kinds[p->kind],p->response.status,(unsigned long)p->response.body_len,mime,
        (unsigned long)(uptime_ms()-started),reason,url);
    log_text(s,p->kind==P_SCRIPT?2:1,message);
}
static int fetch_transport_flags(const char *headers) {
    /* The HTTP host writes this non-field line; server header fields cannot
       forge it. Keep security decisions out of mutable author JS built-ins. */
    const char *meta=headers?strstr(headers,"\r\nHTTP/Nocturne-Meta cors="):NULL;
    unsigned cors=0,redirected=0,opaque=0;
    if(!meta||sscanf(meta+2,"HTTP/Nocturne-Meta cors=%u redirected=%u opaque=%u",&cors,&redirected,&opaque)<2||cors>1||redirected>1||opaque>1)return -1;
    return (cors?NJW_LOADED_CORS:0)|(redirected?NJW_LOADED_REDIRECTED:0)|(opaque?NJW_LOADED_OPAQUE:0);
}
static bool process_pending(struct web_js_state *s) {
    struct js_pending **link = &s->pending;
    uint64_t now = uptime_ms();
    bool ran_task = false;
    while (*link) {
        struct js_pending *p = *link;
        if (!p->done && now >= p->deadline) { if (s->host.cancel) s->host.cancel(s->host.opaque, p->id); pending_error(p, "Resource request exceeded its deadline"); }
        if (!p->done) { link = &p->next; continue; }
        if (ran_task && !s->disabled && (p->kind == P_FETCH ||
            (p->kind == P_WORKER && !strncasecmp(p->url,"blob:",5)))) { link = &p->next; continue; }
        *link = p->next;
        bool ok = !p->response.error[0] && p->response.status >= 200 && p->response.status < 300;
        if (!ok && (p->kind != P_FETCH || p->response.error[0] || p->response.status >= 400))
            diagnostic_resource(s,p,p->aborted?"aborted":p->response.error[0]?p->response.error:"unsuccessful HTTP response");
        if(p->kind==P_FRAME)frame_response(s,p,ok);
        else if (p->kind == P_SCRIPT) {
            struct js_script *script = p->script;
            if (script) {
                bool local = !strncasecmp(p->url, "file://", 7);
                if (ok && script->module && !local && !javascript_mime(web_response_headers(&p->response))) {
                    diagnostic_resource(s,p,"module response is not a JavaScript MIME type"); ok = false;
                }
                script->ready = true; script->failed = !ok;
                if (ok) {
                    script->source = p->response.body; p->response.body = NULL; script->len = p->response.body_len;
                    if (web_response_url(&p->response)[0]) {
                        /* Module-map identity stays the requested URL. The
                           redirect URL is only the relative-import/meta base. */
                        char *final;
                        if (script->module) { begin_task(s); final = module_url(s, "moduleURL", web_response_url(&p->response), script->url); end_task(s); }
                        else final = js_strdup(s->ctx, web_response_url(&p->response));
                        if (final) {
                            char **slot = script->module ? &script->base : &script->url;
                            js_free(s->ctx, *slot); *slot = final;
                        } else { script->failed = true; exception(s); }
                    }
                }
                prepare_module_script(s, script);
            }
        } else if (p->kind == P_WORKER && !s->disabled) {
            if (!strncasecmp(p->url,"blob:",5) && !p->response.body && !p->response.error[0]) {
                char mime[128];
                ran_task = true; begin_named_task(s,"Worker object-URL source",p->worker);
                p->response.body = script_blob_source(s,p->url,true,&p->response.body_len,mime);
                if (!p->response.body) {
                    if (JS_HasException(s->ctx)) exception(s);
                    pending_error(p,"Worker object URL is unavailable or has an invalid JavaScript MIME type");
                } else snprintf(p->response.headers,sizeof p->response.headers,"Content-Type: %s\r\n",mime);
                end_task(s);
            }
            const char *final = web_response_url(&p->response)[0] ? web_response_url(&p->response) : p->url;
            if (!p->response.error[0] && strncasecmp(p->url,"blob:",5)) {
                bool valid = permitted_url(s,final,true);
                bool allocation_failed = false;
                if (p->worker_kind == 0) valid = valid &&
                    same_origin_urls(web_effective_url(s->doc), final, &allocation_failed);
                if (p->worker_kind != 2) valid = valid && p->response.status>=200 && p->response.status<300 &&
                    javascript_mime(web_response_headers(&p->response));
                if (!valid) pending_error(p, allocation_failed ? "Out of memory while checking Worker response origin" :
                    "Worker response rejected (URL, origin, HTTP status or JavaScript MIME type)");
            }
            web_worker_loaded(s->workers,p->worker,p->worker_request,p->response.status,final,
                web_response_headers(&p->response),p->response.body,p->response.body_len,p->response.error);
        } else if (p->kind == P_CSS) doc_css_loaded(s->doc, p->url, web_response_url(&p->response)[0] ? web_response_url(&p->response) : p->url, ok ? p->response.body : NULL, ok ? p->response.body_len : 0);
        else if(p->kind==P_FONT) {
            doc_font_loaded(s->doc,p->font,ok?p->response.body:NULL,ok?p->response.body_len:0);
            if(ok && p->font && p->font->failed)diagnostic_resource(s,p,"Font decode failed: malformed/unsupported SFNT or allocation failure; native CFF/WOFF/WOFF2 decoding is unavailable");
        }
        else if (p->kind == P_IMAGE) {
            web_image_loaded(s->doc, p->image, ok ? p->response.body : NULL, ok ? p->response.body_len : 0);
        } else if (!s->disabled && p->kind == P_FETCH) {
            ran_task = true;
            begin_task(s);
            JSValue value;
            bool success = !p->response.error[0];
            int transport=fetch_transport_flags(web_response_headers(&p->response));
            bool opaque=transport>=0&&(transport & NJW_LOADED_OPAQUE)!=0;
            if(success&&((opaque&&!p->no_cors)||(p->no_cors&&transport<0))){
                pending_error(p,"Native Fetch response is missing or contradicts its filtering policy");success=false;
            }
            if (success) {
                /* Transfer the already runtime-charged native snapshot. The
                   current QuickJS adopts external data only on success;
                   construction failure leaves pending_free as its owner. */
                JSValue bytes = opaque?JS_NULL:JS_NewArrayBuffer(s->ctx, (uint8_t *)p->response.body,
                    p->response.body_len, fetch_body_free, NULL, false);
                if (!opaque&&!JS_IsException(bytes)) p->response.body = NULL;
                /* Response decodes bytes only when its consumer asks for text.
                   The old fourth argument was unused and eagerly expanded
                   every binary media fragment into a large JS string. */
                bool redirected=transport>=0?(transport & NJW_LOADED_REDIRECTED)!=0:
                    web_response_url(&p->response)[0]&&strcmp(p->url,web_response_url(&p->response));
                JSValue type=opaque?JS_NewString(s->ctx,"opaque"):transport<0?JS_UNDEFINED:
                    JS_NewString(s->ctx,(transport & NJW_LOADED_CORS)?"cors":"basic");
                JSValue args[] = {JS_NewInt32(s->ctx,opaque?0:p->response.status),
                    JS_NewString(s->ctx,opaque?"":web_response_url(&p->response)[0]?web_response_url(&p->response):p->url),
                    JS_NewString(s->ctx,opaque?"":web_response_headers(&p->response)),JS_UNDEFINED,bytes,
                    JS_NewBool(s->ctx,!opaque&&redirected),JS_UNDEFINED,type};
                bool valid_args = true;
                for (int i = 0; i < 8; ++i) if (JS_IsException(args[i])) valid_args = false;
                value = valid_args ? JS_Call(s->ctx, s->response, JS_UNDEFINED, 8, args) : JS_EXCEPTION;
                for (int i = 0; i < 8; ++i) JS_FreeValue(s->ctx, args[i]);
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
    struct js_resource_event **link = &s->events, *previous = NULL;
    /* Import fetches must finish before a stylesheet completes. Unrelated
       script/image events are not stalled behind that stylesheet task. */
    bool css_busy = pending_kind(s,P_CSS) || web_pending_stylesheet(s->doc);
    while (*link && (*link)->stylesheet && css_busy) { previous = *link; link = &(*link)->next; }
    struct js_resource_event *e = *link;
    if (!e) return false;
    *link = e->next; if (s->last_event == e) s->last_event = previous;
    if (e->stylesheet && !doc_link_load_current(s->doc,e->node,e->generation)) {
        js_free(s->ctx,e); return false;
    }
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
        if (n->js_image != selected || (img && n->js_image_generation != n->image_generation)) {
            n->js_image = selected; n->js_image_generation = n->image_generation; n->js_image_notified = false;
        }
        if (!n->js_image_notified) {
            struct js_resource_event *event = script_event(s, n, !image || image->failed);
            if (event) { event->image = img; event->generation = n->image_generation; n->js_image_notified = true; }
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
        JSValue e=JS_GetException(s->ctx);JS_FreeValue(s->ctx,e);doc_css_loaded(s->doc,url,url,NULL,0);
        return;
    }
    send_request(s, p, WEB_RESOURCE_CSS, "GET", NULL, NULL, 0);
}
static void request_fonts(struct web_js_state *s) {
    if(!s->ctx)return;
    for(struct web_font_resource *f=s->doc->fonts;f;f=f->next) {
        if(!f->wanted || f->loading || f->done)continue;
        struct js_pending *p=pending_new(s,P_FONT,f->url);
        if(!p) {
            JSValue error=JS_GetException(s->ctx);JS_FreeValue(s->ctx,error);
            doc_font_loaded(s->doc,f,NULL,0);log_text(s,1,"Font request allocation failed");continue;
        }
        p->font=f;f->loading=true;p->credentials=1;
        /* Font loads use the real anonymous CORS fetch path, not image no-CORS
           or the module synchronous loader. The host retains origin/redirect
           checks and cancellation for this document generation. */
        send_request(s,p,WEB_RESOURCE_FETCH,"GET",NULL,NULL,0);
    }
}
static void request_images(struct web_js_state *s) {
    if (!s->ctx) return;
    /* Register candidates before dispatch, including a last timer/idle task's
       detached image when no later input or network event will wake the page. */
    if (s->doc->resources_dirty || s->doc->images_dirty) doc_rescan(s->doc);
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
static bool images_wanted(web_doc *d) {
    for (int i = 0; i < web_image_count(d); i++) if (web_image_wanted(d, i)) return true;
    return false;
}
static bool run_timer(struct web_js_state *s, uint64_t now) {
    struct js_timer *timer = NULL;
    unsigned timer_index = 0;
    for (unsigned i = 0; i < s->timer_capacity; i++) if (s->timers[i].id && s->timers[i].due <= now && (!timer || s->timers[i].due < timer->due)) { timer = &s->timers[i]; timer_index = i; }
    if (!timer) return false;
    uint32_t timer_id = timer->id;
    JSValue fn = JS_DupValue(s->ctx, timer->fn), args_array = JS_DupValue(s->ctx, timer->args);
    int kind = timer->kind;
    if (kind == 1) timer->due = now + timer->interval;
    else release_timer(s, timer_index);
    /* The callback/microtask checkpoint may grow the table, clear its interval
       or reuse a one-shot slot. Only local JSValue roots survive that boundary. */
    timer = NULL;
    uint64_t start = s->host.debug_js?uptime_ms():0;
    struct web_profile before = s->doc->profile;
    uint64_t initial_dom_rev = s->doc ? s->doc->dom_revision : 0;
    begin_named_task(s,kind==2?"animation frame":kind==1?"interval":"timeout",timer_id);
    uint64_t initial_layout_ms = s->task_layout_ms;
    unsigned initial_flushes = s->task_layout_flushes;
    JSValue frame_arg=JS_UNDEFINED,*args=NULL;int count=0;bool prepared=true,heap_args=false;
    if(kind==2){frame_arg=JS_NewFloat64(s->ctx,(double)relative_time(s,now));args=&frame_arg;count=1;}
    else{
        JSValue len=JS_GetPropertyStr(s->ctx,args_array,"length");uint32_t n=0;
        if(JS_IsException(len)||JS_ToUint32(s->ctx,&n,len)<0)prepared=false;
        JS_FreeValue(s->ctx,len);
        /* JS_Call's count is an int; byte arithmetic must not overflow. These
         * are representation limits, not an arbitrary callback-argument cap. */
        if(prepared&&((size_t)n>SIZE_MAX/sizeof *args||n>INT_MAX)){JS_ThrowOutOfMemory(s->ctx);prepared=false;}
        if(prepared&&n){args=js_malloc(s->ctx,(size_t)n*sizeof *args);if(!args)prepared=false;else heap_args=true;}
        for(uint32_t i=0;prepared&&i<n;i++){
            JSValue arg=JS_GetPropertyUint32(s->ctx,args_array,i);
            if(JS_IsException(arg)){prepared=false;break;}args[count++]=arg;
        }
    }
    uint64_t js_start = s->host.debug_js?uptime_ms():0;
    JSValue global=JS_UNDEFINED,result=JS_EXCEPTION;
    if(prepared&&task_context_active(s)){global=JS_GetGlobalObject(s->ctx);result=JS_Call(s->ctx,fn,global,count,args);}
    else if(prepared)result=JS_UNDEFINED;
    uint64_t js_ms = s->host.debug_js?uptime_ms() - js_start:0;
    if (JS_IsException(result)) exception(s); else JS_FreeValue(s->ctx, result);
    for (int i = 0; i < count; i++) JS_FreeValue(s->ctx, args[i]);
    if(heap_args)js_free(s->ctx,args);
    JS_FreeValue(s->ctx, global); JS_FreeValue(s->ctx, fn); JS_FreeValue(s->ctx, args_array);
    end_task(s);
    uint64_t elapsed = s->host.debug_js?uptime_ms() - start:0;
    if (s->host.debug_js&&(elapsed >= 250 || s->task_timed_out)) {
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
    uint64_t start = s->host.debug_js?uptime_ms():0;
    struct web_profile before = s->doc->profile;
    begin_named_task(s,"posted callback",0);
    JSValue result = JS_Call(s->ctx, fn, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(result)) exception(s); else JS_FreeValue(s->ctx, result);
    JS_FreeValue(s->ctx, fn); end_task(s);
    uint64_t elapsed = s->host.debug_js?uptime_ms() - start:0;
    if (s->host.debug_js&&(elapsed >= 250 || s->task_timed_out)) {
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
    if(s->doc->frame_parent){struct web_frame *f=web_frame_find(s->doc->frame_parent,s->doc->frame_element);sx=f?f->scroll_x:0;sy=f?f->scroll_y:0;}
    JSValue init = JS_NewObjectProto(s->ctx, JS_NULL);
    if (JS_IsException(init)) return init;
#define SET(k,v) do { JSValue value_ = (v); if (JS_IsException(value_) || \
    JS_SetPropertyStr(s->ctx, init, k, value_) < 0) goto failed; } while (0)
#define SET_B(k,v) SET(k, JS_NewBool(s->ctx, v))
#define SET_I(k,v) SET(k, JS_NewInt32(s->ctx, v))
    SET_B("bubbles", event->bubbles); SET_B("cancelable", event->cancelable); SET_B("isTrusted", !event->synthetic);
    SET_B("ctrlKey", event->ctrl); SET_B("shiftKey", event->shift); SET_B("altKey", event->alt);
    SET_I("clientX", event->x); SET_I("clientY", event->y); SET_I("pageX", event->x + sx); SET_I("pageY", event->y + sy);
    SET_I("button", event->button); SET_I("buttons", event->buttons);
    if(event->type && !strcmp(event->type,"click")) {
        bool mouse=!event->keyboard_activation && !event->synthetic;
        SET_I("pointerId",mouse?1:-1); SET("pointerType",JS_NewString(s->ctx,mouse?"mouse":""));
        SET_B("isPrimary",mouse); SET_I("width",1); SET_I("height",1);
        SET("pressure",JS_NewFloat64(s->ctx,0));
    }
    if(event->type && !strcmp(event->type,"wheel")) {
        SET("deltaX",JS_NewFloat64(s->ctx,event->delta_x));
        SET("deltaY",JS_NewFloat64(s->ctx,event->delta_y));
        SET("deltaZ",JS_NewFloat64(s->ctx,event->delta_z));
        SET("deltaMode",JS_NewUint32(s->ctx,event->delta_mode));
    }
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
static bool child_event_coordinates(web_doc *outer,web_doc *child,const struct web_event *event,struct web_event *local) {
    if(!outer || !child || !event || !child->frame_element || !child->frame_element->box)return false;
    int x,y,w,h,sx=0,sy=0;
    if(!web_node_rect(outer,child->frame_element,&x,&y,&w,&h))return false;
    if(outer->js && outer->js->host.scroll)outer->js->host.scroll(outer->js->host.opaque,&sx,&sy);
    if(outer->frame_parent){struct web_frame *f=web_frame_find(outer->frame_parent,outer->frame_element);sx=f?f->scroll_x:0;sy=f?f->scroll_y:0;}
    box_t *box=child->frame_element->box;
    *local=*event;local->x+=sx-x-(int)(box->p[3]+box->b[3]);local->y+=sy-y-(int)(box->p[0]+box->b[0]);
    if(local->related_target && local->related_target->owner!=child)local->related_target=NULL;
    return true;
}
bool web_js_dispatch(web_doc *d, node_t *target, const struct web_event *event) {
    if(target && target->owner && target->owner!=d && target->owner->frame_parent){
        struct web_event local;
        if(!target->owner->live || !child_event_coordinates(d,target->owner,event,&local))return true;
        return web_js_dispatch(target->owner,target,&local);
    }
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->disabled || !event || !event->type || JS_IsUndefined(s->dispatch)) return true;
    if(!event->synthetic && target && web_dialog_inert(d,target) &&
       (!strncmp(event->type,"key",3) || !strncmp(event->type,"mouse",5) || !strcmp(event->type,"click") || !strcmp(event->type,"dblclick"))) return false;
    if(!target && !event->synthetic && !strncmp(event->type,"key",3) && web_dialog_top(d)) target=web_dialog_top(d);
    if(!event->synthetic && (!strcmp(event->type,"click") ||
       (!strcmp(event->type,"keydown") && !event->ctrl && !event->alt && event->key &&
        (!strcmp(event->key,"Enter") || !strcmp(event->key," ")))))d->sandbox_activation_until=uptime_ms()+5000;
    begin_named_task(s,"DOM event",0);
    JSValue init = event_init(s, event);
    JSValue args[] = {wrap(s, target), JS_NewString(s->ctx, event->type), init};
    JSValue result = JS_EXCEPTION;
    if (!JS_IsException(args[0]) && !JS_IsException(args[1]) && !JS_IsException(args[2]))
        result = JS_Call(s->ctx, s->dispatch, JS_UNDEFINED, 3, args);
    bool allowed = true;
    if (JS_IsException(result)) { exception(s); allowed = false; } else { int r = JS_ToBool(s->ctx, result); allowed = r != 0; JS_FreeValue(s->ctx, result); }
    for (int i = 0; i < 3; ++i) JS_FreeValue(s->ctx, args[i]);
    if(allowed && !event->synthetic && !strcmp(event->type,"keydown") && !event->ctrl && !event->alt && web_dialog_top(d)) {
        if(event->key && !strcmp(event->key,"Escape")) {
            node_t *top=web_dialog_top(d); const char *by=node_attr(top,"closedby");
            if(!by || !str_ieq(by,"none")) {
                JSValue arg=wrap(s,top),r=JS_IsException(arg)?JS_EXCEPTION:custom_element_hook(s,"dialogRequestClose",1,&arg);
                if(JS_IsException(r))exception(s);else JS_FreeValue(s->ctx,r);
                JS_FreeValue(s->ctx,arg);
            }
            allowed=false;
        } else if(event->key && !strcmp(event->key,"Tab")) {
            web_js_focus_control(d,web_dialog_tab_target(d,event->shift)); allowed=false;
        }
    }
    end_task(s); return allowed;
}
bool web_js_reveal_hidden(web_doc *d,node_t *target) {
    if(!d || !target || target->owner!=d || !doc_node_connected(target))return false;
    struct web_js_state *s=d->js;
    if(!s || s->disabled) {
        for(node_t *n=target;n && doc_flat_parent(n);n=doc_flat_parent(n)) {
            const char *value=!n->foreign?node_attr(n,"hidden"):NULL;
            if(value && str_ieq(value,"until-found") && !doc_node_attr(d,n,"hidden",NULL))return false;
        }
        return true;
    }
    begin_named_task(s,"find ancestor revelation",0);
    JSContext *ctx=s->ctx;
    JSValue args[]={JS_NewArray(ctx),wrap(s,d->root)};
    uint32_t index=0;bool ok=!JS_IsException(args[0]) && !JS_IsException(args[1]);
    /* The real flat ancestors are snapshotted before beforematch can move,
       remove, adopt, or replace them. JS wrappers retain their DOM ownership. */
    for(node_t *n=target;ok && n && doc_flat_parent(n);n=doc_flat_parent(n)) {
        const char *value=!n->foreign?node_attr(n,"hidden"):NULL;
        if(!value || !str_ieq(value,"until-found"))continue;
        if(index==UINT32_MAX){ok=false;break;} /* JS Array index representation */
        JSValue node=wrap(s,n);
        if(JS_IsException(node))ok=false;
        else if(JS_SetPropertyUint32(ctx,args[0],index++,node)<0)ok=false;
    }
    JSValue result=ok?custom_element_hook(s,"revealHidden",2,args):JS_EXCEPTION;
    if(JS_IsException(result)){exception(s);ok=false;}
    else {ok=JS_ToBool(ctx,result)>0;JS_FreeValue(ctx,result);}
    JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);end_task(s);
    return ok && target->owner==d && doc_node_connected(target);
}
void *web_js_edit_begin(web_doc *d) {
    struct web_js_state *s=d?d->js:NULL;if(!task_context_active(s))return NULL;
    begin_named_task(s,"native physical edit",0);return s;
}
void web_js_edit_end(void *scope) {if(scope)end_task(scope);}
bool web_js_input_event(web_doc *d,node_t *target,const char *type,const char *input_type,const char *data,size_t length) {
    if(!d||!d->live||d->inert||!target||target->owner!=d||doc_node_root(target,true)!=d->root)return false;
    struct web_js_state *s=d->js;
    if(!s)return true;
    if(s->disabled)return strcmp(type,"beforeinput")||!s->host.navigation_pending||!s->host.navigation_pending(s->host.opaque);
    node_t *generation=d->root;begin_named_task(s,"native input event",0);
    JSValue args[]={wrap(s,target),JS_NewString(s->ctx,type),data?JS_NewStringLen(s->ctx,data,length):JS_NULL,JS_NewString(s->ctx,input_type)};
    JSValue result=JS_EXCEPTION;bool allowed=false;
    if(!JS_IsException(args[0])&&!JS_IsException(args[1])&&!JS_IsException(args[2])&&!JS_IsException(args[3]))
        result=custom_element_hook(s,"documentCommandEvent",4,args);
    if(JS_IsException(result))exception(s);else{allowed=JS_ToBool(s->ctx,result)>0;JS_FreeValue(s->ctx,result);}
    for(int i=0;i<4;i++)JS_FreeValue(s->ctx,args[i]);end_task(s);
    if(!task_context_active(s)||d->root!=generation||target->owner!=d||doc_node_root(target,true)!=d->root)allowed=false;
    if(!strcmp(type,"beforeinput")&&s->host.navigation_pending&&s->host.navigation_pending(s->host.opaque))allowed=false;
    return allowed;
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
static node_t *pointer_capture_in_document(web_doc *root,web_doc *d,const struct web_event *event) {
    struct web_js_state *s=d && d->live && !d->inert?d->js:NULL;
    if(!s || s->disabled || s->running || !s->pointer_capture_pending)return NULL;
    struct web_event local;
    if(d!=root && (!event || !child_event_coordinates(root,d,event,&local)))return NULL;
    begin_named_task(s,"native pointer capture",0);
    JSValue init=event?event_init(s,d==root?event:&local):JS_UNDEFINED;
    JSValue result=JS_IsException(init)?JS_EXCEPTION:custom_element_hook(s,"pointerCaptureTarget",1,&init);
    node_t *target=NULL;
    if(JS_IsException(result))exception(s);
    else {
        target=node_opaque(result);
        JS_FreeValue(s->ctx,result);
    }
    JS_FreeValue(s->ctx,init);end_task(s);
    if(!target || target->owner!=d || !web_pointer_click_target(root,target,target))target=NULL;
    return target;
}
web_node *web_pointer_capture_target(web_doc *d,const struct web_event *event) {
    if(!d || !d->live)return NULL;
    while(d->frame_parent)d=d->frame_parent;
    node_t *target=pointer_capture_in_document(d,d,event);
    if(target)return target;
    for(struct web_frame *f=d->frames;f;f=web_frame_walk_next(d,f,true)) {
        if(f->detached || !f->document)continue;
        target=pointer_capture_in_document(d,f->document,event);
        if(target)return target;
    }
    return NULL;
}
static void pointer_cancel_document(web_doc *d) {
    struct web_js_state *s=d && d->live && !d->inert?d->js:NULL;
    if(!s || s->disabled || s->running)return;
    begin_named_task(s,"native pointer cancellation",0);
    JSValue result=custom_element_hook(s,"pointerCancel",0,NULL);
    if(JS_IsException(result))exception(s);else JS_FreeValue(s->ctx,result);
    end_task(s);
}
web_node *web_pointer_capture_click_target(web_doc *d,node_t *released_target) {
    web_doc *owner=released_target?released_target->owner:NULL;
    struct web_js_state *s=owner && owner->live && !owner->inert?owner->js:NULL;
    if(!d || !s || s->disabled || s->running)return NULL;
    begin_named_task(s,"native captured click",0);
    JSValue result=custom_element_hook(s,"pointerClickTarget",0,NULL);
    node_t *target=NULL;
    if(JS_IsException(result))exception(s);
    else{target=node_opaque(result);JS_FreeValue(s->ctx,result);}
    end_task(s);
    return target && target->owner==owner?web_pointer_click_target(d,target,target):NULL;
}
void web_pointer_cancel(web_doc *d) {
    if(!d || !d->live)return;
    while(d->frame_parent)d=d->frame_parent;
    pointer_cancel_document(d);
    for(struct web_frame *f=d->frames;f;f=web_frame_walk_next(d,f,true))
        if(!f->detached && f->document)pointer_cancel_document(f->document);
}
void web_hover(web_doc *d, web_node *target, const struct web_event *event) {
    if(!event)return;
    doc_hover_update(d,target);
    if(target && target->owner && target->owner!=d && target->owner->frame_parent){
        struct web_event local;
        if(target->owner->live && child_event_coordinates(d,target->owner,event,&local))web_hover(target->owner,target,&local);
        return;
    }
    struct web_js_state *s = d && d->live ? d->js : NULL;
    if (!s || s->disabled || s->running || !event) return;
    if(target && web_dialog_inert(d,target))target=NULL;
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
    end_task(s);s->observer_due=uptime_ms()+16;s->observer_turn=false;
    return true;
}
static bool run_document_event(struct web_js_state *s) {
    web_doc *d = s->doc;
    if (s->parsing_done && !unfinished_deferred(s) && !s->domcontent_sent) {
        timing_record(s, JS_DOM_CONTENT_START, uptime_ms());
        s->domcontent_sent = true;
        struct web_event e = {.type="DOMContentLoaded", .bubbles=true};
        web_js_dispatch(d, d->root, &e);
        timing_record(s, JS_DOM_CONTENT_END, uptime_ms()); return true;
    }
    if (s->domcontent_sent && !s->load_sent && !unfinished_scripts(s) && !styles_busy(s) &&
        !pending_kind(s, P_IMAGE) && !pending_kind(s, P_SCRIPT) && !pending_kind(s,P_FRAME) && !web_frames_busy(d)) {
        for (int i = 0; i < web_image_count(d); i++) if (web_image_wanted(d, i)) return false;
        timing_record(s, JS_DOM_COMPLETE, uptime_ms());
        s->load_sent = true; struct web_event e = {.type="load"};
        node_t *autofocus = web_autofocus_candidate(d); if (autofocus) web_js_focus_control(d, autofocus);
        timing_record(s, JS_LOAD_START, uptime_ms());
        if(d->frame_parent && d->body && node_attr(d->body,"onload"))web_js_dispatch(d,d->body,&e);
        web_js_dispatch(d, NULL, &e);
        timing_record(s, JS_LOAD_END, uptime_ms()); return true;
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
static uint64_t idle_timeout_due(struct web_js_state *s) {
    uint64_t due = UINT64_MAX;
    for (struct js_idle_task *p = s->idle_runnable; p; p = p->next) if (p->due < due) due = p->due;
    for (struct js_idle_task *p = s->idle_pending; p; p = p->next) if (p->due < due) due = p->due;
    return due;
}
static bool invoke_idle(struct web_js_state *s, uint32_t id, uint64_t end, bool timed_out) {
    struct js_idle_task *p = take_idle_id(s, id); if (!p) return false;
    /* Unpublish/free the queue node before script, including its microtasks.
       Cancellation/re-registration/GC cannot invalidate any retained pointer. */
    JSValue fn = p->fn; js_free(s->ctx, p);
    begin_named_task(s, timed_out ? "idle callback timeout" : "idle callback", id);
    JSValue args[] = {JS_NewFloat64(s->ctx, (double)relative_time(s, end)), JS_NewBool(s->ctx, timed_out)};
    JSValue global = JS_GetGlobalObject(s->ctx), result = JS_Call(s->ctx, fn, global, 2, args);
    if (JS_IsException(result)) exception(s); else JS_FreeValue(s->ctx, result);
    JS_FreeValue(s->ctx, args[0]); JS_FreeValue(s->ctx, args[1]);
    JS_FreeValue(s->ctx, global); JS_FreeValue(s->ctx, fn); end_task(s);
    return true;
}
static bool run_idle_timeout(struct web_js_state *s, uint64_t now) {
    uint64_t due = UINT64_MAX; uint32_t id = 0;
    /* Earlier timeout first, with FIFO ties across older runnable and pending
       lists; a timeout and ordinary idle invocation remove the same entry. */
    for (struct js_idle_task *p = s->idle_runnable; p; p = p->next)
        if (p->due <= now && p->due < due) { due = p->due; id = p->id; }
    for (struct js_idle_task *p = s->idle_pending; p; p = p->next)
        if (p->due <= now && p->due < due) { due = p->due; id = p->id; }
    if (!id) return false;
    s->idle_period_stopped = true;
    return invoke_idle(s, id, now, true);
}
static bool idle_eligible(struct web_js_state *s, uint64_t now) {
    web_doc *d = s->doc, *family = d->dom_family ? d->dom_family : d;
    struct js_script *script;
    if (!s->parsing_done || d->dirty || d->resources_dirty || d->need_style || d->need_boxes || !d->layout_valid ||
        s->events || s->posted || s->media_width != d->width || s->media_height != d->height ||
        family->shadow_slots_pending || family->details_toggle_first || JS_IsJobPending(s->rt) ||
        web_worker_runnable(s->workers) || ready_script(s, &script)) return false;
    if (observers_dirty(s) && s->observer_due <= now) return false;
    for (struct js_pending *p = s->pending; p; p = p->next) if (p->done || p->deadline <= now) return false;
    for (unsigned i = 0; i < s->timer_capacity; i++) if (s->timers[i].id && s->timers[i].due <= now) return false;
    for (struct js_image_decode *p = s->image_decodes; p; p = p->next) if (image_decode_ready(s, p)) return false;
    return true;
}
static uint64_t idle_period_deadline(struct web_js_state *s, uint64_t now) {
    uint64_t end = now + JS_IDLE_PERIOD_MS;
    /* Animation/observer/media and already scheduled resource/timer wakeups
       shorten a quiet 50ms period to their actual native frame/task budget. */
    for (unsigned i = 0; i < s->timer_capacity; i++) if (s->timers[i].id && s->timers[i].due < end) end = s->timers[i].due;
    for (struct js_pending *p = s->pending; p; p = p->next) if (p->deadline < end) end = p->deadline;
    if (observers_dirty(s) && s->observer_due < end) end = s->observer_due;
    int64_t media = web_avmedia_deadline(s->doc, now);
    if (media >= 0 && (uint64_t)media < end) end = (uint64_t)media;
    int64_t worker = web_worker_deadline(s->workers,now);
    if (worker >= 0 && (uint64_t)worker < end) end = (uint64_t)worker;
    return end;
}
static bool run_idle_period(struct web_js_state *s, uint64_t now) {
    if (!s->idle_count) return false;
    if (!idle_eligible(s, now)) { s->idle_period_stopped = true; return false; }
    if (now >= s->idle_period_end) {
        uint64_t end = idle_period_deadline(s, now); if (end <= now) return false;
        if (s->idle_pending) {
            if (s->last_idle_runnable) s->last_idle_runnable->next = s->idle_pending; else s->idle_runnable = s->idle_pending;
            s->last_idle_runnable = s->last_idle_pending;
            s->idle_pending = s->last_idle_pending = NULL;
        }
        s->idle_period_end = end; s->idle_period_callbacks = 0; s->idle_period_stopped = false;
    }
    if (s->idle_period_stopped || !s->idle_runnable || s->idle_period_callbacks >= JS_IDLE_PERIOD_CALLBACKS) return false;
    /* One callback per tick; new registrations remain pending until a later
       period, even if the callback/microtask checkpoint cancels the whole run list. */
    uint32_t id = s->idle_runnable->id; s->idle_period_callbacks++;
    return invoke_idle(s, id, s->idle_period_end, false);
}
void web_js_tick(web_doc *d, uint64_t now) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s || s->running) return;
    web_worker_pump(s->workers,now);
    /* Expired idle requests are ordinary watchdog-bounded tasks, not idle
       periods. Alternate with other task sources so even continuous fetch,
       scripts or posted/timer work cannot starve an author-supplied timeout. */
    if (!s->disabled && s->idle_timeout_turn && run_idle_timeout(s, uptime_ms())) {
        s->idle_timeout_turn = false;
        /* Preserve the usual post-watchdog script/lifecycle cleanup even when
           this fairness turn bypasses the other task sources. */
        if (s->disabled) for (struct js_script *p = s->scripts; p; p = p->next) { p->executed = true; JS_FreeValue(s->ctx, p->evaluation); p->evaluation = JS_UNDEFINED; }
        request_styles(s); request_fonts(s); request_images(s); return;
    }
    s->idle_timeout_turn = true;
    bool ran = false;
    /* Messages are ordinary JS tasks, with the same watchdog/checkpoint as
       events and timers. Alternate ready workers with other sources. */
    if (!s->disabled && s->worker_turn && web_worker_runnable(s->workers)) {
        begin_named_task(s,"Worker message",0); ran = web_worker_run_one(s->workers);
        if (JS_HasException(s->ctx)) exception(s);
        end_task(s); s->worker_turn = false;
    } else { s->worker_turn = true; ran = process_pending(s); }
    request_styles(s);
    if (s->blocker && (s->blocker->executed || s->disabled)) s->blocker = NULL;
    /* A tick executes at most one JavaScript task plus its microtask checkpoint.
       Parsing and native completions may progress without invoking callbacks. */
    if (!s->parsing_done && !s->blocker && d->parser) {
        node_t *created_before = d->owned_nodes;
        for (int step = 0; step < 32 && !s->blocker; step++) {
            node_t *node = NULL; int result = html_resume(d->parser, &node);
            /* Script/EOF boundaries can import a byte-identical forest. Only
               real parser changes publish resources; errors stay conservative. */
            if (result < 0 || html_import_changed(d->parser)) d->dirty = d->resources_dirty = true;
            if(result==2){s->document_waiting=true;break;} /* document.open stream awaits another write or close */
            if (result <= 0) {
                if (result < 0) log_text(s, 2, "HTML parsing stopped at the document's memory limit");
                html_finish(d->parser); d->parser = NULL;
                timing_record(s, JS_DOM_INTERACTIVE, uptime_ms());
                s->parsing_done = true; break;
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
    if (!s->disabled && !ran && (family->shadow_slots_pending || family->shadow_dirty_first)) {
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
        /* A continuously dirty rendering observer cannot preempt every ready
           timer/rAF/posted task. Give another task source the next turn,
           retaining observer delivery when no ordinary task is runnable. */
        else if (s->observer_turn && run_observers(s, now)) ran = true;
        else if (s->posted_turn && run_posted_task(s)) { s->posted_turn = false; s->observer_turn = true; ran = true; }
        else if (run_timer(s, now)) { s->posted_turn = true; s->observer_turn = true; ran = true; }
        else if (run_posted_task(s)) { s->posted_turn = false; s->observer_turn = true; ran = true; }
        else if (run_observers(s, now)) ran = true;
        else if (JS_IsJobPending(s->rt)||s->rejections||s->rejection_oom) { begin_task(s); end_task(s); ran = true; }
        else if (web_worker_runnable(s->workers)) {
            begin_named_task(s,"Worker message",0); ran = web_worker_run_one(s->workers);
            if (JS_HasException(s->ctx)) exception(s);
            end_task(s); s->worker_turn = false;
        }
    }
    if (s->disabled) for (struct js_script *p = s->scripts; p; p = p->next) { p->executed = true; JS_FreeValue(s->ctx, p->evaluation); p->evaluation = JS_UNDEFINED; }
    request_styles(s); request_fonts(s); request_images(s);
    if (!ran) ran = run_document_event(s);
    if (ran) s->idle_period_stopped = true;
    else if (!s->disabled) {
        uint64_t idle_now = uptime_ms();
        if (!run_idle_timeout(s, idle_now)) run_idle_period(s, idle_now);
    }
}
int64_t web_js_deadline(web_doc *d) {
    struct web_js_state *s = d ? d->js : NULL; if (!s) return -1;
    uint64_t deadline = UINT64_MAX, now = uptime_ms();
    int64_t worker_deadline = web_worker_deadline(s->workers,now);
    if (worker_deadline >= 0) deadline = (uint64_t)worker_deadline;
    if (!s->parsing_done && !s->document_waiting && (!s->blocker || s->blocker->executed || s->disabled)) return (int64_t)now;
    for (struct js_pending *p = s->pending; p; p = p->next) {
        if (p->done) return (int64_t)now;
        if (p->deadline < deadline) deadline = p->deadline;
    }
    /* Dirty registration and wanted, unqueued images are work, even after the
       final idle/console task. Do not spin while the bounded queue is full. */
    if (d->resources_dirty || d->images_dirty) return (int64_t)now;
    for(struct web_font_resource *f=d->fonts;f;f=f->next)
        if(f->wanted && !f->done && !f->loading)return (int64_t)now;
    if (s->pending_count < 8) for (int i = 0; i < web_image_count(d); i++) {
        if (!web_image_wanted(d, i)) continue;
        bool pending = false;
        for (struct js_pending *p = s->pending; p; p = p->next)
            if (p->kind == P_IMAGE && p->image == i) { pending = true; break; }
        if (!pending) return (int64_t)now;
    }
    if (!s->disabled) {
        if ((d->dom_family ? d->dom_family : d)->shadow_slots_pending) return (int64_t)now;
        if ((d->dom_family ? d->dom_family : d)->details_toggle_first) return (int64_t)now;
        if (s->media_width != d->width || s->media_height != d->height) return (int64_t)now;
        if (s->events || s->posted || s->rejections || s->rejection_oom) return (int64_t)now;
        for (struct js_image_decode *p = s->image_decodes; p; p = p->next) if (image_decode_ready(s, p)) return (int64_t)now;
        if (JS_IsJobPending(s->rt)) return (int64_t)now;
        if (observers_dirty(s) && s->observer_due < deadline) deadline=s->observer_due;
        struct js_script *ready; if (ready_script(s, &ready)) return (int64_t)now;
        for (unsigned i = 0; i < s->timer_capacity; i++) if (s->timers[i].id && s->timers[i].due < deadline) deadline = s->timers[i].due;
        for (struct js_script *p = s->scripts; p; p = p->next) if (p->module && p->executed && !p->notified && script_finished(s, p)) return (int64_t)now;
        uint64_t idle_due = idle_timeout_due(s); if (idle_due < deadline) deadline = idle_due;
        if (s->idle_count && idle_eligible(s, now)) {
            uint64_t idle_wake = now;
            if (now < s->idle_period_end && (s->idle_period_stopped || !s->idle_runnable || s->idle_period_callbacks >= JS_IDLE_PERIOD_CALLBACKS))
                idle_wake = s->idle_period_end;
            if (idle_wake < deadline) deadline = idle_wake;
        }
    }
    if (s->parsing_done && ((!s->domcontent_sent && !unfinished_deferred(s)) || (s->domcontent_sent && !s->load_sent && !unfinished_scripts(s) && !styles_busy(s) && !images_wanted(d) && !pending_kind(s, P_IMAGE) && !pending_kind(s, P_SCRIPT) && !pending_kind(s,P_FRAME) && !web_frames_busy(d)))) return (int64_t)now;
    return deadline == UINT64_MAX ? -1 : (int64_t)deadline;
}
bool web_js_running(web_doc *d) { return d && d->js && d->js->running; }
bool web_console_eval(web_doc *d,const char *source,size_t length) {
    struct web_js_state *s=d?d->js:NULL;
    if(!s||!source||!length||length>JS_SOURCE_LIMIT||s->running||s->starting||s->disabled)return false;
    begin_named_task(s,"developer console",0);
    JSValue result=JS_Eval(s->ctx,source,length,"nocturne:developer-console",JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(result);
    if(!ok)exception(s);
    else if(!JS_IsUndefined(result)){
        const char *text=JS_ToCString(s->ctx,result);
        if(text){log_text(s,0,text);JS_FreeCString(s->ctx,text);}else {exception(s);ok=false;}
    }
    JS_FreeValue(s->ctx,result);end_task(s);d->dirty=true;return ok;
}
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
void web_visibility_event(web_doc *d) {
    if(!d || !d->js || d->js->running)return;
    struct web_event e={.type="visibilitychange",.bubbles=true};
    web_js_dispatch(d,d->root,&e);
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
                uint8_t *copy = malloc(size);
                if (copy) { memcpy(copy, bytes, size); bindings_bytecode = copy; bindings_bytecode_len = size; }
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
    d->profile_enabled=s->host.debug_js;
    s->runtime_owner=d->frame_parent && d->frame_parent->js?d->frame_parent->js->runtime_owner:s;
    uint32_t budget = s->host.js_task_budget_ms;
    s->task_budget_ms = budget ? budget : WEB_JS_TASK_DEFAULT_MS;
    s->media_width = d->width; s->media_height = d->height;
    uint64_t initialized = uptime_ms();
    const struct web_navigation_timing *navigation = &s->host.navigation_timing;
    /* Embedders with no navigation observations explicitly use document init.
       Reject future epochs instead of underflowing performance.now()/rAF. */
    bool seeded = navigation->valid && navigation->navigation_start_ms <= initialized;
    s->now = seeded ? navigation->navigation_start_ms : initialized;
    timing_record(s, JS_NAV_START, s->now);
    if (seeded && navigation->fetch_valid && navigation->fetch_start_ms >= s->now && navigation->fetch_start_ms <= initialized)
        timing_record(s, JS_FETCH_START, navigation->fetch_start_ms);
    if (seeded && navigation->response_end_valid && navigation->response_end_ms >= s->now && navigation->response_end_ms <= initialized)
        timing_record(s, JS_RESPONSE_END, navigation->response_end_ms);
    timing_record(s, JS_DOM_LOADING, initialized);
    s->observer_turn = true;
    s->hooks = s->dispatch = s->response = s->reject = JS_UNDEFINED;
    for (int i = 0; i < NP_COUNT; i++) s->node_protos[i] = JS_UNDEFINED;
    /* Grow on demand through the real OS allocator. An invented page quota
       must not turn available memory into a JavaScript compatibility failure. */
    s->heap_limit=SIZE_MAX;
    s->rt = s->runtime_owner==s?JS_NewRuntime2(&allocator, &s->allocation):s->runtime_owner->rt;
    if (!s->rt) goto failed;
    if(s->runtime_owner==s){
        JS_SetMemoryLimit(s->rt, s->heap_limit); JS_SetMaxStackSize(s->rt, JS_STACK_LIMIT);
        JS_SetInterruptHandler(s->rt, interrupt, s); JS_SetCanBlock(s->rt, false);
        JS_SetModuleLoaderFunc(s->rt, normalize_module, load_module, s);
        JS_SetHostPromiseRejectionTracker(s->rt, promise_rejection, s);
        JS_SetHostJobFilter(s->rt, browser_job_allowed, NULL);
    }else s->heap_limit=s->runtime_owner->heap_limit;
    s->ctx = JS_NewContext(s->rt); if (!s->ctx) goto failed;
    JS_SetContextOpaque(s->ctx, s);
    if (!node_class) JS_NewClassID(&node_class);
    JSClassDef class_def = {.class_name="NocturneDOMNode",.finalizer=node_finalizer,.gc_mark=node_gc_mark,.exotic=&node_exotic};
    if (s->runtime_owner==s && JS_NewClass(s->rt, node_class, &class_def) < 0) goto failed;
    JSValue api = JS_NewObject(s->ctx);
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("dom", 2, native_dom), JS_CFUNC_DEF("log", 1, native_log),
        JS_CFUNC_DEF("domHooks",2,native_dom_hooks),
        JS_CFUNC_DEF("sandboxFlags",1,native_sandbox_flags),
        JS_CFUNC_DEF("svgGeometry",2,native_svg_geometry),
        JS_CFUNC_DEF("mutationObserve",4,native_mutation_control),
        JS_CFUNC_DEF("mutationTake",0,native_mutation_take),
        JS_CFUNC_DEF("frame", 3, native_frame),JS_CFUNC_DEF("documentStream",1,native_document_stream),
        JS_CFUNC_DEF("now", 0, native_now), JS_CFUNC_DEF("timing", 1, native_timing), JS_CFUNC_DEF("url", 0, native_url), JS_CFUNC_DEF("origin", 0, native_origin),
        JS_CFUNC_DEF("screen", 0, native_screen),
        JS_CFUNC_DEF("windowState",0,native_window_state),
        JS_CFUNC_DEF("cookieEnabled",0,native_cookie_enabled),
        JS_CFUNC_DEF("pack", 1, native_pack), JS_CFUNC_DEF("unpack", 1, native_unpack),
        JS_CFUNC_DEF("classID", 1, native_class_id), JS_CFUNC_DEF("detach", 1, native_detach),
        JS_CFUNC_DEF("history", 4, native_history),JS_CFUNC_DEF("historyCreate",1,native_history_create),JS_CFUNC_DEF("historyCall",3,native_history_call), JS_CFUNC_DEF("cookie", 1, native_cookie), JS_CFUNC_DEF("scroll", 2, native_scroll),
        JS_CFUNC_DEF("media", 1, native_media),
        JS_CFUNC_DEF("canvas", 2, native_canvas),
        JS_CFUNC_DEF("imageBitmap", 5, native_image_bitmap),
        JS_CFUNC_DEF("avmedia", 4, native_avmedia),
        JS_CFUNC_DEF("observers", 1, native_observers),
        JS_CFUNC_DEF("pointerCapturePending", 1, native_pointer_capture_pending),
        JS_CFUNC_DEF("resolve", 1, native_resolve), JS_CFUNC_DEF("ready", 0, native_ready), JS_CFUNC_DEF("current", 0, native_current),
        JS_CFUNC_DEF("write", 1, native_write), JS_CFUNC_DEF("encode", 1, native_encode), JS_CFUNC_DEF("navigate", 1, native_navigate),
        JS_CFUNC_DEF("inline", 1, native_inline),
        JS_CFUNC_DEF("click", 1, native_click), JS_CFUNC_DEF("timer", 4, native_timer), JS_CFUNC_DEF("clear", 1, native_clear),
        JS_CFUNC_DEF("microtask", 1, native_microtask),
        JS_CFUNC_DEF("postTask", 1, native_post_task), JS_CFUNC_DEF("cancelPost", 1, native_cancel_post),
        JS_CFUNC_DEF("broadcast", 3, native_broadcast),
        JS_CFUNC_DEF("idle", 2, native_idle), JS_CFUNC_DEF("cancelIdle", 1, native_cancel_idle),
        JS_CFUNC_DEF("worker", 3, native_worker),
        JS_CFUNC_DEF("cssSupports", 2, native_css_supports),
        JS_CFUNC_DEF("storage", 5, native_storage),
        JS_CFUNC_DEF("fetch", 4, native_fetch), JS_CFUNC_DEF("cancel", 1, native_cancel)
    };
    JS_SetPropertyFunctionList(s->ctx, api, functions, sizeof functions / sizeof *functions);
    JSValue operations=JS_NewObject(s->ctx);
    for(int opcode=0;opcode<DOM_OPCODE_COUNT;opcode++)
        JS_SetPropertyStr(s->ctx,operations,dom_opcode_names[opcode],
            JS_NewCFunctionMagic(s->ctx,native_dom_magic,dom_opcode_names[opcode],1,JS_CFUNC_generic_magic,opcode));
    JS_SetPropertyStr(s->ctx,api,"domOperations",operations);
    JS_SetPropertyStr(s->ctx,api,"noCorsFetch",JS_TRUE);
    web_js_navigator_init(s->ctx,api);
    web_js_crypto_init(s->ctx, api);
    web_js_encoding_init(s->ctx, api);
    web_js_collator_init(s->ctx, api);
    JS_SetPropertyStr(s->ctx, api, "debugCanvasProbe", JS_NewBool(s->ctx,s->host.debug_js));
    JS_SetPropertyStr(s->ctx, api, "document", wrap(s, d->root));
    JSValue global = JS_GetGlobalObject(s->ctx);
    JS_SetPropertyStr(s->ctx, global, "__nocturne_host", api); JS_FreeValue(s->ctx, global);
    /* There is no page script or page callback during bootstrap. Charging the
       trusted library parser/CLDR setup to a future page task disabled all JS
       under QEMU. Keep startup bounded separately from the page task watchdog. */
    s->starting = true; begin_task(s);
    if(!s->runtime_previous)s->task_deadline = uptime_ms() + JS_STARTUP_MS;
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
    node_cache_hold(s);
    bool prototype_failed = false;
    for (struct js_node_ref *r = s->nodes; r; r = r->next)
        if (!JS_IsUndefined(r->hold) && JS_SetPrototype(s->ctx, r->hold, node_prototype(s, r->node)) < 0) {
            prototype_failed = true; break;
        }
    node_cache_release(s);
    if (prototype_failed) { exception(s); end_task(s); goto failed; }
    end_task(s); s->starting = false;
    if(d->sandbox_flags&SB_SCRIPTS)s->disabled=true;
    char limits[192];
    snprintf(limits,sizeof limits,"JavaScript runtime: OS-allocated heap (no page quota), dynamically allocated timers; stack guard %u KiB, task budget %u ms",
             JS_STACK_LIMIT/1024u,s->task_budget_ms);
    log_text(s,0,limits);return;
failed:
    s->starting = false;
    log_text(s, 2, "Could not initialize JavaScript; the document remains usable without scripting");
    s->disabled = true;
}
void web_js_free(web_doc *d) {
    struct web_js_state *s = d ? d->js : NULL; if (!s) return;
    d->js = NULL;
    if (s->ctx) {
        mutation_free(s);
        history_forget(s);
        broadcast_free(s);
        while(s->frame_proxies){struct js_frame_proxy *p=s->frame_proxies;s->frame_proxies=p->next;JS_FreeValue(s->ctx,p->proxy);js_free(s->ctx,p);}
        web_worker_free(s->workers); s->workers = NULL;
        release_document_tasks(s);
        while (s->image_decodes) {
            struct js_image_decode *p = s->image_decodes; s->image_decodes = p->next;
            JS_FreeValue(s->ctx, p->resolve); JS_FreeValue(s->ctx, p->reject); js_free(s->ctx, p);
        }
        while (s->events) { struct js_resource_event *e = s->events; s->events = e->next; js_free(s->ctx, e); }
        while (s->pending) { struct js_pending *p = s->pending; s->pending = p->next; release_pending_request(s, p); pending_free(s, p); }
        while (s->scripts) { struct js_script *p = s->scripts; s->scripts = p->next; JS_FreeValue(s->ctx, p->evaluation); js_free(s->ctx, p->url); js_free(s->ctx, p->base); js_free(s->ctx, p->source); js_free(s->ctx, p); }
        while (s->modules) { struct js_module *m = s->modules; s->modules = m->next; JS_FreeValue(s->ctx, m->compiled); JS_FreeValue(s->ctx, m->error); js_free(s->ctx, m->url); js_free(s->ctx, m->base); js_free(s->ctx, m->source); js_free(s->ctx, m); }
        while(s->rejections){struct js_rejection *p=s->rejections;s->rejections=p->next;
            JS_FreeValue(s->ctx,p->promise);JS_FreeValue(s->ctx,p->reason);js_free(s->ctx,p);}
        s->last_rejection=NULL;
        node_cache_free(s);
        JS_FreeValue(s->ctx, s->hooks); JS_FreeValue(s->ctx, s->dispatch); JS_FreeValue(s->ctx, s->response); JS_FreeValue(s->ctx, s->reject);
        for (int i = 0; i < NP_COUNT; i++) JS_FreeValue(s->ctx, s->node_protos[i]);
        JS_SetContextOpaque(s->ctx,NULL); /* queued host jobs must reject retired realms */
        JS_FreeContext(s->ctx);
    }
    if (s->rt && s->runtime_owner==s) JS_FreeRuntime(s->rt);
    free(s);
}
