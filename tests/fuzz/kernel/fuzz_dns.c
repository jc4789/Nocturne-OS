/* libFuzzer harness: DNS reply parsing (compression pointers, CNAME chains,
   record counts) and name encode/decode in kernel/src/net/net.c + dns_name.h.
   The question name is taken from the reply itself so the parser gets past the
   "does the reply answer our question" gate and reaches the record walk. */
#include "../../../kernel/src/net/net.c"
#include "stubs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    /* exact-size copies: any read past the end is an ASan report */
    uint8_t *msg = malloc(size);
    memcpy(msg, data, size);
    char name[DNS_NAME_TEXT_BYTES + 1];
    size_t off = dns_name_decode(msg, size, 12, name);
    if (off > size) abort();
    struct dns_answer ans = {0};
    int r = dns_parse_reply(msg, size, off ? name : "a.b", &ans);
    if (r == 0) {
        if (!ans.ips || !ans.count) abort();
        for (uint32_t i = 0; i < ans.count; i++) (void)ans.ips[i];
        kfree(ans.ips);
    } else if (ans.ips) abort();
    /* also try a second, unrelated name */
    memset(&ans, 0, sizeof ans);
    r = dns_parse_reply(msg, size, "example.com", &ans);
    if (r == 0) kfree(ans.ips);

    /* encoder: the input is a hostname, possibly with dots and hostile lengths */
    uint8_t wire[DNS_NAME_WIRE_BYTES + 8];
    size_t written = 0;
    if (dns_name_encode((const char *)msg, size, wire, sizeof wire, &written)) {
        if (written > DNS_NAME_WIRE_BYTES) abort();
        char back[DNS_NAME_TEXT_BYTES + 1];
        if (!dns_name_decode(wire, written, 0, back)) abort(); /* an encoded name must decode */
    }
    free(msg);
    return 0;
}
