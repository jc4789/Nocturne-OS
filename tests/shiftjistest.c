/* In-OS public HTML integration checks; run with Nocturne's tcc/libc.
   Optional argument is a raw Shift_JIS HTML document fetched from the real site. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

static int checks, failed;
static void title_is(const char *name, const char *html, size_t length,
                     const char *charset, const char *expected) {
    web_doc *doc = web_parse(html, length, "https://encoding.test/", charset);
    checks++;
    if (!doc || strcmp(web_title(doc), expected)) {
        printf("FAIL %s: title=%s\n", name, doc ? web_title(doc) : "(null document)");
        failed++;
    }
    web_free(doc);
}
#define TITLE(name, source, label, expected) title_is(name, source, sizeof(source) - 1, label, expected)

int main(int argc, char **argv) {
    static const char *const labels[] = {"csshiftjis", "ms932", "ms_kanji", "shift-jis",
        "Shift_JIS", "sjis", "windows-31j", "x-sjis", " \tSHIFT_JIS\r\n"};
    for (size_t i = 0; i < sizeof labels / sizeof *labels; i++)
        TITLE(labels[i], "<title>\x82\xa0\x82\xa2\x82\xa4</title>", labels[i], "\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86");
    TITLE("meta", "<meta charset=Shift_JIS><title>\x8c\x8e</title>", NULL, "\xe6\x9c\x88");
    TITLE("http-equiv", "<meta http-equiv='Content-Type' content='text/html; charset=Shift_JIS'><title>\x8c\x8e</title>", NULL, "\xe6\x9c\x88");
    TITLE("header-priority", "<meta charset=utf-8><title>\x8c\x8e</title>", "shift_jis", "\xe6\x9c\x88");
    TITLE("bom-priority", "\xef\xbb\xbf<title>\xe6\x9c\x88</title>", "shift_jis", "\xe6\x9c\x88");
    TITLE("halfwidth", "<title>\xa1\xdf</title>", "shift_jis", "\xef\xbd\xa1\xef\xbe\x9f");
    TITLE("declared-encoding-not-utf8-heuristic", "<title>\xc2\xa2</title>", "shift_jis", "\xef\xbe\x82\xef\xbd\xa2");
    TITLE("ascii-and-80", "<title>\\~\x80</title>", "shift_jis", "\\~\xc2\x80");
    TITLE("cp932-extensions", "<title>\x87\x40\xfa\x40\xfb\xfc</title>", "shift_jis", "\xe2\x91\xa0\xe2\x85\xb0\xe9\xab\x99");
    TITLE("private-use", "<title>\xf0\x40\xf9\xfc</title>", "shift_jis", "\xee\x80\x80\xee\x9d\x97");
    TITLE("restore-tag-boundary", "<title>\x82</title>", "shift_jis", "\xef\xbf\xbd");
    TITLE("restore-ascii", "<title>\x82 A</title>", "shift_jis", "\xef\xbf\xbd A");
    TITLE("unmapped-valid-ascii-trail", "<title>\xfc\x7e</title>", "shift_jis", "\xef\xbf\xbd~");
    TITLE("invalid-single", "<title>\xa0\xfd\xfe\xff</title>", "shift_jis", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
    TITLE("invalid-nonascii-trail", "<title>\x82\xfd</title>", "shift_jis", "\xef\xbf\xbd");
    TITLE("nul-and-title-space-normalization", "<title>A\0B\r\nC\rD\nE</title>", "shift_jis", "A\xef\xbf\xbd" "B C D E");
    TITLE("existing-latin", "<title>\xe9</title>", "windows-1252", "\xc3\xa9");
    TITLE("existing-utf8", "<title>\xe6\x9c\x88</title>", NULL, "\xe6\x9c\x88");
    if (argc > 1) {
        FILE *file = fopen(argv[1], "rb");
        if (!file || fseek(file, 0, SEEK_END) || ftell(file) < 0) {
            printf("FAIL raw-page: could not read file\n"); failed++; checks++;
        } else {
            size_t n = (size_t)ftell(file);
            char *bytes = malloc(n ? n : 1);
            rewind(file);
            if (!bytes || fread(bytes, 1, n, file) != n) {
                printf("FAIL raw-page: could not read body\n"); failed++; checks++;
            } else title_is("raw-5ch", bytes, n, "Shift_JIS", "\xe3\x80\x80\xe2\x96\xa0" "5ch BBS ..");
            free(bytes);
        }
        if (file) fclose(file);
    }
    printf("shiftjistest: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
