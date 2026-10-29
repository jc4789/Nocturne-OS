/* The product's TLS client (user/libc/tls.c) on the host, over Winsock, against real servers:
   tests/tls_hosttest.py runs it against local OpenSSL servers and Internet hosts. Only the
   native ABI's network, clock, entropy and folder calls are stood in for here; the handshakes,
   key schedules, record layer, certificate checks and resumption are the product's.

   usage: tls_hosttest connect <host> <port> <http 0|1> <times> [send]
              each connection: version, suite, ALPN, resumption, then a request:
              "get" an HTTP/1.1 GET / (or the HTTP/2 preface when h2), "line" a line, "none"
          tls_hosttest keyupdate <host> <port>
              reads until "after", answers "pong", then reads to the server's close_notify
              (the server, tests/tls_hostpeer.c, asks for KeyUpdates before "after")
   The trust anchors: the compiled-in roots, and the certificates of the folder named by
   TLS_TEST_CERTS (as N_FOLDER_CERTIFICATES is in the product). */
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../user/libc/tls.c"

/* ---- the native ABI calls tls.c makes ---- */
static SOCKET sockets[64];
static bool socket_used[64];
uint64_t uptime_ms(void) { return GetTickCount64(); }
n_result n_random(void *buf, size_t n) {
    return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ? -N_ERR_IO : (n_result)n;
}
const char *n_result_text(n_result r) {
    if (r < 0) r = -r;
    switch (r) {
    case N_ERR_NOT_READY: return "not ready";
    case N_ERR_TIMEOUT: return "timed out";
    case N_ERR_CONNECTION_RESET: return "connection reset";
    case N_ERR_CONNECTION_REFUSED: return "connection refused";
    case N_ERR_NOT_FOUND: return "not found";
    case N_ERR_UNREACHABLE: return "unreachable";
    }
    return "error";
}
bool net_wait_up(int timeout_ms) { (void)timeout_ms; return true; }
n_result net_resolve_all(const char *name, uint32_t *ips, size_t capacity, struct n_dns_result *result) {
    struct addrinfo hints = {0}, *list;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(name, NULL, &hints, &list)) return -N_ERR_NOT_FOUND;
    size_t count = 0;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        if (count < capacity) ips[count] = ((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr;
        count++;
    }
    freeaddrinfo(list);
    memset(result, 0, sizeof *result);
    result->count = (uint32_t)count;
    return count ? (n_result)(count < capacity ? count : capacity) : -N_ERR_NOT_FOUND;
}
static int wait_socket(SOCKET s, bool write, int timeout_ms) {
    fd_set set; FD_ZERO(&set); FD_SET(s, &set);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(0, write ? NULL : &set, write ? &set : NULL, NULL, timeout_ms < 0 ? NULL : &tv);
}
n_result tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) {
    int slot = -1;
    for (int i = 0; i < 64; i++) if (!socket_used[i]) { slot = i; break; }
    if (slot < 0) return -N_ERR_NO_MEMORY;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -N_ERR_IO;
    u_long nonblocking = 1; ioctlsocket(s, FIONBIO, &nonblocking);
    BOOL nodelay = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = ip;
    if (connect(s, (struct sockaddr *)&a, sizeof a) && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return -N_ERR_CONNECTION_REFUSED; }
    if (wait_socket(s, true, timeout_ms) <= 0) { closesocket(s); return -N_ERR_TIMEOUT; }
    int error = 0, len = sizeof error;
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
    if (error) { closesocket(s); return -N_ERR_CONNECTION_REFUSED; }
    sockets[slot] = s; socket_used[slot] = true;
    return slot;
}
n_result tcp_recv(n_handle h, void *buf, size_t len, int timeout_ms) {
    int w = wait_socket(sockets[h], false, timeout_ms);
    if (w == 0) return timeout_ms ? -N_ERR_TIMEOUT : -N_ERR_NOT_READY;
    if (w < 0) return -N_ERR_CONNECTION_RESET;
    int got = recv(sockets[h], buf, (int)(len > 1 << 30 ? 1 << 30 : len), 0);
    if (got < 0) return WSAGetLastError() == WSAEWOULDBLOCK ? -N_ERR_NOT_READY : -N_ERR_CONNECTION_RESET;
    return got;
}
n_result tcp_send_some(n_handle h, const void *buf, size_t len) {
    int sent = send(sockets[h], buf, (int)(len > 1 << 30 ? 1 << 30 : len), 0);
    if (sent < 0) return WSAGetLastError() == WSAEWOULDBLOCK ? -N_ERR_NOT_READY : -N_ERR_CONNECTION_RESET;
    return sent;
}
n_result tcp_send_until(n_handle h, const void *buf, size_t len, uint64_t deadline_ms) {
    size_t done = 0;
    while (done < len) {
        n_result r = tcp_send_some(h, (const char *)buf + done, len - done);
        if (r > 0) { done += (size_t)r; continue; }
        if (r != -N_ERR_NOT_READY) return done ? (n_result)done : r;
        uint64_t now = uptime_ms();
        if (deadline_ms && now >= deadline_ms) return done ? (n_result)done : -N_ERR_TIMEOUT;
        wait_socket(sockets[h], true, deadline_ms ? (int)(deadline_ms - now) : -1);
    }
    return (n_result)done;
}
n_result tcp_close(n_handle h) { closesocket(sockets[h]); socket_used[h] = false; return 0; }
n_result tcp_cancel(n_handle h) { shutdown(sockets[h], SD_BOTH); return 0; }
static const char *cert_folder(void) { const char *f = getenv("TLS_TEST_CERTS"); return f ? f : ""; }
n_result n_folder_entries(const char *path, struct n_entry **out) {
    *out = NULL;
    if (strcmp(path, N_FOLDER_CERTIFICATES) || !*cert_folder()) return 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", cert_folder());
    WIN32_FIND_DATAA f;
    HANDLE find = FindFirstFileA(pattern, &f);
    if (find == INVALID_HANDLE_VALUE) return 0;
    n_result count = 0;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        struct n_entry *grown = realloc(*out, (size_t)(count + 1) * sizeof **out);
        if (!grown) break;
        *out = grown;
        memset(&grown[count], 0, sizeof *grown);
        snprintf(grown[count].name, sizeof grown[count].name, "%s", f.cFileName);
        grown[count].kind = 1;
        count++;
    } while (FindNextFileA(find, &f));
    FindClose(find);
    return count;
}
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) {
    (void)folder;
    int n = snprintf(out, cap, "%s\\%s", cert_folder(), name);
    return n < 0 || (size_t)n >= cap ? -N_ERR_INVALID : n;
}
/* C11 threads for tls.c's table and anchors: this harness has one thread. */
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void call_once(once_flag *flag, void (*func)(void)) { if (!flag->state) { flag->state = 2; func(); } }

