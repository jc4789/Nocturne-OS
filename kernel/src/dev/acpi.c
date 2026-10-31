/* The ACPI tables (dev/acpi.h): the root table and what it lists, read in place. Formats from
   ACPI 6.5 chapter 5. The AML is uACPI's: the second half of this file gives it the kernel
   services it asks for (uacpi/kernel_api.h). */
#include "kernel.h"
#include "limine.h"
#include "arch/cpu.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/bootmem.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sys/clock.h"
#include "sys/sched.h"
#include "sys/thread.h"
#include "sys/wait.h"
#include <uacpi/kernel_api.h>
#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include "arch/wake.h"

_Static_assert(sizeof(struct acpi_madt_lapic) == 8 && sizeof(struct acpi_madt_ioapic) == 12 &&
               sizeof(struct acpi_madt_override) == 10 && sizeof(struct acpi_madt_x2apic) == 16 &&
               sizeof(struct acpi_madt_nmi_source) == 8 && sizeof(struct acpi_madt_lapic_nmi) == 6 &&
               sizeof(struct acpi_madt_x2apic_nmi) == 12,
               "the MADT entries' formats");
_Static_assert(sizeof(struct acpi_srat_apic) == 16 && sizeof(struct acpi_srat_memory) == 40 &&
               sizeof(struct acpi_srat_x2apic) == 24 && sizeof(struct acpi_slit) == 44 &&
               sizeof(struct acpi_mcfg_entry) == 16, "the SRAT's, SLIT's and MCFG's formats");

extern volatile struct limine_rsdp_request rsdp_req;

static struct limine_memmap_response *map;
/* The root table (XSDT, else RSDT): its entries are the registry, each a physical address of
   entry_size bytes; valid[i] says the i-th passed its checks. */
static const uint8_t *entries;
static uint32_t nentries, entry_size;
static uint64_t *valid;

static bool checksum_ok(const void *p, size_t n) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += b[i];
    return sum == 0;
}

/* The bootloader maps the tables within these ranges, before the kernel's page tables and after
   (base revision 4; vmm_init maps every range of the map). */
static bool where_tables_lie(uint64_t base, uint64_t length) {
    for (uint64_t i = 0; i < map->entry_count; i++) {
        const struct limine_memmap_entry *e = map->entries[i];
        if (e->type != LIMINE_MEMMAP_ACPI_RECLAIMABLE && e->type != LIMINE_MEMMAP_ACPI_NVS &&
            e->type != LIMINE_MEMMAP_RESERVED_MAPPED)
            continue;
        if (base >= e->base && length <= e->length && base - e->base <= e->length - length) return true;
    }
    return false;
}

const struct acpi_sdt *acpi_table_at(uint64_t phys) {
    if (!phys || !where_tables_lie(phys, sizeof(struct acpi_sdt))) return NULL;
    const struct acpi_sdt *t = phys_to_virt(phys);
    return t->length >= sizeof *t && where_tables_lie(phys, t->length) ? t : NULL;
}

static uint64_t entry_phys(uint32_t i) {
    uint64_t p = 0;
    memcpy(&p, entries + (size_t)i * entry_size, entry_size);
    return p;
}

/* The root the RSDP names (5.2.5): the XSDT when there is one that checks, else the RSDT. */
static const struct acpi_sdt *root(void) {
    if (!rsdp_req.response) return NULL;
    /* base revision 4 gives the RSDP's address in the direct map */
    const uint8_t *rsdp = (const uint8_t *)rsdp_req.response->address;
    if (memcmp(rsdp, "RSD PTR ", 8) || !checksum_ok(rsdp, 20)) {
        kprintf("acpi: RSDP signature or checksum invalid\n");
        return NULL;
    }
    uint32_t rsdt = 0, length = 0;
    uint64_t xsdt = 0;
    memcpy(&rsdt, rsdp + 16, 4);
    if (rsdp[15] >= 2) {
        memcpy(&length, rsdp + 20, 4);
        if (length >= 36 && where_tables_lie(hhdm_virt_to_phys(rsdp), length) && checksum_ok(rsdp, length))
            memcpy(&xsdt, rsdp + 24, 8);
        else kprintf("acpi: RSDP extended checksum invalid, using the RSDT\n");
    }
    const struct acpi_sdt *x = acpi_table_at(xsdt);
    if (x && !memcmp(x->sig, "XSDT", 4) && checksum_ok(x, x->length)) {
        entry_size = 8;
        return x;
    }
    if (xsdt) kprintf("acpi: XSDT invalid, trying the RSDT\n");
    const struct acpi_sdt *r = acpi_table_at(rsdt);
    if (r && !memcmp(r->sig, "RSDT", 4) && checksum_ok(r, r->length)) {
        entry_size = 4;
        return r;
    }
    kprintf("acpi: RSDT invalid\n");
    return NULL;
}

