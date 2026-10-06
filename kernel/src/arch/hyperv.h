#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Hyper-V synthetic MSRs */
#define HV_MSR_GUEST_OS_ID   0x40000000
#define HV_MSR_HYPERCALL     0x40000001
#define HV_MSR_VP_INDEX      0x40000002
#define HV_MSR_TIME_REF_COUNT 0x40000020
#define HV_MSR_REFERENCE_TSC 0x40000021
#define HV_MSR_TSC_FREQUENCY 0x40000022
#define HV_MSR_APIC_FREQUENCY 0x40000023

/* CPUID 0x40000003 EAX: which MSRs the partition may access */
#define HV_ACCESS_TIME_REF_COUNT (1u << 1)
#define HV_ACCESS_SYNIC          (1u << 2)
#define HV_ACCESS_SYNTH_TIMERS   (1u << 3)
#define HV_ACCESS_HYPERCALL      (1u << 5)
#define HV_ACCESS_VP_INDEX       (1u << 6)
#define HV_ACCESS_REFERENCE_TSC  (1u << 9)
#define HV_ACCESS_FREQUENCY_MSRS (1u << 11)

struct hv_info {
    bool present;          /* running on Hyper-V ("Microsoft Hv") */
    uint32_t max_leaf;
    uint32_t features;     /* CPUID 0x40000003 EAX */
    uint32_t privileges;   /* CPUID 0x40000003 EBX */
    uint32_t misc;         /* CPUID 0x40000003 EDX */
    uint32_t build, major, minor;
};
extern struct hv_info hv;

void hv_detect(void);
/* APIC timer input frequency in Hz, or 0 if the hypervisor does not report it */
uint64_t hv_apic_hz(void);
uint64_t hv_tsc_hz(void); /* the TSC's, likewise */
