/* Browser-process BroadcastChannel registry. Independent JS contexts receive
   engine-serialized structured-clone records, never another realm's objects.
   Separate browser processes/workers do not yet share this registry. */
struct js_broadcast {
    struct js_broadcast *next;
    struct web_js_state *state;
    JSValue callback;
    char *name;
    size_t name_len;
    char *origin; /* Full native-owned tuple; NULL only for an opaque origin. */
    uint32_t id;
};
static struct js_broadcast *broadcasts;
static unsigned broadcast_count;
static uint32_t broadcast_serial;
static bool broadcast_id_wrapped;
static uint32_t broadcast_id(void){
    for(;;){uint32_t id=++broadcast_serial;if(!id){broadcast_id_wrapped=true;continue;}
        if(!broadcast_id_wrapped)return id;
        struct js_broadcast *b=broadcasts;while(b&&b->id!=id)b=b->next;if(!b)return id;}
}
struct broadcast_delivery {struct js_posted_task *task;struct web_js_state *target;};

static void broadcast_remove(struct js_broadcast **link) {
    struct js_broadcast *b = *link; *link = b->next;
    JSContext *ctx = b->state->ctx;
    JS_FreeValue(ctx, b->callback); free(b->origin); js_free(ctx, b->name); js_free(ctx, b);
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
    if(sandbox_borrowed(s) || (sandbox_authority(s)&SB_SCRIPTS))return history_security_error(ctx,"Sandbox denied borrowed broadcast authority");
    if (argc < 2 || JS_ToInt32(ctx, &op, argv[0])) return JS_EXCEPTION;
    if (op == 0) {
        if (argc < 3 || !JS_IsFunction(ctx, argv[2])) return JS_ThrowTypeError(ctx, "Expected channel callback");
        if(broadcast_count==UINT_MAX)return JS_ThrowOutOfMemory(ctx);
        size_t length; const char *name = JS_ToCStringLen(ctx, &length, argv[1]);
        if (!name) return JS_EXCEPTION;
        if(length>UINT32_MAX||length==SIZE_MAX){JS_FreeCString(ctx,name);return JS_ThrowOutOfMemory(ctx);}
        struct js_broadcast *b = js_mallocz(ctx, sizeof *b);
        if (!b) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        b->name = js_malloc(ctx, length + 1);
        if (!b->name) { js_free(ctx, b); JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        memcpy(b->name, name, length + 1); b->name_len = length; JS_FreeCString(ctx, name);
        const char *url = web_effective_url(s->doc);
        enum http_url_result origin_status = make_origin(url, &b->origin);
        if (origin_status == HTTP_URL_OOM || origin_status == HTTP_URL_INVALID) {
            free(b->origin); js_free(ctx,b->name); js_free(ctx,b);
            return origin_status == HTTP_URL_OOM ? JS_ThrowOutOfMemory(ctx) :
                JS_ThrowTypeError(ctx,"Channel origin is unavailable");
        }
        b->state = s; b->callback = JS_DupValue(ctx, argv[2]);
        b->id=broadcast_id();
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
    if(length>UINT32_MAX){js_free(ctx,bytes);return JS_ThrowOutOfMemory(ctx);}
    struct broadcast_delivery *pending=NULL;size_t count=0,capacity=0;
    for (struct js_broadcast *b = broadcasts; b; b = b->next) {
        /* Opaque origins are distinct even when both serialize to 'null'. */
        if (b == sender || b->state->disabled || b->name_len != sender->name_len ||
            memcmp(b->name, sender->name, b->name_len) ||
            (sender->origin ? !b->origin || strcmp(b->origin, sender->origin) :
                b->origin || b->state != s)) continue;
        struct web_js_state *target = b->state; unsigned reserved = 0;
        for(size_t i=0;i<count;i++)if(pending[i].target==target)reserved++;
        if(target->posted_count>UINT_MAX-1u-reserved)goto failed;
        if(count==capacity){
            size_t maximum=SIZE_MAX/sizeof *pending;if(maximum>UINT_MAX)maximum=UINT_MAX;
            if(capacity>=maximum)goto failed;
            size_t next=capacity?capacity>maximum/2?maximum:capacity*2:8;
            if(next>maximum)next=maximum;
            struct broadcast_delivery *grown=js_realloc(ctx,pending,next*sizeof *pending);
            if(!grown){JS_FreeValue(ctx,JS_GetException(ctx));goto failed;}
            pending=grown;capacity=next;
        }
        JSContext *dest = target->ctx;
        JSValue record = JS_ReadObject(dest, bytes, length, JS_READ_OBJ_REFERENCE);
        if (JS_IsException(record)) { JS_FreeValue(dest, JS_GetException(dest)); goto failed; }
        JSValue data[] = {b->callback, record};
        JSValue fn = JS_NewCFunctionData(dest, broadcast_deliver, 0, 0, 2, data);
        JS_FreeValue(dest, record);
        if (JS_IsException(fn)) { JS_FreeValue(dest, JS_GetException(dest)); goto failed; }
        struct js_posted_task *p = js_malloc(dest, sizeof *p);
        if (!p) { JS_FreeValue(dest, fn); JS_FreeValue(dest, JS_GetException(dest)); goto failed; }
        p->fn=fn;p->next=NULL;pending[count++]=(struct broadcast_delivery){p,target};
    }
    js_free(ctx, bytes);
    /* Publish only after all allocation succeeds: no partial send on failure. */
    for(size_t i=0;i<count;i++){
        struct web_js_state *target=pending[i].target;struct js_posted_task *p=pending[i].task;
        p->id=allocate_posted_id(target);
        if (target->last_posted) target->last_posted->next = p; else target->posted = p;
        target->last_posted = p; target->posted_count++;
    }
    js_free(ctx,pending);
    return JS_UNDEFINED;
failed:
    js_free(ctx, bytes);
    for(size_t i=0;i<count;i++){JSContext *dest=pending[i].target->ctx;
        JS_FreeValue(dest,pending[i].task->fn);js_free(dest,pending[i].task);}
    js_free(ctx,pending);return JS_ThrowOutOfMemory(ctx);
}
