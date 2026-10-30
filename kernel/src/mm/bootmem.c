/* Memory before the page allocator (mm/bootmem.h). Each usable range of the bootloader's map is
   used from its top down; what was taken of each is one number, kept in the first allocation
   itself, so nothing here has a fixed size. Allocations are rare and boot-time only: each one
   looks at every range. */
#include "kernel.h"
#include "limine.h"
#include "mm/bootmem.h"

#define LOW_END 0x100000ULL /* the first megabyte: the wake code's */

static struct limine_memmap_response *map;
static uint64_t *used; /* bytes taken from the top of each entry of the map, or NULL before the first */
static bool closed;

static bool range_of(uint64_t i, uint64_t *base, uint64_t *end) {
    struct limine_memmap_entry *e = map->entries[i];
    if (e->type != LIMINE_MEMMAP_USABLE) return false;
    *base = MAX(ALIGN_UP(e->base, PAGE_SIZE), LOW_END);
    *end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);
    return *end > *base;
}

/* The highest place bytes fit at with that alignment, below what each range already gave. */
static void *carve(size_t bytes, size_t align, uint64_t *entry) {
    uint64_t best = 0;
    bool found = false;
    for (uint64_t i = 0; i < map->entry_count; i++) {
        uint64_t base, end;
        if (!range_of(i, &base, &end)) continue;
        uint64_t top = end - (used ? used[i] : 0);
        if (top - base < bytes) continue;
        uint64_t at = ALIGN_DOWN(top - bytes, align);
        if (at < base || (found && at <= best)) continue;
        best = at;
        *entry = i;
        found = true;
    }
    if (!found) panic("bootmem: no usable range holds %lu bytes", (uint64_t)bytes);
    return phys_to_virt(best);
}

void bootmem_init(struct limine_memmap_response *mm) {
    map = mm;
    uint64_t entry;
    uint64_t *table = carve(mm->entry_count * sizeof *used, sizeof *used, &entry);
    memset(table, 0, mm->entry_count * sizeof *used);
    uint64_t base, end;
    range_of(entry, &base, &end);
    table[entry] = end - hhdm_virt_to_phys(table);
    used = table;
}

void *bootmem_alloc(size_t bytes, size_t align) {
    if (closed) panic("bootmem: an allocation after the page allocator took over");
    ASSERT(align && !(align & (align - 1)) && align <= PAGE_SIZE && bytes);
    uint64_t entry, base, end;
    void *p = carve(bytes, align, &entry);
    range_of(entry, &base, &end);
    used[entry] = end - hhdm_virt_to_phys(p);
    memset(p, 0, bytes);
    return p;
}

void bootmem_handoff(void (*taken)(uint64_t base, uint64_t end, void *context), void *context) {
    closed = true;
    for (uint64_t i = 0; i < map->entry_count; i++) {
        uint64_t base, end;
        if (range_of(i, &base, &end) && used[i]) taken(ALIGN_DOWN(end - used[i], PAGE_SIZE), end, context);
    }
}
