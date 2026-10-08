/* HTML constraint validation. Native control state is the single source of
   truth, including GUI edits and scripting-disabled documents.
   https://html.spec.whatwg.org/multipage/form-control-infrastructure.html
   https://html.spec.whatwg.org/multipage/input.html */
#include "form_validation.h"
#include "form_file.h"
#include "form_value.h"
#include "nocturne.h"
#include "quickjs.h"
#include "libregexp.h"
#include "js_form_url.inc"
#include <limits.h>

#define VALIDATION_REGEX_CACHE 16
#define VALIDATION_PATTERN_LIMIT 65536u
#define VALIDATION_INPUT_LIMIT (16u << 20)
#define VALIDATION_MEMORY_LIMIT (16u << 20)
#define VALIDATION_MATCH_MS 20u
#define VALIDATION_URL_STARTUP_MS 1000u

struct validation_pattern {
    char *text;
    uint8_t *bytecode;
    uint64_t used;
    bool invalid;
};
struct validation_state {
    JSRuntime *rt;
    JSContext *ctx;
    JSContext *url_ctx;
    JSValue url_check;
    uint64_t deadline, serial;
    bool url_ready;
    struct validation_pattern patterns[VALIDATION_REGEX_CACHE];
};

static bool html_element(const node_t *n) {
    return n && n->type == N_ELEM && !n->foreign;
}
static bool contains(const node_t *ancestor, const node_t *n) {
    for (; n; n = n->parent) if (n == ancestor) return true;
    return false;
}
/* A normal tree walk never enters shadow or template trees. Their root can
   itself be supplied when a detached/shadow-local form is being validated. */
static node_t *tree_next(node_t *root, node_t *n) {
    if (n->first) return n->first;
    while (n != root && !n->next) n = n->parent;
    return n == root ? NULL : n->next;
}
static bool readonly_applies(const node_t *n) {
    if (!html_element(n)) return false;
    if (n->tag == T_textarea) return true;
    if (n->tag != T_input) return false;
    enum web_input_kind type = web_input_type(n);
    return type <= WEB_INPUT_PASSWORD || (type >= WEB_INPUT_DATE && type <= WEB_INPUT_NUMBER);
}
static bool text_constraints(const node_t *n) {
    return html_element(n) && n->tag == T_input && web_input_type(n) <= WEB_INPUT_PASSWORD;
}
bool web_control_validation_interface(const node_t *n) {
    if (!html_element(n)) return false;
    return n->tag == T_input || n->tag == T_button || n->tag == T_select ||
           n->tag == T_textarea || n->tag == T_fieldset || n->tag == T_object || n->tag == T_output;
}
bool web_control_submit_button(const node_t *n) {
    if (!html_element(n)) return false;
    if (n->tag == T_input) return web_input_type(n) == WEB_INPUT_SUBMIT || web_input_type(n) == WEB_INPUT_IMAGE;
    if (n->tag != T_button) return false;
    const char *type = node_attr(n, "type");
    if (type && str_ieq(type, "submit")) return true;
    if (type && (str_ieq(type, "button") || str_ieq(type, "reset"))) return false;
    /* The standard's Auto state is not a submit button when it is a select
       child or is wired to a command. No command implementation is invented. */
    return !node_attr(n, "command") && !node_attr(n, "commandfor") &&
           !(html_element(n->parent) && n->parent->tag == T_select);
}
bool web_control_disabled(const node_t *n) {
    if (!html_element(n)) return false;
    if (n->tag == T_option) {
        return node_attr(n, "disabled") || (n->parent && n->parent->tag == T_optgroup && node_attr(n->parent, "disabled"));
    }
    if (n->tag == T_optgroup) return node_attr(n, "disabled") != NULL;
    if (!(n->tag == T_input || n->tag == T_button || n->tag == T_select || n->tag == T_textarea || n->tag == T_fieldset)) return false;
    if (node_attr(n, "disabled")) return true;
    for (const node_t *p = n->parent; p; p = p->parent) {
        if (!html_element(p) || p->tag != T_fieldset || !node_attr(p, "disabled")) continue;
        const node_t *legend = NULL;
        for (const node_t *c = p->first; c; c = c->next)
            if (html_element(c) && c->tag == T_legend) { legend = c; break; }
        if (!legend || !contains(legend, n)) return true;
    }
    return false;
}
bool web_control_required_applicable(const node_t *n) {
    if (!html_element(n)) return false;
    if (n->tag == T_select || n->tag == T_textarea) return true;
    if (n->tag != T_input) return false;
    enum web_input_kind type = web_input_type(n);
    return type <= WEB_INPUT_PASSWORD || (type >= WEB_INPUT_DATE && type <= WEB_INPUT_NUMBER) ||
           type == WEB_INPUT_CHECKBOX || type == WEB_INPUT_RADIO || type == WEB_INPUT_FILE;
}
bool web_control_required(const node_t *n) {
    return web_control_required_applicable(n) && node_attr(n, "required");
}
bool web_control_read_write(const node_t *n) {
    return readonly_applies(n) && !web_control_disabled(n) && !node_attr(n, "readonly");
}
bool web_control_will_validate(const node_t *n) {
    if (!web_control_validation_interface(n) || web_control_disabled(n)) return false;
    if (n->tag == T_fieldset || n->tag == T_output || n->tag == T_object) return false;
    if (readonly_applies(n) && node_attr(n, "readonly")) return false;
    if (n->tag == T_input) {
        enum web_input_kind type = web_input_type(n);
        if (type == WEB_INPUT_HIDDEN || type == WEB_INPUT_RESET || type == WEB_INPUT_BUTTON) return false;
    }
    if (n->tag == T_button && !web_control_submit_button(n)) return false;
    for (const node_t *p = n->parent; p; p = p->parent)
        if (html_element(p) && p->tag == T_datalist) return false;
    return true;
}