/* ---- the checks ---- */
static const char *version_name(const net_stream *s) { return s->version == TLS13 ? "1.3" : s->version == TLS12 ? "1.2" : "?"; }
static long read_some(net_stream *s, char *buf, size_t cap) {
    long got = ns_read(s, buf, cap - 1);
    if (got > 0) buf[got] = 0;
    return got;
}
static int request(net_stream *s, const char *host, const char *send) {
    char buf[16384];
    if (!strcmp(send, "none")) return 0;
    if (!strcmp(send, "line")) {
        if (ns_write(s, "hello\n", 6) != 6) { printf("  write failed: %s\n", ns_error(s)); return 1; }
        return 0;
    }
    if (s->h2) { /* RFC 9113 §3.4: the preface and SETTINGS; the server's first frame is SETTINGS */
        static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n\0\0\0\4\0\0\0\0\0";
        if (ns_write(s, preface, sizeof preface - 1) != (long)(sizeof preface - 1)) { printf("  write failed: %s\n", ns_error(s)); return 1; }
        size_t have = 0;
        while (have < 9) {
            long got = ns_read(s, buf + have, 9 - have);
            if (got <= 0) { printf("  read failed: %s\n", ns_error(s)); return 1; }
            have += (size_t)got;
        }
        printf("  h2 first frame type %u length %u\n", (unsigned char)buf[3], (unsigned)((unsigned char)buf[0] << 16 | (unsigned char)buf[1] << 8 | (unsigned char)buf[2]));
        return (unsigned char)buf[3] == 4 ? 0 : 1;
    }
    char req[512];
    int n = snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: tls_hosttest\r\nConnection: close\r\n\r\n", host);
    if (ns_write(s, req, (size_t)n) != n) { printf("  write failed: %s\n", ns_error(s)); return 1; }
    long got = read_some(s, buf, sizeof buf);
    if (got <= 0) { printf("  read failed (%ld): %s\n", got, ns_error(s)); return 1; }
    char *eol = strpbrk(buf, "\r\n");
    if (eol) *eol = 0;
    printf("  response: %.80s\n", buf);
    /* the rest, to the server's close: close_notify gives 0 */
    long total = got, r;
    while ((r = ns_read(s, buf, sizeof buf)) > 0) total += r;
    printf("  read %ld bytes, end %s\n", total, r == 0 ? "close_notify" : ns_error(s));
    return 0;
}
static int connect_cases(const char *host, uint16_t port, bool http, int times, const char *send) {
    int failures = 0;
    for (int i = 0; i < times; i++) {
        char err[256] = "";
        struct http_transport_timing timing;
        net_stream *s = http ? ns_open_http_timed(host, port, true, 15000, err, sizeof err, &timing, NULL)
                             : ns_open_timed(host, port, true, 15000, err, sizeof err, &timing);
        if (!s) { printf("RESULT fail errno=%d error=%s\n", errno, err); failures++; continue; }
        printf("RESULT ok version=%s suite=%04x alpn=%s resumed=%d tls_ms=%llu\n", version_name(s), s->suite->id,
               s->timing_info.protocol[0] ? s->timing_info.protocol : "-", ns_session_resumed(s), (unsigned long long)timing.tls_ms);
        failures += request(s, host, send);
        ns_close(s);
    }
    return failures;
}
static int keyupdate_case(const char *host, uint16_t port) {
    char err[256] = "";
    net_stream *s = ns_open(host, port, true, 15000, err, sizeof err);
    if (!s) { printf("RESULT fail errno=%d error=%s\n", errno, err); return 1; }
    printf("RESULT ok version=%s suite=%04x\n", version_name(s), s->suite->id);
    printf("CONNECTED\n");
    unsigned char before[48], wbefore[48];
    memcpy(before, s->rd.secret, sizeof before);
    memcpy(wbefore, s->wr.secret, sizeof wbefore);
    /* the last bytes read, enough to see "after" across reads */
    char buf[4096], tail[64] = "";
    size_t have = 0, total = 0;
    while (!strstr(tail, "after")) {
        long got = ns_read(s, buf, sizeof buf);
        if (got <= 0) { printf("  read failed: %s\n", ns_error(s)); ns_close(s); return 1; }
        total += (size_t)got;
        for (long i = 0; i < got; i++) {
            if (have == sizeof tail - 1) { memmove(tail, tail + 1, have - 1); have--; }
            tail[have++] = buf[i];
        }
        tail[have] = 0;
    }
    bool read_updated = memcmp(before, s->rd.secret, sizeof before) != 0, write_updated = memcmp(wbefore, s->wr.secret, sizeof wbefore) != 0;
    printf("  %zu bytes, read key updated=%d write key updated=%d\n", total, read_updated, write_updated);
    long w = ns_write(s, "pong\n", 5);
    printf("  pong %s\n", w == 5 ? "sent" : ns_error(s));
    long end = ns_read(s, buf, sizeof buf);
    printf("  after pong: %s\n", end == 0 ? "close_notify" : end < 0 ? ns_error(s) : "more data");
    ns_close(s);
    return read_updated && write_updated && w == 5 && end == 0 ? 0 : 1;
}
int main(int argc, char **argv) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 6 && !strcmp(argv[1], "connect"))
        return connect_cases(argv[2], (uint16_t)atoi(argv[3]), atoi(argv[4]) != 0, atoi(argv[5]), argc > 6 ? argv[6] : "get") ? 1 : 0;
    if (argc >= 4 && !strcmp(argv[1], "keyupdate")) return keyupdate_case(argv[2], (uint16_t)atoi(argv[3]));
    if (argc >= 2 && !strcmp(argv[1], "anchors")) {
        size_t n;
        trust_anchors(&n);
        printf("anchors %zu, compiled in %zu\n", n, tls_trust_anchors_num);
        struct n_entry *entries = NULL;
        n_result count = n_folder_entries(N_FOLDER_CERTIFICATES, &entries);
        for (n_result i = 0; i < count; i++) {
            char path[4096];
            n_result joined = n_path_join(N_FOLDER_CERTIFICATES, entries[i].name, path, sizeof path);
            FILE *f = joined < 0 ? NULL : fopen(path, "rb");
            struct der_bytes document = {0};
            unsigned char chunk[4096];
            size_t got;
            while (f && (got = fread(chunk, 1, sizeof chunk, f)) > 0) der_append(&document, chunk, got);
            struct anchor_list list = {0};
            if (document.len) anchors_from_document(&list, document.data, document.len);
            printf("  %s: %zu bytes, %zu certificates\n", path, document.len, list.count);
            free(document.data);
            if (f) fclose(f);
        }
        free(entries);
        return 0;
    }
    fprintf(stderr, "usage: tls_hosttest connect <host> <port> <http> <times> [get|line|none] | keyupdate <host> <port>\n");
    return 2;
}
