/* Kernel heap: address-ordered block list with first-fit and coalescing. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "sys/sched.h"

#define HEAP_MAGIC 0xC0FFEE42u
/* struct block's free word: bit 0 free; the others only on a free block. */
#define BLOCK_FREE     1u
#define BLOCK_RELEASED 2u /* some whole payload pages went back to the page allocator */
#define BLOCK_TRIM     4u /* large and holding backed payload pages worth releasing */
/* A free block this large gives its interior pages back. Smaller ones keep
   theirs: freeing and refilling them is common and costs TLB shootdowns. */
#define TRIM_MIN (1ULL << 20)

struct block {
    uint32_t magic;
    uint32_t free;
    uint64_t size; /* payload bytes */
    struct block *prev, *next;
};

static struct block *head, *tail;
static uint64_t heap_end = KHEAP_BASE;
static uint64_t heap_used;
/* Every block before scan_head (address order) is in use, so first-fit starts
   here instead of walking the thousands of permanent file blocks at the front.
   NULL means no hint yet (start at head). */
static struct block *scan_head;
static bool trim_pending;
static struct wait_queue trim_wq;

/* All or nothing: pages mapped before a failure go back, so heap_end never
   covers pages that no block accounts for. */
static bool grow(uint64_t bytes) {
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    uint64_t start = heap_end;
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t p = pmm_alloc();
        if (!p || !vmm_map_page(kernel_pml4, heap_end, p, PTE_W | pte_nx)) {
            if (p) pmm_free(p);
            while (heap_end > start) {
                heap_end -= PAGE_SIZE;
                uint64_t old = vmm_unmap_page(kernel_pml4, heap_end);
                if (old & PTE_P) pmm_free(old & PTE_ADDR);
            }
            return false;
        }
        heap_end += PAGE_SIZE;
    }
    return true;
}

/* Pages of a released block are mapped again before its bytes are used. */
static bool back(uint64_t start, uint64_t end) {
    for (uint64_t a = ALIGN_DOWN(start, PAGE_SIZE); a < end; a += PAGE_SIZE) {
        if (vmm_translate(kernel_pml4, a)) continue;
        uint64_t p = pmm_alloc();
        if (!p || !vmm_map_page(kernel_pml4, a, p, PTE_W | pte_nx)) {
            if (p) pmm_free(p);
            return false; /* what was mapped stays: the block remains released */
        }
    }
    return true;
}

static void split(struct block *b, size_t size) {
    if (b->size >= size + sizeof(struct block) + 32) {
        struct block *n = (struct block *)((uint8_t *)(b + 1) + size);
        n->magic = HEAP_MAGIC;
        n->free = b->free; /* the remainder may lack pages, or still hold releasable ones */
        n->size = b->size - size - sizeof(struct block);
        n->prev = b;
        n->next = b->next;
        if (b->next) b->next->prev = n;
        else tail = n;
        b->next = n;
        b->size = size;
    }
}

/* Hands out free block b for size bytes; false when its pages cannot be backed. */
static bool take(struct block *b, size_t size) {
    if (b->free & BLOCK_RELEASED) {
        /* The payload and the header split() may write after it. */
        uint64_t start = (uint64_t)(b + 1), end = start + MIN(b->size, size + sizeof(struct block));
        if (!back(start, end)) return false;
    }
    split(b, size);
    b->free = 0;
    heap_used += b->size;
    return true;
}

