/* Native realm metadata. Never accept a page-selected CPU count or identity. */
#pragma once
#include <nocturne.h>
#include <browser_identity.h>
#include <quickjs.h>

static unsigned web_js_hardware_concurrency(void) {
    struct n_cpuinfo info = {0};
    if (cpu_info(&info) < 0 || info.version != 1 || !info.scheduler_cpus ||
        info.scheduler_cpus > info.online_cpus) return 1;
    return info.scheduler_cpus;
}
static void web_js_navigator_init(JSContext *ctx, JSValueConst host) {
    JS_SetPropertyStr(ctx, host, "userAgent", JS_NewString(ctx, NOCTURNE_USER_AGENT));
    JS_SetPropertyStr(ctx, host, "platform", JS_NewString(ctx, NOCTURNE_PLATFORM));
    JS_SetPropertyStr(ctx, host, "hardwareConcurrency", JS_NewUint32(ctx, web_js_hardware_concurrency()));
}
