/* VT-d製品コードを直接実行するMMIO模擬検証。実機のDMA/割込みは証明しない。 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include "kernel.h"
#include "arch/cpu.h"

static void model_step(void);
static uint64_t model_irq_save(void) { return 0; }
static void model_irq_restore(uint64_t f) { (void)f; }
#define pause model_step
#define irq_save model_irq_save
#define irq_restore model_irq_restore
#pragma clang diagnostic push
/* 製品はLP64、WindowsホストはLLP64。kernel kprintf自体はこのfixtureで整形しない。 */
#pragma clang diagnostic ignored "-Wformat"
#include "../kernel/src/dev/iommu_intel.c"
#pragma clang diagnostic pop
#undef pause
#undef irq_save
#undef irq_restore

uint64_t hhdm_offset, kernel_pml4, pmm_highest_usable;
static uint64_t registers[PAGE_SIZE / sizeof(uint64_t)];
static struct unit model_unit;
static void *own_queue, *firmware_queue, *semaphore_page;
static uint64_t ticks;
static bool run_queue, run_commands, mapped_slot;
static unsigned failures, completed_waits;
static uint8_t dmar_fixture[64]; /* SDT/DMAR header + one DRHD, as specified in 8.1/8.3 */
static bool have_dmar;
static void **allocations;
static size_t allocation_count;
static unsigned cache_flushes;
static uint32_t error_on_publish;
static bool require_quiet_before_off, protection_dropped_early;
static enum { CFI_WRITABLE, CFI_BLOCKED, CFI_PASS_ONLY } cfi_behavior;
static bool cfi_on_requested, cfi_clear_requested, remapping_bypass_started;
static struct unit *cfi_pass_only_unit;
static bool early_unit_enable, ioapic_pin_masked, ioapic_pin_remappable, compatibility_pin_blocked;
static bool eime_without_sirtp;

#define CHECK(x) do { if (!(x)) { printf("失敗 %d行: %s\n", __LINE__, #x); failures++; } } while (0)

static void model_step_unit(struct unit *u) {
    if (run_commands) {
        uint32_t old = rd32(u, R_GSTS), cmd = rd32(u, R_GCMD);
        uint32_t next = cmd & ~GSTS_ONESHOT;
        bool extended = (rd64(u, R_IRTA) & IRTA_EIME) != 0;
        if ((cmd & GCMD_CFI) && !(old & GCMD_CFI)) cfi_on_requested = true;
        if (!(cmd & GCMD_CFI) && (old & GCMD_CFI)) cfi_clear_requested = true;
        if (cfi_behavior == CFI_BLOCKED) next &= ~GCMD_CFI;
        if (cfi_behavior == CFI_PASS_ONLY || u == cfi_pass_only_unit) next |= GCMD_CFI;
        if (!extended && !(old & GCMD_IRE) && (next & (GCMD_IRE | GCMD_CFI)) == (GCMD_IRE | GCMD_CFI))
            remapping_bypass_started = true;
        if (!extended && !(old & GCMD_IRE) && (next & GCMD_IRE))
            for (size_t i = 0; i < nunits; i++)
                if (rd32(&units[i], R_GSTS) & GCMD_CFI) early_unit_enable = true;
        if (extended && !(old & GCMD_IRE) && (next & GCMD_IRE) && !(old & GCMD_SIRTP))
            eime_without_sirtp = true;
        if (!ioapic_pin_masked && !ioapic_pin_remappable && (next & GCMD_IRE) &&
            (extended || !(next & GCMD_CFI)))
            compatibility_pin_blocked = true;
        if (require_quiet_before_off && old & (GCMD_TE | GCMD_IRE) &&
            ((old & ~next) & (GCMD_TE | GCMD_IRE)) && !completed_waits) protection_dropped_early = true;
        if (cmd & GCMD_SRTP) next |= GCMD_SRTP;
        if (cmd & GCMD_SIRTP) next |= GCMD_SIRTP;
        wr32(u, R_GSTS, next);
        if (old & GCMD_QIE && !(next & GCMD_QIE)) wr64(u, R_IQH, 0);
    }
    if (!run_queue || !(rd32(u, R_GSTS) & GCMD_QIE) ||
        rd32(u, R_FSTS) & FSTS_QUEUE_ERRORS) return;
    uint64_t iqa = rd64(u, R_IQA), head = rd64(u, R_IQH), tail = rd64(u, R_IQT);
    unsigned bytes = PAGE_SIZE << (iqa & IQA_QS), width = iqa & IQA_DW ? 32 : 16;
    if (head != tail && error_on_publish) {
        wr32(u, R_FSTS, error_on_publish);
        error_on_publish = 0;
        return;
    }
    while (head != tail) {
        uint64_t *slot = (uint64_t *)((uintptr_t)(iqa & ~(PAGE_SIZE - 1)) + head);
        if ((slot[0] & 15) == 5 && slot[0] & (1ull << 5)) {
            *(volatile uint32_t *)(uintptr_t)slot[1] = (uint32_t)(slot[0] >> 32);
            completed_waits++;
        }
        head = (head + width) % bytes;
        wr64(u, R_IQH, head);
    }
}

