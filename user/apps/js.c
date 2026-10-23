/* js: runs JavaScript and TypeScript in the terminal (QuickJS).
 *
 *   js FILE [ARGS]     ES module, CommonJS or JSON; .ts/.mts/.cts are type-checked
 *                      and compiled with tsgo first
 *   js -e CODE, -p CODE, js - (input), js (interactive prompt)
 *   js --init [FOLDER] a TypeScript project for tsgo and js
 *
 * The runtime is JavaScript (js_runtime.js, js_console.js, js_web.js and the
 * browser's DOMException, Encoding, URL and Web Crypto); this file gives it the
 * system through a private host object and runs the event loop: microtasks,
 * timers and fetches answered by worker threads. */
#include <nocturne.h>
#include <display.h>
#include <ui.h>
#include <math.h>
#include <quickjs.h>
#include <http.h>
#include <threads.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libc/web/js_encoding.h"
#include "../libc/web/js_crypto.h"
#include "../libc/web/js_memory.h"

#pragma clang diagnostic ignored "-Wc23-extensions"

static const char src_dom_exception[] = {
#embed "../libc/web/js_dom_exception.js"
};
static const char src_encoding[] = {
#embed "../libc/web/js_encoding.js"
};
static const char src_runtime[] = {
#embed "js_runtime.js"
};
static const char src_console[] = {
#embed "js_console.js"
};
static const char src_web[] = {
#embed "js_web.js"
};
static const char src_ui[] = {
#embed "js_ui.js"
};
static const char src_url[] = {
#embed "../libc/web/js_url.js"
};
static const char src_crypto[] = {
#embed "../libc/web/js_crypto.js"
};

#define VERSION "1.0"
#define PATH_CAP 4096

static JSRuntime *rt;
static JSContext *ctx;
static JSValue host, internal;

/* ---------------------------------------------------------------- helpers */

/* Calls internal[name]; the arguments are consumed. */
static JSValue internal_call(const char *name, int argc, JSValue *argv) {
    JSValue fn = JS_GetPropertyStr(ctx, internal, name);
    JSValue r = JS_Call(ctx, fn, internal, argc, (JSValueConst *)argv);
    JS_FreeValue(ctx, fn);
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, argv[i]);
    return r;
}

/* An exception nothing caught: the runtime prints it and ends the app (not
   at the interactive prompt). Before the runtime is up, print it here. */
static void report_exception(void) {
    JSValue e = JS_GetException(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, internal, "reportError");
    if (JS_IsFunction(ctx, fn)) {
        JSValue r = JS_Call(ctx, fn, internal, 1, (JSValueConst[]){e});
        if (JS_IsException(r)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            fputs("js: an error occurred while reporting an error\n", stderr);
        }
        JS_FreeValue(ctx, r);
    } else {
        const char *s = JS_ToCString(ctx, e);
        JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
        const char *st = JS_IsString(stack) ? JS_ToCString(ctx, stack) : NULL;
        fprintf(stderr, "js: %s\n%s", s ? s : "error", st ? st : "");
        if (st) JS_FreeCString(ctx, st);
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, stack);
        exit(70);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, e);
}

static const char *const result_names[] = {
    NULL, "NOT_FOUND", "EXISTS", "DENIED", "NO_MEMORY", "INVALID", "WRONG_KIND", "NO_HANDLE",
    "BUSY", "NOT_READY", "CLOSED", "TIMEOUT", "CANCELLED", "TOO_LARGE", "NOT_EMPTY", "IO",
    "UNSUPPORTED", "BAD_ADDRESS", "STOPPED", "NOT_FOLDER", "IS_FOLDER", "READ_ONLY", "NO_SPACE",
    "NOT_PROGRAM", "NAME_TOO_LONG", "NO_APP", "TOO_MANY_ARGUMENTS", "CONNECTION_RESET",
    "CONNECTION_REFUSED", "ADDRESS_IN_USE", "NETWORK_DOWN", "NOT_CONNECTED", "UNREACHABLE",
    "PROTOCOL",
};

/* A system failure as an Error with code ("NOT_FOUND"...), result and path. */
static JSValue throw_result(const char *op, const char *path, n_result r) {
    if (r > 0) r = -r;
    char message[PATH_CAP + 160];
    if (path) snprintf(message, sizeof message, "%s '%s': %s", op, path, n_result_text(r));
    else snprintf(message, sizeof message, "%s: %s", op, n_result_text(r));
    JSValue e = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, e, "message", JS_NewString(ctx, message),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    long code = -r;
    const char *name = code > 0 && code < (long)(sizeof result_names / sizeof *result_names)
                           ? result_names[code] : "UNKNOWN";
    JS_SetPropertyStr(ctx, e, "code", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, e, "result", JS_NewInt64(ctx, r));
    if (path) JS_SetPropertyStr(ctx, e, "path", JS_NewString(ctx, path));
    return JS_Throw(ctx, e);
}

/* The complete path for a script's path argument. */
static bool arg_path(JSValueConst v, char *out) {
    const char *s = JS_ToCString(ctx, v);
    if (!s) return false;
    n_result r = n_path_complete(s, out, PATH_CAP);
    if (r < 0) {
        throw_result("path", s, r);
        JS_FreeCString(ctx, s);
        return false;
    }
    JS_FreeCString(ctx, s);
    return true;
}

/* Bytes of a string, ArrayBuffer or view. *str must be freed with JS_FreeCString. */
static bool arg_bytes(JSValueConst v, const uint8_t **data, size_t *len, const char **str) {
    *str = NULL;
    if (JS_IsString(v)) {
        *str = JS_ToCStringLen(ctx, len, v);
        *data = (const uint8_t *)*str;
        return *str != NULL;
    }
    size_t offset, length, element;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, v, &offset, &length, &element);
    if (!JS_IsException(buffer)) {
        size_t size;
        uint8_t *p = JS_GetArrayBuffer(ctx, &size, buffer);
        JS_FreeValue(ctx, buffer);
        if (!p && length) return false;
        *data = p ? p + offset : (const uint8_t *)"";
        *len = length;
        return true;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t size;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, v);
    if (!p && JS_HasException(ctx)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_ThrowTypeError(ctx, "expected a string, ArrayBuffer or typed array");
        return false;
    }
    *data = p ? p : (const uint8_t *)"";
    *len = p ? size : 0;
    return true;
}

static int arg_int(JSValueConst v) {
    int32_t n = 0;
    JS_ToInt32(ctx, &n, v);
    return n;
}

/* ---------------------------------------------------------------- output, input, time */

static FILE *stream(int fd) { return fd == 2 ? stderr : stdout; }

static JSValue h_write(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    int fd = arg_int(argv[0]);
    const uint8_t *data;
    size_t len;
    const char *str;
    if (!arg_bytes(argv[1], &data, &len, &str)) return JS_EXCEPTION;
    FILE *f = stream(fd);
    fwrite(data, 1, len, f);
    if (n_terminal(fd)) fflush(f);
    if (str) JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

static JSValue h_is_terminal(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewBool(ctx, n_terminal(arg_int(argv[0])));
}

static JSValue h_now(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, (double)n_clock_uptime_ms());
}

static JSValue h_exit(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    fflush(stdout);
    fflush(stderr);
    exit(argc ? arg_int(argv[0]) : 0);
}

