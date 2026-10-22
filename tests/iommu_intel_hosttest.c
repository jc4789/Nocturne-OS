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
/* A second page exposes capabilities that point outside an advertised one-page set. */
_Alignas(2 * PAGE_SIZE) static uint64_t registers[2 * PAGE_SIZE / sizeof(uint64_t)];
static struct unit model_unit;
static void *own_queue, *firmware_queue, *semaphore_page;
static uint64_t ticks;
static bool run_queue, run_commands, mapped_slot;
static unsigned failures, completed_waits;
enum {
    DMAR_HEADER_BYTES = 48, DRHD_HEADER_BYTES = 16, RMRR_HEADER_BYTES = 24, DEVICE_SCOPE_BYTES = 8,
    DMAR_SINGLE_BYTES = DMAR_HEADER_BYTES + DRHD_HEADER_BYTES,
    DMAR_PAIR_BYTES = DMAR_HEADER_BYTES + 2 * DRHD_HEADER_BYTES
};
static uint8_t dmar_fixture[DMAR_SINGLE_BYTES + RMRR_HEADER_BYTES + DEVICE_SCOPE_BYTES];
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
static struct pmm_range model_ram[3]; /* own queue, status page, optional overlap counterexample */
static unsigned model_nram;
static bool ram_complete, fail_allocation;
static uint64_t unreadable_mmio;
static uint64_t remapped_mmio, last_mmio_bytes;
static unsigned mmio_maps;
static bool defer_status_write;
static uint32_t *late_status;
static uint32_t late_data;
static unsigned freed_pages;
static unsigned reserved_calls;
static uint64_t reserved_start, reserved_end;

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
            if (defer_status_write) {
                late_status = (uint32_t *)(uintptr_t)slot[1];
                late_data = (uint32_t)(slot[0] >> 32);
            } else {
                *(volatile uint32_t *)(uintptr_t)slot[1] = (uint32_t)(slot[0] >> 32);
                completed_waits++;
            }
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
    if (fail_allocation) return 0;
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
void pmm_free(uint64_t p) { (void)p; freed_pages++; } /* allocation lifetime is fixture-owned */
/* iommu.cの所有判定のモデル: 不完全なmapはOS所有を否定できない。 */
bool iommu_os_ram(uint64_t start, uint64_t end) {
    if (!ram_complete) return true;
    for (unsigned i = 0; i < model_nram; i++)
        if (start < model_ram[i].end && model_ram[i].base < end) return true;
    return false;
}
uint64_t vmm_translate(uint64_t root, uint64_t va) { (void)root; return mapped_slot ? va : 0; }
void *vmm_map_mmio(uint64_t pa, uint64_t size) {
    mmio_maps++;
    last_mmio_bytes = size;
    if (pa == unreadable_mmio) return NULL;
    return pa == remapped_mmio ? registers : (void *)(uintptr_t)pa;
}
void iommu_entry_write(volatile uint64_t *entry, uint64_t value) { *entry = value; }
void iommu_cover(uint16_t first, uint16_t last) { (void)first; (void)last; }
void iommu_kernel_id(uint16_t id) { (void)id; }
void iommu_reserved(uint16_t a, uint16_t b, uint64_t c, uint64_t d, unsigned e) {
    (void)a; (void)b; (void)e;
    reserved_calls++;
    reserved_start = c;
    reserved_end = d;
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
    (void)sig; return have_dmar && !index ? dmar_fixture : NULL;
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
    parse_lost = false;
    units_incomplete = false;
    intel.interrupt_remapping_required = false;
    ram_complete = true;
    fail_allocation = false;
    unreadable_mmio = 0;
    remapped_mmio = last_mmio_bytes = 0;
    mmio_maps = 0;
    defer_status_write = false;
    late_status = NULL;
    late_data = 0;
    freed_pages = 0;
    reserved_calls = 0;
    reserved_start = reserved_end = 0;
    model_nram = 2;
    model_ram[0] = (struct pmm_range){(uintptr_t)own_queue, (uintptr_t)own_queue + PAGE_SIZE};
    model_ram[1] = (struct pmm_range){(uintptr_t)semaphore_page, (uintptr_t)semaphore_page + PAGE_SIZE};
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

    reset_model();
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue); /* retained producer flag cannot authorize another IQA */
    CHECK(!sync() && model_unit.failed && rd64(&model_unit, R_IQT) == 0);

    reset_model();
    wr64(&model_unit, R_IQT, INV_BYTES); /* an untracked publication in our queue */
    CHECK(!queue_quiet(&model_unit) && model_unit.failed);
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
            uint64_t moved = rd64(&model_unit, R_IQA);
            CHECK(moved != iqa && (moved & (IQA_QS | IQA_DW)) == (iqa & (IQA_QS | IQA_DW)));
            CHECK(model_unit.queue == (uintptr_t)own_queue && !model_unit.handoff);
            CHECK(freed_pages == (1u << qs) + 1);
            uint64_t *last = (uint64_t *)((uintptr_t)(moved & ADDR) + bytes - width);
            CHECK(last[0] == INV_WAIT(token) && last[1] == (moved & ADDR) + bytes && completed_waits == 1);
            if (dw) CHECK(last[2] == 0 && last[3] == 0);
            bool original_unchanged = true;
            for (unsigned i = 0; i < bytes; i++)
                if (((uint8_t *)firmware_queue)[i] != 0xA5) original_unchanged = false;
            CHECK(original_unchanged); /* every old slot, including the formerly borrowed free slot */
        }
    }
    reset_model();
    model_unit.queue_on = false;
    ram_complete = false;
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

    /* A directly mapped address or PMM used bit is not a firmware ownership proof.
       Reject any overlap, including a queue spanning into just one allocator page. */
    for (unsigned qs = 0; qs <= IQA_QS; qs++) {
        reset_model();
        model_unit.queue_on = false;
        unsigned bytes = PAGE_SIZE << qs;
        memset(firmware_queue, 0xA5, bytes);
        model_ram[model_nram++] = (struct pmm_range){(uintptr_t)firmware_queue + bytes - PAGE_SIZE,
            (uintptr_t)firmware_queue + bytes};
        uint64_t iqa = (uintptr_t)firmware_queue | qs;
        wr64(&model_unit, R_IQA, iqa);
        size_t allocated = allocation_count;
        CHECK(!intel_start(true) && !intel_shutdown());
        CHECK(allocation_count == allocated && rd64(&model_unit, R_IQA) == iqa);
        CHECK(rd64(&model_unit, R_IQT) == 0 && *(uint8_t *)firmware_queue == 0xA5);
    }

    reset_model();
    model_unit.queue_on = false;
    run_commands = true;
    fail_allocation = true;
    wr32(&model_unit, R_GCMD, GCMD_QIE);
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
    CHECK(!intel_shutdown() && !model_unit.failed && !model_unit.handoff);
    CHECK(rd32(&model_unit, R_GSTS) == GCMD_QIE && rd64(&model_unit, R_IQT) == 0);
    fail_allocation = false; /* no publication/late write: an allocation failure can be retried */
    CHECK(intel_shutdown() && completed_waits == 1);

    reset_model();
    model_unit.queue_on = false;
    run_commands = true;
    mapped_slot = false;
    wr32(&model_unit, R_GCMD, GCMD_QIE);
    /* Unmapped old memory: neither product nor the empty-queue model may dereference it. */
    wr64(&model_unit, R_IQA, (1ull << intel.phys_bits) - PAGE_SIZE);
    CHECK(intel_shutdown() && completed_waits == 1 && !model_unit.failed);

    reset_model();
    model_unit.queue_on = false;
    run_commands = true;
    wr32(&model_unit, R_GSTS, GCMD_TE); /* a register-mode firmware unit, with QIES already off */
    wr32(&model_unit, R_GCMD, GCMD_TE);
    sems = 0;
    size_t allocated = allocation_count;
    CHECK(intel_shutdown() && !model_unit.failed && !model_unit.command_failed);
    CHECK(!(rd32(&model_unit, R_GSTS) & GCMD_TE) && allocation_count == allocated && !completed_waits);

    reset_model();
    ram_complete = false;
    model_unit.queue_on = false; /* S3 retained precisely our allocation, not a firmware RAM guess */
    run_commands = true;
    wr32(&model_unit, R_GCMD, GCMD_QIE);
    CHECK(intel_shutdown() && completed_waits == 1);

    reset_model();
    model_unit.queue_on = false;
    run_commands = defer_status_write = true;
    uint32_t on = GCMD_QIE | GCMD_TE | GCMD_IRE;
    wr32(&model_unit, R_GSTS, on);
    wr32(&model_unit, R_GCMD, on);
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
    CHECK(!intel_shutdown() && model_unit.failed && model_unit.handoff);
    CHECK(rd64(&model_unit, R_IQH) == rd64(&model_unit, R_IQT)); /* empty is not fenced completion */
    CHECK(rd32(&model_unit, R_GSTS) == on && late_status && !freed_pages);
    uint64_t retained = model_unit.handoff;
    if (late_status) *late_status = late_data;
    CHECK(!intel_shutdown() && !intel_resume());
    CHECK(model_unit.handoff == retained && !freed_pages); /* a late write cannot reopen the lifetime */

    reset_model();
    uint64_t other_registers[ARRAY_SIZE(registers)] = {0};
    struct unit pair[] = {model_unit, {.regs = (volatile uint8_t *)other_registers}};
    units = pair;
    nunits = ARRAY_SIZE(pair);
    wr32(&pair[1], R_GSTS, GCMD_QIE | GCMD_TE);
    wr64(&pair[1], R_IQA, (uintptr_t)firmware_queue);
    model_ram[model_nram++] = (struct pmm_range){(uintptr_t)firmware_queue, (uintptr_t)firmware_queue + PAGE_SIZE};
    allocated = allocation_count;
    CHECK(!intel_start(true) && !intel_shutdown());
    CHECK(allocation_count == allocated && !completed_waits); /* preflight ALL units before even the first allocation */
    CHECK(rd32(&pair[0], R_GSTS) == GCMD_QIE && rd32(&pair[1], R_GSTS) == (GCMD_QIE | GCMD_TE));
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
    ((struct acpi_sdt *)dmar_fixture)->length = DMAR_SINGLE_BYTES;
    dmar_fixture[36] = 47; /* HAW=48 */
    dmar_fixture[50] = 16; /* DRHD length */
    uint64_t address = (uintptr_t)registers;
    memcpy(dmar_fixture + 56, &address, sizeof address);
    wr64(&model_unit, R_CAP, (35ull << 16) | (4ull << 8)); /* MGAW=36, SAGAW=48 */
    wr64(&model_unit, R_ECAP, model_unit.ecap);
    units = NULL;
    nunits = units_room = 0;
    have_dmar = true;
    bool plain_safe;
    CHECK(iommu_intel_probe(&plain_safe) == &intel && !plain_safe);
    CHECK(intel.iova_bits == 36 && intel.phys_bits == 48 && intel.levels == 4);
    have_dmar = false;
    free(units);
    units = NULL;
}

