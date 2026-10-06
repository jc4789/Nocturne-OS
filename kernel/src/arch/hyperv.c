/* Hyper-V detection and the bits of its CPUID/MSR interface the rest of the kernel needs. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/hyperv.h"

struct hv_info hv;

void hv_detect(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 31))) return; /* no hypervisor */
    cpuid(0x40000000, 0, &a, &b, &c, &d);
    if (b != 0x7263694D || c != 0x666F736F || d != 0x76482074) return; /* "Microsoft Hv" */
    hv.max_leaf = a;
    if (a < 0x40000003) return;
    hv.present = true;
    cpuid(0x40000002, 0, &a, &b, &c, &d);
    hv.build = a;
    hv.major = b >> 16;
    hv.minor = b & 0xFFFF;
    cpuid(0x40000003, 0, &a, &b, &c, &d);
    hv.features = a;
    hv.privileges = b;
    hv.misc = d;
    kprintf("hyperv: host build %u (%u.%u), features %x privileges %x misc %x\n", hv.build, hv.major,
            hv.minor, hv.features, hv.privileges, hv.misc);
}

uint64_t hv_apic_hz(void) {
    if (!hv.present || !(hv.features & HV_ACCESS_FREQUENCY_MSRS)) return 0;
    return rdmsr(HV_MSR_APIC_FREQUENCY);
}
