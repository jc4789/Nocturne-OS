/* ACPI table registry (RSDP/XSDT/RSDT), MADT and MCFG parsing. Hardware specs only: ACPI 6.x
   chapter 5 (table formats) and the PCI Firmware specification (MCFG). No AML. */
#include "kernel.h"
#include "limine.h"
#include "dev/acpi.h"
#include "mm/vmm.h"

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
    vmm_map_mmio(phys, 4096); /* make sure the header is mapped (uncached is fine for tables) */
    const struct acpi_sdt *h = phys_to_virt(phys);
    uint32_t len = h->length;
    if (len > 4096 && len <= MAX_TABLE_LEN) vmm_map_mmio(phys, len);
    return h;
}

static void add_table(uint64_t phys) {
    if (!phys) return;
    const struct acpi_sdt *t = acpi_map_table(phys);
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
    if (memcmp(r->sig, sig, 4) || r->length < sizeof *r || r->length > MAX_TABLE_LEN ||
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