/* One line of input without its newline; null at the end of input. */
static JSValue h_read_line(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    const char *prompt = argc && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : NULL;
    JSValue r;
    fflush(stdout);
    if (n_terminal(0)) {
        enum { LINE = 64 * 1024 };
        char *buf = malloc(LINE);
        if (!buf) return JS_ThrowOutOfMemory(ctx);
        int n = readline(prompt ? prompt : "", buf, LINE, 0);
        r = n < 0 ? JS_NULL : JS_NewStringLen(ctx, buf, (size_t)n);
        free(buf);
    } else {
        if (prompt && *prompt) fputs(prompt, stdout), fflush(stdout);
        size_t cap = 256, n = 0;
        char *buf = malloc(cap);
        int ch = EOF;
        while (buf && (ch = getchar()) != EOF && ch != '\n') {
            if (n + 1 >= cap) {
                char *more = realloc(buf, cap *= 2);
                if (!more) { free(buf); buf = NULL; break; }
                buf = more;
            }
            buf[n++] = (char)ch;
        }
        if (!buf) r = JS_ThrowOutOfMemory(ctx);
        else {
            if (n && buf[n - 1] == '\r') n--;
            r = ch == EOF && !n ? JS_NULL : JS_NewStringLen(ctx, buf, n);
        }
        free(buf);
    }
    if (prompt) JS_FreeCString(ctx, prompt);
    return r;
}

static char *read_input(size_t *len) {
    size_t cap = 4096, n = 0, got;
    char *buf = malloc(cap + 1);
    while (buf && (got = fread(buf + n, 1, cap - n, stdin)) > 0) {
        n += got;
        if (n == cap) {
            char *more = realloc(buf, (cap *= 2) + 1);
            if (!more) { free(buf); return NULL; }
            buf = more;
        }
    }
    if (buf) buf[n] = 0;
    *len = n;
    return buf;
}

static JSValue h_read_all(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    size_t n;
    char *buf = read_input(&n);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    JSValue r = JS_NewStringLen(ctx, buf, n);
    free(buf);
    return r;
}

/* ---------------------------------------------------------------- documents and folders */

static JSValue h_read_file(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP];
    if (!arg_path(argv[0], path)) return JS_EXCEPTION;
    void *data = NULL;
    size_t size = 0;
    n_result r = n_document_load(path, &data, &size);
    if (r < 0) return throw_result("read", path, r);
    JSValue v = JS_ToBool(ctx, argv[1]) ? JS_NewStringLen(ctx, data ? data : "", size)
                                         : JS_NewArrayBufferCopy(ctx, data, size);
    if (data) n_memory_release(data, size);
    return v;
}

static JSValue h_write_file(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP];
    if (!arg_path(argv[0], path)) return JS_EXCEPTION;
    const uint8_t *data;
    size_t len;
    const char *str;
    if (!arg_bytes(argv[1], &data, &len, &str)) return JS_EXCEPTION;
    n_result r = JS_ToBool(ctx, argv[2]) ? n_document_append(path, data, len)
                                         : n_document_save(path, data, len);
    if (str) JS_FreeCString(ctx, str);
    return r < 0 ? throw_result("write", path, r) : JS_UNDEFINED;
}

static JSValue entry_object(const struct n_entry *e, const char *path) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e->name));
    JS_SetPropertyStr(ctx, o, "path", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, o, "kind", JS_NewInt32(ctx, (int32_t)e->kind));
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt64(ctx, (int64_t)e->size));
    JS_SetPropertyStr(ctx, o, "modified", JS_NewInt64(ctx, e->modified));
    return o;
}

/* The entry a path names, or null when there is none. */
static JSValue h_info(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP];
    if (!arg_path(argv[0], path)) return JS_EXCEPTION;
    struct n_entry e;
    n_result r = n_entry_info(path, &e);
    if (r == -N_ERR_NOT_FOUND || r == -N_ERR_NOT_FOLDER) return JS_NULL;
    if (r < 0) return throw_result("info", path, r);
    if (!e.name[0]) snprintf(e.name, sizeof e.name, "%s", n_path_name(path));
    return entry_object(&e, path);
}

static JSValue h_list(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP], child[PATH_CAP];
    if (!arg_path(argv[0], path)) return JS_EXCEPTION;
    struct n_entry *entries = NULL;
    n_result n = n_folder_entries(path, &entries);
    if (n < 0) return throw_result("list", path, n);
    JSValue a = JS_NewArray(ctx);
    for (n_result i = 0; i < n; i++) {
        if (n_path_join(path, entries[i].name, child, sizeof child) < 0) continue;
        JS_SetPropertyUint32(ctx, a, (uint32_t)i, entry_object(&entries[i], child));
    }
    free(entries);
    return a;
}

static JSValue h_create_folder(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP];
    if (!arg_path(argv[0], path)) return JS_EXCEPTION;
    n_result r = n_folder_create(path);
    return r < 0 ? throw_result("create folder", path, r) : JS_UNDEFINED;
}

static JSValue h_remove(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP];
    if (!arg_path(argv[0], path)) return JS_EXCEPTION;
    n_result r = n_entry_remove(path);
    return r < 0 ? throw_result("remove", path, r) : JS_UNDEFINED;
}

static JSValue h_move(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char from[PATH_CAP], to[PATH_CAP];
    if (!arg_path(argv[0], from) || !arg_path(argv[1], to)) return JS_EXCEPTION;
    n_result r = n_entry_move(from, to);
    return r < 0 ? throw_result("move", from, r) : JS_UNDEFINED;
}

static JSValue h_complete(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP];
    if (!arg_path(argv[0], path)) return JS_EXCEPTION;
    return JS_NewString(ctx, path);
}

/* The app's folder (the setting FOLDER), as a complete path. */
static JSValue h_cwd(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP];
    n_result r = n_path_complete(".", path, sizeof path);
    return r < 0 ? throw_result("folder", NULL, r) : JS_NewString(ctx, path);
}

/* The temporary folder (the setting TEMP), emptied at every start. */
static JSValue h_temp_folder(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewString(ctx, n_temp_folder());
}

static JSValue h_chdir(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    char path[PATH_CAP];
    if (!arg_path(argv[0], path)) return JS_EXCEPTION;
    n_result r = n_folder_set(path);
    return r < 0 ? throw_result("change folder", path, r) : JS_UNDEFINED;
}

/* ---------------------------------------------------------------- settings */

/* Settings this app changed: a launch setting can be removed only here (getenv
   keeps reading the launch list), and run() passes the result on. */
struct setting { char *name, *value; struct setting *next; };
static struct setting *settings;

static struct setting *setting_find(const char *name) {
    for (struct setting *s = settings; s; s = s->next)
        if (!strcmp(s->name, name)) return s;
    return NULL;
}

static const char *setting_get(const char *name) {
    struct setting *s = setting_find(name);
    return s ? s->value : getenv(name);
}

static bool setting_put(const char *name, const char *value) {
    struct setting *s = setting_find(name);
    char *v = value ? strdup(value) : NULL;
    if (value && !v) return false;
    if (!s) {
        s = calloc(1, sizeof *s);
        if (!s || !(s->name = strdup(name))) { free(s); free(v); return false; }
        s->next = settings;
        settings = s;
    }
    free(s->value);
    s->value = v;
    if (value) setenv(name, value, 1);
    else unsetenv(name);
    return true;
}

static JSValue h_get_setting(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const char *v = setting_get(name);
    JS_FreeCString(ctx, name);
    return v ? JS_NewString(ctx, v) : JS_UNDEFINED;
}

static JSValue h_set_setting(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    JSValue r = JS_UNDEFINED;
    if (!name || (argc > 1 && !value)) r = JS_EXCEPTION;
    else if (!*name || strchr(name, '=')) r = JS_ThrowTypeError(ctx, "invalid setting name '%s'", name);
    else if (!setting_put(name, value)) r = JS_ThrowOutOfMemory(ctx);
    if (name) JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    return r;
}

