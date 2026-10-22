/* 実IOAPIC/PMコードの割込み移行境界。単一pinのIOWIN最終値と呼出順を検査する。
   register bank/実edge/CPU割込みのモデルではなく、無損失配送は証明しない。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <setjmp.h>
#include <string.h>
#include "kernel.h"
#include "arch/cpu.h"

static uint64_t host_irq_save(void) { return 0; }
static void host_irq_restore(uint64_t f) { (void)f; }
static uint8_t host_inb(uint16_t port);
static void host_outb(uint16_t port,uint8_t value);
static void host_io_wait(void) {}
#define irq_save host_irq_save
#define irq_restore host_irq_restore
#define inb host_inb
#define outb host_outb
#define io_wait host_io_wait
#include "../kernel/src/arch/idt.c"
#include "../kernel/src/arch/ioapic.c"
#include "../kernel/src/dev/pm.c"
#undef irq_save
#undef irq_restore
#undef inb
#undef outb
#undef io_wait

static unsigned checks, failures, remap_calls, resume_calls, pci_resumes, pic_initializations;
static uint32_t window[5];
static uint16_t pic_masks;
static uint16_t pending_edges;
static unsigned pic_phase[2];
static const uint16_t original_masks = 0xA55A;
static bool route_ok, slept_model, resume_ok, panic_expected;
static const char *suspend_error;
static jmp_buf panic_return;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("失敗 %d行: %s\n",__LINE__,#c); } } while (0)

int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int ksnprintf(char *b,size_t n,const char *fmt,...) {
    va_list ap; va_start(ap,fmt); int r=vsnprintf(b,n,fmt,ap); va_end(ap); return r;
}
NORETURN void panic(const char *fmt,...) {
    (void)fmt;
    if (!panic_expected) abort();
    longjmp(panic_return,1);
}
bool iommu_ioapic_entry(uint8_t id,uint32_t gsi,uint64_t *rte) {
    (void)id; (void)gsi; (void)rte; remap_calls++; return route_ok;
}
static uint8_t host_inb(uint16_t port) {
    if (port == 0x21) return (uint8_t)pic_masks;
    if (port == 0xA1) return (uint8_t)(pic_masks >> 8);
    return 0;
}
static void host_outb(uint16_t port,uint8_t value) {
    unsigned bank = port == 0xA0 || port == 0xA1;
    if ((port == 0x20 || port == 0xA0) && (value & 0x10)) {
        pic_phase[bank]=3; /* ICW2/3/4 follow ICW1 */
        pic_initializations++;
        pending_edges &= bank ? 0x00FF : 0xFF00;
    } else if (port == 0x21 || port == 0xA1) {
        if (pic_phase[bank]) { pic_phase[bank]--; return; }
        if (value != UINT8_MAX) {
            CHECK(!slept_model || resume_calls == 1);
            CHECK(!(window[4] & RTE_MASK));
        }
        if (bank) pic_masks=(pic_masks & 0xFF) | (uint16_t)value << 8;
        else pic_masks=(pic_masks & 0xFF00) | value;
    }
}
void acpi_freeze(bool on) { (void)on; }
const char *pci_suspend(void) { return suspend_error; }
void timer_suspend(void) {}
bool wake_sleep(void (*enter)(void)) {
    (void)enter;
    CHECK(pic_masks == UINT16_MAX && (window[4] & RTE_MASK));
    if (slept_model) { /* wake firmware may have re-enabled plain routes */
        pic_masks=original_masks; window[4]=DELIV_EXTINT << 8;
        pending_edges=0;
    }
    return slept_model;
}
void lapic_resume(void) { CHECK(pic_masks == UINT16_MAX && (window[4] & RTE_MASK)); }
void serial_init(void) {}
bool iommu_resume(void) {
    CHECK(pic_masks == UINT16_MAX && (window[4] & RTE_MASK));
    resume_calls++; return resume_ok;
}
void hv_reference_resume(void) {}
void hv_core_resume(void) {}
int64_t timer_resume(void) { return 0; }
void sched_clock_resume(void) {}
void acpi_wake_prepare(unsigned state) { (void)state; }
void pci_resume(void) {
    CHECK(pic_masks == original_masks && !(window[4] & RTE_MASK));
    pci_resumes++;
}
void acpi_wake(unsigned state) { (void)state; }
void acpi_links_forget(void) {}
void wm_notify(const char *title,const char *text) { (void)title;(void)text; }

