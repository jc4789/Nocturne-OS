/* Network device interface shared by the NIC drivers and the protocol stack. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct n_netstats;
struct net_adapter;
struct hal_device;

/* What the adapter does for the stack itself (struct netif.offloads). The stack reads them and
   saves its own work where they are declared; an adapter declares what its hardware does for
   every frame it is handed, never one that depends on a setting it cannot see. */
#define NET_OFFLOAD_CSUM_TX 1u /* completes the IPv4 header checksum and the TCP or UDP checksum of a frame
                                  sent with NET_TX_*_CSUM (the frame carries the pseudo header's sum) */
#define NET_OFFLOAD_CSUM_RX 2u /* checks the IPv4 header checksum and the TCP or UDP checksum of a received
                                  frame and says so with NET_RX_CSUM_OK */

/* One frame to send: Ethernet header, IPv4 with a 20-byte header, then the transport segment,
   laid out as consecutive parts. The parts and what they point into stay until transmit returns. */
struct net_tx_part {
    const void *data;
    size_t length;
};
#define NET_TX_PARTS 2
#define NET_TX_IP_CSUM 1u  /* the IPv4 header's checksum is not filled in */
#define NET_TX_TCP_CSUM 2u /* the TCP checksum holds the pseudo header's sum: add the segment, fold, complement */
#define NET_TX_UDP_CSUM 4u /* the same for UDP */
struct net_tx_frame {
    struct net_tx_part parts[NET_TX_PARTS];
    unsigned part_count;
    size_t length;    /* all the parts together, at least 60 */
    uint32_t offload; /* NET_TX_*_CSUM; only for an adapter with NET_OFFLOAD_CSUM_TX */
};

/* A received frame, without its FCS, borrowed until the call returns. flags: NET_RX_*. */
#define NET_RX_CSUM_OK 1u /* an adapter with NET_OFFLOAD_CSUM_RX checked the IPv4 header and the TCP or UDP segment */
struct netif;
typedef void (*net_input_fn)(struct netif *dev, const void *frame, size_t length, unsigned flags);

/* An adapter. The stack makes every call into it under its own lock (net.c's net_lock), so one
   at a time, from any CPU, in a task that may sleep. What a driver shares with its interrupt
   handler, or with another context of its own (a USB or VMBus callback), it guards with a lock
   of its own. A driver with several devices has a struct netif for each and finds its device
   from self.

   The hardware has tx_queues transmit queues and rx_queues receive queues (at least 1 each): the
   stack keeps frames for each transmit queue, chosen by the flow, and takes in every receive
   queue in turn.
   transmit: the first frames of the array that the adapter took, in order, from 0 to count.
     One that waits for a queue's room is not taken (the stack keeps it and offers it again after
     the adapter's notice, net_rx_notify, that room has come). A frame the adapter can never send
     (the link is down, the device went) is taken and dropped, so a queue never waits on it.
   poll: up to budget frames of the queue, each handed to input; the number handed over.
   rx_pending: the queue holds something poll would take, or the adapter needs poll for another
     reason (a completion to return). */
struct netif {
    const char *driver;
    uint8_t mac[6];
    unsigned tx_queues, rx_queues;
    unsigned offloads; /* NET_OFFLOAD_* */
    unsigned (*transmit)(struct netif *self, unsigned queue, const struct net_tx_frame *frames, unsigned count);
    size_t (*poll)(struct netif *self, unsigned queue, size_t budget, net_input_fn input);
    bool (*rx_pending)(struct netif *self, unsigned queue);
    void (*get_stats)(struct netif *self, struct n_netstats *stats);
    struct net_adapter *adapter; /* the stack's, from net_register */
    struct hal_device *hal;      /* the device it is of, from net_register */
};

/* An adapter is there (from its attach, or a USB arrival later), the offer of the device hal. The
   stack calls it from now on and says so to the service "network", which gives it its address;
   the ledger shows the device offering a network adapter (N_OFFER_NET). A task that may sleep,
   no driver lock held. The same MAC address coming back is the same adapter to the system. */
void net_register(struct hal_device *hal, struct netif *dev);
/* It is gone: the stack never calls it again after this returns, its connections end, the frames
   it still held for it are dropped. A task that may sleep, no driver lock held. */
void net_unregister(struct netif *dev);
/* hand a received frame (without FCS) to the stack; any context, an interrupt handler too */
void net_rx(struct netif *dev, const void *frame, size_t len, unsigned flags);
/* Wake the net thread without copying a packet; any context, an interrupt handler too. The
   stack never polls an adapter on a period: the net thread calls rx_pending and poll when
   something woke it, so a frame the adapter holds waits for its notice, and frames transmit did
   not take wait for the notice that room has come (a transmit completion). */
void net_rx_notify(void);

/* The index-th adapter (from 0): its MAC and IPv4 address, host byte order, ip 0 before it has
   one. False past the last. Any context, an interrupt handler too. */
bool net_adapter_info(size_t index, uint8_t mac[6], uint32_t *ip, uint32_t *mask);
