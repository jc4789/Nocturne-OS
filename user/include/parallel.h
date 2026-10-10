/* Native shared-address-space jobs. Callbacks own disjoint output ranges;
 * callers keep their inputs alive until this synchronous join returns. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*parallel_fn)(size_t index, void *context);
typedef void (*parallel_service_fn)(void *context);
enum parallel_stage {
    PARALLEL_GENERIC, PARALLEL_STYLE, PARALLEL_INTRINSIC, PARALLEL_LAYOUT,
    PARALLEL_RASTER, PARALLEL_IMAGE_DECODE, PARALLEL_IMAGE_CONVERT,
    PARALLEL_IMAGE_SCALE, PARALLEL_CSS_PARSE, PARALLEL_STAGE_COUNT
};
struct parallel_stats {
    uint64_t batches, items, helper_items, work, helper_work;
    uint64_t wall_ms, worker_ms, helper_ms;
    uint64_t sampled_cpu_mask, sampled_helper_cpu_mask;
};
/* Optional diagnostics: work units are visited/attempted nodes/boxes, candidate
 * pixels, or input bytes according to stage, not completed output counts.
 * Worker time is inclusive elapsed time, not CPU time.
 * CPU masks sample execution CPUs; a migrating job may use unsampled CPUs. */
void parallel_profile_enable(bool enabled);
bool parallel_get_stats(enum parallel_stage stage, struct parallel_stats *out);
const char *parallel_stage_name(enum parallel_stage stage);
void parallel_record_work(enum parallel_stage stage, uint64_t units);
bool parallel_for_stage(enum parallel_stage stage, size_t count, parallel_fn fn,
                        void *context, parallel_service_fn service, void *service_context);
bool parallel_call_stage(enum parallel_stage stage, parallel_fn fn, void *context,
                         parallel_service_fn service, void *service_context);
/* Callbacks must return normally. They must not thread_exit or longjmp across
 * the dispatch; record cancellation/failure and let the caller unwind after join. */
bool parallel_for(size_t count, parallel_fn fn, void *context);
bool parallel_for_service(size_t count, parallel_fn fn, void *context,
                          parallel_service_fn service, void *service_context);
/* Run one expensive operation on a helper while the caller services UI.
 * Nested use is synchronous, to avoid self-deadlock in the bounded pool. */
bool parallel_call(parallel_fn fn, void *context,
                   parallel_service_fn service, void *service_context);
bool parallel_active(void);
unsigned parallel_worker_index(void); /* 0: caller, 1..N: helper */
uint64_t parallel_jobs(void);
/* Native diagnostics only: compare the same workload with the serial path. */
void parallel_enable(bool enabled);
void parallel_shutdown(void);
