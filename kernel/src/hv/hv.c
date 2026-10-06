/* Hyper-V hypercalls and the synthetic interrupt controller (SynIC).
   The hypercall page is filled in by the hypervisor through its physical address; we map it
   read+execute only, so W^X holds for it too. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/hyperv.h"
#include "hv/hv.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define HV_MSR_SCONTROL 0x40000080
#define HV_MSR_SIEFP    0x40000082
#define HV_MSR_SIMP     0x40000083
#define HV_MSR_EOM      0x40000084
#define HV_MSR_SINT0    0x40000090

#define HVCALL_POST_MESSAGE 0x005C
#define HVCALL_SIGNAL_EVENT 0x005D
#define HV_HYPERCALL_FAST   (1ULL << 16)

/* Guest OS identity, "open source" format: bit 63 set, then an OS type and a version. */
#define NOCTURNE_GUEST_ID ((1ULL << 63) | (0x7FULL << 56) | (0x0900ULL << 16))

uint32_t hv_vp_index;
static void *hypercall_page;
static uint8_t *post_buf; /* HvPostMessage input: must not cross a page */
static uint64_t post_buf_pa;
static struct hv_message *msg_page;
static uint8_t *event_page;
static void (*sint_isr)(void);

static uint64_t hypercall(uint64_t control, uint64_t in, uint64_t out) {
    uint64_t status;
    register uint64_t r8 __asm__("r8") = out;
    __asm__ volatile("call *%[pg]"
                     : "=a"(status), "+c"(control), "+d"(in), "+r"(r8)
                     : [pg] "m"(hypercall_page)
                     : "cc", "memory", "r9", "r10", "r11");
    return status;
}

static void vmbus_vector(struct regs *r) {
    (void)r;
    if (sint_isr) sint_isr();
}

bool hv_core_init(void (*isr)(void)) {
    uint32_t need = HV_ACCESS_HYPERCALL | HV_ACCESS_SYNIC | HV_ACCESS_VP_INDEX;
    if (!hv.present || (hv.features & need) != need) return false;

    wrmsr(HV_MSR_GUEST_OS_ID, NOCTURNE_GUEST_ID);
    uint64_t hc_pa = pmm_alloc_zeroed();
    wrmsr(HV_MSR_HYPERCALL, (rdmsr(HV_MSR_HYPERCALL) & 0xFFE) | hc_pa | 1);
    if (!(rdmsr(HV_MSR_HYPERCALL) & 1)) {
        kprintf("hyperv: the hypercall page could not be enabled\n");
        return false;
    }
    hypercall_page = vmalloc_map_phys(hc_pa, 1, 0); /* read + execute */
    post_buf_pa = pmm_alloc_zeroed();
    post_buf = phys_to_virt(post_buf_pa);
    hv_vp_index = (uint32_t)rdmsr(HV_MSR_VP_INDEX);

    uint64_t simp = pmm_alloc_zeroed(), siefp = pmm_alloc_zeroed();
    msg_page = phys_to_virt(simp);
    event_page = phys_to_virt(siefp);
    wrmsr(HV_MSR_SIMP, simp | 1);
    wrmsr(HV_MSR_SIEFP, siefp | 1);
    sint_isr = isr;
    vector_register(VEC_VMBUS, vmbus_vector);
    wrmsr(HV_MSR_SINT0 + VMBUS_MESSAGE_SINT, VEC_VMBUS); /* unmasked, no auto-EOI */
    wrmsr(HV_MSR_SCONTROL, rdmsr(HV_MSR_SCONTROL) | 1);
    kprintf("hyperv: hypercalls and SynIC enabled (VP %u)\n", hv_vp_index);
    return true;
}

uint16_t hv_post_message(uint32_t connection_id, uint32_t type, const void *data, uint32_t len) {
    if (len > HV_MESSAGE_PAYLOAD_BYTES) return 5; /* HV_STATUS_INVALID_PARAMETER */
    uint64_t f = irq_save();
    uint32_t *h = (uint32_t *)post_buf;
    h[0] = connection_id;
    h[1] = 0;
    h[2] = type;
    h[3] = len;
    memcpy(post_buf + 16, data, len);
    uint16_t st = (uint16_t)hypercall(HVCALL_POST_MESSAGE, post_buf_pa, 0);
    irq_restore(f);
    return st;
}

uint16_t hv_signal_event(uint32_t connection_id) {
    return (uint16_t)hypercall(HVCALL_SIGNAL_EVENT | HV_HYPERCALL_FAST, connection_id, 0);
}

struct hv_message *hv_message_slot(int sint) { return &msg_page[sint]; }

volatile uint64_t *hv_event_flags(int sint) { return (volatile uint64_t *)(event_page + sint * 256); }

void hv_message_done(struct hv_message *m) {
    m->type = 0;
    hv_mb();
    if (m->flags & 1) wrmsr(HV_MSR_EOM, 0); /* the hypervisor has another message queued */
}