static int validation_interrupt(JSRuntime *rt, void *opaque) {
    (void)rt;
    struct validation_state *s = opaque;
    return uptime_ms() >= s->deadline;
}
static struct validation_state *validation_state(web_doc *d) {
    /* Inert/template documents share a lifetime owner and need no separate
       pure parser realm. This also bounds a family's aggregate regex/URL heap. */
    if (d->dom_family) d = d->dom_family;
    if (d->form_validation_state) return d->form_validation_state;
    struct validation_state *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->url_check = JS_UNDEFINED;
    s->rt = JS_NewRuntime();
    if (!s->rt) { free(s); return NULL; }
    JS_SetMemoryLimit(s->rt, VALIDATION_MEMORY_LIMIT);
    JS_SetMaxStackSize(s->rt, 256u << 10);
    s->ctx = JS_NewContext(s->rt);
    if (!s->ctx) { JS_FreeRuntime(s->rt); free(s); return NULL; }
    JS_SetInterruptHandler(s->rt, validation_interrupt, s);
    d->form_validation_state = s;
    return s;
}
void web_form_validation_free(web_doc *d) {
    struct validation_state *s = d ? d->form_validation_state : NULL;
    if (!s) return;
    d->form_validation_state = NULL;
    for (int i = 0; i < VALIDATION_REGEX_CACHE; i++) {
        free(s->patterns[i].text);
        lre_realloc(s->ctx, s->patterns[i].bytecode, 0);
    }
    if (s->url_ctx) {
        JS_FreeValue(s->url_ctx, s->url_check);
        JS_FreeContext(s->url_ctx);
    }
    JS_FreeContext(s->ctx); JS_FreeRuntime(s->rt); free(s);
}

