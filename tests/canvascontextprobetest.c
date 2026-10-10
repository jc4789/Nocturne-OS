/* Diagnostic-only getContext probes must not create or fake GPU capabilities. */
#include <nocturne.h>
#include <web.h>
#include <stdio.h>
#include <string.h>

static int checks, failures, errors, probes, per_type[6];
static bool sanitized;
static const char *types[] = {"2d", "webgl", "webgl2", "experimental-webgl", "bitmaprenderer", "webgpu"};
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL canvascontextprobe %s\n", name); }
}
static void log_line(void *unused, int level, const char *message) {
    if (!strncmp(message, "OK probe ", 9)) { checks++; puts(message); }
    else if (!strncmp(message, "FAIL probe ", 11)) { checks++; failures++; puts(message); }
    else if (!strncmp(message, "Canvas context request: ", 24)) {
        probes++;
        for (unsigned i = 0; i < 6; i++) {
            char expected[80]; snprintf(expected, sizeof expected, "type=%s frame=", types[i]);
            if (strstr(message, expected)) per_type[i]++;
        }
        if (!strstr(message, "frame=0 connected=1 origin=https://canvas-probe.test") ||
            strstr(message, "private") || strstr(message, "unknown")) sanitized = false;
    } else if (level >= 2) { errors++; printf("ERROR canvascontextprobe %s\n", message); }
}
static const char page[] = "<!doctype html><canvas id=real width=4 height=4></canvas>";
static const char source[] =
    "const assertProbe=(n,v)=>console.log((v?'OK probe ':'FAIL probe ')+n);"
    "const canvas=document.getElementById('real'),context=canvas.getContext('2d');"
    "context.fillStyle='lime';context.fillRect(0,0,4,4);const pixel=context.getImageData(1,1,1,1).data;"
    "assertProbe('native-2d-pixel',pixel[0]===0&&pixel[1]===255&&pixel[2]===0&&pixel[3]===255);"
    "assertProbe('context-cache',canvas.getContext('2d')===context);"
    "assertProbe('unsupported-gpu-stays-null',['webgl','webgl2','experimental-webgl','bitmaprenderer','webgpu'].every(type=>canvas.getContext(type)===null&&canvas.getContext(type)===null));"
    "assertProbe('unknown-stays-null',canvas.getContext('unknown-private')===null);"
    "let branded=false;try{HTMLCanvasElement.prototype.getContext.call({},'webgl');}catch(error){branded=error instanceof TypeError;}"
    "assertProbe('receiver-brand',branded);let conversions=0;"
    "assertProbe('one-string-conversion',canvas.getContext({toString(){conversions++;return '2d';}})===context&&conversions===1);";

int main(void) {
    for (int debug = 0; debug < 2; debug++) {
        errors = probes = 0; sanitized = true; memset(per_type, 0, sizeof per_type);
        struct web_host host = {.console=log_line, .debug_js=debug, .js_task_budget_ms=5000};
        web_doc *d = web_live(page, sizeof page - 1,
            "https://canvas-probe.test/private?private=not-logged", "utf-8", &host);
        check("document allocated", d != NULL); if (!d) continue;
        /* Match the real browser's incremental parser/task lifecycle. */
        for (int i = 0; i < 8; i++) { web_tick(d, uptime_ms()); msleep(1); }
        web_layout(d, 100, 100);
        check("native JS evaluated", web_console_eval(d, source, sizeof source - 1));
        check("no runtime exception", errors == 0);
        check("debug-gated probe total", probes == (debug ? 6 : 0));
        if (debug) {
            bool once = true; for (unsigned i = 0; i < 6; i++) once &= per_type[i] == 1;
            check("each known type logged once", once);
            check("origin-only and unknown-not-logged", sanitized);
        }
        web_free(d);
    }
    printf("canvascontextprobe: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