void *kmalloc(size_t size) {
    cpu_require_bsp();
    if (size == 0) size = 1;
    /* A size from user space (a channel message, a document) must not wrap
       the alignment or the block header arithmetic into a tiny block. */
    if (size > (uint64_t)1 << 40) return NULL;
    size = ALIGN_UP(size, 16);
    uint64_t f = irq_save();
    bool prefix = true; /* no free block seen yet: the used run extends the hint */
    for (struct block *b = scan_head ? scan_head : head; b; b = b->next) {
        if (!b->free) {
            if (prefix) scan_head = b;
            continue;
        }
        prefix = false;
        if (b->size >= size) {
            bool ok = take(b, size);
            irq_restore(f);
            return ok ? b + 1 : NULL;
        }
    }
    /* extend the heap */
    uint64_t need = size + sizeof(struct block);
    struct block *b;
    if (tail && tail->free) {
        uint64_t have = tail->size;
        if (!grow(size - have)) { irq_restore(f); return NULL; }
        b = tail;
        b->size = heap_end - (uint64_t)(b + 1);
    } else {
        uint64_t start = heap_end;
        if (!grow(need)) { irq_restore(f); return NULL; }
        b = (struct block *)start;
        b->magic = HEAP_MAGIC;
        b->prev = tail;
        b->next = NULL;
        b->size = heap_end - start - sizeof(struct block);
        b->free = BLOCK_FREE;
        if (tail) tail->next = b;
        else head = b;
        tail = b;
    }
    bool ok = take(b, size);
    irq_restore(f);
    return ok ? b + 1 : NULL;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *p) {
    cpu_require_bsp();
    if (!p) return;
    struct block *b = (struct block *)p - 1;
    if (b->magic != HEAP_MAGIC || b->free) {
        panic("kfree: bad pointer %p (magic %x free %d)", p, b->magic, b->free);
    }
    uint64_t f = irq_save();
    b->free = BLOCK_FREE;
    heap_used -= b->size;
    struct block *n = b->next;
    if (n && n->free) {
        b->free |= n->free;
        b->size += sizeof(struct block) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        else tail = b;
        n->magic = 0;
    }
    struct block *pr = b->prev;
    if (pr && pr->free) {
        pr->free |= b->free;
        pr->size += sizeof(struct block) + b->size;
        pr->next = b->next;
        if (b->next) b->next->prev = pr;
        else tail = pr;
        b->magic = 0;
        b = pr;
    }
    /* b is the free block that now covers the freed memory (a merged-away block
       is above it, so the hint can never be left on one). */
    if (scan_head > b) scan_head = b;
    /* The freed bytes are backed: a large block has pages to give back. */
    if (b->size >= TRIM_MIN) { b->free |= BLOCK_TRIM; trim_pending = true; }
    irq_restore(f);
    if (trim_pending) wq_wake_all(&trim_wq);
}

/* Gives the interior pages of large free blocks back to the page allocator.
   Runs in the heap's own task: never inside kfree, whose callers may hold
   interrupts off while APs wait on them, where a TLB shootdown could stall. */
static void heap_trim(void) {
    uint64_t f = irq_save();
    trim_pending = false;
    for (struct block *b = scan_head ? scan_head : head; b; b = b->next) {
        if (!(b->free & BLOCK_TRIM)) continue;
        b->free &= ~BLOCK_TRIM;
        uint64_t first = ALIGN_UP((uint64_t)(b + 1), PAGE_SIZE);
        uint64_t last = ALIGN_DOWN((uint64_t)(b + 1) + b->size, PAGE_SIZE);
        if (last <= first) continue;
        vfree((void *)first, (last - first) / PAGE_SIZE); /* frees only mapped pages */
        b->free |= BLOCK_RELEASED;
    }
    irq_restore(f);
}

static void heap_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = irq_save();
        if (!trim_pending) wq_wait(&trim_wq);
        irq_restore(f);
        heap_trim();
    }
}

void heap_trim_start(void) {
    kthread_create("heap", heap_thread, NULL);
}

void *krealloc(void *p, size_t size) {
    if (!p) return kmalloc(size);
    struct block *b = (struct block *)p - 1;
    if (b->size >= size) return p;
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, p, b->size);
    kfree(p);
    return n;
}

uint64_t heap_used_bytes(void) { return heap_used; }
uint64_t heap_size_bytes(void) { return heap_end - KHEAP_BASE; }
