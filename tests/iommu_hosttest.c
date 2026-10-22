/* 共通IOMMUの実製品コード。物理ページ/PCI/firmwareだけを模擬し、実機検証とは分ける。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "arch/cpu.h"

/* ページテーブル一枚分のスロット数を持つ、試験用物理ページプール。 */
static uint64_t memory[PAGE_SIZE / sizeof(uint64_t)][PAGE_SIZE / sizeof(uint64_t)];
static bool used[ARRAY_SIZE(memory)], opt_in, no_memory;
static size_t live_pages, frees, flushes;
static struct pmm_range *ram_ranges;
static unsigned ram_count;
static bool ram_complete = true;
static bool firmware_declared;
static bool probe_plain;
static bool intel_probe_plain = true;
static uint16_t pic_masks = UINT16_MAX;
static bool pins_masked, route_ok = true, start_ok;
static unsigned starts, stops, routes;
static uint64_t host_irq_save(void) { return 0; }
static void host_irq_restore(uint64_t flags) { (void)flags; }
uint64_t tsc_hz;
volatile uint64_t timer_ticks;
static void *host_phys(uint64_t p) {
    if (!p || p / PAGE_SIZE > ARRAY_SIZE(memory)) abort();
    return (uint8_t *)memory[p / PAGE_SIZE - 1] + p % PAGE_SIZE;
}
#define phys_to_virt host_phys
#define unlink iommu_list_unlink /* ホストstdioが公開する同名のファイルAPIとは別 */
#define irq_save host_irq_save
#define irq_restore host_irq_restore
#include "../kernel/src/dev/iommu.c"
#undef irq_save
#undef irq_restore
#undef unlink

static unsigned checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr,"失敗 %d: %s\n",__LINE__,#c); } } while (0)
int kprintf(const char *format, ...) { (void)format; return 0; }
void panic(const char *format, ...) { (void)format; abort(); }
bool cmdline_has(const char *word) { return opt_in && !strcmp(word,"appdrivers-unisolated"); }
void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return no_memory ? NULL : calloc(1,n); }
void kfree(void *p) { free(p); }
void *kappend(void *at,size_t *n,size_t *room,size_t bytes) {
    if (*n == *room) {
        size_t next = *room ? *room * 2 : 1;
        void *p = realloc(*(void **)at,next * bytes);
        if (!p) return NULL;
        *(void **)at = p; *room = next;
    }
    return (char *)*(void **)at + (*n)++ * bytes;
}
uint64_t pmm_alloc_contig_below(uint64_t n, uint64_t limit) {
    for (size_t i = 0, run = 0; i < ARRAY_SIZE(used) && (i + 2) * PAGE_SIZE <= limit; i++) {
        run = used[i] ? 0 : run + 1;
        if (run == n) {
            for (size_t j = i + 1 - n; j <= i; j++) used[j] = true;
            live_pages += n;
            return (i + 2 - n) * PAGE_SIZE;
        }
    }
    return 0;
}
uint64_t pmm_alloc_contig(uint64_t n) { return pmm_alloc_contig_below(n, UINT64_MAX); }
uint64_t pmm_alloc(void) { return pmm_alloc_contig(1); }
uint64_t pmm_alloc_below(uint64_t limit) {
    for (size_t i = 0; i < ARRAY_SIZE(used) && (i + 2) * PAGE_SIZE <= limit; i++)
        if (!used[i]) { used[i] = true; live_pages++; return (i + 1) * PAGE_SIZE; }
    return 0;
}
unsigned pmm_ram_ranges(const struct pmm_range **out) { *out = ram_ranges; return ram_count; }
bool pmm_ram_complete(void) { return ram_complete; }
void pmm_free(uint64_t p) { CHECK(p && p / PAGE_SIZE <= ARRAY_SIZE(used) && used[p / PAGE_SIZE - 1]); used[p / PAGE_SIZE - 1] = false; live_pages--; frees++; }
/* 列挙された機能（groupの形成が辿る）と、resetから戻らなかった機能（pci_reset_stuck）。
   reset中の印はpci_device.resetが非NULLであることだけで、その中身はpci.cのもの。 */
