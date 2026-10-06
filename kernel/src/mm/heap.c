/* Kernel heap: address-ordered block list with first-fit and coalescing. */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

#define HEAP_MAGIC 0xC0FFEE42u

struct block {
    uint32_t magic;
    uint32_t free;
    uint64_t size; /* payload bytes */
    struct block *prev, *next;
};

static struct block *head, *tail;
static uint64_t heap_end = KHEAP_BASE;
static uint64_t heap_used;

static bool grow(uint64_t bytes) {
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t p = pmm_alloc();
        if (!p) return false;
        vmm_map_page(kernel_pml4, heap_end, p, PTE_W | pte_nx);
        heap_end += PAGE_SIZE;
    }
    return true;
}

static void split(struct block *b, size_t size) {
    if (b->size >= size + sizeof(struct block) + 32) {
        struct block *n = (struct block *)((uint8_t *)(b + 1) + size);
        n->magic = HEAP_MAGIC;
        n->free = 1;
        n->size = b->size - size - sizeof(struct block);
        n->prev = b;
        n->next = b->next;
        if (b->next) b->next->prev = n;
        else tail = n;
        b->next = n;
        b->size = size;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) size = 1;
    size = ALIGN_UP(size, 16);
    uint64_t f = irq_save();
    for (struct block *b = head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split(b, size);
            b->free = 0;
            heap_used += b->size;
            irq_restore(f);
            return b + 1;
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
        if (tail) tail->next = b;
        else head = b;
        tail = b;
    }
    split(b, size);
    b->free = 0;
    heap_used += b->size;
    irq_restore(f);
    return b + 1;
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
    uint64_t f = irq_save();
    b->free = 1;
    heap_used -= b->size;
    struct block *n = b->next;
    if (n && n->free) {
        b->size += sizeof(struct block) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        else tail = b;
        n->magic = 0;
    }
    struct block *pr = b->prev;
    if (pr && pr->free) {
        pr->size += sizeof(struct block) + b->size;
        pr->next = b->next;
        if (b->next) b->next->prev = pr;
        else tail = pr;
        b->magic = 0;
    }
    irq_restore(f);
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