void acpi_tables_init(struct limine_memmap_response *mm) {
    map = mm;
    const struct acpi_sdt *r = root();
    if (!r) return;
    entries = (const uint8_t *)(r + 1);
    nentries = (r->length - sizeof *r) / entry_size;
    valid = bootmem_alloc(ALIGN_UP(MAX(nentries, 1u), 64) / 8, 8);
    char line[128];
    size_t n = 0;
    unsigned listed = 0;
    for (uint32_t i = 0; i < nentries; i++) {
        uint64_t phys = entry_phys(i);
        const struct acpi_sdt *t = acpi_table_at(phys);
        if (!t) {
            if (phys) kprintf("acpi: the table at %lx lies outside the firmware's ranges, skipped\n", phys);
            continue;
        }
        if (!checksum_ok(t, t->length)) {
            kprintf("acpi: table %.4s at %lx fails its checksum, skipped\n", t->sig, phys);
            continue;
        }
        valid[i / 64] |= 1ULL << (i % 64);
        if (n + 6 > sizeof line) {
            kprintf("acpi: tables%s\n", line);
            n = 0;
        }
        n += ksnprintf(line + n, sizeof line - n, " %.4s", t->sig);
        listed++;
    }
    if (n) kprintf("acpi: tables%s\n", line);
    if (!listed) kprintf("acpi: the root table lists no valid table\n");
}

const void *acpi_find(const char sig[4], unsigned index) {
    for (uint32_t i = 0; i < nentries; i++) {
        if (!(valid[i / 64] & (1ULL << (i % 64)))) continue;
        const struct acpi_sdt *t = phys_to_virt(entry_phys(i));
        if (!memcmp(t->sig, sig, 4) && index-- == 0) return t;
    }
    return NULL;
}

const struct acpi_entry *acpi_entry_next(const struct acpi_sdt *table, uint32_t first, const struct acpi_entry *after) {
    const uint8_t *b = (const uint8_t *)table;
    uint32_t off = after ? (uint32_t)((const uint8_t *)after - b) + after->length : first;
    if (off + sizeof(struct acpi_entry) > table->length) return NULL;
    const struct acpi_entry *e = (const struct acpi_entry *)(b + off);
    return e->length >= sizeof *e && e->length <= table->length - off ? e : NULL;
}

/* ---- AML: uACPI (third_party/uacpi) on Nocturne's own kernel services ----
   uACPI is called from any CPU: the "acpi" kernel thread runs its deferred work (GPE methods,
   Notify), and threads that evaluate a method (dev/pciuser.c) or change the power state call in
   where they run. Its locks and events are counting semaphores, each a count under its own
   spinlock with a wait queue; its spinlocks are the kernel's (arch/spin.h); the SCI is an IOAPIC
   line of the interrupt layer's (uacpi_kernel_install_interrupt_handler). */
static bool aml_ready;

/* Any thread waits where uACPI waits, the boot thread too (it halts between the other threads'
   turns, sys/wait.h). While frozen (acpi_freeze) nothing blocks and nothing else runs: every other CPU
   is parked, and the devices are down or not yet back; a wait then spins on the clock. */
static volatile bool frozen;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    if (!rsdp_req.response) return UACPI_STATUS_NOT_FOUND;
    uint64_t a = (uint64_t)rsdp_req.response->address;
    *out = a >= hhdm_offset ? a - hhdm_offset : a;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) { return vmm_map_phys(addr, len ? len : 1); }
void uacpi_kernel_unmap(void *addr, uacpi_size len) {} /* the direct map stays */

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg) {
    kprintf("acpi: %s%s", level == UACPI_LOG_ERROR ? "error: " : level == UACPI_LOG_WARN ? "warning: " : "", msg);
}

struct pci_handle { uint8_t bus, dev, fn; };

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out) {
    if (a.segment) return UACPI_STATUS_UNIMPLEMENTED; /* segment 0 only, like dev/pci.c */
    struct pci_handle *h = kmalloc(sizeof *h);
    if (!h) return UACPI_STATUS_OUT_OF_MEMORY;
    *h = (struct pci_handle){a.bus, a.device, a.function};
    *out = h;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) { kfree(h); }

/* None goes to a function an FLR is under way on (dev/pci.h), which may discard it: a read gets
   all-ones, as from a function that does not answer, and a write is dropped. */
static uint32_t cfg_read(struct pci_handle *h, uacpi_size off) {
    uint32_t v;
    pci_config_firmware_read(h->bus, h->dev, h->fn, (uint16_t)off, &v);
    return v;
}

/* Writes as wide as asked: a read-modify-write of the dword would write back the registers beside
   it, and status registers clear their bits when written with 1. */
