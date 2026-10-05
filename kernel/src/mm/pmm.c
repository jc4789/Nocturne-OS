/* Physical memory manager: a simple bitmap of 4 KiB frames. */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/pmm.h"
#include "limine.h"

static uint64_t *bitmap;     /* 1 bit per page, 1 = used */
static uint64_t total_pages; /* pages covered by the bitmap */
static uint64_t free_pages;
static uint64_t usable_pages;
static uint64_t search_hint;

static inline void set_used(uint64_t p) { bitmap[p / 64] |= (1ULL << (p % 64)); }
static inline void set_free(uint64_t p) { bitmap[p / 64] &= ~(1ULL << (p % 64)); }
static inline bool is_used(uint64_t p) { return bitmap[p / 64] & (1ULL << (p % 64)); }

uint64_t pmm_highest_usable;

void pmm_init(struct limine_memmap_response *mm) {
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
}

uint64_t pmm_alloc(void) {
    uint64_t f = irq_save();
    uint64_t words = (total_pages + 63) / 64;
    for (uint64_t n = 0; n < words; n++) {
        uint64_t w = (search_hint / 64 + n) % words;
        if (bitmap[w] == ~0ULL) continue;
        uint64_t bit = __builtin_ctzll(~bitmap[w]);
        uint64_t p = w * 64 + bit;
        if (p >= total_pages) continue;
        set_used(p);
        free_pages--;
        search_hint = p;
        irq_restore(f);
        return p * PAGE_SIZE;
    }
    irq_restore(f);
    return 0;
}

uint64_t pmm_alloc_zeroed(void) {
    uint64_t p = pmm_alloc();
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

uint64_t pmm_alloc_contig(uint64_t count) {
    uint64_t f = irq_save();
    uint64_t run = 0;
    for (uint64_t p = 256; p < total_pages; p++) {
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

void pmm_free(uint64_t phys) {
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
uint64_t pmm_total_pages(void) { return usable_pages; }
