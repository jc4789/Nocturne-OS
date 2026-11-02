/* 音の流れ：サービス audio への接続と共有の輪（docs/nocturne-native-abi.md §17）。

   音は輪へ写すだけで、続けて鳴る流れにはシステムコールが無い。サービスが眠った流れ（asleep）だけ KICK を一通送る。
   サービスが死ぬと接続が閉じる。次に呼ばれた時に接続し直し、まだ混ぜていない音を新しい輪へ写して数を続ける。 */
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define WAIT_LOOK_NS (100 * N_NS_PER_MS) /* 空きを待つ間に相手の生死を見る間隔 */
#define RETRY_NS (250 * N_NS_PER_MS)     /* 繋がらない時に次に試すまで */
#define CLOSE_EXTRA_NS N_NS_PER_S        /* 閉じる時に残りの時間の後でさらに待つ分 */

struct n_audio_stream {
    n_handle channel;
    struct n_audio_format format;
    struct n_audio_ring *ring;
    size_t bytes;
    uint64_t seen_tail, retry_at;
    uint32_t seen_sequence;
};

static n_result ring_map(n_audio_stream *s, n_handle memory, size_t bytes) {
    void *view;
    n_result r = n_shared_map(memory, bytes, N_MEMORY_READ_WRITE, &view);
    n_handle_close(memory);
    if (r < 0) return r;
    s->ring = view;
    s->bytes = bytes;
    return 0;
}

/* OPEN を送って答えを待つ。request の数を続きとして渡す。 */
static n_result stream_connect(n_audio_stream *s, const struct n_audio_request *request) {
    n_handle channel = n_service_connect(N_AUDIO_SERVICE);
    if (channel < 0) return channel;
    struct n_audio_reply reply;
    n_handle memory = -1;
    n_result r = n_channel_send(channel, request, sizeof *request, -1);
    if (r >= 0) r = n_channel_receive(channel, &reply, sizeof reply, N_CHANNEL_WAIT, &memory);
    if (r >= 0) r = r == sizeof reply ? reply.result : -N_ERR_PROTOCOL;
    if (r >= 0) r = memory >= 0 ? ring_map(s, memory, (size_t)reply.bytes) : -N_ERR_PROTOCOL;
    else if (memory >= 0) n_handle_close(memory);
    if (r < 0) {
        n_handle_close(channel);
        return r;
    }
    s->channel = channel;
    return 0;
}

n_result n_audio_open(const struct n_audio_format *format, n_audio_stream **out) {
    n_audio_stream *s = calloc(1, sizeof *s);
    if (!s) return -N_ERR_NO_MEMORY;
    s->channel = -1;
    s->format = *format;
    struct n_audio_request q = {.op = N_AUDIO_OP_OPEN, .format = *format};
    n_result r = stream_connect(s, &q);
    if (r < 0) {
        free(s);
        return r;
    }
    *out = s;
    return 0;
}

/* サービスが居ないか。 */
static bool peer_gone(const n_audio_stream *s) {
    if (s->channel < 0) return true;
    return n_channel_receive(s->channel, NULL, 0, N_CHANNEL_PEEK, NULL) == -N_ERR_CLOSED;
}

/* 接続し直す。古い輪のまだ混ぜていない音を写し、数を続ける。false はまだ繋がらない。 */
static bool stream_again(n_audio_stream *s) {
    uint64_t now = n_clock_monotonic();
    if (now < s->retry_at) return false;
    struct n_audio_ring *old = s->ring;
    size_t old_bytes = s->bytes;
    uint64_t tail = __atomic_load_n(&old->tail, __ATOMIC_ACQUIRE), head = old->head;
    struct n_audio_request q = {.op = N_AUDIO_OP_OPEN, .generation = old->generation, .format = s->format, .first = tail, .base = old->base,
                                .underruns = old->underruns, .underrun_ns = old->underrun_ns};
    if (s->channel >= 0) n_handle_close(s->channel);
    s->channel = -1;
    n_result r = stream_connect(s, &q);
    if (r < 0) {
        s->ring = old;
        s->bytes = old_bytes;
        s->retry_at = now + RETRY_NS;
        return false;
    }
    struct n_audio_ring *now_ring = s->ring;
    uint8_t *from = (uint8_t *)old + old->data_offset, *to = (uint8_t *)now_ring + now_ring->data_offset;
    uint32_t frame = old->frame_bytes;
    if (head - tail > now_ring->capacity) tail = head - now_ring->capacity;
    for (uint64_t at = tail; at < head; at++) memcpy(to + at % now_ring->capacity * frame, from + at % old->capacity * frame, frame);
    n_memory_release(old, old_bytes);
    if (head != tail) {
        __atomic_store_n(&now_ring->head, head, __ATOMIC_RELEASE);
        __atomic_exchange_n(&now_ring->asleep, 0, __ATOMIC_ACQ_REL);
        struct n_audio_request kick = {.op = N_AUDIO_OP_KICK};
        n_channel_send(s->channel, &kick, sizeof kick, -1);
    }
    return true;
}