static struct pci_device *enumerated[4];
static int enumerated_count;
static const struct pci_device *stuck_function;
static unsigned config_reads; /* ACSを探す・読む構成アクセス */
int pci_count(void) { return enumerated_count; }
struct pci_device *pci_at(int i) { return i >= 0 && i < enumerated_count ? enumerated[i] : NULL; }
bool pci_all_known(void) { return true; }
bool pci_below(const struct pci_device *a,const struct pci_device *b) { (void)a;(void)b;return false; }
bool pci_same_device(const struct pci_device *a,const struct pci_device *b) { return a == b; }
bool pci_reset_stuck(const struct pci_device *d) { return d->reset && d == stuck_function; }
uint16_t pci_find_ext_cap(const struct pci_device *d,uint16_t id) { (void)d;(void)id;config_reads++;return 0; }
uint16_t pci_read16(uint8_t b,uint8_t d,uint8_t f,uint16_t at) { (void)b;(void)d;(void)f;(void)at;config_reads++;return 0; }
void pci_write16(uint8_t b,uint8_t d,uint8_t f,uint16_t at,uint16_t v) { (void)b;(void)d;(void)f;(void)at;(void)v; }
static const struct iommu_hw *probe_backend;
const struct iommu_hw *iommu_amd_probe(bool *plain_safe) { *plain_safe = !firmware_declared || probe_plain; return probe_backend; }
const struct iommu_hw *iommu_intel_probe(bool *plain_safe) { *plain_safe = intel_probe_plain; return NULL; }
static const char *amd_conflict, *intel_conflict;
const char *iommu_amd_x2apic_conflict(void) { return amd_conflict; }
const char *iommu_intel_x2apic_conflict(void) { return intel_conflict; }
const void *acpi_find(const char sig[4],unsigned index) { (void)sig;(void)index;return firmware_declared ? &firmware_declared : NULL; }
uint16_t pic_mask_all(void) { uint16_t old = pic_masks; pic_masks = UINT16_MAX; return old; }
void pic_restore_mask(uint16_t masks) { pic_masks = masks; }
void ioapic_mask_all(void) { pins_masked = true; }
bool ioapic_remap(void) { routes++; CHECK(pic_masks == UINT16_MAX); if (route_ok || !hw) pins_masked = false; return route_ok || !hw; }

static uint64_t host_table(uint64_t p,unsigned level) { (void)level; return p | IOMMU_READ | IOMMU_WRITE; }
static uint64_t host_leaf(uint64_t p,unsigned level,unsigned access) { return p | access | (level > 1 ? 1ull << 7 : 0); }
static bool host_is_leaf(uint64_t e,unsigned level) { return level == 1 || (e & (1ull << 7)); }
static bool host_flush(struct iommu_domain *dom,bool unmapped) { (void)dom;(void)unmapped;flushes++;return true; }
static bool host_power_ok(void) { return true; }
static bool host_power_fail(void) { return false; }
static unsigned shutdown_calls;
static bool host_shutdown_ok(void) { shutdown_calls++; return true; }
static bool host_shutdown_fail(void) { shutdown_calls++; return false; }
static bool host_attach(struct iommu_domain *dom) { (void)dom; return true; }
static bool host_block(struct iommu_domain *dom) { (void)dom; return true; }
static bool host_start_unused(bool translate) { (void)translate; return false; }
static bool host_start_route(bool translate) {
    (void)translate; starts++; CHECK(pic_masks == UINT16_MAX && pins_masked); return start_ok;
}
static bool host_stop_route(void) { stops++; CHECK(pic_masks == UINT16_MAX); return true; }
static struct iommu_hw backend = {.coherent=true,.levels=4,.iova_bits=39,.phys_bits=48,.domains=UINT16_MAX,
    .table=host_table,.leaf=host_leaf,.is_leaf=host_is_leaf,.flush=host_flush,
    .resume=host_power_ok,.shutdown=host_power_ok};

