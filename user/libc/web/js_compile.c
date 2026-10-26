/* Classic scripts compiled away from the document's thread (js_compile.h). */
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <bearssl_hash.h>
#include "js_compile.h"
#include "js_memory.h"

/* Each compiler holds a whole script's parse state, several times its source
   at the peak; two let a page's large bundles compile side by side. */
#define JS_COMPILERS 2
/* Above the runtime's stack guard, which QuickJS measures from the top. */
#define JS_COMPILER_STACK (2u * JS_STACK_LIMIT)
/* QuickJS compiles about 20 MB a second: below this, a script compiles in
   about the time handing it to a compiler and waking its document takes. */
#define JS_COMPILE_MIN_BYTES (64u << 10)
/* Kept bytecode only saves compiling again, so the least recently used goes
   first. A large app's bundles write tens of MB, their sources included. */
#define JS_COMPILE_MEMORY (192u << 20)
#define JS_COMPILE_TOKEN (UINT64_C(0x43) << 56)

/* Bytecode a compiler wrote, shared by the jobs that read it and by the
   memory of compiled scripts. Freed when the last of them lets go. */
struct js_bytecode {
    struct js_bytecode *next; /* in the memory, most recently used first */
    uint8_t key[32];          /* SHA-256 of origin, URL and source */
    uint8_t *bytes;
    size_t size;
    unsigned refs;            /* the memory counts as one while it keeps it */
};
struct js_compile {
    struct js_compile *next; /* in the queue */
    char *source, *url, *origin;
    size_t len;
    struct js_bytecode *code; /* NULL: compiling failed */
    struct js_compile_stats stats;
    volatile uint32_t done;
    unsigned refs;            /* the document's and the compiler's */
    bool dropped, watched;
    n_handle queue;
    uint64_t token;
};

/* Guards the queue, the memory and every reference count. */
static mtx_t lock;
static cnd_t queued;
static struct js_compile *queue_head, **queue_tail = &queue_head;
static struct js_bytecode *memory;
static size_t memory_bytes;
static unsigned compilers;
static once_flag started = ONCE_FLAG_INIT;
static uint64_t tokens;

static void bytecode_release(struct js_bytecode *code) {
    if (!code || --code->refs) return;
    free(code->bytes); free(code);
}
static void job_release(struct js_compile *job) {
    if (--job->refs) return;
    bytecode_release(job->code);
    free(job->source); free(job->url); free(job->origin); free(job);
}
static void key_of(const struct js_compile *job, uint8_t key[32]) {
    br_sha256_context hash;
    br_sha256_init(&hash);
    /* Both names end at their NUL, so no two pairs hash the same bytes. */
    br_sha256_update(&hash, job->origin, strlen(job->origin) + 1);
    br_sha256_update(&hash, job->url, strlen(job->url) + 1);
    br_sha256_update(&hash, job->source, job->len);
    br_sha256_out(&hash, key);
}
/* lock held. The kept bytecode for key, now the most recently used. */
static struct js_bytecode *recall(const uint8_t key[32]) {
    for (struct js_bytecode **link = &memory; *link; link = &(*link)->next) {
        struct js_bytecode *code = *link;
        if (memcmp(code->key, key, sizeof code->key)) continue;
        *link = code->next; code->next = memory; memory = code;
        code->refs++;
        return code;
    }
    return NULL;
}
/* lock held. */
static void keep(struct js_bytecode *code) {
    if (code->size > JS_COMPILE_MEMORY) return;
    code->refs++; code->next = memory; memory = code; memory_bytes += code->size;
    while (memory_bytes > JS_COMPILE_MEMORY) {
        struct js_bytecode **link = &memory;
        while ((*link)->next) link = &(*link)->next;
        struct js_bytecode *last = *link;
        *link = NULL; memory_bytes -= last->size;
        bytecode_release(last);
    }
}
/* The bytecode written into plain memory: the runtime's allocator counts its
   own heap and must not be freed into from other threads. */
