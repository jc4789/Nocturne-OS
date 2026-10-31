#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Hyper-V synthetic MSRs */
#define HV_MSR_GUEST_OS_ID   0x40000000
#define HV_MSR_HYPERCALL     0x40000001
#define HV_MSR_VP_INDEX      0x40000002
#define HV_MSR_TSC_FREQUENCY 0x40000022

/* CPUID 0x40000003 EAX: which MSRs the partition may access */
#define HV_ACCESS_SYNIC          (1u << 2)
#define HV_ACCESS_SYNTH_TIMERS   (1u << 3)
#define HV_ACCESS_HYPERCALL      (1u << 5)
#define HV_ACCESS_VP_INDEX       (1u << 6)
#define HV_ACCESS_FREQUENCY_MSRS (1u << 11)
/* CPUID 0x40000003 EBX: which hypercalls the partition may make (the high half of the TLFS's
   HV_PARTITION_PRIVILEGE_MASK) */
#define HV_PRIV_POST_MESSAGES    (1u << 4)
#define HV_PRIV_SIGNAL_EVENTS    (1u << 5)

struct hv_info {
    bool present;          /* running on Hyper-V ("Microsoft Hv") */
    uint32_t max_leaf;
    uint32_t features;     /* CPUID 0x40000003 EAX */
    uint32_t privileges;   /* CPUID 0x40000003 EBX */
    uint32_t misc;         /* CPUID 0x40000003 EDX */
    uint32_t build, major, minor;
};
extern struct hv_info hv;

/* The interface's presence and features; the TSC's rate to the clock where Hyper-V states it. */
void hv_detect(void);
