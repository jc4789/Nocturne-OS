/* Native realm metadata. Never accept a page-selected CPU count or identity. */
#pragma once
#include <nocturne.h>
#include <browser_identity.h>
#include <quickjs.h>
#include <webprefs.h>

static inline unsigned web_js_hardware_concurrency(void) {
    struct n_cpuinfo info = {0};
    if (cpu_info(&info) < 0 || info.version != 1 || !info.scheduler_cpus ||
        info.scheduler_cpus > info.online_cpus) return 1;
    return info.scheduler_cpus;
}
/* host.languages(): the user's languages as the browser's preferences hold them in this process
   ("ja,en-US", webprefs), read when a realm starts and again in its languagechange task. A
   change reaches every realm of the process through the watch in the queue its loop waits on
   (web_wait_queue, a Worker's own): each realm queues its languagechange task when
   webprefs_languages_generation() moves past the one it answers for. Watch before the first
   realm reads. HTML has navigator.languages and Accept-Language use the same list, and the
   list is kept short enough for its Accept-Language (at most 128 bytes, with the weights), so
   this buffer holds it whole. */
static inline JSValue web_js_native_languages(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char list[WEBPREFS_ACCEPT_LANGUAGE_MAX + 1];
    n_result n = webprefs_languages(list, sizeof list);
    if (n <= 0) return JS_ThrowInternalError(ctx, "The browser's languages cannot be read");
    return JS_NewStringLen(ctx, list, (size_t)n);
}
static inline void web_js_navigator_init(JSContext *ctx, JSValueConst host) {
    JS_SetPropertyStr(ctx, host, "userAgent", JS_NewString(ctx, NOCTURNE_USER_AGENT));
    JS_SetPropertyStr(ctx, host, "platform", JS_NewString(ctx, NOCTURNE_PLATFORM));
    JS_SetPropertyStr(ctx, host, "hardwareConcurrency", JS_NewUint32(ctx, web_js_hardware_concurrency()));
    JS_SetPropertyStr(ctx, host, "languages", JS_NewCFunction(ctx, web_js_native_languages, "languages", 0));
}