static void cfg_write(struct pci_handle *h, uacpi_size off, uint32_t v, unsigned bytes) {
    pci_config_firmware_write(h->bus, h->dev, h->fn, (uint16_t)off, v, bytes);
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v) {
    *v = cfg_read(h, off) >> (off & 3) * 8;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v) {
    *v = cfg_read(h, off) >> (off & 2) * 8;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v) {
    *v = cfg_read(h, off);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) {
    cfg_write(h, off, v, 1);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) {
    cfg_write(h, off, v, 2);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) {
    cfg_write(h, off, v, 4);
    return UACPI_STATUS_OK;
}

/* An I/O range handle is its base port plus one (port 0 is a valid range, a NULL handle is not). */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out) {
    if (base > 0xFFFF || len > 0x10000 - base) return UACPI_STATUS_INVALID_ARGUMENT;
    *out = (uacpi_handle)(uintptr_t)(base + 1);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h) {}
#define PORT(h, off) ((uint16_t)((uintptr_t)(h) - 1 + (off)))
uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v) { *v = inb(PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v) { *v = inw(PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v) { *v = inl(PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) { outb(PORT(h, off), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) { outw(PORT(h, off), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) { outl(PORT(h, off), v); return UACPI_STATUS_OK; }

void *uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size); }
void uacpi_kernel_free(void *p) { kfree(p); }

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return clock_now_ns(); }
void uacpi_kernel_stall(uacpi_u8 us) { clock_spin(us * NS_PER_US); }
void uacpi_kernel_sleep(uacpi_u64 ms) {
    uint64_t ns = MIN(ms, CLOCK_NEVER / NS_PER_MS) * NS_PER_MS;
    if (!frozen) sleep_until(clock_after(ns));
    else clock_spin(ns);
}

/* Frozen, a lock or event nobody gives would hang the machine: after this long its owner may run. */
#define FROZEN_WAIT_NS (2 * NS_PER_S)

/* units under lock, which a giver takes in any context (interrupt handlers, under uACPI's
   spinlocks) and which comes before wq's own lock. */
struct sem {
    struct spinlock lock;
    uint32_t units;
    struct wait_queue wq;
};

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 units) {
    struct sem *s = kzalloc(sizeof *s);
    if (s) s->units = units;
    return s;
}
void uacpi_kernel_free_semaphore(uacpi_handle s) { kfree(s); }

uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle h, uacpi_u16 timeout) {
    struct sem *s = h;
    uint64_t start = clock_now_ns(), end = timeout == 0xFFFF ? CLOCK_NEVER : start + timeout * NS_PER_MS;
    uint64_t f = spin_lock(&s->lock);
    while (!s->units) {
        uint64_t now = clock_now_ns();
        if (now >= end) {
            spin_unlock(&s->lock, f);
            return UACPI_STATUS_TIMEOUT;
        }
        if (!frozen) {
            /* a giver on another CPU takes s->lock to give the unit, and finds this task enrolled */
            wq_wait_locked_until(&s->wq, &s->lock, end);
        } else if (timeout != 0xFFFF || now - start < FROZEN_WAIT_NS) {
            /* only an interrupt can give the unit now, and only where the caller allows them: with
               the devices stopped (machine_sleep's interrupts-off part) none is let in */
            spin_give(&s->lock);
            if (f & (1u << 9)) sti();
            pause();
            cli();
        } else {
            static bool told;
            spin_give(&s->lock);
            if (!__atomic_exchange_n(&told, true, __ATOMIC_RELAXED))
                kprintf("acpi: a lock is held across sleep, other threads run until it is given\n");
            sti(); /* let interrupts and other threads give the unit */
            sched_yield();
            cli();
        }
        spin_take(&s->lock);
    }
    s->units--;
    spin_unlock(&s->lock, f);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_signal_semaphore(uacpi_handle h) {
    struct sem *s = h;
    uint64_t f = spin_lock(&s->lock);
    s->units++;
    wq_wake_all(&s->wq);
    spin_unlock(&s->lock, f);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) { return thread_current(); }
/* This CPU's interrupts only, as uACPI asks: its state across CPUs is under its spinlocks. */
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return irq_save(); }
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state s) { irq_restore(s); }

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r) {
    if (r->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        kprintf("acpi: firmware reports a fatal error: type %u code %x arg %lx\n", r->fatal.type, r->fatal.code,
                r->fatal.arg);
    return UACPI_STATUS_OK;
}

/* The SCI is the only interrupt uACPI installs: the FADT's SCI_INT, through the MADT's override
   of it where there is one (ioapic_isa_gsi), an IOAPIC line that is level and shared, on the CPU
   the interrupt layer chose. sci_running counts the handlers under way, which
   uacpi_kernel_wait_for_work_completion waits out (irq_free does for uninstalling). */
static uacpi_interrupt_handler sci_handler;
static uacpi_handle sci_ctx;
static struct irq *sci;
static uint32_t sci_gsi;
static unsigned sci_running;

static enum irq_result sci_entry(void *context) {
    (void)context;
    __atomic_fetch_add(&sci_running, 1, __ATOMIC_SEQ_CST);
    uacpi_interrupt_ret r = sci_handler(sci_ctx);
    __atomic_fetch_sub(&sci_running, 1, __ATOMIC_RELEASE);
    return r & UACPI_INTERRUPT_HANDLED ? IRQ_HANDLED : IRQ_NONE;
}

static void sci_wait_idle(void) {
    while (__atomic_load_n(&sci_running, __ATOMIC_SEQ_CST)) pause();
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler h, uacpi_handle ctx,
                                                    uacpi_handle *out) {
    if (sci) return UACPI_STATUS_UNIMPLEMENTED;
    sci_handler = h;
    sci_ctx = ctx;
    sci_gsi = ioapic_isa_gsi(irq);
    sci = irq_request_gsi(&(struct irq_spec){.name = "ACPI SCI", .handler = sci_entry, .node = -1}, sci_gsi);
    if (!sci) return UACPI_STATUS_INTERNAL_ERROR;
    *out = sci;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler h, uacpi_handle irq) {
    if (!sci || h != sci_handler || irq != sci) return UACPI_STATUS_INVALID_ARGUMENT;
    irq_free(sci); /* no handler runs from here on: ctx may be freed once this returns */
    sci = NULL;
    return UACPI_STATUS_OK;
}

/* uACPI's spinlocks are the kernel's: taken with this CPU's interrupts off, and while waiting
   for one a CPU answers TLB shootdowns (arch/spin.h). */
uacpi_handle uacpi_kernel_create_spinlock(void) { return kzalloc(sizeof(struct spinlock)); }
void uacpi_kernel_free_spinlock(uacpi_handle h) { kfree(h); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) { return spin_lock(h); }
void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags f) { spin_unlock(h, f); }

