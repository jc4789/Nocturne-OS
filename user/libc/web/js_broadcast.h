/* Browser-process BroadcastChannel registry. Independent JS contexts receive
   engine-serialized structured-clone records, never another realm's objects.
   Separate browser processes/workers do not yet share this registry. */
struct js_broadcast {
    struct js_broadcast *next;
    struct web_js_state *state;
    JSValue callback;
    char *name;
    size_t name_len;
    char origin[256];
    uint32_t id;
};
static struct js_broadcast *broadcasts;
static unsigned broadcast_count;
static uint32_t broadcast_serial;

static void broadcast_remove(struct js_broadcast **link) {
    struct js_broadcast *b = *link; *link = b->next;
    JSContext *ctx = b->state->ctx;
    JS_FreeValue(ctx, b->callback); js_free(ctx, b->name); js_free(ctx, b);
    broadcast_count--;
}
static void broadcast_free(struct web_js_state *s) {
    struct js_broadcast **link = &broadcasts;
    while (*link) { if ((*link)->state == s) broadcast_remove(link); else link = &(*link)->next; }
}
static JSValue broadcast_deliver(JSContext *ctx, JSValueConst this_val, int argc,
                                 JSValueConst *argv, int magic, JSValue *data) {
    return JS_Call(ctx, data[0], JS_UNDEFINED, 1, &data[1]);
}
static JSValue native_broadcast(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    struct web_js_state *s = state(ctx); int32_t op;
    if (argc < 2 || JS_ToInt32(ctx, &op, argv[0])) return JS_EXCEPTION;
    if (op == 0) {
        if (argc < 3 || !JS_IsFunction(ctx, argv[2])) return JS_ThrowTypeError(ctx, "Expected channel callback");
        if (broadcast_count >= 256) return JS_ThrowRangeError(ctx, "Browser channel quota reached");
        size_t length; const char *name = JS_ToCStringLen(ctx, &length, argv[1]);
        if (!name) return JS_EXCEPTION;
        if (length > 4096) { JS_FreeCString(ctx, name); return JS_ThrowRangeError(ctx, "Channel name quota reached"); }
        struct js_broadcast *b = js_mallocz(ctx, sizeof *b);
        if (!b) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        b->name = js_malloc(ctx, length + 1);
        if (!b->name) { js_free(ctx, b); JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        memcpy(b->name, name, length + 1); b->name_len = length; JS_FreeCString(ctx, name);
        const char *url = web_effective_url(s->doc);
        bool origin_ok = make_origin(url, b->origin, sizeof b->origin);
        if (!origin_ok && (!strncasecmp(url,"http://",7) || !strncasecmp(url,"https://",8))) {
            js_free(ctx,b->name); js_free(ctx,b); return JS_ThrowTypeError(ctx,"Channel origin is unavailable");
        }
        b->state = s; b->callback = JS_DupValue(ctx, argv[2]);
        b->id = ++broadcast_serial; if (!b->id) b->id = ++broadcast_serial;
        /* Registration order is preserved within this native event loop. */
        struct js_broadcast **tail = &broadcasts; while (*tail) tail = &(*tail)->next;
        *tail = b; broadcast_count++; return JS_NewUint32(ctx, b->id);
    }
    uint32_t id; if (JS_ToUint32(ctx, &id, argv[1])) return JS_EXCEPTION;
    struct js_broadcast **link = &broadcasts;
    while (*link && ((*link)->id != id || (*link)->state != s)) link = &(*link)->next;
    if (!*link) return op == 1 ? JS_UNDEFINED : JS_ThrowTypeError(ctx, "Closed channel");
    if (op == 1) { broadcast_remove(link); return JS_UNDEFINED; }
    if (op != 2 || argc < 3) return JS_ThrowTypeError(ctx, "Invalid channel operation");
    struct js_broadcast *sender = *link;
    size_t length; uint8_t *bytes = JS_WriteObject(ctx, &length, argv[2], JS_WRITE_OBJ_REFERENCE);
    if (!bytes) return JS_EXCEPTION;
    if (length > (1u << 20)) { js_free(ctx, bytes); return JS_ThrowRangeError(ctx, "Channel message quota reached"); }
    struct js_posted_task *pending[256]; struct web_js_state *targets[256]; unsigned count = 0;
    for (struct js_broadcast *b = broadcasts; b; b = b->next) {
        /* Opaque origins are distinct even when both serialize to 'null'. */
        if (b == sender || b->state->disabled || b->name_len != sender->name_len ||
            memcmp(b->name, sender->name, b->name_len) || strcmp(b->origin, sender->origin) ||
            (!strcmp(sender->origin, "null") && b->state != s)) continue;
        struct web_js_state *target = b->state; unsigned reserved = 0;
        for (unsigned i = 0; i < count; i++) if (targets[i] == target) reserved++;
        if (target->posted_count + reserved >= 4096) goto failed;
        JSContext *dest = target->ctx;
        JSValue record = JS_ReadObject(dest, bytes, length, JS_READ_OBJ_REFERENCE);
        if (JS_IsException(record)) { JS_FreeValue(dest, JS_GetException(dest)); goto failed; }
        JSValue data[] = {b->callback, record};
        JSValue fn = JS_NewCFunctionData(dest, broadcast_deliver, 0, 0, 2, data);
        JS_FreeValue(dest, record);
        if (JS_IsException(fn)) { JS_FreeValue(dest, JS_GetException(dest)); goto failed; }
        struct js_posted_task *p = js_malloc(dest, sizeof *p);
        if (!p) { JS_FreeValue(dest, fn); JS_FreeValue(dest, JS_GetException(dest)); goto failed; }
        p->fn = fn; p->next = NULL; pending[count] = p; targets[count++] = target;
    }
    js_free(ctx, bytes);
    /* Publish only after all allocation succeeds: no partial send on failure. */
    for (unsigned i = 0; i < count; i++) {
        struct web_js_state *target = targets[i]; struct js_posted_task *p = pending[i];
        p->id = ++target->next_posted; if (!p->id) p->id = ++target->next_posted;
        if (target->last_posted) target->last_posted->next = p; else target->posted = p;
        target->last_posted = p; target->posted_count++;
    }
    return JS_UNDEFINED;
failed:
    js_free(ctx, bytes);
    for (unsigned i = 0; i < count; i++) { JSContext *dest = targets[i]->ctx;
        JS_FreeValue(dest, pending[i]->fn); js_free(dest, pending[i]); }
    return JS_ThrowRangeError(ctx, "Channel delivery allocation or task quota reached");
}
