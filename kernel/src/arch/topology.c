/* The CPUs and the memory nodes (arch/topology.h). An APIC ID is hierarchical: its low bits name
   the thread within a core, the next the core, and so on up to the package, each level's ID
   being the APIC ID shifted right past the levels below it (Intel SDM vol. 3A 11.9, CPUID leaves
   0x1F and 0xB; AMD APM vol. 2 7.6.5). The shifts are the first CPU's; every CPU of the machine
   has the same, which each one checks when it starts. The last-level cache is shared by the IDs
   that agree above the shift its sharing count rounds up to (leaf 4, AMD's 0x8000001D). The
   nodes are the SRAT's proximity domains (ACPI 6.5 5.2.16), the distances the SLIT's (5.2.17).
   Everything is boot memory, sized by what the tables hold. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/topology.h"
#include "dev/acpi.h"
#include "mm/bootmem.h"

struct topology topology;

#define LOCAL_DISTANCE 10  /* the SLIT's own */
#define REMOTE_DISTANCE 20 /* between two nodes when no SLIT says */

/* the right shifts that give each level's ID from an APIC ID */
static struct {
    uint32_t leaf;       /* 0x1F or 0xB */
    unsigned core;       /* past the threads of a core */
    unsigned die;        /* past the levels below the die: the package's when there is no die level */
    unsigned package;
    unsigned llc;
    unsigned llc_sharing; /* the cache leaf's count, 0 when it gives none */
    uint32_t first;       /* the first CPU's APIC ID */
} shift;

static unsigned ceil_log2(unsigned n) { return n <= 1 ? 0 : 32 - __builtin_clz(n - 1); }

static void read_levels(void) {
    uint32_t max, a, b, c, d;
    cpuid(0, 0, &max, &b, &c, &d);
    if (max >= 0x1F) {
        cpuid(0x1F, 0, &a, &b, &c, &d);
        if (b & 0xFFFF) shift.leaf = 0x1F;
    }
    if (!shift.leaf && max >= 0xB) {
        cpuid(0xB, 0, &a, &b, &c, &d);
        if (b & 0xFFFF) shift.leaf = 0xB;
    }
    if (!shift.leaf) panic("topology: CPUID gives no extended topology (leaf 0xB)");
    /* Subleaf m's shift ends its level's bits; a level's ID is the APIC ID past the bits of the
       levels below it. The index the leaf echoes in ECX[7:0] is 8 bits: at most 256 levels. */
    unsigned below = 0;
    bool die = false;
    for (uint32_t m = 0; m < 256; m++) {
        cpuid(shift.leaf, m, &a, &b, &c, &d);
        unsigned type = (c >> 8) & 0xFF;
        if (!type || !(b & 0xFFFF)) break;
        if (m == 0) {
            shift.first = d;
            shift.core = a & 0x1F;
        }
        if (type == 5) { /* die */
            shift.die = below;
            die = true;
        }
        below = a & 0x1F;
    }
    shift.package = below;
    if (!die) shift.die = shift.package;

    /* the highest cache level's sharing; its field and its subleaves: 8 levels of 4 types at most */
    uint32_t leaf = 0, ext;
    cpuid(0x80000000, 0, &ext, &b, &c, &d);
    if (cpu_features.vendor == CPU_VENDOR_AMD) {
        if (cpu_features.topology_extensions && ext >= 0x8000001D) leaf = 0x8000001D;
    } else if (max >= 4) leaf = 4;
    unsigned level = 0;
    for (uint32_t i = 0; leaf && i < 32; i++) {
        cpuid(leaf, i, &a, &b, &c, &d);
        if (!(a & 0x1F)) break;
        if (((a >> 5) & 7) >= level) {
            level = (a >> 5) & 7;
            shift.llc_sharing = ((a >> 14) & 0xFFF) + 1;
        }
    }
    shift.llc = shift.llc_sharing ? ceil_log2(shift.llc_sharing) : shift.package;
    shift.llc = MIN(MAX(shift.llc, shift.core), shift.package);
}

/* ---- the CPUs ---- */

struct found {
    uint32_t apic_id, uid;
};

static int by_apic_id(const void *a, const void *b) {
    uint32_t x = ((const struct found *)a)->apic_id, y = ((const struct found *)b)->apic_id;
    return x < y ? -1 : x > y;
}

/* A MADT processor entry: false for one of another type. 0xFF and 0xFFFFFFFF are the xAPIC's
   and the x2APIC's broadcast destinations, never a processor's. */