static void probe_plain_cases(void) {
    reset_model();
    have_dmar = false;
    bool plain_safe = false;
    CHECK(!iommu_intel_probe(&plain_safe) && plain_safe);
    const uint32_t busy_states[] = {0, GCMD_TE, GCMD_IRE, GCMD_QIE, GCMD_WBF};
    for (unsigned required = 0; required <= 1; required++) {
        for (unsigned i = 0; i < ARRAY_SIZE(busy_states); i++) {
            reset_model();
            memset(dmar_fixture, 0, sizeof dmar_fixture);
            ((struct acpi_sdt *)dmar_fixture)->length = DMAR_SINGLE_BYTES;
            dmar_fixture[36] = 47;
            dmar_fixture[50] = 16;
            uint64_t address = (uintptr_t)registers;
            memcpy(dmar_fixture + 56, &address, sizeof address);
            wr64(&model_unit, R_CAP, (47ull << 16) | (4ull << 8));
            wr64(&model_unit, R_ECAP, ECAP_C | (16ull << 8) | (required ? ECAP_IRREQ : 0)); /* no QI: rejected */
            wr32(&model_unit, R_GSTS, busy_states[i]);
            uint64_t before[ARRAY_SIZE(registers)];
            memcpy(before, registers, sizeof before);
            units = NULL;
            nunits = units_room = 0;
            have_dmar = true;
            plain_safe = true;
            CHECK(!iommu_intel_probe(&plain_safe));
            CHECK(plain_safe == (!required && !busy_states[i]));
            CHECK(!memcmp(before, registers, sizeof before)); /* no reject shutdown/masking/ack writes */
            free(units);
            units = NULL;
            have_dmar = false;
        }
    }
    const enum { SHORT_TABLE, SHORT_DRHD, ZERO_BASE, FOREIGN_SEGMENT, UNREADABLE, TRUNCATED_TAIL,
                INVALID_REGISTER, PENDING_REGISTER, PROTECTED_RANGE, ACTIVE_ENHANCED, BROKEN_SCOPE,
                UNKNOWN_STRUCTURE } cases[] = {
        SHORT_TABLE, SHORT_DRHD, ZERO_BASE, FOREIGN_SEGMENT, UNREADABLE, TRUNCATED_TAIL,
        INVALID_REGISTER, PENDING_REGISTER, PROTECTED_RANGE, ACTIVE_ENHANCED, BROKEN_SCOPE,
        UNKNOWN_STRUCTURE
    };
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        reset_model();
        memset(dmar_fixture, 0, sizeof dmar_fixture);
        ((struct acpi_sdt *)dmar_fixture)->length = DMAR_SINGLE_BYTES;
        dmar_fixture[36] = 47;
        dmar_fixture[50] = 16;
        uint64_t address = (uintptr_t)registers;
        memcpy(dmar_fixture + 56, &address, sizeof address);
        wr64(&model_unit, R_CAP, (47ull << 16) | (4ull << 8));
        wr64(&model_unit, R_ECAP, ECAP_C | (16ull << 8));
        wr32(&model_unit, R_GSTS, 0);
        switch (cases[i]) {
        case SHORT_TABLE: ((struct acpi_sdt *)dmar_fixture)->length = 47; break;
        case SHORT_DRHD: dmar_fixture[50] = 8; break;
        case ZERO_BASE: memset(dmar_fixture + 56, 0, 8); break;
        case FOREIGN_SEGMENT: dmar_fixture[54] = 1; break;
        case UNREADABLE: unreadable_mmio = address; break;
        case TRUNCATED_TAIL: ((struct acpi_sdt *)dmar_fixture)->length--; dmar_fixture[50]--; break;
        case INVALID_REGISTER: wr32(&model_unit, R_VER, UINT32_MAX); break;
        case PENDING_REGISTER: wr64(&model_unit, R_CCMD, 1ull << 63); break;
        case PROTECTED_RANGE:
            wr64(&model_unit, R_CAP, rd64(&model_unit, R_CAP) | CAP_PLMR);
            wr32(&model_unit, R_PMEN, PMEN_EPM | PMEN_PRS);
            break;
        case ACTIVE_ENHANCED:
            wr64(&model_unit, R_CAP, rd64(&model_unit, R_CAP) | CAP_ECMDS);
            wr64(&model_unit, R_ECRSP, 1);
            break;
        case BROKEN_SCOPE:
            ((struct acpi_sdt *)dmar_fixture)->length = DMAR_SINGLE_BYTES + 7;
            dmar_fixture[50] = DRHD_HEADER_BYTES + 7;
            dmar_fixture[DMAR_SINGLE_BYTES] = 1;
            dmar_fixture[DMAR_SINGLE_BYTES + 1] = 7; /* fixed header + truncated two-byte Path */
            break;
        case UNKNOWN_STRUCTURE:
            ((struct acpi_sdt *)dmar_fixture)->length = DMAR_PAIR_BYTES;
            dmar_fixture[DMAR_SINGLE_BYTES] = 7; /* 8.2: future structure, not an invented unit description */
            dmar_fixture[DMAR_SINGLE_BYTES + 2] = DRHD_HEADER_BYTES;
            break;
        }
        uint64_t before[ARRAY_SIZE(registers)];
        memcpy(before, registers, sizeof before);
        units = NULL;
        nunits = units_room = 0;
        have_dmar = true;
        plain_safe = true;
        CHECK(!iommu_intel_probe(&plain_safe));
        CHECK(plain_safe == (cases[i] == FOREIGN_SEGMENT || cases[i] == BROKEN_SCOPE));
        CHECK(!memcmp(before, registers, sizeof before));
        free(units);
        units = NULL;
        have_dmar = false;
    }

    for (unsigned mode = 0; mode < 4; mode++) { /* all off, later TE, later IRREQ, unreadable later unit */
        reset_model();
        _Alignas(PAGE_SIZE) uint64_t second_registers[ARRAY_SIZE(registers)] = {0};
        struct unit second = {.regs = (volatile uint8_t *)second_registers};
        memset(dmar_fixture, 0, sizeof dmar_fixture);
        ((struct acpi_sdt *)dmar_fixture)->length = DMAR_PAIR_BYTES;
        dmar_fixture[36] = 47;
        uint64_t addresses[] = {(uintptr_t)registers, (uintptr_t)second_registers};
        for (unsigned j = 0; j < ARRAY_SIZE(addresses); j++) {
            size_t at = DMAR_HEADER_BYTES + j * DRHD_HEADER_BYTES;
            dmar_fixture[at + 2] = DRHD_HEADER_BYTES;
            memcpy(dmar_fixture + at + 8, &addresses[j], sizeof addresses[j]);
        }
        wr64(&model_unit, R_CAP, (47ull << 16) | (4ull << 8));
        wr64(&model_unit, R_ECAP, ECAP_C | (16ull << 8));
        wr32(&model_unit, R_GSTS, 0);
        wr32(&second, R_VER, 1u << 4);
        wr64(&second, R_CAP, (47ull << 16) | (4ull << 8));
        wr64(&second, R_ECAP, ECAP_C | (16ull << 8) | (mode == 2 ? ECAP_IRREQ : 0));
        if (mode == 1) wr32(&second, R_GSTS, GCMD_TE);
        if (mode == 3) unreadable_mmio = addresses[1];
        uint64_t before[ARRAY_SIZE(registers)], second_before[ARRAY_SIZE(second_registers)];
        memcpy(before, registers, sizeof before);
        memcpy(second_before, second_registers, sizeof second_before);
        units = NULL;
        nunits = units_room = 0;
        have_dmar = true;
        plain_safe = true;
        CHECK(!iommu_intel_probe(&plain_safe) && plain_safe == (mode == 0));
        CHECK(!memcmp(before, registers, sizeof before) && !memcmp(second_before, second_registers, sizeof second_before));
        free(units);
        units = NULL;
        have_dmar = false;
    }
}

