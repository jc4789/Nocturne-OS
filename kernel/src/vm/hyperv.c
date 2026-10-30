/* Hyper-V detection and the bits of its CPUID/MSR interface the rest of the kernel needs. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "vm/hyperv.h"
#include "mm/pmm.h"
#include "abi.h"

struct hv_info hv;
/* Set once, under reference_lock (taken alone, only the PMM's lock under it); read without it.
   The reference MSR is the partition's, so any CPU may write it. */
static struct spinlock reference_lock;
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
    uint64_t page = __atomic_load_n(&reference_page, __ATOMIC_ACQUIRE);
    if (page) return page;
    uint64_t f = spin_lock(&reference_lock);
    if (!(page = reference_page) && (page = pmm_alloc_zeroed())) {
        /* Preserve the MSR's reserved bits per TLFS. This page remains hypervisor
           owned for the partition lifetime and is never returned to the PMM. */
        uint64_t previous = rdmsr(HV_MSR_REFERENCE_TSC);
        wrmsr(HV_MSR_REFERENCE_TSC, page | (previous & 0xFFE) | 1);
        __atomic_store_n(&reference_page, page, __ATOMIC_RELEASE);
    }
    spin_unlock(&reference_lock, f);
    return page;
}

/* An overlay too: off before a restored image is copied over its frame, on again after. */
void hv_reference_suspend(void) {
    if (reference_page) wrmsr(HV_MSR_REFERENCE_TSC, rdmsr(HV_MSR_REFERENCE_TSC) & ~1ULL);
}

void hv_reference_resume(void) {
    if (reference_page) wrmsr(HV_MSR_REFERENCE_TSC, reference_page | (rdmsr(HV_MSR_REFERENCE_TSC) & 0xFFE) | 1);
}

bool hv_reference_time(uint64_t *units_100ns) {
    if (!hv.present || !(hv.features & HV_ACCESS_TIME_REF_COUNT)) return false;
    uint64_t page = __atomic_load_n(&reference_page, __ATOMIC_ACQUIRE);
    if (page) {
        const struct n_reference_tsc *p = phys_to_virt(page);
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
