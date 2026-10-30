/* Memory before the page allocator (mm/bootmem.h). The bootloader's usable ranges above the first
   megabyte are pieces, each used from its top down; bootmem_by_node cuts them where the nodes'
   memory meets, so that each lies on one node. The table of pieces is its own first allocation,
   so nothing here has a fixed size. Allocations are rare and boot-time only: each one looks at
   every piece. */
#include "kernel.h"
#include "limine.h"
#include "arch/topology.h"
#include "mm/bootmem.h"

#define LOW_END 0x100000ULL /* the first megabyte: the wake code's */

struct piece {
    uint64_t base, top, end; /* physical; [top, end) was given out */
    unsigned node;
};

static struct piece *pieces;
static uint64_t npieces;
static bool closed;

static bool usable(const struct limine_memmap_entry *e, uint64_t *base, uint64_t *end) {
    if (e->type != LIMINE_MEMMAP_USABLE) return false;
    *base = MAX(ALIGN_UP(e->base, PAGE_SIZE), LOW_END);
    *end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);
    return *end > *base;
}

/* The highest place bytes fit at with that alignment, below what each piece already gave, among
   the pieces of node (all of them when any): taken from that piece. NULL when none holds it. */
static void *carve(size_t bytes, size_t align, bool any, unsigned node) {
    struct piece *best = NULL;
    uint64_t place = 0;
    for (uint64_t i = 0; i < npieces; i++) {
        struct piece *p = &pieces[i];
        if ((!any && p->node != node) || p->top - p->base < bytes) continue;
        uint64_t at = ALIGN_DOWN(p->top - bytes, align);
        if (at < p->base || (best && at <= place)) continue;
        best = p;
        place = at;
    }
    if (!best) return NULL;
    best->top = place;
    return phys_to_virt(place);
}

void bootmem_init(struct limine_memmap_response *mm) {
    uint64_t count = 0, base, end;
    for (uint64_t i = 0; i < mm->entry_count; i++)
        if (usable(mm->entries[i], &base, &end)) count++;
    /* the table: at the top of the usable range whose top is highest and holds it */
    uint64_t bytes = count * sizeof *pieces, place = 0, home = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        if (!usable(mm->entries[i], &base, &end) || end - base < bytes) continue;
        uint64_t at = ALIGN_DOWN(end - bytes, _Alignof(struct piece));
        if (at >= base && at > place) { place = at; home = i; }
    }
    if (!count || !place) panic("bootmem: no usable memory above 1 MiB holds the table of %lu ranges", count);
    pieces = phys_to_virt(place);
    for (uint64_t i = 0; i < mm->entry_count; i++)
        if (usable(mm->entries[i], &base, &end))
            pieces[npieces++] = (struct piece){base, i == home ? place : end, end, 0};
}

void bootmem_by_node(void) {
    if (closed) panic("bootmem: the nodes came after the page allocator took over");
    uint64_t count = 0, run;
    for (uint64_t i = 0; i < npieces; i++)
        for (uint64_t at = pieces[i].base; at < pieces[i].end; at = run, count++)
            topology_node_span(at, pieces[i].end, &run);
    struct piece *cut = carve(count * sizeof *cut, _Alignof(struct piece), true, 0);
    if (!cut) panic("bootmem: no usable range holds the table of %lu pieces", count);
    uint64_t n = 0;
    for (uint64_t i = 0; i < npieces; i++) {
        const struct piece *p = &pieces[i];
        for (uint64_t at = p->base; at < p->end; at = run) {
            unsigned node = topology_node_span(at, p->end, &run);
            cut[n++] = (struct piece){at, MIN(MAX(p->top, at), run), run, node};
        }
    }
    pieces = cut;
    npieces = n;
}

static void *given(void *p, size_t bytes) {
    if (!p) panic("bootmem: no usable range holds %lu bytes", (uint64_t)bytes);
    memset(p, 0, bytes);
    return p;
}

void *bootmem_alloc(size_t bytes, size_t align) {
    if (closed) panic("bootmem: an allocation after the page allocator took over");
    ASSERT(align && !(align & (align - 1)) && align <= PAGE_SIZE && bytes);
    return given(carve(bytes, align, true, 0), bytes);
}

void *bootmem_alloc_node(size_t bytes, size_t align, unsigned node) {
    if (closed) panic("bootmem: an allocation after the page allocator took over");
    ASSERT(align && !(align & (align - 1)) && align <= PAGE_SIZE && bytes);
    void *p = carve(bytes, align, false, node);
    return given(p ? p : carve(bytes, align, true, 0), bytes);
}

void bootmem_handoff(void (*untaken)(uint64_t base, uint64_t end, unsigned node, void *context), void *context) {
    closed = true;
    for (uint64_t i = 0; i < npieces; i++)
        if (pieces[i].top > pieces[i].base) untaken(pieces[i].base, pieces[i].top, pieces[i].node, context);
}