static bool processor(const struct acpi_entry *e, struct found *f, uint32_t *flags) {
    if (e->type == ACPI_MADT_LAPIC && e->length >= sizeof(struct acpi_madt_lapic)) {
        const struct acpi_madt_lapic *l = (const void *)e;
        *f = (struct found){l->apic_id, l->uid};
        *flags = l->flags;
        return l->apic_id != 0xFF;
    }
    if (e->type == ACPI_MADT_X2APIC && e->length >= sizeof(struct acpi_madt_x2apic)) {
        const struct acpi_madt_x2apic *x = (const void *)e;
        *f = (struct found){x->apic_id, x->uid};
        *flags = x->flags;
        return x->apic_id != 0xFFFFFFFF;
    }
    return false;
}

static int by_key(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* keys[0..count) sorted and made unique: the number left */
static unsigned unique(uint32_t *keys, unsigned count) {
    sort(keys, count, sizeof *keys, by_key);
    unsigned n = 0;
    for (unsigned i = 0; i < count; i++)
        if (!n || keys[n - 1] != keys[i]) keys[n++] = keys[i];
    return n;
}

static unsigned rank(const uint32_t *keys, unsigned count, uint32_t key) {
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (keys[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Each CPU's group at the level the shift ends, numbered by APIC ID; the number of groups. */
static unsigned number(struct cpu_place *cpu, unsigned count, unsigned by, size_t field, uint32_t *scratch) {
    for (unsigned i = 0; i < count; i++) scratch[i] = cpu[i].apic_id >> by;
    unsigned groups = unique(scratch, count);
    for (unsigned i = 0; i < count; i++)
        *(unsigned *)((char *)&cpu[i] + field) = rank(scratch, groups, cpu[i].apic_id >> by);
    return groups;
}

static struct cpu_place *read_cpus(void) {
    const struct acpi_sdt *madt = acpi_find("APIC", 0);
    if (!madt) panic("topology: no MADT");
    unsigned n = 0, later = 0;
    struct found f;
    uint32_t flags;
    for (const struct acpi_entry *e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, NULL); e;
         e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, e))
        if (processor(e, &f, &flags)) {
            if (flags & ACPI_CPU_ENABLED) n++;
            else if (flags & ACPI_CPU_ONLINE_CAPABLE) later++;
        }
    if (!n) panic("topology: the MADT lists no enabled processor");
    struct found *all = bootmem_alloc(n * sizeof *all, 8);
    unsigned k = 0;
    for (const struct acpi_entry *e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, NULL); e;
         e = acpi_entry_next(madt, ACPI_MADT_ENTRIES, e))
        if (processor(e, &f, &flags) && (flags & ACPI_CPU_ENABLED)) all[k++] = f;
    sort(all, n, sizeof *all, by_apic_id);
    /* one processor listed twice (a type-0 and a type-9 entry) is one CPU */
    unsigned count = 0, first = n;
    for (unsigned i = 0; i < n; i++) {
        if (count && all[count - 1].apic_id == all[i].apic_id) continue;
        if (all[i].apic_id == shift.first) first = count;
        all[count++] = all[i];
    }
    if (first == n) panic("topology: the first CPU (APIC ID %x) is not in the MADT", shift.first);

    struct cpu_place *cpu = bootmem_alloc(count * sizeof *cpu, 64);
    cpu[0] = (struct cpu_place){.apic_id = all[first].apic_id, .uid = all[first].uid};
    for (unsigned i = 0, j = 1; i < count; i++)
        if (i != first) cpu[j++] = (struct cpu_place){.apic_id = all[i].apic_id, .uid = all[i].uid};
    uint32_t *scratch = bootmem_alloc(count * sizeof *scratch, 4);
    topology.cores = number(cpu, count, shift.core, offsetof(struct cpu_place, core), scratch);
    topology.llcs = number(cpu, count, shift.llc, offsetof(struct cpu_place, llc), scratch);
    topology.dies = number(cpu, count, shift.die, offsetof(struct cpu_place, die), scratch);
    topology.sockets = number(cpu, count, shift.package, offsetof(struct cpu_place, socket), scratch);
    topology.cpu = cpu;
    topology.cpus = count;

    kprintf("cpu: %u present, APIC IDs %x-%x: %u cores, %u LLCs, %u dies, %u sockets\n", count, all[0].apic_id,
            all[count - 1].apic_id, topology.cores, topology.llcs, topology.dies, topology.sockets);
    char llc[48] = "no cache leaf, the package's";
    if (shift.llc_sharing) ksnprintf(llc, sizeof llc, "shared by %u", shift.llc_sharing);
    kprintf("cpu: APIC ID shifts (leaf 0x%x) core %u, die %u, package %u; LLC %s, shift %u\n", shift.leaf, shift.core,
            shift.die, shift.package, llc, shift.llc);
    if (later) kprintf("cpu: %u more entries online-capable only, not used (no hot-add)\n", later);
    return cpu;
}

/* ---- the nodes ---- */

struct affinity {
    uint32_t apic_id, domain;
};

static int by_affinity(const void *a, const void *b) {
    return by_key(&((const struct affinity *)a)->apic_id, &((const struct affinity *)b)->apic_id);
}

static int by_base(const void *a, const void *b) {
    uint64_t x = ((const struct numa_range *)a)->base, y = ((const struct numa_range *)b)->base;
    return x < y ? -1 : x > y;
}

/* An SRAT entry that names a CPU's domain; revision 1 tables have 8-bit domains only. */
static bool cpu_affinity(const struct acpi_sdt *srat, const struct acpi_entry *e, struct affinity *out) {
    if (e->type == ACPI_SRAT_APIC && e->length >= sizeof(struct acpi_srat_apic)) {
        const struct acpi_srat_apic *s = (const void *)e;
        if (!(s->flags & ACPI_SRAT_ENABLED)) return false;
        out->apic_id = s->apic_id;
        out->domain = s->domain_low;
        if (srat->revision >= 2)
            out->domain |= (uint32_t)s->domain_high[0] << 8 | (uint32_t)s->domain_high[1] << 16 |
                           (uint32_t)s->domain_high[2] << 24;
        return true;
    }
    if (e->type == ACPI_SRAT_X2APIC && e->length >= sizeof(struct acpi_srat_x2apic)) {
        const struct acpi_srat_x2apic *s = (const void *)e;
        if (!(s->flags & ACPI_SRAT_ENABLED)) return false;
        *out = (struct affinity){s->apic_id, s->domain};
        return true;
    }
    return false;
}

static const struct acpi_srat_memory *memory_affinity(const struct acpi_entry *e) {
    const struct acpi_srat_memory *m = (const void *)e;
    if (e->type != ACPI_SRAT_MEMORY || e->length < sizeof *m || !(m->flags & ACPI_SRAT_ENABLED) || !m->length)
        return NULL;
    return m->base + m->length > m->base ? m : NULL;
}

/* The SLIT, when it is whole: localities squared bytes, each locality 10 to itself, none below. */
static const struct acpi_slit *read_slit(void) {
    const struct acpi_slit *s = acpi_find("SLIT", 0);
    if (!s || s->h.length < sizeof *s) return NULL;
    uint64_t n = s->localities, room = s->h.length - sizeof *s;
    if (!n || n > 0xFFFF || n * n > room) {
        kprintf("numa: the SLIT's %lu localities do not fit it, not used\n", n);
        return NULL;
    }
    for (uint64_t i = 0; i < n * n; i++)
        if (s->distance[i] < LOCAL_DISTANCE || (i % (n + 1) == 0 && s->distance[i] != LOCAL_DISTANCE)) {
            kprintf("numa: the SLIT's distance %lu is %u, not used\n", i, s->distance[i]);
            return NULL;
        }
    return s;
}

static void one_node(void) {
    struct numa_node *node = bootmem_alloc(sizeof *node, 64);
    uint8_t *distance = bootmem_alloc(1, 1);
    node->cpus = topology.cpus;
    distance[0] = LOCAL_DISTANCE;
    topology.node = node;
    topology.nodes = 1;
    topology.distance = distance;
}

static void read_nodes(struct cpu_place *cpu) {
    const struct acpi_sdt *srat = acpi_find("SRAT", 0);
    unsigned ncpu = 0, nmem = 0;
    struct affinity a;
    for (const struct acpi_entry *e = srat ? acpi_entry_next(srat, ACPI_SRAT_ENTRIES, NULL) : NULL; e;
         e = acpi_entry_next(srat, ACPI_SRAT_ENTRIES, e)) {
        if (cpu_affinity(srat, e, &a)) ncpu++;
        else if (memory_affinity(e)) nmem++;
    }
    if (!ncpu && !nmem) {
        one_node();
        kprintf("numa: %s: one node\n", srat ? "the SRAT names no domain" : "no SRAT");
        return;
    }
    uint32_t *domains = bootmem_alloc((ncpu + nmem) * sizeof *domains, 4);
    struct affinity *cpus = bootmem_alloc(MAX(ncpu, 1u) * sizeof *cpus, 4);
    struct numa_range *range = bootmem_alloc(MAX(nmem, 1u) * sizeof *range, 8);
    unsigned nd = 0, nc = 0, nr = 0;
    for (const struct acpi_entry *e = acpi_entry_next(srat, ACPI_SRAT_ENTRIES, NULL); e;
         e = acpi_entry_next(srat, ACPI_SRAT_ENTRIES, e)) {
        const struct acpi_srat_memory *m;
        if (cpu_affinity(srat, e, &a)) {
            cpus[nc++] = a;
            domains[nd++] = a.domain;
        } else if ((m = memory_affinity(e))) {
            range[nr++] = (struct numa_range){.base = m->base, .end = m->base + m->length, .node = m->domain,
                                              .hotplug = m->flags & ACPI_SRAT_HOTPLUG,
                                              .nonvolatile = m->flags & ACPI_SRAT_NONVOLATILE};
            domains[nd++] = m->domain;
        }
    }
    unsigned nodes = unique(domains, nd);
    struct numa_node *node = bootmem_alloc(nodes * sizeof *node, 64);
    for (unsigned i = 0; i < nodes; i++) node[i].domain = domains[i];

    /* each CPU by its APIC ID; one the SRAT leaves out (it must not, 5.2.16) goes to node 0 */
    sort(cpus, nc, sizeof *cpus, by_affinity);
    unsigned unplaced = 0;
    for (unsigned i = 0; i < topology.cpus; i++) {
        unsigned lo = 0, hi = nc;
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2;
            if (cpus[mid].apic_id < cpu[i].apic_id) lo = mid + 1;
            else hi = mid;
        }
        if (lo < nc && cpus[lo].apic_id == cpu[i].apic_id) cpu[i].node = rank(domains, nodes, cpus[lo].domain);
        else unplaced++;
        node[cpu[i].node].cpus++;
    }

    sort(range, nr, sizeof *range, by_base);
    unsigned overlaps = 0;
    for (unsigned i = 0; i < nr; i++) {
        range[i].node = rank(domains, nodes, range[i].node);
        node[range[i].node].bytes += range[i].end - range[i].base;
        if (range[i].hotplug) node[range[i].node].hotplug_bytes += range[i].end - range[i].base;
        if (i && range[i].base < range[i - 1].end) overlaps++;
    }

    const struct acpi_slit *slit = read_slit();
    uint8_t *distance = bootmem_alloc((size_t)nodes * nodes, 1);
    for (unsigned i = 0; i < nodes; i++)
        for (unsigned j = 0; j < nodes; j++) {
            uint32_t from = node[i].domain, to = node[j].domain;
            distance[i * nodes + j] = slit && from < slit->localities && to < slit->localities
                                          ? slit->distance[from * slit->localities + to]
                                          : i == j ? LOCAL_DISTANCE : REMOTE_DISTANCE;
        }

    topology.node = node;
    topology.nodes = nodes;
    topology.range = range;
    topology.ranges = nr;
    topology.distance = distance;

    for (unsigned i = 0; i < nodes; i++) {
        char hot[48] = "";
        if (node[i].hotplug_bytes) ksnprintf(hot, sizeof hot, ", %lu MiB of it hot-pluggable", node[i].hotplug_bytes >> 20);
        kprintf("numa: node %u (domain %u): %u CPUs, %lu MiB%s%s\n", i, node[i].domain, node[i].cpus,
                node[i].bytes >> 20, hot, node[i].cpus ? "" : " (memory only)");
    }
    if (nodes > 1)
        for (unsigned i = 0; i < nodes; i++) {
            char line[128];
            size_t n = 0;
            for (unsigned j = 0; j < nodes; j++) {
                n += ksnprintf(line + n, sizeof line - n, " %u", distance[i * nodes + j]);
                if (n + 5 > sizeof line || j + 1 == nodes) {
                    kprintf("numa: node %u distances%s\n", i, line);
                    n = 0;
                }
            }
        }
    kprintf("numa: %u memory ranges, distances from %s\n", nr, slit ? "the SLIT" : "none given (10 own, 20 other)");
    if (unplaced) kprintf("numa: %u CPUs the SRAT does not place, on node 0\n", unplaced);
    if (overlaps) kprintf("numa: %u memory ranges overlap the one before\n", overlaps);
}

void topology_init(void) {
    read_levels();
    read_nodes(read_cpus());
}

unsigned topology_node_span(uint64_t at, uint64_t end, uint64_t *run_end) {
    unsigned lo = 0, hi = topology.ranges;
    while (lo < hi) { /* the first range that starts above at */
        unsigned mid = lo + (hi - lo) / 2;
        if (topology.range[mid].base <= at) lo = mid + 1;
        else hi = mid;
    }
    if (lo && at < topology.range[lo - 1].end) {
        *run_end = MIN(end, topology.range[lo - 1].end);
        return topology.range[lo - 1].node;
    }
    *run_end = lo < topology.ranges ? MIN(end, topology.range[lo].base) : end;
    return 0;
}
