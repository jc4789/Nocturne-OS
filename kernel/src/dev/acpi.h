#pragma once
/* ACPI table registry: RSDP -> XSDT/RSDT walk with checksum validation, plus the MADT and MCFG
   parsers. Tables are looked up by signature; no AML is interpreted. */
#include <stdint.h>
#include <stdbool.h>

struct acpi_sdt {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} __attribute__((packed));

/* Walk the RSDP and cache every table whose checksum is valid. Idempotent; acpi_find() calls it. */
void acpi_registry_init(void);
/* index-th valid table with this signature ("FACP", "APIC", "MCFG", ...), or NULL. Mapped, never freed. */
const void *acpi_find(const char sig[4], unsigned index);
/* Map a table that is not in the root tables (the DSDT) given its physical address; no checksum check. */
const struct acpi_sdt *acpi_map_table(uint64_t phys);
unsigned acpi_table_count(void);
const struct acpi_sdt *acpi_table_at(unsigned index);

/* ---- MADT ("APIC") ---- */
struct acpi_lapic {
    uint32_t apic_id, uid, flags; /* flags bit 0: enabled, bit 1: online-capable */
    bool x2apic;                  /* from a type-9 (x2APIC) entry */
};
struct acpi_ioapic {
    uint8_t id;
    uint32_t addr, gsi_base;
};
struct acpi_iso { /* interrupt source override */
    uint8_t bus, source;
    uint32_t gsi;
    uint16_t flags; /* bits 0-1 polarity (3 = active low), bits 2-3 trigger (3 = level) */
};
struct acpi_nmi {
    bool is_lapic;   /* true: processor LINT NMI (uid/lint), false: NMI source (gsi) */
    uint32_t uid;    /* 0xFFFFFFFF = all processors */
    uint32_t gsi;
    uint16_t flags;
    uint8_t lint;
};
uint64_t acpi_madt_lapic_addr(void); /* 0 if there is no MADT */
const struct acpi_lapic *acpi_lapics(unsigned *count);
const struct acpi_ioapic *acpi_ioapics(unsigned *count);
const struct acpi_iso *acpi_isos(unsigned *count);
const struct acpi_nmi *acpi_nmis(unsigned *count);

/* ---- MCFG: PCIe enhanced configuration access regions ---- */
struct acpi_ecam {
    uint64_t base; /* physical address of bus start_bus, device 0, function 0 */
    uint16_t segment;
    uint8_t start_bus, end_bus;
};
const struct acpi_ecam *acpi_ecams(unsigned *count);

/* Registry + MADT + MCFG, with a one-line log summary of each. Called once from acpi_init(). */
void acpi_tables_init(void);