static void test_addresses(void) {
    hw = &backend;
    struct iommu_domain *dom = domain_alloc(2,4,1);
    CHECK(dom && iommu_dma_limit(dom) == (1ull << 39));
    CHECK(range_fits(UINT64_MAX - PAGE_SIZE + 1,1,64));
    CHECK(!range_fits(UINT64_MAX - PAGE_SIZE + 1,2,64));
    CHECK(!range_fits(1,1,48) && !range_fits(0,1,0));
    CHECK(!iommu_map(dom,NULL,1ull << 39,1) && live_pages == 1);
    uint64_t frames[] = {PAGE_SIZE,1ull << 39};
    CHECK(!iommu_map(dom,frames,0,ARRAY_SIZE(frames)) && live_pages == 1);
    CHECK(iommu_unmap(dom,frames,0,ARRAY_SIZE(frames)) && !flushes);
    CHECK(iommu_map(dom,NULL,(1ull << 39) - PAGE_SIZE,1));
    CHECK(iommu_unmap(dom,NULL,(1ull << 39) - PAGE_SIZE,1));
    domain_release(dom); CHECK(!live_pages);
    backend.phys_bits = 12;
    CHECK(!table_new() && !live_pages); /* allocatorの物理上位bitを切捨てない */
    backend.phys_bits = 48;
    dom = domain_alloc(2,3,3);
    CHECK(dom && iommu_map(dom,NULL,0,(1ull << 30) / PAGE_SIZE));
    CHECK(!entry(dom,PAGE_SIZE,1,true));
    size_t before = frees;
    domain_release(dom);
    CHECK(frees == before + 1 && !live_pages); /* 1GiB leafをtableとして辿らない */
}

static void test_reservations(void) {
    struct pci_device device = {0}; uint16_t id = 1;
    struct iommu_domain *dom = domain_alloc(2,4,1);
    dom->d = &device; dom->ids = malloc(sizeof id); *dom->ids = id; dom->nids = 1;
    iommu_reserved(id,id,PAGE_SIZE,2 * PAGE_SIZE,0);
    iommu_reserved(id,id,2 * PAGE_SIZE,3 * PAGE_SIZE,IOMMU_READ);
    iommu_reserved(id,id,3 * PAGE_SIZE,4 * PAGE_SIZE,IOMMU_READ | IOMMU_WRITE);
    iommu_reserved(id,id,2 * PAGE_SIZE,3 * PAGE_SIZE,IOMMU_READ | IOMMU_WRITE);
    CHECK(nreserved == 3 && reserved[1].access == IOMMU_READ);
    CHECK(!reserved_map(dom));
    CHECK(!iommu_map(dom,NULL,PAGE_SIZE,1));
    CHECK(!iommu_map(dom,NULL,2 * PAGE_SIZE,1));
    CHECK(iommu_unmap(dom,NULL,2 * PAGE_SIZE,1));
    CHECK(*entry(dom,2 * PAGE_SIZE,1,false) == host_leaf(2 * PAGE_SIZE,1,IOMMU_READ));
    CHECK(!iommu_map(dom,NULL,3 * PAGE_SIZE,1));
    CHECK(iommu_unmap(dom,NULL,3 * PAGE_SIZE,1));
    CHECK(*entry(dom,3 * PAGE_SIZE,1,false) == host_leaf(3 * PAGE_SIZE,1,IOMMU_READ | IOMMU_WRITE));
    domain_release(dom); CHECK(!live_pages);
    free(reserved); reserved = NULL; nreserved = reserved_room = 0;
    dom = domain_alloc(2,6,1); dom->d = &device;
    dom->ids = malloc(2 * sizeof id); dom->ids[0] = id; dom->ids[1] = id + 1; dom->nids = 2;
    iommu_reserved(id,id,PAGE_SIZE,4 * PAGE_SIZE,IOMMU_READ | IOMMU_WRITE);
    iommu_reserved(id + 1,id + 1,2 * PAGE_SIZE,3 * PAGE_SIZE,IOMMU_READ);
    iommu_reserved(id,id,3 * PAGE_SIZE,4 * PAGE_SIZE,0);
    struct pmm_range os = {3 * PAGE_SIZE,4 * PAGE_SIZE};
    ram_ranges = &os; ram_count = 1;
    CHECK(!reserved_map(dom)); /* 最終NONE部分のOS RAMはmappingしない */
    CHECK(*entry(dom,2 * PAGE_SIZE,1,false) == host_leaf(2 * PAGE_SIZE,1,IOMMU_READ));
    CHECK(!*entry(dom,3 * PAGE_SIZE,1,false));
    os.base = 2 * PAGE_SIZE;
    CHECK(reserved_map(dom) == -N_ERR_UNSUPPORTED); /* ROでもOS RAMへの恒久到達は不可 */
    ram_count = 0; ram_complete = false;
    CHECK(reserved_map(dom) == -N_ERR_UNSUPPORTED);
    ram_complete = true; ram_ranges = NULL;
    domain_release(dom); CHECK(!live_pages);
    free(reserved); reserved = NULL; nreserved = reserved_room = 0;
}

