#pragma once
/* ACPI table registry: RSDP -> XSDT/RSDT walk with checksum validation, plus the MADT and MCFG
   parsers. Tables are looked up by signature. The AML interpreter (uACPI) is at the end. */
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
/* Map a table that is not in the root tables (the DSDT) given its physical address; no checksum check.
   NULL when no page table could be allocated for it. */
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

/* Registry + MADT + MCFG, with a log summary of each, once: from boot (the IOAPICs are masked
   before the APIC is set up) and acpi_init(). The IOMMU tables (DMAR, IVRS) are
   dev/iommu_intel.c's and dev/iommu_amd.c's. */
void acpi_tables_init(void);

/* ---- AML (uACPI) ----
   acpi_aml_init: load the namespace, run the devices' _INI, take the SCI, the power button
   (fixed and control-method) and the GPEs. "noaml" on the command line skips it. */
void acpi_aml_init(void);
bool acpi_aml_ready(void);
/* Sleep states are ACPI's S numbers (3 = suspend to RAM, 5 = soft off). prepare runs _PTS with
   interrupts on; enter writes SLP_TYP with interrupts off and returns only on failure (S5) or
   after waking (S3). acpi_reset is the FADT reset register through uACPI (any address space). */
bool acpi_sleep_supported(unsigned state); /* the firmware has \_Sn */
bool acpi_sleep_prepare(unsigned state);
bool acpi_sleep_enter(unsigned state);
/* After S3: wake_prepare with interrupts off, right after the CPU is back (SLP_TYP back to S0);
   wake with interrupts on before the drivers resume (PCI interrupt links via _SRS, then _WAK). */
void acpi_wake_prepare(unsigned state);
void acpi_wake(unsigned state);
/* Around a sleep, from before the devices stop until they are all back: AML that waits (Sleep,
   Acquire, Wait in _WAK) spins instead of letting another thread at a device not back yet. on
   first lets queued GPE and Notify work finish, so no method holds a lock across the sleep. */
void acpi_freeze(bool on);
/* The PCI interrupt links' settings, kept for acpi_wake (prepare keeps them for S3). Hibernation
   keeps them itself: after a restore the booting kernel's routing is not necessarily the image's.
   forget drops them (a hibernation that did not happen). */
void acpi_links_save(void);
void acpi_links_forget(void);
bool acpi_reset(void);

/* ---- a PCI function's part of the namespace (n_pci_acpi_eval, dev/pciuser.c) ----
   A device node is uACPI's, opaque here. acpi_pci_node: the function's node, found from the host
   bridge of its root bus (_BBN) down its bridges by _ADR; acpi_pci_host: that host bridge's. NULL
   when the firmware describes none, or AML is off. acpi_node_child: the node's device child
   number index in namespace order; NULL past the last. */
struct pci_device;
void *acpi_pci_node(const struct pci_device *d);
void *acpi_pci_host(const struct pci_device *d);
void *acpi_node_child(void *node, uint32_t index);
/* Evaluates the object name (one name segment, directly under node: no search upwards) with
   the arguments args[0..nargs) whose bytes lie in data, and writes the result to out as
   struct n_acpi_value records (the first one is the result) followed by the strings' and
   buffers' bytes. Returns the bytes written, or -N_ERR_NOT_FOUND (no such object), -N_ERR_INVALID
   (a malformed argument or name), -N_ERR_TOO_LARGE (out_size is too small), -N_ERR_IO (the AML
   failed). Threads that may block only. */
struct n_acpi_value;
int64_t acpi_node_eval(void *node, const char *name, const struct n_acpi_value *args, uint32_t nargs,
                       const uint8_t *data, size_t data_size, uint8_t *out, size_t out_size);
/* Notify(node, value) calls fn(context, value) on the acpi thread until acpi_node_unwatch, which
   returns once fn runs no more (never from fn itself). */
struct acpi_watch;
struct acpi_watch *acpi_node_watch(void *node, void (*fn)(void *context, uint64_t value), void *context);
void acpi_node_unwatch(struct acpi_watch *w);