static void model_step(void) {
    for (size_t i = 0; i < nunits; i++) model_step_unit(&units[i]);
}

uint64_t iommu_deadline(unsigned ms) { (void)ms; model_step(); return ticks + 1; }
bool iommu_expired(uint64_t deadline) { model_step(); return ++ticks > deadline; }
void iommu_cache_flush(const volatile void *p, size_t bytes) { (void)p; (void)bytes; cache_flushes++; }
bool iommu_fault_report(void) { return true; }
int kprintf(const char *format, ...) { (void)format; return 0; }
void *kmalloc(size_t size) { return malloc(size); }
void *kzalloc(size_t size) { return calloc(1, size); }
void kfree(void *p) { free(p); }
void *kappend(void *array, size_t *count, size_t *room, size_t size) {
    void **p = array;
    if (*count == *room) {
        size_t next = *room ? *room * 2 : 1;
        void *grown = realloc(*p, next * size);
        if (!grown) return NULL;
        *p = grown;
        *room = next;
    }
    return (uint8_t *)*p + (*count)++ * size;
}
uint64_t iommu_alloc_pages(size_t count) {
    void *p = _aligned_malloc(count * PAGE_SIZE, PAGE_SIZE);
    if (p) {
        void **grown = realloc(allocations, (allocation_count + 1) * sizeof *allocations);
        if (!grown) { _aligned_free(p); return 0; }
        allocations = grown;
        allocations[allocation_count++] = p;
        memset(p, 0, count * PAGE_SIZE);
    }
    return (uintptr_t)p;
}
void pmm_free(uint64_t p) { (void)p; } /* allocation lifetime is fixture-owned */
uint64_t vmm_translate(uint64_t root, uint64_t va) { (void)root; return mapped_slot ? va : 0; }
void *vmm_map_mmio(uint64_t pa, uint64_t size) { (void)size; return (void *)(uintptr_t)pa; }
void iommu_entry_write(volatile uint64_t *entry, uint64_t value) { *entry = value; }
void iommu_cover(uint16_t first, uint16_t last) { (void)first; (void)last; }
void iommu_kernel_id(uint16_t id) { (void)id; }
void iommu_reserved(uint16_t a, uint16_t b, uint64_t c, uint64_t d, unsigned e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
}
unsigned iommu_reserved_count(void) { return 0; }
struct iommu_domain *iommu_domain_new(unsigned levels, unsigned leaf) { (void)levels; (void)leaf; return NULL; }
bool iommu_map(struct iommu_domain *dom, const uint64_t *frames, uint64_t base, size_t count) {
    (void)dom; (void)frames; (void)base; (void)count; return false;
}
void iommu_domain_free(struct iommu_domain *dom) { (void)dom; }
uint16_t *iommu_requester_ids(const struct pci_device *device, unsigned *count) {
    (void)device; *count = 0; return NULL;
}
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    (void)bus; (void)dev; (void)fn; (void)off; return UINT32_MAX;
}
const void *acpi_find(const char sig[4], unsigned index) {
    (void)sig; (void)index; return have_dmar ? dmar_fixture : NULL;
}
const struct acpi_ioapic *acpi_ioapics(unsigned *count) { *count = 0; return NULL; }
uint32_t lapic_current_id(void) { return 0; }
bool lapic_x2apic(void) { return false; }
int irq_vector_alloc(unsigned count, unsigned align) { (void)count; (void)align; return -1; }
void vector_register(int vector, irq_handler_t handler) { (void)vector; (void)handler; }

static void reset_model(void) {
    memset(registers, 0, sizeof registers);
    memset(own_queue, 0, PAGE_SIZE);
    memset(semaphore_page, 0, PAGE_SIZE);
    model_unit = (struct unit){.regs = (volatile uint8_t *)registers, .queue = (uintptr_t)own_queue,
        .queue_on = true, .ecap = ECAP_C | ECAP_QI | ECAP_PT | (16ull << 8)};
    units = &model_unit;
    nunits = units_room = 1;
    sems = (uintptr_t)semaphore_page;
    sem_pages = 1;
    token = 0;
    intel.coherent = true;
    intel.translating = false;
    intel.levels = 4;
    intel.phys_bits = 52; /* the VT-d second-stage address field in the test fixture */
    remap = x2 = irreq = caching_mode = write_buffers = false;
    run_queue = mapped_slot = true;
    run_commands = false;
    ticks = completed_waits = cache_flushes = 0;
    error_on_publish = 0;
    require_quiet_before_off = protection_dropped_early = false;
    cfi_behavior = CFI_WRITABLE;
    cfi_on_requested = cfi_clear_requested = remapping_bypass_started = false;
    cfi_pass_only_unit = NULL;
    early_unit_enable = ioapic_pin_remappable = compatibility_pin_blocked = false;
    eime_without_sirtp = false;
    ioapic_pin_masked = true;
    wr32(&model_unit, R_VER, 1u << 4); /* VER.MAJOR, legacy hardware with optional drains */
    wr32(&model_unit, R_GSTS, GCMD_QIE);
    wr64(&model_unit, R_IQA, model_unit.queue);
}

