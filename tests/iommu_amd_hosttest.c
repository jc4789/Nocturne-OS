/* AMD 48882 Rev 3.10 に基づくMMIOモデル。実機・QEMUの受入を代用しない。
   製品実装そのものを含め、MMIO/PCI/メモリー所有境界だけを置換する。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "kernel.h"
#include "arch/cpu.h"

struct unit;
static uint64_t rd(const struct unit *, unsigned);
static void wr(const struct unit *, unsigned, uint64_t);
static uint32_t rd32(const struct unit *, unsigned);
static void wr32(const struct unit *, unsigned, uint32_t);
static uint64_t host_irq_save(void) { return 0; }
static void host_irq_restore(uint64_t f) { (void)f; }
#define irq_save host_irq_save
#define irq_restore host_irq_restore
#define IOMMU_AMD_HOSTTEST
#include "../kernel/src/dev/iommu_amd.c"

uint64_t hhdm_offset;
static unsigned checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "失敗 %d: %s\n", __LINE__, #c); } } while (0)

/* バッファの寸法は製品側で定義されたMMIO窓・DeviceID・MSI-Xフィールドに対応する。 */
static uint64_t registers[MMIO_BYTES / sizeof(uint64_t)];
static uint64_t second_registers[MMIO_BYTES / sizeof(uint64_t)];
static const uint64_t first_base = 0xFED90000;
static bool plain_safe;
static uint64_t map_failed_base;
static unsigned register_writes, virtual_status_reads;
static uint64_t control_readonly;
static bool control_locked, exclusion_locked, perf_locked;
static volatile uint8_t *locked_control_regs;
static unsigned freed_pages;
static unsigned perf_reads, perf_writes, table_writes, unsafe_pointer_writes, command_restarts;
static unsigned restart_stage, bad_event_restart;
static bool restarting_events;
static const struct acpi_sdt *firmware;
static struct pci_device function;
static uint32_t cap_header, cap_info;
static struct pci_device second_function;
static bool second_function_present;
static uint32_t second_cap_info;
static unsigned msi_calls, msix_inits, msix_routes, msix_size;
static bool msix_unmasked[1u << 11];
static bool covered_ids[1u << 16], platform_ids[1u << 16];
static struct { uint16_t first, last; uint64_t start, end; unsigned access; } reservation;
static unsigned reservations;
static struct iommu_domain *claimed_domain;
static uint64_t deadline_count;
static bool defer_completion, deliver_late_completion;
static uint64_t pending_address, pending_data, late_address, late_data;

static uint64_t rd(const struct unit *u, unsigned off) {
    if (off == 0x190) virtual_status_reads++;
    uint64_t value = ((const uint64_t *)u->regs)[off / sizeof(uint64_t)];
    return value | (off == R_CONTROL ? control_readonly : 0);
}

static void wr(const struct unit *u, unsigned off, uint64_t value) {
    register_writes++;
    uint64_t *r = (uint64_t *)u->regs;
    uint64_t old = r[off / sizeof(uint64_t)];
    if (off == R_CONTROL) {
        if (control_locked && (!locked_control_regs || u->regs == locked_control_regs)) return;
        uint64_t *status = r + R_STATUS / sizeof(uint64_t);
        if ((value & CTL_CMDBUF_EN) && !(*status & ST_CMDBUF_RUN)) command_restarts++;
        if (!(value & CTL_IOMMU_EN) || !(value & CTL_CMDBUF_EN)) *status &= ~ST_CMDBUF_RUN;
        else *status |= ST_CMDBUF_RUN;
        if (!(value & CTL_IOMMU_EN) || !(value & CTL_EVENTLOG_EN)) *status &= ~ST_EVENTLOG_RUN;
        else if (!(old & CTL_EVENTLOG_EN)) {
            if (restarting_events && restart_stage != 4) bad_event_restart++;
            *status = (*status & ~ST_EVENT_OVERFLOW) | ST_EVENTLOG_RUN;
            restarting_events = false;
        }
        if (restarting_events && !(value & CTL_EVENTLOG_EN)) restart_stage = 1;
    } else if (off == R_STATUS) {
        if (restarting_events && (value & ST_EVENT_OVERFLOW)) {
            if (restart_stage != 3) bad_event_restart++;
            restart_stage = 4;
        }
        r[off / sizeof(uint64_t)] &= ~(value & (ST_EVENT_OVERFLOW | ST_EVENT_INT | ST_COMWAIT_INT));
        return;
    } else if (off == R_EXCL_BASE && exclusion_locked) {
        return;
    } else if (off == R_EVT_HEAD || off == R_EVT_TAIL) {
        if (off == R_EVT_TAIL && (rd(u, R_STATUS) & ST_EVENTLOG_RUN)) unsafe_pointer_writes++;
        if (restarting_events && restart_stage) {
            if (off == R_EVT_HEAD && restart_stage == 1) restart_stage = 2;
            else if (off == R_EVT_TAIL && restart_stage == 2) restart_stage = 3;
            else bad_event_restart++;
        }
    } else if (off == R_CMD_HEAD) {
        if (rd(u, R_STATUS) & ST_CMDBUF_RUN) unsafe_pointer_writes++;
    } else if (off == R_DEVTAB) {
        table_writes++;
    }
    r[off / sizeof(uint64_t)] = value;
    /* COMPLETION_WAITのcoherent storeのみモデル化する。その他command実行は検査側で読む。 */
    if (off == R_CMD_TAIL && (rd(u, R_STATUS) & ST_CMDBUF_RUN)) {
        uint32_t *c = (uint32_t *)phys_to_virt(u->cmd) + ((value + RING_BYTES - 16) % RING_BYTES) / 4;
        if ((c[1] >> 28) == CMD_COMPLETION_WAIT && (c[0] & 1)) {
            uint64_t address = (c[0] & ~7u) | (uint64_t)(c[1] & 0xFFFFF) << 32;
            uint64_t data = c[2] | (uint64_t)c[3] << 32;
            if (defer_completion) { pending_address = address; pending_data = data; }
            else *(uint64_t *)phys_to_virt(address) = data;
        }
        r[R_CMD_HEAD / sizeof(uint64_t)] = value;
    }
}

static uint32_t rd32(const struct unit *u, unsigned off) {
    CHECK(off == R_PERF_OPT);
    perf_reads++;
    return *(const uint32_t *)(u->regs + off);
}

static void wr32(const struct unit *u, unsigned off, uint32_t value) {
    register_writes++;
    CHECK(off == R_PERF_OPT);
    perf_writes++;
    if (!perf_locked) *(uint32_t *)(u->regs + off) = value;
}