/* Deferred work: one FIFO, run in order by the "acpi" thread, on whichever CPU it runs. uACPI asks
   for GPE methods on CPU 0 (firmware bugs around SMIs): the scheduler has no way yet to keep a
   thread there. `unfinished` counts queued plus running work for wait_for_work_completion. The
   list and the count are under work_lock, which schedule_work takes in interrupt context too. */
struct work {
    uacpi_work_handler fn;
    uacpi_handle ctx;
    struct work *next;
};
static struct spinlock work_lock;
static struct work *work_head, *work_tail;
static unsigned unfinished;
static struct wait_queue work_wq, done_wq;

uacpi_handle uacpi_kernel_create_work_item(void) { return kzalloc(sizeof(struct work)); }
void uacpi_kernel_free_work_item(uacpi_handle w) { kfree(w); }

void uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_handle item, uacpi_work_handler fn, uacpi_handle ctx) {
    struct work *w = item;
    uint64_t f = spin_lock(&work_lock);
    w->fn = fn;
    w->ctx = ctx;
    w->next = NULL;
    if (work_tail) work_tail->next = w;
    else work_head = w;
    work_tail = w;
    unfinished++;
    wq_wake_all(&work_wq);
    spin_unlock(&work_lock, f);
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    /* Frozen, the worker was idle when the freeze began (acpi_freeze) and cannot run: what is
       queued since waits for the thaw. Otherwise the SCI handler under way first (it may still
       queue work), then the work. */
    if (frozen) return UACPI_STATUS_OK;
    sci_wait_idle();
    uint64_t f = spin_lock(&work_lock);
    while (unfinished) {
        wq_wait_locked(&done_wq, &work_lock);
        spin_take(&work_lock);
    }
    spin_unlock(&work_lock, f);
    return UACPI_STATUS_OK;
}

static void acpi_worker(void *arg) {
    for (;;) {
        uint64_t f = spin_lock(&work_lock);
        while (!work_head) {
            wq_wait_locked(&work_wq, &work_lock);
            spin_take(&work_lock);
        }
        struct work *w = work_head;
        work_head = w->next;
        if (!work_head) work_tail = NULL;
        uacpi_work_handler fn = w->fn; /* the item may be reused or freed by the handler */
        uacpi_handle ctx = w->ctx;
        spin_unlock(&work_lock, f);
        fn(ctx);
        f = spin_lock(&work_lock);
        if (!--unfinished) wq_wake_all(&done_wq);
        spin_unlock(&work_lock, f);
    }
}

/* ---- events and sleep ---- */

/* The power button asks like the menu does: apps may ask to save; a second press within a minute
   of a cancelled request forces it. Interrupt context (fixed event) or the acpi thread (Notify). */
static uacpi_interrupt_ret power_button(uacpi_handle ctx) {
    pm_request(PM_POWER_OFF, true);
    return UACPI_INTERRUPT_HANDLED;
}

