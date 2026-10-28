/* CSS Font Loading host (https://drafts.csswg.org/css-font-loading-3/) for
   js_font_loading.js. FontFace records, the @font-face rules of a document
   and font matching belong to css.c; their resource requests to request_fonts.
   This realm's objects follow the document through one "font loading" task
   (web_js_tick) whenever doc->font_generation moves or the document stops
   being pending on the environment while document.fonts is stuck on it. */
static JSClassID font_face_class;
static void font_face_finalizer(JSRuntime *rt, JSValue value) {
    (void)rt;
    css_font_face_release(JS_GetOpaque(value, font_face_class));
}

/* CSS Font Loading 3: "pending on the environment". The document is still
   loading, a stylesheet it asked for has not arrived, a font its rendering
   selected has not been requested yet, or its layout is not current. While
   stuck, one completed layout since then satisfies the last condition, so a
   continuously restyled page still settles document.fonts.ready. */
static bool fonts_environment_pending(struct web_js_state *s) {
    web_doc *d = s->doc;
    if (!d || !d->live || d->inert || s->disabled) return false;
    if (!s->parsing_done) return true;
    if (doc_css_loading(d) || doc_fonts_unrequested(d)) return true;
    bool layout_dirty = d->resources_dirty || d->need_style || d->need_boxes || !d->layout_valid || d->font_selection_dirty;
    /* A hidden document has no rendering opportunity (observers wait the same way). */
    if (!d->visible || !layout_dirty) return false;
    return !s->fonts_stuck || d->layout_revision == s->fonts_layout_mark;
}
static bool fonts_task_due(struct web_js_state *s) {
    if (!s || s->disabled || !s->fonts_active || !s->doc) return false;
    return s->fonts_seen != __atomic_load_n(&s->doc->font_generation, __ATOMIC_RELAXED) ||
           (s->fonts_stuck && !fonts_environment_pending(s));
}

/* A document font source this realm may change: its own document, or an
   independent document of its family (DOMParser, createHTMLDocument). */
static web_doc *fonts_document(struct web_js_state *s, JSValueConst value) {
    node_t *n = unwrap(s->ctx, value);
    if (!n) { if (!JS_HasException(s->ctx)) JS_ThrowTypeError(s->ctx, "Document required"); return NULL; }
    web_doc *t = n->type == N_DOC ? n->owner : NULL;
    web_doc *family = s->doc->dom_family ? s->doc->dom_family : s->doc;
    if (!t || t->root != n || (t != s->doc && !(t->inert && t->dom_family == family))) {
        JS_ThrowTypeError(s->ctx, "Document of this realm required"); return NULL;
    }
    if (t->resources_dirty) doc_rescan(t);
    return t;
}
static struct css_font_face *fonts_record(JSContext *ctx, JSValueConst value) {
    if (!font_face_class) { JS_ThrowTypeError(ctx, "FontFace record required"); return NULL; }
    return JS_GetOpaque2(ctx, value, font_face_class);
}
static JSValue fonts_string_result(JSContext *ctx, int status, const char *text) {
    if (status < 0) return oom(ctx);
    return status ? JS_NewString(ctx, text ? text : "") : JS_NULL;
}
/* The rule a FontFace key (id, occurrence) names in the document, if any
   (0 also while the rules are unavailable: the caller keeps what it had). */
