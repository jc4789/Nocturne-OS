/* VMBus: the Hyper-V bus that carries every synthetic device (keyboard, mouse, disk, network...). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

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
typedef void (*vmbus_callback_t)(struct vmbus_channel *ch);

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
    struct hv_ring *out, *in;
    uint8_t *out_data, *in_data;
    uint32_t out_size, in_size;
    vmbus_callback_t callback; /* runs in interrupt context when the host signals */
    vmbus_callback_t on_rescind; /* interrupt context: the host withdrew the channel */
    void *priv;
    bool claimed;  /* a driver owns it: not freed on rescind until vmbus_release() */
    struct vmbus_channel *next;
};

/* VMBus socket channels (Hyper-V sockets, chn_flags bit 13) */
#define VMBUS_CHANNEL_SOCKET 0x2000

extern int vmbus_version_major, vmbus_version_minor;

/* Connect to VMBus and collect the device offers. False if this is not Hyper-V. */
bool vmbus_init(void);
bool vmbus_ready(void);
/* The n-th offered channel of a device type, or NULL. */
struct vmbus_channel *vmbus_find(const struct guid *type, int n);
/* Allocate the rings and open the channel. Sizes are rounded up to pages. 0 or -errno. */
int vmbus_open(struct vmbus_channel *ch, uint32_t out_bytes, uint32_t in_bytes, vmbus_callback_t cb, void *priv);
/* Close an open channel and free its rings. */
void vmbus_close(struct vmbus_channel *ch);
/* Give up a claimed channel: it is freed (and its relid handed back) once the host rescinds it. */
void vmbus_release(struct vmbus_channel *ch);
/* Socket channels offered after start-up go to this hook, in interrupt context. It sets
   ch->claimed to keep one; unclaimed ones are freed when the host rescinds them. */
void vmbus_set_socket_hook(void (*hook)(struct vmbus_channel *ch));
/* Share guest memory with the host: returns a GPADL handle, or 0 on failure. */
uint32_t vmbus_gpadl_create(struct vmbus_channel *ch, uint64_t pa, uint32_t pages);
void vmbus_gpadl_teardown(struct vmbus_channel *ch, uint32_t gpadl);
/* Send an in-band packet. 0, or -EAGAIN when the ring is full. */
int vmbus_send(struct vmbus_channel *ch, uint16_t type, uint16_t flags, const void *data, uint32_t len,
               uint64_t trans_id);
/* Send a packet made of a raw header plus data (for GPA-direct packets). */
int vmbus_send_raw(struct vmbus_channel *ch, const void *hdr, uint32_t hdr_len, const void *data, uint32_t len);
/* Copy the next received packet (descriptor and all) into buf. Returns its length, 0 if the ring
   is empty, or -E2BIG (the packet is dropped) if it does not fit. */
int vmbus_recv(struct vmbus_channel *ch, void *buf, uint32_t len);
/* Wait (with interrupts on) until *flag is set; false on timeout. */
bool vmbus_wait(volatile bool *flag, uint64_t ms);
void guid_format(const struct guid *g, char *out); /* 37 bytes */

void hv_input_init(void); /* hv_input.c: synthetic keyboard and mouse */
void storvsc_init(void);  /* storvsc.c: synthetic SCSI disks */
void hv_util_init(void);  /* hv_util.c: heartbeat, shutdown, time sync */
void hvsock_init(void);   /* hvsock.c: Hyper-V sockets */
