/* Classic scripts compiled away from the document's thread.
 *
 * A compiler thread owns a QuickJS runtime of its own. It compiles a copy of
 * the source and writes the function as bytecode; the document's thread reads
 * that bytecode into its own context. Only bytecode this process's compiler
 * wrote in memory is ever read back, never bytes from a page, the network or
 * storage. Compiled bytecode is kept for the same document origin, URL and
 * source, so going back, forward or to the page again does not compile it
 * again. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <nocturne.h>
#include "quickjs.h"

struct js_compile;
/* What a finished job did, for profiles. */
struct js_compile_stats {
    uint64_t compile_ms; /* on the compiler: hashing, compiling and writing */
    size_t bytes;        /* of bytecode; 0 when compiling failed */
    bool remembered;     /* the bytecode was kept from an earlier compile */
};

/* Starts compiling len bytes of source (copied) as the classic script named
   url, for documents of origin (NULL: opaque, whose bytecode is never kept).
   NULL when no compiler takes it (a small script among them): the caller
   compiles it itself. */
struct js_compile *js_compile_start(const char *source, size_t len, const char *url, const char *origin);
bool js_compile_done(const struct js_compile *job);
/* Ends a wait on queue when job finishes. False when that cannot be arranged:
   the job finished first, or the queue refused the watch. */
bool js_compile_watch(struct js_compile *job, n_handle queue);
/* The function bytecode in ctx, waiting for the compiler if it has not
   finished; JS_UNINITIALIZED when it failed, and the caller compiles the
   source itself so that the error belongs to its own realm. stats may be
   NULL. Ends job. */
JSValue js_compile_take(JSContext *ctx, struct js_compile *job, struct js_compile_stats *stats);
/* Ends job without its result. */
void js_compile_drop(struct js_compile *job);
