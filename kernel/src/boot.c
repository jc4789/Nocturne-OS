/* Nocturne OS kernel entry point. */
#include "kernel.h"
#include "limine.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "vm/vm.h"
#include "arch/pat.h"
#include "arch/ioapic.h"
#include "dev/acpi.h"
#include "dev/serial.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "dev/input.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sys/sched.h"
#include "arch/pstate.h"
#include "sys/prof.h"
#include "arch/wake.h"

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(4);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request mm_req = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request ka_req = {.id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_executable_file_request exe_file_req = {.id = LIMINE_EXECUTABLE_FILE_REQUEST_ID, .revision = 0};

/* the volume C: when the system started from a CD or DVD (fs/store.c) */
__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request module_req = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request rsdp_req = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_req = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_mp_request mp_req = {.id = LIMINE_MP_REQUEST_ID, .revision = 0, .flags = LIMINE_MP_REQUEST_X86_64_X2APIC};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

uint64_t hhdm_offset;
const char *kernel_cmdline = "";

void apic_init(const struct limine_mp_response *mp);
void kmain_late(void);

static const char *memtype(uint64_t t) {
    static const char *n[] = {"usable", "reserved", "ACPI reclaim", "ACPI NVS", "bad", "bootloader", "kernel+modules", "framebuffer", "reserved-mapped"};
    return t < ARRAY_SIZE(n) ? n[t] : "?";
}

void kmain(void) {
    serial_init();
    kprintf("\n" OS_NAME " " OS_VERSION " booting...\n");
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) kprintf("warning: Limine base revision not supported\n");
    if (!hhdm_req.response || !mm_req.response || !ka_req.response) {
        kprintf("missing essential Limine responses, halting\n");
        for (;;) hlt();
    }
    hhdm_offset = hhdm_req.response->offset;
    if (cmdline_req.response && cmdline_req.response->cmdline) kernel_cmdline = cmdline_req.response->cmdline;

    cpu_features_init();
    cpu_setup();
    gdt_init();
    idt_init();
    fpu_init();
    /* IA32_PAT before any page table is built: vmm.c maps MMIO as index 3 (UC), the framebuffer as 5 (WC) */
    uint64_t pat = pat_init_cpu();
    if (pat != PAT_NOCTURNE) kprintf("cpu: PAT %016lx (unexpected)\n", pat);

    struct limine_memmap_response *mm = mm_req.response;
    pmm_init(mm);
    vmm_init(mm, ka_req.response);
    wake_init(); /* the real-mode page for waking from sleep and restarting the APs */

    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        fb_init(fb_req.response->framebuffers[0]);
        fbcon_init();
    }
    vm_detect();
    kprintf("video: %ux%u, %u bpp, %u bpc (R%u@%u G%u@%u B%u@%u)%s, pitch %u\n", fb.width, fb.height,
            fb.bpp, fb.bpc, fb.rsize, fb.rshift, fb.gsize, fb.gshift, fb.bsize, fb.bshift,
            fb.simulated ? " simulated by fbsim10" : "", fb.pitch);
    if (!fb.monitor[0]) kprintf("video: the firmware passed no EDID of the monitor\n");
    else if (fb.refresh_mhz)
        kprintf("video: monitor %s%s%s, at %u.%03u Hz\n", fb.monitor, fb.monitor_name[0] ? ", " : "",
                fb.monitor_name, fb.refresh_mhz / 1000, fb.refresh_mhz % 1000);
    else
        kprintf("video: monitor %s%s%s; none of its timings is %ux%u\n", fb.monitor, fb.monitor_name[0] ? ", " : "",
                fb.monitor_name, fb.width, fb.height);
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        serial_write("", 0);
        if (e->length >= 1024 * 1024)
            kprintf("  mem %016lx - %016lx  %s\n", e->base, e->base + e->length, memtype(e->type));
    }
    kprintf("memory: %lu MiB usable, %lu MiB free\n", pmm_total_pages() * 4096 / 1048576,
            pmm_free_pages() * 4096 / 1048576);

    pic_init();
    /* The IOAPICs' pins masked (but a firmware virtual-wire PIC path, the PIC masked above) before
       the APICs change mode: apic_init may leave x2APIC mode and the APs take the BSP's, through
       the disabled state, where no interrupt may be sent to the APIC (SDM 11.4.3). */
    acpi_tables_init();
    clock_init(); /* the hypervisor's reference time, or the TSC by the PM timer's measure */
    ioapic_init();
    apic_init(mp_req.response);
    sched_init();
    alarm_init_cpu(); /* CPU 0's wake device: no periodic tick on any CPU */
    prof_init();      /* before the APs: each starts its samples as it comes up */
    pstate_init_bsp();
    smp_init(mp_req.response);
    sti();
    input_init();

    kmain_late();
    sched_idle(); /* the boot context is CPU 0's idle task */
}