static void queue_failure_cases(void) {
    for (unsigned reason = 0; reason <= 15; reason++) { /* IQEI field, including unavailable/undefined values */
        reset_model();
        ((uint64_t *)own_queue)[0] = INV_IOTLB_GLOBAL;
        wr32(&model_unit, R_FSTS, FSTS_IQE);
        wr64(&model_unit, R_IQERCD, reason | (0x1234ull << 32) | (0x5678ull << 48));
        CHECK(!write_buffers_flush());
        CHECK(!submit(&model_unit, INV_CONTEXT_GLOBAL, 0));
        CHECK(!sync() && !sync());
        CHECK(model_unit.failed && ((uint64_t *)own_queue)[0] == INV_IOTLB_GLOBAL);
        CHECK(rd32(&model_unit, R_FSTS) == FSTS_IQE && rd64(&model_unit, R_IQT) == 0);
        CHECK(rd32(&model_unit, R_FECTL) & FECTL_IM);
    }
    const uint32_t errors[] = {FSTS_ICE, FSTS_ITE};
    for (unsigned i = 0; i < ARRAY_SIZE(errors); i++) {
        reset_model();
        wr32(&model_unit, R_FSTS, errors[i]);
        CHECK(!sync() && model_unit.failed);
    }
    reset_model();
    run_queue = false;
    CHECK(!sync() && model_unit.failed);
    uint32_t late_token = token;
    *(uint32_t *)semaphore_page = late_token; /* hardware completes after software stopped waiting */
    run_queue = true;
    CHECK(!sync() && *(uint32_t *)semaphore_page == late_token);
    CHECK(!write_buffers_flush()); /* CAP.RWBF=0 is not an escape from an uncertain queue */
    reset_model();
    run_queue = false;
    wr64(&model_unit, R_IQH, INV_BYTES);
    CHECK(!submit(&model_unit, INV_CONTEXT_GLOBAL, 0) && model_unit.failed);
    wr64(&model_unit, R_IQH, 0);
    CHECK(!submit(&model_unit, INV_CONTEXT_GLOBAL, 0));
    reset_model();
    wr32(&model_unit, R_GSTS, 0); /* hardware lost QIES while the software still owns its queue */
    CHECK(!sync() && model_unit.failed);
}

static void completion_cases(void) {
    reset_model();
    for (unsigned i = 0; i <= PAGE_SIZE / INV_BYTES; i++) {
        CHECK(submit(&model_unit, INV_CONTEXT_GLOBAL, 0));
        CHECK(sync());
    }
    CHECK(completed_waits == PAGE_SIZE / INV_BYTES + 1 && !model_unit.failed);
    model_unit.cap = CAP_RWBF;
    write_buffers = run_commands = true;
    wr32(&model_unit, R_GCMD, GCMD_QIE);
    CHECK(write_buffers_flush());
    CHECK(rd32(&model_unit, R_GCMD) & GCMD_WBF);
    CHECK(!(rd32(&model_unit, R_GSTS) & GCMD_WBF));
    reset_model();
    intel.coherent = false;
    CHECK(submit(&model_unit, INV_CONTEXT_GLOBAL, 0) && sync() && cache_flushes == 2);
    reset_model();
    CHECK(!command(&model_unit, GCMD_TE, true) && model_unit.command_failed);
    uint32_t submitted = rd32(&model_unit, R_GCMD);
    CHECK(!command(&model_unit, GCMD_IRE, true));
    CHECK(rd32(&model_unit, R_GCMD) == submitted);
    reset_model();
    model_unit.cap = CAP_ECMDS;
    wr64(&model_unit, R_ECRSP, 1);
    CHECK(!command(&model_unit, GCMD_TE, true));
    CHECK(rd32(&model_unit, R_GCMD) == 0);
    reset_model();
    wr32(&model_unit, R_GSTS, GCMD_WBF);
    CHECK(!command(&model_unit, GCMD_TE, true) && model_unit.command_failed);
    CHECK(rd32(&model_unit, R_GCMD) == 0);
    const unsigned invalidation_registers[] = {R_CCMD, ECAP_IRO(model_unit.ecap) + 8};
    for (unsigned i = 0; i < ARRAY_SIZE(invalidation_registers); i++) {
        reset_model();
        wr64(&model_unit, invalidation_registers[i], 1ull << 63); /* ICC or IVT still pending */
        CHECK(!command(&model_unit, GCMD_TE, false) && model_unit.command_failed);
        CHECK(rd32(&model_unit, R_GCMD) == 0);
    }
    reset_model();
    model_unit.cap = CAP_PLMR;
    wr32(&model_unit, R_PMEN, PMEN_EPM); /* enable request pending in firmware */
    CHECK(!command(&model_unit, GCMD_TE, true) && model_unit.command_failed);
    CHECK(rd32(&model_unit, R_GCMD) == 0);
    CHECK(intel_is_leaf(SL_R | SL_PS, 2) && !intel_is_leaf(SL_R, 2) && intel_is_leaf(SL_R, 1));
    reset_model();
    model_unit.queue_on = false;
    run_commands = true;
    wr32(&model_unit, R_GSTS, 0);
    wr64(&model_unit, R_CCMD, 1ull << 63);
    uint64_t original_iqa = rd64(&model_unit, R_IQA);
    CHECK(!queue_start(&model_unit) && model_unit.failed);
    CHECK(rd64(&model_unit, R_IQA) == original_iqa);
}