int kprintf(const char *format, ...) { (void)format; return 0; }
void *kappend(void *array_at, size_t *count, size_t *room, size_t size) {
    if (*count == *room) {
        size_t n = *room ? *room * 2 : 1;
        void *p = realloc(*(void **)array_at, n * size);
        if (!p) return NULL;
        *(void **)array_at = p;
        *room = n;
    }
    return (char *)*(void **)array_at + (*count)++ * size;
}
const void *acpi_find(const char sig[4], unsigned index) { (void)sig; (void)index; return firmware; }
struct pci_device *pci_find_function(uint8_t b, uint8_t d, uint8_t f) {
    if (b == function.bus && d == function.dev && f == function.func) return &function;
    if (second_function_present && b == second_function.bus && d == second_function.dev && f == second_function.func)
        return &second_function;
    return NULL;
}
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint16_t off) {
    if (off == 0x40) return cap_header;
    if (second_function_present && b == second_function.bus && d == second_function.dev && f == second_function.func)
        return second_cap_info;
    return cap_info;
}
void *vmm_map_mmio(uint64_t base, uint64_t bytes) {
    CHECK(bytes == MMIO_BYTES);
    if (base == map_failed_base) return NULL;
    return base == first_base + MMIO_BYTES ? second_registers : registers;
}
void iommu_cover(uint16_t first, uint16_t last) { for (uint32_t i = first; i <= last; i++) covered_ids[i] = true; }
void iommu_kernel_id(uint16_t id) { platform_ids[id] = true; }
void iommu_reserved(uint16_t first, uint16_t last, uint64_t start, uint64_t end, unsigned access) {
    reservation.first = first; reservation.last = last; reservation.start = start;
    reservation.end = end; reservation.access = access; reservations++;
}
unsigned iommu_reserved_count(void) { return reservations; }
uint64_t iommu_alloc_pages(size_t count) {
    /* ホストのpointerはDMAアドレスではない。寿命/zeroingとcommand encoderの検査用。 */
    return (uint64_t)calloc(count, PAGE_SIZE);
}
void pmm_free(uint64_t physical) { (void)physical; freed_pages++; }
void iommu_cache_flush(const volatile void *p, size_t n) { (void)p; (void)n; }
bool iommu_fault_report(void) { return true; }
uint64_t iommu_deadline(unsigned ms) { (void)ms; return ++deadline_count; }
bool iommu_expired(uint64_t t) {
    (void)t;
    if (deliver_late_completion) {
        *(uint64_t *)phys_to_virt(late_address) = late_data;
        deliver_late_completion = false;
    }
    return true;
}
struct iommu_domain *iommu_domain_of(const struct pci_device *d) { (void)d; return claimed_domain; }
int irq_vector_alloc(unsigned count, unsigned align) { (void)count; (void)align; return 0x60; }
void irq_vector_free(unsigned first, unsigned count) { (void)first; (void)count; }
void vector_register(int vector, irq_handler_t handler) { (void)vector; (void)handler; }
uint32_t lapic_current_id(void) { return 0; }
static bool x2apic;
bool lapic_x2apic(void) { return x2apic; }
static uint64_t os_ram_start, os_ram_end;
bool iommu_os_ram(uint64_t start, uint64_t end) { return start < os_ram_end && os_ram_start < end; }
void pci_enable_busmaster(struct pci_device *d) { d->kernel_owned = true; }
bool pci_msi_enable(struct pci_device *d, uint8_t vector) { (void)d; (void)vector; msi_calls++; return true; }
unsigned pci_msix_init(struct pci_device *d) {
    (void)d; msix_inits++; memset(msix_unmasked, 0, sizeof msix_unmasked); return msix_size;
}
bool pci_msix_route(struct pci_device *d, unsigned entry, uint8_t vector, uint32_t dest) {
    (void)d; (void)vector; (void)dest; msix_routes++; return entry < msix_size;
}
void pci_msix_mask(struct pci_device *d, unsigned entry, bool masked) { (void)d; msix_unmasked[entry] = !masked; }
void pci_msix_function_mask(struct pci_device *d, bool masked) { (void)d; CHECK(!masked); }

static void reset(void) {
    for (size_t i = 0; i < nunits; i++) free(units[i].listed);
    free(units); free(aliases);
    units = NULL; nunits = units_room = 0; aliases = NULL; naliases = aliases_room = 0;
    memset(registers, 0, sizeof registers); memset(covered_ids, 0, sizeof covered_ids);
    memset(second_registers, 0, sizeof second_registers);
    plain_safe = true; map_failed_base = 0; register_writes = virtual_status_reads = 0;
    control_readonly = 0;
    memset(platform_ids, 0, sizeof platform_ids);
    parse_lost = np_cache = false; control_locked = exclusion_locked = perf_locked = false;
    locked_control_regs = NULL; freed_pages = 0;
    perf_reads = perf_writes = table_writes = unsafe_pointer_writes = command_restarts = 0;
    restart_stage = bad_event_restart = 0; restarting_events = false;
    msi_calls = msix_inits = msix_routes = msix_size = reservations = 0;
    function = (struct pci_device){0}; cap_header = CAP_EFR; cap_info = 48u << 15 | 48u << 8;
    second_function = (struct pci_device){.func = 1}; second_function_present = false;
    second_cap_info = cap_info;
    firmware = NULL; claimed_domain = NULL; amd.coherent = true; event_vector = -1;
    defer_completion = deliver_late_completion = false;
    pending_address = pending_data = late_address = late_data = 0;
    x2apic = wide = false; os_ram_start = os_ram_end = 0;
}

static void put16(uint8_t *b, uint16_t n) { memcpy(b, &n, sizeof n); }
static void put32(uint8_t *b, uint32_t n) { memcpy(b, &n, sizeof n); }
static void put64(uint8_t *b, uint64_t n) { memcpy(b, &n, sizeof n); }

