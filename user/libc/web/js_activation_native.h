/* DOM "dispatch"'s activation (https://dom.spec.whatwg.org/#concept-event-dispatch steps 6.4,
 * 6.5, 6.9.5.1, 6.9.7.2, 6.12 and 12) and the HTML activation behaviors it runs. The owner of
 * both: every click MouseEvent, whoever made it (the user's click or keyboard activation, the
 * page's click() or dispatchEvent()), reaches these through one dispatch.
 *
 *   js_bootstrap.js dispatch ── host.activation(0) "has activation behavior", chosen on the path
 *                            ── host.activation(1) legacy-pre-activation, before any listener
 *                            ── host.activation(2) activation behavior, when not canceled
 *                            ── host.activation(3) legacy-canceled-activation, when canceled
 *   activation_dispatch_native: the same steps where no realm runs listeners (a sandbox without
 *                               allow-scripts, a realm stopped by its watchdog).
 *
 * Included by js.c: the behaviors keep the realm's sandbox authority (sandbox_authority, SB_*),
 * permitted_url for script-made navigations, and the document's inert and dialog-inert state. */

enum activation_kind { ACTIVATION_NONE, ACTIVATION_HYPERLINK, ACTIVATION_INPUT, ACTIVATION_BUTTON,
    ACTIVATION_LABEL, ACTIVATION_SUMMARY, ACTIVATION_MEDIA };
/* Which elements have activation behavior. HTML gives it to every a, area, input, button, label
   and summary element (each returns early where it does nothing); a media element with controls
   has the platform's play/pause click. */
static enum activation_kind activation_kind(const node_t *n) {
    if (!n || n->type != N_ELEM || n->foreign) return ACTIVATION_NONE;
    switch (n->tag) {
    case T_a: case T_area: return ACTIVATION_HYPERLINK;
    case T_input: return ACTIVATION_INPUT;
    case T_button: return ACTIVATION_BUTTON;
    case T_label: return ACTIVATION_LABEL;
    case T_summary: return ACTIVATION_SUMMARY;
    case T_audio: case T_video: return node_attr(n, "controls") ? ACTIVATION_MEDIA : ACTIVATION_NONE;
    default: return ACTIVATION_NONE;
    }
}
/* The click activation runs for: its realm and Event when a JS dispatch runs it, the native
   click otherwise; target is the event's target as it was dispatched (before retargeting);
   ctrl and shift are the user's own keys, for a trusted click only. */
struct activation_event {
    struct web_js_state *realm;
    JSValueConst event;
    const struct web_event *native;
    node_t *target;
    bool trusted, ctrl, shift;
};
static bool activation_dispatch_native(web_doc *d, node_t *target, const struct web_event *event);
/* A native wait inside a script task (a modal picker) is not the script's time. */
static void activation_waited(web_doc *d, uint64_t waited) {
    struct web_js_state *s = d ? d->js : NULL;
    if (!s) return;
    for (struct web_js_state *active = profile_task(s); active; active = active->runtime_previous)
        if (active->running) { active->task_deadline += waited; active->task_native_wait_ms += waited; }
}
/* An event the user agent fires at a control as its activation's consequence (input, change,
   cancel): trusted, never cancelable. */
static void activation_fire(web_doc *d, node_t *n, const char *type, bool bubbles) {
    struct web_event e = {.type = type, .bubbles = bubbles};
    web_js_dispatch(d, n, &e);
}

/* ---- a and area: HTML "follow the hyperlink" ---- */
/* "Evaluate a javascript: URL": the URL serialization without its scheme, percent-decoded,
   run as a classic script of the target document's realm. A string result would replace the
   document; that is not implemented. */