static void firmware_cases(void) {
    for (unsigned qs = 0; qs <= IQA_QS; qs++) {
        for (unsigned dw = 0; dw <= 1; dw++) {
            reset_model();
            unsigned bytes = PAGE_SIZE << qs, width = dw ? 32 : 16;
            memset(firmware_queue, 0xA5, bytes);
            model_unit.queue_on = false;
            run_commands = true;
            require_quiet_before_off = true;
            uint64_t iqa = (uintptr_t)firmware_queue | qs | (dw ? IQA_DW : 0);
            wr64(&model_unit, R_IQA, iqa);
            wr64(&model_unit, R_IQH, bytes - width);
            wr64(&model_unit, R_IQT, bytes - width);
            wr32(&model_unit, R_GSTS, GCMD_TE | GCMD_IRE | GCMD_QIE);
            wr32(&model_unit, R_GCMD, GCMD_TE | GCMD_IRE | GCMD_QIE);
            CHECK(intel_shutdown());
            CHECK(!protection_dropped_early);
            CHECK(!(rd32(&model_unit, R_GSTS) & (GCMD_TE | GCMD_IRE | GCMD_QIE)));
            CHECK(rd64(&model_unit, R_IQA) == iqa && model_unit.queue == (uintptr_t)own_queue);
            uint64_t *last = (uint64_t *)((uint8_t *)firmware_queue + bytes - width);
            CHECK(last[0] == INV_WAIT(token) && last[1] == sems && completed_waits == 1);
            if (dw) CHECK(last[2] == 0 && last[3] == 0);
            CHECK(*(uint8_t *)firmware_queue == 0xA5); /* not an old in-flight descriptor */
        }
    }
    reset_model();
    model_unit.queue_on = false;
    mapped_slot = false;
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
    CHECK(!queue_quiet(&model_unit));
    CHECK(rd64(&model_unit, R_IQT) == 0);
    reset_model();
    model_unit.queue_on = false;
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
    wr32(&model_unit, R_GSTS, GCMD_QIE | GCMD_TE);
    CHECK(queue_quiet(&model_unit)); /* a valid fence does not require disabling TE first */
    CHECK(rd32(&model_unit, R_GSTS) == (GCMD_QIE | GCMD_TE));
    reset_model();
    model_unit.queue_on = false;
    wr64(&model_unit, R_IQT, INV_BYTES / 2); /* width alignment violation */
    CHECK(!queue_quiet(&model_unit));
}

