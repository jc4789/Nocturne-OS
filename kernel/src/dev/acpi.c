/* ACPI table registry (RSDP/XSDT/RSDT), MADT and MCFG parsing. Hardware specs only: ACPI 6.x
   chapter 5 (table formats) and the PCI Firmware specification (MCFG). The AML is uACPI's: the
   second half of this file gives it the kernel services it asks for (uacpi/kernel_api.h). */
#include "kernel.h"
#include "limine.h"
#include "arch/cpu.h"
#include "dev/acpi.h"
#include "dev/pci.h"
#include "dev/pm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "sys/sched.h"
#include <uacpi/kernel_api.h>
#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/notify.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include "arch/wake.h"

extern volatile struct limine_rsdp_request rsdp_req;

#define MAX_TABLES  96
#define MAX_LAPICS  256
#define MAX_IOAPICS 8
#define MAX_ISOS    32
#define MAX_NMIS    16
#define MAX_ECAMS   8
#define MAX_TABLE_LEN (16u << 20)

static struct {
    uint64_t phys;
    const struct acpi_sdt *t;
} tables[MAX_TABLES];
static unsigned ntables;
static bool registry_done, tables_done;

static struct acpi_lapic lapics[MAX_LAPICS];
static struct acpi_ioapic ioapics[MAX_IOAPICS];
static struct acpi_iso isos[MAX_ISOS];
static struct acpi_nmi nmis[MAX_NMIS];
static struct acpi_ecam ecams[MAX_ECAMS];
static unsigned nlapics, nioapics, nisos, nnmis, necams;
static uint64_t madt_lapic_addr;

static bool checksum_ok(const void *p, size_t n) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += b[i];
    return sum == 0;
}

const struct acpi_sdt *acpi_map_table(uint64_t phys) {
    /* the header first (uncached is fine for tables), then the length it gives */
    const struct acpi_sdt *h = vmm_map_mmio(phys, 4096);
    if (!h) return NULL;
    uint32_t len = h->length;
    if (len > 4096 && len <= MAX_TABLE_LEN && !vmm_map_mmio(phys, len)) return NULL;
    return h;
}

static void add_table(uint64_t phys) {
    if (!phys) return;
    const struct acpi_sdt *t = acpi_map_table(phys);
    if (!t) {
        kprintf("acpi: no memory to map the table at %lx, skipped\n", phys);
        return;
    }
    if (t->length < sizeof *t || t->length > MAX_TABLE_LEN) {
        kprintf("acpi: table at %lx has bad length %u, skipped\n", phys, t->length);
        return;
    }
    if (!checksum_ok(t, t->length)) {
        kprintf("acpi: table %.4s at %lx fails its checksum, skipped\n", t->sig, phys);
        return;
    }
    if (ntables >= MAX_TABLES) {
        kprintf("acpi: more than %d tables, %.4s ignored\n", MAX_TABLES, t->sig);
        return;
    }
    tables[ntables].phys = phys;
    tables[ntables].t = t;
    ntables++;
}

/* Walk one root table (XSDT: 8-byte entries, RSDT: 4-byte entries). False if the root is unusable. */
static bool walk_root(uint64_t phys, unsigned entry_size, const char *sig) {
    const struct acpi_sdt *r = acpi_map_table(phys);
    if (!r || memcmp(r->sig, sig, 4) || r->length < sizeof *r || r->length > MAX_TABLE_LEN ||
        !checksum_ok(r, r->length))
        return false;
    unsigned n = (r->length - sizeof *r) / entry_size;
    const uint8_t *e = (const uint8_t *)(r + 1);
    for (unsigned i = 0; i < n; i++) {
        uint64_t p = 0;
        memcpy(&p, e + (size_t)i * entry_size, entry_size);
        add_table(p);
    }
    return true;
}