static JSValue h_remove_setting(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    bool ok = setting_put(name, NULL);
    JS_FreeCString(ctx, name);
    return ok ? JS_UNDEFINED : JS_ThrowOutOfMemory(ctx);
}

/* ---------------------------------------------------------------- other apps */

static void free_strings(char **v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* Runs a program with this app's input, output, log, folder and settings and
   waits for it; returns its result. */
static JSValue h_run(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    const char *program = JS_ToCString(ctx, argv[0]);
    if (!program) return JS_EXCEPTION;
    char full[PATH_CAP];
    if (!find_program(program, full, sizeof full)) {
        JSValue r = throw_result("run", program, -N_ERR_NOT_FOUND);
        JS_FreeCString(ctx, program);
        return r;
    }
    uint32_t count = 0;
    JSValue len = JS_GetPropertyStr(ctx, argv[1], "length");
    JS_ToUint32(ctx, &count, len);
    JS_FreeValue(ctx, len);
    char **args = calloc(count + 2, sizeof *args);
    uint32_t nargs = 0;
    if (args) {
        args[nargs++] = strdup(n_path_name(program));
        for (uint32_t i = 0; i < count; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
            const char *s = JS_ToCString(ctx, v);
            JS_FreeValue(ctx, v);
            if (!s) { free_strings(args, nargs); JS_FreeCString(ctx, program); return JS_EXCEPTION; }
            args[nargs++] = strdup(s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeCString(ctx, program);
    /* The launch settings as this app sees them now, then the ones it added. */
    uint32_t nlaunch = 0, nset = 0;
    while (n_settings[nlaunch]) nlaunch++;
    for (struct setting *s = settings; s; s = s->next) nset++;
    char **list = calloc(nlaunch + nset + 2, sizeof *list);
    uint32_t n = 0;
    bool folder = false;
    for (uint32_t i = 0; list && i < nlaunch; i++) {
        const char *eq = strchr(n_settings[i], '=');
        if (!eq) continue;
        size_t nl = (size_t)(eq - n_settings[i]);
        char name[256];
        if (nl >= sizeof name) continue;
        memcpy(name, n_settings[i], nl);
        name[nl] = 0;
        if (setting_find(name)) continue;
        if (!strcmp(name, "FOLDER")) continue;
        list[n++] = strdup(n_settings[i]);
    }
    for (struct setting *s = settings; list && s; s = s->next) {
        if (!s->value || !strcmp(s->name, "FOLDER")) continue;
        size_t size = strlen(s->name) + strlen(s->value) + 2;
        if ((list[n] = malloc(size))) snprintf(list[n++], size, "%s=%s", s->name, s->value);
    }
    if (list) {
        const char *f = getenv("FOLDER");
        if (!f || !n_path_is_complete(f)) f = "C:\\";
        size_t size = strlen(f) + 8;
        if ((list[n] = malloc(size))) snprintf(list[n++], size, "FOLDER=%s", f), folder = true;
    }
    bool complete = args && list && folder;
    for (uint32_t i = 0; complete && i < nargs; i++) complete = args[i] != NULL;
    for (uint32_t i = 0; complete && i < n; i++) complete = list[i] != NULL;
    if (!complete) {
        if (args) free_strings(args, nargs);
        if (list) free_strings(list, n);
        return JS_ThrowOutOfMemory(ctx);
    }
    int32_t handles[3];
    for (int i = 0; i < 3; i++) handles[i] = n_handle_kind(i) >= 0 ? i : -1;
    struct n_launch launch = {.arguments = (const char *const *)args, .settings = (const char *const *)list,
                              .handles = handles, .argument_count = nargs, .setting_count = n,
                              .handle_count = 3};
    fflush(stdout);
    fflush(stderr);
    n_handle app = n_app_launch(full, &launch);
    free_strings(args, nargs);
    free_strings(list, n);
    if (app < 0) return throw_result("run", full, app);
    int status = 0;
    n_result r = n_app_result(app, -1, &status);
    n_handle_close(app);
    return r < 0 ? throw_result("run", full, r) : JS_NewInt32(ctx, status);
}

/* ---------------------------------------------------------------- modules and promises */

static JSValue h_detect_module(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    bool module = JS_DetectModule(s, len);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, module);
}

/* A CommonJS module's function; the wrapper keeps its line numbers. */
static JSValue h_compile_function(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    static const char head[] = "(function (exports, require, module, __filename, __dirname) {";
    static const char tail[] = "\n})";
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    JSValue r = JS_EXCEPTION;
    char *code = s && name ? malloc(sizeof head + len + sizeof tail) : NULL;
    if (code) {
        memcpy(code, head, sizeof head - 1);
        memcpy(code + sizeof head - 1, s, len);
        memcpy(code + sizeof head - 1 + len, tail, sizeof tail);
        r = JS_Eval(ctx, code, sizeof head - 1 + len + sizeof tail - 1, name, JS_EVAL_TYPE_GLOBAL);
        free(code);
    } else if (s && name) r = JS_ThrowOutOfMemory(ctx);
    if (s) JS_FreeCString(ctx, s);
    if (name) JS_FreeCString(ctx, name);
    return r;
}

/* Unhandled rejections: QuickJS reports a rejection without a handler and
   later its handling; what is still unhandled after the microtasks is
   reported. The promise of the code being run (watched) is answered by its
   own caller. */
struct rejection { JSValue promise, reason; struct rejection *next; };
static struct rejection *rejections;
static JSValue watched = JS_UNDEFINED;

static bool same_object(JSValueConst a, JSValueConst b) {
    return JS_VALUE_GET_TAG(a) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(b) == JS_TAG_OBJECT &&
           JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

static void forget_rejection(JSValueConst promise) {
    for (struct rejection **p = &rejections; *p;) {
        struct rejection *r = *p;
        if (same_object(r->promise, promise)) {
            *p = r->next;
            JS_FreeValue(ctx, r->promise);
            JS_FreeValue(ctx, r->reason);
            free(r);
        } else p = &r->next;
    }
}

static void track_rejection(JSContext *c, JSValueConst promise, JSValueConst reason, JS_BOOL handled,
                            void *opaque) {
    if (handled) { forget_rejection(promise); return; }
    struct rejection *r = malloc(sizeof *r);
    if (!r) return;
    r->promise = JS_DupValue(ctx, promise);
    r->reason = JS_DupValue(ctx, reason);
    r->next = rejections;
    rejections = r;
}

static void report_rejections(void) {
    while (rejections) {
        struct rejection *r = rejections;
        rejections = r->next;
        if (!same_object(r->promise, watched)) {
            JSValue v = internal_call("unhandledRejection", 1, (JSValue[]){JS_DupValue(ctx, r->reason)});
            if (JS_IsException(v)) JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, v);
        }
        JS_FreeValue(ctx, r->promise);
        JS_FreeValue(ctx, r->reason);
        free(r);
    }
}

/* Runs the microtasks, then reports rejections nothing handled. */
static void drain(void) {
    for (;;) {
        JSContext *c;
        int r = JS_ExecutePendingJob(rt, &c);
        if (r == 0) break;
        if (r < 0) report_exception();
    }
    report_rejections();
}

/* require() of an ES module: its evaluation must finish in the microtasks. */
static JSValue h_await_sync(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    JSValue p = argv[0];
    for (;;) {
        int state = JS_PromiseState(ctx, p);
        if (state < 0) return JS_DupValue(ctx, p);
        if (state == JS_PROMISE_FULFILLED) return JS_PromiseResult(ctx, p);
        if (state == JS_PROMISE_REJECTED) {
            forget_rejection(p);
            return JS_Throw(ctx, JS_PromiseResult(ctx, p));
        }
        JSContext *jc;
        int r = JS_ExecutePendingJob(rt, &jc);
        if (r == 0)
            return JS_ThrowTypeError(ctx, "require() of an ES module that uses top-level await "
                                          "which waits for timers or the network: use import()");
        if (r < 0) report_exception();
    }
}

static JSValue h_promise_state(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    int state = JS_PromiseState(ctx, argv[0]);
    if (state < 0) return JS_UNDEFINED;
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, state));
    JS_SetPropertyUint32(ctx, a, 1, state == JS_PROMISE_PENDING ? JS_UNDEFINED : JS_PromiseResult(ctx, argv[0]));
    return a;
}

/* Each runtime file is the body of function (host, internal). */
static bool run_chunk(const char *name, const char *src, size_t len, const char *tail) {
    static const char head[] = "(function (host, internal) {\n";
    size_t tl = strlen(tail), total = sizeof head - 1 + len + 1 + tl + 4;
    char *code = malloc(total + 1);
    if (!code) { fputs("js: out of memory\n", stderr); exit(70); }
    size_t n = 0;
    memcpy(code, head, sizeof head - 1), n += sizeof head - 1;
    memcpy(code + n, src, len), n += len;
    code[n++] = '\n';
    memcpy(code + n, tail, tl), n += tl;
    memcpy(code + n, "\n})", 4), n += 3;
    char filename[64];
    snprintf(filename, sizeof filename, "nocturne:js/%s", name);
    JSValue fn = JS_Eval(ctx, code, n, filename, JS_EVAL_TYPE_GLOBAL);
    free(code);
    JSValue r = JS_IsException(fn) ? fn : JS_Call(ctx, fn, JS_UNDEFINED, 2, (JSValueConst[]){host, internal});
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r)) return false;
    JS_FreeValue(ctx, r);
    return true;
}