n_result n_audio_submit(n_audio_stream *s, const void *frames, size_t count, uint32_t flags) {
    if (flags & ~N_AUDIO_WAIT) return -N_ERR_INVALID;
    if (!count) return 0;
    for (;;) {
        struct n_audio_ring *r = s->ring;
        if (__atomic_load_n(&r->broken, __ATOMIC_ACQUIRE)) return -N_ERR_PROTOCOL;
        uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE), keep = __atomic_load_n(&r->keep, __ATOMIC_ACQUIRE), head = r->head;
        uint64_t queued = head - tail;
        uint64_t room = queued < r->queue ? r->queue - queued : 0, by_keep = keep + r->capacity - head;
        if (room > by_keep) room = by_keep;
        if (room) {
            size_t take = count < room ? count : (size_t)room;
            uint32_t frame = r->frame_bytes;
            uint8_t *data = (uint8_t *)r + r->data_offset;
            uint64_t at = head % r->capacity;
            size_t first = (size_t)(r->capacity - at) < take ? (size_t)(r->capacity - at) : take;
            memcpy(data + at * frame, frames, first * frame);
            if (take > first) memcpy(data, (const uint8_t *)frames + first * frame, (take - first) * frame);
            __atomic_store_n(&r->head, head + take, __ATOMIC_SEQ_CST);
            if (__atomic_exchange_n(&r->asleep, 0, __ATOMIC_SEQ_CST)) {
                struct n_audio_request kick = {.op = N_AUDIO_OP_KICK};
                if (n_channel_send(s->channel, &kick, sizeof kick, -1) < 0 && !stream_again(s)) return -N_ERR_NOT_READY;
            }
            return (n_result)take;
        }
        uint32_t bell = __atomic_load_n(&r->room_bell, __ATOMIC_ACQUIRE);
        if (peer_gone(s)) {
            if (stream_again(s)) continue;
            if (!(flags & N_AUDIO_WAIT)) return -N_ERR_NOT_READY;
            n_sleep_until(n_clock_after(RETRY_NS));
            continue;
        }
        if (!(flags & N_AUDIO_WAIT)) return -N_ERR_NOT_READY;
        __atomic_store_n(&r->wants_room, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) != tail || __atomic_load_n(&r->keep, __ATOMIC_ACQUIRE) != keep) continue;
        wait_on_address(&r->room_bell, bell, n_clock_after(WAIT_LOOK_NS));
    }
}

/* 動かない輪だけ相手の生死を見る。鳴っている間はシステムコールが無い。 */
n_result n_audio_status(n_audio_stream *s, struct n_audio_status *out) {
    struct n_audio_ring *r = s->ring;
    if (__atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE) == s->seen_sequence && peer_gone(s) && stream_again(s)) r = s->ring;
    struct n_audio_status st;
    uint64_t tail, base, played;
    for (;;) {
        uint32_t sequence = __atomic_load_n(&r->sequence, __ATOMIC_ACQUIRE);
        if (sequence & 1) continue;
        tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        base = r->base;
        played = r->played;
        st.played_at_ns = r->played_at_ns;
        st.underruns = r->underruns;
        st.underrun_ns = r->underrun_ns;
        st.pace_ppb = r->pace_ppb;
        st.generation = r->generation;
        st.flags = r->flags;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&r->sequence, __ATOMIC_RELAXED) == sequence) {
            s->seen_sequence = sequence;
            break;
        }
    }
    uint64_t head = r->head;
    if (tail < base) tail = base;
    if (played < base) played = base;
    st.queued_frames = (uint32_t)(head - tail);
    st.device_pending_frames = (uint32_t)(tail > played ? tail - played : 0);
    st.played_frames = played - base;
    st.committed_frames = head - base;
    *out = st;
    return 0;
}

