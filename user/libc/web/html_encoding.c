/* HTML's encoding sniffing over the Encoding Standard's layer (web_encoding.h).
   https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding */
#include <stdio.h>
#include "html_encoding.h"
#include "web_encoding.h"
#include <lexbor/html/encoding.h>

/* A meta or prescan charset as the encoding it names: UTF-16BE/LE become UTF-8,
   x-user-defined becomes windows-1252. */
static web_encoding meta_encoding(const char *label, size_t length) {
    web_encoding e = web_encoding_get(label, length);
    if (e == LXB_ENCODING_UTF_16BE || e == LXB_ENCODING_UTF_16LE) return WEB_ENCODING_UTF8;
    return e == LXB_ENCODING_X_USER_DEFINED ? LXB_ENCODING_WINDOWS_1252 : e;
}

bool html_meta_label(const char *label, size_t length, char *canonical, size_t capacity) {
    if (!label || !canonical || !capacity) return false;
    web_encoding e = meta_encoding(label, length);
    const char *name = web_encoding_name(e);
    size_t n = strlen(name);
    if (!e || n >= capacity) return false;
    memcpy(canonical, name, n + 1);
    return true;
}

void html_transport_label(const char *type, char *label, size_t capacity) {
    if (!capacity) return;
    label[0] = 0;
    if (!type) return;
    const lxb_char_t *end = NULL;
    const lxb_char_t *name = lxb_html_encoding_content((const lxb_char_t *)type,
                                                       (const lxb_char_t *)type + strlen(type), &end);
    if (name && (size_t)(end - name) < capacity) { memcpy(label, name, end - name); label[end - name] = 0; }
}

char *html_decode_bytes(web_doc *d, const char *src, size_t n, const char *label, size_t *length) {
    if (!d || (!src && n) || !length) return NULL;
    const uint8_t *bytes = (const uint8_t *)(src ? src : "");
    size_t bom = 0;
    /* The BOM, then the transport layer's label: both are certain. */
    web_encoding e = web_encoding_bom_sniff(bytes, n, &bom);
    if (!e && label && *label) e = web_encoding_get(label, strlen(label));
    d->encoding_certain = e != WEB_ENCODING_FAILURE;
    if (!e) {
        lxb_html_encoding_t scan = {0};
        if (lxb_html_encoding_init(&scan) != LXB_STATUS_OK) return NULL;
        size_t name_length = 0;
        const lxb_char_t *name = lxb_html_encoding_prescan(&scan, bytes, bytes + (n < 1024 ? n : 1024), &name_length);
        if (name) {
            e = web_encoding_get((const char *)name, name_length);
            /* Keep UTF-16 for an XML-declared UTF-16 document that has NUL bytes. */
            bool utf16 = (e == LXB_ENCODING_UTF_16BE || e == LXB_ENCODING_UTF_16LE) &&
                         n >= 6 && (!bytes[0] || !bytes[1]);
            if (!utf16) e = meta_encoding((const char *)name, name_length);
        }
        lxb_html_encoding_destroy(&scan, false);
    }
    /* Unlabeled: UTF-8 when the bytes are valid UTF-8 throughout (the standard
       allows autodetection), else the windows-1252 default. */
    if (!e) e = web_utf8_valid(bytes, n) == n ? WEB_ENCODING_UTF8 : LXB_ENCODING_WINDOWS_1252;
    web_encoding used;
    char *text = web_decode(bytes, n, e, &used, length);
    if (text) snprintf(d->encoding, sizeof d->encoding, "%s", web_encoding_name(used));
    return text;
}