static JSValue h_load_chunk(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    bool ok = !strcmp(name, "url") ? run_chunk("url", src_url, sizeof src_url, "")
            : !strcmp(name, "crypto") ? run_chunk("crypto", src_crypto, sizeof src_crypto, "")
            : !strcmp(name, "ui") ? run_chunk("ui", src_ui, sizeof src_ui, "")
            : (JS_ThrowTypeError(ctx, "no chunk '%s'", name), false);
    JS_FreeCString(ctx, name);
    return ok ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue h_security_url(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    return JS_NewString(ctx, "file:///");
}

/* ---------------------------------------------------------------- fetch */

/* Each fetch runs on a thread of its own; finished ones wait in done until
   the event loop gives them to the runtime. */
struct fetch {
    struct fetch *next;
    int id;
    int cancelled; /* atomic */
    char *method, *url, *headers;
    void *body;
    size_t body_len;
    int rc;
    struct http_resp resp;
};
static mtx_t fetch_lock;
static struct fetch *fetch_running, *fetch_done;
static volatile uint32_t fetch_signal;
static int fetch_pending; /* fetches the runtime still waits for */

static void fetch_free(struct fetch *f) {
    http_resp_free(&f->resp);
    free(f->method);
    free(f->url);
    free(f->headers);
    free(f->body);
    free(f);
}

static int fetch_cancelled(void *p) { return __atomic_load_n(&((struct fetch *)p)->cancelled, __ATOMIC_ACQUIRE); }

static int fetch_thread(void *p) {
    struct fetch *f = p;
    struct http_req rq = {.method = f->method, .url = f->url, .headers = f->headers,
                          .body = f->body, .body_len = f->body_len, .timeout_ms = 60000, .ctx = f};
    struct http_stream_hooks hooks = {.cancelled = fetch_cancelled};
    f->rc = http_request_stream(&rq, &f->resp, 0, &hooks);
    mtx_lock(&fetch_lock);
    for (struct fetch **q = &fetch_running; *q; q = &(*q)->next)
        if (*q == f) { *q = f->next; break; }
    f->next = fetch_done;
    fetch_done = f;
    mtx_unlock(&fetch_lock);
    __atomic_add_fetch(&fetch_signal, 1, __ATOMIC_RELEASE);
    wake_address(&fetch_signal, 1);
    return 0;
}

static char *dup_arg(JSValueConst v) {
    const char *s = JS_ToCString(ctx, v);
    char *d = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(ctx, s);
    return d;
}

/* fetch(id, method, url, headers "Name: value\r\n"..., body ArrayBuffer | undefined) */
static JSValue h_fetch(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    struct fetch *f = calloc(1, sizeof *f);
    if (!f) return JS_ThrowOutOfMemory(ctx);
    f->id = arg_int(argv[0]);
    f->method = dup_arg(argv[1]);
    f->url = dup_arg(argv[2]);
    f->headers = dup_arg(argv[3]);
    bool ok = f->method && f->url && f->headers;
    if (ok && argc > 4 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])) {
        const uint8_t *data;
        const char *str;
        ok = arg_bytes(argv[4], &data, &f->body_len, &str);
        if (ok && f->body_len && (f->body = malloc(f->body_len))) memcpy(f->body, data, f->body_len);
        else if (ok && f->body_len) ok = false;
        if (str) JS_FreeCString(ctx, str);
    }
    if (!ok) {
        fetch_free(f);
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowOutOfMemory(ctx);
    }
    mtx_lock(&fetch_lock);
    f->next = fetch_running;
    fetch_running = f;
    mtx_unlock(&fetch_lock);
    thrd_t t;
    if (thrd_create(&t, fetch_thread, f) != thrd_success) {
        mtx_lock(&fetch_lock);
        for (struct fetch **q = &fetch_running; *q; q = &(*q)->next)
            if (*q == f) { *q = f->next; break; }
        mtx_unlock(&fetch_lock);
        fetch_free(f);
        return JS_ThrowInternalError(ctx, "fetch: cannot start a thread");
    }
    thrd_detach(t);
    fetch_pending++;
    return JS_UNDEFINED;
}

static JSValue h_fetch_cancel(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    int id = arg_int(argv[0]);
    bool found = false;
    mtx_lock(&fetch_lock);
    for (struct fetch *f = fetch_running; f; f = f->next)
        if (f->id == id && !f->cancelled) { __atomic_store_n(&f->cancelled, 1, __ATOMIC_RELEASE); found = true; }
    for (struct fetch *f = fetch_done; f; f = f->next)
        if (f->id == id && !f->cancelled) { f->cancelled = 1; found = true; }
    mtx_unlock(&fetch_lock);
    if (found) fetch_pending--;
    return JS_UNDEFINED;
}