static JSValue activation_javascript_run(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    struct web_js_state *s = state(ctx);
    if (s->disabled || !task_context_active(s) || (s->doc->sandbox_flags & SB_SCRIPTS)) return JS_UNDEFINED;
    size_t length = 0; const char *code = JS_ToCStringLen(ctx, &length, data[0]);
    if (!code) return JS_EXCEPTION;
    JSValue result = JS_Eval(ctx, code, length, s->doc->url ? s->doc->url : "about:blank", JS_EVAL_TYPE_GLOBAL);
    JS_FreeCString(ctx, code);
    if (JS_IsException(result)) return result;
    if (JS_IsString(result)) log_text(s, 1, "A javascript: URL's string result does not replace the document (not implemented)");
    JS_FreeValue(ctx, result);
    return JS_UNDEFINED;
}
static int activation_hex(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
/* HTML "navigate" for a javascript: URL the source document's hyperlink chose for target's
   navigable: the initiator must be same origin-domain with the target's active document, the
   source's policies' inline check of type "navigation" must allow the URL, and the script runs
   in a task of the target's realm. */
static void activation_javascript_url(web_doc *source, web_doc *target, const char *url) {
    if (!web_frame_same_origin(source, target)) { web_js_console(source, 1, "Cross-origin javascript: navigation blocked"); return; }
    struct web_js_state *initiator = source->js;
    struct html_csp_request navigation = {.url = url, .original_url = url};
    struct csp_site site = {initiator, NULL};
    bool reported = initiator && !initiator->disabled;
    if (!html_policy_navigation_allowed(source, &navigation, false, reported ? csp_report : NULL, &site)) {
        web_js_console(source, 1, "javascript: navigation blocked by Content Security Policy"); return;
    }
    struct web_js_state *s = target->js;
    if (!s || s->disabled || !task_context_active(s) || (target->sandbox_flags & SB_SCRIPTS)) return;
    if (s->posted_count == UINT_MAX) return;
    const char *encoded = url + 11; size_t length = strlen(encoded);
    char *decoded = malloc(length + 1); size_t used = 0;
    if (!decoded) { log_text(s, 2, "javascript: URL allocation failed"); return; }
    for (size_t i = 0; i < length; i++) {
        int high, low;
        if (encoded[i] == '%' && i + 2 < length && (high = activation_hex((unsigned char)encoded[i + 1])) >= 0 &&
            (low = activation_hex((unsigned char)encoded[i + 2])) >= 0) { decoded[used++] = (char)(high << 4 | low); i += 2; }
        else decoded[used++] = encoded[i];
    }
    JSValue code = JS_NewStringLen(s->ctx, decoded, used);
    free(decoded);
    if (JS_IsException(code)) { exception(s); return; }
    JSValue fn = JS_NewCFunctionData(s->ctx, activation_javascript_run, 0, 0, 1, &code);
    JS_FreeValue(s->ctx, code);
    if (JS_IsException(fn)) { exception(s); return; }
    struct js_posted_task *p = js_malloc(s->ctx, sizeof *p);
    if (!p) { JS_FreeValue(s->ctx, fn); exception(s); return; }
    p->fn = fn; p->next = NULL; p->id = allocate_posted_id(s);
    if (s->last_posted) s->last_posted->next = p; else s->posted = p;
    s->last_posted = p; s->posted_count++;
    target->dirty = true;
}
static void activation_navigate(web_doc *d, node_t *n, const char *url, int disposition) {
    struct web_navigation navigation = {.url = url, .history = WEB_NAVIGATE_PUSH, .disposition = disposition};
    web_navigation_source(d, n, &navigation);
    if (d->host.navigate) d->host.navigate(d->host.opaque, &navigation);
    else web_js_console(d, 2, "The embedder cannot follow this hyperlink");
}
static void activation_hyperlink(struct activation_event *a, node_t *n) {
    web_doc *d = n->owner;
    if (!d || !node_attr(n, "href")) return;
    /* "cannot navigate": not fully active, or an area that is not connected (an a element is
       exempt from connectedness). An element inert under a modal dialog does not navigate. */
    if (!web_doc_fully_active(d) || (n->tag == T_area && !doc_node_connected(n)) || web_dialog_inert(d, n)) return;
    struct web_js_state *realm = a->realm;
    if (node_attr(n, "download") && ((d->sandbox_flags & SB_DOWNLOADS) || (realm && (sandbox_authority(realm) & SB_DOWNLOADS)))) {
        web_js_console(d, 1, "Sandbox denied link download"); return;
    }
    if (d->resources_dirty) doc_sync_tree(d);
    const char *href = node_attr(n, "href");
    char *url = NULL;
    if (!href || doc_encoding_parse_url(d, href, &url) != 1) { free(url); return; }
    bool script = web_url_has_scheme(url, "javascript");
    /* A navigation script made goes only where the page's own requests may. The user's click
       is the browser's to judge (its host refuses a remote page's local file). */
    if (!script && !a->trusted && realm && !permitted_url(realm, url, false)) { free(url); return; }
    web_doc *top = d; while (top->frame_parent) top = top->frame_parent;
    bool activated = a->trusted || d->sandbox_activation_until > uptime_ms();
    if (!script && a->trusted && (a->ctrl || a->shift)) {
        /* The user's preference for a new top-level traversable: Ctrl a tab behind this one,
           Shift a window. A sandbox without allow-popups opens none. */
        if (web_sandbox_auxiliary_allowed(d)) activation_navigate(d, n, url, a->ctrl ? WEB_DISPOSITION_TAB : WEB_DISPOSITION_WINDOW);
        else web_js_console(d, 1, "Sandbox auxiliary policy denied link navigation");
    } else if (!web_frame_navigate(top, n, url, activated)) {
        /* The chosen navigable is the top-level one. */
        if (script) activation_javascript_url(d, top, url);
        else activation_navigate(d, n, url, WEB_DISPOSITION_CURRENT);
    }
    free(url);
}

/* ---- input ---- */
/* HTML "show the picker, if applicable" for a file input: the relevant global's transient
   activation is required and consumed. The picker is the native modal window; its result fires
   input and change, or cancel. */
static void activation_file(node_t *n) {
    web_doc *d = n->owner;
    if (!d || d->sandbox_activation_until <= uptime_ms() || web_control_disabled(n)) return;
    web_user_activation_consume(d);
    if (!web_doc_fully_active(d)) return;
    uint64_t started = uptime_ms();
    bool chosen = web_input_choose_files(d, n);
    activation_waited(d, uptime_ms() - started);
    if (n->owner != d) return;
    if (chosen) { activation_fire(d, n, "input", true); activation_fire(d, n, "change", true); }
    else activation_fire(d, n, "cancel", true);
    d->dirty = true;
}
/* The submit algorithm a submit button's activation behavior runs, under the realm's sandbox
   authority when script made the click. */
static void activation_submit(struct activation_event *a, node_t *n) {
    if (a->realm && (sandbox_authority(a->realm) & SB_FORMS)) { log_text(a->realm, 1, "Sandbox denied form submission"); return; }
    web_form_submit(n->owner, n);
}
static void activation_input(struct activation_event *a, node_t *n) {
    web_doc *d = n->owner;
    if (!d) return;
    enum web_input_kind kind = web_input_type(n);
    /* "If element is not mutable and is in neither the Checkbox nor the Radio state". */
    if (kind != WEB_INPUT_CHECKBOX && kind != WEB_INPUT_RADIO && web_control_disabled(n)) return;
    switch (kind) {
    case WEB_INPUT_CHECKBOX: case WEB_INPUT_RADIO:
        if (!doc_node_connected(n)) break;
        activation_fire(d, n, "input", true);
        activation_fire(d, n, "change", true);
        d->dirty = true;
        break;
    case WEB_INPUT_SUBMIT: case WEB_INPUT_IMAGE:
        if (!web_form_owner(d, n) || !web_doc_fully_active(d)) break;
        /* The coordinate the user selected with this click (doc.c web_dispatch). */
        if (kind == WEB_INPUT_IMAGE && a->trusted && d->image_activation == n) {
            n->image_submit_x = d->image_activation_x; n->image_submit_y = d->image_activation_y;
        }
        activation_submit(a, n);
        break;
    case WEB_INPUT_RESET:
        if (!web_form_owner(d, n) || !web_doc_fully_active(d)) break;
        web_reset(d, n);
        break;
    case WEB_INPUT_FILE:
        activation_file(n);
        break;
    default: break; /* Color's picker, and the popover target of a Button-state input: not implemented. */
    }
}
static void activation_button(struct activation_event *a, node_t *n) {
    web_doc *d = n->owner;
    if (!d || web_control_disabled(n) || !web_doc_fully_active(d) || !web_form_owner(d, n)) return;
    if (web_control_submit_button(n)) { activation_submit(a, n); return; }
    const char *type = node_attr(n, "type");
    if (type && str_ieq(type, "reset")) web_reset(d, n);
    /* commandfor and popovertarget: not implemented. */
}

/* ---- label and summary ---- */
static void activation_label(struct activation_event *a, node_t *label) {
    node_t *control = web_label_activation(label, a->target);
    if (!control || web_control_disabled(control) || !control->owner) return;
    web_doc *d = control->owner;
    if (a->trusted && web_dialog_inert(d, control)) return;
    web_js_focus_control(d, control);
    /* The control gets a click of the label's: the same trust, pointer and keys. */
    struct web_js_state *realm = a->realm;
    if (realm && !realm->disabled && task_context_active(realm)) {
        JSValue args[] = {wrap(realm, control), JS_DupValue(realm->ctx, a->event)};
        JSValue result = JS_IsException(args[0]) ? JS_EXCEPTION : custom_element_hook(realm, "activationForward", 2, args);
        if (JS_IsException(result)) exception(realm); else JS_FreeValue(realm->ctx, result);
        JS_FreeValue(realm->ctx, args[0]); JS_FreeValue(realm->ctx, args[1]);
        return;
    }
    struct web_event click = {.type = "click", .bubbles = true, .cancelable = true, .synthetic = !a->trusted,
        .keyboard_activation = a->native ? a->native->keyboard_activation : false,
        .ctrl = a->ctrl, .shift = a->shift};
    if (a->native) { click.x = a->native->x; click.y = a->native->y; click.alt = a->native->alt; }
    web_js_dispatch(d, control, &click);
}
static void activation_summary(struct activation_event *a, node_t *summary) {
    /* "If this summary element is not the summary for its parent details, then return"; a click
       at interactive content inside the summary keeps its own. */
    node_t *details = doc_details_activation(a->target ? a->target : summary);
    if (details && doc_details_summary(details) == summary) doc_details_toggle(summary->owner, details);
}

static void activation_run(struct activation_event *a, node_t *n) {
    switch (activation_kind(n)) {
    case ACTIVATION_HYPERLINK: activation_hyperlink(a, n); break;
    case ACTIVATION_INPUT: activation_input(a, n); break;
    case ACTIVATION_BUTTON: activation_button(a, n); break;
    case ACTIVATION_LABEL: activation_label(a, n); break;
    case ACTIVATION_SUMMARY: activation_summary(a, n); break;
    case ACTIVATION_MEDIA: web_media_activate(n->owner, n); break;
    default: break;
    }
}

/* host.activation(op, node, ...), from js_bootstrap.js dispatch:
     0 node: whether it has activation behavior
     1 node: its legacy-pre-activation behavior; the record for 3, or undefined
     2 node, event, target, isTrusted, ctrl, shift: its activation behavior
     3 node, record: its legacy-canceled-activation behavior
     4 form, submitter: requestSubmit()'s submit algorithm (submitter is the form for none) */
static JSValue native_activation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    struct web_js_state *s = state(ctx);
    int32_t op = -1;
    if (argc < 2 || JS_ToInt32(ctx, &op, argv[0])) return JS_EXCEPTION;
    node_t *n = node_opaque(argv[1]);
    switch (op) {
    case 0: return JS_NewBool(ctx, n && n->owner && activation_kind(n) != ACTIVATION_NONE);
    case 1: {
        if (!n || !n->owner || activation_kind(n) != ACTIVATION_INPUT) return JS_UNDEFINED;
        struct web_input_activation record;
        web_input_pre_activation(n->owner, n, &record);
        if (!record.used) return JS_UNDEFINED;
        n->owner->dirty = true;
        JSValue value = JS_NewArray(ctx);
        if (JS_IsException(value)) return value;
        JSValue previous = record.previous ? wrap(s, record.previous) : JS_NULL;
        if (JS_IsException(previous) || JS_SetPropertyUint32(ctx, value, 0, JS_NewBool(ctx, record.checked)) < 0 ||
            JS_SetPropertyUint32(ctx, value, 1, JS_NewBool(ctx, record.indeterminate)) < 0 ||
            JS_SetPropertyUint32(ctx, value, 2, previous) < 0) { JS_FreeValue(ctx, value); return JS_EXCEPTION; }
        return value;
    }
    case 2: {
        if (!n || !n->owner || argc < 7) return JS_UNDEFINED;
        /* A sandboxed actor's script reaching another document's element (dispatchEvent there)
           lends it none of that document's authority, as click() does not. */
        if ((sandbox_authority(s) & SB_SCRIPTS) || (sandbox_borrowed(s) && n->owner != sandbox_actor(s)->doc)) {
            log_text(s, 1, "Sandbox denied borrowed activation authority"); return JS_UNDEFINED;
        }
        struct activation_event a = {.realm = s, .event = argv[2], .target = node_opaque(argv[3]),
            .trusted = JS_ToBool(ctx, argv[4]) > 0};
        a.ctrl = a.trusted && JS_ToBool(ctx, argv[5]) > 0; a.shift = a.trusted && JS_ToBool(ctx, argv[6]) > 0;
        if (s->doc->resources_dirty) doc_rescan(s->doc);
        activation_run(&a, n);
        return JS_UNDEFINED;
    }
    case 3: {
        if (!n || !n->owner || argc < 3 || !JS_IsObject(argv[2])) return JS_UNDEFINED;
        struct web_input_activation record = {.used = true};
        JSValue checked = JS_GetPropertyUint32(ctx, argv[2], 0), indeterminate = JS_GetPropertyUint32(ctx, argv[2], 1),
            previous = JS_GetPropertyUint32(ctx, argv[2], 2);
        record.checked = JS_ToBool(ctx, checked) > 0; record.indeterminate = JS_ToBool(ctx, indeterminate) > 0;
        record.previous = node_opaque(previous);
        JS_FreeValue(ctx, checked); JS_FreeValue(ctx, indeterminate); JS_FreeValue(ctx, previous);
        web_input_canceled_activation(n->owner, n, &record);
        n->owner->dirty = true;
        return JS_UNDEFINED;
    }
    case 4: {
        node_t *submitter = argc > 2 ? node_opaque(argv[2]) : NULL;
        if (!n || !n->owner || !submitter) return JS_UNDEFINED;
        if ((sandbox_authority(s) & SB_SCRIPTS) || (sandbox_borrowed(s) && n->owner != sandbox_actor(s)->doc))
            return history_security_error(ctx, "Sandbox denied borrowed form submission authority");
        if (sandbox_authority(s) & SB_FORMS) { log_text(s, 1, "Sandbox denied form submission"); return JS_UNDEFINED; }
        web_form_submit(n->owner, submitter);
        return JS_UNDEFINED;
    }
    default: return JS_ThrowRangeError(ctx, "Unknown activation step");
    }
}