n_result n_audio_flush(n_audio_stream *s) {
    if (peer_gone(s) && !stream_again(s)) return -N_ERR_NOT_READY;
    struct n_audio_request q = {.op = N_AUDIO_OP_FLUSH};
    struct n_audio_reply reply;
    n_result r = n_channel_send(s->channel, &q, sizeof q, -1);
    if (r >= 0) r = n_channel_receive(s->channel, &reply, sizeof reply, N_CHANNEL_WAIT, NULL);
    if (r < 0) return r;
    return r == sizeof reply ? reply.result : -N_ERR_PROTOCOL;
}

void n_audio_close(n_audio_stream *s) {
    struct n_audio_ring *r = s->ring;
    /* 残りが鳴り終わるまで。相手が居なくなれば、もう鳴らない。 */
    uint64_t head = r->head;
    uint64_t rate = s->format.rate ? s->format.rate : 1;
    uint64_t left = head - __atomic_load_n(&r->played, __ATOMIC_ACQUIRE);
    uint64_t until = n_clock_monotonic() + left * N_NS_PER_S / rate + CLOSE_EXTRA_NS;
    while (__atomic_load_n(&r->played, __ATOMIC_ACQUIRE) < head && !__atomic_load_n(&r->broken, __ATOMIC_ACQUIRE) && n_clock_monotonic() < until && !peer_gone(s)) {
        uint32_t bell = __atomic_load_n(&r->room_bell, __ATOMIC_ACQUIRE);
        __atomic_store_n(&r->wants_room, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&r->played, __ATOMIC_ACQUIRE) >= head) break;
        wait_on_address(&r->room_bell, bell, n_clock_after(WAIT_LOOK_NS));
    }
    if (s->channel >= 0) n_handle_close(s->channel);
    n_memory_release(r, s->bytes);
    free(s);
}

n_result n_audio_outputs(struct n_audio_output **out) {
    *out = NULL;
    n_handle channel = n_service_connect(N_AUDIO_SERVICE);
    if (channel < 0) return channel;
    struct n_audio_request q = {.op = N_AUDIO_OP_OUTPUTS};
    struct n_audio_reply reply;
    n_result r = n_channel_send(channel, &q, sizeof q, -1);
    if (r >= 0) r = n_channel_receive(channel, &reply, sizeof reply, N_CHANNEL_WAIT, NULL);
    if (r >= 0) r = r == sizeof reply ? reply.result : -N_ERR_PROTOCOL;
    if (r >= 0 && reply.bytes) {
        void *message = malloc((size_t)reply.bytes);
        r = message ? n_channel_receive(channel, message, (size_t)reply.bytes, N_CHANNEL_WAIT, NULL) : -N_ERR_NO_MEMORY;
        if (r >= 0) *out = message;
        else free(message);
    } else if (r >= 0) r = 0;
    n_handle_close(channel);
    return r;
}

char *n_audio_output_name(void) {
    struct n_audio_output *list;
    n_result bytes = n_audio_outputs(&list);
    if (bytes <= 0) return NULL;
    char *name = NULL;
    for (const uint8_t *at = (const uint8_t *)list; at < (const uint8_t *)list + bytes; at += ((const struct n_audio_output *)at)->bytes) {
        const struct n_audio_output *o = (const void *)at;
        if (o->playing) name = strdup(n_audio_device_name(n_audio_output_device(o)));
    }
    free(list);
    return name;
}

n_handle n_audio_output_watch(void) {
    n_handle channel = n_service_connect(N_AUDIO_SERVICE);
    if (channel < 0) return channel;
    struct n_audio_request q = {.op = N_AUDIO_OP_WATCH};
    struct n_audio_reply reply;
    n_result r = n_channel_send(channel, &q, sizeof q, -1);
    if (r >= 0) r = n_channel_receive(channel, &reply, sizeof reply, N_CHANNEL_WAIT, NULL);
    if (r >= 0) r = r == sizeof reply ? reply.result : -N_ERR_PROTOCOL;
    if (r < 0) {
        n_handle_close(channel);
        return r;
    }
    return channel;
}

n_result n_audio_output_watch_take(n_handle watch) {
    uint8_t change;
    n_result count = 0, r;
    while ((r = n_channel_receive(watch, &change, sizeof change, 0, NULL)) == sizeof change) count++;
    return count ? count : r;
}
