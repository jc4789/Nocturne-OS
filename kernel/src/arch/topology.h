#pragma once
/* Where each CPU present at boot sits, and the memory nodes. Read once on the first CPU before
   the page allocator, from the MADT (the CPUs), the first CPU's topology leaves (how an APIC ID
   splits into thread, core, die and package, and which IDs share the last-level cache), the
   SRAT (each CPU's and each memory range's proximity domain) and the SLIT (the distances), and
   kept for good. The groups of each level are numbered densely, 0 up, in the order of their APIC
   IDs, so a per-group table needs no search. */
#include <stdbool.h>
#include <stdint.h>

struct cpu_place {
    uint32_t apic_id; /* the x2APIC ID */
    uint32_t uid;     /* the ACPI processor UID */
    unsigned core;    /* the CPUs of a core are its SMT threads */
    unsigned llc;     /* the CPUs sharing a last-level cache */
    unsigned die;     /* the socket's own where the CPU names no die level */
    unsigned socket;
    unsigned node;
};

struct numa_node {
    uint32_t domain;        /* the ACPI proximity domain */
    unsigned cpus;          /* none: memory only (CXL memory, ...) */
    uint64_t bytes;         /* its enabled memory ranges, the hot-pluggable ones among them */
    uint64_t hotplug_bytes;
};

struct numa_range {
    uint64_t base, end; /* physical, end exclusive */
    unsigned node;
    bool hotplug, nonvolatile;
};

struct topology {
    unsigned cpus, cores, llcs, dies, sockets, nodes;
    const struct cpu_place *cpu;    /* [cpus]: 0 is the first CPU, the others by APIC ID */
    const struct numa_node *node;   /* [nodes], by proximity domain */
    const struct numa_range *range; /* [ranges], by base; none without an SRAT */
    unsigned ranges;
    const uint8_t *distance;        /* [nodes * nodes]: from a to b at a * nodes + b; 10 is a node's own */
};
extern struct topology topology;

void topology_init(void); /* the first CPU: after acpi_tables_init, before the page allocator */
/* The node of the memory at physical at, and in *run_end where its run on that node ends, at most
   end. Memory no SRAT range describes is node 0's. */
unsigned topology_node_span(uint64_t at, uint64_t end, uint64_t *run_end);