static void test_handoff(void) {
    reset();
    struct unit u = {.regs = (void *)registers, .efr = EFR_PERF_OPT};
    devtab = 0x200000; u.cmd = 0x400000; u.events = 0x401000;
    registers[R_CONTROL / 8] = CTL_IOMMU_EN | CTL_CMDBUF_EN | CTL_EVENTLOG_EN;
    registers[R_STATUS / 8] = ST_CMDBUF_RUN | ST_EVENTLOG_RUN;
    registers[R_EXCL_BASE / 8] = 0x800000 | EXCL_ENABLE | EXCL_ALLOW_ALL;
    *(uint32_t *)((uint8_t *)registers + R_PERF_OPT) = PERF_OPT_EN;
    /* 旧Control=0経路はExEn/Allowに作用しない、という負の対照。 */
    wr(&u, R_CONTROL, 0);
    CHECK((rd(&u, R_EXCL_BASE) & (EXCL_ENABLE | EXCL_ALLOW_ALL)) == (EXCL_ENABLE | EXCL_ALLOW_ALL));
    CHECK(program(&u));
    CHECK(rd(&u, R_EXCL_BASE) == 0);
    CHECK(*(uint32_t *)((uint8_t *)registers + R_PERF_OPT) == 0);
    CHECK(perf_reads == 1 && perf_writes == 1 && unsafe_pointer_writes == 0);
    CHECK(!(rd(&u, R_CONTROL) & CTL_EVENTINT_EN));

    reset(); u = (struct unit){.regs = (void *)registers};
    CHECK(program(&u)); CHECK(!perf_reads && !perf_writes); /* EFR45=0のreserved registerを読まない */
    reset(); u = (struct unit){.regs = (void *)registers};
    registers[R_EXCL_BASE / 8] = EXCL_ENABLE | EXCL_ALLOW_ALL; exclusion_locked = true;
    CHECK(!program(&u) && table_writes == 0);
    reset(); u = (struct unit){.regs = (void *)registers, .efr = EFR_PERF_OPT};
    *(uint32_t *)((uint8_t *)registers + R_PERF_OPT) = PERF_OPT_EN; perf_locked = true;
    CHECK(!program(&u) && table_writes == 0);
    reset(); u = (struct unit){.regs = (void *)registers};
    registers[R_CONTROL / 8] = CTL_IOMMU_EN; control_locked = true;
    CHECK(!program(&u) && table_writes == 0);
}

static void test_optional_stop(void) {
    const struct { uint64_t feature; unsigned off; uint64_t running; } cases[] = {
        {EFR_PPR, R_STATUS, ST_PPRLOG_RUN}, {EFR_GA, R_STATUS, ST_GALOG_RUN},
        {EFR_VIOMMU, R_VSTATUS, VST_IOMMU_RUN}, {EFR_VIOMMU, R_VSTATUS, VST_CMDBUF_RUN}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        reset();
        units = calloc(1, sizeof *units); nunits = units_room = 1;
        units[0].regs = (void *)registers; units[0].efr = cases[i].feature;
        registers[cases[i].off / 8] = cases[i].running;
        CHECK(!program(&units[0]) && !table_writes);
        CHECK(!amd_shutdown());
    }
    reset();
    struct unit u = {.regs = (void *)registers};
    /* 未対応機能のreserved bitはread-as-zeroの保証でない。停止判定へ混ぜない。 */
    control_readonly = CTL_VIOMMU_EN | (1ull << 49);
    registers[R_STATUS / 8] = ST_PPRLOG_RUN | ST_GALOG_RUN;
    registers[R_VSTATUS / 8] = VST_IOMMU_RUN | VST_CMDBUF_RUN;
    CHECK(program(&u) && !virtual_status_reads);
}

static void test_event_restart(void) {
    reset();
    struct unit u = {.regs = (void *)registers};
    u.events = (uint64_t)calloc(1, RING_BYTES);
    registers[R_CONTROL / 8] = CTL_IOMMU_EN | CTL_CMDBUF_EN | CTL_EVENTLOG_EN | CTL_EVENTINT_EN;
    registers[R_STATUS / 8] = ST_CMDBUF_RUN | ST_EVENT_OVERFLOW | ST_EVENT_INT;
    restarting_events = true;
    events_drain(&u);
    CHECK(restart_stage == 4 && !restarting_events && !bad_event_restart && !unsafe_pointer_writes);
    CHECK((rd(&u, R_STATUS) & (ST_EVENT_OVERFLOW | ST_EVENTLOG_RUN)) == ST_EVENTLOG_RUN);
    CHECK(command_restarts == 0);
    registers[R_STATUS / 8] = ST_EVENT_OVERFLOW;
    restarting_events = true; restart_stage = 0;
    events_drain(&u);
    CHECK(!bad_event_restart && command_restarts == 0); /* halted commandは勝手に再開しない */
    CHECK(!(rd(&u, R_CONTROL) & CTL_CMDBUF_EN));
    registers[R_STATUS / 8] = ST_EVENT_OVERFLOW | ST_EVENTLOG_RUN;
    restarting_events = true; restart_stage = 0;
    events_drain(&u);
    CHECK(restart_stage == 0 && u.overflow_told && !unsafe_pointer_writes); /* 停止未確認なら再配置しない */
    free((void *)u.events);
}

