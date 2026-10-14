/* Network device interface shared by the NIC drivers and the protocol stack. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct netif {
    const char *driver;
    uint8_t mac[6];
    int (*send)(const void *frame, size_t len); /* frame without FCS, >= 60 bytes */
    /* Optional DMA receive path. Called only by the net thread; input must
       consume the borrowed frame synchronously, before poll_rx returns it to
       the NIC. rx_pending is also safe with interrupts disabled. */
    bool (*rx_pending)(void);
    size_t (*poll_rx)(void (*input)(const void *frame, size_t len));
};

void net_register(struct netif *nif);
/* hand a received frame (without FCS) to the stack; safe to call from IRQ context */
void net_rx(const void *frame, size_t len);
/* Wake the net thread without copying a packet; safe from interrupt context.
   Also used for TX completion so a resource-blocked sender can retry. */
void net_rx_notify(void);

bool e1000_probe(void);
bool tulip_probe(void);
bool netvsc_probe(void); /* hv/netvsc.c: Hyper-V synthetic network adapter */

/* The adapter's MAC and IPv4 configuration (host byte order). False before DHCP has finished.
   Safe from interrupt context. */
bool net_config(uint8_t mac[6], uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns);

/* busy-wait helper usable before the scheduler can block us */
void net_delay_ms(uint64_t ms);
void net_process_exit(int pid);