static void probe_register_bounds_cases(void) {
    enum {
        BASE_ALIAS, SIZE_ALIGNMENT, RESERVED_SIZE_16, RESERVED_SIZE_31, RESERVED_SIZE_255,
        ADDRESS_WRAP, FRO_OUTSIDE, IRO_OUTSIDE, ALIGNED_OFF, FRO_END, IRO_END, WIDE_REGION, MAX_REGION
    };
    unsigned before_cases = failures;
    for (unsigned scenario = BASE_ALIAS; scenario <= MAX_REGION; scenario++) {
        reset_model();
        memset(dmar_fixture, 0, sizeof dmar_fixture);
        ((struct acpi_sdt *)dmar_fixture)->length = DMAR_SINGLE_BYTES;
        dmar_fixture[36] = 47;
        dmar_fixture[50] = DRHD_HEADER_BYTES;
        wr64(&model_unit, R_CAP, (47ull << 16) | (4ull << 8) | CAP_RWBF);
        wr64(&model_unit, R_ECAP, ECAP_C | (16ull << 8)); /* no QI: independently observed off */
        wr32(&model_unit, R_GSTS, 0);
        uint64_t address = (uintptr_t)registers;
        unsigned size = 0;
        uint64_t observed_bytes = PAGE_SIZE;
        unsigned expected_maps = 1;
        bool expected_plain = scenario >= ALIGNED_OFF;
        switch (scenario) {
        case BASE_ALIAS:
            /* A legitimate IR-only unit has IRTPS but no RTPS. An eight-byte DRHD
               error makes CAP look like VER, GCMD/GSTS like ECAP, and RTADDR like GSTS. */
            wr64(&model_unit, R_ECAP, ECAP_C | ECAP_QI | ECAP_IR | (16ull << 8));
            wr32(&model_unit, R_GCMD, GCMD_IRE);
            wr32(&model_unit, R_GSTS, GCMD_IRE | GCMD_SIRTP);
            address += sizeof(uint64_t);
            expected_maps = 0;
            break;
        case SIZE_ALIGNMENT:
            memcpy((uint8_t *)registers + PAGE_SIZE, registers, PAGE_SIZE);
            address += PAGE_SIZE; /* page-aligned, but not aligned to the reported two pages */
            size = 1;
            expected_maps = 0;
            break;
        case RESERVED_SIZE_16: size = 16; expected_maps = 0; break;
        case RESERVED_SIZE_31: size = 31; expected_maps = 0; break;
        case RESERVED_SIZE_255: size = UINT8_MAX; expected_maps = 0; break;
        case ADDRESS_WRAP:
            address = UINT64_MAX - (uint64_t)PAGE_SIZE + 1;
            unreadable_mmio = address; /* old code may attempt a map, never dereference a host-invalid pointer */
            expected_maps = 0;
            break;
        case FRO_OUTSIDE:
            wr64(&model_unit, R_CAP, rd64(&model_unit, R_CAP) | ((uint64_t)(PAGE_SIZE / 16) << 24));
            break;
        case IRO_OUTSIDE:
            wr64(&model_unit, R_ECAP, ECAP_C | ((uint64_t)(PAGE_SIZE / 16) << 8));
            break;
        case FRO_END:
            wr64(&model_unit, R_CAP, rd64(&model_unit, R_CAP) | ((uint64_t)(PAGE_SIZE / 16 - 1) << 24));
            break;
        case IRO_END:
            wr64(&model_unit, R_ECAP, ECAP_C | ((uint64_t)(PAGE_SIZE / 16 - 1) << 8));
            break;
        case WIDE_REGION:
            size = 1;
            observed_bytes = (uint64_t)PAGE_SIZE << size;
            wr64(&model_unit, R_CAP, rd64(&model_unit, R_CAP) | ((uint64_t)(PAGE_SIZE / 16) << 24));
            wr64(&model_unit, R_ECAP, ECAP_C | ((uint64_t)(PAGE_SIZE / 16) << 8));
            break;
        case MAX_REGION:
            size = 15; /* all four Size bits: defined 128MiB register set, no artificial cutoff */
            observed_bytes = (uint64_t)PAGE_SIZE << size;
            remapped_mmio = address = observed_bytes; /* logical PA; the model does not allocate 128MiB */
            break;
        }
        dmar_fixture[53] = (uint8_t)size;
        memcpy(dmar_fixture + 56, &address, sizeof address);
        uint64_t before[ARRAY_SIZE(registers)];
        memcpy(before, registers, sizeof before);
        units = NULL;
        nunits = units_room = 0;
        have_dmar = true;
        bool plain_safe = true;
        unsigned failures_before = failures;
        CHECK(!iommu_intel_probe(&plain_safe));
        CHECK(plain_safe == expected_plain);
        CHECK(mmio_maps == expected_maps);
        if (expected_maps) CHECK(last_mmio_bytes == observed_bytes);
        CHECK(!memcmp(before, registers, sizeof before));
        if (scenario == BASE_ALIAS) CHECK(rd32(&model_unit, R_GSTS) & GCMD_IRE);
        if (failures != failures_before)
            printf("DRHD境界反例 %u: size=%u map=%u bytes=%llu plain=%u\n", scenario, size, mmio_maps,
                   (unsigned long long)last_mmio_bytes, plain_safe);
        free(units);
        units = NULL;
        have_dmar = false;
    }
    printf("DRHD報告範囲%u条件（未整列・予約bit・wrap・FRO/IRO・整列対照・最大N15）: 追加失敗%u\n",
           MAX_REGION + 1, failures - before_cases);
}