void acpi_registry_init(void) {
    if (registry_done) return;
    registry_done = true;
    if (!rsdp_req.response) return;
    uint8_t *rsdp = (uint8_t *)rsdp_req.response->address;
    if ((uint64_t)rsdp < hhdm_offset) rsdp = phys_to_virt((uint64_t)rsdp);
    if (memcmp(rsdp, "RSD PTR ", 8) || !checksum_ok(rsdp, 20)) {
        kprintf("acpi: RSDP signature or checksum invalid\n");
        return;
    }
    uint8_t rev = rsdp[15];
    uint32_t rsdt = 0, length = 0;
    uint64_t xsdt = 0;
    memcpy(&rsdt, rsdp + 16, 4);
    if (rev >= 2) {
        memcpy(&length, rsdp + 20, 4);
        if (length >= 36 && length <= 4096 && checksum_ok(rsdp, length)) memcpy(&xsdt, rsdp + 24, 8);
        else kprintf("acpi: RSDP extended checksum invalid, using RSDT\n");
    }
    if (xsdt && walk_root(xsdt, 8, "XSDT")) return;
    if (xsdt) kprintf("acpi: XSDT invalid, trying RSDT\n");
    if (rsdt && !walk_root(rsdt, 4, "RSDT")) kprintf("acpi: RSDT invalid\n");
}

const void *acpi_find(const char sig[4], unsigned index) {
    acpi_registry_init();
    for (unsigned i = 0; i < ntables; i++)
        if (!memcmp(tables[i].t->sig, sig, 4) && index-- == 0) return tables[i].t;
    return NULL;
}

unsigned acpi_table_count(void) {
    acpi_registry_init();
    return ntables;
}

const struct acpi_sdt *acpi_table_at(unsigned index) {
    acpi_registry_init();
    return index < ntables ? tables[index].t : NULL;
}

/* ---- MADT ---- */
static void parse_madt(void) {
    const struct acpi_sdt *h = acpi_find("APIC", 0); /* there is only one MADT */
    if (!h || h->length < sizeof *h + 8) return;
    {
        const uint8_t *b = (const uint8_t *)h;
        uint32_t a;
        memcpy(&a, b + sizeof *h, 4);
        madt_lapic_addr = a;
        for (uint32_t off = sizeof *h + 8; off + 2 <= h->length;) {
            const uint8_t *e = b + off;
            uint8_t type = e[0], len = e[1];
            if (len < 2 || off + len > h->length) break;
            off += len;
            switch (type) {
            case 0: /* processor local APIC */
                if (len >= 8 && nlapics < MAX_LAPICS) {
                    uint32_t flags;
                    memcpy(&flags, e + 4, 4);
                    lapics[nlapics++] = (struct acpi_lapic){.apic_id = e[3], .uid = e[2], .flags = flags};
                }
                break;
            case 9: /* processor local x2APIC */
                if (len >= 16 && nlapics < MAX_LAPICS) {
                    uint32_t id, flags, uid;
                    memcpy(&id, e + 4, 4);
                    memcpy(&flags, e + 8, 4);
                    memcpy(&uid, e + 12, 4);
                    lapics[nlapics++] = (struct acpi_lapic){.apic_id = id, .uid = uid, .flags = flags, .x2apic = true};
                }
                break;
            case 1: /* I/O APIC */
                if (len >= 12 && nioapics < MAX_IOAPICS) {
                    uint32_t addr, base;
                    memcpy(&addr, e + 4, 4);
                    memcpy(&base, e + 8, 4);
                    ioapics[nioapics++] = (struct acpi_ioapic){.id = e[2], .addr = addr, .gsi_base = base};
                }
                break;
            case 2: /* interrupt source override */
                if (len >= 10 && nisos < MAX_ISOS) {
                    uint32_t gsi;
                    uint16_t flags;
                    memcpy(&gsi, e + 4, 4);
                    memcpy(&flags, e + 8, 2);
                    isos[nisos++] = (struct acpi_iso){.bus = e[2], .source = e[3], .gsi = gsi, .flags = flags};
                }
                break;
            case 3: /* NMI source */
                if (len >= 8 && nnmis < MAX_NMIS) {
                    uint32_t gsi;
                    uint16_t flags;
                    memcpy(&flags, e + 2, 2);
                    memcpy(&gsi, e + 4, 4);
                    nmis[nnmis++] = (struct acpi_nmi){.is_lapic = false, .gsi = gsi, .flags = flags, .uid = 0xFFFFFFFF};
                }
                break;
            case 4: /* local APIC NMI */
                if (len >= 6 && nnmis < MAX_NMIS) {
                    uint16_t flags;
                    memcpy(&flags, e + 3, 2);
                    nmis[nnmis++] = (struct acpi_nmi){.is_lapic = true, .uid = e[2] == 0xFF ? 0xFFFFFFFF : e[2],
                                                      .flags = flags, .lint = e[5]};
                }
                break;
            case 10: /* local x2APIC NMI */
                if (len >= 12 && nnmis < MAX_NMIS) {
                    uint16_t flags;
                    uint32_t uid;
                    memcpy(&flags, e + 2, 2);
                    memcpy(&uid, e + 4, 4);
                    nmis[nnmis++] = (struct acpi_nmi){.is_lapic = true, .uid = uid, .flags = flags, .lint = e[8]};
                }
                break;
            case 5: /* local APIC address override */
                if (len >= 12) memcpy(&madt_lapic_addr, e + 4, 8);
                break;
            default: break;
            }
        }
    }
    unsigned enabled = 0, x2 = 0;
    for (unsigned i = 0; i < nlapics; i++) {
        if (lapics[i].flags & 3) enabled++;
        if (lapics[i].x2apic) x2++;
    }
    kprintf("acpi: MADT lapic %lx, %u CPUs (%u enabled, %u x2APIC entries), %u IOAPIC, %u overrides, %u NMI\n",
            madt_lapic_addr, nlapics, enabled, x2, nioapics, nisos, nnmis);
    for (unsigned i = 0; i < nioapics; i++)
        kprintf("acpi:   IOAPIC id %u at %x, GSI base %u\n", ioapics[i].id, ioapics[i].addr, ioapics[i].gsi_base);
    for (unsigned i = 0; i < nisos; i++)
        kprintf("acpi:   override bus %u IRQ %u -> GSI %u flags %x\n", isos[i].bus, isos[i].source, isos[i].gsi,
                isos[i].flags);
}