static void test_ivrs(void) {
    reset();
    struct unit u = {0};
    uint8_t entries[] = {0x42, 0x10, 1, 0, 0, 0x20, 2, 0, 0x43, 0, 3, 0, 0, 0x30, 4, 0, 4, 5, 3, 0};
    parse_devices(&u, entries, 0, sizeof entries);
    CHECK(!parse_lost && amd_alias(0x110) == 0x220 && amd_alias(0x305) == 0x430);
    CHECK(covered_ids[0x220] && covered_ids[0x430] && covered_ids[0x300] && covered_ids[0x305]);
    free(u.listed); u = (struct unit){0};
    parse_devices(&u, entries, 0, sizeof entries - 1); CHECK(parse_lost); free(u.listed);
    parse_lost = false; u = (struct unit){0};
    uint8_t f0[22] = {0xF0, 0x44, 0x55};
    parse_devices(&u, f0, 0, sizeof f0); CHECK(!parse_lost && platform_ids[0x5544]); free(u.listed);
    parse_lost = false; f0[21] = 1; u = (struct unit){0};
    parse_devices(&u, f0, 0, sizeof f0); CHECK(parse_lost); free(u.listed);

    reset();
    uint8_t md[32] = {0x21, 2}; put16(md + 4, 0x110); put64(md + 16, 0x8001); put64(md + 24, 2);
    parse_memory(md);
    CHECK(reservations == 1 && reservation.access == IOMMU_READ && reservation.start == 0x8000 && reservation.end == 0x9000);
    md[1] = 0; parse_memory(md); CHECK(reservations == 2 && reservation.access == 0);
    put16(md + 8, 1); parse_memory(md); CHECK(reservations == 2);
    put16(md + 8, 0); put64(md + 16, UINT64_MAX - 1); put64(md + 24, 8);
    parse_memory(md); CHECK(parse_lost && reservations == 2);

    reset();
    uint8_t table[48 + 40 + 4] = {0}; struct acpi_sdt *h = (void *)table;
    h->length = sizeof table; h->revision = 2; put32(table + 36, 1 | 64u << 15 | 52u << 8);
    uint8_t *hd = table + 48; hd[0] = 0x11; hd[1] = 32; put16(hd + 2, 44); put16(hd + 6, 0x40);
    put64(hd + 8, 0xFED90000); put64(hd + 24, 1ull << 10 | EFR_PERF_OPT);
    hd[40] = 2; put16(hd + 41, 0x110); firmware = h;
    cap_info = 64u << 15 | 52u << 8;
    const struct iommu_hw *hw = iommu_amd_probe(&plain_safe);
    CHECK(hw == &amd && amd.levels == 5 && amd.iova_bits == 57 && amd.phys_bits == 52);
    CHECK(units[0].efr & EFR_PERF_OPT); /* IVHD11 EFRがMMIOより優先 */
    CHECK(!wide); /* xAPICでは32bit IRTE */
    /* 2.2.5.3: x2APICではXTSupとGASupの両方がある時だけ128bit IRTE。片方だけでは32bitのまま。 */
    reset(); firmware = h; x2apic = true; put64(hd + 24, 1ull << 10 | EFR_XT | EFR_GA);
    CHECK(iommu_amd_probe(&plain_safe) == &amd && wide);
    reset(); firmware = h; x2apic = true; put64(hd + 24, 1ull << 10 | EFR_XT);
    CHECK(iommu_amd_probe(&plain_safe) == &amd && !wide);
    reset(); firmware = h; put64(hd + 24, 1ull << 10 | EFR_XT | EFR_GA);
    CHECK(iommu_amd_probe(&plain_safe) == &amd && !wide);
    put64(hd + 24, 1ull << 10 | EFR_PERF_OPT);
    reset(); h->length = 48 + 24; put16(hd + 2, 24); firmware = h;
    CHECK(!iommu_amd_probe(&plain_safe) && parse_lost); /* short IVHD11を24byte IVHD10扱いしない */
    reset(); h->length = sizeof table; put16(hd + 2, 44); hd[20] = 1; firmware = h;
    CHECK(!iommu_amd_probe(&plain_safe)); /* HATDis */
    reset(); hd[20] = 0; firmware = h; cap_info = 1u << 15 | 48u << 8;
    CHECK(!iommu_amd_probe(&plain_safe)); /* reserved VAsize */

    /* IVHD10/EFRなしは有効なlegacy記述。値はTables86/89とCapability+10hに従う。
       QEMUが実際にこの内容を公開することまでは、この入力では証明しない。 */
    reset();
    uint8_t legacy[48 + 24 + 4] = {0}; h = (void *)legacy;
    h->length = sizeof legacy; h->revision = 1; put32(legacy + 36, 48u << 15 | 48u << 8);
    hd = legacy + 48; hd[0] = 0x10; hd[1] = 32; put16(hd + 2, 28); put16(hd + 6, 0x40);
    put64(hd + 8, 0xFED90000); hd[24] = 1; cap_header = 0; firmware = h;
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.levels == 4 && amd.iova_bits == 48 && amd.phys_bits == 48);
    CHECK(!units[0].efr && !parse_lost && covered_ids[0x110]);
    reset(); firmware = h; cap_header = 0; cap_info = 40u << 15 | 40u << 8;
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.iova_bits == 40 && amd.phys_bits == 40); /* 共通幅との最小値 */
    reset(); firmware = h; cap_header = 0; cap_info = 48u << 15 | 41u << 8;
    CHECK(!iommu_amd_probe(&plain_safe)); /* 実装未定義の幅を推測して受理しない */
}

static void test_probe_widths(void) {
    /* Table86の共通要約とCapability+10hの実値を別々に入力する。
       48-byte IVRS header、24-byte IVHD10、4-byte device entryを二unit分持つ。 */
    uint8_t table[48 + (24 + 4) * 2] = {0};
    struct acpi_sdt *h = (void *)table;
    h->length = 48 + 24 + 4; h->revision = 1;
    uint8_t *hd = table + 48;
    hd[0] = 0x10; hd[1] = 32; put16(hd + 2, 24 + 4); put16(hd + 6, 0x40);
    put64(hd + 8, 0xFED90000); hd[24] = 1;

    reset(); firmware = h; put32(table + 36, 1 | 40u << 8); cap_info = 48u << 15 | 40u << 8;
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.iova_bits == 48 && amd.phys_bits == 40); /* 要約VA欠損 */
    reset(); firmware = h; put32(table + 36, 0);
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.iova_bits == 48 && amd.phys_bits == 48); /* 両要約欠損 */
    reset(); firmware = h; put32(table + 36, 1u << 15 | 40u << 8);
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.iova_bits == 48 && amd.phys_bits == 40); /* 予約VAは上限にしない */
    reset(); firmware = h; put32(table + 36, 40u << 15 | 41u << 8);
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.iova_bits == 40 && amd.phys_bits == 48); /* 有効VAだけ上限 */
    reset(); firmware = h; put32(table + 36, 32u << 15 | 40u << 8); cap_info = 64u << 15 | 52u << 8;
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.iova_bits == 32 && amd.phys_bits == 40); /* 有効要約の縮小維持 */
    reset(); firmware = h; put32(table + 36, 64u << 15 | 52u << 8); cap_info = 32u << 15 | 40u << 8;
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.iova_bits == 32 && amd.phys_bits == 40); /* 実値の縮小維持 */
    reset(); firmware = h; put32(table + 36, 48u << 15 | 48u << 8); cap_info = 48u << 8;
    CHECK(!iommu_amd_probe(&plain_safe)); /* 実VA0は有効要約で補わない */
    reset(); firmware = h; cap_info = 48u << 15;
    CHECK(!iommu_amd_probe(&plain_safe)); /* 実PA0も拒否 */
    reset(); firmware = h; cap_info = 48u << 15 | 41u << 8;
    CHECK(!iommu_amd_probe(&plain_safe)); /* 実reserved PAも拒否 */
    reset(); firmware = h; function.bus = 1; put32(table + 36, 40u << 8);
    CHECK(!iommu_amd_probe(&plain_safe)); /* 実capabilityなし・要約VA0を推測で補わない */
    reset(); firmware = h; function.bus = 1; put32(table + 36, 48u << 15);
    CHECK(!iommu_amd_probe(&plain_safe)); /* 実capabilityなし・要約PA0も拒否 */
    reset(); firmware = h; function.bus = 1; put32(table + 36, 40u << 15 | 40u << 8);
    CHECK(iommu_amd_probe(&plain_safe) == &amd && amd.iova_bits == 40 && amd.phys_bits == 40); /* 有効要約fallback維持 */

    h->length = sizeof table;
    uint8_t *second_hd = hd + 24 + 4;
    memcpy(second_hd, hd, 24 + 4); put16(second_hd + 4, 1);
    put64(second_hd + 8, 0xFED90000 + MMIO_BYTES);
    reset(); firmware = h; second_function_present = true; put32(table + 36, 0);
    cap_info = 64u << 15 | 52u << 8; second_cap_info = 40u << 15 | 40u << 8;
    CHECK(iommu_amd_probe(&plain_safe) == &amd && nunits == 2 && amd.iova_bits == 40 && amd.phys_bits == 40);
    CHECK(units[0].fn == &function && units[1].fn == &second_function); /* 全実unitの最小値 */
    reset(); firmware = h; second_function_present = true; second_cap_info = 1u << 15 | 40u << 8;
    CHECK(!iommu_amd_probe(&plain_safe)); /* 後続unitの実reserved VAを見落とさない */
    reset(); firmware = h;
    CHECK(!iommu_amd_probe(&plain_safe)); /* 第一unitだけ実値があっても欠損要約では足りない */
}

