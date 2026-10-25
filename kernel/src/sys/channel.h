/* Message channels and named rendezvous (docs/nocturne-native-abi.md §4). */
#pragma once
#include "kernel.h"

struct handle;

/* An unnamed pair: what one end sends, the other receives. */
int message_channel_pair(struct handle **a, struct handle **b);
bool message_channel_is(struct handle *h);
/* udata is the calling app's memory; attach (a document or shared pages) goes with the message. */
int64_t message_channel_send(struct handle *h, const void *udata, size_t n, struct handle *attach);
/* A kernel message (an app's fault report): never waits; a full queue drops it. */
int64_t message_channel_post(struct handle *h, const void *data, size_t n, uint64_t sender_app);
int64_t message_channel_receive(struct handle *h, void *ubuf, size_t cap, bool wait, bool peek,
                                bool discard, struct handle **attached, uint64_t *sender_app);
/* The same on an AP inside an AP read section, for a message channel end with nothing queued:
   true with *result (N_ERR_CLOSED, or N_ERR_NOT_READY without wait), false when the BSP must take
   a message or wait for one. */
bool message_channel_receive_ap(struct handle *h, bool wait, int64_t *result);

/* Named rendezvous: the listener holds its key until its handle closes. */
int channel_listen(const char *key, size_t length, struct handle **listener);
int channel_connect(const char *key, size_t length, struct handle **client);
int channel_accept(struct handle *listener, struct handle **server);