static void dma_lifetime_cases(void) {
    const uint64_t capabilities[] = {0, CAP_DRD, CAP_DWD, CAP_DRD | CAP_DWD};
    for (unsigned major = 1; major <= 2; major++) {
        for (unsigned c = 0; c < ARRAY_SIZE(capabilities); c++) {
            reset_model();
            wr32(&model_unit, R_VER, major << 4);
            model_unit.cap = capabilities[c];
            bool supported = major >= 2 || capabilities[c] == (CAP_DRD | CAP_DWD);
            uint16_t sid = 0x1234; /* arbitrary requester ID, not a hardware capacity */
            struct pci_device device = {0};
            struct iommu_domain dom = {.d = &device, .ids = &sid, .nids = 1, .id = 2,
                .root = iommu_alloc_pages(1), .levels = 4, .leaf_levels = 1};
            model_unit.root = iommu_alloc_pages(1);
            model_unit.pass = iommu_alloc_pages(1);
            CHECK(dom.root && model_unit.root && model_unit.pass);
            uint64_t *root_entry = (uint64_t *)phys_to_virt(model_unit.root) + (sid >> 8) * 2;
            *root_entry = model_unit.pass | CTX_P;
            volatile uint64_t *context = context_entry(&model_unit, sid, false);
            context[0] = CTX_P | CTX_PASS;
            context[1] = 0;
            uint32_t original_tail = model_unit.tail;
            CHECK(dma_draining() == supported);
            CHECK(intel_attach(&dom) == supported);
            CHECK(dom.written == supported);
            if (!supported) {
                CHECK(*root_entry == (model_unit.pass | CTX_P));
                CHECK(context[0] == (CTX_P | CTX_PASS) && model_unit.tail == original_tail);
            }
            CHECK(intel_flush(&dom, false)); /* adding a mapping needs no lifetime drain */
            CHECK(intel_flush(&dom, true) == supported);
            CHECK(intel_block(&dom) == supported);
            CHECK(context_entry(&model_unit, sid, false)[0] == 0);
            CHECK(!model_unit.failed); /* an absent capability is not a queue malfunction */
            if (major == 1) {
                bool saw_iotlb = false;
                uint64_t expected = (capabilities[c] & CAP_DRD ? INV_IOTLB_DR : 0) |
                                    (capabilities[c] & CAP_DWD ? INV_IOTLB_DW : 0);
                for (unsigned i = 0; i < model_unit.tail / INV_BYTES; i++) {
                    uint64_t lo = ((uint64_t *)own_queue)[i * 2];
                    if ((lo & 15) != 2) continue;
                    saw_iotlb = true;
                    CHECK((lo & (INV_IOTLB_DR | INV_IOTLB_DW)) == expected);
                }
                CHECK(saw_iotlb);
            }
        }
    }
    reset_model();
    uint64_t second_registers[PAGE_SIZE / sizeof(uint64_t)] = {0};
    struct unit pair[] = {model_unit, {.regs = (volatile uint8_t *)second_registers, .cap = CAP_DRD}};
    pair[0].cap = CAP_DRD | CAP_DWD;
    units = pair;
    nunits = ARRAY_SIZE(pair);
    CHECK(!dma_draining()); /* one capable unit does not establish an all-unit lifetime fence */
}

static void width_probe_case(void) {
    reset_model();
    memset(dmar_fixture, 0, sizeof dmar_fixture);
    ((struct acpi_sdt *)dmar_fixture)->length = sizeof dmar_fixture;
    dmar_fixture[36] = 47; /* HAW=48 */
    dmar_fixture[50] = 16; /* DRHD length */
    uint64_t address = (uintptr_t)registers;
    memcpy(dmar_fixture + 56, &address, sizeof address);
    wr64(&model_unit, R_CAP, (35ull << 16) | (4ull << 8)); /* MGAW=36, SAGAW=48 */
    wr64(&model_unit, R_ECAP, model_unit.ecap);
    units = NULL;
    nunits = units_room = 0;
    have_dmar = true;
    CHECK(iommu_intel_probe() == &intel);
    CHECK(intel.iova_bits == 36 && intel.phys_bits == 48 && intel.levels == 4);
    have_dmar = false;
    free(units);
    units = NULL;
}

static void startup_cases(void) {
    reset_model();
    sems = 0;
    model_unit.queue_on = false;
    run_commands = true;
    uint32_t on = GCMD_QIE | GCMD_TE | GCMD_IRE;
    wr32(&model_unit, R_GCMD, on);
    wr32(&model_unit, R_GSTS, on);
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
    CHECK(!intel_start(false)); /* intentionally unused, with no semaphore allocation */
    CHECK(!intel_shutdown() && model_unit.failed);
    CHECK(rd32(&model_unit, R_GSTS) == on && rd32(&model_unit, R_GCMD) == on);
    CHECK(rd64(&model_unit, R_IQT) == 0 && !completed_waits);

    reset_model();
    sems = 0;
    model_unit.queue_on = false;
    wr32(&model_unit, R_GSTS, 0);
    CHECK(!intel_start(false));
    CHECK(intel_shutdown() && !model_unit.failed); /* unused and confirmed off is distinguishable */

    reset_model();
    model_unit.queue_on = false;
    model_unit.root = (uintptr_t)own_queue;
    model_unit.cap = CAP_PLMR;
    run_commands = true;
    wr32(&model_unit, R_GSTS, 0);
    wr32(&model_unit, R_PMEN, PMEN_EPM | PMEN_PRS);
    CHECK(!units_on() && model_unit.command_failed); /* PRS stuck must not return success */

    reset_model();
    model_unit.queue_on = false;
    model_unit.ecap &= ~ECAP_PT; /* no identity domain exists on an IR-only start */
    remap = irreq = run_commands = true;
    intel.levels = 0;
    identity = NULL;
    wr32(&model_unit, R_GSTS, 0);
    CHECK(intel_start(false));
    CHECK(!intel.translating && !identity);
    CHECK(*(uint64_t *)phys_to_virt(model_unit.root) == 0);
    CHECK((rd32(&model_unit, R_GSTS) & (GCMD_IRE | GCMD_QIE | GCMD_TE)) == (GCMD_IRE | GCMD_QIE));
}

