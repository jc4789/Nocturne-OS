/* 実装が attach で受け取る資源（hal/hal.h）。ここを通るのは立ち上げと片づけだけで、データの道は通らない。 */
#include "kernel.h"
#include "arch/irq.h"
#include "hal/internal.h"
#include "mm/heap.h"
#include "mm/vmm.h"

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

void hal_release(struct hal_device *d) {
    if (d->irqs_on) hal_irq_disable(d);
    bool stopped = !d->ops || !d->ops->stop || d->ops->stop(d);
    hal_dma_release(d, stopped);
}
