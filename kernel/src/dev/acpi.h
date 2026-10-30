#pragma once
/* The ACPI tables: the root table's entries, each checked once (checksum, and that it lies where
   the bootloader maps ACPI tables), looked up by signature, and the formats of the static tables
   the kernel reads (ACPI 6.5 chapter 5; MCFG: the PCI Firmware specification). A table is read in
   place, through the direct map, for good: nothing is copied out of it, so nothing here has a
   size of its own. Each reader decodes the entries it needs. The AML interpreter (uACPI) is at
   the end. */
#include <stdint.h>
#include <stdbool.h>

struct acpi_sdt {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} __attribute__((packed));

/* The root table, once, from boot before anything reads a table: mm is the bootloader's memory
   map, which says where the tables may lie. Logs the signatures. */
struct limine_memmap_response;
void acpi_tables_init(struct limine_memmap_response *mm);
/* The index-th valid table with this signature ("FACP", "APIC", "SRAT", ...), or NULL. */
const void *acpi_find(const char sig[4], unsigned index);
/* A table the root does not list (the DSDT, from the FADT) at that physical address: its header's
   length checked to lie where tables may, its checksum not. NULL when it does not. */
const struct acpi_sdt *acpi_table_at(uint64_t phys);

/* The entries of a table made of (type, length) records after a fixed part (the MADT's and the
   SRAT's): the one after `after` (NULL: the first), or NULL past the last. Each lies inside the
   table and is at least 2 bytes long; a reader checks the length its type needs. */
struct acpi_entry {
    uint8_t type, length;
} __attribute__((packed));
const struct acpi_entry *acpi_entry_next(const struct acpi_sdt *table, uint32_t first, const struct acpi_entry *after);

/* ---- FADT ("FACP", ACPI 6.5 5.2.9): fields past h.length are not there ---- */
struct acpi_gas {
    uint8_t space, bit_width, bit_offset, access;
    uint64_t address;
} __attribute__((packed));

struct acpi_fadt {
    struct acpi_sdt h;
    uint32_t firmware_ctrl, dsdt;
    uint8_t reserved, pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable, acpi_disable, s4bios_req, pstate_cnt;
    uint32_t pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt, pm2_cnt, pm_tmr, gpe0, gpe1;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len, gpe0_len, gpe1_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2_lat, p_lvl3_lat, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alarm, mon_alarm, century;
    uint16_t boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint8_t reserved3[3];
    uint64_t x_firmware_ctrl, x_dsdt;
    struct acpi_gas x_pm1a_evt, x_pm1b_evt, x_pm1a_cnt, x_pm1b_cnt, x_pm2_cnt, x_pm_tmr, x_gpe0, x_gpe1;
    struct acpi_gas sleep_control, sleep_status; /* ACPI 5: hardware-reduced platforms */
} __attribute__((packed));

#define FADT_TMR_VAL_EXT   (1u << 8)  /* the PM timer counts 32 bits, else 24 */
#define FADT_RESET_REG_SUP (1u << 10)
#define FADT_HW_REDUCED    (1u << 20)

/* ---- MADT ("APIC", 5.2.12): entries from byte 44 ---- */
#define ACPI_MADT_ENTRIES 44
enum {
    ACPI_MADT_LAPIC = 0,      /* struct acpi_madt_lapic */
    ACPI_MADT_IOAPIC = 1,     /* struct acpi_madt_ioapic */
    ACPI_MADT_OVERRIDE = 2,   /* struct acpi_madt_override */
    ACPI_MADT_X2APIC = 9,     /* struct acpi_madt_x2apic */
};
#define ACPI_CPU_ENABLED        1u /* a processor entry's flags: present and usable now */
#define ACPI_CPU_ONLINE_CAPABLE 2u /* not enabled now, may be brought online later */
struct acpi_madt_lapic {
    struct acpi_entry e;
    uint8_t uid, apic_id;
    uint32_t flags;
} __attribute__((packed));
struct acpi_madt_ioapic {
    struct acpi_entry e;
    uint8_t id, reserved;
    uint32_t address, gsi_base;
} __attribute__((packed));
struct acpi_madt_override { /* an ISA interrupt's GSI and signal */
    struct acpi_entry e;
    uint8_t bus, source;
    uint32_t gsi;
    uint16_t flags; /* bits 0-1 polarity (3 = active low), bits 2-3 trigger (3 = level) */
} __attribute__((packed));
struct acpi_madt_x2apic {
    struct acpi_entry e;
    uint16_t reserved;
    uint32_t apic_id, flags, uid;
} __attribute__((packed));

/* ---- SRAT (5.2.16): entries from byte 48 ---- */
#define ACPI_SRAT_ENTRIES 48
enum {
    ACPI_SRAT_APIC = 0,   /* struct acpi_srat_apic */
    ACPI_SRAT_MEMORY = 1, /* struct acpi_srat_memory */
    ACPI_SRAT_X2APIC = 2, /* struct acpi_srat_x2apic */
};
#define ACPI_SRAT_ENABLED     1u
#define ACPI_SRAT_HOTPLUG     2u /* memory: may be added and removed while running */
#define ACPI_SRAT_NONVOLATILE 4u /* memory: non-volatile */
struct acpi_srat_apic {
    struct acpi_entry e;
    uint8_t domain_low, apic_id; /* proximity domain bits 7:0 */
    uint32_t flags;
    uint8_t sapic_eid, domain_high[3]; /* bits 31:8 */
    uint32_t clock_domain;
} __attribute__((packed));
struct acpi_srat_memory {
    struct acpi_entry e;
    uint32_t domain;
    uint16_t reserved;
    uint64_t base, length;
    uint32_t reserved2, flags;
    uint64_t reserved3;
} __attribute__((packed));
struct acpi_srat_x2apic {
    struct acpi_entry e;
    uint16_t reserved;
    uint32_t domain, apic_id, flags, clock_domain, reserved2;
} __attribute__((packed));

/* ---- SLIT (5.2.17): distance[i * localities + j] from locality (proximity domain) i to j;
   10 is a locality's own, 0-9 have no meaning, 255 is unreachable ---- */
struct acpi_slit {
    struct acpi_sdt h;
    uint64_t localities;
    uint8_t distance[];
} __attribute__((packed));

/* ---- MCFG: PCIe enhanced configuration access regions, from byte 44 ---- */
#define ACPI_MCFG_ENTRIES 44
struct acpi_mcfg_entry {
    uint64_t base; /* physical address of bus start_bus, device 0, function 0 */
    uint16_t segment;
    uint8_t start_bus, end_bus;
    uint32_t reserved;
} __attribute__((packed));

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
/* Whether s is one name segment: 4 characters of A-Z, 0-9 and _, the first not a digit (names of
   fewer characters are padded with _, as "_ON_"). Names starting with _ are ACPI's predefined
   ones; the others are the firmware's own. */
bool acpi_name_segment(const char *s);
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