static void check_rejected_state(bool safe) {
    CHECK(!iommu_amd_probe(&plain_safe));
    CHECK(plain_safe == safe);
    CHECK(!register_writes); /* 拒否を理由にfirmwareのengineやqueueを書き換えない */
}

static void test_probe_rejection_state(void) {
    /* 二つの44-byte IVHD11、33-byte不正IVMDと3-byte端数を個別に試す領域。 */
    uint8_t table[48 + (40 + 4) * 2 + 33 + 3] = {0};
    struct acpi_sdt *h = (void *)table;
    uint8_t *hd = table + 48, *next = hd + 44;
    h->length = 48 + 44; h->revision = 2;
    put32(table + 36, 1 | 48u << 15 | 48u << 8);
    hd[0] = 0x11; put16(hd + 2, 44); put16(hd + 6, 0x40);
    put64(hd + 8, first_base);
    put64(hd + 24, (1ull << 55) | (1ull << 7) | (1ull << 1)); /* vIOMMU/GA/PPR */
    hd[40] = 1;
    reset();
    CHECK(!iommu_amd_probe(&plain_safe) && plain_safe && !register_writes); /* IVRSなし */
    reset(); firmware = h; cap_info = 48u << 15 | 41u << 8;
    registers[R_CONTROL / 8] = 1ull << 10; /* Coherentはengineのenableではない */
    check_rejected_state(true);
    CHECK(virtual_status_reads != 0);
    const uint64_t enabled[] = {
        CTL_IOMMU_EN, CTL_CMDBUF_EN, CTL_EVENTLOG_EN, CTL_EVENTINT_EN,
        1ull << 4, /* ComWaitIntEn */
        1ull << 13, 1ull << 14, 1ull << 15, /* PPRLogEn/PprIntEn/PPREn */
        1ull << 17, 1ull << 28, 1ull << 29, /* GAEn/GALogEn/GAIntEn */
        1ull << 52, 1ull << 53 /* vCmdEn/vIommuEn */
    };
    for (size_t i = 0; i < sizeof enabled / sizeof *enabled; i++) {
        reset(); firmware = h; cap_info = 48u << 15 | 41u << 8;
        registers[R_CONTROL / 8] = enabled[i];
        check_rejected_state(false);
        CHECK(registers[R_CONTROL / 8] == enabled[i]);
    }
    const uint64_t running[] = {ST_CMDBUF_RUN, ST_EVENTLOG_RUN, 1ull << 7, 1ull << 8}; /* PPRLogRun/GALogRun */
    for (size_t i = 0; i < sizeof running / sizeof *running; i++) {
        reset(); firmware = h; cap_info = 48u << 15 | 41u << 8;
        registers[R_STATUS / 8] = running[i];
        check_rejected_state(false);
    }
    for (unsigned bit = 0; bit < 2; bit++) { /* vIOMMU StatusのvIommuRun/vCmdRun */
        reset(); firmware = h; cap_info = 48u << 15 | 41u << 8;
        registers[0x190 / 8] = 1ull << bit;
        check_rejected_state(false);
    }
    reset(); firmware = h; put16(hd + 6, 0x3C); check_rejected_state(true); /* cap範囲外 */
    put16(hd + 6, 0x40);
    reset(); firmware = h; hd[20] = 1; check_rejected_state(true); /* HATDis */
    hd[20] = 0;
    reset(); firmware = h; hd[40] = 0x80; check_rejected_state(true); /* entry長不明 */
    reset(); firmware = h; hd[40] = 3; check_rejected_state(true); /* range終端欠損 */
    hd[40] = 1;
    reset(); firmware = h; h->length = 48 + 43; put16(hd + 2, 43);
    check_rejected_state(true); /* IVHD内部端数でも次IVDB位置は明確 */
    h->length = 48 + 44; put16(hd + 2, 44);
    reset(); firmware = h; put16(hd + 16, 1); check_rejected_state(true); /* 全unit非zero segment */
    registers[R_CONTROL / 8] = CTL_IOMMU_EN; check_rejected_state(false);
    put16(hd + 16, 0);
    reset(); firmware = h; next[0] = 0x20; put16(next + 2, 33); h->length = 48 + 44 + 33;
    check_rejected_state(true); /* IVMD長違反とunit列挙は別 */
    reset(); firmware = h; put16(next + 2, 32); h->length = 48 + 44 + 32;
    put64(next + 16, UINT64_MAX - 1); put64(next + 24, 8);
    check_rejected_state(true); /* IVMD加算overflow */
    memcpy(next, hd, 44); put16(next + 4, 1); put64(next + 8, first_base + MMIO_BYTES);
    reset(); firmware = h; h->length = 48 + 44 * 2; cap_info = 48u << 15 | 41u << 8;
    check_rejected_state(true);
    second_registers[R_CONTROL / 8] = CTL_IOMMU_EN; check_rejected_state(false);
    reset(); firmware = h; cap_info = 48u << 15 | 41u << 8; map_failed_base = first_base + MMIO_BYTES;
    check_rejected_state(false);
    put16(next + 16, 1); /* 未対応segmentの後続unitも状態監査の対象 */
    reset(); firmware = h; cap_info = 48u << 15 | 41u << 8;
    second_registers[R_CONTROL / 8] = CTL_IOMMU_EN; check_rejected_state(false);
    reset(); firmware = h; h->length = 48 + 44; map_failed_base = first_base;
    check_rejected_state(false);
    reset(); firmware = h; h->length = 48 + 44 + 3; check_rejected_state(false); /* 外周端数 */
    reset(); firmware = h; h->length = 48 + 44; put16(hd + 2, 0); check_rejected_state(false);
    reset(); firmware = h; h->length = 48 + 24; put16(hd + 2, 24); check_rejected_state(false); /* header欠損 */
    reset(); firmware = h; h->length = 48 + 44; put16(hd + 2, 44); put64(hd + 8, 0); check_rejected_state(false);
    reset(); firmware = h; put64(hd + 8, first_base + 1); check_rejected_state(false); /* MMIO alignment */
    put64(hd + 8, first_base);
    reset(); firmware = h; hd[0] = 0x90; check_rejected_state(false); /* 未知IVDB */
    reset(); firmware = h; h->revision = 3; check_rejected_state(false); /* 未定義format */
    reset(); firmware = h; h->revision = 2; h->length = 48; check_rejected_state(false); /* unitなし */
    reset(); firmware = h; h->length = 47; check_rejected_state(false); /* IVRS header欠損 */
    reset(); firmware = h; hd[0] = 0x10; hd[24] = 0x80; put16(hd + 2, 28); h->length = 48 + 28;
    put32(table + 36, 48u << 15 | 48u << 8); cap_header = 0;
    check_rejected_state(true);
    CHECK(!virtual_status_reads); /* EFRなしのreserved仮想statusを読まない */
}

