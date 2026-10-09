#pragma once
#include "quickjs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct web_workers web_workers;
/* URLs are already absolute. Parent MUST enforce its ordinary document
 * same-origin/CORS/cookie/security policy; child has no direct network API.
 * Completion may queue bytes only, never execute author JS. */
typedef bool (*web_worker_loader)(void *,uint32_t,uint32_t,const char *,int);
web_workers *web_worker_new(JSContext *,uint32_t,web_worker_loader,void *);
JSValue web_worker_native(web_workers *,int,JSValueConst *);
void web_worker_set_callback(web_workers *,JSValueConst);
void web_worker_loaded(web_workers *,uint32_t,uint32_t,int,const char *,const char *,const void *,size_t,const char *);
void web_worker_pump(web_workers *,uint64_t);
bool web_worker_runnable(const web_workers *);
/* Invoke under the caller's ordinary main-JS task budget/microtask checkpoint. */
bool web_worker_run_one(web_workers *);
int64_t web_worker_deadline(const web_workers *,uint64_t);
void web_worker_free(web_workers *);
void web_worker_background(void);
int64_t web_worker_background_deadline(uint64_t);