static uacpi_status button_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value) {
    if (value == 0x80) pm_request(PM_POWER_OFF, true); /* control-method power button: pressed */
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found_button(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    if (uacpi_install_notify_handler(node, button_notify, NULL) == UACPI_STATUS_OK) (*(unsigned *)user)++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static bool aml_ok(uacpi_status st, const char *what) {
    if (st == UACPI_STATUS_OK) return true;
    kprintf("acpi: %s failed: %s; AML is off, power off uses the FADT alone\n", what, uacpi_status_to_string(st));
    return false;
}

void acpi_aml_init(void) {
    if (cmdline_has("noaml")) {
        kprintf("acpi: AML off (noaml)\n");
        return;
    }
    kthread_create("acpi", acpi_worker, NULL);
    uint64_t t0 = clock_now_ns();
    if (!aml_ok(uacpi_initialize(0), "uACPI initialisation")) return;
    if (!aml_ok(uacpi_namespace_load(), "loading the namespace")) return;
    /* \_PIC(1) before any _INI or _PRT: the interrupts go through the IOAPICs (ACPI 6.5 5.8.1) */
    if (!aml_ok(uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC), "telling the firmware of the IOAPICs"))
        return;
    if (!aml_ok(uacpi_namespace_initialize(), "initialising devices")) return;
    uacpi_bool hw_reduced = false;
    uacpi_is_platform_reduced_hardware(&hw_reduced);
    bool fixed = !hw_reduced &&
                 uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, power_button, NULL) == UACPI_STATUS_OK;
    unsigned buttons = 0;
    uacpi_find_devices("PNP0C0C", found_button, &buttons);
    if (!hw_reduced) aml_ok(uacpi_finalize_gpe_initialization(), "enabling GPEs");
    aml_ready = true;
    kprintf("acpi: AML ready in %lu ms, SCI GSI %d, power button: %s%s%u in the namespace\n",
            (clock_now_ns() - t0) / NS_PER_MS, sci ? (int)sci_gsi : -1, fixed ? "fixed" : "", fixed ? " + " : "",
            buttons);
}

bool acpi_aml_ready(void) { return aml_ready; }

bool acpi_sleep_supported(unsigned state) {
    if (!aml_ready || state > 5) return false;
    char path[] = "\\_S0_";
    path[3] = (char)('0' + state);
    uacpi_namespace_node *node;
    return uacpi_namespace_node_find(UACPI_NULL, path, &node) == UACPI_STATUS_OK;
}

/* PCI interrupt links (PNP0C0F): the chipset's routing of INTx to IRQs comes back cleared from
   S3, so each link's current setting (_CRS) is kept and given back to it (_SRS) on waking. */
#define LINKS_MAX 32
static struct link {
    uacpi_namespace_node *node;
    uacpi_resources *res;
} links[LINKS_MAX];
static unsigned nlinks;

