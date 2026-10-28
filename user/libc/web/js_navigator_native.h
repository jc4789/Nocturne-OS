/* Native realm metadata. Never accept a page-selected CPU count or identity. */
#pragma once
#include <nocturne.h>
#include <browser_identity.h>
#include <quickjs.h>

static inline unsigned web_js_hardware_concurrency(void) {
    struct n_cpuinfo info = {0};
    if (cpu_info(&info) < 0 || info.version != 1 || !info.scheduler_cpus ||
        info.scheduler_cpus > info.online_cpus) return 1;
    return info.scheduler_cpus;
}
/* host.languages(): the user's languages as the system keeps them ("ja,en-US"), read when a realm
   starts and again in its languagechange task. The system's own default if the call fails. */
static inline JSValue web_js_native_languages(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char list[N_USER_LANGUAGES_MAX];
    return JS_NewString(ctx, n_user_languages(list, sizeof list) > 0 ? list : "en-US");
}
static inline void web_js_navigator_init(JSContext *ctx, JSValueConst host) {
    JS_SetPropertyStr(ctx, host, "userAgent", JS_NewString(ctx, NOCTURNE_USER_AGENT));
    JS_SetPropertyStr(ctx, host, "platform", JS_NewString(ctx, NOCTURNE_PLATFORM));
    JS_SetPropertyStr(ctx, host, "hardwareConcurrency", JS_NewUint32(ctx, web_js_hardware_concurrency()));
    JS_SetPropertyStr(ctx, host, "languages", JS_NewCFunction(ctx, web_js_native_languages, "languages", 0));
}

/* A change of the user's languages reaches every realm of the process (js_navigator.c): the
   process watches the system's settings in its event queue, and each realm queues its
   languagechange task when the count it saw falls behind. Watch before the first realm reads. */
#define WEB_LANGUAGES_TOKEN (UINT64_C(0x4c) << 56)
void web_js_languages_watch(n_handle queue);
void web_js_languages_events(const struct n_event *events, size_t count);
uint64_t web_js_languages_generation(void);