static void test_policy_and_power(void) {
    struct pci_device device = {0}; struct fns group = {0}; struct iommu_domain *out;
    opt_in = false; hw = NULL;
    CHECK(iommu_attach(&device,&out) == -N_ERR_UNSUPPORTED && !out);
    opt_in = true; CHECK(!iommu_attach(&device,&out) && !out);
    opt_in = false; CHECK(unisolated(&device,&group,-N_ERR_BUSY) == -N_ERR_BUSY);
    opt_in = true; CHECK(!unisolated(&device,&group,-N_ERR_BUSY));
    device.iommu_blocked = true;
    CHECK(unisolated(&device,&group,-N_ERR_BUSY) == -N_ERR_BUSY);
    device.iommu_blocked = false; hw = &backend; backend.translating = true;
    iommu_cover(0,UINT16_MAX); no_memory = true; opt_in = false;
    CHECK(iommu_attach(&device,&out) == -N_ERR_NO_MEMORY && !out);
    CHECK(!(domain_used[2 / 8] & (1u << (2 % 8))));
    no_memory = false; free(covered); covered = NULL; ncovered = covered_room = 0;
    hw = &backend; backend.resume = host_power_fail;
    CHECK(!iommu_resume() && !iommu_active());
    CHECK(iommu_attach(&device,&out) == -N_ERR_IO); /* opt-inでも失敗を迂回できない */
    backend.resume = host_power_ok;
    CHECK(!iommu_resume()); /* 後の成功で不確定な遷移を忘れない */
    restore_failed = false; backend.shutdown = host_power_fail;
    CHECK(!iommu_shutdown());
    restore_failed = false; backend.start = host_start_unused; probe_backend = &backend;
    iommu_init();
    CHECK(restore_failed && !hw && !iommu_shutdown()); /* 初期化失敗後のlive table上書きを拒否 */
    uint32_t address=0x12345678, data=0xABCDEF01; uint64_t rte=0x123456789ABCDEF0ull;
    CHECK(!iommu_msi_message(&device,0,1,0x50,0,&address,&data));
    CHECK(address == 0x12345678 && data == 0xABCDEF01);
    CHECK(!iommu_msi_clear(&device));
    CHECK(!iommu_ioapic_entry(0,0,&rte) && rte == 0x123456789ABCDEF0ull);
    restore_failed = false; backend.shutdown = host_power_ok;
    iommu_init();
    CHECK(!restore_failed && !hw && iommu_shutdown()); /* 全停止を確認した未使用とは区別 */
    CHECK(iommu_msi_message(&device,0,1,0x50,0,&address,&data) && data == 0x50);
    CHECK(iommu_msi_clear(&device) && iommu_ioapic_entry(0,0,&rte));
    CHECK(iommu_power_ready());
    backend.interrupt_remapping_required=true;
    iommu_init();
    CHECK(restore_failed && !hw && !iommu_power_ready());
    CHECK(!iommu_msi_message(&device,0,1,0x50,0,&address,&data) && !iommu_ioapic_entry(0,0,&rte));
    CHECK(pins_masked); /* IRREQ: stopped is not evidence that plain delivery works */
    backend.interrupt_remapping_required=false; restore_failed=false;
    probe_backend = NULL; firmware_declared = true;
    iommu_init();
    CHECK(!iommu_power_ready() && !iommu_shutdown()); /* probe NULLでも既知hardwareを無かったことにしない */
    restore_failed = false; probe_plain = true;
    iommu_init();
    CHECK(iommu_power_ready() && !hw); /* 表の拒否と全unit停止の独立確認を区別 */
    address=0x12345678; data=0xABCDEF01; rte=0x123456789ABCDEF0ull;
    CHECK(iommu_msi_message(&device,0,1,0x50,0,&address,&data) && data == 0x50);
    CHECK(iommu_ioapic_entry(0,0,&rte) && rte == 0x123456789ABCDEF0ull);
    CHECK(iommu_msi_clear(&device));
    intel_probe_plain = false;
    iommu_init();
    CHECK(!iommu_power_ready()); /* 一方がoffでも他方のunknown/IRREQを迂回しない */
    address=0x12345678; data=0xABCDEF01;
    CHECK(!iommu_msi_message(&device,0,1,0x50,0,&address,&data));
    CHECK(address == 0x12345678 && data == 0xABCDEF01);
    intel_probe_plain = true;
    probe_plain = false;
    firmware_declared = false; restore_failed = false;
    iommu_init();
    CHECK(iommu_power_ready() && iommu_shutdown()); /* 本当にIOMMU不在ならPMを不必要に止めない */
    CHECK(iommu_expired(iommu_deadline(100))); /* TSC周波数未知を10GHz等と捏造しない */
}