static uacpi_iteration_decision found_link(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)user;
    (void)depth;
    uacpi_namespace_node *srs;
    if (nlinks == LINKS_MAX || uacpi_namespace_node_find(node, "_SRS", &srs) != UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (uacpi_get_current_resources(node, &links[nlinks].res) == UACPI_STATUS_OK) links[nlinks++].node = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_links_save(void) {
    if (!aml_ready) return;
    acpi_links_forget();
    uacpi_find_devices("PNP0C0F", found_link, NULL);
}

void acpi_links_forget(void) {
    for (unsigned i = 0; i < nlinks; i++) uacpi_free_resources(links[i].res);
    nlinks = 0;
}

bool acpi_sleep_prepare(unsigned state) {
    if (!aml_ready) return false;
    if (state == 3) acpi_links_save();
    uacpi_status st = uacpi_prepare_for_sleep_state((uacpi_sleep_state)state);
    if (st != UACPI_STATUS_OK) kprintf("acpi: preparing S%u failed: %s\n", state, uacpi_status_to_string(st));
    return st == UACPI_STATUS_OK;
}

bool acpi_sleep_enter(unsigned state) {
    if (!aml_ready) return false;
    if (state == 3) {
        /* where the firmware jumps on waking: the wake page, in real mode */
        uacpi_status st = uacpi_set_waking_vector(wake_page(), 0);
        if (st != UACPI_STATUS_OK || !wake_available()) {
            kprintf("acpi: no waking vector: %s\n", uacpi_status_to_string(st));
            return false;
        }
        __asm__ volatile("wbinvd" : : : "memory"); /* the caches lose their contents in S3 */
    }
    uacpi_status st = uacpi_enter_sleep_state((uacpi_sleep_state)state);
    if (st != UACPI_STATUS_OK) kprintf("acpi: entering S%u failed: %s\n", state, uacpi_status_to_string(st));
    return st == UACPI_STATUS_OK;
}

void acpi_freeze(bool on) {
    if (on && !frozen) uacpi_kernel_wait_for_work_completion(); /* no method is halfway, holding a lock */
    frozen = on;
}

void acpi_wake_prepare(unsigned state) {
    if (aml_ready) uacpi_prepare_for_wake_from_sleep_state((uacpi_sleep_state)state);
}

void acpi_wake(unsigned state) {
    if (!aml_ready) return;
    unsigned routed = 0;
    for (unsigned i = 0; i < nlinks; i++)
        if (uacpi_set_resources(links[i].node, links[i].res) == UACPI_STATUS_OK) routed++;
    uacpi_status st = uacpi_wake_from_sleep_state((uacpi_sleep_state)state);
    if (st != UACPI_STATUS_OK) kprintf("acpi: waking from S%u: %s\n", state, uacpi_status_to_string(st));
    if (nlinks) kprintf("acpi: %u of %u PCI interrupt links set again\n", routed, nlinks);
    acpi_links_forget();
}

bool acpi_reset(void) { return aml_ready && uacpi_reboot() == UACPI_STATUS_OK; }

/* ---- a PCI function's part of the namespace (acpi.h) ----
   Names are looked up directly under the node they are relative to (uacpi_eval does not search
   upwards), and one name segment cannot leave it ("^" and "\" are no name characters): a driver
   reaches its function's node, that node's children, and the bridge above it, nothing else. */
#define PCI_DEPTH_MAX 32  /* bridges from a root bus down to a function */
#define EVAL_DEPTH_MAX 32 /* packages in packages of one result */
#define EVAL_ARGS_MAX 7   /* Arg0..Arg6 */

struct host_search {
    uint64_t bus;
    uacpi_namespace_node *found;
};

/* A host bridge (PNP0A03, PNP0A08) of segment 0 whose root bus (_BBN, 0 without one) is bus. */
static uacpi_iteration_decision found_host(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct host_search *s = user;
    uacpi_u64 segment = 0, bus = 0;
    if (uacpi_eval_simple_integer(node, "_SEG", &segment) != UACPI_STATUS_OK) segment = 0;
    if (uacpi_eval_simple_integer(node, "_BBN", &bus) != UACPI_STATUS_OK) bus = 0;
    if (segment || bus != s->bus) return UACPI_ITERATION_DECISION_CONTINUE;
    s->found = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

void *acpi_pci_host(const struct pci_device *d) {
    static const uacpi_char *const hids[] = {"PNP0A03", "PNP0A08", UACPI_NULL};
    if (!aml_ready || !d) return NULL;
    for (unsigned n = 0; d->upstream && n < PCI_DEPTH_MAX; n++) d = d->upstream;
    struct host_search s = {d->bus, UACPI_NULL};
    uacpi_find_devices_at(uacpi_namespace_root(), hids, found_host, &s);
    return s.found;
}

/* The device under parent whose _ADR is the function (dev << 16 | func), else one that stands
   for all of the device's functions (func 0xFFFF). */
static uacpi_namespace_node *child_at(uacpi_namespace_node *parent, uint8_t dev, uint8_t func) {
    uacpi_namespace_node *iter = UACPI_NULL, *every = UACPI_NULL;
    while (uacpi_namespace_node_next_typed(parent, &iter, UACPI_OBJECT_DEVICE_BIT) == UACPI_STATUS_OK) {
        uacpi_u64 address;
        if (uacpi_eval_simple_integer(iter, "_ADR", &address) != UACPI_STATUS_OK) continue;
        if (address == ((uint64_t)dev << 16 | func)) return iter;
        if (!every && address == ((uint64_t)dev << 16 | 0xFFFF)) every = iter;
    }
    return every;
}

void *acpi_pci_node(const struct pci_device *d) {
    const struct pci_device *chain[PCI_DEPTH_MAX];
    unsigned n = 0;
    for (const struct pci_device *p = d; p; p = p->upstream) {
        if (n == PCI_DEPTH_MAX) return NULL;
        chain[n++] = p;
    }
    uacpi_namespace_node *node = n ? acpi_pci_host(d) : UACPI_NULL;
    while (node && n) {
        n--;
        node = child_at(node, chain[n]->dev, chain[n]->func);
    }
    return node;
}

/* The interrupt descriptor number index of a link device's current resources (_CRS): its first
   interrupt, a small IRQ descriptor's an ISA IRQ, an extended one's a GSI. */
struct link_irq {
    uint32_t index, at, gsi;
    bool found, level, low;
};

static uacpi_iteration_decision link_irq_at(void *user, uacpi_resource *r) {
    struct link_irq *l = user;
    if (l->at++ != l->index) return UACPI_ITERATION_DECISION_CONTINUE;
    if (r->type == UACPI_RESOURCE_TYPE_IRQ && r->irq.num_irqs) {
        l->gsi = ioapic_isa_gsi(r->irq.irqs[0]);
        l->level = r->irq.triggering == UACPI_TRIGGERING_LEVEL;
        l->low = r->irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
        l->found = true;
    } else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ && r->extended_irq.num_irqs) {
        l->gsi = r->extended_irq.irqs[0];
        l->level = r->extended_irq.triggering == UACPI_TRIGGERING_LEVEL;
        l->low = r->extended_irq.polarity == UACPI_POLARITY_ACTIVE_LOW;
        l->found = true;
    }
    return UACPI_ITERATION_DECISION_BREAK;
}

/* The entry of a _PRT for device dev's pin (0 = INTA): a GSI, level and active low, or the
   interrupt its link device has now (ACPI 6.5 6.2.13). */
static bool prt_gsi(const uacpi_pci_routing_table *t, uint8_t dev, unsigned pin, uint32_t *gsi) {
    for (size_t i = 0; i < t->num_entries; i++) {
        const uacpi_pci_routing_table_entry *e = &t->entries[i];
        if (e->address >> 16 != dev || e->pin != pin) continue;
        if (!e->source) {
            *gsi = e->index;
            return ioapic_gsi_signal(e->index, true, true);
        }
        struct link_irq l = {.index = e->index};
        if (uacpi_for_each_device_resource(e->source, "_CRS", link_irq_at, &l) != UACPI_STATUS_OK || !l.found)
            return false;
        *gsi = l.gsi;
        return ioapic_gsi_signal(l.gsi, l.level, l.low);
    }
    return false;
}

bool acpi_pci_gsi(const struct pci_device *d, uint32_t *gsi) {
    if (!aml_ready || !d) return false;
    unsigned pin = (pci_read32(d->bus, d->dev, d->func, 0x3C) >> 8) & 0xFF; /* 1 = INTA .. 4 = INTD */
    if (pin < 1 || pin > 4) return false;
    pin--;
    for (unsigned depth = 0; d && depth < PCI_DEPTH_MAX; depth++) {
        uacpi_namespace_node *bus = d->upstream ? acpi_pci_node(d->upstream) : acpi_pci_host(d);
        uacpi_pci_routing_table *table;
        if (bus && uacpi_get_pci_routing_table(bus, &table) == UACPI_STATUS_OK) {
            bool found = prt_gsi(table, d->dev, pin, gsi);
            uacpi_free_pci_routing_table(table);
            return found;
        }
        /* a bridge without a _PRT: its own pin carries it (PCI-to-PCI Bridge 1.2, 9.1) */
        pin = (pin + d->dev) % 4;
        d = d->upstream;
    }
    return false;
}

void *acpi_node_child(void *node, uint32_t index) {
    uacpi_namespace_node *iter = UACPI_NULL;
    if (!aml_ready || !node) return NULL;
    while (uacpi_namespace_node_next_typed(node, &iter, UACPI_OBJECT_DEVICE_BIT) == UACPI_STATUS_OK)
        if (!index--) return iter;
    return NULL;
}

bool acpi_name_segment(const char *s) {
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (i && c >= '0' && c <= '9'))) return false;
    }
    return !s[4];
}

