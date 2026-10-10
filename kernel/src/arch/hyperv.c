/* Hyper-V detection and the bits of its CPUID/MSR interface the rest of the kernel needs. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/hyperv.h"
#include "mm/pmm.h"
#include "abi.h"

struct hv_info hv;
static uint64_t reference_page;

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

uint64_t hv_tsc_hz(void) {
    if (!hv.present || !(hv.features & HV_ACCESS_FREQUENCY_MSRS)) return 0;
    return rdmsr(HV_MSR_TSC_FREQUENCY);
}

uint64_t hv_reference_tsc_page(void) {
    if (!hv.present || !(hv.features & HV_ACCESS_REFERENCE_TSC) ||
        !(hv.features & HV_ACCESS_TIME_REF_COUNT)) return 0;
    if (reference_page) return reference_page;
    uint64_t page = pmm_alloc_zeroed();
    if (!page) return 0;
    /* Preserve the MSR's reserved bits per TLFS. This page remains hypervisor
       owned for the partition lifetime and is never returned to the PMM. */
    uint64_t previous = rdmsr(HV_MSR_REFERENCE_TSC);
    wrmsr(HV_MSR_REFERENCE_TSC, page | (previous & 0xFFE) | 1);
    reference_page = page;
    return page;
}

bool hv_reference_time(uint64_t *units_100ns) {
    if (!hv.present || !(hv.features & HV_ACCESS_TIME_REF_COUNT)) return false;
    if (reference_page) {
        const struct n_reference_tsc *p = phys_to_virt(reference_page);
        for (unsigned attempt = 0; attempt < 4; attempt++) {
            uint32_t sequence = __atomic_load_n(&p->sequence, __ATOMIC_ACQUIRE);
            if (!sequence) break;
            uint32_t low, high;
            __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
            uint64_t tsc = (uint64_t)high << 32 | low;
            uint64_t scale = p->scale;
            int64_t offset = p->offset;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (sequence != __atomic_load_n(&p->sequence, __ATOMIC_ACQUIRE)) continue;
            *units_100ns = (uint64_t)(((__uint128_t)tsc * scale) >> 64) + offset;
            return true;
        }
    }
    /* Sequence zero explicitly means the enlightenment is unavailable, e.g.
       during restore/migration; the reference counter stays rate constant. */
    *units_100ns = rdmsr(HV_MSR_TIME_REF_COUNT);
    return true;
}