/* DOM "get the parent" for a click (composed): the assigned slot, else the parent; a shadow
   root's is its host. */
static node_t *activation_parent(node_t *n) {
    if (n->shadow_host) return n->shadow_host;
    if (n->parent && n->parent->shadow_root) return doc_assigned_slot(n, false);
    return n->parent;
}
/* A click where no realm runs listeners (none can cancel it): the activation target is the
   first element with activation behavior on its path, as a bubbling click's. */
static bool activation_dispatch_native(web_doc *d, node_t *target, const struct web_event *event) {
    if (!d || !target || !event || !event->type || strcmp(event->type, "click")) return true;
    if (!event->synthetic && web_dialog_inert(d, target)) return false;
    if (!event->synthetic) d->sandbox_activation_until = uptime_ms() + 5000;
    node_t *chosen = NULL;
    for (node_t *n = target; n && !chosen; n = activation_parent(n))
        if (n->owner && activation_kind(n) != ACTIVATION_NONE) chosen = n;
    if (!chosen) return true;
    struct web_input_activation record;
    if (activation_kind(chosen) == ACTIVATION_INPUT) { web_input_pre_activation(chosen->owner, chosen, &record); chosen->owner->dirty = true; }
    struct activation_event a = {.native = event, .target = target, .trusted = !event->synthetic};
    a.ctrl = a.trusted && event->ctrl; a.shift = a.trusted && event->shift;
    activation_run(&a, chosen);
    return true;
}
