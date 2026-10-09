#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "quickjs.h"
#define JS_TIMERS_INITIAL 32u
#define JS_TIMERS_MAX 1024u
#define JS_IDLE_MAX 1024u
/* TASK_STRUCTS */
struct web_js_state;
typedef struct {struct web_js_state *js;bool live;} web_doc;
struct web_js_state {
    JSContext *ctx;web_doc *doc;bool disabled;
    struct js_timer *timers;unsigned timer_capacity;uint32_t next_timer;
    struct js_idle_task *idle_pending,*last_idle_pending,*idle_runnable,*last_idle_runnable;
    unsigned idle_count,idle_period_callbacks;uint32_t next_idle;uint64_t idle_period_end;bool idle_period_stopped;
    struct js_posted_task *posted,*last_posted;unsigned posted_count;uint32_t next_posted;
};
static struct web_js_state *state(JSContext *ctx){return JS_GetContextOpaque(ctx);}
static uint64_t uptime_ms(void){return 1234;}