static void power_transition_cases(void) {
    reset_model();
    run_commands = true;
    wr32(&model_unit, R_GCMD, GCMD_QIE); /* fixture's last request agrees with initial QIES */
    model_unit.root = (uintptr_t)own_queue;
    CHECK(intel_shutdown());
    CHECK(!(rd32(&model_unit, R_GSTS) & GCMD_QIE) && !model_unit.queue_on);
    intel.translating = true;
    CHECK(intel_resume());
    CHECK((rd32(&model_unit, R_GSTS) & (GCMD_QIE | GCMD_TE)) == (GCMD_QIE | GCMD_TE));

    reset_model();
    model_unit.failed = true;
    wr32(&model_unit, R_GSTS, GCMD_QIE | GCMD_TE);
    CHECK(!intel_shutdown());
    CHECK(rd32(&model_unit, R_GSTS) == (GCMD_QIE | GCMD_TE)); /* uncertainty never disables isolation */

    const uint32_t errors[] = {FSTS_IQE, FSTS_ICE, FSTS_ITE};
    for (unsigned i = 0; i < ARRAY_SIZE(errors); i++) {
        reset_model();
        run_commands = true;
        uint32_t on = GCMD_QIE | GCMD_TE | GCMD_IRE;
        wr32(&model_unit, R_GCMD, on);
        wr32(&model_unit, R_GSTS, on);
        wr32(&model_unit, R_FSTS, errors[i]); /* no fault ISR observed this error yet */
        CHECK(!model_unit.failed && !intel_shutdown() && model_unit.failed);
        CHECK(rd32(&model_unit, R_GSTS) == on && rd32(&model_unit, R_GCMD) == on);
        CHECK(rd32(&model_unit, R_FSTS) == errors[i]);
        CHECK(!intel_resume() && rd32(&model_unit, R_GSTS) == on);
    }
    for (unsigned inherited = 0; inherited <= 1; inherited++) {
        reset_model();
        run_commands = true;
        uint32_t on = GCMD_QIE | GCMD_TE | GCMD_IRE;
        wr32(&model_unit, R_GCMD, on);
        wr32(&model_unit, R_GSTS, on);
        model_unit.queue_on = !inherited;
        if (inherited) wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
        error_on_publish = FSTS_IQE; /* the newly appended wait cannot be fetched */
        CHECK(!intel_shutdown() && model_unit.failed);
        CHECK(rd32(&model_unit, R_GSTS) == on && rd32(&model_unit, R_GCMD) == on);
        uint64_t original_tail = rd64(&model_unit, R_IQT), original_sems = sems;
        *(uint32_t *)semaphore_page = token; /* late write must not rescue an uncertain queue */
        CHECK(!intel_resume() && !intel_shutdown());
        CHECK(sems == original_sems && rd64(&model_unit, R_IQT) == original_tail);
    }

    reset_model();
    model_unit.queue_on = false;
    run_commands = true;
    run_queue = false; /* unknown old descriptor fetch never completes */
    uint32_t on = GCMD_QIE | GCMD_TE | GCMD_IRE;
    wr32(&model_unit, R_GCMD, on);
    wr32(&model_unit, R_GSTS, on);
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
    wr64(&model_unit, R_IQT, INV_BYTES);
    ((uint64_t *)firmware_queue)[0] = INV_CONTEXT_GLOBAL;
    CHECK(!intel_shutdown() && model_unit.failed);
    CHECK(rd32(&model_unit, R_GSTS) == on && rd32(&model_unit, R_GCMD) == on);
    CHECK(rd64(&model_unit, R_IQT) == INV_BYTES && completed_waits == 0);
    CHECK(((uint64_t *)firmware_queue)[0] == INV_CONTEXT_GLOBAL);

    reset_model();
    model_unit.queue_on = false;
    run_commands = true;
    mapped_slot = false;
    wr32(&model_unit, R_GCMD, on);
    wr32(&model_unit, R_GSTS, on);
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
    CHECK(!intel_shutdown() && model_unit.failed);
    CHECK(rd32(&model_unit, R_GSTS) == on && rd64(&model_unit, R_IQT) == 0);

    reset_model();
    wr32(&model_unit, R_GSTS, 0);
    uint64_t original_queue = model_unit.queue, original_sems = sems;
    run_commands = true;
    CHECK(intel_resume() && !model_unit.failed); /* a real sleep may reset QIES, unlike shutdown */
    CHECK(model_unit.queue == original_queue && sems == original_sems);
    CHECK(model_unit.queue_on && rd32(&model_unit, R_GSTS) & GCMD_QIE);

    reset_model();
    model_unit.queue_on = false;
    wr32(&model_unit, R_GSTS, 0);
    CHECK(!intel_resume() && model_unit.command_failed); /* SRTP itself did not complete */
    CHECK(!intel_shutdown()); /* even a zero status cannot prove a timed-out command completed */

    reset_model();
    run_commands = true;
    wr32(&model_unit, R_GCMD, GCMD_QIE);
    uint64_t second_registers[PAGE_SIZE / sizeof(uint64_t)] = {0};
    struct unit pair[] = {model_unit, {.regs = (volatile uint8_t *)second_registers, .failed = true}};
    units = pair;
    nunits = 2;
    wr32(&pair[0], R_GSTS, GCMD_QIE | GCMD_TE | GCMD_IRE);
    wr32(&pair[0], R_GCMD, GCMD_QIE | GCMD_TE | GCMD_IRE);
    CHECK(!intel_shutdown()); /* the first unit confirming is not an all-unit confirmation */
    CHECK(rd32(&pair[0], R_GSTS) == (GCMD_QIE | GCMD_TE | GCMD_IRE));
    pair[1].failed = false;
    wr32(&pair[1], R_FSTS, FSTS_IQE); /* unobserved fault in a later unit also preserves the first */
    CHECK(!intel_shutdown() && pair[1].failed);
    CHECK(rd32(&pair[0], R_GSTS) == (GCMD_QIE | GCMD_TE | GCMD_IRE));
    pair[1].failed = false;
    wr32(&pair[1], R_FSTS, 0);
    wr64(&pair[1], R_CCMD, 1ull << 63);
    CHECK(!intel_shutdown() && pair[1].command_failed);
    CHECK(rd32(&pair[0], R_GSTS) == (GCMD_QIE | GCMD_TE | GCMD_IRE));
}