static JSValue url_encode(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t len = 0;
    const char *text = argc ? JS_ToCStringLen(ctx, &len, argv[0]) : "";
    if (!text) return JS_EXCEPTION;
    JSValue result = JS_NewArrayBufferCopy(ctx, (const uint8_t *)text, len);
    if (argc) JS_FreeCString(ctx, text);
    return result;
}
static JSValue url_decode(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t len = 0;
    uint8_t *bytes = argc ? JS_GetArrayBuffer(ctx, &len, argv[0]) : NULL;
    if (argc && !bytes) return JS_EXCEPTION;
    if (len > VALIDATION_INPUT_LIMIT) return JS_ThrowRangeError(ctx, "UTF-8 decode limit");
    char *out = malloc(len * 3 + 1);
    if (!out) return JS_ThrowOutOfMemory(ctx);
    size_t written = 0;
    uint32_t codepoint = 0;
    unsigned needed = 0, seen = 0, low = 0x80, high = 0xbf;
    /* WHATWG UTF-8 replacement decoding, including reconsumption after an
       invalid continuation. JS_NewStringLen alone accepts surrogate UTF-8
       and is therefore not a standards-compliant TextDecoder. */
    for (size_t i = 0; i < len; i++) {
        unsigned byte = bytes[i];
        if (!needed) {
            if (byte <= 0x7f) { out[written++] = (char)byte; continue; }
            if (byte >= 0xc2 && byte <= 0xdf) { needed = 1; codepoint = byte & 31; continue; }
            if (byte >= 0xe0 && byte <= 0xef) {
                needed = 2; codepoint = byte & 15;
                if (byte == 0xe0) low = 0xa0;
                if (byte == 0xed) high = 0x9f;
                continue;
            }
            if (byte >= 0xf0 && byte <= 0xf4) {
                needed = 3; codepoint = byte & 7;
                if (byte == 0xf0) low = 0x90;
                if (byte == 0xf4) high = 0x8f;
                continue;
            }
            written += (size_t)utf8_put(out + written, 0xfffd); continue;
        }
        if (byte < low || byte > high) {
            needed = seen = 0; low = 0x80; high = 0xbf; codepoint = 0;
            written += (size_t)utf8_put(out + written, 0xfffd); i--; continue;
        }
        low = 0x80; high = 0xbf; codepoint = (codepoint << 6) | (byte & 63);
        if (++seen == needed) {
            written += (size_t)utf8_put(out + written, codepoint);
            needed = seen = 0; codepoint = 0;
        }
    }
    if (needed) written += (size_t)utf8_put(out + written, 0xfffd);
    JSValue result = JS_NewStringLen(ctx, out, written);
    free(out); return result;
}
/* This private realm loads the same WHATWG URL/IDNA implementation used by
   window.URL. It has no host, network, DOM, filesystem or author callbacks. */
