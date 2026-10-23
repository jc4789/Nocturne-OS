/* Physical memory manager: a simple bitmap of 4 KiB frames. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "mm/pmm.h"
#include "limine.h"

static uint64_t *bitmap;     /* 1 bit per page, 1 = used */
static uint64_t total_pages; /* pages covered by the bitmap */
static uint64_t free_pages;
static uint64_t usable_pages;
static uint64_t search_hint;
static uint64_t low_base, low_pages; /* usable below 1 MiB; never allocated */

static inline void set_used(uint64_t p) { bitmap[p / 64] |= (1ULL << (p % 64)); }
static inline void set_free(uint64_t p) { bitmap[p / 64] &= ~(1ULL << (p % 64)); }
static inline bool is_used(uint64_t p) { return bitmap[p / 64] & (1ULL << (p % 64)); }

uint64_t pmm_highest_usable;

/* RAM as the bootloader described it (usable, its own and the kernel's): what a hibernation image
   saves and restores. The signature also covers the firmware's ACPI memory, which must not move
   between the boot that saved an image and the boot that restores it. */
#define RAM_RANGES 256
static struct pmm_range ram[RAM_RANGES];
static unsigned nram;
static bool ram_lost; /* more ranges than RAM_RANGES: an image would leave RAM out, so none is made */
static uint64_t map_sig = 0xcbf29ce484222325ULL;

static void sig_add(uint64_t v) {
    for (int i = 0; i < 8; i++) map_sig = (map_sig ^ ((v >> (i * 8)) & 0xFF)) * 0x100000001b3ULL;
}

static void note_ranges(struct limine_memmap_response *mm) {
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE || e->type == LIMINE_MEMMAP_ACPI_NVS) {
            sig_add(e->base);
            sig_add(e->length);
            sig_add(e->type);
        }
        if (e->type != LIMINE_MEMMAP_USABLE && e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
            e->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
            continue;
        uint64_t b = ALIGN_UP(e->base, PAGE_SIZE), end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);
        if (end <= b) continue;
        if (nram && ram[nram - 1].end == b) ram[nram - 1].end = end; /* the map is sorted */
        else if (nram < RAM_RANGES) ram[nram++] = (struct pmm_range){b, end};
        else {
            if (!ram_lost) kprintf("pmm: more than %u RAM ranges, hibernation is off\n", RAM_RANGES);
            ram_lost = true;
        }
    }
    for (unsigned i = 0; i < nram; i++) {
        sig_add(ram[i].base);
        sig_add(ram[i].end);
    }
}

unsigned pmm_ram_ranges(const struct pmm_range **out) {
    *out = ram;
    return nram;
}

bool pmm_ram_complete(void) { return !ram_lost; }

uint64_t pmm_map_signature(void) { return map_sig; }

void pmm_init(struct limine_memmap_response *mm) {
    note_ranges(mm);
    uint64_t top = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            if (e->base + e->length > top) top = e->base + e->length;
        }
    }
    pmm_highest_usable = top;
    total_pages = top / PAGE_SIZE;
    uint64_t bitmap_bytes = ALIGN_UP((total_pages + 63) / 64 * 8, PAGE_SIZE);

    /* find a home for the bitmap */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes && e->base >= 0x100000) {
            bitmap = phys_to_virt(e->base);
            break;
        }
    }
    if (!bitmap) panic("pmm: no room for the frame bitmap");
    memset(bitmap, 0xFF, bitmap_bytes);

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t start = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t p = start; p < end; p++) {
            set_free(p);
            free_pages++;
            usable_pages++;
        }
    }
    /* reserve the bitmap itself and the first megabyte */
    uint64_t bstart = hhdm_virt_to_phys(bitmap) / PAGE_SIZE;
    for (uint64_t p = bstart; p < bstart + bitmap_bytes / PAGE_SIZE; p++) {
        if (!is_used(p)) { set_used(p); free_pages--; }
    }
    for (uint64_t p = 0; p < 256 && p < total_pages; p++) {
        if (!is_used(p)) { set_used(p); free_pages--; }
    }
    search_hint = 256;
    /* the longest usable run in the first megabyte (page 0 aside): real-mode entry (arch/wake.c) */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE || e->base >= 0x100000) continue;
        uint64_t start = MAX(ALIGN_UP(e->base, PAGE_SIZE), PAGE_SIZE);
        uint64_t end = ALIGN_DOWN(MIN(e->base + e->length, 0x100000ULL), PAGE_SIZE);
        if (end > start && (end - start) / PAGE_SIZE > low_pages) {
            low_base = start;
            low_pages = (end - start) / PAGE_SIZE;
        }
    }
}

uint64_t pmm_low_range(uint64_t *pages) {
    *pages = low_pages;
    return low_base;
}

uint64_t pmm_alloc_below(uint64_t limit) {
    cpu_require_bsp();
    uint64_t f = irq_save();
    uint64_t pages = MIN(total_pages, limit / PAGE_SIZE);
    uint64_t words = (pages + 63) / 64;
    for (uint64_t n = 0; n < words; n++) {
        uint64_t w = (search_hint / 64 + n) % words;
        uint64_t available = ~bitmap[w];
        if (w == words - 1 && pages % 64) available &= (1ULL << (pages % 64)) - 1;
        if (!available) continue;
        uint64_t bit = __builtin_ctzll(available);
        uint64_t p = w * 64 + bit;
        set_used(p);
        free_pages--;
        search_hint = p;
        irq_restore(f);
        return p * PAGE_SIZE;
    }
    irq_restore(f);
    return 0;
}

uint64_t pmm_alloc(void) { return pmm_alloc_below(UINT64_MAX); }

uint64_t pmm_alloc_zeroed(void) {
    uint64_t p = pmm_alloc();
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

uint64_t pmm_alloc_contig_below(uint64_t count, uint64_t limit) {
    cpu_require_bsp();
    uint64_t f = irq_save();
    uint64_t run = 0, pages = MIN(total_pages, limit / PAGE_SIZE);
    for (uint64_t p = 256; p < pages; p++) {
        if (is_used(p)) { run = 0; continue; }
        if (++run == count) {
            uint64_t start = p - count + 1;
            for (uint64_t q = start; q <= p; q++) set_used(q);
            free_pages -= count;
            irq_restore(f);
            return start * PAGE_SIZE;
        }
    }
    irq_restore(f);
    return 0;
}

uint64_t pmm_alloc_contig(uint64_t count) { return pmm_alloc_contig_below(count, UINT64_MAX); }

void pmm_free(uint64_t phys) {
    cpu_require_bsp();
    uint64_t p = phys / PAGE_SIZE;
    if (p >= total_pages) return;
    uint64_t f = irq_save();
    if (!is_used(p)) {
        irq_restore(f);
        kprintf("pmm: double free of %p\n", (void *)phys);
        return;
    }
    set_free(p);
    free_pages++;
    irq_restore(f);
}

uint64_t pmm_free_pages(void) { return free_pages; }
uint64_t pmm_page_count(void) { return total_pages; }
bool pmm_page_used(uint64_t pfn) { return pfn >= total_pages || is_used(pfn); }

bool pmm_claim(uint64_t pfn) {
    cpu_require_bsp();
    if (pfn >= total_pages) return false;
    uint64_t f = irq_save();
    bool was_free = !is_used(pfn);
    if (was_free) {
        set_used(pfn);
        free_pages--;
    }
    irq_restore(f);
    return was_free;
}
uint64_t pmm_total_pages(void) { return usable_pages; }
