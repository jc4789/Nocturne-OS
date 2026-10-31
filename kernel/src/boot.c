/* Nocturne OS kernel entry point. */
#include "kernel.h"
#include "limine.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/fpu.h"
#include "arch/smp.h"
#include "vm/vm.h"
#include "arch/topology.h"
#include "arch/ioapic.h"
#include "dev/acpi.h"
#include "dev/serial.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "sys/address_wait.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/log.h"
#include "dev/input.h"
#include "mm/bootmem.h"
#include "mm/pmm.h"
#include "mm/vmalloc.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sys/sched.h"
#include "sys/thread.h"
#include "sys/epoch.h"
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

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

uint64_t hhdm_offset;
const char *kernel_cmdline = "";

void kmain_late(void);

bool cmdline_has(const char *word) {
    size_t n = strlen(word);
    for (const char *p = kernel_cmdline; *p; p++) {
        if ((p == kernel_cmdline || p[-1] == ' ') && !strncmp(p, word, n) && (p[n] == 0 || p[n] == ' '))
            return true;
    }
    return false;
}

bool cmdline_number(const char *key, uint64_t *out) {
    size_t n = strlen(key);
    bool found = false;
    for (const char *p = kernel_cmdline; *p; p++) {
        if ((p != kernel_cmdline && p[-1] != ' ') || strncmp(p, key, n) || p[n] != '=') continue;
        const char *q = p + n + 1;
        uint64_t value = 0;
        bool valid = *q >= '0' && *q <= '9';
        while (*q >= '0' && *q <= '9') {
            unsigned digit = *q++ - '0';
            if (value > (UINT64_MAX - digit) / 10) valid = false;
            if (valid) value = value * 10 + digit;
        }
        if (valid && (!*q || *q == ' ')) { *out = value; found = true; }
    }
    return found;
}

/* CPU 0 from here on its idle thread's own stack (sys/sched.h), as each other CPU from ap_main. */
static NORETURN void kmain_scheduled(void *unused) {
    (void)unused;
    alarm_init_cpu(); /* CPU 0's wake device: no periodic tick on any CPU */
    log_start();      /* the log's readers: the boot screen's thread, the serial port's interrupt */
    prof_init();      /* before the other CPUs: each starts its samples as it comes up */
    smp_init();
    sti();
    input_init();

    kmain_late();
    sched_idle(); /* the boot context is CPU 0's idle thread */
}

static const char *memtype(uint64_t t) {
    static const char *n[] = {"usable", "reserved", "ACPI reclaim", "ACPI NVS", "bad", "bootloader", "kernel+modules", "framebuffer", "reserved-mapped"};
    return t < ARRAY_SIZE(n) ? n[t] : "?";
}

void kmain(void) {
    percpu_load(0); /* the first CPU's per-CPU variables: the template, until percpu_init */
    serial_init();
    kprintf("\n" OS_NAME " " OS_VERSION " booting...\n");
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) kprintf("warning: Limine base revision not supported\n");
    if (!hhdm_req.response || !mm_req.response || !ka_req.response) {
        kprintf("missing essential Limine responses, halting\n");
        for (;;) hlt();
    }
    hhdm_offset = hhdm_req.response->offset;
    if (cmdline_req.response && cmdline_req.response->cmdline) kernel_cmdline = cmdline_req.response->cmdline;

    /* The CPU: what it has, then its state as every other CPU's (arch/cpu.h), the memory types
       among it before any page table is built (vmm.c maps MMIO as PAT entry 3, UC, and the
       framebuffer as 5, WC). The IDT first: cpu_setup loads it, and a refused MSR write
       (wrmsr_checked) comes back through it. */
    cpu_features_init();
    fpu_init();
    idt_init();
    if (!cpu_setup(0)) panic("boot: the first CPU cannot be set up");

    /* the machine's tables, in boot memory: what the page allocator is built on */
    struct limine_memmap_response *mm = mm_req.response;
    bootmem_init(mm);
    acpi_tables_init(mm);
    topology_init();
    bootmem_by_node();
    percpu_init();
    pmm_init(mm);
    /* the heap before the kernel's addresses: vmalloc's records are kmalloc's, and the entry
       stacks and the devices' registers are mapped in vmalloc's range */
    heap_init();
    vmm_init(mm, ka_req.response);
    vmalloc_init();
    if (!entry_stacks_init()) panic("boot: no memory for the interrupt stacks");
    log_init(); /* every CPU's ring of the log, sized from the memory and the CPUs */
    wake_init(); /* the real-mode page the other CPUs start through and S3 wakes into */

    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        fb_init(fb_req.response->framebuffers[0]);
        fbcon_init();
    }
    vm_detect(); /* before the clock: a hypervisor that states the TSC's rate registers it */
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
        if (e->length >= 1024 * 1024)
            kprintf("  mem %016lx - %016lx  %s\n", e->base, e->base + e->length, memtype(e->type));
    }
    kprintf("memory: %lu MiB usable, %lu MiB free\n", pmm_total_pages() * 4096 / 1048576,
            pmm_free_pages() * 4096 / 1048576);

    /* The interrupt controllers, every source masked: the 8259, the IOAPICs' pins, then this
       CPU's APIC; the machine-check thresholds' entry in it once it is enabled (an APIC
       software-disabled holds every entry masked, APM vol. 2 16.4.7). */
    irq_init();
    cpu_mca_interrupt_on();
    clock_init(); /* the TSC at the P0 P-state's rate (measured on against the HPET) or a hypervisor's */
    sched_init();
    address_wait_init();
    epoch_init(); /* its workers, which free what readers might still see */
    cpu_switch_stack(sched_cpu_stack(0), kmain_scheduled, NULL); /* the bootloader's is left */
}