static bool url_prepare(struct validation_state *s) {
    if (s->url_ready) return true;
    JS_UpdateStackTop(s->rt);
    s->deadline = uptime_ms() + VALIDATION_URL_STARTUP_MS;
    /* Build transactionally in a fresh context. Timeout/OOM can leave bundle
       globals partly installed; retry must never reuse those partial roots.
       Both contexts share one bounded runtime, and regex cache ownership is
       unaffected by a failed URL initialization. */
    JSContext *ctx = JS_NewContext(s->rt);
    if (!ctx) { JS_RunGC(s->rt); return false; }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue encoder = JS_NewCFunction(ctx, url_encode, "encode", 1);
    int encoded = JS_IsException(encoder) ? -1 : JS_SetPropertyStr(ctx, global, "__formEncode", encoder);
    JSValue decoder = encoded < 0 ? JS_EXCEPTION : JS_NewCFunction(ctx, url_decode, "decode", 1);
    int decoded = JS_IsException(decoder) ? -1 : JS_SetPropertyStr(ctx, global, "__formDecode", decoder);
    JS_FreeValue(ctx, global);
    if (decoded < 0) goto failed;
    JSValue result = JS_Eval(ctx, js_form_url, sizeof js_form_url - 1, "Nocturne form URL", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) goto failed;
    JS_FreeValue(ctx, result);
    static const char check[] = "(function(value){try{new URL(value);return true;}catch(e){if(e instanceof TypeError)return false;throw e;}})";
    JSValue checker = JS_Eval(ctx, check, sizeof check - 1, "Nocturne form URL check", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(checker)) goto failed;
    s->url_ctx = ctx; s->url_check = checker;
    s->url_ready = true;
    return true;
failed: {
    JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error);
    JS_FreeContext(ctx); JS_RunGC(s->rt); return false;
    }
}
static bool absolute_url_valid(web_doc *d, const char *text) {
    struct validation_state *s = validation_state(d);
    if (!s || !url_prepare(s)) return false;
    JS_UpdateStackTop(s->rt); s->deadline = uptime_ms() + VALIDATION_MATCH_MS;
    JSContext *ctx = s->url_ctx;
    JSValue arg = JS_NewString(ctx, text);
    if (JS_IsException(arg)) { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); return false; }
    JSValue result = JS_Call(ctx, s->url_check, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    if (JS_IsException(result)) { JSValue error = JS_GetException(ctx); JS_FreeValue(ctx, error); return false; }
    bool valid = JS_ToBool(ctx, result) > 0; JS_FreeValue(ctx, result); return valid;
}
static struct validation_pattern *pattern_compile(struct validation_state *s, const char *pattern) {
    struct validation_pattern *entry = &s->patterns[0];
    for (int i = 0; i < VALIDATION_REGEX_CACHE; i++) {
        struct validation_pattern *p = &s->patterns[i];
        if (p->text && !strcmp(p->text, pattern)) { p->used = ++s->serial; return p; }
        if (!p->text || p->used < entry->used) entry = p;
    }
    size_t len = strlen(pattern);
    if (len > VALIDATION_PATTERN_LIMIT) return NULL;
    char *source = malloc(len + 8), *key = strdup(pattern);
    if (!source || !key) { free(source); free(key); return NULL; }
    memcpy(source, "^(?:", 4); memcpy(source + 4, pattern, len); memcpy(source + 4 + len, ")$", 3);
    lre_realloc(s->ctx, entry->bytecode, 0); free(entry->text);
    memset(entry, 0, sizeof *entry);
    int bytecode_len = 0;
    char error[128] = "";
    entry->bytecode = lre_compile(&bytecode_len, error, sizeof error, source, len + 6, LRE_FLAG_UNICODE_SETS, s->ctx);
    free(source);
    if (!entry->bytecode && (strstr(error, "memory") || strstr(error, "stack overflow") || strstr(error, "too many"))) { free(key); return NULL; }
    entry->text = key; entry->invalid = !entry->bytecode; entry->used = ++s->serial;
    return entry;
}
static uint32_t utf8_next(const unsigned char **at, const unsigned char *end) {
    const unsigned char *p = *at;
    uint32_t cp = *p++; int count = 0;
    if (cp >= 0xc2 && cp <= 0xdf) { cp &= 31; count = 1; }
    else if (cp >= 0xe0 && cp <= 0xef) { cp &= 15; count = 2; }
    else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; count = 3; }
    else if (cp >= 0x80) cp = 0xfffd;
    if (count) {
        if (end - p < count) cp = 0xfffd;
        else {
            const unsigned char *q = p;
            for (int i = 0; i < count; i++) {
                if ((q[i] & 0xc0) != 0x80) { cp = 0xfffd; count = 0; break; }
                cp = (cp << 6) | (q[i] & 63);
            }
            p += count;
            if (cp > 0x10ffff) cp = 0xfffd;
        }
    }
    *at = p; return cp;
}
/* Invalid regular expressions impose no constraint. Resource exhaustion and
   timeout do NOT turn an untested value into a valid value. */