static void deliver_fetches(void) {
    mtx_lock(&fetch_lock);
    struct fetch *list = fetch_done;
    fetch_done = NULL;
    mtx_unlock(&fetch_lock);
    while (list) {
        struct fetch *f = list;
        list = f->next;
        if (!f->cancelled) {
            fetch_pending--;
            JSValue o = JS_NewObject(ctx);
            if (f->rc < 0) {
                JS_SetPropertyStr(ctx, o, "error", JS_NewString(ctx, f->resp.error[0] ? f->resp.error : "network error"));
            } else {
                JS_SetPropertyStr(ctx, o, "status", JS_NewInt32(ctx, f->resp.status));
                JS_SetPropertyStr(ctx, o, "statusText", JS_NewString(ctx, http_response_status_text(&f->resp)));
                JS_SetPropertyStr(ctx, o, "headers", JS_NewString(ctx, http_response_headers(&f->resp)));
                JS_SetPropertyStr(ctx, o, "body", JS_NewArrayBufferCopy(ctx, (const uint8_t *)f->resp.body,
                                                                         f->resp.body ? f->resp.body_len : 0));
            }
            JSValue r = internal_call("fetchDone", 2, (JSValue[]){JS_NewInt32(ctx, f->id), o});
            if (JS_IsException(r)) report_exception();
            JS_FreeValue(ctx, r);
            drain();
        }
        fetch_free(f);
    }
}

/* ---------------------------------------------------------------- windows (nocturne/ui) */

/* The program's windows are one session of the interface engine (libc/ui.c):
   js_ui.js sends it views and requests as bytes, and the event loop hands its
   events back to internal.uiEvent. */
static struct ui_session *ui;

static struct ui_session *ui_get(void) {
    if (!ui) ui = ui_session_new();
    return ui;
}

static JSValue ui_bytes(const void *p, size_t n) { return JS_NewArrayBufferCopy(ctx, p, n); }

/* (message): 0 or a negative N_ERR_ */
static JSValue h_ui_send(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    size_t n;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, argv[0]);
    if (!p) return JS_EXCEPTION;
    struct ui_session *s = ui_get();
    return JS_NewInt32(ctx, s ? (int32_t)ui_session_send(s, p, n) : -N_ERR_NO_MEMORY);
}

/* (request): its reply, or null */
static JSValue h_ui_request(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    size_t n;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, argv[0]);
    if (!p) return JS_EXCEPTION;
    struct ui_session *s = ui_get();
    if (!s || !n || ui_session_send(s, p, n) != 0) return JS_NULL;
    size_t rn;
    const uint8_t *r = ui_session_peek(s, &rn);
    if (!r || r[0] != (uint8_t)(p[0] | 0x20)) return JS_NULL;
    JSValue reply = ui_bytes(r, rn);
    ui_session_pop(s);
    return reply;
}

static JSValue h_ui_launch(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    const char *arg = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToCString(ctx, argv[1]) : NULL;
    int r = gui_launch(path, arg);
    JS_FreeCString(ctx, path);
    if (arg) JS_FreeCString(ctx, arg);
    return JS_NewBool(ctx, r >= 0);
}

/* The desktop: screen size and depth, the compositor's settings, the volume. */
static JSValue h_desktop(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    struct desktop_settings s;
    if (__syscall(SYS_SCREEN_INFO, (long)&s, sizeof s, SCREEN_SETTINGS_GET, 0, 0) != 0) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    int sw = 0, sh = 0;
    struct n_sysinfo si;
    if (sysinfo(&si) == 0) sw = (int)si.fb_w, sh = (int)si.fb_h;
    else screen_size(&sw, &sh);
    n_result vol = n_audio_volume();
    JS_SetPropertyStr(ctx, o, "screenWidth", JS_NewInt32(ctx, sw));
    JS_SetPropertyStr(ctx, o, "screenHeight", JS_NewInt32(ctx, sh));
    JS_SetPropertyStr(ctx, o, "bitsPerChannel", JS_NewInt32(ctx, display_bits_per_channel()));
    JS_SetPropertyStr(ctx, o, "nativeBitsPerChannel", JS_NewInt32(ctx, s.native_bits_per_channel));
    JS_SetPropertyStr(ctx, o, "refreshRate", JS_NewInt32(ctx, (int32_t)s.refresh_hz));
    JS_SetPropertyStr(ctx, o, "pointerSpeed", JS_NewInt32(ctx, s.pointer_speed));
    JS_SetPropertyStr(ctx, o, "pointerY", JS_NewInt32(ctx, s.pointer_y));
    JS_SetPropertyStr(ctx, o, "pointerYDetected", JS_NewBool(ctx, s.pointer_y_detected));
    JS_SetPropertyStr(ctx, o, "deepColour", JS_NewBool(ctx, s.deep_simulation));
    JS_SetPropertyStr(ctx, o, "deepColourAvailable", JS_NewBool(ctx, s.deep_simulation_available));
    JS_SetPropertyStr(ctx, o, "volume", vol < 0 ? JS_NULL : JS_NewInt32(ctx, (int32_t)vol));
    return o;
}

/* (refresh, pointer speed, pointer y, deep colour): whether the desktop took them */
static JSValue h_desktop_set(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    struct desktop_settings s;
    if (__syscall(SYS_SCREEN_INFO, (long)&s, sizeof s, SCREEN_SETTINGS_GET, 0, 0) != 0) return JS_FALSE;
    int32_t refresh = 0, speed = 0, y = 0;
    JS_ToInt32(ctx, &refresh, argv[0]);
    JS_ToInt32(ctx, &speed, argv[1]);
    JS_ToInt32(ctx, &y, argv[2]);
    s.refresh_hz = (uint32_t)(refresh < 0 ? 0 : refresh);
    s.pointer_speed = (int8_t)speed;
    s.pointer_y = (uint8_t)y;
    s.deep_simulation = (uint8_t)JS_ToBool(ctx, argv[3]);
    return JS_NewBool(ctx, __syscall(SYS_SCREEN_INFO, (long)&s, sizeof s, SCREEN_SETTINGS_SET, 0, 0) == 0);
}

/* The computer: processor, memory, uptime, running tasks. */
static JSValue h_system_info(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    struct n_sysinfo si;
    if (sysinfo(&si) != 0) return JS_NULL;
    struct n_cpuinfo ci = {.version = 1};
    int cpus = cpu_info(&ci) == 0 ? (int)ci.online_cpus : 1;
    si.cpu[sizeof si.cpu - 1] = 0;
    si.os[sizeof si.os - 1] = 0;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "processor", JS_NewString(ctx, si.cpu));
    JS_SetPropertyStr(ctx, o, "processors", JS_NewInt32(ctx, cpus > 0 ? cpus : 1));
    JS_SetPropertyStr(ctx, o, "system", JS_NewString(ctx, si.os));
    JS_SetPropertyStr(ctx, o, "totalMemory", JS_NewFloat64(ctx, (double)si.total_mem));
    JS_SetPropertyStr(ctx, o, "freeMemory", JS_NewFloat64(ctx, (double)si.free_mem));
    JS_SetPropertyStr(ctx, o, "uptimeMs", JS_NewFloat64(ctx, (double)si.uptime_ms));
    JS_SetPropertyStr(ctx, o, "tasks", JS_NewInt32(ctx, si.ntasks));
    JS_SetPropertyStr(ctx, o, "screenWidth", JS_NewInt32(ctx, (int32_t)si.fb_w));
    JS_SetPropertyStr(ctx, o, "screenHeight", JS_NewInt32(ctx, (int32_t)si.fb_h));
    JS_SetPropertyStr(ctx, o, "bitsPerChannel", JS_NewInt32(ctx, display_bits_per_channel()));
    return o;
}

static JSValue h_set_volume(JSContext *c, JSValueConst this, int argc, JSValueConst *argv) {
    int32_t v = 0;
    JS_ToInt32(ctx, &v, argv[0]);
    n_result r = n_audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
    return r < 0 ? JS_NULL : JS_NewInt32(ctx, (int32_t)r);
}