static void test_probe_depth(void) {
    uint8_t table[48 + 40 + 4] = {0};
    struct acpi_sdt *h = (void *)table;
    uint8_t *hd = table + 48;
    h->length = sizeof table; h->revision = 2;
    put32(table + 36, 1 | 64u << 15 | 52u << 8);
    hd[0] = 0x11; put16(hd + 2, 44); put16(hd + 6, 0x40); put64(hd + 8, first_base); hd[40] = 1;
    const struct { unsigned va, pa, hats, levels, iova; } cases[] = {
        {32, 40, 0, 3, 32}, {40, 40, 2, 4, 40}, {48, 48, 1, 4, 48},
        {64, 52, 2, 5, 64}, {64, 52, 0, 4, 48}, {64, 52, 1, 5, 57}, {64, 40, 2, 4, 64}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        reset(); firmware = h; cap_info = cases[i].va << 15 | cases[i].pa << 8;
        put64(hd + 24, (uint64_t)cases[i].hats << 10);
        CHECK(iommu_amd_probe(&plain_safe) == &amd);
        CHECK(amd.levels == cases[i].levels && amd.iova_bits == cases[i].iova);
        unsigned bits = MIN(amd.iova_bits, amd.phys_bits);
        CHECK(12 + 9 * amd.levels >= bits && 12 + 9 * (amd.levels - 1) < bits);
    }
    reset(); firmware = h; put64(hd + 24, 3ull << 10);
    check_rejected_state(true); /* reserved HATSの拒否を緩めない */
    reset(); firmware = h; put64(hd + 24, 0);
    registers[R_EFR / 8] = EFR_VIOMMU;
    CHECK(iommu_amd_probe(&plain_safe) == &amd && !units[0].efr && (units[0].state_efr & EFR_VIOMMU));
    registers[R_VSTATUS / 8] = VST_CMDBUF_RUN;
    CHECK(!program(&units[0]) && !table_writes); /* firmware imageが実engineの停止確認を隠さない */
}

static void test_bypass_handoff(void) {
    uint8_t table[48 + 40 + 4] = {0};
    struct acpi_sdt *h = (void *)table;
    uint8_t *hd = table + 48;
    h->length = sizeof table; h->revision = 2;
    put32(table + 36, 1 | 48u << 15 | 48u << 8);
    hd[0] = 0x11; put16(hd + 2, 44); put16(hd + 6, 0x40);
    put64(hd + 8, first_base); hd[40] = 1;
    const struct { uint64_t hardware, image; bool locked; } perf[] = {
        {EFR_PERF_OPT, 0, false}, {EFR_PERF_OPT, 0, true},
        {0, EFR_PERF_OPT, false}, {EFR_PERF_OPT, EFR_PERF_OPT, false}
    };
    for (size_t i = 0; i < ARRAY_SIZE(perf); i++) {
        reset(); firmware = h;
        registers[R_EFR / 8] = perf[i].hardware;
        put64(hd + 24, perf[i].image);
        *(uint32_t *)((uint8_t *)registers + R_PERF_OPT) = PERF_OPT_EN;
        perf_locked = perf[i].locked;
        CHECK(iommu_amd_probe(&plain_safe) == &amd);
        CHECK(units[0].efr == perf[i].image && (units[0].state_efr & EFR_PERF_OPT));
        CHECK(program(&units[0]) == !perf[i].locked);
        CHECK(perf_reads == 1 && perf_writes == 1);
        if (perf[i].locked) {
            CHECK(!table_writes && !(rd(&units[0], R_CONTROL) & CTL_IOMMU_EN));
        } else {
            CHECK(table_writes == 1 && !(*(uint32_t *)((uint8_t *)registers + R_PERF_OPT) & PERF_OPT_EN));
        }
    }
    /* MarcSup[43:42]の定義済み値01/10。実機能とimage双方から停止確認へ接続する。 */
    const uint64_t marc[] = {1ull << 42, 2ull << 42};
    for (size_t i = 0; i < ARRAY_SIZE(marc); i++) {
        for (unsigned image = 0; image < 2; image++) {
            reset(); firmware = h;
            registers[R_EFR / 8] = image ? 0 : marc[i];
            put64(hd + 24, image ? marc[i] : 0);
            CHECK(iommu_amd_probe(&plain_safe) == &amd);
            control_readonly = 1ull << 40; /* MarcEnがControl=0の後も残るreadback失敗 */
            CHECK(!program(&units[0]) && !table_writes);
            CHECK(!amd_shutdown());
            control_readonly = 0;
            registers[R_CONTROL / 8] = 1ull << 40;
            table_writes = 0;
            CHECK(program(&units[0]) && table_writes == 1 && !(rd(&units[0], R_CONTROL) & (1ull << 40)));
            CHECK(amd_shutdown());
        }
    }
    reset(); firmware = h;
    put64(hd + 24, 0); registers[R_EFR / 8] = EFR_PERF_OPT;
    *(uint32_t *)((uint8_t *)registers + R_PERF_OPT) = PERF_OPT_EN;
    cap_info = 48u << 15 | 41u << 8; /* 隔離拒否と、無効なunitのplain配送は別の責務 */
    check_rejected_state(true);
    CHECK(!perf_reads && !perf_writes && *(uint32_t *)((uint8_t *)registers + R_PERF_OPT) == PERF_OPT_EN);
    reset();
    struct unit u = {.regs = (void *)registers};
    control_readonly = 1ull << 40;
    *(uint32_t *)((uint8_t *)registers + R_PERF_OPT) = PERF_OPT_EN;
    CHECK(program(&u) && !perf_reads && !perf_writes); /* 未対応のreserved bit/レジスターを0要求しない */
}

