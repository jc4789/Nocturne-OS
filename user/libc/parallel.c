#include <parallel.h>
#include <nocturne.h>
#include <limits.h>

#define PARALLEL_MAX_WORKERS 31
#define POOL_TLS 0
struct parallel_work {
    volatile size_t next;
    size_t count;
    parallel_fn fn;
    void *context;
    enum parallel_stage stage;
    bool profile;
};
struct parallel_worker {
    int tid;
    unsigned index;
    volatile uint32_t epoch, done;
    struct parallel_work *work;
    uint64_t items, units, elapsed_ms, cpu_mask;
};
static struct parallel_worker workers[PARALLEL_MAX_WORKERS];
static unsigned worker_count;
static volatile uint32_t pool_lock;
static bool initialized;
static volatile uint32_t stopping;
static uint32_t epoch;
static uint64_t jobs;
static bool enabled = true, profiling;
static struct parallel_stats statistics[PARALLEL_STAGE_COUNT];
void parallel_enable(bool value) { enabled = value; }
void parallel_profile_enable(bool value) { __atomic_store_n(&profiling, value, __ATOMIC_RELAXED); }
const char *parallel_stage_name(enum parallel_stage stage) {
    static const char *names[] = {
        "generic", "style-nodes", "intrinsic-boxes", "layout-boxes",
        "raster-candidate-pixels", "image-decode-bytes", "image-convert-pixels",
        "image-scale-pixels", "css-parse-bytes"
    };
    return (unsigned)stage < PARALLEL_STAGE_COUNT ? names[stage] : "invalid";
}
void parallel_record_work(enum parallel_stage stage, uint64_t units) {
    if (!__atomic_load_n(&profiling, __ATOMIC_RELAXED)) return;
    struct parallel_worker *w = thread_local_get(POOL_TLS);
    if (w && w->work && w->work->profile && w->work->stage == stage) w->units += units;
}

bool parallel_active(void) { return thread_local_get(POOL_TLS) != NULL; }
unsigned parallel_worker_index(void) {
    struct parallel_worker *w = thread_local_get(POOL_TLS);
    return w ? w->index : 0;
}
uint64_t parallel_jobs(void) { return __atomic_load_n(&jobs, __ATOMIC_RELAXED); }

static void pool_acquire(void) {
    while (__atomic_exchange_n(&pool_lock, 1, __ATOMIC_ACQUIRE))
        wait_on_address(&pool_lock, 1, 10);
}
static void pool_release(void) {
    __atomic_store_n(&pool_lock, 0, __ATOMIC_RELEASE);
    wake_address(&pool_lock, 1);
}
/* One claim per independent job avoids equal-count partitions when subtree
 * sizes differ. The bounded CAS cannot wrap even for SIZE_MAX jobs. */
