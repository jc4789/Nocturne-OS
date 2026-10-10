/* DOM ProcessingInstruction's pseudo-attribute grammar is XML, not the HTML
   tokenizer's attribute grammar: quotes/semicolons are mandatory, duplicate
   names and unknown entities invalidate the complete map. */
#include "webi.h"
#include "html_pi.h"

static bool xml_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static bool xml_character(uint32_t c) {
    return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xd7ff) ||
           (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}
static bool value_put(sbuf *out, const char *data, size_t length) {
    if (out->n == SIZE_MAX || length > SIZE_MAX - out->n - 1) return false;
    size_t target = out->n + length + 1;
    if (target > out->cap) {
        size_t capacity = out->cap ? out->cap : 64;
        while (capacity < target) capacity = capacity > SIZE_MAX / 2 ? target : capacity * 2;
        char *bytes = realloc(out->p, capacity);
        if (!bytes) return false;
        out->p = bytes; out->cap = capacity;
    }
    if (length) memcpy(out->p + out->n, data, length);
    out->n += length; out->p[out->n] = 0; return true;
}
void html_pi_free(struct html_pi_attribute *attrs, size_t count) {
    for (size_t i = 0; i < count; i++) { free(attrs[i].name); free(attrs[i].value); }
    free(attrs);
}
static enum html_pi_parse_status append_reference(sbuf *out, const char *s, size_t length) {
    static const struct { const char *name; uint32_t c; } refs[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}
    };
    uint32_t cp = 0;
    if (length && s[0] == '#') {
        size_t at = 1; unsigned base = 10;
        if (at < length && s[at] == 'x') { at++; base = 16; }
        if (at == length) return HTML_PI_INVALID;
        for (; at < length; at++) {
            unsigned char c = (unsigned char)s[at]; unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return HTML_PI_INVALID;
            if (digit >= base || cp > (0x10ffffu - digit) / base) return HTML_PI_INVALID;
            cp = cp * base + digit;
        }
        if (!xml_character(cp)) return HTML_PI_INVALID;
    } else {
        bool found = false;
        for (size_t i = 0; i < sizeof refs / sizeof *refs; i++)
            if (strlen(refs[i].name) == length && !memcmp(refs[i].name, s, length)) {
                cp = refs[i].c; found = true; break;
            }
        if (!found) return HTML_PI_INVALID;
    }
    char bytes[4]; int count = utf8_put(bytes, cp);
    return value_put(out, bytes, (size_t)count) ? HTML_PI_OK : HTML_PI_OOM;
}
enum html_pi_parse_status html_pi_parse_ex(const char *s, size_t length, struct html_pi_attribute **out, size_t *out_count) {
    if (!out || !out_count || (!s && length)) return HTML_PI_INVALID;
    *out = NULL; *out_count = 0;
    struct html_pi_attribute *attrs = NULL;
    size_t count = 0, capacity = 0, at = 0;
    sbuf value = {0}; char *name = NULL;
    enum html_pi_parse_status status = HTML_PI_INVALID;
    while (at < length && xml_space((unsigned char)s[at])) at++;
    while (at < length) {
        size_t start = at;
        while (at < length && !xml_space((unsigned char)s[at]) && s[at] != '=') at++;
        if (!doc_pi_target_valid(s + start, at - start)) goto fail;
        size_t name_length = at - start;
        name = malloc(name_length + 1);
        if (!name) goto oom;
        memcpy(name, s + start, name_length); name[name_length] = 0;
        for (size_t i = 0; i < count; i++) if (!strcmp(attrs[i].name, name)) goto fail;
        while (at < length && xml_space((unsigned char)s[at])) at++;
        if (at == length || s[at++] != '=') goto fail;
        while (at < length && xml_space((unsigned char)s[at])) at++;
        if (at == length || (s[at] != '\'' && s[at] != '"')) goto fail;
        char quote = s[at++];
        while (at < length && s[at] != quote) {
            if (s[at] == '<') goto fail;
            if (s[at] == '&') {
                start = ++at;
                while (at < length && s[at] != ';' && s[at] != quote) at++;
                if (at == length || s[at] != ';') goto fail;
                enum html_pi_parse_status reference = append_reference(&value, s + start, at - start);
                if (reference == HTML_PI_OOM) goto oom;
                if (reference != HTML_PI_OK) goto fail;
                at++;
            } else {
                if (!value_put(&value, s + at, 1)) goto oom;
                at++;
            }
        }
        if (at == length) goto fail;
        at++;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            if (next < capacity || next > SIZE_MAX / sizeof *attrs) goto oom;
            struct html_pi_attribute *grown = realloc(attrs, next * sizeof *attrs);
            if (!grown) goto oom;
            attrs = grown; capacity = next;
        }
        char *copy = malloc(value.n + 1);
        if (!copy) goto oom;
        if (value.n) memcpy(copy, value.p, value.n);
        copy[value.n] = 0;
        attrs[count++] = (struct html_pi_attribute){ name, copy, value.n };
        name = NULL; sb_free(&value);
        if (at < length && !xml_space((unsigned char)s[at])) goto fail;
        while (at < length && xml_space((unsigned char)s[at])) at++;
    }
    *out = attrs; *out_count = count; return HTML_PI_OK;
oom:
    status = HTML_PI_OOM;
fail:
    free(name); sb_free(&value); html_pi_free(attrs, count); return status;
}
bool html_pi_parse(const char *s,size_t length,struct html_pi_attribute **out,size_t *count) {
    return html_pi_parse_ex(s,length,out,count) == HTML_PI_OK;
}
char *html_pi_get(const char *data, size_t length, const char *name) {
    struct html_pi_attribute *attrs; size_t count;
    if (!name || !html_pi_parse(data, length, &attrs, &count)) return NULL;
    char *value = NULL;
    for (size_t i = 0; i < count; i++) if (!strcmp(attrs[i].name, name)) {
        value = attrs[i].value; attrs[i].value = NULL; break;
    }
    html_pi_free(attrs, count); return value;
}
