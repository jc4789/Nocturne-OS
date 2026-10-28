/* Host harness for Resource Timing's contentType: js.c's MIME Sniffing "minimize a supported
   MIME type" of a response's Content-Type (minimized_content_type), with the product's own
   type tables (image_source.c's web_image_type_supported, media_types.c's
   nmedia_can_play_type). tests/check_resource_content_type.py cuts those functions out of
   the product files into the included file and feeds each Content-Type value, one per line
   in hex, printing the minimized type. */
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
const char *nmedia_can_play_type(const char *type);
#include RESOURCE_CONTENT_TYPE_SOURCES
static int hex(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
int main(void) {
    static char line[16384], headers[8192];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = 0, at = strlen("Content-Type: ");
        memcpy(headers, "Content-Type: ", at);
        for (char *p = line; hex(p[0]) >= 0 && hex(p[1]) >= 0 && at + 2 < sizeof headers; p += 2, n++)
            headers[at++] = (char)(hex(p[0]) * 16 + hex(p[1]));
        headers[at++] = '\n'; headers[at] = 0;
        char out[64];
        minimized_content_type(headers, out, sizeof out);
        printf("%s\n", out);
        fflush(stdout);
    }
    return 0;
}