static void test_interrupt_transition(void) {
    probe_backend = &backend; backend.start = host_start_route; backend.shutdown = host_stop_route;
    pic_masks = 0xA55A; routes = starts = stops = 0;
    route_ok = start_ok = true; backend.translating = true;
    iommu_init();
    CHECK(hw == &backend && pic_masks == 0xA55A && !pins_masked);
    CHECK(starts == 1 && !stops && routes == 1);
    route_ok = false;
    iommu_init();
    CHECK(!hw && stops == 1 && routes == 3 && pic_masks == 0xA55A && !pins_masked);
    CHECK(!restore_failed); /* conversion failure stops units, then restores plain ExtINT */
    probe_backend = NULL;
}

/* x2APICを離れる理由はどちらの後端からでも: 片方の機械には片方の表しかない。 */
static void test_x2apic_conflict(void) {
    const char *amd = "amd", *intel = "intel";
    const struct { const char *amd, *intel, *why; } cases[] = {
        {NULL, NULL, NULL}, {amd, NULL, amd}, {NULL, intel, intel}, {amd, intel, amd}
    };
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        amd_conflict = cases[i].amd; intel_conflict = cases[i].intel;
        CHECK(iommu_x2apic_conflict() == cases[i].why);
    }
    amd_conflict = intel_conflict = NULL;
}

int main(void) {
    test_addresses(); test_reservations(); test_policy_and_power(); test_interrupt_transition(); test_x2apic_conflict();
    printf("共通IOMMU focused検査: %u件、失敗%u件（実機・QEMU未検証）\n",checks,failed);
    return failed ? 1 : 0;
}
