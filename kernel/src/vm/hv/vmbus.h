/* VMBus: the Hyper-V bus that carries every synthetic device (keyboard, mouse, disk, network...). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "arch/spin.h"
#include "hal/hal.h"

struct guid {
    uint8_t b[16];
};
/* GUIDs are stored with their first three fields little-endian, like Windows does. */
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                         \
    {{(a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF,           \
      ((b) >> 8) & 0xFF, (c) & 0xFF, ((c) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7}}

/* VMBus packet types */
#define VM_PKT_DATA_INBAND     6
#define VM_PKT_DATA_XFER_PAGES 7
#define VM_PKT_DATA_GPA_DIRECT 9
#define VM_PKT_COMP            11
#define VMBUS_FLAG_COMPLETION_REQUESTED 1

struct vmpacket_desc {
    uint16_t type;
    uint16_t offset8; /* payload offset, in 8-byte units */
    uint16_t len8;    /* whole packet length, in 8-byte units */
    uint16_t flags;
    uint64_t trans_id;
} __attribute__((packed));

/* the payload of a received packet */
static inline void *vmpacket_data(void *pkt) { return (uint8_t *)pkt + ((struct vmpacket_desc *)pkt)->offset8 * 8; }
static inline uint32_t vmpacket_datalen(void *pkt) {
    struct vmpacket_desc *d = pkt;
    return (uint32_t)(d->len8 - d->offset8) * 8;
}

/* the header page of a ring buffer, shared with the host */
struct hv_ring {
    volatile uint32_t write_index;
    volatile uint32_t read_index;
    volatile uint32_t interrupt_mask;
    volatile uint32_t pending_send_sz;
    uint32_t reserved1[12];
    volatile uint32_t feature_bits;
} __attribute__((packed));

struct vmbus_channel;
struct vmbus_gpadl;
typedef void (*vmbus_callback_t)(struct vmbus_channel *ch);

/* rescinded, opened, claimed, callback, priv, on_rescind and node change only inside vmbus.c (under
   its channel lock): a driver reads rescinded as a hint and sets on_rescind with vmbus_set_rescind. */
struct vmbus_channel {
    struct guid type, instance;
    uint16_t chn_flags;
    uint16_t mmio_mb;
    uint8_t user_def[120];
    uint16_t sub_index;
    uint32_t relid;
    uint32_t conn_id;
    bool rescinded;
    bool opened;
    /* rings: outbound (guest to host) pages, then inbound */
    uint8_t *ring;
    uint32_t ring_pages_out, ring_pages_in;
    uint32_t ring_gpadl;
    struct vmbus_gpadl *gpadls; /* host がまだ持つかもしれない頁の一覧（vmbus.c、channel の錠の下） */
    struct hv_ring *out, *in;
    uint8_t *out_data, *in_data;
    uint32_t out_size, in_size;
    struct spinlock out_lock, in_lock; /* one sender, one receiver at a time; each taken alone */
    vmbus_callback_t callback; /* CPU 0's interrupt handler, when the host signals */
    vmbus_callback_t on_rescind; /* CPU 0's interrupt handler: the host withdrew the channel */
    void *priv;
    bool claimed;  /* a driver owns it: not freed on rescind until vmbus_release() */
    /* The channel's node in the device tree (N_BUS_VMBUS), for the devices' channels and not the
       sockets'. node says where the node is between its offer and its removal (vmbus.c); the
       others link the channel into the thread that publishes and removes nodes. */
    struct hal_device hal;
    uint8_t node;
    bool node_rescind, node_queued;
    struct vmbus_channel *node_next;
    struct vmbus_channel *next;
};

/* VMBus socket channels (Hyper-V sockets, chn_flags bit 13) */
#define VMBUS_CHANNEL_SOCKET 0x2000

extern int vmbus_version_major, vmbus_version_minor;

/* Connect to VMBus and collect the device offers. False if this is not Hyper-V. CPU 0, at boot.
   Each offered device's channel becomes a node under the root (bus N_BUS_VMBUS, its type GUID
   as N_PROP_VMBUS_TYPE), published by a thread of its own because an offer arrives in an
   interrupt handler; the host's rescind removes the node (hal_remove, which waits for the
   driver's detach). A driver's rule matches N_VMBUS_TYPE(guid). */
bool vmbus_init(void);
bool vmbus_ready(void);
/* The channel behind a node of bus N_BUS_VMBUS: what attach receives. */
struct vmbus_channel *vmbus_channel_of(struct hal_device *d);
/* The rest from tasks on any CPU, unless said otherwise; the callbacks run in CPU 0's interrupt
   handler and never close, release or open a channel, or wait for a request. */
/* Allocate the rings and open the channel. Sizes are rounded up to pages. 0 or -errno. */
int vmbus_open(struct vmbus_channel *ch, uint32_t out_bytes, uint32_t in_bytes, vmbus_callback_t cb, void *priv);
/* 開いた channel を閉じる。戻った時、channel の callback は走らず、送りも受けも途中に無い。輪の頁は、host が輪の頁の
   一覧を外したと答えた時に返る。答えなければ（host が channel を取り消した時も。取り消しは答えではない）使わずに残す。
   閉じた channel には何もしない。 */
void vmbus_close(struct vmbus_channel *ch);
/* Give up a claimed channel: it is freed (and its relid handed back) once the host rescinds it
   and its node is gone. When it returns, its on_rescind no longer runs. */
void vmbus_release(struct vmbus_channel *ch);
/* What runs when the host rescinds the channel (NULL: nothing). */
void vmbus_set_rescind(struct vmbus_channel *ch, vmbus_callback_t on_rescind);
/* Socket channels offered after start-up go to this hook, in CPU 0's interrupt handler under the
   channel lock (it takes no vmbus call). It returns true to keep one (the channel is then claimed
   before the hook hands it on); unclaimed ones are freed when the host rescinds them. */
void vmbus_set_socket_hook(bool (*hook)(struct vmbus_channel *ch));
/* 客のメモリー（gpa：HAL の DMA の口が返す装置の番地）を host と分ける頁の一覧（GPADL）を作る。番号か 0。channel は
   host が持つかもしれない一覧を、作りの答えが無かったものも、host が外したと答えるまで覚える。バスは覚えた一覧が
   無くなって初めて HAL に装置が止まったと答える（そこで hal_release がメモリーを返す）。 */
uint32_t vmbus_gpadl_create(struct vmbus_channel *ch, uint64_t gpa, uint32_t pages);
/* true：host が外したと答えた。false：メモリーは分けたままで、返しても使い直してもいけない。取り消された channel では
   何も送らずに false。 */
bool vmbus_gpadl_teardown(struct vmbus_channel *ch, uint32_t gpadl);
/* Send an in-band packet. 0, or -N_ERR_NOT_READY when the ring is full. */
int vmbus_send(struct vmbus_channel *ch, uint16_t type, uint16_t flags, const void *data, uint32_t len,
               uint64_t trans_id);
/* Send a packet made of a raw header plus data (for GPA-direct packets). */
int vmbus_send_raw(struct vmbus_channel *ch, const void *hdr, uint32_t hdr_len, const void *data, uint32_t len);
/* Copy the next received packet (descriptor and all) into buf. Returns its length, 0 if the ring
   is empty, or -N_ERR_TOO_LARGE (the packet is dropped) if it does not fit. */
int vmbus_recv(struct vmbus_channel *ch, void *buf, uint32_t len);
/* Readiness only, a hint that takes no lock; both are safe with interrupts disabled. packet_len
   includes the packet descriptor, but excludes the ring's trailing bookkeeping word. */
bool vmbus_rx_pending(struct vmbus_channel *ch);
bool vmbus_write_ready(struct vmbus_channel *ch, uint32_t packet_len);
/* Wait until *flag is set, by a channel's callback or a control message (CPU 0's handler, which
   wakes every such waiter after each run); false on timeout. */
bool vmbus_wait(volatile bool *flag, uint64_t ms);
void guid_format(const struct guid *g, char *out); /* 37 bytes */

/* netvsc.c: the address of the synthetic adapter, which the host asks about (hv_util.c). False without one. */
bool netvsc_ip(uint32_t *ip);
void hvsock_init(void);   /* hvsock.c: Hyper-V sockets */
