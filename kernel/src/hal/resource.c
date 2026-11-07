/* 実装が attach で受け取る資源（hal/hal.h）。ここを通るのは立ち上げと片づけだけで、データの道は通らない。 */
#include "kernel.h"
#include "arch/irq.h"
#include "arch/topology.h"
#include "hal/internal.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

struct hal_dma_block {
    struct hal_dma_block *next;
    void *memory;
    uint64_t phys, pages;
};

void hal_resource_add(struct hal_device *d, enum n_hal_resource_kind kind, uint32_t slot, uint32_t flags, uint64_t base,
                      uint64_t size) {
    struct n_hal_resource *grown = krealloc(d->resources, (d->resource_count + 1) * sizeof *grown);
    if (!grown) {
        d->incomplete = true;
        return;
    }
    d->resources = grown;
    grown[d->resource_count++] = (struct n_hal_resource){.kind = kind, .slot = slot, .flags = flags, .base = base, .size = size};
}

const struct n_hal_resource *hal_resource(const struct hal_device *d, enum n_hal_resource_kind kind, uint32_t slot) {
    for (uint32_t i = 0; i < d->resource_count; i++)
        if (d->resources[i].kind == kind && d->resources[i].slot == slot) return &d->resources[i];
    return NULL;
}

void *hal_map(struct hal_device *d, uint32_t slot) {
    const struct n_hal_resource *r = hal_resource(d, N_HAL_MEMORY, slot);
    return r ? vmm_map_mmio(r->base, ALIGN_UP(r->size, PAGE_SIZE)) : NULL;
}

bool hal_resource_holds(const struct hal_device *d, enum n_hal_resource_kind kind, uint64_t address) {
    for (uint32_t i = 0; i < d->resource_count; i++) {
        const struct n_hal_resource *r = &d->resources[i];
        if (r->kind == kind && address >= r->base && address - r->base < r->size) return true;
    }
    return false;
}

uint16_t hal_port(const struct hal_device *d, uint32_t slot) {
    const struct n_hal_resource *r = hal_resource(d, N_HAL_PORT, slot);
    return r ? (uint16_t)r->base : 0;
}

void hal_enable(struct hal_device *d, unsigned what) {
    if (d->ops && d->ops->enable) d->ops->enable(d, what);
}

unsigned hal_irq_enable(struct hal_device *d, const struct irq_spec *specs, unsigned count, struct irq **irqs) {
    if (!d->ops || !d->ops->irq_enable || !count) return 0;
    struct irq_spec *placed = kmalloc(count * sizeof *placed);
    if (!placed) return 0;
    for (unsigned i = 0; i < count; i++) {
        placed[i] = specs[i];
        placed[i].node = d->node;
    }
    unsigned got = d->ops->irq_enable(d, placed, count, irqs);
    kfree(placed);
    if (got) d->irqs_on = true;
    return got;
}

void hal_irq_disable(struct hal_device *d) {
    if (d->irqs_on && d->ops->irq_disable(d)) d->irqs_on = false;
}

unsigned hal_irq_max(struct hal_device *d) { return d->ops && d->ops->irq_max ? d->ops->irq_max(d) : 0; }

void *hal_dma_alloc(struct hal_device *d, size_t bytes, unsigned bits, uint64_t *bus) {
    uint64_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    unsigned node = d->node >= 0 ? (unsigned)d->node : topology.cpu[cpu_current_index()].node;
    struct hal_dma_block *b = kmalloc(sizeof *b);
    if (!b) return NULL;
    uint64_t phys = 0;
    if (!bits || bits >= 64) phys = pmm_alloc_run(pages, node);
    if (!phys) phys = pmm_alloc_contig(pages, bits ? bits : 64);
    if (!phys) {
        kfree(b);
        return NULL;
    }
    b->memory = phys_to_virt(phys);
    b->phys = phys;
    b->pages = pages;
    memset(b->memory, 0, pages * PAGE_SIZE);
    uint64_t f = spin_lock(&hal_lock);
    b->next = d->dma;
    d->dma = b;
    spin_unlock(&hal_lock, f);
    *bus = hal_dma_address(d, phys);
    return b->memory;
}

void hal_dma_free(struct hal_device *d, void *memory) {
    struct hal_dma_block *found = NULL;
    uint64_t f = spin_lock(&hal_lock);
    for (struct hal_dma_block **p = &d->dma; *p; p = &(*p)->next)
        if ((*p)->memory == memory) {
            found = *p;
            *p = found->next;
            break;
        }
    spin_unlock(&hal_lock, f);
    ASSERT(found);
    pmm_free_contig(found->phys, found->pages);
    kfree(found);
}

void hal_release(struct hal_device *d) {
    if (d->irqs_on) hal_irq_disable(d);
    bool stopped = !d->ops || !d->ops->stop || d->ops->stop(d);
    uint64_t f = spin_lock(&hal_lock);
    struct hal_dma_block *blocks = d->dma;
    d->dma = NULL;
    spin_unlock(&hal_lock, f);
    /* 止まったと確かめられない装置が書くかもしれないメモリーは、誰にも渡さない */
    if (!stopped && blocks) kprintf("hal: %s did not stop: its DMA memory is kept\n", d->location);
    while (blocks) {
        struct hal_dma_block *next = blocks->next;
        if (stopped) pmm_free_contig(blocks->phys, blocks->pages);
        kfree(blocks);
        blocks = next;
    }
}