static bool claim(struct parallel_work *work, size_t *index) {
    size_t next = __atomic_load_n(&work->next, __ATOMIC_RELAXED);
    while (next < work->count) {
        if (__atomic_compare_exchange_n(&work->next, &next, next + 1, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            *index = next; return true;
        }
    }
    return false;
}
static void sample_cpu(struct parallel_worker *w) {
    unsigned cpu = cpu_index();
    if (cpu < 64) w->cpu_mask |= UINT64_C(1) << cpu;
}
static void run_jobs(struct parallel_worker *w, parallel_service_fn service, void *service_context) {
    struct parallel_work *work = w->work;
    w->items = w->units = w->elapsed_ms = w->cpu_mask = 0;
    uint64_t start = work->profile ? uptime_ms() : 0;
    size_t index;
    while (claim(work, &index)) {
        if (work->profile && !(w->items & 63)) sample_cpu(w);
        work->fn(index, work->context);
        w->items++;
        if (service) service(service_context);
    }
    if (work->profile && w->items) {
        sample_cpu(w); w->elapsed_ms = uptime_ms() - start;
    }
}
bool parallel_get_stats(enum parallel_stage stage, struct parallel_stats *out) {
    if (!out || (unsigned)stage >= PARALLEL_STAGE_COUNT || parallel_active()) return false;
    pool_acquire(); *out = statistics[stage]; pool_release(); return true;
}
static void accumulate(struct parallel_stats *s, struct parallel_worker *w, bool helper) {
    s->items += w->items; s->work += w->units;
    s->worker_ms += w->elapsed_ms; s->sampled_cpu_mask |= w->cpu_mask;
    if (helper) {
        s->helper_items += w->items; s->helper_work += w->units;
        s->helper_ms += w->elapsed_ms; s->sampled_helper_cpu_mask |= w->cpu_mask;
    }
}
static void worker_main(void *context) {
    struct parallel_worker *w = context;
    thread_local_set(POOL_TLS, w);
    uint32_t seen = 0;
    for (;;) {
        uint32_t now = __atomic_load_n(&w->epoch, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) break;
        if (now == seen) { wait_on_address(&w->epoch, seen, UINT_MAX); continue; }
        run_jobs(w, NULL, NULL);
        seen = now;
        __atomic_store_n(&w->done, now, __ATOMIC_RELEASE);
        wake_address(&w->done, 1);
    }
    thread_local_set(POOL_TLS, NULL);
}
static void pool_init(void) {
    if (initialized) return;
    initialized = true;
    struct n_cpuinfo info;
    unsigned count = cpu_info(&info) == 0 && info.scheduler_cpus > 1 ? info.scheduler_cpus - 1 : 0;
    if (count > PARALLEL_MAX_WORKERS) count = PARALLEL_MAX_WORKERS;
    for (unsigned i = 0; i < count; i++) {
        struct parallel_worker *w = &workers[worker_count];
        w->index = worker_count + 1;
        w->tid = thread_create(worker_main, w);
        if (w->tid < 0) break; /* low memory/one CPU is a serial fallback, not lost work */
        worker_count++;
    }
}
static void pool_stop(void) {
    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < worker_count; i++) {
        /* Change the waited-on value as well as waking. A helper that read
         * stopping=false immediately before this store must not register an
         * infinite wait after the wake already happened. */
        uint32_t value = __atomic_load_n(&workers[i].epoch, __ATOMIC_RELAXED);
        __atomic_store_n(&workers[i].epoch, value + 1, __ATOMIC_RELEASE);
        wake_address(&workers[i].epoch, UINT_MAX);
    }
    for (unsigned i = 0; i < worker_count; i++) thread_join(workers[i].tid);
    for (unsigned i = 0; i < worker_count; i++) workers[i] = (struct parallel_worker){0};
    worker_count = 0; initialized = false; epoch = 0;
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
}
static bool dispatch(enum parallel_stage stage, size_t count, parallel_fn fn, void *context,
                     parallel_service_fn service, void *service_context, bool offload) {
    if (!count || !fn) return false;
    if (parallel_active()) { for (size_t i = 0; i < count; i++) fn(i, context); return false; }
    if ((unsigned)stage >= PARALLEL_STAGE_COUNT) stage = PARALLEL_GENERIC;
    pool_acquire();
    /* A helper unused for a complete 32-bit epoch cycle must not mistake a
     * fresh descriptor for its last job. Join before recycling identities. */
    if (epoch == UINT32_MAX) pool_stop();
    if (enabled) pool_init();
    unsigned helpers = !enabled ? 0 : offload ? (worker_count ? 1 : 0) :
        (count > 1 ? (unsigned)MIN(count - 1, (size_t)worker_count) : 0);
    struct parallel_work work = { .count=count, .fn=fn, .context=context, .stage=stage,
        .profile=__atomic_load_n(&profiling, __ATOMIC_RELAXED) };
    uint64_t start = work.profile ? uptime_ms() : 0;
    if (helpers) {
        if (++epoch == 0) ++epoch;
        for (unsigned i = 0; i < helpers; i++) {
            struct parallel_worker *w = &workers[i];
            w->work = &work;
            __atomic_store_n(&w->epoch, epoch, __ATOMIC_RELEASE);
            wake_address(&w->epoch, 1);
        }
    }
    struct parallel_worker caller = {.index=0, .work=&work};
    thread_local_set(POOL_TLS, &caller);
    if (!offload || !helpers) run_jobs(&caller, service, service_context);
    for (unsigned i = 0; i < helpers; i++) {
        struct parallel_worker *w = &workers[i];
        while (__atomic_load_n(&w->done, __ATOMIC_ACQUIRE) != epoch) {
            uint32_t value = __atomic_load_n(&w->done, __ATOMIC_RELAXED);
            if (value == epoch) break;
            if (service) service(service_context);
            wait_on_address(&w->done, value, service ? 2 : UINT_MAX);
        }
    }
    thread_local_set(POOL_TLS, NULL);
    if (work.profile) {
        struct parallel_stats *s = &statistics[stage];
        s->batches++; s->wall_ms += uptime_ms() - start;
        accumulate(s, &caller, false);
        for (unsigned i = 0; i < helpers; i++) accumulate(s, &workers[i], true);
    }
    for (unsigned i = 0; i < helpers; i++) workers[i].work = NULL;
    if (helpers) __atomic_fetch_add(&jobs, 1, __ATOMIC_RELAXED);
    pool_release();
    return helpers != 0;
}
bool parallel_for_stage(enum parallel_stage stage, size_t count, parallel_fn fn, void *context,
                        parallel_service_fn service, void *service_context) {
    return dispatch(stage,count,fn,context,service,service_context,false);
}
bool parallel_call_stage(enum parallel_stage stage, parallel_fn fn, void *context,
                         parallel_service_fn service, void *service_context) {
    return dispatch(stage,1,fn,context,service,service_context,true);
}
bool parallel_for(size_t count, parallel_fn fn, void *context) {
    return dispatch(PARALLEL_GENERIC, count, fn, context, NULL, NULL, false);
}
bool parallel_for_service(size_t count, parallel_fn fn, void *context,
                          parallel_service_fn service, void *service_context) {
    return dispatch(PARALLEL_GENERIC, count, fn, context, service, service_context, false);
}
bool parallel_call(parallel_fn fn, void *context,
                   parallel_service_fn service, void *service_context) {
    return dispatch(PARALLEL_GENERIC, 1, fn, context, service, service_context, true);
}
void parallel_shutdown(void) {
    if (parallel_active()) return;
    pool_acquire();
    pool_stop();
    pool_release();
}
