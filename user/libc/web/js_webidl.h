/* The Web IDL binding layer: the ECMAScript shape of the interfaces a realm's bindings define
   (js_webidl.c, from the specifications' IDL in webidl_data.inc). */
#pragma once
#include <stdbool.h>
#include <quickjs.h>

/* What the last web_js_webidl_install found, for the realm's setup trace. */
struct web_js_webidl_report {
    unsigned interfaces, members, wrapped, setters_dropped, misplaced;
    /* Functions given the built-in shape: called through a native function (a binding's
       constructible function), or given %Function.prototype% (an async or generator method). */
    unsigned builtin_wrapped, builtin_reprototyped;
    unsigned milliseconds;
    /* The first ones, "Interface.member" (a member in another form or place than the IDL's) or
       "Interface" (a prototype chain the IDL does not give), and the readonly attributes whose
       setters were dropped. */
    char misplaced_names[2048], dropped_names[1024];
};

/* host.webidl(global): shapes every interface, namespace and member the realm's bindings
   defined, for the global interface named (Window, DedicatedWorkerGlobalScope, ...). The
   bindings call it once, after they defined everything and before any author script. */
void web_js_webidl_init(JSContext *ctx, JSValueConst host);
/* The same for the interfaces in exports, a program of interfaces a realm runs later (Web
   Audio): the interface objects and their members, not the global's properties. */
void web_js_webidl_shape_exports(JSContext *ctx, JSValueConst exports, const char *global);
const struct web_js_webidl_report *web_js_webidl_last(void);
