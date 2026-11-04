/* Network device interface shared by the NIC drivers and the protocol stack. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct n_netstats;
struct net_adapter;

/* An adapter. The stack makes every call into it under its own lock (net.c's net_lock), so one
   at a time, from any CPU, in a task that may sleep: system calls send, the net thread sends,
   polls and asks for the counters. What a driver shares with its interrupt handler, or with
   another context of its own (a USB or VMBus callback), it guards with a lock of its own. A
   driver with several devices has a struct netif for each and finds its device from self. */
struct netif {
    const char *driver;
    uint8_t mac[6];
    void (*get_stats)(struct netif *self, struct n_netstats *stats);
    struct net_adapter *adapter; /* the stack's, from net_register */
};

/* An adapter is there (from its probe, or a USB arrival later). The stack calls it from now on
   and says so to the service "network", which gives it its address. A task that may sleep, no
   driver lock held. The same MAC address coming back is the same adapter to the system. */
void net_register(struct netif *dev);
/* It is gone: the stack never calls it again after this returns, its connections end. A task
   that may sleep, no driver lock held. */
void net_unregister(struct netif *dev);
/* hand a received frame (without FCS) to the stack; any context, an interrupt handler too */
void net_rx(struct netif *dev, const void *frame, size_t len);
/* Wake the net thread without copying a packet; any context, an interrupt handler too. The
   stack never polls an adapter on a period: the net thread calls rx_pending and poll_rx when
   something woke it, so a frame the adapter holds waits for its notice, and a send refused for
   want of room (-N_ERR_NOT_READY or -N_ERR_BUSY) is tried again once the adapter's notice says
   room has come (its transmit completion). */
void net_rx_notify(void);

/* The index-th adapter (from 0): its MAC and IPv4 address, host byte order, ip 0 before it has
   one. False past the last. Any context, an interrupt handler too. */
bool net_adapter_info(size_t index, uint8_t mac[6], uint32_t *ip, uint32_t *mask);
