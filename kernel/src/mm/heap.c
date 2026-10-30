/* Kernel heap: an address-ordered block list whose free neighbours merge, and an index of the free
   blocks by size (two-level segregated fit), so that a block is found in a few steps however many
   blocks the heap holds (the store's cache alone holds tens of thousands).

   Any CPU allocates and frees, under heap_lock (taken before the kernel page tables' and pmm's).
   The heap grows and backs released pages by adding mappings, which needs no shootdown; only the
   heap's own task, on the BSP, takes pages away (heap_trim). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
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
    struct block *prev, *next;           /* address order */
    struct block *free_prev, *free_next; /* its size class's list, while free (a released block's
                                            payload is not there to hold them) */
};

static struct block *head, *tail;
static uint64_t heap_end = KHEAP_BASE;
static uint64_t heap_used;
static bool trim_pending;
static struct wait_queue trim_wq;
static struct spinlock heap_lock;

/* The free blocks' size classes: below SMALL bytes one per 16 bytes, from there SL_COUNT for each
   power of two. A bit of class_map says which lists of a power of two have blocks, a bit of
   power_map which powers of two have any. */
#define SL_BITS 4
#define SL_COUNT (1u << SL_BITS)
#define SMALL_SHIFT (SL_BITS + 4)
#define SMALL (1ULL << SMALL_SHIFT)
#define FL_COUNT (64 - SMALL_SHIFT + 1)
static struct block *lists[FL_COUNT][SL_COUNT];
static uint16_t class_map[FL_COUNT];
static uint64_t power_map;

static void class_of(uint64_t size, unsigned *fl, unsigned *sl) {
    if (size < SMALL) {
        *fl = 0;
        *sl = (unsigned)(size >> 4);
        return;
    }
    unsigned top = 63u - (unsigned)__builtin_clzll(size);
    *fl = top - SMALL_SHIFT + 1;
    *sl = (unsigned)(size >> (top - SL_BITS)) & (SL_COUNT - 1);
}

static void list_add(struct block *b) {
    unsigned fl, sl;
    class_of(b->size, &fl, &sl);
    b->free_prev = NULL;
    b->free_next = lists[fl][sl];
    if (b->free_next) b->free_next->free_prev = b;
    lists[fl][sl] = b;
    class_map[fl] |= (uint16_t)(1u << sl);
    power_map |= 1ULL << fl;
}

static void list_remove(struct block *b) {
    unsigned fl, sl;
    class_of(b->size, &fl, &sl);
    if (b->free_next) b->free_next->free_prev = b->free_prev;
    if (b->free_prev) {
        b->free_prev->free_next = b->free_next;
    } else if (!(lists[fl][sl] = b->free_next)) {
        class_map[fl] &= (uint16_t)~(1u << sl);
        if (!class_map[fl]) power_map &= ~(1ULL << fl);
    }
}

/* A free block of at least size bytes, or NULL. The size is rounded up to the next class, so any
   block of the class found is large enough. */
static struct block *find(uint64_t size) {
    if (size >= SMALL) size += (1ULL << (63u - (unsigned)__builtin_clzll(size) - SL_BITS)) - 1;
    unsigned fl, sl;
    class_of(size, &fl, &sl);
    unsigned classes = class_map[fl] & (~0u << sl);
    if (!classes) {
        uint64_t powers = power_map & (~0ULL << fl << 1);
        if (!powers) return NULL;
        fl = (unsigned)__builtin_ctzll(powers);
        classes = class_map[fl];
    }
    return lists[fl][__builtin_ctz(classes)];
}

/* Maps pages at heap_end until bytes more are there; false when memory ran out first. The pages
   mapped stay: the caller's block (the free tail, or a new one) takes whatever heap_end reached,
   so it never covers pages that no block accounts for, and nothing is unmapped here. */