/* ---- MCFG ---- */
static void parse_mcfg(void) {
    const struct acpi_sdt *h = acpi_find("MCFG", 0);
    if (!h || h->length < sizeof *h + 8) return;
    const uint8_t *b = (const uint8_t *)h;
    for (uint32_t off = sizeof *h + 8; off + 16 <= h->length && necams < MAX_ECAMS; off += 16) {
        struct acpi_ecam e;
        memcpy(&e.base, b + off, 8);
        memcpy(&e.segment, b + off + 8, 2);
        e.start_bus = b[off + 10];
        e.end_bus = b[off + 11];
        if (!e.base || e.end_bus < e.start_bus) continue;
        ecams[necams++] = e;
        kprintf("acpi: MCFG segment %u buses %u-%u at %lx\n", e.segment, e.start_bus, e.end_bus, e.base);
    }
}

void acpi_tables_init(void) {
    if (tables_done) return;
    tables_done = true;
    acpi_registry_init();
    char line[160];
    size_t n = 0;
    for (unsigned i = 0; i < ntables && n + 6 < sizeof line; i++) {
        memcpy(line + n, tables[i].t->sig, 4);
        line[n + 4] = ' ';
        n += 5;
    }
    line[n] = 0;
    kprintf("acpi: tables %s\n", line);
    parse_madt();
    parse_mcfg();
}

uint64_t acpi_madt_lapic_addr(void) { return madt_lapic_addr; }
const struct acpi_lapic *acpi_lapics(unsigned *count) { *count = nlapics; return lapics; }
const struct acpi_ioapic *acpi_ioapics(unsigned *count) { *count = nioapics; return ioapics; }
const struct acpi_iso *acpi_isos(unsigned *count) { *count = nisos; return isos; }
const struct acpi_nmi *acpi_nmis(unsigned *count) { *count = nnmis; return nmis; }
const struct acpi_ecam *acpi_ecams(unsigned *count) { *count = necams; return ecams; }

/* ---- AML: uACPI (third_party/uacpi) on Nocturne's own kernel services ----
   Everything here runs on the BSP, like every other kernel thread. uACPI's locks and events are
   counting semaphores on wait queues; its deferred work (GPE methods, Notify) runs in the "acpi"
   kernel thread; the SCI is an ISA interrupt through the 8259 like the other legacy lines. */