static void test_commands_and_dte(void) {
    reset();
    units = calloc(1, sizeof *units); nunits = units_room = 1;
    units[0].regs = (void *)registers; units[0].cmd = (uint64_t)calloc(1, RING_BYTES);
    units[0].events = (uint64_t)calloc(1, RING_BYTES); sems = (uint64_t)calloc(1, PAGE_SIZE);
    registers[R_CONTROL / 8] = CTL_IOMMU_EN | CTL_CMDBUF_EN; registers[R_STATUS / 8] = ST_CMDBUF_RUN;
    send_domain_flush(7);
    uint32_t *commands = phys_to_virt(units[0].cmd);
    CHECK(commands[0] == 0 && commands[1] == (CMD_INV_PAGES << 28 | 7));
    CHECK(commands[2] == 0xFFFFF003u && commands[3] == 0x7FFFFFFFu);
    CHECK(sync());
    devtab = (uint64_t)calloc(1, DT_BYTES);
    dte_write(0x110, DTE_V | DTE_TV | DTE_MODE(5) | 0x200000 | DTE_IR | DTE_IW, 7,
              DTE_IV | DTE_IRT_LEN(IRT_LOG2) | 0x300000 | DTE_INT_REMAP);
    uint64_t *dte = (uint64_t *)phys_to_virt(devtab) + 0x110 * 4;
    CHECK(((dte[0] >> 9) & 7) == 5 && dte[1] == 7 && (dte[2] >> 60) == 2);
    CHECK(amd_is_leaf(amd_leaf(0x200000, 2, IOMMU_READ), 2));
    CHECK(amd_is_leaf(PTE_PR | PTE_NEXT_LEVEL, 2));
    CHECK(!amd_is_leaf(amd_table(0x200000, 5), 5));
    uint16_t id = 0x110; struct iommu_domain dom = {.id = 7, .ids = &id, .nids = 1};
    memset(commands, 0, RING_BYTES); units[0].cmd_tail = 0; registers[R_CMD_HEAD / 8] = 0;
    CHECK(amd_block(&dom));
    /* Table 7: 解放後に再利用されるIDを、Mode0/root0のblock DTEへ残さない。旧IDのpageは2.4.2で消す。 */
    CHECK(dte[0] == (DTE_V | DTE_TV) && dte[1] == BLOCK_DOMAIN && dte[2] == DTE_IV);
    CHECK(commands[2 * 4 + 1] == (CMD_INV_PAGES << 28 | 7));
    uint32_t address, data; claimed_domain = &dom;
    CHECK(!amd_msi(&function, UINT_MAX, 2, 0x60, 0, &address, &data));
    dom.irt = (uint64_t)calloc(1, PAGE_SIZE);
    CHECK(amd_msi(&function, 1, 2, 0x60, 0x12, &address, &data) && address == 0xFEE00000u && data == 1);
    CHECK(((uint32_t *)phys_to_virt(dom.irt))[1] == (1u | 0x12u << 8 | 0x60u << 16));
    CHECK(!amd_msi(&function, 0, 1, 0x60, 0x100, &address, &data)); /* 32bit IRTEは8bit宛先だけ */
    /* x2APIC: Figure 19のGuestMode=0形式。Dest[23:0]→31:8、Vector→71:64、Dest[31:24]→127:120。 */
    wide = true;
    CHECK(amd_msi(&function, 1, 2, 0x60, 0x12345678, &address, &data) && address == 0xFEE00000u && data == 1);
    uint64_t *wide_irt = phys_to_virt(dom.irt);
    CHECK(wide_irt[2] == (1u | 0x345678ull << 8) && wide_irt[3] == (0x60 | 0x12ull << 56));
    CHECK(wide_irt[4] == (1u | 0x345678ull << 8) && wide_irt[5] == (0x61 | 0x12ull << 56));
    CHECK(amd_msi(&function, 1, 1, 0x70, 3, &address, &data) && wide_irt[2] == (1u | 3u << 8) && wide_irt[3] == 0x70);
    CHECK(amd_msi_clear(&function) && !wide_irt[2] && !wide_irt[3] && !wide_irt[4] && !wide_irt[5]);
    claimed_domain = NULL; /* 未claimの平文MSIは128bit IRTEでも8bit宛先しか持てない */
    CHECK(!amd_msi(&function, 0, 1, 0x60, 0x100, &address, &data));
    CHECK(amd_msi(&function, 0, 1, 0x60, 0x12, &address, &data) && address == iommu_msi_plain(0x12) && data == 0x60);
    wide = false; claimed_domain = &dom; free(phys_to_virt(dom.irt)); dom.irt = 0;
    registers[R_STATUS / 8] = 0; /* command processor faultの再開は当該syncを成功させない */
    CHECK(!amd_block(&dom));
    CHECK(amd_block(&dom)); /* 全invalidationを再発行してから確認する */
    uint64_t retained_devtab = devtab, retained_cmd = units[0].cmd;
    control_locked = true; registers[R_CONTROL / 8] = CTL_IOMMU_EN;
    CHECK(!amd_shutdown()); CHECK(!amd_resume());
    CHECK(devtab == retained_devtab && units[0].cmd == retained_cmd);
    control_locked = false; CHECK(amd_shutdown());
    free(phys_to_virt(devtab)); free(phys_to_virt(sems));
    free(phys_to_virt(units[0].cmd)); free(phys_to_virt(units[0].events)); devtab = sems = 0;
}

static void test_stalled_unit(void) {
    reset();
    units = calloc(1, sizeof *units); nunits = units_room = 1;
    units[0].regs = (void *)registers; units[0].cmd = (uint64_t)calloc(1, RING_BYTES);
    units[0].events = (uint64_t)calloc(1, RING_BYTES); sems = (uint64_t)calloc(1, PAGE_SIZE);
    units[0].listed = calloc(1, sizeof *units[0].listed); units[0].nlisted = units[0].listed_room = 1;
    units[0].listed[0] = (struct span){0x10, 0x10};
    registers[R_CONTROL / 8] = CTL_IOMMU_EN | CTL_CMDBUF_EN; registers[R_STATUS / 8] = ST_CMDBUF_RUN;
    /* IASupなし: 掲載IDのDTE/IRTと、block DTEのdomainのpageを消す。 */
    send_everything_flush();
    uint32_t *commands = phys_to_virt(units[0].cmd);
    CHECK(commands[1] == (CMD_INV_PAGES << 28 | BLOCK_DOMAIN));
    CHECK(commands[5] == (CMD_INV_DEVTAB << 28) && commands[4] == 0x10 && commands[9] == (CMD_INV_IRT << 28));
    CHECK(sync());
    /* 止まったユニット: 最初の1件だけ待ち、残り（全ID分）を各100ms待たない。syncで解除。 */
    units[0].listed[0] = (struct span){0, 0xFFFF};
    registers[R_CMD_HEAD / 8] = (units[0].cmd_tail + 16) % RING_BYTES;
    deadline_count = 0;
    send_everything_flush();
    CHECK(deadline_count == 1 && units[0].lost && units[0].stalled);
    CHECK(!sync() && deadline_count == 2 && !units[0].lost && !units[0].stalled);
    registers[R_CMD_HEAD / 8] = units[0].cmd_tail;
    CHECK(sync());
    free(phys_to_virt(sems)); free(phys_to_virt(units[0].cmd)); free(phys_to_virt(units[0].events)); sems = 0;
}