/* Hands the engine's events to js_ui.js, one at a time. */
static void ui_deliver(void) {
    size_t n;
    const void *p;
    while (ui && (p = ui_session_peek(ui, &n))) {
        JSValue e = ui_bytes(p, n);
        ui_session_pop(ui);
        JSValue r = internal_call("uiEvent", 1, (JSValue[]){e});
        if (JS_IsException(r)) report_exception();
        JS_FreeValue(ctx, r);
        drain();
    }
}

static bool ui_open(void) { return ui && ui_session_open(ui); }

/* ---------------------------------------------------------------- event loop */

/* Runs tasks until nothing is left to wait for, or until the promise until
   (when given) settles. */
static void run_loop(JSValueConst until) {
    bool waiting = JS_VALUE_GET_TAG(until) == JS_TAG_OBJECT;
    for (;;) {
        uint32_t seen = __atomic_load_n(&fetch_signal, __ATOMIC_ACQUIRE);
        drain();
        deliver_fetches();
        if (ui) { ui_session_pump(ui); ui_deliver(); }
        for (;;) {
            JSValue now = JS_NewFloat64(ctx, (double)n_clock_uptime_ms());
            JSValue fired = internal_call("fireTimer", 1, (JSValue[]){now});
            bool any = JS_ToBool(ctx, fired);
            if (JS_IsException(fired)) report_exception();
            JS_FreeValue(ctx, fired);
            if (!any) break;
            drain();
            if (waiting && JS_PromiseState(ctx, until) != JS_PROMISE_PENDING) return;
        }
        if (waiting && JS_PromiseState(ctx, until) != JS_PROMISE_PENDING) return;
        JSValue next = internal_call("nextTimer", 0, NULL);
        double at = -1;
        JS_ToFloat64(ctx, &at, next);
        JS_FreeValue(ctx, next);
        if (JS_IsJobPending(rt)) continue;
        if (at < 0 && fetch_pending <= 0 && !ui_open()) return;
        uint64_t ms = 1000, t = n_clock_uptime_ms();
        if (at >= 0) ms = at <= (double)t ? 0 : (uint64_t)(at - (double)t + 0.999);
        if (ms > 1000) ms = 1000;
        if (!ms) continue;
        fflush(stdout);
        if (ui_open()) { ui_session_wait(ui, fetch_pending > 0 && ms > 10 ? 10 : (int)ms); ui_deliver(); } /* window events end the wait */
        else if (fetch_pending > 0) wait_on_address(&fetch_signal, seen, (unsigned)ms);
        else n_sleep_ms(ms);
    }
}

/* ---------------------------------------------------------------- module loader */

/* CommonJS, JSON and the "nocturne" module are synthetic modules whose
   exports the runtime names before they are linked. */
struct synthetic { JSModuleDef *m; char *path, *format; char **names; uint32_t count; struct synthetic *next; };
static struct synthetic *synthetics;

static int synthetic_init(JSContext *c, JSModuleDef *m) {
    struct synthetic *s = synthetics;
    while (s && s->m != m) s = s->next;
    if (!s) return -1;
    JSValue ns = internal_call("namespace", 2, (JSValue[]){JS_NewString(ctx, s->path), JS_NewString(ctx, s->format)});
    if (JS_IsException(ns)) return -1;
    for (uint32_t i = 0; i < s->count; i++)
        JS_SetModuleExport(ctx, m, s->names[i], JS_GetPropertyStr(ctx, ns, s->names[i]));
    JS_FreeValue(ctx, ns);
    return 0;
}

static char *module_normalize(JSContext *c, const char *base, const char *name, void *opaque) {
    JSValue r = internal_call("resolve", 2, (JSValue[]){JS_NewString(ctx, base), JS_NewString(ctx, name)});
    if (JS_IsException(r)) return NULL;
    const char *s = JS_ToCString(ctx, r);
    JS_FreeValue(ctx, r);
    if (!s) return NULL;
    char *out = js_strdup(ctx, s);
    JS_FreeCString(ctx, s);
    return out;
}

static bool fill_meta(JSModuleDef *m, const char *path, bool main) {
    JSValue meta = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta)) return false;
    JSValue r = internal_call("fillMeta", 3, (JSValue[]){meta, JS_NewString(ctx, path), JS_NewBool(ctx, main)});
    if (JS_IsException(r)) return false;
    JS_FreeValue(ctx, r);
    return true;
}

/* An ES module's compiled form (dependencies are loaded as it compiles). */
static JSValue compile_module(const char *path) {
    JSValue src = internal_call("source", 1, (JSValue[]){JS_NewString(ctx, path)});
    if (JS_IsException(src)) return src;
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, src);
    JS_FreeValue(ctx, src);
    if (!s) return JS_EXCEPTION;
    JSValue fn = JS_Eval(ctx, s, len, path, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    JS_FreeCString(ctx, s);
    return fn;
}

static JSModuleDef *module_load(JSContext *c, const char *path, void *opaque, JSValueConst attributes) {
    const char *format;
    JSValue fv = JS_UNDEFINED;
    if (!strcmp(path, "nocturne") || !strcmp(path, "nocturne/ui")) format = "nocturne";
    else {
        fv = internal_call("moduleFormat", 1, (JSValue[]){JS_NewString(ctx, path)});
        if (JS_IsException(fv)) return NULL;
        format = JS_ToCString(ctx, fv);
        if (!format) { JS_FreeValue(ctx, fv); return NULL; }
    }
    JSModuleDef *m = NULL;
    if (!strcmp(format, "module")) {
        JSValue fn = compile_module(path);
        if (!JS_IsException(fn)) {
            m = JS_VALUE_GET_PTR(fn);
            JS_FreeValue(ctx, fn);
            if (!fill_meta(m, path, false)) m = NULL;
        }
    } else {
        JSValue names = internal_call("exportNames", 2, (JSValue[]){JS_NewString(ctx, path), JS_NewString(ctx, format)});
        struct synthetic *s = calloc(1, sizeof *s);
        uint32_t count = 0;
        if (!JS_IsException(names)) {
            JSValue len = JS_GetPropertyStr(ctx, names, "length");
            JS_ToUint32(ctx, &count, len);
            JS_FreeValue(ctx, len);
        }
        if (s && !JS_IsException(names) && (s->names = calloc(count + 1, sizeof *s->names)) &&
            (s->path = strdup(path)) && (s->format = strdup(format)) &&
            (m = JS_NewCModule(ctx, path, synthetic_init))) {
            s->names[s->count++] = strdup("default");
            JS_AddModuleExport(ctx, m, "default");
            for (uint32_t i = 0; i < count; i++) {
                JSValue v = JS_GetPropertyUint32(ctx, names, i);
                const char *name = JS_ToCString(ctx, v);
                JS_FreeValue(ctx, v);
                if (!name) continue;
                if ((s->names[s->count] = strdup(name))) {
                    JS_AddModuleExport(ctx, m, name);
                    s->count++;
                }
                JS_FreeCString(ctx, name);
            }
            s->m = m;
            s->next = synthetics;
            synthetics = s;
        } else {
            if (s) { free(s->names); free(s->path); free(s->format); free(s); }
            if (!JS_IsException(names) && !JS_HasException(ctx)) JS_ThrowOutOfMemory(ctx);
        }
        JS_FreeValue(ctx, names);
    }
    if (!JS_IsUndefined(fv)) {
        JS_FreeCString(ctx, format);
        JS_FreeValue(ctx, fv);
    }
    return m;
}

/* ---------------------------------------------------------------- running code */