static bool aml_ready;

/* The boot task becomes the idle task: it may not block, so it spins (letting other threads run)
   where a thread would sleep. While frozen (acpi_freeze) nothing blocks and nothing else runs:
   the devices are down or not yet back. */
static volatile bool frozen;
static bool may_block(void) { return !frozen && current_task->pid != 0; }

static uint64_t ns_now(void) {
    if (!tsc_hz) return uptime_ms() * 1000000;
    uint64_t t = rdtsc();
    return t / tsc_hz * 1000000000 + t % tsc_hz * 1000000000 / tsc_hz;
}

static void spin_ns(uint64_t ns) {
    if (!tsc_hz) {
        for (uint64_t i = 0; i < ns / 1000 + 1; i++) io_wait(); /* about a microsecond each */
        return;
    }
    uint64_t end = ns_now() + ns;
    while (ns_now() < end) pause();
}

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
    if (pci_config_blocked(h->bus, h->dev, h->fn)) return 0xFFFFFFFFu;
    return pci_read32(h->bus, h->dev, h->fn, off & ~3u);
}

/* Writes as wide as asked: a read-modify-write of the dword would write back the registers beside
   it, and status registers clear their bits when written with 1. */
static void cfg_write(struct pci_handle *h, uacpi_size off, uint32_t v, unsigned bytes) {
    if (pci_config_blocked(h->bus, h->dev, h->fn)) return;
    if (bytes == 1) pci_write8(h->bus, h->dev, h->fn, off, (uint8_t)v);
    else if (bytes == 2) pci_write16(h->bus, h->dev, h->fn, off, (uint16_t)v);
    else pci_write32(h->bus, h->dev, h->fn, off & ~3u, v);
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

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return ns_now(); }
void uacpi_kernel_stall(uacpi_u8 us) { spin_ns(us * 1000ull); }
void uacpi_kernel_sleep(uacpi_u64 ms) {
    if (may_block()) sleep_ms(ms);
    else spin_ns(ms * 1000000);
}

/* Frozen, a lock or event nobody gives would hang the machine: after this long its owner may run. */
#define FROZEN_WAIT_MS 2000

struct sem {
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
    uint64_t start = uptime_ms(), end = start + timeout;
    uint64_t f = irq_save();
    while (!s->units) {
        uint64_t now = uptime_ms();
        if (timeout != 0xFFFF && now >= end) {
            irq_restore(f);
            return UACPI_STATUS_TIMEOUT;
        }
        if (may_block()) {
            if (timeout == 0xFFFF) wq_wait(&s->wq);
            else wq_wait_timeout(&s->wq, end - now);
        } else if (frozen && (timeout != 0xFFFF || now - start < FROZEN_WAIT_MS)) {
            /* only an interrupt can give the unit now, and only where the caller allows them: with
               the devices stopped (machine_sleep's interrupts-off part) none is let in */
            if (f & (1u << 9)) sti();
            pause();
            cli();
        } else {
            static bool told;
            if (frozen && !told) {
                told = true;
                kprintf("acpi: a lock is held across sleep, other threads run until it is given\n");
            }
            sti(); /* the boot task: let interrupts and other threads give the unit */
            schedule();
            cli();
        }
    }
    s->units--;
    irq_restore(f);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_signal_semaphore(uacpi_handle h) {
    struct sem *s = h;
    uint64_t f = irq_save();
    s->units++;
    wq_wake_all(&s->wq);
    irq_restore(f);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) { return current_task; }
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return irq_save(); }
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state s) { irq_restore(s); }

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r) {
    if (r->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        kprintf("acpi: firmware reports a fatal error: type %u code %x arg %lx\n", r->fatal.type, r->fatal.code,
                r->fatal.arg);
    return UACPI_STATUS_OK;
}

/* The SCI is the only interrupt uACPI installs. */
static uacpi_interrupt_handler sci_handler;
static uacpi_handle sci_ctx;
static int sci_irq = -1;

static void sci_entry(struct regs *r) {
    if (sci_handler) sci_handler(sci_ctx);
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler h, uacpi_handle ctx,
                                                    uacpi_handle *out) {
    if (irq >= 16 || sci_handler) return UACPI_STATUS_UNIMPLEMENTED;
    sci_handler = h;
    sci_ctx = ctx;
    sci_irq = (int)irq;
    irq_register(sci_irq, sci_entry);
    pic_unmask(sci_irq);
    *out = &sci_irq;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler h, uacpi_handle irq) {
    if (h != sci_handler) return UACPI_STATUS_INVALID_ARGUMENT;
    irq_unregister(sci_irq, sci_entry);
    sci_handler = NULL;
    return UACPI_STATUS_OK;
}

struct spin { volatile uint32_t held; };
uacpi_handle uacpi_kernel_create_spinlock(void) { return kzalloc(sizeof(struct spin)); }
void uacpi_kernel_free_spinlock(uacpi_handle h) { kfree(h); }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) {
    struct spin *l = h;
    uint64_t f = irq_save();
    while (__atomic_exchange_n(&l->held, 1, __ATOMIC_ACQUIRE)) pause();
    return f;
}
void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags f) {
    struct spin *l = h;
    __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}