/* 2.3.1/2.6.4.5: firmwareのSMI filterとStopMark遮断はengineの停止・再有効化で消さない。
   2.2.5.3: x2APICではXTEnとGAEn（GAMEn=000b）。引継ぎ時の表がOS所有RAMにあれば検出する。 */
static void test_firmware_bits(void) {
    reset();
    units = calloc(1, sizeof *units); nunits = units_room = 1;
    struct unit *u = &units[0];
    *u = (struct unit){.regs = (void *)registers, .efr = EFR_XT | EFR_GA};
    wide = true;
    registers[R_CONTROL / 8] = CTL_IOMMU_EN | CTL_FIRMWARE | CTL_PPR_EN;
    CHECK(program(u));
    CHECK((rd(u, R_CONTROL) & (CTL_FIRMWARE | CTL_XT_EN | CTL_GA_EN | CTL_PPR_EN | 7ull << 25)) ==
          (CTL_FIRMWARE | CTL_XT_EN | CTL_GA_EN));
    CHECK(units_off() && rd(u, R_CONTROL) == CTL_FIRMWARE);
    wide = false; registers[R_CONTROL / 8] = CTL_SMIF_EN | CTL_XT_EN | CTL_GA_EN;
    CHECK(program(u) && (rd(u, R_CONTROL) & (CTL_FIRMWARE | CTL_XT_EN | CTL_GA_EN)) == CTL_SMIF_EN);
    registers[R_DEVTAB / 8] = 0x200000 | 1; registers[R_CMDBUF / 8] = 0x400000 | RING_LEN;
    registers[R_EVENTLOG / 8] = 0x300000 | RING_LEN;
    os_ram_start = 0x300000; os_ram_end = 0x301000;
    CHECK(left_in_os_ram(u, CTL_IOMMU_EN | CTL_EVENTLOG_EN));
    CHECK(!left_in_os_ram(u, CTL_IOMMU_EN | CTL_CMDBUF_EN) && !left_in_os_ram(u, CTL_EVENTLOG_EN));
    os_ram_start = 0x201000; os_ram_end = 0x202000;
    CHECK(left_in_os_ram(u, CTL_IOMMU_EN)); /* Size 1は2ページ */
}

static void test_shared_msix(void) {
    reset();
    units = calloc(2, sizeof *units); nunits = units_room = 2;
    for (size_t i = 0; i < nunits; i++) {
        units[i].fn = &function; units[i].regs = (void *)registers; units[i].msi_num = (uint8_t)(i + 2);
    }
    registers[R_CONTROL / 8] = CTL_IOMMU_EN | CTL_CMDBUF_EN; registers[R_STATUS / 8] = ST_CMDBUF_RUN;
    event_vector = 0x60; msix_size = 4;
    event_interrupts_enable();
    CHECK(msix_inits == 1 && msix_routes == 2 && msi_calls == 0);
    CHECK(units[0].event_irq && units[1].event_irq && msix_unmasked[2] && msix_unmasked[3]);
    CHECK(command_restarts == 0 && function.kernel_owned);
}

static void test_late_completion(void) {
    reset();
    units = calloc(1, sizeof *units); nunits = units_room = 1;
    units[0].regs = (void *)registers; units[0].cmd = (uint64_t)calloc(1, RING_BYTES);
    units[0].events = (uint64_t)calloc(1, RING_BYTES); sems = (uint64_t)calloc(1, PAGE_SIZE);
    registers[R_CONTROL / 8] = CTL_IOMMU_EN | CTL_CMDBUF_EN; registers[R_STATUS / 8] = ST_CMDBUF_RUN;
    token = 0; defer_completion = true;
    CHECK(!sync()); /* headが進んでも、未完了storeを完了としない */
    late_address = pending_address; late_data = pending_data; deliver_late_completion = true;
    CHECK(!sync()); /* 前回の遅延storeでは今回のbarrierを確認できない */
    CHECK(*(uint64_t *)phys_to_virt(sems) == late_data && late_data != token);
    CHECK(!unsafe_pointer_writes && !command_restarts);
    defer_completion = false;
    CHECK(sync()); /* 今回のtokenのstoreだけで回復を確認する */
    token = UINT64_MAX; defer_completion = true;
    CHECK(!sync()); /* uint64_tのwrapでも初期値0を完了と誤認しない */
    defer_completion = false;
    CHECK(sync());
    free(phys_to_virt(sems)); free(phys_to_virt(units[0].cmd)); free(phys_to_virt(units[0].events)); sems = 0;
}

static void test_partial_program(void) {
    reset();
    static uint64_t other_registers[MMIO_BYTES / sizeof(uint64_t)];
    memset(other_registers, 0, sizeof other_registers);
    units = calloc(2, sizeof *units); nunits = units_room = 2;
    units[0].regs = (void *)registers; units[1].regs = (void *)other_registers;
    locked_control_regs = units[1].regs; control_locked = true;
    other_registers[R_CONTROL / 8] = CTL_IOMMU_EN;
    CHECK(!amd_start(true));
    CHECK(freed_pages == 0 && devtab && sems && units[0].cmd && units[1].cmd);
    CHECK(!(registers[R_CONTROL / 8] & CTL_IOMMU_EN) && (other_registers[R_CONTROL / 8] & CTL_IOMMU_EN));
    uint64_t retained_devtab = devtab, retained_sems = sems;
    uint64_t retained_cmd[] = {units[0].cmd, units[1].cmd};
    uint64_t retained_events[] = {units[0].events, units[1].events};
    CHECK(!amd_resume()); /* 第一unitが再開しても第二unitの失敗は成功化しない */
    CHECK(!amd_shutdown());
    CHECK(!freed_pages && devtab == retained_devtab && sems == retained_sems && units[0].cmd == retained_cmd[0]);
    control_locked = false;
    CHECK(amd_shutdown());
    tables_free();
    CHECK(freed_pages == DT_BYTES / PAGE_SIZE + sem_pages + nunits * 2 && !devtab && !sems && !units[0].cmd && !units[1].cmd);
    /* pmm_freeは個別pageの所有通知だけを数える。ホストのcontiguous allocationはここで回収。 */
    free(phys_to_virt(retained_devtab)); free(phys_to_virt(retained_sems));
    for (size_t i = 0; i < nunits; i++) { free(phys_to_virt(retained_cmd[i])); free(phys_to_virt(retained_events[i])); }
}

int main(void) {
    test_handoff(); test_event_restart(); test_ivrs(); test_probe_widths(); test_commands_and_dte(); test_shared_msix();
    test_probe_rejection_state(); test_probe_depth(); test_optional_stop(); test_bypass_handoff();
    test_late_completion(); test_partial_program(); test_stalled_unit(); test_firmware_bits(); reset();
    printf("AMD IOMMU focused検査: %u件、失敗%u件（実機・QEMU未検証）\n", checks, failed);
    return failed ? 1 : 0;
}