/* ここから下は idt.c/pm.c のうち本試験が到達しない経路（割込み入口、sleep開始、shutdown、
   ACPI/IOAPIC初期化）が参照する境界。PEリンカーは未解決を許さないため定義するだけで、
   挙動は模擬しない。万一到達したら黙って成功せず異常終了する。 */
uint64_t hhdm_offset, isr_stub_table[256];
volatile uint64_t timer_ticks;
volatile bool tick_stopped;
static NORETURN void unreached(const char *name) {
    fprintf(stderr,"試験が到達しない境界関数に到達: %s\n",name); abort();
}
size_t strlcpy(char *d,const char *s,size_t n) {
    size_t len=strlen(s);
    if (n) { size_t keep=len < n-1 ? len : n-1; memcpy(d,s,keep); d[keep]=0; }
    return len;
}
const struct acpi_ioapic *acpi_ioapics(unsigned *count) { (void)count; unreached(__func__); }
const struct acpi_iso *acpi_isos(unsigned *count) { (void)count; unreached(__func__); }
void acpi_links_save(void) { unreached(__func__); }
bool acpi_sleep_enter(unsigned state) { (void)state; unreached(__func__); }
bool acpi_sleep_prepare(unsigned state) { (void)state; unreached(__func__); }
bool acpi_sleep_supported(unsigned state) { (void)state; unreached(__func__); }
int blk_count(void) { unreached(__func__); }
struct blkdev *blk_get(int i) { (void)i; unreached(__func__); }
void blk_hold(void) { unreached(__func__); }
void blk_release(void) { unreached(__func__); }
bool cpu_is_runner(void) { unreached(__func__); }
bool cpu_is_worker(void) { unreached(__func__); }
void cpu_require_bsp(void) { unreached(__func__); }
void cpu_worker_fault(struct regs *regs) { (void)regs; unreached(__func__); }
bool smp_suspend(void) { unreached(__func__); }
void smp_resume(void) { unreached(__func__); }
void entropy_add(uint64_t v) { (void)v; unreached(__func__); }
void lapic_eoi(void) { unreached(__func__); }
const char *hibernate_blocker(void) { unreached(__func__); }
bool hibernate_prepare(const char **why) { (void)why; unreached(__func__); }
void hibernate_snapshot(void) { unreached(__func__); }
const char *hibernate_write(void) { unreached(__func__); }
void hibernate_cleanup(bool restored) { (void)restored; unreached(__func__); }
bool iommu_power_ready(void) { unreached(__func__); }
struct task *kthread_create(const char *name,void (*fn)(void *),void *arg) {
    (void)name; (void)fn; (void)arg; unreached(__func__);
}
void wq_wait(struct wait_queue *wq) { (void)wq; unreached(__func__); }
void wq_wake_all(struct wait_queue *wq) { (void)wq; unreached(__func__); }
void sleep_ms(uint64_t ms) { (void)ms; unreached(__func__); }
int task_stop_apps(void) { unreached(__func__); }
void task_exit_app(int code) { (void)code; unreached(__func__); }
void task_printf_stderr(struct task *t,const char *fmt,...) { (void)t; (void)fmt; unreached(__func__); }
void sched_ap_interrupt(struct regs *r) { (void)r; unreached(__func__); }
void sched_user_return(struct regs *r) { (void)r; unreached(__func__); }
void syscall_dispatch(struct regs *r) { (void)r; unreached(__func__); }
void panic_regs(const char *what,struct regs *r) { (void)what; (void)r; unreached(__func__); }
void power_off(void) { unreached(__func__); }
void power_reboot(void) { unreached(__func__); }
void timer_wake(void) { unreached(__func__); }
bool vmm_handle_user_fault(uint64_t addr,uint64_t err) { (void)addr; (void)err; unreached(__func__); }
void *vmm_map_mmio(uint64_t phys,uint64_t size) { (void)phys; (void)size; unreached(__func__); }
void vmm_tlb_ipi(void) { unreached(__func__); }
uint64_t vmm_translate(uint64_t pml4,uint64_t va) { (void)pml4; (void)va; unreached(__func__); }
bool wake_available(void) { unreached(__func__); }
int wm_window_count(char *top_title,size_t n) { (void)top_title; (void)n; unreached(__func__); }
void wm_close_windows(void) { unreached(__func__); }
void wm_repaint(void) { unreached(__func__); }
int pci_count(void) { unreached(__func__); }
struct pci_device *pci_at(int i) { (void)i; unreached(__func__); }

