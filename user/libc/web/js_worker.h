#pragma once
#include "quickjs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct web_workers web_workers;
typedef struct web_worker_publication web_worker_publication;
/* Private bridge: prepare owns all wire/ledger allocation; publish is pointer
 * linking only, after the caller's native transfer plan commits. */
web_worker_publication *web_worker_prepare(web_workers *,uint32_t,unsigned,uint32_t,unsigned,JSValueConst);
web_worker_publication *web_worker_prepare_captured(web_workers *,uint32_t,unsigned,uint32_t,unsigned,JSValueConst,uint32_t);
JSValue web_worker_capture(web_workers *,uint32_t,uint32_t,unsigned);
void web_worker_release_capture(web_workers *,uint32_t);
void web_worker_cancel_captures(web_workers *);
void web_worker_publish(web_worker_publication *);
void web_worker_abort(web_worker_publication *);
/* URLs are already absolute. Parent MUST enforce its ordinary document
 * same-origin/CORS/cookie/security policy; child has no direct network API.
 * Completion may queue bytes only, never execute author JS. */
typedef bool (*web_worker_loader)(void *,uint32_t,uint32_t,const char *,int);
/* A Worker's fetch (NJW_LOAD kind 2) as the child sent it: its modes are the kind's
 * NJW_LOAD_* flags. referrer is the child's referrer source (NULL: its client), destination a
 * webnet destination's name. Binary Fetch request snapshots are borrowed only for this call. The
 * browser transport copies them before returning and retains the native initiator. */
struct web_worker_fetch {
    const char *url,*method,*headers,*referrer,*destination;
    const void *body;size_t body_len;
    int referrer_policy;
};
typedef bool (*web_worker_requester)(void *,uint32_t,uint32_t,int,const struct web_worker_fetch *);
void web_worker_set_requester(web_workers *,web_worker_requester);
/* The child took the last frame of a streamed fetch (NJW_RESUME): send the next. */
typedef void (*web_worker_resumer)(void *,uint32_t,uint32_t);
void web_worker_set_resumer(web_workers *,web_worker_resumer);
/* Request zero retires all outstanding resources of this real child owner. */
typedef void (*web_worker_canceler)(void *,uint32_t,uint32_t);
web_workers *web_worker_new(JSContext *,uint32_t,web_worker_loader,void *);
/* Private native creator policy snapshot, copied when each child is created.
 * Local-script workers inherit it; HTTP workers use their response policy. */
bool web_worker_set_policy(web_workers *,const char *,int);
/* Native document authority, never supplied by the author's constructor. */
bool web_worker_set_origin(web_workers *,const char *);
/* The creator's agent-cluster isolation, inherited by dedicated Workers. */
void web_worker_set_isolated(web_workers *,bool);
/* Whether a child is in this document's storage partition (FileAPI's same-partition blob URL
 * usage), so a blob: URL it fetches may be obtained from this document's store. */
bool web_worker_blob_partition(web_workers *,uint32_t);
typedef void (*web_worker_reporter)(void *,JSValueConst);
void web_worker_set_reporter(web_workers *,web_worker_reporter,void *);
void web_worker_set_canceler(web_workers *,web_worker_canceler);
JSValue web_worker_native(web_workers *,int,JSValueConst *);
void web_worker_set_callback(web_workers *,JSValueConst);
/* A request's response or error (NJW_LOADED; request 0 is the Worker's script, NJW_START). timing is
 * the request's report timing (NJW_LOADED_TIMING) or JS_UNDEFINED. */
void web_worker_loaded(web_workers *,uint32_t,uint32_t,int,const char *,const char *,const void *,size_t,const char *,JSValueConst);
/* One frame of a streamed fetch (event: WEBNET_HEADERS, WEBNET_CHUNK, WEBNET_END or
 * WEBNET_UPLOAD; bytes: a chunk's, or the uploaded count for WEBNET_UPLOAD). An opaque response's
 * headers enter the child blank; WEBNET_END carries the error and the request's report timing
 * (NJW_LOADED_TIMING, JS_UNDEFINED for none). False when the child is gone or the frame was not
 * queued; the caller then cancels the request. */
bool web_worker_frame(web_workers *,uint32_t,uint32_t,int,int,const char *,const char *,const void *,size_t,const char *,JSValueConst);
void web_worker_pump(web_workers *,uint64_t);
bool web_worker_runnable(const web_workers *);
/* Invoke under the caller's ordinary main-JS task budget/microtask checkpoint. */
bool web_worker_run_one(web_workers *);
int64_t web_worker_deadline(const web_workers *,uint64_t);
void web_worker_free(web_workers *);
void web_worker_background(void);
int64_t web_worker_background_deadline(uint64_t);