static bool grow(uint64_t bytes) {
    uint64_t end = heap_end + ALIGN_UP(bytes, PAGE_SIZE);
    while (heap_end < end) {
        uint64_t p = pmm_alloc();
        if (!p || !vmm_map_page(kernel_pml4, heap_end, p, PTE_W | PTE_NX)) {
            if (p) pmm_free(p);
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
        if (!p || !vmm_map_page(kernel_pml4, a, p, PTE_W | PTE_NX)) {
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
        list_add(n);
    }
}

/* Hands out free block b, which no list holds, for size bytes; false when its pages cannot be
   backed (b is listed again). */
static bool take(struct block *b, size_t size) {
    if (b->free & BLOCK_RELEASED) {
        /* The payload and the header split() may write after it. */
        uint64_t start = (uint64_t)(b + 1), end = start + MIN(b->size, size + sizeof(struct block));
        if (!back(start, end)) {
            list_add(b);
            return false;
        }
    }
    split(b, size);
    b->free = 0;
    heap_used += b->size;
    return true;
}

void *kmalloc(size_t size) {
    if (size == 0) size = 1;
    /* A size from user space (a channel message, a document) must not wrap
       the alignment or the block header arithmetic into a tiny block. */
    if (size > (uint64_t)1 << 40) return NULL;
    size = ALIGN_UP(size, 16);
    uint64_t f = spin_lock(&heap_lock);
    struct block *b = find(size);
    if (b) {
        list_remove(b);
    } else if (tail && tail->free) { /* the heap grows under its last block */
        b = tail;
        list_remove(b);
        bool grown = b->size >= size || grow(size - b->size);
        b->size = heap_end - (uint64_t)(b + 1);
        if (!grown) {
            list_add(b);
            spin_unlock(&heap_lock, f);
            return NULL;
        }
    } else {
        uint64_t start = heap_end;
        bool grown = grow(size + sizeof(struct block));
        if (heap_end - start <= sizeof(struct block)) { /* not even a header's room */
            spin_unlock(&heap_lock, f);
            return NULL;
        }
        b = (struct block *)start;
        b->magic = HEAP_MAGIC;
        b->prev = tail;
        b->next = NULL;
        b->size = heap_end - start - sizeof(struct block);
        b->free = BLOCK_FREE;
        if (tail) tail->next = b;
        else head = b;
        tail = b;
        if (!grown) {
            list_add(b);
            spin_unlock(&heap_lock, f);
            return NULL;
        }
    }
    bool ok = take(b, size);
    spin_unlock(&heap_lock, f);
    return ok ? b + 1 : NULL;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    struct block *b = (struct block *)p - 1;
    if (b->magic != HEAP_MAGIC || b->free) {
        panic("kfree: bad pointer %p (magic %x free %d)", p, b->magic, b->free);
    }
    uint64_t f = spin_lock(&heap_lock);
    b->free = BLOCK_FREE;
    heap_used -= b->size;
    struct block *n = b->next;
    if (n && n->free) {
        list_remove(n);
        b->free |= n->free;
        b->size += sizeof(struct block) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        else tail = b;
        n->magic = 0;
    }
    struct block *pr = b->prev;
    if (pr && pr->free) {
        list_remove(pr);
        pr->free |= b->free;
        pr->size += sizeof(struct block) + b->size;
        pr->next = b->next;
        if (b->next) b->next->prev = pr;
        else tail = pr;
        b->magic = 0;
        b = pr;
    }
    /* The freed bytes are backed: a large block has pages to give back. */
    bool trim = b->size >= TRIM_MIN;
    if (trim) { b->free |= BLOCK_TRIM; trim_pending = true; }
    list_add(b);
    spin_unlock(&heap_lock, f);
    if (trim) wq_wake_all(&trim_wq);
}

/* Gives the interior pages of large free blocks back to the page allocator.
   Runs in the heap's own task: never inside kfree, whose callers may hold
   spinlocks, under which a long walk and its TLB shootdowns would keep other CPUs waiting.
   Blocks of TRIM_MIN bytes and more are in the classes from TRIM_MIN's on. */
static void heap_trim(void) {
    uint64_t f = spin_lock(&heap_lock);
    trim_pending = false;
    unsigned first_fl, first_sl;
    class_of(TRIM_MIN, &first_fl, &first_sl);
    for (unsigned fl = first_fl; fl < FL_COUNT; fl++)
        for (unsigned sl = 0; sl < SL_COUNT; sl++)
            for (struct block *b = lists[fl][sl]; b; b = b->free_next) {
                if (!(b->free & BLOCK_TRIM)) continue;
                b->free &= ~BLOCK_TRIM;
                uint64_t first = ALIGN_UP((uint64_t)(b + 1), PAGE_SIZE);
                uint64_t last = ALIGN_DOWN((uint64_t)(b + 1) + b->size, PAGE_SIZE);
                if (last <= first) continue;
                vfree((void *)first, (last - first) / PAGE_SIZE); /* frees only mapped pages */
                b->free |= BLOCK_RELEASED;
            }
    spin_unlock(&heap_lock, f);
}

static void heap_thread(void *arg) {
    (void)arg;
    for (;;) {
        /* kfree sets trim_pending under heap_lock and wakes trim_wq after: looked at under the
           lock, no large free is missed. */
        uint64_t f = spin_lock(&heap_lock);
        if (!trim_pending) wq_wait_locked(&trim_wq, &heap_lock);
        else spin_give(&heap_lock);
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

void *kappend(void *array_at, size_t *count, size_t *room, size_t size) {
    void **array = array_at;
    if (*count == *room) {
        size_t more = *room ? *room * 2 : 8;
        if (more < *room || more > SIZE_MAX / size) return NULL;
        void *grown = krealloc(*array, more * size);
        if (!grown) return NULL;
        *array = grown;
        *room = more;
    }
    return (uint8_t *)*array + (*count)++ * size;
}

uint64_t heap_used_bytes(void) { return heap_used; }
uint64_t heap_size_bytes(void) { return heap_end - KHEAP_BASE; }