/* Deferred work: one FIFO, run in order by the "acpi" thread (on the BSP, CPU0, as GPE methods
   should be). `unfinished` counts queued plus running work for wait_for_work_completion. */
struct work {
    uacpi_work_handler fn;
    uacpi_handle ctx;
    struct work *next;
};
static struct work *work_head, *work_tail;
static unsigned unfinished;
static struct wait_queue work_wq, done_wq;

uacpi_handle uacpi_kernel_create_work_item(void) { return kzalloc(sizeof(struct work)); }
void uacpi_kernel_free_work_item(uacpi_handle w) { kfree(w); }

void uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_handle item, uacpi_work_handler fn, uacpi_handle ctx) {
    struct work *w = item;
    uint64_t f = irq_save();
    w->fn = fn;
    w->ctx = ctx;
    w->next = NULL;
    if (work_tail) work_tail->next = w;
    else work_head = w;
    work_tail = w;
    unfinished++;
    wq_wake_all(&work_wq);
    irq_restore(f);
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    /* interrupt handlers run to completion with interrupts off on this CPU: none is in flight here.
       Frozen, the worker was idle when the freeze began (acpi_freeze) and cannot run: what is queued
       since waits for the thaw. */
    if (frozen) return UACPI_STATUS_OK;
    uint64_t f = irq_save();
    while (unfinished) {
        if (may_block()) wq_wait(&done_wq);
        else { sti(); schedule(); cli(); }
    }
    irq_restore(f);
    return UACPI_STATUS_OK;
}

static void acpi_worker(void *arg) {
    for (;;) {
        uint64_t f = irq_save();
        while (!work_head) wq_wait(&work_wq);
        struct work *w = work_head;
        work_head = w->next;
        if (!work_head) work_tail = NULL;
        uacpi_work_handler fn = w->fn; /* the item may be reused or freed by the handler */
        uacpi_handle ctx = w->ctx;
        irq_restore(f);
        fn(ctx);
        f = irq_save();
        if (!--unfinished) wq_wake_all(&done_wq);
        irq_restore(f);
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
    uint64_t t0 = ns_now();
    if (!aml_ok(uacpi_initialize(0), "uACPI initialisation")) return;
    if (!aml_ok(uacpi_namespace_load(), "loading the namespace")) return;
    if (!aml_ok(uacpi_namespace_initialize(), "initialising devices")) return;
    uacpi_bool hw_reduced = false;
    uacpi_is_platform_reduced_hardware(&hw_reduced);
    bool fixed = !hw_reduced &&
                 uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, power_button, NULL) == UACPI_STATUS_OK;
    unsigned buttons = 0;
    uacpi_find_devices("PNP0C0C", found_button, &buttons);
    if (!hw_reduced) aml_ok(uacpi_finalize_gpe_initialization(), "enabling GPEs");
    aml_ready = true;
    kprintf("acpi: AML ready in %lu ms, SCI IRQ %d, power button: %s%s%u in the namespace\n",
            (ns_now() - t0) / 1000000, sci_irq, fixed ? "fixed" : "", fixed ? " + " : "", buttons);
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