static void reset_model(void) {
    memset(ios,0,sizeof ios); memset(window,0,sizeof window); memset(stages,0,sizeof stages);
    nios=1; ios[0].regs=window; ios[0].pins=1;
    given[0][0]=window[4]=DELIV_EXTINT << 8;
    route_ok=resume_ok=true; panic_expected=false; suspend_error=NULL;
    remap_calls=resume_calls=pci_resumes=pic_initializations=0; pic_masks=original_masks;
    memset(pic_phase,0,sizeof pic_phase); pending_edges=1;
}

static void test_routes(void) {
    reset_model(); ioapic_mask_all(); route_ok=false;
    CHECK(!ioapic_remap() && (window[4] & RTE_MASK));
    CHECK(!(given[0][0] & RTE_MASK)); /* original request remains for stopped-unit rollback */
    route_ok=true;
    CHECK(ioapic_remap() && !(window[4] & RTE_MASK));
    route_ok=false;
    CHECK(!ioapic_route(0,0x30,0,false,false));
    CHECK(given[0][0] & RTE_MASK); /* failed new route must not revive a caller's retired vector */
    unsigned before=remap_calls; route_ok=true;
    CHECK(ioapic_remap() && remap_calls == before);
    reset_model(); ioapic_suspend();
    CHECK((window[4] & RTE_MASK) && !(saved_rte[0][0] & RTE_MASK));
    ioapic_resume(); CHECK(!(window[4] & RTE_MASK));
}

static void no_enter(void) {}
static void test_sleep(void) {
    for (unsigned mode=0;mode<3;mode++) {
        reset_model(); slept_model=mode == 0;
        if (mode == 2) suspend_error="PCI config save";
        const char *why=NULL;
        CHECK(machine_sleep(no_enter,3,&why) == slept_model);
        CHECK(resume_calls == (unsigned)slept_model && pci_resumes == 1);
        CHECK(pic_initializations == (slept_model ? 2u : 0u));
        CHECK(slept_model || pending_edges == 1); /* 中止sleepはICW1を書かず保留edgeを維持 */
        CHECK(mode != 2 || (why && strstr(why,"PCI config save")));
    }
    reset_model(); slept_model=true; resume_ok=false; panic_expected=true;
    if (!setjmp(panic_return)) {
        const char *why=NULL;
        machine_sleep(no_enter,3,&why);
        CHECK(false);
    }
    CHECK(resume_calls == 1 && !pci_resumes && !pic_initializations);
    CHECK(pic_masks == UINT16_MAX && (window[4] & RTE_MASK));
}

int main(void) {
    test_routes(); test_sleep();
    printf("IOMMU/電源割込み移行: %u判定、失敗%u（実機配送未検証）\n",checks,failures);
    return failures ? 1 : 0;
}