/* The promise of the running program: rejected ends it with the error. */
static int finish(JSValue promise, bool print) {
    watched = promise;
    run_loop(JS_UNDEFINED);
    int state = JS_PromiseState(ctx, promise);
    int status = 0;
    if (state == JS_PROMISE_REJECTED) {
        JSValue e = JS_PromiseResult(ctx, promise);
        JS_Throw(ctx, e);
        report_exception();
        status = 1;
    } else if (state == JS_PROMISE_PENDING) {
        fputs("Warning: the program ended with a top-level await that never settled\n", stderr);
        status = 13;
    } else if (print && state == JS_PROMISE_FULFILLED) {
        JSValue r = JS_PromiseResult(ctx, promise);
        JSValue v = JS_GetPropertyStr(ctx, r, "value");
        JSValue p = internal_call("replResult", 1, (JSValue[]){JS_DupValue(ctx, v)});
        if (JS_IsException(p)) report_exception();
        JS_FreeValue(ctx, p);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, r);
    }
    watched = JS_UNDEFINED;
    JS_FreeValue(ctx, promise);
    return status;
}

static int run_module(const char *path, JSValue fn) {
    if (JS_IsException(fn)) { report_exception(); return 1; }
    if (!fill_meta(JS_VALUE_GET_PTR(fn), path, true)) {
        JS_FreeValue(ctx, fn);
        report_exception();
        return 1;
    }
    JSValue p = JS_EvalFunction(ctx, fn);
    if (JS_IsException(p)) { report_exception(); return 1; }
    return finish(p, false);
}

static int run_file(const char *path) {
    JSValue fv = internal_call("moduleFormat", 1, (JSValue[]){JS_NewString(ctx, path)});
    if (JS_IsException(fv)) { report_exception(); return 1; }
    const char *format = JS_ToCString(ctx, fv);
    bool module = format && !strcmp(format, "module");
    if (format) JS_FreeCString(ctx, format);
    JS_FreeValue(ctx, fv);
    if (module) return run_module(path, compile_module(path));
    JSValue r = internal_call("runMain", 1, (JSValue[]){JS_NewString(ctx, path)});
    if (JS_IsException(r)) { report_exception(); return 1; }
    JS_FreeValue(ctx, r);
    run_loop(JS_UNDEFINED);
    return 0;
}

/* Code from -e, -p or the input: an ES module when it has import or export,
   otherwise a script where await works at the top level. */