static struct js_bytecode *compile(JSContext *ctx, const struct js_compile *job) {
    JSValue fn = JS_Eval(ctx, job->source, job->len, job->url, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fn)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
    size_t size = 0;
    uint8_t *bytes = JS_WriteObject(ctx, &size, fn, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, fn);
    if (!bytes) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
    struct js_bytecode *code = calloc(1, sizeof *code);
    if (code) code->bytes = malloc(size);
    if (code && code->bytes) { memcpy(code->bytes, bytes, size); code->size = size; code->refs = 1; }
    else { free(code); code = NULL; }
    js_free(ctx, bytes);
    return code;
}
static void run(JSContext *ctx, struct js_compile *job) {
    uint64_t start = n_clock_uptime_ms();
    uint8_t key[32];
    if (job->origin) {
        key_of(job, key);
        mtx_lock(&lock); job->code = recall(key); mtx_unlock(&lock);
        job->stats.remembered = job->code != NULL;
    }
    if (!job->code && ctx && (job->code = compile(ctx, job)) && job->origin) {
        memcpy(job->code->key, key, sizeof key);
        mtx_lock(&lock); keep(job->code); mtx_unlock(&lock);
    }
    job->stats.bytes = job->code ? job->code->size : 0;
    job->stats.compile_ms = n_clock_uptime_ms() - start;
}
static void compiler_main(void *unused) {
    (void)unused;
    /* Made here, so that QuickJS measures its stack guard from this thread's
       stack. A runtime that could not be made fails each job: its document
       compiles the script itself. */
    JSRuntime *rt = web_js_new_runtime(NULL);
    JSContext *ctx = NULL;
    if (rt) {
        JS_SetMemoryLimit(rt, SIZE_MAX); JS_SetMaxStackSize(rt, JS_STACK_LIMIT);
        ctx = JS_NewContext(rt);
    }
    for (;;) {
        mtx_lock(&lock);
        while (!queue_head) cnd_wait(&queued, &lock);
        struct js_compile *job = queue_head;
        if (!(queue_head = job->next)) queue_tail = &queue_head;
        bool dropped = job->dropped;
        mtx_unlock(&lock);
        if (!dropped) run(ctx, job);
        __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
        wake_address(&job->done, 0);
        mtx_lock(&lock); job_release(job); mtx_unlock(&lock);
    }
}
static void start_compilers(void) {
    struct n_cpuinfo info;
    unsigned count = cpu_info(&info) == 0 && info.scheduler_cpus > 1 ? info.scheduler_cpus - 1 : 0;
    if (count > JS_COMPILERS) count = JS_COMPILERS;
    for (unsigned i = 0; i < count; i++) {
        n_result tid = thread_create_sized(compiler_main, NULL, JS_COMPILER_STACK);
        if (tid < 0) break;
        thread_detach(tid);
        compilers++;
    }
}

struct js_compile *js_compile_start(const char *source, size_t len, const char *url, const char *origin) {
    call_once(&started, start_compilers);
    if (!compilers || !source || !url || len < JS_COMPILE_MIN_BYTES) return NULL;
    struct js_compile *job = calloc(1, sizeof *job);
    if (!job) return NULL;
    job->source = malloc(len + 1);
    job->url = strdup(url);
    job->origin = origin ? strdup(origin) : NULL;
    if (!job->source || !job->url || (origin && !job->origin)) {
        free(job->source); free(job->url); free(job->origin); free(job);
        return NULL;
    }
    memcpy(job->source, source, len); job->source[len] = 0; /* QuickJS reads to a NUL */
    job->len = len;
    job->refs = 2;
    mtx_lock(&lock);
    *queue_tail = job; queue_tail = &job->next;
    cnd_signal(&queued);
    mtx_unlock(&lock);
    return job;
}
bool js_compile_done(const struct js_compile *job) {
    return __atomic_load_n(&job->done, __ATOMIC_ACQUIRE) != 0;
}
bool js_compile_watch(struct js_compile *job, n_handle queue) {
    if (job->watched) return !js_compile_done(job);
    uint64_t token = JS_COMPILE_TOKEN | (__atomic_add_fetch(&tokens, 1, __ATOMIC_RELAXED) & ((UINT64_C(1) << 56) - 1));
    if (queue < 0 || n_events_watch_address(queue, &job->done, 0, token) < 0) return false;
    job->watched = true; job->queue = queue; job->token = token;
    return true;
}
static void end(struct js_compile *job) {
    if (job->watched) n_events_unwatch(job->queue, job->token);
    mtx_lock(&lock); job->dropped = true; job_release(job); mtx_unlock(&lock);
}
JSValue js_compile_take(JSContext *ctx, struct js_compile *job, struct js_compile_stats *stats) {
    while (!js_compile_done(job)) wait_on_address(&job->done, 0, UINT32_MAX);
    JSValue fn = JS_UNINITIALIZED;
    if (job->code) {
        fn = JS_ReadObject(ctx, job->code->bytes, job->code->size, JS_READ_OBJ_BYTECODE);
        if (JS_IsException(fn)) { JS_FreeValue(ctx, JS_GetException(ctx)); fn = JS_UNINITIALIZED; }
    }
    if (stats) *stats = job->stats;
    end(job);
    return fn;
}
void js_compile_drop(struct js_compile *job) {
    if (job) end(job);
}
