/* WHATWG Encoding Standard, Shift_JIS decoder:
   https://encoding.spec.whatwg.org/#shift_jis-decoder
   The generated index includes Windows-31J/CP932 extensions; the EUDC range is
   specified separately by the decoder. Source and license: encoding/. */
#include "web_encoding.h"
#include "encoding/jis0208.inc"

static bool ascii_space(unsigned char c) {
    return c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

bool web_shift_jis_label(const char *label) {
    static const char *const labels[] = {
        "csshiftjis", "ms932", "ms_kanji", "shift-jis", "shift_jis",
        "sjis", "windows-31j", "x-sjis"
    };
    if (!label) return false;
    while (ascii_space((unsigned char)*label)) label++;
    size_t n = 0;
    while (label[n]) n++;
    while (n && ascii_space((unsigned char)label[n - 1])) n--;
    for (size_t j = 0; j < sizeof labels / sizeof *labels; j++) {
        size_t i = 0;
        for (; i < n && labels[j][i]; i++) {
            unsigned char c = (unsigned char)label[i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (unsigned char)labels[j][i]) break;
        }
        if (i == n && !labels[j][i]) return true;
    }
    return false;
}

bool web_shift_jis_decode(web_shift_jis_decoder *decoder,
                         const uint8_t *bytes, size_t length, bool final,
                         web_decode_emit emit, void *opaque) {
    size_t i = 0;
    while (i < length) {
        unsigned byte = bytes[i++];
        if (decoder->leading) {
            unsigned leading = decoder->leading;
            decoder->leading = 0;
            uint32_t cp = 0;
            if ((byte >= 0x40 && byte <= 0x7e) || (byte >= 0x80 && byte <= 0xfc)) {
                unsigned offset = byte < 0x7f ? 0x40 : 0x41;
                unsigned leading_offset = leading < 0xa0 ? 0x81 : 0xc1;
                unsigned pointer = (leading - leading_offset) * 188 + byte - offset;
                if (pointer >= 8836 && pointer <= 10715) cp = 0xe000 + pointer - 8836;
                else if (pointer < sizeof jis0208 / sizeof *jis0208) cp = jis0208[pointer];
            }
            if (cp) {
                if (!emit(opaque, cp, false)) return false;
            } else {
                if (!emit(opaque, 0xfffd, true)) return false;
                /* Restore even a syntactically valid but unmapped ASCII trail.
                   In HTML, losing '<' or a quote after an invalid lead can turn
                   text into markup or change an attribute's boundary. */
                if (byte <= 0x7f) i--;
            }
        } else if (byte <= 0x80) {
            if (!emit(opaque, byte, false)) return false;
        } else if (byte >= 0xa1 && byte <= 0xdf) {
            if (!emit(opaque, 0xff61 + byte - 0xa1, false)) return false;
        } else if ((byte >= 0x81 && byte <= 0x9f) || (byte >= 0xe0 && byte <= 0xfc)) {
            decoder->leading = (uint8_t)byte;
        } else if (!emit(opaque, 0xfffd, true)) return false;
    }
    if (final && decoder->leading) {
        decoder->leading = 0;
        if (!emit(opaque, 0xfffd, true)) return false;
    }
    return true;
}