static void eval_globals(void) {
    JSValue global = JS_GetGlobalObject(ctx);
    char base[PATH_CAP];
    if (n_path_complete("[eval]", base, sizeof base) < 0) strcpy(base, "C:\\[eval]");
    JSValue req = internal_call("requireFrom", 1, (JSValue[]){JS_NewString(ctx, base)});
    if (!JS_IsException(req)) JS_DefinePropertyValueStr(ctx, global, "require", req, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    else JS_FreeValue(ctx, JS_GetException(ctx));
    JS_DefinePropertyValueStr(ctx, global, "nocturne", JS_GetPropertyStr(ctx, internal, "nocturne"),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);
}

static int run_code(const char *code, size_t len, const char *name, bool print) {
    char path[PATH_CAP];
    if (n_path_complete(name, path, sizeof path) < 0) snprintf(path, sizeof path, "C:\\%s", name);
    eval_globals();
    if (!print && JS_DetectModule(code, len))
        return run_module(path, JS_Eval(ctx, code, len, path, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
    JSValue p = JS_Eval(ctx, code, len, path, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC);
    if (JS_IsException(p)) { report_exception(); return 1; }
    return finish(p, print);
}

/* ---------------------------------------------------------------- interactive prompt */

/* Whether the code so far leaves a bracket, string or comment open. */
static bool incomplete(const char *s) {
    int depth = 0;
    char quote = 0;
    for (; *s; s++) {
        if (quote) {
            if (*s == '\\' && s[1]) s++;
            else if (*s == quote) quote = 0;
            else if (*s == '\n' && quote != '`') return false;
        } else if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n') s++;
            if (!*s) break;
        } else if (s[0] == '/' && s[1] == '*') {
            const char *end = strstr(s + 2, "*/");
            if (!end) return true;
            s = end + 1;
        } else if (*s == '"' || *s == '\'' || *s == '`') quote = *s;
        else if (*s == '(' || *s == '[' || *s == '{') depth++;
        else if (*s == ')' || *s == ']' || *s == '}') depth--;
    }
    return quote == '`' || depth > 0;
}

static int repl(void) {
    JSValue interactive = JS_TRUE;
    JS_SetPropertyStr(ctx, internal, "interactive", interactive);
    eval_globals();
    printf("Welcome to js %s (QuickJS). Type .help for help, .exit or Ctrl+D to leave.\n", VERSION);
    enum { LINE = 64 * 1024 };
    char *line = malloc(LINE);
    size_t cap = 4096, n = 0;
    char *code = malloc(cap);
    if (!line || !code) return 70;
    for (;;) {
        fflush(stdout);
        int got = readline(n ? "... " : "> ", line, LINE, 0);
        if (got < 0) {
            if (n) { n = 0; putchar('\n'); continue; }
            break;
        }
        if (!n) {
            if (!strcmp(line, ".exit")) break;
            if (!strcmp(line, ".help")) {
                puts(".exit     leave (also Ctrl+D)\n"
                     ".help     this text\n"
                     "await works at the top level; _ is the last result.\n"
                     "nocturne (the system module) and require() are ready to use.");
                continue;
            }
            if (!got) continue;
        }
        if (n + (size_t)got + 2 > cap) {
            while (n + (size_t)got + 2 > cap) cap *= 2;
            char *more = realloc(code, cap);
            if (!more) { n = 0; continue; }
            code = more;
        }
        memcpy(code + n, line, (size_t)got);
        n += (size_t)got;
        code[n++] = '\n';
        code[n] = 0;
        if (incomplete(code)) continue;
        JSValue p = JS_Eval(ctx, code, n, "<repl>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_ASYNC);
        n = 0;
        if (JS_IsException(p)) { report_exception(); continue; }
        watched = p;
        run_loop(p);
        drain();
        int state = JS_PromiseState(ctx, p);
        if (state == JS_PROMISE_FULFILLED) {
            JSValue r = JS_PromiseResult(ctx, p);
            JSValue v = JS_GetPropertyStr(ctx, r, "value");
            JSValue global = JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx, global, "_", JS_DupValue(ctx, v));
            JS_FreeValue(ctx, global);
            JSValue shown = internal_call("replResult", 1, (JSValue[]){JS_DupValue(ctx, v)});
            if (JS_IsException(shown)) report_exception();
            JS_FreeValue(ctx, shown);
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, r);
        } else if (state == JS_PROMISE_REJECTED) {
            JS_Throw(ctx, JS_PromiseResult(ctx, p));
            report_exception();
        } else puts("(still waiting: nothing left that could settle it)");
        watched = JS_UNDEFINED;
        JS_FreeValue(ctx, p);
    }
    free(line);
    free(code);
    return 0;
}

/* ---------------------------------------------------------------- start */

static const JSCFunctionListEntry host_functions[] = {
    JS_CFUNC_DEF("write", 2, h_write),
    JS_CFUNC_DEF("isTerminal", 1, h_is_terminal),
    JS_CFUNC_DEF("now", 0, h_now),
    JS_CFUNC_DEF("exit", 1, h_exit),
    JS_CFUNC_DEF("readLine", 1, h_read_line),
    JS_CFUNC_DEF("readAll", 0, h_read_all),
    JS_CFUNC_DEF("readFile", 2, h_read_file),
    JS_CFUNC_DEF("writeFile", 3, h_write_file),
    JS_CFUNC_DEF("info", 1, h_info),
    JS_CFUNC_DEF("list", 1, h_list),
    JS_CFUNC_DEF("createFolder", 1, h_create_folder),
    JS_CFUNC_DEF("remove", 1, h_remove),
    JS_CFUNC_DEF("move", 2, h_move),
    JS_CFUNC_DEF("complete", 1, h_complete),
    JS_CFUNC_DEF("cwd", 0, h_cwd),
    JS_CFUNC_DEF("tempFolder", 0, h_temp_folder),
    JS_CFUNC_DEF("chdir", 1, h_chdir),
    JS_CFUNC_DEF("getSetting", 1, h_get_setting),
    JS_CFUNC_DEF("setSetting", 2, h_set_setting),
    JS_CFUNC_DEF("removeSetting", 1, h_remove_setting),
    JS_CFUNC_DEF("run", 2, h_run),
    JS_CFUNC_DEF("detectModule", 1, h_detect_module),
    JS_CFUNC_DEF("compileFunction", 2, h_compile_function),
    JS_CFUNC_DEF("awaitSync", 1, h_await_sync),
    JS_CFUNC_DEF("promiseState", 1, h_promise_state),
    JS_CFUNC_DEF("loadChunk", 1, h_load_chunk),
    JS_CFUNC_DEF("securityURL", 0, h_security_url),
    JS_CFUNC_DEF("fetch", 5, h_fetch),
    JS_CFUNC_DEF("fetchCancel", 1, h_fetch_cancel),
    JS_CFUNC_DEF("uiSend", 1, h_ui_send),
    JS_CFUNC_DEF("uiRequest", 1, h_ui_request),
    JS_CFUNC_DEF("uiLaunch", 2, h_ui_launch),
    JS_CFUNC_DEF("desktop", 0, h_desktop),
    JS_CFUNC_DEF("desktopSet", 4, h_desktop_set),
    JS_CFUNC_DEF("setVolume", 1, h_set_volume),
    JS_CFUNC_DEF("systemInfo", 0, h_system_info),
};

static void usage(FILE *f) {
    fputs("usage: js [FILE.js | FILE.ts] [ARGUMENTS]\n"
          "       js -e CODE [ARGUMENTS]    run code\n"
          "       js -p CODE [ARGUMENTS]    run code and print the result\n"
          "       js - [ARGUMENTS]          run the code from the input\n"
          "       js                        interactive prompt (or the input when it is not a terminal)\n"
          "       js --init [FOLDER]        start a TypeScript project for tsgo and js\n"
          "       js --version\n"
          "Files are ES modules, CommonJS or JSON. A .ts, .mts or .cts file is type-checked\n"
          "and compiled with tsgo, then run. Programs import the system from \"nocturne\".\n", f);
}

int main(int argc, char **argv) {
    enum { FILE_, EVAL, PRINT, INPUT, REPL, INIT } mode = REPL;
    const char *target = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) { printf("js %s (QuickJS)\n", VERSION); return 0; }
        if (!strcmp(a, "-e") || !strcmp(a, "--eval") || !strcmp(a, "-p") || !strcmp(a, "--print")) {
            if (i + 1 >= argc) { fprintf(stderr, "js: %s needs code\n", a); return 2; }
            mode = a[1] == 'e' || a[2] == 'e' ? EVAL : PRINT;
            target = argv[++i];
            i++;
            break;
        }
        if (!strcmp(a, "--init")) {
            mode = INIT;
            if (i + 1 < argc) target = argv[++i];
            i++;
            break;
        }
        if (!strcmp(a, "-")) { mode = INPUT; i++; break; }
        if (!strcmp(a, "--")) { i++; if (i < argc) { mode = FILE_; target = argv[i++]; } break; }
        if (a[0] == '-') { fprintf(stderr, "js: unknown option %s\n", a); usage(stderr); return 2; }
        mode = FILE_;
        target = argv[i++];
        break;
    }
    if (mode == REPL && !n_terminal(0)) mode = INPUT;

    if (mtx_init(&fetch_lock, mtx_plain) != thrd_success) return 70;
    rt = web_js_new_runtime(NULL);
    if (!rt) return 70;
    JS_SetMaxStackSize(rt, 4u << 20); /* the main thread has 8 MiB */
    ctx = JS_NewContext(rt);
    if (!ctx) return 70;
    JS_SetModuleLoaderFunc2(rt, module_normalize, module_load, NULL, NULL);
    JS_SetHostPromiseRejectionTracker(rt, track_rejection, NULL);
    host = JS_NewObject(ctx);
    internal = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, host, host_functions, sizeof host_functions / sizeof *host_functions);
    web_js_encoding_init_isolated(ctx, host);
    web_js_crypto_init(ctx, host);
    JSValue args = JS_NewArray(ctx);
    for (uint32_t n = 0; i < argc; i++, n++) JS_SetPropertyUint32(ctx, args, n, JS_NewString(ctx, argv[i]));
    JS_SetPropertyStr(ctx, host, "args", args);

    if (!run_chunk("dom_exception", src_dom_exception, sizeof src_dom_exception,
                   "Object.defineProperty(globalThis,'DOMException',{value:domExceptionBridge.DOMException,writable:true,configurable:true});") ||
        !run_chunk("encoding", src_encoding, sizeof src_encoding, "") ||
        !run_chunk("runtime", src_runtime, sizeof src_runtime, "") ||
        !run_chunk("console", src_console, sizeof src_console, "") ||
        !run_chunk("web", src_web, sizeof src_web, "")) {
        report_exception();
        return 70;
    }

    int status = 0;
    switch (mode) {
    case REPL:
        status = repl();
        break;
    case INIT: {
        JSValue r = internal_call("init", 1, (JSValue[]){target ? JS_NewString(ctx, target) : JS_UNDEFINED});
        if (JS_IsException(r)) { report_exception(); status = 1; }
        JS_FreeValue(ctx, r);
        break;
    }
    case EVAL:
    case PRINT:
        status = run_code(target, strlen(target), "[eval]", mode == PRINT);
        break;
    case INPUT: {
        size_t len;
        char *code = read_input(&len);
        if (!code) { fputs("js: out of memory\n", stderr); return 70; }
        status = run_code(code, len, "[input]", false);
        free(code);
        break;
    }
    case FILE_: {
        char path[PATH_CAP];
        if (n_path_complete(target, path, sizeof path) < 0) { fprintf(stderr, "js: path too long\n"); return 2; }
        struct n_entry e;
        if (n_entry_info(path, &e) < 0 || e.kind != N_KIND_DOCUMENT) {
            fprintf(stderr, "js: cannot find %s\n", path);
            return 2;
        }
        size_t len = strlen(path);
        bool ts = (len > 3 && !strcmp(path + len - 3, ".ts")) ||
                  (len > 4 && (!strcmp(path + len - 4, ".mts") || !strcmp(path + len - 4, ".cts")));
        if (ts && !(len > 5 && !strcmp(path + len - 5, ".d.ts"))) {
            JSValue r = internal_call("compileTypeScript", 1, (JSValue[]){JS_NewString(ctx, path)});
            if (JS_IsException(r)) { report_exception(); return 1; }
            const char *out = JS_ToCString(ctx, r);
            JS_FreeValue(ctx, r);
            if (!out) { report_exception(); return 1; }
            snprintf(path, sizeof path, "%s", out);
            JS_FreeCString(ctx, out);
        }
        status = run_file(path);
        break;
    }
    }
    fflush(stdout);
    fflush(stderr);
    return status;
}
