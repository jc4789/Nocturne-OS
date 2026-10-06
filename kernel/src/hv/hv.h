/* Hyper-V hypercalls and the synthetic interrupt controller (SynIC). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HV_STATUS_SUCCESS               0
#define HV_STATUS_INSUFFICIENT_BUFFERS  0x13
#define HV_STATUS_INVALID_CONNECTION_ID 0x12

#define HV_MESSAGE_PAYLOAD_BYTES 240
#define VMBUS_MESSAGE_SINT 2
#define VEC_VMBUS 0x31

/* One slot of the SynIC message page. */
struct hv_message {
    uint32_t type;      /* 0 = slot empty */
    uint8_t payload_size;
    uint8_t flags;      /* bit 0: another message is pending */
    uint16_t reserved;
    uint64_t sender;
    uint8_t payload[HV_MESSAGE_PAYLOAD_BYTES];
} __attribute__((packed));

extern uint32_t hv_vp_index;

/* Set up the hypercall page and the SynIC; SINT2 interrupts go to `isr`. False if unavailable. */
bool hv_core_init(void (*isr)(void));
/* HvPostMessage: send a message to the host on a connection. Returns an HV status. */
uint16_t hv_post_message(uint32_t connection_id, uint32_t type, const void *data, uint32_t len);
/* HvSignalEvent: signal the host on an event connection. */
uint16_t hv_signal_event(uint32_t connection_id);
/* The message slot and event flags (2048 bits) for a SINT. */
struct hv_message *hv_message_slot(int sint);
volatile uint64_t *hv_event_flags(int sint);
/* Release the current message slot (call after copying it out). */
void hv_message_done(struct hv_message *m);

static inline void hv_mb(void) { __asm__ volatile("mfence" ::: "memory"); }