/* The values a result is written as: one for the object, then a package's elements. -1 when
   packages nest deeper than EVAL_DEPTH_MAX. */
static int64_t result_values(uacpi_object *o, unsigned depth) {
    uacpi_object_array a;
    if (depth > EVAL_DEPTH_MAX) return -1;
    if (!o || uacpi_object_get_type(o) != UACPI_OBJECT_PACKAGE || uacpi_object_get_package(o, &a) != UACPI_STATUS_OK)
        return 1;
    int64_t n = 1;
    for (size_t i = 0; i < a.count; i++) {
        int64_t c = result_values(a.objects[i], depth + 1);
        if (c < 0) return -1;
        n += c;
    }
    return n;
}

/* The result as acpi.h says: its values in breadth-first order (a package's elements together),
   then the bytes of its strings and buffers. */
static int64_t result_write(uacpi_object *result, uint8_t *out, size_t out_size) {
    int64_t count = result_values(result, 0);
    if (count < 0) return -N_ERR_IO;
    if ((uint64_t)count > out_size / sizeof(struct n_acpi_value)) return -N_ERR_TOO_LARGE;
    uacpi_object **objects = kmalloc((size_t)count * sizeof *objects);
    if (!objects) return -N_ERR_NO_MEMORY;
    struct n_acpi_value *v = (struct n_acpi_value *)out;
    size_t bytes = (size_t)count * sizeof *v, next = 1;
    int64_t r = 0;
    objects[0] = result;
    for (size_t i = 0; i < (size_t)count && !r; i++) {
        uacpi_object *o = objects[i];
        uacpi_object_type type = o ? uacpi_object_get_type(o) : UACPI_OBJECT_UNINITIALIZED;
        v[i] = (struct n_acpi_value){0};
        if (type == UACPI_OBJECT_INTEGER) {
            uacpi_u64 n = 0;
            uacpi_object_get_integer(o, &n);
            v[i] = (struct n_acpi_value){N_ACPI_INTEGER, 0, n};
        } else if (type == UACPI_OBJECT_STRING || type == UACPI_OBJECT_BUFFER) {
            uacpi_data_view view = {0};
            uacpi_object_get_string_or_buffer(o, &view);
            size_t length = view.length;
            if (type == UACPI_OBJECT_STRING && length && !view.const_text[length - 1]) length--;
            if (length > out_size - bytes) {
                r = -N_ERR_TOO_LARGE;
                break;
            }
            memcpy(out + bytes, view.const_data, length);
            v[i] = (struct n_acpi_value){type == UACPI_OBJECT_STRING ? N_ACPI_STRING : N_ACPI_BUFFER, (uint32_t)length,
                                         bytes};
            bytes += length;
        } else if (type == UACPI_OBJECT_PACKAGE) {
            uacpi_object_array a = {0};
            uacpi_object_get_package(o, &a);
            if (a.count > (size_t)count - next) {
                r = -N_ERR_IO; /* not what was counted */
                break;
            }
            v[i] = (struct n_acpi_value){N_ACPI_PACKAGE, (uint32_t)a.count, next};
            for (size_t k = 0; k < a.count; k++) objects[next++] = a.objects[k];
        } else if (type != UACPI_OBJECT_UNINITIALIZED) {
            v[i].type = N_ACPI_REFERENCE;
        }
    }
    kfree(objects);
    return r ? r : (int64_t)bytes;
}

