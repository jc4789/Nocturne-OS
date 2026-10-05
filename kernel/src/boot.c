/* Nocturne OS kernel entry point. */
#include "kernel.h"
#include "limine.h"
#include "arch/cpu.h"
#include "dev/serial.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "dev/timer.h"
#include "dev/input.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sys/sched.h"

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
volatile struct limine_module_request module_req = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request rsdp_req = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_req = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

uint64_t hhdm_offset;
const char *kernel_cmdline = "";
char cpu_brand[64];

void apic_init(void);
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

    gdt_init();
    idt_init();
    fpu_init();
    cpu_get_brand(cpu_brand);

    struct limine_memmap_response *mm = mm_req.response;
    pmm_init(mm);
    vmm_init(mm, ka_req.response);

    if (fb_req.response && fb_req.response->framebuffer_count > 0) {
        fb_init(fb_req.response->framebuffers[0]);
        fbcon_init();
    }
    kprintf("cpu: %s\n", cpu_brand);
    kprintf("video: %ux%u, %u bpp, pitch %u\n", fb.width, fb.height, fb.bpp, fb.pitch);
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        serial_write("", 0);
        if (e->length >= 1024 * 1024)
            kprintf("  mem %016lx - %016lx  %s\n", e->base, e->base + e->length, memtype(e->type));
    }
    kprintf("memory: %lu MiB usable, %lu MiB free\n", pmm_total_pages() * 4096 / 1048576,
            pmm_free_pages() * 4096 / 1048576);

    pic_init();
    apic_init();
    sched_init();
    timer_init();
    sti();
    input_init();

    kmain_late();
    /* become the idle task */
    for (;;) {
        sti();
        hlt();
        schedule();
    }
}
