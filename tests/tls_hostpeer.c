/* A TLS 1.3 server for tests/tls_hosttest.py, for the post-handshake cases OpenSSL's s_server
   cannot drive on Windows (it reads a redirected stdin only between one-second socket waits, so
   its interactive commands arrive late or not at all). Host only, built with the OpenSSL of
   tools/msys64.

     tls_hostpeer keyupdate <port> <certificate chain.pem> <key.pem>

   Accepts one connection on 127.0.0.1:<port> and, after the handshake, writes "hello\n", then
   three times a KeyUpdate asking for one back (RFC 8446 §4.6.3) and "x\n", then 64 KiB of 'y'
   (full records) and "after\n"; reads until "pong\n"; closes with close_notify and waits for the
   client's. Prints one line:
     PEER key_updates_received=<n> pong=<0|1> close_notify=<0|1>
   OpenSSL moves its read key at each KeyUpdate it receives, so "pong" decrypting after one is the
   proof the client's write key moved with it. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static int key_updates;

static void on_message(int write_p, int version, int content_type, const void *buf, size_t len, SSL *ssl, void *arg) {
    (void)version; (void)ssl; (void)arg;
    if (!write_p && content_type == SSL3_RT_HANDSHAKE && len >= 1 && ((const unsigned char *)buf)[0] == SSL3_MT_KEY_UPDATE)
        key_updates++;
}

static int write_all(SSL *ssl, const void *data, int len) {
    while (len > 0) {
        int n = SSL_write(ssl, data, len);
        if (n <= 0) return 0;
        data = (const char *)data + n;
        len -= n;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 5 || strcmp(argv[1], "keyupdate")) {
        fprintf(stderr, "usage: tls_hostpeer keyupdate <port> <chain.pem> <key.pem>\n");
        return 2;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_num_tickets(ctx, 0);
    if (SSL_CTX_use_certificate_chain_file(ctx, argv[3]) != 1 || SSL_CTX_use_PrivateKey_file(ctx, argv[4], SSL_FILETYPE_PEM) != 1) {
        ERR_print_errors_fp(stderr);
        return 2;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(argv[2]));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&a, sizeof a) || listen(listener, 1)) {
        fprintf(stderr, "cannot listen on %s\n", argv[2]);
        return 2;
    }
    printf("LISTENING\n");
    fflush(stdout);
    SOCKET c = accept(listener, NULL, NULL);
    DWORD timeout = 15000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, (int)c);
    SSL_set_msg_callback(ssl, on_message);
    int pong = 0, closed = 0;
    if (SSL_accept(ssl) != 1) {
        ERR_print_errors_fp(stderr);
        goto done;
    }
    if (!write_all(ssl, "hello\n", 6)) goto done;
    for (int i = 0; i < 3; i++) {
        if (SSL_key_update(ssl, SSL_KEY_UPDATE_REQUESTED) != 1 || !write_all(ssl, "x\n", 2)) goto done;
    }
    static char big[65536];
    memset(big, 'y', sizeof big);
    if (!write_all(ssl, big, sizeof big) || !write_all(ssl, "\nafter\n", 7)) goto done;
    char got[256] = "";
    size_t have = 0;
    while (!strstr(got, "pong\n")) {
        char buf[256];
        int n = SSL_read(ssl, buf, sizeof buf);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (have == sizeof got - 1) { memmove(got, got + 1, have - 1); have--; }
            got[have++] = buf[i];
        }
        got[have] = 0;
    }
    pong = strstr(got, "pong\n") != NULL;
    if (pong && SSL_shutdown(ssl) >= 0) {
        char buf[256];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof buf)) > 0) {}
        closed = SSL_get_error(ssl, n) == SSL_ERROR_ZERO_RETURN;
    }
done:
    ERR_print_errors_fp(stderr);
    printf("PEER key_updates_received=%d pong=%d close_notify=%d\n", key_updates, pong, closed);
    fflush(stdout);
    SSL_free(ssl);
    closesocket(c);
    closesocket(listener);
    return pong && closed && key_updates ? 0 : 1;
}