int64_t acpi_node_eval(void *node, const char *name, const struct n_acpi_value *args, uint32_t nargs,
                       const uint8_t *data, size_t data_size, uint8_t *out, size_t out_size) {
    if (!aml_ready || !node) return -N_ERR_NOT_FOUND;
    if (!acpi_name_segment(name) || nargs > EVAL_ARGS_MAX) return -N_ERR_INVALID;
    uacpi_object *list[EVAL_ARGS_MAX] = {0};
    int64_t r = 0;
    for (uint32_t i = 0; i < nargs && !r; i++) {
        const struct n_acpi_value *a = &args[i];
        if (a->type == N_ACPI_INTEGER) {
            list[i] = uacpi_object_create_integer(a->value);
        } else if (a->type == N_ACPI_BUFFER || a->type == N_ACPI_STRING) {
            if (a->value > data_size || a->length > data_size - a->value) {
                r = -N_ERR_INVALID;
                break;
            }
            if (a->type == N_ACPI_BUFFER) {
                list[i] = uacpi_object_create_buffer((uacpi_data_view){.const_data = data + a->value, .length = a->length});
            } else { /* uACPI's strings end with their zero */
                char *text = kmalloc((size_t)a->length + 1);
                if (text) {
                    memcpy(text, data + a->value, a->length);
                    text[a->length] = 0;
                    list[i] = uacpi_object_create_string((uacpi_data_view){.const_text = text, .length = a->length + 1});
                    kfree(text);
                }
            }
        } else {
            r = -N_ERR_INVALID;
            break;
        }
        if (!list[i]) r = -N_ERR_NO_MEMORY;
    }
    uacpi_object *result = UACPI_NULL;
    if (!r) {
        uacpi_object_array array = {list, nargs};
        uacpi_status st = uacpi_eval(node, name, nargs ? &array : UACPI_NULL, &result);
        if (st == UACPI_STATUS_OK) r = result_write(result, out, out_size);
        else r = st == UACPI_STATUS_NOT_FOUND ? -N_ERR_NOT_FOUND : -N_ERR_IO;
    }
    if (result) uacpi_object_unref(result);
    for (uint32_t i = 0; i < nargs; i++)
        if (list[i]) uacpi_object_unref(list[i]);
    return r;
}

struct acpi_watch {
    uacpi_namespace_node *node;
    void (*fn)(void *context, uint64_t value);
    void *context;
};

static uacpi_status watch_notify(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)node;
    struct acpi_watch *w = context;
    w->fn(w->context, value);
    return UACPI_STATUS_OK;
}

struct acpi_watch *acpi_node_watch(void *node, void (*fn)(void *context, uint64_t value), void *context) {
    if (!aml_ready || !node) return NULL;
    struct acpi_watch *w = kmalloc(sizeof *w);
    if (!w) return NULL;
    *w = (struct acpi_watch){node, fn, context};
    if (uacpi_install_notify_handler(node, watch_notify, w) != UACPI_STATUS_OK) {
        kfree(w);
        return NULL;
    }
    return w;
}

void acpi_node_unwatch(struct acpi_watch *w) {
    if (!w) return;
    uacpi_uninstall_notify_handler(w->node, watch_notify);
    kfree(w);
}