static bool pattern_matches(web_doc *d, const char *pattern, const char *text, size_t bytes) {
    struct validation_state *s = validation_state(d);
    if (!s || bytes > VALIDATION_INPUT_LIMIT) return false;
    JS_UpdateStackTop(s->rt); s->deadline = uptime_ms() + VALIDATION_MATCH_MS;
    struct validation_pattern *entry = pattern_compile(s, pattern);
    if (!entry) return false;
    if (entry->invalid) return true;
    uint16_t *input = malloc((bytes + 1) * sizeof *input);
    int captures = lre_get_alloc_count(entry->bytecode);
    uint8_t **capture = captures > 0 ? calloc((size_t)captures, sizeof *capture) : NULL;
    if (!input || !capture) { free(input); free(capture); return false; }
    const unsigned char *at = (const unsigned char *)text, *end = at + bytes;
    int len = 0;
    while (at < end) {
        uint32_t cp = utf8_next(&at, end);
        if (cp > 0xffff) { cp -= 0x10000; input[len++] = (uint16_t)(0xd800 + (cp >> 10)); input[len++] = (uint16_t)(0xdc00 + (cp & 1023)); }
        else input[len++] = (uint16_t)cp;
    }
    int matched = lre_exec(capture, entry->bytecode, (uint8_t *)input, 0, len, 1, s->ctx);
    bool valid = matched > 0 && capture[0] == (uint8_t *)input && capture[1] == (uint8_t *)(input + len);
    free(input); free(capture); return valid;
}
static bool ascii_alnum(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
static bool email_valid(const char *text, size_t len) {
    size_t i = 0;
    while (i < len && text[i] != '@') {
        int c = (unsigned char)text[i++];
        if (!ascii_alnum(c) && !strchr(".!#$%&'*+/=?^_`{|}~-", c)) return false;
    }
    if (!i || i == len) return false;
    i++;
    while (i < len) {
        size_t start = i;
        if (!ascii_alnum((unsigned char)text[i])) return false;
        while (i < len && text[i] != '.') {
            if (!ascii_alnum((unsigned char)text[i]) && text[i] != '-') return false;
            i++;
        }
        if (i - start > 63 || !ascii_alnum((unsigned char)text[i - 1])) return false;
        if (i == len) return true;
        if (++i == len) return false;
    }
    return false;
}
static bool email_list_valid(const char *text, bool multiple) {
    if (!multiple) return email_valid(text, strlen(text));
    for (const char *at = text;;) {
        const char *end = strchr(at, ',');
        if (!end) end = at + strlen(at);
        const char *start = at, *trim = end;
        while (start < trim && is_space((unsigned char)*start)) start++;
        while (trim > start && is_space((unsigned char)trim[-1])) trim--;
        if (!email_valid(start, (size_t)(trim - start))) return false;
        if (!*end) return true;
        at = end + 1;
    }
}
static bool input_pattern_valid(web_doc *d, node_t *n, const char *pattern, const char *value) {
    if (web_input_type(n) != WEB_INPUT_EMAIL || !node_attr(n, "multiple")) return pattern_matches(d, pattern, value, strlen(value));
    for (const char *at = value;;) {
        const char *end = strchr(at, ','); if (!end) end = at + strlen(at);
        const char *start = at, *trim = end;
        while (start < trim && is_space((unsigned char)*start)) start++;
        while (trim > start && is_space((unsigned char)trim[-1])) trim--;
        if (!pattern_matches(d, pattern, start, (size_t)(trim - start))) return false;
        if (!*end) return true;
        at = end + 1;
    }
}
static bool radio_missing(web_doc *d, node_t *n) {
    const char *name = node_attr(n, "name");
    if (!name || !*name) return node_attr(n, "required") && !n->checked;
    node_t *root = doc_node_root(n, false), *form = web_form_owner(d, n);
    bool required = false, checked = false;
    for (node_t *p = root; p; p = tree_next(root, p)) {
        if (!html_element(p) || p->tag != T_input || web_input_type(p) != WEB_INPUT_RADIO) continue;
        const char *other = node_attr(p, "name");
        if (!other || strcmp(name, other) || web_form_owner(d, p) != form) continue;
        doc_control_init(d, p);
        if (node_attr(p, "required")) required = true;
        if (p->checked) checked = true;
    }
    return required && !checked;
}
static bool nonnegative_integer(const char *text, uint32_t *value) {
    if (!text) return false;
    while (is_space((unsigned char)*text)) text++;
    if (*text == '+') text++;
    if (*text < '0' || *text > '9') return false;
    uint32_t n = 0;
    while (*text >= '0' && *text <= '9') {
        unsigned digit = (unsigned)(*text++ - '0');
        if (n > (UINT32_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *value = n; return true;
}
static bool option_empty(node_t *option) {
    const char *value = node_attr(option, "value");
    if (value) return !*value;
    sbuf text = {0}; node_text_content(option, &text);
    bool empty = true;
    for (size_t i = 0; i < text.n; i++) if (!is_space((unsigned char)text.p[i])) { empty = false; break; }
    sb_free(&text); return empty;
}
static bool select_missing(web_doc *d, node_t *n) {
    doc_control_init(d, n);
    int count = 0; node_t *selected = NULL;
    for (int i = 0;; i++) {
        node_t *option = doc_select_option(n, i); if (!option) break;
        if (doc_option_selected(option)) { count++; selected = option; }
    }
    if (!count) return true;
    uint32_t size = 0;
    if (!nonnegative_integer(node_attr(n, "size"), &size) || !size) size = node_attr(n, "multiple") ? 4 : 1;
    node_t *first = doc_select_option(n, 0);
    return count == 1 && size == 1 && selected == first && first->parent == n && option_empty(first);
}
uint32_t web_control_validity(web_doc *d, node_t *n) {
    if (!d || !web_control_validation_interface(n)) return 0;
    uint32_t flags = n->custom_validity_length ? WEB_VALIDITY_CUSTOM_ERROR : 0;
    if (n->tag == T_button || n->tag == T_fieldset || n->tag == T_object || n->tag == T_output) return flags;
    doc_control_init(d, n);
    const char *value = n->value ? n->value : "";
    bool mutable = !web_control_disabled(n) && !(readonly_applies(n) && node_attr(n, "readonly"));
    if (n->tag == T_select) {
        if (web_control_required(n) && select_missing(d, n)) flags |= WEB_VALIDITY_VALUE_MISSING;
        return flags;
    }
    if (n->tag == T_textarea) {
        if (web_control_required(n) && mutable && !*value) flags |= WEB_VALIDITY_VALUE_MISSING;
    } else {
        enum web_input_kind type = web_input_type(n);
        if (type == WEB_INPUT_RADIO) {
            if (radio_missing(d, n)) flags |= WEB_VALIDITY_VALUE_MISSING;
        } else if (type == WEB_INPUT_CHECKBOX) {
            if (web_control_required(n) && !n->checked) flags |= WEB_VALIDITY_VALUE_MISSING;
        } else if (type == WEB_INPUT_FILE) {
            if (web_control_required(n) && (!n->files || !n->files->count)) flags |= WEB_VALIDITY_VALUE_MISSING;
        } else if (web_control_required(n) && mutable && !*value) flags |= WEB_VALIDITY_VALUE_MISSING;
        if (*value && type == WEB_INPUT_EMAIL && !email_list_valid(value, node_attr(n, "multiple") != NULL)) flags |= WEB_VALIDITY_TYPE_MISMATCH;
        if (*value && type == WEB_INPUT_URL && !absolute_url_valid(d, value)) flags |= WEB_VALIDITY_TYPE_MISMATCH;
        const char *pattern = node_attr(n, "pattern");
        if (*value && text_constraints(n) && pattern && !input_pattern_valid(d, n, pattern, value)) flags |= WEB_VALIDITY_PATTERN_MISMATCH;
        if (n->input_bad_input && web_input_numeric(type)) flags |= WEB_VALIDITY_BAD_INPUT;
        double number;
        struct web_input_limits limits; web_input_constraints(n, &limits);
        if (limits.numeric && web_input_parse_number(type, value, &number)) {
            if (type == WEB_INPUT_TIME && limits.has_min && limits.has_max && limits.min > limits.max) {
                if (number > limits.max && number < limits.min) flags |= WEB_VALIDITY_RANGE_UNDERFLOW | WEB_VALIDITY_RANGE_OVERFLOW;
            } else {
                if (limits.has_min && number < limits.min) flags |= WEB_VALIDITY_RANGE_UNDERFLOW;
                if (limits.has_max && number > limits.max) flags |= WEB_VALIDITY_RANGE_OVERFLOW;
            }
            if (web_input_step_mismatch(n, number)) flags |= WEB_VALIDITY_STEP_MISMATCH;
        }
    }
    if ((n->tag == T_textarea || text_constraints(n)) && n->value_dirty && n->control_user_edited) {
        uint32_t limit, length = doc_utf16_length(value);
        if (nonnegative_integer(node_attr(n, "maxlength"), &limit) && length > limit) flags |= WEB_VALIDITY_TOO_LONG;
        if (*value && nonnegative_integer(node_attr(n, "minlength"), &limit) && length < limit) flags |= WEB_VALIDITY_TOO_SHORT;
    }
    return flags;
}
int web_control_in_range(web_doc *d, node_t *n) {
    if (!d || !html_element(n) || n->tag != T_input) return -1;
    struct web_input_limits limits; web_input_constraints(n, &limits);
    if (!limits.numeric || (!limits.has_min && !limits.has_max)) return -1;
    return !(web_control_validity(d, n) & (WEB_VALIDITY_RANGE_UNDERFLOW | WEB_VALIDITY_RANGE_OVERFLOW));
}
bool web_control_set_custom_validity(web_doc *d, node_t *n, const char *text, size_t length) {
    if (!d || !web_control_validation_interface(n) || !text || length > (1u << 20)) return false;
    web_doc *allocation = n->allocation_doc ? n->allocation_doc : d;
    if (!length) { n->custom_validity = NULL; n->custom_validity_length = 0; }
    else if (!n->custom_validity || n->custom_validity_length != length || memcmp(n->custom_validity, text, length)) {
        jmp_buf trap; jmp_buf *old = allocation->mem.trap;
        allocation->mem.trap = &trap; doc_dom_budget(allocation);
        if (setjmp(trap)) { allocation->mem.trap = old; return false; }
        const char *copy = ar_strndup(&allocation->mem, text, length);
        allocation->mem.trap = old; n->custom_validity = copy; n->custom_validity_length = length;
    }
    d->dirty = d->need_style = true; return true;
}
const char *web_control_validation_message(web_doc *d, node_t *n) {
    if (!web_control_will_validate(n)) return "";
    uint32_t flags = web_control_validity(d, n);
    if (flags & WEB_VALIDITY_CUSTOM_ERROR) return n->custom_validity;
    if (flags & WEB_VALIDITY_VALUE_MISSING) return "この項目を入力または選択してください。";
    if (flags & WEB_VALIDITY_TYPE_MISMATCH) return n->tag == T_input && web_input_type(n) == WEB_INPUT_EMAIL ? "正しいメールアドレスを入力してください。" : "正しい絶対URLを入力してください。";
    if (flags & WEB_VALIDITY_PATTERN_MISMATCH) return "指定された書式に合わせて入力してください。";
    if (flags & WEB_VALIDITY_TOO_LONG) return "入力文字数が上限を超えています。";
    if (flags & WEB_VALIDITY_TOO_SHORT) return "入力文字数が下限に達していません。";
    if (flags & WEB_VALIDITY_RANGE_UNDERFLOW) return "値が指定された範囲より小さすぎます。";
    if (flags & WEB_VALIDITY_RANGE_OVERFLOW) return "値が指定された範囲より大きすぎます。";
    if (flags & WEB_VALIDITY_STEP_MISMATCH) return "指定された刻み幅に合う値を入力してください。";
    if (flags & WEB_VALIDITY_BAD_INPUT) return "入力した値を数値または日時に変換できません。";
    return "";
}
size_t web_control_validation_message_length(web_doc *d, node_t *n) {
    const char *message = web_control_validation_message(d, n);
    if (web_control_will_validate(n) && n->custom_validity_length) return n->custom_validity_length;
    return strlen(message);
}
static bool report_control(web_doc *d, node_t *n) {
    if (!n || d->inert || n->owner != d || !doc_node_connected(n) || !web_control_will_validate(n)) return false;
    if (!web_control_validity(d, n)) return false;
    web_js_focus_control(d, n);
    /* Focus listeners can remove/adopt/disable/fix the candidate too. Nodes
       keep their allocation arena, but stale owner/UI metadata must not be
       published. A form pass may then report its next unhandled candidate. */
    if (n->owner != d || !doc_node_connected(n) || !web_control_will_validate(n)) return false;
    const char *message = web_control_validation_message(d, n);
    size_t length = web_control_validation_message_length(d, n);
    if (!length) return false;
    d->validation_target = n; d->validation_message = message;
    d->validation_message_length = length;
    d->validation_report_pending = true;
    d->dirty = true;
    return true;
}
bool web_control_check_validity(web_doc *d, node_t *n, bool report) {
    if (!web_control_will_validate(n) || !web_control_validity(d, n)) return true;
    struct web_event event = {.type = "invalid", .cancelable = true};
    bool unhandled = web_dispatch(d->dom_family ? d->dom_family : d, n, &event);
    if (report && unhandled) report_control(d, n);
    return false;
}
static bool snapshot_push(pvec *p, node_t *n) {
    if (p->n == p->cap) {
        int capacity = p->cap ? p->cap * 2 : 16;
        if (capacity <= p->cap || capacity > INT_MAX / (int)sizeof(void *)) return false;
        void **items = realloc(p->v, (size_t)capacity * sizeof *items);
        if (!items) return false;
        p->v = items; p->cap = capacity;
    }
    p->v[p->n++] = n; return true;
}
bool web_form_constraints_valid(web_doc *d, node_t *container) {
    if (!d || !html_element(container) || (container->tag != T_form && container->tag != T_fieldset)) return true;
    node_t *root = container->tag == T_form ? doc_node_root(container, false) : container;
    for (node_t *n = root; n; n = tree_next(root, n))
        if (web_control_will_validate(n) && (container->tag == T_fieldset || web_form_owner(d, n) == container) && web_control_validity(d, n)) return false;
    return true;
}
bool web_form_check_validity(web_doc *d, node_t *form, bool report) {
    if (!d || !html_element(form) || form->tag != T_form) return false;
    node_t *root = doc_node_root(form, false);
    pvec invalid = {0};
    for (node_t *n = root; n; n = tree_next(root, n)) {
        if (web_control_will_validate(n) && web_form_owner(d, n) == form && web_control_validity(d, n)) {
            if (!snapshot_push(&invalid, n)) { pv_free(&invalid); return false; }
        }
    }
    bool valid = invalid.n == 0;
    struct web_event event = {.type = "invalid", .cancelable = true};
    web_doc *live = d->dom_family ? d->dom_family : d;
    /* The invalid set is snapshotted before callbacks. A handler may remove,
       adopt, disable or fix later controls; each original candidate still
       receives its invalid event once, and insertion cannot extend this pass. */
    for (int i = 0; i < invalid.n; i++) {
        node_t *n = invalid.v[i];
        bool unhandled = web_dispatch(live, n, &event);
        if (!unhandled) invalid.v[i] = NULL;
    }
    if (report) for (int i = 0; i < invalid.n; i++)
        if (report_control(d, invalid.v[i])) break;
    pv_free(&invalid); return valid;
}
bool web_form_submission_validate(web_doc *d, node_t *submitter) {
    if (!d || !html_element(submitter)) return false;
    node_t *form = submitter->tag == T_form ? submitter : web_form_owner(d, submitter);
    if (!form) return false;
    if (node_attr(form, "novalidate") || (web_control_submit_button(submitter) && node_attr(submitter, "formnovalidate"))) return true;
    return web_form_check_validity(d, form, true);
}
