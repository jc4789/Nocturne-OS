/* Message channels (docs/nocturne-native-abi.md §4). */
#pragma once
#include "kernel.h"

struct handle;

/* An unnamed pair: what one end sends, the other receives. 両端の身元は identity_kernel で始まる。 */
int message_channel_pair(struct handle **a, struct handle **b);
bool message_channel_is(struct handle *h);
/* udata is the calling app's memory; attach goes with the message (a document, shared pages, a
   capability, or a channel end: chains of ends in flight are refused, N_ERR_BUSY).
   A full peer queue is waited out until the deadline (clock_now_ns; CLOCK_NEVER: no end; one
   already past: N_ERR_NOT_READY at once; one that comes while it waits: N_ERR_TIMEOUT). */
int64_t message_channel_send(struct handle *h, const void *udata, size_t n, struct handle *attach, uint64_t deadline);
/* カーネルが作る見出し（head）と、呼ぶ側のアプリのメモリーの続き（utail）を一つのメッセージにして、
   呼ぶ側のアプリに課金して送る。待ちと添付は message_channel_send と同じ。 */
int64_t message_channel_request(struct handle *h, const void *head, size_t head_n, const void *utail, size_t tail_n,
                                struct handle *attach, uint64_t deadline);
/* A kernel message (an app's fault report): never waits; a full queue drops it. */
int64_t message_channel_post(struct handle *h, const void *data, size_t n, uint64_t sender_app);
/* An empty queue is waited out until the deadline, as a send's. */
int64_t message_channel_receive(struct handle *h, void *ubuf, size_t cap, uint64_t deadline, bool peek,
                                bool discard, struct handle **attached, uint64_t *sender_app);

/* h の端の身元を決める（端を作った者）。 */
void message_channel_bind(struct handle *h, const struct n_identity *who);
/* h の端が who の手へ渡った：who が今の身元を超えないときだけ身元が who へ下がる
   （渡した者も端を持ち続けられるので、上がる向きには動かない。sys/authority.h の identity_covers）。 */
void message_channel_handed(struct handle *h, const struct n_identity *who);
/* もう一端の身元。 */
void message_channel_peer(struct handle *h, struct n_identity *out);
/* もう一端がまだ開いているか。 */
bool message_channel_peer_open(struct handle *h);