static void compatibility_blocking_cases(void) {
    const struct { bool inherited, initially_on, read_only_zero, extended_mode; } cases[] = {
        {false, false, false, false}, /* own queue, CFI already disabled */
        {true, true, false, false},   /* firmware left CFI/IRE/TE/QIE enabled */
        {false, false, true, false},  /* hardware always blocks compatibility format */
        {false, false, false, true},  /* extended interrupt mode also rejects compatibility format */
    };
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        reset_model();
        remap = run_commands = true;
        x2 = cases[i].extended_mode;
        model_unit.root = (uintptr_t)own_queue;
        model_unit.queue_on = !cases[i].inherited;
        uint32_t on = GCMD_QIE | (cases[i].initially_on ? GCMD_TE | GCMD_IRE | GCMD_CFI : 0);
        wr32(&model_unit, R_GCMD, on);
        wr32(&model_unit, R_GSTS, on);
        if (cases[i].inherited) wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
        if (cases[i].read_only_zero) cfi_behavior = CFI_BLOCKED;
        CHECK(units_start(true));
        CHECK(intel.translating && !model_unit.command_failed);
        CHECK((rd32(&model_unit, R_GSTS) & (GCMD_IRE | GCMD_TE | GCMD_CFI)) == (GCMD_IRE | GCMD_TE));
        CHECK(!cfi_on_requested && !remapping_bypass_started && !early_unit_enable);
        CHECK(!compatibility_pin_blocked && ioapic_pin_masked && !ioapic_pin_remappable);
        /* The common caller, not this backend, publishes the pin after units_start(). */
        ioapic_pin_remappable = true;
        CHECK(intel_shutdown());
        /* S3 fixture: saved remappable IOAPIC entries remain masked until the caller restores them.
           This tests the backend contract, not the common caller's actual IOAPIC/PIC writes. */
        wr32(&model_unit, R_GCMD, 0);
        wr32(&model_unit, R_GSTS, 0);
        CHECK(intel_resume());
        CHECK((rd32(&model_unit, R_GSTS) & (GCMD_IRE | GCMD_TE | GCMD_CFI)) == (GCMD_IRE | GCMD_TE));
        CHECK(!cfi_on_requested && !remapping_bypass_started);
        CHECK(!compatibility_pin_blocked && ioapic_pin_masked && ioapic_pin_remappable);
        ioapic_pin_masked = false; /* caller restores the saved pin only after VT-d succeeds */
        model_step();
        CHECK(!compatibility_pin_blocked);
    }

    /* EIME blocks compatibility requests independently of CFIS (5.1.4). A deliberately
       fixed-one CFIS tests that this irrelevant prerequisite is not imposed; it is not
       evidence that a conforming implementation ignores CFI updates outside xAPIC mode. */
    for (unsigned translate = 0; translate <= 1; translate++) {
        reset_model();
        remap = x2 = run_commands = true;
        model_unit.queue_on = false;
        model_unit.root = (uintptr_t)own_queue;
        cfi_behavior = CFI_PASS_ONLY;
        wr32(&model_unit, R_GCMD, GCMD_CFI);
        wr32(&model_unit, R_GSTS, GCMD_CFI);
        CHECK(units_start(translate));
        CHECK(!model_unit.command_failed && intel.translating == !!translate);
        CHECK(rd64(&model_unit, R_IRTA) & IRTA_EIME);
        CHECK((rd32(&model_unit, R_GSTS) & (GCMD_IRE | GCMD_TE | GCMD_CFI)) ==
              (GCMD_IRE | GCMD_CFI | (translate ? GCMD_TE : 0)));
        CHECK(!cfi_clear_requested && !cfi_on_requested && !remapping_bypass_started &&
              !early_unit_enable && !eime_without_sirtp);
        ioapic_pin_masked = false; /* a plain pin is rejected, not passed through, even with CFIS=1 */
        model_step();
        CHECK(compatibility_pin_blocked);
        ioapic_pin_masked = true;
        CHECK(intel_shutdown());
        CHECK(intel_resume());
        CHECK(!model_unit.command_failed && !cfi_clear_requested && !remapping_bypass_started);
        CHECK((rd32(&model_unit, R_GSTS) & (GCMD_IRE | GCMD_CFI)) == (GCMD_IRE | GCMD_CFI));
        CHECK(!eime_without_sirtp);
        model_unit.command_failed = true; /* EIME must never erase an earlier uncertain command. */
        uint32_t last_command = rd32(&model_unit, R_GCMD);
        CHECK(!units_start(translate));
        CHECK(model_unit.command_failed && !intel.translating && rd32(&model_unit, R_GCMD) == last_command);
    }

    for (unsigned translate = 0; translate <= 1; translate++) {
        reset_model();
        remap = run_commands = true;
        model_unit.queue_on = false;
        model_unit.root = (uintptr_t)own_queue;
        cfi_behavior = CFI_PASS_ONLY;
        wr32(&model_unit, R_GCMD, GCMD_CFI);
        wr32(&model_unit, R_GSTS, GCMD_CFI);
        CHECK(!units_start(translate));
        CHECK(model_unit.command_failed && !intel.translating && !ioapic_pin_remappable);
        CHECK(!(rd32(&model_unit, R_GSTS) & (GCMD_IRE | GCMD_TE)));
        uint32_t last_command = rd32(&model_unit, R_GCMD);
        CHECK(!intel_resume() && !intel_shutdown());
        CHECK(model_unit.command_failed && rd32(&model_unit, R_GCMD) == last_command);
    }

    reset_model();
    remap = run_commands = true;
    model_unit.root = (uintptr_t)own_queue;
    uint64_t second_registers[PAGE_SIZE / sizeof(uint64_t)] = {0};
    struct unit pair[] = {model_unit, model_unit};
    pair[1].regs = (volatile uint8_t *)second_registers;
    pair[1].queue = iommu_alloc_pages(1);
    pair[1].queue_on = false;
    units = pair;
    nunits = ARRAY_SIZE(pair);
    wr32(&pair[0], R_GCMD, GCMD_QIE);
    wr32(&pair[1], R_GCMD, GCMD_CFI);
    wr32(&pair[1], R_GSTS, GCMD_CFI);
    cfi_pass_only_unit = &pair[1];
    CHECK(!units_start(true)); /* later-unit CFI failure must precede the first unit's enable */
    CHECK(pair[1].command_failed && !ioapic_pin_remappable && !early_unit_enable);
    CHECK(!(rd32(&pair[0], R_GSTS) & (GCMD_IRE | GCMD_TE)));

    reset_model();
    remap = run_commands = true;
    model_unit.root = (uintptr_t)own_queue;
    wr32(&model_unit, R_GCMD, GCMD_QIE);
    ioapic_pin_masked = false; /* counterexample: real init retains an unmasked plain ExtINT */
    CHECK(units_start(true));
    CHECK(compatibility_pin_blocked && !ioapic_pin_remappable);
    CHECK(!remapping_bypass_started); /* isolation is not evidence of lossless pin migration */
}

int main(void) {
    own_queue = _aligned_malloc(PAGE_SIZE, PAGE_SIZE);
    semaphore_page = _aligned_malloc(PAGE_SIZE, PAGE_SIZE);
    firmware_queue = _aligned_malloc(PAGE_SIZE << IQA_QS, PAGE_SIZE);
    if (!own_queue || !semaphore_page || !firmware_queue) return 2;
    queue_failure_cases();
    completion_cases();
    firmware_cases();
    dma_lifetime_cases();
    startup_cases();
    power_transition_cases();
    compatibility_blocking_cases();
    width_probe_case();
    for (size_t i = 0; i < allocation_count; i++) _aligned_free(allocations[i]);
    free(allocations);
    _aligned_free(own_queue);
    _aligned_free(semaphore_page);
    _aligned_free(firmware_queue);
    printf("Intel VT-d MMIO模擬検証: 失敗 %u（実機DMA・割込みは未検証）\n", failures);
    return failures != 0;
}
