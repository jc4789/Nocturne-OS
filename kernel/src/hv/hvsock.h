/* Hyper-V sockets: byte streams between the host and the guest, each carried by its own VMBus
   channel. Services are named by GUIDs; Linux-style "vsock ports" map to the GUID
   <port>-facb-11e6-bd58-64006a7986d3, which is what Hyper-V's enhanced session uses (port 3389). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hvsock;

/* Accept host connections to `port`. `accept` runs in the hvsock thread for each new connection
   and must not block for long (start a thread for the session). 0 or -errno. */
int hvsock_listen(uint32_t port, void (*accept)(struct hvsock *s, void *arg), void *arg);
/* Read up to len bytes, waiting at most timeout_ms. Returns the byte count, 0 at end of stream
   (the host closed it) or -ETIMEDOUT. */
int hvsock_read(struct hvsock *s, void *buf, size_t len, uint64_t timeout_ms);
/* Read exactly len bytes: 0, or -errno (-EPIPE if the stream ended). */
int hvsock_read_full(struct hvsock *s, void *buf, size_t len, uint64_t timeout_ms);
/* Wait until data (or end of stream) is ready to read: true if so. */
bool hvsock_wait(struct hvsock *s, uint64_t timeout_ms);
/* Write all of buf, waiting for room as needed: 0 or -errno. */
int hvsock_write(struct hvsock *s, const void *buf, size_t len);
/* Send end of stream, close the channel and free the socket. */
void hvsock_close(struct hvsock *s);