static int fonts_rule(JSContext *ctx, web_doc *d, JSValueConst id_value, JSValueConst occurrence_value,
                      struct css_font_rule *out) {
    double id; uint32_t occurrence;
    if (JS_ToFloat64(ctx, &id, id_value) || JS_ToUint32(ctx, &occurrence, occurrence_value)) return -1;
    struct css_font_rule *rules; size_t count;
    int listed = css_font_rules(d, &rules, &count);
    if (listed < 0) { oom(ctx); return -1; }
    int found = 0;
    for (size_t i = 0; i < count; i++)
        if ((double)rules[i].id == id && rules[i].occurrence == occurrence) { *out = rules[i]; found = 1; break; }
    free(rules);
    return found;
}
static JSValue native_fonts(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    struct web_js_state *s = state(ctx);
    if (!s || !s->doc || !argc) return JS_ThrowTypeError(ctx, "Font loading host is inactive");
    const char *op = JS_ToCString(ctx, argv[0]);
    if (!op) return JS_EXCEPTION;
    JSValue result = JS_UNDEFINED;
    web_doc *d = s->doc;
    if (!strcmp(op, "create")) {
        if (!font_face_class) JS_NewClassID(&font_face_class);
        if (!JS_IsRegisteredClass(JS_GetRuntime(ctx), font_face_class)) {
            JSClassDef definition = {.class_name = "NocturneFontFace", .finalizer = font_face_finalizer};
            if (JS_NewClass(JS_GetRuntime(ctx), font_face_class, &definition) < 0) { result = oom(ctx); goto out; }
        }
        struct css_font_face *f = css_font_face_new();
        if (!f) { result = oom(ctx); goto out; }
        result = JS_NewObjectProtoClass(ctx, JS_NULL, font_face_class);
        if (JS_IsException(result)) css_font_face_release(f); else JS_SetOpaque(result, f);
        /* Objects made later never skip progress the earlier ones wait for. */
        if (!s->fonts_active) s->fonts_seen = __atomic_load_n(&d->font_generation, __ATOMIC_RELAXED);
        s->fonts_active = true;
    } else if (!strcmp(op, "activate")) {
        if (!s->fonts_active) s->fonts_seen = __atomic_load_n(&d->font_generation, __ATOMIC_RELAXED);
        s->fonts_active = true;
    } else if (!strcmp(op, "set")) {
        /* set(record|null, descriptor, value): null only validates. */
        uint32_t which; size_t n;
        if (argc < 4 || JS_ToUint32(ctx, &which, argv[2])) { result = JS_EXCEPTION; goto out; }
        if (which >= CSS_FONT_DESCRIPTORS) { result = JS_ThrowRangeError(ctx, "Unknown font descriptor"); goto out; }
        struct css_font_face *f = JS_IsNull(argv[1]) ? NULL : fonts_record(ctx, argv[1]);
        if (!f && !JS_IsNull(argv[1])) { result = JS_EXCEPTION; goto out; }
        const char *value = JS_ToCStringLen(ctx, &n, argv[3]);
        if (!value) { result = JS_EXCEPTION; goto out; }
        if (f) {
            int status = css_font_face_set(d, f, which, value, n);
            result = fonts_string_result(ctx, status, status > 0 ? css_font_face_descriptor(f, which) : NULL);
        } else {
            char *serialized = NULL;
            int status = css_font_descriptor(which, value, n, &serialized);
            result = fonts_string_result(ctx, status, serialized);
            free(serialized);
        }
        JS_FreeCString(ctx, value);
    } else if (!strcmp(op, "source") || !strcmp(op, "data")) {
        struct css_font_face *f = argc > 2 ? fonts_record(ctx, argv[1]) : NULL;
        if (!f) { result = argc > 2 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Font source required"); goto out; }
        int status;
        if (op[0] == 's') {
            size_t n; const char *value = JS_ToCStringLen(ctx, &n, argv[2]);
            if (!value) { result = JS_EXCEPTION; goto out; }
            /* URLs resolve against the document's base URL as its rules do. */
            if (d->resources_dirty) doc_rescan(d);
            status = css_font_face_source(d, f, d->base, value, n);
            JS_FreeCString(ctx, value);
        } else {
            size_t n = 0; uint8_t *bytes = JS_GetArrayBuffer(ctx, &n, argv[2]);
            if (!bytes && JS_HasException(ctx)) { result = JS_EXCEPTION; goto out; }
            status = css_font_face_data(d, f, bytes, n);
        }
        result = status < 0 ? oom(ctx) : JS_NewBool(ctx, status > 0);
    } else if (!strcmp(op, "load") || !strcmp(op, "status") || !strcmp(op, "id")) {
        struct css_font_face *f = argc > 1 ? fonts_record(ctx, argv[1]) : NULL;
        if (!f) { result = argc > 1 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "FontFace record required"); goto out; }
        if (op[0] == 'i') result = JS_NewFloat64(ctx, (double)css_font_face_id(f));
        else result = JS_NewInt32(ctx, css_font_face_status(d, f, op[0] == 'l'));
    } else if (!strcmp(op, "add") || !strcmp(op, "delete")) {
        web_doc *t = argc > 2 ? fonts_document(s, argv[1]) : NULL;
        struct css_font_face *f = t ? fonts_record(ctx, argv[2]) : NULL;
        if (!f) { result = JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Font source required"); goto out; }
        if (op[0] == 'a') { if (!css_font_set_add(t, f)) result = oom(ctx); }
        else css_font_set_delete(t, f);
    } else if (!strcmp(op, "clear")) {
        web_doc *t = argc > 1 ? fonts_document(s, argv[1]) : NULL;
        if (!t) { result = JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Document required"); goto out; }
        css_font_set_clear(t);
    } else if (!strcmp(op, "rules")) {
        /* The document's CSS-connected faces in document order: id, occurrence, status. */
        web_doc *t = argc > 1 ? fonts_document(s, argv[1]) : NULL;
        if (!t) { result = JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Document required"); goto out; }
        struct css_font_rule *rules; size_t count;
        int listed = css_font_rules(t, &rules, &count);
        if (listed < 0) { result = oom(ctx); goto out; }
        /* Unavailable now: null, and the realm keeps the membership it had. */
        if (!listed) { result = JS_NULL; goto out; }
        result = JS_NewArray(ctx);
        for (size_t i = 0; i < count && !JS_IsException(result); i++) {
            int status = t->live && !t->inert ? css_font_rule_status(t, rules[i].face, false) : CSS_FONT_UNLOADED;
            if (JS_SetPropertyUint32(ctx, result, (uint32_t)(3 * i), JS_NewFloat64(ctx, (double)rules[i].id)) < 0 ||
                JS_SetPropertyUint32(ctx, result, (uint32_t)(3 * i + 1), JS_NewUint32(ctx, rules[i].occurrence)) < 0 ||
                JS_SetPropertyUint32(ctx, result, (uint32_t)(3 * i + 2), JS_NewInt32(ctx, status)) < 0) {
                JS_FreeValue(ctx, result); result = JS_EXCEPTION;
            }
        }
        free(rules);
    } else if (!strcmp(op, "rule") || !strcmp(op, "ruleSet") || !strcmp(op, "ruleLoad")) {
        web_doc *t = argc > 3 ? fonts_document(s, argv[1]) : NULL;
        if (!t) { result = JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Font rule required"); goto out; }
        struct css_font_rule rule;
        int found = fonts_rule(ctx, t, argv[2], argv[3], &rule);
        if (found < 0) { result = JS_EXCEPTION; goto out; }
        if (!found) { result = JS_NULL; goto out; }
        if (!strcmp(op, "rule")) {
            result = JS_NewArray(ctx);
            for (uint32_t i = 0; i < CSS_FONT_DESCRIPTORS && !JS_IsException(result); i++) {
                const char *text = css_font_rule_descriptor(rule.face, i);
                if (JS_SetPropertyUint32(ctx, result, i, JS_NewString(ctx, text ? text : "")) < 0) {
                    JS_FreeValue(ctx, result); result = JS_EXCEPTION;
                }
            }
        } else if (!strcmp(op, "ruleLoad")) {
            /* Without a browsing context to fetch for it, a load fails. */
            result = JS_NewInt32(ctx, t->live && !t->inert ? css_font_rule_status(t, rule.face, true) : CSS_FONT_ERROR);
        } else {
            uint32_t which; size_t n;
            if (argc < 6 || JS_ToUint32(ctx, &which, argv[4])) { result = argc < 6 ? JS_ThrowTypeError(ctx, "Font descriptor required") : JS_EXCEPTION; goto out; }
            if (which >= CSS_FONT_DESCRIPTORS) { result = JS_ThrowRangeError(ctx, "Unknown font descriptor"); goto out; }
            const char *value = JS_ToCStringLen(ctx, &n, argv[5]);
            if (!value) { result = JS_EXCEPTION; goto out; }
            int status = css_font_rule_set(t, &rule, which, value, n);
            result = fonts_string_result(ctx, status, status > 0 ? css_font_rule_descriptor(rule.face, which) : NULL);
            JS_FreeCString(ctx, value);
        }
    } else if (!strcmp(op, "match")) {
        /* match(document, font, text): null for a syntax error, otherwise
           rule id, occurrence and record id (0 for a rule) per face. */
        web_doc *t = argc > 3 ? fonts_document(s, argv[1]) : NULL;
        if (!t) { result = JS_HasException(ctx) ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Font required"); goto out; }
        size_t fn, tn;
        const char *font = JS_ToCStringLen(ctx, &fn, argv[2]);
        const char *text = font ? JS_ToCStringLen(ctx, &tn, argv[3]) : NULL;
        if (!font || !text) { JS_FreeCString(ctx, font); result = JS_EXCEPTION; goto out; }
        struct css_font_match *matches = NULL; size_t count = 0;
        int status = css_font_match(t, font, fn, text, tn, &matches, &count);
        JS_FreeCString(ctx, font); JS_FreeCString(ctx, text);
        if (status < 0) { result = oom(ctx); goto out; }
        if (!status) { result = JS_NULL; goto out; }
        result = JS_NewArray(ctx);
        for (size_t i = 0; i < count && !JS_IsException(result); i++) {
            double author = matches[i].author ? (double)css_font_face_id(matches[i].author) : 0;
            if (JS_SetPropertyUint32(ctx, result, (uint32_t)(3 * i), JS_NewFloat64(ctx, (double)matches[i].id)) < 0 ||
                JS_SetPropertyUint32(ctx, result, (uint32_t)(3 * i + 1), JS_NewUint32(ctx, matches[i].occurrence)) < 0 ||
                JS_SetPropertyUint32(ctx, result, (uint32_t)(3 * i + 2), JS_NewFloat64(ctx, author)) < 0) {
                JS_FreeValue(ctx, result); result = JS_EXCEPTION;
            }
        }
        free(matches);
    } else if (!strcmp(op, "pending")) {
        result = JS_NewBool(ctx, fonts_environment_pending(s));
    } else if (!strcmp(op, "stuck")) {
        bool stuck = argc > 1 && JS_ToBool(ctx, argv[1]) > 0;
        if (stuck && !s->fonts_stuck) s->fonts_layout_mark = d->layout_revision;
        s->fonts_stuck = stuck;
    } else result = JS_ThrowTypeError(ctx, "Unknown font loading operation");
out:
    JS_FreeCString(ctx, op);
    return result;
}
