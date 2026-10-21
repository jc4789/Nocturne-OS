/* Harness-only replacement for user/libc/net.c/tls.c: serves fuzz bytes as the "network". */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "../../../user/include/http.h"
typedef struct net_stream net_stream;
#include "net_stub.h"

struct net_stream { int unused; };
static struct net_stream the_stream;
const uint8_t *stub_in; size_t stub_len, stub_at, stub_chunk = 4096; bool stub_h2;
uint64_t stub_clock;

void stub_reset(const uint8_t *in, size_t len, size_t chunk, bool h2) {
    stub_in = in; stub_len = len; stub_at = 0; stub_chunk = chunk ? chunk : 1; stub_h2 = h2;
}
uint64_t uptime_ms(void) { return stub_clock += 1; }
void yield(void) {}
void n_sleep_ms(uint64_t ms) { stub_clock += ms; }

net_stream *ns_open_http_cancelled_timed(const char *h, uint16_t p, bool t, int to, char *e, size_t el,
    struct http_transport_timing *tm, const char *part, int (*c)(void *), void *x) { return &the_stream; }
net_stream *ns_open(const char *h, uint16_t p, bool t, int to, char *e, size_t el) { return &the_stream; }
net_stream *ns_open_timed(const char *h, uint16_t p, bool t, int to, char *e, size_t el, struct http_transport_timing *tm) { return &the_stream; }
net_stream *ns_open_http_timed(const char *h, uint16_t p, bool t, int to, char *e, size_t el,
    struct http_transport_timing *tm, const char *part) { return &the_stream; }
int ns_get_connection_info(const net_stream *s, struct ns_connection_info *i) { return -1; }
long ns_write(net_stream *s, const void *b, size_t n) { return (long)n; }
long ns_read(net_stream *s, void *b, size_t n) {
    size_t left = stub_len - stub_at; if (n > stub_chunk) n = stub_chunk; if (n > left) n = left;
    memcpy(b, stub_in + stub_at, n); stub_at += n; return (long)n;
}
long ns_try_read(net_stream *s, void *b, size_t n) { return ns_read(s, b, n); }
long ns_read_wait(net_stream *s, void *b, size_t n, int t, volatile uint32_t *l) { return ns_read(s, b, n); }
long ns_try_write(net_stream *s, const void *b, size_t n) { return (long)n; }
void ns_engine_lock(volatile uint32_t *l) {}
void ns_engine_unlock(volatile uint32_t *l) {}
void ns_reset(net_stream *s) {}
void ns_set_timeout(net_stream *s, int t) {}
const char *ns_error(net_stream *s) { return "stub"; }
void ns_close(net_stream *s) {}
void ns_abort(net_stream *s) {}
const char *ns_protocol(net_stream *s) { return stub_h2 ? "h2" : "http/1.1"; }
bool ns_session_resumed(net_stream *s) { return false; }
void ns_set_cancel(net_stream *s, int (*c)(void *), void *x) {}
uint64_t ns_bytes_sent(net_stream *s) { return 0; }
void ns_sessions_clear(void) {}
