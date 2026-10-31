/* Network device interface shared by the NIC drivers and the protocol stack. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct n_netstats;

/* An adapter. The stack makes every call into it under its own lock (net.c's net_lock), so one
   at a time, from any CPU, in a task that may sleep: system calls send, the net thread sends,
   polls and asks for the counters. What a driver shares with its interrupt handler, or with
   another context of its own (a USB or VMBus callback), it guards with a lock of its own. */
struct netif {
    const char *driver;
    uint8_t mac[6];
    int (*send)(const void *frame, size_t len); /* frame without FCS, >= 60 bytes */
    /* Optional DMA receive path, called by the net thread only. input consumes the borrowed
       frame before it returns (poll_rx gives the buffer back to the NIC after). */
    bool (*rx_pending)(void);
    size_t (*poll_rx)(void (*input)(const void *frame, size_t len));
    void (*get_stats)(struct n_netstats *stats);
};

/* An adapter is there (from its probe, or a USB arrival later): the first one becomes the
   stack's. The same adapter registering again has a new mac and driver name (another USB
   adapter in its place), which the stack takes. A task that may sleep, no driver lock held. */
void net_register(struct netif *nif);
/* hand a received frame (without FCS) to the stack; any context, an interrupt handler too */
void net_rx(const void *frame, size_t len);
/* Wake the net thread without copying a packet; any context, an interrupt handler too. The
   stack never polls an adapter on a period: the net thread calls rx_pending and poll_rx when
   something woke it, so a frame the adapter holds waits for its notice, and a send refused for
   want of room (-N_ERR_NOT_READY or -N_ERR_BUSY) is tried again once the adapter's notice says
   room has come (its transmit completion). */
void net_rx_notify(void);

bool rtl8125_probe(void);

/* The adapter's MAC and IPv4 configuration (host byte order). False before DHCP has finished.
   Any context, an interrupt handler too. */
bool net_config(uint8_t mac[6], uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns);