static void probe_rmrr_bounds_cases(void) {
    enum {
        SHORT_RECORD, SHORT_FIXED, REVERSE_RANGE, INCREMENT_WRAP, ROUND_WRAP,
        NO_SCOPE, EQUAL_RANGE, MISALIGNED_BASE, MISALIGNED_LIMIT, LAST_PHYS_PAGE, PHYS_RANGE, NORMAL_RANGE
    };
    unsigned before_cases = failures;
    for (unsigned scenario = SHORT_RECORD; scenario <= NORMAL_RANGE; scenario++) {
        for (unsigned active = 0; active <= 1; active++) {
            reset_model();
            memset(dmar_fixture, 0, sizeof dmar_fixture);
            dmar_fixture[36] = 51; /* HAW=52; a normal range may end at the last encodable physical page */
            dmar_fixture[50] = DRHD_HEADER_BYTES;
            uint64_t address = (uintptr_t)registers;
            memcpy(dmar_fixture + 56, &address, sizeof address);
            wr64(&model_unit, R_CAP, (51ull << 16) | (8ull << 8)); /* MGAW=52, five levels */
            wr64(&model_unit, R_ECAP, model_unit.ecap);
            wr32(&model_unit, R_GSTS, active ? GCMD_TE | GCMD_SRTP : 0);
            uint64_t base = PAGE_SIZE, limit = 2 * PAGE_SIZE - 1;
            unsigned length = RMRR_HEADER_BYTES + DEVICE_SCOPE_BYTES;
            switch (scenario) {
            case SHORT_RECORD: length = 4; break; /* just the common type/length header */
            case SHORT_FIXED: length = RMRR_HEADER_BYTES - 1; break;
            case REVERSE_RANGE: base = 2 * PAGE_SIZE; limit = PAGE_SIZE - 1; break;
            case INCREMENT_WRAP: limit = UINT64_MAX; break;
            case ROUND_WRAP: limit = UINT64_MAX - PAGE_SIZE / 2; break;
            case NO_SCOPE: length = RMRR_HEADER_BYTES; break;
            case EQUAL_RANGE: limit = base; break;
            case MISALIGNED_BASE: base++; break;
            case MISALIGNED_LIMIT: limit--; break;
            case LAST_PHYS_PAGE: base = (1ull << 52) - PAGE_SIZE; limit = (1ull << 52) - 1; break;
            case PHYS_RANGE: limit = (1ull << 52) - 1; break;
            }
            uint8_t *rmrr = dmar_fixture + DMAR_SINGLE_BYTES;
            rmrr[0] = 1;
            rmrr[2] = (uint8_t)length;
            memcpy(rmrr + 8, &base, sizeof base);
            memcpy(rmrr + 16, &limit, sizeof limit);
            rmrr[RMRR_HEADER_BYTES] = 1; /* endpoint, one PCI path component */
            rmrr[RMRR_HEADER_BYTES + 1] = DEVICE_SCOPE_BYTES;
            ((struct acpi_sdt *)dmar_fixture)->length = DMAR_SINGLE_BYTES + length;
            uint64_t before[ARRAY_SIZE(registers)];
            memcpy(before, registers, sizeof before);
            units = NULL;
            nunits = units_room = 0;
            have_dmar = true;
            bool invalid = scenario <= MISALIGNED_LIMIT, plain_safe = false;
            unsigned failures_before = failures;
            const struct iommu_hw *found = iommu_intel_probe(&plain_safe);
            CHECK(found == (invalid ? NULL : &intel));
            CHECK(parse_lost == invalid && !units_incomplete);
            CHECK(plain_safe == !active); /* malformed reservation is not an unknown DRHD */
            CHECK(reserved_calls == (unsigned)!invalid);
            if (!invalid) CHECK(reserved_start == base && reserved_end == limit + 1);
            CHECK(!memcmp(before, registers, sizeof before));
            if (failures != failures_before)
                printf("RMRR境界反例 %u: active=%u parse_lost=%u incomplete=%u plain=%u reserved=%u\n",
                       scenario, active, parse_lost, units_incomplete, plain_safe, reserved_calls);
            free(units);
            units = NULL;
            have_dmar = false;
        }
    }
    printf("RMRR境界%u条件×停止/稼働（短縮・不正範囲・加算/丸めwrap・52bit上端対照）: 追加失敗%u\n",
           NORMAL_RANGE + 1, failures - before_cases);
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
    CHECK(intel_shutdown() && !model_unit.failed); /* migration has its own retained status page */
    CHECK(!(rd32(&model_unit, R_GSTS) & on) && completed_waits == 1);

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
    CHECK(!intel_shutdown() && !model_unit.failed); /* no OS wait was published; only old fetches are pending */
    CHECK(rd32(&model_unit, R_GSTS) == on && rd32(&model_unit, R_GCMD) == on);
    CHECK(rd64(&model_unit, R_IQT) == INV_BYTES && completed_waits == 0);
    CHECK(((uint64_t *)firmware_queue)[0] == INV_CONTEXT_GLOBAL);

    reset_model();
    model_unit.queue_on = false;
    run_commands = true;
    ram_complete = false;
    wr32(&model_unit, R_GCMD, on);
    wr32(&model_unit, R_GSTS, on);
    wr64(&model_unit, R_IQA, (uintptr_t)firmware_queue);
    CHECK(!intel_shutdown() && !model_unit.failed);
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
    probe_plain_cases();
    probe_register_bounds_cases();
    probe_rmrr_bounds_cases();
    for (size_t i = 0; i < allocation_count; i++) _aligned_free(allocations[i]);
    free(allocations);
    _aligned_free(own_queue);
    _aligned_free(semaphore_page);
    _aligned_free(firmware_queue);
    printf("Intel VT-d MMIO模擬検証: 失敗 %u（実機DMA・割込みは未検証）\n", failures);
    return failures != 0;
}
